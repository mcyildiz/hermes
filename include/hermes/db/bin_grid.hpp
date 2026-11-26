#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>

#include "hermes/core/types.hpp"
#include "hermes/db/changes.hpp"

namespace hermes {

/// Uniform nx * ny grid over a region, in integer DBU. Shared by the
/// congestion map, density and region bookkeeping so all of them bin the
/// same way. The last row/column absorbs the remainder of the division.
class BinGrid {
 public:
  BinGrid() = default;
  BinGrid(const Rect& region, int nx, int ny);

  int nx() const { return nx_; }
  int ny() const { return ny_; }
  std::size_t size() const { return static_cast<std::size_t>(nx_) * ny_; }
  const Rect& region() const { return region_; }
  Dbu bin_width() const { return bin_w_; }
  Dbu bin_height() const { return bin_h_; }

  std::size_t index(int ix, int iy) const { return static_cast<std::size_t>(iy) * nx_ + ix; }
  Rect bin_rect(int ix, int iy) const {
    const Dbu xlo = region_.xlo + ix * bin_w_;
    const Dbu ylo = region_.ylo + iy * bin_h_;
    const Dbu xhi = (ix == nx_ - 1) ? region_.xhi : xlo + bin_w_;
    const Dbu yhi = (iy == ny_ - 1) ? region_.yhi : ylo + bin_h_;
    return {xlo, ylo, xhi, yhi};
  }
  /// Bin containing a point (clamped to the grid).
  std::pair<int, int> bin_of(Point p) const {
    const int ix = static_cast<int>(std::clamp<Dbu>((p.x - region_.xlo) / bin_w_, 0, nx_ - 1));
    const int iy = static_cast<int>(std::clamp<Dbu>((p.y - region_.ylo) / bin_h_, 0, ny_ - 1));
    return {ix, iy};
  }

  /// Calls f(ix, iy) for every bin the rectangle's corners span (clamped).
  template <class F>
  void for_each_bin(const Rect& r, F&& f) const {
    const auto [x0, y0] = bin_of({r.xlo, r.ylo});
    const auto [x1, y1] = bin_of({r.xhi, r.yhi});
    for (int iy = y0; iy <= y1; ++iy)
      for (int ix = x0; ix <= x1; ++ix) f(ix, iy);
  }
  /// Marks the bins under `r` (bin index = index(ix, iy)), e.g. to turn
  /// dirty cells/nets into dirty bins.
  void mark_bins(const Rect& r, DirtySet& bins) const {
    for_each_bin(r, [&](int ix, int iy) { bins.mark(static_cast<Id>(index(ix, iy))); });
  }

 private:
  Rect region_;
  int nx_ = 0;
  int ny_ = 0;
  Dbu bin_w_ = 1;
  Dbu bin_h_ = 1;
};

}  // namespace hermes
