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
  FlowEngines eng(opts.router);
  return run_flow(design, eng, opts, ctl);
}

FlowReport run_flow(Design& design, FlowEngines& eng, const FlowOptions& opts, RunControl* ctl) {
  FlowReport rep;
  HLOG_INFO(log_flow, "design {}: {} cells ({} macros), {} nets", design.name(),
            design.cells().size(), count_macros(design), design.nets().size());
  auto t0 = std::chrono::steady_clock::now();
  eng.router.set_options(opts.router);
  eng.clock_period_ps = opts.clock_period_ps;
  eng.wire_delay_ps_per_dbu = opts.wire_delay_ps_per_dbu;

  // Placement with the router in the loop for congestion feedback.
  if (ctl) ctl->begin_stage("placement", 0.0, opts.run_routing ? 0.85 : 0.98);
  rep.placement = place_design(design, eng, opts.placer, ctl);
  if (rep.placement.cancelled || (ctl && ctl->cancelled())) {
    rep.cancelled = true;
    HLOG_INFO(log_flow, "flow cancelled after {:.3f} s", seconds_since(t0));
    return rep;
  }
  HLOG_INFO(log_flow, "placement done in {:.3f} s: HPWL {:.6g}, routing overflow {:.4g}",
            seconds_since(t0), rep.placement.hpwl, rep.placement.overflow);
  rep.stage_seconds.emplace_back("placement", seconds_since(t0));
  if (opts.on_stage_done) opts.on_stage_done("placement", design);

  if (opts.run_routing) {
    t0 = std::chrono::steady_clock::now();
    if (ctl) ctl->begin_stage("global route", 0.85, 0.95);
    const RouteStepResult gr = global_route_design(design, eng, RouteMode::Full, ctl);
    rep.global_route = gr.stats;
    rep.stage_seconds.emplace_back("global route", seconds_since(t0));
    if (rep.global_route.cancelled || (ctl && ctl->cancelled())) {
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
  }

  // Timing (TODO: use routed parasitics).
  const auto tt = std::chrono::steady_clock::now();
  if (ctl) ctl->begin_stage("timing", 0.98, 0.99);
  const TimingStepResult tm = time_design(design, eng);
  rep.final_wns_ps = tm.wns_ps;
  rep.final_tns_ps = tm.tns_ps;
  if (opts.run_routing) rep.stage_seconds.emplace_back("timing", seconds_since(tt));
  HLOG_INFO(log_flow, "timing: WNS {:.2f} ps, TNS {:.2f} ps", rep.final_wns_ps, rep.final_tns_ps);

  if (opts.report_qor) {
    if (ctl) ctl->begin_stage("report", 0.99, 1.0);
    const auto tq = std::chrono::steady_clock::now();
    QorOptions qo = opts.qor;
    qo.target_density = opts.placer.target_density;
    rep.qor = qor_design(design, eng, qo);
    if (opts.run_routing)
      rep.qor.notes.push_back(std::format(
          "Placement 'routing overflow' {:.4g} is the RUDY estimate on the router's GCell grid in tracks; global "
          "route overflow {:.4g} is the router's 2-D edge overflow (tracks) after rip-up and reroute.",
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
