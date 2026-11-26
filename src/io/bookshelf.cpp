#include "hermes/io/bookshelf.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>

#include "hermes/db/design_access.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"

namespace hermes::io {

namespace {

HERMES_LOG_CHANNEL(lg_bs, "io.bookshelf");
namespace fs = std::filesystem;
using DA = detail::DesignAccess;

// Bookshelf separates fields with whitespace and ':' ("NetDegree : 4 n0",
// "o1 I : -0.5 2", "o0 10 20 : N /FIXED"), so ':' counts as a separator.
class BsTokens {
 public:
  explicit BsTokens(std::string_view s) : s_(s) {}
  std::string_view next() {
    while (p_ < s_.size() && sep(s_[p_])) ++p_;
    const std::size_t b = p_;
    while (p_ < s_.size() && !sep(s_[p_])) ++p_;
    return s_.substr(b, p_ - b);
  }

 private:
  static bool sep(char c) { return c == ':' || Tokens::is_space(c); }
  std::string_view s_;
  std::size_t p_ = 0;
};

std::size_t line_of(const InputFile& f, std::size_t off) {
  return 1 + static_cast<std::size_t>(std::count(f.data(), f.data() + std::min(off, f.size()), '\n'));
}

[[noreturn]] void fail_at(const InputFile& f, std::size_t off, const std::string& msg) {
  throw ParseError(f.path() + ":" + std::to_string(line_of(f, off)) + ": " + msg);
}

/// Calls f(line, offset) for each line in [b, e) that is not blank, a
/// comment, or a "UCLA ..." header.
template <class F>
void for_lines(std::string_view text, std::size_t b, std::size_t e, F&& f) {
  std::size_t p = b;
  while (p < e) {
    const void* nl = std::memchr(text.data() + p, '\n', e - p);
    const std::size_t q = nl ? static_cast<std::size_t>(static_cast<const char*>(nl) - text.data()) : e;
    std::size_t s = p;
    while (s < q && Tokens::is_space(text[s])) ++s;
    if (s < q && text[s] != '#' && !(q - s >= 4 && text.compare(s, 4, "UCLA") == 0)) {
      f(text.substr(s, q - s), s);
    }
    p = q + 1;
  }
}

std::size_t num_parts(std::size_t bytes) {
  constexpr std::size_t kMinPart = 1 << 18;
  const std::size_t by_size = bytes / kMinPart + 1;
  return std::min<std::size_t>(by_size, static_cast<std::size_t>(par::num_threads()) * 4);
}

template <class T>
std::vector<T> concat(std::vector<std::vector<T>>& parts) {
  std::vector<std::size_t> off(parts.size() + 1, 0);
  for (std::size_t i = 0; i < parts.size(); ++i) off[i + 1] = off[i] + parts[i].size();
  std::vector<T> out(off.back());
  par::parallel_for_each(0, parts.size(), [&](std::size_t i) {
    std::copy(parts[i].begin(), parts[i].end(), out.begin() + static_cast<std::ptrdiff_t>(off[i]));
    std::vector<T>().swap(parts[i]);
  });
  return out;
}

bool is_header_key(std::string_view t) {
  return t == "NumNodes" || t == "NumTerminals" || t == "NumNets" || t == "NumPins" ||
         t == "NumRows";
}

double num(const InputFile& f, std::size_t off, std::string_view t, const char* what) {
  double v = 0;
  if (!parse_double(t, v)) {
    fail_at(f, off, std::string("expected ") + what + ", got '" + std::string(t) + "'");
  }
  return v;
}

Dbu scaled(double v, double scale) { return static_cast<Dbu>(std::llround(v * scale)); }

// Capped warning list shared by the parallel parsers.
class Warnings {
 public:
  explicit Warnings(BookshelfReport& rep) : rep_(rep) {}
  void add(std::string msg) {
    std::lock_guard<std::mutex> lk(mu_);
    ++rep_.num_warnings;
    if (rep_.warnings.size() < 20) {
      HLOG_WARN(lg_bs, "{}", msg);
      rep_.warnings.push_back(std::move(msg));
    }
  }

 private:
  BookshelfReport& rep_;
  std::mutex mu_;
};

// --- .nodes ----------------------------------------------------------------

struct NodeRec {
  std::string_view name;
  double w = 0, h = 0;
  std::uint8_t kind = 0;  // 0 movable, 1 terminal, 2 terminal_NI
};

std::vector<NodeRec> parse_nodes(const InputFile& f) {
  const std::string_view text = f.text();
  const auto cuts = split_lines(text, num_parts(text.size()), 1 << 18);
  std::vector<std::vector<NodeRec>> parts(cuts.size() - 1);
  par::parallel_for_each(0, parts.size(), [&](std::size_t i) {
    auto& out = parts[i];
    out.reserve((cuts[i + 1] - cuts[i]) / 24);
    for_lines(text, cuts[i], cuts[i + 1], [&](std::string_view line, std::size_t off) {
      BsTokens tk(line);
      NodeRec r;
      r.name = tk.next();
      if (is_header_key(r.name)) return;
      r.w = num(f, off, tk.next(), "node width");
      r.h = num(f, off, tk.next(), "node height");
      const std::string_view kind = tk.next();
      if (kind == "terminal") {
        r.kind = 1;
      } else if (kind == "terminal_NI") {
        r.kind = 2;
      } else if (!kind.empty()) {
        fail_at(f, off, "unknown node type '" + std::string(kind) + "'");
      }
      out.push_back(r);
    });
  });
  return concat(parts);
}

// --- .pl -------------------------------------------------------------------

struct PlRec {
  std::string_view name;
  double x = 0, y = 0;
  Orient orient = Orient::N;
  bool fixed = false;
  std::size_t off = 0;
};

std::vector<PlRec> parse_pl(const InputFile& f) {
  const std::string_view text = f.text();
  const auto cuts = split_lines(text, num_parts(text.size()), 1 << 18);
  std::vector<std::vector<PlRec>> parts(cuts.size() - 1);
  par::parallel_for_each(0, parts.size(), [&](std::size_t i) {
    auto& out = parts[i];
    out.reserve((cuts[i + 1] - cuts[i]) / 24);
    for_lines(text, cuts[i], cuts[i + 1], [&](std::string_view line, std::size_t off) {
      BsTokens tk(line);
      PlRec r;
      r.off = off;
      r.name = tk.next();
      r.x = num(f, off, tk.next(), "x");
      r.y = num(f, off, tk.next(), "y");
      for (std::string_view t = tk.next(); !t.empty(); t = tk.next()) {
        if (t == "/FIXED" || t == "/FIXED_NI") {
          r.fixed = true;
        } else if (!parse_orient(t, r.orient)) {
          fail_at(f, off, "bad orientation or flag '" + std::string(t) + "'");
        }
      }
      out.push_back(r);
    });
  });
  return concat(parts);
}

// --- .nets -----------------------------------------------------------------

struct PinRec;
struct NetRec {
  std::string_view name;
  std::uint32_t degree = 0;
  std::size_t off = 0;
  std::size_t first = 0;          ///< Global number of the net's first pin.
  const PinRec* pins = nullptr;   ///< Its pin records (in a parse chunk).
};
struct PinRec {
  std::string_view node;
  double dx = 0, dy = 0;
  PinDir dir = PinDir::Input;
  std::size_t off = 0;
};

bool starts_net(std::string_view line) {
  std::size_t s = 0;
  while (s < line.size() && Tokens::is_space(line[s])) ++s;
  return line.compare(s, 9, "NetDegree") == 0;
}

/// Nets in order; pin records stay in their parse chunks (`pin_store`),
/// reached through NetRec::pins. Returns the total pin count.
std::size_t parse_nets(const InputFile& f, std::vector<NetRec>& nets,
                       std::vector<std::vector<PinRec>>& pin_store) {
  const std::string_view text = f.text();
  const auto cuts = split_lines(text, num_parts(text.size()), 1 << 18, &starts_net);
  const std::size_t np = cuts.size() - 1;
  std::vector<std::vector<NetRec>> net_parts(np);
  std::vector<std::vector<PinRec>> pin_parts(np);
  par::parallel_for_each(0, np, [&](std::size_t i) {
    auto& ns = net_parts[i];
    auto& ps = pin_parts[i];
    ps.reserve((cuts[i + 1] - cuts[i]) / 28);
    std::uint32_t missing = 0;  // Pins still expected for the current net.
    for_lines(text, cuts[i], cuts[i + 1], [&](std::string_view line, std::size_t off) {
      BsTokens tk(line);
      const std::string_view first = tk.next();
      if (first == "NetDegree") {
        if (missing) fail_at(f, ns.back().off, "net has fewer pins than its NetDegree");
        NetRec n;
        n.off = off;
        const double deg = num(f, off, tk.next(), "net degree");
        if (deg < 0 || deg > 1e9) fail_at(f, off, "bad net degree");
        n.degree = static_cast<std::uint32_t>(deg);
        n.name = tk.next();
        n.first = ps.size();  // Local for now.
        missing = n.degree;
        ns.push_back(n);
        return;
      }
      if (is_header_key(first)) return;
      if (ns.empty() || missing == 0) fail_at(f, off, "pin line outside a net: '" + std::string(line) + "'");
      PinRec p;
      p.off = off;
      p.node = first;
      const std::string_view d = tk.next();
      if (d == "I") {
        p.dir = PinDir::Input;
      } else if (d == "O") {
        p.dir = PinDir::Output;
      } else if (d == "B") {
        p.dir = PinDir::Inout;
      } else if (!d.empty()) {
        fail_at(f, off, "bad pin direction '" + std::string(d) + "'");
      }
      const std::string_view tx = tk.next();
      if (!tx.empty()) {
        p.dx = num(f, off, tx, "pin x offset");
        p.dy = num(f, off, tk.next(), "pin y offset");
      }
      ps.push_back(p);
      --missing;
    });
    if (missing) fail_at(f, ns.back().off, "net has fewer pins than its NetDegree");
  });
  std::size_t base = 0;
  for (std::size_t i = 0; i < np; ++i) {
    for (NetRec& n : net_parts[i]) {
      n.pins = pin_parts[i].data() + n.first;
      n.first += base;
    }
    base += pin_parts[i].size();
  }
  nets = concat(net_parts);
  pin_store = std::move(pin_parts);
  return base;
}

// --- .scl ------------------------------------------------------------------

struct SclRow {
  double y = 0, height = 0, site_w = 1, site_step = 1, x = 0, num_sites = 0;
  Orient orient = Orient::N;
};

std::vector<SclRow> parse_scl(const InputFile& f) {
  std::vector<SclRow> rows;
  SclRow cur;
  bool in_row = false;
  for_lines(f.text(), 0, f.size(), [&](std::string_view line, std::size_t off) {
    BsTokens tk(line);
    const std::string_view key = tk.next();
    if (key == "NumRows") return;
    if (key == "CoreRow") {
      if (in_row) fail_at(f, off, "CoreRow inside a CoreRow (missing End)");
      in_row = true;
      cur = SclRow{};
      return;
    }
    if (!in_row) fail_at(f, off, "unexpected '" + std::string(key) + "' outside a CoreRow");
    if (key == "End") {
      in_row = false;
      return;
    }
    if (key == "Coordinate") {
      cur.y = num(f, off, tk.next(), "Coordinate");
    } else if (key == "Height") {
      cur.height = num(f, off, tk.next(), "Height");
    } else if (key == "Sitewidth") {
      cur.site_w = num(f, off, tk.next(), "Sitewidth");
    } else if (key == "Sitespacing") {
      cur.site_step = num(f, off, tk.next(), "Sitespacing");
    } else if (key == "Siteorient") {
      const std::string_view t = tk.next();
      Orient o;
      if (parse_orient(t, o)) cur.orient = o;  // Numeric codes keep N.
    } else if (key == "Sitesymmetry") {
      // Not stored.
    } else if (key == "SubrowOrigin") {
      cur.x = num(f, off, tk.next(), "SubrowOrigin");
      for (std::string_view t = tk.next(); !t.empty(); t = tk.next()) {
        if (t == "NumSites" || t == "Numsites") cur.num_sites = num(f, off, tk.next(), "NumSites");
      }
      rows.push_back(cur);  // One row per subrow.
    } else {
      fail_at(f, off, "unknown .scl keyword '" + std::string(key) + "'");
    }
  });
  if (in_row) throw ParseError(f.path() + ": missing End of the last CoreRow");
  return rows;
}

// --- .route ----------------------------------------------------------------

RouteGrid parse_route(const InputFile& f, const Design& d, double scale, Warnings& warn) {
  RouteGrid g;
  enum class Block { None, NiTerminals, Blockages, Adjust } block = Block::None;
  long remaining = 0;
  auto values = [&](BsTokens& tk, std::size_t off) {
    std::vector<double> v;
    for (std::string_view t = tk.next(); !t.empty(); t = tk.next()) v.push_back(num(f, off, t, "value"));
    return v;
  };
  auto per_layer = [&](std::vector<double> v, std::size_t off, const char* what) {
    if (g.num_layers > 0 && static_cast<long>(v.size()) != g.num_layers) {
      fail_at(f, off, std::string(what) + " needs one value per layer");
    }
    return v;
  };
  for_lines(f.text(), 0, f.size(), [&](std::string_view line, std::size_t off) {
    BsTokens tk(line);
    const std::string_view key = tk.next();
    if (key == "route") return;  // "route 1.0" header
    if (remaining > 0) {
      --remaining;
      if (block == Block::NiTerminals) {
        const Id c = d.find_cell(key);
        if (c == kInvalidId) {
          warn.add(f.path() + ":" + std::to_string(line_of(f, off)) + ": unknown NI terminal '" + std::string(key) + "'");
          return;
        }
        g.pin_layer_cell.push_back(c);
        g.pin_layer.push_back(static_cast<std::int32_t>(num(f, off, tk.next(), "layer")));
      } else if (block == Block::Blockages) {
        const Id c = d.find_cell(key);
        const long n = static_cast<long>(num(f, off, tk.next(), "layer count"));
        std::vector<std::int32_t> layers;
        for (long k = 0; k < n; ++k) layers.push_back(static_cast<std::int32_t>(num(f, off, tk.next(), "layer")));
        if (c == kInvalidId) {
          warn.add(f.path() + ":" + std::to_string(line_of(f, off)) + ": unknown blockage node '" + std::string(key) + "'");
          return;
        }
        g.blockage_cell.push_back(c);
        g.blockage_layers.insert(g.blockage_layers.end(), layers.begin(), layers.end());
        g.blockage_begin.push_back(static_cast<std::uint32_t>(g.blockage_layers.size()));
      } else if (block == Block::Adjust) {
        std::vector<double> v{num(f, off, key, "x")};
        for (std::string_view t = tk.next(); !t.empty(); t = tk.next()) v.push_back(num(f, off, t, "value"));
        if (v.size() != 7) fail_at(f, off, "capacity adjustment needs 7 values");
        for (int k = 0; k < 6; ++k) g.adjust_edges.push_back(static_cast<std::int32_t>(v[k]));
        g.adjust_capacity.push_back(v[6]);
      }
      return;
    }
    if (key == "Grid") {
      const auto v = values(tk, off);
      if (v.size() != 3) fail_at(f, off, "Grid needs x y layers");
      g.nx = static_cast<std::int32_t>(v[0]);
      g.ny = static_cast<std::int32_t>(v[1]);
      g.num_layers = static_cast<std::int32_t>(v[2]);
    } else if (key == "VerticalCapacity") {
      g.vertical_capacity = per_layer(values(tk, off), off, "VerticalCapacity");
    } else if (key == "HorizontalCapacity") {
      g.horizontal_capacity = per_layer(values(tk, off), off, "HorizontalCapacity");
    } else if (key == "MinWireWidth") {
      g.min_wire_width = per_layer(values(tk, off), off, "MinWireWidth");
    } else if (key == "MinWireSpacing") {
      g.min_wire_spacing = per_layer(values(tk, off), off, "MinWireSpacing");
    } else if (key == "ViaSpacing") {
      g.via_spacing = per_layer(values(tk, off), off, "ViaSpacing");
    } else if (key == "GridOrigin") {
      const auto v = values(tk, off);
      if (v.size() != 2) fail_at(f, off, "GridOrigin needs x y");
      g.origin = {scaled(v[0], scale), scaled(v[1], scale)};
    } else if (key == "TileSize") {
      const auto v = values(tk, off);
      if (v.size() != 2) fail_at(f, off, "TileSize needs w h");
      g.tile_w = scaled(v[0], scale);
      g.tile_h = scaled(v[1], scale);
    } else if (key == "BlockagePorosity") {
      g.blockage_porosity = num(f, off, tk.next(), "porosity");
    } else if (key == "NumNiTerminals") {
      block = Block::NiTerminals;
      remaining = static_cast<long>(num(f, off, tk.next(), "count"));
    } else if (key == "NumBlockageNodes") {
      block = Block::Blockages;
      remaining = static_cast<long>(num(f, off, tk.next(), "count"));
    } else if (key == "NumEdgeCapacityAdjustments") {
      block = Block::Adjust;
      remaining = static_cast<long>(num(f, off, tk.next(), "count"));
    } else {
      warn.add(f.path() + ":" + std::to_string(line_of(f, off)) + ": unknown .route keyword '" + std::string(key) + "' skipped");
    }
  });
  if (remaining > 0) throw ParseError(f.path() + ": file ends inside a list");
  return g;
}

}  // namespace

// ---------------------------------------------------------------------------
// .aux
// ---------------------------------------------------------------------------

BookshelfFiles parse_aux(const std::string& aux_path) {
  const InputFile f(aux_path);
  const fs::path dir = fs::path(aux_path).parent_path();
  BookshelfFiles out;
  bool any = false;
  for_lines(f.text(), 0, f.size(), [&](std::string_view line, std::size_t off) {
    BsTokens tk(line);
    tk.next();  // RowBasedPlacement
    for (std::string_view t = tk.next(); !t.empty(); t = tk.next()) {
      const std::string p = (dir / std::string(t)).string();
      const std::string ext = fs::path(std::string(t)).extension().string();
      any = true;
      if (ext == ".nodes") out.nodes = p;
      else if (ext == ".nets") out.nets = p;
      else if (ext == ".pl") out.pl = p;
      else if (ext == ".scl") out.scl = p;
      else if (ext == ".wts") out.wts = p;
      else if (ext == ".route") out.route = p;
      else if (ext == ".shapes") out.shapes = p;
      else HLOG_WARN(lg_bs, "{}:{}: unknown .aux entry '{}' skipped", aux_path, line_of(f, off), t);
    }
  });
  if (!any) throw ParseError(aux_path + ": no files listed");
  // DAC 2012 benchmarks ship a .route next to the .aux without listing it.
  if (out.route.empty() && !out.nodes.empty()) {
    fs::path r = fs::path(out.nodes).replace_extension(".route");
    if (fs::exists(r)) out.route = r.string();
  }
  return out;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

Design read_bookshelf(const BookshelfFiles& files, const Options& opts, BookshelfReport* report) {
  BookshelfReport local;
  BookshelfReport& rep = report ? *report : local;
  rep = {};
  Warnings warn(rep);
  if (files.nodes.empty() || files.nets.empty()) {
    throw ParseError("bookshelf: a .nodes and a .nets file are required");
  }
  const double scale = opts.get_double("bookshelf.scale", 1.0);
  if (!(scale > 0)) throw std::invalid_argument("bookshelf.scale must be > 0");
  PhaseTimer pt(lg_bs, files.nodes);

  // Map the files; parse them concurrently (each parser is parallel too).
  InputFile f_nodes(files.nodes), f_nets(files.nets);
  InputFile f_pl, f_scl, f_wts, f_route;
  if (!files.pl.empty()) f_pl = InputFile(files.pl);
  if (!files.scl.empty()) f_scl = InputFile(files.scl);
  if (!files.wts.empty()) f_wts = InputFile(files.wts);
  if (!files.route.empty()) f_route = InputFile(files.route);
  if (!files.shapes.empty()) warn.add(files.shapes + ": .shapes (non-rectangular nodes) are not read yet; nodes use their bounding boxes");

  std::vector<NodeRec> nodes;
  std::vector<PlRec> pl;
  std::vector<NetRec> nets;
  std::vector<std::vector<PinRec>> pin_store;
  std::size_t num_pin_recs = 0;
  std::vector<SclRow> scl;
  par::parallel_invoke([&] { nodes = parse_nodes(f_nodes); },
                       [&] { num_pin_recs = parse_nets(f_nets, nets, pin_store); },
                       [&] { if (f_pl.data()) pl = parse_pl(f_pl); },
                       [&] { if (f_scl.data()) scl = parse_scl(f_scl); });

  pt.lap("parse files");
  const std::string name = fs::path(files.nodes).stem().string();
  Design d(name);

  // Rows and row height first: they decide which nodes are macros.
  std::vector<Row> rows;
  rows.reserve(scl.size());
  Rect core{std::numeric_limits<Dbu>::max(), std::numeric_limits<Dbu>::max(),
            std::numeric_limits<Dbu>::min(), std::numeric_limits<Dbu>::min()};
  for (const SclRow& s : scl) {
    Row r;
    r.origin = {scaled(s.x, scale), scaled(s.y, scale)};
    r.site_width = scaled(s.site_w, scale);
    r.site_step = scaled(s.site_step, scale);
    r.site_height = scaled(s.height, scale);
    r.num_sites = static_cast<std::int32_t>(s.num_sites);
    r.orient = s.orient;
    rows.push_back(r);
    const Rect b = r.bbox();
    core = {std::min(core.xlo, b.xlo), std::min(core.ylo, b.ylo), std::max(core.xhi, b.xhi),
            std::max(core.yhi, b.yhi)};
  }
  rep.num_rows = rows.size();
  d.set_rows(std::move(rows));  // Sets the most common row height.
  const double row_h = scale > 0 ? static_cast<double>(d.row_height()) / scale : 0.0;

  pt.lap("rows");
  // --- Cells ---------------------------------------------------------------
  const std::size_t nc = nodes.size();
  {
    std::vector<std::string_view> names(nc);
    par::parallel_for(0, nc, 16384, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) names[i] = nodes[i].name;
    });
    try {
      DA::cell_names(d).assign(names);
    } catch (const std::invalid_argument& e) {
      throw ParseError(files.nodes + ": " + e.what());
    }
  }
  pt.lap("cell names");
  // Placement records per cell (parallel name lookups).
  std::vector<Id> pl_cell(pl.size());
  std::atomic<std::size_t> bad_pl{0};
  par::parallel_for(0, pl.size(), 8192, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      pl_cell[i] = d.find_cell(pl[i].name);
      if (pl_cell[i] == kInvalidId) bad_pl.fetch_add(1, std::memory_order_relaxed);
    }
  });
  std::vector<std::int32_t> cell_pl(nc, -1);
  for (std::size_t i = 0; i < pl.size(); ++i) {
    if (pl_cell[i] == kInvalidId) {
      warn.add(f_pl.path() + ":" + std::to_string(line_of(f_pl, pl[i].off)) + ": unknown node '" +
               std::string(pl[i].name) + "'");
      continue;
    }
    cell_pl[static_cast<std::size_t>(pl_cell[i])] = static_cast<std::int32_t>(i);
  }
  bool pl_placed = false;
  if (f_pl.data()) {
    const std::string_view head = f_pl.text().substr(0, 4096);
    pl_placed = head.find("# hermes: placed") != std::string_view::npos;
  }
  pl_placed = opts.get_bool("bookshelf.pl_placed", pl_placed);
  rep.pl_placed = pl_placed;

  pt.lap("pl lookup");
  std::vector<Cell>& cells = DA::cells(d);
  cells.resize(nc);
  std::atomic<std::size_t> n_term{0}, n_macro{0}, n_fixed{0}, n_nopl{0};
  par::parallel_for(0, nc, 8192, [&](std::size_t lo, std::size_t hi) {
    std::size_t t = 0, m = 0, fx = 0, np = 0;
    for (std::size_t i = lo; i < hi; ++i) {
      const NodeRec& nr = nodes[i];
      Cell c;
      const std::int32_t k = cell_pl[i];
      const Orient o = k >= 0 ? pl[static_cast<std::size_t>(k)].orient : Orient::N;
      const Dbu w0 = scaled(nr.w, scale), h0 = scaled(nr.h, scale);
      const bool rot = swaps_xy(o);
      c.width = rot ? h0 : w0;
      c.height = rot ? w0 : h0;
      c.orient = o;
      c.fixed = nr.kind != 0 || (k >= 0 && pl[static_cast<std::size_t>(k)].fixed);
      c.is_macro = row_h > 0 && nr.h > row_h * 1.0001;
      if (k >= 0) {
        c.loc = {scaled(pl[static_cast<std::size_t>(k)].x, scale), scaled(pl[static_cast<std::size_t>(k)].y, scale)};
        c.placed = c.fixed || pl_placed;
      } else {
        ++np;
      }
      t += nr.kind != 0;
      m += c.is_macro;
      fx += c.fixed;
      cells[i] = c;
    }
    n_term += t;
    n_macro += m;
    n_fixed += fx;
    n_nopl += np;
  });
  rep.num_nodes = nc;
  rep.num_terminals = n_term;
  rep.num_macros = n_macro;
  rep.num_fixed = n_fixed;
  if (f_pl.data() && n_nopl > 0) warn.add(std::to_string(n_nopl.load()) + " nodes have no .pl entry; they start at (0, 0), unplaced");

  pt.lap("cells");
  // --- Nets and pins -------------------------------------------------------
  const std::size_t nn = nets.size(), np = num_pin_recs;
  std::deque<std::string> generated;  // Names for nets without one (stable storage).
  {
    std::vector<std::string_view> names(nn);
    for (std::size_t i = 0; i < nn; ++i) {
      if (nets[i].name.empty()) {
        generated.push_back("net_" + std::to_string(i));
        names[i] = generated.back();
      } else {
        names[i] = nets[i].name;
      }
    }
    try {
      DA::net_names(d).assign(names);
    } catch (const std::invalid_argument& e) {
      throw ParseError(files.nets + ": " + e.what());
    }
  }
  pt.lap("net names");
  std::vector<std::uint32_t> degree(nn);
  std::vector<Net>& dnets = DA::nets(d);
  std::vector<Pin>& dpins = DA::pins(d);
  par::parallel_invoke([&] { dnets.assign(nn, Net{}); }, [&] { dpins.resize(np); });
  for (std::size_t i = 0; i < nn; ++i) degree[i] = nets[i].degree;
  pt.lap("pin arrays");
  std::atomic<std::size_t> first_bad{std::numeric_limits<std::size_t>::max()};
  par::parallel_for(0, nn, 2048, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t n = lo; n < hi; ++n) {
      const NetRec& net = nets[n];
      for (std::uint32_t k = 0; k < net.degree; ++k) {
        const std::size_t i = net.first + k;
        const PinRec& pr = net.pins[k];
        const Id c = d.find_cell(pr.node);
        Pin& p = dpins[i];
        p.net = static_cast<Id>(n);
        p.cell = c;
        p.dir = pr.dir;
        if (c == kInvalidId) {
          std::size_t cur = first_bad.load();
          while (n < cur && !first_bad.compare_exchange_weak(cur, n)) {}
          continue;
        }
        const NodeRec& nr = nodes[static_cast<std::size_t>(c)];
        const Cell& cell = cells[static_cast<std::size_t>(c)];
        const Dbu w0 = scaled(nr.w, scale), h0 = scaled(nr.h, scale);
        const Point raw{static_cast<Dbu>(std::llround((0.5 * nr.w + pr.dx) * scale)),
                        static_cast<Dbu>(std::llround((0.5 * nr.h + pr.dy) * scale))};
        p.offset = orient_offset(raw, w0, h0, cell.orient);
      }
    }
  });
  if (first_bad.load() != std::numeric_limits<std::size_t>::max()) {
    const NetRec& net = nets[first_bad.load()];
    for (std::uint32_t k = 0; k < net.degree; ++k) {
      if (dpins[net.first + k].cell == kInvalidId) {
        fail_at(f_nets, net.pins[k].off, "unknown node '" + std::string(net.pins[k].node) + "'");
      }
    }
  }
  pt.lap("pins");
  {
    std::vector<Id> data(np);
    par::parallel_for(0, np, 65536, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) data[i] = static_cast<Id>(i);
    });
    DA::net_pins(d).assign(std::move(data), degree);
  }
  pt.lap("net lists");
  // Cell -> pins (ascending pin ids) by counting sort; pin names p0, p1, ...
  // by position on the cell.
  {
    std::vector<std::uint32_t> count(nc, 0);
    for (const Pin& p : dpins) ++count[static_cast<std::size_t>(p.cell)];
    std::vector<std::uint32_t> start(nc + 1, 0);
    std::uint32_t max_pins = 0;
    for (std::size_t c = 0; c < nc; ++c) {
      start[c + 1] = start[c] + count[c];
      max_pins = std::max(max_pins, count[c]);
    }
    std::vector<Id> data(np);
    std::vector<Id>& name_ids = DA::pin_name_ids(d);
    name_ids.resize(np);
    std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
    for (std::size_t i = 0; i < np; ++i) {
      const std::size_t c = static_cast<std::size_t>(dpins[i].cell);
      name_ids[i] = static_cast<Id>(fill[c] - start[c]);
      data[fill[c]++] = static_cast<Id>(i);
    }
    DA::cell_pins(d).assign(std::move(data), count);
    std::vector<std::string> pnames(max_pins);
    std::vector<std::string_view> views(max_pins);
    for (std::uint32_t k = 0; k < max_pins; ++k) {
      pnames[k] = "p" + std::to_string(k);
      views[k] = pnames[k];
    }
    DA::pin_names(d).assign(views);
  }
  pt.lap("cell lists");
  rep.num_nets = nn;
  rep.num_pins = np;
  DA::finish(d);

  // --- Floorplan, weights, routing -----------------------------------------
  d.set_dbu_per_micron(scale);
  if (core.xlo <= core.xhi && !scl.empty()) {
    d.set_die(core);
  } else {
    Rect r{std::numeric_limits<Dbu>::max(), std::numeric_limits<Dbu>::max(),
           std::numeric_limits<Dbu>::min(), std::numeric_limits<Dbu>::min()};
    for (const Cell& c : d.cells()) {
      const Rect b = c.bbox();
      r = {std::min(r.xlo, b.xlo), std::min(r.ylo, b.ylo), std::max(r.xhi, b.xhi), std::max(r.yhi, b.yhi)};
    }
    if (nc) d.set_die(r);
    warn.add("no .scl rows: die is the bounding box of all nodes");
  }

  if (f_wts.data()) {
    std::size_t node_weights = 0;
    for_lines(f_wts.text(), 0, f_wts.size(), [&](std::string_view line, std::size_t off) {
      BsTokens tk(line);
      const std::string_view n = tk.next();
      const std::string_view w = tk.next();
      if (w.empty()) return;
      const double v = num(f_wts, off, w, "weight");
      const Id net = d.find_net(n);
      if (net != kInvalidId) {
        if (v != 1.0) ++rep.num_weighted_nets;
        d.set_net_weight(net, v);
      } else if (d.find_cell(n) != kInvalidId) {
        ++node_weights;
      } else {
        warn.add(f_wts.path() + ":" + std::to_string(line_of(f_wts, off)) + ": unknown name '" + std::string(n) + "'");
      }
    });
    if (node_weights) HLOG_VERBOSE(lg_bs, "{}: {} node weights ignored", f_wts.path(), node_weights);
  }
  pt.lap("weights");
  if (f_route.data()) {
    d.set_route_grid(parse_route(f_route, d, scale, warn));
    rep.route = true;
  }
  d.flush();
  HLOG_INFO(lg_bs, "read Bookshelf {}: {} nodes ({} terminals, {} macros, {} fixed), {} nets, {} pins, "
            "{} rows{}{}, {} warnings",
            name, rep.num_nodes, rep.num_terminals, rep.num_macros, rep.num_fixed, rep.num_nets,
            rep.num_pins, rep.num_rows, rep.route ? ", routing grid" : "",
            rep.pl_placed ? ", .pl placed" : "", rep.num_warnings);
  return d;
}

Design read_bookshelf_aux(const std::string& aux_path, const Options& opts, BookshelfReport* report) {
  return read_bookshelf(parse_aux(aux_path), opts, report);
}

// ---------------------------------------------------------------------------
// .pl writer
// ---------------------------------------------------------------------------

void write_bookshelf_pl(const Design& d, const std::string& path, double scale, bool placed_marker) {
  if (!(scale > 0)) scale = 1.0;
  const std::size_t n = d.num_cells();
  const std::size_t nb = std::max<std::size_t>(1, std::min<std::size_t>(par::block_count(n, 16384), 256));
  std::vector<std::string> parts(nb + 1);
  std::string& head = parts[0];
  head = "UCLA pl 1.0\n# Written by hermes for design ";
  head += d.name();
  head += '\n';
  if (placed_marker) head += "# hermes: placed\n";
  head += '\n';
  const bool integral = scale == 1.0;
  auto coord = [&](std::string& out, Dbu v) {
    if (integral) {
      put_int(out, v);
    } else {
      put_double(out, static_cast<double>(v) / scale);
    }
  };
  const std::size_t bs = (n + nb - 1) / nb;
  par::parallel_for_each(0, nb, [&](std::size_t b) {
    std::string& out = parts[b + 1];
    const std::size_t lo = b * bs, hi = std::min(n, lo + bs);
    if (lo >= hi) return;
    out.reserve((hi - lo) * 32);
    for (std::size_t i = lo; i < hi; ++i) {
      const Cell& c = d.cells()[i];
      if (c.removed) continue;
      out += d.cell_name(static_cast<Id>(i));
      out += '\t';
      coord(out, c.loc.x);
      out += '\t';
      coord(out, c.loc.y);
      out += "\t: ";
      out += orient_name(c.orient);
      if (c.fixed) out += " /FIXED";
      out += '\n';
    }
  });
  write_parts(path, parts);
  HLOG_INFO(lg_bs, "wrote {} ({} cells)", path, n);
}

}  // namespace hermes::io

// ---------------------------------------------------------------------------
// Full Bookshelf writer
// ---------------------------------------------------------------------------

namespace hermes::io {

namespace {

/// Unrotated, lower-left-relative pin offset for an oriented offset `p` of
/// a cell whose unrotated size is (w0, h0): inverse of orient_offset.
Point unorient_offset(Point p, Dbu w0, Dbu h0, Orient o) {
  const Point t = orient_offset({0, 0}, w0, h0, o);
  const Point ax = orient_offset({1, 0}, w0, h0, o) - t;
  const Point ay = orient_offset({0, 1}, w0, h0, o) - t;
  const Point q = p - t;
  // A = [ax ay] is a signed permutation: A^-1 = A^T.
  return {ax.x * q.x + ax.y * q.y, ay.x * q.x + ay.y * q.y};
}

template <class F>
std::vector<std::string> format_blocks(std::size_t n, std::size_t per_item, F&& f) {
  const std::size_t nb = std::max<std::size_t>(1, std::min<std::size_t>(par::block_count(n, 16384), 256));
  const std::size_t bs = (n + nb - 1) / nb;
  std::vector<std::string> parts(nb);
  par::parallel_for_each(0, nb, [&](std::size_t b) {
    const std::size_t lo = b * bs, hi = std::min(n, lo + bs);
    if (lo >= hi) return;
    parts[b].reserve((hi - lo) * per_item);
    for (std::size_t i = lo; i < hi; ++i) f(parts[b], i);
  });
  return parts;
}

}  // namespace

std::string write_bookshelf(const Design& d, const std::string& dir, std::string base, double scale) {
  if (!(scale > 0)) scale = 1.0;
  if (base.empty()) base = d.name();
  fs::create_directories(dir);
  const fs::path stem = fs::path(dir) / base;
  auto val = [&](std::string& out, double v) { put_double(out, v / scale); };

  std::size_t live_cells = 0, terminals = 0;
  for (const Cell& c : d.cells()) {
    if (c.removed) continue;
    ++live_cells;
    terminals += c.fixed;
  }
  // .nodes
  {
    std::vector<std::string> parts = format_blocks(d.num_cells(), 32, [&](std::string& out, std::size_t i) {
      const Cell& c = d.cells()[i];
      if (c.removed) return;
      const bool rot = swaps_xy(c.orient);
      out += '\t';
      out += d.cell_name(static_cast<Id>(i));
      out += '\t';
      val(out, static_cast<double>(rot ? c.height : c.width));
      out += '\t';
      val(out, static_cast<double>(rot ? c.width : c.height));
      if (c.fixed) out += "\tterminal";
      out += '\n';
    });
    std::string head = "UCLA nodes 1.0\n# Written by hermes\n\nNumNodes : " + std::to_string(live_cells) +
                       "\nNumTerminals : " + std::to_string(terminals) + "\n";
    parts.insert(parts.begin(), std::move(head));
    write_parts(stem.string() + ".nodes", parts);
  }
  // .nets (nets without cell pins are skipped: Bookshelf has no I/O pins)
  {
    std::size_t live_nets = 0, live_pins = 0;
    std::vector<std::uint32_t> deg(d.num_nets(), 0);
    for (std::size_t n = 0; n < d.num_nets(); ++n) {
      if (d.nets()[n].removed) continue;
      for (Id p : d.net_pins(static_cast<Id>(n))) deg[n] += d.pin(p).cell != kInvalidId;
      if (deg[n]) {
        ++live_nets;
        live_pins += deg[n];
      }
    }
    std::size_t io = 0;
    for (Id p : d.io_pins()) io += d.pin(p).net != kInvalidId;
    if (io) HLOG_WARN(lg_bs, "write_bookshelf: {} top-level I/O pins are not written (Bookshelf has none)", io);
    std::vector<std::string> parts = format_blocks(d.num_nets(), 96, [&](std::string& out, std::size_t n) {
      if (!deg[n]) return;
      out += "NetDegree : ";
      put_int(out, deg[n]);
      out += "   ";
      out += d.net_name(static_cast<Id>(n));
      out += '\n';
      for (Id pid : d.net_pins(static_cast<Id>(n))) {
        const Pin& p = d.pin(pid);
        if (p.cell == kInvalidId) continue;
        const Cell& c = d.cell(p.cell);
        const bool rot = swaps_xy(c.orient);
        const Dbu w0 = rot ? c.height : c.width, h0 = rot ? c.width : c.height;
        const Point u = unorient_offset(p.offset, w0, h0, c.orient);
        out += '\t';
        out += d.cell_name(p.cell);
        out += p.dir == PinDir::Output ? " O : " : p.dir == PinDir::Inout ? " B : " : " I : ";
        val(out, static_cast<double>(u.x) - 0.5 * static_cast<double>(w0));
        out += ' ';
        val(out, static_cast<double>(u.y) - 0.5 * static_cast<double>(h0));
        out += '\n';
      }
    });
    std::string head = "UCLA nets 1.0\n# Written by hermes\n\nNumNets : " + std::to_string(live_nets) +
                       "\nNumPins : " + std::to_string(live_pins) + "\n\n";
    parts.insert(parts.begin(), std::move(head));
    write_parts(stem.string() + ".nets", parts);
  }
  // .wts (only weights != 1)
  {
    std::string out = "UCLA wts 1.0\n# Written by hermes\n\n";
    for (std::size_t n = 0; n < d.num_nets(); ++n) {
      const Net& net = d.nets()[n];
      if (net.removed || net.weight == 1.0) continue;
      out += d.net_name(static_cast<Id>(n));
      out += '\t';
      put_double(out, net.weight);
      out += '\n';
    }
    write_parts(stem.string() + ".wts", {out});
  }
  // .scl
  {
    std::string out = "UCLA scl 1.0\n# Written by hermes\n\nNumRows : " + std::to_string(d.rows().size()) + "\n\n";
    for (const Row& r : d.rows()) {
      out += "CoreRow Horizontal\n  Coordinate    :   ";
      val(out, static_cast<double>(r.origin.y));
      out += "\n  Height        :   ";
      val(out, static_cast<double>(r.site_height));
      out += "\n  Sitewidth     :   ";
      val(out, static_cast<double>(r.site_width));
      out += "\n  Sitespacing   :   ";
      val(out, static_cast<double>(r.site_step));
      out += "\n  Siteorient    :   ";
      out += orient_name(r.orient);
      out += "\n  Sitesymmetry  :   Y\n  SubrowOrigin  :   ";
      val(out, static_cast<double>(r.origin.x));
      out += "\tNumSites  :  ";
      put_int(out, r.num_sites);
      out += "\nEnd\n";
    }
    write_parts(stem.string() + ".scl", {out});
  }
  write_bookshelf_pl(d, stem.string() + ".pl", scale, true);
  const std::string aux = stem.string() + ".aux";
  write_parts(aux, {"RowBasedPlacement : " + base + ".nodes " + base + ".nets " + base + ".wts " + base +
                        ".pl " + base + ".scl\n"});
  HLOG_INFO(lg_bs, "wrote Bookshelf {} ({} nodes)", aux, live_cells);
  return aux;
}

}  // namespace hermes::io
