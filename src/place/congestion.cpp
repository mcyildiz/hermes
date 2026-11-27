#include "hermes/place/congestion.hpp"

#include "hermes/db/geometry.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/par/row_bands.hpp"

#include <algorithm>
#include <stdexcept>

namespace hermes {

CongestionMap::CongestionMap(const Rect& region, int nx, int ny,
                             double capacity_per_bin)
    : grid_(region, nx, ny) {
  demand_.assign(static_cast<std::size_t>(nx) * ny, 0.0);
  capacity_.assign(static_cast<std::size_t>(nx) * ny, capacity_per_bin);
}

void CongestionMap::clear_demand() { std::fill(demand_.begin(), demand_.end(), 0.0); }

void CongestionMap::scale_demand(double f) {
  for (double& d : demand_) d *= f;
}

double CongestionMap::utilization(int ix, int iy) const {
  const double cap = capacity(ix, iy);
  return cap > 0.0 ? demand(ix, iy) / cap : (demand(ix, iy) > 0.0 ? 1e9 : 0.0);
}

double CongestionMap::total_overflow() const {
  return par::parallel_sum(0, demand_.size(), 1 << 14, [&](std::size_t lo, std::size_t hi) {
    double of = 0.0;
    for (std::size_t i = lo; i < hi; ++i) of += std::max(0.0, demand_[i] - capacity_[i]);
    return of;
  });
}

double CongestionMap::max_utilization() const {
  return par::parallel_reduce(
      0, static_cast<std::size_t>(ny()), 64, 0.0,
      [&](std::size_t lo, std::size_t hi) {
        double m = 0.0;
        for (int iy = static_cast<int>(lo); iy < static_cast<int>(hi); ++iy)
          for (int ix = 0; ix < nx(); ++ix) m = std::max(m, utilization(ix, iy));
        return m;
      },
      [](double a, double b) { return std::max(a, b); });
}

void estimate_rudy(const Design& design, CongestionMap& map) {
  // Each net spreads wl / area uniformly over its bounding box.
  //
  // Per GCell row the overlap with a box is (overlap width) * (row overlap
  // height). Interior bins of a row all get the full bin width, so they go
  // into a per-row difference array (O(1) per row instead of per bin) and
  // only the two end bins are added directly: O(rows) per net instead of
  // O(rows * columns), so large (clock, reset) nets stay cheap.
  //
  // Parallel and deterministic: coarse maps (the usual 64x64 .. 256x256)
  // give each fixed block of nets a private grid and sum the grids in block
  // order; fine maps scatter by bands of rows (par::RowBands). Either way the
  // result does not depend on the thread count.
  map.clear_demand();
  const std::size_t n = design.num_nets();
  const BinGrid& g = map.grid();
  const int nx = g.nx(), ny = g.ny();
  const std::size_t rowlen = static_cast<std::size_t>(nx) + 1;
  const double bw = static_cast<double>(g.bin_width());
  struct Box {
    Rect bb;
    double density;  // wl / area
    int x0, x1, y0, y1;
  };
  std::vector<Box> box(n);
  par::parallel_for(0, n, 4096, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      const Id net = static_cast<Id>(i);
      if (design.net_pins(net).size() < 2) {
        box[i].y0 = 1;
        box[i].y1 = 0;
        continue;
      }
      Rect bb = net_bbox(design, net);
      // Give degenerate (zero-width/height) boxes a minimum extent of 1 DBU.
      bb.xhi = std::max(bb.xhi, bb.xlo + 1);
      bb.yhi = std::max(bb.yhi, bb.ylo + 1);
      const auto [x0, y0] = g.bin_of({bb.xlo, bb.ylo});
      const auto [x1, y1] = g.bin_of({bb.xhi, bb.yhi});
      box[i] = {bb, static_cast<double>(bb.width() + bb.height()) / static_cast<double>(bb.area()),
                x0, x1, y0, y1};
    }
  });
  // Adds net i's rows [y0, y1] into a demand grid and its difference array.
  auto deposit = [&](const Box& b, int y0, int y1, double* dem, double* diff) {
    for (int iy = y0; iy <= y1; ++iy) {
      const Rect r0 = g.bin_rect(b.x0, iy);
      const Dbu oy = std::min(b.bb.yhi, r0.yhi) - std::max(b.bb.ylo, r0.ylo);
      if (oy <= 0) continue;
      const double row = b.density * static_cast<double>(oy);
      const std::size_t base = static_cast<std::size_t>(iy) * nx;
      const Dbu ox0 = std::min(b.bb.xhi, r0.xhi) - std::max(b.bb.xlo, r0.xlo);
      if (ox0 > 0) dem[base + b.x0] += row * static_cast<double>(ox0);
      if (b.x1 > b.x0) {
        const Rect r1 = g.bin_rect(b.x1, iy);
        const Dbu ox1 = std::min(b.bb.xhi, r1.xhi) - std::max(b.bb.xlo, r1.xlo);
        if (ox1 > 0) dem[base + b.x1] += row * static_cast<double>(ox1);
        if (b.x1 > b.x0 + 1) {
          double* d = diff + static_cast<std::size_t>(iy) * rowlen;
          d[b.x0 + 1] += row * bw;
          d[b.x1] -= row * bw;
        }
      }
    }
  };
  // Prefix-sums the difference rows into `dem`.
  auto integrate = [&](double* dem, const double* diff, std::size_t iy) {
    double run = 0.0;
    for (int ix = 0; ix < nx; ++ix) {
      run += diff[iy * rowlen + ix];
      dem[iy * nx + ix] += run;
    }
  };

  std::vector<double> demand(g.size(), 0.0);
  if (g.size() <= (std::size_t{1} << 16)) {
    const std::size_t nb = std::min<std::size_t>(64, par::block_count(n, 16384));
    const std::size_t bs = nb ? (n + nb - 1) / nb : 0;
    std::vector<std::vector<double>> part(nb);
    par::parallel_for_each(0, nb, [&](std::size_t blk) {
      std::vector<double> diff(rowlen * ny, 0.0);
      part[blk].assign(g.size(), 0.0);
      for (std::size_t i = blk * bs; i < std::min(n, (blk + 1) * bs); ++i) {
        if (box[i].y0 <= box[i].y1) deposit(box[i], box[i].y0, box[i].y1, part[blk].data(), diff.data());
      }
      for (int iy = 0; iy < ny; ++iy) integrate(part[blk].data(), diff.data(), iy);
    });
    par::parallel_for(0, g.size(), 4096, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t k = lo; k < hi; ++k) {
        double s = 0.0;
        for (std::size_t blk = 0; blk < nb; ++blk) s += part[blk][k];
        demand[k] = s;
      }
    });
  } else {
    std::vector<double> diff(rowlen * ny, 0.0);
    par::RowBands bands;
    bands.build(n, ny, [&](std::size_t i) { return std::pair<int, int>{box[i].y0, box[i].y1}; });
    bands.run([&](std::size_t i, int y0, int y1) { deposit(box[i], y0, y1, demand.data(), diff.data()); });
    par::parallel_for(0, static_cast<std::size_t>(ny), 4, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t iy = lo; iy < hi; ++iy) integrate(demand.data(), diff.data(), iy);
    });
  }
  par::parallel_for(0, static_cast<std::size_t>(ny), 8, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t iy = lo; iy < hi; ++iy)
      for (int ix = 0; ix < nx; ++ix) {
        const double v = demand[iy * nx + ix];
        if (v != 0.0) map.add_demand(ix, static_cast<int>(iy), v);
      }
  });
}

void apply_macro_blockages(const Design& design, CongestionMap& map,
                           double blocked_fraction) {
  for (const Cell& c : design.cells()) {
    if (!c.is_macro || c.removed) continue;
    const Rect bb = c.bbox();
    const auto [x0, y0] = map.bin_of({bb.xlo, bb.ylo});
    const auto [x1, y1] = map.bin_of({bb.xhi, bb.yhi});
    for (int iy = y0; iy <= y1; ++iy) {
      for (int ix = x0; ix <= x1; ++ix) {
        const Rect br = map.bin_rect(ix, iy);
        const double frac = static_cast<double>(br.overlap_area(bb)) /
                            static_cast<double>(br.area());
        map.set_capacity(ix, iy, map.capacity(ix, iy) * (1.0 - blocked_fraction * frac));
      }
    }
  }
}

}  // namespace hermes
