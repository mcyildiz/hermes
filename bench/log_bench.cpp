// Measures what logging costs inside hot loops.
//
//   hermes_log_bench [iterations]
//
// Each kernel runs with and without a log call in its loop body; the log
// call's channel is off (the normal case in production). Reported numbers
// are the best of several repetitions, in ns per loop iteration.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hermes/log/log.hpp"
#include "hermes/log/sinks.hpp"

namespace lg = hermes::log;

namespace {

HERMES_LOG_CHANNEL(log_bench, "bench.hot");

volatile double g_sink_d;
volatile std::int64_t g_sink_i;

constexpr std::size_t kArr = 1 << 14;  // fits in L1/L2
std::vector<double> g_x(kArr);
std::vector<std::int32_t> g_v(kArr);

enum class Mode { None, Debug, DebugId, Trace, CappedWarn };

// Floating-point kernel with a loop-carried dependency (typical of
// placement/routing inner loops: gradient accumulation, cost updates).
template <Mode M>
__attribute__((noinline)) double fp_kernel(std::size_t n) {
  double s = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double x = g_x[i & (kArr - 1)];
    s += std::sqrt(x) * 1.000001;
    if constexpr (M == Mode::Debug) HLOG_DEBUG(log_bench, "i={} x={} s={}", i, x, s);
    if constexpr (M == Mode::DebugId) HLOG_DEBUG_ID(log_bench, "BENCH-1", "i={} s={}", i, s);
    if constexpr (M == Mode::Trace) HLOG_TRACE(log_bench, "i={} x={} s={}", i, x, s);
    if constexpr (M == Mode::CappedWarn) HLOG_WARN_ID(log_bench, "BENCH-2", "i={} s={}", i, s);
  }
  return s;
}

// Integer reduction the compiler vectorizes when nothing else is in the loop
// (worst case: a log call can stop vectorization).
template <Mode M>
__attribute__((noinline)) std::int64_t int_kernel(std::size_t n) {
  std::int64_t s = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const std::int32_t v = g_v[i & (kArr - 1)];
    s += v;
    if constexpr (M == Mode::Debug) HLOG_DEBUG(log_bench, "i={} v={}", i, v);
    if constexpr (M == Mode::Trace) HLOG_TRACE(log_bench, "i={} v={}", i, v);
  }
  return s;
}

template <class F>
double best_ns_per_iter(std::size_t n, F&& f, int reps = 7) {
  double best = 1e300;
  for (int r = 0; r < reps; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto t1 = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
  }
  return best;
}

template <Mode M>
double fp(std::size_t n) {
  return best_ns_per_iter(n, [&] { g_sink_d = fp_kernel<M>(n); });
}
template <Mode M>
double in(std::size_t n) {
  return best_ns_per_iter(n, [&] { g_sink_i = int_kernel<M>(n); });
}

template <Mode M>
double fp_threads(std::size_t n, unsigned threads) {
  return best_ns_per_iter(n, [&] {
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; ++t) ts.emplace_back([&] { g_sink_d = fp_kernel<M>(n); });
    for (auto& t : ts) t.join();
  }, 5);
}

struct NullSink : lg::Sink {
  void write(const lg::Record&) override {}
};

void row(const char* name, double base, double with) {
  std::printf("  %-44s %7.3f ns  %7.3f ns  %+7.3f ns  (%+5.1f%%)\n", name, base, with, with - base,
              100.0 * (with - base) / base);
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 50'000'000;
  for (std::size_t i = 0; i < kArr; ++i) {
    g_x[i] = 1.0 + static_cast<double>(i % 97);
    g_v[i] = static_cast<std::int32_t>(i % 1013);
  }
  lg::set_level("*", lg::Level::Info);  // Debug off, Trace compiled out in Release
  lg::set_message_cap("BENCH-2", 1);    // enabled level, capped after 1 print
  lg::console_sink()->set_level(lg::Level::Off);

  std::printf("hermes log overhead, %zu iterations, compiled max level = %s\n", n,
              std::string(lg::to_string(lg::kCompiledMaxLevel)).c_str());
  std::printf("  %-44s %10s  %10s  %10s\n", "kernel / log call in loop", "no log", "with log",
              "delta/iter");

  const double fp0 = fp<Mode::None>(n);
  row("fp loop, HLOG_DEBUG (channel off)", fp0, fp<Mode::Debug>(n));
  row("fp loop, HLOG_DEBUG_ID (channel off)", fp0, fp<Mode::DebugId>(n));
  row("fp loop, HLOG_TRACE (compiled out)", fp0, fp<Mode::Trace>(n));
  row("fp loop, HLOG_WARN_ID (on, capped at 1)", fp0, fp<Mode::CappedWarn>(n));

  const double in0 = in<Mode::None>(n);
  row("int loop (vectorizable), HLOG_DEBUG (off)", in0, in<Mode::Debug>(n));
  row("int loop (vectorizable), HLOG_TRACE (out)", in0, in<Mode::Trace>(n));

  const unsigned hw = std::max(2u, std::min(16u, std::thread::hardware_concurrency()));
  const double mt0 = fp_threads<Mode::None>(n / 4, hw);
  char name[96];
  std::snprintf(name, sizeof name, "fp loop x %u threads, HLOG_DEBUG (off)", hw);
  row(name, mt0, fp_threads<Mode::Debug>(n / 4, hw));
  std::snprintf(name, sizeof name, "fp loop x %u threads, WARN_ID capped", hw);
  row(name, mt0, fp_threads<Mode::CappedWarn>(n / 4, hw));

  // Enabled path, for reference (formatting + dispatch, no I/O).
  {
    const std::size_t m = 1'000'000;
    auto null = std::make_shared<NullSink>();
    const auto id = lg::add_sink(null);
    lg::set_level("bench", lg::Level::Debug);
    const double t = best_ns_per_iter(m, [&] {
      for (std::size_t i = 0; i < m; ++i) HLOG_DEBUG(log_bench, "i={} x={}", i, 0.5 * i);
    }, 3);
    std::printf("  enabled HLOG_DEBUG to a null sink:          %7.1f ns per message\n", t);
    lg::remove_sink(id);
    auto devnull = std::make_shared<lg::TextSink>(std::string("/dev/null"));
    const auto id2 = lg::add_sink(devnull);
    const double t2 = best_ns_per_iter(m, [&] {
      for (std::size_t i = 0; i < m; ++i) HLOG_DEBUG(log_bench, "i={} x={}", i, 0.5 * i);
    }, 3);
    std::printf("  enabled HLOG_DEBUG to a text file sink:     %7.1f ns per message\n", t2);
    lg::remove_sink(id2);
    lg::clear_level("bench");
  }
  return 0;
}
