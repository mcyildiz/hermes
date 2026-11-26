#include "hermes/log/log.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>

#include "hermes/log/sinks.hpp"

namespace hermes::log {

namespace detail {

struct Node {
  std::string name;  // "" for the root
  Node* parent = nullptr;
  std::vector<Node*> children;
  std::optional<Level> explicit_level;
  Level effective = Level::Info;
  std::vector<Channel*> handles;
  std::unique_ptr<Channel> shared;  // for log::channel(name)

  void publish() {
    for (Channel* h : handles) {
      h->level_.store(static_cast<std::uint8_t>(effective), std::memory_order_relaxed);
    }
  }
};

}  // namespace detail

using detail::Node;

// ---------------------------------------------------------------------------
// Registry: channel tree, message table and sink list. Intentionally leaked
// so channel handles in other translation units can unregister during static
// destruction in any order.
// ---------------------------------------------------------------------------
class Registry {
 public:
  Registry() {
    auto root = std::make_unique<Node>();
    root->explicit_level = Level::Info;
    root_ = root.get();
    nodes_.emplace("", std::move(root));
    console_ = std::make_shared<TextSink>(stdout);
    sinks_.emplace_back(kConsoleSinkId, console_);
    start_ = std::chrono::steady_clock::now();
  }

  // ---- channels (mu_) ----
  Node* node(std::string_view name) {  // requires mu_
    auto it = nodes_.find(std::string(name));
    if (it != nodes_.end()) return it->second.get();
    const auto dot = name.rfind('.');
    Node* parent = dot == std::string_view::npos ? root_ : node(name.substr(0, dot));
    auto n = std::make_unique<Node>();
    n->name = std::string(name);
    n->parent = parent;
    n->effective = parent->effective;
    Node* raw = n.get();
    parent->children.push_back(raw);
    nodes_.emplace(raw->name, std::move(n));
    return raw;
  }

  static void recompute(Node* n) {
    n->effective = n->explicit_level ? *n->explicit_level : n->parent->effective;
    n->publish();
    for (Node* c : n->children) {
      if (!c->explicit_level) recompute(c);
    }
  }

  void attach(Channel* ch, std::string_view name) {
    std::lock_guard lk(mu_);
    Node* n = node(name);
    ch->node_ = n;
    ch->name_ = n->name.empty() ? "*" : n->name;
    ch->level_.store(static_cast<std::uint8_t>(n->effective), std::memory_order_relaxed);
    n->handles.push_back(ch);
  }

  void detach(Channel* ch) {
    std::lock_guard lk(mu_);
    if (!ch->node_) return;
    auto& h = ch->node_->handles;
    h.erase(std::remove(h.begin(), h.end(), ch), h.end());
  }

  Channel& shared(std::string_view name) {
    std::lock_guard lk(mu_);
    Node* n = node(name);
    if (!n->shared) {
      n->shared.reset(new Channel(n, Channel::NodeTag{}));
      n->shared->name_ = n->name.empty() ? "*" : n->name;
      n->shared->level_.store(static_cast<std::uint8_t>(n->effective), std::memory_order_relaxed);
      n->handles.push_back(n->shared.get());
    }
    return *n->shared;
  }

  void set_level(std::string_view name, std::optional<Level> l) {
    std::lock_guard lk(mu_);
    Node* n = node(name);
    if (n == root_ && !l) l = Level::Info;
    n->explicit_level = l;
    recompute_root_or(n);
  }

  void recompute_root_or(Node* n) {
    if (n == root_) {
      n->effective = *n->explicit_level;
      n->publish();
      for (Node* c : n->children) {
        if (!c->explicit_level) recompute(c);
      }
    } else {
      recompute(n);
    }
  }

  Level effective(std::string_view name) {
    std::lock_guard lk(mu_);
    return node(name)->effective;
  }
  std::optional<Level> explicit_of(std::string_view name) {
    std::lock_guard lk(mu_);
    return node(name)->explicit_level;
  }

  std::vector<ChannelInfo> channels() {
    std::lock_guard lk(mu_);
    std::map<std::string, ChannelInfo> sorted;
    for (const auto& [name, n] : nodes_) {
      const std::string shown = name.empty() ? "*" : name;
      sorted.emplace(shown, ChannelInfo{shown, n->effective, n->explicit_level});
    }
    std::vector<ChannelInfo> out;
    for (auto& [k, v] : sorted) out.push_back(std::move(v));
    return out;
  }

  // ---- messages (mu_) ----
  Message& msg(std::string_view id) {
    std::lock_guard lk(mu_);
    return msg_locked(id);
  }
  Message& msg_locked(std::string_view id) {
    auto it = msgs_.find(std::string(id));
    if (it != msgs_.end()) return *it->second;
    auto m = std::make_unique<Message>(std::string(id), default_cap_);
    Message& ref = *m;
    msgs_.emplace(std::string(id), std::move(m));
    return ref;
  }
  void set_enabled(std::string_view id, bool on) {
    std::lock_guard lk(mu_);
    msg_locked(id).enabled_.store(on, std::memory_order_relaxed);
  }
  void set_cap(std::string_view id, std::uint64_t cap) {
    std::lock_guard lk(mu_);
    Message& m = msg_locked(id);
    m.cap_explicit_ = cap != kUnlimited;
    m.cap_.store(m.cap_explicit_ ? cap : default_cap_, std::memory_order_relaxed);
  }
  void set_default_cap(std::uint64_t cap) {
    std::lock_guard lk(mu_);
    default_cap_ = cap;
    for (auto& [id, m] : msgs_) {
      if (!m->cap_explicit_) m->cap_.store(cap, std::memory_order_relaxed);
    }
  }
  void reset_counts() {
    std::lock_guard lk(mu_);
    for (auto& [id, m] : msgs_) m->count_.store(0, std::memory_order_relaxed);
  }
  std::vector<MessageInfo> messages() {
    std::lock_guard lk(mu_);
    std::map<std::string, MessageInfo> sorted;
    for (const auto& [id, m] : msgs_) {
      sorted.emplace(id, MessageInfo{id, m->enabled(), m->cap(), m->count()});
    }
    std::vector<MessageInfo> out;
    for (auto& [k, v] : sorted) out.push_back(std::move(v));
    return out;
  }

  // ---- sinks (sinks_mu_) ----
  SinkId add(std::shared_ptr<Sink> s) {
    if (!s) throw std::invalid_argument("null log sink");
    std::unique_lock lk(sinks_mu_);
    const SinkId id = next_sink_++;
    sinks_.emplace_back(id, std::move(s));
    return id;
  }
  bool remove(SinkId id) {
    std::unique_lock lk(sinks_mu_);
    for (auto it = sinks_.begin(); it != sinks_.end(); ++it) {
      if (it->first == id) {
        sinks_.erase(it);
        return true;
      }
    }
    return false;
  }
  void dispatch(const Record& r) {
    std::shared_lock lk(sinks_mu_);
    for (const auto& [id, s] : sinks_) {
      if (!s->accepts(r.level)) continue;
      try {
        s->write(r);
      } catch (...) {
        // A failing sink must never take down the engine.
      }
    }
  }
  void flush_all() {
    std::shared_lock lk(sinks_mu_);
    for (const auto& [id, s] : sinks_) {
      try {
        s->flush();
      } catch (...) {
      }
    }
  }
  std::shared_ptr<TextSink> console() const { return console_; }
  double elapsed() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  }

 private:
  std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<Node>> nodes_;
  Node* root_ = nullptr;
  std::unordered_map<std::string, std::unique_ptr<Message>> msgs_;
  std::uint64_t default_cap_ = kUnlimited;

  std::shared_mutex sinks_mu_;
  std::vector<std::pair<SinkId, std::shared_ptr<Sink>>> sinks_;
  SinkId next_sink_ = kConsoleSinkId + 1;
  std::shared_ptr<TextSink> console_;
  std::chrono::steady_clock::time_point start_;
};

namespace {

void configure_into(Registry& r, std::string_view spec);
void apply_env(Registry& r);

Registry& reg() {
  static Registry* r = [] {
    auto* p = new Registry;
    apply_env(*p);
    std::atexit([] { flush(); });
    return p;
  }();
  return *r;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string_view canon(std::string_view name) {
  name = trim(name);
  if (name == "*" || name == "root") return {};
  return name;
}

std::atomic<std::uint32_t> g_next_thread{0};

std::uint32_t thread_index() {
  thread_local const std::uint32_t idx = g_next_thread.fetch_add(1, std::memory_order_relaxed);
  return idx;
}

thread_local bool t_in_emit = false;

void dispatch_text(const Channel& ch, Level level, std::string_view id, bool last,
                   const char* file, int line, std::string_view text) {
  Registry& r = reg();
  Record rec{level, ch.name(), id, text, last, std::chrono::system_clock::now(),
             r.elapsed(), thread_index(), file ? file : "", line};
  r.dispatch(rec);
}

}  // namespace

// ---------------------------------------------------------------------------

std::string_view to_string(Level level) noexcept {
  switch (level) {
    case Level::Off: return "off";
    case Level::Error: return "error";
    case Level::Warn: return "warn";
    case Level::Info: return "info";
    case Level::Verbose: return "verbose";
    case Level::Debug: return "debug";
    case Level::Trace: return "trace";
  }
  return "?";
}

std::optional<Level> parse_level(std::string_view s) noexcept {
  const std::string l = lower(trim(s));
  if (l == "off" || l == "none") return Level::Off;
  if (l == "error") return Level::Error;
  if (l == "warn" || l == "warning") return Level::Warn;
  if (l == "info") return Level::Info;
  if (l == "verbose") return Level::Verbose;
  if (l == "debug") return Level::Debug;
  if (l == "trace") return Level::Trace;
  if (l.size() == 1 && l[0] >= '0' && l[0] <= '6') return static_cast<Level>(l[0] - '0');
  return std::nullopt;
}

Channel::Channel(std::string_view name) { reg().attach(this, canon(name)); }
Channel::Channel(detail::Node* node, NodeTag) : node_(node) {}
Channel::~Channel() {
  // Shared handles live in the (leaked) registry and are never destroyed.
  if (node_ && node_->shared.get() != this) reg().detach(this);
}

Channel& channel(std::string_view name) { return reg().shared(canon(name)); }
Message& message(std::string_view id) { return reg().msg(trim(id)); }

void set_level(std::string_view ch, Level level) { reg().set_level(canon(ch), level); }
void clear_level(std::string_view ch) { reg().set_level(canon(ch), std::nullopt); }
Level level(std::string_view ch) { return reg().effective(canon(ch)); }
std::optional<Level> explicit_level(std::string_view ch) { return reg().explicit_of(canon(ch)); }

void set_message_enabled(std::string_view id, bool enabled) { reg().set_enabled(trim(id), enabled); }
void set_message_cap(std::string_view id, std::uint64_t cap) { reg().set_cap(trim(id), cap); }
void set_default_message_cap(std::uint64_t cap) { reg().set_default_cap(cap); }
void reset_message_counts() { reg().reset_counts(); }

std::vector<ChannelInfo> channels() { return reg().channels(); }
std::vector<MessageInfo> messages() { return reg().messages(); }
void flush() { reg().flush_all(); }

namespace {

void configure_into(Registry& r, std::string_view spec) {
  std::size_t pos = 0;
  while (pos < spec.size()) {
    std::size_t end = spec.find_first_of(", \t\n;", pos);
    if (end == std::string_view::npos) end = spec.size();
    const std::string_view item = trim(spec.substr(pos, end - pos));
    pos = end + 1;
    if (item.empty()) continue;

    const auto bad = [&](const char* why) {
      throw std::invalid_argument("log spec item '" + std::string(item) + "': " + why);
    };
    const auto eq = item.find('=');
    if (item.substr(0, 4) == "msg:") {
      if (eq == std::string_view::npos) bad("expected msg:ID=on|off|N|unlimited");
      const std::string_view id = trim(item.substr(4, eq - 4));
      const std::string val = lower(trim(item.substr(eq + 1)));
      if (id.empty()) bad("empty message id");
      if (val == "on" || val == "off") {
        if (id == "*") bad("msg:* only takes a cap");
        r.set_enabled(id, val == "on");
        continue;
      }
      std::uint64_t cap = kUnlimited;
      if (val != "unlimited" && val != "inf") {
        if (val.empty() || val.find_first_not_of("0123456789") != std::string::npos) {
          bad("expected on, off, a count or unlimited");
        }
        cap = std::strtoull(val.c_str(), nullptr, 10);
      }
      if (id == "*") r.set_default_cap(cap);
      else r.set_cap(id, cap);
      continue;
    }
    if (eq == std::string_view::npos) {
      const auto l = parse_level(item);
      if (!l) bad("unknown level");
      r.set_level("", *l);
      continue;
    }
    const std::string_view ch = trim(item.substr(0, eq));
    const std::string_view val = trim(item.substr(eq + 1));
    if (lower(val) == "inherit") {
      r.set_level(canon(ch), std::nullopt);
      continue;
    }
    const auto l = parse_level(val);
    if (!l) bad("unknown level");
    r.set_level(canon(ch), *l);
  }
}

void apply_env(Registry& r) {
  const char* env = std::getenv("HERMES_LOG");
  if (!env || !*env) return;
  try {
    configure_into(r, env);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "hermes: ignoring HERMES_LOG: %s\n", e.what());
  }
}

}  // namespace

void configure(std::string_view spec) { configure_into(reg(), spec); }
void configure_from_env() { apply_env(reg()); }

SinkId add_sink(std::shared_ptr<Sink> sink) { return reg().add(std::move(sink)); }
bool remove_sink(SinkId id) { return reg().remove(id); }
std::shared_ptr<TextSink> console_sink() { return reg().console(); }

namespace detail {

Message& MessageSite::resolve() noexcept {
  Message& m = message(id_);  // registry lookup; idempotent if threads race here
  msg_.store(&m, std::memory_order_release);
  return m;
}

void vemit(const Channel& ch, Level level, std::string_view id, bool last, const char* file,
           int line, std::string_view fmt, std::format_args args) {
  // A sink that logs (e.g. a Python callback) would recurse; drop those.
  if (t_in_emit) return;
  t_in_emit = true;
  struct Reset {
    ~Reset() { t_in_emit = false; }
  } reset;

  thread_local std::string buf;
  buf.clear();
  try {
    std::vformat_to(std::back_inserter(buf), fmt, args);
  } catch (const std::exception& e) {
    buf.assign("<log format error: ").append(e.what()).append("> ").append(fmt);
  }
  dispatch_text(ch, level, id, last, file, line, buf);
  // Keep a pathological huge message from pinning memory per thread.
  if (buf.capacity() > (1u << 16)) std::string().swap(buf);
}

}  // namespace detail

namespace {

void write_impl(const Channel& ch, Level level, std::string_view text, Message* msg) {
  if (level == Level::Off || !ch.enabled(level)) return;
  bool last = false;
  if (msg) {
    const auto v = msg->admit();
    if (v == Message::Verdict::Drop) return;
    last = v == Message::Verdict::EmitLast;
  }
  if (t_in_emit) return;
  t_in_emit = true;
  struct Reset {
    ~Reset() { t_in_emit = false; }
  } reset;
  dispatch_text(ch, level, msg ? std::string_view(msg->id()) : std::string_view{}, last, "", 0,
                text);
}

}  // namespace

void write(const Channel& ch, Level level, std::string_view text, std::string_view id) {
  if (level == Level::Off || !ch.enabled(level)) return;  // skip the id lookup when off
  write_impl(ch, level, text, id.empty() ? nullptr : &message(id));
}

void write(const Channel& ch, Level level, std::string_view text, Message& msg) {
  write_impl(ch, level, text, &msg);
}

}  // namespace hermes::log
