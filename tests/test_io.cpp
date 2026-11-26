// Tests for step 4: reader/writer plugins, directory input + manifests,
// Bookshelf, DEF writer and parallel DEF reader, the .hdb design file and
// its codec, and keeping input locations in the placer.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

#include "../src/io/codec.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/io/bookshelf.hpp"
#include "hermes/io/design_file.hpp"
#include "hermes/io/lefdef.hpp"
#include "hermes/io/registry.hpp"
#include "hermes/log/sinks.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/place/placer.hpp"

using namespace hermes;
namespace fs = std::filesystem;

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

template <class E, class F>
std::string throws(F&& f) {
  try {
    f();
  } catch (const E& e) {
    return std::string("!") + e.what();
  } catch (...) {
    return "";
  }
  return "";
}
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

const std::string kData = HERMES_TEST_DATA_DIR;

fs::path tmpdir(const std::string& name) {
  const fs::path p = fs::temp_directory_path() / ("hermes_test_io_" + std::to_string(::getpid())) / name;
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

void write_file(const fs::path& p, const std::string& text) {
  std::ofstream o(p, std::ios::binary);
  o << text;
}

/// Exact equality of everything a design file stores.
bool same_design(const Design& a, const Design& b, std::string* why = nullptr) {
  auto no = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  if (a.name() != b.name()) return no("name");
  if (a.die() != b.die() || a.row_height() != b.row_height() || a.dbu_per_micron() != b.dbu_per_micron()) {
    return no("floorplan");
  }
  if (a.num_cells() != b.num_cells() || a.num_nets() != b.num_nets() || a.num_pins() != b.num_pins()) {
    return no("counts");
  }
  for (Id c = 0; c < static_cast<Id>(a.num_cells()); ++c) {
    const Cell &x = a.cell(c), &y = b.cell(c);
    if (x.loc != y.loc || x.width != y.width || x.height != y.height || x.orient != y.orient ||
        x.is_macro != y.is_macro || x.fixed != y.fixed || x.removed != y.removed || x.placed != y.placed) {
      return no("cell " + std::to_string(c));
    }
    if (a.cell_name(c) != b.cell_name(c) || a.cell_master(c) != b.cell_master(c)) return no("cell names");
    if (!x.removed && b.find_cell(a.cell_name(c)) != c) return no("cell lookup");
    if (!std::equal(a.cell_pins(c).begin(), a.cell_pins(c).end(), b.cell_pins(c).begin(), b.cell_pins(c).end())) {
      return no("cell pins");
    }
  }
  for (Id n = 0; n < static_cast<Id>(a.num_nets()); ++n) {
    if (a.net(n).weight != b.net(n).weight || a.net(n).removed != b.net(n).removed) return no("net");
    if (a.net_name(n) != b.net_name(n)) return no("net name");
    if (!a.net(n).removed && b.find_net(a.net_name(n)) != n) return no("net lookup");
    if (!std::equal(a.net_pins(n).begin(), a.net_pins(n).end(), b.net_pins(n).begin(), b.net_pins(n).end())) {
      return no("net pins");
    }
  }
  for (Id p = 0; p < static_cast<Id>(a.num_pins()); ++p) {
    const Pin &x = a.pin(p), &y = b.pin(p);
    if (x.cell != y.cell || x.net != y.net || x.offset != y.offset || x.dir != y.dir) return no("pin");
    if (a.pin_name(p) != b.pin_name(p)) return no("pin name");
  }
  if (!std::equal(a.io_pins().begin(), a.io_pins().end(), b.io_pins().begin(), b.io_pins().end())) return no("io");
  if (a.rows().size() != b.rows().size()) return no("rows");
  for (std::size_t i = 0; i < a.rows().size(); ++i) {
    const Row &x = a.rows()[i], &y = b.rows()[i];
    if (x.origin != y.origin || x.site_width != y.site_width || x.site_height != y.site_height ||
        x.site_step != y.site_step || x.num_sites != y.num_sites || x.orient != y.orient ||
        a.site_name(x.site) != b.site_name(y.site)) {
      return no("row " + std::to_string(i));
    }
  }
  const RouteGrid &g = a.route_grid(), &h = b.route_grid();
  if (g.nx != h.nx || g.ny != h.ny || g.num_layers != h.num_layers || g.tile_w != h.tile_w ||
      g.vertical_capacity != h.vertical_capacity || g.blockage_cell != h.blockage_cell ||
      g.blockage_layers != h.blockage_layers || g.adjust_capacity != h.adjust_capacity) {
    return no("route grid");
  }
  return true;
}

/// A mixed design with every kind of data the file format stores.
Design rich_design(int n, unsigned seed) {
  std::mt19937 rng(seed);
  Design d("rich");
  d.set_die({0, 0, 400000, 300000});
  d.set_dbu_per_micron(2000);
  const Id site = d.intern_site_name("core");
  std::vector<Row> rows;
  for (int r = 0; r < 100; ++r) rows.push_back({{1000, r * 2000}, 200, 2000, 200, 1500, r % 2 ? Orient::FS : Orient::N, site});
  d.set_rows(rows);
  std::uniform_int_distribution<Dbu> pos(0, 390000), w(1, 8);
  for (int i = 0; i < n; ++i) {
    const bool macro = i % 97 == 0;
    const Id c = d.add_cell("c" + std::to_string(i), macro ? 20000 : 200 * w(rng), macro ? 20000 : 2000, macro);
    d.move_cell(c, {pos(rng), pos(rng) % 300000});
    d.set_orient(c, static_cast<Orient>(i % 8));
    d.set_fixed(c, macro);
    d.set_placed(c, i % 3 != 0);
    if (i % 5) d.set_cell_master(c, macro ? "RAM" : "INV_X" + std::to_string(i % 4));
  }
  std::uniform_int_distribution<int> deg(2, 7), pick(0, n - 1);
  for (int k = 0; k < n; ++k) {
    const Id net = d.add_net("net/" + std::to_string(k));
    d.set_net_weight(net, k % 11 == 0 ? 2.5 : 1.0);
    const int m = deg(rng);
    for (int j = 0; j < m; ++j) {
      d.connect(pick(rng), net, j == 0 ? "Z" : "A" + std::to_string(j), {pos(rng) % 2000, pos(rng) % 2000},
                j == 0 ? PinDir::Output : PinDir::Input);
    }
    if (k % 50 == 0) d.connect(kInvalidId, net, "io" + std::to_string(k), {0, k}, PinDir::Output);
  }
  d.add_pin(3, "unconnected");
  d.remove_cell(5);
  d.remove_net(7);
  RouteGrid g;
  g.nx = 10;
  g.ny = 8;
  g.num_layers = 2;
  g.tile_w = g.tile_h = 40000;
  g.vertical_capacity = {0, 20};
  g.horizontal_capacity = {20, 0};
  g.blockage_cell = {0, 97};
  g.blockage_layers = {1, 1, 2};
  g.blockage_begin = {0, 1, 3};
  g.adjust_edges = {0, 0, 1, 1, 0, 1};
  g.adjust_capacity = {5};
  d.set_route_grid(g);
  d.flush();
  return d;
}

// --- Codec ------------------------------------------------------------------

void test_codec() {
  std::mt19937 rng(3);
  for (std::size_t n : {0u, 1u, 3u, 11u, 12u, 13u, 64u, 1000u, 70000u, 300000u}) {
    for (int kind = 0; kind < 3; ++kind) {
      std::vector<std::uint8_t> src(n);
      for (std::size_t i = 0; i < n; ++i) {
        src[i] = kind == 0 ? static_cast<std::uint8_t>(rng()) : kind == 1 ? static_cast<std::uint8_t>(i % 7)
                                                                           : static_cast<std::uint8_t>((i / 100) & 3);
      }
      std::vector<std::uint8_t> c, back(n);
      io::codec::compress(src.data(), n, c);
      CHECK(c.size() <= io::codec::compress_bound(n));
      CHECK(io::codec::decompress(c.data(), c.size(), back.data(), n));
      CHECK(back == src);
      if (kind == 1 && n >= 1000) CHECK(c.size() < n / 10);
      if (!c.empty()) {  // Corruption is detected, never overruns.
        std::vector<std::uint8_t> bad = c;
        bad.resize(bad.size() / 2);
        io::codec::decompress(bad.data(), bad.size(), back.data(), n);  // must not crash
        CHECK(!io::codec::decompress(c.data(), c.size(), back.data(), n + 1));
      }
    }
  }
  // Filters invert.
  std::vector<std::uint8_t> v(8 * 1000), sh(v.size()), un(v.size());
  for (auto& x : v) x = static_cast<std::uint8_t>(rng());
  for (std::size_t elem : {1u, 4u, 8u, 3u}) {
    const std::size_t n = v.size() / elem;
    io::codec::shuffle(v.data(), sh.data(), n, elem);
    io::codec::unshuffle(sh.data(), un.data(), n, elem);
    CHECK(std::equal(v.begin(), v.begin() + n * elem, un.begin()));
  }
  std::vector<std::uint8_t> dl = v;
  io::codec::delta_encode(dl.data(), 1000, 8);
  io::codec::delta_decode(dl.data(), 1000, 8);
  CHECK(dl == v);
}

// --- Design file ------------------------------------------------------------

void test_design_file_roundtrip() {
  const fs::path dir = tmpdir("hdb");
  const Design d = rich_design(3000, 1);
  for (bool compress : {true, false}) {
    io::SaveOptions so;
    so.compress = compress;
    so.chunk_bytes = 4096;  // Many chunks: exercises the parallel paths.
    so.note = "unit test";
    so.extra["placer.state"] = {1, 2, 3, 4, 5};
    const std::string path = (dir / (compress ? "c.hdb" : "r.hdb")).string();
    const io::SaveReport rep = io::save_design(d, path, so);
    CHECK(rep.file_bytes == fs::file_size(path));
    CHECK(!fs::exists(path + ".tmp"));
    if (compress) CHECK(rep.file_bytes < rep.raw_bytes);
    CHECK(io::is_design_file(path));
    io::DesignFileInfo info;
    std::map<std::string, std::vector<std::uint8_t>> extra;
    const Design e = io::load_design(path, {}, &info, &extra);
    std::string why;
    CHECK(same_design(d, e, &why));
    if (!why.empty()) std::fprintf(stderr, "  differs: %s\n", why.c_str());
    CHECK(info.major == io::kDesignFileMajor);
    CHECK(info.meta.at("note") == "unit test");
    CHECK(extra.size() == 1 && extra["placer.state"] == std::vector<std::uint8_t>({1, 2, 3, 4, 5}));
    CHECK(e.find_cell("c5") == kInvalidId);  // Removed cells stay unfindable.
    CHECK(e.find_net("net/7") == kInvalidId);
    CHECK(std::abs(total_hpwl(e) - total_hpwl(d)) < 1e-6);
    const io::DesignFileInfo ins = io::inspect_design_file(path);
    CHECK(ins.meta.at("num_cells") == "3000");
  }
  // Same result with 1 thread.
  {
    par::ThreadLimit one(1);
    const Design e = io::load_design((dir / "c.hdb").string());
    CHECK(same_design(d, e));
  }
  // Empty design.
  {
    const Design empty("empty");
    io::save_design(empty, (dir / "e.hdb").string());
    const Design e = io::load_design((dir / "e.hdb").string());
    CHECK(e.num_cells() == 0 && e.name() == "empty");
  }
}

void test_design_file_errors() {
  const fs::path dir = tmpdir("hdb_err");
  const Design d = rich_design(2000, 2);
  const std::string path = (dir / "d.hdb").string();
  io::SaveOptions so;
  so.chunk_bytes = 8192;
  io::save_design(d, path, so);
  std::vector<char> bytes(fs::file_size(path));
  {
    std::ifstream in(path, std::ios::binary);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  auto write_bytes = [&](const std::string& p, const std::vector<char>& b) {
    std::ofstream o(p, std::ios::binary);
    o.write(b.data(), static_cast<std::streamsize>(b.size()));
  };
  // A flipped data byte: checksum error naming the section.
  {
    std::vector<char> b = bytes;
    b[b.size() / 3] ^= 0x5a;
    write_bytes((dir / "flip.hdb").string(), b);
    const std::string e = throws<io::ParseError>([&] { io::load_design((dir / "flip.hdb").string()); });
    CHECK(has(e, "checksum") || has(e, "corrupt"));
  }
  // Truncated file.
  {
    std::vector<char> b(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
    write_bytes((dir / "trunc.hdb").string(), b);
    CHECK(has(throws<io::ParseError>([&] { io::load_design((dir / "trunc.hdb").string()); }), "corrupt"));
  }
  // A newer major version is refused with a clear message.
  {
    std::vector<char> b = bytes;
    const std::uint16_t major = io::kDesignFileMajor + 1;
    std::memcpy(b.data() + 8, &major, 2);
    write_bytes((dir / "new.hdb").string(), b);
    CHECK(has(throws<io::ParseError>([&] { io::load_design((dir / "new.hdb").string()); }), "not supported"));
  }
  // Not a design file.
  write_file(dir / "text.hdb", "hello");
  CHECK(has(throws<io::ParseError>([&] { io::load_design((dir / "text.hdb").string()); }), "not a hermes design file"));
}

void test_save_job() {
  const fs::path dir = tmpdir("job");
  Design d = rich_design(1000, 3);
  io::SaveReport rep;
  JobHandle j = io::start_save_job(d, (dir / "bg.hdb").string(), {}, &rep);
  d.move_cell(0, {1, 1});  // The running engine keeps editing its own copy.
  j.wait();
  CHECK(j.state() == JobState::Done);
  CHECK(rep.file_bytes > 0);
  const Design e = io::load_design((dir / "bg.hdb").string());
  CHECK(e.cell(0).loc != Point(1, 1) || d.cell(0).loc == e.cell(0).loc);
  CHECK(e.num_cells() == 1000);
}

// --- Bookshelf --------------------------------------------------------------

void write_bookshelf_sample(const fs::path& dir) {
  write_file(dir / "t.aux", "RowBasedPlacement : t.nodes t.nets t.wts t.pl t.scl\n");
  write_file(dir / "t.nodes",
             "UCLA nodes 1.0\n# comment\n\nNumNodes : 5\nNumTerminals : 2\n"
             "  a 4 12\n  b 6 12\n  m 40 36\n  p0 1 1 terminal\n  blk 20 24 terminal\n");
  write_file(dir / "t.nets",
             "UCLA nets 1.0\nNumNets : 3\nNumPins : 7\n"
             "NetDegree : 3 n0\n  a O : 1.0 0.0\n  b I : -2.0 1.5\n  p0 I : 0 0\n"
             "NetDegree : 2   n1\n\ta O\n\tm I : 10 -8\n"
             "NetDegree:2\n  blk B : 0 0\n  b I : 0 0\n");
  write_file(dir / "t.pl",
             "UCLA pl 1.0\n\na 10 0 : N\nb 30 12 : FS\nm 100 0 : E\np0 0 50 : N /FIXED\nblk 200 0 : N /FIXED\n");
  write_file(dir / "t.scl",
             "UCLA scl 1.0\nNumRows : 2\n"
             "CoreRow Horizontal\n Coordinate : 0\n Height : 12\n Sitewidth : 1\n Sitespacing : 1\n"
             " Siteorient : 1\n Sitesymmetry : 1\n SubrowOrigin : 0 NumSites : 300\nEnd\n"
             "CoreRow Horizontal\n Coordinate : 12\n Height : 12\n Sitewidth : 1\n Sitespacing : 1\n"
             " Siteorient : FS\n Sitesymmetry : 1\n SubrowOrigin : 0 NumSites : 300\nEnd\n");
  write_file(dir / "t.wts", "UCLA wts 1.0\nn1 3\na 0\n");
}

void test_bookshelf_reader() {
  const fs::path dir = tmpdir("bs");
  write_bookshelf_sample(dir);
  io::BookshelfReport rep;
  const Design d = io::read_bookshelf_aux((dir / "t.aux").string(), {}, &rep);
  CHECK(d.name() == "t");
  CHECK(d.num_cells() == 5 && d.num_nets() == 3 && d.num_pins() == 7);
  CHECK(rep.num_terminals == 2 && rep.num_rows == 2);
  CHECK(d.rows().size() == 2 && d.rows()[1].orient == Orient::FS && d.row_height() == 12);
  CHECK(d.die() == Rect(0, 0, 300, 24));
  const Id a = d.find_cell("a"), b = d.find_cell("b"), m = d.find_cell("m"), p0 = d.find_cell("p0");
  CHECK(d.cell(a).loc == Point(10, 0) && !d.cell(a).fixed && !d.cell(a).placed);  // plain .pl: unplaced
  CHECK(d.cell(p0).fixed && d.cell(p0).placed);
  CHECK(d.cell(m).is_macro && !d.cell(a).is_macro);
  // m is E: 40x36 -> 36x40.
  CHECK(d.cell(m).width == 36 && d.cell(m).height == 40 && d.cell(m).orient == Orient::E);
  // Pin a/n0: center (2, 6) + (1, 0) = (3, 6).
  const Id pa = d.net_pins(d.find_net("n0"))[0];
  CHECK(d.pin(pa).offset == Point(3, 6) && d.pin(pa).dir == PinDir::Output);
  // b is FS: offset (3-2, 6+1.5) = (1, 7.5) -> (1, 12-7.5) = (1, 4.5) -> rounds to (1, 5) (llround).
  const Id pb = d.net_pins(d.find_net("n0"))[1];
  CHECK(d.pin(pb).offset.x == 1 && (d.pin(pb).offset.y == 4 || d.pin(pb).offset.y == 5));
  // m (E): raw (20+10, 18-8) = (30, 10); E maps (x, y) -> (y, w - x) = (10, 10).
  const Id pm = d.net_pins(d.find_net("n1"))[1];
  CHECK(d.pin(pm).offset == Point(10, 10));
  CHECK(d.pin(d.net_pins(d.find_net("n1"))[0]).offset == Point(2, 6));  // no offset given: center
  CHECK(d.net_name(2) == "net_2");  // unnamed net
  CHECK(d.net(d.find_net("n1")).weight == 3.0);
  CHECK(d.pin_name(pa) == "p0" && d.pin_name(d.cell_pins(b)[1]) == "p1");

  // Scale.
  io::Options o;
  o.set("bookshelf.scale=10");
  const Design s = io::read_bookshelf_aux((dir / "t.aux").string(), o);
  CHECK(s.cell(a).loc == Point(100, 0) && s.cell(a).width == 40 && s.dbu_per_micron() == 10);
  CHECK(s.pin(s.net_pins(s.find_net("n0"))[1]).offset == Point(10, 45));

  // Errors carry file:line.
  write_file(dir / "bad.nets", "UCLA nets 1.0\nNetDegree : 2 x\n a I : 0 0\n zz O : 0 0\n");
  io::BookshelfFiles f = io::parse_aux((dir / "t.aux").string());
  f.nets = (dir / "bad.nets").string();
  CHECK(has(throws<io::ParseError>([&] { io::read_bookshelf(f); }), "bad.nets:4: unknown node 'zz'"));
  write_file(dir / "bad2.nets", "UCLA nets 1.0\nNetDegree : 3 x\n a I : 0 0\nNetDegree : 1 y\n a I\n");
  f.nets = (dir / "bad2.nets").string();
  CHECK(has(throws<io::ParseError>([&] { io::read_bookshelf(f); }), "bad2.nets:2: net has fewer pins"));
}

void test_bookshelf_route() {
  const fs::path dir = tmpdir("bs_route");
  write_bookshelf_sample(dir);
  write_file(dir / "t.route",
             "route 1.0\nGrid : 4 3 2\nVerticalCapacity : 0 10\nHorizontalCapacity : 10 0\n"
             "MinWireWidth : 1 1\nMinWireSpacing : 1 1\nViaSpacing : 0 0\nGridOrigin : 0 0\nTileSize : 75 8\n"
             "BlockagePorosity : 0\nNumNiTerminals : 1\n p0 1\nNumBlockageNodes : 1\n blk 2 1 2\n");
  const Design d = io::read_bookshelf_aux((dir / "t.aux").string());  // .route found next to .nodes
  const RouteGrid& g = d.route_grid();
  CHECK(g.nx == 4 && g.ny == 3 && g.num_layers == 2 && g.tile_w == 75 && g.tile_h == 8);
  CHECK(g.vertical_capacity == std::vector<double>({0, 10}));
  CHECK(g.pin_layer_cell.size() == 1 && g.pin_layer[0] == 1);
  CHECK(g.num_blockages() == 1 && g.blockage_cell[0] == d.find_cell("blk"));
  CHECK(g.blockage_layers == std::vector<std::int32_t>({1, 2}));
}

void test_bookshelf_roundtrip_and_parallel() {
  const fs::path dir = tmpdir("bs_rt");
  // Big enough that every file is parsed in several chunks.
  Design d = rich_design(30000, 4);
  // Bookshelf has no I/O pins or removed objects: drop them first.
  Design clean("clean");
  {
    clean.set_die(d.die());
    clean.set_rows(std::vector<Row>(d.rows().begin(), d.rows().end()));
    for (Id c = 0; c < static_cast<Id>(d.num_cells()); ++c) {
      const Cell& x = d.cell(c);
      if (x.removed) continue;
      const Id n = clean.add_cell(d.cell_name(c), x.width, x.height, x.is_macro);
      clean.move_cell(n, x.loc);
      clean.set_fixed(n, x.fixed);
      clean.set_placed(n, true);
      clean.set_orient(n, Orient::N);
    }
    for (Id n = 0; n < static_cast<Id>(d.num_nets()); ++n) {
      if (d.net(n).removed) continue;
      const Id nn = clean.add_net(d.net_name(n));
      clean.set_net_weight(nn, d.net(n).weight);
      for (Id p : d.net_pins(n)) {
        const Pin& pin = d.pin(p);
        if (pin.cell == kInvalidId) continue;
        const Id cc = clean.find_cell(d.cell_name(pin.cell));
        clean.connect(cc, nn, "x", pin.offset, pin.dir);
      }
    }
  }
  const std::string aux = io::write_bookshelf(clean, dir.string(), "rt");
  const Design back = io::read_bookshelf_aux(aux);
  CHECK(back.num_cells() == clean.num_cells() && back.num_pins() == clean.num_pins());
  bool same = true;
  for (Id c = 0; c < static_cast<Id>(clean.num_cells()); ++c) {
    const Cell &x = clean.cell(c), &y = back.cell(back.find_cell(clean.cell_name(c)));
    same &= x.loc == y.loc && x.width == y.width && x.fixed == y.fixed && y.placed;  // hermes .pl: placed
  }
  CHECK(same);
  CHECK(std::abs(total_hpwl(clean) - total_hpwl(back)) < 1e-6);
  for (Id n = 0; n < static_cast<Id>(clean.num_nets()); n += 97) {
    CHECK(back.net(back.find_net(clean.net_name(n))).weight == clean.net(n).weight);
  }
  // 1 thread gives the same design as many.
  Design one;
  {
    par::ThreadLimit lim(1);
    one = io::read_bookshelf_aux(aux);
  }
  CHECK(same_design(back, one));
  // .pl only, and with a scale.
  io::write_bookshelf_pl(back, (dir / "x.pl").string(), 2.0);
  std::ifstream in(dir / "x.pl");
  std::string head;
  std::getline(in, head);
  CHECK(head == "UCLA pl 1.0");
}

// --- LEF/DEF ----------------------------------------------------------------

void test_def_keeps_locations_and_writes() {
  const fs::path dir = tmpdir("def");
  io::LefLibrary lib;
  io::read_lef(kData + "/sample.lef", lib);
  const Design d = io::read_def(kData + "/sample.def", lib);
  std::size_t placed = 0, unplaced = 0;
  for (const Cell& c : d.cells()) {
    if (c.placed) ++placed;
    else ++unplaced;
    if (c.fixed) CHECK(c.placed);
  }
  CHECK(placed == 4 && unplaced == 1);
  CHECK(!d.cell_master(0).empty());
  CHECK(!d.rows().empty() && d.site_name(d.rows()[0].site) == "core");
  CHECK(d.dbu_per_micron() == 2000);
  // Write and read back.
  const std::string out = (dir / "o.def").string();
  io::write_def(d, out);
  const Design e = io::read_def(out, lib);
  CHECK(e.num_cells() == d.num_cells() && e.num_nets() == d.num_nets() && e.num_pins() == d.num_pins());
  for (Id c = 0; c < static_cast<Id>(d.num_cells()); ++c) {
    const Id k = e.find_cell(d.cell_name(c));
    CHECK(k != kInvalidId && e.cell(k).loc == d.cell(c).loc && e.cell(k).orient == d.cell(c).orient &&
          e.cell(k).placed == d.cell(c).placed && e.cell(k).fixed == d.cell(c).fixed);
  }
  CHECK(std::abs(total_hpwl(e) - total_hpwl(d)) < 1e-6);
  CHECK(e.rows().size() == d.rows().size() && e.rows()[0].num_sites == d.rows()[0].num_sites);
}

void test_def_weight_and_warnings() {
  io::LefLibrary lib;
  io::read_lef(kData + "/sample.lef", lib);
  const std::string def =
      "VERSION 5.8 ;\nDESIGN w ;\nUNITS DISTANCE MICRONS 1000 ;\nDIEAREA ( 0 0 ) ( 10000 10000 ) ;\n"
      "COMPONENTS 2 ;\n- u1 INV + PLACED ( 0 0 ) N ;\n- u2 INV ;\nEND COMPONENTS\n"
      "PINS 1 ;\n- in + NET a + DIRECTION INPUT + PLACED ( 5 5 ) N ;\nEND PINS\n"
      "NETS 3 ;\n- a ( PIN in ) ( u1 A ) ( PIN in ) + WEIGHT 4 ;\n"
      "- b ( u1 Y ) ( u2 A ) ( * VDD ) + ROUTED M1 ( 0 0 ) ( 10 0 ) + WEIGHT 2 ;\n"
      "- c ( u2 Y ) ( u1 NOPE ) ;\nEND NETS\nEND DESIGN\n";
  io::DefReadReport rep;
  const Design d = io::read_def_string(def, lib, &rep);
  CHECK(d.net(d.find_net("a")).weight == 4.0 && d.net(d.find_net("b")).weight == 2.0);
  CHECK(d.net_pins(d.find_net("a")).size() == 2);  // I/O pin connects once
  CHECK(d.net_pins(d.find_net("b")).size() == 2);  // '*' skipped
  CHECK(rep.num_warnings >= 2);
  CHECK(d.cell(d.find_cell("u1")).placed && !d.cell(d.find_cell("u2")).placed);
  // Errors name the line.
  const std::string bad = def.substr(0, def.find("- c")) + "- c ( zz Y ) ;\nEND NETS\nEND DESIGN\n";
  CHECK(has(throws<io::ParseError>([&] { io::read_def_string(bad, lib, nullptr, {}, "x.def"); }),
            "x.def:15: net 'c' references unknown component 'zz'"));
  const std::string dup = "DESIGN w ;\nUNITS DISTANCE MICRONS 1000 ;\nCOMPONENTS 2 ;\n- u1 INV ;\n- u1 INV ;\nEND COMPONENTS\nEND DESIGN\n";
  CHECK(has(throws<io::ParseError>([&] { io::read_def_string(dup, lib, nullptr, {}, "d.def"); }),
            "d.def:5: duplicate component 'u1'"));
}

void test_def_parallel_matches_serial() {
  // A DEF large enough to be split into many chunks.
  const fs::path dir = tmpdir("defpar");
  io::LefLibrary lib;
  io::read_lef(kData + "/sample.lef", lib);
  std::string def = "VERSION 5.8 ;\nDESIGN big ;\nUNITS DISTANCE MICRONS 1000 ;\nDIEAREA ( 0 0 ) ( 2000000 2000000 ) ;\n";
  const int n = 40000;
  def += "COMPONENTS " + std::to_string(n) + " ;\n";
  for (int i = 0; i < n; ++i) {
    def += "- u" + std::to_string(i) + " INV + PLACED ( " + std::to_string(i * 7 % 1999000) + " " +
           std::to_string(i * 13 % 1999000) + " ) " + (i % 2 ? "FS" : "N") + " ;\n";
  }
  def += "END COMPONENTS\nNETS " + std::to_string(n - 1) + " ;\n";
  for (int i = 0; i + 1 < n; ++i) {
    def += "- n" + std::to_string(i) + " ( u" + std::to_string(i) + " Y )\n  ( u" + std::to_string(i + 1) + " A ) ;\n";
  }
  def += "END NETS\nEND DESIGN\n";
  write_file(dir / "big.def", def);
  const Design many = io::read_def((dir / "big.def").string(), lib);
  Design one;
  {
    par::ThreadLimit lim(1);
    one = io::read_def((dir / "big.def").string(), lib);
  }
  CHECK(many.num_cells() == static_cast<std::size_t>(n) && many.num_pins() == static_cast<std::size_t>(2 * (n - 1)));
  CHECK(same_design(many, one));
  // An error deep in the file reports the right line.
  const std::size_t at = def.find("- n30000 ");
  std::string bad = def;
  bad.replace(at, 9, "- n30000 ( qq Y ) ");
  const std::size_t line = 1 + static_cast<std::size_t>(std::count(def.begin(), def.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
  write_file(dir / "bad.def", bad);
  const std::string e = throws<io::ParseError>([&] { io::read_def((dir / "bad.def").string(), lib); });
  CHECK(has(e, (":" + std::to_string(line) + ": net 'n30000' references unknown component 'qq'").c_str()));
}

// --- Registry / directory input ------------------------------------------------

void test_directory_detection() {
  const fs::path root = tmpdir("detect");
  // LEF/DEF directory.
  const fs::path ld = root / "ld";
  fs::create_directories(ld);
  fs::copy_file(kData + "/sample.lef", ld / "z_tech.lef");  // has LAYERs: tech LEF
  write_file(ld / "a_cells.lef",
             "VERSION 5.8 ;\nMACRO EXTRA\n  CLASS CORE ;\n  SIZE 1 BY 2 ;\nEND EXTRA\nEND LIBRARY\n");
  fs::copy_file(kData + "/sample.def", ld / "top.def");
  io::InputPlan p = io::plan_input(ld.string());
  CHECK(p.format == "lefdef");
  CHECK(p.items.size() == 3 && fs::path(p.items[0].path).filename() == "z_tech.lef");  // tech LEF first
  CHECK(p.items[2].role == "def");
  io::LoadInfo info;
  const Design d = io::load_input(ld.string(), {}, &info);
  CHECK(d.num_cells() == 5 && info.seconds >= 0 && !info.stats.empty());

  // Bookshelf directory.
  const fs::path bs = root / "bs";
  fs::create_directories(bs);
  write_bookshelf_sample(bs);
  CHECK(io::plan_input(bs.string()).format == "bookshelf");
  CHECK(io::load_input(bs.string()).num_cells() == 5);
  // Bookshelf without .aux.
  fs::remove(bs / "t.aux");
  p = io::plan_input(bs.string());
  CHECK(p.format == "bookshelf" && !p.paths("scl").empty());
  CHECK(io::load_input(bs.string()).num_nets() == 3);

  // Design file directory, and a single .hdb file.
  const fs::path hd = root / "hd";
  fs::create_directories(hd);
  io::write_design(d, (hd / "s.hdb").string());
  CHECK(io::plan_input(hd.string()).format == "hdb");
  CHECK(io::load_input((hd / "s.hdb").string()).num_cells() == 5);

  // Ambiguous: two DEFs; and DEF + hdb.
  fs::copy_file(ld / "top.def", ld / "other.def");
  std::string e = throws<io::InputError>([&] { io::plan_input(ld.string()); });
  CHECK(has(e, "more than one input") && has(e, "other.def") && has(e, "top.def") && has(e, "hermes.inputs"));
  // ... a DEF file path picks it, with the LEFs next to it.
  p = io::plan_input((ld / "top.def").string());
  CHECK(p.format == "lefdef" && p.paths("lef").size() == 2 && p.paths("def").size() == 1);
  // Nothing readable.
  const fs::path empty = root / "empty";
  fs::create_directories(empty);
  write_file(empty / "readme.txt", "x");
  e = throws<io::InputError>([&] { io::plan_input(empty.string()); });
  CHECK(has(e, "nothing to read") && has(e, "readme.txt"));
  // DEF without LEF.
  const fs::path nolef = root / "nolef";
  fs::create_directories(nolef);
  fs::copy_file(ld / "top.def", nolef / "top.def");
  CHECK(has(throws<io::InputError>([&] { io::plan_input(nolef.string()); }), "no .lef"));
  CHECK(has(throws<io::InputError>([&] { io::plan_input((root / "missing").string()); }), "not found"));
}

void test_manifest() {
  const fs::path root = tmpdir("manifest");
  fs::create_directories(root / "lib");
  fs::copy_file(kData + "/sample.lef", root / "lib" / "a.lef");
  fs::copy_file(kData + "/sample.def", root / "one.def");
  fs::copy_file(kData + "/sample.def", root / "two.def");  // ambiguous without the manifest
  write_file(root / "hermes.inputs",
             "# inputs for this run\nlef lib/*.lef\ndef two.def   # the one we want\n"
             "option def.allow_unknown_macros=1\n");
  io::InputPlan p = io::plan_input(root.string());
  CHECK(p.format == "lefdef" && has(p.origin, "manifest"));
  CHECK(p.paths("lef").size() == 1 && fs::path(p.paths("def")[0]).filename() == "two.def");
  CHECK(p.options.get_bool("def.allow_unknown_macros", false));
  CHECK(io::load_input(root.string()).num_cells() == 5);
  // Errors.
  write_file(root / "hermes.inputs", "lef lib/*.lef\nfoo x.def\n");
  CHECK(has(throws<io::InputError>([&] { io::plan_input(root.string()); }), "file not found"));
  write_file(root / "hermes.inputs", "format lefdef\nnodes one.def\n");
  CHECK(has(throws<io::InputError>([&] { io::plan_input(root.string()); }), "role 'nodes' is not used by format lefdef"));
  write_file(root / "hermes.inputs", "lef nothing*.lef\n");
  CHECK(has(throws<io::InputError>([&] { io::plan_input(root.string()); }), "no file matches"));
  write_file(root / "hermes.inputs", "format foo\n");
  CHECK(has(throws<io::InputError>([&] { io::plan_input(root.string()); }), "unknown format"));
  // A manifest path directly.
  write_file(root / "alt.inputs", "lef lib/a.lef\ndef one.def\n");
  CHECK(io::plan_input((root / "alt.inputs").string()).format == "lefdef");
}

void test_registry_and_writers() {
  auto& reg = io::Registry::global();
  CHECK(reg.reader("lefdef") && reg.reader("bookshelf") && reg.reader("hdb"));
  CHECK(reg.writer_for("x.def") == reg.writer("def"));
  CHECK(reg.writer_for("x.HDB") == reg.writer("hdb"));
  CHECK(reg.writer_for("x.txt") == nullptr);
  const fs::path dir = tmpdir("writers");
  const Design d = rich_design(500, 9);
  io::write_design(d, (dir / "a" / "d.hdb").string());
  io::write_design(d, (dir / "d.def").string());
  io::write_design(d, (dir / "d.pl").string());
  io::write_design(d, (dir / "bs" / "d.aux").string());
  CHECK(fs::exists(dir / "a" / "d.hdb") && fs::exists(dir / "d.def") && fs::exists(dir / "d.pl"));
  CHECK(fs::exists(dir / "bs" / "d.nodes") && fs::exists(dir / "bs" / "d.scl"));
  CHECK(!throws<std::invalid_argument>([&] { io::write_design(d, (dir / "x.txt").string()); }).empty());

  // A custom reader plugs in next to the built-ins.
  struct Dummy final : io::Reader {
    std::string_view name() const override { return "dummy"; }
    std::string_view description() const override { return "test"; }
    std::vector<std::string_view> roles() const override { return {"dummy"}; }
    std::vector<io::InputPlan> detect(const std::vector<fs::path>& files, std::string&) const override {
      std::vector<io::InputPlan> v;
      for (const auto& f : files) {
        if (f.extension() == ".dummy") v.push_back({"dummy", {{"dummy", f.string()}}, {}, {}});
      }
      return v;
    }
    bool accepts_file(const fs::path&, io::InputPlan&) const override { return false; }
    Design read(const io::InputPlan&, const io::Options&, io::LoadInfo&) const override {
      Design x("dummy");
      x.add_cell("only", 1, 1);
      return x;
    }
  };
  io::Registry local;
  local.add_reader(std::make_unique<Dummy>());
  const fs::path dd = tmpdir("dummy");
  write_file(dd / "a.dummy", "");
  CHECK(io::load_input(dd.string(), {}, nullptr, local).name() == "dummy");
}

// --- Placer keeps input locations ------------------------------------------

void test_placer_keeps_placed() {
  Design d("keep");
  d.set_die({0, 0, 100000, 100000});
  d.set_row_height(1000);
  const Id a = d.add_cell("a", 1000, 1000), b = d.add_cell("b", 1000, 1000);
  d.move_cell(a, {10000, 20000});
  d.set_placed(a, true);
  const Id n = d.add_net("n");
  d.connect(a, n, "Z", {500, 500}, PinDir::Output);
  d.connect(b, n, "A", {500, 500});
  PlacerOptions o;
  o.max_iterations = 0;  // Initial placement only.
  o.legalize = o.detailed_placement = false;
  o.timing_interval = o.routability_interval = 0;
  MixedSizePlacer(o).run(d, nullptr, nullptr);
  CHECK(d.cell(a).loc == Point(10000, 20000));  // kept
  CHECK(std::abs(d.cell(b).loc.x - 49500) < 2000 && std::abs(d.cell(b).loc.y - 49500) < 2000);  // die center
  CHECK(d.cell(b).placed);  // placed by the placer now
  o.keep_placed = false;
  d.set_placed(b, false);
  MixedSizePlacer(o).run(d, nullptr, nullptr);
  CHECK(std::abs(d.cell(a).loc.x - 49500) < 2000);  // reset to the center
}

void test_options() {
  io::Options o;
  o.set("a=1");
  o.set("flag");
  o.set("x.y=2.5");
  CHECK(o.get_bool("a", false) && o.get_bool("flag", false) && o.get_double("x.y", 0) == 2.5);
  CHECK(o.get_int("missing", 7) == 7 && o.get("missing", "d") == "d");
  o.set("b=maybe");
  CHECK(!throws<std::invalid_argument>([&] { o.get_bool("b", false); }).empty());
  io::Options m;
  m.set("a=0");
  m.set("c=3");
  o.merge_defaults(m);
  CHECK(o.get_bool("a", false) && o.get_int("c", 0) == 3);
}

}  // namespace

int main() {
  hermes::log::console_sink()->set_level(hermes::log::Level::Off);
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"options", test_options},
      {"codec", test_codec},
      {"design_file_roundtrip", test_design_file_roundtrip},
      {"design_file_errors", test_design_file_errors},
      {"save_job", test_save_job},
      {"bookshelf_reader", test_bookshelf_reader},
      {"bookshelf_route", test_bookshelf_route},
      {"bookshelf_roundtrip_and_parallel", test_bookshelf_roundtrip_and_parallel},
      {"def_keeps_locations_and_writes", test_def_keeps_locations_and_writes},
      {"def_weight_and_warnings", test_def_weight_and_warnings},
      {"def_parallel_matches_serial", test_def_parallel_matches_serial},
      {"directory_detection", test_directory_detection},
      {"manifest", test_manifest},
      {"registry_and_writers", test_registry_and_writers},
      {"placer_keeps_placed", test_placer_keeps_placed},
  };
  for (const auto& [name, fn] : tests) {
    const int before = g_failures;
    try {
      fn();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "  EXCEPTION %s\n", e.what());
      ++g_failures;
    }
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name.c_str());
  }
  fs::remove_all(fs::temp_directory_path() / ("hermes_test_io_" + std::to_string(::getpid())));
  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
