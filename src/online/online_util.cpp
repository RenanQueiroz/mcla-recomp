#include "online_util.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>

namespace mcla::online {

std::string Hex64(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
  return buf;
}

static int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

uint64_t ParseHex64(std::string_view s) {
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s.remove_prefix(2);
  }
  if (s.empty() || s.size() > 16) {
    return 0;
  }
  uint64_t v = 0;
  for (char c : s) {
    int d = HexDigit(c);
    if (d < 0) {
      return 0;
    }
    v = (v << 4) | uint64_t(d);
  }
  return v;
}

std::string HexBytes(const uint8_t* data, size_t size) {
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0xF]);
  }
  return out;
}

std::vector<uint8_t> ParseHexBytes(std::string_view s) {
  std::vector<uint8_t> out;
  if (s.size() % 2) {
    return out;
  }
  out.reserve(s.size() / 2);
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    int hi = HexDigit(s[i]);
    int lo = HexDigit(s[i + 1]);
    if (hi < 0 || lo < 0) {
      return {};
    }
    out.push_back(uint8_t((hi << 4) | lo));
  }
  return out;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const uint8_t* data, size_t size) {
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < size) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(kB64[(v >> 6) & 63]);
    out.push_back(kB64[v & 63]);
    i += 3;
  }
  if (i + 1 == size) {
    uint32_t v = data[i] << 16;
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out += "==";
  } else if (i + 2 == size) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(kB64[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::vector<uint8_t> Base64Decode(std::string_view s) {
  std::vector<uint8_t> out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else if (c == '=') break;
    else continue;  // whitespace
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t((acc >> bits) & 0xFF));
    }
  }
  return out;
}

uint64_t Fnv1a64(std::string_view s) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

static std::mt19937_64& Rng() {
  static std::mt19937_64 rng([] {
    std::random_device rd;
    return (uint64_t(rd()) << 32) ^ rd() ^
           uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  }());
  return rng;
}
static std::mutex& RngMutex() {
  static std::mutex m;
  return m;
}

uint64_t RandomU64() {
  std::lock_guard<std::mutex> lock(RngMutex());
  return Rng()();
}

void RandomBytes(uint8_t* out, size_t size) {
  std::lock_guard<std::mutex> lock(RngMutex());
  for (size_t i = 0; i < size; i += 8) {
    uint64_t v = Rng()();
    for (size_t j = 0; j < 8 && i + j < size; ++j) {
      out[i + j] = uint8_t(v >> (8 * j));
    }
  }
}

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

bool SplitHostPort(std::string_view text, std::string* host, uint16_t* port,
                   uint16_t default_port) {
  std::string t = Trim(text);
  // A scheme is accepted and ignored.
  size_t scheme = t.find("://");
  if (scheme != std::string::npos) {
    t = t.substr(scheme + 3);
  }
  while (!t.empty() && t.back() == '/') {
    t.pop_back();
  }
  if (t.empty()) {
    return false;
  }
  size_t colon = t.rfind(':');
  if (colon == std::string::npos) {
    *host = t;
    *port = default_port;
    return true;
  }
  *host = t.substr(0, colon);
  int p = std::atoi(t.c_str() + colon + 1);
  if (p <= 0 || p > 65535 || host->empty()) {
    return false;
  }
  *port = uint16_t(p);
  return true;
}

std::string Ipv4ToString(uint32_t v) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255,
                v & 255);
  return buf;
}

uint32_t ParseIpv4(std::string_view text) {
  uint32_t parts[4] = {0, 0, 0, 0};
  int n = 0;
  uint32_t cur = 0;
  bool any = false;
  for (char c : text) {
    if (c >= '0' && c <= '9') {
      cur = cur * 10 + uint32_t(c - '0');
      if (cur > 255) {
        return 0;
      }
      any = true;
    } else if (c == '.') {
      if (!any || n >= 3) {
        return 0;
      }
      parts[n++] = cur;
      cur = 0;
      any = false;
    } else {
      return 0;
    }
  }
  if (!any || n != 3) {
    return 0;
  }
  parts[3] = cur;
  return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

std::string Trim(std::string_view s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
    --e;
  return std::string(s.substr(b, e - b));
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return out;
}

}  // namespace mcla::online
