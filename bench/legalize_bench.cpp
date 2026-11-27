// Legalizer quality and speed: full Abacus, and region-local / incremental
// use on an already legal placement.
//
//   ./build/hermes_legalize_bench <input> [--gp] [--save gp.hdb] [--trials N]
//   ./build/hermes_legalize_bench <input> --refine-ab
//
// <input> is anything hermes reads, or synth:N for a synthetic design of N
// std cells whose macros are movable (unplaced). With --gp, global placement runs first
// (legalization off) and --save keeps that result, so later runs can start
// from the .hdb directly. Without --gp the input's locations are the
// global-placement result.
//
// Reports, for the full run: HPWL before/after (%), displacement, time,
// legality. Then, from the legal result:
//   single   one random cell moved up to 20 rows away, legalized alone
//   batch    1% / 10% of the cells jittered, legalized as a cell list
//   fence    every cell of a 1/16-die region jittered, legalized in the fence
// with the time per call, HPWL change and a legality + "nothing else moved" check.
//
// --refine-ab: the flow's placement (with legalization) twice, with and
// without the mixed-size refinement pass, and the HPWL after each step.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "hermes/db/geometry.hpp"
#include "hermes/db/synthetic.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/io/design_file.hpp"
#include "hermes/io/registry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/place/legalizer.hpp"
#include "hermes/place/placer.hpp"

using namespace hermes;

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

bool is_std(const Cell& c) { return !c.is_macro && c.movable() && !c.removed; }

void fix_macros(Design& d) {
  for (Id i = 0; i < static_cast<Id>(d.num_cells()); ++i) {
    if (d.cell(i).is_macro && !d.cell(i).removed) d.set_fixed(i, true);
  }
}

std::vector<Id> std_cells(const Design& d) {
  std::vector<Id> v;
  for (Id i = 0; i < static_cast<Id>(d.num_cells()); ++i) {
    if (is_std(d.cell(i))) v.push_back(i);
  }
  return v;
}

/// True if only cells in `allowed` (a 0/1 mask) differ from `before`.
bool only_moved(const Design& d, const std::vector<Point>& before, const std::vector<std::uint8_t>& allowed) {
  for (Id i = 0; i < static_cast<Id>(d.num_cells()); ++i) {
    if (!allowed[i] && d.cell(i).loc != before[i]) return false;
  }
  return true;
}

std::vector<Point> locs(const Design& d) {
  std::vector<Point> v(d.num_cells());
  for (Id i = 0; i < static_cast<Id>(d.num_cells()); ++i) v[i] = d.cell(i).loc;
  return v;
}

const char* yn(bool b) { return b ? "yes" : "NO"; }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: hermes_legalize_bench <input> [--gp] [--save gp.hdb] [--trials N]\n");
    return 2;
  }
  std::string input = argv[1], save;
  bool gp = false, refine_ab = false;
  int trials = 1000;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--gp") gp = true;
    else if (a == "--refine-ab") refine_ab = true;
    else if (a == "--save" && i + 1 < argc) save = argv[++i];
    else if (a == "--trials" && i + 1 < argc) trials = std::atoi(argv[++i]);
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  log::configure("warn");
  Design d;
  if (input.rfind("synth:", 0) == 0) {
    SyntheticOptions so;
    so.cells = std::atoi(input.c_str() + 6);
    so.placement = SyntheticOptions::Placement::Unplaced;
    d = make_synthetic_design(so);
    for (Id i = 0; i < static_cast<Id>(d.num_cells()); ++i) {
      if (d.cell(i).is_macro) {
        d.set_fixed(i, false);
        d.set_placed(i, false);
      }
    }
  } else {
    d = io::load_input(input);
  }
  if (refine_ab) {
    for (int refine : {0, 1}) {
      Design x = d;
      FlowOptions fo;
      fo.placer.detailed_placement = false;
      if (!refine) fo.placer.refine_iterations = 0;
      fo.run_routing = false;
      fo.report_qor = false;
      const auto t = Clock::now();
      const FlowReport r = run_flow(x, fo);
      const PlacementResult& p = r.placement;
      std::printf("refinement %-3s: placement %.0f ms (legalization %.0f ms, %d refine iterations,"
                  " %.2f%% std area on macros); HPWL GP %.6g -> final %.6g (%+.2f%%); legal %s\n",
                  refine ? "on" : "off", ms_since(t), p.legalize_ms, p.refine_iterations,
                  100 * p.macro_overlap, p.global_hpwl, p.hpwl, 100 * (p.hpwl / p.global_hpwl - 1),
                  yn(check_legal(x, legalizer_row_height(x))));
    }
    return 0;
  }
  if (gp) {
    // The flow's placer setup (timing + routability feedback), unlegalized.
    FlowOptions fo;
    fo.placer.legalize = false;
    fo.placer.detailed_placement = false;
    fo.run_routing = false;
    fo.report_qor = false;
    const auto t = Clock::now();
    run_flow(d, fo);
    std::printf("global placement: %.0f ms, HPWL %.6g\n", ms_since(t), total_hpwl(d));
    if (!save.empty()) io::save_design(d, save);
  }
  const Dbu rh = legalizer_row_height(d);
  const double hpwl0 = total_hpwl(d);
  std::printf("%s: %zu cells, %zu rows, row height %lld, HPWL %.6g\n", d.name().c_str(), d.num_cells(),
              d.rows().size(), static_cast<long long>(rh), hpwl0);

  // --- Full legalization ------------------------------------------------------
  Design legal_d;
  for (double ww : {0.0, 1.0}) {
    Design x = d;
    auto t = Clock::now();
    const MacroLegalizeResult mr = legalize_macros(x);
    fix_macros(x);
    const double t_macro = ms_since(t);
    CellLegalizerOptions lo;
    lo.width_weight = ww;
    t = Clock::now();
    const CellLegalizeResult r = abacus_legalize(x, lo);
    const double t_cells = ms_since(t);
    std::string why;
    const bool ok = check_legal(x, rh, &why);
    const double h = total_hpwl(x);
    std::printf("full (width_weight %.0f): macros %d moved in %.1f ms; cells %d legal, %d failed in %.1f ms;"
                " avg displacement %.1f, max %lld; HPWL %.6g (%+.2f%%); legal %s %s\n",
                ww, mr.moved, t_macro, r.legalized, r.failed, t_cells,
                r.total_displacement / std::max(1, r.legalized + r.failed),
                static_cast<long long>(r.max_displacement), h, 100.0 * (h / hpwl0 - 1.0), yn(ok),
                ok ? "" : why.c_str());
    if (ww == 0.0) legal_d = std::move(x);
  }

  // --- Incremental use on the legal placement ----------------------------------
  std::mt19937 rng(5);
  const Rect die = d.die();
  const std::vector<Id> cells = std_cells(legal_d);
  if (cells.empty()) return 0;
  {
    Design x = legal_d;
    std::uniform_int_distribution<std::size_t> pick(0, cells.size() - 1);
    std::uniform_int_distribution<Dbu> off(-20 * rh, 20 * rh);
    double total = 0, worst = 0;
    int failed = 0;
    const double h_before = total_hpwl(x);
    std::vector<Point> before = locs(x);
    std::vector<std::uint8_t> moved(x.num_cells(), 0);
    for (int k = 0; k < trials; ++k) {
      const Id c = cells[pick(rng)];
      const Cell& cc = x.cell(c);
      x.move_cell(c, {std::clamp(cc.loc.x + off(rng), die.xlo, die.xhi - cc.width),
                      std::clamp(cc.loc.y + off(rng), die.ylo, die.yhi - cc.height)});
      moved[c] = 1;
      CellLegalizerOptions lo;
      const Id one[] = {c};
      lo.cells = one;
      const auto t = Clock::now();
      const CellLegalizeResult r = abacus_legalize(x, lo);
      const double ms = ms_since(t);
      total += ms;
      worst = std::max(worst, ms);
      failed += r.failed;
    }
    const bool ok = check_legal(x, rh);
    std::printf("single: %d one-cell moves, %.3f ms avg, %.3f ms max, %d failed; legal %s; others untouched %s;"
                " HPWL %+.3f%%\n",
                trials, total / trials, worst, failed, yn(ok), yn(only_moved(x, before, moved)),
                100.0 * (total_hpwl(x) / h_before - 1.0));
  }
  for (double frac : {0.01, 0.10}) {
    Design x = legal_d;
    std::vector<Id> sub;
    std::vector<std::uint8_t> moved(x.num_cells(), 0);
    std::bernoulli_distribution take(frac);
    std::uniform_int_distribution<Dbu> off(-3 * rh, 3 * rh);
    for (Id c : cells) {
      if (!take(rng)) continue;
      const Cell& cc = x.cell(c);
      x.move_cell(c, {std::clamp(cc.loc.x + off(rng), die.xlo, die.xhi - cc.width),
                      std::clamp(cc.loc.y + off(rng), die.ylo, die.yhi - cc.height)});
      sub.push_back(c);
      moved[c] = 1;
    }
    const std::vector<Point> before = locs(x);
    const double h_before = total_hpwl(legal_d);
    CellLegalizerOptions lo;
    lo.cells = sub;
    const auto t = Clock::now();
    const CellLegalizeResult r = abacus_legalize(x, lo);
    const double ms = ms_since(t);
    const bool ok = check_legal(x, rh);
    std::printf("batch %2.0f%%: %zu cells in %.1f ms (%d windows), %d failed; legal %s; others untouched %s;"
                " HPWL vs. legal start %+.3f%%\n",
                100 * frac, sub.size(), ms, r.attempts, r.failed, yn(ok), yn(only_moved(x, before, moved)),
                100.0 * (total_hpwl(x) / h_before - 1.0));
  }
  if (die.height() < 40 * rh) {
    std::printf("fence: skipped (design has fewer than 40 rows)\n");
  } else {
    Design x = legal_d;
    // The middle 1/16 of the die, at least 5 rows tall (whole rows are used).
    const Dbu fh = std::max<Dbu>(die.height() / 4, std::min<Dbu>(5 * rh, die.height()));
    const Rect f{die.xlo + die.width() * 3 / 8, die.ylo + (die.height() - fh) / 2,
                 die.xlo + die.width() * 5 / 8, die.ylo + (die.height() + fh) / 2};
    std::vector<std::uint8_t> moved(x.num_cells(), 0);
    std::uniform_int_distribution<Dbu> off(-3 * rh, 3 * rh);
    std::size_t n = 0;
    for (Id c : cells) {
      const Cell& cc = x.cell(c);
      if (!f.contains(cc.bbox().center())) continue;
      const Point p{std::clamp(cc.loc.x + off(rng), f.xlo, f.xhi - cc.width),
                    std::clamp(cc.loc.y + off(rng), f.ylo, f.yhi - cc.height)};
      if (!f.contains(Rect{p.x, p.y, p.x + cc.width, p.y + cc.height}.center())) continue;
      x.move_cell(c, p);
      moved[c] = 1;
      ++n;
    }
    const std::vector<Point> before = locs(x);
    CellLegalizerOptions lo;
    lo.fence = f;
    const auto t = Clock::now();
    const CellLegalizeResult r = abacus_legalize(x, lo);
    const double ms = ms_since(t);
    // Cells that were inside the fence may all move; nothing else.
    std::vector<std::uint8_t> inside(x.num_cells(), 0);
    for (Id c : cells) inside[c] = f.contains(Rect{before[c].x, before[c].y, before[c].x + x.cell(c).width,
                                                   before[c].y + x.cell(c).height}.center());
    const bool ok = check_legal(x, rh);
    std::printf("fence (middle 1/16): %d cells (%zu jittered) in %.1f ms, %d failed; legal %s; outside untouched %s;"
                " HPWL vs. legal start %+.3f%%\n",
                r.legalized + r.failed, n, ms, r.failed, yn(ok), yn(only_moved(x, before, inside)),
                100.0 * (total_hpwl(x) / total_hpwl(legal_d) - 1.0));
  }
  return 0;
}
