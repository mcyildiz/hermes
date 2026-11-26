#include "hermes/par/job.hpp"

#include <algorithm>
#include <condition_variable>
#include <thread>

#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"

namespace hermes {

namespace {
HERMES_LOG_CHANNEL(log_job, "par.job");
}  // namespace

const char* to_string(JobState s) {
  switch (s) {
    case JobState::Running: return "running";
    case JobState::Done: return "done";
    case JobState::Cancelled: return "cancelled";
    case JobState::Failed: return "failed";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// RunControl
// ---------------------------------------------------------------------------

RunControl::RunControl(double publish_interval_s)
    : publish_interval_s_(publish_interval_s), start_(Clock::now()), last_publish_(start_) {}

void RunControl::progress(double fraction, std::string_view message) {
  std::function<void(const JobProgress&)> cb;
  JobProgress copy;
  {
    std::lock_guard lk(mu_);
    p_.fraction = std::clamp(fraction, 0.0, 1.0);
    if (!message.empty()) p_.message.assign(message);
    ++p_.updates;
    p_.elapsed_s = std::chrono::duration<double>(Clock::now() - start_).count();
    if (cb_) {
      cb = cb_;
      copy = p_;
    }
  }
  if (cb) cb(copy);
}

void RunControl::begin_stage(std::string_view name, double lo, double hi) {
  {
    std::lock_guard lk(mu_);
    p_.stage.assign(name);
    stage_lo_ = lo;
    stage_hi_ = hi;
  }
  HLOG_VERBOSE(log_job, "stage: {}", name);
  progress(lo);
}

void RunControl::stage_progress(double fraction, std::string_view message) {
  double lo, hi;
  {
    std::lock_guard lk(mu_);
    lo = stage_lo_;
    hi = stage_hi_;
  }
  progress(lo + (hi - lo) * std::clamp(fraction, 0.0, 1.0), message);
}

bool RunControl::publish_due() {
  if (publish_interval_s_ <= 0.0) return false;
  const auto now = Clock::now();
  if (std::chrono::duration<double>(now - last_publish_).count() < publish_interval_s_) return false;
  last_publish_ = now;
  return true;
}

JobProgress RunControl::snapshot() const {
  std::lock_guard lk(mu_);
  JobProgress p = p_;
  p.elapsed_s = std::chrono::duration<double>(Clock::now() - start_).count();
  return p;
}

void RunControl::on_progress(std::function<void(const JobProgress&)> cb) {
  std::lock_guard lk(mu_);
  cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------

struct JobHandle::State {
  std::string name;
  RunControl ctl;
  std::mutex mu;
  std::condition_variable cv;
  JobState state = JobState::Running;
  std::exception_ptr error;
  std::thread thread;

  explicit State(std::string n, double publish) : name(std::move(n)), ctl(publish) {}
};

// The last handle cancels and joins a job that is still running.
struct JobHandle::Owner {
  std::unique_ptr<State> st;
  ~Owner() {
    if (!st) return;
    st->ctl.request_cancel();
    if (st->thread.joinable()) st->thread.join();
  }
};

JobHandle start_job(std::string name, std::function<void(RunControl&)> fn, JobOptions opts) {
  JobHandle h;
  h.s_ = std::make_shared<JobHandle::Owner>();
  h.s_->st = std::make_unique<JobHandle::State>(std::move(name), opts.publish_interval_s);
  JobHandle::State* st = h.s_->st.get();
  if (opts.on_progress) st->ctl.on_progress(std::move(opts.on_progress));
  const int max_threads = opts.max_threads;
  HLOG_DEBUG(log_job, "job '{}' started", st->name);
  st->thread = std::thread([st, max_threads, fn = std::move(fn)] {
    JobState end = JobState::Done;
    std::exception_ptr err;
    try {
      par::ThreadLimit limit(max_threads);
      fn(st->ctl);
      if (st->ctl.cancelled()) end = JobState::Cancelled;
    } catch (const Cancelled&) {
      end = JobState::Cancelled;
    } catch (...) {
      end = JobState::Failed;
      err = std::current_exception();
    }
    if (end != JobState::Failed) st->ctl.progress(end == JobState::Done ? 1.0 : st->ctl.snapshot().fraction);
    HLOG_DEBUG(log_job, "job '{}' {}", st->name, to_string(end));
    {
      std::lock_guard lk(st->mu);
      st->state = end;
      st->error = err;
    }
    st->cv.notify_all();
  });
  return h;
}

const std::string& JobHandle::name() const { return s_->st->name; }

JobState JobHandle::state() const {
  std::lock_guard lk(s_->st->mu);
  return s_->st->state;
}

JobProgress JobHandle::progress() const { return s_->st->ctl.snapshot(); }

void JobHandle::cancel() { s_->st->ctl.request_cancel(); }

void JobHandle::wait() {
  State& st = *s_->st;
  std::unique_lock lk(st.mu);
  st.cv.wait(lk, [&] { return st.state != JobState::Running; });
}

bool JobHandle::wait_for(double seconds) {
  State& st = *s_->st;
  std::unique_lock lk(st.mu);
  return st.cv.wait_for(lk, std::chrono::duration<double>(seconds),
                        [&] { return st.state != JobState::Running; });
}

std::exception_ptr JobHandle::error() const {
  std::lock_guard lk(s_->st->mu);
  return s_->st->error;
}

void JobHandle::rethrow_if_failed() const {
  if (auto e = error()) std::rethrow_exception(e);
}

RunControl& JobHandle::control() { return s_->st->ctl; }

}  // namespace hermes
