#include "gui/scene/scene.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "hermes/par/parallel.hpp"

namespace hermes::gui {

namespace {

double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Calls f(row_lo, row_hi) on bands of rows in three phases (row mod 3), so
// tasks running together never touch neighboring bands. Used for scatters
// where an item may spill one band up or down: every bin then receives its
// contributions in a fixed order, independent of the thread count.
template <class F>
void three_phase_rows(int nrows, F&& f, bool parallel = true) {
  if (!parallel) {
    for (int r = 0; r < nrows; ++r) f(r);
    return;
  }
  for (int phase = 0; phase < 3; ++phase) {
    const int count = nrows > phase ? (nrows - phase + 2) / 3 : 0;
    par::parallel_for_each(0, static_cast<std::size_t>(count),
                           [&](std::size_t k) { f(static_cast<int>(k) * 3 + phase); });
  }
}

// Adds the area of world rect r (relative coordinates) into image `img`.
inline void splat_area(ScalarImage& img, double bw, double bh, double x0, double y0, double x1, double y1,
                       double scale) {
  const int w = img.w, h = img.h;
  int ix0 = static_cast<int>(x0 / bw), ix1 = static_cast<int>(x1 / bw);
  int iy0 = static_cast<int>(y0 / bh), iy1 = static_cast<int>(y1 / bh);
  ix0 = std::clamp(ix0, 0, w - 1);
  ix1 = std::clamp(ix1, 0, w - 1);
  iy0 = std::clamp(iy0, 0, h - 1);
  iy1 = std::clamp(iy1, 0, h - 1);
  for (int iy = iy0; iy <= iy1; ++iy) {
    const double oy = std::min(y1, (iy + 1) * bh) - std::max(y0, iy * bh);
    if (oy <= 0) continue;
    float* row = img.values.data() + static_cast<std::size_t>(iy) * w;
    for (int ix = ix0; ix <= ix1; ++ix) {
      const double ox = std::min(x1, (ix + 1) * bw) - std::max(x0, ix * bw);
      if (ox > 0) row[ix] += static_cast<float>(ox * oy * scale);
    }
  }
}

void finish_range(ScalarImage& img) {
  float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
  for (float v : img.values) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  if (img.values.empty()) lo = hi = 0;
  img.vmin = lo;
  img.vmax = hi;
}

}  // namespace

void Scene::tiles_for(const Rect& world, int& ix0, int& iy0, int& ix1, int& iy1) const {
  const double x0 = static_cast<double>(world.xlo - origin.x), x1 = static_cast<double>(world.xhi - origin.x);
  const double y0 = static_cast<double>(world.ylo - origin.y), y1 = static_cast<double>(world.yhi - origin.y);
  auto cl = [](double v, int n) { return static_cast<int>(std::clamp(v, 0.0, static_cast<double>(n - 1))); };
  ix0 = cl(std::floor(x0 / tile_w), tiles_x);
  ix1 = cl(std::floor(x1 / tile_w), tiles_x);
  iy0 = cl(std::floor(y0 / tile_h), tiles_y);
  iy1 = cl(std::floor(y1 / tile_h), tiles_y);
}

std::shared_ptr<Scene> build_scene(std::shared_ptr<const DesignSnapshot> snap, const SceneOptions& o) {
  const double t0 = now_ms();
  auto sc = std::make_shared<Scene>();
  Scene& s = *sc;
  s.snap = snap;
  s.version = snap->version();
  s.die = snap->die();
  s.origin = {s.die.xlo, s.die.ylo};
  const double dw = std::max<double>(1.0, static_cast<double>(s.die.width()));
  const double dh = std::max<double>(1.0, static_cast<double>(s.die.height()));

  const auto& arr = snap->cell_array();
  const auto order = snap->cell_order();
  const std::size_t n = arr.size();
  auto id_of = [&](std::size_t i) { return order.empty() ? static_cast<Id>(i) : order[i]; };

  // Pass 1 (parallel blocks): classify, stats.
  constexpr std::size_t kBlocks = 64;
  const std::size_t bs = (n + kBlocks - 1) / kBlocks;
  struct BlockStats {
    std::size_t std = 0, fixed = 0, macros = 0, unplaced = 0;
    double sum_w = 0, sum_h = 0;
    Dbu max_w = 0, max_h = 0;
  };
  std::vector<BlockStats> bst(kBlocks);
  par::parallel_for_each(0, kBlocks, [&](std::size_t b) {
    BlockStats& st = bst[b];
    const std::size_t e = std::min(n, (b + 1) * bs);
    for (std::size_t i = b * bs; i < e; ++i) {
      const Cell& c = arr[i];
      if (c.removed) continue;
      if (c.is_macro) {
        ++st.macros;
        continue;
      }
      ++st.std;
      st.fixed += c.fixed;
      st.unplaced += !c.placed;
      st.sum_w += static_cast<double>(c.width);
      st.sum_h += static_cast<double>(c.height);
      st.max_w = std::max(st.max_w, c.width);
      st.max_h = std::max(st.max_h, c.height);
    }
  });
  Dbu max_w = 0, max_h = 0;
  double sum_w = 0, sum_h = 0;
  for (const BlockStats& st : bst) {
    s.num_std += st.std;
    s.num_fixed_std += st.fixed;
    s.num_macros += st.macros;
    s.num_unplaced += st.unplaced;
    sum_w += st.sum_w;
    sum_h += st.sum_h;
    max_w = std::max(max_w, st.max_w);
    max_h = std::max(max_h, st.max_h);
  }
  s.max_cell_w = static_cast<float>(max_w);
  s.max_cell_h = static_cast<float>(max_h);
  s.avg_cell_w = s.num_std ? sum_w / static_cast<double>(s.num_std) : 0.0;
  s.avg_cell_h = s.num_std ? sum_h / static_cast<double>(s.num_std) : 0.0;

  // Tile grid: ~64+ cells per tile, tiles at least 2 cells tall/wide so a
  // cell spills at most into the neighboring tile row (three-phase scatter).
  int t = static_cast<int>(std::sqrt(static_cast<double>(s.num_std) / 64.0));
  t = std::clamp(t, 1, std::max(1, o.tiles_per_side));
  s.tiles_x = std::max(1, std::min(t, static_cast<int>(dw / std::max<double>(1.0, 2.0 * max_w))));
  s.tiles_y = std::max(1, std::min(t, static_cast<int>(dh / std::max<double>(1.0, 2.0 * max_h))));
  s.tile_w = dw / s.tiles_x;
  s.tile_h = dh / s.tiles_y;
  const std::size_t T = static_cast<std::size_t>(s.tiles_x) * s.tiles_y;

  auto tile_of = [&](const Cell& c) -> std::uint32_t {
    const double cx = static_cast<double>(c.loc.x - s.origin.x) + c.width * 0.5;
    const double cy = static_cast<double>(c.loc.y - s.origin.y) + c.height * 0.5;
    const int ix = std::clamp(static_cast<int>(cx / s.tile_w), 0, s.tiles_x - 1);
    const int iy = std::clamp(static_cast<int>(cy / s.tile_h), 0, s.tiles_y - 1);
    return static_cast<std::uint32_t>(iy * s.tiles_x + ix);
  };

  // Pass 2: per-block tile histograms -> offsets (tile-major, then block).
  std::vector<std::uint32_t> counts(kBlocks * T, 0);
  par::parallel_for_each(0, kBlocks, [&](std::size_t b) {
    std::uint32_t* cnt = counts.data() + b * T;
    const std::size_t e = std::min(n, (b + 1) * bs);
    for (std::size_t i = b * bs; i < e; ++i) {
      const Cell& c = arr[i];
      if (c.removed || c.is_macro) continue;
      ++cnt[tile_of(c)];
    }
  });
  s.tile_begin.assign(T + 1, 0);
  std::uint32_t total = 0;
  for (std::size_t tile = 0; tile < T; ++tile) {
    s.tile_begin[tile] = total;
    for (std::size_t b = 0; b < kBlocks; ++b) {
      const std::uint32_t c = counts[b * T + tile];
      counts[b * T + tile] = total;
      total += c;
    }
  }
  s.tile_begin[T] = total;
  s.cells.resize(total);
  s.cell_ids.resize(total);
  // Pass 3: scatter.
  par::parallel_for_each(0, kBlocks, [&](std::size_t b) {
    std::uint32_t* off = counts.data() + b * T;
    const std::size_t e = std::min(n, (b + 1) * bs);
    for (std::size_t i = b * bs; i < e; ++i) {
      const Cell& c = arr[i];
      if (c.removed || c.is_macro) continue;
      const std::uint32_t k = off[tile_of(c)]++;
      const float x0 = s.fx(c.loc.x), y0 = s.fy(c.loc.y);
      std::uint32_t flags = (c.fixed ? style::kFixed : 0u) | (c.placed ? 0u : style::kUnplaced);
      s.cells[k] = {x0, y0, x0 + static_cast<float>(c.width), y0 + static_cast<float>(c.height),
                    style::make(c.fixed ? LayerSet::kFixedCells : LayerSet::kStdCells, flags)};
      s.cell_ids[k] = id_of(i);
    }
  });

  // Macros (serial: few), I/O pins, rows.
  for (std::size_t i = 0; i < n; ++i) {
    const Cell& c = arr[i];
    if (c.removed || !c.is_macro) continue;
    const float x0 = s.fx(c.loc.x), y0 = s.fy(c.loc.y);
    s.macros.push_back({x0, y0, x0 + static_cast<float>(c.width), y0 + static_cast<float>(c.height),
                        style::make(LayerSet::kMacros, c.fixed ? style::kFixed : 0u)});
    s.macro_ids.push_back(id_of(i));
  }
  for (Id p : snap->io_pins()) {
    const Point q = snap->pin_location(p);
    s.io_pins.push_back({s.fx(q.x), s.fy(q.y), 4.0f, 0.0f, style::make(LayerSet::kIoPins, style::kMarker)});
    s.io_pin_ids.push_back(p);
  }
  for (const Row& r : snap->rows()) {
    const float x0 = s.fx(r.origin.x), y0 = s.fy(r.origin.y);
    const float x1 = x0 + static_cast<float>(r.site_step) * static_cast<float>(r.num_sites);
    s.rows.push_back({x0, y0, x1, y0, style::make(LayerSet::kRows)});
  }

  // Density image: std-cell area / bin area, aspect like the die.
  const double td = now_ms();
  {
    ScalarImage& img = s.density;
    const int side = std::max(16, o.density_max_side);
    img.w = dw >= dh ? side : std::max(1, static_cast<int>(side * dw / dh));
    img.h = dh >= dw ? side : std::max(1, static_cast<int>(side * dh / dw));
    img.region = s.die;
    img.name = "Cell density";
    img.unit = "area ratio";
    img.values.assign(static_cast<std::size_t>(img.w) * img.h, 0.0f);
    const double bw = dw / img.w, bh = dh / img.h;
    const double inv = 1.0 / (bw * bh);
    // Parallel only when a density row cannot be shared by tile rows three
    // apart (see three_phase_rows).
    const bool safe = bh <= s.tile_h && s.max_cell_h * 2.0 <= s.tile_h;
    // Map tile rows to density rows: tile row iy covers cells whose centers
    // are in it; with tiles >= 2 cells tall a cell spills into neighbors
    // only, so three phases keep writers apart.
    three_phase_rows(s.tiles_y, [&](int iy) {
      const auto [b, e] = s.tile_row_range(iy, 0, s.tiles_x - 1);
      for (std::uint32_t k = b; k < e; ++k) {
        const Instance& r = s.cells[k];
        splat_area(img, bw, bh, r.x0, r.y0, r.x1, r.y1, inv);
      }
    }, safe);
    finish_range(img);
    if (o.pin_density) {
      ScalarImage& pd = s.pin_density;
      pd.w = img.w;
      pd.h = img.h;
      pd.region = s.die;
      pd.name = "Pin density";
      const double dbu = snap->dbu_per_micron() > 0 ? snap->dbu_per_micron() : 1.0;
      pd.unit = snap->dbu_per_micron() > 0 ? "pins/um^2" : "pins/DBU^2";
      pd.values.assign(img.values.size(), 0.0f);
      const float per = static_cast<float>(dbu * dbu * inv);
      three_phase_rows(s.tiles_y, [&](int iy) {
        const auto [b, e] = s.tile_row_range(iy, 0, s.tiles_x - 1);
        for (std::uint32_t k = b; k < e; ++k) {
          for (Id p : snap->cell_pins(s.cell_ids[k])) {
            const Point q = snap->pin_location(p);
            const int ix = std::clamp(static_cast<int>(static_cast<double>(q.x - s.origin.x) / bw), 0, pd.w - 1);
            const int jy = std::clamp(static_cast<int>(static_cast<double>(q.y - s.origin.y) / bh), 0, pd.h - 1);
            pd.values[static_cast<std::size_t>(jy) * pd.w + ix] += per;
          }
        }
      }, safe);
      for (Id id : s.macro_ids)
        for (Id p : snap->cell_pins(id)) {
          const Point q = snap->pin_location(p);
          const int ix = std::clamp(static_cast<int>(static_cast<double>(q.x - s.origin.x) / bw), 0, pd.w - 1);
          const int jy = std::clamp(static_cast<int>(static_cast<double>(q.y - s.origin.y) / bh), 0, pd.h - 1);
          pd.values[static_cast<std::size_t>(jy) * pd.w + ix] += per;
        }
      finish_range(pd);
    }
  }
  const double t1 = now_ms();
  s.density_ms = t1 - td;
  s.build_ms = t1 - t0;
  return sc;
}

// --- Detail ------------------------------------------------------------------

namespace {
inline int metal_display_layer(int metal) {
  return std::min(LayerSet::kFirstMetal + metal, LayerSet::kMaxLayers - 1);
}
void append_net(const DesignSnapshot& s, const TechInfo& tech, Id net, const Scene& sc, std::vector<Point>& pts,
                std::vector<WireSegment>& segs, std::vector<ViaShape>* vias, std::vector<Instance>& out_w,
                std::vector<Instance>* out_v, std::vector<Id>* out_net) {
  pts.clear();
  for (Id p : s.net_pins(net)) pts.push_back(s.pin_location(p));
  segs.clear();
  if (vias) vias->clear();
  tech.routes.route_net(pts, segs, vias);
  for (const WireSegment& w : segs) {
    out_w.push_back({sc.fx(w.a.x), sc.fy(w.a.y), sc.fx(w.b.x), sc.fy(w.b.y),
                     style::make(metal_display_layer(w.layer))});
    if (out_net) out_net->push_back(net);
  }
  if (vias && out_v)
    for (const ViaShape& v : *vias)
      out_v->push_back({sc.fx(v.at.x), sc.fy(v.at.y), 2.0f, static_cast<float>(v.to),
                        style::make(LayerSet::kVias, style::kMarker)});
}
}  // namespace

std::shared_ptr<Detail> build_detail(const Scene& sc, const TechInfo& tech, const DetailOptions& o) {
  const double t0 = now_ms();
  auto out = std::make_shared<Detail>();
  Detail& d = *out;
  d.version = sc.version;
  d.window = o.window;
  d.opts = o;
  const DesignSnapshot& s = *sc.snap;
  const Rect& w = o.window;
  const float wx0 = sc.fx(w.xlo), wy0 = sc.fy(w.ylo), wx1 = sc.fx(w.xhi), wy1 = sc.fy(w.yhi);

  // Pins of cells in the window.
  if (o.pins) {
    Rect grow{w.xlo - static_cast<Dbu>(sc.max_cell_w), w.ylo - static_cast<Dbu>(sc.max_cell_h),
              w.xhi + static_cast<Dbu>(sc.max_cell_w), w.yhi + static_cast<Dbu>(sc.max_cell_h)};
    int ix0, iy0, ix1, iy1;
    sc.tiles_for(grow, ix0, iy0, ix1, iy1);
    auto add_cell_pins = [&](Id id) {
      for (Id p : s.cell_pins(id)) {
        const Point q = s.pin_location(p);
        d.pins.push_back({sc.fx(q.x), sc.fy(q.y), 2.5f, 0.0f, style::make(LayerSet::kPins, style::kMarker)});
        d.pin_ids.push_back(p);
      }
    };
    for (int iy = iy0; iy <= iy1; ++iy) {
      const auto [b, e] = sc.tile_row_range(iy, ix0, ix1);
      for (std::uint32_t k = b; k < e; ++k) {
        const Instance& r = sc.cells[k];
        if (r.x1 < wx0 || r.x0 > wx1 || r.y1 < wy0 || r.y0 > wy1) continue;
        add_cell_pins(sc.cell_ids[k]);
      }
    }
    for (std::size_t m = 0; m < sc.macros.size(); ++m) {
      const Instance& r = sc.macros[m];
      if (r.x1 < wx0 || r.x0 > wx1 || r.y1 < wy0 || r.y0 > wy1) continue;
      add_cell_pins(sc.macro_ids[m]);
    }
  }

  // Wires of nets whose bounding box meets the window.
  if (o.wires || o.vias) {
    const std::size_t nn = s.num_nets();
    constexpr std::size_t kBlocks = 64;
    const std::size_t bs = (nn + kBlocks - 1) / kBlocks;
    std::vector<std::vector<Id>> hits(kBlocks);
    std::vector<std::size_t> big(kBlocks, 0);
    par::parallel_for_each(0, kBlocks, [&](std::size_t b) {
      const std::size_t e = std::min(nn, (b + 1) * bs);
      for (std::size_t i = b * bs; i < e; ++i) {
        const Id net = static_cast<Id>(i);
        if (s.net(net).removed) continue;
        const auto pins = s.net_pins(net);
        if (pins.size() < 2) continue;
        Dbu x0 = std::numeric_limits<Dbu>::max(), y0 = x0, x1 = std::numeric_limits<Dbu>::min(), y1 = x1;
        for (Id p : pins) {
          const Point q = s.pin_location(p);
          x0 = std::min(x0, q.x);
          x1 = std::max(x1, q.x);
          y0 = std::min(y0, q.y);
          y1 = std::max(y1, q.y);
        }
        if (x1 < w.xlo || x0 > w.xhi || y1 < w.ylo || y0 > w.yhi) continue;
        if (pins.size() > o.max_net_pins) {
          ++big[b];
          continue;
        }
        hits[b].push_back(net);
      }
    });
    std::vector<Id> nets;
    for (std::size_t b = 0; b < kBlocks; ++b) {
      nets.insert(nets.end(), hits[b].begin(), hits[b].end());
      d.skipped_big_nets += big[b];
    }
    if (nets.size() > o.max_nets) {
      nets.resize(o.max_nets);
      d.truncated = true;
    }
    d.nets = nets.size();
    // Route in parallel blocks, concatenate in block order.
    const std::size_t nb = std::max<std::size_t>(1, std::min<std::size_t>(kBlocks, nets.size() / 256 + 1));
    const std::size_t rb = (nets.size() + nb - 1) / nb;
    std::vector<std::vector<Instance>> bw(nb), bv(nb);
    std::vector<std::vector<Id>> bn(nb);
    par::parallel_for_each(0, nb, [&](std::size_t b) {
      std::vector<Point> pts;
      std::vector<WireSegment> segs;
      std::vector<ViaShape> vias;
      const std::size_t e = std::min(nets.size(), (b + 1) * rb);
      for (std::size_t k = b * rb; k < e; ++k)
        append_net(s, tech, nets[k], sc, pts, segs, o.vias ? &vias : nullptr, bw[b], o.vias ? &bv[b] : nullptr,
                   &bn[b]);
    });
    std::size_t tw = 0, tv = 0;
    for (std::size_t b = 0; b < nb; ++b) {
      tw += bw[b].size();
      tv += bv[b].size();
    }
    if (o.wires) {
      d.wires.reserve(std::min(tw, o.max_segments));
      d.wire_nets.reserve(std::min(tw, o.max_segments));
    }
    d.vias.reserve(tv);
    for (std::size_t b = 0; b < nb; ++b) {
      if (o.wires) {
        const std::size_t room = o.max_segments - d.wires.size();
        const std::size_t take = std::min(room, bw[b].size());
        d.wires.insert(d.wires.end(), bw[b].begin(), bw[b].begin() + static_cast<std::ptrdiff_t>(take));
        d.wire_nets.insert(d.wire_nets.end(), bn[b].begin(), bn[b].begin() + static_cast<std::ptrdiff_t>(take));
        if (take < bw[b].size()) d.truncated = true;
      }
      d.vias.insert(d.vias.end(), bv[b].begin(), bv[b].end());
    }
  }
  // Stable counting sort of the wires by layer, so lower metals draw first.
  {
    constexpr int L = LayerSet::kMaxLayers;
    d.layer_begin.assign(L + 1, 0);
    for (const Instance& w : d.wires) ++d.layer_begin[static_cast<std::size_t>(style::layer_of(w.style)) + 1];
    for (int l = 0; l < L; ++l) d.layer_begin[l + 1] += d.layer_begin[l];
    std::vector<std::uint32_t> pos(d.layer_begin.begin(), d.layer_begin.end() - 1);
    std::vector<Instance> w2(d.wires.size());
    std::vector<Id> n2(d.wires.size());
    for (std::size_t i = 0; i < d.wires.size(); ++i) {
      const std::uint32_t k = pos[static_cast<std::size_t>(style::layer_of(d.wires[i].style))]++;
      w2[k] = d.wires[i];
      n2[k] = d.wire_nets[i];
    }
    d.wires = std::move(w2);
    d.wire_nets = std::move(n2);
  }
  d.build_ms = now_ms() - t0;
  return out;
}

void net_wires(const DesignSnapshot& s, const TechInfo& tech, Id net, const Scene& sc, std::vector<Instance>& wires,
               std::vector<Instance>* vias) {
  if (net < 0 || static_cast<std::size_t>(net) >= s.num_nets()) return;
  std::vector<Point> pts;
  std::vector<WireSegment> segs;
  std::vector<ViaShape> v;
  append_net(s, tech, net, sc, pts, segs, vias ? &v : nullptr, wires, vias, nullptr);
}

// --- Picking ------------------------------------------------------------------

namespace {
double seg_dist2(double px, double py, const Instance& s) {
  const double dx = s.x1 - s.x0, dy = s.y1 - s.y0;
  const double l2 = dx * dx + dy * dy;
  double t = l2 > 0 ? ((px - s.x0) * dx + (py - s.y0) * dy) / l2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  const double ex = s.x0 + t * dx - px, ey = s.y0 + t * dy - py;
  return ex * ex + ey * ey;
}
}  // namespace

PickResult pick(const PickContext& ctx, Point p) {
  PickResult r;
  if (!ctx.scene || !ctx.layers) return r;
  const Scene& sc = *ctx.scene;
  const LayerSet& L = *ctx.layers;
  const double px = static_cast<double>(p.x - sc.origin.x), py = static_cast<double>(p.y - sc.origin.y);
  const double tol = ctx.tolerance_px * ctx.world_per_px;
  const double tol2 = tol * tol;

  // Pins (markers, a few pixels big).
  double best = tol2;
  if (ctx.detail && L.pickable(LayerSet::kPins)) {
    const auto& v = ctx.detail->pins;
    for (std::size_t i = 0; i < v.size(); ++i) {
      const double dx = v[i].x0 - px, dy = v[i].y0 - py, d2 = dx * dx + dy * dy;
      if (d2 <= best) {
        best = d2;
        r = {PickResult::Kind::Pin, ctx.detail->pin_ids[i]};
      }
    }
  }
  if (L.pickable(LayerSet::kIoPins)) {
    for (std::size_t i = 0; i < sc.io_pins.size(); ++i) {
      const double dx = sc.io_pins[i].x0 - px, dy = sc.io_pins[i].y0 - py, d2 = dx * dx + dy * dy;
      if (d2 <= best) {
        best = d2;
        r = {PickResult::Kind::Pin, sc.io_pin_ids[i]};
      }
    }
  }
  if (r.kind != PickResult::Kind::None) return r;

  // Wires.
  if (ctx.detail) {
    const auto& v = ctx.detail->wires;
    best = tol2;
    // Prefer the top-most metal on ties (it is drawn last).
    for (std::size_t i = 0; i < v.size(); ++i) {
      const int layer = style::layer_of(v[i].style);
      if (layer >= L.size() || !L.pickable(layer)) continue;
      const double d2 = seg_dist2(px, py, v[i]);
      if (d2 <= best) {
        best = d2;
        r = {PickResult::Kind::Net, ctx.detail->wire_nets[i]};
      }
    }
    if (r.kind != PickResult::Kind::None) return r;
  }

  // Std cells.
  if (L.pickable(LayerSet::kStdCells) || L.pickable(LayerSet::kFixedCells)) {
    const Rect around{p.x - static_cast<Dbu>(sc.max_cell_w), p.y - static_cast<Dbu>(sc.max_cell_h),
                      p.x + static_cast<Dbu>(sc.max_cell_w), p.y + static_cast<Dbu>(sc.max_cell_h)};
    int ix0, iy0, ix1, iy1;
    sc.tiles_for(around, ix0, iy0, ix1, iy1);
    double best_area = std::numeric_limits<double>::max();
    for (int iy = iy0; iy <= iy1; ++iy) {
      const auto [b, e] = sc.tile_row_range(iy, ix0, ix1);
      for (std::uint32_t k = b; k < e; ++k) {
        const Instance& c = sc.cells[k];
        if (!L.pickable(style::layer_of(c.style))) continue;
        // Tiny cells: accept clicks within the pick tolerance.
        const double sx = std::max<double>(0.0, tol - (c.x1 - c.x0)) * 0.5;
        const double sy = std::max<double>(0.0, tol - (c.y1 - c.y0)) * 0.5;
        if (px < c.x0 - sx || px > c.x1 + sx || py < c.y0 - sy || py > c.y1 + sy) continue;
        const double a = static_cast<double>(c.x1 - c.x0) * (c.y1 - c.y0);
        if (a < best_area) {
          best_area = a;
          r = {PickResult::Kind::Cell, sc.cell_ids[k]};
        }
      }
    }
    if (r.kind != PickResult::Kind::None) return r;
  }

  // Macros (last drawn = on top).
  if (L.pickable(LayerSet::kMacros)) {
    for (std::size_t m = sc.macros.size(); m-- > 0;) {
      const Instance& c = sc.macros[m];
      if (px >= c.x0 && px <= c.x1 && py >= c.y0 && py <= c.y1) return {PickResult::Kind::Cell, sc.macro_ids[m]};
    }
  }
  return r;
}

}  // namespace hermes::gui
