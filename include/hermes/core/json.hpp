#pragma once

// Minimal JSON writer (no parser): enough for reports, run records and the
// dashboard feed. Numbers use the shortest round-trip form; NaN/inf become
// null.
//
//   JsonWriter w;
//   w.begin_object();
//   w.kv("hpwl", 1.5e6).kv("design", "adaptec2");
//   w.key("layers").begin_array(); ... w.end_array();
//   w.end_object();
//   std::string s = w.take();

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace hermes {

inline void json_escape(std::string& out, std::string_view s) {
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
          out += ch;
        }
    }
  }
  out += '"';
}

class JsonWriter {
 public:
  JsonWriter& begin_object() { return open('{'); }
  JsonWriter& end_object() { return close('}'); }
  JsonWriter& begin_array() { return open('['); }
  JsonWriter& end_array() { return close(']'); }

  JsonWriter& key(std::string_view k) {
    sep();
    json_escape(s_, k);
    s_ += ':';
    after_key_ = true;
    return *this;
  }
  JsonWriter& value(std::string_view v) {
    sep();
    json_escape(s_, v);
    return *this;
  }
  JsonWriter& value(const char* v) { return value(std::string_view(v)); }
  JsonWriter& value(const std::string& v) { return value(std::string_view(v)); }
  JsonWriter& value(bool v) {
    sep();
    s_ += v ? "true" : "false";
    return *this;
  }
  JsonWriter& value(double v) {
    sep();
    if (std::isfinite(v)) {
      s_ += std::format("{}", v);
    } else {
      s_ += "null";
    }
    return *this;
  }
  JsonWriter& value(float v) { return value(static_cast<double>(v)); }
  template <class T>
    requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)
  JsonWriter& value(T v) {
    sep();
    s_ += std::format("{}", v);
    return *this;
  }
  JsonWriter& null() {
    sep();
    s_ += "null";
    return *this;
  }
  /// Inserts already-serialized JSON.
  JsonWriter& raw(std::string_view json) {
    sep();
    s_ += json.empty() ? std::string_view("null") : json;
    return *this;
  }
  template <class T>
  JsonWriter& kv(std::string_view k, const T& v) {
    key(k);
    return value(v);
  }

  const std::string& str() const { return s_; }
  std::string take() { return std::move(s_); }

 private:
  JsonWriter& open(char c) {
    sep();
    s_ += c;
    first_.push_back(true);
    return *this;
  }
  JsonWriter& close(char c) {
    s_ += c;
    if (!first_.empty()) first_.pop_back();
    return *this;
  }
  void sep() {
    if (after_key_) {
      after_key_ = false;
      return;
    }
    if (!first_.empty()) {
      if (!first_.back()) s_ += ',';
      first_.back() = false;
    }
  }

  std::string s_;
  std::vector<bool> first_;
  bool after_key_ = false;
};

}  // namespace hermes
