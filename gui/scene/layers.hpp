#pragma once

// Display layers of the viewer: what can be shown, hidden, and picked.
//
// Fixed layers come first (die, rows, cells, macros, pins, ...), then one
// layer per routing (metal) layer of the design's stack, then vias and the
// GCell grid. Each layer has a color, a visible flag and a selectable flag;
// picking ignores layers that are hidden or not selectable.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hermes/route/route_usage.hpp"

namespace hermes::gui {

struct Color {
  float r = 1, g = 1, b = 1, a = 1;
};

enum class LayerKind : std::uint8_t { Die, Rows, StdCells, FixedCells, Macros, IoPins, Pins, Metal, Vias, GcellGrid };

struct DisplayLayer {
  std::string name;
  LayerKind kind = LayerKind::StdCells;
  Color color;
  bool visible = true;
  bool selectable = true;
  bool can_select = true;        ///< false: never pickable (die, rows, grid).
  int metal = -1;                ///< LayerStack index for Metal layers.
  std::string group;             ///< "Design", "Routing", ...
};

class LayerSet {
 public:
  static constexpr int kMaxLayers = 32;  ///< Fits the renderer's 32-bit visibility mask.
  // Fixed layer ids.
  static constexpr int kDie = 0, kRows = 1, kStdCells = 2, kFixedCells = 3, kMacros = 4, kIoPins = 5,
                       kPins = 6, kVias = 7, kGcellGrid = 8, kFirstMetal = 9;

  LayerSet();
  /// Rebuilds the metal layers from a layer stack (keeps fixed layers' state).
  void set_metals(const LayerStack& stack);

  int size() const { return static_cast<int>(layers_.size()); }
  DisplayLayer& operator[](int i) { return layers_[static_cast<std::size_t>(i)]; }
  const DisplayLayer& operator[](int i) const { return layers_[static_cast<std::size_t>(i)]; }
  int num_metals() const { return size() - kFirstMetal; }
  int metal_layer(int metal_index) const { return kFirstMetal + metal_index; }

  /// Case-insensitive lookup ("macros", "M3", "std cells"); -1 if none.
  int find(std::string_view name) const;
  bool visible(int i) const { return i >= 0 && i < size() && layers_[static_cast<std::size_t>(i)].visible; }
  bool pickable(int i) const {
    const auto& l = layers_[static_cast<std::size_t>(i)];
    return l.visible && l.selectable && l.can_select;
  }
  std::uint32_t visible_mask() const;
  bool any_metal_visible() const;

 private:
  std::vector<DisplayLayer> layers_;
};

/// Modern, high-contrast palette on a dark background.
Color metal_color(int index);

}  // namespace hermes::gui
