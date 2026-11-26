// Cost of the end-of-run QoR report and of run tracking on a 1M-cell design.
//
//   ./build/hermes_qor_bench [num_cells]      (default 1000000)
//
// Two placements: "local" (cells laid out in netlist order along the rows,
// so nets are short, like a placed design) and "random" (worst case: every
// net spans a large part of the die).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <random>
#include <string>

#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/qor/qor.hpp"
#include "hermes/route/global_router.hpp"
#include "hermes/track/run_tracker.hpp"

using namespace hermes;

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

constexpr Dbu kSite = 200, kRowH = 2000;

Design make_design(int n, bool local) {
  std::mt19937 rng(11);
  std::uniform_int_distribution<int> wk(1, 4);
  std::vector<int> w(n);
  double area = 0;
  for (int& x : w) {
    x = 2 * wk(rng);
    area += static_cast<double>(x * kSite) * kRowH;
  }
  const int macros = std::max(4, n / 20000);
  const Dbu mside = 80000;
  area += macros * static_cast<double>(mside) * mside;
  const Dbu side = static_cast<Dbu>(std::sqrt(area / 0.7) / kRowH + 1) * kRowH;
  Design d(local ? "qor_local" : "qor_random");
  d.set_die({0, 0, side, side});
  d.set_dbu_per_micron(2000);
  d.set_row_height(kRowH);
  d.reserve(n + macros, n, 4 * static_cast<std::size_t>(n));
  // Macros on a coarse grid; std cells fill rows in netlist order (local) or randomly.
  std::uniform_int_distribution<Dbu> px(0, side / kSite - 10), py(0, side / kRowH - 1);
  Dbu x = 0, y = 0;
  for (int i = 0; i < n; ++i) {
    const Id c = d.add_cell("u" + std::to_string(i), w[i] * kSite, kRowH);
    if (local) {
      if (x + w[i] * kSite > side) {
        x = 0;
        y += kRowH;
      }
      d.move_cell(c, {x, std::min(y, side - kRowH)});
      x += w[i] * kSite * 10 / 7;  // ~70% row utilization
    } else {
      d.move_cell(c, {px(rng) * kSite, py(rng) * kRowH});
    }
  }
  for (int m = 0; m < macros; ++m) {
    const Id c = d.add_cell("ram" + std::to_string(m), mside, mside, true);
    d.move_cell(c, {(m * 7919 % 97) * (side - mside) / 97, (m * 104729 % 89) * (side - mside) / 89});
    d.set_fixed(c, true);
  }
  // Nets: 2-6 pins among netlist neighbours (+-200 ids), like the io bench.
  std::uniform_int_distribution<int> deg(2, 6), near(-200, 200);
  for (int i = 0; i < n; ++i) {
    const Id net = d.add_net("n" + std::to_string(i));
    const int k = deg(rng);
    for (int p = 0; p < k; ++p) {
      const int c = p == 0 ? i : std::clamp(i + near(rng), 0, n - 1);
      d.connect(c, net, "p" + std::to_string(p), {kSite, kRowH / 2}, p == 0 ? PinDir::Output : PinDir::Input);
    }
  }
  std::vector<RoutingLayer> layers;
  const Dbu pitch[9] = {200, 200, 200, 400, 400, 400, 800, 800, 1600};
  for (int l = 0; l < 9; ++l)
    layers.push_back({"M" + std::to_string(l + 1), l % 2 ? LayerDir::Vertical : LayerDir::Horizontal, pitch[l],
                      pitch[l] / 2, pitch[l] / 2});
  d.set_routing_layers(std::move(layers));
  d.compact();
  return d;
}

void bench_qor(const Design& d, int grid) {
  GlobalRouterOptions ro;
  ro.grid_x = ro.grid_y = grid;
  GlobalRouter gr(ro);
  for (int t : {1, 2, 4, 0}) {
    if (t > par::hardware_threads()) continue;
    par::set_num_threads(t);
    auto t0 = Clock::now();
    const RouteUsage3D u = gr.route_usage(d);
    const double tu = ms_since(t0);
    QorOptions qo;
    t0 = Clock::now();
    const QorReport q = compute_qor(d, qo, &u);
    const double tq = ms_since(t0);
    qo.maps = false;
    t0 = Clock::now();
    (void)compute_qor(d, qo, &u);
    const double tqn = ms_since(t0);
    std::string secs;
    for (const auto& [k, v] : q.seconds) secs += std::format(" {} {:.0f}", k, 1e3 * v);
    std::printf("  %-10s grid %4d  threads %2d   route usage %7.1f ms   report %7.1f ms (no maps %7.1f)   [%s ]\n",
                d.name().c_str(), grid, par::num_threads(), tu, tq, tqn, secs.c_str());
  }
  par::set_num_threads(0);
}

}  // namespace

int main(int argc, char** argv) {
  const int n = argc > 1 ? std::atoi(argv[1]) : 1000000;
  log::console_sink()->set_level(log::Level::Warn);
  std::printf("QoR report cost, %d cells (%d hardware threads)\n", n, par::hardware_threads());
  for (bool local : {true, false}) {
    auto t0 = Clock::now();
    const Design d = make_design(n, local);
    std::printf("  built %s in %.0f ms: %zu cells, %zu nets, %zu pins, die %lld dbu\n", d.name().c_str(), ms_since(t0),
                d.num_cells(), d.num_nets(), d.num_pins(), static_cast<long long>(d.die().width()));
    for (int grid : {64, 256, 1024}) bench_qor(d, grid);
    if (local) {
      const QorReport q = compute_qor(d, {}, nullptr, 256, 256);
      std::printf("\n%s\n", format_qor(q).c_str());
    }
  }

  // Run tracking overhead on the engine thread.
  const std::string db = (std::filesystem::temp_directory_path() / "hermes_qor_bench_runs.db").string();
  std::filesystem::remove(db);
  {
    track::TrackerOptions to;
    to.db_path = db;
    track::RunTracker t({"bench", "bench", "", "hermes_qor_bench", {}, "{}"}, to);
    constexpr int kN = 150000;  // below the default queue cap (200k events per flush interval)
    auto t0 = Clock::now();
    for (int i = 0; i < kN; ++i) track::metric("bench.metric", i, i * 0.5);
    const double per = ms_since(t0) * 1e6 / kN;
    t0 = Clock::now();
    t.sync();
    std::printf("run tracking: track::metric() %.0f ns per call on the engine thread (%zu dropped), "
                "writer committed %d metrics in %.0f ms after the loop\n",
                per, t.dropped(), kN, ms_since(t0));
    t.finish("done");
  }
  {
    // No active tracker: the cost engines pay when tracking is off.
    constexpr int kN = 100000000;
    double s = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < kN; ++i) {
      if (track::active()) track::metric("x", i, i);
      s += i;
    }
    std::printf("run tracking off: %.2f ns per check (sum %g)\n", ms_since(t0) * 1e6 / kN, s);
  }
  std::filesystem::remove(db);
  return 0;
}
