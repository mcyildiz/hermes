// hermes CLI.
//
//   hermes <input>                 run the flow on a directory / file
//   hermes run --lef a.lef --lef b.lef --def d.def
//   hermes load <input>            read only, print a summary and load time
//   hermes convert <input> <out>   read, then write (.hdb .def .pl .aux)
//   hermes detect <input>          show what would be read
//   hermes info <file.hdb>         design-file header and sections
//   hermes demo [run options]      synthetic design
//   hermes shell [<input>]         interactive console (+ optional AI assistant)
//   hermes mcp [<input>]           MCP server for AI assistants (Claude Code, ...)

#include "apps/assist_cli.hpp"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <chrono>
#include <filesystem>
#include <vector>

#include "hermes/core/json.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/io/design_file.hpp"
#include "hermes/io/registry.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/log/log.hpp"
#include "hermes/log/sinks.hpp"
#include "hermes/qor/qor.hpp"
#include "hermes/track/run_tracker.hpp"

using namespace hermes;

namespace {

HERMES_LOG_CHANNEL(log_app, "app");

void usage() {
  std::puts(
      "hermes - Holistic Engine for Routability-driven Mixed-size placement with Embedded STA\n"
      "\n"
      "usage:\n"
      "  hermes [log options] <input> [run options]   run the flow (same as 'hermes run')\n"
      "  hermes run <input> [run options]\n"
      "  hermes run --lef tech.lef --lef cells.lef --def design.def [run options]\n"
      "  hermes load <input> [--opt K=V]              read only: summary + load time\n"
      "  hermes convert <input> <output>... [--opt K=V]   write .hdb .def .pl or .aux\n"
      "  hermes detect <input>                        show which files would be read\n"
      "  hermes info <file.hdb>                       design-file header and sections\n"
      "  hermes formats                               list readers and writers\n"
      "  hermes demo [run options]                    run the flow on a synthetic design\n"
      "  hermes shell [<input>] [--opt K=V]           interactive console (hermes_gui commands, 'ai <question>')\n"
      "  hermes mcp [<input>] [--allow-edits]         MCP server on stdio for AI assistants (read-only by default)\n"
      "  hermes ai init|status|privacy|mcp-config     set up the optional AI assistant\n"
      "  hermes --log-channels | --version | --help\n"
      "\n"
      "input: a directory, a manifest file (hermes.inputs), or a .def / .aux / .hdb file.\n"
      "  In a directory, a 'hermes.inputs' manifest decides what is read, e.g.\n"
      "      lef tech.lef\n"
      "      lef cells/*.lef\n"
      "      def design.def\n"
      "      option def.allow_unknown_macros=1\n"
      "  Without one, hermes detects LEF+DEF, Bookshelf (.aux) or a saved .hdb and\n"
      "  stops with a list of what it found when that is ambiguous.\n"
      "\n"
      "run options:\n"
      "  --out DIR          output directory (default hermes_out/<design>)\n"
      "  --write FILE       also write FILE (.def .pl .aux .hdb; relative to --out); repeatable\n"
      "  --no-save          do not save <out>/<design>.hdb at the end\n"
      "  --checkpoint       also save <out>/<design>.placed.hdb after placement\n"
      "  --place-only       skip routing\n"
      "  --opt K=V          reader/writer/flow option, e.g. bookshelf.scale=10,\n"
      "                     def.allow_unknown_macros=1, place.keep_placed=0; repeatable\n"
      "  --threads N        worker threads (default: all; env HERMES_THREADS)\n"
      "  --clock-period PS  clock period for the embedded STA (default 1000)\n"
      "  --no-qor           skip the end-of-run QoR report (<out>/<design>.qor.txt/.json)\n"
      "\n"
      "run tracking (every run and 'demo' is recorded; see tools/hermes_dashboard.py):\n"
      "  --track-db PATH    run database (default ~/.hermes/runs.db; env HERMES_RUNS_DB)\n"
      "  --no-track         do not record this run\n"
      "  --track-server URL also push the run live to a dashboard server, e.g. http://host:8765\n"
      "                     (env HERMES_TRACK_SERVER; token: --track-token or HERMES_TRACK_TOKEN)\n"
      "  --run-name NAME    label shown in the dashboard\n"
      "  --tag TAG          tag for filtering/comparing runs; repeatable\n"
      "\n"
      "log options (also: environment variable HERMES_LOG=SPEC):\n"
      "  --log SPEC        levels, e.g. 'warn,place=info,place.gp=debug,route.tree=trace'\n"
      "                    (off error warn info verbose debug trace; CHAN=inherit clears)\n"
      "  --log-msg ID=V    message control: V = on | off | N (print at most N times)\n"
      "                    | unlimited; ID '*' sets the default cap for all ids\n"
      "  --log-file PATH   also write a text log (all enabled levels, with timestamps)\n"
      "  --log-json PATH   also write JSON lines (one object per message)\n"
      "  --log-time        prefix console lines with elapsed time and thread\n"
      "  -q, --quiet       console shows warnings and errors only");
}

Design make_demo_design() {
  Design d("demo");
  d.set_die({0, 0, 100000, 100000});
  d.set_row_height(1000);

  // Two fixed I/O anchors on opposite edges and one movable macro.
  const Id io_in = d.add_cell("io_in", 1000, 1000);
  d.move_cell(io_in, {0, 50000});
  d.set_fixed(io_in, true);
  const Id io_out = d.add_cell("io_out", 1000, 1000);
  d.move_cell(io_out, {99000, 50000});
  d.set_fixed(io_out, true);
  const Id ram = d.add_cell("ram0", 20000, 20000, /*is_macro=*/true);

  // A chain of standard cells from io_in -> ram -> io_out.
  constexpr int kChain = 40;
  Id prev = io_in;
  for (int i = 0; i < kChain; ++i) {
    const Id c = d.add_cell("u" + std::to_string(i), 2000, 1000);
    const Id n = d.add_net("n" + std::to_string(i));
    d.connect(prev, n, "Z", {500, 500}, PinDir::Output);
    d.connect(c, n, "A", {0, 500}, PinDir::Input);
    prev = c;
    if (i == kChain / 2) {
      const Id nm = d.add_net("n_ram");
      d.connect(prev, nm, "Z", {2000, 500}, PinDir::Output);
      d.connect(ram, nm, "D", {0, 10000}, PinDir::Input);
      prev = ram;
    }
  }
  const Id nlast = d.add_net("n_out");
  d.connect(prev, nlast, "Z", {2000, 500}, PinDir::Output);
  d.connect(io_out, nlast, "A", {0, 500}, PinDir::Input);
  return d;
}

namespace fs = std::filesystem;

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string g_cmdline;  // for run tracking

struct RunArgs {
  std::string input;
  std::vector<std::string> lefs;
  std::string def;
  std::string out_dir;
  std::vector<std::string> writes;
  bool save = true;
  bool checkpoint = false;
  bool place_only = false;
  bool qor = true;
  double clock_period_ps = 1000.0;
  io::Options opts;
  // Run tracking.
  bool track = true;
  std::string track_db, track_server, track_token, run_name;
  std::vector<std::string> tags;
};

// --- Run tracking + Ctrl-C ---------------------------------------------------

std::atomic<RunControl*> g_ctl{nullptr};

extern "C" void on_sigint(int) {
  // First Ctrl-C: cancel at the next safe point (the run is recorded as
  // cancelled and the design is still saved). Second: default action.
  if (RunControl* c = g_ctl.load()) c->request_cancel();
  std::signal(SIGINT, SIG_DFL);
}

std::string env_or(const char* name, const std::string& v) {
  if (!v.empty()) return v;
  const char* e = std::getenv(name);
  return e ? e : "";
}

std::unique_ptr<track::RunTracker> start_tracking(const RunArgs& a, const std::string& input,
                                                  const FlowOptions& fo) {
  if (!a.track) return nullptr;
  track::RunInfo info;
  info.name = a.run_name;
  info.input = input;
  info.command = g_cmdline;
  info.tags = a.tags;
  std::string io = "{";
  for (const auto& [k, v] : a.opts.all()) {
    std::string kv;
    json_escape(kv, k);
    kv += ':';
    json_escape(kv, v);
    io += (io.size() > 1 ? "," : "") + kv;
  }
  io += "}";
  info.settings_json = "{\"flow\":" + flow_options_json(fo) + ",\"io\":" + io + "}";
  track::TrackerOptions to;
  to.db_path = a.track_db;
  to.server_url = env_or("HERMES_TRACK_SERVER", a.track_server);
  to.token = env_or("HERMES_TRACK_TOKEN", a.track_token);
  auto t = std::make_unique<track::RunTracker>(std::move(info), std::move(to));
  t->attach_log();
  HLOG_INFO(log_app, "run {} recorded in {}{}", t->run_id(), t->db_ok() ? t->db_path() : "(no local database)",
            a.track_server.empty() && !std::getenv("HERMES_TRACK_SERVER") ? "" : " and pushed to the run server");
  return t;
}

void report_results(track::RunTracker* t, const FlowReport& r) {
  if (!t) return;
  t->summary("iterations", static_cast<double>(r.placement.iterations));
  t->summary("wns_ps", r.final_wns_ps);
  t->summary("tns_ps", r.final_tns_ps);
  if (r.has_qor) t->set_qor(r.qor);
  else t->summary("hpwl", r.placement.hpwl);
}

/// Prints the QoR report and writes it next to the outputs.
void emit_qor(const FlowReport& r, const std::string& out, const std::string& design) {
  if (!r.has_qor) return;
  const std::string text = format_qor(r.qor);
  std::fputs(text.c_str(), stdout);
  std::fflush(stdout);
  if (out.empty()) return;
  const fs::path base = fs::path(out) / design;
  std::ofstream(base.string() + ".qor.txt") << text;
  std::ofstream(base.string() + ".qor.json") << qor_to_json(r.qor);
}

int run_demo(const RunArgs& a) {
  Design d = make_demo_design();
  FlowOptions opts;
  opts.clock_period_ps = 50.0;
  opts.router.grid_x = opts.router.grid_y = 32;
  opts.report_qor = a.qor;
  auto tr = start_tracking(a, "demo", opts);
  if (tr) tr->set_design(d.name());
  RunControl ctl(0.0);
  if (tr) tr->attach(ctl);
  g_ctl = &ctl;
  const double t0 = now_s();
  const FlowReport r = run_flow(d, opts, &ctl);
  g_ctl = nullptr;

  HLOG_INFO(log_app, "summary: placement {} iters, HPWL {:.0f}, overflow {:.1f}, max util {:.2f}",
            r.placement.iterations, r.placement.hpwl, r.placement.overflow,
            r.placement.max_utilization);
  HLOG_INFO(log_app, "summary: global route WL {:.0f}, overflow {:.1f}, max util {:.2f}",
            r.global_route.wirelength, r.global_route.total_overflow,
            r.global_route.max_utilization);
  HLOG_INFO(log_app, "summary: detailed route {}",
            r.detailed_route.completed ? "done" : "not implemented yet");
  HLOG_INFO(log_app, "summary: timing WNS {:.2f} ps, TNS {:.2f} ps", r.final_wns_ps,
            r.final_tns_ps);
  emit_qor(r, a.out_dir, d.name());
  if (tr) {
    tr->summary("flow_s", now_s() - t0);
    report_results(tr.get(), r);
    tr->finish(r.cancelled ? "cancelled" : "done");
  }
  return 0;
}

/// Parses the arguments after the command. `positional` collects bare words.
RunArgs parse_run_args(int argc, char** argv, int i, std::vector<std::string>* positional) {
  RunArgs a;
  for (; i < argc; ++i) {
    const std::string_view s = argv[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument(std::string(s) + " needs a value");
      return argv[++i];
    };
    if (s == "--lef") a.lefs.push_back(value());
    else if (s == "--def") a.def = value();
    else if (s == "--out" || s == "-o") a.out_dir = value();
    else if (s == "--write" || s == "-w") a.writes.push_back(value());
    else if (s == "--no-save") a.save = false;
    else if (s == "--checkpoint") a.checkpoint = true;
    else if (s == "--place-only") a.place_only = true;
    else if (s == "--opt") a.opts.set(value());
    else if (s == "--threads") par::set_num_threads(std::stoi(value()));
    else if (s == "--clock-period") a.clock_period_ps = std::stod(value());
    else if (s == "--no-qor") a.qor = false;
    else if (s == "--no-track") a.track = false;
    else if (s == "--track-db") a.track_db = value();
    else if (s == "--track-server") a.track_server = value();
    else if (s == "--track-token") a.track_token = value();
    else if (s == "--run-name") a.run_name = value();
    else if (s == "--tag") a.tags.push_back(value());
    else if (!s.empty() && s[0] == '-') throw std::invalid_argument("unknown option " + std::string(s));
    else if (positional) positional->push_back(std::string(s));
    else if (a.input.empty()) a.input = std::string(s);
    else throw std::invalid_argument("unexpected argument " + std::string(s));
  }
  if (!a.def.empty() || !a.lefs.empty()) {
    if (!a.input.empty()) throw std::invalid_argument("give either an input or --lef/--def, not both");
    if (a.def.empty()) throw std::invalid_argument("--lef needs a --def");
  }
  return a;
}

Design load(const RunArgs& a, io::LoadInfo* info) {
  if (!a.def.empty()) {
    io::InputPlan plan;
    plan.format = "lefdef";
    plan.origin = "command line";
    for (const auto& l : a.lefs) plan.items.push_back({"lef", l});
    plan.items.push_back({"def", a.def});
    HLOG_INFO(log_app, "input {}", plan.describe());
    return io::load_plan(plan, a.opts, info);
  }
  if (a.input.empty()) throw std::invalid_argument("no input given");
  return io::load_input(a.input, a.opts, info);
}

void print_summary(const Design& d, const io::LoadInfo& info) {
  std::printf("design %s (%s), loaded in %.3f s\n", d.name().c_str(), info.plan.format.c_str(), info.seconds);
  for (const auto& [k, v] : info.stats) std::printf("  %-14s %s\n", k.c_str(), v.c_str());
  const Rect& r = d.die();
  std::printf("  %-14s (%lld %lld) - (%lld %lld), row height %lld, %zu rows, %g dbu/um\n", "die",
              static_cast<long long>(r.xlo), static_cast<long long>(r.ylo), static_cast<long long>(r.xhi),
              static_cast<long long>(r.yhi), static_cast<long long>(d.row_height()), d.rows().size(),
              d.dbu_per_micron());
  std::size_t placed = 0, movable = 0;
  for (const Cell& c : d.cells()) {
    if (!c.movable()) continue;
    ++movable;
    placed += c.placed;
  }
  std::printf("  %-14s %zu of %zu movable cells have an input location\n", "placed", placed, movable);
  std::printf("  %-14s %.6g\n", "HPWL", total_hpwl(d));
  if (info.num_warnings) std::printf("  %-14s %zu\n", "warnings", info.num_warnings);
}

int cmd_run(const RunArgs& a) {
  FlowOptions fo;
  fo.clock_period_ps = a.clock_period_ps;
  fo.run_routing = !a.place_only;
  fo.placer.keep_placed = a.opts.get_bool("place.keep_placed", true);
  fo.report_qor = a.qor;
  std::string input = a.input;
  if (!a.def.empty()) {
    input = a.def;
    for (const auto& l : a.lefs) input += " " + l;
  }
  auto tr = start_tracking(a, input, fo);
  RunControl ctl(0.0);
  if (tr) tr->attach(ctl);
  try {
    if (tr) tr->stage("load");
    const double tl = now_s();
    io::LoadInfo info;
    Design d = load(a, &info);
    if (tr) {
      tr->set_design(d.name());
      tr->summary("load_s", now_s() - tl);
      tr->summary("cells", static_cast<double>(d.num_cells()));
      tr->summary("nets", static_cast<double>(d.num_nets()));
    }
    const std::string out = a.out_dir.empty() ? (fs::path("hermes_out") / d.name()).string() : a.out_dir;
    fs::create_directories(out);
    HLOG_INFO(log_app, "output directory {}", out);

    if (a.checkpoint) {
      fo.on_stage_done = [&](std::string_view stage, const Design& des) {
        if (stage != "placement") return;
        io::SaveOptions so;
        so.note = "after placement";
        io::save_design(des, (fs::path(out) / (des.name() + ".placed.hdb")).string(), so);
      };
    }
    const double t0 = now_s();
    g_ctl = &ctl;
    const FlowReport r = run_flow(d, fo, &ctl);
    g_ctl = nullptr;
    const double t_flow = now_s() - t0;

    if (tr) tr->stage("save");
    if (a.save) {
      io::SaveOptions so;
      so.note = r.cancelled ? "cancelled" : a.place_only ? "after placement" : "after flow";
      io::save_design(d, (fs::path(out) / (d.name() + ".hdb")).string(), so);
    }
    for (const std::string& w : a.writes) {
      const fs::path p = fs::path(w).is_absolute() ? fs::path(w) : fs::path(out) / w;
      io::write_design(d, p.string(), {}, a.opts);
    }
    HLOG_INFO(log_app, "summary: load {:.3f} s, flow {:.3f} s", info.seconds, t_flow);
    HLOG_INFO(log_app, "summary: placement {} iters, HPWL {:.6g}, overflow {:.1f}, max util {:.2f}",
              r.placement.iterations, r.placement.hpwl, r.placement.overflow, r.placement.max_utilization);
    if (!a.place_only) {
      HLOG_INFO(log_app, "summary: global route WL {:.0f}, overflow {:.1f}, max util {:.2f}",
                r.global_route.wirelength, r.global_route.total_overflow, r.global_route.max_utilization);
    }
    HLOG_INFO(log_app, "summary: timing WNS {:.2f} ps, TNS {:.2f} ps", r.final_wns_ps, r.final_tns_ps);
    emit_qor(r, out, d.name());
    if (tr) {
      tr->summary("flow_s", t_flow);
      report_results(tr.get(), r);
      tr->finish(r.cancelled ? "cancelled" : "done");
    }
    return r.cancelled ? 130 : 0;
  } catch (const std::exception& e) {
    g_ctl = nullptr;
    if (tr) tr->finish("failed", 2, e.what());
    throw;
  }
}

int cmd_load(const RunArgs& a) {
  io::LoadInfo info;
  const Design d = load(a, &info);
  print_summary(d, info);
  return 0;
}

int cmd_convert(int argc, char** argv, int i) {
  std::vector<std::string> pos;
  RunArgs a = parse_run_args(argc, argv, i, &pos);
  if (a.def.empty()) {
    if (pos.size() < 2) throw std::invalid_argument("convert needs an input and at least one output");
    a.input = pos[0];
    pos.erase(pos.begin());
  } else if (pos.empty()) {
    throw std::invalid_argument("convert needs at least one output");
  }
  io::LoadInfo info;
  const Design d = load(a, &info);
  for (const std::string& o : pos) {
    const double t0 = now_s();
    io::write_design(d, o, {}, a.opts);
    std::printf("wrote %s in %.3f s\n", o.c_str(), now_s() - t0);
  }
  return 0;
}

int cmd_detect(const std::string& input) {
  const io::InputPlan p = io::plan_input(input);
  std::printf("%s\n", p.describe().c_str());
  return 0;
}

int cmd_info(const std::string& path) {
  const io::DesignFileInfo fi = io::inspect_design_file(path);
  std::printf("%s: hermes design file format %u.%u, %.2f MB\n", path.c_str(), fi.major, fi.minor, fi.file_bytes / 1e6);
  for (const auto& [k, v] : fi.meta) std::printf("  %-16s %s\n", k.c_str(), v.c_str());
  std::printf("  %-22s %6s %12s %12s %12s\n", "section", "elem", "count", "raw", "stored");
  for (const auto& s : fi.sections) {
    std::printf("  %-22s %6u %12llu %12llu %12llu%s\n", s.tag.c_str(), s.elem_size,
                static_cast<unsigned long long>(s.count), static_cast<unsigned long long>(s.raw_bytes),
                static_cast<unsigned long long>(s.stored_bytes), s.compressed ? "" : " raw");
  }
  return 0;
}

int cmd_formats() {
  const auto& reg = io::Registry::global();
  std::puts("readers:");
  for (const auto& r : reg.readers()) {
    std::string roles;
    for (auto x : r->roles()) roles += std::string(roles.empty() ? "" : " ") + std::string(x);
    std::printf("  %-10s %s  [roles: %s]\n", std::string(r->name()).c_str(), std::string(r->description()).c_str(),
                roles.c_str());
  }
  std::puts("writers:");
  for (const auto& w : reg.writers()) {
    std::string ext;
    for (auto x : w->extensions()) ext += std::string(ext.empty() ? "" : " ") + std::string(x);
    std::printf("  %-10s %s  [%s]\n", std::string(w->name()).c_str(), std::string(w->description()).c_str(),
                ext.c_str());
  }
  return 0;
}

void list_channels() {
  std::printf("%-28s %-8s %s\n", "channel", "level", "set");
  for (const auto& c : log::channels()) {
    std::printf("%-28s %-8s %s\n", c.name.c_str(), std::string(log::to_string(c.level)).c_str(),
                c.explicit_level ? "explicit" : "inherited");
  }
}

}  // namespace

int main(int argc, char** argv) {
  // Global log options come before the command; they override HERMES_LOG.
  for (int k = 0; k < argc; ++k) {
    const std::string_view w = argv[k];
    const bool quote = w.empty() || w.find_first_of(" \t\"'") != std::string_view::npos;
    g_cmdline += (k ? " " : "") + (quote ? "'" + std::string(w) + "'" : std::string(w));
  }
  std::signal(SIGINT, on_sigint);
  int i = 1;
  try {
    for (; i < argc; ++i) {
      const std::string_view a = argv[i];
      const auto value = [&]() -> std::string {
        if (i + 1 >= argc) throw std::invalid_argument(std::string(a) + " needs a value");
        return argv[++i];
      };
      if (a == "--log") {
        log::configure(value());
      } else if (a == "--log-msg") {
        log::configure("msg:" + value());
      } else if (a == "--log-file") {
        log::add_sink(std::make_shared<log::TextSink>(value()));
      } else if (a == "--log-json") {
        log::add_sink(std::make_shared<log::JsonLinesSink>(value()));
      } else if (a == "--log-time") {
        log::console_sink()->set_format({true, true, false});
      } else if (a == "-q" || a == "--quiet") {
        log::console_sink()->set_level(log::Level::Warn);
      } else if (a == "--log-channels") {
        list_channels();
        return 0;
      } else {
        break;
      }
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "hermes: %s\n", e.what());
    return 2;
  }

  if (i >= argc || !std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
    usage();
    return i >= argc ? 1 : 0;
  }
  if (!std::strcmp(argv[i], "--version")) {
    std::puts("hermes 0.1.0");
    return 0;
  }
  const std::string_view cmd = argv[i];
  try {
    if (cmd == "demo") return run_demo(parse_run_args(argc, argv, i + 1, nullptr));
    if (cmd == "formats") return cmd_formats();
    if (cmd == "shell") return cli::cmd_shell(argc, argv, i + 1);
    if (cmd == "mcp") return cli::cmd_mcp(argc, argv, i + 1);
    if (cmd == "ai") return cli::cmd_ai(argc, argv, i + 1);
    if (cmd == "run") return cmd_run(parse_run_args(argc, argv, i + 1, nullptr));
    if (cmd == "load") return cmd_load(parse_run_args(argc, argv, i + 1, nullptr));
    if (cmd == "convert") return cmd_convert(argc, argv, i + 1);
    if (cmd == "detect" || cmd == "info") {
      if (i + 1 >= argc) throw std::invalid_argument(std::string(cmd) + " needs a path");
      return cmd == "detect" ? cmd_detect(argv[i + 1]) : cmd_info(argv[i + 1]);
    }
    // Bare input: hermes <dir|file> [run options]
    std::error_code ec;
    if (fs::exists(std::string(cmd), ec)) return cmd_run(parse_run_args(argc, argv, i, nullptr));
    std::fprintf(stderr, "unknown command or input not found: %s\n\n", argv[i]);
    usage();
    return 1;
  } catch (const std::exception& e) {
    HLOG_ERROR(log_app, "{}", e.what());
    return 2;
  }
}
