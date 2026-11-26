#include "hermes/place/legalizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
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

struct Cluster {
  double e = 0;      ///< Total weight.
  double q = 0;      ///< Weighted sum of (target x - offset in cluster).
  Dbu w = 0;         ///< Total width.
  double x = 0;      ///< Optimal (clamped) left edge.
  int first = 0;     ///< Index into Segment::cells of the first cell.
};

struct Segment {
  Dbu lo = 0, hi = 0;          ///< Free span [lo, hi).
  Dbu used = 0;                ///< Width already assigned.
  std::vector<Id> cells;       ///< In insertion (= left-to-right) order.
  std::vector<Cluster> clusters;
};

struct Row {
  Dbu y = 0;
  std::vector<Segment> segs;   ///< Sorted by lo, disjoint.
};

/// Subtracts the given x-intervals from [lo, hi) and returns what is left.
std::vector<std::pair<Dbu, Dbu>> free_spans(Dbu lo, Dbu hi,
                                            std::vector<std::pair<Dbu, Dbu>> blocked) {
  std::sort(blocked.begin(), blocked.end());
  std::vector<std::pair<Dbu, Dbu>> out;
  Dbu cur = lo;
  for (auto [a, b] : blocked) {
    a = std::max(a, lo);
    b = std::min(b, hi);
    if (b <= a) continue;
    if (a > cur) out.emplace_back(cur, a);
    cur = std::max(cur, b);
  }
  if (cur < hi) out.emplace_back(cur, hi);
  return out;
}

class Abacus {
 public:
  Abacus(Design& d, Dbu rh) : d_(d), rh_(rh) {
    const Rect& die = d.die();
    const Dbu n = die.height() / rh;
    rows_.resize(static_cast<std::size_t>(std::max<Dbu>(n, 0)));
    for (Dbu r = 0; r < n; ++r) rows_[r].y = die.ylo + r * rh;
    blocked_.resize(rows_.size());
    for (const Cell& c : d.cells()) {
      if (!c.removed && !is_movable_std(c)) add_obstacle(c.bbox());
    }
    rebuild_all();
  }

  int num_rows() const { return static_cast<int>(rows_.size()); }

  void add_obstacle(const Rect& r) {
    for (int ri = 0; ri < num_rows(); ++ri) {
      const Dbu y = rows_[ri].y;
      if (r.ylo < y + rh_ && y < r.yhi) blocked_[ri].emplace_back(r.xlo, r.xhi);
    }
  }

  void rebuild_all() {
    for (int ri = 0; ri < num_rows(); ++ri) rebuild(ri);
  }

  void rebuild(int ri) {
    const Rect& die = d_.die();
    rows_[ri].segs.clear();
    for (auto [a, b] : free_spans(die.xlo, die.xhi, blocked_[ri])) {
      Segment s;
      s.lo = a;
      s.hi = b;
      rows_[ri].segs.push_back(std::move(s));
    }
  }

  /// Greedy nearest placement of a cell spanning `k` rows; becomes an obstacle.
  bool place_multi_row(Id ci, int k) {
    const Cell& c = d_.cell(ci);
    const Point t = c.loc;
    double best = kInf;
    Point bp{};
    for (int r0 = 0; r0 + k <= num_rows(); ++r0) {
      const double dy = std::fabs(static_cast<double>(rows_[r0].y - t.y));
      if (dy >= best) continue;
      // Intersect free spans of rows r0 .. r0+k-1.
      std::vector<std::pair<Dbu, Dbu>> inter;
      for (const Segment& s : rows_[r0].segs) inter.emplace_back(s.lo, s.hi);
      for (int r = r0 + 1; r < r0 + k && !inter.empty(); ++r) {
        std::vector<std::pair<Dbu, Dbu>> next;
        for (auto [a, b] : inter) {
          for (const Segment& s : rows_[r].segs) {
            const Dbu lo = std::max(a, s.lo), hi = std::min(b, s.hi);
            if (hi > lo) next.emplace_back(lo, hi);
          }
        }
        inter = std::move(next);
      }
      for (auto [a, b] : inter) {
        if (b - a < c.width) continue;
        const Dbu x = std::clamp(t.x, a, b - c.width);
        const double cost = dy + std::fabs(static_cast<double>(x - t.x));
        if (cost < best) { best = cost; bp = {x, rows_[r0].y}; }
      }
    }
    if (best == kInf) return false;
    d_.move_cell(ci, bp);
    for (int r = 0; r < num_rows(); ++r) {
      const Dbu y = rows_[r].y;
      if (bp.y < y + rh_ && y < bp.y + c.height) {
        blocked_[r].emplace_back(bp.x, bp.x + c.width);
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
    int r0 = static_cast<int>(std::lround((ty - static_cast<double>(d_.die().ylo)) /
                                          static_cast<double>(rh_)));
    r0 = std::clamp(r0, 0, n - 1);

    double best = kInf;
    int best_r = -1, best_s = -1;
    auto try_row = [&](int ri) {
      Row& row = rows_[ri];
      const double dy = std::fabs(static_cast<double>(row.y) - ty);
      for (int si = 0; si < static_cast<int>(row.segs.size()); ++si) {
        const Segment& s = row.segs[si];
        if (s.hi - s.lo - s.used < c.width) continue;
        // Lower bound: distance from target to the segment's feasible span.
        const double lo = static_cast<double>(s.lo);
        const double hi = static_cast<double>(s.hi - c.width);
        const double lb = dy + (tx < lo ? lo - tx : (tx > hi ? tx - hi : 0.0));
        if (lb >= best) continue;
        const double x = trial(s, c.width, tx);
        const double cost = dy + std::fabs(x - tx);
        if (cost < best) { best = cost; best_r = ri; best_s = si; }
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
    commit(rows_[best_r].segs[best_s], ci, c.width, tx);
    return true;
  }

  /// Writes final x/y of every row-assigned cell back into the design.
  void finalize() {
    for (Row& row : rows_) {
      for (Segment& s : row.segs) {
        for (std::size_t k = 0; k < s.clusters.size(); ++k) {
          const Cluster& cl = s.clusters[k];
          const int end = k + 1 < s.clusters.size() ? s.clusters[k + 1].first
                                                    : static_cast<int>(s.cells.size());
          // round() is monotone, so cluster order (and thus legality) survives.
          Dbu x = static_cast<Dbu>(std::llround(cl.x));
          x = std::clamp(x, s.lo, s.hi - cl.w);
          for (int i = cl.first; i < end; ++i) {
            const Cell& c = d_.cell(s.cells[i]);
            d_.move_cell(s.cells[i], {x, row.y});
            x += c.width;
          }
        }
      }
    }
  }

 private:
  static double clampx(double x, const Segment& s, Dbu w) {
    return std::clamp(x, static_cast<double>(s.lo), static_cast<double>(s.hi - w));
  }

  /// x the new cell would get if appended to `s`, without changing `s`.
  static double trial(const Segment& s, Dbu w, double tx) {
    const double t = clampx(tx, s, w);
    if (s.clusters.empty()) return t;
    const Cluster& last = s.clusters.back();
    if (last.x + static_cast<double>(last.w) <= t) return t;
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

  /// Appends cell `ci` (width `w`, target `tx`) to `s` and re-collapses.
  static void commit(Segment& s, Id ci, Dbu w, double tx) {
    s.cells.push_back(ci);
    s.used += w;
    const double t = clampx(tx, s, w);
    if (s.clusters.empty() ||
        s.clusters.back().x + static_cast<double>(s.clusters.back().w) <= t) {
      Cluster c;
      c.e = 1.0;
      c.q = tx;
      c.w = w;
      c.x = t;
      c.first = static_cast<int>(s.cells.size()) - 1;
      s.clusters.push_back(c);
      return;
    }
    Cluster& last = s.clusters.back();
    last.e += 1.0;
    last.q += tx - static_cast<double>(last.w);
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

  Design& d_;
  Dbu rh_;
  std::vector<Row> rows_;
  std::vector<std::vector<std::pair<Dbu, Dbu>>> blocked_;
};

}  // namespace

CellLegalizeResult abacus_legalize(Design& design) {
  CellLegalizeResult res;
  const auto cells = design.cells();

  Dbu rh = design.row_height();
  if (rh <= 0) {
    for (const Cell& c : cells) {
      if (is_movable_std(c) && c.height > 0 && (rh <= 0 || c.height < rh)) rh = c.height;
    }
  }
  if (rh <= 0) return res;  // No movable std cells.

  std::vector<Point> orig(cells.size());
  for (std::size_t i = 0; i < cells.size(); ++i) orig[i] = cells[i].loc;

  Abacus ab(design, rh);

  std::vector<Id> single, multi;
  for (Id i = 0; i < static_cast<Id>(cells.size()); ++i) {
    const Cell& c = cells[i];
    if (!is_movable_std(c)) continue;
    (c.height > rh ? multi : single).push_back(i);
  }

  // Multi-row cells first (largest first), then they block rows.
  std::stable_sort(multi.begin(), multi.end(), [&](Id a, Id b) {
    return cells[a].width * cells[a].height > cells[b].width * cells[b].height;
  });
  auto clamp_in_die = [&](Id ci) {
    const Rect& die = design.die();
    const Cell& c = cells[ci];
    design.move_cell(ci, {std::clamp(c.loc.x, die.xlo, std::max(die.xlo, die.xhi - c.width)),
                          std::clamp(c.loc.y, die.ylo, std::max(die.ylo, die.yhi - c.height))});
  };
  for (Id ci : multi) {
    const int k = static_cast<int>((cells[ci].height + rh - 1) / rh);
    ++res.multi_row;
    if (ab.place_multi_row(ci, k)) ++res.legalized;
    else { ++res.failed; clamp_in_die(ci); }
  }

  // Abacus order: by target x (ties by id for determinism).
  std::stable_sort(single.begin(), single.end(),
                   [&](Id a, Id b) { return cells[a].loc.x < cells[b].loc.x; });
  std::vector<Id> unplaced;
  for (Id ci : single) {
    if (ab.place(ci)) ++res.legalized;
    else { ++res.failed; unplaced.push_back(ci); }
  }
  ab.finalize();
  for (Id ci : unplaced) clamp_in_die(ci);

  for (Id i = 0; i < static_cast<Id>(cells.size()); ++i) {
    if (!is_movable_std(cells[i])) continue;
    const Dbu d = manhattan(cells[i].loc, orig[i]);
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
  for (Id id = 0; id < static_cast<Id>(cells.size()); ++id) {
    const Cell& c = cells[id];
    if (c.removed) continue;
    const Rect b = c.bbox();
    if (b.xlo < die.xlo || b.ylo < die.ylo || b.xhi > die.xhi || b.yhi > die.yhi) {
      return fail("cell " + name(id) + " outside die");
    }
    if (row_height > 0 && is_movable_std(c) && (c.loc.y - die.ylo) % row_height != 0) {
      return fail("cell " + name(id) + " not on a row");
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
      if (a.overlaps(b)) {
        return fail("cells " + name(order[i]) + " and " + name(order[j]) +
                    " overlap");
      }
    }
  }
  return true;
}

}  // namespace hermes
