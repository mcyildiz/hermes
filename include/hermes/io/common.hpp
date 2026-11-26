#pragma once

// Shared pieces of the readers and writers: errors, key/value options,
// memory-mapped input files, fast text scanning and chunked parallel text
// output.

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "hermes/core/types.hpp"
#include "hermes/log/log.hpp"

namespace hermes::io {

/// Phase timings for readers/writers: lap("phase") logs
/// "<what>: <phase> <ms> (total <ms>)" at Debug on the channel (turn on
/// with e.g. --log io=debug).
class PhaseTimer {
 public:
  PhaseTimer(const log::Channel& ch, std::string what)
      : ch_(ch), what_(std::move(what)), t_(clock::now()), t0_(t_) {}
  void lap(std::string_view phase) {
    if (!HLOG_ENABLED(ch_, Debug)) return;
    const auto now = clock::now();
    HLOG_DEBUG(ch_, "{}: {} {:.1f} ms (total {:.1f} ms)", what_, phase,
               std::chrono::duration<double, std::milli>(now - t_).count(),
               std::chrono::duration<double, std::milli>(now - t0_).count());
    t_ = now;
  }

 private:
  using clock = std::chrono::steady_clock;
  const log::Channel& ch_;
  std::string what_;
  clock::time_point t_, t0_;
};

/// Thrown on malformed input. what() is "file:line: message".
class ParseError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Reader / writer options as key=value strings, so every plugin can define
/// its own keys (e.g. "bookshelf.scale=10", "def.allow_unknown_macros=1")
/// and they pass unchanged through the CLI (--opt) and input manifests.
class Options {
 public:
  Options() = default;
  /// Parses "k=v" (a bare "k" means "k=1").
  void set(std::string_view kv);
  void set(std::string key, std::string value) { kv_[std::move(key)] = std::move(value); }
  bool has(std::string_view key) const { return kv_.find(key) != kv_.end(); }
  std::string get(std::string_view key, std::string_view def = {}) const;
  bool get_bool(std::string_view key, bool def) const;
  long long get_int(std::string_view key, long long def) const;
  double get_double(std::string_view key, double def) const;
  const std::map<std::string, std::string, std::less<>>& all() const { return kv_; }
  /// Adds every key of `o` that is not set here (manifest < command line).
  void merge_defaults(const Options& o);

 private:
  std::map<std::string, std::string, std::less<>> kv_;
};

/// Read-only file contents: memory-mapped where the OS allows it (no copy,
/// pages load on first touch, so parallel parsers read in parallel too),
/// otherwise read into memory.
class InputFile {
 public:
  InputFile() = default;
  explicit InputFile(const std::string& path);
  ~InputFile();
  InputFile(InputFile&& o) noexcept;
  InputFile& operator=(InputFile&& o) noexcept;
  InputFile(const InputFile&) = delete;
  InputFile& operator=(const InputFile&) = delete;

  std::string_view text() const { return {data_, size_}; }
  const char* data() const { return data_; }
  std::size_t size() const { return size_; }
  const std::string& path() const { return path_; }
  bool mapped() const { return mapped_; }

 private:
  void reset();
  std::string path_;
  const char* data_ = nullptr;
  std::size_t size_ = 0;
  bool mapped_ = false;
  std::string copy_;  // fallback storage
};

/// Splits `text` into about `parts` pieces whose boundaries are line starts
/// for which `is_start(line)` is true (e.g. a "NetDegree" line). Returns
/// part start offsets plus text.size() at the end. Pieces are at least
/// `min_bytes` long. Deterministic: depends only on the text and parts.
std::vector<std::size_t> split_lines(std::string_view text, std::size_t parts,
                                     std::size_t min_bytes,
                                     bool (*is_start)(std::string_view line) = nullptr);

/// Whitespace tokenizer over one line or a whole buffer (for line-oriented
/// formats such as Bookshelf). Never allocates.
class Tokens {
 public:
  explicit Tokens(std::string_view s) : s_(s) {}
  /// Next whitespace-separated token, or empty at the end.
  std::string_view next() {
    while (p_ < s_.size() && is_space(s_[p_])) ++p_;
    const std::size_t b = p_;
    while (p_ < s_.size() && !is_space(s_[p_])) ++p_;
    return s_.substr(b, p_ - b);
  }
  bool done() {
    while (p_ < s_.size() && is_space(s_[p_])) ++p_;
    return p_ >= s_.size();
  }
  static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

 private:
  std::string_view s_;
  std::size_t p_ = 0;
};

inline bool parse_double(std::string_view t, double& v) {
  if (!t.empty() && t[0] == '+') t.remove_prefix(1);
  const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
  return ec == std::errc() && p == t.data() + t.size();
}
inline bool parse_int(std::string_view t, long long& v) {
  if (!t.empty() && t[0] == '+') t.remove_prefix(1);
  const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
  return ec == std::errc() && p == t.data() + t.size();
}

/// Appends integers / fixed-point numbers to a string (for writers).
inline void put_int(std::string& out, long long v) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, r.ptr);
}
/// Shortest decimal that reads back to exactly `v`.
void put_double(std::string& out, double v);

/// Writes `parts.size()` strings to `path` in order (parts are usually
/// formatted in parallel). Throws std::runtime_error on failure.
void write_parts(const std::string& path, const std::vector<std::string>& parts);

/// Transform a point given relative to an unrotated cell of size (w, h) into
/// the lower-left-relative frame of the cell placed with orientation `o`.
Point orient_offset(Point p, Dbu w, Dbu h, Orient o);
/// True for E, W, FE, FW (width and height swap).
bool swaps_xy(Orient o);
/// Parse an orientation keyword (N, S, E, W, FN, FS, FE, FW and the
/// R0/R90/MX/MY aliases). Returns false if `s` is not one.
bool parse_orient(std::string_view s, Orient& out);
std::string_view orient_name(Orient o);

}  // namespace hermes::io
