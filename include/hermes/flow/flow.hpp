#pragma once

#include <functional>
#include <string_view>

#include "hermes/db/design.hpp"
#include "hermes/par/job.hpp"
#include "hermes/place/placer.hpp"
#include "hermes/qor/qor.hpp"
#include "hermes/route/detailed_router.hpp"
#include "hermes/route/global_router.hpp"
#include "hermes/flow/steps.hpp"

namespace hermes {

struct FlowOptions {
  PlacerOptions placer;
  GlobalRouterOptions router;
  double clock_period_ps = 1000.0;
  double wire_delay_ps_per_dbu = 0.001;
  bool run_detailed_route = true;
  bool run_routing = true;          ///< false: stop after placement.
  /// Compute the end-of-run QoR report (FlowReport::qor). Its target density
  /// is taken from placer.target_density.
  bool report_qor = true;
  QorOptions qor;
  /// Called on the flow's thread after each stage that changed the design
  /// ("placement", "route"), with the design flushed: e.g. to save a
  /// checkpoint (io::save_design, or a copy + io::start_save_job).
  std::function<void(std::string_view stage, const Design& design)> on_stage_done;
};

struct FlowReport {
  PlacementResult placement;
  GlobalRouteResult global_route;
  DetailedRouteResult detailed_route;
  double final_wns_ps = 0.0;
  double final_tns_ps = 0.0;
  bool cancelled = false;   ///< Stopped early through RunControl (later stages skipped).
  bool has_qor = false;
  QorReport qor;            ///< End-of-run report (FlowOptions::report_qor).
  /// Wall time per stage, in order ("placement", "global route", ...).
  std::vector<std::pair<std::string, double>> stage_seconds;
};

/// The flow options as a JSON object (stored with tracked runs).
std::string flow_options_json(const FlowOptions& opts);

/// Full Hermes flow: routability- and timing-driven mixed-size placement,
/// then global and detailed routing, then a final timing update.
/// With `ctl` the flow reports progress per stage, lets the placer publish
/// snapshots at ctl's interval, and stops between stages (or inside
/// placement) when cancelled. Use start_flow_job() to run it in the
/// background.
FlowReport run_flow(Design& design, const FlowOptions& opts = {}, RunControl* ctl = nullptr);

/// run_flow() with engine state that outlives it (the routes stay in
/// `eng.router` for later incremental steps). opts.router and the timing
/// settings are copied into `eng`.
FlowReport run_flow(Design& design, FlowEngines& eng, const FlowOptions& opts, RunControl* ctl = nullptr);

/// run_flow() as a background job. `report` (if given) is filled when the job
/// ends; it and `design` must outlive the job.
JobHandle start_flow_job(Design& design, const FlowOptions& opts, FlowReport* report = nullptr,
                         JobOptions job = {});

}  // namespace hermes
