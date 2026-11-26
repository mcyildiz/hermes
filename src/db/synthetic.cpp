#include "hermes/db/synthetic.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace hermes {

namespace {
constexpr Dbu kSite = 200, kRowH = 2000;
constexpr int kWidths = 4;
constexpr Dbu kMacro = 40000;
const char* kPin[] = {"Z", "A", "B", "C", "D", "E"};

Point pin_offset(bool macro, Dbu w, int p) {
  if (macro) return {100, static_cast<Dbu>((2.05 + 3 * p) * 2000)};
  const double x = static_cast<double>(w) * (p + 0.5) / 6.0;
  return {static_cast<Dbu>(std::llround(x)), 1000};
}
}  // namespace

Design make_synthetic_design(const SyntheticOptions& o) {
  const int n = std::max(1, o.cells);
  std::mt19937 rng(o.seed);
  std::uniform_int_distribution<int> wk(1, kWidths);
  std::vector<int> k(static_cast<std::size_t>(n));
  double area = 0;
  for (int& x : k) {
    x = wk(rng);
    area += static_cast<double>(2 * x * kSite) * kRowH;
  }
  const int macros = std::max(4, n / 20000);
  area += macros * static_cast<double>(kMacro) * kMacro;
  const Dbu side =
      static_cast<Dbu>(std::sqrt(area / std::clamp(o.utilization, 0.05, 0.95)) / kRowH + 1) * kRowH;
  Design d("synth" + std::to_string(n));
  d.set_die({0, 0, side, side});
  d.set_dbu_per_micron(2000);
  std::vector<Row> rows;
  const Id site = d.intern_site_name("core");
  for (Dbu y = 0; y + kRowH <= side; y += kRowH)
    rows.push_back({{0, y}, kSite, kRowH, kSite, static_cast<std::int32_t>(side / kSite), Orient::N, site});
  d.set_rows(std::move(rows));
  d.reserve(static_cast<std::size_t>(n + macros), static_cast<std::size_t>(n + 1000), 4 * static_cast<std::size_t>(n));

  // Macros: random (Random placement, as in io_bench) or on a regular grid.
  std::vector<Rect> mboxes;
  std::uniform_int_distribution<Dbu> mp(0, side - kMacro);
  const int mside = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(macros))));
  for (int m = 0; m < macros; ++m) {
    Point at;
    if (o.placement == SyntheticOptions::Placement::Random) {
      at = {mp(rng) / kSite * kSite, mp(rng) / kRowH * kRowH};
    } else {
      const Dbu pitch = side / (mside + 1);
      at = {((m % mside) + 1) * pitch - kMacro / 2, ((m / mside) + 1) * pitch - kMacro / 2};
      at = {at.x / kSite * kSite, at.y / kRowH * kRowH};
    }
    mboxes.push_back({at.x, at.y, at.x + kMacro, at.y + kMacro});
  }

  std::uniform_int_distribution<Dbu> px(0, side / kSite - 10), py(0, side / kRowH - 1);
  // Ordered: per-row free length, spread the slack evenly between cells.
  const double macro_area = macros * static_cast<double>(kMacro) * kMacro;
  const double free_area = static_cast<double>(side) * static_cast<double>(side) - macro_area;
  const double spread = std::max(1.0, 0.97 * free_area / std::max(1.0, area - macro_area));
  Dbu cx = 0, cy = 0;
  bool left_to_right = true;
  double carry = 0.0;
  auto blocked = [&](Dbu x0, Dbu x1, Dbu y) -> const Rect* {
    for (const Rect& r : mboxes)
      if (y < r.yhi && y + kRowH > r.ylo && x0 < r.xhi && x1 > r.xlo) return &r;
    return nullptr;
  };
  for (int i = 0; i < n; ++i) {
    const Dbu w = 2 * k[static_cast<std::size_t>(i)] * kSite;
    const Id c = d.add_cell("u" + std::to_string(i), w, kRowH);
    d.set_cell_master(c, "CELLW" + std::to_string(k[static_cast<std::size_t>(i)]));
    switch (o.placement) {
      case SyntheticOptions::Placement::Random:
        d.move_cell(c, {px(rng) * kSite, py(rng) * kRowH});
        d.set_placed(c, true);
        break;
      case SyntheticOptions::Placement::Unplaced:
        d.move_cell(c, {side / 2, side / 2});
        break;
      case SyntheticOptions::Placement::Ordered: {
        // Snake through the rows; skip macro footprints.
        for (int guard = 0; guard < 100000; ++guard) {
          if (cy + kRowH > side) cy = 0;  // overfull: wrap (only with utilization near 1)
          Dbu x0 = left_to_right ? cx : side - cx - w;
          if (cx + w > side) {
            cx = 0;
            cy += kRowH;
            left_to_right = !left_to_right;
            continue;
          }
          if (const Rect* r = blocked(x0, x0 + w, cy)) {
            cx = left_to_right ? r->xhi : side - r->xlo;
            cx = (cx + kSite - 1) / kSite * kSite;
            continue;
          }
          d.move_cell(c, {x0, cy});
          carry += static_cast<double>(w) * (spread - 1.0);
          const Dbu gap = static_cast<Dbu>(carry / kSite) * kSite;
          carry -= static_cast<double>(gap);
          cx += w + gap;
          break;
        }
        d.set_placed(c, true);
        break;
      }
    }
  }
  for (int m = 0; m < macros; ++m) {
    const Id c = d.add_cell("ram" + std::to_string(m), kMacro, kMacro, true);
    d.set_cell_master(c, "RAM");
    d.move_cell(c, {mboxes[static_cast<std::size_t>(m)].xlo, mboxes[static_cast<std::size_t>(m)].ylo});
    d.set_fixed(c, true);
    d.set_placed(c, true);
  }
  std::uniform_int_distribution<int> deg(2, 6), near(-200, 200);
  for (int i = 0; i < n; ++i) {
    const Id net = d.add_net("n" + std::to_string(i));
    const int kd = deg(rng);
    for (int p = 0; p < kd; ++p) {
      int c = p == 0 ? i : std::clamp(i + near(rng), 0, n - 1);
      if (p == kd - 1 && i % 500 == 0) c = n + (i / 500) % macros;  // some nets reach macros
      const Cell& cc = d.cell(c);
      d.connect(c, net, kPin[p], pin_offset(cc.is_macro, cc.width, p), p == 0 ? PinDir::Output : PinDir::Input);
    }
  }
  const int nio = std::min(1000, n);
  for (int io = 0; io < nio; ++io) {
    const Id net = d.find_net("n" + std::to_string(io * (n / nio)));
    // Around the die edge.
    const Dbu t = side * 4 / nio * io;
    Point at = t < side ? Point{t, 0} : t < 2 * side ? Point{side, t - side} : t < 3 * side ? Point{3 * side - t, side} : Point{0, 4 * side - t};
    d.connect(kInvalidId, net, "io" + std::to_string(io), at, PinDir::Output);
  }
  d.compact();
  return d;
}

}  // namespace hermes
