#pragma once

// Background jobs: engines run off the caller's thread with progress
// reporting and cancellation, so a GUI (or a Python prompt, or a server)
// stays responsive while placement or routing runs.
//
//   SnapshotPublisher pub(design);                 // GUI reads pub.latest()
//   JobHandle job = start_job("place", [&](RunControl& ctl) {
//     MixedSizePlacer(opts).run(design, &sta, &router, &ctl);
//   });
//   ... GUI loop: draw(pub.latest()); show(job.progress()); if (esc) job.cancel();
//   job.wait();
//
// Rules:
//  * The job is the design's only writer while it runs. Other threads read
//    snapshots (SnapshotPublisher), never the Design itself.
//  * Engines poll ctl.cancelled() at safe points (between iterations,
//    batches, stages) and stop there, leaving the design consistent and
//    flushed. Throwing Cancelled from inside an engine is also fine.
//  * Engines call ctl.publish_due() at batch points; when it returns true
//    they write their state back and flush, so snapshot readers see
//    progress at the job's publish interval (not on every iteration).
//  * A job runs on its own thread and uses the shared par:: pool for its
//    parallel loops, so several jobs can run at once.
//  * JobHandle is a shared handle. When the last handle goes away, a job
//    still running is cancelled and joined (like std::jthread).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hermes {

enum class JobState : std::uint8_t { Running, Done, Cancelled, Failed };
const char* to_string(JobState s);

/// Thrown by RunControl::check_cancelled() (and by engines that unwind on
/// cancellation). The job then ends in state Cancelled, not Failed.
struct Cancelled : std::runtime_error {
  Cancelled() : std::runtime_error("cancelled") {}
};

/// Snapshot of a job's progress, safe to read from any thread.
struct JobProgress {
  std::string stage;          ///< e.g. "global placement", "legalization"
  double fraction = 0.0;      ///< Overall [0, 1] (best effort estimate).
  std::string message;        ///< Latest short status line.
  std::uint64_t updates = 0;  ///< Number of progress reports so far.
  double elapsed_s = 0.0;
};

/// The engine side of a job: cancellation, progress, publish cadence. Also
/// usable without a job (e.g. a test that cancels from another thread).
class RunControl {
 public:
  explicit RunControl(double publish_interval_s = 0.25);

  // --- Engine side ----------------------------------------------------------
  bool cancelled() const { return cancel_.load(std::memory_order_relaxed); }
  void check_cancelled() const {
    if (cancelled()) throw Cancelled();
  }
  /// Reports progress. `fraction` is the overall fraction done; stages that
  /// know only their own progress use stage_progress().
  void progress(double fraction, std::string_view message = {});
  /// Starts a stage that covers [lo, hi] of the overall progress.
  void begin_stage(std::string_view name, double lo, double hi);
  /// Progress within the current stage, mapped to [lo, hi].
  void stage_progress(double fraction, std::string_view message = {});
  /// True at most once per publish interval: the engine should write back
  /// and flush now so snapshot readers see its progress. Always false with
  /// an interval <= 0.
  bool publish_due();

  // --- Controller side (any thread) ---------------------------------------
  void request_cancel() { cancel_.store(true, std::memory_order_relaxed); }
  JobProgress snapshot() const;
  /// Optional callback on every progress() call (runs on the engine thread;
  /// keep it cheap).
  void on_progress(std::function<void(const JobProgress&)> cb);
  void set_publish_interval(double seconds) { publish_interval_s_ = seconds; }

 private:
  using Clock = std::chrono::steady_clock;
  std::atomic<bool> cancel_{false};
  double publish_interval_s_;
  Clock::time_point start_, last_publish_;
  mutable std::mutex mu_;  // guards the fields below
  JobProgress p_;
  double stage_lo_ = 0.0, stage_hi_ = 1.0;
  std::function<void(const JobProgress&)> cb_;
};

struct JobOptions {
  /// Seconds between snapshot publishes requested from the engine (0 = only
  /// at the engine's own batch points).
  double publish_interval_s = 0.25;
  /// Max threads the job's parallel loops use (0 = whole pool), e.g. to
  /// keep cores free for the GUI or another job.
  int max_threads = 0;
  std::function<void(const JobProgress&)> on_progress;
};

class JobHandle {
 public:
  JobHandle() = default;

  bool valid() const { return s_ != nullptr; }
  const std::string& name() const;
  JobState state() const;
  bool running() const { return state() == JobState::Running; }
  JobProgress progress() const;
  /// Asks the job to stop at its next safe point (returns immediately).
  void cancel();
  /// Blocks until the job ends. Rethrows nothing: see error().
  void wait();
  /// Waits up to `seconds`; true if the job has ended.
  bool wait_for(double seconds);
  /// The exception a Failed job ended with (null otherwise).
  std::exception_ptr error() const;
  /// Rethrows error() if the job failed.
  void rethrow_if_failed() const;
  RunControl& control();

 private:
  friend JobHandle start_job(std::string, std::function<void(RunControl&)>, JobOptions);
  struct State;
  struct Owner;
  std::shared_ptr<Owner> s_;
};

/// Runs fn(ctl) on a new thread and returns at once.
JobHandle start_job(std::string name, std::function<void(RunControl&)> fn,
                    JobOptions opts = {});

}  // namespace hermes
