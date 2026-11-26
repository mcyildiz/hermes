// Global router speed: full route (threads, GCell sizes) and incremental
// reroute at scales from 1 net to half the nets.
//
//   ./build/hermes_route_bench <input> [--place] [--save placed.hdb]
//                              [--gcell-rows R] [--threads 1,4,0] [--seed S]
//                              [--inc-penalty P]   (0 = the full route's final penalty)
//                              [--repair-iters N] [--skip-sweep]
//
// <input> is anything hermes reads (Bookshelf dir/.aux, LEF/DEF dir, .hdb).
// Without a placement in the input (or with --place) it runs the placer
// first (flow without routing); --save writes the placed design so later
// runs skip that. Every incremental case runs inside a router trial (and a
// design Transaction for moves) and is rolled back, so all cases start from
// the same routed state.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "hermes/db/geometry.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/io/design_file.hpp"
#include "hermes/io/registry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/route/global_router.hpp"

using namespace hermes;

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::vector<int> parse_list(const char* s) {
  std::vector<int> v;
  for (const char* p = s; *p;) {
    v.push_back(std::atoi(p));
    while (*p && *p != ',') ++p;
    if (*p) ++p;
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <input> [--place] [--save out.hdb] [--gcell-rows R] [--threads 1,4,0]\n", argv[0]);
    return 2;
  }
  log::set_level("", log::Level::Warn);
  std::string input = argv[1], save;
  bool place = false, skip_sweep = false;
  double gcell_rows = 6.0;
  std::vector<int> threads = {1, 4, 0};
  unsigned seed = 5;
  double inc_penalty = -1.0;
  int repair_iters = -1;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--place")) place = true;
    else if (!std::strcmp(argv[i], "--save") && i + 1 < argc) save = argv[++i];
    else if (!std::strcmp(argv[i], "--gcell-rows") && i + 1 < argc) gcell_rows = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = parse_list(argv[++i]);
    else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (!std::strcmp(argv[i], "--inc-penalty") && i + 1 < argc) inc_penalty = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--repair-iters") && i + 1 < argc) repair_iters = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--skip-sweep")) skip_sweep = true;
  }
  auto t0 = Clock::now();
  Design d = io::is_design_file(input) ? io::load_design(input) : io::load_input(input);
  std::size_t movable = 0, placed = 0;
  for (const Cell& c : d.cells()) {
    movable += c.movable();
    placed += c.movable() && c.placed;
  }
  std::printf("design %s: %zu cells (%zu movable, %zu placed), %zu nets, load %.0f ms, %d threads available\n",
              d.name().c_str(), d.num_cells(), movable, placed, d.num_nets(), ms_since(t0), par::hardware_threads());
  if (place || placed < movable / 2) {
    t0 = Clock::now();
    FlowOptions fo;
    fo.run_routing = false;
    fo.report_qor = false;
    const FlowReport r = run_flow(d, fo);
    std::printf("placed in %.2f s, HPWL %.6g\n", ms_since(t0) / 1000.0, r.placement.hpwl);
    if (!save.empty()) {
      io::save_design(d, save);
      std::printf("saved %s\n", save.c_str());
    }
  }
  d.flush();
  std::printf("HPWL %.6g\n\n", total_hpwl(d));

  GlobalRouterOptions ro;
  ro.gcell_rows = gcell_rows;
  if (inc_penalty >= 0) ro.incremental_penalty = inc_penalty;
  if (repair_iters >= 0) ro.repair_iterations = repair_iters;

  // --- Full route: threads -------------------------------------------------------
  std::printf("full route, %.1f rows per GCell\n", gcell_rows);
  std::printf("%8s %8s %10s %10s %10s %12s %10s %5s  %s\n", "threads", "grid", "total ms", "pattern", "rrr", "wirelength",
              "overflow", "iter", "vias / 3-D overflow");
  GlobalRouter best(ro);
  for (int t : threads) {
    par::set_num_threads(t);
    GlobalRouter r(ro);
    t0 = Clock::now();
    const GlobalRouteResult res = r.route(d);
    const double ms = ms_since(t0);
    auto sec = [&](const char* k) {
      for (const auto& [n, v] : res.seconds)
        if (n == k) return v * 1000.0;
      return 0.0;
    };
    std::printf("%8d %4dx%-4d %10.1f %10.1f %10.1f %12.6g %10.4g %5d  %llu / %.4g\n", par::num_threads(),
                r.graph()->nx(), r.graph()->ny(), ms, sec("pattern route"), sec("rip-up and reroute"), res.wirelength,
                res.total_overflow, res.iterations, static_cast<unsigned long long>(res.vias), res.layer_overflow);
    if (t == threads.back()) best = std::move(r);
  }
  // GCell size sweep at the last thread count.
  std::printf("\nGCell size (threads %d)\n", par::num_threads());
  for (double rows : skip_sweep ? std::vector<double>{} : std::vector<double>{3.0, 4.0, 6.0, 10.0}) {
    GlobalRouterOptions o = ro;
    o.gcell_rows = rows;
    GlobalRouter r(o);
    t0 = Clock::now();
    const GlobalRouteResult res = r.route(d);
    std::printf("  %4.1f rows: %4dx%-4d %9.1f ms  wirelength %.6g  overflow %.4g (%zu edges)  vias %llu  iter %d\n",
                rows, r.graph()->nx(), r.graph()->ny(), ms_since(t0), res.wirelength, res.total_overflow,
                res.overflow_edges, static_cast<unsigned long long>(res.vias), res.iterations);
  }

  GlobalRouter& r = best;
  const GlobalRouteResult base = r.stats();
  std::mt19937 rng(seed);
  std::vector<Id> live;
  for (std::size_t n = 0; n < d.num_nets(); ++n)
    if (!d.net(static_cast<Id>(n)).removed && d.net_pins(static_cast<Id>(n)).size() >= 2) live.push_back(static_cast<Id>(n));
  const std::size_t N = live.size();
  auto ladder = [](std::size_t n) {
    std::vector<std::size_t> v;
    for (std::size_t k : {std::size_t{1}, std::size_t{10}, std::size_t{100}, std::size_t{1000}, n / 100, n / 10, n / 2})
      if (k > 0 && k <= n && (v.empty() || k > v.back())) v.push_back(k);
    return v;
  };
  const std::vector<std::size_t> scales = ladder(N);

  // --- Reroute k random nets ---------------------------------------------------
  std::printf("\nreroute k random nets (rip up + A*), then repair; rolled back after each\n");
  std::printf("%10s %12s %12s %12s %12s %14s %10s\n", "k", "reroute ms", "+repair ms", "us/net", "rollback ms",
              "overflow", "wl delta %");
  for (std::size_t k : scales) {
    if (k == 0 || k > N) continue;
    std::vector<Id> pick = live;
    std::shuffle(pick.begin(), pick.end(), rng);
    pick.resize(k);
    RerouteRequest req;
    req.nets = pick;
    req.repair = false;  // timed without report: totals are measured outside
    r.begin_trial();
    t0 = Clock::now();
    const RerouteResult a = r.reroute(d, req);
    const double ms_a = ms_since(t0);
    t0 = Clock::now();
    r.rollback_trial();
    const double ms_rb = ms_since(t0);
    req.repair = true;
    r.begin_trial();
    t0 = Clock::now();
    r.reroute(d, req);
    const double ms_b = ms_since(t0);
    const GlobalRouteResult after = r.stats();
    r.rollback_trial();
    std::printf("%10zu %12.3f %12.3f %12.2f %12.3f %6.4g->%-7.4g %10.3f\n", k, ms_a, ms_b, 1000.0 * ms_a / k, ms_rb,
                base.total_overflow, after.total_overflow,
                base.wirelength > 0 ? 100.0 * (after.wirelength - base.wirelength) / base.wirelength : 0.0);
    (void)a;
  }

  // --- Cell moves + update() ---------------------------------------------------
  std::printf("\nmove k random std cells by up to 2 GCells, flush, update() (tracking on, attached)\n");
  std::printf("%10s %10s %12s %12s %12s %14s\n", "k", "nets", "update ms", "no-op ms", "rollback ms", "overflow");
  d.set_tracking(true);
  r.attach(d);
  d.flush();
  r.update(d);  // consume the attach's "all"
  const BinGrid g = r.gcell_grid(d);
  std::vector<Id> cells;
  for (Id c = 0; c < static_cast<Id>(d.num_cells()); ++c)
    if (d.cell(c).movable() && !d.cell(c).is_macro) cells.push_back(c);
  for (std::size_t k : ladder(cells.size() / 5)) {
    if (k == 0 || k > cells.size()) continue;
    std::vector<Id> pick = cells;
    std::shuffle(pick.begin(), pick.end(), rng);
    pick.resize(k);
    std::uniform_int_distribution<Dbu> dx(-2 * g.bin_width(), 2 * g.bin_width()), dy(-2 * g.bin_height(), 2 * g.bin_height());
    Transaction tx(d);
    r.begin_trial();
    for (Id c : pick) {
      const Rect& die = d.die();
      const Cell& cell = d.cell(c);
      d.move_cell(c, {std::clamp(cell.loc.x + dx(rng), die.xlo, die.xhi - cell.width),
                      std::clamp(cell.loc.y + dy(rng), die.ylo, die.yhi - cell.height)});
    }
    d.flush();
    t0 = Clock::now();
    const RerouteResult u = r.update(d);
    const double ms_u = ms_since(t0);
    const double after_overflow = r.stats().total_overflow;
    t0 = Clock::now();
    r.update(d);
    const double ms_noop = ms_since(t0);
    tx.rollback();
    d.flush();
    t0 = Clock::now();
    r.rollback_trial();
    const double ms_rb = ms_since(t0);
    r.update(d);  // nothing to do after the rollback
    std::printf("%10zu %10zu %12.3f %12.3f %12.3f %6.4g->%-7.4g\n", k, u.nets_rerouted, ms_u, ms_noop, ms_rb,
                base.total_overflow, after_overflow);
  }
  {
    // update() without attach: scans every net's pin key.
    GlobalRouter scan = r;
    t0 = Clock::now();
    scan.update(d);
    std::printf("update() with no changes, not attached (scans all %zu nets): %.3f ms\n", d.num_nets(), ms_since(t0));
  }
  r.detach();

  // --- Region reroute -----------------------------------------------------------
  std::printf("\nregion reroute (every net with a pin or route in the region; only route edges inside change)\n");
  for (int parts : {16, 8, 4, 2}) {
    const Rect& die = d.die();
    const Dbu w = die.width() / parts, h = die.height() / parts;
    const Rect area{die.xlo + (parts / 2) * w - w / 2, die.ylo + (parts / 2) * h - h / 2,
                    die.xlo + (parts / 2) * w + w / 2, die.ylo + (parts / 2) * h + h / 2};
    r.begin_trial();
    t0 = Clock::now();
    const RerouteResult a = r.reroute_region(d, area, true, nullptr, true);
    const double ms = ms_since(t0);
    r.rollback_trial();
    std::printf("  1/%-3d of the die side: %8zu nets  %10.3f ms  overflow in region %.4g -> %.4g\n", parts,
                a.nets_rerouted, ms, a.overflow_before, a.overflow_after);
  }
  return 0;
}
