#pragma once
// Hermes logging.
//
//   Channels   Hierarchical, dot-separated names ("route", "route.tree",
//              "place.gp"). A channel without an explicit level inherits the
//              nearest ancestor's level; the root ("*") defaults to Info.
//   Levels     Off < Error < Warn < Info < Verbose < Debug < Trace.
//   Messages   Optional ids ("DEF-0003") that can be switched off or capped
//              (print at most N times) at runtime.
//   Sinks      Pluggable outputs (console, text file, JSON lines, memory,
//              callback). See hermes/log/sinks.hpp.
//
// Cost model
//   * Disabled call: one relaxed load of the channel's cached level, one
//     compare and one predicted branch. Arguments are NOT evaluated and
//     nothing is formatted. No locks, no shared writes, so any number of
//     threads can hit disabled calls without contention.
//   * Levels above HERMES_LOG_MAX_LEVEL are removed at compile time
//     (default: Trace is removed when NDEBUG is defined).
//   * Enabled call: formatting into a thread-local buffer, then each sink
//     writes one whole line under its own mutex (lines never interleave).
//
// Usage
//   namespace { HERMES_LOG_CHANNEL(log_tree, "route.tree"); }
//   HLOG_DEBUG(log_tree, "net {} has {} pins", n, pins.size());
//   HLOG_WARN_ID(log_tree, "GRT-0001", "net {} unroutable", n);
//   if (HLOG_ENABLED(log_tree, Debug)) { /* expensive stats only for logs */ }
//
// Runtime control (also from CLI `--log`, env HERMES_LOG and Python):
//   hermes::log::configure("info,route=debug,route.tree=trace,place=off,"
//                          "msg:DEF-0003=off,msg:GRT-0001=5");

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef HERMES_LOG_MAX_LEVEL
#  ifdef NDEBUG
#    define HERMES_LOG_MAX_LEVEL 5  // Debug: Trace compiled out of release builds.
#  else
#    define HERMES_LOG_MAX_LEVEL 6  // Trace
#  endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define HERMES_LOG_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define HERMES_LOG_COLD [[gnu::cold, gnu::noinline]]
#else
#  define HERMES_LOG_UNLIKELY(x) (x)
#  define HERMES_LOG_COLD
#endif

namespace hermes::log {

enum class Level : std::uint8_t { Off = 0, Error, Warn, Info, Verbose, Debug, Trace };

inline constexpr Level kCompiledMaxLevel = static_cast<Level>(HERMES_LOG_MAX_LEVEL);
inline constexpr std::uint64_t kUnlimited = std::numeric_limits<std::uint64_t>::max();

std::string_view to_string(Level level) noexcept;
/// "off", "error", "warn"/"warning", "info", "verbose", "debug", "trace" or 0-6.
std::optional<Level> parse_level(std::string_view s) noexcept;

namespace detail { struct Node; }

/// Handle to a channel. Cheap to test; declare one per file with
/// HERMES_LOG_CHANNEL, or get a shared one with log::channel(name).
class Channel {
 public:
  explicit Channel(std::string_view name);
  ~Channel();
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  bool enabled(Level level) const noexcept {
    return static_cast<std::uint8_t>(level) <= level_.load(std::memory_order_relaxed);
  }
  Level level() const noexcept { return static_cast<Level>(level_.load(std::memory_order_relaxed)); }
  const std::string& name() const noexcept { return name_; }

 private:
  friend struct detail::Node;
  friend class Registry;
  struct NodeTag {};
  Channel(detail::Node* node, NodeTag);

  std::atomic<std::uint8_t> level_{static_cast<std::uint8_t>(Level::Info)};
  std::string name_;
  detail::Node* node_ = nullptr;
};

/// Per-id message state, shared by every call site that uses the id.
class Message {
 public:
  enum class Verdict : std::uint8_t { Drop, Emit, EmitLast };

  explicit Message(std::string id, std::uint64_t cap) : id_(std::move(id)), cap_(cap) {}

  /// Lock-free. Once the cap is reached this is two relaxed loads, no writes.
  Verdict admit() noexcept {
    if (!enabled_.load(std::memory_order_relaxed)) return Verdict::Drop;
    const std::uint64_t cap = cap_.load(std::memory_order_relaxed);
    if (count_.load(std::memory_order_relaxed) >= cap) return Verdict::Drop;
    const std::uint64_t n = count_.fetch_add(1, std::memory_order_relaxed);
    if (n >= cap) return Verdict::Drop;
    return (cap != kUnlimited && n + 1 == cap) ? Verdict::EmitLast : Verdict::Emit;
  }

  const std::string& id() const noexcept { return id_; }
  bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
  std::uint64_t cap() const noexcept { return cap_.load(std::memory_order_relaxed); }
  /// Number of times the message was printed (saturates at the cap).
  std::uint64_t count() const noexcept {
    return std::min(count_.load(std::memory_order_relaxed), cap());
  }

 private:
  friend class Registry;
  const std::string id_;
  std::atomic<bool> enabled_{true};
  std::atomic<std::uint64_t> cap_;
  std::atomic<std::uint64_t> count_{0};
  bool cap_explicit_ = false;  // guarded by the registry mutex
};

// ---------------------------------------------------------------------------
// Runtime configuration. All functions are thread-safe and may be called
// while other threads log. Channel names: "" / "*" / "root" mean the root.
// ---------------------------------------------------------------------------

/// Shared channel handle for `name` (created on first use, lives forever).
Channel& channel(std::string_view name);
/// Shared state for message `id` (created on first use, lives forever).
Message& message(std::string_view id);

void set_level(std::string_view channel, Level level);
/// Drop an explicit level so the channel inherits from its parent again.
void clear_level(std::string_view channel);
Level level(std::string_view channel);  ///< Effective level.
std::optional<Level> explicit_level(std::string_view channel);

void set_message_enabled(std::string_view id, bool enabled);
/// Print message `id` at most `cap` times (0 = never). kUnlimited removes the
/// explicit cap, so the id follows set_default_message_cap() again.
void set_message_cap(std::string_view id, std::uint64_t cap);
/// Cap applied to every id that has no explicit cap (default kUnlimited).
void set_default_message_cap(std::uint64_t cap);
void reset_message_counts();

/// Applies a spec: comma/space separated items, in order.
///   LEVEL              root level            e.g. "warn"
///   CHAN=LEVEL         channel level         e.g. "route.tree=trace"
///   CHAN=inherit       clear explicit level
///   msg:ID=off|on      switch a message
///   msg:ID=N           cap a message at N prints ("msg:ID=unlimited" clears)
///   msg:*=N            default cap for all ids
/// Throws std::invalid_argument on a malformed item (earlier items stay applied).
void configure(std::string_view spec);
/// Applies $HERMES_LOG if set. Called automatically once at startup; bad
/// specs are reported on stderr instead of throwing.
void configure_from_env();

struct ChannelInfo {
  std::string name;  ///< "*" for the root.
  Level level;       ///< Effective.
  std::optional<Level> explicit_level;
};
std::vector<ChannelInfo> channels();  ///< Sorted by name.

struct MessageInfo {
  std::string id;
  bool enabled;
  std::uint64_t cap;
  std::uint64_t count;
};
std::vector<MessageInfo> messages();  ///< Ids seen so far or configured; sorted.

/// Flush every sink.
void flush();

// ---------------------------------------------------------------------------
// Emission (call through the macros below).
// ---------------------------------------------------------------------------
namespace detail {

HERMES_LOG_COLD void vemit(const Channel& ch, Level level, std::string_view id, bool last,
                           const char* file, int line, std::string_view fmt,
                           std::format_args args);

// Arguments are taken BY VALUE on purpose: binding a hot loop's variables to
// references would make them escape, and the compiler would then keep them in
// memory for the whole loop (store/reload on every iteration even when the
// log is off). Copies happen only on the enabled path.
template <class... Args>
HERMES_LOG_COLD void emit(const Channel& ch, Level level, std::string_view id, bool last,
                          const char* file, int line, std::format_string<Args...> fmt,
                          Args... args) {
  vemit(ch, level, id, last, file, line, fmt.get(), std::make_format_args(args...));
}

/// Per-call-site cache of the Message for an id. Constant-initialized, so a
/// function-local static of this type has no initialization guard.
class MessageSite {
 public:
  explicit constexpr MessageSite(const char* id) noexcept : id_(id) {}
  Message::Verdict admit() noexcept { return get().admit(); }
  Message& get() noexcept {
    Message* m = msg_.load(std::memory_order_acquire);
    return m ? *m : resolve();
  }

 private:
  HERMES_LOG_COLD Message& resolve() noexcept;
  const char* id_;
  std::atomic<Message*> msg_{nullptr};
};

constexpr bool compiled_in(Level level) noexcept {
  return static_cast<int>(level) <= HERMES_LOG_MAX_LEVEL;
}

}  // namespace detail

/// Logs a pre-formatted string (no format parsing). For bindings and tools.
void write(const Channel& ch, Level level, std::string_view text, std::string_view id = {});
/// Same, with an already resolved message (no id lookup).
void write(const Channel& ch, Level level, std::string_view text, Message& msg);

}  // namespace hermes::log

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

/// Declares a channel handle. Use at namespace scope (anonymous namespace in a .cpp).
#define HERMES_LOG_CHANNEL(var, name) ::hermes::log::Channel var { name }

/// True if `lvl` (Error, Warn, Info, Verbose, Debug, Trace) is on for `ch`.
#define HLOG_ENABLED(ch, lvl)                                        \
  (::hermes::log::detail::compiled_in(::hermes::log::Level::lvl) &&  \
   HERMES_LOG_UNLIKELY((ch).enabled(::hermes::log::Level::lvl)))

/// Log at a runtime level (a ::hermes::log::Level value).
#define HLOG_AT(ch, level_value, ...)                                                  \
  do {                                                                                 \
    const ::hermes::log::Level hermes_log_lvl_ = (level_value);                        \
    if (::hermes::log::detail::compiled_in(hermes_log_lvl_) &&                         \
        HERMES_LOG_UNLIKELY((ch).enabled(hermes_log_lvl_))) {                          \
      ::hermes::log::detail::emit((ch), hermes_log_lvl_, {}, false, __FILE__, __LINE__, \
                                  __VA_ARGS__);                                        \
    }                                                                                  \
  } while (0)

/// Log with a message id (string literal) so it can be switched off or capped.
#define HLOG_ID_AT(ch, level_value, msg_id, ...)                                        \
  do {                                                                                  \
    const ::hermes::log::Level hermes_log_lvl_ = (level_value);                         \
    if (::hermes::log::detail::compiled_in(hermes_log_lvl_) &&                          \
        HERMES_LOG_UNLIKELY((ch).enabled(hermes_log_lvl_))) {                           \
      static constinit ::hermes::log::detail::MessageSite hermes_log_site_{msg_id};    \
      const auto hermes_log_v_ = hermes_log_site_.admit();                              \
      if (hermes_log_v_ != ::hermes::log::Message::Verdict::Drop) {                     \
        ::hermes::log::detail::emit(                                                    \
            (ch), hermes_log_lvl_, hermes_log_site_.get().id(),                         \
            hermes_log_v_ == ::hermes::log::Message::Verdict::EmitLast, __FILE__,       \
            __LINE__, __VA_ARGS__);                                                     \
      }                                                                                 \
    }                                                                                   \
  } while (0)

#define HLOG_ERROR(ch, ...)   HLOG_AT(ch, ::hermes::log::Level::Error, __VA_ARGS__)
#define HLOG_WARN(ch, ...)    HLOG_AT(ch, ::hermes::log::Level::Warn, __VA_ARGS__)
#define HLOG_INFO(ch, ...)    HLOG_AT(ch, ::hermes::log::Level::Info, __VA_ARGS__)
#define HLOG_VERBOSE(ch, ...) HLOG_AT(ch, ::hermes::log::Level::Verbose, __VA_ARGS__)
#define HLOG_DEBUG(ch, ...)   HLOG_AT(ch, ::hermes::log::Level::Debug, __VA_ARGS__)
#define HLOG_TRACE(ch, ...)   HLOG_AT(ch, ::hermes::log::Level::Trace, __VA_ARGS__)

#define HLOG_ERROR_ID(ch, id, ...)   HLOG_ID_AT(ch, ::hermes::log::Level::Error, id, __VA_ARGS__)
#define HLOG_WARN_ID(ch, id, ...)    HLOG_ID_AT(ch, ::hermes::log::Level::Warn, id, __VA_ARGS__)
#define HLOG_INFO_ID(ch, id, ...)    HLOG_ID_AT(ch, ::hermes::log::Level::Info, id, __VA_ARGS__)
#define HLOG_VERBOSE_ID(ch, id, ...) HLOG_ID_AT(ch, ::hermes::log::Level::Verbose, id, __VA_ARGS__)
#define HLOG_DEBUG_ID(ch, id, ...)   HLOG_ID_AT(ch, ::hermes::log::Level::Debug, id, __VA_ARGS__)
#define HLOG_TRACE_ID(ch, id, ...)   HLOG_ID_AT(ch, ::hermes::log::Level::Trace, id, __VA_ARGS__)
