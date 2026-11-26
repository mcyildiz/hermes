#include "hermes/region/region.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

#include "hermes/db/geometry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"

namespace hermes::region {

namespace {
HERMES_LOG_CHANNEL(log_region, "region");

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

// ---------------------------------------------------------------------------
// Partition
// ---------------------------------------------------------------------------

Partition::Partition(const Design& d, const RegionGrid& grid) : grid_(grid) {
  const std::size_t nc = d.num_cells();
  const auto cells = d.cells();
  cell_region_.resize(nc);
  par::parallel_for(0, nc, 16384, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      cell_region_[i] = cells[i].removed ? -1 : grid_.region_of(cells[i].center());
    }
  });
  const int R = grid_.size();
  start_.assign(R + 1, 0);
  for (int r : cell_region_) {
    if (r >= 0) ++start_[r + 1];
  }
  for (int r = 0; r < R; ++r) start_[r + 1] += start_[r];
  cells_.resize(start_[R]);
  std::vector<int> fill(start_.begin(), start_.end() - 1);
  for (std::size_t i = 0; i < nc; ++i) {
    if (cell_region_[i] >= 0) cells_[fill[cell_region_[i]]++] = static_cast<Id>(i);
  }
  const std::size_t nn = d.num_nets();
  net_cut_.assign(nn, 0);
  num_cut_ = par::parallel_sum(0, nn, 8192, [&](std::size_t lo, std::size_t hi) {
    std::size_t cut = 0;
    for (std::size_t n = lo; n < hi; ++n) {
      int r0 = -2;
      bool c = false;
      for (Id p : d.net_pins(static_cast<Id>(n))) {
        const Id cell = d.pins()[static_cast<std::size_t>(p)].cell;
        const int r = cell == kInvalidId ? -1 : cell_region_[static_cast<std::size_t>(cell)];
        if (r < 0 || (r0 != -2 && r != r0)) {
          c = true;
          break;
        }
        r0 = r;
      }
      net_cut_[n] = c;
      cut += c;
    }
    return cut;
  });
}

std::span<const Id> Partition::cells(int r) const {
  return std::span<const Id>(cells_).subspan(start_[r], start_[r + 1] - start_[r]);
}

std::vector<Id> Partition::spatial_order() const {
  std::vector<Id> order;
  order.reserve(cells_.size());
  for (int iy = 0; iy < grid_.ny(); ++iy) {
    for (int k = 0; k < grid_.nx(); ++k) {
      const int ix = (iy % 2 == 0) ? k : grid_.nx() - 1 - k;  // serpentine
      const auto c = cells(iy * grid_.nx() + ix);
      order.insert(order.end(), c.begin(), c.end());
    }
  }
  return order;
}

std::vector<int> Partition::all_regions() const {
  std::vector<int> r(num_regions());
  for (int i = 0; i < num_regions(); ++i) r[i] = i;
  return r;
}

std::vector<int> Partition::regions_of_color(int color) const {
  std::vector<int> r;
  for (int i = 0; i < num_regions(); ++i) {
    if (grid_.color(i) == color) r.push_back(i);
  }
  return r;
}

// ---------------------------------------------------------------------------
// Problems and solutions
// ---------------------------------------------------------------------------

double RegionProblem::hpwl(std::span<const Point> loc) const {
  if (loc.empty()) loc = cell_loc;
  double total = 0.0;
  for (std::size_t n = 0; n < num_nets(); ++n) {
    if (net_start[n + 1] - net_start[n] < 2) continue;
    Dbu xl = INT64_MAX, xh = INT64_MIN, yl = INT64_MAX, yh = INT64_MIN;
    for (int p = net_start[n]; p < net_start[n + 1]; ++p) {
      const Point q = pin_cell[p] >= 0 ? loc[pin_cell[p]] + pin_pos[p] : pin_pos[p];
      xl = std::min(xl, q.x);
      xh = std::max(xh, q.x);
      yl = std::min(yl, q.y);
      yh = std::max(yh, q.y);
    }
    total += net_weight[n] * static_cast<double>((xh - xl) + (yh - yl));
  }
  return total;
}

RegionProblem extract(const Design& d, const Partition& part, int r, std::string_view solver) {
  RegionProblem p;
  p.region = r;
  p.bounds = part.grid().rect(r);
  p.design_version = d.version();
  p.solver.assign(solver);
  std::unordered_map<Id, int> local;
  for (Id c : part.cells(r)) {
    const Cell& cell = d.cell(c);
    if (!cell.movable()) continue;
    local.emplace(c, static_cast<int>(p.cell_id.size()));
    p.cell_id.push_back(c);
    p.cell_loc.push_back(cell.loc);
    p.cell_w.push_back(cell.width);
    p.cell_h.push_back(cell.height);
  }
  for (Id c : p.cell_id) {
    for (Id pin : d.cell_pins(c)) {
      const Id n = d.pin(pin).net;
      if (n != kInvalidId) p.net_id.push_back(n);
    }
  }
  std::sort(p.net_id.begin(), p.net_id.end());
  p.net_id.erase(std::unique(p.net_id.begin(), p.net_id.end()), p.net_id.end());
  p.net_start.push_back(0);
  for (Id n : p.net_id) {
    p.net_weight.push_back(d.net(n).weight);
    for (Id pin : d.net_pins(n)) {
      const Pin& pn = d.pin(pin);
      auto it = pn.cell == kInvalidId ? local.end() : local.find(pn.cell);
      if (it != local.end()) {
        p.pin_cell.push_back(it->second);
        p.pin_pos.push_back(pn.offset);
      } else {
        p.pin_cell.push_back(-1);
        p.pin_pos.push_back(pin_location(d, pin));  // frozen terminal
      }
    }
    p.net_start.push_back(static_cast<int>(p.pin_cell.size()));
  }
  return p;
}

namespace {
Point clamp_center(const RegionProblem& p, std::size_t i, Point loc) {
  // Keep the cell's center inside the region, so it stays in the region.
  const Rect& b = p.bounds;
  const Dbu hw = p.cell_w[i] / 2, hh = p.cell_h[i] / 2;
  const Dbu cx = std::clamp(loc.x + hw, b.xlo, std::max(b.xlo, b.xhi - 1));
  const Dbu cy = std::clamp(loc.y + hh, b.ylo, std::max(b.ylo, b.yhi - 1));
  return {cx - hw, cy - hh};
}
}  // namespace

void apply(Design& d, const RegionProblem& p, const RegionSolution& s) {
  if (s.region != p.region) throw std::invalid_argument("region::apply: solution for another region");
  if (p.design_version != d.version() || s.design_version != p.design_version) {
    throw std::invalid_argument("region::apply: design changed since the problem was extracted");
  }
  if (s.cell_id.size() != s.cell_loc.size()) throw std::invalid_argument("region::apply: bad solution");
  // cell_id is ascending (Partition::cells order), so membership is a search.
  for (std::size_t k = 0; k < s.cell_id.size(); ++k) {
    const auto it = std::lower_bound(p.cell_id.begin(), p.cell_id.end(), s.cell_id[k]);
    if (it == p.cell_id.end() || *it != s.cell_id[k]) {
      throw std::invalid_argument("region::apply: cell " + std::to_string(s.cell_id[k]) +
                                  " is not a movable cell of region " + std::to_string(p.region));
    }
  }
  for (std::size_t k = 0; k < s.cell_id.size(); ++k) {
    const std::size_t i = static_cast<std::size_t>(
        std::lower_bound(p.cell_id.begin(), p.cell_id.end(), s.cell_id[k]) - p.cell_id.begin());
    const Point loc = clamp_center(p, i, s.cell_loc[k]);
    if (d.cell(s.cell_id[k]).loc != loc) d.move_cell(s.cell_id[k], loc);
  }
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

namespace {
static_assert(std::endian::native == std::endian::little,
              "region serialization assumes a little-endian host");

constexpr std::uint32_t kProblemMagic = 0x31505248;   // "HRP1"
constexpr std::uint32_t kSolutionMagic = 0x31535248;  // "HRS1"
constexpr std::uint32_t kFormat = 1;

struct Writer {
  std::vector<std::uint8_t> out;
  template <class T>
  void put(const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof(T));
  }
  template <class T>
  void vec(const std::vector<T>& v) {
    put<std::uint64_t>(v.size());
    const auto* b = reinterpret_cast<const std::uint8_t*>(v.data());
    out.insert(out.end(), b, b + v.size() * sizeof(T));
  }
  void str(const std::string& s) {
    put<std::uint64_t>(s.size());
    out.insert(out.end(), s.begin(), s.end());
  }
};

struct Reader {
  std::span<const std::uint8_t> in;
  std::size_t pos = 0;
  void need(std::size_t n) {
    if (in.size() - pos < n) throw std::runtime_error("region: truncated message");
  }
  template <class T>
  T get() {
    need(sizeof(T));
    T v;
    std::memcpy(&v, in.data() + pos, sizeof(T));
    pos += sizeof(T);
    return v;
  }
  template <class T>
  void vec(std::vector<T>& v) {
    const auto n = get<std::uint64_t>();
    if (n > (in.size() - pos) / sizeof(T)) throw std::runtime_error("region: truncated message");
    v.resize(n);
    std::memcpy(v.data(), in.data() + pos, n * sizeof(T));
    pos += n * sizeof(T);
  }
  void str(std::string& s) {
    const auto n = get<std::uint64_t>();
    need(n);
    s.assign(reinterpret_cast<const char*>(in.data() + pos), n);
    pos += n;
  }
};

void header(Reader& r, std::uint32_t magic) {
  if (r.get<std::uint32_t>() != magic) throw std::runtime_error("region: bad message type");
  if (r.get<std::uint32_t>() != kFormat) throw std::runtime_error("region: unsupported format version");
}
}  // namespace

std::vector<std::uint8_t> serialize(const RegionProblem& p) {
  Writer w;
  w.put(kProblemMagic);
  w.put(kFormat);
  w.put<std::int32_t>(p.region);
  w.put(p.bounds);
  w.put(p.design_version);
  w.str(p.solver);
  w.vec(p.cell_id);
  w.vec(p.cell_loc);
  w.vec(p.cell_w);
  w.vec(p.cell_h);
  w.vec(p.net_id);
  w.vec(p.net_weight);
  w.vec(p.net_start);
  w.vec(p.pin_cell);
  w.vec(p.pin_pos);
  return std::move(w.out);
}

std::vector<std::uint8_t> serialize(const RegionSolution& s) {
  Writer w;
  w.put(kSolutionMagic);
  w.put(kFormat);
  w.put<std::int32_t>(s.region);
  w.put(s.design_version);
  w.vec(s.cell_id);
  w.vec(s.cell_loc);
  w.put(s.hpwl_before);
  w.put(s.hpwl_after);
  return std::move(w.out);
}

RegionProblem deserialize_problem(std::span<const std::uint8_t> bytes) {
  Reader r{bytes};
  header(r, kProblemMagic);
  RegionProblem p;
  p.region = r.get<std::int32_t>();
  p.bounds = r.get<Rect>();
  p.design_version = r.get<std::uint64_t>();
  r.str(p.solver);
  r.vec(p.cell_id);
  r.vec(p.cell_loc);
  r.vec(p.cell_w);
  r.vec(p.cell_h);
  r.vec(p.net_id);
  r.vec(p.net_weight);
  r.vec(p.net_start);
  r.vec(p.pin_cell);
  r.vec(p.pin_pos);
  // Validate the indices a solver will follow.
  const std::size_t nc = p.cell_id.size();
  if (p.cell_loc.size() != nc || p.cell_w.size() != nc || p.cell_h.size() != nc ||
      p.net_weight.size() != p.net_id.size() || p.net_start.size() != p.net_id.size() + 1 ||
      p.pin_pos.size() != p.pin_cell.size() || p.net_start.front() != 0 ||
      p.net_start.back() != static_cast<int>(p.pin_cell.size())) {
    throw std::runtime_error("region: inconsistent problem");
  }
  for (std::size_t n = 0; n + 1 < p.net_start.size(); ++n) {
    if (p.net_start[n] > p.net_start[n + 1]) throw std::runtime_error("region: inconsistent problem");
  }
  for (int c : p.pin_cell) {
    if (c < -1 || c >= static_cast<int>(nc)) throw std::runtime_error("region: inconsistent problem");
  }
  return p;
}

RegionSolution deserialize_solution(std::span<const std::uint8_t> bytes) {
  Reader r{bytes};
  header(r, kSolutionMagic);
  RegionSolution s;
  s.region = r.get<std::int32_t>();
  s.design_version = r.get<std::uint64_t>();
  r.vec(s.cell_id);
  r.vec(s.cell_loc);
  s.hpwl_before = r.get<double>();
  s.hpwl_after = r.get<double>();
  if (s.cell_id.size() != s.cell_loc.size()) throw std::runtime_error("region: inconsistent solution");
  return s;
}

// ---------------------------------------------------------------------------
// Solvers
// ---------------------------------------------------------------------------

namespace {
struct SolverRegistry {
  std::mutex mu;
  std::map<std::string, RegionSolver, std::less<>> solvers{{"median", solve_median}};
};
SolverRegistry& solvers() {
  static SolverRegistry r;
  return r;
}
}  // namespace

void register_solver(std::string name, RegionSolver s) {
  std::lock_guard lk(solvers().mu);
  solvers().solvers[std::move(name)] = std::move(s);
}

RegionSolver solver(std::string_view name) {
  std::lock_guard lk(solvers().mu);
  auto it = solvers().solvers.find(name);
  if (it == solvers().solvers.end()) {
    throw std::invalid_argument("unknown region solver '" + std::string(name) + "'");
  }
  return it->second;
}

std::vector<std::string> solver_names() {
  std::lock_guard lk(solvers().mu);
  std::vector<std::string> out;
  for (const auto& [n, s] : solvers().solvers) out.push_back(n);
  return out;
}

RegionSolution solve_median(const RegionProblem& p) {
  RegionSolution s;
  s.region = p.region;
  s.design_version = p.design_version;
  const std::size_t nc = p.num_cells();
  std::vector<Point> loc = p.cell_loc;
  s.hpwl_before = p.hpwl(loc);
  // Cell -> nets (CSR).
  std::vector<int> cstart(nc + 1, 0), cnets;
  for (std::size_t n = 0; n < p.num_nets(); ++n) {
    for (int q = p.net_start[n]; q < p.net_start[n + 1]; ++q) {
      if (p.pin_cell[q] >= 0) ++cstart[p.pin_cell[q] + 1];
    }
  }
  for (std::size_t i = 0; i < nc; ++i) cstart[i + 1] += cstart[i];
  cnets.resize(cstart[nc]);
  {
    std::vector<int> fill(cstart.begin(), cstart.end() - 1);
    for (std::size_t n = 0; n < p.num_nets(); ++n) {
      for (int q = p.net_start[n]; q < p.net_start[n + 1]; ++q) {
        if (p.pin_cell[q] >= 0) cnets[fill[p.pin_cell[q]]++] = static_cast<int>(n);
      }
    }
  }
  auto net_cost = [&](int n) {
    Dbu xl = INT64_MAX, xh = INT64_MIN, yl = INT64_MAX, yh = INT64_MIN;
    for (int q = p.net_start[n]; q < p.net_start[n + 1]; ++q) {
      const Point a = p.pin_cell[q] >= 0 ? loc[p.pin_cell[q]] + p.pin_pos[q] : p.pin_pos[q];
      xl = std::min(xl, a.x);
      xh = std::max(xh, a.x);
      yl = std::min(yl, a.y);
      yh = std::max(yh, a.y);
    }
    return p.net_start[n + 1] - p.net_start[n] < 2
               ? 0.0
               : p.net_weight[n] * static_cast<double>((xh - xl) + (yh - yl));
  };
  std::vector<Dbu> xs, ys;
  for (std::size_t i = 0; i < nc; ++i) {
    xs.clear();
    ys.clear();
    double before = 0.0;
    for (int k = cstart[i]; k < cstart[i + 1]; ++k) {
      const int n = cnets[k];
      before += net_cost(n);
      for (int q = p.net_start[n]; q < p.net_start[n + 1]; ++q) {
        if (p.pin_cell[q] == static_cast<int>(i)) continue;
        const Point a = p.pin_cell[q] >= 0 ? loc[p.pin_cell[q]] + p.pin_pos[q] : p.pin_pos[q];
        xs.push_back(a.x);
        ys.push_back(a.y);
      }
    }
    if (xs.empty()) continue;
    auto mid = [](std::vector<Dbu>& v) {
      std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
      return v[v.size() / 2];
    };
    const Point old = loc[i];
    const Point target{mid(xs) - p.cell_w[i] / 2, mid(ys) - p.cell_h[i] / 2};
    loc[i] = clamp_center(p, i, target);
    double after = 0.0;
    for (int k = cstart[i]; k < cstart[i + 1]; ++k) after += net_cost(cnets[k]);
    if (after >= before) loc[i] = old;  // keep only improving moves
  }
  for (std::size_t i = 0; i < nc; ++i) {
    if (loc[i] != p.cell_loc[i]) {
      s.cell_id.push_back(p.cell_id[i]);
      s.cell_loc.push_back(loc[i]);
    }
  }
  s.hpwl_after = p.hpwl(loc);
  return s;
}

// ---------------------------------------------------------------------------
// Executors
// ---------------------------------------------------------------------------

std::vector<RegionSolution> LocalExecutor::run(std::span<const RegionProblem> problems,
                                               const RegionSolver& solve, RunControl* ctl) {
  std::vector<RegionSolution> out(problems.size());
  std::atomic<std::size_t> done{0};
  par::parallel_for_each(0, problems.size(), [&](std::size_t i) {
    if (ctl && ctl->cancelled()) {
      out[i].region = problems[i].region;  // empty: nothing moves
      out[i].design_version = problems[i].design_version;
      return;
    }
    out[i] = solve(problems[i]);
    const std::size_t k = done.fetch_add(1) + 1;
    if (ctl) ctl->stage_progress(static_cast<double>(k) / problems.size());
  });
  return out;
}

std::vector<std::uint8_t> serve(std::span<const std::uint8_t> request) {
  // Response: one status byte (0 = ok + solution, 1 = error + message).
  std::vector<std::uint8_t> resp;
  try {
    const RegionProblem p = deserialize_problem(request);
    const RegionSolution s = solver(p.solver)(p);
    resp.push_back(0);
    const auto bytes = serialize(s);
    resp.insert(resp.end(), bytes.begin(), bytes.end());
  } catch (const std::exception& e) {
    resp.assign(1, 1);
    const std::string_view m = e.what();
    resp.insert(resp.end(), m.begin(), m.end());
  }
  return resp;
}

std::vector<RegionSolution> RemoteExecutor::run(std::span<const RegionProblem> problems,
                                                const RegionSolver&, RunControl* ctl) {
  if (workers_.empty()) throw std::invalid_argument("RemoteExecutor: no workers");
  std::vector<RegionSolution> out(problems.size());
  const std::size_t W = workers_.size();
  std::vector<std::size_t> sent(W, 0), recv(W, 0);
  std::vector<std::string> errors(W);
  par::parallel_for_each(0, W, [&](std::size_t w) {
    for (std::size_t i = w; i < problems.size(); i += W) {
      if (ctl && ctl->cancelled()) {
        out[i].region = problems[i].region;
        out[i].design_version = problems[i].design_version;
        continue;
      }
      const auto req = serialize(problems[i]);
      sent[w] += req.size();
      const auto resp = workers_[w]->call(req);
      recv[w] += resp.size();
      if (resp.empty() || resp[0] != 0) {
        errors[w] = resp.size() > 1 ? std::string(resp.begin() + 1, resp.end()) : "empty response";
        return;
      }
      out[i] = deserialize_solution(std::span<const std::uint8_t>(resp).subspan(1));
    }
  });
  for (std::size_t w = 0; w < W; ++w) {
    sent_ += sent[w];
    received_ += recv[w];
    if (!errors[w].empty()) throw std::runtime_error("remote region worker: " + errors[w]);
  }
  return out;
}

RegionRunStats run_regions(Design& d, const Partition& part, std::span<const int> regions,
                           std::string_view solver_name, RegionExecutor& exec, RunControl* ctl) {
  RegionRunStats st;
  st.regions = static_cast<int>(regions.size());
  double t = now_s();
  std::vector<RegionProblem> problems(regions.size());
  par::parallel_for_each(0, regions.size(), [&](std::size_t i) {
    problems[i] = extract(d, part, regions[i], solver_name);
  });
  st.extract_s = now_s() - t;
  t = now_s();
  const RegionSolver solve = solver(solver_name);
  const auto sols = exec.run(problems, solve, ctl);
  st.solve_s = now_s() - t;
  t = now_s();
  for (std::size_t i = 0; i < problems.size(); ++i) {
    apply(d, problems[i], sols[i]);
    st.cells_moved += sols[i].cell_id.size();
    st.hpwl_before += sols[i].cell_id.empty() ? problems[i].hpwl() : sols[i].hpwl_before;
    st.hpwl_after += sols[i].cell_id.empty() ? problems[i].hpwl() : sols[i].hpwl_after;
  }
  d.flush();
  st.apply_s = now_s() - t;
  HLOG_VERBOSE(log_region, "{} regions: {} cells moved, local HPWL {:.6g} -> {:.6g}", st.regions,
               st.cells_moved, st.hpwl_before, st.hpwl_after);
  return st;
}

}  // namespace hermes::region
