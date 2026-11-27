#include "hermes/flow/steps.hpp"

#include <algorithm>
#include <chrono>
#include <format>

#include "hermes/db/geometry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/timing/sta.hpp"

namespace hermes {

namespace {
HERMES_LOG_CHANNEL(log_step, "flow.step");

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

void FlowEngines::clear_routes() {
  router = GlobalRouter(router.options());
  routed_version = 0;
}

PlacementResult place_design(Design& design, FlowEngines& eng, const PlacerOptions& opts, RunControl* ctl) {
  eng.clear_routes();  // Placement moves (nearly) everything: the old routes say nothing.
  WireDelaySta sta(eng.clock_period_ps, eng.wire_delay_ps_per_dbu);
  MixedSizePlacer placer(opts);
  PlacementResult r = placer.run(design, &sta, &eng.router, ctl);
  design.flush();
  return r;
}

PlacementResult global_place_design(Design& design, FlowEngines& eng, PlacerOptions opts, RunControl* ctl) {
  opts.legalize = false;
  opts.detailed_placement = false;
  return place_design(design, eng, opts, ctl);
}

LegalizeStepResult legalize_design(Design& design, const LegalizeStepOptions& opts) {
  const auto t0 = std::chrono::steady_clock::now();
  LegalizeStepResult r;
  r.hpwl_before = total_hpwl(design);
  const bool region = opts.fence.width() > 0 && opts.fence.height() > 0;
  if (opts.macros && !region) {
    // Same as the placer: clamp into the die, remove overlap, then fix.
    const Rect& die = design.die();
    for (Id ci = 0; ci < static_cast<Id>(design.num_cells()); ++ci) {
      const Cell& c = design.cell(ci);
      if (!c.is_macro || !c.movable()) continue;
      const Point p{std::clamp(c.loc.x, die.xlo, std::max(die.xlo, die.xhi - c.width)),
                    std::clamp(c.loc.y, die.ylo, std::max(die.ylo, die.yhi - c.height))};
      if (p != c.loc) design.move_cell(ci, p);
    }
    r.macros = legalize_macros(design);
    for (Id ci = 0; ci < static_cast<Id>(design.num_cells()); ++ci) {
      const Cell& c = design.cell(ci);
      if (c.is_macro && !c.removed) design.set_fixed(ci, true);
    }
    design.flush();
  }
  if (opts.cells) {
    CellLegalizerOptions co;
    co.fence = opts.fence;
    r.cells = abacus_legalize(design, co);
    design.flush();
  }
  r.hpwl_after = total_hpwl(design);
  r.legal = check_legal(design, legalizer_row_height(design), &r.violation);
  r.seconds = seconds_since(t0);
  return r;
}

RouteStepResult global_route_design(const Design& design, FlowEngines& eng, RouteMode mode, RunControl* ctl) {
  const auto t0 = std::chrono::steady_clock::now();
  RouteStepResult r;
  const bool have = eng.router.routed();
  if (mode == RouteMode::Full || !have) {
    r.stats = eng.router.route(design, ctl);
  } else {
    r.incremental = true;
    r.reroute = eng.router.update(design, ctl);
    r.stats = eng.router.stats();
    r.stats.nets_routed = r.reroute.nets_rerouted;
    r.stats.cancelled = r.reroute.cancelled;
  }
  if (!r.stats.cancelled) eng.routed_version = design.version();
  r.seconds = seconds_since(t0);
  return r;
}

RouteStepResult reroute_region_design(const Design& design, FlowEngines& eng, const Rect& area, RunControl* ctl) {
  const auto t0 = std::chrono::steady_clock::now();
  RouteStepResult r;
  if (!eng.router.routed()) {
    HLOG_INFO(log_step, "no routes yet: routing the whole design first");
    eng.router.route(design, ctl);
    if (ctl && ctl->cancelled()) {
      r.stats = eng.router.stats();
      r.stats.cancelled = true;
      return r;
    }
  }
  r.incremental = true;
  r.reroute = eng.router.reroute_region(design, area, true, ctl, true);
  r.stats = eng.router.stats();
  r.stats.nets_routed = r.reroute.nets_rerouted;
  r.stats.cancelled = r.reroute.cancelled;
  r.seconds = seconds_since(t0);
  return r;
}

TimingStepResult time_design(const Design& design, const FlowEngines& eng) {
  const auto t0 = std::chrono::steady_clock::now();
  WireDelaySta sta(eng.clock_period_ps, eng.wire_delay_ps_per_dbu);
  sta.update(design);
  TimingStepResult r;
  r.wns_ps = sta.wns();
  r.tns_ps = sta.tns();
  r.clock_period_ps = eng.clock_period_ps;
  r.seconds = seconds_since(t0);
  return r;
}

QorReport qor_design(const Design& design, const FlowEngines& eng, QorOptions opts) {
  if (eng.router.routed()) {
    const auto tu = std::chrono::steady_clock::now();
    const RouteUsage3D usage = eng.router.route_usage(design);
    const double us = seconds_since(tu);
    QorReport q = compute_qor(design, opts, &usage);
    q.seconds.insert(q.seconds.begin(), {"route usage", us});
    q.total_seconds += us;
    if (eng.routed_version != design.version())
      q.notes.push_back(
          "The design changed after the last global route; congestion is from the routes as they were then "
          "(run global_route to update them incrementally).");
    return q;
  }
  const BinGrid g = eng.router.gcell_grid(design);
  return compute_qor(design, opts, nullptr, g.nx(), g.ny());
}

}  // namespace hermes
