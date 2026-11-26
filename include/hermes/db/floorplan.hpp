#pragma once

// Floorplan and routing-resource data that readers put into the Design:
// placement rows and an optional global-routing grid. Plain input data, read
// by every engine that needs it (legalizers, routers, writers); no algorithm
// state lives here.

#include <cstdint>
#include <string>
#include <vector>

#include "hermes/core/types.hpp"

namespace hermes {

/// One placement row (DEF ROW, Bookshelf .scl CoreRow / SubrowOrigin).
/// Sites run from `origin` in +x: site k covers
/// [origin.x + k * site_step, origin.x + k * site_step + site_width).
struct Row {
  Point origin;               ///< Lower-left corner of the first site.
  Dbu site_width = 0;
  Dbu site_height = 0;        ///< Row height.
  Dbu site_step = 0;          ///< Distance between site origins (usually site_width).
  std::int32_t num_sites = 0;
  Orient orient = Orient::N;
  Id site = kInvalidId;       ///< Site name id (Design::site_name), kInvalidId if unnamed.

  Dbu xhi() const { return origin.x + (num_sites > 0 ? (num_sites - 1) * site_step + site_width : 0); }
  Rect bbox() const { return {origin.x, origin.y, xhi(), origin.y + site_height}; }
};

/// Preferred routing direction of a metal layer.
enum class LayerDir : std::uint8_t { Horizontal, Vertical };

/// One routing (metal) layer, bottom to top, as given by a technology LEF
/// (LAYER ... TYPE ROUTING: DIRECTION, PITCH, WIDTH, SPACING). Lengths in DBU.
struct RoutingLayer {
  std::string name;
  LayerDir dir = LayerDir::Horizontal;
  Dbu pitch = 0;     ///< Track pitch in the preferred direction (0 = unknown).
  Dbu width = 0;     ///< Default wire width.
  Dbu spacing = 0;   ///< Minimum spacing.
};

/// Global-routing grid and capacities, as given by the input (e.g. a
/// Bookshelf .route file of the ISPD 2011 / DAC 2012 routability contests).
/// Routers may use it or build their own grid; it is optional input.
struct RouteGrid {
  std::int32_t nx = 0, ny = 0;          ///< Tiles in x / y.
  std::int32_t num_layers = 0;
  Point origin;                         ///< Lower-left of tile (0, 0).
  Dbu tile_w = 0, tile_h = 0;
  /// Per layer (size num_layers): edge capacities in tracks*width units,
  /// minimum width / spacing and via spacing as given by the input.
  std::vector<double> vertical_capacity, horizontal_capacity;
  std::vector<double> min_wire_width, min_wire_spacing, via_spacing;
  double blockage_porosity = 0.0;
  /// Pins of non-image terminals: the layer their pin sits on (1-based as in
  /// the input). Parallel arrays.
  std::vector<Id> pin_layer_cell;
  std::vector<std::int32_t> pin_layer;
  /// Cells that block routing on some layers: blockage_cell[i] blocks layers
  /// blockage_layers[blockage_begin[i] .. blockage_begin[i + 1]).
  std::vector<Id> blockage_cell;
  std::vector<std::uint32_t> blockage_begin{0};
  std::vector<std::int32_t> blockage_layers;
  /// Capacity adjustments: (x, y, layer) -> (x2, y2, layer2) edge gets
  /// `capacity`. Flattened as 6 ints per edge plus one value.
  std::vector<std::int32_t> adjust_edges;
  std::vector<double> adjust_capacity;

  bool empty() const { return nx == 0 || ny == 0; }
  std::size_t num_blockages() const { return blockage_cell.size(); }
};

}  // namespace hermes
