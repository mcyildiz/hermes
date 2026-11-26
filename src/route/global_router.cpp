#include "hermes/route/global_router.hpp"

#include <algorithm>
#include <cstdlib>

#include "hermes/db/geometry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"

namespace hermes {

namespace {
HERMES_LOG_CHANNEL(log_grt, "route.global");
}  // namespace

CongestionMap GlobalRouter::make_map(const Design& design) const {
  const Rect& die = design.die();
  double cap = opts_.capacity_per_gcell;
  if (cap <= 0.0) {
    // Default: enough capacity for wire length equal to ~1 bin perimeter of
    // track length. Real values come from LEF track pitches and layer count.
    const double bw = static_cast<double>(die.width()) / opts_.grid_x;
    const double bh = static_cast<double>(die.height()) / opts_.grid_y;
    cap = 2.0 * (bw + bh);
  }
  CongestionMap map(die, opts_.grid_x, opts_.grid_y, cap);
  apply_macro_blockages(design, map, opts_.macro_blockage);
  return map;
}

void GlobalRouter::estimate_congestion(const Design& design,
                                       CongestionMap& map) const {
  estimate_rudy(design, map);
}

namespace {

// Adds one unit of bin-length demand per GCell along a horizontal-then-
// vertical L path from a to b, into `demand` (row-major over the grid).
double route_l(const BinGrid& g, double* demand, Point a, Point b) {
  auto [ax, ay] = g.bin_of(a);
  auto [bx, by] = g.bin_of(b);
  const double bin_w = static_cast<double>(g.bin_rect(0, 0).width());
  const double bin_h = static_cast<double>(g.bin_rect(0, 0).height());
  const int sx = (bx >= ax) ? 1 : -1;
  for (int x = ax; x != bx; x += sx) demand[g.index(x, ay)] += bin_w;
  const int sy = (by >= ay) ? 1 : -1;
  for (int y = ay; y != by; y += sy) demand[g.index(bx, y)] += bin_h;
  return static_cast<double>(std::llabs(a.x - b.x) + std::llabs(a.y - b.y));
}

}  // namespace

GlobalRouteResult GlobalRouter::route(const Design& design,
                                      CongestionMap& map) const {
  map.clear_demand();
  GlobalRouteResult res;
  // Nets are routed independently (no rip-up yet), in parallel: each block
  // of nets fills a private demand grid, and the grids are summed in block
  // order. Demands are whole multiples of the GCell size, so the sums are
  // exact and match the serial loop bit for bit.
  // TODO(hermes): a negotiated-congestion router needs shared costs; route
  // regions (par::RowBands-style tiles) or batches of non-overlapping nets.
  const BinGrid& g = map.grid();
  const std::size_t nnets = design.num_nets();
  const std::size_t nb = std::min<std::size_t>(par::block_count(nnets, 4096), 64);
  const std::size_t bs = nb ? (nnets + nb - 1) / nb : 0;
  std::vector<std::vector<double>> part(nb);
  std::vector<double> wl(nb, 0.0);
  par::parallel_for_each(0, nb, [&](std::size_t blk) {
    part[blk].assign(g.size(), 0.0);
    double* dem = part[blk].data();
    double w = 0.0;
    for (std::size_t i = blk * bs; i < std::min(nnets, (blk + 1) * bs); ++i) {
      const auto pins = design.net_pins(static_cast<Id>(i));
      if (pins.size() < 2) continue;
      // Star decomposition from the first (driver) pin.
      // TODO(hermes): RSMT (e.g. FLUTE) decomposition.
      const Point src = pin_location(design, pins.front());
      for (std::size_t k = 1; k < pins.size(); ++k) {
        w += route_l(g, dem, src, pin_location(design, pins[k]));
      }
    }
    wl[blk] = w;
  });
  par::parallel_for(0, static_cast<std::size_t>(g.ny()), 4, [&](std::size_t lo, std::size_t hi) {
    for (int iy = static_cast<int>(lo); iy < static_cast<int>(hi); ++iy) {
      for (int ix = 0; ix < g.nx(); ++ix) {
        double d = 0.0;
        for (std::size_t blk = 0; blk < nb; ++blk) d += part[blk][g.index(ix, iy)];
        map.add_demand(ix, iy, d);
      }
    }
  });
  for (double w : wl) res.wirelength += w;
  res.total_overflow = map.total_overflow();
  res.max_utilization = map.max_utilization();
  HLOG_INFO(log_grt, "global route: wirelength {:.6g}, overflow {:.4g}, max utilization {:.3f}",
            res.wirelength, res.total_overflow, res.max_utilization);
  return res;
}

RouteUsage3D GlobalRouter::route_usage(const Design& design) const {
  RouteUsageOptions o;
  o.grid_x = opts_.grid_x;
  o.grid_y = opts_.grid_y;
  o.first_routing_layer = opts_.first_routing_layer;
  o.macro_blocked_layers = opts_.macro_blocked_layers;
  return build_route_usage(design, o);
}

}  // namespace hermes
