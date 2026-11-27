#include "hermes/route/route_usage.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>

#include "hermes/db/geometry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"

namespace hermes {

namespace {
HERMES_LOG_CHANNEL(log_usage, "route.usage");

template <class T>
void atomic_add(T& slot, T v) {
  std::atomic_ref<T>(slot).fetch_add(v, std::memory_order_relaxed);
}

Dbu default_pitch(const Design& d) {
  if (d.row_height() > 0) return std::max<Dbu>(1, d.row_height() / 9);
  return std::max<Dbu>(1, std::min(d.die().width(), d.die().height()) / 2000);
}

}  // namespace

double RouteUsage3D::total_wirelength() const {
  double s = 0.0;
  for (double w : wirelength) s += w;
  return s;
}

std::uint64_t RouteUsage3D::total_vias() const {
  std::uint64_t s = 0;
  for (auto v : via_count) s += v;
  return s;
}

LayerStack layer_stack_for(const Design& d, const RouteUsageOptions& o) {
  LayerStack st;
  const Dbu p0 = default_pitch(d);
  if (!d.routing_layers().empty()) {
    st.source = "lef";
    const auto layers = d.routing_layers();
    const int n = static_cast<int>(layers.size());
    const int first = n >= o.first_routing_layer + 2 ? o.first_routing_layer : 0;
    for (int i = 0; i < n; ++i) {
      const RoutingLayer& l = layers[static_cast<std::size_t>(i)];
      Dbu pitch = l.pitch > 0 ? l.pitch : l.width + l.spacing;
      if (pitch <= 0) pitch = p0;
      st.layers.push_back({l.name, l.dir, pitch, i >= first});
    }
    return st;
  }
  const RouteGrid& rg = d.route_grid();
  if (o.use_route_grid && !rg.empty() && rg.num_layers > 0) {
    st.source = "route file";
    for (int l = 0; l < rg.num_layers; ++l) {
      const auto at = [&](const std::vector<double>& v) {
        return static_cast<std::size_t>(l) < v.size() ? v[static_cast<std::size_t>(l)] : 0.0;
      };
      const double h = at(rg.horizontal_capacity), v = at(rg.vertical_capacity);
      Dbu pitch = static_cast<Dbu>(std::llround(at(rg.min_wire_width) + at(rg.min_wire_spacing)));
      if (pitch <= 0) pitch = p0;
      st.layers.push_back({"M" + std::to_string(l + 1), h >= v ? LayerDir::Horizontal : LayerDir::Vertical,
                           pitch, h > 0.0 || v > 0.0});
    }
    return st;
  }
  // Default: a typical 6-metal stack for a 9-track library, M1 horizontal.
  st.source = "default (assumed)";
  const Dbu mult[6] = {1, 1, 1, 2, 2, 4};
  for (int i = 0; i < 6; ++i) {
    st.layers.push_back({"M" + std::to_string(i + 1), i % 2 == 0 ? LayerDir::Horizontal : LayerDir::Vertical,
                         p0 * mult[i], i >= o.first_routing_layer});
  }
  return st;
}

BinGrid route_gcell_grid(const Design& d, const RouteUsageOptions& o) {
  const RouteGrid& rg = d.route_grid();
  if (!rg.empty() && o.use_route_grid) {
    const Rect r{rg.origin.x, rg.origin.y, rg.origin.x + rg.nx * rg.tile_w, rg.origin.y + rg.ny * rg.tile_h};
    return BinGrid(r, rg.nx, rg.ny);
  }
  return BinGrid(d.die(), std::max(1, o.grid_x), std::max(1, o.grid_y));
}

std::vector<std::vector<float>> layer_capacity(const Design& d, const LayerStack& stack, const BinGrid& g,
                                               const RouteUsageOptions& o) {
  const RouteGrid& rg = d.route_grid();
  const bool from_file = o.use_route_grid && !rg.empty() && stack.source == "route file";
  const int L = stack.num_layers();
  const int nx = g.nx(), ny = g.ny();
  const std::size_t ng = g.size();
  std::vector<std::vector<float>> capacity(static_cast<std::size_t>(L), std::vector<float>(ng, 0.0f));
  // Blocked fraction per gcell and layer.
  std::vector<std::vector<float>> blocked(static_cast<std::size_t>(L));
  auto block = [&](const Rect& bb, int layer) {
    if (layer < 0 || layer >= L) return;
    auto& b = blocked[static_cast<std::size_t>(layer)];
    if (b.empty()) b.assign(ng, 0.0f);
    g.for_each_bin(bb, [&](int ix, int iy) {
      const Rect br = g.bin_rect(ix, iy);
      const double a = static_cast<double>(br.area());
      if (a > 0) b[g.index(ix, iy)] += static_cast<float>(static_cast<double>(br.overlap_area(bb)) / a);
    });
  };
  if (from_file && rg.num_blockages() > 0) {
    for (std::size_t i = 0; i < rg.num_blockages(); ++i) {
      const Id c = rg.blockage_cell[i];
      if (c == kInvalidId || static_cast<std::size_t>(c) >= d.num_cells() || d.cell(c).removed) continue;
      for (std::uint32_t k = rg.blockage_begin[i]; k < rg.blockage_begin[i + 1]; ++k)
        block(d.cell(c).bbox(), rg.blockage_layers[k] - 1);
    }
  } else {
    const int nb = std::clamp(o.macro_blocked_layers, 0, std::max(0, L - 2));
    for (const Cell& c : d.cells()) {
      if (!c.is_macro || c.removed) continue;
      for (int l = 0; l < nb; ++l) block(c.bbox(), l);
    }
  }
  std::atomic<std::size_t> snapped{0};
  par::parallel_for_each(0, static_cast<std::size_t>(L), [&](std::size_t l) {
    const LayerInfo& li = stack.layers[l];
    if (!li.routable) return;
    double file_tracks = -1.0;
    if (from_file) {
      const auto& cap = li.dir == LayerDir::Horizontal ? rg.horizontal_capacity : rg.vertical_capacity;
      file_tracks = l < cap.size() ? cap[l] / static_cast<double>(std::max<Dbu>(1, li.pitch)) : 0.0;
    }
    std::size_t sn = 0;
    for (int iy = 0; iy < ny; ++iy)
      for (int ix = 0; ix < nx; ++ix) {
        const Rect br = g.bin_rect(ix, iy);
        double t = file_tracks >= 0.0
                       ? file_tracks
                       : static_cast<double>(li.dir == LayerDir::Horizontal ? br.height() : br.width()) /
                             static_cast<double>(li.pitch);
        if (!blocked[l].empty()) {
          // Overlapping macros can sum past 1, and float sums of a fully
          // covered GCell land just below 1 (e.g. 0.99999994): that left a
          // capacity of ~1e-6 tracks and utilizations of 1e7 (adaptec2 M4).
          const double bf = static_cast<double>(blocked[l][g.index(ix, iy)]);
          if (bf >= 1.0 - 1e-4) {
            sn += bf < 1.0;
            t = 0.0;
          } else {
            t *= 1.0 - bf;
          }
        }
        if (t < 1e-3) t = 0.0;
        capacity[l][g.index(ix, iy)] = static_cast<float>(t);
      }
    snapped.fetch_add(sn, std::memory_order_relaxed);
  });
  HLOG_DEBUG(log_usage, "layer capacity: {}x{} GCells, {} layers ({}), {} nearly-blocked GCell layers snapped to 0",
             nx, ny, L, stack.source, snapped.load());
  return capacity;
}

RouteUsage3D build_route_usage(const Design& d, const RouteUsageOptions& o) {
  RouteUsage3D u;
  u.stack = layer_stack_for(d, o);
  u.grid = route_gcell_grid(d, o);
  u.source = "estimate (star + L-shape) with greedy length-based layer assignment";
  const BinGrid& g = u.grid;
  const int nx = g.nx(), ny = g.ny();
  const std::size_t ng = g.size();
  const int L = u.stack.num_layers();
  const Rect& reg = g.region();

  // Layers by direction, bottom to top.
  std::vector<int> hl, vl;
  for (int l = 0; l < L; ++l) {
    const LayerInfo& li = u.stack.layers[static_cast<std::size_t>(l)];
    if (!li.routable) continue;
    (li.dir == LayerDir::Horizontal ? hl : vl).push_back(l);
  }
  if (hl.empty()) hl = vl;
  if (vl.empty()) vl = hl;

  u.capacity = layer_capacity(d, u.stack, g, o);

  // Demand: covered length (DBU) per layer and gcell, accumulated with
  // integer atomics so the result is independent of thread count and order.
  // Interior gcells of a run get a full GCell length through a difference
  // array (O(1) per run); only the two end gcells are added directly.
  std::vector<std::vector<std::int64_t>> cov(static_cast<std::size_t>(L)), diff(static_cast<std::size_t>(L));
  for (int l = 0; l < L; ++l) {
    if (!u.stack.layers[static_cast<std::size_t>(l)].routable) continue;
    cov[static_cast<std::size_t>(l)].assign(ng, 0);
    diff[static_cast<std::size_t>(l)].assign(
        u.stack.layers[static_cast<std::size_t>(l)].dir == LayerDir::Horizontal
            ? static_cast<std::size_t>(nx + 1) * ny
            : static_cast<std::size_t>(ny + 1) * nx,
        0);
  }
  const int ncut = std::max(0, L - 1);
  u.vias.assign(static_cast<std::size_t>(ncut), std::vector<std::uint32_t>(ng, 0));
  std::vector<std::int64_t> wl(static_cast<std::size_t>(L), 0);
  std::vector<std::uint64_t> vcount(static_cast<std::size_t>(ncut), 0);
  std::atomic<std::size_t> nseg{0};
  const Dbu bw = g.bin_width(), bh = g.bin_height();

  auto tier = [](int n) {
    int t = 0;
    for (int m = n + 1; m >= 4; m /= 4) ++t;
    return t;
  };
  auto clampx = [&](Dbu x) { return std::clamp(x, reg.xlo, reg.xhi); };
  auto clampy = [&](Dbu y) { return std::clamp(y, reg.ylo, reg.yhi); };

  par::parallel_for(0, d.num_nets(), 1024, [&](std::size_t lo, std::size_t hi) {
    std::vector<std::int64_t> lwl(static_cast<std::size_t>(L), 0);
    std::vector<std::uint64_t> lvc(static_cast<std::size_t>(ncut), 0);
    std::size_t lseg = 0;
    auto add_vias = [&](int ix, int iy, int from, int to) {
      if (from > to) std::swap(from, to);
      for (int c = from; c < to; ++c) {
        atomic_add(u.vias[static_cast<std::size_t>(c)][g.index(ix, iy)], 1u);
        ++lvc[static_cast<std::size_t>(c)];
      }
    };
    auto h_run = [&](int layer, int iy, Dbu x0, Dbu x1) {
      if (x0 > x1) std::swap(x0, x1);
      const int ix0 = g.bin_of({x0, reg.ylo}).first, ix1 = g.bin_of({x1, reg.ylo}).first;
      auto& c = cov[static_cast<std::size_t>(layer)];
      const std::size_t row = static_cast<std::size_t>(iy) * nx;
      if (ix0 == ix1) {
        atomic_add(c[row + ix0], x1 - x0);
      } else {
        atomic_add(c[row + ix0], g.bin_rect(ix0, iy).xhi - x0);
        atomic_add(c[row + ix1], x1 - g.bin_rect(ix1, iy).xlo);
        if (ix1 > ix0 + 1) {
          auto& df = diff[static_cast<std::size_t>(layer)];
          const std::size_t base = static_cast<std::size_t>(iy) * (nx + 1);
          atomic_add(df[base + ix0 + 1], bw);
          atomic_add(df[base + ix1], -bw);
        }
      }
      lwl[static_cast<std::size_t>(layer)] += x1 - x0;
    };
    auto v_run = [&](int layer, int ix, Dbu y0, Dbu y1) {
      if (y0 > y1) std::swap(y0, y1);
      const int iy0 = g.bin_of({reg.xlo, y0}).second, iy1 = g.bin_of({reg.xlo, y1}).second;
      auto& c = cov[static_cast<std::size_t>(layer)];
      if (iy0 == iy1) {
        atomic_add(c[g.index(ix, iy0)], y1 - y0);
      } else {
        atomic_add(c[g.index(ix, iy0)], g.bin_rect(ix, iy0).yhi - y0);
        atomic_add(c[g.index(ix, iy1)], y1 - g.bin_rect(ix, iy1).ylo);
        if (iy1 > iy0 + 1) {
          auto& df = diff[static_cast<std::size_t>(layer)];
          const std::size_t base = static_cast<std::size_t>(ix) * (ny + 1);
          atomic_add(df[base + iy0 + 1], bh);
          atomic_add(df[base + iy1], -bh);
        }
      }
      lwl[static_cast<std::size_t>(layer)] += y1 - y0;
    };

    for (std::size_t n = lo; n < hi; ++n) {
      const Id net = static_cast<Id>(n);
      if (d.net(net).removed) continue;
      const auto pins = d.net_pins(net);
      if (pins.size() < 2 || hl.empty()) continue;
      Point a = pin_location(d, pins.front());
      a = {clampx(a.x), clampy(a.y)};
      const auto [ax, ay] = g.bin_of(a);
      int src_top = 0;
      for (std::size_t k = 1; k < pins.size(); ++k) {
        Point b = pin_location(d, pins[k]);
        b = {clampx(b.x), clampy(b.y)};
        const auto [bx, by] = g.bin_of(b);
        int lh = -1, lv = -1;
        if (a.x != b.x) {
          lh = hl[static_cast<std::size_t>(std::min<int>(tier(std::abs(bx - ax)), static_cast<int>(hl.size()) - 1))];
          h_run(lh, ay, a.x, b.x);
        }
        if (a.y != b.y) {
          lv = vl[static_cast<std::size_t>(std::min<int>(tier(std::abs(by - ay)), static_cast<int>(vl.size()) - 1))];
          v_run(lv, bx, a.y, b.y);
        }
        if (lh < 0 && lv < 0) continue;
        ++lseg;
        src_top = std::max(src_top, lh >= 0 ? lh : lv);
        add_vias(bx, by, 0, lv >= 0 ? lv : lh);
        if (lh >= 0 && lv >= 0) add_vias(bx, ay, lh, lv);
      }
      add_vias(ax, ay, 0, src_top);
    }
    for (int l = 0; l < L; ++l)
      if (lwl[static_cast<std::size_t>(l)]) atomic_add(wl[static_cast<std::size_t>(l)], lwl[static_cast<std::size_t>(l)]);
    for (int c = 0; c < ncut; ++c)
      if (lvc[static_cast<std::size_t>(c)]) atomic_add(vcount[static_cast<std::size_t>(c)], lvc[static_cast<std::size_t>(c)]);
    nseg.fetch_add(lseg, std::memory_order_relaxed);
  });

  // Integrate the difference arrays and convert covered length to tracks.
  u.demand.assign(static_cast<std::size_t>(L), std::vector<float>(ng, 0.0f));
  par::parallel_for_each(0, static_cast<std::size_t>(L), [&](std::size_t l) {
    if (cov[l].empty()) return;
    auto& c = cov[l];
    const auto& df = diff[l];
    const bool horiz = u.stack.layers[l].dir == LayerDir::Horizontal;
    if (horiz) {
      for (int iy = 0; iy < ny; ++iy) {
        std::int64_t run = 0;
        for (int ix = 0; ix < nx; ++ix) {
          run += df[static_cast<std::size_t>(iy) * (nx + 1) + ix];
          c[g.index(ix, iy)] += run;
        }
      }
    } else {
      for (int ix = 0; ix < nx; ++ix) {
        std::int64_t run = 0;
        for (int iy = 0; iy < ny; ++iy) {
          run += df[static_cast<std::size_t>(ix) * (ny + 1) + iy];
          c[g.index(ix, iy)] += run;
        }
      }
    }
    for (int iy = 0; iy < ny; ++iy)
      for (int ix = 0; ix < nx; ++ix) {
        const Rect br = g.bin_rect(ix, iy);
        const double len = static_cast<double>(horiz ? br.width() : br.height());
        u.demand[l][g.index(ix, iy)] = static_cast<float>(static_cast<double>(c[g.index(ix, iy)]) / len);
      }
    std::vector<std::int64_t>().swap(c);
  });
  u.wirelength.assign(wl.begin(), wl.end());
  u.via_count = std::move(vcount);
  u.num_segments = nseg.load();
  HLOG_DEBUG(log_usage, "3-D usage: {}x{} GCells, {} layers ({}), {} segments, wirelength {:.6g}, {} vias", nx, ny,
             L, u.stack.source, u.num_segments, u.total_wirelength(), u.total_vias());
  return u;
}

}  // namespace hermes
