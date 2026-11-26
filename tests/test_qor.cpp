// QoR report, 3-D route usage, routing layers (LEF + .hdb) and run tracking.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

#include "hermes/core/json.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/io/design_file.hpp"
#include "hermes/io/lefdef.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/qor/qor.hpp"
#include "hermes/route/global_router.hpp"
#include "hermes/route/route_usage.hpp"
#include "hermes/track/run_tracker.hpp"

#if HERMES_HAVE_SQLITE
#include <sqlite3.h>
#endif

using namespace hermes;
namespace fs = std::filesystem;

namespace {

HERMES_LOG_CHANNEL(log_test, "test.track");
int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)
#define CHECK_NEAR(a, b, tol) CHECK(std::abs((a) - (b)) <= (tol) * (1.0 + std::abs(b)))

const std::string kData = HERMES_TEST_DATA_DIR;

fs::path tmp(const std::string& name) {
  return fs::temp_directory_path() / ("hermes_qor_test_" + std::to_string(::getpid()) + "_" + name);
}

Design make_design(int n, unsigned seed) {
  std::mt19937 rng(seed);
  Design d("t");
  const Dbu side = static_cast<Dbu>(std::sqrt(n * 2000.0 * 1000.0 / 0.6) / 1000 + 1) * 1000;
  d.set_die({0, 0, side, side});
  d.set_row_height(1000);
  std::uniform_int_distribution<Dbu> px(0, side - 2000), py(0, side / 1000 - 1);
  for (int i = 0; i < n; ++i) {
    const Id c = d.add_cell("u" + std::to_string(i), 2000, 1000);
    d.move_cell(c, {px(rng), py(rng) * 1000});
  }
  const Id m = d.add_cell("ram", side / 4, side / 4, true);
  d.move_cell(m, {side / 3, side / 3});
  d.set_fixed(m, true);
  std::uniform_int_distribution<int> deg(2, 5), near(-30, 30);
  for (int i = 0; i < n; ++i) {
    const Id net = d.add_net("n" + std::to_string(i));
    const int k = deg(rng);
    for (int p = 0; p < k; ++p) {
      const int c = p == 0 ? i : std::clamp(i + near(rng), 0, n - 1);
      d.connect(c, net, "p" + std::to_string(p), {1000, 500}, p == 0 ? PinDir::Output : PinDir::Input);
    }
  }
  return d;
}

// --- Routing layers ----------------------------------------------------------

void test_lef_layers_and_hdb_roundtrip() {
  const Design d = io::read_lef_def({kData + "/sample.lef"}, kData + "/sample.def");
  const auto L = d.routing_layers();
  CHECK(L.size() == 2);
  if (L.size() == 2) {
    CHECK(L[0].name == "metal1");
    CHECK(L[0].dir == LayerDir::Horizontal);
    CHECK(L[1].dir == LayerDir::Vertical);
    const Dbu p = static_cast<Dbu>(std::llround(0.2 * d.dbu_per_micron()));
    CHECK(L[0].pitch == p);
    CHECK(L[1].width == p / 2);
  }
  const fs::path f = tmp("layers.hdb");
  io::save_design(d, f.string());
  const Design e = io::load_design(f.string());
  CHECK(e.routing_layers().size() == L.size());
  for (std::size_t i = 0; i < std::min(L.size(), e.routing_layers().size()); ++i) {
    CHECK(e.routing_layers()[i].name == L[i].name);
    CHECK(e.routing_layers()[i].pitch == L[i].pitch);
    CHECK(e.routing_layers()[i].dir == L[i].dir);
  }
  fs::remove(f);
  const LayerStack st = layer_stack_for(d);
  CHECK(st.source == "lef");
}

void test_default_layer_stack() {
  Design d("x");
  d.set_die({0, 0, 90000, 90000});
  d.set_row_height(900);
  const LayerStack st = layer_stack_for(d);
  CHECK(st.source.rfind("default", 0) == 0);
  CHECK(st.num_layers() == 6);
  CHECK(!st.layers[0].routable);
  CHECK(st.layers[1].routable && st.layers[1].dir == LayerDir::Vertical);
  CHECK(st.layers[0].pitch == 100);
}

// --- 3-D usage ---------------------------------------------------------------

// One two-pin horizontal net across 3 GCells and one L-shaped net.
void test_route_usage_simple() {
  Design d("u");
  d.set_die({0, 0, 1000, 1000});
  d.set_row_height(90);  // default pitch 10
  const Id a = d.add_cell("a", 1, 1), b = d.add_cell("b", 1, 1), c = d.add_cell("c", 1, 1);
  d.move_cell(a, {50, 50});
  d.move_cell(b, {350, 50});
  d.move_cell(c, {350, 650});
  const Id n1 = d.add_net("n1");
  d.connect(a, n1, "z", {}, PinDir::Output);
  d.connect(b, n1, "a");
  const Id n2 = d.add_net("n2");
  d.connect(b, n2, "z", {}, PinDir::Output);
  d.connect(c, n2, "a");
  RouteUsageOptions o;
  o.grid_x = o.grid_y = 10;  // 100 x 100 GCells
  const RouteUsage3D u = build_route_usage(d, o);
  CHECK(u.num_layers() == 6);
  CHECK(u.num_segments == 2);
  CHECK_NEAR(u.total_wirelength(), 300.0 + 600.0, 1e-12);
  // n1: 300 long horizontal in row 0, 3 GCells (tier 1 -> second H layer = M5, index 4).
  // Covered: gcell0 50, gcell1 100, gcell2 100, gcell3 50 -> 0.5, 1, 1, 0.5 tracks.
  CHECK_NEAR(u.demand[4][u.grid.index(0, 0)], 0.5, 1e-6);
  CHECK_NEAR(u.demand[4][u.grid.index(1, 0)], 1.0, 1e-6);
  CHECK_NEAR(u.demand[4][u.grid.index(2, 0)], 1.0, 1e-6);
  CHECK_NEAR(u.demand[4][u.grid.index(3, 0)], 0.5, 1e-6);
  CHECK_NEAR(u.wirelength[4], 300.0, 1e-12);
  // n2: vertical 600 (6 GCells -> tier 1 -> second V layer = M4, index 3).
  CHECK_NEAR(u.wirelength[3], 600.0, 1e-12);
  double col = 0;
  for (int iy = 0; iy < 10; ++iy) col += u.demand[3][u.grid.index(3, iy)];
  CHECK_NEAR(col, 6.0, 1e-6);
  // Capacity: horizontal layer tracks = GCell height / pitch (100 / 10).
  CHECK_NEAR(u.capacity[2][0], 10.0, 1e-6);
  CHECK_NEAR(u.capacity[0][0], 0.0, 1e-6);  // M1 pin access
  // Vias: n1 source 0->4, sink 0->4 (8); n2 source b: 0->3 (3), sink 0->3 (3).
  CHECK(u.total_vias() == 14);
}

void test_usage_and_qor_identical_across_threads() {
  const Design d = make_design(3000, 5);
  QorOptions qo;
  std::string j1, j4;
  std::vector<float> m1, m4;
  for (int t : {1, 4}) {
    par::set_num_threads(t);
    QorReport q = compute_qor(d, qo, nullptr, 48, 48);
    q.seconds.clear();
    q.total_seconds = 0;
    (t == 1 ? j1 : j4) = qor_to_json(q);
    (t == 1 ? m1 : m4) = q.map("rudy.util")->values;
  }
  par::set_num_threads(0);
  CHECK(j1 == j4);
  CHECK(m1 == m4);
}

// --- RUDY --------------------------------------------------------------------

void test_rudy_by_direction_matches_brute_force() {
  const Design d = make_design(800, 9);
  for (int grid : {7, 32, 300}) {  // 300^2 > 64K bins: the atomic path
    const BinGrid g(d.die(), grid, grid);
    std::vector<double> h, v;
    rudy_by_direction(d, g, h, v);
    std::vector<double> rh(g.size(), 0.0), rv(g.size(), 0.0);
    for (std::size_t n = 0; n < d.num_nets(); ++n) {
      Rect bb = net_bbox(d, static_cast<Id>(n));
      bb.xhi = std::max(bb.xhi, bb.xlo + 1);
      bb.yhi = std::max(bb.yhi, bb.ylo + 1);
      const double area = static_cast<double>(bb.area());
      g.for_each_bin(bb, [&](int ix, int iy) {
        const double a = static_cast<double>(g.bin_rect(ix, iy).overlap_area(bb));
        rh[g.index(ix, iy)] += static_cast<double>(bb.width()) * a / area;
        rv[g.index(ix, iy)] += static_cast<double>(bb.height()) * a / area;
      });
    }
    double err = 0, tot = 0;
    for (std::size_t k = 0; k < g.size(); ++k) {
      err = std::max({err, std::abs(h[k] - rh[k]), std::abs(v[k] - rv[k])});
      tot += h[k];
    }
    // Fixed-point rounding: at most 1/512 DBU per term and net.
    CHECK(err < 0.5);
    CHECK(tot > 0);
  }
}

// --- Density -----------------------------------------------------------------

void test_density_numbers() {
  Design d("dens");
  d.set_die({0, 0, 2000, 1000});
  d.set_row_height(100);  // bins of 10 rows = 1000: 2 x 1
  const Id c = d.add_cell("c", 1000, 900);  // 90% of bin 0
  d.move_cell(c, {0, 0});
  const Id f = d.add_cell("f", 500, 1000);  // fixed, half of bin 1
  d.move_cell(f, {1500, 0});
  d.set_fixed(f, true);
  QorOptions o;
  o.target_density = 0.5;
  const DensityQor q = compute_density_qor(d, o);
  CHECK(q.nx == 2 && q.ny == 1);
  CHECK_NEAR(q.movable_area, 900000.0, 1e-12);
  CHECK_NEAR(q.free_area, 1500000.0, 1e-12);
  CHECK_NEAR(q.avg_util, 0.6, 1e-12);
  CHECK_NEAR(q.peak_util, 0.9, 1e-12);
  // Overflow: bin 0: 900000 - 0.5 * 1e6 = 400000 -> / 900000.
  CHECK_NEAR(q.overflow, 400000.0 / 900000.0, 1e-12);
  // Evenness over the 2 bins: utils 0.9 and 0.
  CHECK_NEAR(q.util_mean, 0.45, 1e-12);
  CHECK_NEAR(q.cv, 1.0, 1e-9);
  CHECK_NEAR(q.overfull_share, 0.5, 1e-12);
  CHECK_NEAR(q.over100_share, 0.0, 1e-12);
}

// --- Flow --------------------------------------------------------------------

void test_flow_report_has_qor() {
  Design d = make_design(1500, 3);
  FlowOptions o;
  o.placer.max_iterations = 60;
  o.router.grid_x = o.router.grid_y = 24;
  const FlowReport r = run_flow(d, o);
  CHECK(r.has_qor);
  CHECK(r.qor.congestion.routed);
  CHECK_NEAR(r.qor.wirelength.routed, r.global_route.wirelength, 1e-9);
  CHECK_NEAR(r.qor.wirelength.hpwl, total_hpwl(d), 1e-12);
  CHECK(r.qor.map("density.util") != nullptr);
  CHECK(r.qor.map("congestion.h.util") != nullptr);
  CHECK(r.qor.map("vias.total") != nullptr);
  CHECK(!r.stage_seconds.empty() && r.stage_seconds.back().first == "report");
  const std::string text = format_qor(r.qor);
  CHECK(text.find("Placement density") != std::string::npos);
  CHECK(text.find("ESTIMATES") != std::string::npos);
  CHECK(text.find("2-D numbers in the log disagree") != std::string::npos);
  const std::string maps = qor_maps_to_json(r.qor, 8);
  CHECK(maps.find("\"nx\":8") != std::string::npos);

  Design p = make_design(500, 4);
  o.run_routing = false;
  const FlowReport rp = run_flow(p, o);
  CHECK(rp.has_qor && !rp.qor.congestion.routed);
}

void test_json_writer() {
  JsonWriter j;
  j.begin_object().kv("a\"b", "x\n\x01").kv("n", 3).kv("f", 0.5).key("arr").begin_array().value(1).null().end_array();
  j.kv("nan", std::nan("")).end_object();
  CHECK(j.str() == "{\"a\\\"b\":\"x\\n\\u0001\",\"n\":3,\"f\":0.5,\"arr\":[1,null],\"nan\":null}");
}

// --- Run tracking ------------------------------------------------------------

void test_tracker_writes_database() {
#if HERMES_HAVE_SQLITE
  const fs::path db = tmp("runs.db");
  fs::remove(db);
  std::string id;
  {
    track::TrackerOptions to;
    to.db_path = db.string();
    to.flush_interval_s = 0.05;
    track::RunTracker t({"unit", "", "in", "cmd", {"a", "b"}, "{\"k\":1}"}, to);
    CHECK(t.db_ok());
    id = t.run_id();
    CHECK(track::RunTracker::active() == &t);
    t.attach_log();
    t.set_design("tdesign");
    RunControl ctl(0.0);
    t.attach(ctl);
    ctl.begin_stage("placement", 0, 0.5);
    for (int i = 0; i < 100; ++i) track::metric("m", i, i * 2.0);
    HLOG_WARN(log_test, "hello {}", 42);
    ctl.begin_stage("route", 0.5, 1.0);
    t.summary("hpwl", 123.0);
    t.sync();
    t.finish("done");
    CHECK(track::RunTracker::active() == nullptr);
  }
  sqlite3* h = nullptr;
  CHECK(sqlite3_open(db.string().c_str(), &h) == SQLITE_OK);
  auto one = [&](const std::string& sql) {
    sqlite3_stmt* s = nullptr;
    std::string out;
    if (sqlite3_prepare_v2(h, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) {
      const auto* p = sqlite3_column_text(s, 0);
      out = p ? reinterpret_cast<const char*>(p) : "";
    }
    sqlite3_finalize(s);
    return out;
  };
  CHECK(one("SELECT status FROM runs WHERE id='" + id + "'") == "done");
  CHECK(one("SELECT design FROM runs WHERE id='" + id + "'") == "tdesign");
  CHECK(one("SELECT tags FROM runs WHERE id='" + id + "'") == "[\"a\",\"b\"]");
  CHECK(one("SELECT count(*) FROM metrics WHERE run_id='" + id + "' AND name='m'") == "100");
  CHECK(one("SELECT count(*) FROM stages WHERE run_id='" + id + "' AND ended IS NOT NULL") == "2");
  CHECK(one("SELECT text FROM events WHERE run_id='" + id + "' AND kind='log'") == "hello 42");
  CHECK(one("SELECT summary FROM runs WHERE id='" + id + "'").find("\"hpwl\":123") != std::string::npos);
  CHECK(one("SELECT version FROM runs WHERE id='" + id + "'") == track::build_version());
  sqlite3_close(h);
  fs::remove(db);
  fs::remove(db.string() + "-wal");
  fs::remove(db.string() + "-shm");
#endif
}

void test_tracker_unreachable_server_does_not_block() {
  track::TrackerOptions to;
  to.db_path = "-";
  to.server_url = "http://127.0.0.1:9";  // discard port: connection refused
  to.flush_interval_s = 0.05;
  log::configure("error");
  track::RunTracker t({"x", "d", "", "", {}, ""}, to);
  for (int i = 0; i < 1000; ++i) t.metric("m", i, i);
  t.sync();
  CHECK(t.push_failures() > 0);
  t.finish("done");
  log::configure("warn");
}

}  // namespace

int main() {
  log::configure("warn");
  struct {
    const char* name;
    void (*fn)();
  } tests[] = {
      {"lef_layers_and_hdb_roundtrip", test_lef_layers_and_hdb_roundtrip},
      {"default_layer_stack", test_default_layer_stack},
      {"route_usage_simple", test_route_usage_simple},
      {"usage_and_qor_identical_across_threads", test_usage_and_qor_identical_across_threads},
      {"rudy_by_direction_matches_brute_force", test_rudy_by_direction_matches_brute_force},
      {"density_numbers", test_density_numbers},
      {"flow_report_has_qor", test_flow_report_has_qor},
      {"json_writer", test_json_writer},
      {"tracker_writes_database", test_tracker_writes_database},
      {"tracker_unreachable_server_does_not_block", test_tracker_unreachable_server_does_not_block},
  };
  for (const auto& t : tests) {
    const int before = g_failures;
    t.fn();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
  }
  std::printf("%d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
