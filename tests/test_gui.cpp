// GUI scene-model tests (no graphics needed): estimated wires agree with the
// router's 3-D usage, scenes hold every cell in the right tile, scene builds
// are identical for any thread count, detail windows, picking with layer
// visibility/selectability, the session's background jobs (UI never blocks,
// edits refused while a job runs), and console commands.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>

#include "gui/scene/commands.hpp"
#include "gui/scene/scene.hpp"
#include "gui/scene/session.hpp"
#include "gui/scene/worker.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/db/synthetic.hpp"
#include "hermes/io/design_file.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/route/route_usage.hpp"

using namespace hermes;
using namespace hermes::gui;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Design small(int n = 5000, SyntheticOptions::Placement pl = SyntheticOptions::Placement::Ordered) {
  SyntheticOptions o;
  o.cells = n;
  o.placement = pl;
  return make_synthetic_design(o);
}

std::shared_ptr<const DesignSnapshot> snap_of(Design& d) {
  SnapshotPublisher pub(d);
  return pub.latest();
}

void test_estimated_routes_match_route_usage() {
  Design d = small(4000);
  RouteUsageOptions ro;
  const RouteUsage3D u = build_route_usage(d, ro);
  auto tech = make_tech_info(d);
  std::vector<double> wl(static_cast<std::size_t>(u.num_layers()), 0.0);
  std::uint64_t vias = 0;
  std::vector<Point> pts;
  std::vector<WireSegment> segs;
  std::vector<ViaShape> vs;
  for (std::size_t n = 0; n < d.num_nets(); ++n) {
    pts.clear();
    for (Id p : d.net_pins(static_cast<Id>(n))) pts.push_back(pin_location(d, p));
    segs.clear();
    vs.clear();
    tech->routes.route_net(pts, segs, &vs);
    for (const WireSegment& s : segs) {
      CHECK(s.a.x == s.b.x || s.a.y == s.b.y);
      wl[static_cast<std::size_t>(s.layer)] += static_cast<double>(std::abs(s.b.x - s.a.x) + std::abs(s.b.y - s.a.y));
    }
    for (const ViaShape& v : vs) vias += static_cast<std::uint64_t>(v.to - v.from);
  }
  for (int l = 0; l < u.num_layers(); ++l)
    CHECK(std::abs(wl[static_cast<std::size_t>(l)] - u.wirelength[static_cast<std::size_t>(l)]) < 0.5);
  CHECK(vias == u.total_vias());
  CHECK(u.total_wirelength() > 0);
}

void test_snapshot_has_masters_and_rows() {
  Design d = small(300);
  auto s = snap_of(d);
  CHECK(s->cell_master(0).starts_with("CELLW"));
  CHECK(s->rows().size() == d.rows().size());
  CHECK(s->dbu_per_micron() == 2000);
}

void test_scene_holds_every_cell() {
  Design d = small(20000);
  auto snap = snap_of(d);
  auto sc = build_scene(snap);
  std::size_t live_std = 0, macros = 0;
  double area = 0;
  for (const Cell& c : d.cells()) {
    if (c.removed) continue;
    if (c.is_macro) {
      ++macros;
    } else {
      ++live_std;
      area += static_cast<double>(c.width) * c.height;
    }
  }
  CHECK(sc->cells.size() == live_std);
  CHECK(sc->macros.size() == macros);
  CHECK(sc->tile_begin.back() == sc->cells.size());
  for (std::size_t t = 0; t + 1 < sc->tile_begin.size(); ++t) CHECK(sc->tile_begin[t] <= sc->tile_begin[t + 1]);
  // Each cell is in the tile of its center and matches the design.
  int bad = 0;
  for (int iy = 0; iy < sc->tiles_y; ++iy)
    for (int ix = 0; ix < sc->tiles_x; ++ix) {
      const auto [b, e] = sc->tile_row_range(iy, ix, ix);
      for (std::uint32_t k = b; k < e; ++k) {
        const Instance& r = sc->cells[k];
        const double cx = 0.5 * (r.x0 + r.x1), cy = 0.5 * (r.y0 + r.y1);
        if (static_cast<int>(cx / sc->tile_w) != ix && !(ix == sc->tiles_x - 1)) ++bad;
        if (static_cast<int>(cy / sc->tile_h) != iy && !(iy == sc->tiles_y - 1)) ++bad;
        const Cell& c = d.cell(sc->cell_ids[k]);
        if (r.x0 != static_cast<float>(c.loc.x - d.die().xlo) || r.x1 - r.x0 != static_cast<float>(c.width)) ++bad;
      }
    }
  CHECK(bad == 0);
  // Density image conserves std-cell area.
  const ScalarImage& img = sc->density;
  double sum = 0;
  for (float v : img.values) sum += v;
  const double bin_area = static_cast<double>(d.die().width()) / img.w * (static_cast<double>(d.die().height()) / img.h);
  CHECK(std::abs(sum * bin_area - area) / area < 1e-3);
  CHECK(img.vmax > 0 && img.vmax < 1.5);
}

void test_scene_identical_across_threads() {
  Design d = small(30000, SyntheticOptions::Placement::Random);
  auto snap = snap_of(d);
  SceneOptions o;
  o.pin_density = true;
  std::shared_ptr<Scene> a, b;
  {
    par::ThreadLimit one(1);
    a = build_scene(snap, o);
  }
  b = build_scene(snap, o);
  CHECK(a->cells.size() == b->cells.size());
  CHECK(std::memcmp(a->cells.data(), b->cells.data(), a->cells.size() * sizeof(Instance)) == 0);
  CHECK(a->cell_ids == b->cell_ids);
  CHECK(a->density.values == b->density.values);
  CHECK(a->pin_density.values == b->pin_density.values);
  CHECK(!a->pin_density.empty());
}

void test_detail_window() {
  Design d = small(20000);
  auto snap = snap_of(d);
  auto sc = build_scene(snap);
  auto tech = make_tech_info(d);
  const Rect die = d.die();
  DetailOptions o;
  o.window = {die.xlo + die.width() * 2 / 5, die.ylo + die.height() * 2 / 5, die.xlo + die.width() * 3 / 5,
              die.ylo + die.height() * 3 / 5};
  auto det = build_detail(*sc, *tech, o);
  // Pins: exactly those of cells overlapping the window.
  std::size_t want_pins = 0;
  std::size_t want_nets = 0;
  for (std::size_t c = 0; c < d.num_cells(); ++c) {
    const Rect b = d.cell(static_cast<Id>(c)).bbox();
    if (b.xhi < o.window.xlo || b.xlo > o.window.xhi || b.yhi < o.window.ylo || b.ylo > o.window.yhi) continue;
    want_pins += d.cell_pins(static_cast<Id>(c)).size();
  }
  for (std::size_t n = 0; n < d.num_nets(); ++n) {
    if (d.net_pins(static_cast<Id>(n)).size() < 2) continue;
    const Rect b = net_bbox(d, static_cast<Id>(n));
    if (b.xhi < o.window.xlo || b.xlo > o.window.xhi || b.yhi < o.window.ylo || b.ylo > o.window.yhi) continue;
    ++want_nets;
  }
  CHECK(det->pins.size() == want_pins);
  CHECK(det->nets == want_nets);
  CHECK(!det->wires.empty());
  CHECK(det->wires.size() == det->wire_nets.size());
  CHECK(!det->truncated);
  // Budget.
  o.max_nets = 10;
  auto small_det = build_detail(*sc, *tech, o);
  CHECK(small_det->truncated && small_det->nets == 10);
}

void test_pick() {
  Design d = small(5000);
  auto snap = snap_of(d);
  auto sc = build_scene(snap);
  auto tech = make_tech_info(d);
  LayerSet layers;
  layers.set_metals(tech->routes.stack());
  PickContext ctx{sc.get(), nullptr, &layers, 1.0, 0.0};
  const Id c = 1234;
  const Point at = d.cell(c).center();
  PickResult r = pick(ctx, at);
  CHECK(r.kind == PickResult::Kind::Cell && r.id == c);
  layers[LayerSet::kStdCells].selectable = false;
  r = pick(ctx, at);
  CHECK(r.kind != PickResult::Kind::Cell || r.id != c);
  layers[LayerSet::kStdCells].selectable = true;
  layers[LayerSet::kStdCells].visible = false;
  CHECK(pick(ctx, at).kind == PickResult::Kind::None);
  layers[LayerSet::kStdCells].visible = true;
  // Macro.
  Id macro = kInvalidId;
  for (std::size_t i = 0; i < d.num_cells(); ++i)
    if (d.cell(static_cast<Id>(i)).is_macro) macro = static_cast<Id>(i);
  r = pick(ctx, d.cell(macro).center());
  CHECK(r.kind == PickResult::Kind::Cell && r.id == macro);
  // Pin (needs detail) beats the cell under it.
  DetailOptions o;
  o.window = d.cell(c).bbox();
  auto det = build_detail(*sc, *tech, o);
  ctx.detail = det.get();
  ctx.tolerance_px = 3.0;
  const Id pin = d.cell_pins(c)[0];
  r = pick(ctx, pin_location(d, pin));
  // (Synthetic cells can have several pins at one spot: any of them is fine.)
  CHECK(r.kind == PickResult::Kind::Pin && pin_location(d, r.id) == pin_location(d, pin));
  // Wire: a point on a segment of a net, with pins unselectable.
  layers[LayerSet::kPins].selectable = false;
  const Instance& w = det->wires.front();
  const double dx = d.die().xlo, dy = d.die().ylo;
  const Point mid{static_cast<Dbu>(dx + 0.5 * (w.x0 + w.x1)), static_cast<Dbu>(dy + 0.5 * (w.y0 + w.y1))};
  r = pick(ctx, mid);
  CHECK(r.kind == PickResult::Kind::Net);
  // Wires unselectable: click through to the cell/macro below or nothing.
  for (int l = LayerSet::kFirstMetal; l < layers.size(); ++l) layers[l].selectable = false;
  CHECK(pick(ctx, mid).kind != PickResult::Kind::Net);
}

void test_worker_latest_wins() {
  Design d = small(20000);
  SnapshotPublisher pub(d);
  std::atomic<int> ready{0};
  SceneWorker w([&] { ready.fetch_add(1); });
  for (int i = 0; i < 5; ++i) {
    d.move_cell(static_cast<Id>(i), {d.cell(static_cast<Id>(i)).loc.x + 200, d.cell(static_cast<Id>(i)).loc.y});
    d.flush();
    w.request_scene(pub.latest(), {});
  }
  for (int k = 0; k < 2000 && !(w.idle() && w.scene()); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  CHECK(w.scene() && w.scene()->version == pub.latest()->version());
  CHECK(ready.load() >= 1);
}

// The UI side never waits on a running flow: snapshot(), busy(),
// job_progress() and scene builds stay fast while placement runs.
void test_session_flow_nonblocking() {
  Session s;
  s.adopt(small(3000, SyntheticOptions::Placement::Unplaced), "test");
  CHECK(s.has_design());
  const std::uint64_t v0 = s.snapshot()->version();
  FlowOptions f;
  f.placer.max_iterations = 150;
  std::string why;
  CHECK(s.start_flow(f, &why));
  CHECK(s.busy());
  CHECK(!s.move_cell(0, {0, 0}, &why));
  CHECK(!s.start_qor(&why));
  double worst_ms = 0;
  std::uint64_t seen = v0;
  int versions = 0, frames = 0;
  while (s.busy()) {
    const double t = now_ms();
    auto snap = s.snapshot();
    (void)s.job_progress();
    worst_ms = std::max(worst_ms, now_ms() - t);
    if (snap->version() != seen) {
      seen = snap->version();
      ++versions;
      auto sc = build_scene(snap);  // as the worker would
      CHECK(sc->cells.size() + sc->macros.size() == snap->num_cells());
    }
    ++frames;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    s.poll();
  }
  s.poll();
  CHECK(worst_ms < 20.0);
  CHECK(versions >= 2);  // live updates during the run
  CHECK(frames > 3);
  CHECK(s.last_flow() != nullptr);
  CHECK(s.qor() != nullptr);
  // Edits now work and undo.
  const Point before = s.snapshot()->cell(5).loc;
  CHECK(s.move_cell(5, {before.x + 400, before.y}, &why));
  CHECK(s.snapshot()->cell(5).loc.x == before.x + 400);
  CHECK(s.undo(&why));
  CHECK(s.snapshot()->cell(5).loc == before);
}

void test_session_open_hdb() {
  const auto dir = std::filesystem::temp_directory_path() / "hermes_gui_test";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "t.hdb").string();
  {
    Design d = small(2000);
    io::save_design(d, path);
  }
  Session s;
  std::string why;
  CHECK(s.open(path, {}, &why));
  CHECK(!s.open(path, {}, &why));  // one job at a time
  s.wait();
  CHECK(s.has_design());
  CHECK(s.snapshot()->num_cells() == 2000 + 4);
  CHECK(s.generation() == 1);
  CHECK(s.open(path + ".missing", {}, &why));
  s.wait();
  CHECK(s.generation() == 1);  // failed load keeps the old design
  bool saw_error = false;
  for (const LogLine& l : s.log_lines())
    if (l.level == log::Level::Error && l.text.find("load failed") != std::string::npos) saw_error = true;
  CHECK(saw_error);
  std::filesystem::remove_all(dir);
}

struct StubView : ViewApi {
  std::map<std::string, int> layers;
  bool quit = false;
  void zoom_fit() override {}
  void zoom_to(const Rect&) override {}
  bool set_layer(std::string_view n, int v, int, std::string* why) override {
    if (n != "macros") {
      *why = "no layer";
      return false;
    }
    layers[std::string(n)] = v;
    return true;
  }
  std::string layer_list() const override { return "macros"; }
  bool select_cell(std::string_view, bool, std::string*) override { return true; }
  bool select_net(std::string_view, bool, std::string*) override { return true; }
  void clear_selection() override {}
  bool set_heatmap(std::string_view, std::string*) override { return true; }
  std::string heatmap_list() const override { return ""; }
  bool screenshot(const std::string&, std::string*) override { return true; }
  void request_quit() override { quit = true; }
  bool view_settled() const override { return true; }
  std::string stats() const override { return "stats"; }
};

void test_commands() {
  CHECK((split_command_line("open \"a b\" x=1") == std::vector<std::string>{"open", "a b", "x=1"}));
  Session s;
  s.adopt(small(2000, SyntheticOptions::Placement::Unplaced));
  StubView v;
  CommandContext ctx{s, v};
  CommandRegistry reg;
  CHECK(reg.run(ctx, "layer macros off").ok);
  CHECK(v.layers["macros"] == 0);
  CHECK(!reg.run(ctx, "layer nope off").ok);
  CHECK(!reg.run(ctx, "bogus").ok);
  CHECK(reg.run(ctx, "# comment").ok);
  CHECK(!reg.run(ctx, "run density=abc").ok);
  CHECK(reg.run(ctx, "move u7 1.0 2.0").ok);
  CHECK(s.snapshot()->cell(s.snapshot()->find_cell("u7")).loc == (Point{2000, 4000}));
  CHECK(reg.run(ctx, "undo").ok);
  CHECK(reg.run(ctx, "place iters=100").ok);
  CHECK(s.busy());
  const CommandResult r = reg.run(ctx, "move u7 1 2");
  CHECK(!r.ok && r.text.find("not available while") != std::string::npos);
  CHECK(reg.run(ctx, "layer macros on").ok);  // view commands still work
  CommandResult w = reg.run(ctx, "wait");
  CHECK(w.ok && w.wait_until);
  while (!w.wait_until()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  s.poll();
  CHECK(!s.busy());
  CHECK(reg.run(ctx, "help").text.find("screenshot") != std::string::npos);
  CHECK(reg.complete("sc").size() == 1);
  CHECK(reg.run(ctx, "quit").ok && v.quit);
}

}  // namespace

int main() {
  log::configure("warn");
  log::console_sink()->set_level(log::Level::Off);
  struct {
    const char* name;
    void (*fn)();
  } tests[] = {
      {"estimated_routes_match_route_usage", test_estimated_routes_match_route_usage},
      {"snapshot_has_masters_and_rows", test_snapshot_has_masters_and_rows},
      {"scene_holds_every_cell", test_scene_holds_every_cell},
      {"scene_identical_across_threads", test_scene_identical_across_threads},
      {"detail_window", test_detail_window},
      {"pick", test_pick},
      {"worker_latest_wins", test_worker_latest_wins},
      {"session_flow_nonblocking", test_session_flow_nonblocking},
      {"session_open_hdb", test_session_open_hdb},
      {"commands", test_commands},
  };
  for (const auto& t : tests) {
    const int before = g_failures;
    t.fn();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
  }
  std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "all passed", g_failures);
  return g_failures ? 1 : 0;
}
