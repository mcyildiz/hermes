// Core design-database benchmark (`hermes_db_bench`).
//
// Builds a synthetic netlist (default 1M cells, 1M nets, ~4M pins) and times
// the operations engines lean on: building, full HPWL, moving every cell with
// change tracking off/on and inside a transaction, flushing changes to
// listeners, rolling back, incremental HPWL, and publishing snapshots for
// concurrent readers. A small full flow (place + route) is timed at the end.
//
//   ./build/hermes_db_bench [num_cells] [flow_cells]
//
// Build with -DHERMES_OLD_DB against the pre-restructure tree to get the
// baseline numbers for the operations both versions have.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "hermes/db/design.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/log/log.hpp"
#ifndef HERMES_OLD_DB
#include "hermes/db/geometry.hpp"
#include "hermes/db/snapshot.hpp"
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#include <fstream>
#endif

using namespace hermes;

namespace {

#ifdef HERMES_OLD_DB
#define MOVE(d, i, p) ((d).cell(i).loc = (p))
#define FIX(d, i, f) ((d).cell(i).fixed = (f))
#define HPWL(d) (d).total_hpwl()
#else
#define MOVE(d, i, p) (d).move_cell((i), (p))
#define FIX(d, i, f) (d).set_fixed((i), (f))
#define HPWL(d) total_hpwl(d)
#endif

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::size_t rss_bytes() {
#if defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                &n) != KERN_SUCCESS)
    return 0;
  return info.resident_size;
#elif defined(__linux__)
  std::ifstream f("/proc/self/statm");
  std::size_t pages = 0, resident = 0;
  f >> pages >> resident;
  return resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#else
  return 0;
#endif
}

template <class F>
double time_best(int reps, F&& f) {
  double best = 1e300;
  for (int r = 0; r < reps; ++r) {
    const double t0 = now_s();
    f();
    best = std::min(best, now_s() - t0);
  }
  return best;
}

void row(const char* what, double seconds, double per = 0.0, const char* unit = "") {
  if (per > 0)
    std::printf("  %-46s %10.3f ms  %8.2f ns/%s\n", what, seconds * 1e3, seconds * 1e9 / per, unit);
  else
    std::printf("  %-46s %10.3f ms\n", what, seconds * 1e3);
}

Design make_design(int num_cells, unsigned seed, Dbu* side_out = nullptr) {
  std::mt19937 rng(seed);
  const Dbu rh = 1000;
  std::uniform_int_distribution<Dbu> width(200, 800);
  std::vector<Dbu> w(num_cells);
  double area = 0;
  for (auto& x : w) { x = width(rng); area += static_cast<double>(x) * rh; }
  const Dbu side = static_cast<Dbu>(std::sqrt(area / 0.7) / rh + 1) * rh;
  if (side_out) *side_out = side;

  Design d("synthetic");
  d.set_die({0, 0, side, side});
  d.set_row_height(rh);
  d.reserve(num_cells, num_cells, 4 * static_cast<std::size_t>(num_cells));
  std::uniform_int_distribution<Dbu> pos(0, side - 1000);
  char name[32];
  for (int i = 0; i < num_cells; ++i) {
    std::snprintf(name, sizeof(name), "u%d", i);
    const Id c = d.add_cell(name, w[i], rh);
    MOVE(d, c, (Point{pos(rng), pos(rng) / rh * rh}));
  }
  // Nets of 2..6 pins; pins land on cells near the driver (locality, like a
  // real netlist) so incremental work stays local.
  std::uniform_int_distribution<int> deg(2, 6), near(-200, 200);
  static const char* kPin[] = {"Z", "A", "B", "C", "D", "E"};
  for (int n = 0; n < num_cells; ++n) {
    std::snprintf(name, sizeof(name), "n%d", n);
    const Id net = d.add_net(name);
    const int k = deg(rng);
    for (int p = 0; p < k; ++p) {
      const int c = p == 0 ? n : std::clamp(n + near(rng), 0, num_cells - 1);
      d.connect(c, net, kPin[p], Point{w[c] / 2, rh / 2}, p == 0 ? PinDir::Output : PinDir::Input);
    }
  }
  return d;
}

void bench_db(int num_cells) {
  std::printf("DB: %d cells\n", num_cells);
  const std::size_t rss0 = rss_bytes();
  double t0 = now_s();
  Design d = make_design(num_cells, 1);
#ifndef HERMES_OLD_DB
  d.compact();
#endif
  const double t_build = now_s() - t0;
  const std::size_t rss1 = rss_bytes();
  std::printf("  nets %zu, pins %zu\n", d.nets().size(), d.pins().size());
  row("build (add cells, nets, connect)", t_build, num_cells, "cell");
  std::printf("  %-46s %10.1f MB\n", "RSS growth after build", (rss1 - rss0) / 1048576.0);

  double h = 0;
  const double t_hpwl = time_best(5, [&] { h = HPWL(d); });
  row("full HPWL", t_hpwl, static_cast<double>(d.nets().size()), "net");

  std::vector<Point> locs(num_cells);
  for (int i = 0; i < num_cells; ++i) locs[i] = d.cell(i).loc + Point{1, 0};
  auto move_all = [&] {
    for (int i = 0; i < num_cells; ++i) MOVE(d, i, locs[i]);
  };
#ifdef HERMES_OLD_DB
  row("move all cells (direct struct write)", time_best(5, move_all), num_cells, "cell");
#else
  d.set_tracking(false);
  row("move all cells, tracking off", time_best(5, move_all), num_cells, "cell");
  d.flush();
  d.set_tracking(true);
  {
    double t_on = 1e300, t_again = 1e300, t_flush = 1e300;
    for (int r = 0; r < 5; ++r) {
      d.flush();
      double t1 = now_s();
      move_all();  // first move of each cell in the batch: marks it
      t_on = std::min(t_on, now_s() - t1);
      t1 = now_s();
      move_all();  // already marked: bit test only
      t_again = std::min(t_again, now_s() - t1);
      t1 = now_s();
      d.flush();
      t_flush = std::min(t_flush, now_s() - t1);
      move_all();
    }
    row("move all cells, tracking on (first move)", t_on, num_cells, "cell");
    row("move all cells, tracking on (already dirty)", t_again, num_cells, "cell");
    row("flush (derive nets, notify) after all moved", t_flush, num_cells, "cell");
    d.flush();
    const int k1 = num_cells / 100;
    double t_f1 = 1e300;
    std::mt19937 r1(3);
    for (int r = 0; r < 5; ++r) {
      for (int i = 0; i < k1; ++i) { const Id c = static_cast<Id>(r1() % num_cells); d.move_cell(c, locs[c]); }
      const double t1 = now_s();
      d.flush();
      t_f1 = std::min(t_f1, now_s() - t1);
    }
    row("flush after 1% cells moved", t_f1);
  }
  {
    double t_tx = 0, t_rb = 0;
    t_tx = time_best(3, [&] {
      Transaction tx(d);
      move_all();
      const double t1 = now_s();
      tx.rollback();
      t_rb = now_s() - t1;
    });
    row("move all cells, tracking on + transaction", t_tx - t_rb, num_cells, "cell");
    row("rollback of 1M moves", t_rb, num_cells, "cell");
    d.flush();
  }

  // Incremental HPWL through NetBoxCache.
  NetBoxCache boxes(d);
  row("NetBoxCache full build", time_best(1, [&] { h = static_cast<double>(boxes.total_hpwl()); }));
  std::mt19937 rng(5);
  std::uniform_int_distribution<int> pick(0, num_cells - 1);
  const double t_inc1 = time_best(5, [&] {
    const Id c = pick(rng);
    d.move_cell(c, d.cell(c).loc + Point{50, 0});
    d.flush();
    h = static_cast<double>(boxes.total_hpwl());
  });
  row("move 1 cell + flush + incremental HPWL", t_inc1);
  const int k = num_cells / 100;
  const double t_inc = time_best(3, [&] {
    for (int i = 0; i < k; ++i) {
      const Id c = pick(rng);
      d.move_cell(c, d.cell(c).loc + Point{50, 0});
    }
    d.flush();
    h = static_cast<double>(boxes.total_hpwl());
  });
  row("move 1% cells + flush + incremental HPWL", t_inc);
  if (static_cast<Dbu>(h) != static_cast<Dbu>(total_hpwl(d))) std::printf("  !! incremental HPWL mismatch\n");

  // Snapshots for concurrent readers.
  const std::size_t rss2 = rss_bytes();
  double t_snap = now_s();
  SnapshotPublisher pub(d);
  t_snap = now_s() - t_snap;
  row("first snapshot (full copy)", t_snap);
  std::printf("  %-46s %10.1f MB\n", "RSS growth for one snapshot", (rss_bytes() - rss2) / 1048576.0);
  row("publish after 1 cell moved", time_best(5, [&] {
        const Id c = pick(rng);
        d.move_cell(c, d.cell(c).loc + Point{10, 0});
        d.flush();
        pub.publish();
      }));
  row("publish after 1% cells moved", time_best(3, [&] {
        for (int i = 0; i < k; ++i) { const Id c = pick(rng); d.move_cell(c, d.cell(c).loc + Point{10, 0}); }
        d.flush();
        pub.publish();
      }));
  row("publish after all cells moved", time_best(3, [&] {
        move_all();
        d.flush();
        pub.publish();
      }));
  row("reader: latest() + read 1M cell locs", time_best(5, [&] {
        auto s = pub.latest();
        Dbu acc = 0;
        for (Id i = 0; i < static_cast<Id>(s->num_cells()); ++i) acc += s->cell(i).loc.x;
        if (acc == 42) std::printf(" ");
      }), num_cells, "cell");
#endif
  if (h < 0) std::printf("%g\n", h);
}

void bench_flow(int num_cells) {
  std::printf("Flow: %d cells, 200 GP iterations, legalize, global route\n", num_cells);
  Dbu side = 0;
  Design d = make_design(num_cells, 2, &side);
  // A few fixed anchors on the boundary.
  for (int i = 0; i < 16; ++i) {
    MOVE(d, i, (Point{(i % 2) ? 0 : side - 1000, (i * side / 16) / 1000 * 1000}));
    FIX(d, i, true);
  }
  FlowOptions o;
  o.placer.max_iterations = 200;
  o.placer.stop_overflow = 0.0;
  o.placer.log_interval = 0;
  const double t0 = now_s();
  FlowReport r = run_flow(d, o);
  row("run_flow", now_s() - t0);
  std::printf("  HPWL %.6g, GP iterations %d\n", r.placement.hpwl, r.placement.iterations);
}

}  // namespace

int main(int argc, char** argv) {
  const int n = argc > 1 ? std::atoi(argv[1]) : 1000000;
  const int nf = argc > 2 ? std::atoi(argv[2]) : 20000;
  log::configure("warn");
  bench_db(n);
  if (nf > 0) bench_flow(nf);
  return 0;
}
