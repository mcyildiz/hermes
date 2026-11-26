#pragma once

// End-of-run quality-of-results (QoR) report: placement density, evenness,
// 3-D congestion and 3-D wirelength, plus per-metric maps for heatmaps.
//
//   QorReport q = compute_qor(design, opts, &usage);   // usage: router's 3-D result
//   std::puts(format_qor(q).c_str());
//   std::string json = qor_to_json(q);
//
// Models (stated in the report itself):
//  * Density: bins of `bin_rows` row heights; free area = bin area minus
//    fixed cells; utilization = movable cell area / free area. Overflow is
//    ISPD-style: sum over bins of max(0, movable - target * free), divided by
//    the total movable area.
//  * Evenness: coefficient of variation (std / mean) of bin utilization over
//    bins with at least `min_free_fraction` free area, and the share of
//    those bins above the target density (and above 100%).
//  * Congestion: the 3-D track model of route/route_usage.hpp, for the
//    routed result (per layer) and, per direction, for the RUDY estimate the
//    placer works with, on the same GCells and capacities.
//  * Wirelength: HPWL (exact), routed length per layer and vias per cut
//    layer from the 3-D usage (layer split and vias are estimates until the
//    router assigns layers).

#include <cstdint>
#include <string>
#include <vector>

#include "hermes/core/types.hpp"
#include "hermes/db/design.hpp"
#include "hermes/route/route_usage.hpp"

namespace hermes {

struct QorOptions {
  double target_density = 0.7;   ///< Usually the placer's target density.
  int bin_rows = 10;             ///< Density bin side in row heights (ISPD 2006 style).
  int max_bins_per_side = 1024;
  double min_free_fraction = 0.2;  ///< Bins with less free area are left out of evenness/peak.
  int worst_spots = 10;          ///< Hot spots listed in the report.
  bool maps = true;              ///< Keep full-resolution maps (QorReport::maps).
};

/// A scalar map over a grid (row-major, value(ix, iy) = values[iy * nx + ix]).
struct QorMap {
  std::string name;              ///< e.g. "density.util", "congestion.M3.util"
  std::string unit;              ///< "ratio", "count", ...
  int nx = 0, ny = 0;
  Rect region;
  std::vector<float> values;
  float at(int ix, int iy) const { return values[static_cast<std::size_t>(iy) * nx + ix]; }
  /// Downsampled copy with at most `max_side` bins per side, combining
  /// blocks by max (hot spots stay visible) or mean.
  QorMap downsample(int max_side, bool use_max = true) const;
};

struct DensityQor {
  int nx = 0, ny = 0;
  Dbu bin_w = 0, bin_h = 0;
  double target = 0.0;
  double movable_area = 0.0;     ///< DBU^2
  double fixed_area = 0.0;       ///< Fixed cells inside the die (overlaps counted once per bin).
  double free_area = 0.0;
  double avg_util = 0.0;         ///< movable / free over the die.
  double peak_util = 0.0;        ///< Max bin utilization (bins with enough free area).
  double overflow = 0.0;         ///< ISPD-style: sum max(0, movable - target*free) / movable area.
  double overflow_area = 0.0;    ///< The numerator, DBU^2.
  std::size_t bins_counted = 0;  ///< Bins used for peak / evenness.
  // Evenness.
  double util_mean = 0.0;
  double util_std = 0.0;
  double cv = 0.0;               ///< util_std / util_mean.
  double overfull_share = 0.0;   ///< Share of counted bins with util > target.
  double over100_share = 0.0;    ///< Share of counted bins with util > 1.
};

struct LayerQor {
  std::string name;
  LayerDir dir = LayerDir::Horizontal;
  bool routable = true;
  double capacity = 0.0;         ///< Sum over GCells, tracks.
  double demand = 0.0;
  double overflow = 0.0;         ///< Sum max(0, demand - capacity).
  double max_util = 0.0;
  double avg_util = 0.0;         ///< demand / capacity.
  std::size_t overflow_gcells = 0;
  double wirelength = 0.0;       ///< DBU
};

struct HotSpot {
  int ix = 0, iy = 0;
  Rect box;
  double util = 0.0;             ///< Utilization of the GCell's worst layer.
  double overflow = 0.0;         ///< Summed over layers.
  std::string worst_layer;
};

struct CongestionQor {
  int nx = 0, ny = 0;
  Dbu gcell_w = 0, gcell_h = 0;
  std::string layer_source;      ///< LayerStack::source
  std::string model;             ///< One-line description of the model.
  std::string usage_source;      ///< RouteUsage3D::source
  bool routed = true;            ///< false: no routing in this run; usage built for the report.
  std::vector<LayerQor> layers;
  double total_overflow = 0.0;
  double h_overflow = 0.0, v_overflow = 0.0;
  std::size_t overflow_gcells = 0;     ///< GCells with overflow on any layer.
  double max_util = 0.0;               ///< Over layers and GCells.
  double h_max_util = 0.0, v_max_util = 0.0;  ///< 2-D per direction (summed layers).
  /// Average utilization of the worst 0.5/1/2/5% GCell-directions, and
  /// their mean (RC, DAC 2012 style; computed on GCells, not edges).
  double ace[4] = {0, 0, 0, 0};
  double rc = 0.0;
  std::vector<HotSpot> hot_spots;
  // RUDY on the same GCells/capacities, per direction.
  double rudy_h_overflow = 0.0, rudy_v_overflow = 0.0;
  double rudy_max_util = 0.0;
  double rudy_ace[4] = {0, 0, 0, 0};
};

struct WirelengthQor {
  double hpwl = 0.0;                   ///< DBU
  double weighted_hpwl = 0.0;          ///< With input net weights.
  double routed = 0.0;                 ///< DBU, sum over layers.
  std::vector<double> layer_wl;        ///< DBU per layer (parallel to CongestionQor::layers)
  std::vector<std::uint64_t> vias;     ///< Per cut layer.
  std::uint64_t total_vias = 0;
  std::size_t nets = 0, pins = 0, segments = 0;
  bool layers_estimated = true;
};

struct QorReport {
  std::string design;
  double dbu_per_micron = 0.0;
  std::size_t cells = 0, macros = 0, movable = 0, nets = 0;
  DensityQor density;
  CongestionQor congestion;
  WirelengthQor wirelength;
  std::vector<QorMap> maps;            ///< Empty unless QorOptions::maps.
  /// Compute time per section, seconds ("density", "congestion", "rudy", ...).
  std::vector<std::pair<std::string, double>> seconds;
  double total_seconds = 0.0;
  std::vector<std::string> notes;      ///< What is real vs estimated, model disagreements.

  const QorMap* map(std::string_view name) const;
};

/// Computes the report. `usage` is the router's 3-D usage (null: it is
/// built here from the router's routes, and the report says the design was
/// not routed in this run). `router_grid_x/y` are used for that case.
QorReport compute_qor(const Design& design, const QorOptions& opts = {},
                      const RouteUsage3D* usage = nullptr, int router_grid_x = 64, int router_grid_y = 64);

/// Density part only (also usable inside a placer).
DensityQor compute_density_qor(const Design& design, const QorOptions& opts, QorMap* util_map = nullptr,
                               QorMap* overflow_map = nullptr);

/// RUDY split by direction: expected horizontal and vertical wire length
/// (DBU) of all nets in each bin of `g` (each net's HPWL spread uniformly
/// over its bounding box). O(1) per net, deterministic.
void rudy_by_direction(const Design& design, const BinGrid& g, std::vector<double>& h_len,
                       std::vector<double>& v_len);

/// Human-readable report (tables, units in microns when known).
std::string format_qor(const QorReport& q);
/// Machine-readable report (no maps; see qor_maps_to_json).
std::string qor_to_json(const QorReport& q);
/// Maps as JSON: [{"name","unit","nx","ny","region","values"}...], each
/// downsampled to at most max_side per side.
std::string qor_maps_to_json(const QorReport& q, int max_side = 128);

}  // namespace hermes
