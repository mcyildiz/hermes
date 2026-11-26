// Parallel runtime benchmark (`hermes_par_bench`): speedup vs threads.
//
//   ./build/hermes_par_bench [num_cells=1000000] [max_threads=all] [flow_cells=20000]
//
// For 1, 2, 4, ... threads it times the parallel hot loops on the synthetic
// design of hermes_db_bench: pool overhead, HPWL, RUDY, global route, STA,
// the placer's kernels (WA gradient, density deposit, Poisson solve,
// density gradient), whole global-placement iterations, snapshot publishing
// and a small full flow. Every result is checked to be identical across
// thread counts (the runtime is deterministic).
//
// It also builds against a tree without the parallel runtime (it then times
// only the global-placement iterations and the flow) for before/after
// numbers.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "hermes/db/design.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/log/log.hpp"
#include "hermes/place/placer.hpp"

#if __has_include("hermes/par/parallel.hpp")
#define HERMES_HAS_PAR 1
#include "hermes/compute/backend.hpp"
#include "hermes/db/snapshot.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/place/analytic.hpp"
#include "hermes/place/congestion.hpp"
#include "hermes/route/global_router.hpp"
#include "hermes/timing/sta.hpp"
#endif

using namespace hermes;

namespace {

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

template <class F>
double time_best(int reps, F&& f) {
  double best = 1e300;
  for (int r = 0; r < reps; ++r) {
    const double t0 = now_s();
    f();
    best = std::min(best, now_s() - t0);
  }
  return best;
}

Design make_design(int num_cells, unsigned seed, Dbu* side_out = nullptr) {
  std::mt19937 rng(seed);
  const Dbu rh = 1000;
  std::uniform_int_distribution<Dbu> width(200, 800);
  std::vector<Dbu> w(num_cells);
  double area = 0;
  for (auto& x : w) { x = width(rng); area += static_cast<double>(x) * rh; }
  const Dbu side = static_cast<Dbu>(std::sqrt(area / 0.7) / rh + 1) * rh;
  if (side_out) *side_out = side;
  Design d("synthetic");
  d.set_die({0, 0, side, side});
  d.set_row_height(rh);
  d.reserve(num_cells, num_cells, 4 * static_cast<std::size_t>(num_cells));
  std::uniform_int_distribution<Dbu> pos(0, side - 1000);
  char name[32];
  for (int i = 0; i < num_cells; ++i) {
    std::snprintf(name, sizeof(name), "u%d", i);
    const Id c = d.add_cell(name, w[i], rh);
    d.move_cell(c, Point{pos(rng), pos(rng) / rh * rh});
  }
  std::uniform_int_distribution<int> deg(2, 6), near(-200, 200);
  static const char* kPin[] = {"Z", "A", "B", "C", "D", "E"};
  for (int n = 0; n < num_cells; ++n) {
    std::snprintf(name, sizeof(name), "n%d", n);
    const Id net = d.add_net(name);
    const int k = deg(rng);
    for (int p = 0; p < k; ++p) {
      const int c = p == 0 ? n : std::clamp(n + near(rng), 0, num_cells - 1);
      d.connect(c, net, kPin[p], Point{w[c] / 2, rh / 2}, p == 0 ? PinDir::Output : PinDir::Input);
    }
  }
  d.compact();
  return d;
}

// Global placement only: `iters` Nesterov iterations, no feedback.
double gp_seconds(const Design& base, int iters, double* hpwl) {
  Design d = base;
  PlacerOptions o;
  o.max_iterations = iters;
  o.stop_overflow = 0.0;
  o.timing_interval = 0;
  o.routability_interval = 0;
  o.legalize = false;
  o.detailed_placement = false;
  o.log_interval = 0;
  MixedSizePlacer p(o);
  const double t0 = now_s();
  const PlacementResult r = p.run(d, nullptr, nullptr);
  *hpwl = r.global_hpwl;
  return now_s() - t0;
}

double flow_seconds(int cells, double* hpwl) {
  Dbu side = 0;
  Design d = make_design(cells, 2, &side);
  for (int i = 0; i < 16; ++i) {
    d.move_cell(i, Point{(i % 2) ? 0 : side - 1000, (i * side / 16) / 1000 * 1000});
    d.set_fixed(i, true);
  }
  FlowOptions o;
  o.placer.max_iterations = 200;
  o.placer.stop_overflow = 0.0;
  o.placer.log_interval = 0;
  const double t0 = now_s();
  FlowReport r = run_flow(d, o);
  *hpwl = r.placement.hpwl;
  return now_s() - t0;
}

struct BenchRow {
  std::string name;
  std::string unit = "ms";
  std::vector<double> t;   // per thread count
  std::vector<double> val; // result checked for equality
};

void print(const std::vector<int>& threads, const std::vector<BenchRow>& rows) {
  std::printf("\n  %-38s", "kernel (best of N, ms)");
  for (int t : threads) std::printf(" %8dT", t);
  std::printf("   speedup\n");
  for (const BenchRow& r : rows) {
    std::printf("  %-38s", r.name.c_str());
    for (double x : r.t) std::printf(" %9.3f", x * 1e3);
    const double s = r.t.size() > 1 && r.t.back() > 0 ? r.t.front() / r.t.back() : 1.0;
    bool same = true;
    for (double v : r.val) same = same && v == r.val.front();
    std::printf("   %5.2fx%s\n", s, same ? "" : "  !! result differs across thread counts");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int n = argc > 1 ? std::atoi(argv[1]) : 1000000;
  log::configure("warn");
  const double t0 = now_s();
  Design d = make_design(n, 1);
  std::printf("design: %d cells, %zu nets, %zu pins (built in %.2f s)\n", n, d.num_nets(),
              d.num_pins(), now_s() - t0);
  const int gp_iters = 10;
  const int flow_cells = argc > 3 ? std::atoi(argv[3]) : 20000;

#ifndef HERMES_HAS_PAR
  // Baseline tree: serial only.
  double hp = 0;
  const double g0 = gp_seconds(d, 0, &hp), g1 = gp_seconds(d, gp_iters, &hp);
  std::printf("  GP setup %.1f ms, GP iteration %.1f ms, HPWL after %d iterations %.6g\n", g0 * 1e3,
              (g1 - g0) / gp_iters * 1e3, gp_iters, hp);
  double h = 0;
  const double f = flow_seconds(flow_cells, &h);
  std::printf("  flow %d cells: %.1f ms, HPWL %.6g\n", flow_cells, f * 1e3, h);
  return 0;
#else
  const int hw = par::hardware_threads();
  const int maxt = argc > 2 && std::atoi(argv[2]) > 0 ? std::atoi(argv[2]) : hw;
  std::vector<int> threads;
  for (int t = 1; t < maxt; t *= 2) threads.push_back(t);
  threads.push_back(maxt);
  std::printf("hardware threads: %d, measuring:", hw);
  for (int t : threads) std::printf(" %d", t);
  std::printf("\n");

  // --- Placer kernel inputs: objects = movable cells, positions = centers --
  const std::size_t N = d.num_cells();
  std::vector<int> net_start{0}, pin_obj, obj_start(N + 1, 0), obj_pins;
  std::vector<double> pin_dx, pin_dy, nw;
  int max_deg = 0;
  for (Id net = 0; net < static_cast<Id>(d.num_nets()); ++net) {
    const auto pins = d.net_pins(net);
    for (Id p : pins) {
      const Pin& pin = d.pin(p);
      const Cell& c = d.cell(pin.cell);
      pin_obj.push_back(pin.cell);
      pin_dx.push_back(static_cast<double>(pin.offset.x) - 0.5 * c.width);
      pin_dy.push_back(static_cast<double>(pin.offset.y) - 0.5 * c.height);
      ++obj_start[pin.cell + 1];
    }
    max_deg = std::max<int>(max_deg, static_cast<int>(pins.size()));
    net_start.push_back(static_cast<int>(pin_obj.size()));
    nw.push_back(1.0);
  }
  for (std::size_t k = 0; k < N; ++k) obj_start[k + 1] += obj_start[k];
  obj_pins.resize(pin_obj.size());
  {
    std::vector<int> fill(obj_start.begin(), obj_start.end() - 1);
    for (int p = 0; p < static_cast<int>(pin_obj.size()); ++p) obj_pins[fill[pin_obj[p]]++] = p;
  }
  compute::NetlistView nl;
  nl.num_objects = static_cast<int>(N);
  nl.net_start = net_start;
  nl.pin_obj = pin_obj;
  nl.pin_dx = pin_dx;
  nl.pin_dy = pin_dy;
  nl.net_weight = nw;
  nl.obj_start = obj_start;
  nl.obj_pins = obj_pins;
  nl.max_degree = max_deg;
  std::vector<double> pos(2 * N), cw(N), ch(N), ratio(N, 1.0);
  for (std::size_t k = 0; k < N; ++k) {
    const Cell& c = d.cell(static_cast<Id>(k));
    pos[k] = static_cast<double>(c.loc.x) + 0.5 * c.width;
    pos[N + k] = static_cast<double>(c.loc.y) + 0.5 * c.height;
  }
  const int bins = std::clamp(gp::next_pow2(static_cast<int>(std::sqrt(static_cast<double>(N)))), 8, 1024);
  auto& be = compute::cpu_backend();
  auto solver = be.make_density_solver(d.die(), bins, bins);
  const double minw = 1.41421356 * std::max(solver->bin_w(), solver->bin_h());
  for (std::size_t k = 0; k < N; ++k) {
    const Cell& c = d.cell(static_cast<Id>(k));
    cw[k] = std::max<double>(static_cast<double>(c.width), minw);
    ch[k] = std::max<double>(static_cast<double>(c.height), minw);
    ratio[k] = static_cast<double>(c.width) * c.height / (cw[k] * ch[k]);
  }
  compute::DensityView dv{cw, ch, ratio, d.die()};
  std::vector<double> grad(2 * N), pin_scratch(2 * pin_obj.size()), charge(bins * bins), base;
  std::vector<double> u2(2 * N), v2(2 * N), lo(2 * N, -1e18), hi(2 * N, 1e18);

  GlobalRouter router;
  CongestionMap map = router.make_map(d);
  CongestionMap fine(d.die(), 512, 512, 1e6);
  WireDelaySta sta(1000.0, 0.001);
  std::vector<Id> ids(N);
  std::vector<Point> locs(N);
  for (std::size_t k = 0; k < N; ++k) {
    ids[k] = static_cast<Id>(k);
    locs[k] = d.cell(static_cast<Id>(k)).loc;
  }

  std::vector<BenchRow> rows;
  auto add = [&](const char* name, int reps, const std::function<double()>& f) {
    BenchRow r;
    r.name = name;
    for (int t : threads) {
      par::set_num_threads(t);
      double v = 0;
      r.t.push_back(time_best(reps, [&] { v = f(); }));
      r.val.push_back(v);
    }
    rows.push_back(std::move(r));
  };
  double sink = 0;
  add("empty parallel_for (pool round trip)", 2000, [&] {
    par::parallel_for(0, 64, 1, [&](std::size_t, std::size_t) {});
    return 0.0;
  });
  add("total_hpwl (1M nets)", 5, [&] { return total_hpwl(d); });
  add("RUDY, 64x64 GCells", 5, [&] {
    router.estimate_congestion(d, map);
    return map.total_overflow();
  });
  add("RUDY, 512x512 GCells", 3, [&] {
    estimate_rudy(d, fine);
    return fine.total_overflow();
  });
  add("global route (L, star), 64x64", 3, [&] { return router.route(d, map).wirelength; });
  add("STA update (wire-delay model)", 5, [&] {
    sta.update(d);
    return sta.tns();
  });
  add("placer: HPWL of positions", 5, [&] { return be.hpwl(nl, pos); });
  add("placer: WA wirelength gradient", 5, [&] {
    be.wirelength_gradient(nl, pos, 5000.0, pin_scratch, grad);
    return grad[12345] + grad[N + 999];
  });
  char dep[64];
  std::snprintf(dep, sizeof(dep), "placer: density deposit (%dx%d)", bins, bins);
  add(dep, 5, [&] {
    solver->deposit(dv, pos, 0, static_cast<int>(N), base, charge);
    return charge[bins * bins / 2 + 7];
  });
  char sol[64];
  std::snprintf(sol, sizeof(sol), "placer: Poisson solve (%dx%d)", bins, bins);
  add(sol, 5, [&] {
    solver->solve(charge);
    return 0.0;
  });
  add("placer: density gradient", 5, [&] {
    solver->gradient(dv, pos, static_cast<int>(N), grad);
    return grad[777] + grad[N + 4242];
  });
  add("placer: Nesterov vector update", 10, [&] {
    be.nesterov_predict(pos, grad, pos, 0.1, 0.5, lo, hi, u2, v2);
    return be.distance(u2, v2);
  });
  add("move_cells (1M, tracking off)", 5, [&] {
    d.move_cells(ids, locs);
    return 0.0;
  });
  {
    SnapshotPublisher pub(d);
    add("snapshot publish, all cells moved", 3, [&] {
      d.move_cells(ids, locs);
      d.flush();
      pub.publish();
      return 0.0;
    });
  }
  {
    BenchRow r;
    r.name = "global placement, per iteration";
    for (int t : threads) {
      par::set_num_threads(t);
      double h = 0;
      const double g0 = gp_seconds(d, 0, &h), g1 = gp_seconds(d, gp_iters, &h);
      r.t.push_back((g1 - g0) / gp_iters);
      r.val.push_back(h);
    }
    rows.push_back(std::move(r));
  }
  if (flow_cells > 0) {
    BenchRow r;
    r.name = "full flow, " + std::to_string(flow_cells) + " cells";
    for (int t : threads) {
      par::set_num_threads(t);
      double h = 0;
      r.t.push_back(flow_seconds(flow_cells, &h));
      r.val.push_back(h);
    }
    rows.push_back(std::move(r));
  }
  print(threads, rows);
  if (sink == 42) std::printf(" ");
  return 0;
#endif
}
