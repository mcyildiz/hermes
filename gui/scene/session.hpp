#pragma once

// The GUI's controller: owns the Design, runs every engine as a background
// job, and hands the UI thread only immutable snapshots. It has no
// rendering code, so it is tested headless.
//
// Threading rules (what keeps the GUI non-blocking):
//  * At most one job uses the Design at a time (load, flow, QoR, save).
//    While it runs, the UI thread reads only snapshot() (a published
//    DesignSnapshot) and job_progress(); it never touches the Design.
//  * Edits from the UI (move_cell, undo) run on the UI thread, and only
//    when no job is running (can_edit()). They are cheap (one cell, a
//    tracked flush and a one-chunk publish).
//  * poll() on the UI thread picks up finished jobs (a loaded design, a flow
//    report, a QoR report) and queues messages for the console.
//
// Jobs leave a core free for the UI (JobOptions::max_threads).

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>
#include <string>
#include <vector>

#include "gui/scene/scene.hpp"
#include "hermes/db/design.hpp"
#include "hermes/db/snapshot.hpp"
#include "hermes/db/synthetic.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/flow/steps.hpp"
#include "hermes/io/common.hpp"
#include "hermes/log/sinks.hpp"
#include "hermes/par/job.hpp"
#include "hermes/qor/qor.hpp"

namespace hermes::gui {

struct LogLine {
  log::Level level = log::Level::Info;
  std::string channel;
  std::string text;
  double elapsed_s = 0.0;
};

/// What a background task leaves behind; poll() publishes it on the UI
/// thread (Session::last_placement() etc.).
struct TaskOutput {
  std::optional<PlacementResult> placement;
  std::optional<LegalizeStepResult> legalize;
  std::optional<RouteStepResult> route;
  std::optional<TimingStepResult> timing;
  std::shared_ptr<QorReport> qor;
  std::shared_ptr<FlowReport> flow;
  /// Logged (channel "cmd") and shown in the console when the task ends.
  std::string summary;
};

/// A background task on the session's design and engine state. It runs on
/// the job thread with exclusive access to both.
using SessionTask = std::function<void(Design&, FlowEngines&, RunControl&, TaskOutput&)>;

/// How the last job ended (scripts stop on a failed job).
struct JobEnd {
  std::string kind;
  JobState state = JobState::Done;
  std::string error;
  std::uint64_t serial = 0;   ///< Counts finished jobs.
};

class Session {
 public:
  Session();
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // --- Design lifecycle (UI thread) -----------------------------------------
  /// Loads a directory or file (anything `hermes <path>` reads) in the
  /// background. False (and *why) if a job is running.
  bool open(const std::string& path, const io::Options& opts = {}, std::string* why = nullptr);
  bool open_synthetic(const SyntheticOptions& o, std::string* why = nullptr);
  /// Takes a design synchronously (tests, embedding).
  void adopt(Design design, std::string source = "memory");

  bool has_design() const { return design_ != nullptr; }
  /// Increments whenever a different design is adopted.
  std::uint64_t generation() const { return generation_; }
  /// Latest published snapshot (null without a design). Any thread.
  std::shared_ptr<const DesignSnapshot> snapshot() const;
  std::shared_ptr<const TechInfo> tech() const { return tech_; }
  const std::string& source() const { return source_; }
  double load_seconds() const { return load_seconds_; }

  // --- Jobs (UI thread) --------------------------------------------------------
  bool busy() const;
  const JobHandle& job() const { return job_; }
  std::string job_kind() const { return job_kind_; }
  JobProgress job_progress() const;
  /// Runs `task` as a background job named `kind` ("placement", "global
  /// route", ...). `edits`: the task changes the design (undo history is
  /// dropped). False (and *why) without a design or while a job runs.
  bool start_task(std::string kind, SessionTask task, bool edits, std::string* why = nullptr);
  /// The full flow (place, route, timing, QoR) on the session's engines.
  bool start_flow(const FlowOptions& opts, std::string* why = nullptr);
  bool start_qor(std::string* why = nullptr, QorOptions opts = {});
  /// Saves (.hdb) or writes (.def .pl .aux: io::write_design) the design.
  bool save(const std::string& path, std::string* why = nullptr, const io::Options& opts = {});
  void cancel();
  /// Blocks until the current job ends (scripts, tests), then polls.
  void wait();
  /// Picks up finished jobs. Call once per frame.
  void poll();
  /// Threads a job may use (0 = all but one hardware thread).
  void set_job_threads(int n) { job_threads_ = n; }
  int job_threads() const;

  /// How the last finished job ended (null before any).
  const JobEnd* last_end() const { return last_end_.serial ? &last_end_ : nullptr; }

  // --- Results (UI thread; set when a task ends) -----------------------------
  std::shared_ptr<const FlowReport> last_flow() const { return flow_report_done_; }
  std::shared_ptr<const PlacementResult> last_placement() const { return placement_; }
  std::shared_ptr<const LegalizeStepResult> last_legalize() const { return legalize_; }
  std::shared_ptr<const RouteStepResult> last_route() const { return route_; }
  std::shared_ptr<const TimingStepResult> last_timing() const { return timing_; }
  std::shared_ptr<const QorReport> qor() const { return qor_; }
  std::uint64_t qor_version() const { return qor_version_; }

  // --- Edits (UI thread, only when !busy()) ----------------------------------
  bool can_edit() const { return has_design() && !busy(); }
  bool move_cell(Id cell, Point loc, std::string* why = nullptr);
  bool undo(std::string* why = nullptr);
  std::size_t undo_depth() const { return undo_.size(); }

  // --- Messages -----------------------------------------------------------------
  /// Log records (Info and above) from every thread, newest last.
  std::vector<LogLine> log_lines(std::size_t max = 2000) const;
  std::uint64_t log_count() const { return log_count_.load(); }
  void note(std::string text, log::Level level = log::Level::Info);

 private:
  struct Loaded;
  void install(std::unique_ptr<Loaded> l);
  bool check_free(std::string* why) const;

  std::unique_ptr<Design> design_;
  std::unique_ptr<SnapshotPublisher> pub_;
  std::shared_ptr<const TechInfo> tech_;
  std::string source_;
  double load_seconds_ = 0.0;
  std::uint64_t generation_ = 0;

  JobHandle job_;
  std::string job_kind_;
  bool job_edits_ = false;
  JobEnd last_end_;
  /// Engine state for the current design (routes, timing settings). Only
  /// jobs touch it.
  std::unique_ptr<FlowEngines> engines_;
  std::shared_ptr<TaskOutput> task_out_;
  std::shared_ptr<const PlacementResult> placement_;
  std::shared_ptr<const LegalizeStepResult> legalize_;
  std::shared_ptr<const RouteStepResult> route_;
  std::shared_ptr<const TimingStepResult> timing_;
  int job_threads_ = 0;
  // Job outputs, filled on the job thread, picked up by poll().
  std::mutex mu_;
  std::unique_ptr<Loaded> loaded_;
  std::string load_error_;
  std::shared_ptr<const FlowReport> flow_report_done_;
  std::shared_ptr<const QorReport> qor_;
  std::uint64_t qor_version_ = 0;

  std::vector<std::pair<Id, Point>> undo_;

  mutable std::mutex log_mu_;
  std::deque<LogLine> log_;
  std::atomic<std::uint64_t> log_count_{0};
  log::SinkId sink_id_ = -1;
};

/// The layer stack and wire model for a design (call while no job writes it).
std::shared_ptr<TechInfo> make_tech_info(const Design& d, const GlobalRouterOptions& router = {});

}  // namespace hermes::gui
