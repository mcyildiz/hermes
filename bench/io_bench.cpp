// Reader / writer / design-file benchmark on a synthetic design.
//
//   hermes_io_bench [num_cells=1000000] [work_dir=io_bench_out]
//
// Builds a mixed-size design (std cells on rows, fixed macros, I/O pins),
// writes it as LEF+DEF, Bookshelf and .hdb, then times reading each back and
// checks the round trips.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

#include "hermes/db/geometry.hpp"
#include "hermes/io/bookshelf.hpp"
#include "hermes/io/design_file.hpp"
#include "hermes/io/lefdef.hpp"
#include "hermes/io/registry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/log/sinks.hpp"
#include "hermes/par/parallel.hpp"

using namespace hermes;
namespace fs = std::filesystem;

namespace {

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr Dbu kSite = 200, kRowH = 2000;  // 0.1 x 1.0 um at 2000 dbu/um
constexpr int kWidths = 4;                // masters CELLW1..CELLW4 (1..4 x 2 sites)
const char* kPin[] = {"Z", "A", "B", "C", "D", "E"};

std::string make_lef() {
  std::string s = "VERSION 5.8 ;\nUNITS\n  DATABASE MICRONS 2000 ;\nEND UNITS\n\n"
                  "LAYER M1\n  TYPE ROUTING ;\nEND M1\n\n"
                  "SITE core\n  CLASS CORE ;\n  SIZE 0.1 BY 1.0 ;\nEND core\n\n";
  for (int k = 1; k <= kWidths; ++k) {
    const double w = 0.2 * k;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "MACRO CELLW%d\n  CLASS CORE ;\n  ORIGIN 0 0 ;\n  SIZE %.1f BY 1.0 ;\n  SITE core ;\n", k, w);
    s += buf;
    for (int p = 0; p < 6; ++p) {
      const double x = w * (p + 0.5) / 6.0;
      std::snprintf(buf, sizeof(buf),
                    "  PIN %s\n    DIRECTION %s ;\n    PORT\n      LAYER M1 ;\n        RECT %.4f 0.4 %.4f 0.6 ;\n    END\n  END %s\n",
                    kPin[p], p == 0 ? "OUTPUT" : "INPUT", x - 0.005, x + 0.005, kPin[p]);
      s += buf;
    }
    std::snprintf(buf, sizeof(buf), "END CELLW%d\n\n", k);
    s += buf;
  }
  s += "MACRO RAM\n  CLASS BLOCK ;\n  ORIGIN 0 0 ;\n  SIZE 20 BY 20 ;\n";
  for (int p = 0; p < 6; ++p) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "  PIN %s\n    DIRECTION %s ;\n    PORT\n      LAYER M1 ;\n        RECT 0 %.1f 0.1 %.1f ;\n    END\n  END %s\n",
                  kPin[p], p == 0 ? "OUTPUT" : "INPUT", 2.0 + 3 * p, 2.1 + 3 * p, kPin[p]);
    s += buf;
  }
  s += "END RAM\n\nEND LIBRARY\n";
  return s;
}

/// Pin offset of master pin p (lower-left frame, orientation N).
Point pin_offset(bool macro, Dbu w, int p) {
  if (macro) return {100, static_cast<Dbu>((2.05 + 3 * p) * 2000)};
  const double x = static_cast<double>(w) * (p + 0.5) / 6.0;
  return {static_cast<Dbu>(std::llround(x)), 1000};
}

Design make_design(int n) {
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> wk(1, kWidths);
  std::vector<int> k(n);
  double area = 0;
  for (int& x : k) {
    x = wk(rng);
    area += static_cast<double>(2 * x * kSite) * kRowH;
  }
  const int macros = std::max(4, n / 20000);
  area += macros * 40000.0 * 40000.0;
  const Dbu side = static_cast<Dbu>(std::sqrt(area / 0.7) / kRowH + 1) * kRowH;
  Design d("synth");
  d.set_die({0, 0, side, side});
  d.set_dbu_per_micron(2000);
  std::vector<Row> rows;
  const Id site = d.intern_site_name("core");
  for (Dbu y = 0; y + kRowH <= side; y += kRowH) {
    rows.push_back({{0, y}, kSite, kRowH, kSite, static_cast<std::int32_t>(side / kSite), Orient::N, site});
  }
  d.set_rows(std::move(rows));
  d.reserve(n + macros, n + 1000, 4 * static_cast<std::size_t>(n));
  std::uniform_int_distribution<Dbu> px(0, side / kSite - 10), py(0, side / kRowH - 1);
  for (int i = 0; i < n; ++i) {
    const Id c = d.add_cell("u" + std::to_string(i), 2 * k[i] * kSite, kRowH);
    d.set_cell_master(c, "CELLW" + std::to_string(k[i]));
    d.move_cell(c, {px(rng) * kSite, py(rng) * kRowH});
    d.set_placed(c, true);
  }
  std::uniform_int_distribution<Dbu> mp(0, side - 40000);
  for (int m = 0; m < macros; ++m) {
    const Id c = d.add_cell("ram" + std::to_string(m), 40000, 40000, true);
    d.set_cell_master(c, "RAM");
    d.move_cell(c, {mp(rng) / kSite * kSite, mp(rng) / kRowH * kRowH});
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
  for (int io = 0; io < 1000 && io < n; ++io) {
    const Id net = d.find_net("n" + std::to_string(io * (n / 1000)));
    d.connect(kInvalidId, net, "io" + std::to_string(io), {0, (side / 1000) * io}, PinDir::Output);
  }
  d.compact();
  return d;
}

bool same_geometry(const Design& a, const Design& b, std::string* why, bool hpwl = true) {
  auto fail = [&](std::string w) {
    if (why) *why = std::move(w);
    return false;
  };
  if (a.num_cells() != b.num_cells()) return fail("cell count");
  if (a.num_nets() != b.num_nets()) return fail("net count");
  for (Id c = 0; c < static_cast<Id>(a.num_cells()); ++c) {
    const Id cb = b.find_cell(a.cell_name(c));
    if (cb == kInvalidId) return fail("missing cell " + std::string(a.cell_name(c)));
    const Cell &x = a.cell(c), &y = b.cell(cb);
    if (x.loc != y.loc || x.width != y.width || x.height != y.height || x.fixed != y.fixed) {
      return fail("cell differs: " + std::string(a.cell_name(c)));
    }
  }
  if (!hpwl) return true;
  const double ha = total_hpwl(a), hb = total_hpwl(b);
  if (std::abs(ha - hb) > 1e-9 * std::max(1.0, ha)) return fail("HPWL " + std::to_string(ha) + " vs " + std::to_string(hb));
  return true;
}

void row(const char* what, double s, double mb = 0) {
  if (mb > 0) {
    std::printf("  %-40s %9.3f s  %8.1f MB  %7.0f MB/s\n", what, s, mb, mb / s);
  } else {
    std::printf("  %-40s %9.3f s\n", what, s);
  }
}

void check(bool ok, const std::string& why) {
  std::printf("    round trip: %s %s\n", ok ? "ok" : "MISMATCH", ok ? "" : why.c_str());
}

double file_mb(const fs::path& p) { return static_cast<double>(fs::file_size(p)) / 1e6; }

double dir_mb(const fs::path& d, const char* stem) {
  double t = 0;
  for (const auto& e : fs::directory_iterator(d)) {
    if (e.path().stem() == stem) t += static_cast<double>(e.file_size()) / 1e6;
  }
  return t;
}

}  // namespace

int main(int argc, char** argv) {
  const int n = argc > 1 ? std::atoi(argv[1]) : 1000000;
  const fs::path dir = argc > 2 ? argv[2] : "io_bench_out";
  log::console_sink()->set_level(log::Level::Warn);
  fs::create_directories(dir / "lefdef");
  fs::create_directories(dir / "bookshelf");
  std::printf("io bench: %d cells, %d threads\n", n, par::num_threads());
  double t = now_s();
  Design d = make_design(n);
  row("build synthetic design", now_s() - t);
  std::printf("  cells %zu, nets %zu, pins %zu, rows %zu, HPWL %.6g\n", d.num_cells(), d.num_nets(), d.num_pins(),
              d.rows().size(), total_hpwl(d));
  {
    std::FILE* f = std::fopen((dir / "lefdef" / "synth.lef").string().c_str(), "wb");
    const std::string lef = make_lef();
    std::fwrite(lef.data(), 1, lef.size(), f);
    std::fclose(f);
  }

  std::puts("write:");
  const fs::path def = dir / "lefdef" / "synth.def";
  t = now_s();
  io::write_def(d, def.string());
  row("DEF", now_s() - t, file_mb(def));
  t = now_s();
  const std::string aux = io::write_bookshelf(d, (dir / "bookshelf").string(), "synth");
  row("Bookshelf (aux nodes nets pl scl wts)", now_s() - t, dir_mb(dir / "bookshelf", "synth"));
  const fs::path pl = dir / "synth_only.pl";
  t = now_s();
  io::write_bookshelf_pl(d, pl.string());
  row("Bookshelf .pl only", now_s() - t, file_mb(pl));
  const fs::path hdb = dir / "synth.hdb", hdb_raw = dir / "synth_raw.hdb";
  io::SaveReport sr = io::save_design(d, hdb.string());
  row("design file .hdb (compressed)", sr.seconds, sr.file_bytes / 1e6);
  std::printf("  %-40s %9.1f MB raw -> %.1f MB (%.1fx)\n", "", sr.raw_bytes / 1e6, sr.file_bytes / 1e6,
              static_cast<double>(sr.raw_bytes) / sr.file_bytes);
  io::SaveOptions raw;
  raw.compress = false;
  sr = io::save_design(d, hdb_raw.string(), raw);
  row("design file .hdb (uncompressed)", sr.seconds, sr.file_bytes / 1e6);

  std::puts("read:");
  std::string why;
  t = now_s();
  Design a = io::load_input((dir / "lefdef").string());
  row("LEF + DEF (directory input)", now_s() - t, file_mb(def));
  check(same_geometry(d, a, &why), why);
  t = now_s();
  Design b = io::load_input(aux);
  row("Bookshelf .aux", now_s() - t, dir_mb(dir / "bookshelf", "synth"));
  check(same_geometry(d, b, &why, false), why + " (cells only: Bookshelf has no I/O pins)");
  t = now_s();
  Design c = io::load_design(hdb.string());
  row("design file .hdb (compressed)", now_s() - t, file_mb(hdb));
  check(same_geometry(d, c, &why), why);
  t = now_s();
  Design e = io::load_design(hdb_raw.string());
  row("design file .hdb (uncompressed)", now_s() - t, file_mb(hdb_raw));
  io::LoadOptions nv;
  nv.verify = false;
  t = now_s();
  Design f = io::load_design(hdb.string(), nv);
  row("design file .hdb (compressed, no checksum)", now_s() - t, file_mb(hdb));

  std::puts("threads (hdb load compressed / save compressed / bookshelf read / DEF read):");
  for (int th : {1, 2, 4, 8, 16}) {
    if (th > par::hardware_threads() * 2) break;
    par::ThreadLimit lim(th);
    double t1 = now_s();
    Design x = io::load_design(hdb.string());
    t1 = now_s() - t1;
    double t2 = now_s();
    io::save_design(x, (dir / "tmp.hdb").string());
    t2 = now_s() - t2;
    double t3 = now_s();
    Design y = io::load_input(aux);
    t3 = now_s() - t3;
    double t4 = now_s();
    Design z = io::load_input((dir / "lefdef").string());
    t4 = now_s() - t4;
    std::printf("  %2d threads: %7.3f s  %7.3f s  %7.3f s  %7.3f s\n", th, t1, t2, t3, t4);
  }
  return 0;
}
