#include "hermes/qor/qor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>

#include "hermes/core/json.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"

namespace hermes {

namespace {

HERMES_LOG_CHANNEL(log_qor, "qor");

using Clock = std::chrono::steady_clock;
double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

template <class T>
void atomic_add(T& slot, T v) {
  std::atomic_ref<T>(slot).fetch_add(v, std::memory_order_relaxed);
}

// Clipped overlap area of every cell with the bins it touches, added into
// `mov` (movable cells) or `fix` (fixed cells). Integer atomics: exact and
// independent of the thread count.
void deposit_cells(const Design& d, const BinGrid& g, std::vector<std::int64_t>& mov,
                   std::vector<std::int64_t>& fix, std::atomic<std::int64_t>& mov_total) {
  const Rect die = g.region();
  const auto cells = d.cells();
  par::parallel_for(0, cells.size(), 4096, [&](std::size_t lo, std::size_t hi) {
    std::int64_t local = 0;
    for (std::size_t i = lo; i < hi; ++i) {
      const Cell& c = cells[i];
      if (c.removed || c.width <= 0 || c.height <= 0) continue;
      const Rect bb = c.bbox();
      const Rect r{std::max(bb.xlo, die.xlo), std::max(bb.ylo, die.ylo), std::min(bb.xhi, die.xhi),
                   std::min(bb.yhi, die.yhi)};
      if (c.movable()) local += bb.area();
      if (r.xhi <= r.xlo || r.yhi <= r.ylo) continue;
      auto& dst = c.movable() ? mov : fix;
      const auto [x0, y0] = g.bin_of({r.xlo, r.ylo});
      const auto [x1, y1] = g.bin_of({r.xhi - 1, r.yhi - 1});
      for (int iy = y0; iy <= y1; ++iy) {
        for (int ix = x0; ix <= x1; ++ix) {
          const std::int64_t a = g.bin_rect(ix, iy).overlap_area(r);
          if (a > 0) atomic_add(dst[g.index(ix, iy)], a);
        }
      }
    }
    mov_total.fetch_add(local, std::memory_order_relaxed);
  });
}

// Average of the top `frac` of `v` (descending); v is reordered.
double top_average(std::vector<float>& v, double frac) {
  if (v.empty()) return 0.0;
  const std::size_t k = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(frac * static_cast<double>(v.size()))));
  double s = 0.0;
  for (std::size_t i = 0; i < k; ++i) s += v[i];
  return s / static_cast<double>(k);
}

void ace_of(std::vector<float> v, double out[4]) {
  if (v.empty()) return;
  const std::size_t k = std::min(v.size(), static_cast<std::size_t>(std::ceil(0.05 * static_cast<double>(v.size()))) + 1);
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k - 1), v.end(), std::greater<float>());
  std::sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), std::greater<float>());
  const double fr[4] = {0.005, 0.01, 0.02, 0.05};
  for (int i = 0; i < 4; ++i) out[i] = top_average(v, fr[i]);
}

// RUDY per direction on grid g: H and V wire length (DBU) per GCell.
//
// A net spreads density w/area (H) and h/area (V) uniformly over its
// bounding box, so its share of GCell (ix, iy) is density * ox(ix) * oy(iy)
// with ox/oy the overlap width/height: an outer product. ox is the full GCell
// width on interior columns plus corrections at the two end columns (same
// for oy), so the product splits into a constant rectangle (2-D difference
// array, 4 corners), end-row and end-column segments (1-D difference
// arrays) and corner points: O(1) per net, whatever its size. Fixed-point
// (1/256 DBU) integer atomics keep it exact and independent of the thread
// count.
}  // namespace

void rudy_by_direction(const Design& d, const BinGrid& g, std::vector<double>& h_len, std::vector<double>& v_len) {
  const int nx = g.nx(), ny = g.ny();
  const std::size_t W = static_cast<std::size_t>(nx) + 1, ng = g.size();
  // Per direction: d2 ((nx+1)*(ny+1)), drow ((nx+1)*ny, along x), dcol (nx*(ny+1), along y), pt (nx*ny).
  struct Acc {
    std::vector<std::int64_t> d2, drow, dcol, pt;
  };
  Acc acc[2];
  for (Acc& a : acc) {
    a.d2.assign(W * (ny + 1), 0);
    a.drow.assign(W * ny, 0);
    a.dcol.assign(static_cast<std::size_t>(nx) * (ny + 1), 0);
    a.pt.assign(ng, 0);
  }
  constexpr double kFix = 256.0;
  const double bw = static_cast<double>(g.bin_width()), bh = static_cast<double>(g.bin_height());
  // Coarse grids: each thread block fills private arrays with plain adds
  // (atomics on a few thousand shared bins contend); fine grids: shared
  // arrays with atomics. Integer sums, so both give the same result.
  const bool private_acc = ng <= (std::size_t{1} << 16);
  auto deposit = [&](std::size_t lo, std::size_t hi, Acc* A, bool atomic) {
    auto add = [atomic](std::int64_t& slot, std::int64_t v) {
      if (atomic) atomic_add(slot, v); else slot += v;
    };
    for (std::size_t n = lo; n < hi; ++n) {
      const Id net = static_cast<Id>(n);
      if (d.net(net).removed || d.net_pins(net).size() < 2) continue;
      Rect bb = net_bbox(d, net);
      bb.xhi = std::max(bb.xhi, bb.xlo + 1);
      bb.yhi = std::max(bb.yhi, bb.ylo + 1);
      const double area = static_cast<double>(bb.area());
      const double dens[2] = {static_cast<double>(bb.width()) / area, static_cast<double>(bb.height()) / area};
      const auto [x0, y0] = g.bin_of({bb.xlo, bb.ylo});
      const auto [x1, y1] = g.bin_of({bb.xhi, bb.yhi});
      // Overlap of the box with the end columns/rows.
      const Rect c0 = g.bin_rect(x0, y0), c1 = g.bin_rect(x1, y1);
      const double ox0 = static_cast<double>(std::min(bb.xhi, c0.xhi) - std::max(bb.xlo, c0.xlo));
      const double ox1 = static_cast<double>(std::min(bb.xhi, c1.xhi) - std::max(bb.xlo, c1.xlo));
      const double oy0 = static_cast<double>(std::min(bb.yhi, c0.yhi) - std::max(bb.ylo, c0.ylo));
      const double oy1 = static_cast<double>(std::min(bb.yhi, c1.yhi) - std::max(bb.ylo, c1.ylo));
      // ox(ix) = bw on [x0, x1] + ex0 at x0 + ex1 at x1 (one end term when x0 == x1).
      double ex0, ex1 = 0.0, ey0, ey1 = 0.0;
      if (x0 == x1) {
        ex0 = static_cast<double>(std::min(bb.xhi, c0.xhi) - std::max(bb.xlo, c0.xlo)) - bw;
      } else {
        ex0 = ox0 - bw;
        ex1 = ox1 - bw;
      }
      if (y0 == y1) {
        ey0 = static_cast<double>(std::min(bb.yhi, c0.yhi) - std::max(bb.ylo, c0.ylo)) - bh;
      } else {
        ey0 = oy0 - bh;
        ey1 = oy1 - bh;
      }
      for (int k = 0; k < 2; ++k) {
        Acc& a = A[k];
        const double dn = dens[k] * kFix;
        auto q = [](double v) { return static_cast<std::int64_t>(std::llround(v)); };
        // bw*bh on the rectangle.
        const std::int64_t r = q(dn * bw * bh);
        if (r) {
          add(a.d2[static_cast<std::size_t>(y0) * W + x0], r);
          add(a.d2[static_cast<std::size_t>(y0) * W + x1 + 1], -r);
          add(a.d2[static_cast<std::size_t>(y1 + 1) * W + x0], -r);
          add(a.d2[static_cast<std::size_t>(y1 + 1) * W + x1 + 1], r);
        }
        // ey * bw along rows y0 / y1 over [x0, x1].
        auto row = [&](int iy, double ey) {
          const std::int64_t v = q(dn * bw * ey);
          if (!v) return;
          add(a.drow[static_cast<std::size_t>(iy) * W + x0], v);
          add(a.drow[static_cast<std::size_t>(iy) * W + x1 + 1], -v);
        };
        row(y0, ey0);
        if (y1 != y0) row(y1, ey1);
        // ex * bh along columns x0 / x1 over [y0, y1].
        auto col = [&](int ix, double ex) {
          const std::int64_t v = q(dn * bh * ex);
          if (!v) return;
          add(a.dcol[static_cast<std::size_t>(ix) * (ny + 1) + y0], v);
          add(a.dcol[static_cast<std::size_t>(ix) * (ny + 1) + y1 + 1], -v);
        };
        col(x0, ex0);
        if (x1 != x0) col(x1, ex1);
        // ex * ey at the corners.
        auto pt = [&](int ix, int iy, double v) {
          const std::int64_t qv = q(dn * v);
          if (qv) add(a.pt[g.index(ix, iy)], qv);
        };
        pt(x0, y0, ex0 * ey0);
        if (x1 != x0) pt(x1, y0, ex1 * ey0);
        if (y1 != y0) pt(x0, y1, ex0 * ey1);
        if (x1 != x0 && y1 != y0) pt(x1, y1, ex1 * ey1);
      }
    }
  };
  if (private_acc) {
    const std::size_t nb = static_cast<std::size_t>(std::max(1, par::num_threads()));
    const std::size_t nn = d.num_nets(), bs = (nn + nb - 1) / nb;
    std::vector<std::array<Acc, 2>> part(nb);
    par::parallel_for_each(0, nb, [&](std::size_t b) {
      for (Acc& a : part[b]) {
        a.d2.assign(acc[0].d2.size(), 0);
        a.drow.assign(acc[0].drow.size(), 0);
        a.dcol.assign(acc[0].dcol.size(), 0);
        a.pt.assign(ng, 0);
      }
      deposit(std::min(nn, b * bs), std::min(nn, (b + 1) * bs), part[b].data(), false);
    });
    par::parallel_for_each(0, 2, [&](std::size_t k) {
      for (std::size_t b = 0; b < nb; ++b) {
        auto sum = [](std::vector<std::int64_t>& dst, const std::vector<std::int64_t>& src) {
          for (std::size_t i = 0; i < dst.size(); ++i) dst[i] += src[i];
        };
        sum(acc[k].d2, part[b][k].d2);
        sum(acc[k].drow, part[b][k].drow);
        sum(acc[k].dcol, part[b][k].dcol);
        sum(acc[k].pt, part[b][k].pt);
      }
    });
  } else {
    par::parallel_for(0, d.num_nets(), 2048, [&](std::size_t lo, std::size_t hi) { deposit(lo, hi, acc, true); });
  }
  h_len.assign(ng, 0.0);
  v_len.assign(ng, 0.0);
  par::parallel_for_each(0, 2, [&](std::size_t k) {
    Acc& a = acc[k];
    std::vector<double>& out = k == 0 ? h_len : v_len;
    // 2-D prefix sum of d2 (rows, then columns), 1-D sums of drow / dcol.
    for (int iy = 0; iy <= ny; ++iy) {
      std::int64_t run = 0;
      for (int ix = 0; ix <= nx; ++ix) {
        run += a.d2[static_cast<std::size_t>(iy) * W + ix];
        a.d2[static_cast<std::size_t>(iy) * W + ix] = run;
      }
    }
    for (int ix = 0; ix < nx; ++ix) {
      std::int64_t run = 0, rc = 0;
      for (int iy = 0; iy < ny; ++iy) {
        run += a.d2[static_cast<std::size_t>(iy) * W + ix];
        rc += a.dcol[static_cast<std::size_t>(ix) * (ny + 1) + iy];
        a.pt[g.index(ix, iy)] += run + rc;
      }
    }
    for (int iy = 0; iy < ny; ++iy) {
      std::int64_t run = 0;
      for (int ix = 0; ix < nx; ++ix) {
        run += a.drow[static_cast<std::size_t>(iy) * W + ix];
        out[g.index(ix, iy)] = static_cast<double>(a.pt[g.index(ix, iy)] + run) / kFix;
      }
    }
  });
}

namespace {

QorMap make_map(std::string name, std::string unit, const BinGrid& g) {
  QorMap m;
  m.name = std::move(name);
  m.unit = std::move(unit);
  m.nx = g.nx();
  m.ny = g.ny();
  m.region = g.region();
  m.values.assign(g.size(), 0.0f);
  return m;
}

// GCells without capacity (fully blocked) count in overflow but not in
// utilization maxima or ACE, where they would be infinite.
double util_of(double dem, double cap) { return cap > 0.0 ? dem / cap : 0.0; }

}  // namespace

QorMap QorMap::downsample(int max_side, bool use_max) const {
  if (nx <= max_side && ny <= max_side) return *this;
  const int f = std::max((nx + max_side - 1) / max_side, (ny + max_side - 1) / max_side);
  QorMap m;
  m.name = name;
  m.unit = unit;
  m.region = region;
  m.nx = (nx + f - 1) / f;
  m.ny = (ny + f - 1) / f;
  m.values.assign(static_cast<std::size_t>(m.nx) * m.ny, 0.0f);
  for (int y = 0; y < m.ny; ++y)
    for (int x = 0; x < m.nx; ++x) {
      double acc = use_max ? -1e30 : 0.0;
      int cnt = 0;
      for (int iy = y * f; iy < std::min(ny, (y + 1) * f); ++iy)
        for (int ix = x * f; ix < std::min(nx, (x + 1) * f); ++ix) {
          const double v = at(ix, iy);
          acc = use_max ? std::max(acc, v) : acc + v;
          ++cnt;
        }
      m.values[static_cast<std::size_t>(y) * m.nx + x] =
          static_cast<float>(use_max ? acc : (cnt ? acc / cnt : 0.0));
    }
  return m;
}

const QorMap* QorReport::map(std::string_view n) const {
  for (const QorMap& m : maps)
    if (m.name == n) return &m;
  return nullptr;
}

DensityQor compute_density_qor(const Design& d, const QorOptions& o, QorMap* util_map, QorMap* overflow_map) {
  DensityQor q;
  const Rect die = d.die();
  q.target = o.target_density;
  if (die.width() <= 0 || die.height() <= 0) return q;
  Dbu side = d.row_height() > 0 ? d.row_height() * std::max(1, o.bin_rows)
                                : std::max<Dbu>(1, std::max(die.width(), die.height()) / 256);
  const int cap = std::max(1, o.max_bins_per_side);
  q.nx = static_cast<int>(std::clamp<Dbu>((die.width() + side - 1) / side, 1, cap));
  q.ny = static_cast<int>(std::clamp<Dbu>((die.height() + side - 1) / side, 1, cap));
  const BinGrid g(die, q.nx, q.ny);
  q.bin_w = g.bin_width();
  q.bin_h = g.bin_height();
  const std::size_t ng = g.size();
  std::vector<std::int64_t> mov(ng, 0), fix(ng, 0);
  std::atomic<std::int64_t> mov_total{0};
  deposit_cells(d, g, mov, fix, mov_total);
  q.movable_area = static_cast<double>(mov_total.load());

  if (util_map) *util_map = make_map("density.util", "ratio", g);
  if (overflow_map) *overflow_map = make_map("density.overflow", "ratio", g);
  double sum = 0.0, sum2 = 0.0, fixed = 0.0, free_total = 0.0, of = 0.0, mov_in = 0.0;
  std::size_t counted = 0, over_t = 0, over_1 = 0;
  double peak = 0.0;
  for (int iy = 0; iy < q.ny; ++iy)
    for (int ix = 0; ix < q.nx; ++ix) {
      const std::size_t k = g.index(ix, iy);
      const double ba = static_cast<double>(g.bin_rect(ix, iy).area());
      const double fx = std::min(ba, static_cast<double>(fix[k]));
      const double fr = ba - fx;
      const double m = static_cast<double>(mov[k]);
      fixed += fx;
      free_total += fr;
      mov_in += m;
      const double o_bin = std::max(0.0, m - o.target_density * fr);
      of += o_bin;
      const double u = fr > 0.0 ? m / fr : 0.0;
      if (util_map) util_map->values[k] = static_cast<float>(u);
      if (overflow_map) overflow_map->values[k] = static_cast<float>(ba > 0 ? o_bin / ba : 0.0);
      if (fr >= o.min_free_fraction * ba && ba > 0) {
        ++counted;
        sum += u;
        sum2 += u * u;
        peak = std::max(peak, u);
        over_t += u > o.target_density;
        over_1 += u > 1.0;
      }
    }
  q.fixed_area = fixed;
  q.free_area = free_total;
  q.avg_util = free_total > 0 ? mov_in / free_total : 0.0;
  q.peak_util = peak;
  q.overflow_area = of;
  q.overflow = q.movable_area > 0 ? of / q.movable_area : 0.0;
  q.bins_counted = counted;
  if (counted) {
    q.util_mean = sum / static_cast<double>(counted);
    q.util_std = std::sqrt(std::max(0.0, sum2 / static_cast<double>(counted) - q.util_mean * q.util_mean));
    q.cv = q.util_mean > 0 ? q.util_std / q.util_mean : 0.0;
    q.overfull_share = static_cast<double>(over_t) / static_cast<double>(counted);
    q.over100_share = static_cast<double>(over_1) / static_cast<double>(counted);
  }
  return q;
}

QorReport compute_qor(const Design& d, const QorOptions& o, const RouteUsage3D* usage_in, int gx, int gy) {
  const auto t_all = Clock::now();
  QorReport q;
  q.design = d.name();
  q.dbu_per_micron = d.dbu_per_micron();
  q.cells = d.num_cells();
  q.nets = d.num_nets();
  for (const Cell& c : d.cells()) {
    if (c.removed) continue;
    q.macros += c.is_macro;
    q.movable += c.movable();
  }

  // Density + evenness.
  auto t0 = Clock::now();
  {
    QorMap um, om;
    q.density = compute_density_qor(d, o, o.maps ? &um : nullptr, o.maps ? &om : nullptr);
    if (o.maps) {
      q.maps.push_back(std::move(um));
      q.maps.push_back(std::move(om));
    }
  }
  q.seconds.emplace_back("density", since(t0));

  // 3-D usage.
  t0 = Clock::now();
  RouteUsage3D built;
  const RouteUsage3D* u = usage_in;
  if (!u) {
    RouteUsageOptions uo;
    uo.grid_x = gx;
    uo.grid_y = gy;
    built = build_route_usage(d, uo);
    u = &built;
    q.seconds.emplace_back("route usage", since(t0));
    t0 = Clock::now();
  }
  CongestionQor& c = q.congestion;
  const BinGrid& g = u->grid;
  const std::size_t ng = g.size();
  const int L = u->num_layers();
  c.nx = g.nx();
  c.ny = g.ny();
  c.gcell_w = g.bin_width();
  c.gcell_h = g.bin_height();
  c.layer_source = u->stack.source;
  c.usage_source = u->source;
  c.routed = usage_in != nullptr;
  c.model = "3-D GCell track model: per layer, demand = wire length in GCell / GCell length, "
            "capacity = tracks crossing the GCell (extent / pitch), reduced under blockages; demand on fully "
            "blocked GCells counts as overflow but not in utilization";
  std::vector<double> hd(ng, 0.0), hc(ng, 0.0), vd(ng, 0.0), vc(ng, 0.0), of(ng, 0.0);
  std::vector<float> best_u(ng, 0.0f);
  std::vector<int> best_l(ng, -1);
  for (int l = 0; l < L; ++l) {
    const LayerInfo& li = u->stack.layers[static_cast<std::size_t>(l)];
    LayerQor lq;
    lq.name = li.name;
    lq.dir = li.dir;
    lq.routable = li.routable;
    lq.wirelength = static_cast<std::size_t>(l) < u->wirelength.size() ? u->wirelength[static_cast<std::size_t>(l)] : 0.0;
    const auto& dem = u->demand[static_cast<std::size_t>(l)];
    const auto& cap = u->capacity[static_cast<std::size_t>(l)];
    QorMap lm;
    if (o.maps && li.routable) lm = make_map("congestion." + li.name + ".util", "ratio", g);
    const bool h = li.dir == LayerDir::Horizontal;
    for (std::size_t k = 0; k < ng; ++k) {
      const double dd = dem[k], cc = cap[k];
      lq.capacity += cc;
      lq.demand += dd;
      const double ov = std::max(0.0, dd - cc);
      lq.overflow += ov;
      lq.overflow_gcells += ov > 1e-9;
      of[k] += ov;
      const double ut = util_of(dd, cc);
      lq.max_util = std::max(lq.max_util, ut);
      if (ut > best_u[k]) {
        best_u[k] = static_cast<float>(ut);
        best_l[k] = l;
      }
      if (!lm.values.empty()) lm.values[k] = static_cast<float>(ut);
      (h ? hd : vd)[k] += dd;
      (h ? hc : vc)[k] += cc;
    }
    lq.avg_util = lq.capacity > 0 ? lq.demand / lq.capacity : 0.0;
    c.total_overflow += lq.overflow;
    (h ? c.h_overflow : c.v_overflow) += lq.overflow;
    c.max_util = std::max(c.max_util, lq.max_util);
    c.layers.push_back(std::move(lq));
    if (!lm.values.empty()) q.maps.push_back(std::move(lm));
  }
  QorMap mh, mv, mo;
  if (o.maps) {
    mh = make_map("congestion.h.util", "ratio", g);
    mv = make_map("congestion.v.util", "ratio", g);
    mo = make_map("congestion.overflow", "tracks", g);
  }
  std::vector<float> ace_vals;
  ace_vals.reserve(2 * ng);
  for (std::size_t k = 0; k < ng; ++k) {
    const double uh = util_of(hd[k], hc[k]), uv = util_of(vd[k], vc[k]);
    c.h_max_util = std::max(c.h_max_util, uh);
    c.v_max_util = std::max(c.v_max_util, uv);
    c.overflow_gcells += of[k] > 1e-9;
    if (hc[k] > 0) ace_vals.push_back(static_cast<float>(uh));
    if (vc[k] > 0) ace_vals.push_back(static_cast<float>(uv));
    if (o.maps) {
      mh.values[k] = static_cast<float>(uh);
      mv.values[k] = static_cast<float>(uv);
      mo.values[k] = static_cast<float>(of[k]);
    }
  }
  ace_of(ace_vals, c.ace);
  c.rc = (c.ace[0] + c.ace[1] + c.ace[2] + c.ace[3]) / 4.0;
  {
    // Hot spots: worst GCells by their worst layer's utilization.
    std::vector<std::uint32_t> idx(ng);
    for (std::size_t k = 0; k < ng; ++k) idx[k] = static_cast<std::uint32_t>(k);
    const std::size_t k = std::min<std::size_t>(ng, static_cast<std::size_t>(std::max(0, o.worst_spots)));
    auto worse = [&](std::uint32_t i) { return static_cast<double>(best_u[i]); };
    std::partial_sort(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(k), idx.end(),
                      [&](std::uint32_t a, std::uint32_t b) {
                        const double wa = worse(a), wb = worse(b);
                        return wa != wb ? wa > wb : a < b;
                      });
    for (std::size_t i = 0; i < k; ++i) {
      HotSpot s;
      s.ix = static_cast<int>(idx[i] % static_cast<std::uint32_t>(g.nx()));
      s.iy = static_cast<int>(idx[i] / static_cast<std::uint32_t>(g.nx()));
      s.box = g.bin_rect(s.ix, s.iy);
      s.util = worse(idx[i]);
      s.overflow = of[idx[i]];
      if (best_l[idx[i]] >= 0) s.worst_layer = u->stack.layers[static_cast<std::size_t>(best_l[idx[i]])].name;
      if (s.util <= 0.0) break;
      c.hot_spots.push_back(s);
    }
  }
  q.seconds.emplace_back("congestion", since(t0));

  // RUDY on the same GCells and capacities.
  t0 = Clock::now();
  {
    std::vector<double> rh, rv;
    rudy_by_direction(d, g, rh, rv);
    std::vector<float> vals;
    vals.reserve(2 * ng);
    QorMap rm;
    if (o.maps) rm = make_map("rudy.util", "ratio", g);
    for (int iy = 0; iy < g.ny(); ++iy)
      for (int ix = 0; ix < g.nx(); ++ix) {
        const std::size_t k = g.index(ix, iy);
        const Rect br = g.bin_rect(ix, iy);
        const double th = rh[k] / static_cast<double>(br.width());   // tracks
        const double tv = rv[k] / static_cast<double>(br.height());
        c.rudy_h_overflow += std::max(0.0, th - hc[k]);
        c.rudy_v_overflow += std::max(0.0, tv - vc[k]);
        const double uh = util_of(th, hc[k]), uv = util_of(tv, vc[k]);
        c.rudy_max_util = std::max({c.rudy_max_util, uh, uv});
        if (hc[k] > 0) vals.push_back(static_cast<float>(uh));
        if (vc[k] > 0) vals.push_back(static_cast<float>(uv));
        if (o.maps) rm.values[k] = static_cast<float>(std::max(uh, uv));
      }
    ace_of(std::move(vals), c.rudy_ace);
    if (o.maps) {
      q.maps.push_back(std::move(mh));
      q.maps.push_back(std::move(mv));
      q.maps.push_back(std::move(mo));
      q.maps.push_back(std::move(rm));
    }
  }
  q.seconds.emplace_back("rudy", since(t0));

  // Wirelength.
  t0 = Clock::now();
  WirelengthQor& w = q.wirelength;
  w.hpwl = total_hpwl(d);
  w.weighted_hpwl = total_hpwl(d, true);
  w.layer_wl = u->wirelength;
  w.routed = u->total_wirelength();
  w.vias = u->via_count;
  w.total_vias = u->total_vias();
  w.segments = u->num_segments;
  w.layers_estimated = u->layers_estimated;
  w.pins = d.num_pins();
  for (const Net& n : d.nets()) w.nets += !n.removed;
  if (o.maps && !u->vias.empty()) {
    QorMap vm = make_map("vias.total", "count", g);
    for (const auto& cut : u->vias)
      for (std::size_t k = 0; k < ng; ++k) vm.values[k] += static_cast<float>(cut[k]);
    q.maps.push_back(std::move(vm));
  }
  q.seconds.emplace_back("wirelength", since(t0));

  // Notes: what is measured and what is estimated.
  if (u->layers_estimated) {
    q.notes.push_back(
        "Per-layer wirelength, per-layer congestion and via counts are ESTIMATES: no routed layer assignment "
        "is available (no global route in this run, or layer assignment off), so a greedy length-based layer "
        "assignment of star + L-shape routes is used for this report.");
  }
  if (!c.routed) q.notes.push_back("No routing in this run: congestion uses star + L-shape routes computed for this report.");
  if (u->stack.source.rfind("default", 0) == 0) {
    q.notes.push_back(std::format(
        "The input has no routing-layer data (no technology LEF, no .route file): an assumed 6-layer "
        "stack is used (M1 pins only; pitch = row height / 9, x2 on M4-M5, x4 on M6). Capacities are "
        "therefore relative, not absolute."));
  }
  q.notes.push_back(std::format(
      "RUDY (the placer's estimate, HPWL-based) on the same GCells and capacities: H/V overflow "
      "{:.4g}/{:.4g} tracks, max utilization {:.3f}; routed: {:.4g}/{:.4g}, {:.3f}.",
      c.rudy_h_overflow, c.rudy_v_overflow, c.rudy_max_util, c.h_overflow, c.v_overflow,
      std::max(c.h_max_util, c.v_max_util)));
  q.total_seconds = since(t_all);
  HLOG_DEBUG(log_qor, "QoR computed in {:.3f} s", q.total_seconds);
  return q;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

namespace {

std::string len_str(const QorReport& q, double dbu) {
  if (q.dbu_per_micron > 0) return std::format("{:.6g} um", dbu / q.dbu_per_micron);
  return std::format("{:.6g} dbu", dbu);
}

std::string pct(double v) { return std::format("{:.2f}%", 100.0 * v); }

}  // namespace

std::string format_qor(const QorReport& q) {
  std::string s;
  auto line = [&](std::string_view t) {
    s += t;
    s += '\n';
  };
  line(std::format("==================== QoR report: {} ====================", q.design));
  line(std::format("{} cells ({} macros, {} movable), {} nets, {} pins", q.cells, q.macros, q.movable, q.wirelength.nets,
                   q.wirelength.pins));
  const DensityQor& dn = q.density;
  line("");
  line(std::format("Placement density   ({}x{} bins of {}x{} dbu, target {:.2f})", dn.nx, dn.ny, dn.bin_w, dn.bin_h,
                   dn.target));
  line(std::format("  average utilization      {:.3f}   (movable area / free area)", dn.avg_util));
  line(std::format("  peak bin utilization     {:.3f}", dn.peak_util));
  line(std::format("  density overflow         {:.4f}   (ISPD style: sum max(0, area - target*free) / movable area)",
                   dn.overflow));
  line("Evenness");
  line(std::format("  bin utilization CV       {:.3f}   (std {:.3f} / mean {:.3f}, {} bins)", dn.cv, dn.util_std,
                   dn.util_mean, dn.bins_counted));
  line(std::format("  overfull bins            {}  (> target), {} (> 100%)", pct(dn.overfull_share),
                   pct(dn.over100_share)));

  const CongestionQor& c = q.congestion;
  line("");
  line(std::format("Congestion (3-D)    ({}x{} GCells of {}x{} dbu, layers: {}{})", c.nx, c.ny, c.gcell_w, c.gcell_h,
                   c.layer_source, c.routed ? "" : ", not routed"));
  line(std::format("  {:<8} {:>3} {:>12} {:>12} {:>10} {:>8} {:>8} {:>9}  {:>14}", "layer", "dir", "capacity",
                   "demand", "overflow", "avg", "max", "of.gcells", "wirelength"));
  for (std::size_t l = 0; l < c.layers.size(); ++l) {
    const LayerQor& L = c.layers[l];
    if (!L.routable) {
      line(std::format("  {:<8} {:>3}   (pin access only)", L.name, L.dir == LayerDir::Horizontal ? "H" : "V"));
      continue;
    }
    line(std::format("  {:<8} {:>3} {:>12.6g} {:>12.6g} {:>10.4g} {:>8.3f} {:>8.3f} {:>9}  {:>14}", L.name,
                     L.dir == LayerDir::Horizontal ? "H" : "V", L.capacity, L.demand, L.overflow, L.avg_util,
                     L.max_util, L.overflow_gcells, len_str(q, L.wirelength)));
  }
  line(std::format("  total overflow {:.4g} tracks (H {:.4g}, V {:.4g}), {} GCells overflowed, max util {:.3f}",
                   c.total_overflow, c.h_overflow, c.v_overflow, c.overflow_gcells, c.max_util));
  line(std::format("  2-D max util H {:.3f} V {:.3f};  ACE 0.5/1/2/5%: {:.3f} {:.3f} {:.3f} {:.3f}  (RC {:.3f})",
                   c.h_max_util, c.v_max_util, c.ace[0], c.ace[1], c.ace[2], c.ace[3], c.rc));
  line(std::format("  RUDY estimate, same model: H/V overflow {:.4g}/{:.4g}, max util {:.3f}, ACE {:.3f} {:.3f} {:.3f} "
                   "{:.3f}",
                   c.rudy_h_overflow, c.rudy_v_overflow, c.rudy_max_util, c.rudy_ace[0], c.rudy_ace[1], c.rudy_ace[2],
                   c.rudy_ace[3]));
  if (!c.hot_spots.empty()) {
    line("  worst GCells (ix,iy  box  util  overflow  layer):");
    for (const HotSpot& h : c.hot_spots) {
      line(std::format("    ({:>4},{:>4})  ({},{})-({},{})  {:.3f}  {:.4g}  {}", h.ix, h.iy, h.box.xlo, h.box.ylo,
                       h.box.xhi, h.box.yhi, h.util, h.overflow, h.worst_layer));
    }
  }

  const WirelengthQor& w = q.wirelength;
  line("");
  line("Wirelength (3-D)");
  line(std::format("  HPWL                     {}  (weighted {})", len_str(q, w.hpwl), len_str(q, w.weighted_hpwl)));
  line(std::format("  routed (global)          {}  ({:.3f} x HPWL, {} two-pin segments)", len_str(q, w.routed),
                   w.hpwl > 0 ? w.routed / w.hpwl : 0.0, w.segments));
  std::string vias;
  for (std::size_t k = 0; k < w.vias.size(); ++k) {
    const std::string a = k < c.layers.size() ? c.layers[k].name : std::to_string(k);
    const std::string b = k + 1 < c.layers.size() ? c.layers[k + 1].name : std::to_string(k + 1);
    vias += std::format("{}{}-{} {}", vias.empty() ? "" : ", ", a, b, w.vias[k]);
  }
  line(std::format("  vias                     {}{}  ({})", w.total_vias, w.layers_estimated ? " (estimated)" : "",
                   vias));
  line("");
  line("Notes");
  for (const std::string& n : q.notes) line("  - " + n);
  std::string secs;
  for (const auto& [k, v] : q.seconds) secs += std::format("{}{} {:.3f}", secs.empty() ? "" : ", ", k, v);
  line(std::format("Report computed in {:.3f} s ({})", q.total_seconds, secs));
  return s;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

std::string qor_to_json(const QorReport& q) {
  JsonWriter j;
  j.begin_object();
  j.kv("design", q.design).kv("dbu_per_micron", q.dbu_per_micron);
  j.kv("cells", q.cells).kv("macros", q.macros).kv("movable", q.movable).kv("nets", q.nets);
  const DensityQor& d = q.density;
  j.key("density").begin_object();
  j.kv("nx", d.nx).kv("ny", d.ny).kv("bin_w", d.bin_w).kv("bin_h", d.bin_h).kv("target", d.target);
  j.kv("movable_area", d.movable_area).kv("fixed_area", d.fixed_area).kv("free_area", d.free_area);
  j.kv("avg_util", d.avg_util).kv("peak_util", d.peak_util).kv("overflow", d.overflow);
  j.kv("overflow_area", d.overflow_area).kv("bins_counted", d.bins_counted);
  j.end_object();
  j.key("evenness").begin_object();
  j.kv("cv", d.cv).kv("util_mean", d.util_mean).kv("util_std", d.util_std);
  j.kv("overfull_share", d.overfull_share).kv("over100_share", d.over100_share);
  j.end_object();
  const CongestionQor& c = q.congestion;
  j.key("congestion").begin_object();
  j.kv("nx", c.nx).kv("ny", c.ny).kv("gcell_w", c.gcell_w).kv("gcell_h", c.gcell_h);
  j.kv("layer_source", c.layer_source).kv("model", c.model).kv("usage_source", c.usage_source).kv("routed", c.routed);
  j.kv("total_overflow", c.total_overflow).kv("h_overflow", c.h_overflow).kv("v_overflow", c.v_overflow);
  j.kv("overflow_gcells", c.overflow_gcells).kv("max_util", c.max_util);
  j.kv("h_max_util", c.h_max_util).kv("v_max_util", c.v_max_util).kv("rc", c.rc);
  j.key("ace").begin_array();
  for (double a : c.ace) j.value(a);
  j.end_array();
  j.kv("rudy_h_overflow", c.rudy_h_overflow).kv("rudy_v_overflow", c.rudy_v_overflow).kv("rudy_max_util", c.rudy_max_util);
  j.key("rudy_ace").begin_array();
  for (double a : c.rudy_ace) j.value(a);
  j.end_array();
  j.key("layers").begin_array();
  for (const LayerQor& L : c.layers) {
    j.begin_object();
    j.kv("name", L.name).kv("dir", L.dir == LayerDir::Horizontal ? "H" : "V").kv("routable", L.routable);
    j.kv("capacity", L.capacity).kv("demand", L.demand).kv("overflow", L.overflow);
    j.kv("avg_util", L.avg_util).kv("max_util", L.max_util).kv("overflow_gcells", L.overflow_gcells);
    j.kv("wirelength", L.wirelength);
    j.end_object();
  }
  j.end_array();
  j.key("hot_spots").begin_array();
  for (const HotSpot& h : c.hot_spots) {
    j.begin_object();
    j.kv("ix", h.ix).kv("iy", h.iy).kv("util", h.util).kv("overflow", h.overflow).kv("layer", h.worst_layer);
    j.key("box").begin_array().value(h.box.xlo).value(h.box.ylo).value(h.box.xhi).value(h.box.yhi).end_array();
    j.end_object();
  }
  j.end_array();
  j.end_object();
  const WirelengthQor& w = q.wirelength;
  j.key("wirelength").begin_object();
  j.kv("hpwl", w.hpwl).kv("weighted_hpwl", w.weighted_hpwl).kv("routed", w.routed);
  j.kv("total_vias", w.total_vias).kv("segments", w.segments).kv("layers_estimated", w.layers_estimated);
  j.kv("nets", w.nets).kv("pins", w.pins);
  j.key("layer_wl").begin_array();
  for (double v : w.layer_wl) j.value(v);
  j.end_array();
  j.key("vias").begin_array();
  for (auto v : w.vias) j.value(v);
  j.end_array();
  j.end_object();
  j.key("notes").begin_array();
  for (const auto& n : q.notes) j.value(n);
  j.end_array();
  j.key("seconds").begin_object();
  for (const auto& [k, v] : q.seconds) j.kv(k, v);
  j.kv("total", q.total_seconds);
  j.end_object();
  j.end_object();
  return j.take();
}

std::string qor_maps_to_json(const QorReport& q, int max_side) {
  JsonWriter j;
  j.begin_array();
  for (const QorMap& m0 : q.maps) {
    const QorMap m = m0.downsample(max_side, true);
    j.begin_object();
    j.kv("name", m.name).kv("unit", m.unit).kv("nx", m.nx).kv("ny", m.ny);
    j.key("region").begin_array().value(m.region.xlo).value(m.region.ylo).value(m.region.xhi).value(m.region.yhi).end_array();
    j.key("values").begin_array();
    for (float v : m.values) j.value(std::round(static_cast<double>(v) * 1e4) / 1e4);
    j.end_array();
    j.end_object();
  }
  j.end_array();
  return j.take();
}

}  // namespace hermes
