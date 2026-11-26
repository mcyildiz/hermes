#pragma once

// Small JSON value (parse + serialize) for the assistant layer: MCP
// JSON-RPC messages, tool arguments and model provider requests/responses.
// Objects keep insertion order (stable output, readable prompts).
// Not used on any engine hot path.

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hermes::assist {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  using Member = std::pair<std::string, Json>;

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : t_(Type::Bool), b_(b) {}
  Json(double n) : t_(Type::Number), n_(n) {}
  Json(int n) : t_(Type::Number), n_(n) {}
  Json(long n) : t_(Type::Number), n_(static_cast<double>(n)) {}
  Json(long long n) : t_(Type::Number), n_(static_cast<double>(n)) {}
  Json(unsigned n) : t_(Type::Number), n_(n) {}
  Json(unsigned long n) : t_(Type::Number), n_(static_cast<double>(n)) {}
  Json(unsigned long long n) : t_(Type::Number), n_(static_cast<double>(n)) {}
  Json(const char* s) : t_(Type::String), s_(s) {}
  Json(std::string s) : t_(Type::String), s_(std::move(s)) {}
  Json(std::string_view s) : t_(Type::String), s_(s) {}

  static Json array(std::initializer_list<Json> items = {});
  static Json object(std::initializer_list<Member> members = {});
  /// Parses `text`; on error returns null and sets *err (if given).
  static Json parse(std::string_view text, std::string* err = nullptr);

  Type type() const { return t_; }
  bool is_null() const { return t_ == Type::Null; }
  bool is_bool() const { return t_ == Type::Bool; }
  bool is_number() const { return t_ == Type::Number; }
  bool is_string() const { return t_ == Type::String; }
  bool is_array() const { return t_ == Type::Array; }
  bool is_object() const { return t_ == Type::Object; }

  bool as_bool(bool def = false) const { return t_ == Type::Bool ? b_ : def; }
  double as_number(double def = 0.0) const { return t_ == Type::Number ? n_ : def; }
  std::int64_t as_int(std::int64_t def = 0) const;
  /// The string, or `def` when this is not a string.
  std::string as_string(std::string_view def = {}) const { return t_ == Type::String ? s_ : std::string(def); }
  const std::string& str_ref() const { return s_; }

  // Arrays.
  std::size_t size() const { return t_ == Type::Array ? a_.size() : t_ == Type::Object ? o_.size() : 0; }
  const Json& operator[](std::size_t i) const;
  const Json& operator[](int i) const { return (*this)[static_cast<std::size_t>(i < 0 ? 0 : i)]; }
  Json& push_back(Json v);
  const std::vector<Json>& items() const { return a_; }

  // Objects.
  bool contains(std::string_view key) const;
  /// Member value, or a null Json when missing (or not an object).
  const Json& operator[](std::string_view key) const;
  const Json& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
  /// Inserts (or replaces) a member; turns a null value into an object.
  Json& set(std::string_view key, Json v);
  const std::vector<Member>& members() const { return o_; }

  /// Compact serialization (indent > 0: pretty).
  std::string dump(int indent = 0) const;
  void dump_to(std::string& out, int indent = 0, int depth = 0) const;

  bool operator==(const Json& o) const;

 private:
  Type t_ = Type::Null;
  bool b_ = false;
  double n_ = 0.0;
  std::string s_;
  std::vector<Json> a_;
  std::vector<Member> o_;
};

}  // namespace hermes::assist
