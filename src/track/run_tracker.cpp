#include "hermes/track/run_tracker.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "hermes/core/json.hpp"
#include "hermes/log/sinks.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/qor/qor.hpp"
#include "track/http_post.hpp"

#if HERMES_HAVE_SQLITE
#include <sqlite3.h>
#endif

#if __has_include("hermes_version.h")
#include "hermes_version.h"
#endif
#ifndef HERMES_BUILD_VERSION
#define HERMES_BUILD_VERSION "unknown"
#endif

namespace hermes::track {

namespace detail {
std::atomic<RunTracker*> g_active{nullptr};
}

namespace {

HERMES_LOG_CHANNEL(log_track, "track");

double wall_now() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string hostname() {
  char buf[256] = {};
  if (gethostname(buf, sizeof buf - 1) != 0) return "unknown";
  return buf;
}

std::string os_string() {
  struct utsname u{};
  if (uname(&u) != 0) return "unknown";
  return std::format("{} {} {}", u.sysname, u.release, u.machine);
}

std::string cpu_model() {
#if defined(__APPLE__)
  char buf[256] = {};
  std::size_t n = sizeof buf;
  if (sysctlbyname("machdep.cpu.brand_string", buf, &n, nullptr, 0) == 0) return buf;
  return "unknown";
#else
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("model name", 0) == 0) {
      const auto p = line.find(':');
      if (p != std::string::npos) return line.substr(line.find_first_not_of(' ', p + 1));
    }
  }
  return "unknown";
#endif
}

std::int64_t memory_bytes() {
#if defined(__APPLE__)
  std::int64_t m = 0;
  std::size_t n = sizeof m;
  if (sysctlbyname("hw.memsize", &m, &n, nullptr, 0) == 0) return m;
  return 0;
#else
  const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
  return pages > 0 && page > 0 ? static_cast<std::int64_t>(pages) * page : 0;
#endif
}

std::int64_t peak_rss_bytes() {
  struct rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#if defined(__APPLE__)
  return ru.ru_maxrss;  // bytes
#else
  return static_cast<std::int64_t>(ru.ru_maxrss) * 1024;  // KiB
#endif
}

std::string make_run_id() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::random_device rd;
  const unsigned r = rd() ^ static_cast<unsigned>(getpid() * 2654435761u) ^
                     static_cast<unsigned>(now.time_since_epoch().count());
  return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}-{:06x}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec, r & 0xffffff);
}

struct Ev {
  std::int64_t seq;
  double t;
  const char* kind;  // "log" | "progress" | "stage"
  std::string level, channel, id, text;
};
struct Met {
  std::string name;
  std::int64_t step;
  double t, value;
};
struct StageRec {
  int seq;
  std::string name;
  double started, ended;  // ended < 0: running
};

class TrackSink final : public log::Sink {
 public:
  explicit TrackSink(RunTracker* t) : t_(t) {}
  void write(const log::Record& r) override {
    if (RunTracker* t = t_.load(std::memory_order_acquire)) t->log(r);
  }
  void detach() { t_ = nullptr; }

 private:
  std::atomic<RunTracker*> t_;
};

}  // namespace

const char* build_version() { return HERMES_BUILD_VERSION; }

bool have_sqlite() { return HERMES_HAVE_SQLITE != 0; }

std::string default_db_path() {
  if (const char* e = std::getenv("HERMES_RUNS_DB"); e && *e) return e;
  const char* home = std::getenv("HOME");
  return (std::filesystem::path(home && *home ? home : ".") / ".hermes" / "runs.db").string();
}

// ---------------------------------------------------------------------------

struct RunTracker::Impl {
  RunTracker* self = nullptr;
  RunInfo info;
  TrackerOptions opts;
  std::string id, db_path;
  double started = 0.0;
  std::string host, os, cpu;
  std::int64_t mem = 0;
  int threads = 0, cores = 0;
#if HERMES_HAVE_SQLITE
  sqlite3* db = nullptr;
#endif

  // Guarded by mu.
  std::mutex mu;
  std::condition_variable cv, cv_done;
  std::vector<Ev> events;
  std::vector<Met> metrics;
  std::vector<StageRec> stages;
  bool stages_dirty = false;
  std::int64_t seq = 0;
  double progress = 0.0;
  std::string cur_stage, message;
  double last_progress_ev = -1e30;
  std::map<std::string, std::string, std::less<>> summary;  // key -> JSON value
  bool summary_dirty = false;
  std::string results, maps;
  bool results_dirty = false, maps_dirty = false, design_dirty = false;
  std::string status = "running", error;
  int exit_code = 0;
  double ended = 0.0;
  bool final_dirty = false;
  std::size_t log_count = 0;
  std::atomic<std::size_t> dropped{0};
  bool stop = false;
  std::uint64_t sync_req = 0, sync_done = 0;

  // Push (writer thread only).
  std::deque<std::string> push_queue;
  double push_next = 0.0, push_backoff = 1.0;
  std::atomic<std::size_t> push_fail{0};
  bool push_warned = false;

  std::thread writer;
  std::shared_ptr<TrackSink> sink;
  log::SinkId sink_id{};
  bool sink_on = false;
  bool finished = false;

  bool queue_full() const { return events.size() + metrics.size() >= opts.max_queue; }

  // --- Database -------------------------------------------------------------
#if HERMES_HAVE_SQLITE
  bool exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
      HLOG_WARN(log_track, "run database {}: {}", db_path, err ? err : "error");
      sqlite3_free(err);
      return false;
    }
    return true;
  }
  struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
      if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) s = nullptr;
    }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt& text(int i, std::string_view v) {
      sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
      return *this;
    }
    Stmt& real(int i, double v) {
      sqlite3_bind_double(s, i, v);
      return *this;
    }
    Stmt& integer(int i, std::int64_t v) {
      sqlite3_bind_int64(s, i, v);
      return *this;
    }
    Stmt& null(int i) {
      sqlite3_bind_null(s, i);
      return *this;
    }
    bool step() {
      if (!s) return false;
      const int rc = sqlite3_step(s);
      sqlite3_reset(s);
      sqlite3_clear_bindings(s);
      return rc == SQLITE_DONE || rc == SQLITE_ROW;
    }
  };

  void open_db() {
    std::error_code ec;
    const auto parent = std::filesystem::path(db_path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      HLOG_WARN(log_track, "cannot open run database {}: {}", db_path, db ? sqlite3_errmsg(db) : "error");
      sqlite3_close(db);
      db = nullptr;
      return;
    }
    sqlite3_busy_timeout(db, 5000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    const bool ok = exec(
        "CREATE TABLE IF NOT EXISTS runs("
        " id TEXT PRIMARY KEY, name TEXT, status TEXT, started REAL, ended REAL, heartbeat REAL,"
        " design TEXT, input TEXT, command TEXT, cwd TEXT, host TEXT, os TEXT, cpu TEXT, cores INTEGER,"
        " mem_bytes INTEGER, threads INTEGER, version TEXT, pid INTEGER, user TEXT, tags TEXT, settings TEXT,"
        " progress REAL, stage TEXT, message TEXT, summary TEXT, results TEXT, maps TEXT,"
        " exit_code INTEGER, error TEXT, peak_rss INTEGER, dropped INTEGER, origin TEXT);"
        "CREATE TABLE IF NOT EXISTS stages(run_id TEXT, seq INTEGER, name TEXT, started REAL, ended REAL,"
        " PRIMARY KEY(run_id, seq));"
        "CREATE TABLE IF NOT EXISTS events(run_id TEXT, seq INTEGER, t REAL, kind TEXT, level TEXT,"
        " channel TEXT, msg_id TEXT, text TEXT, PRIMARY KEY(run_id, seq));"
        "CREATE TABLE IF NOT EXISTS metrics(run_id TEXT, name TEXT, step INTEGER, t REAL, value REAL);"
        "CREATE INDEX IF NOT EXISTS metrics_run ON metrics(run_id, name, step);"
        "CREATE INDEX IF NOT EXISTS runs_started ON runs(started);"
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
        "INSERT OR IGNORE INTO meta VALUES('schema_version', '1');");
    if (!ok) {
      sqlite3_close(db);
      db = nullptr;
    }
  }

  void insert_run_row() {
    if (!db) return;
    Stmt st(db,
            "INSERT OR REPLACE INTO runs(id, name, status, started, heartbeat, design, input, command, cwd, host,"
            " os, cpu, cores, mem_bytes, threads, version, pid, user, tags, settings, progress, stage, message,"
            " origin) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,'','','local')");
    std::error_code ec;
    const std::string cwd = std::filesystem::current_path(ec).string();
    const char* user = std::getenv("USER");
    st.text(1, id).text(2, info.name).text(3, "running").real(4, started).real(5, started).text(6, info.design);
    st.text(7, info.input).text(8, info.command).text(9, cwd).text(10, host).text(11, os).text(12, cpu);
    st.integer(13, cores).integer(14, mem).integer(15, threads).text(16, build_version()).integer(17, getpid());
    st.text(18, user ? user : "").text(19, tags_json()).text(20, info.settings_json.empty() ? "{}" : info.settings_json);
    if (!st.step()) HLOG_WARN(log_track, "run database {}: {}", db_path, sqlite3_errmsg(db));
  }
#endif

  std::string tags_json() const {
    JsonWriter j;
    j.begin_array();
    for (const auto& t : info.tags) j.value(t);
    j.end_array();
    return j.take();
  }

  struct Batch {
    std::vector<Ev> events;
    std::vector<Met> metrics;
    std::vector<StageRec> stages;  // all stages when dirty
    double progress = 0.0;
    std::string stage, message, design_now;
    std::string summary, results, maps, design;  // empty = unchanged
    bool final = false;
    std::string status, error;
    int exit_code = 0;
    double ended = 0.0;
    std::uint64_t sync = 0;
    bool empty() const {
      return events.empty() && metrics.empty() && stages.empty() && summary.empty() && results.empty() &&
             maps.empty() && design.empty() && !final;
    }
  };

  Batch take() {
    Batch b;
    b.events.swap(events);
    b.metrics.swap(metrics);
    if (stages_dirty) b.stages = stages;
    stages_dirty = false;
    b.progress = progress;
    b.design_now = info.design;
    b.stage = cur_stage;
    b.message = message;
    if (summary_dirty) {
      JsonWriter j;
      j.begin_object();
      for (const auto& [k, v] : summary) j.key(k).raw(v);
      j.end_object();
      b.summary = j.take();
      summary_dirty = false;
    }
    if (results_dirty) b.results = results;
    if (maps_dirty) b.maps = maps;
    results_dirty = maps_dirty = false;
    if (design_dirty) b.design = info.design.empty() ? std::string(" ") : info.design;
    design_dirty = false;
    if (final_dirty) {
      b.final = true;
      b.status = status;
      b.error = error;
      b.exit_code = exit_code;
      b.ended = ended;
      final_dirty = false;
    }
    b.sync = sync_req;
    return b;
  }

  void write_db(const Batch& b, double now) {
#if HERMES_HAVE_SQLITE
    if (!db) return;
    exec("BEGIN");
    {
      Stmt st(db, "INSERT OR REPLACE INTO events VALUES(?,?,?,?,?,?,?,?)");
      for (const Ev& e : b.events) {
        st.text(1, id).integer(2, e.seq).real(3, e.t).text(4, e.kind).text(5, e.level).text(6, e.channel);
        st.text(7, e.id).text(8, e.text).step();
      }
    }
    {
      Stmt st(db, "INSERT INTO metrics VALUES(?,?,?,?,?)");
      for (const Met& m : b.metrics) st.text(1, id).text(2, m.name).integer(3, m.step).real(4, m.t).real(5, m.value).step();
    }
    if (!b.stages.empty()) {
      Stmt st(db, "INSERT OR REPLACE INTO stages VALUES(?,?,?,?,?)");
      for (const StageRec& s : b.stages) {
        st.text(1, id).integer(2, s.seq).text(3, s.name).real(4, s.started);
        if (s.ended >= 0) st.real(5, s.ended); else st.null(5);
        st.step();
      }
    }
    {
      Stmt st(db, "UPDATE runs SET heartbeat=?, progress=?, stage=?, message=?, dropped=? WHERE id=?");
      st.real(1, now).real(2, b.progress).text(3, b.stage).text(4, b.message);
      st.integer(5, static_cast<std::int64_t>(dropped.load())).text(6, id).step();
    }
    if (!b.summary.empty()) Stmt(db, "UPDATE runs SET summary=? WHERE id=?").text(1, b.summary).text(2, id).step();
    if (!b.results.empty()) Stmt(db, "UPDATE runs SET results=? WHERE id=?").text(1, b.results).text(2, id).step();
    if (!b.maps.empty()) Stmt(db, "UPDATE runs SET maps=? WHERE id=?").text(1, b.maps).text(2, id).step();
    if (!b.design.empty()) Stmt(db, "UPDATE runs SET design=? WHERE id=?").text(1, b.design == " " ? std::string() : b.design).text(2, id).step();
    if (b.final) {
      Stmt st(db, "UPDATE runs SET status=?, ended=?, exit_code=?, error=?, peak_rss=? WHERE id=?");
      st.text(1, b.status).real(2, b.ended).integer(3, b.exit_code).text(4, b.error).integer(5, peak_rss_bytes());
      st.text(6, id).step();
    }
    exec("COMMIT");
#else
    (void)b;
    (void)now;
#endif
  }

  std::string batch_json(const Batch& b, double now) const {
    JsonWriter j;
    j.begin_object();
    j.key("run").begin_object();
    std::error_code ec;
    j.kv("id", id).kv("name", info.name).kv("started", started).kv("heartbeat", now).kv("design", b.design_now);
    j.kv("input", info.input).kv("command", info.command).kv("cwd", std::filesystem::current_path(ec).string());
    j.kv("host", host).kv("os", os).kv("cpu", cpu).kv("cores", cores).kv("mem_bytes", mem).kv("threads", threads);
    j.kv("version", build_version()).kv("pid", static_cast<std::int64_t>(getpid()));
    const char* user = std::getenv("USER");
    j.kv("user", user ? user : "").kv("tags", tags_json());
    j.kv("settings", info.settings_json.empty() ? std::string("{}") : info.settings_json);
    j.kv("progress", b.progress).kv("stage", b.stage).kv("message", b.message);
    j.kv("dropped", dropped.load());
    if (!b.summary.empty()) j.kv("summary", b.summary);
    if (!b.results.empty()) j.kv("results", b.results);
    if (!b.maps.empty()) j.kv("maps", b.maps);
    j.kv("status", b.final ? b.status : std::string("running"));
    if (b.final) {
      j.kv("ended", b.ended).kv("exit_code", b.exit_code).kv("error", b.error).kv("peak_rss", peak_rss_bytes());
    }
    j.end_object();
    j.key("events").begin_array();
    for (const Ev& e : b.events) {
      j.begin_object().kv("seq", e.seq).kv("t", e.t).kv("kind", e.kind).kv("level", e.level);
      j.kv("channel", e.channel).kv("msg_id", e.id).kv("text", e.text).end_object();
    }
    j.end_array();
    j.key("metrics").begin_array();
    for (const Met& m : b.metrics) {
      j.begin_object().kv("name", m.name).kv("step", m.step).kv("t", m.t).kv("value", m.value).end_object();
    }
    j.end_array();
    j.key("stages").begin_array();
    for (const StageRec& s : b.stages) {
      j.begin_object().kv("seq", s.seq).kv("name", s.name).kv("started", s.started);
      if (s.ended >= 0) j.kv("ended", s.ended); else j.key("ended").null();
      j.end_object();
    }
    j.end_array();
    j.end_object();
    return j.take();
  }

  void push(const Batch& b, double now, bool final_flush) {
    if (opts.server_url.empty()) return;
    // Every batch is sent, so the server keeps the full history; the run row
    // is always included, so a server that missed batches still shows the
    // current state.
    push_queue.push_back(batch_json(b, now));
    while (push_queue.size() > 512) {
      push_queue.pop_front();
      dropped.fetch_add(1, std::memory_order_relaxed);
    }
    const double deadline = now + (final_flush ? 5.0 : 0.0);
    while (!push_queue.empty()) {
      const double t = wall_now();
      if (t < push_next && !final_flush) return;
      std::string err;
      if (http_post(opts.server_url, "/api/ingest", push_queue.front(), opts.token, 3.0, &err)) {
        push_queue.pop_front();
        push_backoff = 1.0;
        push_next = 0.0;
        continue;
      }
      push_fail.fetch_add(1, std::memory_order_relaxed);
      if (err.find(" 401") != std::string::npos || err.find(" 403") != std::string::npos) {
        // Wrong token: retrying will not help. Stop pushing this run.
        HLOG_WARN(log_track, "run server {} refused the run ({}); check --track-token. Not pushing this run.",
                  opts.server_url, err);
        push_queue.clear();
        opts.server_url.clear();
        return;
      }
      if (!push_warned) {
        HLOG_WARN(log_track, "cannot reach run server {}: {} (will retry; the local database keeps everything)",
                  opts.server_url, err);
        push_warned = true;
      }
      push_next = t + push_backoff;
      push_backoff = std::min(30.0, push_backoff * 2);
      if (!final_flush || wall_now() > deadline) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

  void writer_loop() {
    double last_heartbeat = 0.0;
    for (;;) {
      Batch b;
      bool stopping;
      {
        std::unique_lock lk(mu);
        cv.wait_for(lk, std::chrono::duration<double>(opts.flush_interval_s),
                    [&] { return stop || sync_req != sync_done; });
        b = take();
        stopping = stop;
      }
      const double now = wall_now();
      if (!b.empty() || now - last_heartbeat >= opts.heartbeat_s || stopping) {
        write_db(b, now);
        push(b, now, stopping);
        last_heartbeat = now;
      }
      {
        std::lock_guard lk(mu);
        sync_done = b.sync;
      }
      cv_done.notify_all();
      if (stopping) break;
    }
  }

  void push_event(const char* kind, std::string level, std::string channel, std::string mid, std::string text,
                  double t) {
    if (queue_full()) {
      dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    events.push_back({++seq, t, kind, std::move(level), std::move(channel), std::move(mid), std::move(text)});
  }

  void begin_stage_locked(std::string_view name, double t) {
    if (!stages.empty() && stages.back().ended < 0) stages.back().ended = t;
    stages.push_back({static_cast<int>(stages.size()), std::string(name), t, -1.0});
    stages_dirty = true;
    cur_stage.assign(name);
    push_event("stage", "", "", "", std::string(name), t);
  }
};

RunTracker::RunTracker(RunInfo info, TrackerOptions opts) : impl_(std::make_unique<Impl>()) {
  Impl& m = *impl_;
  m.self = this;
  m.info = std::move(info);
  m.opts = std::move(opts);
  m.id = make_run_id();
  m.started = wall_now();
  m.host = hostname();
  m.os = os_string();
  m.cpu = cpu_model();
  m.mem = memory_bytes();
  m.cores = static_cast<int>(std::thread::hardware_concurrency());
  m.threads = static_cast<int>(par::num_threads());
  m.db_path = m.opts.db_path.empty() ? default_db_path() : m.opts.db_path;
  if (m.db_path == "-") m.db_path.clear();
#if HERMES_HAVE_SQLITE
  if (!m.db_path.empty()) {
    m.open_db();
    m.insert_run_row();
  }
#else
  if (!m.db_path.empty()) HLOG_WARN(log_track, "this hermes was built without SQLite: no local run database");
#endif
  m.writer = std::thread([&m] { m.writer_loop(); });
  detail::g_active.store(this, std::memory_order_release);
  HLOG_VERBOSE(log_track, "run {} registered in {}{}", m.id, m.db_path.empty() ? "(no database)" : m.db_path,
               m.opts.server_url.empty() ? "" : ", pushing to " + m.opts.server_url);
}

RunTracker::~RunTracker() {
  if (!impl_->finished) finish("aborted", -1, "tracker destroyed before finish()");
}

const std::string& RunTracker::run_id() const { return impl_->id; }
const std::string& RunTracker::db_path() const { return impl_->db_path; }
bool RunTracker::db_ok() const {
#if HERMES_HAVE_SQLITE
  return impl_->db != nullptr;
#else
  return false;
#endif
}
RunTracker* RunTracker::active() { return detail::g_active.load(std::memory_order_acquire); }

void RunTracker::progress(const JobProgress& p) {
  Impl& m = *impl_;
  const double t = wall_now();
  std::lock_guard lk(m.mu);
  if (m.finished) return;
  if (!p.stage.empty() && p.stage != m.cur_stage) m.begin_stage_locked(p.stage, t);
  m.progress = p.fraction;
  m.message = p.message;
  if (t - m.last_progress_ev >= m.opts.progress_event_interval_s) {
    m.last_progress_ev = t;
    m.push_event("progress", "", p.stage, "", p.message, t);
    if (!m.queue_full()) m.metrics.push_back({"progress", static_cast<std::int64_t>(p.updates), t, p.fraction});
  }
}

void RunTracker::stage(std::string_view name) {
  Impl& m = *impl_;
  std::lock_guard lk(m.mu);
  if (!m.finished) m.begin_stage_locked(name, wall_now());
}

void RunTracker::end_stage() {
  Impl& m = *impl_;
  std::lock_guard lk(m.mu);
  if (!m.stages.empty() && m.stages.back().ended < 0) {
    m.stages.back().ended = wall_now();
    m.stages_dirty = true;
  }
}

void RunTracker::metric(std::string_view name, std::int64_t step, double value) {
  Impl& m = *impl_;
  const double t = wall_now();
  std::lock_guard lk(m.mu);
  if (m.finished) return;
  if (m.queue_full()) {
    m.dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  m.metrics.push_back({std::string(name), step, t, value});
}

void RunTracker::log(const hermes::log::Record& r) {
  Impl& m = *impl_;
  if (static_cast<std::uint8_t>(r.level) > static_cast<std::uint8_t>(m.opts.log_level)) return;
  const double t = std::chrono::duration<double>(r.time.time_since_epoch()).count();
  std::lock_guard lk(m.mu);
  if (m.finished) return;
  if (++m.log_count > m.opts.max_log_records) {
    if (m.log_count == m.opts.max_log_records + 1)
      m.push_event("log", "warn", "track", "", "log record cap reached; later messages are not stored", t);
    return;
  }
  m.push_event("log", std::string(log::to_string(r.level)), std::string(r.channel), std::string(r.id),
               std::string(r.text), t);
}

void RunTracker::summary(std::string_view key, double value) {
  JsonWriter j;
  j.value(value);
  std::lock_guard lk(impl_->mu);
  impl_->summary[std::string(key)] = j.take();
  impl_->summary_dirty = true;
}

void RunTracker::summary(std::string_view key, std::string_view value) {
  JsonWriter j;
  j.value(value);
  std::lock_guard lk(impl_->mu);
  impl_->summary[std::string(key)] = j.take();
  impl_->summary_dirty = true;
}

void RunTracker::set_design(std::string_view name) {
  std::lock_guard lk(impl_->mu);
  impl_->info.design.assign(name);
  impl_->design_dirty = true;
}

void RunTracker::set_results_json(std::string json) {
  std::lock_guard lk(impl_->mu);
  impl_->results = std::move(json);
  impl_->results_dirty = true;
}

void RunTracker::set_qor(const QorReport& q, int map_side) {
  std::string res = qor_to_json(q), maps = qor_maps_to_json(q, map_side);
  {
    std::lock_guard lk(impl_->mu);
    impl_->results = std::move(res);
    impl_->maps = std::move(maps);
    impl_->results_dirty = impl_->maps_dirty = true;
  }
  summary("hpwl", q.wirelength.hpwl);
  summary("routed_wl", q.wirelength.routed);
  summary("vias", static_cast<double>(q.wirelength.total_vias));
  summary("density_overflow", q.density.overflow);
  summary("peak_util", q.density.peak_util);
  summary("cv", q.density.cv);
  summary("overfull_share", q.density.overfull_share);
  summary("route_overflow", q.congestion.total_overflow);
  summary("max_congestion", q.congestion.max_util);
  summary("rc", q.congestion.rc);
  if (q.dbu_per_micron > 0) summary("dbu_per_micron", q.dbu_per_micron);
}

void RunTracker::attach_log() {
  Impl& m = *impl_;
  if (m.sink_on) return;
  m.sink = std::make_shared<TrackSink>(this);
  m.sink->set_level(m.opts.log_level);
  m.sink_id = log::add_sink(m.sink);
  m.sink_on = true;
}

void RunTracker::attach(RunControl& ctl) {
  ctl.on_progress([this](const JobProgress& p) { progress(p); });
}

void RunTracker::finish(std::string_view status, int exit_code, std::string_view error) {
  Impl& m = *impl_;
  {
    std::lock_guard lk(m.mu);
    if (m.finished) return;
  }
  RunTracker* self = this;
  detail::g_active.compare_exchange_strong(self, nullptr);
  if (m.sink_on) {
    log::remove_sink(m.sink_id);
    m.sink->detach();
    m.sink_on = false;
  }
  {
    std::lock_guard lk(m.mu);
    const double t = wall_now();
    if (!m.stages.empty() && m.stages.back().ended < 0) {
      m.stages.back().ended = t;
      m.stages_dirty = true;
    }
    m.status.assign(status);
    m.exit_code = exit_code;
    m.error.assign(error);
    m.ended = t;
    if (m.status == "done") m.progress = 1.0;
    m.final_dirty = true;
    m.finished = true;
    m.stop = true;
  }
  m.cv.notify_all();
  if (m.writer.joinable()) m.writer.join();
#if HERMES_HAVE_SQLITE
  if (m.db) {
    sqlite3_close(m.db);
    m.db = nullptr;
  }
#endif
  if (m.dropped.load()) HLOG_WARN(log_track, "run {}: {} tracking events were dropped (queue full)", m.id, m.dropped.load());
}

void RunTracker::sync() {
  Impl& m = *impl_;
  std::unique_lock lk(m.mu);
  if (m.finished) return;
  const std::uint64_t want = ++m.sync_req;
  m.cv.notify_all();
  m.cv_done.wait(lk, [&] { return m.sync_done >= want || m.finished; });
}

std::size_t RunTracker::dropped() const { return impl_->dropped.load(); }
std::size_t RunTracker::push_failures() const { return impl_->push_fail.load(); }

}  // namespace hermes::track
