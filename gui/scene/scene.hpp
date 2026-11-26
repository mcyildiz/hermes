#pragma once

// GUI scene: what the viewer draws, built from an immutable DesignSnapshot
// on a worker thread (never on the UI thread, never from the Design).
//
// Level of detail, coarse to fine:
//   1. Scene::density   cell-area image of the whole die (std cells), drawn
//                       when cells are smaller than a pixel. Cheap, built
//                       first, so a 1M-cell design shows up at once.
//   2. Scene::cells     one rectangle per std cell, sorted by tile so the
//                       renderer draws only the tile rows in view.
//      Scene::macros    always drawn as rectangles (few, large).
//   3. Detail           pins, wires and vias of the nets in a window around
//                       the view, built on demand when zoomed in.
//
// Coordinates are floats relative to Scene::origin (the die's lower-left
// corner), in DBU. A float holds integers exactly up to 2^24 DBU (8.4 mm at
// 2000 DBU/um); beyond that positions round to 2+ DBU, invisible on screen.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "hermes/db/snapshot.hpp"
#include "hermes/route/route_shapes.hpp"
#include "gui/scene/layers.hpp"

namespace hermes::gui {

/// One rectangle / segment / marker as the GPU gets it (20 bytes).
///  Rect:    (x0, y0) - (x1, y1).
///  Segment: from (x0, y0) to (x1, y1), drawn `width` pixels wide.
///  Marker:  centered at (x0, y0), x1 = half size in pixels.
struct Instance {
  float x0, y0, x1, y1;
  std::uint32_t style;  ///< Display layer (bits 0-7) | flags (see below).
};
static_assert(sizeof(Instance) == 20);

namespace style {
inline constexpr std::uint32_t kLayerMask = 0xffu;
inline constexpr std::uint32_t kMarker = 1u << 8;    ///< Pixel-sized marker at (x0, y0).
inline constexpr std::uint32_t kFixed = 1u << 9;     ///< Fixed object (drawn hatched / dimmer).
inline constexpr std::uint32_t kUnplaced = 1u << 10; ///< Location not meaningful yet.
inline constexpr std::uint32_t make(int layer, std::uint32_t flags = 0) {
  return static_cast<std::uint32_t>(layer) | flags;
}
inline constexpr int layer_of(std::uint32_t s) { return static_cast<int>(s & kLayerMask); }
}  // namespace style

/// Static technology data the viewer needs, taken from the Design once at
/// load (it does not change while engines run).
struct TechInfo {
  EstimatedRouteModel routes;   ///< Router's wire model (layer stack + GCell grid).
  std::string wires_source;     ///< What the drawn wires are, for the UI.
};

/// A float image over a world rectangle (row-major, y up).
struct ScalarImage {
  int w = 0, h = 0;
  Rect region;                   ///< World rectangle covered (DBU).
  std::vector<float> values;     ///< w * h
  float vmin = 0.0f, vmax = 0.0f;  ///< Range over the image.
  std::string name, unit;
  bool empty() const { return values.empty(); }
};

struct SceneOptions {
  int tiles_per_side = 128;      ///< Tile grid for view culling (clamped to cell count).
  int density_max_side = 1024;   ///< Density image resolution (long side).
  bool pin_density = false;      ///< Also build the pin-density image.
};

struct Scene {
  std::shared_ptr<const DesignSnapshot> snap;
  std::uint64_t version = 0;
  Rect die;
  Point origin;                  ///< World origin of the float coordinates.

  // Tiles: std cells sorted by tile (row-major), by cell center.
  int tiles_x = 1, tiles_y = 1;
  double tile_w = 1, tile_h = 1;  ///< DBU
  std::vector<Instance> cells;
  std::vector<Id> cell_ids;       ///< Parallel to cells.
  std::vector<std::uint32_t> tile_begin;  ///< tiles_x * tiles_y + 1 offsets into cells.
  float max_cell_w = 0, max_cell_h = 0;   ///< Largest std cell (culling margin).
  double avg_cell_w = 0, avg_cell_h = 0;

  std::vector<Instance> macros;
  std::vector<Id> macro_ids;
  std::vector<Instance> io_pins;  ///< Markers.
  std::vector<Id> io_pin_ids;
  std::vector<Instance> rows;     ///< Row outlines as segments (bottom edges).

  ScalarImage density;            ///< Std-cell area / bin area.
  ScalarImage pin_density;        ///< Pins per um^2 (if SceneOptions::pin_density).

  std::size_t num_std = 0, num_fixed_std = 0, num_macros = 0, num_unplaced = 0;
  double build_ms = 0.0;
  double density_ms = 0.0;

  /// Cells range [first, last) of tile row iy, tile columns [ix0, ix1].
  std::pair<std::uint32_t, std::uint32_t> tile_row_range(int iy, int ix0, int ix1) const {
    const std::size_t b = static_cast<std::size_t>(iy) * tiles_x;
    return {tile_begin[b + ix0], tile_begin[b + ix1 + 1]};
  }
  /// Tile index range covering a world rectangle (clamped).
  void tiles_for(const Rect& world, int& ix0, int& iy0, int& ix1, int& iy1) const;
  float fx(Dbu x) const { return static_cast<float>(x - origin.x); }
  float fy(Dbu y) const { return static_cast<float>(y - origin.y); }
};

/// Builds the scene (parallel on the hermes::par pool).
std::shared_ptr<Scene> build_scene(std::shared_ptr<const DesignSnapshot> snap, const SceneOptions& o = {});

/// Pins, wires and vias near the view.
struct DetailOptions {
  Rect window;                    ///< World rectangle (DBU).
  bool pins = true;
  bool wires = true;
  bool vias = true;
  std::size_t max_nets = 400000;  ///< Budget; Detail::truncated if exceeded.
  std::size_t max_segments = 4000000;
  /// Skip nets with more pins (clock / reset trees would cover the view).
  std::size_t max_net_pins = 2000;
};

struct Detail {
  std::uint64_t version = 0;
  Rect window;
  DetailOptions opts;
  std::vector<Instance> pins;     ///< Markers (Pins layer).
  std::vector<Id> pin_ids;
  std::vector<Instance> wires;    ///< Segments, sorted by display layer (draw bottom up).
  std::vector<Id> wire_nets;      ///< Parallel to wires.
  /// wires of display layer L are [layer_begin[L], layer_begin[L + 1]).
  std::vector<std::uint32_t> layer_begin;
  std::vector<Instance> vias;     ///< Markers (Vias layer).
  std::size_t nets = 0;
  std::size_t skipped_big_nets = 0;
  bool truncated = false;
  double build_ms = 0.0;
};

std::shared_ptr<Detail> build_detail(const Scene& scene, const TechInfo& tech, const DetailOptions& o);

/// Wires of one net (for highlighting a selection), same model as Detail.
void net_wires(const DesignSnapshot& s, const TechInfo& tech, Id net, const Scene& scene,
               std::vector<Instance>& wires, std::vector<Instance>* vias = nullptr);

// --- Picking ---------------------------------------------------------------

struct PickResult {
  enum class Kind : std::uint8_t { None, Cell, Net, Pin } kind = Kind::None;
  Id id = kInvalidId;
};

struct PickContext {
  const Scene* scene = nullptr;
  const Detail* detail = nullptr;   ///< May be null (no pins / wires to pick).
  const LayerSet* layers = nullptr;
  double world_per_px = 1.0;
  double tolerance_px = 4.0;
};

/// The object under world point p, honoring layer visibility and
/// selectability. Order: pins, wires, std cells, macros (the smallest
/// object wins; make wires unselectable to click through them).
PickResult pick(const PickContext& ctx, Point p);

}  // namespace hermes::gui
