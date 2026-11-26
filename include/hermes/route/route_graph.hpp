#pragma once

// 2-D global routing graph over a GCell grid, with per-layer capacities and
// usage for layer assignment.
//
// Nodes are GCells (node = y * nx + x). Edges join horizontal and vertical
// neighbours:
//   horizontal edge (x, y) -- (x + 1, y): id y * (nx - 1) + x          (x < nx - 1)
//   vertical   edge (x, y) -- (x, y + 1): id H + y * nx + x            (y < ny - 1)
// with H = (nx - 1) * ny. Edge capacity on a layer is the smaller of the two
// GCells' track capacities on that layer (layer_capacity()), rounded down to
// whole tracks, so a route with no edge overflow also has no GCell overflow
// in the QoR track model. The
// 2-D capacity of an edge is the sum over the layers of its direction.
//
// Usage is in tracks (integers), one per net per edge.

#include <cstdint>
#include <utility>
#include <vector>

#include "hermes/db/bin_grid.hpp"
#include "hermes/route/route_usage.hpp"

namespace hermes {

class RouteGraph {
 public:
  RouteGraph() = default;
  RouteGraph(const Design& d, const BinGrid& grid, const RouteUsageOptions& o);

  const BinGrid& grid() const { return grid_; }
  const LayerStack& stack() const { return stack_; }
  int nx() const { return nx_; }
  int ny() const { return ny_; }
  int num_layers() const { return stack_.num_layers(); }
  std::size_t num_nodes() const { return static_cast<std::size_t>(nx_) * ny_; }
  std::uint32_t num_h_edges() const { return nh_; }
  std::uint32_t num_edges() const { return nh_ + nv_; }

  int node(int x, int y) const { return y * nx_ + x; }
  int node_x(int n) const { return n % nx_; }
  int node_y(int n) const { return n / nx_; }
  std::uint32_t h_edge(int x, int y) const { return static_cast<std::uint32_t>(y * (nx_ - 1) + x); }
  std::uint32_t v_edge(int x, int y) const { return nh_ + static_cast<std::uint32_t>(y * nx_ + x); }
  bool is_h(std::uint32_t e) const { return e < nh_; }
  /// Lower-left / lower end node of an edge, and the other end.
  std::pair<int, int> ends(std::uint32_t e) const {
    if (e < nh_) {
      const int y = static_cast<int>(e) / (nx_ - 1), x = static_cast<int>(e) % (nx_ - 1);
      return {node(x, y), node(x + 1, y)};
    }
    const int n = static_cast<int>(e - nh_);
    return {n, n + nx_};
  }
  /// Edge between two neighbouring nodes.
  std::uint32_t edge_between(int a, int b) const {
    if (a > b) std::swap(a, b);
    return b == a + 1 ? h_edge(node_x(a), node_y(a)) : v_edge(node_x(a), node_y(a));
  }
  /// Length of an edge in DBU (GCell center to center).
  double edge_length(std::uint32_t e) const;

  /// Routable layers of each direction, bottom to top.
  const std::vector<int>& h_layers() const { return hl_; }
  const std::vector<int>& v_layers() const { return vl_; }
  const std::vector<int>& layers_of(std::uint32_t e) const { return is_h(e) ? hl_ : vl_; }

  // 2-D capacity / usage / history.
  float cap(std::uint32_t e) const { return cap_[e]; }
  std::int32_t use(std::uint32_t e) const { return use_[e]; }
  float hist(std::uint32_t e) const { return hist_[e]; }
  float overflow(std::uint32_t e) const {
    const float o = static_cast<float>(use_[e]) - cap_[e];
    return o > 1e-4f ? o : 0.0f;
  }

  // Per layer, indexed by edge id (only edges of the layer's direction).
  float layer_cap(int l, std::uint32_t e) const { return lcap_[static_cast<std::size_t>(l)][e]; }
  std::int32_t layer_use(int l, std::uint32_t e) const { return luse_[static_cast<std::size_t>(l)][e]; }
  /// [layer][gcell] track capacities (layer_capacity()).
  const std::vector<std::vector<float>>& gcell_capacity() const { return gcap_; }

  // --- Mutation (router internals) ---------------------------------------------
  std::vector<std::int32_t>& use_vec() { return use_; }
  std::vector<float>& hist_vec() { return hist_; }
  std::vector<std::int32_t>& layer_use_vec(int l) { return luse_[static_cast<std::size_t>(l)]; }
  void clear_usage();

 private:
  BinGrid grid_;
  LayerStack stack_;
  int nx_ = 0, ny_ = 0;
  std::uint32_t nh_ = 0, nv_ = 0;
  std::vector<int> hl_, vl_;
  std::vector<float> cap_, hist_;
  std::vector<std::int32_t> use_;
  std::vector<std::vector<float>> lcap_;
  std::vector<std::vector<std::int32_t>> luse_;
  std::vector<std::vector<float>> gcap_;
};

}  // namespace hermes
