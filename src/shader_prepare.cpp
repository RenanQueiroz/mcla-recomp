// Shader preparation: mcla.exe --mcla_prepare_shaders
//
// Fills this player's shader storage (<cache>/shaders/shareable) from the shader pack that comes
// with the fork (shaderpack/545407F8.shaderpack), then has the GPU create every stored pipeline
// with the settings the game is played with, and quits. Afterwards, places and effects the pack
// covers no longer compile shaders while playing.
//
// The pack holds no game data: only fingerprints (XXH3 hashes), offsets and lengths into the
// player's own default.xex, and the pipeline descriptions (render state and shader hashes) that
// the storage holds anyway. The shaders themselves come from the player's game:
// - pixel shaders, as they are, from the RAGE effects in xarchive_cache.rpf
//   (shaders/*/fxl_final/*.fxc) and from default.xex, found by their fingerprint;
// - vertex shaders, which the game's Direct3D patches when it binds one to a mesh's vertex layout
//   and to the pixel shader that reads its outputs: the effect's program bound with the game's own
//   binding code (IDirect3DVertexShader9::Bind, recompiled: sub_82424EB0) to vertex layouts made
//   from the effects' preset files (shaders/*/<presets>/*.sps) and the game's vertex type table,
//   until the result's fingerprint is the one in the pack;
// - three shaders of the console's own Direct3D, at a known place in default.xex.
// Every shader rebuilt is checked against its fingerprint; one that doesn't match is left out.
//
// Standard output gets lines for the launcher, all starting with "SHADERPREP ":
//   start <entries>                          rebuild <done> <total>
//   rebuild-done <ok> <failed> <ms>          merge <shaders added> <pipelines added>
//   preload <done> <total>  /  preload-done <created> <total> <ms>   (from the GPU plugin)
//   done <rebuild ms> <preload ms> <private MB> <GPU MB>
//   error <code> <message>                   (then the process ends with that code)
// Exit codes: 0 done, 2 no pack, 3 bad pack, 4 game files don't match, 5 pack for another
// version of the graphics code, 6 storage not writable or another error.
//
// Nothing is written to the storage before every check passed, and nothing else is written: not
// mcla.toml, not the save. Files are replaced by writing a new one and renaming it; entries
// already in a file stay, in their order.

#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <rex/hash.h>  // XXH3, inline

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/shader_storage.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/window.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
#endif

REXCVAR_DEFINE_BOOL(mcla_prepare_shaders, false, "MCLA",
                    "Prepare the shader storage from the shader pack, create every stored "
                    "pipeline with the current settings, then quit (the launcher runs this)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(mcla_shader_pack, "", "MCLA",
                      "Shader pack for --mcla_prepare_shaders (default: "
                      "shaderpack/545407F8.shaderpack in the repository the game is built in)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(mcla_prepare_shaders_check, "", "MCLA",
                      "Development: with --mcla_prepare_shaders, rebuild the pack's shaders, "
                      "write whether each one was rebuilt to this file and quit, without "
                      "changing the storage")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The game's Direct3D: the vertex shader object's constructor and IDirect3DVertexShader9::Bind.
REX_EXTERN(sub_82417B78);
REX_EXTERN(sub_82424EB0);

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kTitleId = 0x545407F8;
constexpr uint32_t kImageBase = 0x82000000;
constexpr uint32_t kImageSize = 0x009E0000;
// In the decrypted default.xex (title update 0.0.0.8): the key of the archives' file tables, and
// the game's table from its mesh vertex data types to Direct3D vertex declaration types.
constexpr uint32_t kRpfKeyAddress = 0x827D8AC4;
constexpr uint32_t kDeclTypeTableAddress = 0x827D42B0;
// The folder of the effects' preset files (.sps), by its name's hash: its name isn't known.
constexpr uint32_t kPresetFolderHash = 0x6D1F986C;

struct Failure {
  int code;
  std::string message;
};

void Line(const std::string& text) {
  std::fprintf(stdout, "SHADERPREP %s\n", text.c_str());
  std::fflush(stdout);
}

[[noreturn]] void Quit(int code) {
  rex::FlushLogging();
  std::_Exit(code);
}

[[noreturn]] void Fail(const Failure& failure) {
  REXLOG_ERROR("prepare shaders: {}", failure.message);
  Line("error " + std::to_string(failure.code) + " " + failure.message);
  Quit(failure.code);
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t Le64(const uint8_t* p) { return Le32(p) | (uint64_t(Le32(p + 4)) << 32); }
void PutLe32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void PutLe64(std::vector<uint8_t>& out, uint64_t v) {
  PutLe32(out, uint32_t(v));
  PutLe32(out, uint32_t(v >> 32));
}
void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
uint64_t Hash(const void* data, size_t size) { return XXH3_64bits(data, size); }
uint32_t ByteSwap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

// RAGE's name hash (Jenkins one-at-a-time over the lower-case name).
uint32_t NameHash(std::string_view name) {
  uint32_t h = 0;
  for (char c : name) {
    h += uint8_t(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;
  h += h << 15;
  return h;
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  file.seekg(0, std::ios::end);
  out.resize(size_t(file.tellg()));
  file.seekg(0);
  return bool(file.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
}

// ---------------------------------------------------------------------------------------------
// Raw deflate (RFC 1951), for the archive's compressed files. After Mark Adler's puff.c (zlib's
// contrib/puff, zlib licence; THIRD-PARTY-NOTICES.md): canonical Huffman codes decoded from the
// count of codes of each length, without tables. Rewritten for this file, not a copy.

class Inflater {
 public:
  Inflater(const uint8_t* in, size_t size) : in_(in), size_(size) {}

  bool Run(std::vector<uint8_t>& out) {
    out_ = &out;
    for (;;) {
      int last = Bits(1);
      int type = Bits(2);
      if (failed_) return false;
      bool ok = type == 0 ? Stored() : type == 1 ? Fixed() : type == 2 ? Dynamic() : false;
      if (!ok || failed_) return false;
      if (last) return true;
    }
  }

 private:
  struct Huffman {
    uint16_t count[16]{};
    uint16_t symbol[320]{};
  };

  int Bits(int n) {
    while (bit_count_ < n) {
      if (pos_ >= size_) {
        failed_ = true;
        return 0;
      }
      bit_buffer_ |= uint32_t(in_[pos_++]) << bit_count_;
      bit_count_ += 8;
    }
    int v = int(bit_buffer_ & ((1u << n) - 1));
    bit_buffer_ >>= n;
    bit_count_ -= n;
    return v;
  }

  bool Build(Huffman& h, const uint8_t* lengths, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
    if (h.count[0] == n) return true;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
      left = (left << 1) - h.count[len];
      if (left < 0) return false;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i) {
      if (lengths[i]) h.symbol[offs[lengths[i]]++] = uint16_t(i);
    }
    return true;
  }

  int Decode(const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= Bits(1);
      if (failed_) return -1;
      int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }

  bool Stored() {
    bit_buffer_ = 0;
    bit_count_ = 0;
    if (pos_ + 4 > size_) return false;
    uint32_t len = in_[pos_] | (in_[pos_ + 1] << 8);
    uint32_t nlen = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
    pos_ += 4;
    if (len != (~nlen & 0xFFFF) || pos_ + len > size_) return false;
    out_->insert(out_->end(), in_ + pos_, in_ + pos_ + len);
    pos_ += len;
    return true;
  }

  bool Codes(const Huffman& lengthcode, const Huffman& distcode) {
    static const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                             15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                             67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                           17,   25,   33,   49,   65,   97,    129,   193,
                                           257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                           4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int symbol = Decode(lengthcode);
      if (symbol < 0) return false;
      if (symbol < 256) {
        out_->push_back(uint8_t(symbol));
      } else if (symbol == 256) {
        return true;
      } else {
        symbol -= 257;
        if (symbol >= 29) return false;
        size_t len = kLengthBase[symbol] + Bits(kLengthExtra[symbol]);
        int dsym = Decode(distcode);
        if (dsym < 0 || dsym >= 30) return false;
        size_t dist = kDistBase[dsym] + Bits(kDistExtra[dsym]);
        if (failed_ || dist > out_->size()) return false;
        size_t from = out_->size() - dist;
        for (size_t i = 0; i < len; ++i) out_->push_back((*out_)[from + i]);
      }
    }
  }

  bool Fixed() {
    uint8_t lengths[320];
    int i = 0;
    for (; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    Huffman lengthcode, distcode;
    Build(lengthcode, lengths, 288);
    for (i = 0; i < 30; ++i) lengths[i] = 5;
    Build(distcode, lengths, 30);
    return Codes(lengthcode, distcode);
  }

  bool Dynamic() {
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
    if (failed_ || nlen > 286 || ndist > 30) return false;
    uint8_t lengths[320] = {};
    for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = uint8_t(Bits(3));
    Huffman lencode, distcode;
    if (!Build(lencode, lengths, 19)) return false;
    int index = 0;
    while (index < nlen + ndist) {
      int symbol = Decode(lencode);
      if (symbol < 0) return false;
      if (symbol < 16) {
        lengths[index++] = uint8_t(symbol);
      } else {
        int len = 0, repeat;
        if (symbol == 16) {
          if (index == 0) return false;
          len = lengths[index - 1];
          repeat = 3 + Bits(2);
        } else if (symbol == 17) {
          repeat = 3 + Bits(3);
        } else {
          repeat = 11 + Bits(7);
        }
        if (failed_ || index + repeat > nlen + ndist) return false;
        while (repeat--) lengths[index++] = uint8_t(len);
      }
    }
    if (lengths[256] == 0) return false;
    if (!Build(lencode, lengths, nlen)) return false;
    if (!Build(distcode, lengths + nlen, ndist)) return false;
    return Codes(lencode, distcode);
  }

  const uint8_t* in_;
  size_t size_;
  size_t pos_ = 0;
  uint32_t bit_buffer_ = 0;
  int bit_count_ = 0;
  bool failed_ = false;
  std::vector<uint8_t>* out_ = nullptr;
};

// ---------------------------------------------------------------------------------------------
// The archive (RPF3): its file table is encrypted (AES-256, ECB, 16 times) with a key that is in
// the game's own executable.

struct ArchiveFile {
  std::vector<uint32_t> path;  // name hashes from the root
  uint32_t entry[4];
};

// AES-256 decryption (FIPS-197) of single blocks, for the archive's file table. The SDK's AES
// code is internal to it and only 128-bit, and Windows' bcrypt isn't on the other platforms.
class Aes256Decryptor {
 public:
  explicit Aes256Decryptor(const uint8_t key[32]) {
    // The S-box: the multiplicative inverse in GF(2^8), then the affine transform.
    uint8_t p = 1, q = 1;
    do {
      p = uint8_t(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
      q = uint8_t(q ^ (q << 1));
      q = uint8_t(q ^ (q << 2));
      q = uint8_t(q ^ (q << 4));
      if (q & 0x80) q ^= 0x09;
      sbox_[p] = uint8_t(q ^ Rotl(q, 1) ^ Rotl(q, 2) ^ Rotl(q, 3) ^ Rotl(q, 4) ^ 0x63);
    } while (p != 1);
    sbox_[0] = 0x63;
    for (int i = 0; i < 256; ++i) {
      inverse_[sbox_[i]] = uint8_t(i);
      mul9_[i] = Multiply(uint8_t(i), 9);
      mul11_[i] = Multiply(uint8_t(i), 11);
      mul13_[i] = Multiply(uint8_t(i), 13);
      mul14_[i] = Multiply(uint8_t(i), 14);
    }
    // The key schedule: 15 round keys of 16 bytes.
    std::memcpy(round_keys_, key, 32);
    uint8_t rcon = 1;
    for (int i = 8; i < 60; ++i) {
      uint8_t t[4];
      std::memcpy(t, round_keys_ + (i - 1) * 4, 4);
      if (i % 8 == 0) {
        const uint8_t first = t[0];
        t[0] = uint8_t(sbox_[t[1]] ^ rcon);
        t[1] = sbox_[t[2]];
        t[2] = sbox_[t[3]];
        t[3] = sbox_[first];
        rcon = Multiply(rcon, 2);
      } else if (i % 8 == 4) {
        for (uint8_t& b : t) b = sbox_[b];
      }
      for (int j = 0; j < 4; ++j) round_keys_[i * 4 + j] = uint8_t(round_keys_[(i - 8) * 4 + j] ^ t[j]);
    }
  }

  void DecryptBlock(uint8_t block[16]) const {
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = uint8_t(block[i] ^ round_keys_[14 * 16 + i]);
    for (int round = 13; round >= 0; --round) {
      // InvShiftRows and InvSubBytes (column-major state: byte r + 4c is row r, column c).
      uint8_t t[16];
      for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) t[r + 4 * c] = inverse_[s[r + 4 * ((c - r + 4) % 4)]];
      }
      for (int i = 0; i < 16; ++i) t[i] ^= round_keys_[round * 16 + i];
      if (round == 0) {
        std::memcpy(s, t, 16);
        break;
      }
      for (int c = 0; c < 4; ++c) {  // InvMixColumns
        const uint8_t* a = t + 4 * c;
        s[4 * c + 0] = uint8_t(mul14_[a[0]] ^ mul11_[a[1]] ^ mul13_[a[2]] ^ mul9_[a[3]]);
        s[4 * c + 1] = uint8_t(mul9_[a[0]] ^ mul14_[a[1]] ^ mul11_[a[2]] ^ mul13_[a[3]]);
        s[4 * c + 2] = uint8_t(mul13_[a[0]] ^ mul9_[a[1]] ^ mul14_[a[2]] ^ mul11_[a[3]]);
        s[4 * c + 3] = uint8_t(mul11_[a[0]] ^ mul13_[a[1]] ^ mul9_[a[2]] ^ mul14_[a[3]]);
      }
    }
    std::memcpy(block, s, 16);
  }

 private:
  static uint8_t Rotl(uint8_t x, int n) { return uint8_t((x << n) | (x >> (8 - n))); }
  static uint8_t Multiply(uint8_t a, uint8_t b) {
    uint8_t product = 0;
    for (; b; b >>= 1) {
      if (b & 1) product ^= a;
      a = uint8_t((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
    }
    return product;
  }

  uint8_t sbox_[256], inverse_[256], mul9_[256], mul11_[256], mul13_[256], mul14_[256];
  uint8_t round_keys_[240];
};

// The archive's file table: AES-256 in ECB mode, applied 16 times.
void DecryptTable(std::vector<uint8_t>& table, const uint8_t key[32]) {
  const Aes256Decryptor aes(key);
  const size_t size = table.size() / 16 * 16;
  for (int round = 0; round < 16; ++round) {
    for (size_t offset = 0; offset < size; offset += 16) aes.DecryptBlock(table.data() + offset);
  }
}

// The plain (not resource) files under shaders/: the effects and the preset files.
std::vector<std::pair<ArchiveFile, std::vector<uint8_t>>> ReadShaderFiles(
    const std::filesystem::path& archive_path, const uint8_t key[32]) {
  std::ifstream file(archive_path, std::ios::binary);
  if (!file) throw Failure{4, "can't open " + archive_path.string()};
  uint8_t header[20];
  if (!file.read(reinterpret_cast<char*>(header), 20) || std::memcmp(header, "RPF3", 4) != 0)
    throw Failure{4, archive_path.filename().string() + " is not the expected archive"};
  const uint32_t table_size = Le32(header + 4), count = Le32(header + 8);
  if (table_size > (64u << 20) || uint64_t(count) * 16 > table_size)
    throw Failure{4, archive_path.filename().string() + " has an unexpected file table"};
  std::vector<uint8_t> table(table_size);
  file.seekg(0x800);
  if (!file.read(reinterpret_cast<char*>(table.data()), table_size))
    throw Failure{4, "can't read the file table of " + archive_path.filename().string()};
  if (Le32(header + 16)) DecryptTable(table, key);
  auto entry = [&](uint32_t i) {
    std::array<uint32_t, 4> e;
    for (int k = 0; k < 4; ++k) e[k] = Le32(table.data() + i * 16 + k * 4);
    return e;
  };
  const auto root = entry(0);
  if (!(root[2] & 0x80000000u)) throw Failure{4, "the archive's file table can't be read"};
  const uint32_t shaders = NameHash("shaders");
  std::vector<std::pair<ArchiveFile, std::vector<uint8_t>>> out;
  // Depth-first from shaders/, at most three levels below it.
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> stack;
  for (uint32_t c = root[2] & 0x7FFFFFFF, n = 0; n < root[3] && c < count; ++c, ++n) {
    if (entry(c)[0] == shaders) stack.push_back({c, {shaders}});
  }
  while (!stack.empty()) {
    auto [index, path] = stack.back();
    stack.pop_back();
    const auto e = entry(index);
    if (e[2] & 0x80000000u) {
      if (path.size() > 4) continue;
      for (uint32_t c = e[2] & 0x7FFFFFFF, n = 0; n < e[3] && c < count; ++c, ++n) {
        auto child = path;
        child.push_back(entry(c)[0]);
        stack.push_back({c, std::move(child)});
      }
      continue;
    }
    const uint32_t kind = e[3] >> 30;
    if (kind & 2) continue;  // a resource: not needed
    std::vector<uint8_t> data;
    file.seekg(std::streamoff(e[2]));
    if (kind == 1) {
      std::vector<uint8_t> packed(e[3] & 0x3FFFFFFF);
      if (!file.read(reinterpret_cast<char*>(packed.data()), std::streamsize(packed.size())))
        throw Failure{4, "can't read " + archive_path.filename().string()};
      Inflater inflater(packed.data(), packed.size());
      if (!inflater.Run(data) || data.size() != e[1])
        throw Failure{4, "a file in " + archive_path.filename().string() + " doesn't decompress"};
    } else {
      data.resize(e[1]);
      if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())))
        throw Failure{4, "can't read " + archive_path.filename().string()};
    }
    ArchiveFile f;
    f.path = path;
    std::memcpy(f.entry, e.data(), 16);
    out.push_back({std::move(f), std::move(data)});
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Shader programs: the Xbox 360 compiled shader blobs inside the effects and default.xex.
// Header (big-endian): magic 0x102A1100 (pixel) or 0x102A1101 (vertex), the offset and size of
// the data (literal constants, then the microcode), ..., at +24 the offset of the program info,
// which starts with the literals' and the microcode's sizes.

struct Program {
  const uint8_t* blob = nullptr;  // the blob's header part [0, data offset), then its data
  uint32_t data_offset = 0, data_size = 0, info_offset = 0;
  uint32_t literal_bytes = 0, ucode_bytes = 0;
  bool vertex = false;
  const uint8_t* ucode() const { return blob + data_offset + literal_bytes; }
  // A vertex shader's inputs: (usage, usage index) each, from the program info's input table.
  std::vector<std::pair<uint8_t, uint8_t>> Inputs() const {
    std::vector<std::pair<uint8_t, uint8_t>> inputs;
    const uint8_t* info = blob + info_offset;
    const uint32_t count = Be32(info + 28), first = Be32(info + 24) + 9;
    if (info_offset + (first + count) * 4 > data_offset) return inputs;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t e = Be32(info + (first + i) * 4);
      inputs.push_back({uint8_t((e >> 12) & 15), uint8_t((e >> 16) & 15)});
    }
    return inputs;
  }
};

void FindPrograms(const uint8_t* data, size_t size, std::unordered_map<uint64_t, Program>& out) {
  for (size_t b = 0; b + 64 <= size; ++b) {
    if (data[b] != 0x10 || data[b + 1] != 0x2A || data[b + 2] != 0x11 || data[b + 3] > 1) continue;
    Program p;
    p.blob = data + b;
    p.vertex = data[b + 3] == 1;
    p.data_offset = Be32(data + b + 4);
    p.data_size = Be32(data + b + 8);
    p.info_offset = Be32(data + b + 24);
    if (p.data_offset < 0x24 || p.data_offset >= 0x10000 || !p.data_size ||
        p.data_size >= 0x40000 || b + p.data_offset + p.data_size > size ||
        p.info_offset + 8 > p.data_offset) {
      continue;
    }
    p.literal_bytes = Be32(p.blob + p.info_offset);
    p.ucode_bytes = Be32(p.blob + p.info_offset + 4);
    if (uint64_t(p.literal_bytes) + p.ucode_bytes != p.data_size || !p.ucode_bytes ||
        p.ucode_bytes % 4) {
      continue;
    }
    out.emplace(Hash(p.ucode(), p.ucode_bytes), p);
  }
}

// ---------------------------------------------------------------------------------------------
// Vertex layouts to bind a vertex shader with, made by rules from the effects' preset files (the
// mesh channels of each preset) and the game's own vertex type table: RAGE lays a mesh's vertex
// out as its channels in a fixed order, each in one of a few types.

enum TypeId : uint8_t { kHalf2, kHalf4, kFloat2, kFloat3, kFloat4, kUbyte4, kColor, kDec3n, kTypes };
constexpr uint8_t kTypeSize[kTypes] = {4, 8, 8, 12, 16, 4, 4, 4};
// Index of each type in the game's table (its mesh vertex data types).
constexpr uint8_t kTypeTableIndex[kTypes] = {1, 3, 5, 6, 7, 8, 9, 10};

struct Element {
  uint8_t stream, usage, index, type;
  uint16_t offset;
  bool operator<(const Element& o) const {
    return std::tie(stream, offset, usage, index, type) <
           std::tie(o.stream, o.offset, o.usage, o.index, o.type);
  }
  bool operator==(const Element& o) const {
    return stream == o.stream && usage == o.usage && index == o.index && type == o.type &&
           offset == o.offset;
  }
};

struct Layout {
  std::vector<Element> elements;
  std::vector<uint8_t> strides;  // bytes, per stream
};

// Channel order of RAGE's vertex format (its bit in a preset's channel mask), with the usage and
// usage index of each and the types it comes in.
struct Channel {
  uint8_t usage, index;
  std::vector<TypeId> types;
};
const std::vector<Channel>& Channels() {
  static const std::vector<Channel> channels = [] {
    std::vector<Channel> c;
    c.push_back({0, 0, {kFloat3, kFloat4, kFloat2}});  // position
    c.push_back({1, 0, {kColor, kUbyte4}});            // blend weight
    c.push_back({2, 0, {kColor, kUbyte4}});            // blend indices
    c.push_back({3, 0, {kDec3n, kFloat3, kFloat4}});   // normal
    c.push_back({10, 0, {kColor}});                    // diffuse
    c.push_back({10, 1, {kColor}});                    // specular
    for (uint8_t i = 0; i < 8; ++i) c.push_back({5, i, {kHalf2, kHalf4, kFloat2, kFloat4, kColor}});
    for (uint8_t i = 0; i < 2; ++i) c.push_back({6, i, {kDec3n, kFloat4}});  // tangent
    for (uint8_t i = 0; i < 2; ++i) c.push_back({7, i, {kDec3n, kFloat4}});  // binormal
    return c;
  }();
  return channels;
}

// Every layout of a channel mask: each channel in each of its types, packed in channel order.
template <typename F>
bool ForEachLayoutOfMask(uint32_t mask, F&& visit) {
  std::vector<uint32_t> channels;
  for (uint32_t c = 0; c < Channels().size(); ++c)
    if (mask >> c & 1) channels.push_back(c);
  std::vector<size_t> choice(channels.size(), 0);
  for (;;) {
    Layout layout;
    uint32_t offset = 0;
    for (size_t i = 0; i < channels.size(); ++i) {
      const Channel& ch = Channels()[channels[i]];
      const TypeId t = ch.types[choice[i]];
      layout.elements.push_back({0, ch.usage, ch.index, uint8_t(t), uint16_t(offset)});
      offset += kTypeSize[t];
    }
    layout.strides.push_back(uint8_t(offset));
    if (visit(layout)) return true;
    size_t i = 0;
    for (; i < channels.size(); ++i) {
      if (++choice[i] < Channels()[channels[i]].types.size()) break;
      choice[i] = 0;
    }
    if (i == channels.size()) return false;
  }
}

// The families of layouts tried, in order, until one gives the expected shader:
// F1 each preset's channels; F2 the same with skinning channels added; F3 the shader's own
// channels plus up to three others; F4 any of those plus an instance stream (stream 1); F5 one
// stream per input.
template <typename F>
bool ForEachLayout(const std::vector<uint32_t>& preset_masks,
                   const std::vector<std::pair<uint8_t, uint8_t>>& inputs, F&& visit) {
  for (uint32_t mask : preset_masks)
    if (ForEachLayoutOfMask(mask, visit)) return true;
  std::vector<uint32_t> skinned;
  for (uint32_t mask : preset_masks) skinned.push_back(mask | 6);
  for (uint32_t mask : preset_masks) skinned.push_back(mask | 4);
  for (uint32_t mask : skinned)
    if (ForEachLayoutOfMask(mask, visit)) return true;
  uint32_t required = 0;
  for (auto [usage, index] : inputs) {
    for (uint32_t c = 0; c < Channels().size(); ++c)
      if (Channels()[c].usage == usage && Channels()[c].index == index) required |= 1u << c;
  }
  static const uint32_t kHidden[] = {1, 2, 3, 4, 5, 6, 7, 14};
  std::vector<uint32_t> optional;
  for (uint32_t h : kHidden)
    if (!(required >> h & 1)) optional.push_back(h);
  std::set<uint32_t> with_hidden;
  with_hidden.insert(required);
  for (size_t a = 0; a < optional.size(); ++a) {
    with_hidden.insert(required | 1u << optional[a]);
    for (size_t b = a + 1; b < optional.size(); ++b) {
      with_hidden.insert(required | 1u << optional[a] | 1u << optional[b]);
      for (size_t c = b + 1; c < optional.size(); ++c)
        with_hidden.insert(required | 1u << optional[a] | 1u << optional[b] | 1u << optional[c]);
    }
  }
  for (uint32_t mask : with_hidden)
    if (ForEachLayoutOfMask(mask, visit)) return true;
  // F4: an instance stream for each input the layout lacks.
  std::vector<uint32_t> base(skinned);
  base.insert(base.end(), with_hidden.begin(), with_hidden.end());
  base.insert(base.end(), preset_masks.begin(), preset_masks.end());
  for (uint32_t mask : base) {
    bool found = ForEachLayoutOfMask(mask, [&](const Layout& layout) {
      for (auto [usage, index] : inputs) {
        bool present = false;
        for (const Element& e : layout.elements)
          present |= e.usage == usage && e.index == index;
        if (present) continue;
        for (TypeId t : {kFloat4, kFloat3, kHalf4}) {
          Layout with = layout;
          with.elements.push_back({1, usage, index, uint8_t(t), 0});
          with.strides.push_back(kTypeSize[t]);
          if (visit(with)) return true;
        }
      }
      return false;
    });
    if (found) return true;
  }
  // F5: one stream per input.
  static const TypeId kPerStream[] = {kFloat2, kFloat4, kHalf2, kHalf4, kFloat3, kColor};
  std::vector<size_t> choice(inputs.size(), 0);
  if (inputs.empty() || inputs.size() > 6) return false;
  for (;;) {
    Layout layout;
    for (size_t i = 0; i < inputs.size(); ++i) {
      const TypeId t = kPerStream[choice[i]];
      layout.elements.push_back({uint8_t(i), inputs[i].first, inputs[i].second, uint8_t(t), 0});
      layout.strides.push_back(kTypeSize[t]);
    }
    if (visit(layout)) return true;
    size_t i = 0;
    for (; i < inputs.size(); ++i) {
      if (++choice[i] < std::size(kPerStream)) break;
      choice[i] = 0;
    }
    if (i == inputs.size()) return false;
  }
}

// ---------------------------------------------------------------------------------------------
// Binding with the game's own code, in guest memory set aside for it.

class GuestBinder {
 public:
  GuestBinder(rex::memory::Memory* memory, const uint32_t decl_types[kTypes])
      : memory_(memory), base_(memory->virtual_membase()) {
    std::memcpy(decl_types_, decl_types, sizeof(decl_types_));
    scratch_ = memory_->SystemHeapAlloc(kScratchSize, 4096);
  }
  ~GuestBinder() {
    if (scratch_) memory_->SystemHeapFree(scratch_);
  }
  bool ok() const { return scratch_ != 0; }

  // Builds the vertex shader object (as D3DDevice_CreateVertexShader would), the pixel shader
  // object, and keeps copies to start every attempt from.
  bool Prepare(const Program& vs, const Program* ps) {
    vs_ = &vs;
    const uint32_t object_size = 872 + vs.data_offset;
    const uint32_t ps_size = ps ? 40 + ps->data_offset : 0;
    if (object_size + vs.data_size + ps_size + 0x1000 > kObjectArea) return false;
    vs_object_ = scratch_;
    vs_data_ = (vs_object_ + object_size + 0xFFF) & ~0xFFFu;
    ps_object_ = ps ? (vs_data_ + vs.data_size + 0xF) & ~0xFu : 0;
    std::memset(base_ + vs_object_, 0, 872);
    std::memcpy(base_ + vs_object_ + 872, vs.blob, vs.data_offset);
    std::memcpy(base_ + vs_data_, vs.blob + vs.data_offset, vs.data_size);
    if (ps) {
      std::memset(base_ + ps_object_, 0, 40);
      std::memcpy(base_ + ps_object_ + 40, ps->blob, ps->data_offset);
    }
    {
      PPCContext& ctx = Context(vs_object_, vs_data_, 0, 0, 0);
      sub_82417B78(ctx, base_);
    }
    PutBe32(base_ + vs_object_, Be32(base_ + vs_object_) | 0x100000);
    used_ = (ps ? ps_object_ + ps_size : vs_data_ + vs.data_size) - scratch_;
    snapshot_.assign(base_ + scratch_, base_ + scratch_ + used_);
    return true;
  }

  // The shader the game binds for this layout: its fingerprint, and its microcode on request.
  uint64_t Bind(const Layout& layout, std::vector<uint8_t>* ucode = nullptr) {
    std::memcpy(base_ + scratch_, snapshot_.data(), used_);
    // The declaration object: element count, highest stream, a non-zero id (so that none is
    // taken from the game's counter), then its elements and the end marker.
    const uint32_t decl = scratch_ + kObjectArea;
    std::memset(base_ + decl, 0, 52);
    PutBe32(base_ + decl + 24, uint32_t(layout.elements.size()));
    uint32_t streams = 0;
    for (const Element& e : layout.elements) streams = std::max<uint32_t>(streams, e.stream);
    PutBe32(base_ + decl + 28, streams);
    PutBe32(base_ + decl + 48, 1);
    uint8_t* el = base_ + decl + 52;
    for (const Element& e : layout.elements) {
      el[0] = 0; el[1] = e.stream; el[2] = uint8_t(e.offset >> 8); el[3] = uint8_t(e.offset);
      PutBe32(el + 4, decl_types_[e.type]);
      el[8] = 0; el[9] = e.usage; el[10] = e.index; el[11] = 0;
      el += 12;
    }
    el[0] = 0; el[1] = 0xFF; el[2] = 0; el[3] = 0;
    PutBe32(el + 4, 0xFFFFFFFF);
    el[8] = el[9] = el[10] = el[11] = 0;
    const uint32_t strides = decl + 52 + 12 * 64;
    for (uint32_t i = 0; i < 16; ++i)
      PutBe32(base_ + strides + 4 * i, i < layout.strides.size() ? layout.strides[i] : 0);
    {
      PPCContext& ctx = Context(vs_object_, 0, decl, strides, ps_object_);
      sub_82424EB0(ctx, base_);
    }
    const uint8_t* code = base_ + vs_data_ + vs_->literal_bytes;
    if (ucode) ucode->assign(code, code + vs_->ucode_bytes);
    return Hash(code, vs_->ucode_bytes);
  }

 private:
  static constexpr uint32_t kObjectArea = 0x10000;
  static constexpr uint32_t kScratchSize = kObjectArea + 0x2000 + 0x10000;  // + decl, + stack

  // A fresh context for a call: the arguments, and a stack in the scratch memory.
  PPCContext& Context(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7) {
    ctx_ = PPCContext{};
    ctx_.r1.u64 = scratch_ + kScratchSize - 0x100;
    ctx_.r3.u64 = r3;
    ctx_.r4.u64 = r4;
    ctx_.r5.u64 = r5;
    ctx_.r6.u64 = r6;
    ctx_.r7.u64 = r7;
    return ctx_;
  }

  rex::memory::Memory* memory_;
  uint8_t* base_;
  uint32_t decl_types_[kTypes];
  uint32_t scratch_ = 0;
  uint32_t vs_object_ = 0, vs_data_ = 0, ps_object_ = 0, used_ = 0;
  const Program* vs_ = nullptr;
  std::vector<uint8_t> snapshot_;
  alignas(64) PPCContext ctx_{};
};

// ---------------------------------------------------------------------------------------------
// The pack (shaderpack/545407F8.shaderpack, made by launcher/dev/shaderpack/build_pack.py).

struct PackShader {
  uint64_t hash;
  uint8_t kind;  // 0 vertex shader bound from a program, 1 program as it is, 2 default.xex range
  uint8_t pixel;
  uint32_t dwords;
  uint64_t source;
  std::vector<uint64_t> pixel_shaders;
};
struct PackList {
  std::string name;
  std::array<uint8_t, 12> header;
  uint32_t record_size;
  std::vector<std::vector<uint8_t>> records;
};
struct Pack {
  uint64_t rpf_key_fingerprint, decl_types_fingerprint;
  uint32_t versions[3];
  std::vector<PackShader> shaders;
  std::vector<PackList> lists;
};

Pack ReadPack(const std::filesystem::path& path) {
  std::vector<uint8_t> d;
  if (!ReadFile(path, d)) throw Failure{2, "no shader pack at " + path.string()};
  auto bad = [] { return Failure{3, "the shader pack is damaged"}; };
  if (d.size() < 64 || std::memcmp(d.data(), "MCLASPK1", 8) != 0) throw bad();
  if (Hash(d.data(), d.size() - 8) != Le64(d.data() + d.size() - 8)) throw bad();
  size_t p = 8;
  auto need = [&](size_t n) {
    if (p + n > d.size() - 8) throw bad();
  };
  need(44);
  if (Le32(d.data() + p) != 1) throw Failure{3, "the shader pack's format is newer than this game"};
  if (Le32(d.data() + p + 4) != kTitleId) throw Failure{3, "the shader pack is for another game"};
  Pack pack;
  for (int i = 0; i < 3; ++i) pack.versions[i] = Le32(d.data() + p + 8 + 4 * i);
  pack.rpf_key_fingerprint = Le64(d.data() + p + 20);
  pack.decl_types_fingerprint = Le64(d.data() + p + 28);
  const uint32_t shader_count = Le32(d.data() + p + 36), list_count = Le32(d.data() + p + 40);
  p += 44;
  for (uint32_t i = 0; i < shader_count; ++i) {
    need(24);
    PackShader s;
    s.hash = Le64(d.data() + p);
    s.kind = d[p + 8];
    s.pixel = d[p + 9];
    const uint8_t ps_count = d[p + 10];
    s.dwords = Le32(d.data() + p + 12);
    s.source = Le64(d.data() + p + 16);
    p += 24;
    need(8 * size_t(ps_count));
    for (uint8_t k = 0; k < ps_count; ++k, p += 8) s.pixel_shaders.push_back(Le64(d.data() + p));
    if (s.kind > 2 || s.pixel > 1) throw bad();
    pack.shaders.push_back(std::move(s));
  }
  for (uint32_t i = 0; i < list_count; ++i) {
    need(16 + 12 + 8);
    PackList l;
    l.name.assign(reinterpret_cast<const char*>(d.data() + p), strnlen(reinterpret_cast<const char*>(d.data() + p), 16));
    std::memcpy(l.header.data(), d.data() + p + 16, 12);
    l.record_size = Le32(d.data() + p + 28);
    const uint32_t records = Le32(d.data() + p + 32);
    p += 36;
    if (l.record_size != 72 && l.record_size != 68) throw bad();
    need(size_t(records) * l.record_size);
    for (uint32_t r = 0; r < records; ++r, p += l.record_size) {
      l.records.emplace_back(d.data() + p, d.data() + p + l.record_size);
      if (Hash(l.records.back().data() + 8, l.record_size - 8) != Le64(l.records.back().data()))
        throw bad();
    }
    pack.lists.push_back(std::move(l));
  }
  if (p != d.size() - 8) throw bad();
  return pack;
}

// ---------------------------------------------------------------------------------------------
// The storage files.

struct Rebuilt {
  uint64_t hash;
  bool pixel;
  std::vector<uint8_t> ucode;  // big-endian, as the storage holds it
};

// A file of records after a 12- (pipelines) or 8-byte (shaders) header: the valid ones as they
// are (as the GPU loads them: up to the first one whose hash doesn't match), and their keys.
struct StorageFile {
  bool exists = false, header_ok = false;
  std::vector<uint8_t> valid;  // header and valid records
  std::unordered_set<uint64_t> shader_hashes;
  std::set<std::vector<uint8_t>> records;
};

StorageFile ReadShaderStorage(const std::filesystem::path& path) {
  StorageFile f;
  std::vector<uint8_t> d;
  f.exists = ReadFile(path, d);
  if (!f.exists || d.size() < 8 || Le32(d.data()) != 0x48534558 ||
      Be32(d.data() + 4) != rex::graphics::shader_storage::kShaderStorageVersion) {
    return f;
  }
  f.header_ok = true;
  size_t p = 8;
  while (p + 12 <= d.size()) {
    const uint64_t hash = Le64(d.data() + p);
    const uint32_t dwords = Le32(d.data() + p + 8) & 0x7FFFFFFF;
    if (p + 12 + size_t(dwords) * 4 > d.size() || Hash(d.data() + p + 12, size_t(dwords) * 4) != hash)
      break;
    f.shader_hashes.insert(hash);
    p += 12 + size_t(dwords) * 4;
  }
  f.valid.assign(d.begin(), d.begin() + p);
  return f;
}

StorageFile ReadPipelineStorage(const std::filesystem::path& path, const uint8_t header[12],
                                uint32_t record_size) {
  StorageFile f;
  std::vector<uint8_t> d;
  f.exists = ReadFile(path, d);
  if (!f.exists || d.size() < 12 || std::memcmp(d.data(), header, 12) != 0) return f;
  f.header_ok = true;
  size_t p = 12;
  while (p + record_size <= d.size() &&
         Hash(d.data() + p + 8, record_size - 8) == Le64(d.data() + p)) {
    f.records.emplace(d.data() + p, d.data() + p + record_size);
    p += record_size;
  }
  f.valid.assign(d.begin(), d.begin() + p);
  return f;
}

void WriteAtomically(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  auto temp = path;
  temp += ".prepare.tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size())))
      throw Failure{6, "can't write " + temp.string()};
    out.flush();
    if (!out) throw Failure{6, "can't write " + temp.string()};
  }
#ifdef _WIN32
  if (HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      h != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(h);
    CloseHandle(h);
  }
  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw Failure{6, "can't replace " + path.string()};
#else
  std::error_code error;
  std::filesystem::rename(temp, path, error);
  if (error) throw Failure{6, "can't replace " + path.string()};
#endif
}

// ---------------------------------------------------------------------------------------------

struct State {
  bool active = false;
  bool then_play = false;  // the PS5: the game goes on after preparing (there is no launcher)
  Clock::time_point preload_start;
  long long rebuild_ms = 0;
};
State g_state;

std::filesystem::path FindPack() {
  std::string option = REXCVAR_GET(mcla_shader_pack);
  if (!option.empty()) return std::filesystem::path(option);
  std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
  const auto relative = std::filesystem::path("shaderpack") / "545407F8.shaderpack";
  for (int level = 0; level < 6 && !folder.empty(); ++level) {
    std::error_code error;
    if (std::filesystem::exists(folder / relative, error)) return folder / relative;
    if (folder == folder.parent_path()) break;
    folder = folder.parent_path();
  }
  return rex::filesystem::GetExecutableFolder() / relative;
}

void Prepare(rex::Runtime* runtime) {
  const auto start = Clock::now();
  const std::filesystem::path pack_path = FindPack();
  const Pack pack = ReadPack(pack_path);
  namespace ss = rex::graphics::shader_storage;
  if (pack.versions[0] != ss::kShaderStorageVersion ||
      pack.versions[1] != ss::kD3D12PipelineStorageVersion ||
      pack.versions[2] != ss::kVulkanPipelineStorageVersion) {
    throw Failure{5, "the shader pack was made for another version of the graphics code; "
                     "update the game, or wait for a pack for this version"};
  }
  Line("start " + std::to_string(pack.shaders.size()));
  REXLOG_INFO("prepare shaders: {} ({} shaders, {} pipeline lists)", pack_path.string(),
              pack.shaders.size(), pack.lists.size());

  auto* memory = runtime->memory();
  uint8_t* const base = memory->virtual_membase();
  if (Hash(base + kRpfKeyAddress, 32) != pack.rpf_key_fingerprint ||
      Hash(base + kDeclTypeTableAddress, 64) != pack.decl_types_fingerprint) {
    throw Failure{4, "this default.xex is not the one the shader pack was made for "
                     "(Midnight Club: Los Angeles Complete Edition, USA/Europe)"};
  }
  uint8_t key[32];
  std::memcpy(key, base + kRpfKeyAddress, 32);
  uint32_t decl_types[kTypes];
  for (int t = 0; t < kTypes; ++t)
    decl_types[t] = Be32(base + kDeclTypeTableAddress + 4 * kTypeTableIndex[t]);

  // The player's effects and presets, and the programs in them and in default.xex.
  const auto files =
      ReadShaderFiles(runtime->game_data_root() / "xarchive_cache.rpf", key);
  const uint32_t fxl_final = NameHash("fxl_final");
  std::unordered_map<uint64_t, Program> programs;
  std::vector<uint32_t> preset_masks;
  for (const auto& [file, data] : files) {
    if (file.path.size() >= 2 && file.path[file.path.size() - 2] == fxl_final) {
      FindPrograms(data.data(), data.size(), programs);
    } else if (file.path.size() >= 2 && file.path[file.path.size() - 2] == kPresetFolderHash) {
      // "<channel mask> ; (version 2) see grcore/fvfchannels.h ..."
      uint32_t mask = 0;
      size_t i = 0;
      while (i < data.size() && data[i] == ' ') ++i;
      bool digits = false;
      for (; i < data.size() && data[i] >= '0' && data[i] <= '9'; ++i, digits = true)
        mask = mask * 10 + (data[i] - '0');
      if (digits && mask && std::find(preset_masks.begin(), preset_masks.end(), mask) == preset_masks.end())
        preset_masks.push_back(mask);
    }
  }
  std::sort(preset_masks.begin(), preset_masks.end());
  // The executable's image: the effects built into the game, and the console's own shaders.
  FindPrograms(base + kImageBase, kImageSize, programs);
  if (programs.empty() || preset_masks.empty())
    throw Failure{4, "no shaders found in the game's files"};
  REXLOG_INFO("prepare shaders: {} programs and {} vertex layouts of presets in the game's files",
              programs.size(), preset_masks.size());

  GuestBinder binder(memory, decl_types);
  if (!binder.ok()) throw Failure{6, "no guest memory for binding shaders"};

  // Shaders already in the storage (an earlier preparation, or play) aren't rebuilt again; the
  // development check rebuilds all of them.
  const bool check_only = !std::string(REXCVAR_GET(mcla_prepare_shaders_check)).empty();
  const auto folder = runtime->cache_root() / "shaders" / "shareable";
  const std::string title = fmt::format("{:08X}", kTitleId);
  std::unordered_set<uint64_t> stored;
  if (!check_only) stored = ReadShaderStorage(folder / (title + ".xsh")).shader_hashes;

  std::vector<Rebuilt> rebuilt;
  std::vector<std::pair<uint64_t, bool>> results;
  size_t done = 0, failed = 0;
  const size_t total = pack.shaders.size();
  for (const PackShader& s : pack.shaders) {
    bool ok = false;
    if (stored.count(s.hash)) {
      ok = true;
    } else if (s.kind == 1) {
      auto it = programs.find(s.source);
      if (it != programs.end() && it->second.ucode_bytes == s.dwords * 4 &&
          Hash(it->second.ucode(), it->second.ucode_bytes) == s.hash) {
        rebuilt.push_back({s.hash, bool(s.pixel), {it->second.ucode(), it->second.ucode() + it->second.ucode_bytes}});
        ok = true;
      }
    } else if (s.kind == 2) {
      if (s.source >= kImageBase && s.source + uint64_t(s.dwords) * 4 <= uint64_t(kImageBase) + kImageSize &&
          Hash(base + s.source, size_t(s.dwords) * 4) == s.hash) {
        rebuilt.push_back({s.hash, bool(s.pixel), {base + s.source, base + s.source + s.dwords * 4}});
        ok = true;
      }
    } else {
      auto vs = programs.find(s.source);
      if (vs != programs.end() && vs->second.vertex && vs->second.ucode_bytes == s.dwords * 4) {
        const auto inputs = vs->second.Inputs();
        for (uint64_t ps_hash : s.pixel_shaders.empty() ? std::vector<uint64_t>{0} : s.pixel_shaders) {
          const Program* ps = nullptr;
          if (ps_hash) {
            auto it = programs.find(ps_hash);
            if (it == programs.end() || it->second.vertex) continue;
            ps = &it->second;
          }
          if (!binder.Prepare(vs->second, ps)) continue;
          // Layouts that differ only in what this shader doesn't read give the same shader.
          std::set<std::pair<std::vector<Element>, std::vector<uint8_t>>> seen;
          ok = ForEachLayout(preset_masks, inputs, [&](const Layout& layout) {
            std::pair<std::vector<Element>, std::vector<uint8_t>> key;
            for (const Element& e : layout.elements) {
              for (auto [usage, index] : inputs)
                if (e.usage == usage && e.index == index) {
                  key.first.push_back(e);
                  break;
                }
            }
            key.second = layout.strides;
            if (!seen.insert(key).second) return false;
            Layout used{key.first, key.second};
            if (binder.Bind(used) != s.hash) return false;
            Rebuilt r{s.hash, false, {}};
            binder.Bind(used, &r.ucode);
            rebuilt.push_back(std::move(r));
            return true;
          });
          if (ok) break;
        }
      }
    }
    failed += !ok;
    results.push_back({s.hash, ok});
    if (++done % 25 == 0 && done < total) Line("rebuild " + std::to_string(done) + " " + std::to_string(total));
  }
  g_state.rebuild_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  Line("rebuild-done " + std::to_string(total - failed) + " " + std::to_string(failed) + " " +
       std::to_string(g_state.rebuild_ms));
  REXLOG_INFO("prepare shaders: {} of {} shaders rebuilt in {} ms", total - failed, total,
              g_state.rebuild_ms);

  // Development: the reachability check of a new pack (launcher/dev/shaderpack).
  if (std::string report = REXCVAR_GET(mcla_prepare_shaders_check); !report.empty()) {
    std::ofstream out(report, std::ios::trunc);
    for (auto [hash, ok] : results) out << fmt::format("{:016X} {}\n", hash, ok ? "ok" : "failed");
    out.close();
    Quit(out ? 0 : 6);
  }

  // Merge into the storage.
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  size_t shaders_added = 0, pipelines_added = 0;
  std::unordered_set<uint64_t> available;
  {
    const auto path = folder / (title + ".xsh");
    StorageFile f = ReadShaderStorage(path);
    std::vector<uint8_t> out = f.header_ok ? f.valid : std::vector<uint8_t>{};
    if (!f.header_ok) {
      if (f.exists) REXLOG_WARN("prepare shaders: {} is from another version and is replaced", path.string());
      PutLe32(out, 0x48534558);
      PutLe32(out, ByteSwap32(ss::kShaderStorageVersion));
    }
    available = f.shader_hashes;
    for (const Rebuilt& r : rebuilt) {
      if (!available.insert(r.hash).second) continue;
      PutLe64(out, r.hash);
      PutLe32(out, uint32_t(r.ucode.size() / 4) | (r.pixel ? 0x80000000u : 0));
      out.insert(out.end(), r.ucode.begin(), r.ucode.end());
      ++shaders_added;
    }
    if (shaders_added || !f.header_ok || f.valid.size() != std::filesystem::file_size(path, error))
      WriteAtomically(path, out);
  }
  for (const PackList& list : pack.lists) {
    const auto path = folder / (title + "." + list.name);
    StorageFile f = ReadPipelineStorage(path, list.header.data(), list.record_size);
    std::vector<uint8_t> out = f.header_ok ? f.valid : std::vector<uint8_t>(list.header.begin(), list.header.end());
    if (!f.header_ok && f.exists)
      REXLOG_WARN("prepare shaders: {} is from another version and is replaced", path.string());
    size_t added = 0;
    for (const auto& record : list.records) {
      // Only pipelines whose shaders are in the storage.
      const uint64_t vs = Le64(record.data() + 8), ps = Le64(record.data() + 24);
      if (!available.count(vs) || (ps && !available.count(ps))) continue;
      if (f.records.count(record)) continue;
      out.insert(out.end(), record.begin(), record.end());
      ++added;
    }
    pipelines_added += added;
    if (added || !f.header_ok || f.valid.size() != std::filesystem::file_size(path, error))
      WriteAtomically(path, out);
  }
  Line("merge " + std::to_string(shaders_added) + " " + std::to_string(pipelines_added));
  REXLOG_INFO("prepare shaders: {} shaders and {} pipelines added to {}", shaders_added,
              pipelines_added, folder.string());
}

}  // namespace

bool MclaPrepareShadersIsRequested() { return REXCVAR_GET(mcla_prepare_shaders); }

// Called before the module starts, before the SDK loads the storage (MclaApp::OnPreLaunchModule;
// on the PS5, main_ps5.cpp): prepares the storage, which the SDK then loads, creating every
// pipeline (its progress on standard output). With then_play (the PS5), a failure is reported and
// the game starts as it would have; otherwise the process ends with the failure's code.
bool MclaPrepareShadersBeforeLaunch(rex::Runtime* runtime, rex::ui::Window* window, bool then_play) {
  if (!REXCVAR_GET(mcla_prepare_shaders)) return false;
  g_state.active = true;
  g_state.then_play = then_play;
#ifdef _WIN32
  if (window && !then_play) {
    if (HWND hwnd = static_cast<HWND>(window->GetNativeWindowHandle())) ShowWindow(hwnd, SW_HIDE);
  }
#else
  (void)window;
#endif
  Failure failure{0, {}};
  try {
    Prepare(runtime);
  } catch (const Failure& f) {
    failure = f;
  } catch (const std::exception& error) {
    failure = {6, std::string("unexpected error: ") + error.what()};
  }
  if (failure.code) {
    if (!then_play) Fail(failure);
    REXLOG_ERROR("prepare shaders: {}", failure.message);
    Line("error " + std::to_string(failure.code) + " " + failure.message);
    g_state.active = false;
    return false;
  }
  if (rex::cvar::GetFlagSource("shader_storage_progress") == rex::cvar::Source::kDefault) {
    rex::cvar::SetFlagByName("shader_storage_progress", "50");
  }
  g_state.preload_start = Clock::now();
  return true;
}

// Called once the SDK created the stored pipelines (MclaApp::OnPostLaunchModule): reports and
// quits before the game starts.
void MclaPrepareShadersAfterPreload() {
  if (!g_state.active) return;
  const auto preload_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_state.preload_start)
          .count();
  double private_mb = 0, gpu_mb = 0;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters))) {
    private_mb = counters.PrivateUsage / 1048576.0;
  }
  IDXGIFactory4* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
      IDXGIAdapter3* adapter3 = nullptr;
      DXGI_QUERY_VIDEO_MEMORY_INFO info{};
      if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))) &&
          SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        gpu_mb += info.CurrentUsage / 1048576.0;
      }
      if (adapter3) adapter3->Release();
      adapter->Release();
    }
    factory->Release();
  }
#endif
  Line(fmt::format("done {} {} {:.0f} {:.0f}", g_state.rebuild_ms, preload_ms, private_mb, gpu_mb));
  REXLOG_INFO("prepare shaders: done; rebuild {} ms, pipelines {} ms, {:.0f} MB private memory, "
              "{:.0f} MB GPU memory",
              g_state.rebuild_ms, preload_ms, private_mb, gpu_mb);
  g_state.active = false;
  if (!g_state.then_play) Quit(0);
}
