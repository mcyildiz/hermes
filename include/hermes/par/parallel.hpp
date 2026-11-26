#pragma once

// Parallel runtime: one shared worker pool and deterministic fork-join loops.
//
//   hermes::par::set_num_threads(8);            // 0 = all hardware threads
//   par::parallel_for(0, n, [&](std::size_t lo, std::size_t hi) {
//     for (std::size_t i = lo; i < hi; ++i) out[i] = f(in[i]);
//   });
//   double s = par::parallel_sum(0, n, [&](std::size_t lo, std::size_t hi) {
//     double a = 0; for (auto i = lo; i < hi; ++i) a += v[i]; return a;
//   });
//
// Determinism: a range is cut into blocks whose boundaries depend only on the
// range and the grain, never on the number of threads. parallel_reduce
// combines the per-block results in block order, so a reduction gives
// bit-identical results with 1, 4 or 64 threads (the placer relies on this:
// its result does not depend on the machine it runs on).
//
// Scheduling: blocks are claimed dynamically from an atomic counter by the
// calling thread and by idle workers (load balance without work stealing
// queues). Any thread may call these functions, including workers (nested
// calls) and background job threads, and several loops may run at once: the
// pool shares its workers between them. The caller always works on its own
// loop, so nested and concurrent loops cannot deadlock.
//
// Exceptions thrown by a block are rethrown in the caller (the first one; the
// remaining blocks still run to completion).

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

namespace hermes::par {

/// Threads used by parallel loops (workers + the calling thread), >= 1.
int num_threads();
/// Sets the pool size. 0 = std::thread::hardware_concurrency(). 1 runs every
/// loop serially on the calling thread. Also read once from the environment
/// variable HERMES_THREADS at first use. Call it while no loop is running.
void set_num_threads(int n);
/// Hardware threads reported by the OS.
int hardware_threads();

/// RAII: limits loops started by *this thread* (and the loops nested in
/// them) to `n` threads while in scope, e.g. to run several engines side by
/// side, or to measure scaling. n <= 0 means no limit.
class ThreadLimit {
 public:
  explicit ThreadLimit(int n);
  ~ThreadLimit();
  ThreadLimit(const ThreadLimit&) = delete;
  ThreadLimit& operator=(const ThreadLimit&) = delete;

 private:
  int prev_;
};

/// Blocks a range [0, n) is cut into for a given grain (independent of the
/// thread count). At most kMaxBlocks blocks; each has >= grain items unless
/// n < grain.
inline constexpr std::size_t kMaxBlocks = 4096;
std::size_t block_count(std::size_t n, std::size_t grain);

namespace detail {
/// Non-owning reference to a callable void(std::size_t) (no allocation).
class BlockFn {
 public:
  template <class F>
  BlockFn(F& f) : obj_(&f), call_([](void* o, std::size_t b) { (*static_cast<F*>(o))(b); }) {}
  void operator()(std::size_t b) const { call_(obj_, b); }

 private:
  void* obj_;
  void (*call_)(void*, std::size_t);
};
/// Runs body(block) for block in [0, nblocks) on the pool.
void run_blocks(std::size_t nblocks, BlockFn body);
}  // namespace detail

/// Default grain: blocks of at least this many items.
inline constexpr std::size_t kDefaultGrain = 2048;

/// Calls f(lo, hi) on disjoint sub-ranges covering [begin, end).
template <class F>
void parallel_for(std::size_t begin, std::size_t end, std::size_t grain, F&& f) {
  if (end <= begin) return;
  const std::size_t n = end - begin;
  const std::size_t nb = block_count(n, grain);
  if (nb <= 1) {
    f(begin, end);
    return;
  }
  const std::size_t bs = (n + nb - 1) / nb;
  auto body = [&](std::size_t b) {
    const std::size_t lo = begin + b * bs;
    if (lo < end) f(lo, std::min(end, lo + bs));
  };
  detail::run_blocks(nb, body);
}
template <class F>
void parallel_for(std::size_t begin, std::size_t end, F&& f) {
  parallel_for(begin, end, kDefaultGrain, std::forward<F>(f));
}

/// Calls f(i) for each i in [begin, end) (one task per index: for coarse
/// work such as regions, rows or independent engines).
template <class F>
void parallel_for_each(std::size_t begin, std::size_t end, F&& f) {
  if (end <= begin) return;
  if (end - begin == 1) {
    f(begin);
    return;
  }
  auto body = [&](std::size_t b) { f(begin + b); };
  detail::run_blocks(end - begin, body);
}

/// Deterministic reduction: map(lo, hi) -> T per block, combined in block
/// order with combine(acc, block_value) starting from `identity`.
template <class T, class Map, class Combine>
T parallel_reduce(std::size_t begin, std::size_t end, std::size_t grain, T identity, Map&& map,
                  Combine&& combine) {
  if (end <= begin) return identity;
  const std::size_t n = end - begin;
  const std::size_t nb = block_count(n, grain);
  if (nb <= 1) return combine(std::move(identity), map(begin, end));
  const std::size_t bs = (n + nb - 1) / nb;
  std::vector<T> part(nb, identity);
  auto body = [&](std::size_t b) {
    const std::size_t lo = begin + b * bs;
    if (lo < end) part[b] = map(lo, std::min(end, lo + bs));
  };
  detail::run_blocks(nb, body);
  T acc = std::move(identity);
  for (std::size_t b = 0; b < nb; ++b) acc = combine(std::move(acc), std::move(part[b]));
  return acc;
}

/// parallel_reduce with +.
template <class Map>
auto parallel_sum(std::size_t begin, std::size_t end, std::size_t grain, Map&& map) {
  using T = std::decay_t<decltype(map(begin, end))>;
  return parallel_reduce(begin, end, grain, T{}, std::forward<Map>(map),
                         [](T a, T b) { return a + b; });
}
template <class Map>
auto parallel_sum(std::size_t begin, std::size_t end, Map&& map) {
  return parallel_sum(begin, end, kDefaultGrain, std::forward<Map>(map));
}

/// Runs the callables concurrently and waits for all of them.
template <class... F>
void parallel_invoke(F&&... f) {
  std::function<void()> fs[] = {std::function<void()>(std::forward<F>(f))...};
  parallel_for_each(0, sizeof...(F), [&](std::size_t i) { fs[i](); });
}

/// Pool statistics (for tests and benchmarks).
struct PoolStats {
  std::uint64_t loops = 0;          ///< Loops that went to the pool.
  std::uint64_t inline_loops = 0;   ///< Loops run serially (1 block / 1 thread).
  std::uint64_t blocks = 0;         ///< Blocks executed by the pool.
};
PoolStats pool_stats();

/// Index of the current pool worker in [1, num_threads()), or 0 for any
/// thread that is not a pool worker.
int worker_index();

}  // namespace hermes::par
