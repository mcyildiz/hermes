#include "hermes/route/route_shapes.hpp"

#include <algorithm>
#include <cstdlib>

namespace hermes {

EstimatedRouteModel::EstimatedRouteModel(LayerStack stack, BinGrid grid)
    : stack_(std::move(stack)), grid_(grid) {
  for (int l = 0; l < stack_.num_layers(); ++l) {
    const LayerInfo& li = stack_.layers[static_cast<std::size_t>(l)];
    if (!li.routable) continue;
    (li.dir == LayerDir::Horizontal ? hl_ : vl_).push_back(l);
  }
  if (hl_.empty()) hl_ = vl_;
  if (vl_.empty()) vl_ = hl_;
}

// Must match build_route_usage() (src/route/route_usage.cpp): same source
// pin, same L orientation, same tiers, same clamping.
std::size_t EstimatedRouteModel::route_net(std::span<const Point> pins, std::vector<WireSegment>& out,
                                           std::vector<ViaShape>* vias) const {
  if (pins.size() < 2 || hl_.empty()) return 0;
  const Rect& reg = grid_.region();
  auto clamp = [&](Point p) { return Point{std::clamp(p.x, reg.xlo, reg.xhi), std::clamp(p.y, reg.ylo, reg.yhi)}; };
  auto tier = [](int n) {
    int t = 0;
    for (int m = n + 1; m >= 4; m /= 4) ++t;
    return t;
  };
  const Point a = clamp(pins.front());
  const auto [ax, ay] = grid_.bin_of(a);
  std::size_t n = 0;
  int src_top = 0;
  for (std::size_t k = 1; k < pins.size(); ++k) {
    const Point b = clamp(pins[k]);
    const auto [bx, by] = grid_.bin_of(b);
    int lh = -1, lv = -1;
    if (a.x != b.x) {
      lh = hl_[static_cast<std::size_t>(std::min<int>(tier(std::abs(bx - ax)), static_cast<int>(hl_.size()) - 1))];
      out.push_back({a, {b.x, a.y}, lh});
      ++n;
    }
    if (a.y != b.y) {
      lv = vl_[static_cast<std::size_t>(std::min<int>(tier(std::abs(by - ay)), static_cast<int>(vl_.size()) - 1))];
      out.push_back({{b.x, a.y}, b, lv});
      ++n;
    }
    if (lh < 0 && lv < 0) continue;
    src_top = std::max(src_top, lh >= 0 ? lh : lv);
    if (vias) {
      vias->push_back({b, 0, lv >= 0 ? lv : lh});
      if (lh >= 0 && lv >= 0 && lh != lv) vias->push_back({{b.x, a.y}, std::min(lh, lv), std::max(lh, lv)});
    }
  }
  if (vias && src_top > 0) vias->push_back({a, 0, src_top});
  return n;
}

}  // namespace hermes
