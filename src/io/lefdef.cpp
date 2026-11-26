#include "hermes/io/lefdef.hpp"


#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

#include "hermes/db/design_access.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"

namespace hermes::io {

namespace {
HERMES_LOG_CHANNEL(lg_lef, "io.lef");
HERMES_LOG_CHANNEL(lg_def, "io.def");
}  // namespace

// ---------------------------------------------------------------------------
// Library containers
// ---------------------------------------------------------------------------

const LefPin* LefMacro::find_pin(std::string_view pin) const {
  const auto it = pin_index_.find(pin);
  return it == pin_index_.end() ? nullptr : &pins_[it->second];
}

void LefMacro::add_pin(LefPin p) {
  const auto it = pin_index_.find(p.name);
  if (it != pin_index_.end()) {
    pins_[it->second] = std::move(p);
    return;
  }
  pin_index_.emplace(p.name, pins_.size());
  pins_.push_back(std::move(p));
}

const LefMacro* LefLibrary::find_macro(std::string_view name) const {
  const auto it = macros_.find(name);
  return it == macros_.end() ? nullptr : &it->second;
}

const LefSite* LefLibrary::find_site(std::string_view name) const {
  const auto it = sites_.find(name);
  return it == sites_.end() ? nullptr : &it->second;
}

LefMacro& LefLibrary::add_macro(LefMacro m) {
  std::string key = m.name;
  return macros_.insert_or_assign(std::move(key), std::move(m)).first->second;
}

void LefLibrary::add_routing_layer(LefLayer l) {
  for (LefLayer& o : routing_layers) {
    if (o.name == l.name) {
      o = std::move(l);
      return;
    }
  }
  routing_layers.push_back(std::move(l));
}

LefSite& LefLibrary::add_site(LefSite s) {
  std::string key = s.name;
  return sites_.insert_or_assign(std::move(key), std::move(s)).first->second;
}

// Pure rotation/mirror about the origin (no translation). Used for DEF I/O
// pin shapes, which are given relative to the pin's placement point.
static Point orient_about_origin(Point p, Orient o) {
  switch (o) {
    case Orient::N:  return {p.x, p.y};
    case Orient::S:  return {-p.x, -p.y};
    case Orient::W:  return {-p.y, p.x};
    case Orient::E:  return {p.y, -p.x};
    case Orient::FN: return {-p.x, p.y};
    case Orient::FS: return {p.x, -p.y};
    case Orient::FW: return {-p.y, -p.x};
    case Orient::FE: return {p.y, p.x};
  }
  return p;
}

// ---------------------------------------------------------------------------
// Lexer shared by both readers
// ---------------------------------------------------------------------------

namespace {

class Lexer {
 public:
  Lexer(std::string_view text, std::string_view source, std::size_t line0 = 1)
      : text_(text), source_(source), line_(line0), tok_line_(line0) {}

  std::string_view text() const { return text_; }
  /// Byte offset of the next unread character (no token may be pending).
  std::size_t offset() const { return pos_; }
  std::size_t cur_line() const { return line_; }
  void seek(std::size_t pos, std::size_t line) {
    pos_ = pos;
    line_ = tok_line_ = line;
    pending_ = {};
  }

  /// Next token, or an empty view at end of input. Tokens are separated by
  /// whitespace; `#` starts a comment; "..." is one token (quotes kept).
  /// A trailing ';' glued to a word is split off as its own token.
  std::string_view next() {
    if (!pending_.empty()) {
      const std::string_view t = pending_;
      pending_ = {};
      return t;
    }
    skip_space_and_comments();
    if (pos_ >= text_.size()) return {};
    tok_line_ = line_;
    const std::size_t start = pos_;
    if (text_[pos_] == '"') {
      ++pos_;
      while (pos_ < text_.size() && text_[pos_] != '"') {
        if (text_[pos_] == '\n') ++line_;
        ++pos_;
      }
      if (pos_ < text_.size()) ++pos_;
      return text_.substr(start, pos_ - start);
    }
    while (pos_ < text_.size() && !is_space(text_[pos_])) ++pos_;
    std::string_view t = text_.substr(start, pos_ - start);
    if (t.size() > 1 && t.back() == ';') {
      pending_ = t.substr(t.size() - 1);
      t.remove_suffix(1);
    }
    return t;
  }

  std::string_view peek() {
    if (!pending_.empty()) return pending_;
    const std::size_t p = pos_, l = line_, tl = tok_line_;
    const std::string_view t = next();
    // Re-lexing is cheap; restore state but keep any split ';' consistent.
    pos_ = p;
    line_ = l;
    tok_line_ = tl;
    pending_ = {};
    return t;
  }

  std::string_view expect_any(const char* what) {
    const std::string_view t = next();
    if (t.empty()) fail(std::string("unexpected end of file, expected ") + what);
    return t;
  }

  void expect(std::string_view want) {
    const std::string_view t = next();
    if (t != want) {
      fail("expected '" + std::string(want) + "', got '" + std::string(t) + "'");
    }
  }

  double number(const char* what) {
    const std::string_view t = expect_any(what);
    double v = 0.0;
    const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (ec != std::errc() || p != t.data() + t.size()) {
      fail(std::string("expected ") + what + ", got '" + std::string(t) + "'");
    }
    return v;
  }

  /// Skip tokens up to and including the next ';'.
  void skip_statement() {
    for (std::string_view t = next(); t != ";"; t = next()) {
      if (t.empty()) fail("unexpected end of file inside a statement");
    }
  }

  /// Skip tokens up to and including `END <name>` (name may be empty: `END`
  /// followed by anything is not enough, so callers pass the block name).
  void skip_block(std::string_view name) {
    for (;;) {
      const std::string_view t = next();
      if (t.empty()) fail("unexpected end of file, missing 'END " + std::string(name) + "'");
      if (t == "END" && peek() == name) {
        next();
        return;
      }
    }
  }

  [[noreturn]] void fail(const std::string& msg) const {
    std::ostringstream os;
    os << source_ << ':' << tok_line_ << ": " << msg;
    throw ParseError(os.str());
  }

  std::size_t line() const { return tok_line_; }
  std::string_view source() const { return source_; }

 private:
  static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  }

  void skip_space_and_comments() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '\n') {
        ++line_;
        ++pos_;
      } else if (is_space(c)) {
        ++pos_;
      } else if (c == '#') {
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
      } else {
        break;
      }
    }
  }

  std::string_view text_;
  std::string_view source_;
  std::size_t pos_ = 0;
  std::size_t line_ = 1;
  std::size_t tok_line_ = 1;
  std::string_view pending_;
};

Dbu to_dbu(double v) { return static_cast<Dbu>(std::llround(v)); }

PinDir parse_lef_dir(std::string_view s) {
  if (s == "OUTPUT") return PinDir::Output;
  if (s == "INOUT" || s == "FEEDTHRU") return PinDir::Inout;
  return PinDir::Input;
}

PinUse parse_use(std::string_view s) {
  if (s == "SIGNAL") return PinUse::Signal;
  if (s == "CLOCK") return PinUse::Clock;
  if (s == "POWER") return PinUse::Power;
  if (s == "GROUND") return PinUse::Ground;
  if (s == "ANALOG") return PinUse::Analog;
  return PinUse::Other;
}

}  // namespace

// ---------------------------------------------------------------------------
// LEF
// ---------------------------------------------------------------------------

class LefParser {
 public:
  LefParser(std::string_view text, std::string_view source, LefLibrary& lib)
      : lx_(text, source), lib_(lib) {}

  void parse() {
    for (std::string_view t = lx_.next(); !t.empty(); t = lx_.next()) {
      if (t == "END") {
        const std::string_view what = lx_.next();
        if (what == "LIBRARY" || what.empty()) return;
        lx_.fail("unexpected 'END " + std::string(what) + "'");
      } else if (t == "UNITS") {
        parse_units();
      } else if (t == "SITE") {
        parse_site();
      } else if (t == "MACRO") {
        parse_macro();
      } else if (t == "LAYER") {
        parse_layer();
      } else if (t == "VIA" || t == "VIARULE" ||
                 t == "NONDEFAULTRULE") {
        const std::string name(lx_.expect_any("name"));
        // A VIA/VIARULE header line may carry extra words (e.g. DEFAULT,
        // GENERATE); skip_block scans for `END <name>` regardless.
        lx_.skip_block(name);
      } else if (t == "SPACING" || t == "PROPERTYDEFINITIONS" || t == "ARRAY" ||
                 t == "IRDROP" || t == "NOISETABLE" || t == "CORRECTIONTABLE" ||
                 t == "MAXVIASTACK" || t == "BEGINEXT") {
        if (t == "MAXVIASTACK") {
          lx_.skip_statement();
        } else if (t == "BEGINEXT") {
          while (lx_.expect_any("ENDEXT") != "ENDEXT") {}
        } else if (t == "ARRAY") {
          const std::string name(lx_.expect_any("name"));
          lx_.skip_block(name);
        } else {
          lx_.skip_block(t);
        }
      } else {
        // VERSION, BUSBITCHARS, DIVIDERCHAR, MANUFACTURINGGRID, CLEARANCEMEASURE,
        // USEMINSPACING, NAMESCASESENSITIVE, ...: single statements.
        lx_.skip_statement();
      }
    }
  }

 private:
  void parse_units() {
    for (;;) {
      const std::string_view t = lx_.expect_any("END UNITS");
      if (t == "END") {
        lx_.expect("UNITS");
        return;
      }
      if (t == "DATABASE") {
        lx_.expect("MICRONS");
        lib_.database_microns = lx_.number("DATABASE MICRONS value");
        lx_.expect(";");
      } else if (t != ";") {
        lx_.skip_statement();
      }
    }
  }

  // LAYER name ... END name. Keeps TYPE ROUTING layers (direction, pitch,
  // width, spacing); other layer types and statements are skipped.
  void parse_layer() {
    LefLayer l;
    l.name = std::string(lx_.expect_any("layer name"));
    bool routing = false;
    for (;;) {
      const std::string_view t = lx_.expect_any("END");
      if (t == "END" && lx_.peek() == l.name) {
        lx_.next();
        break;
      }
      if (t == "TYPE") {
        routing = lx_.expect_any("layer type") == "ROUTING";
        lx_.skip_statement();
      } else if (t == "DIRECTION") {
        l.horizontal = lx_.expect_any("direction") == "HORIZONTAL";
        lx_.skip_statement();
      } else if (t == "PITCH" || t == "WIDTH") {
        const double v = lx_.number("value");
        (t == "PITCH" ? l.pitch : l.width) = v;
        if (lx_.peek() != ";") lx_.skip_statement(); else lx_.next();
      } else if (t == "SPACING") {
        const double v = lx_.number("spacing");
        if (l.spacing == 0.0) l.spacing = v;
        if (lx_.peek() != ";") lx_.skip_statement(); else lx_.next();
      } else if (t != ";") {
        lx_.skip_statement();
      }
    }
    if (routing) lib_.add_routing_layer(std::move(l));
  }

  void parse_site() {
    LefSite s;
    s.name = std::string(lx_.expect_any("site name"));
    for (;;) {
      const std::string_view t = lx_.expect_any("END");
      if (t == "END") {
        lx_.expect(s.name);
        break;
      }
      if (t == "SIZE") {
        s.width = lx_.number("SITE width");
        lx_.expect("BY");
        s.height = lx_.number("SITE height");
        lx_.expect(";");
      } else {
        lx_.skip_statement();
      }
    }
    lib_.add_site(std::move(s));
  }

  void parse_macro() {
    LefMacro m;
    m.name = std::string(lx_.expect_any("macro name"));
    double ox = 0.0, oy = 0.0;
    std::vector<LefPin> pins;
    for (;;) {
      const std::string_view t = lx_.expect_any("END");
      if (t == "END") {
        lx_.expect(m.name);
        break;
      }
      if (t == "CLASS") {
        std::string cls;
        for (std::string_view w = lx_.expect_any(";"); w != ";"; w = lx_.expect_any(";")) {
          if (!cls.empty()) cls += ' ';
          cls += w;
        }
        m.cls = std::move(cls);
      } else if (t == "SIZE") {
        m.width = lx_.number("MACRO width");
        lx_.expect("BY");
        m.height = lx_.number("MACRO height");
        lx_.expect(";");
      } else if (t == "ORIGIN") {
        ox = lx_.number("ORIGIN x");
        oy = lx_.number("ORIGIN y");
        lx_.expect(";");
      } else if (t == "PIN") {
        pins.push_back(parse_pin());
      } else if (t == "OBS") {
        skip_until_end();
      } else {
        lx_.skip_statement();
      }
    }
    // Shapes are in the macro's coordinate system; the macro's lower-left
    // corner sits at -ORIGIN in that system.
    for (LefPin& p : pins) {
      p.x += ox;
      p.y += oy;
      m.add_pin(std::move(p));
    }
    lib_.add_macro(std::move(m));
  }

  LefPin parse_pin() {
    LefPin p;
    p.name = std::string(lx_.expect_any("pin name"));
    double xlo = std::numeric_limits<double>::max(), ylo = xlo;
    double xhi = std::numeric_limits<double>::lowest(), yhi = xhi;
    auto add_pt = [&](double x, double y) {
      xlo = std::min(xlo, x);
      ylo = std::min(ylo, y);
      xhi = std::max(xhi, x);
      yhi = std::max(yhi, y);
      p.has_shape = true;
    };
    for (;;) {
      const std::string_view t = lx_.expect_any("END");
      if (t == "END") {
        lx_.expect(p.name);
        break;
      }
      if (t == "DIRECTION") {
        p.dir = parse_lef_dir(lx_.expect_any("direction"));
        lx_.skip_statement();  // OUTPUT TRISTATE ;
      } else if (t == "USE") {
        p.use = parse_use(lx_.expect_any("use"));
        lx_.skip_statement();
      } else if (t == "PORT") {
        for (;;) {
          const std::string_view s = lx_.expect_any("END");
          if (s == "END") break;
          if (s == "RECT" || s == "POLYGON") {
            std::string_view w = lx_.peek();
            if (w == "MASK") {
              lx_.next();
              lx_.next();
            }
            if (lx_.peek() == "ITERATE") lx_.fail("ITERATE shapes are not supported");
            std::vector<double> v;
            for (w = lx_.peek(); w != ";"; w = lx_.peek()) v.push_back(lx_.number("coordinate"));
            lx_.next();
            if (v.size() < 4 || v.size() % 2) lx_.fail("bad " + std::string(s) + " coordinates");
            for (std::size_t i = 0; i + 1 < v.size(); i += 2) add_pt(v[i], v[i + 1]);
          } else {
            lx_.skip_statement();  // LAYER, WIDTH, PATH, VIA, CLASS ...
          }
        }
      } else {
        lx_.skip_statement();
      }
    }
    if (p.has_shape) {
      p.x = 0.5 * (xlo + xhi);
      p.y = 0.5 * (ylo + yhi);
    }
    return p;
  }

  // OBS ... END (no name after END).
  void skip_until_end() {
    for (std::string_view t = lx_.expect_any("END"); t != "END"; t = lx_.expect_any("END")) {}
  }

  Lexer lx_;
  LefLibrary& lib_;
};

void read_lef_string(std::string_view text, LefLibrary& lib, std::string_view source) {
  LefParser(text, source, lib).parse();
}

void read_lef(const std::string& path, LefLibrary& lib) {
  const InputFile f(path);
  read_lef_string(f.text(), lib, path);
  HLOG_INFO(lg_lef, "read LEF {}: library now has {} macros, {} sites", path, lib.num_macros(),
            lib.num_sites());
}

// ---------------------------------------------------------------------------
// DEF
// ---------------------------------------------------------------------------

namespace {

struct CompRec {
  std::string_view name, macro;
  Point loc;
  Orient orient = Orient::N;
  bool placed = false, fixed = false;
  std::size_t line = 0;
};
struct NetRec {
  std::string_view name;
  std::uint32_t first = 0, count = 0;  // global connection numbers
  const struct ConnRec* conns = nullptr;  // this net's connection records
  double weight = 1.0;
  bool mustjoin = false;
  std::size_t line = 0;
};
struct ConnRec {
  std::string_view comp, pin;
};

struct IoPin {
  std::string net;
  PinDir dir = PinDir::Inout;   // Net-side direction.
  Point loc;
  bool placed = false;
  bool connected = false;
};

class DefParser {
 public:
  DefParser(std::string_view text, std::string_view source, const LefLibrary& lib,
            DefReadReport& rep, const DefReadOptions& opts)
      : lx_(text, source), lib_(lib), rep_(rep), opts_(opts) {}

  Design parse() {
    for (std::string_view t = lx_.next(); !t.empty(); t = lx_.next()) {
      if (t == "END") {
        const std::string_view what = lx_.next();
        if (what == "DESIGN" || what.empty()) break;
        lx_.fail("unexpected 'END " + std::string(what) + "'");
      } else if (t == "DESIGN") {
        rep_.design_name = std::string(lx_.expect_any("design name"));
        lx_.expect(";");
        if (!comps_.empty() || !nets_.empty() || !io_pins_.empty()) {
          lx_.fail("DESIGN must come before COMPONENTS, PINS and NETS");
        }
        design_ = Design(rep_.design_name);
      } else if (t == "UNITS") {
        lx_.expect("DISTANCE");
        lx_.expect("MICRONS");
        dbu_ = lx_.number("DISTANCE MICRONS value");
        lx_.expect(";");
      } else if (t == "DIEAREA") {
        parse_diearea();
      } else if (t == "ROW") {
        parse_row();
      } else if (t == "COMPONENTS") {
        parse_components();
      } else if (t == "PINS") {
        parse_pins();
      } else if (t == "NETS") {
        parse_nets();
      } else if (t == "VIAS" || t == "SPECIALNETS" || t == "BLOCKAGES" ||
                 t == "REGIONS" || t == "GROUPS" || t == "FILLS" ||
                 t == "NONDEFAULTRULES" || t == "SCANCHAINS" || t == "PINPROPERTIES" ||
                 t == "PROPERTYDEFINITIONS" || t == "STYLES" || t == "SLOTS" ||
                 t == "COMPONENTMASKSHIFT" || t == "BEGINEXT") {
        if (t == "BEGINEXT") {
          while (lx_.expect_any("ENDEXT") != "ENDEXT") {}
        } else if (t == "COMPONENTMASKSHIFT") {
          lx_.skip_statement();
        } else {
          lx_.skip_block(t);
        }
      } else {
        // VERSION, DIVIDERCHAR, BUSBITCHARS, TECHNOLOGY, TRACKS, GCELLGRID,
        // HISTORY, ...
        lx_.skip_statement();
      }
    }
    return finish();
  }

 private:
  // Every warning goes to the log (channel io.def, id DEF-xxxx, so it can be
  // switched off or capped) and, up to max_warnings, into the report.
#define HERMES_DEF_WARN(id, ...)                                         \
  do {                                                                   \
    static constinit ::hermes::log::detail::MessageSite def_site_{id};   \
    warn(def_site_, __VA_ARGS__);                                        \
  } while (0)

#define HERMES_DEF_WARN_AT(id, line, ...)                                \
  do {                                                                   \
    static constinit ::hermes::log::detail::MessageSite def_site_{id};   \
    warn(def_site_, line, __VA_ARGS__);                                  \
  } while (0)

  void warn(log::detail::MessageSite& site, const std::string& msg) { warn(site, lx_.line(), msg); }
  void warn(log::detail::MessageSite& site, std::size_t line, const std::string& msg) {
    ++rep_.num_warnings;
    const bool keep = rep_.warnings.size() < opts_.max_warnings;
    if (!keep && !log_def().enabled(log::Level::Warn)) return;
    std::string text = std::string(lx_.source()) + ':' + std::to_string(line) + ": " + msg;
    log::write(log_def(), log::Level::Warn, text, site.get());
    if (keep) rep_.warnings.push_back(std::move(text));
  }
  static const log::Channel& log_def() { return lg_def; }

  void require_units() {
    if (dbu_ <= 0.0) lx_.fail("UNITS DISTANCE MICRONS must come before geometry");
  }

  Dbu um(double microns) const { return to_dbu(microns * dbu_); }

  // "( x y )" with '*' meaning "same as the previous point".
  Point point(Point prev = {}) {
    lx_.expect("(");
    Point p = prev;
    std::string_view t = lx_.peek();
    if (t == "*") lx_.next(); else p.x = to_dbu(lx_.number("x"));
    t = lx_.peek();
    if (t == "*") lx_.next(); else p.y = to_dbu(lx_.number("y"));
    // Optional extension value (e.g. in paths) before ')'.
    while (lx_.peek() != ")") lx_.number("point value");
    lx_.next();
    return p;
  }

  Orient orient() {
    const std::string_view t = lx_.expect_any("orientation");
    Orient o = Orient::N;
    if (!parse_orient(t, o)) lx_.fail("bad orientation '" + std::string(t) + "'");
    return o;
  }

  void parse_diearea() {
    Rect r{std::numeric_limits<Dbu>::max(), std::numeric_limits<Dbu>::max(),
           std::numeric_limits<Dbu>::min(), std::numeric_limits<Dbu>::min()};
    Point prev;
    int n = 0;
    while (lx_.peek() == "(") {
      prev = point(prev);
      r.xlo = std::min(r.xlo, prev.x);
      r.ylo = std::min(r.ylo, prev.y);
      r.xhi = std::max(r.xhi, prev.x);
      r.yhi = std::max(r.yhi, prev.y);
      ++n;
    }
    lx_.expect(";");
    if (n < 2) lx_.fail("DIEAREA needs at least two points");
    die_ = r;
    have_die_ = true;
  }

  void parse_row() {
    require_units();
    lx_.expect_any("row name");
    const std::string site(lx_.expect_any("site name"));
    const Dbu x = to_dbu(lx_.number("row x"));
    const Dbu y = to_dbu(lx_.number("row y"));
    const Orient o = orient();
    long nx = 1, ny = 1;
    Dbu sx = 0, sy = 0;
    if (lx_.peek() == "DO") {
      lx_.next();
      nx = static_cast<long>(lx_.number("DO count"));
      lx_.expect("BY");
      ny = static_cast<long>(lx_.number("BY count"));
      if (lx_.peek() == "STEP") {
        lx_.next();
        sx = to_dbu(lx_.number("STEP x"));
        sy = to_dbu(lx_.number("STEP y"));
      }
    }
    lx_.skip_statement();  // [+ PROPERTY ...] ;
    ++rep_.num_rows;
    const LefSite* s = lib_.find_site(site);
    if (!s) {
      HERMES_DEF_WARN("DEF-0001", "ROW uses unknown site '" + site + "'");
      return;
    }
    const Dbu sw = um(s->width), sh = um(s->height);
    ++site_count_[{sw, sh}];
    Row r;
    r.site_width = sw;
    r.site_height = sh;
    r.site_step = sx > 0 ? sx : sw;
    r.num_sites = static_cast<std::int32_t>(std::max(1L, nx));
    r.orient = o;
    for (long j = 0; j < std::max(1L, ny); ++j) {
      r.origin = {x, y + j * (sy > 0 ? sy : sh)};
      rows_.push_back(r);
      row_sites_.push_back(site);
    }
  }

  // Reads "COUNT ;" after a section keyword; returns the declared count.
  long section_count() {
    const double n = lx_.number("count");
    lx_.expect(";");
    return static_cast<long>(n);
  }

  // Declared section counts are only hints: clamp so a bogus count cannot
  // trigger a huge allocation.
  static std::size_t reserve_hint(long n) {
    constexpr long kMax = 1L << 24;
    return static_cast<std::size_t>(std::clamp(n, 0L, kMax));
  }

  // --- COMPONENTS and NETS: parsed in parallel chunks ----------------------
  //
  // The section body is cut at lines starting with '-' (one record each),
  // every chunk is tokenized by its own Lexer into plain records, and
  // finish() builds the Design from the records in bulk.

  /// Position of `END <name>` after `from` (parallel search over blocks;
  /// the first match wins, so the result does not depend on threads).
  std::size_t find_section_end(std::string_view name, std::size_t from) const {
    const std::string_view text = lx_.text();
    const std::string pat = "END " + std::string(name);
    auto valid = [&](std::size_t p) {
      const std::size_t e = p + pat.size();
      return (p == 0 || Tokens::is_space(text[p - 1])) && (e == text.size() || Tokens::is_space(text[e]));
    };
    constexpr std::size_t kBlock = std::size_t{1} << 22;
    const std::size_t n = text.size() - from;
    const std::size_t nb = n / kBlock + 1;
    std::atomic<std::size_t> best{std::string_view::npos};
    par::parallel_for_each(0, nb, [&](std::size_t blk) {
      const std::size_t lo = from + blk * kBlock;
      if (lo >= best.load(std::memory_order_relaxed)) return;  // An earlier block already matched.
      const std::size_t hi = std::min(text.size(), lo + kBlock + pat.size());
      const std::string_view w = text.substr(lo, hi - lo);
      for (std::size_t q = w.find(pat); q != std::string_view::npos; q = w.find(pat, q + 1)) {
        if (!valid(lo + q)) continue;
        std::size_t cur = best.load();
        while (lo + q < cur && !best.compare_exchange_weak(cur, lo + q)) {}
        return;
      }
    });
    if (best.load() != std::string_view::npos) return best.load();
    // Unusual spacing ("END\n  COMPONENTS"): token scan.
    for (std::size_t p = text.find("END", from); p != std::string_view::npos; p = text.find("END", p + 3)) {
      if (p > 0 && !Tokens::is_space(text[p - 1])) continue;
      std::size_t q = p + 3;
      if (q >= text.size() || !Tokens::is_space(text[q])) continue;
      while (q < text.size() && Tokens::is_space(text[q])) ++q;
      if (text.compare(q, name.size(), name) == 0 &&
          (q + name.size() == text.size() || Tokens::is_space(text[q + name.size()]))) {
        return p;
      }
    }
    lx_.fail("missing 'END " + std::string(name) + "'");
  }

  static bool starts_record(std::string_view line) {
    std::size_t s = 0;
    while (s < line.size() && Tokens::is_space(line[s])) ++s;
    return s + 1 < line.size() && line[s] == '-' && Tokens::is_space(line[s + 1]);
  }

  /// Parses the body of section `name` (from the lexer position to
  /// `END <name>`) in parallel chunks cut at '-' lines:
  /// parse_chunk(lexer, out) fills one Out per chunk. Leaves the lexer
  /// after `END <name>`. Outputs are in file order.
  template <class Out, class F>
  std::vector<Out> parse_section(std::string_view name, F&& parse_chunk) {
    const std::size_t b = lx_.offset(), line0 = lx_.cur_line();
    const std::size_t e = find_section_end(name, b);
    const std::string_view body = lx_.text().substr(b, e - b);
    const std::size_t parts = std::min<std::size_t>(body.size() / (1 << 18) + 1,
                                                    static_cast<std::size_t>(par::num_threads()) * 4);
    const auto cuts = split_lines(body, parts, 1 << 18, &starts_record);
    const std::size_t n = cuts.size() - 1;
    std::vector<std::size_t> nl(n + 1, 0);
    par::parallel_for_each(0, n, [&](std::size_t i) {
      nl[i + 1] = static_cast<std::size_t>(std::count(body.data() + cuts[i], body.data() + cuts[i + 1], '\n'));
    });
    std::vector<std::size_t> line(n + 1, line0);
    for (std::size_t i = 1; i <= n; ++i) line[i] = line[i - 1] + nl[i];
    std::vector<Out> outs(n);
    par::parallel_for_each(0, n, [&](std::size_t i) {
      Lexer lx(body.substr(cuts[i], cuts[i + 1] - cuts[i]), lx_.source(), line[i]);
      parse_chunk(lx, outs[i]);
    });
    lx_.seek(e, line[n]);
    lx_.expect("END");
    lx_.expect(name);
    return outs;
  }

  /// Appends the per-chunk vectors to `dst` (parallel copy).
  template <class T, class Get>
  static void append_parts(std::vector<T>& dst, std::size_t n, Get get) {
    std::vector<std::size_t> off(n + 1, dst.size());
    for (std::size_t i = 0; i < n; ++i) off[i + 1] = off[i] + get(i).size();
    dst.resize(off[n]);
    par::parallel_for_each(0, n, [&](std::size_t i) {
      auto& v = get(i);
      std::move(v.begin(), v.end(), dst.begin() + static_cast<std::ptrdiff_t>(off[i]));
      std::remove_reference_t<decltype(v)>().swap(v);
    });
  }

  void parse_components() {
    require_units();
    section_count();
    PhaseTimer pt(lg_def, std::string(lx_.source()) + " COMPONENTS");
    auto chunks = parse_section<std::vector<CompRec>>("COMPONENTS", [](Lexer& lx, std::vector<CompRec>& out) {
      out.reserve(lx.text().size() / 64);
      for (std::string_view t = lx.next(); !t.empty(); t = lx.next()) {
        if (t != "-") lx.fail("expected '-' to start a component, got '" + std::string(t) + "'");
        out.push_back(parse_component(lx));
      }
    });
    pt.lap("parse");
    append_parts(comps_, chunks.size(), [&](std::size_t i) -> std::vector<CompRec>& { return chunks[i]; });
    pt.lap("concat");
  }

  static Point def_point(Lexer& lx, Point prev = {}) {
    lx.expect("(");
    Point p = prev;
    std::string_view t = lx.peek();
    if (t == "*") lx.next(); else p.x = to_dbu(lx.number("x"));
    t = lx.peek();
    if (t == "*") lx.next(); else p.y = to_dbu(lx.number("y"));
    while (lx.peek() != ")") lx.number("point value");
    lx.next();
    return p;
  }

  static Orient def_orient(Lexer& lx) {
    const std::string_view t = lx.expect_any("orientation");
    Orient o = Orient::N;
    if (!parse_orient(t, o)) lx.fail("bad orientation '" + std::string(t) + "'");
    return o;
  }

  static CompRec parse_component(Lexer& lx) {
    CompRec r;
    r.name = lx.expect_any("component name");
    r.line = lx.line();
    r.macro = lx.expect_any("macro name");
    for (std::string_view t = lx.expect_any(";"); t != ";";) {
      if (t != "+") {
        t = lx.expect_any(";");
        continue;
      }
      const std::string_view kw = lx.expect_any("component option");
      if (kw == "PLACED" || kw == "FIXED" || kw == "COVER") {
        r.loc = def_point(lx);
        r.orient = def_orient(lx);
        r.placed = true;
        r.fixed = kw != "PLACED";
      } else if (kw == "UNPLACED") {
        r.placed = false;
      }
      // Skip the rest of this option (SOURCE, WEIGHT, REGION, PROPERTY, HALO, ...).
      t = lx.expect_any(";");
      while (t != "+" && t != ";") t = lx.expect_any(";");
    }
    return r;
  }

  void parse_pins() {
    require_units();
    section_count();
    for (;;) {
      const std::string_view t = lx_.expect_any("END PINS");
      if (t == "END") {
        lx_.expect("PINS");
        return;
      }
      if (t != "-") lx_.fail("expected '-' to start a pin, got '" + std::string(t) + "'");
      parse_io_pin();
    }
  }

  void parse_io_pin() {
    std::string name(lx_.expect_any("pin name"));
    IoPin io;
    bool have_shape = false;
    Point shape_center;
    Point place;
    Orient o = Orient::N;
    for (std::string_view t = lx_.expect_any(";"); t != ";";) {
      if (t != "+") {
        t = lx_.expect_any(";");
        continue;
      }
      const std::string_view kw = lx_.expect_any("pin option");
      if (kw == "NET") {
        io.net = std::string(lx_.expect_any("net name"));
      } else if (kw == "DIRECTION") {
        const std::string_view d = lx_.expect_any("direction");
        // Net-side view: a chip input drives the net.
        io.dir = d == "INPUT" ? PinDir::Output : d == "OUTPUT" ? PinDir::Input : PinDir::Inout;
      } else if (kw == "LAYER" && !have_shape) {
        lx_.expect_any("layer name");
        while (lx_.peek() != "(") {
          lx_.next();  // MASK n | SPACING d | DESIGNRULEWIDTH d
          lx_.next();
        }
        const Point a = point();
        const Point b = point(a);
        shape_center = {(a.x + b.x) / 2, (a.y + b.y) / 2};
        have_shape = true;
      } else if (kw == "PLACED" || kw == "FIXED" || kw == "COVER") {
        place = point();
        o = orient();
        io.placed = true;
      }
      t = lx_.expect_any(";");
      while (t != "+" && t != ";") t = lx_.expect_any(";");
    }
    if (io.placed) {
      io.loc = place + (have_shape ? orient_about_origin(shape_center, o) : Point{});
    } else {
      HERMES_DEF_WARN("DEF-0003", "I/O pin '" + name + "' is not placed; using (0, 0)");
    }
    ++rep_.num_io_pins;
    io_pins_.insert_or_assign(std::move(name), std::move(io));
  }

  struct NetChunk {
    std::vector<NetRec> nets;
    std::vector<ConnRec> conns;
  };
  std::vector<std::vector<ConnRec>> conn_chunks_;
  std::uint32_t total_conns_ = 0;

  void parse_nets() {
    section_count();
    PhaseTimer pt(lg_def, std::string(lx_.source()) + " NETS");
    auto chunks = parse_section<NetChunk>("NETS", [](Lexer& lx, NetChunk& c) {
      c.conns.reserve(lx.text().size() / 12);
      c.nets.reserve(lx.text().size() / 64);
      for (std::string_view t = lx.next(); !t.empty(); t = lx.next()) {
        if (t != "-") lx.fail("expected '-' to start a net, got '" + std::string(t) + "'");
        parse_net(lx, c);
      }
    });
    pt.lap("parse");
    // Rebase connection indices to one global numbering; connection records
    // stay in their chunk (NetRec::conns points at them).
    std::uint32_t base = total_conns_;
    for (auto& c : chunks) {
      for (NetRec& n : c.nets) {
        n.conns = c.conns.data() + n.first;
        n.first += base;
      }
      base += static_cast<std::uint32_t>(c.conns.size());
    }
    total_conns_ = base;
    append_parts(nets_, chunks.size(), [&](std::size_t i) -> std::vector<NetRec>& { return chunks[i].nets; });
    for (auto& c : chunks) conn_chunks_.push_back(std::move(c.conns));
    pt.lap("concat");
  }

  template <class Chunk>
  static void parse_net(Lexer& lx, Chunk& c) {
    NetRec n;
    n.name = lx.expect_any("net name");
    n.line = lx.line();
    n.first = static_cast<std::uint32_t>(c.conns.size());
    if (n.name == "MUSTJOIN") {
      n.mustjoin = true;
      lx.skip_statement();
      c.nets.push_back(n);
      return;
    }
    for (;;) {
      const std::string_view t = lx.expect_any(";");
      if (t == ";") break;
      if (t == "+") {
        // Net options and routed wiring: only + WEIGHT is kept.
        if (lx.peek() == "WEIGHT") {
          lx.next();
          n.weight = lx.number("net weight");
        }
        for (std::string_view w = lx.expect_any(";"); w != ";"; w = lx.expect_any(";")) {
          if (w == "+" && lx.peek() == "WEIGHT") {
            lx.next();
            n.weight = lx.number("net weight");
          }
        }
        break;
      }
      if (t != "(") lx.fail("expected '(' in net '" + std::string(n.name) + "', got '" + std::string(t) + "'");
      ConnRec cr;
      cr.comp = lx.expect_any("component name");
      cr.pin = lx.expect_any("pin name");
      // Optional "+ SYNTHESIZED" inside the parentheses.
      for (std::string_view w = lx.expect_any(")"); w != ")"; w = lx.expect_any(")")) {}
      c.conns.push_back(cr);
    }
    n.count = static_cast<std::uint32_t>(c.conns.size()) - n.first;
    c.nets.push_back(n);
  }

  [[noreturn]] void fail_line(std::size_t line, const std::string& msg) const {
    throw ParseError(std::string(lx_.source()) + ':' + std::to_string(line) + ": " + msg);
  }

  /// Builds the Design from the records (bulk, mostly parallel).
  void build_netlist() {
    using DA = detail::DesignAccess;
    PhaseTimer pt(lg_def, std::string(lx_.source()) + " build");
    const std::size_t nc = comps_.size();
    // Macro lookup without allocations: string_view -> (macro, index).
    std::unordered_map<std::string_view, std::pair<const LefMacro*, std::uint32_t>> macros;
    std::vector<const LefMacro*> macro_list;
    lib_.for_each_macro([&](const LefMacro& m) {
      macros.emplace(m.name, std::make_pair(&m, static_cast<std::uint32_t>(macro_list.size())));
      macro_list.push_back(&m);
    });

    // --- Cells ---
    {
      std::vector<std::string_view> names(nc);
      par::parallel_for(0, nc, 16384, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) names[i] = comps_[i].name;
      });
      try {
        DA::cell_names(design_).assign(names);
      } catch (const std::invalid_argument&) {
        // Report the second occurrence, as a serial reader would.
        std::unordered_map<std::string_view, std::size_t> seen;
        for (std::size_t i = 0; i < nc; ++i) {
          if (!seen.emplace(comps_[i].name, i).second) {
            fail_line(comps_[i].line, "duplicate component '" + std::string(comps_[i].name) + "'");
          }
        }
        throw;
      }
    }
    comp_macro_.assign(nc, nullptr);
    std::vector<std::uint32_t> macro_idx(nc, ~0u);
    std::vector<std::uint8_t> used(macro_list.size(), 0);
    std::vector<Cell>& cells = DA::cells(design_);
    cells.resize(nc);
    struct Counts {
      std::size_t macros = 0, fixed = 0, unplaced = 0, unknown = 0;
    };
    const Counts cnt = par::parallel_reduce(
        0, nc, 8192, Counts{},
        [&](std::size_t lo, std::size_t hi) {
          Counts k;
          for (std::size_t i = lo; i < hi; ++i) {
            const CompRec& r = comps_[i];
            const auto it = macros.find(r.macro);
            const LefMacro* m = nullptr;
            if (it != macros.end()) {
              m = it->second.first;
              macro_idx[i] = it->second.second;
              used[it->second.second] = 1;  // Benign race: every writer stores 1.
            }
            comp_macro_[i] = m;
            const Dbu w0 = m ? um(m->width) : 0, h0 = m ? um(m->height) : 0;
            const bool rot = swaps_xy(r.orient);
            Cell c;
            c.width = rot ? h0 : w0;
            c.height = rot ? w0 : h0;
            c.is_macro = m && m->is_block();
            c.loc = r.loc;
            c.orient = r.orient;
            c.fixed = r.fixed;
            c.placed = r.placed;
            cells[i] = c;
            k.macros += c.is_macro;
            k.fixed += r.fixed;
            k.unplaced += !r.placed;
            k.unknown += m == nullptr;
          }
          return k;
        },
        [](Counts a, Counts b) {
          a.macros += b.macros;
          a.fixed += b.fixed;
          a.unplaced += b.unplaced;
          a.unknown += b.unknown;
          return a;
        });
    rep_.num_components = nc;
    rep_.num_macros = cnt.macros;
    rep_.num_fixed = cnt.fixed;
    rep_.num_unplaced = cnt.unplaced;
    // Masters: intern the used macro names (in library order), then map.
    std::vector<Id> master_of(macro_list.size(), kInvalidId);
    NameTable& masters = DA::master_names(design_);
    std::vector<std::size_t> order;
    for (std::size_t k = 0; k < macro_list.size(); ++k) {
      if (used[k]) order.push_back(k);
    }
    std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return macro_list[x]->name < macro_list[y]->name; });
    for (std::size_t k : order) master_of[k] = masters.intern(macro_list[k]->name);
    std::vector<Id>& cell_master = DA::cell_master(design_);
    cell_master.assign(nc, kInvalidId);
    par::parallel_for(0, nc, 16384, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) {
        if (macro_idx[i] != ~0u) cell_master[i] = master_of[macro_idx[i]];
      }
    });
    if (cnt.unknown) {
      for (std::size_t i = 0; i < nc; ++i) {
        if (comp_macro_[i]) continue;
        const CompRec& r = comps_[i];
        const std::string msg = "component '" + std::string(r.name) + "' uses unknown macro '" + std::string(r.macro) + "'";
        if (!opts_.allow_unknown_macros) fail_line(r.line, msg);
        HERMES_DEF_WARN_AT("DEF-0002", r.line, msg);
        cell_master[i] = masters.intern(r.macro);
      }
    }

    pt.lap("cells");
    // --- Nets ---
    std::vector<std::uint32_t> kept;  // net record index per design net
    kept.reserve(nets_.size());
    for (std::size_t i = 0; i < nets_.size(); ++i) {
      if (nets_[i].mustjoin) {
        HERMES_DEF_WARN_AT("DEF-0004", nets_[i].line, "MUSTJOIN nets are skipped");
        continue;
      }
      kept.push_back(static_cast<std::uint32_t>(i));
    }
    const std::size_t nn = kept.size();
    {
      std::vector<std::string_view> names(nn);
      par::parallel_for(0, nn, 16384, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) names[i] = nets_[kept[i]].name;
      });
      try {
        DA::net_names(design_).assign(names);
      } catch (const std::invalid_argument&) {
        std::unordered_map<std::string_view, std::size_t> seen;
        for (std::size_t i = 0; i < nn; ++i) {
          if (!seen.emplace(names[i], i).second) {
            fail_line(nets_[kept[i]].line, "net '" + std::string(names[i]) + "' is defined twice");
          }
        }
        throw;
      }
    }
    std::vector<Net>& dnets = DA::nets(design_);
    dnets.resize(nn);
    par::parallel_for(0, nn, 16384, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) dnets[i] = Net{nets_[kept[i]].weight, false};
    });
    rep_.num_nets = nn;

    pt.lap("nets");
    // Pin names: every pin name of the macros in use; I/O names come later.
    NameTable& pin_names = DA::pin_names(design_);
    for (std::size_t k : order) {
      for (const LefPin& p : macro_list[k]->pins()) pin_names.intern(p.name);
    }

    pt.lap("pin names");
    // --- Connections, resolved in parallel straight into the pin array
    // (pin k = connection k; dropped connections are squeezed out after).
    const std::size_t nconn = total_conns_;
    std::vector<Pin>& pins = DA::pins(design_);
    std::vector<Id>& name_ids = DA::pin_name_ids(design_);
    std::vector<std::uint8_t> flag(nconn, 0);  // bit0 io, bit1 star, bit2 no lef pin, bit3 dropped
    par::parallel_invoke([&] { pins.resize(nconn); }, [&] { name_ids.resize(nconn); });
    pt.lap("pin arrays");
    std::atomic<std::size_t> bad_comp{std::numeric_limits<std::size_t>::max()};
    using Special = std::vector<std::uint32_t>;
    Special specials = par::parallel_reduce(0, nn, 2048, Special{}, [&](std::size_t lo, std::size_t hi) {
      Special sp;
      for (std::size_t i = lo; i < hi; ++i) {
        const NetRec& r = nets_[kept[i]];
        for (std::uint32_t k = r.first; k < r.first + r.count; ++k) {
          const ConnRec& c = r.conns[k - r.first];
          Pin& p = pins[k];
          p.net = static_cast<Id>(i);
          if (c.comp == "PIN") {
            flag[k] = 1;
            sp.push_back(k);
            continue;
          }
          if (c.comp == "*") {
            flag[k] = 2 | 8;
            sp.push_back(k);
            continue;
          }
          const Id cid = design_.find_cell(c.comp);
          if (cid == kInvalidId) {
            std::size_t cur = bad_comp.load();
            while (k < cur && !bad_comp.compare_exchange_weak(cur, k)) {}
            continue;
          }
          const LefMacro* m = comp_macro_[static_cast<std::size_t>(cid)];
          const Cell& cell = cells[static_cast<std::size_t>(cid)];
          p.cell = cid;
          p.offset = {cell.width / 2, cell.height / 2};
          p.dir = PinDir::Input;
          Id nid = kInvalidId;
          if (m) {
            if (const LefPin* lp = m->find_pin(c.pin)) {
              p.dir = lp->dir;
              const Point raw = lp->has_shape ? Point{um(lp->x), um(lp->y)}
                                              : Point{um(m->width / 2), um(m->height / 2)};
              p.offset = orient_offset(raw, um(m->width), um(m->height), cell.orient);
              nid = pin_names.find(c.pin);
            } else {
              flag[k] = 4;
            }
          }
          if (nid == kInvalidId) sp.push_back(k);
          name_ids[k] = nid;
        }
      }
      return sp;
    }, [](Special a, Special b) {
      if (a.empty()) return b;
      a.insert(a.end(), b.begin(), b.end());
      return a;
    });
    if (bad_comp.load() != std::numeric_limits<std::size_t>::max()) {
      const std::size_t k = bad_comp.load();
      const NetRec& r = nets_[kept[static_cast<std::size_t>(pins[k].net)]];
      fail_line(r.line, "net '" + std::string(r.name) + "' references unknown component '" + std::string(r.conns[k - r.first].comp) + "'");
    }
    pt.lap("connections");
    std::size_t dropped = 0;
    if (!specials.empty()) {
      // Serial pass in file order over the special connections: warnings,
      // I/O pins (each connects once), pin names not found in a macro.
      for (const std::uint32_t k : specials) {
        const std::uint8_t f = flag[k];
        Pin& p = pins[k];
        const NetRec& nr = nets_[kept[static_cast<std::size_t>(p.net)]];
        const ConnRec& c = nr.conns[k - nr.first];
        if (f & 2) {
          HERMES_DEF_WARN_AT("DEF-0006", nr.line, "net '" + std::string(nr.name) + "': '( * " + std::string(c.pin) + " )' connections are skipped");
          ++dropped;
        } else if (f & 1) {
          auto it = io_pins_.find(c.pin);
          if (it == io_pins_.end()) {
            HERMES_DEF_WARN_AT("DEF-0005", nr.line, "net '" + std::string(nr.name) + "' references unknown I/O pin '" + std::string(c.pin) + "'");
            IoPin io;
            io.net = std::string(nr.name);
            it = io_pins_.emplace(std::string(c.pin), std::move(io)).first;
          }
          IoPin& io = it->second;
          if (io.connected) {
            flag[k] = 8;
            ++dropped;
            continue;
          }
          io.connected = true;
          p.cell = kInvalidId;
          p.offset = io.loc;
          p.dir = io.dir;
          name_ids[k] = pin_names.intern(c.pin);
        } else {
          if (f & 4) {
            const LefMacro* m = comp_macro_[static_cast<std::size_t>(p.cell)];
            HERMES_DEF_WARN_AT("DEF-0007", nr.line, "component '" + std::string(c.comp) + "' (" + m->name + ") has no pin '" + std::string(c.pin) + "'");
          }
          if (name_ids[k] == kInvalidId) name_ids[k] = pin_names.intern(c.pin);
        }
      }
    }
    if (dropped) {
      // Squeeze out dropped connections (stable).
      std::size_t w = 0;
      for (std::size_t k = 0; k < nconn; ++k) {
        if (flag[k] & 8) continue;
        if (w != k) {
          pins[w] = pins[k];
          name_ids[w] = name_ids[k];
        }
        ++w;
      }
      pins.resize(w);
      name_ids.resize(w);
    }
    pt.lap("special connections");
    const std::size_t np = pins.size();

    // --- Pools: net lists are the pin ranges; cell lists by counting sort.
    std::vector<std::uint32_t> net_size(nn, 0);
    for (std::size_t p = 0; p < np; ++p) ++net_size[static_cast<std::size_t>(pins[p].net)];
    std::vector<Id> net_data(np);
    par::parallel_for(0, np, 65536, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t p = lo; p < hi; ++p) net_data[p] = static_cast<Id>(p);
    });
    DA::net_pins(design_).assign(std::move(net_data), net_size);
    pt.lap("net lists");
    std::vector<std::uint32_t> cell_count(nc, 0);
    std::vector<Id>& io_list = DA::io_pins(design_);
    for (std::size_t p = 0; p < np; ++p) {
      const Id c = pins[p].cell;
      if (c == kInvalidId) {
        io_list.push_back(static_cast<Id>(p));
      } else {
        ++cell_count[static_cast<std::size_t>(c)];
      }
    }
    {
      std::vector<std::uint32_t> fill(nc + 1, 0);
      for (std::size_t c = 0; c < nc; ++c) fill[c + 1] = fill[c] + cell_count[c];
      std::vector<Id> data(fill[nc]);
      for (std::size_t p = 0; p < np; ++p) {
        const Id c = pins[p].cell;
        if (c != kInvalidId) data[fill[static_cast<std::size_t>(c)]++] = static_cast<Id>(p);
      }
      DA::cell_pins(design_).assign(std::move(data), cell_count);
    }
    pt.lap("cell lists");
    (void)dropped;
    rep_.num_pins = np;
    DA::finish(design_);
  }

  Id net_id(const std::string& name) {
    const Id id = design_.find_net(name);
    return id != kInvalidId ? id : design_.add_net(name);
  }

  void connect_io(const std::string& pin_name, IoPin& io, Id net) {
    if (io.connected) return;
    design_.connect(kInvalidId, net, pin_name, io.loc, io.dir);
    io.connected = true;
    ++rep_.num_pins;
  }

  Design finish() {
    build_netlist();
    PhaseTimer pt(lg_def, std::string(lx_.source()) + " finish");
    // I/O pins whose net is named in PINS but which NETS did not list.
    for (auto& [name, io] : io_pins_) {
      if (!io.connected && !io.net.empty()) connect_io(name, io, net_id(io.net));
    }

    if (have_die_) {
      design_.set_die(die_);
    } else {
      HERMES_DEF_WARN("DEF-0008", "no DIEAREA; using the bounding box of placed objects");
      Rect r{std::numeric_limits<Dbu>::max(), std::numeric_limits<Dbu>::max(),
             std::numeric_limits<Dbu>::min(), std::numeric_limits<Dbu>::min()};
      for (const Cell& c : design_.cells()) {
        const Rect b = c.bbox();
        r = {std::min(r.xlo, b.xlo), std::min(r.ylo, b.ylo), std::max(r.xhi, b.xhi),
             std::max(r.yhi, b.yhi)};
      }
      if (!design_.cells().empty()) design_.set_die(r);
    }

    if (!site_count_.empty()) {
      const auto best = std::max_element(
          site_count_.begin(), site_count_.end(),
          [](const auto& a, const auto& b) { return a.second < b.second; });
      rep_.site_width = best->first.first;
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) rows_[i].site = design_.intern_site_name(row_sites_[i]);
    if (!rows_.empty()) design_.set_rows(std::move(rows_));  // Also sets row height.
    rep_.dbu_per_micron = dbu_;
    design_.set_dbu_per_micron(dbu_);
    if (!lib_.routing_layers.empty() && dbu_ > 0.0) {
      std::vector<RoutingLayer> layers;
      for (const LefLayer& l : lib_.routing_layers) {
        const auto dbu = [&](double um) { return static_cast<Dbu>(std::llround(um * dbu_)); };
        layers.push_back({l.name, l.horizontal ? LayerDir::Horizontal : LayerDir::Vertical, dbu(l.pitch),
                          dbu(l.width), dbu(l.spacing)});
      }
      design_.set_routing_layers(std::move(layers));
    }
    design_.compact();
    pt.lap("floorplan");
    return std::move(design_);
  }

  Lexer lx_;
  const LefLibrary& lib_;
  DefReadReport& rep_;
  const DefReadOptions& opts_;

  Design design_;
  double dbu_ = 0.0;
  Rect die_;
  bool have_die_ = false;
  std::vector<const LefMacro*> comp_macro_;
  StringMap<IoPin> io_pins_;
  std::map<std::pair<Dbu, Dbu>, std::size_t> site_count_;
  std::vector<Row> rows_;
  std::vector<std::string> row_sites_;
  std::vector<CompRec> comps_;
  std::vector<NetRec> nets_;
};

}  // namespace

Design read_def_string(std::string_view text, const LefLibrary& lib, DefReadReport* report,
                       const DefReadOptions& opts, std::string_view source) {
  DefReadReport local;
  DefReadReport& rep = report ? *report : local;
  rep = {};
  Design d = DefParser(text, source, lib, rep, opts).parse();
  HLOG_INFO(lg_def, "read DEF {}: {} components ({} macros, {} fixed, {} unplaced), {} I/O pins, "
            "{} nets, {} pins, {} rows, {} warnings",
            source, rep.num_components, rep.num_macros, rep.num_fixed, rep.num_unplaced,
            rep.num_io_pins, rep.num_nets, rep.num_pins, rep.num_rows, rep.num_warnings);
  return d;
}

Design read_def(const std::string& path, const LefLibrary& lib, DefReadReport* report,
                const DefReadOptions& opts) {
  const InputFile f(path);
  return read_def_string(f.text(), lib, report, opts, path);
}

Design read_lef_def(const std::vector<std::string>& lef_paths, const std::string& def_path,
                    DefReadReport* report, const DefReadOptions& opts) {
  LefLibrary lib;
  for (const std::string& p : lef_paths) read_lef(p, lib);
  return read_def(def_path, lib, report, opts);
}

}  // namespace hermes::io

// ---------------------------------------------------------------------------
// DEF writer
// ---------------------------------------------------------------------------

#include "hermes/par/parallel.hpp"

namespace hermes::io {

namespace {

template <class F>
std::vector<std::string> def_blocks(std::size_t n, std::size_t per_item, F&& f) {
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

void put_pt(std::string& out, Point p) {
  out += "( ";
  put_int(out, p.x);
  out += ' ';
  put_int(out, p.y);
  out += " )";
}

}  // namespace

void write_def(const Design& d, const std::string& path, const DefWriteOptions& opts) {
  std::vector<std::string> parts;
  std::string head;
  head += "VERSION 5.8 ;\nDIVIDERCHAR \"/\" ;\nBUSBITCHARS \"[]\" ;\nDESIGN ";
  head += d.name();
  head += " ;\nUNITS DISTANCE MICRONS ";
  put_double(head, d.dbu_per_micron() > 0 ? d.dbu_per_micron() : 1000.0);
  head += " ;\n\nDIEAREA ";
  put_pt(head, {d.die().xlo, d.die().ylo});
  head += ' ';
  put_pt(head, {d.die().xhi, d.die().yhi});
  head += " ;\n\n";
  std::size_t ri = 0;
  for (const Row& r : d.rows()) {
    head += "ROW ROW_";
    put_int(head, static_cast<long long>(ri++));
    head += ' ';
    const std::string_view site = d.site_name(r.site);
    head += site.empty() ? std::string_view("core") : site;
    head += ' ';
    put_int(head, r.origin.x);
    head += ' ';
    put_int(head, r.origin.y);
    head += ' ';
    head += orient_name(r.orient);
    head += " DO ";
    put_int(head, r.num_sites);
    head += " BY 1 STEP ";
    put_int(head, r.site_step);
    head += " 0 ;\n";
  }
  std::size_t live = 0;
  for (const Cell& c : d.cells()) live += !c.removed;
  head += "\nCOMPONENTS ";
  put_int(head, static_cast<long long>(live));
  head += " ;\n";
  parts.push_back(std::move(head));

  std::atomic<std::size_t> no_master{0};
  auto comps = def_blocks(d.num_cells(), 64, [&](std::string& out, std::size_t i) {
    const Cell& c = d.cells()[i];
    if (c.removed) return;
    out += "- ";
    out += d.cell_name(static_cast<Id>(i));
    out += ' ';
    const std::string_view m = d.cell_master(static_cast<Id>(i));
    if (m.empty()) {
      no_master.fetch_add(1, std::memory_order_relaxed);
      const bool rot = swaps_xy(c.orient);
      out += opts.missing_master_prefix;
      put_int(out, rot ? c.height : c.width);
      out += 'x';
      put_int(out, rot ? c.width : c.height);
    } else {
      out += m;
    }
    if (c.fixed) {
      out += " + FIXED ";
    } else if (c.placed) {
      out += " + PLACED ";
    } else {
      out += " + UNPLACED ;\n";
      return;
    }
    put_pt(out, c.loc);
    out += ' ';
    out += orient_name(c.orient);
    out += " ;\n";
  });
  for (auto& s : comps) parts.push_back(std::move(s));

  std::string mid = "END COMPONENTS\n\n";
  std::size_t nio = 0;
  for (Id p : d.io_pins()) nio += d.pin(p).net != kInvalidId;
  mid += "PINS ";
  put_int(mid, static_cast<long long>(nio));
  mid += " ;\n";
  for (Id pid : d.io_pins()) {
    const Pin& p = d.pin(pid);
    if (p.net == kInvalidId) continue;
    mid += "- ";
    mid += d.pin_name(pid);
    mid += " + NET ";
    mid += d.net_name(p.net);
    // Stored from the net's side: a chip input drives its net.
    mid += p.dir == PinDir::Output ? " + DIRECTION INPUT" : p.dir == PinDir::Input ? " + DIRECTION OUTPUT" : " + DIRECTION INOUT";
    mid += " + USE SIGNAL + PLACED ";
    put_pt(mid, p.offset);
    mid += " N ;\n";
  }
  mid += "END PINS\n\n";
  parts.push_back(std::move(mid));

  if (opts.nets) {
    std::size_t live_nets = 0;
    for (const Net& n : d.nets()) live_nets += !n.removed;
    std::string nh = "NETS ";
    put_int(nh, static_cast<long long>(live_nets));
    nh += " ;\n";
    parts.push_back(std::move(nh));
    auto nets = def_blocks(d.num_nets(), 96, [&](std::string& out, std::size_t n) {
      const Net& net = d.nets()[n];
      if (net.removed) return;
      out += "- ";
      out += d.net_name(static_cast<Id>(n));
      int k = 0;
      for (Id pid : d.net_pins(static_cast<Id>(n))) {
        const Pin& p = d.pin(pid);
        out += (++k % 6 == 0) ? "\n  ( " : " ( ";
        if (p.cell == kInvalidId) {
          out += "PIN";
        } else {
          out += d.cell_name(p.cell);
        }
        out += ' ';
        out += d.pin_name(pid);
        out += " )";
      }
      if (net.weight != 1.0) {
        out += " + WEIGHT ";
        put_int(out, std::llround(net.weight));
      }
      out += " ;\n";
    });
    for (auto& s : nets) parts.push_back(std::move(s));
    parts.push_back("END NETS\n\n");
  }
  parts.push_back("END DESIGN\n");
  write_parts(path, parts);
  if (no_master) {
    HLOG_INFO(lg_def, "write_def: {} cells had no master; wrote {}<w>x<h> names", no_master.load(),
              opts.missing_master_prefix);
  }
  HLOG_INFO(lg_def, "wrote DEF {} ({} components, {} nets)", path, live, d.num_nets());
}

}  // namespace hermes::io
