#include "hermes/io/registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>

#include "hermes/io/bookshelf.hpp"
#include "hermes/io/design_file.hpp"
#include "hermes/io/lefdef.hpp"
#include "hermes/log/log.hpp"

namespace hermes::io {

namespace fs = std::filesystem;

namespace {

HERMES_LOG_CHANNEL(lg_io, "io");

std::string lower_ext(const fs::path& p) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e;
}

bool wildcard(std::string_view pat, std::string_view s) {
  // Iterative '*' / '?' matcher.
  std::size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
  while (i < s.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == s[i])) {
      ++p;
      ++i;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = i;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      i = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

std::vector<fs::path> regular_files(const fs::path& dir) {
  std::vector<fs::path> out;
  for (const auto& e : fs::directory_iterator(dir)) {
    std::error_code ec;
    if (e.is_regular_file(ec)) out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

template <class... S>
std::vector<fs::path> with_ext(const std::vector<fs::path>& files, S... exts) {
  std::vector<fs::path> out;
  for (const fs::path& f : files) {
    const std::string e = lower_ext(f);
    if (((e == exts) || ...)) out.push_back(f);
  }
  return out;
}

std::string join_names(const std::vector<fs::path>& v) {
  std::string s;
  for (const auto& p : v) {
    if (!s.empty()) s += ", ";
    s += p.filename().string();
  }
  return s;
}

// --- LEF/DEF ---------------------------------------------------------------

/// Technology LEFs (LAYER definitions before any MACRO) go first.
bool is_tech_lef(const fs::path& p) {
  try {
    const InputFile f(p.string());
    const std::string_view t = f.text();
    auto find_kw = [&](std::string_view kw) {
      for (std::size_t pos = t.find(kw); pos != std::string_view::npos; pos = t.find(kw, pos + 1)) {
        const bool start = pos == 0 || t[pos - 1] == '\n' || t[pos - 1] == ' ' || t[pos - 1] == '\t';
        const std::size_t e = pos + kw.size();
        if (start && e < t.size() && (t[e] == ' ' || t[e] == '\t')) return pos;
      }
      return std::string_view::npos;
    };
    const std::size_t layer = find_kw("LAYER"), macro = find_kw("MACRO");
    return layer != std::string_view::npos && (macro == std::string_view::npos || layer < macro);
  } catch (const std::exception&) {
    return false;
  }
}

std::vector<InputItem> ordered_lefs(const std::vector<fs::path>& lefs) {
  std::vector<std::pair<int, fs::path>> v;
  for (const auto& p : lefs) v.emplace_back(is_tech_lef(p) ? 0 : 1, p);
  std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<InputItem> out;
  for (const auto& [k, p] : v) out.push_back({"lef", p.string()});
  return out;
}

class LefDefReader final : public Reader {
 public:
  std::string_view name() const override { return "lefdef"; }
  std::string_view description() const override { return "LEF (technology + cells) and one DEF"; }
  std::vector<std::string_view> roles() const override { return {"lef", "def"}; }

  std::vector<InputPlan> detect(const std::vector<fs::path>& files, std::string& problem) const override {
    const auto defs = with_ext(files, ".def");
    const auto lefs = with_ext(files, ".lef", ".tlef");
    if (defs.empty()) return {};
    if (lefs.empty()) {
      problem = "found DEF (" + join_names(defs) + ") but no .lef file";
      return {};
    }
    std::vector<InputPlan> plans;
    const auto lef_items = ordered_lefs(lefs);
    for (const auto& d : defs) {
      InputPlan p;
      p.format = "lefdef";
      p.items = lef_items;
      p.items.push_back({"def", d.string()});
      plans.push_back(std::move(p));
    }
    return plans;
  }

  bool accepts_file(const fs::path& file, InputPlan& plan) const override {
    if (lower_ext(file) != ".def") return false;
    const auto files = regular_files(file.parent_path().empty() ? fs::path(".") : file.parent_path());
    plan.format = "lefdef";
    plan.items = ordered_lefs(with_ext(files, ".lef", ".tlef"));
    plan.items.push_back({"def", file.string()});
    return true;
  }

  Design read(const InputPlan& plan, const Options& opts, LoadInfo& info) const override {
    const auto defs = plan.paths("def");
    if (defs.size() != 1) throw InputError("lefdef: exactly one DEF is needed, got " + std::to_string(defs.size()));
    LefLibrary lib;
    for (const std::string& p : plan.paths("lef")) read_lef(p, lib);
    if (lib.num_macros() == 0) HLOG_WARN(lg_io, "lefdef: the LEF files define no macros");
    DefReadOptions o;
    o.allow_unknown_macros = opts.get_bool("def.allow_unknown_macros", false);
    o.max_warnings = static_cast<std::size_t>(opts.get_int("def.max_warnings", 50));
    DefReadReport rep;
    Design d = read_def(defs[0], lib, &rep, o);
    info.num_warnings = rep.num_warnings;
    info.stats = {{"lef macros", std::to_string(lib.num_macros())},
                  {"components", std::to_string(rep.num_components)},
                  {"macros", std::to_string(rep.num_macros)},
                  {"fixed", std::to_string(rep.num_fixed)},
                  {"unplaced", std::to_string(rep.num_unplaced)},
                  {"io pins", std::to_string(rep.num_io_pins)},
                  {"nets", std::to_string(rep.num_nets)},
                  {"pins", std::to_string(rep.num_pins)},
                  {"rows", std::to_string(rep.num_rows)}};
    return d;
  }
};

// --- Bookshelf -------------------------------------------------------------

class BookshelfReader final : public Reader {
 public:
  std::string_view name() const override { return "bookshelf"; }
  std::string_view description() const override { return "Bookshelf .aux (+ .nodes .nets .pl .scl .wts .route)"; }
  std::vector<std::string_view> roles() const override {
    return {"aux", "nodes", "nets", "pl", "scl", "wts", "route", "shapes"};
  }

  std::vector<InputPlan> detect(const std::vector<fs::path>& files, std::string& problem) const override {
    std::vector<InputPlan> plans;
    const auto aux = with_ext(files, ".aux");
    for (const auto& a : aux) {
      InputPlan p;
      p.format = "bookshelf";
      p.items.push_back({"aux", a.string()});
      plans.push_back(std::move(p));
    }
    if (!aux.empty()) return plans;
    const auto nodes = with_ext(files, ".nodes"), nets = with_ext(files, ".nets");
    if (nodes.empty() && nets.empty()) return {};
    if (nodes.size() != 1 || nets.size() != 1) {
      problem = "found Bookshelf files without an .aux (nodes: " + join_names(nodes) + "; nets: " + join_names(nets) + ")";
      return {};
    }
    InputPlan p;
    p.format = "bookshelf";
    p.items.push_back({"nodes", nodes[0].string()});
    p.items.push_back({"nets", nets[0].string()});
    for (const char* ext : {".pl", ".scl", ".wts", ".route"}) {
      const auto v = with_ext(files, std::string(ext));
      if (v.size() == 1) p.items.push_back({std::string(ext + 1), v[0].string()});
    }
    plans.push_back(std::move(p));
    return plans;
  }

  bool accepts_file(const fs::path& file, InputPlan& plan) const override {
    if (lower_ext(file) != ".aux") return false;
    plan.format = "bookshelf";
    plan.items = {{"aux", file.string()}};
    return true;
  }

  Design read(const InputPlan& plan, const Options& opts, LoadInfo& info) const override {
    BookshelfFiles f;
    const auto aux = plan.paths("aux");
    if (aux.size() > 1) throw InputError("bookshelf: more than one .aux given");
    if (!aux.empty()) f = parse_aux(aux[0]);
    auto take = [&](const char* role, std::string& slot) {
      const auto v = plan.paths(role);
      if (v.size() > 1) throw InputError(std::string("bookshelf: more than one '") + role + "' file");
      if (!v.empty()) slot = v[0];  // Manifest entries override the .aux.
    };
    take("nodes", f.nodes);
    take("nets", f.nets);
    take("pl", f.pl);
    take("scl", f.scl);
    take("wts", f.wts);
    take("route", f.route);
    take("shapes", f.shapes);
    BookshelfReport rep;
    Design d = read_bookshelf(f, opts, &rep);
    info.num_warnings = rep.num_warnings;
    info.stats = {{"nodes", std::to_string(rep.num_nodes)},
                  {"terminals", std::to_string(rep.num_terminals)},
                  {"macros", std::to_string(rep.num_macros)},
                  {"fixed", std::to_string(rep.num_fixed)},
                  {"nets", std::to_string(rep.num_nets)},
                  {"pins", std::to_string(rep.num_pins)},
                  {"rows", std::to_string(rep.num_rows)},
                  {"routing grid", rep.route ? "yes" : "no"},
                  {".pl placed", rep.pl_placed ? "yes" : "no"}};
    return d;
  }
};

// --- hermes design file ----------------------------------------------------

class HdbReader final : public Reader {
 public:
  std::string_view name() const override { return "hdb"; }
  std::string_view description() const override { return "hermes design file (.hdb, save/restore)"; }
  std::vector<std::string_view> roles() const override { return {"hdb"}; }

  std::vector<InputPlan> detect(const std::vector<fs::path>& files, std::string&) const override {
    std::vector<InputPlan> plans;
    for (const auto& f : with_ext(files, ".hdb")) {
      InputPlan p;
      p.format = "hdb";
      p.items = {{"hdb", f.string()}};
      plans.push_back(std::move(p));
    }
    return plans;
  }

  bool accepts_file(const fs::path& file, InputPlan& plan) const override {
    if (lower_ext(file) != ".hdb" && !is_design_file(file.string())) return false;
    plan.format = "hdb";
    plan.items = {{"hdb", file.string()}};
    return true;
  }

  Design read(const InputPlan& plan, const Options& opts, LoadInfo& info) const override {
    const auto v = plan.paths("hdb");
    if (v.size() != 1) throw InputError("hdb: exactly one design file is needed");
    LoadOptions lo;
    lo.verify = opts.get_bool("hdb.verify", true);
    DesignFileInfo fi;
    Design d = load_design(v[0], lo, &fi);
    info.stats = {{"format", std::to_string(fi.major) + "." + std::to_string(fi.minor)},
                  {"cells", std::to_string(d.num_cells())},
                  {"nets", std::to_string(d.num_nets())},
                  {"pins", std::to_string(d.num_pins())},
                  {"file MB", std::to_string(static_cast<double>(fi.file_bytes) / 1e6)}};
    if (fi.meta.count("note")) info.stats.push_back({"note", fi.meta.at("note")});
    return d;
  }
};

// --- Writers ---------------------------------------------------------------

class HdbWriter final : public Writer {
 public:
  std::string_view name() const override { return "hdb"; }
  std::string_view description() const override { return "hermes design file (save/restore)"; }
  std::vector<std::string_view> extensions() const override { return {".hdb"}; }
  void write(const Design& d, const std::string& path, const Options& o) const override {
    SaveOptions so;
    so.compress = o.get_bool("hdb.compress", true);
    so.note = o.get("hdb.note");
    save_design(d, path, so);
  }
};

class DefWriter final : public Writer {
 public:
  std::string_view name() const override { return "def"; }
  std::string_view description() const override { return "DEF 5.8"; }
  std::vector<std::string_view> extensions() const override { return {".def"}; }
  void write(const Design& d, const std::string& path, const Options& o) const override {
    DefWriteOptions wo;
    wo.nets = o.get_bool("def.nets", true);
    write_def(d, path, wo);
  }
};

class PlWriter final : public Writer {
 public:
  std::string_view name() const override { return "pl"; }
  std::string_view description() const override { return "Bookshelf placement (.pl)"; }
  std::vector<std::string_view> extensions() const override { return {".pl"}; }
  void write(const Design& d, const std::string& path, const Options& o) const override {
    write_bookshelf_pl(d, path, o.get_double("bookshelf.scale", 0.0), true);
  }
};

class BookshelfWriter final : public Writer {
 public:
  std::string_view name() const override { return "bookshelf"; }
  std::string_view description() const override { return "full Bookshelf set (directory or .aux path)"; }
  std::vector<std::string_view> extensions() const override { return {".aux"}; }
  void write(const Design& d, const std::string& path, const Options& o) const override {
    const fs::path p(path);
    if (lower_ext(p) == ".aux") {
      write_bookshelf(d, p.parent_path().empty() ? "." : p.parent_path().string(), p.stem().string(),
                      o.get_double("bookshelf.scale", 0.0));
    } else {
      write_bookshelf(d, path, {}, o.get_double("bookshelf.scale", 0.0));
    }
  }
};

const Reader* reader_for_roles(const Registry& reg, const InputPlan& p) {
  const Reader* found = nullptr;
  for (const auto& r : reg.readers()) {
    const auto roles = r->roles();
    const bool all = std::all_of(p.items.begin(), p.items.end(), [&](const InputItem& it) {
      return std::find(roles.begin(), roles.end(), it.role) != roles.end();
    });
    if (all) {
      if (found) return nullptr;  // Ambiguous: needs "format".
      found = r.get();
    }
  }
  return found;
}

}  // namespace

// ---------------------------------------------------------------------------
// InputPlan / Registry
// ---------------------------------------------------------------------------

std::vector<std::string> InputPlan::paths(std::string_view role) const {
  std::vector<std::string> v;
  for (const auto& it : items) {
    if (it.role == role) v.push_back(it.path);
  }
  return v;
}

std::string InputPlan::describe() const {
  std::string s = format + " (" + origin + "):";
  for (const auto& it : items) s += "\n    " + it.role + "  " + it.path;
  return s;
}

Registry& Registry::global() {
  static Registry reg;
  static std::once_flag once;
  std::call_once(once, [] {
    reg.add_reader(std::make_unique<LefDefReader>());
    reg.add_reader(std::make_unique<BookshelfReader>());
    reg.add_reader(std::make_unique<HdbReader>());
    reg.add_writer(std::make_unique<HdbWriter>());
    reg.add_writer(std::make_unique<DefWriter>());
    reg.add_writer(std::make_unique<PlWriter>());
    reg.add_writer(std::make_unique<BookshelfWriter>());
  });
  return reg;
}

void Registry::add_reader(std::unique_ptr<Reader> r) {
  if (reader(r->name())) throw std::invalid_argument("reader '" + std::string(r->name()) + "' already registered");
  readers_.push_back(std::move(r));
}

void Registry::add_writer(std::unique_ptr<Writer> w) {
  if (writer(w->name())) throw std::invalid_argument("writer '" + std::string(w->name()) + "' already registered");
  writers_.push_back(std::move(w));
}

const Reader* Registry::reader(std::string_view name) const {
  for (const auto& r : readers_) {
    if (r->name() == name) return r.get();
  }
  return nullptr;
}

const Writer* Registry::writer(std::string_view name) const {
  for (const auto& w : writers_) {
    if (w->name() == name) return w.get();
  }
  return nullptr;
}

const Writer* Registry::writer_for(std::string_view path) const {
  const std::string e = lower_ext(fs::path(std::string(path)));
  for (const auto& w : writers_) {
    for (std::string_view x : w->extensions()) {
      if (x == e) return w.get();
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

InputPlan read_manifest(const std::string& path, const Registry& reg) {
  const InputFile f(path);
  const fs::path dir = fs::path(path).parent_path();
  InputPlan plan;
  plan.origin = "manifest " + path;
  std::size_t line_no = 0;
  std::string_view t = f.text();
  auto fail = [&](const std::string& msg) {
    throw InputError(path + ":" + std::to_string(line_no) + ": " + msg);
  };
  while (!t.empty()) {
    ++line_no;
    const std::size_t nl = t.find('\n');
    std::string_view line = t.substr(0, nl);
    t = nl == std::string_view::npos ? std::string_view() : t.substr(nl + 1);
    if (const std::size_t h = line.find('#'); h != std::string_view::npos) line = line.substr(0, h);
    Tokens tk(line);
    const std::string_view key = tk.next();
    if (key.empty()) continue;
    // The rest of the line is the value (paths may contain spaces).
    std::string_view rest = line.substr(static_cast<std::size_t>(key.data() + key.size() - line.data()));
    while (!rest.empty() && Tokens::is_space(rest.front())) rest.remove_prefix(1);
    while (!rest.empty() && Tokens::is_space(rest.back())) rest.remove_suffix(1);
    if (rest.empty()) fail("'" + std::string(key) + "' needs a value");
    if (key == "format") {
      plan.format = std::string(rest);
      if (!reg.reader(plan.format)) fail("unknown format '" + plan.format + "'");
    } else if (key == "option") {
      plan.options.set(rest);
    } else {
      const fs::path rel(std::string{rest});
      const fs::path full = rel.is_absolute() ? rel : dir / rel;
      const std::string fname = full.filename().string();
      if (fname.find_first_of("*?") != std::string::npos) {
        std::vector<fs::path> hits;
        const fs::path gdir = full.parent_path();
        if (fs::is_directory(gdir)) {
          for (const auto& p : regular_files(gdir)) {
            if (wildcard(fname, p.filename().string())) hits.push_back(p);
          }
        }
        if (hits.empty()) fail("no file matches '" + std::string(rest) + "'");
        for (const auto& p : hits) plan.items.push_back({std::string(key), p.string()});
      } else {
        if (!fs::exists(full)) fail("file not found: " + full.string());
        plan.items.push_back({std::string(key), full.string()});
      }
    }
  }
  if (plan.items.empty()) throw InputError(path + ": the manifest lists no input files");
  if (plan.format.empty()) {
    const Reader* r = reader_for_roles(reg, plan);
    if (!r) throw InputError(path + ": cannot tell the format from the roles; add a 'format' line");
    plan.format = std::string(r->name());
  }
  const Reader* r = reg.reader(plan.format);
  const auto roles = r->roles();
  for (const auto& it : plan.items) {
    if (std::find(roles.begin(), roles.end(), it.role) == roles.end()) {
      std::string all;
      for (auto x : roles) all += std::string(all.empty() ? "" : ", ") + std::string(x);
      throw InputError(path + ": role '" + it.role + "' is not used by format " + plan.format + " (roles: " + all + ")");
    }
  }
  return plan;
}

// ---------------------------------------------------------------------------
// Input resolution
// ---------------------------------------------------------------------------

InputPlan plan_input(const std::string& path_str, const Registry& reg) {
  const fs::path path(path_str);
  std::error_code ec;
  if (!fs::exists(path, ec)) throw InputError("input not found: " + path_str);
  InputPlan plan;
  if (!fs::is_directory(path, ec)) {
    if (path.filename() == kManifestName || lower_ext(path) == ".inputs") {
      plan = read_manifest(path_str, reg);
    } else {
      bool ok = false;
      for (const auto& r : reg.readers()) {
        if (r->accepts_file(path, plan)) {
          ok = true;
          break;
        }
      }
      if (!ok) throw InputError("no reader for file " + path_str);
      plan.origin = "file " + path_str;
    }
  } else if (fs::exists(path / kManifestName)) {
    plan = read_manifest((path / kManifestName).string(), reg);
  } else {
    const auto files = regular_files(path);
    std::vector<InputPlan> found;
    std::vector<std::string> problems;
    for (const auto& r : reg.readers()) {
      std::string problem;
      auto plans = r->detect(files, problem);
      if (!problem.empty()) problems.push_back(std::string(r->name()) + ": " + problem);
      for (auto& p : plans) found.push_back(std::move(p));
    }
    std::ostringstream os;
    if (found.size() != 1) {
      os << (found.empty() ? "nothing to read in " : "more than one input in ") << path_str << ".\n";
      for (const auto& p : found) {
        os << "  candidate " << p.format << ":";
        for (const auto& it : p.items) os << ' ' << fs::path(it.path).filename().string();
        os << '\n';
      }
      for (const auto& pr : problems) os << "  " << pr << '\n';
      os << "  files: " << join_names(files) << '\n';
      os << "Add a '" << kManifestName << "' file listing what to read, e.g.\n"
         << "    lef tech.lef\n    lef cells.lef\n    def design.def\n"
         << "or pass the file itself (.def, .aux, .hdb).";
      throw InputError(os.str());
    }
    for (const auto& pr : problems) HLOG_WARN(lg_io, "{} (ignored)", pr);
    plan = std::move(found[0]);
    plan.origin = "detected in " + path_str;
  }
  HLOG_INFO(lg_io, "input {}", plan.describe());
  return plan;
}

Design load_plan(const InputPlan& plan, const Options& opts, LoadInfo* info, const Registry& reg) {
  const Reader* r = reg.reader(plan.format);
  if (!r) throw InputError("unknown input format '" + plan.format + "'");
  Options o = opts;
  o.merge_defaults(plan.options);
  LoadInfo local;
  LoadInfo& li = info ? *info : local;
  li.plan = plan;
  const auto t0 = std::chrono::steady_clock::now();
  Design d = r->read(plan, o, li);
  li.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  HLOG_INFO(lg_io, "loaded {} ({}): {} cells, {} nets, {} pins in {:.3f} s", d.name(), plan.format, d.num_cells(),
            d.num_nets(), d.num_pins(), li.seconds);
  return d;
}

Design load_input(const std::string& path, const Options& opts, LoadInfo* info, const Registry& reg) {
  return load_plan(plan_input(path, reg), opts, info, reg);
}

void write_design(const Design& d, const std::string& path, std::string_view format, const Options& opts,
                  const Registry& reg) {
  const Writer* w = format.empty() ? reg.writer_for(path) : reg.writer(format);
  if (!w) {
    throw std::invalid_argument(format.empty() ? "no writer for '" + path + "' (use .hdb, .def, .pl or .aux)"
                                               : "unknown output format '" + std::string(format) + "'");
  }
  if (const fs::path parent = fs::path(path).parent_path(); !parent.empty() && w->name() != "bookshelf") {
    fs::create_directories(parent);
  }
  w->write(d, path, opts);
}

}  // namespace hermes::io
