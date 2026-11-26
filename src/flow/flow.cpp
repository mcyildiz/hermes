#include "hermes/flow/flow.hpp"

#include <utility>

#include <chrono>
#include <format>

#include "hermes/core/json.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/timing/sta.hpp"

namespace hermes {

namespace {
HERMES_LOG_CHANNEL(log_flow, "flow");

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

FlowReport run_flow(Design& design, const FlowOptions& opts, RunControl* ctl) {
  FlowReport rep;
  HLOG_INFO(log_flow, "design {}: {} cells ({} macros), {} nets", design.name(),
            design.cells().size(), count_macros(design), design.nets().size());
  auto t0 = std::chrono::steady_clock::now();

  // TODO(hermes): choose STA engine from options once a Liberty-based one exists.
  WireDelaySta sta(opts.clock_period_ps, opts.wire_delay_ps_per_dbu);
  GlobalRouter groute(opts.router);

  // Placement with the router in the loop for congestion feedback.
  MixedSizePlacer placer(opts.placer);
  if (ctl) ctl->begin_stage("placement", 0.0, 0.85);
  rep.placement = placer.run(design, &sta, &groute, ctl);
  if (rep.placement.cancelled || (ctl && ctl->cancelled())) {
    rep.cancelled = true;
    HLOG_INFO(log_flow, "flow cancelled after {:.3f} s", seconds_since(t0));
    return rep;
  }
  HLOG_INFO(log_flow, "placement done in {:.3f} s: HPWL {:.6g}, routing overflow {:.4g}",
            seconds_since(t0), rep.placement.hpwl, rep.placement.overflow);
  rep.stage_seconds.emplace_back("placement", seconds_since(t0));
  if (opts.on_stage_done) opts.on_stage_done("placement", design);
  if (!opts.run_routing) {
    sta.update(design);
    rep.final_wns_ps = sta.wns();
    rep.final_tns_ps = sta.tns();
    if (opts.report_qor) {
      if (ctl) ctl->begin_stage("report", 0.99, 1.0);
      const auto tq = std::chrono::steady_clock::now();
      QorOptions qo = opts.qor;
      qo.target_density = opts.placer.target_density;
      rep.qor = compute_qor(design, qo, nullptr, opts.router.grid_x, opts.router.grid_y);
      rep.has_qor = true;
      rep.stage_seconds.emplace_back("report", seconds_since(tq));
    }
    return rep;
  }
  t0 = std::chrono::steady_clock::now();

  // Routing.
  if (ctl) ctl->begin_stage("global route", 0.85, 0.95);
  CongestionMap map = groute.make_map(design);
  rep.global_route = groute.route(design, map);
  // The router's 3-D result (layer usage, vias) for the report.
  RouteUsage3D usage;
  double usage_s = 0.0;
  if (opts.report_qor) {
    const auto tu = std::chrono::steady_clock::now();
    usage = groute.route_usage(design);
    usage_s = seconds_since(tu);
  }
  rep.stage_seconds.emplace_back("global route", seconds_since(t0));
  if (ctl && ctl->cancelled()) {
    rep.cancelled = true;
    return rep;
  }
  if (opts.run_detailed_route) {
    const auto td = std::chrono::steady_clock::now();
    if (ctl) ctl->begin_stage("detailed route", 0.95, 0.98);
    DetailedRouter droute;
    rep.detailed_route = droute.route(design, rep.global_route);
    rep.stage_seconds.emplace_back("detailed route", seconds_since(td));
  }

  HLOG_INFO(log_flow, "routing done in {:.3f} s", seconds_since(t0));
  if (opts.on_stage_done) opts.on_stage_done("route", design);

  // Post-route timing (TODO: use routed parasitics).
  const auto tt = std::chrono::steady_clock::now();
  if (ctl) ctl->begin_stage("timing", 0.98, 0.99);
  sta.update(design);
  rep.final_wns_ps = sta.wns();
  rep.final_tns_ps = sta.tns();
  rep.stage_seconds.emplace_back("timing", seconds_since(tt));
  HLOG_INFO(log_flow, "timing: WNS {:.2f} ps, TNS {:.2f} ps", rep.final_wns_ps, rep.final_tns_ps);

  if (opts.report_qor) {
    if (ctl) ctl->begin_stage("report", 0.99, 1.0);
    const auto tq = std::chrono::steady_clock::now();
    QorOptions qo = opts.qor;
    qo.target_density = opts.placer.target_density;
    rep.qor = compute_qor(design, qo, &usage);
    rep.qor.seconds.insert(rep.qor.seconds.begin(), {"route usage", usage_s});
    rep.qor.total_seconds += usage_s;
    rep.qor.notes.push_back(std::format(
        "The 2-D numbers in the log disagree: placement 'routing overflow' {:.4g} (RUDY) vs global route "
        "overflow {:.4g}. Both use the old 2-D map (capacity 2*(w+h) of wire per GCell, 80% lost under macros), "
        "but RUDY spreads each net's HPWL over its bounding box while the router adds one whole GCell length per "
        "GCell its L path crosses and nothing for wire inside one GCell, so the two are not comparable. The 3-D track model above is the one to use; the placer and router move to "
        "it with the real router (step 8).",
        rep.placement.overflow, rep.global_route.total_overflow));
    rep.has_qor = true;
    rep.stage_seconds.emplace_back("report", seconds_since(tq));
  }
  return rep;
}

std::string flow_options_json(const FlowOptions& o) {
  JsonWriter j;
  j.begin_object();
  const PlacerOptions& p = o.placer;
  j.key("placer").begin_object();
  j.kv("max_iterations", p.max_iterations).kv("target_density", p.target_density).kv("stop_overflow", p.stop_overflow);
  j.kv("bins_x", p.bins_x).kv("bins_y", p.bins_y).kv("initial_density_penalty", p.initial_density_penalty);
  j.kv("seed", p.seed).kv("timing_interval", p.timing_interval).kv("timing_weight", p.timing_weight);
  j.kv("timing_exponent", p.timing_exponent).kv("routability_interval", p.routability_interval);
  j.kv("inflation_threshold", p.inflation_threshold).kv("max_inflation", p.max_inflation);
  j.kv("routability_start_overflow", p.routability_start_overflow).kv("legalize", p.legalize);
  j.kv("detailed_placement", p.detailed_placement).kv("keep_placed", p.keep_placed);
  j.end_object();
  j.key("router").begin_object();
  j.kv("grid_x", o.router.grid_x).kv("grid_y", o.router.grid_y).kv("capacity_per_gcell", o.router.capacity_per_gcell);
  j.kv("macro_blockage", o.router.macro_blockage).kv("first_routing_layer", o.router.first_routing_layer);
  j.kv("macro_blocked_layers", o.router.macro_blocked_layers);
  j.end_object();
  j.kv("clock_period_ps", o.clock_period_ps).kv("wire_delay_ps_per_dbu", o.wire_delay_ps_per_dbu);
  j.kv("run_routing", o.run_routing).kv("run_detailed_route", o.run_detailed_route).kv("report_qor", o.report_qor);
  j.key("qor").begin_object().kv("bin_rows", o.qor.bin_rows).kv("min_free_fraction", o.qor.min_free_fraction).end_object();
  j.end_object();
  return j.take();
}

JobHandle start_flow_job(Design& design, const FlowOptions& opts, FlowReport* report,
                         JobOptions job) {
  return start_job(
      "flow",
      [&design, opts, report](RunControl& ctl) {
        FlowReport r = run_flow(design, opts, &ctl);
        if (report) *report = r;
      },
      std::move(job));
}

}  // namespace hermes
