#pragma once

// 3-D routing usage on a GCell grid: per metal layer and GCell, demand and
// capacity in tracks of the layer's preferred direction, wirelength per
// layer and via counts per cut layer.
//
// This is the one congestion model the QoR report uses, for the routed
// result and (in 2-D, per direction) for the RUDY placement estimate, so the
// two are on the same scale:
//
//   capacity(layer, g) = tracks crossing GCell g in the layer's direction
//                        (GCell height / pitch for a horizontal layer),
//                        reduced under blockages
//   demand(layer, g)   = wire length on the layer inside g / GCell length
//                        along the direction (1.0 = one full track used)
//
// Today the global router is a stub (star decomposition, L-shapes, no layer
// assignment, no rip-up), so build_route_usage() adds a greedy, congestion-
// unaware layer assignment on top of the router's L-shapes: short runs on
// low layers, long runs on high layers. The per-layer split and the via
// counts are therefore ESTIMATES until a real 3-D router fills this
// structure itself.

#include <cstdint>
#include <string>
#include <vector>

#include "hermes/db/bin_grid.hpp"
#include "hermes/db/design.hpp"

namespace hermes {

struct LayerInfo {
  std::string name;
  LayerDir dir = LayerDir::Horizontal;
  Dbu pitch = 0;          ///< Track pitch used for capacity (DBU).
  bool routable = true;   ///< false: pin access only (no global-route demand).
};

/// Metal layers bottom to top, and where they came from.
struct LayerStack {
  std::vector<LayerInfo> layers;
  /// "lef" (technology LEF), "route file" (Bookshelf .route) or
  /// "default (assumed)" when the input has no layer data.
  std::string source;
  int num_layers() const { return static_cast<int>(layers.size()); }
};

struct RouteUsageOptions {
  /// GCell grid when the design has no input route grid (Bookshelf .route);
  /// the GlobalRouter passes its own grid so both use the same GCells.
  int grid_x = 64;
  int grid_y = 64;
  bool use_route_grid = true;     ///< Use the design's RouteGrid tiles/capacities if present.
  /// Layers below this index carry no routing (0-based; 1 = M1 is pin access
  /// only). Ignored with a route file, whose capacities say it.
  int first_routing_layer = 1;
  /// Macros block layers [0, macro_blocked_layers) (clamped so the top two
  /// layers stay open). Ignored with a route file that lists blockages.
  int macro_blocked_layers = 4;
};

/// The layer stack for a design: LEF routing layers, else the route file's
/// layers, else a default 6-layer stack derived from the row height.
LayerStack layer_stack_for(const Design& d, const RouteUsageOptions& o = {});

struct RouteUsage3D {
  BinGrid grid;
  LayerStack stack;
  /// [layer][gcell] in tracks (gcell = grid.index(ix, iy)).
  std::vector<std::vector<float>> demand;
  std::vector<std::vector<float>> capacity;
  /// [cut][gcell]: vias between layer cut and cut + 1.
  std::vector<std::vector<std::uint32_t>> vias;
  std::vector<double> wirelength;         ///< [layer] DBU
  std::vector<std::uint64_t> via_count;   ///< [cut]
  std::size_t num_segments = 0;           ///< Two-pin connections routed.
  /// How the usage was produced (printed in reports).
  std::string source;
  bool layers_estimated = true;           ///< Layer split / vias are estimates.

  int num_layers() const { return stack.num_layers(); }
  double total_wirelength() const;
  std::uint64_t total_vias() const;
};

/// Routes every net as the global router does today (star from the driver,
/// horizontal-then-vertical L per sink) and assigns each horizontal and
/// vertical run to a layer of that direction by its length in GCells (log4
/// tiers: < 3 GCells on the lowest layer, 3-14 the next, ...). Pins are on
/// the lowest layer; vias stack from it to the run's layer, and at each bend
/// between the two layers. Parallel and deterministic (integer
/// accumulation).
RouteUsage3D build_route_usage(const Design& d, const RouteUsageOptions& o = {});

}  // namespace hermes
