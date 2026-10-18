// Shader preparation and the shader pack's import
//
// The storage (<cache>/shaders/shareable) keeps every pipeline the game has created, for the next
// start. The shader pack the fork ships (shaderpack/545407F8.shaderpack) lets a player's storage
// start out with what the fork's test sessions reached:
// - at every start (mcla_shader_pack_auto, on by default), before the storage loads, whatever of
//   the pack the storage lacks is rebuilt from the player's own game files and added (about 5 ms
//   when nothing is missing, a few seconds when shaders must be rebuilt);
// - with --mcla_prepare_shaders=true (the launcher, after a build, before Play when the driver,
//   settings or storage changed, and from More), the same, then every stored pipeline is created
//   with the settings the game is played with, so the graphics driver compiles them now, and the
//   game quits before its code starts.
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
// The player's storage is protected:
// - Every mcla process holds a session lock (<cache>/shaders/local/545407F8.lock) from before it
//   reads the storage until it ends: a preparation refuses to run beside a game (code 7), and a
//   game started during a preparation waits for it.
// - A file that exists but can't be read stops the import before anything is written: it is
//   never taken for a missing one.
// - The pack's lists are accepted only as the four this build knows (names, headers and record
//   sizes from the SDK's own constants); paths never come from the pack.
// - Entries already in a file stay, in their order; each file is written beside the old one,
//   flushed and renamed over it.
// - A journal (<cache>/shaders/local/545407F8.import) records each file's length before an import.
//   If the process ends after the import and before the stored pipelines were all created, the
//   next start cuts the files back to those lengths. After two such failed starts with the same
//   pack, importing that pack is disabled (the player's own entries stay); More > Prepare shaders
//   (--mcla_prepare_shaders_retry) or deleting the journal tries again.
// - After the stored pipelines were created, a stamp (<cache>/shaders/local/545407F8.stamp.json)
//   records what was loaded: the pack, the storage versions, the API, the pipeline storage file
//   in use (which also says the EDRAM path), the resolution scale, the adapter and its driver,
//   the files' sizes. The launcher compares it with the current driver, settings and files to
//   decide whether to prepare again before Play.
//
// Standard output gets lines for the launcher, all starting with "SHADERPREP ":
//   start <shaders to rebuild>               rebuild <done> <total>
//   rebuild-done <ok> <failed> <ms>          merge <shaders added> <pipelines added>
//   rollback <files cut back>                import-disabled <pack> <failed starts>
//   preload <done> <total>  /  preload-done <created> <total> <ms>   (from the GPU plugin)
//   done <rebuild ms> <preload ms> <private MB> <GPU MB> <process ms>
//   error <code> <message>                   (then the process ends with that code)
// Exit codes: 0 done; permanent: 2 no pack, 3 damaged pack, 4 game files don't match, 5 pack for
// another version of the graphics code, 6 storage can't be read or written; transient: 7 storage
// in use by another program (try again later).
//
// Nothing else is written: not mcla.toml (the overrides here are command-line values, which the
// settings file never saves), not the save.

#include "generated/default/mcla_init.h"

#include "mcla_app.h"
#include "shader_storage_files.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
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
#include <rex/system/interfaces/graphics.h>
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
#else
#include <unistd.h>
#endif

REXCVAR_DEFINE_BOOL(mcla_prepare_shaders, false, "MCLA",
                    "Prepare the shader storage from the shader pack, create every stored "
                    "pipeline with the current settings, then quit (the launcher runs this; on PC "
                    "only taken from the command line)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_prepare_shaders_retry, false, "MCLA",
                    "With --mcla_prepare_shaders: also import a shader pack whose import was "
                    "disabled after failed starts (the launcher's More > Prepare shaders)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_shader_pack_auto, true, "MCLA",
                    "At every start, add whatever of the shader pack the shader storage lacks "
                    "before it loads")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(mcla_shader_pack, "", "MCLA",
                      "The shader pack (default: shaderpack/545407F8.shaderpack in the repository "
                      "the game is built in)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(mcla_shader_pack_test_crash, 0, "MCLA",
                     "Development: end the process as a crash would, 1 right after the pack's "
                     "import, 2 while its shaders are rebuilt (tests the rollback)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(mcla_prepare_shaders_check, "", "MCLA",
                      "Development: with --mcla_prepare_shaders, rebuild all the pack's shaders, "
                      "write whether each one was rebuilt to this file and quit, without "
                      "changing the storage")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The game's Direct3D: the vertex shader object's constructor and IDirect3DVertexShader9::Bind.
REX_EXTERN(sub_82417B78);
REX_EXTERN(sub_82424EB0);

namespace {

namespace ss = rex::graphics::shader_storage;
using mcla::shader_storage::Failure;
using mcla::shader_storage::kBadPack;
using mcla::shader_storage::kInUse;
using mcla::shader_storage::kIoError;
using mcla::shader_storage::kNoPack;
using mcla::shader_storage::kOtherGame;
using mcla::shader_storage::kOtherVersions;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kTitleId = 0x545407F8;
constexpr char kTitle[] = "545407F8";
constexpr uint32_t kImageBase = 0x82000000;
constexpr uint32_t kImageSize = 0x009E0000;
// In the decrypted default.xex (title update 0.0.0.8): the key of the archives' file tables, and
// the game's table from its mesh vertex data types to Direct3D vertex declaration types.
constexpr uint32_t kRpfKeyAddress = 0x827D8AC4;
constexpr uint32_t kDeclTypeTableAddress = 0x827D42B0;
// The folder of the effects' preset files (.sps), by its name's hash: its name isn't known.
constexpr uint32_t kPresetFolderHash = 0x6D1F986C;
// Failed starts with the same pack after which its import is disabled.
constexpr uint32_t kFailuresBeforeDisabling = 2;

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

// ---------------------------------------------------------------------------------------------
// Raw deflate (RFC 1951), for the archive's compressed files. After Mark Adler's puff.c (zlib's
// contrib/puff, zlib licence; THIRD-PARTY-NOTICES.md): canonical Huffman codes decoded from the
// count of codes of each length, without tables. Rewritten for this file, not a copy.

class Inflater {
 public:
  // max_out: the size the output must not pass (the file's size in the archive's table).
  Inflater(const uint8_t* in, size_t size, size_t max_out)
      : in_(in), size_(size), max_out_(max_out) {}

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
    if (len != (~nlen & 0xFFFF) || pos_ + len > size_ || out_->size() + len > max_out_) return false;
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
      if (out_->size() >= max_out_ && symbol != 256) return false;
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
        if (failed_ || dist > out_->size() || out_->size() + len > max_out_) return false;
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
  size_t max_out_;
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

// The plain (not resource) files under shaders/: the effects and the preset files. Defensive
// against a damaged or unusual archive: every child range inside the table and after its parent,
// each entry visited once, sizes within the archive, decompression bounded by the stated size.
std::vector<std::pair<ArchiveFile, std::vector<uint8_t>>> ReadShaderFiles(
    const std::filesystem::path& archive_path, const uint8_t key[32]) {
  std::ifstream file(archive_path, std::ios::binary);
  if (!file) throw Failure{kOtherGame, "can't open " + archive_path.string()};
  file.seekg(0, std::ios::end);
  const uint64_t archive_size = uint64_t(std::max<std::streamoff>(file.tellg(), 0));
  file.seekg(0);
  uint8_t header[20];
  if (!file.read(reinterpret_cast<char*>(header), 20) || std::memcmp(header, "RPF3", 4) != 0)
    throw Failure{kOtherGame, archive_path.filename().string() + " is not the expected archive"};
  const uint32_t table_size = Le32(header + 4), count = Le32(header + 8);
  if (count == 0 || table_size > (64u << 20) || uint64_t(count) * 16 > table_size ||
      0x800 + uint64_t(table_size) > archive_size)
    throw Failure{kOtherGame, archive_path.filename().string() + " has an unexpected file table"};
  std::vector<uint8_t> table(table_size);
  file.seekg(0x800);
  if (!file.read(reinterpret_cast<char*>(table.data()), table_size))
    throw Failure{kOtherGame, "can't read the file table of " + archive_path.filename().string()};
  if (Le32(header + 16)) DecryptTable(table, key);
  auto entry = [&](uint32_t i) {
    std::array<uint32_t, 4> e;
    for (int k = 0; k < 4; ++k) e[k] = Le32(table.data() + size_t(i) * 16 + k * 4);
    return e;
  };
  // A folder's children: inside the table, after the folder itself.
  auto children = [&](uint32_t index, const std::array<uint32_t, 4>& e, uint32_t& first,
                      uint32_t& n) {
    first = e[2] & 0x7FFFFFFF;
    n = e[3];
    return first > index && uint64_t(first) + n <= count;
  };
  const auto root = entry(0);
  uint32_t first = 0, n = 0;
  if (!(root[2] & 0x80000000u) || !children(0, root, first, n))
    throw Failure{kOtherGame, "the archive's file table can't be read"};
  const uint32_t shaders = NameHash("shaders");
  std::vector<std::pair<ArchiveFile, std::vector<uint8_t>>> out;
  std::vector<bool> visited(count, false);
  visited[0] = true;
  // Depth-first from shaders/, at most three levels below it.
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> stack;
  for (uint32_t c = first; c < first + n; ++c) {
    if (entry(c)[0] == shaders) stack.push_back({c, {shaders}});
  }
  constexpr uint32_t kMaxFileSize = 64u << 20;
  while (!stack.empty()) {
    auto [index, path] = stack.back();
    stack.pop_back();
    if (visited[index]) continue;
    visited[index] = true;
    const auto e = entry(index);
    if (e[2] & 0x80000000u) {
      if (path.size() > 4 || !children(index, e, first, n)) continue;
      for (uint32_t c = first; c < first + n; ++c) {
        auto child = path;
        child.push_back(entry(c)[0]);
        stack.push_back({c, std::move(child)});
      }
      continue;
    }
    const uint32_t kind = e[3] >> 30;
    if (kind & 2) continue;  // a resource: not needed
    const uint64_t stored_size = kind == 1 ? (e[3] & 0x3FFFFFFF) : e[1];
    if (e[1] > kMaxFileSize || uint64_t(e[2]) + stored_size > archive_size)
      throw Failure{kOtherGame, "a file in " + archive_path.filename().string() + " is damaged"};
    std::vector<uint8_t> data;
    file.seekg(std::streamoff(e[2]));
    if (kind == 1) {
      std::vector<uint8_t> packed(static_cast<size_t>(stored_size));
      if (!file.read(reinterpret_cast<char*>(packed.data()), std::streamsize(packed.size())))
        throw Failure{kOtherGame, "can't read " + archive_path.filename().string()};
      Inflater inflater(packed.data(), packed.size(), e[1]);
      if (!inflater.Run(data) || data.size() != e[1])
        throw Failure{kOtherGame,
                      "a file in " + archive_path.filename().string() + " doesn't decompress"};
    } else {
      data.resize(e[1]);
      if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())))
        throw Failure{kOtherGame, "can't read " + archive_path.filename().string()};
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
// which starts with the literals' and the microcode's sizes. Every offset is checked in 64 bits.

struct Program {
  const uint8_t* blob = nullptr;  // the blob's header part [0, data offset), then its data
  uint32_t data_offset = 0, data_size = 0, info_offset = 0;
  uint32_t literal_bytes = 0, ucode_bytes = 0;
  bool vertex = false;
  const uint8_t* ucode() const { return blob + data_offset + literal_bytes; }
  // A vertex shader's inputs: (usage, usage index) each, from the program info's input table.
  std::vector<std::pair<uint8_t, uint8_t>> Inputs() const {
    std::vector<std::pair<uint8_t, uint8_t>> inputs;
    if (uint64_t(info_offset) + 32 > data_offset) return inputs;
    const uint8_t* info = blob + info_offset;
    const uint64_t count = Be32(info + 28), first = uint64_t(Be32(info + 24)) + 9;
    if (count > 32 || uint64_t(info_offset) + (first + count) * 4 > data_offset) return inputs;
    for (uint64_t i = 0; i < count; ++i) {
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
        p.data_size >= 0x40000 || uint64_t(b) + p.data_offset + p.data_size > size ||
        uint64_t(p.info_offset) + 8 > p.data_offset) {
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
  // object, and keeps copies to start every attempt from. False for a program whose header the
  // game's code can't take safely: only the microcode is covered by the fingerprint, and the
  // header's counts drive the guest's writes (at most 32 vertex fetches, the info's tables inside
  // the header).
  bool Prepare(const Program& vs, const Program* ps) {
    vs_ = &vs;
    if (!HeaderIsSafe(vs) || (ps && !HeaderIsSafe(*ps))) return false;
    const uint32_t object_size = 872 + vs.data_offset;
    const uint32_t ps_size = ps ? 40 + ps->data_offset : 0;
    // 4 KB alignment of the data, 16 bytes of the pixel shader object.
    if (uint64_t(object_size) + vs.data_size + ps_size + 0x1010 > kObjectArea) return false;
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
  static bool HeaderIsSafe(const Program& p) {
    const uint8_t* info = p.blob + p.info_offset;
    if (uint64_t(p.info_offset) + 32 > p.data_offset) return false;
    if (!p.vertex) return true;
    // A vertex shader's fetch table, which the constructor copies 12 bytes an entry from: at most
    // 32 entries, inside the header (in the game's own programs it can end right at the data).
    const uint64_t fetches = Be32(info + 28), first = Be32(info + 24);
    return fetches <= 32 && uint64_t(p.info_offset) + 4 * (9 + first + fetches) <= p.data_offset;
  }

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
// The storage's files. The pipeline lists this build knows, with the header the SDK writes:
// 'XEPS', the API's magic, the version byte-swapped; then records of an XXH3 and a description.
// Nothing about a list (its file name, header or record size) is ever taken from the pack.

struct ListSpec {
  const char* name;
  uint32_t magic_api;
  uint32_t version;
  uint32_t record_size;
};
constexpr ListSpec kLists[] = {
    {"rtv.d3d12.xpso", 0x54525844, ss::kD3D12PipelineStorageVersion, 72},   // 'DXRT'
    {"rov.d3d12.xpso", 0x4F525844, ss::kD3D12PipelineStorageVersion, 72},   // 'DXRO'
    {"fbo.vk.xpso", 0, ss::kVulkanPipelineStorageVersion, 68},              // framebuffer path
    {"fsi.vk.xpso", 1, ss::kVulkanPipelineStorageVersion, 68},              // interlock path
};
constexpr size_t kListCount = std::size(kLists);

std::array<uint8_t, 12> ListHeader(const ListSpec& spec) {
  std::array<uint8_t, 12> h{};
  const uint32_t words[3] = {0x53504558, spec.magic_api, ByteSwap32(spec.version)};
  for (int w = 0; w < 3; ++w)
    for (int i = 0; i < 4; ++i) h[w * 4 + i] = uint8_t(words[w] >> (8 * i));
  return h;
}

// The files the import writes, by their name after "<title>.".
std::vector<std::string> StorageFileNames() {
  std::vector<std::string> names = {"xsh"};
  for (const ListSpec& spec : kLists) names.push_back(spec.name);
  return names;
}

struct Folders {
  std::filesystem::path shareable;  // the storage the SDK loads
  std::filesystem::path local;      // this machine's: lock, journal, stamp
  std::filesystem::path File(const std::string& name) const {
    return shareable / (std::string(kTitle) + "." + name);
  }
  std::filesystem::path Local(const std::string& name) const {
    return local / (std::string(kTitle) + "." + name);
  }
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
  size_t spec;  // in kLists
  std::vector<std::vector<uint8_t>> records;
};
struct Pack {
  uint64_t hash = 0;  // the XXH3 the file ends with
  uint64_t rpf_key_fingerprint = 0, decl_types_fingerprint = 0;
  uint32_t versions[3] = {};
  std::vector<PackShader> shaders;
  std::vector<PackList> lists;
};

std::filesystem::path FindPack() {
  std::string option = rex::cvar::GetFlagByName("mcla_shader_pack");
  if (!option.empty()) return std::filesystem::path(option);
  std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
  const auto relative = std::filesystem::path("shaderpack") / (std::string(kTitle) + ".shaderpack");
  for (int level = 0; level < 6 && !folder.empty(); ++level) {
    std::error_code error;
    if (std::filesystem::exists(folder / relative, error)) return folder / relative;
    if (folder == folder.parent_path()) break;
    folder = folder.parent_path();
  }
  return rex::filesystem::GetExecutableFolder() / relative;
}

Pack ReadPack(const std::filesystem::path& path) {
  std::vector<uint8_t> d;
  if (!mcla::shader_storage::ReadWholeFile(path, d))
    throw Failure{kNoPack, "no shader pack at " + path.string()};
  auto bad = [] { return Failure{kBadPack, "the shader pack is damaged"}; };
  if (d.size() < 64 || std::memcmp(d.data(), "MCLASPK1", 8) != 0) throw bad();
  Pack pack;
  pack.hash = Le64(d.data() + d.size() - 8);
  if (Hash(d.data(), d.size() - 8) != pack.hash) throw bad();
  size_t p = 8;
  auto need = [&](uint64_t n) {
    if (p + n > d.size() - 8) throw bad();
  };
  need(44);
  if (Le32(d.data() + p) != 1)
    throw Failure{kBadPack, "the shader pack's format is newer than this game's"};
  if (Le32(d.data() + p + 4) != kTitleId) throw Failure{kBadPack, "the shader pack is for another game"};
  for (int i = 0; i < 3; ++i) pack.versions[i] = Le32(d.data() + p + 8 + 4 * i);
  if (pack.versions[0] != ss::kShaderStorageVersion ||
      pack.versions[1] != ss::kD3D12PipelineStorageVersion ||
      pack.versions[2] != ss::kVulkanPipelineStorageVersion) {
    throw Failure{kOtherVersions,
                  "the shader pack was made for another version of the graphics code; update "
                  "the game, or wait for a pack for this version"};
  }
  pack.rpf_key_fingerprint = Le64(d.data() + p + 20);
  pack.decl_types_fingerprint = Le64(d.data() + p + 28);
  const uint32_t shader_count = Le32(d.data() + p + 36), list_count = Le32(d.data() + p + 40);
  p += 44;
  std::unordered_set<uint64_t> seen;
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
    need(8 * uint64_t(ps_count));
    for (uint8_t k = 0; k < ps_count; ++k, p += 8) s.pixel_shaders.push_back(Le64(d.data() + p));
    if (s.kind > 2 || s.pixel > 1 || !s.dwords || s.dwords > (1u << 16) || !seen.insert(s.hash).second)
      throw bad();
    pack.shaders.push_back(std::move(s));
  }
  if (list_count > kListCount) throw bad();
  bool list_seen[kListCount] = {};
  for (uint32_t i = 0; i < list_count; ++i) {
    need(16 + 12 + 8);
    // Only the lists this build knows, each once, with the SDK's header and record size.
    const std::string name(reinterpret_cast<const char*>(d.data() + p),
                           strnlen(reinterpret_cast<const char*>(d.data() + p), 16));
    size_t spec = kListCount;
    for (size_t k = 0; k < kListCount; ++k)
      if (name == kLists[k].name) spec = k;
    if (spec == kListCount || list_seen[spec]) throw bad();
    list_seen[spec] = true;
    const auto header = ListHeader(kLists[spec]);
    if (std::memcmp(header.data(), d.data() + p + 16, 12) != 0 ||
        Le32(d.data() + p + 28) != kLists[spec].record_size) {
      throw bad();
    }
    const uint32_t record_size = kLists[spec].record_size;
    const uint32_t records = Le32(d.data() + p + 32);
    p += 36;
    need(uint64_t(records) * record_size);
    PackList l;
    l.spec = spec;
    for (uint32_t r = 0; r < records; ++r, p += record_size) {
      l.records.emplace_back(d.data() + p, d.data() + p + record_size);
      if (Hash(l.records.back().data() + 8, record_size - 8) != Le64(l.records.back().data()))
        throw bad();
    }
    pack.lists.push_back(std::move(l));
  }
  if (p != d.size() - 8) throw bad();
  return pack;
}

// ---------------------------------------------------------------------------------------------
// What the storage holds: each file's valid part as the GPU loads it (up to the first record
// whose hash doesn't match), and its keys. A file that exists but can't be read throws (it is
// never taken for a missing one).

struct StoredFile {
  bool present = false;
  bool header_ok = false;
  std::vector<uint8_t> valid;          // the header and the valid records
  std::unordered_set<uint64_t> keys;   // shader hashes, or description hashes
};

StoredFile ReadShaderStorage(const std::filesystem::path& path) {
  StoredFile f;
  std::vector<uint8_t> d;
  f.present = mcla::shader_storage::ReadWholeFile(path, d);
  if (!f.present || d.size() < 8 || Le32(d.data()) != 0x48534558 ||
      Be32(d.data() + 4) != ss::kShaderStorageVersion) {
    return f;
  }
  f.header_ok = true;
  size_t p = 8;
  while (p + 12 <= d.size()) {
    const uint64_t hash = Le64(d.data() + p);
    const uint32_t dwords = Le32(d.data() + p + 8) & 0x7FFFFFFF;
    if (p + 12 + uint64_t(dwords) * 4 > d.size() ||
        Hash(d.data() + p + 12, size_t(dwords) * 4) != hash)
      break;
    f.keys.insert(hash);
    p += 12 + size_t(dwords) * 4;
  }
  f.valid.assign(d.begin(), d.begin() + p);
  return f;
}

StoredFile ReadPipelineStorage(const std::filesystem::path& path, const ListSpec& spec) {
  StoredFile f;
  std::vector<uint8_t> d;
  f.present = mcla::shader_storage::ReadWholeFile(path, d);
  const auto header = ListHeader(spec);
  if (!f.present || d.size() < 12 || std::memcmp(d.data(), header.data(), 12) != 0) return f;
  f.header_ok = true;
  size_t p = 12;
  while (p + spec.record_size <= d.size() &&
         Hash(d.data() + p + 8, spec.record_size - 8) == Le64(d.data() + p)) {
    f.keys.insert(Le64(d.data() + p));
    p += spec.record_size;
  }
  f.valid.assign(d.begin(), d.begin() + p);
  return f;
}

// ---------------------------------------------------------------------------------------------
// The journal of imports (<local>/545407F8.import), a text file:
//   pack <XXH3 of the pack, 16 hex digits>
//   phase <rebuild | merged | done | rolled-back>
//   failures <failed starts with this pack in a row>
//   disabled <0 | 1>
//   unbuildable <hash> ...       (shaders of this pack that couldn't be rebuilt here)
//   file <name> <length before> <length after>     (one per file the last import changed)

struct JournalFile {
  std::string name;
  uint64_t before = 0, after = 0;
};
struct Journal {
  uint64_t pack = 0;
  std::string phase;
  uint32_t failures = 0;
  bool disabled = false;
  std::unordered_set<uint64_t> unbuildable;
  std::vector<JournalFile> files;
};

bool ReadJournal(const std::filesystem::path& path, Journal& journal) {
  std::vector<uint8_t> d;
  if (!mcla::shader_storage::ReadWholeFile(path, d)) return false;
  std::istringstream in(std::string(d.begin(), d.end()));
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string key;
    words >> key;
    if (key == "pack") {
      std::string hex;
      words >> hex;
      journal.pack = std::strtoull(hex.c_str(), nullptr, 16);
    } else if (key == "phase") {
      words >> journal.phase;
    } else if (key == "failures") {
      words >> journal.failures;
    } else if (key == "disabled") {
      int v = 0;
      words >> v;
      journal.disabled = v != 0;
    } else if (key == "unbuildable") {
      std::string hex;
      while (words >> hex) journal.unbuildable.insert(std::strtoull(hex.c_str(), nullptr, 16));
    } else if (key == "file") {
      JournalFile f;
      words >> f.name >> f.before >> f.after;
      // Only the storage's own files.
      const auto names = StorageFileNames();
      if (words && std::find(names.begin(), names.end(), f.name) != names.end())
        journal.files.push_back(f);
    }
  }
  return true;
}

void WriteJournal(const std::filesystem::path& path, const Journal& journal) {
  std::string text = fmt::format("pack {:016X}\nphase {}\nfailures {}\ndisabled {}\n", journal.pack,
                                 journal.phase, journal.failures, journal.disabled ? 1 : 0);
  if (!journal.unbuildable.empty()) {
    text += "unbuildable";
    for (uint64_t h : journal.unbuildable) text += fmt::format(" {:016X}", h);
    text += "\n";
  }
  for (const JournalFile& f : journal.files)
    text += fmt::format("file {} {} {}\n", f.name, f.before, f.after);
  mcla::shader_storage::WriteFileAtomically(path, std::vector<uint8_t>(text.begin(), text.end()));
}

// ---------------------------------------------------------------------------------------------
// The process's state between the module launch and the end of the preload.

enum class Mode { kOff, kAuto, kPrepare };

struct State {
  Mode mode = Mode::kOff;
  bool storage = false;  // the storage is in use (cache root, store_shaders, session lock)
  Folders folders;
  Journal journal;
  bool journal_dirty = false;
  bool merged = false;   // this process imported something
  uint64_t pack_hash = 0;
  std::string import_state = "off";  // for the stamp: on, off, disabled, unavailable
  Clock::time_point preload_start;
  long long rebuild_ms = 0;
};
State g_state;

// The previous run ended after an import and before the stored pipelines were all created (a
// crash, a hang that was ended, a power cut): the files are cut back to their lengths before it.
// Two such starts in a row with the same pack disable its import.
void RecoverInterruptedImport() {
  Journal& j = g_state.journal;
  if (j.phase != "merged" && j.phase != "rebuild") return;
  size_t cut = 0;
  if (j.phase == "merged") {
    for (const JournalFile& f : j.files) {
      const auto path = g_state.folders.File(f.name);
      const int64_t size = mcla::shader_storage::FileSizeOrAbsent(path);
      if (size < 0) continue;
      if (f.before == 0) {
        // Created (or replaced, being of another version) by the import.
        mcla::shader_storage::RemoveFile(path);
        ++cut;
      } else if (uint64_t(size) >= f.before) {
        if (uint64_t(size) != f.before) {
          mcla::shader_storage::TruncateFile(path, f.before);
          ++cut;
        }
      } else {
        REXLOG_WARN("shader pack: {} is shorter than before the interrupted import; left as it is",
                    path.string());
      }
    }
  }
  ++j.failures;
  if (j.failures >= kFailuresBeforeDisabling) j.disabled = true;
  REXLOG_WARN(
      "shader pack: the last start ended {} the import of pack {:016X} was finished; {} file(s) "
      "cut back to their lengths before it. {}",
      j.phase == "merged" ? "after the import and before" : "before", j.pack, cut,
      j.disabled ? "Its import is now disabled: More > Prepare shaders in the launcher (or "
                   "deleting shaders/local/545407F8.import) tries again."
                 : "It will be tried once more.");
  if (g_state.mode == Mode::kPrepare) Line(fmt::format("rollback {}", cut));
  j.phase = "rolled-back";
  j.files.clear();
  WriteJournal(g_state.folders.Local("import"), j);
}

// ---------------------------------------------------------------------------------------------
// The import.

struct Rebuilt {
  uint64_t hash;
  bool pixel;
  std::vector<uint8_t> ucode;  // big-endian, as the storage holds it
};

struct Import {
  Pack pack;
  StoredFile shaders;
  StoredFile lists[kListCount];
  std::vector<const PackShader*> missing_shaders;
  size_t missing_records = 0;
};

// Rebuilds the given shaders from the game's files: what could be rebuilt, the rest in failed.
std::vector<Rebuilt> RebuildShaders(rex::Runtime* runtime,
                                    const std::vector<const PackShader*>& wanted,
                                    std::vector<std::pair<uint64_t, bool>>& results) {
  auto* memory = runtime->memory();
  uint8_t* const base = memory->virtual_membase();
  uint8_t key[32];
  std::memcpy(key, base + kRpfKeyAddress, 32);
  uint32_t decl_types[kTypes];
  for (int t = 0; t < kTypes; ++t)
    decl_types[t] = Be32(base + kDeclTypeTableAddress + 4 * kTypeTableIndex[t]);

  // The player's effects and presets, and the programs in them and in default.xex.
  const auto files = ReadShaderFiles(runtime->game_data_root() / "xarchive_cache.rpf", key);
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
      for (; i < data.size() && data[i] >= '0' && data[i] <= '9' && mask < 0x10000000; ++i, digits = true)
        mask = mask * 10 + (data[i] - '0');
      if (digits && mask && std::find(preset_masks.begin(), preset_masks.end(), mask) == preset_masks.end())
        preset_masks.push_back(mask);
    }
  }
  std::sort(preset_masks.begin(), preset_masks.end());
  // The executable's image: the effects built into the game, and the console's own shaders.
  FindPrograms(base + kImageBase, kImageSize, programs);
  if (programs.empty() || preset_masks.empty())
    throw Failure{kOtherGame, "no shaders found in the game's files"};
  REXLOG_INFO("shader pack: {} programs and {} vertex layouts of presets in the game's files",
              programs.size(), preset_masks.size());

  mcla::shader_storage::Failure no_memory{kIoError, "no guest memory for binding shaders"};
  GuestBinder binder(memory, decl_types);
  if (!binder.ok()) throw no_memory;

  std::vector<Rebuilt> rebuilt;
  size_t done = 0;
  const size_t total = wanted.size();
  for (const PackShader* sp : wanted) {
    const PackShader& s = *sp;
    bool ok = false;
    if (s.kind == 1) {
      auto it = programs.find(s.source);
      // The pack's pixel flag must be the program's own.
      if (it != programs.end() && it->second.vertex == !s.pixel &&
          it->second.ucode_bytes == uint64_t(s.dwords) * 4 &&
          Hash(it->second.ucode(), it->second.ucode_bytes) == s.hash) {
        rebuilt.push_back({s.hash, bool(s.pixel), {it->second.ucode(), it->second.ucode() + it->second.ucode_bytes}});
        ok = true;
      }
    } else if (s.kind == 2) {
      const uint64_t bytes = uint64_t(s.dwords) * 4;
      if (s.source >= kImageBase && s.source <= uint64_t(kImageBase) + kImageSize &&
          bytes <= uint64_t(kImageBase) + kImageSize - s.source &&
          Hash(base + s.source, size_t(bytes)) == s.hash) {
        rebuilt.push_back({s.hash, bool(s.pixel), {base + s.source, base + s.source + bytes}});
        ok = true;
      }
    } else {
      auto vs = programs.find(s.source);
      if (!s.pixel && vs != programs.end() && vs->second.vertex &&
          vs->second.ucode_bytes == uint64_t(s.dwords) * 4) {
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
    results.push_back({s.hash, ok});
    if (g_state.mode == Mode::kPrepare && ++done % 25 == 0 && done < total)
      Line("rebuild " + std::to_string(done) + " " + std::to_string(total));
  }
  return rebuilt;
}

// Reads the pack and the storage and works out what is missing. Throws Failure.
Import PlanImport(rex::Runtime* runtime, bool all_shaders) {
  Import im;
  im.pack = ReadPack(FindPack());
  g_state.pack_hash = im.pack.hash;
  uint8_t* const base = runtime->memory()->virtual_membase();
  if (Hash(base + kRpfKeyAddress, 32) != im.pack.rpf_key_fingerprint ||
      Hash(base + kDeclTypeTableAddress, 64) != im.pack.decl_types_fingerprint) {
    throw Failure{kOtherGame,
                  "this default.xex is not the one the shader pack was made for (Midnight Club: "
                  "Los Angeles Complete Edition, USA/Europe)"};
  }
  im.shaders = ReadShaderStorage(g_state.folders.File("xsh"));
  for (size_t k = 0; k < kListCount; ++k)
    im.lists[k] = ReadPipelineStorage(g_state.folders.File(kLists[k].name), kLists[k]);
  const Journal& j = g_state.journal;
  std::unordered_set<uint64_t> available = im.shaders.header_ok ? im.shaders.keys
                                                                : std::unordered_set<uint64_t>{};
  for (const PackShader& s : im.pack.shaders) {
    const bool stored = available.count(s.hash) != 0;
    const bool unbuildable = j.pack == im.pack.hash && j.unbuildable.count(s.hash);
    if (all_shaders || (!stored && !unbuildable)) im.missing_shaders.push_back(&s);
  }
  for (const PackShader* s : im.missing_shaders) available.insert(s->hash);
  for (const PackList& l : im.pack.lists) {
    const StoredFile& f = im.lists[l.spec];
    for (const auto& r : l.records) {
      const uint64_t vs = Le64(r.data() + 8), ps = Le64(r.data() + 24);
      if ((!f.header_ok || !f.keys.count(Le64(r.data()))) && available.count(vs) &&
          (!ps || available.count(ps))) {
        ++im.missing_records;
      }
    }
  }
  return im;
}

// Adds what was rebuilt and the pack's descriptions whose shaders are stored to the storage.
// The journal is written first, with each file's length before; files are replaced atomically,
// the shaders first. A failure while replacing cuts the files already replaced back at once.
void MergeImport(Import& im, const std::vector<Rebuilt>& rebuilt) {
  struct Out {
    std::string name;
    std::vector<uint8_t> data;
    uint64_t before;
  };
  std::vector<Out> outs;
  size_t shaders_added = 0, pipelines_added = 0;
  std::unordered_set<uint64_t> available = im.shaders.header_ok ? im.shaders.keys
                                                                : std::unordered_set<uint64_t>{};
  {
    std::vector<uint8_t> out = im.shaders.header_ok ? im.shaders.valid : std::vector<uint8_t>{};
    if (!im.shaders.header_ok) {
      PutLe32(out, 0x48534558);
      PutLe32(out, ByteSwap32(ss::kShaderStorageVersion));
    }
    for (const Rebuilt& r : rebuilt) {
      if (!available.insert(r.hash).second) continue;
      PutLe64(out, r.hash);
      PutLe32(out, uint32_t(r.ucode.size() / 4) | (r.pixel ? 0x80000000u : 0));
      out.insert(out.end(), r.ucode.begin(), r.ucode.end());
      ++shaders_added;
    }
    if (shaders_added) {
      if (im.shaders.present && !im.shaders.header_ok)
        REXLOG_WARN("shader pack: the shader storage is from another version and is replaced");
      outs.push_back({"xsh", std::move(out), im.shaders.header_ok ? im.shaders.valid.size() : 0});
    }
  }
  for (const PackList& l : im.pack.lists) {
    const ListSpec& spec = kLists[l.spec];
    const StoredFile& f = im.lists[l.spec];
    std::vector<uint8_t> out = f.header_ok ? f.valid : std::vector<uint8_t>{};
    if (!f.header_ok) {
      const auto header = ListHeader(spec);
      out.assign(header.begin(), header.end());
    }
    std::unordered_set<uint64_t> keys = f.header_ok ? f.keys : std::unordered_set<uint64_t>{};
    size_t added = 0;
    for (const auto& r : l.records) {
      // Only descriptions whose shaders are in the storage.
      const uint64_t vs = Le64(r.data() + 8), ps = Le64(r.data() + 24);
      if (!available.count(vs) || (ps && !available.count(ps))) continue;
      if (!keys.insert(Le64(r.data())).second) continue;
      out.insert(out.end(), r.begin(), r.end());
      ++added;
    }
    if (added) {
      if (f.present && !f.header_ok)
        REXLOG_WARN("shader pack: {} is from another version and is replaced", spec.name);
      outs.push_back({spec.name, std::move(out), f.header_ok ? f.valid.size() : 0});
      pipelines_added += added;
    }
  }
  if (!outs.empty()) {
    Journal& j = g_state.journal;
    j.phase = "merged";
    j.files.clear();
    for (const Out& o : outs) j.files.push_back({o.name, o.before, o.data.size()});
    WriteJournal(g_state.folders.Local("import"), j);
    size_t replaced = 0;
    try {
      for (const Out& o : outs) {
        mcla::shader_storage::WriteFileAtomically(g_state.folders.File(o.name), o.data);
        ++replaced;
      }
    } catch (const Failure&) {
      // Back to the lengths before, now, rather than at the next start.
      try {
        for (size_t i = 0; i < replaced; ++i) {
          const auto path = g_state.folders.File(outs[i].name);
          if (outs[i].before) {
            mcla::shader_storage::TruncateFile(path, outs[i].before);
          } else {
            mcla::shader_storage::RemoveFile(path);
          }
        }
        j.phase = "rolled-back";
        j.files.clear();
        WriteJournal(g_state.folders.Local("import"), j);
      } catch (const Failure& undo) {
        REXLOG_ERROR("shader pack: couldn't undo the import at once ({}); the next start will",
                     undo.message);
      }
      throw;
    }
    g_state.merged = true;
    if (REXCVAR_GET(mcla_shader_pack_test_crash) == 1) {
      REXLOG_WARN("shader pack: test crash after the import");
      Quit(99);
    }
  }
  if (g_state.mode == Mode::kPrepare)
    Line("merge " + std::to_string(shaders_added) + " " + std::to_string(pipelines_added));
  REXLOG_INFO("shader pack: {} shaders and {} pipelines added to {}", shaders_added,
              pipelines_added, g_state.folders.shareable.string());
}

// The import at a start or in a preparation run. Throws Failure.
void RunImport(rex::Runtime* runtime) {
  const auto start = Clock::now();
  const bool check_only = !rex::cvar::GetFlagByName("mcla_prepare_shaders_check").empty();
  Import im = PlanImport(runtime, check_only);
  Journal& j = g_state.journal;
  if (j.pack != im.pack.hash) {
    // Another pack: its own attempts.
    j = Journal{};
    j.pack = im.pack.hash;
  }
  if (j.disabled && !check_only) {
    g_state.import_state = "disabled";
    REXLOG_WARN(
        "shader pack: importing pack {:016X} is disabled after {} failed starts; the storage is "
        "used as it is. More > Prepare shaders in the launcher (or deleting "
        "shaders/local/545407F8.import) tries again.",
        im.pack.hash, j.failures);
    if (g_state.mode == Mode::kPrepare)
      Line(fmt::format("import-disabled {:016X} {}", im.pack.hash, j.failures));
    return;
  }
  g_state.import_state = "on";
  if (g_state.mode == Mode::kPrepare) Line("start " + std::to_string(im.missing_shaders.size()));
  if (im.missing_shaders.empty() && !im.missing_records) {
    g_state.rebuild_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    if (g_state.mode == Mode::kPrepare) {
      Line("rebuild-done 0 0 " + std::to_string(g_state.rebuild_ms));
      Line("merge 0 0");
    }
    REXLOG_INFO("shader pack: nothing missing from the storage ({} ms)", g_state.rebuild_ms);
    return;
  }

  // Recorded before anything is rebuilt: a crash in the game's code (a damaged effect) then
  // counts as a failed start too.
  std::vector<std::pair<uint64_t, bool>> results;
  std::vector<Rebuilt> rebuilt;
  if (!im.missing_shaders.empty()) {
    if (!check_only) {
      j.phase = "rebuild";
      WriteJournal(g_state.folders.Local("import"), j);
    }
    if (REXCVAR_GET(mcla_shader_pack_test_crash) == 2 && !check_only) {
      REXLOG_WARN("shader pack: test crash while rebuilding");
      Quit(98);
    }
    try {
      rebuilt = RebuildShaders(runtime, im.missing_shaders, results);
    } catch (const Failure&) {
      // Not a crash: nothing to count against the pack, nothing written.
      if (!check_only) {
        j.phase = "idle";
        WriteJournal(g_state.folders.Local("import"), j);
      }
      throw;
    }
  }
  size_t failed = 0;
  for (auto [hash, ok] : results) {
    if (!ok) {
      ++failed;
      j.unbuildable.insert(hash);
    }
  }
  g_state.rebuild_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  if (g_state.mode == Mode::kPrepare) {
    Line("rebuild-done " + std::to_string(results.size() - failed) + " " + std::to_string(failed) +
         " " + std::to_string(g_state.rebuild_ms));
  }
  REXLOG_INFO("shader pack: {} of {} shaders rebuilt in {} ms", results.size() - failed,
              results.size(), g_state.rebuild_ms);

  // Development: the reachability check of a new pack (launcher/dev/shaderpack).
  if (check_only) {
    std::ofstream out(rex::cvar::GetFlagByName("mcla_prepare_shaders_check"), std::ios::trunc);
    for (auto [hash, ok] : results) out << fmt::format("{:016X} {}\n", hash, ok ? "ok" : "failed");
    out.close();
    Quit(out ? 0 : kIoError);
  }

  MergeImport(im, rebuilt);
  if (!g_state.merged) {
    // Nothing written (all missing shaders unbuildable): no import in progress.
    j.phase = "idle";
    WriteJournal(g_state.folders.Local("import"), j);
  }
}

// ---------------------------------------------------------------------------------------------
// The stamp, after the stored pipelines were created: what the launcher compares before Play.

void WriteStamp(rex::Runtime* runtime, const std::string& report) {
  std::string files;
  {
    const auto names = StorageFileNames();
    for (const std::string& name : names) {
      int64_t size = -1;
      try {
        size = mcla::shader_storage::FileSizeOrAbsent(g_state.folders.File(name));
      } catch (const Failure&) {
      }
      if (size < 0) continue;
      std::string entry = fmt::format("{}:{{\"size\":{}", mcla::shader_storage::JsonString(name), size);
      for (const ListSpec& spec : kLists)
        if (name == spec.name && size >= 12)
          entry += fmt::format(",\"count\":{}", (uint64_t(size) - 12) / spec.record_size);
      files += (files.empty() ? "" : ",") + entry + "}";
    }
  }
  char when[32] = {};
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  const bool have_time = localtime_s(&local, &now) == 0;
#else
  const bool have_time = localtime_r(&now, &local) != nullptr;
#endif
  if (have_time) std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", &local);
  auto setting = [](const char* name) {
    return mcla::shader_storage::JsonString(rex::cvar::GetFlagByName(name));
  };
  const std::string json = fmt::format(
      "{{\n"
      "  \"format\": 1,\n"
      "  \"written\": \"{}\",\n"
      "  \"by\": \"{}\",\n"
      "  \"title\": \"{}\",\n"
      "  \"pack\": {},\n"
      "  \"pack_import\": \"{}\",\n"
      "  \"storage_versions\": [\"{:08X}\", \"{:08X}\", \"{:08X}\"],\n"
      "  \"storage\": {},\n"
      "  \"settings\": {{\"mcla_gpu_backend\": {}, \"resolution_scale\": {}, \"mcla_anti_aliasing\": {}}},\n"
      "  \"files\": {{{}}},\n"
      "  \"display_drivers\": {}\n"
      "}}\n",
      when, g_state.mode == Mode::kPrepare ? "prepare" : "start", kTitle,
      g_state.pack_hash ? fmt::format("\"{:016X}\"", g_state.pack_hash) : "null",
      g_state.import_state, ss::kShaderStorageVersion, ss::kD3D12PipelineStorageVersion,
      ss::kVulkanPipelineStorageVersion, report.empty() ? "null" : report,
      setting("mcla_gpu_backend"), setting("resolution_scale"), setting("mcla_anti_aliasing"),
      files, mcla::shader_storage::DisplayDriversJson());
  (void)runtime;
  mcla::shader_storage::WriteFileAtomically(g_state.folders.Local("stamp.json"),
                                            std::vector<uint8_t>(json.begin(), json.end()));
}

bool KeepWaitingForPreparation(const std::string& holder) {
  // A preparation run ends by itself within a minute or so; another game doesn't.
  return holder.rfind("prepare", 0) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

bool MclaPrepareShadersIsRequested() {
  // Only from the command line: a line in mcla.toml would make every start quit.
  return rex::cvar::GetFlagByName("mcla_prepare_shaders") == "true" &&
         rex::cvar::GetFlagSource("mcla_prepare_shaders") == rex::cvar::Source::kCommandLine;
}

// Called before the module starts and before the SDK loads the storage (MclaApp::OnPreLaunchModule;
// on the PS5, main_ps5.cpp, with then_play): takes the session lock, recovers an interrupted
// import, and imports what the storage lacks of the pack. In a preparation run (not then_play),
// a failure ends the process with its code; otherwise it is logged and the game starts as it
// would have.
void MclaShaderStorageBeforeLaunch(rex::Runtime* runtime, rex::ui::Window* window,
                                   bool then_play) {
  const bool prepare = !then_play && MclaPrepareShadersIsRequested();
  if (!then_play && rex::cvar::GetFlagByName("mcla_prepare_shaders") == "true" && !prepare) {
    REXLOG_WARN("mcla_prepare_shaders is only taken from the command line (the launcher passes it); "
                "the game starts as usual");
  }
  g_state.mode = prepare ? Mode::kPrepare
                         : (rex::cvar::GetFlagByName("mcla_shader_pack_auto") == "true" ? Mode::kAuto
                                                                                         : Mode::kOff);
#ifdef _WIN32
  if (prepare) {
    // A crash ends the hidden process at once, without an error dialog nobody sees.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (window) {
      if (HWND hwnd = static_cast<HWND>(window->GetNativeWindowHandle())) ShowWindow(hwnd, SW_HIDE);
    }
  }
#else
  (void)window;
#endif
  if (prepare && rex::cvar::GetFlagSource("shader_storage_progress") == rex::cvar::Source::kDefault) {
    // A command-line value: never saved to mcla.toml.
    rex::cvar::SetFlagFromCommandLine("shader_storage_progress", "50");
  }
  auto fail = [&](const Failure& failure) {
    if (prepare) Fail(failure);
    REXLOG_WARN("shader pack: {}; the game starts with the storage as it is", failure.message);
    g_state.import_state = "unavailable";
  };

  // The storage the SDK will load.
  if (runtime->cache_root().empty() || rex::cvar::GetFlagByName("store_shaders") != "true") {
    if (prepare) Fail({kIoError, "the shader storage is off (no cache folder, or store_shaders false)"});
    return;
  }
  g_state.folders.shareable = runtime->cache_root() / "shaders" / "shareable";
  g_state.folders.local = runtime->cache_root() / "shaders" / "local";
  std::error_code error;
  std::filesystem::create_directories(g_state.folders.shareable, error);
  std::filesystem::create_directories(g_state.folders.local, error);
#ifdef _WIN32
  const int pid = int(GetCurrentProcessId());
#else
  const int pid = int(getpid());
#endif
  std::string holder;
  const bool locked = mcla::shader_storage::AcquireSessionLock(
      g_state.folders.Local("lock"), fmt::format("{} {}", prepare ? "prepare" : "game", pid),
      prepare ? 2000 : 120000, prepare ? nullptr : &KeepWaitingForPreparation, &holder);
  if (!locked) {
    if (prepare) Fail({kInUse, "the shader storage is in use by a running game"});
    REXLOG_WARN("shader pack: the storage is in use by another process ({}); no import", holder);
    g_state.import_state = "unavailable";
    return;
  }
  g_state.storage = true;
  try {
    mcla::shader_storage::RemoveStaleTemporaries(g_state.folders.shareable, StorageFileNames());
    mcla::shader_storage::RemoveStaleTemporaries(g_state.folders.local, {"import", "stamp.json"});
    ReadJournal(g_state.folders.Local("import"), g_state.journal);
    RecoverInterruptedImport();
    if (prepare && rex::cvar::GetFlagByName("mcla_prepare_shaders_retry") == "true" &&
        g_state.journal.disabled) {
      REXLOG_INFO("shader pack: importing pack {:016X} is tried again", g_state.journal.pack);
      g_state.journal.disabled = false;
      g_state.journal.failures = 0;
      g_state.journal.unbuildable.clear();
      WriteJournal(g_state.folders.Local("import"), g_state.journal);
    }
    if (g_state.mode != Mode::kOff) RunImport(runtime);
  } catch (const Failure& failure) {
    fail(failure);
  } catch (const std::exception& e) {
    fail({kIoError, std::string("unexpected error: ") + e.what()});
  }
  g_state.preload_start = Clock::now();
}

// Called once the SDK created the stored pipelines (MclaApp::OnPostLaunchModule; on the PS5,
// after InitializeShaderStorage): the import is done, the stamp is written, and a preparation run
// reports and quits before the game starts.
void MclaShaderStorageAfterPreload(rex::Runtime* runtime) {
  if (!g_state.storage) return;
  const bool prepare = g_state.mode == Mode::kPrepare;
  const auto preload_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_state.preload_start)
          .count();
  try {
    // The import, or an earlier one this start loaded, got through the creation of the stored
    // pipelines: no failed starts in a row any more.
    Journal& j = g_state.journal;
    if (g_state.merged ||
        (j.failures && !j.disabled && j.pack == g_state.pack_hash && g_state.import_state == "on")) {
      j.phase = "done";
      j.failures = 0;
      j.files.clear();
      WriteJournal(g_state.folders.Local("import"), j);
    }
  } catch (const Failure& failure) {
    REXLOG_WARN("shader pack: can't record the finished import: {}", failure.message);
  }
  // In a preparation run the storage is closed first (a driver pipeline cache saved, the files
  // flushed), as the process ends without the graphics shutdown.
  std::string report;
  if (auto* graphics = runtime->graphics_system()) report = graphics->ShaderStorageReport(prepare);
  try {
    WriteStamp(runtime, report);
  } catch (const Failure& failure) {
    REXLOG_WARN("shader pack: can't write the storage's stamp: {}", failure.message);
  }
  if (!prepare) return;
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
  const int64_t process_ms = mcla::shader_storage::ProcessUptimeMs();
  Line(fmt::format("done {} {} {:.0f} {:.0f} {}", g_state.rebuild_ms, preload_ms, private_mb,
                   gpu_mb, process_ms));
  REXLOG_INFO(
      "prepare shaders: done; rebuild {} ms, pipelines {} ms, {:.0f} MB private memory, {:.0f} MB "
      "GPU memory, {} ms since the process started",
      g_state.rebuild_ms, preload_ms, private_mb, gpu_mb, process_ms);
  Quit(0);
}
