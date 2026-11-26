#include "hermes/io/common.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__unix__) || defined(__APPLE__)
#define HERMES_HAVE_MMAP 1
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hermes::io {

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

void Options::set(std::string_view kv) {
  const std::size_t eq = kv.find('=');
  if (eq == std::string_view::npos) {
    kv_[std::string(kv)] = "1";
  } else {
    kv_[std::string(kv.substr(0, eq))] = std::string(kv.substr(eq + 1));
  }
}

std::string Options::get(std::string_view key, std::string_view def) const {
  const auto it = kv_.find(key);
  return it == kv_.end() ? std::string(def) : it->second;
}

bool Options::get_bool(std::string_view key, bool def) const {
  const auto it = kv_.find(key);
  if (it == kv_.end()) return def;
  const std::string& v = it->second;
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  throw std::invalid_argument("option " + std::string(key) + ": expected a boolean, got '" + v + "'");
}

long long Options::get_int(std::string_view key, long long def) const {
  const auto it = kv_.find(key);
  if (it == kv_.end()) return def;
  long long v = 0;
  if (!parse_int(it->second, v)) {
    throw std::invalid_argument("option " + std::string(key) + ": expected an integer, got '" + it->second + "'");
  }
  return v;
}

double Options::get_double(std::string_view key, double def) const {
  const auto it = kv_.find(key);
  if (it == kv_.end()) return def;
  double v = 0;
  if (!parse_double(it->second, v)) {
    throw std::invalid_argument("option " + std::string(key) + ": expected a number, got '" + it->second + "'");
  }
  return v;
}

void Options::merge_defaults(const Options& o) {
  for (const auto& [k, v] : o.kv_) kv_.emplace(k, v);
}

// ---------------------------------------------------------------------------
// InputFile
// ---------------------------------------------------------------------------

InputFile::InputFile(const std::string& path) : path_(path) {
#ifdef HERMES_HAVE_MMAP
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw std::runtime_error("cannot stat " + path);
  }
  if (S_ISDIR(st.st_mode)) {
    ::close(fd);
    throw std::runtime_error(path + " is a directory");
  }
  size_ = static_cast<std::size_t>(st.st_size);
  if (size_ > 0) {
#ifdef MAP_POPULATE
    // Prefault the whole mapping in one call (much cheaper than one page
    // fault per 4 KB page during parsing).
    constexpr int kFlags = MAP_PRIVATE | MAP_POPULATE;
#else
    constexpr int kFlags = MAP_PRIVATE;
#endif
    void* p = ::mmap(nullptr, size_, PROT_READ, kFlags, fd, 0);
    if (p != MAP_FAILED) {
      data_ = static_cast<const char*>(p);
      mapped_ = true;
#ifdef MADV_WILLNEED
      ::madvise(p, size_, MADV_WILLNEED);
#endif
    }
  }
  ::close(fd);
  if (mapped_ || size_ == 0) {
    if (!data_) data_ = "";
    return;
  }
#endif
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  copy_ = std::move(ss).str();
  data_ = copy_.data();
  size_ = copy_.size();
}

void InputFile::reset() {
#ifdef HERMES_HAVE_MMAP
  if (mapped_) ::munmap(const_cast<char*>(data_), size_);
#endif
  data_ = nullptr;
  size_ = 0;
  mapped_ = false;
  copy_.clear();
}

InputFile::~InputFile() { reset(); }

InputFile::InputFile(InputFile&& o) noexcept { *this = std::move(o); }

InputFile& InputFile::operator=(InputFile&& o) noexcept {
  if (this == &o) return *this;
  reset();
  path_ = std::move(o.path_);
  mapped_ = o.mapped_;
  size_ = o.size_;
  if (o.mapped_ || o.copy_.empty()) {
    data_ = o.data_;
  } else {
    copy_ = std::move(o.copy_);
    data_ = copy_.data();
  }
  o.data_ = nullptr;
  o.size_ = 0;
  o.mapped_ = false;
  return *this;
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::vector<std::size_t> split_lines(std::string_view text, std::size_t parts, std::size_t min_bytes,
                                     bool (*is_start)(std::string_view line)) {
  std::vector<std::size_t> cuts{0};
  if (parts > 1 && text.size() > min_bytes) {
    const std::size_t want = std::max<std::size_t>(min_bytes, text.size() / parts);
    std::size_t pos = want;
    while (pos < text.size()) {
      // Advance to the next line start that qualifies.
      std::size_t p = text.find('\n', pos > 0 ? pos - 1 : 0);
      bool found = false;
      while (p != std::string_view::npos && p + 1 < text.size()) {
        const std::size_t ls = p + 1;
        if (!is_start) {
          found = true;
          p = ls;
          break;
        }
        std::size_t le = text.find('\n', ls);
        if (le == std::string_view::npos) le = text.size();
        if (is_start(text.substr(ls, le - ls))) {
          found = true;
          p = ls;
          break;
        }
        p = le == text.size() ? std::string_view::npos : le;
      }
      if (!found) break;
      if (p > cuts.back()) cuts.push_back(p);
      pos = p + want;
    }
  }
  cuts.push_back(text.size());
  return cuts;
}

void put_double(std::string& out, double v) {
  if (v == static_cast<double>(static_cast<long long>(v)) && std::abs(v) < 9e15) {
    put_int(out, static_cast<long long>(v));
    return;
  }
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, r.ptr);
}

void write_parts(const std::string& path, const std::vector<std::string>& parts) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) throw std::runtime_error("cannot write " + path + ": " + std::strerror(errno));
  bool ok = true;
  for (const std::string& s : parts) {
    if (!s.empty() && std::fwrite(s.data(), 1, s.size(), f) != s.size()) {
      ok = false;
      break;
    }
  }
  if (std::fclose(f) != 0) ok = false;
  if (!ok) throw std::runtime_error("error writing " + path);
}

// ---------------------------------------------------------------------------
// Orientation
// ---------------------------------------------------------------------------

bool parse_orient(std::string_view s, Orient& out) {
  static constexpr std::pair<std::string_view, Orient> kTable[] = {
      {"N", Orient::N},   {"S", Orient::S},   {"E", Orient::E},   {"W", Orient::W},
      {"FN", Orient::FN}, {"FS", Orient::FS}, {"FE", Orient::FE}, {"FW", Orient::FW},
      // LEF-style aliases sometimes seen in DEF writers.
      {"R0", Orient::N},  {"R180", Orient::S}, {"R270", Orient::E}, {"R90", Orient::W},
      {"MY", Orient::FN}, {"MX", Orient::FS},  {"MX90", Orient::FE}, {"MXR90", Orient::FE},
      {"MY90", Orient::FW}, {"MYR90", Orient::FW},
  };
  for (const auto& [name, o] : kTable) {
    if (s == name) {
      out = o;
      return true;
    }
  }
  return false;
}

std::string_view orient_name(Orient o) {
  static constexpr std::string_view kNames[] = {"N", "S", "E", "W", "FN", "FS", "FE", "FW"};
  return kNames[static_cast<int>(o) & 7];
}

bool swaps_xy(Orient o) {
  return o == Orient::E || o == Orient::W || o == Orient::FE || o == Orient::FW;
}

Point orient_offset(Point p, Dbu w, Dbu h, Orient o) {
  // DEF semantics: rotate/mirror the cell, then translate so its bounding
  // box's lower-left corner is at the placement point.
  switch (o) {
    case Orient::N:  return {p.x, p.y};
    case Orient::S:  return {w - p.x, h - p.y};
    case Orient::W:  return {h - p.y, p.x};
    case Orient::E:  return {p.y, w - p.x};
    case Orient::FN: return {w - p.x, p.y};
    case Orient::FS: return {p.x, h - p.y};
    case Orient::FW: return {h - p.y, w - p.x};
    case Orient::FE: return {p.y, p.x};
  }
  return p;
}

}  // namespace hermes::io
