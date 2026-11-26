// Tests for the LEF/DEF readers, using tests/data/sample.{lef,def} plus a few
// inline snippets. Same minimal runner style as test_core.cpp.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "hermes/log/sinks.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/io/lefdef.hpp"

using namespace hermes;
using namespace hermes::io;

#ifndef HERMES_TEST_DATA_DIR
#error "HERMES_TEST_DATA_DIR must be defined"
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

const std::string kData = HERMES_TEST_DATA_DIR;

Id find_cell(const Design& d, const std::string& name) { return d.find_cell(name); }

Id find_net(const Design& d, const std::string& name) { return d.find_net(name); }

/// Pin `pin` of cell `cell` (or top-level pin if cell is empty).
const Pin* find_pin(const Design& d, const std::string& cell, const std::string& pin) {
  const Id cid = cell.empty() ? kInvalidId : find_cell(d, cell);
  if (!cell.empty() && cid == kInvalidId) return nullptr;
  const Id pid = d.find_pin(cid, pin);
  return pid == kInvalidId ? nullptr : &d.pin(pid);
}

LefLibrary sample_lib() {
  LefLibrary lib;
  read_lef(kData + "/sample.lef", lib);
  return lib;
}

void test_lef_library() {
  const LefLibrary lib = sample_lib();
  CHECK(lib.database_microns == 1000.0);
  CHECK(lib.num_macros() == 3);
  CHECK(lib.num_sites() == 1);
  const LefSite* site = lib.find_site("core");
  CHECK(site && site->width == 0.2 && site->height == 2.0);

  const LefMacro* inv = lib.find_macro("INV");
  CHECK(inv && !inv->is_block());
  CHECK(inv && inv->width == 0.6 && inv->height == 2.0);
  CHECK(inv && inv->pins().size() == 3);
  const LefPin* z = inv ? inv->find_pin("Z") : nullptr;
  CHECK(z && z->dir == PinDir::Output && z->use == PinUse::Signal);
  CHECK(z && std::abs(z->x - 0.45) < 1e-9 && std::abs(z->y - 1.0) < 1e-9);
  const LefPin* vdd = inv ? inv->find_pin("VDD") : nullptr;
  CHECK(vdd && vdd->use == PinUse::Power && vdd->dir == PinDir::Inout);

  const LefMacro* nand = lib.find_macro("NAND2");
  const LefPin* nz = nand ? nand->find_pin("Z") : nullptr;  // POLYGON + TRISTATE
  CHECK(nz && nz->dir == PinDir::Output && std::abs(nz->x - 0.65) < 1e-9);

  const LefMacro* ram = lib.find_macro("RAM");
  CHECK(ram && ram->is_block());
  const LefPin* d = ram ? ram->find_pin("D") : nullptr;  // ORIGIN 1 2 applied
  CHECK(d && std::abs(d->x - 0.1) < 1e-9 && std::abs(d->y - 4.2) < 1e-9);
}

void test_def_design() {
  const LefLibrary lib = sample_lib();
  DefReadReport rep;
  const Design d = read_def(kData + "/sample.def", lib, &rep);

  CHECK(d.name() == "sample");
  CHECK(rep.dbu_per_micron == 2000.0);
  CHECK(d.die() == (Rect{0, 0, 200000, 160000}));
  CHECK(d.row_height() == 4000);
  CHECK(rep.site_width == 400);
  CHECK(rep.num_rows == 3);

  CHECK(d.cells().size() == 5);
  CHECK(rep.num_components == 5 && rep.num_macros == 1 && rep.num_fixed == 2 &&
        rep.num_unplaced == 1 && rep.num_io_pins == 2);
  CHECK(count_macros(d) == 1);
  CHECK(d.nets().size() == 4 && rep.num_nets == 4);
  CHECK(d.pins().size() == 11 && rep.num_pins == 11);
  CHECK(rep.num_warnings == 0);

  // N placement: LEF microns -> DEF DBU (x2000).
  const Cell& u1 = d.cell(find_cell(d, "u1"));
  CHECK(u1.width == 1200 && u1.height == 4000 && u1.loc == (Point{1000, 4000}));
  CHECK(!u1.fixed && !u1.is_macro);
  const Pin* u1a = find_pin(d, "u1", "A");
  CHECK(u1a && u1a->offset == (Point{300, 2000}) && u1a->dir == PinDir::Input);
  CHECK(u1a && pin_location(d, static_cast<Id>(u1a - d.pins().data())) == (Point{1300, 6000}));

  // FS: y mirrored.
  const Cell& u2 = d.cell(find_cell(d, "u2"));
  CHECK(u2.orient == Orient::FS);
  const Pin* u2z = find_pin(d, "u2", "Z");
  CHECK(u2z && u2z->offset == (Point{900, 2000}) && u2z->dir == PinDir::Output);

  // E: width/height swap, fixed.
  const Cell& u4 = d.cell(find_cell(d, "u4"));
  CHECK(u4.fixed && u4.orient == Orient::E && u4.width == 4000 && u4.height == 1200);
  const Pin* u4a = find_pin(d, "u4", "A");
  CHECK(u4a && u4a->offset == (Point{2000, 900}));

  // Macro with ORIGIN, rotated W, fixed.
  const Cell& ram = d.cell(find_cell(d, "ram0"));
  CHECK(ram.is_macro && ram.fixed && ram.width == 20000 && ram.height == 40000);
  const Pin* rd = find_pin(d, "ram0", "D");
  CHECK(rd && rd->offset == (Point{11600, 200}));
  const Pin* rq = find_pin(d, "ram0", "Q");
  CHECK(rq && rq->offset == (Point{3000, 39800}));

  // Unplaced component.
  const Cell& u3 = d.cell(find_cell(d, "u3"));
  CHECK(u3.loc == (Point{0, 0}) && !u3.fixed && u3.width == 1600);

  // Top-level pins: absolute location, net-side direction.
  const Pin* in1 = find_pin(d, "", "in1");
  CHECK(in1 && in1->offset == (Point{200, 80000}) && in1->dir == PinDir::Output);
  CHECK(in1 && d.net_name(in1->net) == "n_in");
  const Pin* out1 = find_pin(d, "", "out1");
  CHECK(out1 && out1->offset == (Point{199800, 80000}) && out1->dir == PinDir::Input);

  // Net membership; routed wiring did not leak in as connections.
  const Id n1 = find_net(d, "n1");
  CHECK(n1 != kInvalidId && d.net_pins(n1).size() == 3);
  const Id nout = find_net(d, "n_out");
  CHECK(nout != kInvalidId && d.net_pins(nout).size() == 3);
}

void test_orient_offset_roundtrip() {
  // The bounding box of a transformed rectangle must stay inside the
  // transformed cell for every orientation.
  const Dbu w = 10, h = 4;
  const Point p{1, 3};
  for (Orient o : {Orient::N, Orient::S, Orient::E, Orient::W, Orient::FN, Orient::FS,
                   Orient::FE, Orient::FW}) {
    const bool rot = o == Orient::E || o == Orient::W || o == Orient::FE || o == Orient::FW;
    const Dbu W = rot ? h : w, H = rot ? w : h;
    for (Point c : {Point{0, 0}, Point{w, h}, p}) {
      const Point q = orient_offset(c, w, h, o);
      CHECK(q.x >= 0 && q.x <= W && q.y >= 0 && q.y <= H);
    }
    // Corners map to corners.
    const Point a = orient_offset({0, 0}, w, h, o);
    const Point b = orient_offset({w, h}, w, h, o);
    CHECK(std::abs(a.x - b.x) == W && std::abs(a.y - b.y) == H);
  }
  CHECK(orient_offset(p, w, h, Orient::S) == (Point{9, 1}));
  CHECK(orient_offset(p, w, h, Orient::FN) == (Point{9, 3}));
  CHECK(orient_offset(p, w, h, Orient::W) == (Point{1, 1}));
  CHECK(orient_offset(p, w, h, Orient::E) == (Point{3, 9}));
}

void test_errors() {
  const LefLibrary lib = sample_lib();
  bool threw = false;
  try {
    read_def_string(
        "DESIGN t ;\nUNITS DISTANCE MICRONS 1000 ;\nCOMPONENTS 1 ;\n"
        "- x NOSUCH + PLACED ( 0 0 ) N ;\nEND COMPONENTS\nEND DESIGN\n",
        lib, nullptr, {}, "bad.def");
  } catch (const ParseError& e) {
    threw = std::string(e.what()).find("bad.def:4:") == 0;
  }
  CHECK(threw);

  // Same input is accepted with allow_unknown_macros, with a warning.
  DefReadOptions opts;
  opts.allow_unknown_macros = true;
  DefReadReport rep;
  const Design d = read_def_string(
      "DESIGN t ;\nUNITS DISTANCE MICRONS 1000 ;\nDIEAREA ( 0 0 ) ( 10 10 ) ;\n"
      "COMPONENTS 1 ;\n- x NOSUCH + PLACED ( 0 0 ) N ;\nEND COMPONENTS\nEND DESIGN\n",
      lib, &rep, opts);
  CHECK(d.cells().size() == 1 && rep.num_warnings == 1);

  // Duplicate component names are rejected (names are unique keys).
  threw = false;
  try {
    read_def_string(
        "DESIGN t ;\nUNITS DISTANCE MICRONS 1000 ;\nCOMPONENTS 2 ;\n"
        "- x NOSUCH ;\n- x NOSUCH ;\nEND COMPONENTS\nEND DESIGN\n",
        lib, nullptr, opts, "dup.def");
  } catch (const ParseError& e) {
    const std::string msg = e.what();
    threw = msg.find("dup.def:5:") == 0 && msg.find("duplicate component 'x'") != std::string::npos;
  }
  CHECK(threw);

  threw = false;
  try {
    LefLibrary l2;
    read_lef_string("MACRO X\n SIZE 1 BY 2 ;\n", l2, "trunc.lef");
  } catch (const ParseError& e) {
    threw = std::string(e.what()).find("trunc.lef:") == 0;
  }
  CHECK(threw);
}

void test_flow_on_sample() {
  const LefLibrary lib = sample_lib();
  Design d = read_def(kData + "/sample.def", lib);
  FlowOptions opts;
  opts.placer.max_iterations = 20;
  opts.router.grid_x = opts.router.grid_y = 8;
  const FlowReport r = run_flow(d, opts);
  CHECK(r.placement.iterations > 0 && r.placement.iterations <= 20);  // May stop early.
  CHECK(r.global_route.wirelength > 0);
  CHECK(d.cell(find_cell(d, "ram0")).loc == (Point{40000, 40000}));  // Fixed.
}

}  // namespace

int main() {
  // Engine progress goes to the log; keep test output to pass/fail lines.
  hermes::log::console_sink()->set_level(hermes::log::Level::Error);
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"lef_library", test_lef_library},
      {"def_design", test_def_design},
      {"orient_offset", test_orient_offset_roundtrip},
      {"errors", test_errors},
      {"flow_on_sample", test_flow_on_sample},
  };
  for (const auto& [name, fn] : tests) {
    const int before = g_failures;
    try {
      fn();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "  exception: %s\n", e.what());
      ++g_failures;
    }
    std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name.c_str());
  }
  std::printf("%zu tests, %d failures\n", tests.size(), g_failures);
  return g_failures == 0 ? 0 : 1;
}
