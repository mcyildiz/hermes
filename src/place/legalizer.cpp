#include "hermes/place/legalizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <cstdint>
#include <vector>

namespace hermes {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

Dbu manhattan(Point a, Point b) { return std::llabs(a.x - b.x) + std::llabs(a.y - b.y); }

Rect grow(const Rect& r, Dbu d) { return {r.xlo - d, r.ylo - d, r.xhi + d, r.yhi + d}; }

bool is_movable_std(const Cell& c) { return !c.is_macro && c.movable(); }

/// Rounds `y` down/up onto the row grid anchored at `y0`.
Dbu row_floor(Dbu y, Dbu y0, Dbu rh) {
  const Dbu d = y - y0;
  Dbu q = d / rh;
  if (d < 0 && d % rh != 0) --q;
  return y0 + q * rh;
}
Dbu row_ceil(Dbu y, Dbu y0, Dbu rh) {
  const Dbu f = row_floor(y, y0, rh);
  return f == y ? f : f + rh;
}

}  // namespace

// ===========================================================================
// Macro legalization
// ===========================================================================

MacroLegalizeResult legalize_macros(Design& design, const MacroLegalizerOptions& opts) {
  MacroLegalizeResult res;
  const auto cells = design.cells();
  const Rect& die = design.die();
  const Dbu rh = design.row_height();
  const bool snap = opts.snap_to_rows && rh > 0;

  // Obstacles: fixed cells (macros or not). Placed macros are appended.
  std::vector<Rect> placed;
  std::vector<Id> movable;
  for (Id i = 0; i < static_cast<Id>(cells.size()); ++i) {
    const Cell& c = cells[i];
    if (c.removed) continue;
    if (c.fixed) placed.push_back(c.bbox());
    else if (c.is_macro) movable.push_back(i);
  }
  // Largest first: big macros are hardest to fit; small ones fill the gaps.
  std::stable_sort(movable.begin(), movable.end(), [&](Id a, Id b) {
    return cells[a].width * cells[a].height > cells[b].width * cells[b].height;
  });

  for (Id mi : movable) {
    const Cell& m = cells[mi];
    const Point target = m.loc;
    const Dbu w = m.width, h = m.height;
    const Dbu xmax = std::max(die.xlo, die.xhi - w);
    const Dbu ymax = std::max(die.ylo, die.yhi - h);

    std::vector<Dbu> xs{target.x, die.xlo, die.xhi - w};
    std::vector<Dbu> ys{target.y, die.ylo, die.yhi - h};
    for (const Rect& p : placed) {
      xs.push_back(p.xhi + opts.halo);
      xs.push_back(p.xlo - opts.halo - w);
      ys.push_back(p.yhi + opts.halo);
      ys.push_back(p.ylo - opts.halo - h);
    }
    for (Dbu& x : xs) x = std::clamp(x, die.xlo, xmax);
    if (snap) {
      // Row-aligned macro rows: try the grid point on either side of each y.
      std::vector<Dbu> snapped;
      snapped.reserve(ys.size() * 2);
      for (Dbu y : ys) {
        snapped.push_back(row_floor(y, die.ylo, rh));
        snapped.push_back(row_ceil(y, die.ylo, rh));
      }
      ys = std::move(snapped);
      const Dbu ymax_aligned = row_floor(ymax, die.ylo, rh);
      for (Dbu& y : ys) y = std::clamp(y, die.ylo, std::max(die.ylo, ymax_aligned));
    } else {
      for (Dbu& y : ys) y = std::clamp(y, die.ylo, ymax);
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

    // Pass 1: cheapest overlap-free candidate.
    // Pass 2 (only if pass 1 fails): least total overlap, then displacement.
    Point best = target;
    Dbu best_cost = std::numeric_limits<Dbu>::max();
    bool found = false;
    for (Dbu y : ys) {
      for (Dbu x : xs) {
        const Point p{x, y};
        const Dbu cost = manhattan(p, target);
        if (cost >= best_cost) continue;
        const Rect r = grow(Rect{x, y, x + w, y + h}, opts.halo);
        bool clash = false;
        for (const Rect& o : placed) {
          if (r.overlaps(o)) { clash = true; break; }
        }
        if (!clash) { best = p; best_cost = cost; found = true; }
      }
    }
    if (!found) {
      Dbu best_ov = std::numeric_limits<Dbu>::max();
      for (Dbu y : ys) {
        for (Dbu x : xs) {
          const Rect r{x, y, x + w, y + h};
          Dbu ov = 0;
          for (const Rect& o : placed) ov += r.overlap_area(o);
          const Dbu cost = manhattan({x, y}, target);
          if (ov < best_ov || (ov == best_ov && cost < best_cost)) {
            best_ov = ov; best_cost = cost; best = {x, y};
          }
        }
      }
      ++res.unresolved;
    }

    design.move_cell(mi, best);
    placed.push_back(m.bbox());
    ++res.moved;
    const Dbu d = manhattan(best, target);
    res.total_displacement += static_cast<double>(d);
    res.max_displacement = std::max(res.max_displacement, d);
  }
  return res;
}

// ===========================================================================
// Abacus standard-cell legalization
// ===========================================================================

namespace {

/// Snaps `x` onto the site grid {origin + k * step}: down, up or nearest.
Dbu grid_floor(Dbu x, Dbu origin, Dbu step) { return row_floor(x, origin, step); }
Dbu grid_ceil(Dbu x, Dbu origin, Dbu step) { return row_ceil(x, origin, step); }
Dbu grid_round(double x, Dbu origin, Dbu step) {
  const double k = std::floor((x - static_cast<double>(origin)) / static_cast<double>(step) + 0.5);
  return origin + static_cast<Dbu>(k) * step;
}

struct Cluster {
  double e = 0;      ///< Total weight.
  double q = 0;      ///< Weighted sum of (target x - offset in cluster).
  Dbu w = 0;         ///< Total (site-rounded) width.
  double x = 0;      ///< Optimal (clamped, continuous) left edge.
  int first = 0;     ///< Index into Segment::cells of the first cell.
};

/// A free span of one row. Cells sit on its site grid {origin + k * step};
/// lo and hi are on that grid.
struct Segment {
  Dbu lo = 0, hi = 0;          ///< Free span [lo, hi).
  Dbu origin = 0, step = 1;    ///< Site grid.
  Dbu used = 0;                ///< Width already assigned.
  std::vector<Id> cells;       ///< In insertion (= left-to-right) order.
  std::vector<Dbu> widths;     ///< Site-rounded width of each cell.
  std::vector<Cluster> clusters;
};

/// One row span from the floorplan (a DEF ROW / Bookshelf subrow, clipped to
/// the window), before obstacles are cut out.
struct Span {
  Dbu lo = 0, hi = 0, origin = 0, step = 1;
};

struct LRow {
  Dbu y = 0, h = 0;
  std::vector<Span> spans;                       ///< Sorted by lo.
  std::vector<std::pair<Dbu, Dbu>> blocked;      ///< Obstacle x-intervals.
  std::vector<Segment> segs;                     ///< Sorted by lo, disjoint.
};

/// Subtracts the given x-intervals from [lo, hi) and returns what is left
/// (`blocked` must be sorted).
void free_spans(Dbu lo, Dbu hi, const std::vector<std::pair<Dbu, Dbu>>& blocked,
                std::vector<std::pair<Dbu, Dbu>>& out) {
  out.clear();
  Dbu cur = lo;
  for (auto [a, b] : blocked) {
    if (b <= cur) continue;
    if (a >= hi) break;
    a = std::max(a, lo);
    b = std::min(b, hi);
    if (a > cur) out.emplace_back(cur, a);
    cur = std::max(cur, b);
  }
  if (cur < hi) out.emplace_back(cur, hi);
}

class Abacus {
 public:
  /// `in_set[i]` marks the cells being legalized; every other live cell that
  /// overlaps `window` is an obstacle. Only rows fully inside `window` are used.
  Abacus(const Design& d, Dbu rh, const Rect& window, const std::vector<std::uint8_t>& in_set,
         bool snap_to_sites, double width_weight)
      : d_(d), rh_(rh), width_weight_(width_weight) {
    build_rows(window, snap_to_sites);
    const auto cells = d.cells();
    for (Id i = 0; i < static_cast<Id>(cells.size()); ++i) {
      const Cell& c = cells[i];
      if (c.removed || in_set[i]) continue;
      const Rect b = c.bbox();
      if (b.overlaps(window)) add_obstacle(b);
    }
    for (int ri = 0; ri < num_rows(); ++ri) rebuild(ri);
  }

  int num_rows() const { return static_cast<int>(rows_.size()); }

  /// Greedy nearest placement of a cell spanning `k` rows; becomes an obstacle.
  bool place_multi_row(Id ci, int k) {
    const Cell& c = d_.cell(ci);
    const Point t = c.loc;
    double best = kInf;
    Point bp{};
    std::vector<Span> inter, next;
    for (int r0 = 0; r0 + k <= num_rows(); ++r0) {
      const double dy = std::fabs(static_cast<double>(rows_[r0].y - t.y));
      if (dy >= best) continue;
      // Rows r0 .. r0+k-1 must be stacked without gaps.
      bool stacked = true;
      for (int r = r0 + 1; r < r0 + k; ++r) {
        if (rows_[r].y != rows_[r - 1].y + rows_[r - 1].h) { stacked = false; break; }
      }
      if (!stacked) continue;
      inter.clear();
      for (const Segment& s : rows_[r0].segs) inter.push_back({s.lo, s.hi, s.origin, s.step});
      for (int r = r0 + 1; r < r0 + k && !inter.empty(); ++r) {
        next.clear();
        for (const Span& a : inter) {
          for (const Segment& s : rows_[r].segs) {
            const Dbu lo = std::max(a.lo, s.lo), hi = std::min(a.hi, s.hi);
            if (hi > lo) next.push_back({lo, hi, a.origin, a.step});
          }
        }
        std::swap(inter, next);
      }
      for (const Span& a : inter) {
        const Dbu lo = grid_ceil(a.lo, a.origin, a.step);
        const Dbu hi = grid_floor(a.hi - c.width, a.origin, a.step);  // last legal x
        if (hi < lo) continue;
        const Dbu x = std::clamp(grid_round(static_cast<double>(t.x), a.origin, a.step), lo, hi);
        const double cost = dy + std::fabs(static_cast<double>(x - t.x));
        if (cost < best) { best = cost; bp = {x, rows_[r0].y}; }
      }
    }
    if (best == kInf) return false;
    multi_.emplace_back(ci, bp);
    for (int r = 0; r < num_rows(); ++r) {
      const Dbu y = rows_[r].y;
      if (bp.y < y + rows_[r].h && y < bp.y + c.height) {
        insert_blocked(r, {bp.x, bp.x + c.width});
        rebuild(r);
      }
    }
    return true;
  }

  /// Places one single-row cell into the best row/segment; returns false if
  /// no segment has room.
  bool place(Id ci) {
    const Cell& c = d_.cell(ci);
    const double tx = static_cast<double>(c.loc.x);
    const double ty = static_cast<double>(c.loc.y);
    const int n = num_rows();
    if (n == 0) return false;
    // Nearest row by y.
    auto it = std::lower_bound(rows_.begin(), rows_.end(), c.loc.y,
                               [](const LRow& r, Dbu y) { return r.y < y; });
    int r0 = static_cast<int>(it - rows_.begin());
    if (r0 == n || (r0 > 0 && ty - static_cast<double>(rows_[r0 - 1].y) <
                                  static_cast<double>(rows_[r0].y) - ty)) {
      r0 = std::max(0, r0 - 1);
    }

    double best = kInf;
    int best_r = -1, best_s = -1;
    Dbu best_w = 0;
    auto try_seg = [&](int ri, int si, double dy) {
      const Segment& s = rows_[ri].segs[si];
      const Dbu w = rounded(c.width, s.step);
      if (s.hi - s.lo - s.used < w) return;
      // Lower bound: distance from target to the segment's feasible span.
      const double lo = static_cast<double>(s.lo);
      const double hi = static_cast<double>(s.hi - w);
      const double lb = dy + (tx < lo ? lo - tx : (tx > hi ? tx - hi : 0.0));
      if (lb >= best) return;
      const double x = trial(s, w, tx);
      const double cost = dy + std::fabs(x - tx);
      if (cost < best) { best = cost; best_r = ri; best_s = si; best_w = w; }
    };
    auto try_row = [&](int ri) {
      const LRow& row = rows_[ri];
      if (c.height > row.h) return;
      const double dy = std::fabs(static_cast<double>(row.y) - ty);
      const int ns = static_cast<int>(row.segs.size());
      // First segment whose lo is right of the target; scan both ways.
      int k = static_cast<int>(std::upper_bound(row.segs.begin(), row.segs.end(), c.loc.x,
                                                [](Dbu x, const Segment& s) { return x < s.lo; }) -
                               row.segs.begin());
      for (int si = k - 1; si >= 0; --si) {
        const Segment& s = row.segs[si];
        const double lb = dy + std::max(0.0, tx - static_cast<double>(s.hi));
        if (lb >= best) break;
        try_seg(ri, si, dy);
      }
      for (int si = k; si < ns; ++si) {
        const Segment& s = row.segs[si];
        const double lb = dy + std::max(0.0, static_cast<double>(s.lo) - tx);
        if (lb >= best) break;
        try_seg(ri, si, dy);
      }
    };
    // Search outward from the nearest row; stop once vertical distance alone
    // exceeds the best cost found.
    for (int k = 0; k < n; ++k) {
      bool any = false;
      for (int ri : {r0 - k, r0 + k}) {
        if (ri < 0 || ri >= n || (k == 0 && ri != r0)) continue;
        const double dy = std::fabs(static_cast<double>(rows_[ri].y) - ty);
        if (dy >= best) continue;
        any = true;
        try_row(ri);
        if (k == 0) break;
      }
      if (!any && best < kInf) break;
    }
    if (best_r < 0) return false;
    commit(rows_[best_r].segs[best_s], ci, best_w, tx, weight(c));
    return true;
  }

  /// Final locations of every placed cell (multi-row first, then rows).
  void results(std::vector<std::pair<Id, Point>>& out) const {
    out.insert(out.end(), multi_.begin(), multi_.end());
    for (const LRow& row : rows_) {
      for (const Segment& s : row.segs) {
        for (std::size_t k = 0; k < s.clusters.size(); ++k) {
          const Cluster& cl = s.clusters[k];
          const int end = k + 1 < s.clusters.size() ? s.clusters[k + 1].first
                                                    : static_cast<int>(s.cells.size());
          // Snapping to the grid is monotone and cluster widths are whole
          // sites, so cluster order (and thus legality) survives.
          Dbu x = grid_round(cl.x, s.origin, s.step);
          x = std::clamp(x, s.lo, s.hi - cl.w);
          for (int i = cl.first; i < end; ++i) {
            out.emplace_back(s.cells[i], Point{x, row.y});
            x += s.widths[i];
          }
        }
      }
    }
  }

 private:
  static Dbu rounded(Dbu w, Dbu step) { return (w + step - 1) / step * step; }
  double weight(const Cell& c) const {
    return width_weight_ > 0.0 ? 1.0 + width_weight_ * (static_cast<double>(c.width) / rh_) : 1.0;
  }

  void build_rows(const Rect& win, bool snap) {
    const auto rows = d_.rows();
    std::vector<std::pair<Dbu, Span>> spans;  // (y, span), height = rh_ unless rows say
    std::vector<std::pair<Dbu, Dbu>> heights;
    if (!rows.empty()) {
      for (const Row& r : rows) {
        const Dbu h = r.site_height > 0 ? r.site_height : rh_;
        if (r.origin.y < win.ylo || r.origin.y + h > win.yhi) continue;
        const Dbu step = snap && r.site_step > 0 ? r.site_step : 1;
        Span s{std::max(r.origin.x, win.xlo), std::min(r.xhi(), win.xhi), r.origin.x, step};
        if (s.hi <= s.lo) continue;
        spans.push_back({r.origin.y, s});
        heights.push_back({r.origin.y, h});
      }
    } else {
      // No floorplan rows: rows of height rh_ from the die's bottom edge.
      const Rect& die = d_.die();
      const Dbu n = die.height() / rh_;
      for (Dbu k = 0; k < n; ++k) {
        const Dbu y = die.ylo + k * rh_;
        if (y < win.ylo || y + rh_ > win.yhi) continue;
        Span s{std::max(die.xlo, win.xlo), std::min(die.xhi, win.xhi), die.xlo, 1};
        if (s.hi <= s.lo) continue;
        spans.push_back({y, s});
        heights.push_back({y, rh_});
      }
    }
    std::stable_sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) {
      return a.first != b.first ? a.first < b.first : a.second.lo < b.second.lo;
    });
    std::sort(heights.begin(), heights.end());
    for (const auto& [y, s] : spans) {
      if (rows_.empty() || rows_.back().y != y) {
        LRow r;
        r.y = y;
        r.h = std::lower_bound(heights.begin(), heights.end(), std::pair<Dbu, Dbu>{y, 0})->second;
        rows_.push_back(std::move(r));
      }
      rows_.back().spans.push_back(s);
    }
  }

  void add_obstacle(const Rect& r) {
    // Rows are sorted by y: find the first row that can overlap.
    auto it = std::lower_bound(rows_.begin(), rows_.end(), r.ylo,
                               [](const LRow& row, Dbu y) { return row.y + row.h <= y; });
    for (; it != rows_.end() && it->y < r.yhi; ++it) {
      if (r.ylo < it->y + it->h) it->blocked.emplace_back(r.xlo, r.xhi);
    }
  }

  void insert_blocked(int ri, std::pair<Dbu, Dbu> iv) {
    auto& b = rows_[ri].blocked;
    b.insert(std::upper_bound(b.begin(), b.end(), iv), iv);
  }

  void rebuild(int ri) {
    LRow& row = rows_[ri];
    if (!std::is_sorted(row.blocked.begin(), row.blocked.end())) {
      std::sort(row.blocked.begin(), row.blocked.end());
    }
    row.segs.clear();
    for (const Span& sp : row.spans) {
      free_spans(sp.lo, sp.hi, row.blocked, scratch_);
      for (auto [a, b] : scratch_) {
        Segment s;
        s.origin = sp.origin;
        s.step = sp.step;
        s.lo = grid_ceil(a, sp.origin, sp.step);
        s.hi = grid_floor(b, sp.origin, sp.step);
        if (s.hi > s.lo) row.segs.push_back(std::move(s));
      }
    }
  }

  static double clampx(double x, const Segment& s, Dbu w) {
    return std::clamp(x, static_cast<double>(s.lo), static_cast<double>(s.hi - w));
  }

  /// x the new cell would get if appended to `s`, without changing `s`.
  static double trial(const Segment& s, Dbu w, double tx) {
    const double t = clampx(tx, s, w);
    if (s.clusters.empty()) return t;
    const Cluster& last = s.clusters.back();
    if (last.x + static_cast<double>(last.w) <= t) return t;
    // Only the position matters here; unit weight is close enough for the
    // choice of segment (commit() uses the real weight).
    double e = last.e + 1.0;
    double q = last.q + (tx - static_cast<double>(last.w));
    Dbu cw = last.w + w;
    int idx = static_cast<int>(s.clusters.size()) - 2;
    double x = 0;
    while (true) {
      x = clampx(q / e, s, cw);
      if (idx >= 0 && s.clusters[idx].x + static_cast<double>(s.clusters[idx].w) > x) {
        const Cluster& p = s.clusters[idx];
        q = p.q + q - e * static_cast<double>(p.w);
        e += p.e;
        cw += p.w;
        --idx;
      } else {
        break;
      }
    }
    return x + static_cast<double>(cw - w);  // New cell is the cluster's last.
  }

  /// Appends cell `ci` (rounded width `w`, target `tx`, weight `wt`) to `s`.
  static void commit(Segment& s, Id ci, Dbu w, double tx, double wt) {
    s.cells.push_back(ci);
    s.widths.push_back(w);
    s.used += w;
    const double t = clampx(tx, s, w);
    if (s.clusters.empty() ||
        s.clusters.back().x + static_cast<double>(s.clusters.back().w) <= t) {
      Cluster c;
      c.e = wt;
      c.q = wt * tx;
      c.w = w;
      c.x = t;
      c.first = static_cast<int>(s.cells.size()) - 1;
      s.clusters.push_back(c);
      return;
    }
    Cluster& last = s.clusters.back();
    last.e += wt;
    last.q += wt * (tx - static_cast<double>(last.w));
    last.w += w;
    collapse(s);
  }

  static void collapse(Segment& s) {
    while (true) {
      Cluster& c = s.clusters.back();
      c.x = clampx(c.q / c.e, s, c.w);
      if (s.clusters.size() < 2) return;
      Cluster& p = s.clusters[s.clusters.size() - 2];
      if (p.x + static_cast<double>(p.w) <= c.x) return;
      p.q += c.q - c.e * static_cast<double>(p.w);
      p.e += c.e;
      p.w += c.w;
      s.clusters.pop_back();
    }
  }

  const Design& d_;
  Dbu rh_;
  double width_weight_;
  std::vector<LRow> rows_;
  std::vector<std::pair<Id, Point>> multi_;
  std::vector<std::pair<Dbu, Dbu>> scratch_;
};

Rect intersect(const Rect& a, const Rect& b) {
  return {std::max(a.xlo, b.xlo), std::max(a.ylo, b.ylo), std::min(a.xhi, b.xhi),
          std::min(a.yhi, b.yhi)};
}
bool empty_rect(const Rect& r) { return r.xhi <= r.xlo || r.yhi <= r.ylo; }

}  // namespace

Dbu legalizer_row_height(const Design& design) {
  Dbu rh = design.row_height();
  if (rh > 0) return rh;
  for (const Cell& c : design.cells()) {
    if (is_movable_std(c) && c.height > 0 && (rh <= 0 || c.height < rh)) rh = c.height;
  }
  return rh;
}

CellLegalizeResult abacus_legalize(Design& design, const CellLegalizerOptions& opts) {
  CellLegalizeResult res;
  const auto cells = design.cells();
  const Rect& die = design.die();
  const Dbu rh = legalizer_row_height(design);
  if (rh <= 0) return res;  // No movable std cells.

  // The cells to legalize.
  const bool has_fence = !empty_rect(opts.fence);
  const Rect fence = has_fence ? intersect(opts.fence, die) : die;
  std::vector<Id> todo;
  if (!opts.cells.empty()) {
    for (Id ci : opts.cells) {
      if (ci >= 0 && ci < static_cast<Id>(cells.size()) && is_movable_std(cells[ci]) &&
          !cells[ci].removed) {
        todo.push_back(ci);
      }
    }
    std::sort(todo.begin(), todo.end());
    todo.erase(std::unique(todo.begin(), todo.end()), todo.end());
  } else {
    for (Id i = 0; i < static_cast<Id>(cells.size()); ++i) {
      const Cell& c = cells[i];
      if (c.removed || !is_movable_std(c)) continue;
      if (has_fence && !fence.contains(c.bbox().center())) continue;
      todo.push_back(i);
    }
  }
  if (todo.empty()) return res;
  std::vector<std::uint8_t> in_set(cells.size(), 0);
  for (Id ci : todo) in_set[ci] = 1;

  // Window: the fence, the die, or (a cell list without a fence) the cells'
  // bounding box plus a margin that doubles until everything fits.
  const bool windowed = !opts.cells.empty() && !has_fence;
  Rect window = fence;
  Dbu margin = 0;
  if (windowed) {
    Rect bb{std::numeric_limits<Dbu>::max(), std::numeric_limits<Dbu>::max(),
            std::numeric_limits<Dbu>::min(), std::numeric_limits<Dbu>::min()};
    Dbu maxw = 0;
    for (Id ci : todo) {
      const Rect b = cells[ci].bbox();
      bb = {std::min(bb.xlo, b.xlo), std::min(bb.ylo, b.ylo), std::max(bb.xhi, b.xhi),
            std::max(bb.yhi, b.yhi)};
      maxw = std::max(maxw, cells[ci].width);
    }
    margin = std::max<Dbu>(std::max(1, opts.window_margin_rows) * rh, 2 * maxw);
    window = intersect(grow(bb, margin), die);
  }

  std::vector<Point> orig(todo.size());
  for (std::size_t k = 0; k < todo.size(); ++k) orig[k] = cells[todo[k]].loc;

  std::vector<Id> single, multi;
  for (Id ci : todo) (cells[ci].height > rh ? multi : single).push_back(ci);
  // Multi-row cells first (largest first), then they block rows.
  std::stable_sort(multi.begin(), multi.end(), [&](Id a, Id b) {
    return cells[a].width * cells[a].height > cells[b].width * cells[b].height;
  });
  // Abacus order: by target x (ties by id for determinism).
  std::stable_sort(single.begin(), single.end(),
                   [&](Id a, Id b) { return cells[a].loc.x < cells[b].loc.x; });

  std::vector<std::pair<Id, Point>> placed;
  std::vector<Id> unplaced;
  for (;;) {
    ++res.attempts;
    Abacus ab(design, rh, window, in_set, opts.snap_to_sites, opts.width_weight);
    unplaced.clear();
    res.multi_row = 0;
    for (Id ci : multi) {
      const int k = static_cast<int>((cells[ci].height + rh - 1) / rh);
      ++res.multi_row;
      if (!ab.place_multi_row(ci, k)) unplaced.push_back(ci);
    }
    for (Id ci : single) {
      if (!ab.place(ci)) unplaced.push_back(ci);
    }
    const bool whole = window.xlo <= die.xlo && window.ylo <= die.ylo && window.xhi >= die.xhi &&
                       window.yhi >= die.yhi;
    if (!unplaced.empty() && windowed && !whole) {
      margin *= 2;  // Not enough room nearby: widen the window and retry.
      window = intersect(grow(window, margin), die);
      continue;
    }
    placed.clear();
    ab.results(placed);
    break;
  }

  // Write back in one batch.
  std::vector<Id> ids;
  std::vector<Point> locs;
  ids.reserve(placed.size() + unplaced.size());
  locs.reserve(placed.size() + unplaced.size());
  for (const auto& [ci, p] : placed) {
    ids.push_back(ci);
    locs.push_back(p);
  }
  for (Id ci : unplaced) {  // Left where they are, clamped into the fence.
    const Cell& c = cells[ci];
    ids.push_back(ci);
    locs.push_back({std::clamp(c.loc.x, fence.xlo, std::max(fence.xlo, fence.xhi - c.width)),
                    std::clamp(c.loc.y, fence.ylo, std::max(fence.ylo, fence.yhi - c.height))});
  }
  design.move_cells(ids, locs);
  res.legalized = static_cast<int>(placed.size());
  res.failed = static_cast<int>(unplaced.size());

  for (std::size_t k = 0; k < todo.size(); ++k) {
    const Dbu d = manhattan(cells[todo[k]].loc, orig[k]);
    res.total_displacement += static_cast<double>(d);
    res.max_displacement = std::max(res.max_displacement, d);
  }
  return res;
}
// ===========================================================================
// Legality check
// ===========================================================================

bool check_legal(const Design& design, Dbu row_height, std::string* why) {
  const auto cells = design.cells();
  const Rect& die = design.die();
  auto fail = [&](std::string msg) {
    if (why) *why = std::move(msg);
    return false;
  };
  auto name = [&](Id id) { return std::string(design.cell_name(id)); };
  // Floorplan rows by y (when the design has them): a movable std cell must
  // start on a row, inside one of its spans, on that span's site grid.
  std::vector<Row> rows(design.rows().begin(), design.rows().end());
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return a.origin.y != b.origin.y ? a.origin.y < b.origin.y : a.origin.x < b.origin.x;
  });
  auto on_row = [&](const Cell& c) {
    auto it = std::lower_bound(rows.begin(), rows.end(), c.loc.y,
                               [](const Row& r, Dbu y) { return r.origin.y < y; });
    for (; it != rows.end() && it->origin.y == c.loc.y; ++it) {
      const Dbu step = it->site_step > 0 ? it->site_step : 1;
      if (c.loc.x >= it->origin.x && c.loc.x + c.width <= it->xhi() &&
          (c.loc.x - it->origin.x) % step == 0) {
        return true;
      }
    }
    return false;
  };
  for (Id id = 0; id < static_cast<Id>(cells.size()); ++id) {
    const Cell& c = cells[id];
    if (c.removed) continue;
    const Rect b = c.bbox();
    // Fixed cells are input (pads and terminals may sit outside the die).
    if (!c.fixed && (b.xlo < die.xlo || b.ylo < die.ylo || b.xhi > die.xhi || b.yhi > die.yhi)) {
      return fail("cell " + name(id) + " outside die");
    }
    if (row_height > 0 && is_movable_std(c)) {
      if (!rows.empty()) {
        if (!on_row(c)) return fail("cell " + name(id) + " not on a row site");
      } else if ((c.loc.y - die.ylo) % row_height != 0) {
        return fail("cell " + name(id) + " not on a row");
      }
    }
  }
  std::vector<Id> order;
  for (Id id = 0; id < static_cast<Id>(cells.size()); ++id) {
    if (!cells[id].removed) order.push_back(id);
  }
  std::sort(order.begin(), order.end(),
            [&](Id a, Id b) { return cells[a].loc.x < cells[b].loc.x; });
  for (std::size_t i = 0; i < order.size(); ++i) {
    const Rect a = cells[order[i]].bbox();
    for (std::size_t j = i + 1; j < order.size(); ++j) {
      const Rect b = cells[order[j]].bbox();
      if (b.xlo >= a.xhi) break;
      if (cells[order[i]].fixed && cells[order[j]].fixed) continue;  // input overlaps
      if (a.overlaps(b)) {
        return fail("cells " + name(order[i]) + " and " + name(order[j]) +
                    " overlap");
      }
    }
  }
  return true;
}

}  // namespace hermes
