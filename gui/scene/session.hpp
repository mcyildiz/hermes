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
#include <string>
#include <vector>

#include "gui/scene/scene.hpp"
#include "hermes/db/design.hpp"
#include "hermes/db/snapshot.hpp"
#include "hermes/db/synthetic.hpp"
#include "hermes/flow/flow.hpp"
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
  bool start_flow(const FlowOptions& opts, std::string* why = nullptr);
  bool start_qor(std::string* why = nullptr);
  bool save(const std::string& path, std::string* why = nullptr);
  void cancel();
  /// Blocks until the current job ends (scripts, tests), then polls.
  void wait();
  /// Picks up finished jobs. Call once per frame.
  void poll();
  /// Threads a job may use (0 = all but one hardware thread).
  void set_job_threads(int n) { job_threads_ = n; }
  int job_threads() const;

  // --- Results ---------------------------------------------------------------
  std::shared_ptr<const FlowReport> last_flow() const { return flow_report_done_; }
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
  int job_threads_ = 0;
  // Job outputs, filled on the job thread, picked up by poll().
  std::mutex mu_;
  std::unique_ptr<Loaded> loaded_;
  std::string load_error_;
  std::shared_ptr<FlowReport> flow_report_;      // being filled by a flow job
  std::shared_ptr<const FlowReport> flow_report_done_;
  std::shared_ptr<QorReport> qor_pending_;
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
