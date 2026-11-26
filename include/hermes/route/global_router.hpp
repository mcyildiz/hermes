#pragma once

// Global router: pattern routing, negotiated-congestion rip-up and reroute
// (A* maze), greedy layer assignment, and incremental rerouting of selected
// nets or regions.
//
//   GlobalRouter gr(opts);
//   gr.route(design, &ctl);                          // full route
//   gr.reroute(design, {.nets = {n1, n2}});          // rip up + reroute 2 nets,
//                                                    // then repair overflow near them
//   gr.reroute_region(design, rect);                 // only route edges inside rect change
//   design.move_cell(c, p); design.flush();
//   gr.update(design);                               // reroute nets whose pins changed GCell
//
// Model. A 2-D GCell graph (route_graph.hpp): one edge between each pair of
// neighbouring GCells, its capacity the sum over the layers of that
// direction of the tracks crossing it (layer_capacity(), the same numbers
// the QoR report uses). Nets are decomposed into two-pin connections by a
// rectilinear MST over their pin GCells (TODO: RSMT/FLUTE). The initial
// route picks the cheapest L or Z pattern per connection, in rounds that
// read a frozen usage (deterministic, parallel). Rip-up and reroute then
// repeats, while edges overflow: raise history cost on overflowed edges,
// rip up the nets that use them and reroute them with A* in their box plus a
// margin, under PathFinder costs (1 + history) * (1 + penalty * overflow).
// Nets are rerouted in parallel batches whose search windows do not share a
// tile, so a batch sees no races and the result does not depend on the
// thread count. Finally each straight run of a net is put on the layer of
// its direction with the least overflow, then the least utilization.
//
// Locality. Every reroute takes a fence (default: the whole grid). Only
// route edges with both ends inside the fence are ripped up; the rest of a
// net's route is kept and its cut ends become terminals, so a region reroute
// reconnects the net inside the region and changes nothing outside it. The
// repair phase after an incremental reroute only rips up nets that use
// overflowed edges inside the window around the rerouted nets, and only
// their part inside that window. Nets outside never change.
//
// State. Routes, usage and history live in the router (indexed by net id),
// never in Design. The router reads Design only; it is not thread safe (run
// it from one thread, e.g. a job) but uses the par:: pool inside. With a
// RunControl it reports progress and stops at batch boundaries when
// cancelled, leaving every net with either its old or its new route.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "hermes/db/design.hpp"
#include "hermes/par/job.hpp"
#include "hermes/place/congestion.hpp"
#include "hermes/route/route_graph.hpp"
#include "hermes/route/route_shapes.hpp"
#include "hermes/route/route_usage.hpp"

namespace hermes {

struct GlobalRouterOptions {
  /// GCell grid. 0 = auto: square GCells of `gcell_rows` row heights.
  /// A Bookshelf .route grid in the design takes precedence.
  int grid_x = 0;
  int grid_y = 0;
  double gcell_rows = 6.0;
  int max_grid = 1024;              ///< Auto grid cap per side.
  /// > 0: the placer's 2-D map (make_map) uses this uniform capacity per
  /// GCell in DBU of wire and `macro_blockage` (the old model). 0: the map
  /// is in tracks from the layer stack, on the same scale as the router.
  double capacity_per_gcell = 0.0;
  double macro_blockage = 0.8;
  /// Lowest routing layer (0-based) and layers macros block (see
  /// RouteUsageOptions).
  int first_routing_layer = 1;
  int macro_blocked_layers = 4;

  // --- Routing --------------------------------------------------------------
  int initial_rounds = 8;           ///< Pattern-route rounds (each reads the usage of the previous ones).
  int max_iterations = 40;          ///< Rip-up and reroute iterations (full route).
  int maze_margin = 8;              ///< GCells added around a connection's box for the A* search.
  /// Rip-up and reroute only the part of a net around its overflowed edges
  /// (their box plus rrr_margin), not the whole net: less work and smaller,
  /// more parallel batches for big nets. Falls back to the whole net when
  /// the part cannot be reconnected locally.
  bool local_rrr = true;
  int rrr_margin = 6;
  double history_increment = 1.0;   ///< Added per overflowed track per iteration.
  double overflow_penalty = 4.0;    ///< Initial penalty per overflowed track.
  double penalty_growth = 1.5;      ///< Penalty factor per iteration.
  double max_penalty = 1000.0;
  /// Stop rip-up and reroute after this many iterations without lowering
  /// the overflow by at least 1% of the best so far (0 = never).
  int stall_iterations = 4;
  double bend_cost = 0.5;           ///< Pattern route: cost per bend (fewer vias).
  int big_net_pins = 64;            ///< Larger nets search from the connection's pin only, not the tree.
  bool layer_assignment = true;

  // --- Incremental ------------------------------------------------------------
  int repair_margin = 4;            ///< GCells around rerouted nets searched for overflow to repair.
  int repair_iterations = 8;
  /// Initial overflow penalty for incremental reroutes and their repair.
  /// (Starting high, e.g. 50, detoured more and repaired worse in tests.)
  double incremental_penalty = 4.0;
};

struct GlobalRouteResult {
  double total_overflow = 0.0;      ///< 2-D edge overflow, tracks.
  double max_utilization = 0.0;     ///< 2-D edges with capacity.
  double wirelength = 0.0;          ///< In DBU (GCell-center to GCell-center).
  std::size_t overflow_edges = 0;
  std::uint64_t vias = 0;
  double layer_overflow = 0.0;      ///< 3-D (per layer) edge overflow after layer assignment.
  int iterations = 0;               ///< Rip-up and reroute iterations run.
  std::size_t nets_routed = 0;
  bool cancelled = false;
  std::vector<std::pair<std::string, double>> seconds;  ///< Per stage.
};

/// An incremental request. `nets` are ripped up (inside `fence`) and
/// rerouted; then, with `repair`, the overflow they caused nearby (on edges
/// whose use grew, within repair_margin GCells of their boxes) is resolved.
/// Overflow that was there before is left alone.
struct RerouteRequest {
  std::vector<Id> nets;
  std::optional<Rect> fence;        ///< DBU. Nothing outside it changes.
  bool repair = true;
  int repair_iterations = -1;       ///< -1: options().repair_iterations.
  /// Fill the RerouteResult overflow and wirelength fields (a scan over all
  /// edges and nets: costs more than rerouting a few nets).
  bool report = false;
};

struct RerouteResult {
  std::size_t nets_rerouted = 0;
  std::size_t nets_repaired = 0;    ///< Rip-ups of other nets during repair (a net can count per iteration).
  std::size_t nets_skipped = 0;     ///< Could not be connected inside the fence (kept as they were).
  int iterations = 0;
  // With RerouteRequest::report:
  double overflow_before = 0.0;     ///< 2-D overflow in the repair region.
  double overflow_after = 0.0;
  double total_overflow = 0.0;      ///< Whole design, after.
  double wirelength_delta = 0.0;    ///< DBU.
  bool cancelled = false;
  double seconds = 0.0;
};

/// Global router. Also the congestion oracle the placer queries during
/// routability-driven placement (make_map / estimate_congestion), on the same
/// GCell grid and track capacities.
class GlobalRouter {
 public:
  explicit GlobalRouter(GlobalRouterOptions opts = {});
  ~GlobalRouter();
  GlobalRouter(const GlobalRouter& o);
  GlobalRouter& operator=(const GlobalRouter& o);
  GlobalRouter(GlobalRouter&&) noexcept;
  GlobalRouter& operator=(GlobalRouter&&) noexcept;

  const GlobalRouterOptions& options() const { return opts_; }
  /// Options take effect on the next full route() (grid/capacity options) or
  /// the next call (routing options).
  void set_options(const GlobalRouterOptions& o) { opts_ = o; }

  /// The GCell grid this router uses for a design.
  BinGrid gcell_grid(const Design& design) const;

  // --- Placer-side estimate ---------------------------------------------------
  /// GCell map sized to the router's grid with capacities and blockages set.
  CongestionMap make_map(const Design& design) const;
  /// Fast routability estimate used inside placement (RUDY, converted to
  /// tracks when the map is in tracks).
  void estimate_congestion(const Design& design, CongestionMap& map) const;

  // --- Full route -------------------------------------------------------------
  /// Routes every net from scratch (clears all state and history).
  GlobalRouteResult route(const Design& design, RunControl* ctl = nullptr);
  /// route(), then fills `map` (rebuilt with make_map() if its grid differs)
  /// with the routed 2-D demand, in the map's units.
  GlobalRouteResult route(const Design& design, CongestionMap& map, RunControl* ctl = nullptr);

  // --- Incremental ------------------------------------------------------------
  RerouteResult reroute(const Design& design, const RerouteRequest& req, RunControl* ctl = nullptr);
  /// Reroutes, inside `area`, every net that has a pin in it or a route
  /// through it. Routes outside `area` are untouched.
  RerouteResult reroute_region(const Design& design, const Rect& area, bool repair = true,
                               RunControl* ctl = nullptr, bool report = false);
  /// Brings the routes up to date with the design: routes new nets, drops
  /// removed ones and reroutes nets whose pin GCells changed (pins that moved
  /// within their GCell cost nothing). Attached (attach()), only the nets the
  /// design reported dirty are checked; otherwise every net is (cheap: one
  /// hash per net). A changed floorplan or grid triggers a full route().
  RerouteResult update(const Design& design, RunControl* ctl = nullptr);
  /// Subscribes to the design's flushes to collect dirty nets for update().
  /// Turn design tracking on for that to narrow anything (otherwise flushes
  /// report `all` and update() checks every net).
  void attach(Design& design);
  void detach();

  // --- Trials -----------------------------------------------------------------
  /// Records the routes of nets changed from here on, so a trial move
  /// (Transaction + reroute) can be undone: rollback_trial() restores them
  /// exactly (history is kept). Trials do not nest.
  void begin_trial();
  void commit_trial();
  void rollback_trial();
  bool in_trial() const;

  // --- Results ----------------------------------------------------------------
  bool routed() const;
  /// The routing graph (null before the first route).
  const RouteGraph* graph() const;
  /// A net's 2-D route edges (sorted graph edge ids) and their layers.
  std::span<const std::uint32_t> net_edges(Id net) const;
  std::span<const std::uint8_t> net_layers(Id net) const;
  /// Current totals (2-D overflow, wirelength, vias).
  GlobalRouteResult stats() const;
  /// Total 2-D overflow inside a DBU rectangle (edges with both ends in it).
  double overflow_in(const Rect& area) const;
  /// Wires (GCell center to center, merged runs, on their layers) and vias
  /// of one net, for viewers.
  void net_shapes(const Design& design, Id net, std::vector<WireSegment>& wires,
                  std::vector<ViaShape>* vias = nullptr) const;
  /// 3-D usage of the current routes (per-layer demand/capacity in tracks,
  /// vias). Before any route, the estimate build_route_usage() on this
  /// router's grid.
  RouteUsage3D route_usage(const Design& design) const;

 public:
  struct State;  ///< Implementation (src/route/global_router.cpp).

 private:
  GlobalRouterOptions opts_;
  std::unique_ptr<State> st_;
};

}  // namespace hermes
