// Global router: full route, negotiated congestion, incremental reroute of
// nets and regions, change-tracking updates, trials, cancellation, 3-D usage.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
#include <string_view>
#include <thread>
#include <vector>

#include "hermes/db/geometry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/job.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/place/placer.hpp"
#include "hermes/qor/qor.hpp"
#include "hermes/route/global_router.hpp"

using namespace hermes;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

// Uniform cells on a die, nets of 2-5 pins within `radius` of a seed cell.
inline hermes::Design local_design(int cells, double radius_frac, hermes::Dbu pitch, unsigned seed) {
  using namespace hermes;
  std::mt19937 rng(seed);
  Design d("local");
  const Dbu side = 1000 * static_cast<Dbu>(std::sqrt(double(cells)) * 4);
  d.set_die({0, 0, side, side});
  d.set_row_height(1000);
  std::uniform_int_distribution<Dbu> u(0, side - 2000);
  std::vector<Point> loc(cells);
  for (int i = 0; i < cells; ++i) {
    const Id c = d.add_cell("c" + std::to_string(i), 2000, 1000);
    loc[i] = {u(rng), u(rng) / 1000 * 1000};
    d.move_cell(c, loc[i]);
  }
  // Spatial buckets to find neighbours.
  const int B = std::max(1, static_cast<int>(std::sqrt(double(cells)) / 2));
  std::vector<std::vector<int>> bucket(B * B);
  auto bk = [&](Point p) { int x = std::min<int>(B - 1, p.x * B / side), y = std::min<int>(B - 1, p.y * B / side); return y * B + x; };
  for (int i = 0; i < cells; ++i) bucket[bk(loc[i])].push_back(i);
  const Dbu R = static_cast<Dbu>(radius_frac * side);
  std::uniform_int_distribution<int> pc(0, cells - 1), np(2, 5);
  for (int n = 0; n < cells; ++n) {
    const Id net = d.add_net("n" + std::to_string(n));
    const int a = pc(rng);
    d.connect(a, net, "Z", {1000, 500}, PinDir::Output);
    const int k = np(rng);
    for (int j = 1; j < k; ++j) {
      std::uniform_int_distribution<Dbu> off(-R, R);
      const Point q{std::clamp<Dbu>(loc[a].x + off(rng), 0, side - 1), std::clamp<Dbu>(loc[a].y + off(rng), 0, side - 1)};
      const auto& b = bucket[bk(q)];
      if (b.empty()) continue;
      const int c = b[rng() % b.size()];
      if (c != a) d.connect(c, net, "A" + std::to_string(j), {500, 500});
    }
  }
  if (pitch > 0) {
    std::vector<RoutingLayer> ls;
    for (int i = 0; i < 6; ++i)
      ls.push_back({"M" + std::to_string(i + 1), i % 2 == 0 ? LayerDir::Horizontal : LayerDir::Vertical, pitch * (i >= 3 ? 2 : 1), 0, 0});
    d.set_routing_layers(ls);
  }
  d.flush();
  return d;
}

/// Local nets (2-5 pins within 2% of the die side); `pitch` sets the track
/// pitch of a 6-layer stack: 900 is mildly congested, 1300 congested.
Design make(int cells, Dbu pitch = 900, unsigned seed = 1) { return local_design(cells, 0.02, pitch, seed); }

GlobalRouterOptions opts(int grid = 0) {
  GlobalRouterOptions o;
  o.grid_x = o.grid_y = grid;  // 0: auto (6 rows per GCell)
  return o;
}

std::vector<int> pin_nodes(const Design& d, const RouteGraph& g, Id n) {
  std::vector<int> v;
  for (Id p : d.net_pins(n)) {
    const auto [x, y] = g.grid().bin_of(pin_location(d, p));
    v.push_back(g.node(x, y));
  }
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

/// Every pin GCell of the net is connected by its route edges.
bool connected(const Design& d, const GlobalRouter& r, Id n) {
  const RouteGraph& g = *r.graph();
  const auto pins = pin_nodes(d, g, n);
  if (pins.size() < 2) return r.net_edges(n).empty();
  std::vector<int> par(g.num_nodes());
  std::iota(par.begin(), par.end(), 0);
  auto find = [&](int a) {
    while (par[static_cast<std::size_t>(a)] != a) a = par[static_cast<std::size_t>(a)] = par[static_cast<std::size_t>(par[static_cast<std::size_t>(a)])];
    return a;
  };
  for (std::uint32_t e : r.net_edges(n)) {
    const auto [a, b] = g.ends(e);
    par[static_cast<std::size_t>(find(a))] = find(b);
  }
  for (int p : pins)
    if (find(p) != find(pins[0])) return false;
  return true;
}

bool all_connected(const Design& d, const GlobalRouter& r) {
  for (std::size_t n = 0; n < d.num_nets(); ++n)
    if (!d.net(static_cast<Id>(n)).removed && !connected(d, r, static_cast<Id>(n))) {
      std::fprintf(stderr, "  net %zu not connected\n", n);
      return false;
    }
  return true;
}

/// Graph usage (2-D and per layer) equals the sum of the nets' routes.
bool usage_consistent(const Design& d, const GlobalRouter& r) {
  const RouteGraph& g = *r.graph();
  std::vector<int> use(g.num_edges(), 0);
  std::vector<std::vector<int>> lu(static_cast<std::size_t>(g.num_layers()), std::vector<int>(g.num_edges(), 0));
  for (std::size_t n = 0; n < d.num_nets(); ++n) {
    const auto E = r.net_edges(static_cast<Id>(n));
    const auto L = r.net_layers(static_cast<Id>(n));
    if (E.size() != L.size()) return false;
    for (std::size_t k = 0; k < E.size(); ++k) {
      if (k && E[k] <= E[k - 1]) return false;  // sorted, distinct
      ++use[E[k]];
      if (L[k] != 0xff) ++lu[L[k]][E[k]];
    }
  }
  for (std::uint32_t e = 0; e < g.num_edges(); ++e) {
    if (use[e] != g.use(e)) return false;
    for (int l : g.layers_of(e))
      if (lu[static_cast<std::size_t>(l)][e] != g.layer_use(l, e)) return false;
  }
  return true;
}

std::vector<std::vector<std::uint32_t>> routes(const Design& d, const GlobalRouter& r) {
  std::vector<std::vector<std::uint32_t>> v(d.num_nets());
  for (std::size_t n = 0; n < d.num_nets(); ++n) {
    const auto e = r.net_edges(static_cast<Id>(n));
    v[n].assign(e.begin(), e.end());
  }
  return v;
}

// --- Tests -----------------------------------------------------------------------

void test_capacity_snaps_blocked_gcells() {
  // Two overlapping macros over the same GCell: blocked fraction >= 1, and
  // float sums just below 1 must give exactly 0, not ~1e-6 tracks.
  Design d("snap");
  d.set_die({0, 0, 3000, 3000});
  d.set_row_height(100);
  for (int i = 0; i < 3; ++i) {
    const Id m = d.add_cell("m" + std::to_string(i), 1000, 1000, true);
    d.move_cell(m, {0, 0});
  }
  d.flush();
  RouteUsageOptions o;
  o.grid_x = o.grid_y = 3;
  const LayerStack st = layer_stack_for(d, o);
  const auto cap = layer_capacity(d, st, BinGrid(d.die(), 3, 3), o);
  for (int l = 0; l < 4; ++l) CHECK(cap[static_cast<std::size_t>(l)][0] == 0.0f);
  CHECK(cap[4][0] > 0.0f);
  CHECK(cap[1][4] > 0.0f);
}

void test_full_route_connects_and_is_consistent() {
  Design d = make(8000);
  GlobalRouter r(opts());
  const GlobalRouteResult res = r.route(d);
  CHECK(r.routed());
  CHECK(res.nets_routed > 0);
  CHECK(res.wirelength > 0);
  CHECK(res.vias > 0);
  CHECK(all_connected(d, r));
  CHECK(usage_consistent(d, r));
  // Every edge has a layer of its direction.
  const RouteGraph& g = *r.graph();
  bool layers_ok = true;
  for (std::size_t n = 0; n < d.num_nets(); ++n) {
    const auto E = r.net_edges(static_cast<Id>(n));
    const auto L = r.net_layers(static_cast<Id>(n));
    for (std::size_t k = 0; k < E.size(); ++k) {
      const auto& ls = g.layers_of(E[k]);
      layers_ok = layers_ok && std::find(ls.begin(), ls.end(), L[k]) != ls.end();
    }
  }
  CHECK(layers_ok);
}

void test_deterministic_across_threads() {
  Design d = make(20000, 1300);
  par::set_num_threads(1);
  GlobalRouter a(opts());
  const GlobalRouteResult ra = a.route(d);
  par::set_num_threads(4);
  GlobalRouter b(opts());
  const GlobalRouteResult rb = b.route(d);
  par::set_num_threads(0);
  CHECK(ra.total_overflow == rb.total_overflow);
  CHECK(ra.wirelength == rb.wirelength);
  CHECK(ra.vias == rb.vias);
  CHECK(routes(d, a) == routes(d, b));
}

void test_rip_up_and_reroute_reduces_overflow() {
  // Random placement with coarse pitches: heavily congested.
  Design d = make(20000, 1300);
  GlobalRouterOptions o = opts();
  o.max_iterations = 0;
  GlobalRouter pattern(o);
  const GlobalRouteResult p = pattern.route(d);
  o.max_iterations = 30;
  GlobalRouter nego(o);
  const GlobalRouteResult n = nego.route(d);
  std::fprintf(stderr, "  pattern overflow %.1f (%zu edges), negotiated %.1f (%zu edges), %d iterations\n",
               p.total_overflow, p.overflow_edges, n.total_overflow, n.overflow_edges, n.iterations);
  CHECK(p.total_overflow > 0);
  CHECK(n.total_overflow < 0.5 * p.total_overflow);
  CHECK(n.iterations > 0);
  CHECK(all_connected(d, nego));
  CHECK(usage_consistent(d, nego));
}

void test_reroute_nets_changes_only_scope() {
  Design d = make(8000);
  GlobalRouter r(opts());
  r.route(d);
  const auto before = routes(d, r);
  // Move one cell far away; reroute its nets without repair: only they change.
  const Id c = 100;
  d.move_cell(c, {d.die().xhi - 50000, d.die().yhi - 50000});
  d.flush();
  std::vector<Id> nets;
  for (Id p : d.cell_pins(c))
    if (d.pin(p).net != kInvalidId) nets.push_back(d.pin(p).net);
  RerouteRequest req;
  req.nets = nets;
  req.repair = false;
  const RerouteResult res = r.reroute(d, req);
  CHECK(res.nets_rerouted == nets.size());
  const auto after = routes(d, r);
  std::size_t changed = 0, outside = 0;
  for (std::size_t n = 0; n < before.size(); ++n) {
    if (before[n] == after[n]) continue;
    ++changed;
    outside += std::find(nets.begin(), nets.end(), static_cast<Id>(n)) == nets.end();
  }
  CHECK(changed > 0);
  CHECK(outside == 0);
  CHECK(all_connected(d, r));
  CHECK(usage_consistent(d, r));
}

void test_region_reroute_keeps_outside() {
  Design d = make(20000, 1300);
  GlobalRouter r(opts());
  r.route(d);
  const RouteGraph& g = *r.graph();
  const auto before = routes(d, r);
  const Rect die = d.die();
  const Rect area{die.xlo + die.width() / 4, die.ylo + die.height() / 4, die.xlo + die.width() / 2,
                  die.ylo + die.height() / 2};
  // Some cells inside move a little (as a local placer would); the region
  // reroute reconnects them inside.
  std::mt19937 rng(3);
  const Dbu step = die.width() / 50;
  for (Id c = 0; c < static_cast<Id>(d.num_cells()); ++c) {
    const Cell& cell = d.cell(c);
    if (!cell.movable() || !area.contains(cell.center()) || rng() % 3) continue;
    const Dbu dx = static_cast<Dbu>(rng() % static_cast<unsigned>(2 * step)) - step;
    const Dbu dy = static_cast<Dbu>(rng() % static_cast<unsigned>(2 * step)) - step;
    d.move_cell(c, {std::clamp(cell.loc.x + dx, area.xlo, area.xhi - cell.width),
                    std::clamp(cell.loc.y + dy, area.ylo, area.yhi - cell.height)});
  }
  d.flush();
  const RerouteResult res = r.reroute_region(d, area);
  std::fprintf(stderr, "  region reroute: %zu nets, %zu repaired, overflow %.1f -> %.1f, %.4f s\n",
               res.nets_rerouted, res.nets_repaired, res.overflow_before, res.overflow_after, res.seconds);
  CHECK(res.nets_rerouted > 0);
  CHECK(res.overflow_after <= res.overflow_before + 0.05 * res.overflow_before + 20.0);
  const auto after = routes(d, r);
  // Edges with an end outside the area are exactly as before.
  const auto [ax0, ay0] = g.grid().bin_of({area.xlo, area.ylo});
  const auto [ax1, ay1] = g.grid().bin_of({area.xhi - 1, area.yhi - 1});
  auto inside = [&](std::uint32_t e) {
    const auto [a, b] = g.ends(e);
    auto in = [&](int n) {
      return g.node_x(n) >= ax0 && g.node_x(n) <= ax1 && g.node_y(n) >= ay0 && g.node_y(n) <= ay1;
    };
    return in(a) && in(b);
  };
  bool outside_same = true;
  for (std::size_t n = 0; n < before.size(); ++n) {
    std::vector<std::uint32_t> o1, o2;
    for (auto e : before[n])
      if (!inside(e)) o1.push_back(e);
    for (auto e : after[n])
      if (!inside(e)) o2.push_back(e);
    outside_same = outside_same && o1 == o2;
  }
  CHECK(outside_same);
  CHECK(usage_consistent(d, r));
  // Moved pins stayed inside the area, so every net is connected again.
  CHECK(all_connected(d, r));
}

void test_update_follows_design_changes() {
  Design d = make(8000);
  d.set_tracking(true);
  GlobalRouter r(opts());
  r.attach(d);
  r.route(d);
  // A move inside the same GCell: nothing to reroute.
  const Id c = 200;
  d.move_cell(c, d.cell(c).loc + Point{1, 0});
  d.flush();
  RerouteResult u = r.update(d);
  CHECK(u.nets_rerouted == 0);
  // A move across the die: its nets are rerouted, all connected.
  d.move_cell(c, {d.die().xlo + 1000, d.die().yhi - 20000});
  d.flush();
  u = r.update(d);
  CHECK(u.nets_rerouted > 0);
  CHECK(all_connected(d, r));
  CHECK(usage_consistent(d, r));
  // New net: routed by the next update.
  const Id n = d.add_net("extra");
  d.connect(10, n, "X");
  d.connect(5000, n, "Y");
  d.flush();
  u = r.update(d);
  CHECK(!r.net_edges(n).empty());
  CHECK(connected(d, r, n));
  // Removed net: ripped up.
  d.remove_net(n);
  d.flush();
  r.update(d);
  CHECK(r.net_edges(n).empty());
  CHECK(usage_consistent(d, r));
  // Without attach (no tracking), update() finds the changes by pin key.
  GlobalRouter r2(opts());
  r2.route(d);
  d.move_cell(300, {d.die().xhi - 30000, d.die().ylo + 1000});
  d.flush();
  u = r2.update(d);
  CHECK(u.nets_rerouted > 0);
  CHECK(all_connected(d, r2));
  r.detach();
}

void test_trial_rollback_restores_routes() {
  Design d = make(8000);
  d.set_tracking(true);
  GlobalRouter r(opts());
  r.attach(d);
  r.route(d);
  const auto before = routes(d, r);
  const GlobalRouteResult s0 = r.stats();
  {
    Transaction t(d);
    r.begin_trial();
    for (Id c = 50; c < 60; ++c) d.move_cell(c, {d.die().xhi - 40000, d.die().yhi - 40000});
    d.flush();
    const RerouteResult u = r.update(d);
    CHECK(u.nets_rerouted > 0);
    CHECK(routes(d, r) != before);
    t.rollback();
    d.flush();
    r.rollback_trial();
  }
  CHECK(routes(d, r) == before);
  const GlobalRouteResult s1 = r.stats();
  CHECK(s1.wirelength == s0.wirelength && s1.vias == s0.vias);
  CHECK(usage_consistent(d, r));
  // The design is back where the routes are: update() has nothing to do.
  CHECK(r.update(d).nets_rerouted == 0);
  r.detach();
}

void test_cancel_leaves_consistent_state() {
  Design d = make(40000, 1300);
  GlobalRouter r(opts());
  RunControl ctl;
  ctl.request_cancel();
  const GlobalRouteResult res = r.route(d, &ctl);
  CHECK(res.cancelled);
  CHECK(usage_consistent(d, r));
  // In a job, cancelled midway.
  GlobalRouter r2(opts());
  JobHandle job = start_job("route", [&](RunControl& c) { r2.route(d, &c); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  job.cancel();
  job.wait();
  CHECK(job.state() == JobState::Done || job.state() == JobState::Cancelled);
  CHECK(usage_consistent(d, r2));
}

void test_route_usage_matches_stats() {
  Design d = make(8000);
  GlobalRouter r(opts());
  const GlobalRouteResult res = r.route(d);
  const RouteUsage3D u = r.route_usage(d);
  CHECK(!u.layers_estimated);
  CHECK(std::abs(u.total_wirelength() - res.wirelength) < 1e-6 * res.wirelength);
  CHECK(u.total_vias() == res.vias);
  // Shapes add up to the same wirelength and vias.
  double wl = 0.0;
  std::uint64_t vias = 0;
  std::vector<WireSegment> w;
  std::vector<ViaShape> v;
  for (std::size_t n = 0; n < d.num_nets(); ++n) {
    w.clear();
    v.clear();
    r.net_shapes(d, static_cast<Id>(n), w, &v);
    for (const auto& s : w) wl += static_cast<double>(std::abs(s.b.x - s.a.x) + std::abs(s.b.y - s.a.y));
    for (const auto& s : v) vias += static_cast<std::uint64_t>(s.to - s.from);
  }
  CHECK(vias == res.vias);
  // Center-to-center shapes vs edge lengths (last row/column absorb the remainder).
  CHECK(std::abs(wl - res.wirelength) < 0.02 * res.wirelength);
  // QoR on the routed usage: no estimate note.
  const QorReport q = compute_qor(d, {}, &u);
  CHECK(q.congestion.routed);
}

void test_placer_map_in_tracks() {
  Design d = make(8000);
  GlobalRouter r(opts());
  CongestionMap m = r.make_map(d);
  r.estimate_congestion(d, m);
  const RouteUsage3D u = r.route_usage(d);  // estimate before routing, same grid and capacities
  double cap_map = 0.0, cap_u = 0.0;
  for (int y = 0; y < m.ny(); ++y)
    for (int x = 0; x < m.nx(); ++x) cap_map += m.capacity(x, y);
  for (const auto& l : u.capacity)
    for (float c : l) cap_u += c;
  CHECK(std::abs(cap_map - cap_u) < 1e-3 * cap_u);
  // Routed demand on the same map is on the same scale as the estimate.
  CongestionMap rm = r.make_map(d);
  r.route(d, rm);
  double de = 0.0, dr = 0.0;
  for (int y = 0; y < m.ny(); ++y)
    for (int x = 0; x < m.nx(); ++x) {
      de += m.demand(x, y);
      dr += rm.demand(x, y);
    }
  std::fprintf(stderr, "  demand: RUDY %.1f tracks, routed %.1f tracks\n", de, dr);
  CHECK(dr > 0.3 * de && dr < 3.0 * de);
}

void test_routability_feedback_on_track_map() {
  // The placer's routability feedback with the default (track) map: a
  // congested design gets cells inflated where utilization exceeds 1.
  Design d = make(4000, 1800);
  GlobalRouter r(opts());
  CongestionMap m = r.make_map(d);
  r.estimate_congestion(d, m);
  CHECK(m.max_utilization() > 1.0);
  PlacerOptions po;
  po.max_iterations = 40;
  po.timing_interval = 0;
  po.routability_interval = 5;
  po.routability_start_overflow = 1.0;
  po.stop_overflow = 0.0;
  po.legalize = false;
  po.detailed_placement = false;
  po.keep_placed = true;
  MixedSizePlacer placer(po);
  placer.run(d, nullptr, &r);
  const auto& infl = placer.inflation();
  CHECK(std::count_if(infl.begin(), infl.end(), [](double f) { return f > 1.0; }) > 0);
}

}  // namespace

int main(int argc, char** argv) {
  log::set_level("", log::Level::Warn);
  struct T {
    const char* name;
    void (*fn)();
  } tests[] = {
      {"capacity_snaps_blocked_gcells", test_capacity_snaps_blocked_gcells},
      {"full_route_connects_and_is_consistent", test_full_route_connects_and_is_consistent},
      {"deterministic_across_threads", test_deterministic_across_threads},
      {"rip_up_and_reroute_reduces_overflow", test_rip_up_and_reroute_reduces_overflow},
      {"reroute_nets_changes_only_scope", test_reroute_nets_changes_only_scope},
      {"region_reroute_keeps_outside", test_region_reroute_keeps_outside},
      {"update_follows_design_changes", test_update_follows_design_changes},
      {"trial_rollback_restores_routes", test_trial_rollback_restores_routes},
      {"cancel_leaves_consistent_state", test_cancel_leaves_consistent_state},
      {"route_usage_matches_stats", test_route_usage_matches_stats},
      {"placer_map_in_tracks", test_placer_map_in_tracks},
      {"routability_feedback_on_track_map", test_routability_feedback_on_track_map},
  };
  const char* only = argc > 1 ? argv[1] : nullptr;
  for (const auto& t : tests) {
    if (only && std::string_view(t.name).find(only) == std::string_view::npos) continue;
    const int before = g_failures;
    const auto t0 = std::chrono::steady_clock::now();
    t.fn();
    std::fprintf(stderr, "%s %s (%.2f s)\n", g_failures == before ? "ok  " : "FAIL", t.name,
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  }
  if (g_failures) {
    std::fprintf(stderr, "FAILED (%d failures)\n", g_failures);
    return 1;
  }
  std::fprintf(stderr, "all route tests passed\n");
  return 0;
}
