#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define HERMES_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#define HERMES_PAUSE() asm volatile("yield")
#else
#define HERMES_PAUSE() ((void)0)
#endif

namespace hermes::par {

namespace {

HERMES_LOG_CHANNEL(log_par, "par");

thread_local int t_limit = 0;         // ThreadLimit of this thread (0 = none)
thread_local int t_worker = 0;        // 1-based worker index, 0 = not a worker

struct Loop {
  const detail::BlockFn* body = nullptr;
  std::size_t nblocks = 0;
  int max_helpers = 0;  // workers allowed to join (threads - 1)
  int limit = 0;        // ThreadLimit propagated into nested loops
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> done{0};
  std::atomic<int> helpers{0};
  std::mutex err_mu;
  std::exception_ptr err;

  // Claims and runs blocks until none is left. Returns when this thread can
  // claim no more (others may still be running theirs).
  void work() {
    for (;;) {
      const std::size_t b = next.fetch_add(1, std::memory_order_relaxed);
      if (b >= nblocks) return;
      try {
        (*body)(b);
      } catch (...) {
        std::lock_guard lk(err_mu);
        if (!err) err = std::current_exception();
      }
      if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == nblocks) done.notify_all();
    }
  }
};

class Pool {
 public:
  ~Pool() { stop_workers(); }

  int threads() {
    ensure_started();
    return threads_.load(std::memory_order_relaxed);
  }

  void resize(int n) {
    std::lock_guard ctl(control_mu_);
    if (n <= 0) n = hardware_threads();
    n = std::clamp(n, 1, 1024);
    if (started_ && n == threads_.load()) return;
    stop_workers();
    start_workers(n);
  }

  void run(std::size_t nblocks, const detail::BlockFn& body) {
    const int total = threads();
    const int lim = t_limit > 0 ? std::min(t_limit, total) : total;
    if (lim <= 1 || nblocks <= 1) {
      inline_loops_.fetch_add(1, std::memory_order_relaxed);
      for (std::size_t b = 0; b < nblocks; ++b) body(b);
      return;
    }
    loops_.fetch_add(1, std::memory_order_relaxed);
    blocks_.fetch_add(nblocks, std::memory_order_relaxed);
    auto loop = std::make_shared<Loop>();
    loop->body = &body;
    loop->nblocks = nblocks;
    loop->max_helpers = static_cast<int>(std::min<std::size_t>(lim - 1, nblocks - 1));
    loop->limit = t_limit;
    {
      std::lock_guard lk(mu_);
      active_.push_back(loop);
      epoch_.fetch_add(1, std::memory_order_release);
    }
    if (sleeping_.load(std::memory_order_acquire) > 0) {
      for (int i = 0; i < loop->max_helpers; ++i) cv_.notify_one();
    }
    loop->work();
    {
      std::lock_guard lk(mu_);
      auto it = std::find(active_.begin(), active_.end(), loop);
      if (it != active_.end()) active_.erase(it);
    }
    // Wait for blocks other threads are still running.
    for (int spin = 0; loop->done.load(std::memory_order_acquire) < nblocks; ++spin) {
      if (spin < 4096) {
        HERMES_PAUSE();
      } else {
        const std::size_t d = loop->done.load(std::memory_order_acquire);
        if (d < nblocks) loop->done.wait(d, std::memory_order_acquire);
      }
    }
    if (loop->err) std::rethrow_exception(loop->err);
  }

  PoolStats stats() const {
    return {loops_.load(), inline_loops_.load(), blocks_.load()};
  }

 private:
  void ensure_started() {
    if (started_flag_.load(std::memory_order_acquire)) return;
    std::lock_guard ctl(control_mu_);
    if (started_) return;
    int n = 0;
    if (const char* e = std::getenv("HERMES_THREADS")) n = std::atoi(e);
    if (n <= 0) n = hardware_threads();
    start_workers(std::clamp(n, 1, 1024));
  }

  void start_workers(int n) {
    stop_ = false;
    threads_.store(n, std::memory_order_relaxed);
    workers_.reserve(n - 1);
    for (int i = 1; i < n; ++i) workers_.emplace_back([this, i] { worker_main(i); });
    started_ = true;
    started_flag_.store(true, std::memory_order_release);
    HLOG_DEBUG(log_par, "thread pool: {} threads", n);
  }

  void stop_workers() {
    {
      std::lock_guard lk(mu_);
      stop_ = true;
      epoch_.fetch_add(1, std::memory_order_release);
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
    workers_.clear();
  }

  // Finds a loop that still has blocks and room for a helper (mu_ held).
  std::shared_ptr<Loop> pick() {
    for (auto& l : active_) {
      if (l->next.load(std::memory_order_relaxed) < l->nblocks &&
          l->helpers.load(std::memory_order_relaxed) < l->max_helpers) {
        l->helpers.fetch_add(1, std::memory_order_relaxed);
        return l;
      }
    }
    return nullptr;
  }

  void worker_main(int index) {
    t_worker = index;
    std::uint64_t seen = 0;
    for (;;) {
      std::shared_ptr<Loop> loop;
      {
        // Spin briefly first: loops often come back to back (one Nesterov
        // iteration runs a dozen), and sleeping/waking costs ~10-50 us.
        for (int spin = 0; spin < 20000; ++spin) {
          if (epoch_.load(std::memory_order_acquire) != seen) break;
          HERMES_PAUSE();
        }
        std::unique_lock lk(mu_);
        for (;;) {
          if (stop_) return;
          seen = epoch_.load(std::memory_order_acquire);
          loop = pick();
          if (loop) break;
          sleeping_.fetch_add(1, std::memory_order_release);
          cv_.wait(lk, [&] { return stop_ || epoch_.load(std::memory_order_acquire) != seen; });
          sleeping_.fetch_sub(1, std::memory_order_release);
        }
      }
      t_limit = loop->limit;
      loop->work();
      t_limit = 0;
      loop->helpers.fetch_sub(1, std::memory_order_relaxed);
      // Another loop may have been pushed while this one ran: look again
      // without waiting for the epoch to change.
      seen = 0;
    }
  }

  std::mutex control_mu_;  // serializes start/resize
  bool started_ = false;
  std::atomic<bool> started_flag_{false};
  std::atomic<int> threads_{1};
  std::vector<std::thread> workers_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::shared_ptr<Loop>> active_;
  std::atomic<std::uint64_t> epoch_{1};
  std::atomic<int> sleeping_{0};
  bool stop_ = false;

  std::atomic<std::uint64_t> loops_{0}, inline_loops_{0}, blocks_{0};
};

Pool& pool() {
  static Pool p;
  return p;
}

}  // namespace

int hardware_threads() {
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 1 : static_cast<int>(n);
}

int num_threads() {
  const int total = pool().threads();
  return t_limit > 0 ? std::min(t_limit, total) : total;
}

void set_num_threads(int n) { pool().resize(n); }

ThreadLimit::ThreadLimit(int n) : prev_(t_limit) {
  if (n > 0) t_limit = prev_ > 0 ? std::min(prev_, n) : n;
}
ThreadLimit::~ThreadLimit() { t_limit = prev_; }

std::size_t block_count(std::size_t n, std::size_t grain) {
  if (n == 0) return 0;
  grain = std::max<std::size_t>(1, grain);
  return std::min(kMaxBlocks, (n + grain - 1) / grain);
}

void detail::run_blocks(std::size_t nblocks, BlockFn body) {
  pool().run(nblocks, body);
}

PoolStats pool_stats() { return pool().stats(); }

int worker_index() { return t_worker; }

}  // namespace hermes::par
