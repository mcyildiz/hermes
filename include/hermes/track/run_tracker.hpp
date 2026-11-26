#pragma once

// Run tracking: every run is registered in a local SQLite database (and,
// optionally, pushed to a central hermes dashboard server), with its
// settings, inputs, source version, machine, thread count, live progress,
// stage timings, metrics, log messages and final results.
//
//   track::RunInfo info;  info.design = d.name();  info.command = ...;
//   track::RunTracker t(info);                      // opens ~/.hermes/runs.db
//   t.attach_log();                                 // log records -> run events
//   RunControl ctl;  t.attach(ctl);                 // progress + stages
//   run_flow(d, opts, &ctl);
//   t.set_qor(report.qor);  t.finish("done");
//
// Speed: the engine never waits for I/O. Every call appends to an in-memory
// queue under a short lock (about 100 ns) and a writer thread commits it to
// SQLite in one transaction every flush interval (and POSTs it to the
// server, if any). When the queue is full, events are dropped and counted,
// never blocked on. Engine code reports metrics through track::metric(),
// which costs one atomic load when no tracker is active.
//
// Several processes can write the same database at once (WAL mode).

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hermes/log/log.hpp"
#include "hermes/log/sinks.hpp"
#include "hermes/par/job.hpp"

namespace hermes {
struct QorReport;
}

namespace hermes::track {

struct RunInfo {
  std::string name;              ///< Optional label (--run-name).
  std::string design;
  std::string input;
  std::string command;           ///< Full command line.
  std::vector<std::string> tags;
  std::string settings_json;     ///< JSON object of the run's options.
};

struct TrackerOptions {
  std::string db_path;           ///< "" = default_db_path(); "-" = no local database.
  std::string server_url;        ///< e.g. "http://192.168.1.20:8765" ("" = no push).
  std::string token;             ///< Sent as X-Hermes-Token to the server.
  double flush_interval_s = 0.5;
  double heartbeat_s = 2.0;
  std::size_t max_queue = 200000;           ///< Events beyond this are dropped (counted).
  log::Level log_level = log::Level::Info;  ///< Log records at or above this level are kept.
  std::size_t max_log_records = 100000;     ///< Per run.
  double progress_event_interval_s = 1.0;   ///< Progress history resolution (the live value is always current).
};

/// $HERMES_RUNS_DB, else ~/.hermes/runs.db.
std::string default_db_path();
/// Build version: `git describe` when built from a git checkout, else a
/// hash of the sources ("src-<hash>"), plus the build type.
const char* build_version();

/// Which run tracking features this build has.
bool have_sqlite();

class RunTracker {
 public:
  explicit RunTracker(RunInfo info, TrackerOptions opts = {});
  /// Finishes the run as "aborted" if finish() was not called; joins the writer.
  ~RunTracker();
  RunTracker(const RunTracker&) = delete;
  RunTracker& operator=(const RunTracker&) = delete;

  const std::string& run_id() const;
  const std::string& db_path() const;
  bool db_ok() const;            ///< Local database is open.
  /// The tracker track::metric() reports to (the most recently created live one).
  static RunTracker* active();

  // --- Engine side: cheap, never blocks on I/O ------------------------------
  void progress(const JobProgress& p);
  /// Starts a stage (ends the current one). Stages also follow progress().
  void stage(std::string_view name);
  void end_stage();
  void metric(std::string_view name, std::int64_t step, double value);
  void log(const hermes::log::Record& r);
  /// One summary number shown in run lists and comparisons (e.g. "hpwl").
  void summary(std::string_view key, double value);
  void summary(std::string_view key, std::string_view value);
  /// The final QoR report (JSON) and its maps (downsampled for the dashboard).
  void set_qor(const QorReport& q, int map_side = 128);
  void set_results_json(std::string json);
  /// Sets the design name once it is known (e.g. after loading).
  void set_design(std::string_view name);

  /// Forwards log records (at or above options().log_level) to this run.
  void attach_log();
  /// Follows a RunControl's progress and stages (sets its on_progress callback).
  void attach(RunControl& ctl);

  /// Ends the run: status "done", "failed", "cancelled". Flushes and joins.
  void finish(std::string_view status, int exit_code = 0, std::string_view error = {});

  /// Blocks until everything queued so far is written (tests, shutdown).
  void sync();
  std::size_t dropped() const;
  /// Batches the server refused or could not be reached for (still queued for retry).
  std::size_t push_failures() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

namespace detail {
extern std::atomic<RunTracker*> g_active;
}

/// Engine-side metric hook: free when no tracker is active.
inline void metric(std::string_view name, std::int64_t step, double value) {
  if (RunTracker* t = detail::g_active.load(std::memory_order_acquire)) t->metric(name, step, value);
}
inline bool active() { return detail::g_active.load(std::memory_order_relaxed) != nullptr; }

}  // namespace hermes::track
