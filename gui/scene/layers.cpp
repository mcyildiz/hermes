#include "gui/scene/layers.hpp"

#include <algorithm>
#include <cctype>

namespace hermes::gui {

namespace {
Color hex(std::uint32_t rgb, float a = 1.0f) {
  return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, a};
}
bool iequal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}
std::string squash(std::string_view s) {  // "Std cells" -> "stdcells"
  std::string o;
  for (char c : s)
    if (!std::isspace(static_cast<unsigned char>(c)) && c != '_' && c != '-')
      o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}
}  // namespace

Color metal_color(int i) {
  // Distinct hues, bright enough on #0f1115; cycles for deep stacks.
  static const std::uint32_t pal[] = {0x7aa2f7, 0xf7768e, 0x9ece6a, 0xe0af68, 0xbb9af7, 0x2ac3de,
                                      0xff9e64, 0x73daca, 0xc0caf5, 0xdb4b4b, 0x41a6b5, 0xcfc9c2};
  return hex(pal[static_cast<std::size_t>(i) % (sizeof(pal) / sizeof(pal[0]))]);
}

LayerSet::LayerSet() {
  layers_.resize(kFirstMetal);
  layers_[kDie] = {"Die", LayerKind::Die, hex(0x8b93a7), true, false, false, -1, "Design"};
  layers_[kRows] = {"Rows", LayerKind::Rows, hex(0x2a2f3a), true, false, false, -1, "Design"};
  layers_[kStdCells] = {"Std cells", LayerKind::StdCells, hex(0x4f7cff), true, true, true, -1, "Design"};
  layers_[kFixedCells] = {"Fixed cells", LayerKind::FixedCells, hex(0x8a93a6), true, true, true, -1, "Design"};
  layers_[kMacros] = {"Macros", LayerKind::Macros, hex(0xd08bff), true, true, true, -1, "Design"};
  layers_[kIoPins] = {"I/O pins", LayerKind::IoPins, hex(0xffd166), true, true, true, -1, "Design"};
  layers_[kPins] = {"Pins", LayerKind::Pins, hex(0xf5f5f5), true, true, true, -1, "Design"};
  layers_[kVias] = {"Vias", LayerKind::Vias, hex(0xeeeeee), false, false, true, -1, "Routing"};
  layers_[kGcellGrid] = {"GCell grid", LayerKind::GcellGrid, hex(0x39404f), false, false, false, -1, "Routing"};
}

void LayerSet::set_metals(const LayerStack& stack) {
  std::vector<DisplayLayer> old(layers_.begin() + kFirstMetal, layers_.end());
  layers_.resize(kFirstMetal);
  const int n = std::min(stack.num_layers(), kMaxLayers - kFirstMetal);
  for (int i = 0; i < n; ++i) {
    const LayerInfo& li = stack.layers[static_cast<std::size_t>(i)];
    DisplayLayer l{li.name, LayerKind::Metal, metal_color(i), li.routable, true, true, i, "Routing"};
    for (const DisplayLayer& o : old)
      if (o.name == l.name) {
        l.visible = o.visible;
        l.selectable = o.selectable;
        l.color = o.color;
      }
    layers_.push_back(std::move(l));
  }
}

int LayerSet::find(std::string_view name) const {
  for (int i = 0; i < size(); ++i)
    if (iequal(layers_[static_cast<std::size_t>(i)].name, name)) return i;
  const std::string q = squash(name);
  for (int i = 0; i < size(); ++i) {
    const std::string n = squash(layers_[static_cast<std::size_t>(i)].name);
    if (n == q || (q.size() >= 3 && n.rfind(q, 0) == 0)) return i;
  }
  return -1;
}

std::uint32_t LayerSet::visible_mask() const {
  std::uint32_t m = 0;
  for (int i = 0; i < size() && i < kMaxLayers; ++i)
    if (layers_[static_cast<std::size_t>(i)].visible) m |= 1u << i;
  return m;
}

bool LayerSet::any_metal_visible() const {
  for (int i = kFirstMetal; i < size(); ++i)
    if (layers_[static_cast<std::size_t>(i)].visible) return true;
  return false;
}

}  // namespace hermes::gui
