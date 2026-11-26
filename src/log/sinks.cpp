#include "hermes/log/sinks.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <format>
#include <iterator>
#include <stdexcept>

namespace hermes::log {

namespace {

std::string_view level_tag(Level l) {
  switch (l) {
    case Level::Error: return "ERROR";
    case Level::Warn: return "WARN";
    case Level::Info: return "INFO";
    case Level::Verbose: return "VERB";
    case Level::Debug: return "DEBUG";
    case Level::Trace: return "TRACE";
    case Level::Off: break;
  }
  return "?";
}

std::string_view basename(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::FILE* open_or_throw(const std::string& path, bool append) {
  std::FILE* f = std::fopen(path.c_str(), append ? "ab" : "wb");
  if (!f) {
    throw std::runtime_error("cannot open log file " + path + ": " + std::strerror(errno));
  }
  return f;
}

void append_text(std::string& out, const Record& r, const TextFormat& fmt) {
  auto it = std::back_inserter(out);
  if (fmt.time || fmt.thread) {
    out += '[';
    if (fmt.time) std::format_to(it, "{:8.3f}", r.elapsed_s);
    if (fmt.time && fmt.thread) out += ' ';
    if (fmt.thread) std::format_to(it, "t{}", r.thread);
    out += "] ";
  }
  std::format_to(it, "[{} {}", level_tag(r.level), r.channel);
  if (!r.id.empty()) std::format_to(it, " {}", r.id);
  out += "] ";
  out += r.text;
  if (fmt.location && r.file && *r.file) {
    std::format_to(it, " ({}:{})", basename(r.file), r.line);
  }
  if (r.last_of_id) {
    std::format_to(it, " [{} reached its limit; further ones are suppressed]", r.id);
  }
  out += '\n';
}

void json_escape(std::string& out, std::string_view s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(c));
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

}  // namespace

std::string format_text(const Record& rec, const TextFormat& fmt) {
  std::string s;
  append_text(s, rec, fmt);
  return s;
}

// ---------------------------------------------------------------------------
// TextSink

TextSink::TextSink(std::FILE* stream, TextFormat fmt)
    : f_(stream), owned_(false), flush_each_(true), fmt_(fmt) {}

TextSink::TextSink(const std::string& path, bool append, TextFormat fmt)
    : f_(open_or_throw(path, append)), owned_(true), flush_each_(false), fmt_(fmt) {}

TextSink::~TextSink() {
  if (owned_) std::fclose(f_);
  else std::fflush(f_);
}

void TextSink::write(const Record& rec) {
  std::lock_guard lk(mu_);
  buf_.clear();
  append_text(buf_, rec, fmt_);
  std::fwrite(buf_.data(), 1, buf_.size(), f_);  // one call per line
  if (flush_each_ || rec.level <= Level::Warn) std::fflush(f_);
}

void TextSink::flush() {
  std::lock_guard lk(mu_);
  std::fflush(f_);
}

void TextSink::set_format(TextFormat fmt) {
  std::lock_guard lk(mu_);
  fmt_ = fmt;
}

TextFormat TextSink::format() const {
  std::lock_guard lk(mu_);
  return fmt_;
}

void TextSink::set_flush_each_line(bool on) {
  std::lock_guard lk(mu_);
  flush_each_ = on;
}

// ---------------------------------------------------------------------------
// JsonLinesSink

JsonLinesSink::JsonLinesSink(const std::string& path, bool append)
    : f_(open_or_throw(path, append)), owned_(true) {}
JsonLinesSink::JsonLinesSink(std::FILE* stream) : f_(stream), owned_(false) {}

JsonLinesSink::~JsonLinesSink() {
  if (owned_) std::fclose(f_);
  else std::fflush(f_);
}

void JsonLinesSink::write(const Record& r) {
  // UTC wall time with microseconds, ISO 8601.
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      r.time.time_since_epoch()).count();
  const std::time_t secs = static_cast<std::time_t>(us / 1000000);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &secs);
#else
  gmtime_r(&secs, &tm);
#endif
  std::lock_guard lk(mu_);
  buf_.clear();
  auto it = std::back_inserter(buf_);
  std::format_to(it, "{{\"time\":\"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:06}Z\",\"elapsed\":{:.6f},",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                 tm.tm_sec, static_cast<long long>(us % 1000000), r.elapsed_s);
  buf_ += "\"level\":";
  json_escape(buf_, to_string(r.level));
  buf_ += ",\"channel\":";
  json_escape(buf_, r.channel);
  if (!r.id.empty()) {
    buf_ += ",\"id\":";
    json_escape(buf_, r.id);
  }
  std::format_to(it, ",\"thread\":{}", r.thread);
  if (r.file && *r.file) {
    buf_ += ",\"file\":";
    json_escape(buf_, basename(r.file));
    std::format_to(it, ",\"line\":{}", r.line);
  }
  if (r.last_of_id) buf_ += ",\"capped\":true";
  buf_ += ",\"msg\":";
  json_escape(buf_, r.text);
  buf_ += "}\n";
  std::fwrite(buf_.data(), 1, buf_.size(), f_);
  if (r.level <= Level::Warn) std::fflush(f_);
}

void JsonLinesSink::flush() {
  std::lock_guard lk(mu_);
  std::fflush(f_);
}

// ---------------------------------------------------------------------------
// MemorySink / CallbackSink

void MemorySink::write(const Record& r) {
  std::lock_guard lk(mu_);
  if (recs_.size() >= max_) return;
  recs_.push_back({r.level, std::string(r.channel), std::string(r.id), std::string(r.text),
                   r.last_of_id, r.elapsed_s, r.thread});
}

std::vector<StoredRecord> MemorySink::records() const {
  std::lock_guard lk(mu_);
  return recs_;
}

std::vector<StoredRecord> MemorySink::take() {
  std::lock_guard lk(mu_);
  std::vector<StoredRecord> out;
  out.swap(recs_);
  return out;
}

std::size_t MemorySink::size() const {
  std::lock_guard lk(mu_);
  return recs_.size();
}

void MemorySink::clear() {
  std::lock_guard lk(mu_);
  recs_.clear();
}

void CallbackSink::write(const Record& r) {
  std::lock_guard lk(mu_);
  fn_(r);
}

}  // namespace hermes::log
