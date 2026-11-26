#pragma once

#include <vector>

#include "hermes/db/bin_grid.hpp"
#include "hermes/db/design.hpp"

namespace hermes {

/// 2-D routing demand/capacity grid shared by the placer (routability
/// feedback) and the global router.
class CongestionMap {
 public:
  CongestionMap(const Rect& region, int nx, int ny, double capacity_per_bin);

  int nx() const { return grid_.nx(); }
  int ny() const { return grid_.ny(); }
  const Rect& region() const { return grid_.region(); }
  const BinGrid& grid() const { return grid_; }
  Rect bin_rect(int ix, int iy) const { return grid_.bin_rect(ix, iy); }

  double demand(int ix, int iy) const { return demand_[index(ix, iy)]; }
  double capacity(int ix, int iy) const { return capacity_[index(ix, iy)]; }
  void add_demand(int ix, int iy, double d) { demand_[index(ix, iy)] += d; }
  void set_capacity(int ix, int iy, double c) { capacity_[index(ix, iy)] = c; }
  void clear_demand();

  /// demand / capacity for one bin.
  double utilization(int ix, int iy) const;

  /// Sum over bins of max(0, demand - capacity).
  double total_overflow() const;
  double max_utilization() const;

  /// Bin containing a point (clamped to the grid).
  std::pair<int, int> bin_of(Point p) const { return grid_.bin_of(p); }

 private:
  std::size_t index(int ix, int iy) const { return grid_.index(ix, iy); }

  BinGrid grid_;
  std::vector<double> demand_;
  std::vector<double> capacity_;
};

/// RUDY (Rectangular Uniform wire DensitY) estimate: spreads each net's
/// expected wirelength uniformly over its bounding box. Cheap enough to run
/// inside the global-placement loop.
void estimate_rudy(const Design& design, CongestionMap& map);

/// Derates bin capacity under macros (routing blockage).
void apply_macro_blockages(const Design& design, CongestionMap& map,
                           double blocked_fraction = 0.8);

}  // namespace hermes
