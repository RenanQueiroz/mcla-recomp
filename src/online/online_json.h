// A small JSON value with a parser and a writer: what the private server and the
// game exchange. No dependencies; numbers are doubles, so 64-bit ids travel as
// hexadecimal strings (see Hex64 in online_util.h).

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mcla::online {

class Json {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : type_(Type::kBool), bool_(b) {}
  Json(int v) : type_(Type::kNumber), number_(v) {}
  Json(unsigned v) : type_(Type::kNumber), number_(v) {}
  Json(double v) : type_(Type::kNumber), number_(v) {}
  Json(const char* s) : type_(Type::kString), string_(s) {}
  Json(std::string s) : type_(Type::kString), string_(std::move(s)) {}

  static Json Array() {
    Json j;
    j.type_ = Type::kArray;
    return j;
  }
  static Json Object() {
    Json j;
    j.type_ = Type::kObject;
    return j;
  }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::kNull; }
  bool is_bool() const { return type_ == Type::kBool; }
  bool is_number() const { return type_ == Type::kNumber; }
  bool is_string() const { return type_ == Type::kString; }
  bool is_array() const { return type_ == Type::kArray; }
  bool is_object() const { return type_ == Type::kObject; }

  bool as_bool(bool fallback = false) const { return is_bool() ? bool_ : fallback; }
  double as_number(double fallback = 0) const { return is_number() ? number_ : fallback; }
  int64_t as_int(int64_t fallback = 0) const {
    return is_number() ? static_cast<int64_t>(number_) : fallback;
  }
  uint32_t as_u32(uint32_t fallback = 0) const {
    return is_number() ? static_cast<uint32_t>(static_cast<int64_t>(number_)) : fallback;
  }
  const std::string& as_string() const { return string_; }
  std::string as_string(const std::string& fallback) const {
    return is_string() ? string_ : fallback;
  }

  // Arrays.
  size_t size() const { return is_array() ? array_.size() : (is_object() ? object_.size() : 0); }
  const Json& operator[](size_t i) const { return i < array_.size() ? array_[i] : Null(); }
  const std::vector<Json>& items() const { return array_; }
  Json& push_back(Json v) {
    type_ = Type::kArray;
    array_.push_back(std::move(v));
    return array_.back();
  }

  // Objects.
  const Json& operator[](const char* key) const { return get(key); }
  const Json& get(const std::string& key) const {
    for (const auto& kv : object_) {
      if (kv.first == key) {
        return kv.second;
      }
    }
    return Null();
  }
  bool has(const std::string& key) const {
    for (const auto& kv : object_) {
      if (kv.first == key) {
        return true;
      }
    }
    return false;
  }
  Json& set(const std::string& key, Json v) {
    type_ = Type::kObject;
    for (auto& kv : object_) {
      if (kv.first == key) {
        kv.second = std::move(v);
        return kv.second;
      }
    }
    object_.emplace_back(key, std::move(v));
    return object_.back().second;
  }
  const std::vector<std::pair<std::string, Json>>& members() const { return object_; }

  // Text.
  std::string dump() const;
  // Parses `text`; on a syntax error returns false and leaves `out` null.
  static bool Parse(const std::string& text, Json* out);

 private:
  static const Json& Null() {
    static const Json null;
    return null;
  }
  void DumpTo(std::string& out) const;

  Type type_ = Type::kNull;
  bool bool_ = false;
  double number_ = 0;
  std::string string_;
  std::vector<Json> array_;
  std::vector<std::pair<std::string, Json>> object_;
};

}  // namespace mcla::online
