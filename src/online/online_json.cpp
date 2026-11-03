#include "online_json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mcla::online {

namespace {

void AppendEscaped(std::string& out, const std::string& s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  bool ParseDocument(Json* out) {
    SkipWs();
    if (!ParseValue(out)) {
      return false;
    }
    SkipWs();
    return pos_ == s_.size();
  }

 private:
  void SkipWs() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' ||
                                s_[pos_] == '\r')) {
      ++pos_;
    }
  }
  bool Match(const char* lit) {
    size_t n = std::strlen(lit);
    if (s_.compare(pos_, n, lit) == 0) {
      pos_ += n;
      return true;
    }
    return false;
  }

  bool ParseValue(Json* out) {
    if (pos_ >= s_.size()) {
      return false;
    }
    char c = s_[pos_];
    if (c == '{') {
      return ParseObject(out);
    }
    if (c == '[') {
      return ParseArray(out);
    }
    if (c == '"') {
      std::string str;
      if (!ParseString(&str)) {
        return false;
      }
      *out = Json(std::move(str));
      return true;
    }
    if (Match("true")) {
      *out = Json(true);
      return true;
    }
    if (Match("false")) {
      *out = Json(false);
      return true;
    }
    if (Match("null")) {
      *out = Json(nullptr);
      return true;
    }
    return ParseNumber(out);
  }

  bool ParseNumber(Json* out) {
    size_t start = pos_;
    if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) {
      ++pos_;
    }
    while (pos_ < s_.size() &&
           (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' ||
            s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '-' || s_[pos_] == '+')) {
      ++pos_;
    }
    if (start == pos_) {
      return false;
    }
    std::string num = s_.substr(start, pos_ - start);
    char* end = nullptr;
    double v = std::strtod(num.c_str(), &end);
    if (!end || *end != '\0') {
      return false;
    }
    *out = Json(v);
    return true;
  }

  static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseHex4(uint32_t* out) {
    if (pos_ + 4 > s_.size()) {
      return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= c - '0';
      } else if (c >= 'a' && c <= 'f') {
        v |= c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        v |= c - 'A' + 10;
      } else {
        return false;
      }
    }
    *out = v;
    return true;
  }

  bool ParseString(std::string* out) {
    if (s_[pos_] != '"') {
      return false;
    }
    ++pos_;
    while (pos_ < s_.size()) {
      char c = s_[pos_++];
      if (c == '"') {
        return true;
      }
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) {
        return false;
      }
      char e = s_[pos_++];
      switch (e) {
        case '"':
          out->push_back('"');
          break;
        case '\\':
          out->push_back('\\');
          break;
        case '/':
          out->push_back('/');
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          uint32_t cp;
          if (!ParseHex4(&cp)) {
            return false;
          }
          if (cp >= 0xD800 && cp < 0xDC00 && Match("\\u")) {
            uint32_t lo;
            if (!ParseHex4(&lo)) {
              return false;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          AppendUtf8(*out, cp);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool ParseArray(Json* out) {
    ++pos_;  // [
    *out = Json::Array();
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipWs();
      Json item;
      if (!ParseValue(&item)) {
        return false;
      }
      out->push_back(std::move(item));
      SkipWs();
      if (pos_ >= s_.size()) {
        return false;
      }
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool ParseObject(Json* out) {
    ++pos_;  // {
    *out = Json::Object();
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipWs();
      std::string key;
      if (pos_ >= s_.size() || !ParseString(&key)) {
        return false;
      }
      SkipWs();
      if (pos_ >= s_.size() || s_[pos_] != ':') {
        return false;
      }
      ++pos_;
      SkipWs();
      Json value;
      if (!ParseValue(&value)) {
        return false;
      }
      out->set(key, std::move(value));
      SkipWs();
      if (pos_ >= s_.size()) {
        return false;
      }
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  const std::string& s_;
  size_t pos_ = 0;
};

}  // namespace

void Json::DumpTo(std::string& out) const {
  switch (type_) {
    case Type::kNull:
      out += "null";
      break;
    case Type::kBool:
      out += bool_ ? "true" : "false";
      break;
    case Type::kNumber: {
      if (std::isfinite(number_) && number_ == std::floor(number_) &&
          std::fabs(number_) < 9007199254740992.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(number_));
        out += buf;
      } else if (std::isfinite(number_)) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.17g", number_);
        out += buf;
      } else {
        out += "null";
      }
      break;
    }
    case Type::kString:
      AppendEscaped(out, string_);
      break;
    case Type::kArray: {
      out.push_back('[');
      bool first = true;
      for (const auto& item : array_) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        item.DumpTo(out);
      }
      out.push_back(']');
      break;
    }
    case Type::kObject: {
      out.push_back('{');
      bool first = true;
      for (const auto& kv : object_) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        AppendEscaped(out, kv.first);
        out.push_back(':');
        kv.second.DumpTo(out);
      }
      out.push_back('}');
      break;
    }
  }
}

std::string Json::dump() const {
  std::string out;
  DumpTo(out);
  return out;
}

bool Json::Parse(const std::string& text, Json* out) {
  Parser parser(text);
  Json value;
  if (!parser.ParseDocument(&value)) {
    *out = Json();
    return false;
  }
  *out = std::move(value);
  return true;
}

}  // namespace mcla::online
