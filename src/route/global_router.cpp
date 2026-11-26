#include "hermes/route/global_router.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <format>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <queue>

#include "hermes/db/geometry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"

namespace hermes {

namespace {
HERMES_LOG_CHANNEL(log_grt, "route.global");
HERMES_LOG_CHANNEL(log_rrr, "route.global.rrr");
HERMES_LOG_CHANNEL(log_inc, "route.global.incr");
HERMES_LOG_CHANNEL(log_la, "route.global.layer");

using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

constexpr std::uint8_t kNoLayer = 0xff;

template <class T>
void atomic_add(T& slot, T v) {
  std::atomic_ref<T>(slot).fetch_add(v, std::memory_order_relaxed);
}

/// Inclusive GCell box.
struct GBox {
  int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
  bool empty() const { return x1 < x0 || y1 < y0; }
  void add(int x, int y) {
    if (empty()) {
      x0 = x1 = x;
      y0 = y1 = y;
      return;
    }
    x0 = std::min(x0, x);
    x1 = std::max(x1, x);
    y0 = std::min(y0, y);
    y1 = std::max(y1, y);
  }
  void add(const GBox& b) {
    if (b.empty()) return;
    add(b.x0, b.y0);
    add(b.x1, b.y1);
  }
  bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
  bool overlaps(const GBox& b) const {
    return !empty() && !b.empty() && b.x0 <= x1 && x0 <= b.x1 && b.y0 <= y1 && y0 <= b.y1;
  }
  GBox expanded(int m, int nx, int ny) const {
    if (empty()) return *this;
    return {std::max(0, x0 - m), std::max(0, y0 - m), std::min(nx - 1, x1 + m), std::min(ny - 1, y1 + m)};
  }
  GBox intersect(const GBox& b) const {
    return {std::max(x0, b.x0), std::max(y0, b.y0), std::min(x1, b.x1), std::min(y1, b.y1)};
  }
  long long area() const { return empty() ? 0 : static_cast<long long>(x1 - x0 + 1) * (y1 - y0 + 1); }
};

/// Per-thread search scratch, sized to the largest grid seen. Stamps avoid
/// clearing arrays between searches.
struct Scratch {
  std::vector<std::uint32_t> nstamp;  // dist/prev valid
  std::vector<float> dist;
  std::vector<int> prev;
  std::vector<std::uint32_t> estamp;  // edge is the current net's own
  std::vector<std::uint32_t> mstamp;  // node -> local id valid
  std::vector<int> local;
  std::vector<std::int8_t> lmin, lmax;  // via computation
  std::uint32_t ns = 0, es = 0, ms = 0;
  std::vector<std::pair<float, int>> heap;
  std::vector<int> uf;

  void ensure(std::size_t nodes, std::size_t edges) {
    if (nstamp.size() < nodes) {
      nstamp.assign(nodes, 0);
      dist.assign(nodes, 0.0f);
      prev.assign(nodes, -1);
      mstamp.assign(nodes, 0);
      local.assign(nodes, -1);
      lmin.assign(nodes, 0);
      lmax.assign(nodes, 0);
      ns = ms = 0;
    }
    if (estamp.size() < edges) {
      estamp.assign(edges, 0);
      es = 0;
    }
  }
  void next_net() {
    if (++es == 0) {
      std::fill(estamp.begin(), estamp.end(), 0);
      es = 1;
    }
    if (++ms == 0) {
      std::fill(mstamp.begin(), mstamp.end(), 0);
      ms = 1;
    }
  }
  void next_search() {
    if (++ns == 0) {
      std::fill(nstamp.begin(), nstamp.end(), 0);
      ns = 1;
    }
  }
  int find(int a) {
    while (uf[static_cast<std::size_t>(a)] != a) {
      uf[static_cast<std::size_t>(a)] = uf[static_cast<std::size_t>(uf[static_cast<std::size_t>(a)])];
      a = uf[static_cast<std::size_t>(a)];
    }
    return a;
  }
};

Scratch& scratch(const RouteGraph& g) {
  thread_local Scratch s;
  s.ensure(g.num_nodes(), g.num_edges());
  return s;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

/// Sorted distinct GCells of a net's pins.
void pin_nodes(const Design& d, const RouteGraph& g, Id net, std::vector<int>& out) {
  out.clear();
  for (Id p : d.net_pins(net)) {
    const auto [x, y] = g.grid().bin_of(pin_location(d, p));
    out.push_back(g.node(x, y));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

std::uint64_t pin_key(std::span<const int> nodes) {
  std::uint64_t h = 0x51ed2701ULL + nodes.size();
  for (int n : nodes) h = mix(h, static_cast<std::uint64_t>(n));
  return h;
}

bool net_live(const Design& d, Id n) { return static_cast<std::size_t>(n) < d.num_nets() && !d.net(n).removed; }

/// Tiers of run length (in GCells) for preferring layers: < 3 lowest, ...
int length_tier(int n) {
  int t = 0;
  for (int m = n + 1; m >= 4; m /= 4) ++t;
  return t;
}

struct Saved {
  Id net;
  std::vector<std::uint32_t> edges;
  std::vector<std::uint8_t> layers;
  std::uint64_t key;
  GBox box;
  std::uint32_t vias;
};

}  // namespace

// ============================================================================
// State
// ============================================================================

struct GlobalRouter::State {
  struct Listener final : DesignListener {
    State* s = nullptr;
    void on_flush(const Design& d, const ChangeSet& c) override {
      if (c.all || c.structure || c.floorplan) s->dirty_all = true;
      if (c.floorplan) s->floorplan_changed = true;
      for (Id n : c.nets.ids()) s->dirty.mark(n);
      for (Id ci : c.cells.ids())
        if (static_cast<std::size_t>(ci) < d.num_cells() && d.cell(ci).is_macro) s->macros_changed = true;
    }
  };

  RouteGraph g;
  bool routed = false;
  std::vector<std::vector<std::uint32_t>> edges;
  std::vector<std::vector<std::uint8_t>> layers;
  std::vector<std::uint64_t> key;
  std::vector<GBox> box;
  std::vector<std::uint32_t> vias;
  std::uint64_t macro_key = 0;
  std::vector<std::uint8_t> oflag;  // scratch: overflowed edge in the current window

  Listener listener;
  Design* attached = nullptr;
  DirtySet dirty;
  bool dirty_all = true, floorplan_changed = false, macros_changed = false;

  bool trial = false;
  DirtySet trial_set;
  std::vector<Saved> saved;

  State() { listener.s = this; }
  State(const State& o)
      : g(o.g), routed(o.routed), edges(o.edges), layers(o.layers), key(o.key), box(o.box), vias(o.vias),
        macro_key(o.macro_key) {
    listener.s = this;
  }
  ~State() {
    if (attached) attached->unsubscribe(&listener);
  }

  GBox full() const { return {0, 0, g.nx() - 1, g.ny() - 1}; }
  GBox gbox(const Rect& r) const {
    const auto [x0, y0] = g.grid().bin_of({r.xlo, r.ylo});
    // A rectangle's upper edge belongs to it only if it lies past a GCell's
    // lower boundary (exclusive upper bound, so abutting regions do not share
    // GCells).
    const auto [x1, y1] = g.grid().bin_of({std::max(r.xlo, r.xhi - 1), std::max(r.ylo, r.yhi - 1)});
    return {x0, y0, x1, y1};
  }
  bool edge_in(std::uint32_t e, const GBox& b) const {
    const auto [a, c] = g.ends(e);
    return b.contains(g.node_x(a), g.node_y(a)) && b.contains(g.node_x(c), g.node_y(c));
  }
  void resize_nets(std::size_t n) {
    if (edges.size() >= n) return;
    edges.resize(n);
    layers.resize(n);
    key.resize(n, 0);
    box.resize(n);
    vias.resize(n, 0);
  }
  void recompute_box(Id net, std::span<const int> pins) {
    GBox b;
    for (int p : pins) b.add(g.node_x(p), g.node_y(p));
    for (std::uint32_t e : edges[static_cast<std::size_t>(net)]) {
      const auto [a, c] = g.ends(e);
      b.add(g.node_x(a), g.node_y(a));
      b.add(g.node_x(c), g.node_y(c));
    }
    box[static_cast<std::size_t>(net)] = b;
  }
  void save(Id net) {
    if (!trial || trial_set.test(net)) return;
    trial_set.mark(net);
    const std::size_t i = static_cast<std::size_t>(net);
    saved.push_back({net, edges[i], layers[i], key[i], box[i], vias[i]});
  }
};

// ============================================================================
// Edge costs, pattern route, maze route
// ============================================================================

namespace {

struct Ctx {
  const RouteGraph& g;
  Scratch& sc;
  const GlobalRouterOptions& o;
  float pen;
};

inline float edge_cost(const Ctx& c, std::uint32_t e) {
  if (c.sc.estamp[e] == c.sc.es) return 0.0f;  // already part of this net
  const float over = static_cast<float>(c.g.use(e)) + 1.0f - c.g.cap(e);
  float cost = 1.0f + c.g.hist(e);
  if (over > 0.0f) cost *= 1.0f + c.pen * over;
  return cost;
}

float hrun(const Ctx& c, int y, int xa, int xb) {
  if (xa > xb) std::swap(xa, xb);
  float s = 0.0f;
  for (int x = xa; x < xb; ++x) s += edge_cost(c, c.g.h_edge(x, y));
  return s;
}
float vrun(const Ctx& c, int x, int ya, int yb) {
  if (ya > yb) std::swap(ya, yb);
  float s = 0.0f;
  for (int y = ya; y < yb; ++y) s += edge_cost(c, c.g.v_edge(x, y));
  return s;
}
void emit_h(const Ctx& c, int y, int xa, int xb, std::vector<std::uint32_t>& out) {
  if (xa > xb) std::swap(xa, xb);
  for (int x = xa; x < xb; ++x) {
    const std::uint32_t e = c.g.h_edge(x, y);
    if (c.sc.estamp[e] != c.sc.es) {
      c.sc.estamp[e] = c.sc.es;
      out.push_back(e);
    }
  }
}
void emit_v(const Ctx& c, int x, int ya, int yb, std::vector<std::uint32_t>& out) {
  if (ya > yb) std::swap(ya, yb);
  for (int y = ya; y < yb; ++y) {
    const std::uint32_t e = c.g.v_edge(x, y);
    if (c.sc.estamp[e] != c.sc.es) {
      c.sc.estamp[e] = c.sc.es;
      out.push_back(e);
    }
  }
}

/// Up to k positions strictly between a and b (exclusive), evenly spread.
template <class F>
void samples(int a, int b, int k, F&& f) {
  if (a > b) std::swap(a, b);
  const int n = b - a - 1;
  if (n <= 0) return;
  if (n <= k) {
    for (int v = a + 1; v < b; ++v) f(v);
    return;
  }
  for (int i = 1; i <= k; ++i) f(a + static_cast<int>(static_cast<long long>(i) * (n + 1) / (k + 1)));
}

/// Cheapest of the two L shapes and up to 2 x 6 Z shapes from u to v.
void pattern_route(const Ctx& c, int u, int v, std::vector<std::uint32_t>& out) {
  const int ax = c.g.node_x(u), ay = c.g.node_y(u), bx = c.g.node_x(v), by = c.g.node_y(v);
  if (ax == bx) return emit_v(c, ax, ay, by, out);
  if (ay == by) return emit_h(c, ay, ax, bx, out);
  const float bend = static_cast<float>(c.o.bend_cost);
  // kind: 0 = HV (L), 1 = VH (L), 2 = HVH at x = m, 3 = VHV at y = m.
  int best_kind = 0, best_m = 0;
  float best = hrun(c, ay, ax, bx) + vrun(c, bx, ay, by) + bend;
  if (const float s = vrun(c, ax, ay, by) + hrun(c, by, ax, bx) + bend; s < best) {
    best = s;
    best_kind = 1;
  }
  samples(ax, bx, 6, [&](int m) {
    const float s = hrun(c, ay, ax, m) + vrun(c, m, ay, by) + hrun(c, by, m, bx) + 2 * bend;
    if (s < best) {
      best = s;
      best_kind = 2;
      best_m = m;
    }
  });
  samples(ay, by, 6, [&](int m) {
    const float s = vrun(c, ax, ay, m) + hrun(c, m, ax, bx) + vrun(c, bx, m, by) + 2 * bend;
    if (s < best) {
      best = s;
      best_kind = 3;
      best_m = m;
    }
  });
  switch (best_kind) {
    case 0: emit_h(c, ay, ax, bx, out); emit_v(c, bx, ay, by, out); break;
    case 1: emit_v(c, ax, ay, by, out); emit_h(c, by, ax, bx, out); break;
    case 2: emit_h(c, ay, ax, best_m, out); emit_v(c, best_m, ay, by, out); emit_h(c, by, best_m, bx, out); break;
    default: emit_v(c, ax, ay, best_m, out); emit_h(c, best_m, ax, bx, out); emit_v(c, bx, best_m, by, out); break;
  }
}

/// A* from `sources` to `target` inside window w. Appends the path's new
/// edges (marking them own) and its nodes. False if unreachable.
bool maze_route(const Ctx& c, const GBox& w, std::span<const int> sources, int target,
                std::vector<std::uint32_t>& out, std::vector<int>* path_nodes) {
  Scratch& sc = c.sc;
  const RouteGraph& g = c.g;
  sc.next_search();
  auto& heap = sc.heap;
  heap.clear();
  const int tx = g.node_x(target), ty = g.node_y(target);
  auto h = [&](int n) { return static_cast<float>(std::abs(g.node_x(n) - tx) + std::abs(g.node_y(n) - ty)); };
  auto cmp = [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
    return a.first > b.first || (a.first == b.first && a.second > b.second);
  };
  for (int s : sources) {
    if (!w.contains(g.node_x(s), g.node_y(s))) continue;
    if (sc.nstamp[static_cast<std::size_t>(s)] == sc.ns) continue;
    sc.nstamp[static_cast<std::size_t>(s)] = sc.ns;
    sc.dist[static_cast<std::size_t>(s)] = 0.0f;
    sc.prev[static_cast<std::size_t>(s)] = -1;
    heap.push_back({h(s), s});
    std::push_heap(heap.begin(), heap.end(), cmp);
  }
  bool found = false;
  while (!heap.empty()) {
    std::pop_heap(heap.begin(), heap.end(), cmp);
    const auto [f, n] = heap.back();
    heap.pop_back();
    const float dn = sc.dist[static_cast<std::size_t>(n)];
    if (f > dn + h(n) + 1e-4f) continue;  // stale
    if (n == target) {
      found = true;
      break;
    }
    const int x = g.node_x(n), y = g.node_y(n);
    auto relax = [&](int m, std::uint32_t e) {
      const float nd = dn + edge_cost(c, e);
      const std::size_t mi = static_cast<std::size_t>(m);
      if (sc.nstamp[mi] != sc.ns || nd < sc.dist[mi]) {
        sc.nstamp[mi] = sc.ns;
        sc.dist[mi] = nd;
        sc.prev[mi] = n;
        heap.push_back({nd + h(m), m});
        std::push_heap(heap.begin(), heap.end(), cmp);
      }
    };
    if (x > w.x0) relax(n - 1, g.h_edge(x - 1, y));
    if (x < w.x1) relax(n + 1, g.h_edge(x, y));
    if (y > w.y0) relax(n - g.nx(), g.v_edge(x, y - 1));
    if (y < w.y1) relax(n + g.nx(), g.v_edge(x, y));
  }
  if (!found) return false;
  for (int n = target; sc.prev[static_cast<std::size_t>(n)] >= 0; n = sc.prev[static_cast<std::size_t>(n)]) {
    const int p = sc.prev[static_cast<std::size_t>(n)];
    const std::uint32_t e = g.edge_between(n, p);
    if (sc.estamp[e] != sc.es) {
      sc.estamp[e] = sc.es;
      out.push_back(e);
    }
    if (path_nodes) path_nodes->push_back(p);
  }
  if (path_nodes) path_nodes->push_back(target);
  return true;
}

enum class Mode { Pattern, Maze };

/// Connects a net inside fence F. `kept` are the net's route edges that stay
/// (not inside F); `pins` its pin GCells. Appends the new edges (all inside
/// F, sorted, distinct) to `added`. Returns false if some part of the net
/// cannot be reached from inside F (then `added` is meaningless).
bool connect_net(const Ctx& c, std::span<const int> pins, std::span<const std::uint32_t> kept, const GBox& F,
                 Mode mode, std::vector<std::uint32_t>& added) {
  const RouteGraph& g = c.g;
  Scratch& sc = c.sc;
  added.clear();
  // Local ids for terminals: pins, then kept-edge endpoints.
  std::vector<int> nodes;
  auto local = [&](int n) {
    const std::size_t ni = static_cast<std::size_t>(n);
    if (sc.mstamp[ni] != sc.ms) {
      sc.mstamp[ni] = sc.ms;
      sc.local[ni] = static_cast<int>(nodes.size());
      nodes.push_back(n);
      sc.uf.push_back(static_cast<int>(sc.uf.size()));
    }
    return sc.local[ni];
  };
  sc.uf.clear();
  for (int p : pins) local(p);
  for (std::uint32_t e : kept) {
    sc.estamp[e] = sc.es;
    const auto [a, b] = g.ends(e);
    const int la = local(a), lb = local(b);
    const int ra = sc.find(la), rb = sc.find(lb);
    if (ra != rb) sc.uf[static_cast<std::size_t>(std::max(ra, rb))] = std::min(ra, rb);
  }
  // Terminals inside F; every component needs one.
  std::vector<int> term;  // local ids
  std::size_t ncomp = 0;
  {
    std::vector<std::uint8_t> has_in(nodes.size(), 0), is_root(nodes.size(), 0);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      const int r = sc.find(static_cast<int>(i));
      if (!is_root[static_cast<std::size_t>(r)]) {
        is_root[static_cast<std::size_t>(r)] = 1;
        ++ncomp;
      }
      if (F.contains(g.node_x(nodes[i]), g.node_y(nodes[i]))) {
        has_in[static_cast<std::size_t>(r)] = 1;
        term.push_back(static_cast<int>(i));
      }
    }
    if (ncomp <= 1) return true;
    for (std::size_t i = 0; i < nodes.size(); ++i)
      if (is_root[i] && !has_in[i]) return false;
  }
  // Prim over the terminals (distance 0 within a component); route each
  // tree edge that joins two components.
  const std::size_t T = term.size();
  std::vector<int> best(T, std::numeric_limits<int>::max()), parent(T, -1);
  std::vector<std::uint8_t> in(T, 0);
  auto dist = [&](std::size_t i, std::size_t j) {
    if (sc.find(term[i]) == sc.find(term[j])) return 0;
    const int a = nodes[static_cast<std::size_t>(term[i])], b = nodes[static_cast<std::size_t>(term[j])];
    return std::abs(g.node_x(a) - g.node_x(b)) + std::abs(g.node_y(a) - g.node_y(b));
  };
  std::vector<int> tree;  // nodes of the connected tree (maze sources)
  const bool use_tree = pins.size() <= static_cast<std::size_t>(c.o.big_net_pins);
  in[0] = 1;
  tree.push_back(nodes[static_cast<std::size_t>(term[0])]);
  for (std::size_t j = 1; j < T; ++j) {
    best[j] = dist(0, j);
    parent[j] = 0;
  }
  std::vector<int> srcs;
  for (std::size_t step = 1; step < T; ++step) {
    std::size_t v = T;
    for (std::size_t j = 0; j < T; ++j)
      if (!in[j] && (v == T || best[j] < best[v])) v = j;
    in[v] = 1;
    const std::size_t u = static_cast<std::size_t>(parent[v]);
    const int un = nodes[static_cast<std::size_t>(term[u])], vn = nodes[static_cast<std::size_t>(term[v])];
    const int ru = sc.find(term[u]), rv = sc.find(term[v]);
    if (ru != rv) {
      const std::size_t before = added.size();
      bool ok = false;
      if (mode == Mode::Maze) {
        GBox w;
        w.add(g.node_x(un), g.node_y(un));
        w.add(g.node_x(vn), g.node_y(vn));
        w = w.expanded(c.o.maze_margin, g.nx(), g.ny()).intersect(F);
        srcs.clear();
        srcs.push_back(un);
        if (use_tree)
          for (int t : tree)
            if (w.contains(g.node_x(t), g.node_y(t))) srcs.push_back(t);
        ok = maze_route(c, w, srcs, vn, added, use_tree ? &tree : nullptr);
      }
      if (!ok) {
        pattern_route(c, un, vn, added);
        if (use_tree)
          for (std::size_t k = before; k < added.size(); ++k) {
            const auto [a, b] = g.ends(added[k]);
            tree.push_back(a);
            tree.push_back(b);
          }
      }
      sc.uf[static_cast<std::size_t>(std::max(ru, rv))] = std::min(ru, rv);
    }
    if (use_tree) tree.push_back(vn);
    for (std::size_t j = 0; j < T; ++j)
      if (!in[j]) {
        const int dj = dist(v, j);
        if (dj < best[j]) {
          best[j] = dj;
          parent[j] = static_cast<int>(v);
        }
      }
  }
  std::sort(added.begin(), added.end());
  added.erase(std::unique(added.begin(), added.end()), added.end());
  return true;
}

/// Greedy layer assignment of a net's unassigned edges, run by run.
void assign_layers(RouteGraph& g, std::span<const std::uint32_t> edges, std::span<std::uint8_t> layers) {
  std::vector<std::size_t> hs, vs;
  for (std::size_t i = 0; i < edges.size(); ++i)
    if (layers[i] == kNoLayer) (g.is_h(edges[i]) ? hs : vs).push_back(i);
  // Vertical edges ordered by column, then row.
  auto vkey = [&](std::size_t i) {
    const int n = static_cast<int>(edges[i] - g.num_h_edges());
    return static_cast<long long>(g.node_x(n)) * g.ny() + g.node_y(n);
  };
  std::sort(vs.begin(), vs.end(), [&](std::size_t a, std::size_t b) { return vkey(a) < vkey(b); });
  auto do_run = [&](std::span<const std::size_t> run, const std::vector<int>& ls) {
    if (ls.empty()) return;
    const int pref = std::min(length_tier(static_cast<int>(run.size())), static_cast<int>(ls.size()) - 1);
    int best_l = ls[0];
    double best = std::numeric_limits<double>::max();
    for (int r = 0; r < static_cast<int>(ls.size()); ++r) {
      const int l = ls[static_cast<std::size_t>(r)];
      double ovf = 0.0, util = 0.0;
      for (std::size_t i : run) {
        const std::uint32_t e = edges[i];
        const double cap = g.layer_cap(l, e), u = g.layer_use(l, e) + 1.0;
        if (u > cap + 1e-4) ovf += u - cap;
        util += cap > 0 ? u / cap : 10.0;
      }
      const double s = 100.0 * ovf + util / static_cast<double>(run.size()) + 0.25 * std::abs(r - pref);
      if (s < best) {
        best = s;
        best_l = l;
      }
    }
    auto& lu = g.layer_use_vec(best_l);
    for (std::size_t i : run) {
      ++lu[edges[i]];
      layers[i] = static_cast<std::uint8_t>(best_l);
    }
  };
  const std::uint32_t rowlen = static_cast<std::uint32_t>(std::max(1, g.nx() - 1));
  for (std::size_t a = 0; a < hs.size();) {
    std::size_t b = a + 1;
    while (b < hs.size() && edges[hs[b]] == edges[hs[b - 1]] + 1 && edges[hs[b]] % rowlen != 0) ++b;
    do_run(std::span<const std::size_t>(hs.data() + a, b - a), g.h_layers());
    a = b;
  }
  for (std::size_t a = 0; a < vs.size();) {
    std::size_t b = a + 1;
    while (b < vs.size() && edges[vs[b]] == edges[vs[b - 1]] + static_cast<std::uint32_t>(g.nx())) ++b;
    do_run(std::span<const std::size_t>(vs.data() + a, b - a), g.v_layers());
    a = b;
  }
}

/// Vias of one net: at each GCell, from its lowest to its highest layer
/// (pins are on layer 0). `f(node, lo, hi)` per GCell with hi > lo.
template <class F>
std::uint32_t net_vias(const RouteGraph& g, Scratch& sc, std::span<const int> pins,
                       std::span<const std::uint32_t> edges, std::span<const std::uint8_t> layers, F&& f) {
  sc.next_net();
  std::vector<int> touched;
  auto touch = [&](int n, int l) {
    const std::size_t ni = static_cast<std::size_t>(n);
    if (sc.mstamp[ni] != sc.ms) {
      sc.mstamp[ni] = sc.ms;
      sc.lmin[ni] = sc.lmax[ni] = static_cast<std::int8_t>(l);
      touched.push_back(n);
    } else {
      sc.lmin[ni] = std::min<std::int8_t>(sc.lmin[ni], static_cast<std::int8_t>(l));
      sc.lmax[ni] = std::max<std::int8_t>(sc.lmax[ni], static_cast<std::int8_t>(l));
    }
  };
  for (std::size_t i = 0; i < edges.size(); ++i) {
    const int l = layers[i] == kNoLayer ? g.layers_of(edges[i]).front() : layers[i];
    const auto [a, b] = g.ends(edges[i]);
    touch(a, l);
    touch(b, l);
  }
  if (!edges.empty())
    for (int p : pins) touch(p, 0);
  std::uint32_t v = 0;
  for (int n : touched) {
    const int lo = sc.lmin[static_cast<std::size_t>(n)], hi = sc.lmax[static_cast<std::size_t>(n)];
    if (hi > lo) {
      v += static_cast<std::uint32_t>(hi - lo);
      f(n, lo, hi);
    }
  }
  return v;
}

/// Removes the net's edges inside F from the usage (and its route).
/// Returns the removed edges and their layers (to restore on failure).
void rip(GlobalRouter::State& s, Id net, const GBox& F, std::vector<std::uint32_t>& removed,
         std::vector<std::uint8_t>& removed_layers) {
  s.save(net);
  const std::size_t i = static_cast<std::size_t>(net);
  auto& E = s.edges[i];
  auto& Lv = s.layers[i];
  removed.clear();
  removed_layers.clear();
  const bool all = F.x0 == 0 && F.y0 == 0 && F.x1 == s.g.nx() - 1 && F.y1 == s.g.ny() - 1;
  std::size_t w = 0;
  auto& use = s.g.use_vec();
  for (std::size_t k = 0; k < E.size(); ++k) {
    const std::uint32_t e = E[k];
    if (all || s.edge_in(e, F)) {
      --use[e];
      if (Lv[k] != kNoLayer) --s.g.layer_use_vec(Lv[k])[e];
      removed.push_back(e);
      removed_layers.push_back(Lv[k]);
    } else {
      E[w] = e;
      Lv[w] = Lv[k];
      ++w;
    }
  }
  E.resize(w);
  Lv.resize(w);
}

/// Adds `added` (sorted, inside F, disjoint from the net's edges) to the net
/// and the usage; with `la`, assigns their layers. `add_layers` (optional)
/// gives fixed layers instead (restore).
void add(GlobalRouter::State& s, Id net, std::span<const std::uint32_t> added, const std::uint8_t* add_layers,
         bool la) {
  const std::size_t i = static_cast<std::size_t>(net);
  auto& E = s.edges[i];
  auto& Lv = s.layers[i];
  auto& use = s.g.use_vec();
  std::vector<std::uint32_t> ne;
  std::vector<std::uint8_t> nl;
  ne.reserve(E.size() + added.size());
  nl.reserve(E.size() + added.size());
  std::size_t a = 0, b = 0;
  while (a < E.size() || b < added.size()) {
    if (b == added.size() || (a < E.size() && E[a] < added[b])) {
      ne.push_back(E[a]);
      nl.push_back(Lv[a]);
      ++a;
    } else {
      const std::uint32_t e = added[b];
      ++use[e];
      ne.push_back(e);
      std::uint8_t l = add_layers ? add_layers[b] : kNoLayer;
      if (l != kNoLayer) ++s.g.layer_use_vec(l)[e];
      nl.push_back(l);
      ++b;
    }
  }
  E.swap(ne);
  Lv.swap(nl);
  if (la && !add_layers) assign_layers(s.g, E, Lv);
}

}  // namespace

// ============================================================================
// GlobalRouter
// ============================================================================

GlobalRouter::GlobalRouter(GlobalRouterOptions opts) : opts_(opts), st_(std::make_unique<State>()) {}
GlobalRouter::~GlobalRouter() = default;
GlobalRouter::GlobalRouter(const GlobalRouter& o) : opts_(o.opts_), st_(std::make_unique<State>(*o.st_)) {}
GlobalRouter& GlobalRouter::operator=(const GlobalRouter& o) {
  if (this != &o) {
    opts_ = o.opts_;
    st_ = std::make_unique<State>(*o.st_);
  }
  return *this;
}
GlobalRouter::GlobalRouter(GlobalRouter&&) noexcept = default;
GlobalRouter& GlobalRouter::operator=(GlobalRouter&&) noexcept = default;

BinGrid GlobalRouter::gcell_grid(const Design& design) const {
  if (!design.route_grid().empty()) return route_gcell_grid(design, {});
  const Rect& die = design.die();
  if (opts_.grid_x > 0 && opts_.grid_y > 0) return BinGrid(die, opts_.grid_x, opts_.grid_y);
  const double w = static_cast<double>(std::max<Dbu>(1, die.width()));
  const double h = static_cast<double>(std::max<Dbu>(1, die.height()));
  double side = design.row_height() > 0 ? opts_.gcell_rows * static_cast<double>(design.row_height())
                                        : std::max(w, h) / 64.0;
  side = std::max(side, 1.0);
  auto n = [&](double len) {
    return static_cast<int>(std::clamp<double>(std::round(len / side), 1.0, std::max(1, opts_.max_grid)));
  };
  return BinGrid(die, opts_.grid_x > 0 ? opts_.grid_x : n(w), opts_.grid_y > 0 ? opts_.grid_y : n(h));
}

namespace {
RouteUsageOptions usage_options(const GlobalRouterOptions& o, const BinGrid& g) {
  RouteUsageOptions u;
  u.grid_x = g.nx();
  u.grid_y = g.ny();
  u.first_routing_layer = o.first_routing_layer;
  u.macro_blocked_layers = o.macro_blocked_layers;
  return u;
}
std::uint64_t macro_key(const Design& d) {
  std::uint64_t h = 0x6d61;
  for (const Cell& c : d.cells())
    if (c.is_macro && !c.removed) {
      const Rect b = c.bbox();
      h = mix(mix(mix(mix(h, static_cast<std::uint64_t>(b.xlo)), static_cast<std::uint64_t>(b.ylo)),
                  static_cast<std::uint64_t>(b.xhi)),
              static_cast<std::uint64_t>(b.yhi));
    }
  return h;
}
}  // namespace

CongestionMap GlobalRouter::make_map(const Design& design) const {
  const BinGrid g = gcell_grid(design);
  if (opts_.capacity_per_gcell > 0.0) {
    CongestionMap map(g.region(), g.nx(), g.ny(), opts_.capacity_per_gcell);
    apply_macro_blockages(design, map, opts_.macro_blockage);
    return map;
  }
  // Tracks: the sum over all routable layers of the GCell's capacity.
  const RouteUsageOptions uo = usage_options(opts_, g);
  const LayerStack st = layer_stack_for(design, uo);
  const auto cap = layer_capacity(design, st, g, uo);
  CongestionMap map(g.region(), g.nx(), g.ny(), 0.0);
  for (int iy = 0; iy < g.ny(); ++iy)
    for (int ix = 0; ix < g.nx(); ++ix) {
      double c = 0.0;
      for (const auto& l : cap) c += l.empty() ? 0.0 : l[g.index(ix, iy)];
      map.set_capacity(ix, iy, c);
    }
  return map;
}

void GlobalRouter::estimate_congestion(const Design& design, CongestionMap& map) const {
  estimate_rudy(design, map);
  if (opts_.capacity_per_gcell > 0.0) return;
  // RUDY deposits wire length; a GCell's tracks are used by length / GCell
  // side (half the wire taken as horizontal, half vertical).
  const double bw = static_cast<double>(map.grid().bin_width()), bh = static_cast<double>(map.grid().bin_height());
  map.scale_demand(0.5 * (1.0 / bw + 1.0 / bh));
}

// ---------------------------------------------------------------------------
// Rip-up and reroute
// ---------------------------------------------------------------------------

namespace {

struct Scope {
  GBox window;          // overflow counted on edges inside
  GBox fence;           // nets change only inside
  bool all_nets = true; // else nets whose box overlaps the window
  bool la = false;      // assign layers inline
  int iterations = 0;
};

struct NegotiateStats {
  int iterations = 0;
  std::size_t ripped = 0;
  bool cancelled = false;
};

/// Overflow in a window; marks overflowed edges in s.oflag (and returns the
/// marked list) and, with `hist`, raises their history.
double scan_overflow(GlobalRouter::State& s, const GBox& w, double hist_inc, std::vector<std::uint32_t>* marked) {
  RouteGraph& g = s.g;
  if (s.oflag.size() < g.num_edges()) s.oflag.assign(g.num_edges(), 0);
  auto& hist = g.hist_vec();
  const std::size_t rows = static_cast<std::size_t>(w.y1 - w.y0 + 1);
  std::vector<std::vector<std::uint32_t>> parts(rows);
  const double total = par::parallel_sum(0, rows, 4, [&](std::size_t lo, std::size_t hi) {
    double sum = 0.0;
    for (std::size_t r = lo; r < hi; ++r) {
      const int y = w.y0 + static_cast<int>(r);
      auto visit = [&](std::uint32_t e) {
        const float o = g.overflow(e);
        if (o <= 0.0f) return;
        sum += o;
        if (hist_inc > 0) hist[e] += static_cast<float>(hist_inc * o);
        if (marked) {
          s.oflag[e] = 1;
          parts[r].push_back(e);
        }
      };
      for (int x = w.x0; x < w.x1; ++x) visit(g.h_edge(x, y));
      if (y < w.y1)
        for (int x = w.x0; x <= w.x1; ++x) visit(g.v_edge(x, y));
    }
    return sum;
  });
  if (marked) {
    marked->clear();
    for (auto& p : parts) marked->insert(marked->end(), p.begin(), p.end());
  }
  return total;
}

/// Routes `nets` in parallel batches whose windows share no tile.
/// `work(net, scratch)` reroutes one net. Returns false if cancelled.
template <class Work>
bool run_batches(GlobalRouter::State& s, std::span<const Id> nets, const GBox& fence, int margin, RunControl* ctl,
                 Work&& work) {
  const RouteGraph& g = s.g;
  constexpr int kTile = 8;
  const int tx = (g.nx() + kTile - 1) / kTile, ty = (g.ny() + kTile - 1) / kTile;
  std::vector<std::uint64_t> mask(static_cast<std::size_t>(tx) * ty);
  std::vector<Id> pending(nets.begin(), nets.end()), next;
  std::vector<std::vector<Id>> batches;
  while (!pending.empty()) {
    std::fill(mask.begin(), mask.end(), 0);
    std::vector<std::vector<Id>> round(64);
    next.clear();
    for (Id n : pending) {
      const GBox w = s.box[static_cast<std::size_t>(n)].expanded(margin, g.nx(), g.ny()).intersect(fence);
      if (w.empty()) {
        round[0].push_back(n);
        continue;
      }
      const int a0 = w.x0 / kTile, a1 = w.x1 / kTile, b0 = w.y0 / kTile, b1 = w.y1 / kTile;
      std::uint64_t used = 0;
      for (int b = b0; b <= b1; ++b)
        for (int a = a0; a <= a1; ++a) used |= mask[static_cast<std::size_t>(b) * tx + a];
      if (used == ~std::uint64_t{0}) {
        next.push_back(n);
        continue;
      }
      const int k = std::countr_one(used);
      const std::uint64_t bit = std::uint64_t{1} << k;
      for (int b = b0; b <= b1; ++b)
        for (int a = a0; a <= a1; ++a) mask[static_cast<std::size_t>(b) * tx + a] |= bit;
      round[static_cast<std::size_t>(k)].push_back(n);
    }
    for (auto& r : round)
      if (!r.empty()) batches.push_back(std::move(r));
    pending.swap(next);
  }
  for (const auto& b : batches) {
    if (ctl && ctl->cancelled()) return false;
    par::parallel_for(0, b.size(), 8, [&](std::size_t lo, std::size_t hi) {
      Scratch& sc = scratch(s.g);
      for (std::size_t i = lo; i < hi; ++i) work(b[i], sc);
    });
  }
  return true;
}

/// Rip up and reroute one net inside F with the maze; restores it if it
/// cannot be connected inside F. Returns false then.
bool reroute_one(GlobalRouter::State& s, const Design& d, const GlobalRouterOptions& o, float pen, Id net,
                 const GBox& F, bool la, Scratch& sc) {
  std::vector<int> pins;
  pin_nodes(d, s.g, net, pins);
  std::vector<std::uint32_t> removed, added;
  std::vector<std::uint8_t> removed_l;
  rip(s, net, F, removed, removed_l);
  sc.next_net();
  const Ctx c{s.g, sc, o, pen};
  const std::size_t i = static_cast<std::size_t>(net);
  if (!connect_net(c, pins, s.edges[i], F, Mode::Maze, added)) {
    add(s, net, removed, removed_l.data(), false);
    return false;
  }
  add(s, net, added, nullptr, la);
  s.recompute_box(net, pins);
  if (la) s.vias[i] = net_vias(s.g, sc, pins, s.edges[i], s.layers[i], [](int, int, int) {});
  return true;
}

NegotiateStats negotiate(GlobalRouter::State& s, const Design& d, const GlobalRouterOptions& o, const Scope& sc,
                         RunControl* ctl, double prog_lo, double prog_hi, double pen0) {
  NegotiateStats st;
  float pen = static_cast<float>(pen0);
  std::vector<std::uint32_t> marked;
  const std::size_t nn = std::min(s.edges.size(), d.num_nets());
  double best_of = std::numeric_limits<double>::max();
  int stall = 0;
  for (int it = 0; it < sc.iterations; ++it) {
    if (ctl && ctl->cancelled()) {
      st.cancelled = true;
      break;
    }
    const auto t0 = Clock::now();
    const double of = scan_overflow(s, sc.window, o.history_increment, &marked);
    if (of <= 0.0) break;
    if (of < best_of * 0.99) {
      best_of = of;
      stall = 0;
    } else if (o.stall_iterations > 0 && ++stall >= o.stall_iterations) {
      for (std::uint32_t e : marked) s.oflag[e] = 0;
      HLOG_DEBUG_ID(log_rrr, "GRT-0011", "rrr stopped after {} iterations: overflow {:.4g} (best {:.4g}) stalled", it,
                    of, best_of);
      break;
    }
    // Nets that use an overflowed edge.
    std::vector<std::vector<Id>> parts(par::block_count(nn, 4096));
    const std::size_t bs = parts.empty() ? 0 : (nn + parts.size() - 1) / parts.size();
    par::parallel_for_each(0, parts.size(), [&](std::size_t b) {
      for (std::size_t n = b * bs; n < std::min(nn, (b + 1) * bs); ++n) {
        if (!sc.all_nets && !s.box[n].overlaps(sc.window)) continue;
        for (std::uint32_t e : s.edges[n])
          if (s.oflag[e]) {
            parts[b].push_back(static_cast<Id>(n));
            break;
          }
      }
    });
    for (std::uint32_t e : marked) s.oflag[e] = 0;
    std::vector<Id> cand;
    for (auto& p : parts) cand.insert(cand.end(), p.begin(), p.end());
    std::stable_sort(cand.begin(), cand.end(), [&](Id a, Id b) {
      return s.box[static_cast<std::size_t>(a)].area() > s.box[static_cast<std::size_t>(b)].area();
    });
    std::atomic<std::size_t> skipped{0};
    const bool done = run_batches(s, cand, sc.fence, o.maze_margin, ctl, [&](Id n, Scratch& scr) {
      if (!reroute_one(s, d, o, pen, n, sc.fence, sc.la, scr)) skipped.fetch_add(1, std::memory_order_relaxed);
    });
    st.ripped += cand.size();
    ++st.iterations;
    HLOG_DEBUG_ID(log_rrr, "GRT-0010",
                  "rrr iteration {}: overflow {:.4g} on {} edges, rerouted {} nets ({} kept), penalty {:.3g}, {:.3f} s",
                  it + 1, of, marked.size(), cand.size(), skipped.load(), pen, since(t0));
    if (ctl) ctl->stage_progress(prog_lo + (prog_hi - prog_lo) * (it + 1) / std::max(1, sc.iterations),
                                 std::format("rip-up and reroute {}: overflow {:.4g}", it + 1, of));
    if (!done) {
      st.cancelled = true;
      break;
    }
    pen = std::min(static_cast<float>(o.max_penalty), pen * static_cast<float>(o.penalty_growth));
  }
  return st;
}

}  // namespace

GlobalRouteResult GlobalRouter::route(const Design& design, RunControl* ctl) {
  State& s = *st_;
  const auto t_all = Clock::now();
  GlobalRouteResult res;
  auto t0 = Clock::now();
  const BinGrid grid = gcell_grid(design);
  s.g = RouteGraph(design, grid, usage_options(opts_, grid));
  const std::size_t nn = design.num_nets();
  s.edges.assign(nn, {});
  s.layers.assign(nn, {});
  s.key.assign(nn, 0);
  s.box.assign(nn, {});
  s.vias.assign(nn, 0);
  s.macro_key = macro_key(design);
  s.trial = false;
  s.saved.clear();
  s.trial_set.clear();
  s.dirty.clear();
  s.dirty_all = s.floorplan_changed = s.macros_changed = false;
  s.routed = true;
  res.seconds.emplace_back("graph", since(t0));
  HLOG_DEBUG_ID(log_grt, "GRT-0001", "graph: {}x{} GCells ({}x{} DBU), {} layers ({}), {} edges, {:.3f} s",
                s.g.nx(), s.g.ny(), grid.bin_width(), grid.bin_height(), s.g.num_layers(), s.g.stack().source,
                s.g.num_edges(), since(t0));

  // Pins, keys, boxes; order nets by pin box half-perimeter.
  t0 = Clock::now();
  std::vector<std::uint32_t> hp(nn, 0);
  par::parallel_for(0, nn, 1024, [&](std::size_t lo, std::size_t hi) {
    std::vector<int> pins;
    for (std::size_t n = lo; n < hi; ++n) {
      if (design.net(static_cast<Id>(n)).removed) continue;
      pin_nodes(design, s.g, static_cast<Id>(n), pins);
      s.key[n] = pin_key(pins);
      s.recompute_box(static_cast<Id>(n), pins);
      const GBox& b = s.box[n];
      hp[n] = pins.size() < 2 ? 0 : static_cast<std::uint32_t>(b.x1 - b.x0 + b.y1 - b.y0);
    }
  });
  std::vector<Id> order;
  order.reserve(nn);
  for (std::size_t n = 0; n < nn; ++n)
    if (hp[n] > 0) order.push_back(static_cast<Id>(n));
  std::stable_sort(order.begin(), order.end(), [&](Id a, Id b) { return hp[a] < hp[b]; });
  res.nets_routed = order.size();

  // Initial pattern route in rounds: a round reads the usage committed by
  // the rounds before it (deterministic), then commits.
  if (ctl) ctl->stage_progress(0.0, "pattern route");
  const int rounds = std::max(1, opts_.initial_rounds);
  const float pen0 = static_cast<float>(opts_.overflow_penalty);
  for (int r = 0; r < rounds; ++r) {
    if (ctl && ctl->cancelled()) break;
    const std::size_t lo = order.size() * static_cast<std::size_t>(r) / rounds;
    const std::size_t hi = order.size() * static_cast<std::size_t>(r + 1) / rounds;
    par::parallel_for(lo, hi, 256, [&](std::size_t a, std::size_t b) {
      Scratch& sc = scratch(s.g);
      std::vector<int> pins;
      for (std::size_t k = a; k < b; ++k) {
        const Id n = order[k];
        pin_nodes(design, s.g, n, pins);
        sc.next_net();
        const Ctx c{s.g, sc, opts_, pen0};
        auto& E = s.edges[static_cast<std::size_t>(n)];
        connect_net(c, pins, {}, s.full(), Mode::Pattern, E);
        s.layers[static_cast<std::size_t>(n)].assign(E.size(), kNoLayer);
      }
    });
    auto& use = s.g.use_vec();
    par::parallel_for(lo, hi, 1024, [&](std::size_t a, std::size_t b) {
      for (std::size_t k = a; k < b; ++k)
        for (std::uint32_t e : s.edges[static_cast<std::size_t>(order[k])]) atomic_add(use[e], 1);
    });
  }
  res.seconds.emplace_back("pattern route", since(t0));
  if (HLOG_ENABLED(log_grt, Debug)) {
    const GlobalRouteResult p = stats();
    HLOG_DEBUG_ID(log_grt, "GRT-0002", "pattern route: {} nets, wirelength {:.6g}, overflow {:.4g} on {} edges, {:.3f} s",
                  order.size(), p.wirelength, p.total_overflow, p.overflow_edges, since(t0));
  }

  // Negotiated rip-up and reroute.
  t0 = Clock::now();
  Scope sc;
  sc.window = sc.fence = s.full();
  sc.iterations = opts_.max_iterations;
  const NegotiateStats ns = negotiate(s, design, opts_, sc, ctl, 0.1, 0.9, opts_.overflow_penalty);
  res.iterations = ns.iterations;
  res.cancelled = ns.cancelled || (ctl && ctl->cancelled());
  res.seconds.emplace_back("rip-up and reroute", since(t0));

  // Layer assignment.
  t0 = Clock::now();
  if (opts_.layer_assignment && !res.cancelled) {
    if (ctl) ctl->stage_progress(0.9, "layer assignment");
    for (int l = 0; l < s.g.num_layers(); ++l) {
      auto& lu = s.g.layer_use_vec(l);
      std::fill(lu.begin(), lu.end(), 0);
    }
    // Long nets first: they need the upper layers most.
    std::vector<Id> la_order = order;
    std::stable_sort(la_order.begin(), la_order.end(), [&](Id a, Id b) {
      return s.edges[static_cast<std::size_t>(a)].size() > s.edges[static_cast<std::size_t>(b)].size();
    });
    for (Id n : la_order) {
      auto& Lv = s.layers[static_cast<std::size_t>(n)];
      std::fill(Lv.begin(), Lv.end(), kNoLayer);
      assign_layers(s.g, s.edges[static_cast<std::size_t>(n)], Lv);
    }
    par::parallel_for(0, nn, 1024, [&](std::size_t lo, std::size_t hi) {
      Scratch& scr = scratch(s.g);
      std::vector<int> pins;
      for (std::size_t n = lo; n < hi; ++n) {
        if (s.edges[n].empty()) continue;
        pin_nodes(design, s.g, static_cast<Id>(n), pins);
        s.vias[n] = net_vias(s.g, scr, pins, s.edges[n], s.layers[n], [](int, int, int) {});
      }
    });
    res.seconds.emplace_back("layer assignment", since(t0));
  }
  const GlobalRouteResult t = stats();
  res.total_overflow = t.total_overflow;
  res.max_utilization = t.max_utilization;
  res.wirelength = t.wirelength;
  res.overflow_edges = t.overflow_edges;
  res.vias = t.vias;
  res.layer_overflow = t.layer_overflow;
  HLOG_INFO_ID(log_grt, "GRT-0003",
               "global route: {} nets on {}x{} GCells, wirelength {:.6g}, vias {}, overflow {:.4g} on {} edges "
               "(3-D {:.4g}), {} rrr iterations, {:.3f} s{}",
               res.nets_routed, s.g.nx(), s.g.ny(), res.wirelength, res.vias, res.total_overflow, res.overflow_edges,
               res.layer_overflow, res.iterations, since(t_all), res.cancelled ? " (cancelled)" : "");
  for (const auto& [k, v] : res.seconds) HLOG_DEBUG(log_grt, "  {}: {:.3f} s", k, v);
  return res;
}

GlobalRouteResult GlobalRouter::route(const Design& design, CongestionMap& map, RunControl* ctl) {
  GlobalRouteResult r = route(design, ctl);
  const RouteGraph& g = st_->g;
  if (map.nx() != g.nx() || map.ny() != g.ny() || map.region() != g.grid().region()) map = make_map(design);
  map.clear_demand();
  const bool tracks = opts_.capacity_per_gcell <= 0.0;
  for (int y = 0; y < g.ny(); ++y)
    for (int x = 0; x < g.nx(); ++x) {
      double dsum = 0.0;
      auto inc = [&](std::uint32_t e) { dsum += 0.5 * g.use(e) * (tracks ? 1.0 : g.edge_length(e)); };
      if (x > 0) inc(g.h_edge(x - 1, y));
      if (x + 1 < g.nx()) inc(g.h_edge(x, y));
      if (y > 0) inc(g.v_edge(x, y - 1));
      if (y + 1 < g.ny()) inc(g.v_edge(x, y));
      if (dsum != 0.0) map.add_demand(x, y, dsum);
    }
  return r;
}

// ---------------------------------------------------------------------------
// Incremental
// ---------------------------------------------------------------------------

RerouteResult GlobalRouter::reroute(const Design& design, const RerouteRequest& req, RunControl* ctl) {
  State& s = *st_;
  RerouteResult res;
  const auto t0 = Clock::now();
  if (!s.routed) {
    const GlobalRouteResult r = route(design, ctl);
    res.nets_rerouted = r.nets_routed;
    res.total_overflow = r.total_overflow;
    res.cancelled = r.cancelled;
    res.seconds = since(t0);
    return res;
  }
  s.resize_nets(design.num_nets());
  const GBox F = req.fence ? s.gbox(*req.fence).intersect(s.full()) : s.full();
  const double wl_before = stats().wirelength;  // TODO: per-net deltas if this shows up in profiles

  // Selected live nets; removed nets are just ripped up.
  std::vector<Id> nets;
  nets.reserve(req.nets.size());
  {
    DirtySet seen;
    std::vector<std::uint32_t> rem;
    std::vector<std::uint8_t> reml;
    for (Id n : req.nets) {
      if (static_cast<std::size_t>(n) >= s.edges.size() || seen.test(n)) continue;
      seen.mark(n);
      if (!net_live(design, n)) {
        rip(s, n, s.full(), rem, reml);
        s.box[static_cast<std::size_t>(n)] = {};
        s.key[static_cast<std::size_t>(n)] = 0;
        s.vias[static_cast<std::size_t>(n)] = 0;
        continue;
      }
      nets.push_back(n);
    }
  }
  // Boxes from the current pins too (pins may have moved), and the repair
  // window around everything that will change.
  GBox win;
  par::parallel_for(0, nets.size(), 256, [&](std::size_t lo, std::size_t hi) {
    std::vector<int> pins;
    for (std::size_t k = lo; k < hi; ++k) {
      const Id n = nets[k];
      pin_nodes(design, s.g, n, pins);
      GBox b = s.box[static_cast<std::size_t>(n)];
      for (int p : pins) b.add(s.g.node_x(p), s.g.node_y(p));
      s.box[static_cast<std::size_t>(n)] = b;
    }
  });
  for (Id n : nets) win.add(s.box[static_cast<std::size_t>(n)].intersect(F));
  win = win.expanded(opts_.repair_margin, s.g.nx(), s.g.ny()).intersect(F);
  if (!win.empty()) res.overflow_before = scan_overflow(s, win, 0.0, nullptr);

  // Reroute the selected nets (maze, layers inline).
  if (ctl) ctl->stage_progress(0.0, std::format("reroute {} nets", nets.size()));
  std::atomic<std::size_t> skipped{0};
  const float pen = static_cast<float>(opts_.incremental_penalty);
  bool ok = run_batches(s, nets, F, opts_.maze_margin, ctl, [&](Id n, Scratch& scr) {
    if (!reroute_one(s, design, opts_, pen, n, F, opts_.layer_assignment, scr)) skipped.fetch_add(1, std::memory_order_relaxed);
  });
  // Pin keys of the nets now routed to their current pins.
  par::parallel_for(0, nets.size(), 256, [&](std::size_t lo, std::size_t hi) {
    std::vector<int> pins;
    for (std::size_t k = lo; k < hi; ++k) {
      pin_nodes(design, s.g, nets[k], pins);
      s.key[static_cast<std::size_t>(nets[k])] = pin_key(pins);
    }
  });
  res.nets_rerouted = nets.size() - skipped.load();
  res.nets_skipped = skipped.load();
  const double t_reroute = since(t0);

  // Repair overflow in the window: only nets with routes there, only their
  // part inside it.
  if (ok && req.repair && !win.empty()) {
    Scope sc;
    sc.window = sc.fence = win;
    sc.all_nets = false;
    sc.la = opts_.layer_assignment;
    sc.iterations = req.repair_iterations >= 0 ? req.repair_iterations : opts_.repair_iterations;
    const NegotiateStats ns = negotiate(s, design, opts_, sc, ctl, 0.2, 1.0, opts_.incremental_penalty);
    res.iterations = ns.iterations;
    res.nets_repaired = ns.ripped;
    ok = !ns.cancelled;
  }
  if (!win.empty()) res.overflow_after = scan_overflow(s, win, 0.0, nullptr);
  const GlobalRouteResult t = stats();
  res.total_overflow = t.total_overflow;
  res.wirelength_delta = t.wirelength - wl_before;
  res.cancelled = !ok;
  res.seconds = since(t0);
  HLOG_DEBUG_ID(log_inc, "GRT-0020",
                "reroute: {} nets ({} kept: not connectable inside the fence) in {:.4f} s; repair window {}x{} GCells, "
                "{} iterations, {} rip-ups, overflow {:.4g} -> {:.4g}, wirelength {:+.6g}, {:.4f} s total",
                res.nets_rerouted, res.nets_skipped, t_reroute, win.empty() ? 0 : win.x1 - win.x0 + 1,
                win.empty() ? 0 : win.y1 - win.y0 + 1, res.iterations, res.nets_repaired, res.overflow_before,
                res.overflow_after, res.wirelength_delta, res.seconds);
  return res;
}

RerouteResult GlobalRouter::reroute_region(const Design& design, const Rect& area, bool repair, RunControl* ctl) {
  State& s = *st_;
  if (!s.routed) return reroute(design, {}, ctl);
  s.resize_nets(design.num_nets());
  const GBox F = s.gbox(area).intersect(s.full());
  RerouteRequest req;
  req.fence = area;
  req.repair = repair;
  const std::size_t nn = design.num_nets();
  std::vector<std::uint8_t> pick(nn, 0);
  par::parallel_for(0, nn, 2048, [&](std::size_t lo, std::size_t hi) {
    std::vector<int> pins;
    for (std::size_t n = lo; n < hi; ++n) {
      if (design.net(static_cast<Id>(n)).removed || !s.box[n].overlaps(F)) continue;
      bool hit = false;
      for (std::uint32_t e : s.edges[n])
        if (s.edge_in(e, F)) {
          hit = true;
          break;
        }
      if (!hit) {
        pin_nodes(design, s.g, static_cast<Id>(n), pins);
        if (pins.size() >= 2)
          for (int p : pins) hit = hit || F.contains(s.g.node_x(p), s.g.node_y(p));
      }
      pick[n] = hit;
    }
  });
  for (std::size_t n = 0; n < nn; ++n)
    if (pick[n]) req.nets.push_back(static_cast<Id>(n));
  HLOG_DEBUG_ID(log_inc, "GRT-0021", "region reroute: {} nets in GCells [{},{}]-[{},{}]", req.nets.size(), F.x0, F.y0,
                F.x1, F.y1);
  return reroute(design, req, ctl);
}

RerouteResult GlobalRouter::update(const Design& design, RunControl* ctl) {
  State& s = *st_;
  const auto t0 = Clock::now();
  const BinGrid grid = gcell_grid(design);
  const bool grid_changed = !s.routed || grid.nx() != s.g.nx() || grid.ny() != s.g.ny() ||
                            grid.region() != s.g.grid().region() || s.floorplan_changed;
  bool macros = s.macros_changed;
  if (!macros && !grid_changed && (!s.attached || s.dirty_all)) macros = macro_key(design) != s.macro_key;
  if (grid_changed || macros) {
    HLOG_DEBUG_ID(log_inc, "GRT-0022", "update: {} changed: full route",
                  grid_changed ? "GCell grid or floorplan" : "macro placement (capacities)");
    RerouteResult res;
    const GlobalRouteResult r = route(design, ctl);
    res.nets_rerouted = r.nets_routed;
    res.total_overflow = r.total_overflow;
    res.cancelled = r.cancelled;
    res.seconds = since(t0);
    return res;
  }
  s.resize_nets(design.num_nets());
  // Candidates: dirty nets (attached, tracking) or all, plus new nets.
  std::vector<Id> cand;
  const std::size_t nn = s.edges.size();
  const bool all = !s.attached || s.dirty_all;
  std::vector<std::uint8_t> changed(all ? nn : 0, 0);
  auto check = [&](Id n, std::vector<int>& pins) {
    const std::size_t i = static_cast<std::size_t>(n);
    if (!net_live(design, n)) return !s.edges[i].empty();
    pin_nodes(design, s.g, n, pins);
    return pin_key(pins) != s.key[i];
  };
  if (all) {
    par::parallel_for(0, nn, 2048, [&](std::size_t lo, std::size_t hi) {
      std::vector<int> pins;
      for (std::size_t n = lo; n < hi; ++n) changed[n] = check(static_cast<Id>(n), pins);
    });
    for (std::size_t n = 0; n < nn; ++n)
      if (changed[n]) cand.push_back(static_cast<Id>(n));
  } else {
    std::vector<int> pins;
    for (Id n : s.dirty.ids())
      if (static_cast<std::size_t>(n) < nn && check(n, pins)) cand.push_back(n);
    std::sort(cand.begin(), cand.end());
  }
  const std::size_t checked = all ? nn : s.dirty.size();
  s.dirty.clear();
  s.dirty_all = false;
  RerouteResult res;
  if (!cand.empty()) {
    RerouteRequest req;
    req.nets = std::move(cand);
    res = reroute(design, req, ctl);
  } else {
    res.total_overflow = stats().total_overflow;
  }
  res.seconds = since(t0);
  HLOG_DEBUG_ID(log_inc, "GRT-0023", "update: checked {} nets, {} changed GCells, {:.4f} s", checked,
                res.nets_rerouted + res.nets_skipped, res.seconds);
  return res;
}

void GlobalRouter::attach(Design& design) {
  State& s = *st_;
  if (s.attached == &design) return;
  detach();
  design.subscribe(&s.listener);
  s.attached = &design;
  s.dirty_all = true;  // changes before attaching are unknown
}

void GlobalRouter::detach() {
  State& s = *st_;
  if (!s.attached) return;
  s.attached->unsubscribe(&s.listener);
  s.attached = nullptr;
}

void GlobalRouter::begin_trial() {
  State& s = *st_;
  s.trial = true;
  s.trial_set.clear();
  s.saved.clear();
}
void GlobalRouter::commit_trial() {
  State& s = *st_;
  s.trial = false;
  s.trial_set.clear();
  s.saved.clear();
}
void GlobalRouter::rollback_trial() {
  State& s = *st_;
  s.trial = false;
  std::vector<std::uint32_t> rem;
  std::vector<std::uint8_t> reml;
  for (auto it = s.saved.rbegin(); it != s.saved.rend(); ++it) {
    rip(s, it->net, s.full(), rem, reml);
    add(s, it->net, it->edges, it->layers.data(), false);
    const std::size_t i = static_cast<std::size_t>(it->net);
    s.key[i] = it->key;
    s.box[i] = it->box;
    s.vias[i] = it->vias;
  }
  HLOG_DEBUG_ID(log_inc, "GRT-0024", "trial rolled back: {} nets restored", s.saved.size());
  s.trial_set.clear();
  s.saved.clear();
}
bool GlobalRouter::in_trial() const { return st_->trial; }

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

bool GlobalRouter::routed() const { return st_->routed; }
const RouteGraph* GlobalRouter::graph() const { return st_->routed ? &st_->g : nullptr; }
std::span<const std::uint32_t> GlobalRouter::net_edges(Id net) const {
  const auto i = static_cast<std::size_t>(net);
  return i < st_->edges.size() ? std::span<const std::uint32_t>(st_->edges[i]) : std::span<const std::uint32_t>{};
}
std::span<const std::uint8_t> GlobalRouter::net_layers(Id net) const {
  const auto i = static_cast<std::size_t>(net);
  return i < st_->layers.size() ? std::span<const std::uint8_t>(st_->layers[i]) : std::span<const std::uint8_t>{};
}

GlobalRouteResult GlobalRouter::stats() const {
  const State& s = *st_;
  GlobalRouteResult r;
  if (!s.routed) return r;
  const RouteGraph& g = s.g;
  struct Acc {
    double of = 0, wl = 0, mu = 0, lof = 0;
    std::size_t oe = 0;
  };
  const Acc a = par::parallel_reduce(
      0, g.num_edges(), 1 << 14, Acc{},
      [&](std::size_t lo, std::size_t hi) {
        Acc x;
        for (std::size_t k = lo; k < hi; ++k) {
          const auto e = static_cast<std::uint32_t>(k);
          const float o = g.overflow(e);
          x.of += o;
          x.oe += o > 0.0f;
          if (g.use(e) > 0) x.wl += g.use(e) * g.edge_length(e);
          if (g.cap(e) > 0) x.mu = std::max(x.mu, g.use(e) / static_cast<double>(g.cap(e)));
          for (int l : g.layers_of(e)) {
            const double lo2 = g.layer_use(l, e) - g.layer_cap(l, e);
            if (lo2 > 1e-4) x.lof += lo2;
          }
        }
        return x;
      },
      [](Acc p, Acc q) {
        p.of += q.of;
        p.wl += q.wl;
        p.lof += q.lof;
        p.oe += q.oe;
        p.mu = std::max(p.mu, q.mu);
        return p;
      });
  r.total_overflow = a.of;
  r.overflow_edges = a.oe;
  r.wirelength = a.wl;
  r.max_utilization = a.mu;
  r.layer_overflow = a.lof;
  for (std::uint32_t v : s.vias) r.vias += v;
  for (const auto& e : s.edges) r.nets_routed += !e.empty();
  return r;
}

double GlobalRouter::overflow_in(const Rect& area) const {
  State& s = *st_;
  if (!s.routed) return 0.0;
  const GBox b = s.gbox(area).intersect(s.full());
  return b.empty() ? 0.0 : scan_overflow(s, b, 0.0, nullptr);
}

void GlobalRouter::net_shapes(const Design& design, Id net, std::vector<WireSegment>& wires,
                              std::vector<ViaShape>* vias) const {
  const State& s = *st_;
  const auto i = static_cast<std::size_t>(net);
  if (!s.routed || i >= s.edges.size()) return;
  const RouteGraph& g = s.g;
  const auto& E = s.edges[i];
  const auto& Lv = s.layers[i];
  auto center = [&](int n) { return g.grid().bin_rect(g.node_x(n), g.node_y(n)).center(); };
  auto layer_of = [&](std::size_t k) { return Lv[k] == kNoLayer ? g.layers_of(E[k]).front() : int{Lv[k]}; };
  // Merge consecutive edges on the same layer: horizontal edges are
  // consecutive ids in a row; vertical ones differ by nx.
  std::vector<std::size_t> hs, vs;
  for (std::size_t k = 0; k < E.size(); ++k) (g.is_h(E[k]) ? hs : vs).push_back(k);
  auto vkey = [&](std::size_t k) {
    const int n = static_cast<int>(E[k] - g.num_h_edges());
    return static_cast<long long>(g.node_x(n)) * g.ny() + g.node_y(n);
  };
  std::sort(vs.begin(), vs.end(), [&](std::size_t a, std::size_t b) { return vkey(a) < vkey(b); });
  const std::uint32_t rowlen = static_cast<std::uint32_t>(std::max(1, g.nx() - 1));
  auto emit = [&](const std::vector<std::size_t>& ks, std::uint32_t step, bool h) {
    for (std::size_t a = 0; a < ks.size();) {
      std::size_t b = a + 1;
      while (b < ks.size() && E[ks[b]] == E[ks[b - 1]] + step && layer_of(ks[b]) == layer_of(ks[a]) &&
             (!h || E[ks[b]] % rowlen != 0))
        ++b;
      wires.push_back({center(g.ends(E[ks[a]]).first), center(g.ends(E[ks[b - 1]]).second), layer_of(ks[a])});
      a = b;
    }
  };
  emit(hs, 1, true);
  emit(vs, static_cast<std::uint32_t>(g.nx()), false);
  if (vias) {
    std::vector<int> pins;
    pin_nodes(design, g, net, pins);
    Scratch& sc = scratch(g);
    net_vias(g, sc, pins, E, Lv, [&](int n, int lo, int hi) { vias->push_back({center(n), lo, hi}); });
  }
}

RouteUsage3D GlobalRouter::route_usage(const Design& design) const {
  const State& s = *st_;
  if (!s.routed) {
    const BinGrid g = gcell_grid(design);
    return build_route_usage(design, usage_options(opts_, g));
  }
  const RouteGraph& g = s.g;
  RouteUsage3D u;
  u.grid = g.grid();
  u.stack = g.stack();
  u.capacity = g.gcell_capacity();
  const int L = g.num_layers();
  const int nx = g.nx(), ny = g.ny();
  const std::size_t ng = g.num_nodes();
  u.demand.assign(static_cast<std::size_t>(L), std::vector<float>(ng, 0.0f));
  u.wirelength.assign(static_cast<std::size_t>(L), 0.0);
  // Layer use of each edge. Edges without a layer (layer assignment off)
  // count on their direction's lowest layer.
  const bool any_unassigned = std::any_of(s.layers.begin(), s.layers.end(), [](const auto& v) {
    return std::find(v.begin(), v.end(), kNoLayer) != v.end();
  });
  std::vector<std::vector<std::int32_t>> extra;
  if (any_unassigned) {
    extra.assign(static_cast<std::size_t>(L), std::vector<std::int32_t>(g.num_edges(), 0));
    for (std::size_t n = 0; n < s.edges.size(); ++n)
      for (std::size_t k = 0; k < s.edges[n].size(); ++k) {
        const std::uint32_t e = s.edges[n][k];
        const int l = s.layers[n][k] == kNoLayer ? g.layers_of(e).front() : s.layers[n][k];
        ++extra[static_cast<std::size_t>(l)][e];
      }
  }
  auto use_l = [&](int l, std::uint32_t e) -> double {
    return any_unassigned ? extra[static_cast<std::size_t>(l)][e] : g.layer_use(l, e);
  };
  par::parallel_for_each(0, static_cast<std::size_t>(L), [&](std::size_t li) {
    const int l = static_cast<int>(li);
    const LayerInfo& inf = g.stack().layers[li];
    if (!inf.routable) return;
    const bool in_h = std::find(g.h_layers().begin(), g.h_layers().end(), l) != g.h_layers().end();
    const bool in_v = std::find(g.v_layers().begin(), g.v_layers().end(), l) != g.v_layers().end();
    double wl = 0.0;
    auto& dem = u.demand[li];
    for (int y = 0; y < ny; ++y)
      for (int x = 0; x < nx; ++x) {
        double d = 0.0;
        if (in_h) {
          if (x > 0) d += use_l(l, g.h_edge(x - 1, y));
          if (x + 1 < nx) {
            const double v = use_l(l, g.h_edge(x, y));
            d += v;
            wl += v * g.edge_length(g.h_edge(x, y));
          }
        }
        if (in_v) {
          if (y > 0) d += use_l(l, g.v_edge(x, y - 1));
          if (y + 1 < ny) {
            const double v = use_l(l, g.v_edge(x, y));
            d += v;
            wl += v * g.edge_length(g.v_edge(x, y));
          }
        }
        dem[g.node(x, y)] = static_cast<float>(0.5 * d);
      }
    u.wirelength[li] = wl;
  });
  // Vias per cut and GCell.
  const int ncut = std::max(0, L - 1);
  u.vias.assign(static_cast<std::size_t>(ncut), std::vector<std::uint32_t>(ng, 0));
  std::vector<std::uint64_t> vc(static_cast<std::size_t>(ncut), 0);
  std::atomic<std::size_t> nedges{0};
  par::parallel_for(0, s.edges.size(), 1024, [&](std::size_t lo, std::size_t hi) {
    Scratch& sc = scratch(g);
    std::vector<int> pins;
    std::vector<std::uint64_t> lvc(static_cast<std::size_t>(ncut), 0);
    std::size_t ne = 0;
    for (std::size_t n = lo; n < hi; ++n) {
      if (s.edges[n].empty() || !net_live(design, static_cast<Id>(n))) continue;
      ne += s.edges[n].size();
      pin_nodes(design, g, static_cast<Id>(n), pins);
      net_vias(g, sc, pins, s.edges[n], s.layers[n], [&](int node, int a, int b) {
        for (int c = a; c < b && c < ncut; ++c) {
          atomic_add(u.vias[static_cast<std::size_t>(c)][static_cast<std::size_t>(node)], 1u);
          ++lvc[static_cast<std::size_t>(c)];
        }
      });
    }
    for (int c = 0; c < ncut; ++c) atomic_add(vc[static_cast<std::size_t>(c)], lvc[static_cast<std::size_t>(c)]);
    nedges.fetch_add(ne, std::memory_order_relaxed);
  });
  u.via_count = std::move(vc);
  u.num_segments = nedges.load();
  u.source = "hermes global router (pattern + negotiated A* rip-up and reroute, greedy layer assignment)";
  u.layers_estimated = any_unassigned;
  return u;
}

}  // namespace hermes
