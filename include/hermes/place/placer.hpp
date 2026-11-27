#pragma once

#include <vector>

#include "hermes/db/design.hpp"
#include "hermes/par/job.hpp"
#include "hermes/place/legalizer.hpp"
#include "hermes/route/global_router.hpp"
#include "hermes/timing/sta.hpp"

namespace hermes {

struct PlacerOptions {
  int max_iterations = 1000;        ///< Upper bound; usually stops on stop_overflow.
  /// Target bin utilization. Filler cells make up the difference so the
  /// electrostatic equilibrium spreads cells to this density, not to 100%.
  double target_density = 0.7;

  // Analytic global placement: Nesterov's method on
  //   sum_nets w * WA(gamma) + lambda * electrostatic energy
  // (ePlace / RePlAce). Macros sit in the same density field as std cells.
  double stop_overflow = 0.07;      ///< Stop once density overflow is below this.
  int bins_x = 0;                   ///< Density grid (0 = auto; rounded up to 2^k).
  int bins_y = 0;
  double initial_density_penalty = 8e-5;  ///< lambda_0 = this * |grad WL| / |grad D|.
  int max_backtracks = 10;          ///< Step-size backtracking per Nesterov step.
  unsigned seed = 1;                ///< Initial jitter and filler positions.
  /// Global-placement progress goes to log channel "place.gp": every
  /// iteration at Debug, and every `log_interval` iterations (and the first)
  /// at Verbose. 0 = Debug only. Turn it on with e.g. `--log place.gp=verbose`.
  int log_interval = 50;

  // Timing-driven: net weight = 1 + timing_weight * criticality^timing_exponent
  int timing_interval = 25;         ///< Iterations between STA updates (0 = off).
  double timing_weight = 2.0;
  double timing_exponent = 2.0;

  // Routability-driven: cells in bins over `inflation_threshold` utilization
  // are virtually inflated so density spreading pushes them apart.
  int routability_interval = 25;    ///< Iterations between congestion updates (0 = off).
  double inflation_threshold = 1.0;
  double max_inflation = 2.5;
  /// Inflation only starts once density overflow is below this: before cells
  /// have spread, the congestion estimate says little about the final layout.
  double routability_start_overflow = 0.3;

  bool legalize = true;
  /// Mixed-size refinement: once macro legalization has fixed the macros,
  /// std cells still sitting on them would be pushed off by the legalizer
  /// in a staircase that costs wirelength. If more than `refine_min_overlap`
  /// of the std-cell area overlaps a macro, a short global-placement pass
  /// (macros fixed, fillers and density penalty carried over) re-spreads the
  /// cells first, for at most `refine_iterations` iterations or until the
  /// density overflow is below stop_overflow. 0 iterations = off. Designs
  /// without movable macros never run it.
  int refine_iterations = 200;
  double refine_min_overlap = 0.002;
  /// Starting density penalty = this * the global placer's final penalty.
  double refine_lambda_scale = 1.0;
  bool detailed_placement = true;
  /// Start cells with Cell::placed (DEF PLACED/FIXED, a placed .pl, a saved
  /// result) at their current location instead of the die center.
  bool keep_placed = true;
};

struct PlacementResult {
  int iterations = 0;
  double hpwl = 0.0;
  double wns_ps = 0.0;
  double tns_ps = 0.0;
  double overflow = 0.0;             ///< Routing (congestion map) overflow.
  double max_utilization = 0.0;
  double density_overflow = 0.0;     ///< Global-placement density overflow at exit.
  double global_hpwl = 0.0;          ///< HPWL right after global placement.
  /// Fraction of std-cell area on (legalized) macros before refinement, and
  /// the refinement iterations run (0 = skipped).
  double macro_overlap = 0.0;
  int refine_iterations = 0;
  double legalize_ms = 0.0;          ///< Macro + cell legalization (and refinement) time.
  bool cancelled = false;            ///< Stopped early by RunControl::request_cancel().
};

/// Placer-owned per-object state, in arrays indexed by design ids. Created by
/// MixedSizePlacer::run(); the Design never sees it.
struct PlacerState {
  /// Working net weights: Net::weight * (1 + timing_weight * crit^exp).
  std::vector<double> net_weight;
  /// Virtual inflation factor per cell (1.0 = none). It scales the cell's
  /// area in the density field (each side by sqrt(factor)).
  std::vector<double> inflation;
};

/// Mixed-size global placer (macros + standard cells placed together) with
/// embedded timing and routability feedback, followed by legalization and
/// detailed placement.
///
/// Flow inside run():
///   1. initial placement
///   2. global placement loop (Nesterov, WA wirelength + electrostatics)
///        - one Nesterov step, until density overflow < stop_overflow
///        - every `timing_interval`:      STA -> net re-weighting
///        - every `routability_interval`: router congestion -> cell inflation
///   3. macro legalization (fix macros), then a short refinement pass if
///      std cells overlap the macros (see PlacerOptions::refine_iterations)
///   4. standard-cell legalization
///   5. detailed placement
/// The design is flushed (Design::flush) after each step that writes
/// positions back, so listeners and snapshot readers follow progress.
class MixedSizePlacer {
 public:
  explicit MixedSizePlacer(PlacerOptions opts = {}) : opts_(opts) {}

  /// `sta` and `router` may be null to disable timing / routability feedback.
  /// With `ctl` (e.g. from a background job, see "hermes/par/job.hpp") the
  /// placer reports progress, writes back and flushes whenever
  /// ctl->publish_due() (so snapshot readers follow global placement), and
  /// stops at the next iteration when cancelled, leaving the design flushed
  /// at the last written-back positions (result.cancelled = true).
  PlacementResult run(Design& design, StaEngine* sta, const GlobalRouter* router,
                      RunControl* ctl = nullptr);

  /// State of the last run (net weights, inflation), indexed by design ids.
  const PlacerState& state() const { return state_; }
  /// Current virtual inflation factor per cell (see PlacerState::inflation).
  const std::vector<double>& inflation() const { return state_.inflation; }

  /// Statistics from the last run's legalization steps.
  const MacroLegalizeResult& macro_legalize_result() const { return macro_result_; }
  const CellLegalizeResult& cell_legalize_result() const { return cell_result_; }

 private:
  void initial_placement(Design& design);
  void update_timing_weights(Design& design, StaEngine& sta);
  /// Returns true if any cell's inflation changed.
  bool update_routability(Design& design, const GlobalRouter& router,
                          CongestionMap& map);
  void legalize_macros(Design& design);
  /// Mixed-size refinement after macro legalization; returns iterations run.
  int refine_after_macros(Design& design, const std::vector<double>& fillers, double lambda,
                          PlacementResult& res, RunControl* ctl);
  void legalize_cells(Design& design);
  void detailed_place(Design& design);
  void clamp_to_die(Design& design, Id cell) const;

  PlacerOptions opts_;
  PlacerState state_;
  MacroLegalizeResult macro_result_;
  CellLegalizeResult cell_result_;
};

}  // namespace hermes
