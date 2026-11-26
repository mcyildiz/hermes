// GUI scene-model benchmark (no graphics): how fast the viewer can turn a
// design snapshot into drawable data, on a synthetic design.
//
//   hermes_gui_bench [num_cells=1000000]
//
// Rendering itself (GPU frame time, time to first frame) is measured by
// `hermes_gui --bench`, which needs a display.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gui/scene/scene.hpp"
#include "gui/scene/session.hpp"
#include "hermes/db/synthetic.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"

using namespace hermes;
using namespace hermes::gui;

namespace {
double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void row(const char* what, double ms, const char* extra = "") { std::printf("  %-44s %9.1f ms  %s\n", what, ms, extra); }
}  // namespace

int main(int argc, char** argv) {
  log::configure("warn");
  const int n = argc > 1 ? std::atoi(argv[1]) : 1000000;
  std::printf("hermes_gui_bench: %d cells, %d threads\n", n, par::num_threads());
  double t = now_ms();
  SyntheticOptions so;
  so.cells = n;
  Design d = make_synthetic_design(so);
  row("generate synthetic design", now_ms() - t);
  t = now_ms();
  SnapshotPublisher pub(d);
  auto snap = pub.latest();
  row("first snapshot", now_ms() - t);
  auto tech = make_tech_info(d);

  for (int threads : {1, par::hardware_threads()}) {
    par::ThreadLimit lim(threads);
    std::printf(" at %d thread(s):\n", threads);
    std::shared_ptr<Scene> sc;
    double best = 1e30, dens = 0;
    for (int rep = 0; rep < 3; ++rep) {
      t = now_ms();
      sc = build_scene(snap);
      const double ms = now_ms() - t;
      if (ms < best) {
        best = ms;
        dens = sc->density_ms;
      }
    }
    char buf[128];
    std::snprintf(buf, sizeof buf, "(density image %.1f ms, %dx%d tiles, %.1f MB)", dens, sc->tiles_x, sc->tiles_y,
                  sc->cells.size() * sizeof(Instance) / 1e6);
    row("build scene (cells, tiles, macros, density)", best, buf);
    SceneOptions po;
    po.pin_density = true;
    t = now_ms();
    auto sp = build_scene(snap, po);
    row("build scene + pin density", now_ms() - t);
    for (double frac : {0.01, 0.05, 0.2}) {
      const Rect die = d.die();
      const double s = std::sqrt(frac);
      DetailOptions o;
      o.window = {die.xlo + static_cast<Dbu>(die.width() * (0.5 - s / 2)),
                  die.ylo + static_cast<Dbu>(die.height() * (0.5 - s / 2)),
                  die.xlo + static_cast<Dbu>(die.width() * (0.5 + s / 2)),
                  die.ylo + static_cast<Dbu>(die.height() * (0.5 + s / 2))};
      t = now_ms();
      auto det = build_detail(*sc, *tech, o);
      std::snprintf(buf, sizeof buf, "(%zu pins, %zu nets, %zu wires, %zu vias%s)", det->pins.size(), det->nets,
                    det->wires.size(), det->vias.size(), det->truncated ? ", truncated" : "");
      char what[64];
      std::snprintf(what, sizeof what, "detail for a window of %.0f%% of the die", frac * 100);
      row(what, now_ms() - t, buf);
      if (frac == 0.05) {
        LayerSet layers;
        layers.set_metals(tech->routes.stack());
        PickContext ctx{sc.get(), det.get(), &layers, static_cast<double>(die.width()) / 1500.0, 4.0};
        t = now_ms();
        for (int k = 0; k < 10; ++k) (void)pick(ctx, die.center());
        row("pick (5% window detail, wires selectable)", (now_ms() - t) / 10);
      }
    }
    // A placer publish moves every cell: snapshot + rebuild per update.
    for (std::size_t i = 0; i < d.num_cells(); i += 1) {
      const Cell& c = d.cell(static_cast<Id>(i));
      if (!c.fixed) d.move_cell(static_cast<Id>(i), {c.loc.x + 200, c.loc.y});
    }
    t = now_ms();
    d.flush();
    auto s2 = pub.latest();
    const double pub_ms = now_ms() - t;
    t = now_ms();
    auto sc2 = build_scene(s2);
    std::snprintf(buf, sizeof buf, "(publish %.1f ms + scene %.1f ms)", pub_ms, now_ms() - t);
    row("live update after all cells moved", pub_ms + (now_ms() - t), buf);
  }
  return 0;
}
