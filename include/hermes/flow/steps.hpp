#pragma once

// Flow steps: each engine as one call on a Design plus the engine state that
// lives between calls (FlowEngines, kept outside Design).
//
// The console commands (place, global_place, legalize, global_route,
// timing, qor, flow) and run_flow() are all built on these, so a step run
// on its own behaves exactly as it does inside the full flow.
//
//   FlowEngines eng;
//   place_design(d, eng, popts, &ctl);       // GP (router as congestion oracle) + legalization
//   global_route_design(d, eng);             // full route (placement changed)
//   d.move_cell(c, p); d.flush();
//   global_route_design(d, eng);             // incremental: only nets whose pins changed GCell
//   time_design(d, eng);
//   QorReport q = qor_design(d, eng);        // uses the router's routes when it has them

#include <optional>
#include <string>

#include "hermes/db/design.hpp"
#include "hermes/par/job.hpp"
#include "hermes/place/legalizer.hpp"
#include "hermes/place/placer.hpp"
#include "hermes/qor/qor.hpp"
#include "hermes/route/global_router.hpp"

namespace hermes {

/// Engine state that persists between steps. One per design; never stored
/// in Design. Not thread safe: one step at a time (e.g. one background job).
struct FlowEngines {
  explicit FlowEngines(GlobalRouterOptions router_opts = {}) : router(router_opts) {}

  /// Holds the global routes between steps. Steps that move most cells
  /// (place_design, global_place) drop its routes, so the next
  /// global_route_design() is a full route; small changes (legalize in a
  /// region, a moved cell) keep them and the next route is incremental.
  GlobalRouter router;
  // Timing (placeholder wire-delay STA until a Liberty engine exists).
  double clock_period_ps = 1000.0;
  double wire_delay_ps_per_dbu = 0.001;
  /// Design version the routes were last brought up to date with (0 = none).
  std::uint64_t routed_version = 0;

  /// Drops the routes (keeps the router options).
  void clear_routes();
};

// --- Placement -----------------------------------------------------------------

/// Mixed-size placement: global placement with timing and routability
/// feedback (the router is the congestion oracle), then legalization and
/// detailed placement as `opts` says. Drops the router's routes.
PlacementResult place_design(Design& design, FlowEngines& eng, const PlacerOptions& opts,
                             RunControl* ctl = nullptr);

/// Global placement only (no legalization, no detailed placement).
PlacementResult global_place_design(Design& design, FlowEngines& eng, PlacerOptions opts,
                                    RunControl* ctl = nullptr);

struct LegalizeStepOptions {
  bool macros = true;   ///< Legalize movable macros (whole design; then they are fixed).
  bool cells = true;    ///< Abacus legalization of the std cells.
  /// Non-empty: only std cells inside this rectangle move (macros are not
  /// touched). See CellLegalizerOptions::fence.
  Rect fence{};
};

struct LegalizeStepResult {
  MacroLegalizeResult macros;
  CellLegalizeResult cells;
  double hpwl_before = 0.0;
  double hpwl_after = 0.0;
  bool legal = false;       ///< check_legal() after the step.
  std::string violation;    ///< First violation when !legal.
  double seconds = 0.0;
};

/// Legalizes the current placement (macros, then std cells). Keeps the
/// router's routes: the next global route updates them incrementally.
LegalizeStepResult legalize_design(Design& design, const LegalizeStepOptions& opts = {});

// --- Routing -------------------------------------------------------------------

enum class RouteMode {
  Auto,         ///< Incremental when the router has routes for this design, else full.
  Full,         ///< Route every net from scratch.
  Incremental,  ///< Reroute only nets whose pins changed GCell (routes first if needed).
};

struct RouteStepResult {
  GlobalRouteResult stats;      ///< Totals after the step (router.stats() for an incremental step).
  bool incremental = false;
  RerouteResult reroute;        ///< Incremental / region steps.
  double seconds = 0.0;
};

/// Global routing on the current placement. Works on any placed design
/// (e.g. one just loaded from DEF): it does not need a placement step first.
RouteStepResult global_route_design(const Design& design, FlowEngines& eng, RouteMode mode = RouteMode::Auto,
                                    RunControl* ctl = nullptr);

/// Reroutes every net with a pin in or a route through `area` (DBU); routes
/// outside it are untouched. Routes the whole design first if needed.
RouteStepResult reroute_region_design(const Design& design, FlowEngines& eng, const Rect& area,
                                      RunControl* ctl = nullptr);

// --- Timing / QoR --------------------------------------------------------------

struct TimingStepResult {
  double wns_ps = 0.0;
  double tns_ps = 0.0;
  double clock_period_ps = 0.0;
  double seconds = 0.0;
};

/// Static timing on the current placement (WireDelaySta placeholder).
TimingStepResult time_design(const Design& design, const FlowEngines& eng);

/// QoR report. Uses the router's 3-D usage when it has routes, else the
/// estimate on the router's GCell grid (the report says which).
QorReport qor_design(const Design& design, const FlowEngines& eng, QorOptions opts = {});

}  // namespace hermes
