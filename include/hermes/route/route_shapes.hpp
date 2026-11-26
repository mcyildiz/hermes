#pragma once

// Wire shapes of the global router's current routes, for viewers and
// reports that want to SEE wires (the GUI), not just usage totals.
//
// The router does not store routes yet (it is a stub: star from each net's
// first pin, horizontal-then-vertical L to every other pin, no layer
// assignment). EstimatedRouteModel reproduces exactly those routes with the
// same greedy length-tier layer assignment build_route_usage() uses, so the
// wires a viewer draws add up to the per-layer wirelength in the QoR report
// (tested). When a real router keeps its routes, viewers switch to its route
// store and this model goes away.
//
// The model needs only points, a layer stack and a GCell grid, so it runs
// on design snapshots from any thread.

#include <span>
#include <vector>

#include "hermes/core/types.hpp"
#include "hermes/db/bin_grid.hpp"
#include "hermes/route/route_usage.hpp"

namespace hermes {

struct WireSegment {
  Point a, b;        ///< Axis-parallel: a.x == b.x or a.y == b.y.
  int layer = 0;     ///< Index into the LayerStack.
};

struct ViaShape {
  Point at;
  int from = 0, to = 0;  ///< Layers (from < to): one via per cut in between.
};

class EstimatedRouteModel {
 public:
  EstimatedRouteModel() = default;
  /// `grid` is the router's GCell grid (RouteUsage3D::grid); its region
  /// clamps pin locations as build_route_usage does.
  EstimatedRouteModel(LayerStack stack, BinGrid grid);

  const LayerStack& stack() const { return stack_; }
  const BinGrid& grid() const { return grid_; }

  /// Appends the wires (and vias, if `vias` is given) of one net whose pin
  /// locations are `pins` (first pin = star source). Returns the number of
  /// segments appended.
  std::size_t route_net(std::span<const Point> pins, std::vector<WireSegment>& out,
                        std::vector<ViaShape>* vias = nullptr) const;

 private:
  LayerStack stack_;
  BinGrid grid_;
  std::vector<int> hl_, vl_;  // routable layers per direction, bottom to top
};

}  // namespace hermes
