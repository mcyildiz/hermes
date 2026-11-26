#pragma once

#include "hermes/db/design.hpp"
#include "hermes/place/congestion.hpp"
#include "hermes/route/route_usage.hpp"

namespace hermes {

struct GlobalRouterOptions {
  int grid_x = 64;                  ///< GCell grid size.
  int grid_y = 64;
  double capacity_per_gcell = 0.0;  ///< 0 = derive from GCell size.
  double macro_blockage = 0.8;      ///< Fraction of capacity lost under macros.
  /// 3-D usage (route_usage()): lowest routing layer (0-based) and the
  /// layers macros block. See RouteUsageOptions.
  int first_routing_layer = 1;
  int macro_blocked_layers = 4;
};

struct GlobalRouteResult {
  double total_overflow = 0.0;
  double max_utilization = 0.0;
  double wirelength = 0.0;          ///< In DBU.
};

/// Global router. Also serves as the congestion oracle the placer queries
/// during routability-driven placement, so both stages agree on what
/// "congested" means.
class GlobalRouter {
 public:
  explicit GlobalRouter(GlobalRouterOptions opts = {}) : opts_(opts) {}

  const GlobalRouterOptions& options() const { return opts_; }

  /// Builds a GCell map sized to the die with capacities and blockages set.
  CongestionMap make_map(const Design& design) const;

  /// Fast routability estimate used inside placement.
  /// TODO(hermes): swap RUDY for probabilistic / pattern-route estimates.
  void estimate_congestion(const Design& design, CongestionMap& map) const;

  /// Full global route after placement.
  /// TODO(hermes): rip-up-and-reroute with maze routing and layer assignment.
  GlobalRouteResult route(const Design& design, CongestionMap& map) const;

  /// 3-D usage of the same routes (per layer demand/capacity in tracks, vias)
  /// on this router's GCell grid, or the design's route grid if it has one.
  /// The layer assignment is an estimate until the router assigns layers.
  RouteUsage3D route_usage(const Design& design) const;

 private:
  GlobalRouterOptions opts_;
};

}  // namespace hermes
