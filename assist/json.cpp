#include "assist/json.hpp"

#include <cmath>
#include <cstdlib>
#include <format>

#include "hermes/core/json.hpp"

namespace hermes::assist {

namespace {

const Json& null_json() {
  static const Json n;
  return n;
}

struct Parser {
  std::string_view s;
  std::size_t i = 0;
  std::string err;
  int depth = 0;

  bool fail(std::string m) {
    if (err.empty()) err = std::format("{} at offset {}", m, i);
    return false;
  }
  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  bool lit(std::string_view w) {
    if (s.substr(i, w.size()) != w) return fail("invalid literal");
    i += w.size();
    return true;
  }
  static void put_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(std::uint32_t& v) {
    if (i + 4 > s.size()) return fail("short \\u escape");
    v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
      else return fail("bad \\u escape");
    }
    return true;
  }
  bool string(std::string& out) {
    ++i;  // opening quote
    while (true) {
      if (i >= s.size()) return fail("unterminated string");
      const char c = s[i++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size()) return fail("unterminated escape");
      const char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          std::uint32_t cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            i += 2;
            std::uint32_t lo = 0;
            if (!hex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          put_utf8(out, cp);
          break;
        }
        default: return fail("bad escape");
      }
    }
  }
  bool value(Json& out) {
    if (++depth > 512) return fail("nesting too deep");
    ws();
    if (i >= s.size()) return fail("unexpected end");
    const char c = s[i];
    bool ok = true;
    if (c == '{') {
      ++i;
      out = Json::object();
      ws();
      if (i < s.size() && s[i] == '}') {
        ++i;
      } else {
        while (true) {
          ws();
          if (i >= s.size() || s[i] != '"') return fail("expected key");
          std::string k;
          if (!string(k)) return false;
          ws();
          if (i >= s.size() || s[i] != ':') return fail("expected ':'");
          ++i;
          Json v;
          if (!value(v)) return false;
          out.set(k, std::move(v));
          ws();
          if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
          }
          if (i < s.size() && s[i] == '}') {
            ++i;
            break;
          }
          return fail("expected ',' or '}'");
        }
      }
    } else if (c == '[') {
      ++i;
      out = Json::array();
      ws();
      if (i < s.size() && s[i] == ']') {
        ++i;
      } else {
        while (true) {
          Json v;
          if (!value(v)) return false;
          out.push_back(std::move(v));
          ws();
          if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
          }
          if (i < s.size() && s[i] == ']') {
            ++i;
            break;
          }
          return fail("expected ',' or ']'");
        }
      }
    } else if (c == '"') {
      std::string str;
      ok = string(str);
      out = Json(std::move(str));
    } else if (c == 't') {
      ok = lit("true");
      out = Json(true);
    } else if (c == 'f') {
      ok = lit("false");
      out = Json(false);
    } else if (c == 'n') {
      ok = lit("null");
      out = Json();
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      const std::size_t b = i;
      if (s[i] == '-') ++i;
      while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                              s[i] == '+' || s[i] == '-'))
        ++i;
      const std::string num(s.substr(b, i - b));
      char* e = nullptr;
      const double v = std::strtod(num.c_str(), &e);
      if (!e || *e) return fail("bad number");
      out = Json(v);
    } else {
      return fail(std::format("unexpected character '{}'", c));
    }
    --depth;
    return ok;
  }
};

void indent_to(std::string& out, int indent, int depth) {
  if (indent <= 0) return;
  out += '\n';
  out.append(static_cast<std::size_t>(indent * depth), ' ');
}

}  // namespace

Json Json::array(std::initializer_list<Json> items) {
  Json j;
  j.t_ = Type::Array;
  j.a_.assign(items.begin(), items.end());
  return j;
}

Json Json::object(std::initializer_list<Member> members) {
  Json j;
  j.t_ = Type::Object;
  for (const auto& m : members) j.set(m.first, m.second);
  return j;
}

Json Json::parse(std::string_view text, std::string* err) {
  Parser p{text};
  Json out;
  if (!p.value(out)) {
    if (err) *err = p.err;
    return {};
  }
  p.ws();
  if (p.i != text.size()) {
    if (err) *err = std::format("trailing characters at offset {}", p.i);
    return {};
  }
  if (err) err->clear();
  return out;
}

std::int64_t Json::as_int(std::int64_t def) const {
  if (t_ != Type::Number || !std::isfinite(n_)) return def;
  return static_cast<std::int64_t>(std::llround(n_));
}

const Json& Json::operator[](std::size_t i) const { return t_ == Type::Array && i < a_.size() ? a_[i] : null_json(); }

Json& Json::push_back(Json v) {
  if (t_ != Type::Array) {
    *this = array();
  }
  a_.push_back(std::move(v));
  return a_.back();
}

bool Json::contains(std::string_view key) const {
  if (t_ != Type::Object) return false;
  for (const auto& m : o_)
    if (m.first == key) return true;
  return false;
}

const Json& Json::operator[](std::string_view key) const {
  if (t_ == Type::Object)
    for (const auto& m : o_)
      if (m.first == key) return m.second;
  return null_json();
}

Json& Json::set(std::string_view key, Json v) {
  if (t_ != Type::Object) *this = object();
  for (auto& m : o_)
    if (m.first == key) {
      m.second = std::move(v);
      return m.second;
    }
  o_.emplace_back(std::string(key), std::move(v));
  return o_.back().second;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
  switch (t_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number:
      if (!std::isfinite(n_)) {
        out += "null";
      } else if (n_ == std::floor(n_) && std::fabs(n_) < 9.007199254740992e15) {
        out += std::format("{}", static_cast<long long>(n_));
      } else {
        out += std::format("{}", n_);
      }
      break;
    case Type::String: json_escape(out, s_); break;
    case Type::Array:
      out += '[';
      for (std::size_t k = 0; k < a_.size(); ++k) {
        if (k) out += ',';
        indent_to(out, indent, depth + 1);
        a_[k].dump_to(out, indent, depth + 1);
      }
      if (!a_.empty()) indent_to(out, indent, depth);
      out += ']';
      break;
    case Type::Object:
      out += '{';
      for (std::size_t k = 0; k < o_.size(); ++k) {
        if (k) out += ',';
        indent_to(out, indent, depth + 1);
        json_escape(out, o_[k].first);
        out += indent > 0 ? ": " : ":";
        o_[k].second.dump_to(out, indent, depth + 1);
      }
      if (!o_.empty()) indent_to(out, indent, depth);
      out += '}';
      break;
  }
}

std::string Json::dump(int indent) const {
  std::string s;
  dump_to(s, indent, 0);
  return s;
}

bool Json::operator==(const Json& o) const {
  if (t_ != o.t_) return false;
  switch (t_) {
    case Type::Null: return true;
    case Type::Bool: return b_ == o.b_;
    case Type::Number: return n_ == o.n_;
    case Type::String: return s_ == o.s_;
    case Type::Array: return a_ == o.a_;
    case Type::Object: return o_ == o.o_;
  }
  return false;
}

}  // namespace hermes::assist
