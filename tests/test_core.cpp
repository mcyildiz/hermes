// Minimal dependency-free test runner. Swap for GoogleTest/Catch2 when the
// suite grows.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "hermes/log/sinks.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/place/analytic.hpp"
#include "hermes/place/congestion.hpp"
#include "hermes/place/legalizer.hpp"

using namespace hermes;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

Design two_pin_design() {
  Design d;
  d.set_die({0, 0, 1000, 1000});
  const Id a = d.add_cell("a", 10, 10);
  const Id b = d.add_cell("b", 10, 10);
  d.move_cell(a, {0, 0});
  d.move_cell(b, {300, 400});
  const Id n = d.add_net("n");
  d.connect(a, n, "Z");
  d.connect(b, n, "A");
  return d;
}

void test_hpwl() {
  Design d = two_pin_design();
  CHECK(net_hpwl(d, 0) == 700);
  d.set_net_weight(0, 2.0);
  CHECK(std::abs(total_hpwl(d, true) - 1400.0) < 1e-9);
  CHECK(std::abs(total_hpwl(d, false) - 700.0) < 1e-9);
}

void test_rect() {
  const Rect a{0, 0, 10, 10};
  const Rect b{5, 5, 20, 20};
  CHECK(a.overlap_area(b) == 25);
  CHECK(a.overlaps(b));
  CHECK(Rect({0, 0, 1, 1}).overlap_area({2, 2, 3, 3}) == 0);
}

void test_rudy_conserves_wirelength() {
  Design d = two_pin_design();
  CongestionMap map(d.die(), 10, 10, 1e9);
  estimate_rudy(d, map);
  double total = 0;
  for (int y = 0; y < map.ny(); ++y)
    for (int x = 0; x < map.nx(); ++x) total += map.demand(x, y);
  CHECK(std::abs(total - 700.0) < 1e-6);  // RUDY spreads exactly HPWL.
  CHECK(map.total_overflow() == 0.0);
}

void test_macro_blockage() {
  Design d;
  d.set_die({0, 0, 100, 100});
  const Id m = d.add_cell("m", 50, 50, true);
  d.move_cell(m, {0, 0});
  CongestionMap map(d.die(), 2, 2, 10.0);
  apply_macro_blockages(d, map, 1.0);
  CHECK(map.capacity(0, 0) == 0.0);
  CHECK(map.capacity(1, 1) == 10.0);
}

// A movable macro starts at the die center and is pulled into the lower-left
// corner. The congestion map used for routability feedback must derate
// capacity where the macro is now, not where it started. A std cell tied to
// the corner pad (heavier net) sits under the macro, and a long net from it to the far
// corner puts ~20 units of RUDY demand in every bin: under capacity (50)
// where unblocked, over it (10) under the macro's blockage.
void test_macro_blockages_follow_macros() {
  Design d;
  d.set_die({0, 0, 1000, 1000});
  const Id pad = d.add_cell("pad", 10, 10);
  d.move_cell(pad, {0, 0});
  d.set_fixed(pad, true);
  const Id m = d.add_cell("m", 200, 200, /*is_macro=*/true);
  const Id s = d.add_cell("s", 10, 10);
  const Id n1 = d.add_net("n1");
  d.connect(pad, n1, "Z");
  d.connect(m, n1, "A");
  const Id n2 = d.add_net("n2");
  d.connect(pad, n2, "Z2");
  d.connect(s, n2, "A");
  const Id far = d.add_cell("far", 10, 10);
  d.move_cell(far, {990, 990});
  d.set_fixed(far, true);
  d.set_net_weight(n2, 4.0);  // Keeps s at the pad, under the macro.
  const Id n3 = d.add_net("n3");
  d.connect(s, n3, "Z");
  d.connect(far, n3, "A");

  GlobalRouterOptions ropts;
  ropts.grid_x = ropts.grid_y = 10;
  ropts.capacity_per_gcell = 50.0;
  ropts.macro_blockage = 0.8;
  GlobalRouter router(ropts);

  PlacerOptions popts;
  popts.max_iterations = 20;
  popts.timing_interval = 0;
  popts.routability_interval = 5;
  popts.routability_start_overflow = 1.0;  // Update before cells have spread,
  popts.stop_overflow = 0.0;               // and run all 20 iterations.
  popts.detailed_placement = false;
  MixedSizePlacer placer(popts);
  const PlacementResult r = placer.run(d, nullptr, &router);

  // Reached the corner; legalization pushes it just off the fixed 10x10 pad.
  CHECK(d.cell(m).loc.x <= 10 && d.cell(m).loc.y <= 10);
  // The std cell under the macro's new footprint saw the blocked capacity.
  CHECK(placer.inflation()[s] > 1.0);

  // Reported congestion matches a map built from the final placement.
  CongestionMap fresh = router.make_map(d);
  router.estimate_congestion(d, fresh);
  CHECK(fresh.capacity(0, 0) < ropts.capacity_per_gcell);
  CHECK(fresh.capacity(5, 5) == ropts.capacity_per_gcell);
  CHECK(std::abs(r.max_utilization - fresh.max_utilization()) < 1e-9);
  CHECK(std::abs(r.overflow - fresh.total_overflow()) < 1e-9);
}

void test_timing_weights_increase() {
  Design d = two_pin_design();
  WireDelaySta sta(/*period=*/100.0, /*ps_per_dbu=*/1.0);
  sta.update(d);
  CHECK(sta.wns() == -600.0);
  CHECK(sta.net_criticality(0) == 1.0);
}

void test_flow_runs() {
  Design d = two_pin_design();
  d.set_fixed(0, true);
  FlowOptions opts;
  opts.placer.max_iterations = 20;
  opts.router.grid_x = opts.router.grid_y = 8;
  const double before = total_hpwl(d);
  const FlowReport r = run_flow(d, opts);
  CHECK(r.placement.iterations > 0 && r.placement.iterations <= 20);
  CHECK(r.placement.hpwl < before);  // Movable cell pulled toward its anchor.
  CHECK(d.cell(0).loc == (Point{0, 0}));  // Fixed cell untouched.
}


// --- Legalization -----------------------------------------------------------

bool legal(const Design& d, Dbu rh) {
  std::string why;
  const bool ok = check_legal(d, rh, &why);
  if (!ok) std::fprintf(stderr, "  illegal: %s\n", why.c_str());
  return ok;
}

void test_macro_overlap_removal() {
  Design d;
  d.set_die({0, 0, 1000, 1000});
  d.set_row_height(10);
  const Id f = d.add_cell("fixed", 200, 200, true);
  d.move_cell(f, {400, 400});
  d.set_fixed(f, true);
  const Id a = d.add_cell("a", 300, 300, true);
  const Id b = d.add_cell("b", 100, 100, true);
  d.move_cell(a, {350, 353});  // overlaps the fixed macro
  d.move_cell(b, {420, 420});  // overlaps both
  const MacroLegalizeResult r = legalize_macros(d);
  CHECK(r.moved == 2);
  CHECK(r.unresolved == 0);
  CHECK(d.cell(f).loc == (Point{400, 400}));  // fixed never moves
  CHECK(!d.cell(a).bbox().overlaps(d.cell(f).bbox()));
  CHECK(!d.cell(b).bbox().overlaps(d.cell(f).bbox()));
  CHECK(!d.cell(a).bbox().overlaps(d.cell(b).bbox()));
  CHECK(d.cell(a).loc.y % 10 == 0 && d.cell(b).loc.y % 10 == 0);  // row-aligned
  CHECK(legal(d, 10));
}

void test_macro_minimal_displacement() {
  // A lone movable macro overlapping a fixed one should move the short way.
  Design d;
  d.set_die({0, 0, 1000, 1000});
  const Id f = d.add_cell("f", 100, 100, true);
  d.move_cell(f, {0, 0});
  d.set_fixed(f, true);
  const Id m = d.add_cell("m", 100, 100, true);
  d.move_cell(m, {90, 0});
  const MacroLegalizeResult r = legalize_macros(d);
  CHECK(d.cell(m).loc == (Point{100, 0}));
  CHECK(r.max_displacement == 10);
}

void test_macro_halo() {
  Design d;
  d.set_die({0, 0, 1000, 1000});
  const Id f = d.add_cell("f", 100, 100, true);
  d.move_cell(f, {0, 0});
  d.set_fixed(f, true);
  const Id m = d.add_cell("m", 100, 100, true);
  d.move_cell(m, {90, 0});
  MacroLegalizerOptions o;
  o.halo = 20;
  legalize_macros(d, o);
  CHECK(d.cell(m).loc == (Point{120, 0}));
}

void test_macro_unresolvable() {
  Design d;
  d.set_die({0, 0, 100, 100});
  const Id a = d.add_cell("a", 80, 80, true);
  const Id b = d.add_cell("b", 80, 80, true);
  d.move_cell(a, {0, 0});
  d.move_cell(b, {10, 10});
  const MacroLegalizeResult r = legalize_macros(d);
  CHECK(r.unresolved == 1);
  CHECK(d.cell(b).bbox().xhi <= 100 && d.cell(b).bbox().yhi <= 100);  // stays in die
}

void test_abacus_cluster_optimum() {
  // Two cells wanting x=100 in one row: Abacus centers the cluster at 95..115.
  Design d;
  d.set_die({0, 0, 1000, 10});
  d.set_row_height(10);
  const Id a = d.add_cell("a", 10, 10);
  const Id b = d.add_cell("b", 10, 10);
  d.move_cell(a, {100, 3});
  d.move_cell(b, {100, 3});
  const CellLegalizeResult r = abacus_legalize(d);
  CHECK(r.legalized == 2 && r.failed == 0);
  const Dbu xa = d.cell(a).loc.x, xb = d.cell(b).loc.x;
  CHECK(std::min(xa, xb) == 95 && std::max(xa, xb) == 105);
  CHECK(d.cell(a).loc.y == 0 && d.cell(b).loc.y == 0);
  CHECK(legal(d, 10));
}

void test_abacus_keeps_legal_placement() {
  Design d;
  d.set_die({0, 0, 1000, 100});
  d.set_row_height(10);
  for (int i = 0; i < 20; ++i) {
    const Id c = d.add_cell("c" + std::to_string(i), 20, 10);
    d.move_cell(c, {static_cast<Dbu>(40 * i), static_cast<Dbu>(10 * (i % 10))});
  }
  const CellLegalizeResult r = abacus_legalize(d);
  CHECK(r.total_displacement == 0.0);
}

void test_abacus_around_macro() {
  Design d;
  d.set_die({0, 0, 1000, 1000});
  d.set_row_height(10);
  const Id m = d.add_cell("ram", 400, 400, true);
  d.move_cell(m, {300, 300});
  d.set_fixed(m, true);
  for (int i = 0; i < 200; ++i) {
    const Id c = d.add_cell("u" + std::to_string(i), 10, 10);
    d.move_cell(c, {500, 500});  // all piled up inside the macro
  }
  const CellLegalizeResult r = abacus_legalize(d);
  CHECK(r.failed == 0);
  CHECK(d.cell(m).loc == (Point{300, 300}));
  CHECK(legal(d, 10));
  // Cells hug the macro edge. The best possible assignment here averages
  // ~247 DBU of displacement (max 270); allow Abacus's greedy order 1.5x.
  CHECK(r.total_displacement / 200.0 < 370.0);
  CHECK(r.max_displacement < 600);
}

void test_abacus_multi_row() {
  Design d;
  d.set_die({0, 0, 200, 100});
  d.set_row_height(10);
  const Id big = d.add_cell("ff2", 20, 20);  // double-height
  d.move_cell(big, {50, 33});
  for (int i = 0; i < 10; ++i) {
    const Id c = d.add_cell("u" + std::to_string(i), 10, 10);
    d.move_cell(c, {55, 35});
  }
  const CellLegalizeResult r = abacus_legalize(d);
  CHECK(r.multi_row == 1);
  CHECK(r.failed == 0);
  CHECK(d.cell(big).loc.y % 10 == 0);
  CHECK(legal(d, 10));
}

void test_abacus_full_rows() {
  // Exactly 100% utilization: everything must still fit.
  Design d;
  d.set_die({0, 0, 100, 40});
  d.set_row_height(10);
  for (int i = 0; i < 40; ++i) {
    const Id c = d.add_cell("u" + std::to_string(i), 10, 10);
    d.move_cell(c, {45, 15});
  }
  const CellLegalizeResult r = abacus_legalize(d);
  CHECK(r.failed == 0);
  CHECK(legal(d, 10));
}

void test_legalize_random_mixed_size() {
  std::mt19937 rng(7);
  Design d;
  d.set_die({0, 0, 20000, 20000});
  d.set_row_height(100);
  std::uniform_int_distribution<Dbu> pos(0, 19000), mw(1000, 4000), cw(1, 8);
  for (int i = 0; i < 12; ++i) {
    const Id m = d.add_cell("m" + std::to_string(i), mw(rng), mw(rng), true);
    d.move_cell(m, {pos(rng), pos(rng)});
    d.set_fixed(m, (i < 3));
  }
  for (int i = 0; i < 3000; ++i) {
    const Id c = d.add_cell("u" + std::to_string(i), 100 * cw(rng), 100);
    d.move_cell(c, {pos(rng), pos(rng)});
  }
  // Fixed macros may overlap each other from the random draw; drop those.
  for (Id i = 1; i < 3; ++i)
    for (Id j = 0; j < i; ++j)
      if (d.cell(i).bbox().overlaps(d.cell(j).bbox())) d.set_fixed(i, false);

  const MacroLegalizeResult mr = legalize_macros(d);
  CHECK(mr.unresolved == 0);
  for (Id i = 0; i < static_cast<Id>(d.num_cells()); ++i) if (d.cell(i).is_macro) d.set_fixed(i, true);
  const CellLegalizeResult cr = abacus_legalize(d);
  CHECK(cr.failed == 0);
  CHECK(legal(d, 100));
}

void test_flow_legal_with_macros() {
  Design d;
  d.set_die({0, 0, 10000, 10000});
  d.set_row_height(100);
  const Id io = d.add_cell("io", 100, 100);
  d.move_cell(io, {0, 5000});
  d.set_fixed(io, true);
  const Id m0 = d.add_cell("ram0", 2000, 2000, true);
  const Id m1 = d.add_cell("ram1", 2000, 2000, true);
  Id prev = io;
  for (int i = 0; i < 100; ++i) {
    const Id c = d.add_cell("u" + std::to_string(i), 200, 100);
    const Id n = d.add_net("n" + std::to_string(i));
    d.connect(prev, n, "Z");
    d.connect(c, n, "A");
    if (i % 25 == 0) d.connect(i % 50 == 0 ? m0 : m1, n, "D");
    prev = c;
  }
  FlowOptions opts;
  opts.placer.max_iterations = 30;
  opts.router.grid_x = opts.router.grid_y = 8;
  run_flow(d, opts);
  CHECK(d.cell(m0).fixed && d.cell(m1).fixed);
  CHECK(legal(d, 100));
}


// --- Analytic global placement ----------------------------------------------

// Random netlist: `n` movable std cells (w x h) on a square die of side
// `side`, 2-4 pin nets, optional fixed I/O pads on the die edge.
Design random_design(int n, Dbu w, Dbu h, Dbu side, int pads, unsigned seed) {
  Design d("rand");
  d.set_die({0, 0, side, side});
  d.set_row_height(h);
  std::mt19937 rng(seed);
  std::vector<Id> ids;
  for (int i = 0; i < pads; ++i) {
    const Id p = d.add_cell("pad" + std::to_string(i), 1, 1);
    const Dbu t = side * i / std::max(1, pads);
    d.move_cell(p, (i % 2) ? Point{0, t} : Point{t, side - 1});
    d.set_fixed(p, true);
    ids.push_back(p);
  }
  for (int i = 0; i < n; ++i) ids.push_back(d.add_cell("u" + std::to_string(i), w, h));
  std::uniform_int_distribution<int> pick(0, static_cast<int>(ids.size()) - 1);
  std::uniform_int_distribution<int> deg(2, 4);
  for (int i = 0; i < n; ++i) {
    const Id net = d.add_net("n" + std::to_string(i));
    d.connect(ids[pads + i], net, "Z", {w / 2, h / 2}, PinDir::Output);
    const int k = deg(rng);
    for (int j = 1; j < k; ++j) d.connect(ids[pick(rng)], net, "A" + std::to_string(j), {w / 2, h / 2});
  }
  return d;
}

PlacerOptions gp_only(int iters = 1000) {
  PlacerOptions o;
  o.max_iterations = iters;
  o.timing_interval = 0;
  o.routability_interval = 0;
  o.legalize = false;
  o.detailed_placement = false;
  return o;
}

// Fraction of movable std-cell area above `target` in a g x g grid.
double grid_overflow(const Design& d, int g, double target) {
  const Rect& die = d.die();
  CongestionMap map(die, g, g, 1.0);
  std::vector<double> use(static_cast<std::size_t>(g) * g, 0.0), cap(use.size(), 0.0);
  double total = 0.0;
  for (int iy = 0; iy < g; ++iy)
    for (int ix = 0; ix < g; ++ix) cap[iy * g + ix] = target * map.bin_rect(ix, iy).area();
  for (const Cell& c : d.cells()) {
    if (c.fixed || c.width * c.height <= 1) continue;
    total += static_cast<double>(c.width * c.height);
    for (int iy = 0; iy < g; ++iy)
      for (int ix = 0; ix < g; ++ix)
        use[iy * g + ix] += static_cast<double>(map.bin_rect(ix, iy).overlap_area(c.bbox()));
  }
  double over = 0.0;
  for (std::size_t b = 0; b < use.size(); ++b) over += std::max(0.0, use[b] - cap[b]);
  return total > 0 ? over / total : 0.0;
}

void test_wa_wirelength() {
  const std::vector<double> x = {3.0, -1.0, 7.5, 2.0, 7.0};
  const double hp = 8.5;
  // Bounded by HPWL, and tends to it as gamma -> 0.
  CHECK(gp::wa_wirelength_1d(x.data(), 5, 2.0, nullptr) < hp);
  CHECK(std::abs(gp::wa_wirelength_1d(x.data(), 5, 0.01, nullptr) - hp) < 1e-3);
  // Analytic gradient matches central differences.
  for (double gamma : {0.5, 2.0, 10.0}) {
    std::vector<double> g(5, 0.0);
    gp::wa_wirelength_1d(x.data(), 5, gamma, g.data());
    double gsum = 0.0;
    for (int i = 0; i < 5; ++i) {
      std::vector<double> xp = x, xm = x;
      const double h = 1e-6;
      xp[i] += h;
      xm[i] -= h;
      const double fd = (gp::wa_wirelength_1d(xp.data(), 5, gamma, nullptr) -
                         gp::wa_wirelength_1d(xm.data(), 5, gamma, nullptr)) / (2 * h);
      CHECK(std::abs(fd - g[i]) < 1e-6);
      gsum += g[i];
    }
    CHECK(std::abs(gsum) < 1e-9);  // Translation invariant.
  }
}

void test_electrostatics() {
  const Rect region{0, 0, 1000, 500};
  gp::ElectrostaticSystem es(region, 32, 16);
  // Uniform density: no field.
  es.add_rect(0, 0, 1000, 500, 0.6);
  es.solve(true);
  double maxf = 0.0;
  for (int iy = 0; iy < 16; ++iy)
    for (int ix = 0; ix < 32; ++ix)
      maxf = std::max({maxf, std::abs(es.field_x(ix, iy)), std::abs(es.field_y(ix, iy))});
  CHECK(maxf < 1e-9);

  // One blob: the field points away from it on every side.
  es.reset();
  es.add_rect(450, 200, 550, 300, 1.0);
  es.solve(true);
  CHECK(es.field_x(28, 8) > 0 && es.field_x(3, 8) < 0);
  CHECK(es.field_y(16, 14) > 0 && es.field_y(16, 1) < 0);
  CHECK(es.potential(16, 8) > es.potential(2, 2));
  // Field is -grad(psi): compare with a finite difference of the potential.
  const double bw = es.bin_w();
  const double fd = -(es.potential(25, 8) - es.potential(23, 8)) / (2 * bw);
  CHECK(std::abs(fd - es.field_x(24, 8)) < 0.1 * std::abs(es.field_x(24, 8)));

  // Energy gradient of a rectangle matches a finite difference of the energy.
  auto energy_at = [&](double cx) {
    es.reset();
    es.add_rect(300, 100, 500, 300, 1.0);          // fixed blob
    es.add_rect(cx - 100, 150, cx + 100, 350, 1.0);  // moving rect
    es.solve(true);
    return es.energy();
  };
  const double cx = 560, h = 1.0;
  energy_at(cx);
  double gx = 0, gy = 0;
  es.rect_gradient(cx - 100, 150, cx + 100, 350, 1.0, gx, gy);
  const double fde = (energy_at(cx + h) - energy_at(cx - h)) / (2 * h);
  CHECK(gx < 0);  // Moving right (away from the blob) lowers the energy.
  CHECK(std::abs(gx - fde) < 0.15 * std::abs(fde));
}

void test_gp_spreads_to_target_density() {
  Design d = random_design(1000, 10, 10, 400, 8, 7);  // 62.5% utilization
  PlacerOptions o = gp_only();
  o.target_density = 0.8;
  MixedSizePlacer placer(o);
  const PlacementResult r = placer.run(d, nullptr, nullptr);
  CHECK(r.density_overflow < o.stop_overflow);
  CHECK(r.iterations < o.max_iterations);
  CHECK(grid_overflow(d, 16, o.target_density) < 0.15);
  for (const Cell& c : d.cells()) {
    CHECK(c.loc.x >= 0 && c.loc.y >= 0 && c.loc.x + c.width <= 400 && c.loc.y + c.height <= 400);
  }
  // Much shorter than a random spread of the same cells.
  Design rnd = random_design(1000, 10, 10, 400, 8, 7);
  std::mt19937 rng(3);
  std::uniform_int_distribution<Dbu> u(0, 390);
  for (Id i = 0; i < static_cast<Id>(rnd.num_cells()); ++i) if (!rnd.cell(i).fixed) rnd.move_cell(i, {u(rng), u(rng)});
  CHECK(r.hpwl < 0.5 * total_hpwl(rnd));
}

void test_gp_target_density_controls_spread() {
  auto spread = [](double target) {
    Design d = random_design(400, 10, 10, 1000, 0, 11);  // 4% utilization
    PlacerOptions o = gp_only();
    o.target_density = target;
    MixedSizePlacer(o).run(d, nullptr, nullptr);
    Dbu xl = 1 << 30, yl = 1 << 30, xh = 0, yh = 0;
    for (const Cell& c : d.cells()) {
      xl = std::min(xl, c.loc.x);
      yl = std::min(yl, c.loc.y);
      xh = std::max(xh, c.loc.x + c.width);
      yh = std::max(yh, c.loc.y + c.height);
    }
    return static_cast<double>((xh - xl) * (yh - yl));
  };
  const double a_loose = spread(0.3), a_tight = spread(0.9);
  // Cell area is 40000: ~133000 at 30%, ~44000 at 90% (plus edge slack).
  CHECK(a_loose > 2.0 * a_tight);
  CHECK(a_tight < 40000.0 / 0.9 * 2.5);
  CHECK(a_loose < 40000.0 / 0.3 * 2.5);
}

void test_gp_chain_between_pads() {
  Design d("chain");
  d.set_die({0, 0, 10000, 10000});
  const Id a = d.add_cell("in", 10, 10);
  d.move_cell(a, {0, 4995});
  d.set_fixed(a, true);
  const Id b = d.add_cell("out", 10, 10);
  d.move_cell(b, {9990, 4995});
  d.set_fixed(b, true);
  Id prev = a;
  constexpr int kN = 20;
  for (int i = 0; i < kN; ++i) {
    const Id c = d.add_cell("c" + std::to_string(i), 100, 100);
    const Id n = d.add_net("n" + std::to_string(i));
    d.connect(prev, n, "Z", prev == a ? Point{5, 5} : Point{50, 50});
    d.connect(c, n, "A", {50, 50});
    prev = c;
  }
  const Id n = d.add_net("nout");
  d.connect(prev, n, "Z", {50, 50});
  d.connect(b, n, "A", {5, 5});
  const PlacementResult r = MixedSizePlacer(gp_only()).run(d, nullptr, nullptr);
  // The straight line between the pads is 9990 long.
  CHECK(r.hpwl < 9990 * 1.15);
  // Cells stay near the line, in chain order.
  int inversions = 0;
  for (int i = 0; i < kN; ++i) {
    CHECK(std::abs(d.cell(2 + i).center().y - 5000) < 500);
    if (i > 0 && d.cell(2 + i).loc.x < d.cell(1 + i).loc.x) ++inversions;
  }
  CHECK(inversions == 0);
  CHECK(d.cell(a).loc == (Point{0, 4995}));  // Fixed cells untouched.
}

void test_gp_macro_in_density_field() {
  // One movable macro and many std cells all tied to it: wirelength pulls
  // everything together, the density field keeps the cells off the macro.
  Design d("mix");
  d.set_die({0, 0, 2000, 2000});
  d.set_row_height(10);
  const Id m = d.add_cell("ram", 400, 400, /*is_macro=*/true);
  std::mt19937 rng(5);
  std::uniform_int_distribution<Dbu> off(0, 399);
  for (int i = 0; i < 3000; ++i) {
    const Id c = d.add_cell("u" + std::to_string(i), 20, 10);
    const Id n = d.add_net("n" + std::to_string(i));
    d.connect(c, n, "Z", {10, 5});
    d.connect(m, n, "D" + std::to_string(i), {off(rng), off(rng)});
  }
  PlacerOptions o = gp_only();
  const PlacementResult r = MixedSizePlacer(o).run(d, nullptr, nullptr);
  CHECK(r.density_overflow < o.stop_overflow);
  const Rect mb = d.cell(m).bbox();
  double over_macro = 0.0, std_area = 0.0;
  for (const Cell& c : d.cells()) {
    if (c.is_macro) continue;
    std_area += static_cast<double>(c.width * c.height);
    over_macro += static_cast<double>(c.bbox().overlap_area(mb));
  }
  CHECK(over_macro < 0.1 * static_cast<double>(mb.area()));
  // Cells pack around the macro instead of filling the die (~39% at target 0.7).
  CHECK(r.hpwl < 0.5 * 3000 * 1000.0);
}

void test_gp_fixed_macro_is_avoided() {
  Design d = random_design(600, 10, 10, 500, 0, 13);
  const Id blk = d.add_cell("blk", 200, 200, /*is_macro=*/true);
  d.move_cell(blk, {150, 150});  // Die center.
  d.set_fixed(blk, true);
  const Rect bb = d.cell(blk).bbox();
  MixedSizePlacer(gp_only()).run(d, nullptr, nullptr);
  double over = 0.0;
  for (const Cell& c : d.cells())
    if (!c.fixed) over += static_cast<double>(c.bbox().overlap_area(bb));
  CHECK(over < 0.05 * static_cast<double>(bb.area()));
  CHECK(d.cell(blk).loc == (Point{150, 150}));
}

void test_gp_deterministic() {
  Design a = random_design(300, 10, 10, 300, 4, 17);
  Design b = random_design(300, 10, 10, 300, 4, 17);
  MixedSizePlacer(gp_only()).run(a, nullptr, nullptr);
  MixedSizePlacer(gp_only()).run(b, nullptr, nullptr);
  bool same = true;
  for (std::size_t i = 0; i < a.cells().size(); ++i) same = same && a.cells()[i].loc == b.cells()[i].loc;
  CHECK(same);
}

void test_gp_inflation_feeds_density() {
  // Tight routing capacity: routability feedback inflates cells in hot bins,
  // and the inflated cells spread out, lowering peak congestion.
  auto run = [](bool feedback, std::vector<double>* infl) {
    Design d = random_design(300, 10, 10, 400, 8, 19);
    GlobalRouterOptions ro;
    ro.grid_x = ro.grid_y = 16;
    ro.capacity_per_gcell = 400.0;
    GlobalRouter router(ro);
    PlacerOptions o = gp_only();
    o.target_density = 0.9;
    o.routability_interval = feedback ? 10 : 0;
    MixedSizePlacer placer(o);
    const PlacementResult r = placer.run(d, nullptr, &router);
    if (infl) *infl = placer.inflation();
    return r;
  };
  std::vector<double> infl;
  const PlacementResult off = run(false, nullptr);
  const PlacementResult on = run(true, &infl);
  const auto inflated = std::count_if(infl.begin(), infl.end(), [](double f) { return f > 1.0; });
  CHECK(inflated > 0);
  CHECK(on.density_overflow < PlacerOptions{}.stop_overflow);
  CHECK(on.max_utilization < off.max_utilization);
  CHECK(on.overflow < off.overflow);
}


void test_name_table() {
  NameTable t;
  CHECK(t.empty() && t.find("a") == kInvalidId);
  CHECK(t.add("a") == 0 && t.add("bb") == 1 && t.add("") == 2);
  CHECK(t.name(0) == "a" && t.name(1) == "bb" && t.name(2).empty());
  CHECK(t.find("bb") == 1 && t.find("") == 2 && t.find("b") == kInvalidId);
  CHECK(t.intern("bb") == 1 && t.intern("ccc") == 3 && t.size() == 4);
  bool threw = false;
  try {
    t.add("a");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw && t.size() == 4);

  // Many names: forces arena growth and several rehashes.
  NameTable big;
  constexpr int kN = 50000;
  for (int i = 0; i < kN; ++i) big.add("inst_" + std::to_string(i));
  bool ok = big.size() == kN;
  for (int i = 0; i < kN && ok; ++i) {
    const std::string n = "inst_" + std::to_string(i);
    ok = big.find(n) == i && big.name(i) == n;
  }
  CHECK(ok);
  CHECK(big.find("inst_") == kInvalidId && big.find("inst_50000") == kInvalidId);

  // Copies and moves keep working lookups (slots hold ids, not pointers).
  NameTable copy = big;
  NameTable moved = std::move(big);
  CHECK(copy.find("inst_123") == 123 && moved.find("inst_49999") == 49999);
  CHECK(copy.add("new") == kN && copy.find("new") == kN && moved.find("new") == kInvalidId);
}

void test_design_names() {
  Design d;
  const Id a = d.add_cell("u_a", 10, 10);
  const Id b = d.add_cell("u_b", 10, 10);
  const Id n = d.add_net("n0");
  const Id m = d.add_net("n1");
  const Id pa = d.connect(a, n, "Z", {}, PinDir::Output);
  const Id pb = d.connect(b, n, "A");
  const Id pb2 = d.connect(b, m, "B");
  const Id pa2 = d.connect(a, m, "A");
  const Id io = d.connect(kInvalidId, m, "out0", {100, 0});

  CHECK(d.cell_name(a) == "u_a" && d.cell_name(b) == "u_b");
  CHECK(d.net_name(n) == "n0" && d.net_name(m) == "n1");
  CHECK(d.find_cell("u_b") == b && d.find_cell("nope") == kInvalidId);
  CHECK(d.find_net("n1") == m && d.find_net("u_a") == kInvalidId);
  CHECK(d.pin_name(pa) == "Z" && d.pin_name(pb) == "A" && d.pin_name(io) == "out0");
  CHECK(d.find_pin(a, "Z") == pa && d.find_pin(a, "A") == pa2);
  CHECK(d.find_pin(b, "A") == pb && d.find_pin(b, "B") == pb2 && d.find_pin(b, "Z") == kInvalidId);
  CHECK(d.find_pin(kInvalidId, "out0") == io && d.find_pin(kInvalidId, "A") == kInvalidId);
  // "A" appears on two cells but is stored once.
  CHECK(d.pin_names().size() == 4);

  auto throws = [](auto&& fn) {
    try {
      fn();
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };
  CHECK(throws([&] { d.add_cell("u_a", 1, 1); }));
  CHECK(throws([&] { d.add_net("n0"); }));
  CHECK(d.cells().size() == 2 && d.nets().size() == 2);
  // A cell and a net may share a name (separate namespaces).
  CHECK(d.add_net("u_a") == 2);

  // A copied design resolves names on its own.
  const Design c = d;
  CHECK(c.find_cell("u_b") == b && c.find_pin(b, "B") == pb2 && c.net_name(2) == "u_a");

  // Hot structs carry no names.
  CHECK(sizeof(Pin) <= 32 && sizeof(Net) <= 32 && sizeof(Cell) <= 64);
}

}  // namespace

int main() {
  // Engine progress goes to the log; keep test output to pass/fail lines.
  hermes::log::console_sink()->set_level(hermes::log::Level::Error);
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"hpwl", test_hpwl},
      {"name_table", test_name_table},
      {"design_names", test_design_names},
      {"rect", test_rect},
      {"rudy_conserves_wirelength", test_rudy_conserves_wirelength},
      {"macro_blockage", test_macro_blockage},
      {"macro_blockages_follow_macros", test_macro_blockages_follow_macros},
      {"timing_weights_increase", test_timing_weights_increase},
      {"flow_runs", test_flow_runs},
      {"macro_overlap_removal", test_macro_overlap_removal},
      {"macro_minimal_displacement", test_macro_minimal_displacement},
      {"macro_halo", test_macro_halo},
      {"macro_unresolvable", test_macro_unresolvable},
      {"abacus_cluster_optimum", test_abacus_cluster_optimum},
      {"abacus_keeps_legal_placement", test_abacus_keeps_legal_placement},
      {"abacus_around_macro", test_abacus_around_macro},
      {"abacus_multi_row", test_abacus_multi_row},
      {"abacus_full_rows", test_abacus_full_rows},
      {"legalize_random_mixed_size", test_legalize_random_mixed_size},
      {"flow_legal_with_macros", test_flow_legal_with_macros},
      {"wa_wirelength", test_wa_wirelength},
      {"electrostatics", test_electrostatics},
      {"gp_spreads_to_target_density", test_gp_spreads_to_target_density},
      {"gp_target_density_controls_spread", test_gp_target_density_controls_spread},
      {"gp_chain_between_pads", test_gp_chain_between_pads},
      {"gp_macro_in_density_field", test_gp_macro_in_density_field},
      {"gp_fixed_macro_is_avoided", test_gp_fixed_macro_is_avoided},
      {"gp_deterministic", test_gp_deterministic},
      {"gp_inflation_feeds_density", test_gp_inflation_feeds_density},
  };
  for (const auto& [name, fn] : tests) {
    const int before = g_failures;
    fn();
    std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name.c_str());
  }
  std::printf("%zu tests, %d failures\n", tests.size(), g_failures);
  return g_failures == 0 ? 0 : 1;
}
