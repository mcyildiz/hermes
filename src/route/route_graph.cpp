#include "hermes/route/route_graph.hpp"

#include <algorithm>

#include "hermes/par/parallel.hpp"

namespace hermes {

RouteGraph::RouteGraph(const Design& d, const BinGrid& grid, const RouteUsageOptions& o)
    : grid_(grid), stack_(layer_stack_for(d, o)), nx_(grid.nx()), ny_(grid.ny()) {
  nh_ = static_cast<std::uint32_t>(std::max(0, nx_ - 1) * ny_);
  nv_ = static_cast<std::uint32_t>(nx_ * std::max(0, ny_ - 1));
  const int L = stack_.num_layers();
  for (int l = 0; l < L; ++l) {
    const LayerInfo& li = stack_.layers[static_cast<std::size_t>(l)];
    if (!li.routable) continue;
    (li.dir == LayerDir::Horizontal ? hl_ : vl_).push_back(l);
  }
  // Single-direction stacks (unusual inputs): let both directions use them.
  if (hl_.empty()) hl_ = vl_;
  if (vl_.empty()) vl_ = hl_;
  gcap_ = layer_capacity(d, stack_, grid_, o);
  const std::uint32_t E = num_edges();
  cap_.assign(E, 0.0f);
  hist_.assign(E, 0.0f);
  use_.assign(E, 0);
  lcap_.assign(static_cast<std::size_t>(L), {});
  luse_.assign(static_cast<std::size_t>(L), {});
  for (int l = 0; l < L; ++l) {
    const bool in_h = std::find(hl_.begin(), hl_.end(), l) != hl_.end();
    const bool in_v = std::find(vl_.begin(), vl_.end(), l) != vl_.end();
    if (!in_h && !in_v) continue;
    auto& lc = lcap_[static_cast<std::size_t>(l)];
    lc.assign(E, 0.0f);
    luse_[static_cast<std::size_t>(l)].assign(E, 0);
    const auto& gc = gcap_[static_cast<std::size_t>(l)];
    par::parallel_for(0, E, 1 << 14, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t e = lo; e < hi; ++e) {
        const bool h = e < nh_;
        if ((h && !in_h) || (!h && !in_v)) continue;
        const auto [a, b] = ends(static_cast<std::uint32_t>(e));
        lc[e] = std::min(gc[static_cast<std::size_t>(a)], gc[static_cast<std::size_t>(b)]);
      }
    });
  }
  par::parallel_for(0, E, 1 << 14, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t e = lo; e < hi; ++e) {
      float c = 0.0f;
      for (int l : (e < nh_ ? hl_ : vl_)) c += lcap_[static_cast<std::size_t>(l)][e];
      cap_[e] = c;
    }
  });
}

double RouteGraph::edge_length(std::uint32_t e) const {
  const auto [a, b] = ends(e);
  const Rect ra = grid_.bin_rect(node_x(a), node_y(a));
  const Rect rb = grid_.bin_rect(node_x(b), node_y(b));
  if (is_h(e)) return 0.5 * static_cast<double>(ra.width() + rb.width());
  return 0.5 * static_cast<double>(ra.height() + rb.height());
}

void RouteGraph::clear_usage() {
  std::fill(use_.begin(), use_.end(), 0);
  std::fill(hist_.begin(), hist_.end(), 0.0f);
  for (auto& v : luse_) std::fill(v.begin(), v.end(), 0);
}

}  // namespace hermes
