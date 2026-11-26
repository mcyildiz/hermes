#pragma once
// Log sinks. A sink receives every record that passed its channel's level
// and its own level filter. write() may be called from many threads at once;
// the sinks here serialize per sink so lines never interleave.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "hermes/log/log.hpp"

namespace hermes::log {

/// One log event. Views are valid only during Sink::write().
struct Record {
  Level level;
  std::string_view channel;
  std::string_view id;    ///< Message id, empty if none.
  std::string_view text;
  bool last_of_id;        ///< This id just hit its cap; later ones are dropped.
  std::chrono::system_clock::time_point time;
  double elapsed_s;       ///< Seconds since the logger started.
  std::uint32_t thread;   ///< Small per-process thread index (0 = first thread to log).
  const char* file;       ///< May be empty for records from bindings.
  int line;
};

class Sink {
 public:
  virtual ~Sink() = default;
  virtual void write(const Record& rec) = 0;
  virtual void flush() {}

  /// Extra per-sink filter (e.g. console Info, file Debug). Default: everything.
  void set_level(Level l) noexcept { level_.store(static_cast<std::uint8_t>(l), std::memory_order_relaxed); }
  Level level() const noexcept { return static_cast<Level>(level_.load(std::memory_order_relaxed)); }
  bool accepts(Level l) const noexcept {
    return static_cast<std::uint8_t>(l) <= level_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<std::uint8_t> level_{static_cast<std::uint8_t>(Level::Trace)};
};

using SinkId = int;

/// Registers a sink; returns a handle for remove_sink().
SinkId add_sink(std::shared_ptr<Sink> sink);
bool remove_sink(SinkId id);
/// The console sink installed at startup (stdout). Silence it with
/// console_sink()->set_level(Level::Off) or remove_sink(kConsoleSinkId).
inline constexpr SinkId kConsoleSinkId = 0;
class TextSink;
std::shared_ptr<TextSink> console_sink();

// ---------------------------------------------------------------------------

struct TextFormat {
  bool time = false;      ///< Elapsed seconds, "[  12.345]".
  bool thread = false;    ///< "t3".
  bool location = false;  ///< "placer.cpp:781".
};

/// "[  1.234 t0] [WARN io.def DEF-0003] text"
std::string format_text(const Record& rec, const TextFormat& fmt);

/// Human-readable lines to a FILE* (stdout/stderr) or to a file it opens.
class TextSink : public Sink {
 public:
  explicit TextSink(std::FILE* stream, TextFormat fmt = {});
  /// Opens `path` for writing (append=false truncates). Throws on failure.
  explicit TextSink(const std::string& path, bool append = false,
                    TextFormat fmt = {true, true, false});
  ~TextSink() override;

  void write(const Record& rec) override;
  void flush() override;
  void set_format(TextFormat fmt);
  TextFormat format() const;
  /// Flush after every line (default true for console, false for files,
  /// which flush on Warn/Error, on log::flush() and at exit).
  void set_flush_each_line(bool on);

 private:
  mutable std::mutex mu_;
  std::FILE* f_;
  bool owned_;
  bool flush_each_;
  TextFormat fmt_;
  std::string buf_;
};

/// One JSON object per line: {"time":..,"elapsed":..,"level":..,"channel":..,
/// "id":..,"thread":..,"file":..,"line":..,"msg":..}. Meant for run tracking.
class JsonLinesSink : public Sink {
 public:
  explicit JsonLinesSink(const std::string& path, bool append = false);
  explicit JsonLinesSink(std::FILE* stream);
  ~JsonLinesSink() override;
  void write(const Record& rec) override;
  void flush() override;

 private:
  std::mutex mu_;
  std::FILE* f_;
  bool owned_;
  std::string buf_;
};

/// Owning copy of a Record.
struct StoredRecord {
  Level level;
  std::string channel, id, text;
  bool last_of_id;
  double elapsed_s;
  std::uint32_t thread;
};

/// Keeps records in memory (tests, Python capture, GUI console backlog).
class MemorySink : public Sink {
 public:
  explicit MemorySink(std::size_t max_records = 100000) : max_(max_records) {}
  void write(const Record& rec) override;
  std::vector<StoredRecord> records() const;
  std::vector<StoredRecord> take();  ///< Returns and clears.
  std::size_t size() const;
  void clear();

 private:
  mutable std::mutex mu_;
  std::size_t max_;
  std::vector<StoredRecord> recs_;
};

/// Calls a function for each record (serialized per sink).
class CallbackSink : public Sink {
 public:
  using Fn = std::function<void(const Record&)>;
  explicit CallbackSink(Fn fn) : fn_(std::move(fn)) {}
  void write(const Record& rec) override;

 private:
  std::mutex mu_;
  Fn fn_;
};

}  // namespace hermes::log
