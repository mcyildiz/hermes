// Parallel runtime tests: thread pool, deterministic loops, row-band scatter,
// parallel kernels (identical across thread counts), background jobs with
// progress/cancel next to snapshot readers, region partitioning and the
// remote-worker protocol, compute backend registry.
//
// Also built with -fsanitize=thread (HERMES_SANITIZE=thread) to catch races.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "hermes/compute/backend.hpp"
#include "hermes/db/design.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/db/snapshot.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/job.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/par/row_bands.hpp"
#include "hermes/place/analytic.hpp"
#include "hermes/place/congestion.hpp"
#include "hermes/region/region.hpp"

using namespace hermes;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

template <class E, class F>
bool throws(F&& f) {
  try {
    f();
  } catch (const E&) {
    return true;
  }
  return false;
}

/// Small random netlist with local nets (cells near each other by id are
/// near each other in space).
Design make_design(int n, unsigned seed, bool placed = true) {
  std::mt19937 rng(seed);
  Design d("t");
  const Dbu side = static_cast<Dbu>(std::sqrt(n * 500.0 * 1000.0 / 0.6) / 1000 + 1) * 1000;
  d.set_die({0, 0, side, side});
  d.set_row_height(1000);
  std::uniform_int_distribution<Dbu> wd(200, 800), pos(0, side - 1000);
  for (int i = 0; i < n; ++i) {
    const Id c = d.add_cell("u" + std::to_string(i), wd(rng), 1000);
    if (placed) d.move_cell(c, {pos(rng), pos(rng) / 1000 * 1000});
  }
  std::uniform_int_distribution<int> deg(2, 5), near(-30, 30);
  for (int k = 0; k < n; ++k) {
    const Id net = d.add_net("n" + std::to_string(k));
    const int m = deg(rng);
    for (int p = 0; p < m; ++p) {
      const int c = p == 0 ? k : std::clamp(k + near(rng), 0, n - 1);
      d.connect(c, net, p == 0 ? "Z" : "A" + std::to_string(p), {100, 500},
                p == 0 ? PinDir::Output : PinDir::Input);
    }
  }
  for (int i = 0; i < 8; ++i) {  // fixed anchors
    d.move_cell(i, {(i % 2) ? 0 : side - 1000, (i * side / 8) / 1000 * 1000});
    d.set_fixed(i, true);
  }
  d.compact();
  return d;
}

// ---------------------------------------------------------------------------
// Pool and loops
// ---------------------------------------------------------------------------

void test_parallel_for_covers_range() {
  for (std::size_t n : {0u, 1u, 7u, 100u, 4097u, 100000u}) {
    for (std::size_t grain : {1u, 3u, 64u, 5000u}) {
      std::vector<int> hit(n, 0);
      par::parallel_for(0, n, grain, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) ++hit[i];
      });
      CHECK(std::all_of(hit.begin(), hit.end(), [](int h) { return h == 1; }));
    }
  }
  // Offset ranges.
  std::vector<int> hit(50, 0);
  par::parallel_for(10, 40, 1, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) ++hit[i];
  });
  for (int i = 0; i < 50; ++i) CHECK(hit[i] == (i >= 10 && i < 40 ? 1 : 0));
}

void test_reduce_deterministic() {
  std::mt19937_64 rng(1);
  std::uniform_real_distribution<double> u(-1e6, 1e6);
  std::vector<double> v(300001);
  for (double& x : v) x = u(rng) * std::exp(u(rng) * 1e-5);
  auto sum = [&] {
    return par::parallel_sum(0, v.size(), 1000, [&](std::size_t lo, std::size_t hi) {
      double s = 0;
      for (std::size_t i = lo; i < hi; ++i) s += v[i];
      return s;
    });
  };
  std::vector<double> results;
  for (int t : {1, 2, 3, 4, 8}) {
    par::ThreadLimit lim(t);
    results.push_back(sum());
  }
  for (double r : results) CHECK(r == results.front());  // bit-identical
  // Max reduction.
  const double m = par::parallel_reduce(
      0, v.size(), 777, -1e300,
      [&](std::size_t lo, std::size_t hi) { return *std::max_element(v.begin() + lo, v.begin() + hi); },
      [](double a, double b) { return std::max(a, b); });
  CHECK(m == *std::max_element(v.begin(), v.end()));
}

void test_nested_and_concurrent_loops() {
  // Nested loops from workers, and loops started from several threads at
  // once, must all complete (no deadlock) with the right result.
  std::atomic<long> total{0};
  par::parallel_for_each(0, 16, [&](std::size_t i) {
    const long s = par::parallel_sum(0, 10000, 100, [&](std::size_t lo, std::size_t hi) {
      long a = 0;
      for (std::size_t k = lo; k < hi; ++k) a += static_cast<long>(k % 7);
      return a;
    });
    total += s + static_cast<long>(i);
  });
  long expect1 = 0;
  for (int k = 0; k < 10000; ++k) expect1 += k % 7;
  CHECK(total == 16 * expect1 + 120);

  std::vector<std::thread> ts;
  std::atomic<int> ok{0};
  for (int t = 0; t < 4; ++t) {
    ts.emplace_back([&, t] {
      for (int rep = 0; rep < 20; ++rep) {
        std::vector<int> v(5000 + t);
        par::parallel_for(0, v.size(), 64, [&](std::size_t lo, std::size_t hi) {
          for (std::size_t i = lo; i < hi; ++i) v[i] = static_cast<int>(i);
        });
        bool good = true;
        for (std::size_t i = 0; i < v.size(); ++i) good = good && v[i] == static_cast<int>(i);
        if (good) ++ok;
      }
    });
  }
  for (auto& th : ts) th.join();
  CHECK(ok == 80);
}

void test_exceptions_propagate() {
  CHECK(throws<std::runtime_error>([] {
    par::parallel_for(0, 1000, 10, [&](std::size_t lo, std::size_t) {
      if (lo >= 500) throw std::runtime_error("boom");
    });
  }));
  // The pool still works afterwards.
  const auto s = par::parallel_sum(0, 1000, 10, [](std::size_t lo, std::size_t hi) {
    return static_cast<long>(hi - lo);
  });
  CHECK(s == 1000);
}

void test_thread_limit() {
  {
    par::ThreadLimit one(1);
    CHECK(par::num_threads() == 1);
    const auto before = par::pool_stats().loops;
    par::parallel_for(0, 100000, 10, [](std::size_t, std::size_t) {});
    CHECK(par::pool_stats().loops == before);  // ran inline
  }
  CHECK(par::num_threads() == std::min(par::num_threads(), par::hardware_threads()) ||
        par::num_threads() >= 1);
}

void test_row_bands_match_serial() {
  std::mt19937 rng(3);
  const int rows = 37, cols = 23;
  std::uniform_int_distribution<int> r(-3, rows + 2), len(0, 12), c(0, cols - 1);
  struct Item {
    int r0, r1, col;
    double v;
  };
  std::vector<Item> items(20000);
  for (auto& it : items) {
    it.r0 = r(rng);
    it.r1 = it.r0 + len(rng);
    it.col = c(rng);
    it.v = std::ldexp(1.0 + (rng() % 1000) / 997.0, static_cast<int>(rng() % 60) - 30);
  }
  auto clamp_rows = [&](const Item& it) -> std::pair<int, int> {
    const int a = std::max(0, it.r0), b = std::min(rows - 1, it.r1);
    return {a, b};
  };
  std::vector<double> serial(rows * cols, 0.0);
  for (const auto& it : items) {
    const auto [a, b] = clamp_rows(it);
    for (int y = a; y <= b; ++y) serial[y * cols + it.col] += it.v;
  }
  for (int bands : {1, 2, 5, 40}) {
    std::vector<double> grid(rows * cols, 0.0);
    par::RowBands rb;
    rb.build(items.size(), rows, [&](std::size_t i) { return clamp_rows(items[i]); }, bands);
    rb.run([&](std::size_t i, int a, int b) {
      for (int y = a; y <= b; ++y) grid[y * cols + items[i].col] += items[i].v;
    });
    CHECK(grid == serial);  // bit-identical, any band count
  }
}

// ---------------------------------------------------------------------------
// Kernels: identical results with any number of threads
// ---------------------------------------------------------------------------

void test_poisson_identical_across_threads() {
  std::vector<std::vector<double>> fields;
  for (int t : {1, 4}) {
    par::ThreadLimit lim(t);
    gp::ElectrostaticSystem es({0, 0, 6400, 3200}, 64, 32);
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> u(0, 6000);
    for (int k = 0; k < 500; ++k) {
      const double x = u(rng), y = u(rng) / 2;
      es.add_rect(x, y, x + 150, y + 90, 1.0);
    }
    es.solve(true);
    std::vector<double> f(es.field_x());
    f.insert(f.end(), es.field_y().begin(), es.field_y().end());
    f.push_back(es.energy());
    fields.push_back(std::move(f));
  }
  CHECK(fields[0] == fields[1]);
}

void rudy_case(const Design& d, int NX, int NY) {
  CongestionMap fast(d.die(), NX, NY, 1e6);
  estimate_rudy(d, fast);
  // Reference: the per-bin overlap loop.
  CongestionMap ref(d.die(), NX, NY, 1e6);
  for (Id n = 0; n < static_cast<Id>(d.num_nets()); ++n) {
    if (d.net_pins(n).size() < 2) continue;
    Rect bb = net_bbox(d, n);
    bb.xhi = std::max(bb.xhi, bb.xlo + 1);
    bb.yhi = std::max(bb.yhi, bb.ylo + 1);
    const double dens = static_cast<double>(bb.width() + bb.height()) / static_cast<double>(bb.area());
    const auto [x0, y0] = ref.bin_of({bb.xlo, bb.ylo});
    const auto [x1, y1] = ref.bin_of({bb.xhi, bb.yhi});
    for (int iy = y0; iy <= y1; ++iy)
      for (int ix = x0; ix <= x1; ++ix) {
        const Dbu ov = ref.bin_rect(ix, iy).overlap_area(bb);
        if (ov > 0) ref.add_demand(ix, iy, dens * static_cast<double>(ov));
      }
  }
  double maxrel = 0;
  for (int iy = 0; iy < NY; ++iy)
    for (int ix = 0; ix < NX; ++ix) {
      const double a = fast.demand(ix, iy), b = ref.demand(ix, iy);
      maxrel = std::max(maxrel, std::abs(a - b) / std::max(1.0, std::abs(b)));
    }
  CHECK(maxrel < 1e-9);
  // Same bits with any thread count.
  CongestionMap one(d.die(), NX, NY, 1e6);
  {
    par::ThreadLimit lim(1);
    estimate_rudy(d, one);
  }
  bool same = true;
  for (int iy = 0; iy < NY; ++iy)
    for (int ix = 0; ix < NX; ++ix) same = same && one.demand(ix, iy) == fast.demand(ix, iy);
  CHECK(same);
}

void test_rudy_fast_matches_reference() {
  Design d = make_design(3000, 5);
  rudy_case(d, 37, 29);    // coarse: private grids per block
  rudy_case(d, 300, 260);  // fine: row bands
}

void test_placement_identical_across_threads() {
  // Whole global placement + legalization: the same result to the last bit
  // (every cell location) with 1 and with all threads.
  std::vector<std::vector<Point>> locs;
  std::vector<double> hpwl;
  for (int t : {1, 0}) {
    par::ThreadLimit lim(t);
    Design d = make_design(4000, 11, /*placed=*/false);
    FlowOptions o;
    o.placer.max_iterations = 120;
    o.placer.timing_interval = 30;
    o.placer.routability_interval = 30;
    o.placer.log_interval = 0;
    const FlowReport r = run_flow(d, o);
    hpwl.push_back(r.placement.hpwl);
    std::vector<Point> l;
    for (const Cell& c : d.cells()) l.push_back(c.loc);
    locs.push_back(std::move(l));
  }
  CHECK(hpwl[0] == hpwl[1]);
  CHECK(locs[0] == locs[1]);
}

void test_move_cells() {
  Design d = make_design(1000, 2);
  std::vector<Id> ids{3, 500, 999};
  std::vector<Point> to{{1, 2}, {3, 4}, {5, 6}};
  d.move_cells(ids, to);  // tracking off: parallel path
  CHECK(d.cell(500).loc == (Point{3, 4}));
  d.set_tracking(true);
  d.flush();
  std::vector<Point> to2{{7, 7}, {8, 8}, {9, 9}};
  d.move_cells(ids, to2);  // tracking on: recorded
  CHECK(d.pending().cells.size() == 3 && d.pending().cells.test(999));
  {
    Transaction tx(d);
    d.move_cells(ids, to);
    tx.rollback();
  }
  CHECK(d.cell(3).loc == (Point{7, 7}));
  CHECK(throws<std::invalid_argument>([&] { d.move_cells(ids, std::vector<Point>(2)); }));
}

// ---------------------------------------------------------------------------
// Snapshots: what a publish copies
// ---------------------------------------------------------------------------

void test_snapshot_tracking_off_shares_nets_and_pins() {
  // The placer's case: tracking off, every flush is `all`, but only cells
  // moved. Nets, pins and the topology must stay shared.
  Design d = make_design(20000, 4);
  d.flush();  // the build's structural changes
  SnapshotPublisher pub(d);
  auto s1 = pub.latest();
  std::vector<Id> ids;
  std::vector<Point> locs;
  for (Id i = 8; i < static_cast<Id>(d.num_cells()); ++i) {
    ids.push_back(i);
    locs.push_back(d.cell(i).loc + Point{10, 0});
  }
  d.move_cells(ids, locs);
  d.flush();
  auto s2 = pub.latest();
  const std::size_t cell_chunks = (d.num_cells() + 4095) / 4096;
  CHECK(pub.last_copied_chunks() == cell_chunks);
  CHECK(s2->cell(100).loc == d.cell(100).loc && s1->cell(100).loc != d.cell(100).loc);
  CHECK(s2->net_pins(5).data() == s1->net_pins(5).data());  // topology shared
  // A net weight change (tracking off) does copy the nets.
  d.set_net_weight(3, 2.0);
  d.flush();
  CHECK(pub.latest()->net(3).weight == 2.0);
  CHECK(pub.last_copied_chunks() == cell_chunks + (d.num_nets() + 4095) / 4096);
}

void test_snapshot_region_order() {
  Design d = make_design(40000, 6);
  d.set_tracking(true);
  d.flush();
  region::Partition part(d, region::RegionGrid(d.die(), 10, 10));
  SnapshotPublisher pub(d);
  // Move every movable cell of one region (1% of the die).
  const int r = 55;
  auto move_region = [&] {
    for (Id c : part.cells(r)) {
      if (d.cell(c).movable()) d.move_cell(c, d.cell(c).loc + Point{1, 0});
    }
    d.flush();
    return pub.last_copied_chunks();
  };
  const std::size_t by_id = move_region();
  pub.set_cell_order(part.spatial_order());
  pub.publish();  // full re-layout
  const std::size_t by_region = move_region();
  std::printf("  chunks copied for a 1%%-region edit: id order %zu, region order %zu\n", by_id,
              by_region);
  CHECK(by_region <= 2 && by_region < by_id);
  // Reads by id still see the right cells.
  auto s = pub.latest();
  bool ok = s->cell_order().size() == d.num_cells();
  for (Id c = 0; c < static_cast<Id>(d.num_cells()); c += 97) ok = ok && s->cell(c).loc == d.cell(c).loc;
  CHECK(ok);
  // New cells are appended to the order.
  const Id extra = d.add_cell("extra", 10, 10);
  d.move_cell(extra, {123, 456});
  d.flush();
  CHECK(pub.latest()->cell(extra).loc == (Point{123, 456}) &&
        pub.latest()->cell_order().back() == extra);
}

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------

void test_job_runs_with_snapshot_reader() {
  Design d = make_design(6000, 8, false);
  SnapshotPublisher pub(d);
  FlowOptions o;
  o.placer.max_iterations = 150;
  o.placer.log_interval = 0;
  FlowReport rep;
  JobOptions jo;
  jo.publish_interval_s = 0.01;
  JobHandle job = start_flow_job(d, o, &rep, jo);
  // "GUI" thread: reads snapshots and progress while the job runs.
  std::uint64_t versions = 0, last = 0;
  double last_fraction = 0.0;
  bool monotonic = true;
  while (!job.wait_for(0.005)) {
    auto s = pub.latest();
    if (s->version() != last) {
      ++versions;
      last = s->version();
    }
    Dbu acc = 0;
    for (Id i = 0; i < static_cast<Id>(s->num_cells()); i += 13) acc += s->cell(i).loc.x;
    CHECK(acc >= 0);
    const JobProgress p = job.progress();
    monotonic = monotonic && p.fraction + 1e-12 >= last_fraction;
    last_fraction = p.fraction;
  }
  CHECK(job.state() == JobState::Done);
  CHECK(job.progress().fraction == 1.0 && monotonic);
  CHECK(!rep.cancelled && rep.placement.iterations > 0);
  CHECK(versions >= 2);  // saw intermediate versions
  CHECK(pub.latest()->version() == d.version());
}

void test_job_cancel() {
  Design d = make_design(20000, 9, false);
  SnapshotPublisher pub(d);
  FlowOptions o;
  o.placer.max_iterations = 100000;
  o.placer.stop_overflow = 0.0;  // would run "forever"
  o.placer.log_interval = 0;
  FlowReport rep;
  JobHandle job = start_flow_job(d, o, &rep);
  while (job.progress().updates < 5) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  job.cancel();
  CHECK(job.wait_for(30.0));
  CHECK(job.state() == JobState::Cancelled);
  CHECK(rep.cancelled && rep.placement.cancelled);
  CHECK(rep.placement.iterations < 100000);
  // The design is consistent and published at its last flush.
  CHECK(pub.latest()->version() == d.version());
}

void test_job_failure_and_handle_lifetime() {
  JobHandle j = start_job("fail", [](RunControl&) { throw std::logic_error("bad"); });
  j.wait();
  CHECK(j.state() == JobState::Failed);
  CHECK(throws<std::logic_error>([&] { j.rethrow_if_failed(); }));
  // Dropping the last handle cancels and joins a running job.
  std::atomic<bool> stopped{false};
  {
    JobHandle k = start_job("loop", [&](RunControl& ctl) {
      while (!ctl.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      stopped = true;
    });
  }
  CHECK(stopped);
  // Cancelled via exception.
  JobHandle c = start_job("c", [](RunControl& ctl) {
    ctl.request_cancel();
    ctl.check_cancelled();
  });
  c.wait();
  CHECK(c.state() == JobState::Cancelled);
}

void test_jobs_side_by_side() {
  // Two flows on two designs at once, sharing the pool; same results as
  // running them alone (determinism holds under contention too).
  Design a = make_design(3000, 21, false), b = make_design(3000, 22, false);
  Design a2 = a, b2 = b;
  FlowOptions o;
  o.placer.max_iterations = 80;
  o.placer.log_interval = 0;
  FlowReport ra, rb;
  JobOptions half;
  half.max_threads = std::max(1, par::num_threads() / 2);
  JobHandle ja = start_flow_job(a, o, &ra, half), jb = start_flow_job(b, o, &rb, half);
  ja.wait();
  jb.wait();
  const FlowReport sa = run_flow(a2, o), sb = run_flow(b2, o);
  CHECK(ra.placement.hpwl == sa.placement.hpwl && rb.placement.hpwl == sb.placement.hpwl);
}

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------

void test_partition() {
  Design d = make_design(5000, 12);
  region::Partition p(d, region::RegionGrid(d.die(), 4, 3));
  std::size_t total = 0;
  for (int r = 0; r < p.num_regions(); ++r) {
    for (Id c : p.cells(r)) {
      CHECK(p.region_of_cell(c) == r && p.grid().rect(r).contains(d.cell(c).center()));
    }
    total += p.cells(r).size();
  }
  CHECK(total == d.num_cells());
  CHECK(p.spatial_order().size() == d.num_cells());
  CHECK(p.num_cut_nets() > 0 && p.num_cut_nets() < d.num_nets());
  // Colors: same-color regions never touch.
  const auto& g = p.grid();
  for (int a = 0; a < g.size(); ++a)
    for (int b = a + 1; b < g.size(); ++b) {
      const bool touch = std::abs(a % g.nx() - b % g.nx()) <= 1 && std::abs(a / g.nx() - b / g.nx()) <= 1;
      if (touch) CHECK(g.color(a) != g.color(b));
    }
}

void test_region_problem_roundtrip() {
  Design d = make_design(3000, 13);
  region::Partition p(d, region::RegionGrid(d.die(), 3, 3));
  const region::RegionProblem a = region::extract(d, p, 4, "median");
  CHECK(a.num_cells() > 0 && a.num_nets() > 0);
  const auto bytes = region::serialize(a);
  const region::RegionProblem b = region::deserialize_problem(bytes);
  CHECK(b.cell_id == a.cell_id && b.cell_loc == a.cell_loc && b.net_start == a.net_start &&
        b.pin_cell == a.pin_cell && b.pin_pos == a.pin_pos && b.solver == "median" &&
        b.bounds == a.bounds && b.design_version == a.design_version);
  // Problem HPWL equals the design HPWL of the same nets.
  double ref = 0;
  for (Id n : a.net_id) ref += d.net(n).weight * static_cast<double>(net_hpwl(d, n));
  CHECK(a.hpwl() == ref);
  // Corrupt input is rejected, not trusted.
  auto bad = bytes;
  bad.resize(bad.size() / 2);
  CHECK(throws<std::runtime_error>([&] { region::deserialize_problem(bad); }));
  auto bad2 = bytes;
  bad2[0] ^= 0xff;
  CHECK(throws<std::runtime_error>([&] { region::deserialize_problem(bad2); }));
  const region::RegionSolution s = region::solve_median(a);
  const auto sb = region::deserialize_solution(region::serialize(s));
  CHECK(sb.cell_id == s.cell_id && sb.cell_loc == s.cell_loc && sb.hpwl_after == s.hpwl_after);
}

void test_region_local_only() {
  // Region-local work changes the region's cells and nothing else.
  Design d = make_design(8000, 14);
  d.set_tracking(true);
  d.flush();
  region::Partition p(d, region::RegionGrid(d.die(), 4, 4));
  const std::vector<int> regions{5, 10};
  const double before = total_hpwl(d);
  struct Spy : DesignListener {
    std::vector<Id> cells;
    void on_flush(const Design&, const ChangeSet& ch) override {
      cells.assign(ch.cells.ids().begin(), ch.cells.ids().end());
    }
  } spy;
  d.subscribe(&spy);
  region::LocalExecutor ex;
  const auto st = region::run_regions(d, p, regions, "median", ex);
  d.unsubscribe(&spy);
  CHECK(st.cells_moved > 0 && st.hpwl_after < st.hpwl_before);
  CHECK(spy.cells.size() == st.cells_moved);
  for (Id c : spy.cells) {
    const int r = p.region_of_cell(c);
    CHECK((r == 5 || r == 10) && d.cell(c).movable());
    CHECK(p.grid().rect(r).contains(d.cell(c).center()));  // stayed in its region
  }
  CHECK(total_hpwl(d) < before);
  // apply() refuses a solution that moves a cell outside the problem.
  const auto prob = region::extract(d, p, 5);
  region::RegionSolution evil;
  evil.region = 5;
  evil.design_version = prob.design_version;
  evil.cell_id = {p.cells(0)[0]};
  evil.cell_loc = {{0, 0}};
  CHECK(throws<std::invalid_argument>([&] { region::apply(d, prob, evil); }));
  // ...and a stale problem.
  d.move_cell(prob.cell_id[0], d.cell(prob.cell_id[0]).loc);
  d.flush();
  CHECK(throws<std::invalid_argument>([&] { region::apply(d, prob, region::solve_median(prob)); }));
}

void test_remote_loopback_matches_local() {
  Design a = make_design(6000, 15);
  Design b = a;
  region::Partition pa(a, region::RegionGrid(a.die(), 4, 4));
  region::Partition pb(b, region::RegionGrid(b.die(), 4, 4));
  region::LocalExecutor local;
  auto w1 = std::make_shared<region::LoopbackTransport>();
  auto w2 = std::make_shared<region::LoopbackTransport>();
  region::RemoteExecutor remote({w1, w2});
  for (int color = 0; color < region::RegionGrid::kColors; ++color) {
    region::run_regions(a, pa, pa.regions_of_color(color), "median", local);
    region::run_regions(b, pb, pb.regions_of_color(color), "median", remote);
  }
  bool same = true;
  for (Id c = 0; c < static_cast<Id>(a.num_cells()); ++c) same = same && a.cell(c).loc == b.cell(c).loc;
  CHECK(same);
  CHECK(w1->calls() + w2->calls() == 16 && remote.bytes_sent() > 0 && remote.bytes_received() > 0);
  // Unknown solver on the worker comes back as an error.
  region::RegionProblem p = region::extract(a, pa, 0, "no-such-solver");
  const auto resp = region::serve(region::serialize(p));
  CHECK(!resp.empty() && resp[0] == 1);
}

// ---------------------------------------------------------------------------
// Compute backends
// ---------------------------------------------------------------------------

void test_backend_registry() {
  CHECK(compute::backend().name() == "cpu" && !compute::backend().is_gpu());
  const auto names = compute::backends();
  CHECK(!names.empty() && names.front() == "cpu");
  CHECK(throws<std::invalid_argument>([] { compute::set_backend("no-such-gpu"); }));
  compute::register_backend("unavailable", [] { return std::unique_ptr<compute::Backend>(); });
  CHECK(throws<std::invalid_argument>([] { compute::set_backend("unavailable"); }));
  compute::set_backend("cpu");
  CHECK(compute::backend().name() == "cpu");
}

}  // namespace

int main() {
  log::configure("warn");
  std::printf("threads: %d\n", par::num_threads());
  struct {
    const char* name;
    void (*fn)();
  } tests[] = {
      {"parallel_for_covers_range", test_parallel_for_covers_range},
      {"reduce_deterministic", test_reduce_deterministic},
      {"nested_and_concurrent_loops", test_nested_and_concurrent_loops},
      {"exceptions_propagate", test_exceptions_propagate},
      {"thread_limit", test_thread_limit},
      {"row_bands_match_serial", test_row_bands_match_serial},
      {"poisson_identical_across_threads", test_poisson_identical_across_threads},
      {"rudy_fast_matches_reference", test_rudy_fast_matches_reference},
      {"placement_identical_across_threads", test_placement_identical_across_threads},
      {"move_cells", test_move_cells},
      {"snapshot_tracking_off_shares_nets_and_pins", test_snapshot_tracking_off_shares_nets_and_pins},
      {"snapshot_region_order", test_snapshot_region_order},
      {"job_runs_with_snapshot_reader", test_job_runs_with_snapshot_reader},
      {"job_cancel", test_job_cancel},
      {"job_failure_and_handle_lifetime", test_job_failure_and_handle_lifetime},
      {"jobs_side_by_side", test_jobs_side_by_side},
      {"partition", test_partition},
      {"region_problem_roundtrip", test_region_problem_roundtrip},
      {"region_local_only", test_region_local_only},
      {"remote_loopback_matches_local", test_remote_loopback_matches_local},
      {"backend_registry", test_backend_registry},
  };
  for (const auto& t : tests) {
    const int before = g_failures;
    t.fn();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
  }
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all parallel tests passed\n");
  return 0;
}
