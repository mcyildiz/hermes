#pragma once

#include <vector>

#include "hermes/db/design.hpp"

namespace hermes {

/// Embedded static timing analysis.
///
/// The placer calls `update()` periodically during global placement, then
/// reads per-net criticality to re-weight nets. Implementations can range from
/// a fast wire-delay model (good enough inside the placement loop) to a full
/// Liberty/SDC-based engine used for sign-off-style reporting.
class StaEngine {
 public:
  virtual ~StaEngine() = default;

  /// Recompute arrival/required times from the current placement
  /// (and, after routing, from routed parasitics).
  virtual void update(const Design& design) = 0;

  /// Worst negative slack and total negative slack, in picoseconds.
  virtual double wns() const = 0;
  virtual double tns() const = 0;

  /// Per-net criticality in [0, 1]; 1 = on the worst path.
  virtual double net_criticality(Id net) const = 0;
};

/// Simple placeholder model: delay proportional to net HPWL, with a single
/// clock period. Lets the timing-driven loop run end to end before a real
/// Liberty-based engine is wired in.
class WireDelaySta final : public StaEngine {
 public:
  /// `ps_per_dbu`: wire delay per database unit of HPWL.
  WireDelaySta(double clock_period_ps, double ps_per_dbu)
      : period_ps_(clock_period_ps), ps_per_dbu_(ps_per_dbu) {}

  void update(const Design& design) override;
  double wns() const override { return wns_; }
  double tns() const override { return tns_; }
  double net_criticality(Id net) const override;

 private:
  double period_ps_;
  double ps_per_dbu_;
  double wns_ = 0.0;
  double tns_ = 0.0;
  std::vector<double> criticality_;
};

}  // namespace hermes
