// hermes CLI.
//
//   hermes [<input>] [options]        open the GUI with the design loaded; run nothing
//                                     until a command is given (console, -c, --script)
//   hermes <input> --no-gui -s flow.cmd   batch: run the commands without a window, then exit
//   hermes <input> --no-gui           interactive console without a window
//   hermes load|convert|detect|info|formats ...   file utilities
//   hermes shell [<input>]            same as --no-gui without commands
//   hermes mcp [<input>]              MCP server for AI assistants (Claude Code, ...)
//
// Log options (--log, --log-file, -q, ...) may appear anywhere on the line.

#include "apps/assist_cli.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "assist/tools.hpp"
#include "gui/scene/commands.hpp"
#include "gui/scene/session.hpp"
#include "hermes/core/json.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/io/design_file.hpp"
#include "hermes/io/registry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/log/sinks.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/qor/qor.hpp"
#include "hermes/track/run_tracker.hpp"

#if HERMES_HAVE_GUI
#include "gui/app/launch.hpp"
#endif

using namespace hermes;

namespace {

HERMES_LOG_CHANNEL(log_app, "app");

void usage() {
  std::puts(
      "hermes - Holistic Engine for Routability-driven Mixed-size placement with Embedded STA\n"
      "\n"
      "usage:\n"
      "  hermes [<input>] [options]       open the GUI (with the design loaded); nothing runs on its own\n"
      "  hermes <input> -c \"place; global_route; timing\"   ... then run these commands\n"
      "  hermes <input> -s flow.cmd       ... then run a command file (one command per line)\n"
      "  hermes <input> --no-gui -s flow.cmd   batch: no window; run the commands, then exit\n"
      "  hermes <input> --no-gui          interactive console without a window (same commands)\n"
      "  hermes load <input> [--opt K=V]              read only: summary + load time\n"
      "  hermes convert <input> <output>... [--opt K=V]   write .hdb .def .pl or .aux\n"
      "  hermes detect <input>                        show which files would be read\n"
      "  hermes info <file.hdb>                       design-file header and sections\n"
      "  hermes formats                               list readers and writers\n"
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
      "commands (GUI console, shell, -c, command files; 'help' lists all, 'help <cmd>' explains one):\n"
      "  place [route=1 density=0.7 ...]   global placement + legalization (router as congestion oracle)\n"
      "  global_place / legalize            the two halves of 'place'\n"
      "  global_route [mode=full|incremental] [region=x0,y0,x1,y1]   route the current placement\n"
      "  timing [clock=PS]                  WNS/TNS        qor / report [file]   QoR report\n"
      "  flow                               place + global_route + timing + qor in one job\n"
      "  save <file.hdb|.def|.pl|.aux>      source <file>  open <path>  results  cancel  wait\n"
      "  Steps run as background jobs (the GUI stays usable). In a command file each step waits\n"
      "  for the previous one; in the GUI console a step typed while a job runs is queued.\n"
      "\n"
      "options:\n"
      "  -c, --commands \"A; B\"  commands to run after the design is loaded; repeatable\n"
      "  -s, --script FILE    command file to run (after the design is loaded); repeatable\n"
      "  --no-gui             no window: with commands, run them and exit (batch, exit code 1 on\n"
      "                       a failed command); without, an interactive console\n"
      "  --exit               GUI: quit when the commands are done\n"
      "  --opt K=V            reader option, e.g. bookshelf.scale=10, def.allow_unknown_macros=1\n"
      "  --threads N          worker threads (default: all; env HERMES_THREADS)\n"
      "  --synth N[:ordered|random|unplaced]  start from a synthetic design instead of an input\n"
      "  --size WxH, --hidden, --screenshot FILE   GUI window options (as hermes_gui)\n"
      "  Long options also work with one dash (-script, -no-gui, -exit), as in other EDA tools.\n"
      "\n"
      "run tracking (batch runs are recorded; see tools/hermes_dashboard.py):\n"
      "  --track-db PATH    run database (default ~/.hermes/runs.db; env HERMES_RUNS_DB)\n"
      "  --no-track         do not record this run\n"
      "  --track-server URL also push the run live to a dashboard server, e.g. http://host:8765\n"
      "                     (env HERMES_TRACK_SERVER; token: --track-token or HERMES_TRACK_TOKEN)\n"
      "  --run-name NAME    label shown in the dashboard\n"
      "  --tag TAG          tag for filtering/comparing runs; repeatable\n"
      "\n"
      "log options (anywhere on the line; also: environment variable HERMES_LOG=SPEC):\n"
      "  --log SPEC        levels, e.g. 'warn,place=info,place.gp=debug,route.tree=trace'\n"
      "                    (off error warn info verbose debug trace; CHAN=inherit clears)\n"
      "  --log-msg ID=V    message control: V = on | off | N (print at most N times)\n"
      "                    | unlimited; ID '*' sets the default cap for all ids\n"
      "  --log-file PATH   also write a text log (all enabled levels, with timestamps)\n"
      "  --log-json PATH   also write JSON lines (one object per message)\n"
      "  --log-time        prefix console lines with elapsed time and thread\n"
      "  -q, --quiet       console shows warnings and errors only");
}

namespace fs = std::filesystem;

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string g_cmdline;  // for run tracking

// --- File utilities (load / convert) ----------------------------------------------

struct RunArgs {
  std::string input;
  std::vector<std::string> lefs;
  std::string def;
  io::Options opts;
};

/// Arguments of load/convert. `positional` collects bare words.
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
    else if (s == "--opt") a.opts.set(value());
    else if (s == "--threads") par::set_num_threads(std::stoi(value()));
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


std::string env_or(const char* name, const std::string& v) {
  if (!v.empty()) return v;
  const char* e = std::getenv(name);
  return e ? e : "";
}

// --- Design sessions (GUI, batch, console) -------------------------------------------

struct SessionArgs {
  std::string input;
  std::string synth;
  io::Options opts;
  std::vector<std::string> commands;  ///< From -c and -s, in order.
  bool no_gui = false;
  bool exit_after = false;
  int threads = 0;
  // GUI window.
  int width = 1600, height = 1000;
  bool hidden = false;
  std::string screenshot;
  // Run tracking (batch).
  bool track = true;
  std::string track_db, track_server, track_token, run_name;
  std::vector<std::string> tags;
};

SessionArgs parse_session_args(const std::vector<std::string>& args) {
  SessionArgs a;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string s = args[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= args.size()) throw std::invalid_argument(s + " needs a value");
      return args[++i];
    };
    // One-dash long options (-script, -no-gui, -exit): EDA tool habit.
    if (s.size() > 2 && s[0] == '-' && s[1] != '-') s = "-" + s;
    for (char& ch : s)
      if (ch == '_') ch = '-';
    if (s == "-c" || s == "--commands" || s == "--cmd") {
      for (auto& l : gui::split_commands(value())) a.commands.push_back(std::move(l));
    } else if (s == "-s" || s == "--script" || s == "--source") {
      for (auto& l : gui::read_command_file(value())) a.commands.push_back(std::move(l));
    } else if (s == "--no-gui" || s == "--nogui" || s == "--batch") {
      a.no_gui = true;
    } else if (s == "--gui") {
      a.no_gui = false;
    } else if (s == "--exit") {
      a.exit_after = true;
    } else if (s == "--opt") {
      a.opts.set(value());
    } else if (s == "--threads") {
      a.threads = std::stoi(value());
      par::set_num_threads(a.threads);
    } else if (s == "--synth") {
      a.synth = value();
    } else if (s == "--size") {
      const std::string v = value();
      if (std::sscanf(v.c_str(), "%dx%d", &a.width, &a.height) != 2) throw std::invalid_argument("--size WxH");
    } else if (s == "--hidden") {
      a.hidden = true;
    } else if (s == "--screenshot") {
      a.screenshot = value();
    } else if (s == "--no-track") {
      a.track = false;
    } else if (s == "--track-db") {
      a.track_db = value();
    } else if (s == "--track-server") {
      a.track_server = value();
    } else if (s == "--track-token") {
      a.track_token = value();
    } else if (s == "--run-name") {
      a.run_name = value();
    } else if (s == "--tag") {
      a.tags.push_back(value());
    } else if (s == "--place-only" || s == "--out" || s == "--write" || s == "--checkpoint" || s == "--no-save" ||
               s == "--clock-period" || s == "--no-qor") {
      throw std::invalid_argument(
          s + " is gone: hermes no longer runs a flow on its own. Give commands instead, e.g. -c \"flow route=0; "
              "save out/design.hdb; report out/design.qor.txt\" (see 'hermes --help')");
    } else if (!s.empty() && s[0] == '-') {
      throw std::invalid_argument("unknown option " + args[i]);
    } else if (a.input.empty()) {
      a.input = args[i];
    } else {
      throw std::invalid_argument("more than one input: " + args[i]);
    }
  }
  if (!a.input.empty() && !a.synth.empty()) throw std::invalid_argument("give an input or --synth, not both");
  return a;
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

std::string open_line(const SessionArgs& a) {
  if (!a.synth.empty()) {
    const auto colon = a.synth.find(':');
    return "synth " + a.synth.substr(0, colon) + (colon == std::string::npos ? "" : " " + a.synth.substr(colon + 1));
  }
  if (a.input.empty()) return {};
  std::string l = "open " + quoted(a.input);
  for (const auto& [k, v] : a.opts.all()) l += " " + k + "=" + v;
  return l;
}

std::atomic<int> g_interrupts{0};
extern "C" void on_batch_sigint(int) {
  // First Ctrl-C: cancel the job and stop the commands. Second: default action.
  g_interrupts.fetch_add(1);
  std::signal(SIGINT, SIG_DFL);
}

std::unique_ptr<track::RunTracker> start_tracking(const SessionArgs& a) {
  if (!a.track) return nullptr;
  track::RunInfo info;
  info.name = a.run_name;
  info.input = a.input.empty() ? (a.synth.empty() ? "" : "synthetic " + a.synth) : a.input;
  info.command = g_cmdline;
  info.tags = a.tags;
  std::string cmds = "[";
  for (const auto& c : a.commands) {
    if (cmds.size() > 1) cmds += ",";
    json_escape(cmds, c);
  }
  cmds += "]";
  std::string io = "{";
  for (const auto& [k, v] : a.opts.all()) {
    std::string kv;
    json_escape(kv, k);
    kv += ':';
    json_escape(kv, v);
    io += (io.size() > 1 ? "," : "") + kv;
  }
  io += "}";
  info.settings_json = "{\"commands\":" + cmds + ",\"io\":" + io + "}";
  track::TrackerOptions to;
  to.db_path = a.track_db;
  to.server_url = env_or("HERMES_TRACK_SERVER", a.track_server);
  to.token = env_or("HERMES_TRACK_TOKEN", a.track_token);
  auto t = std::make_unique<track::RunTracker>(std::move(info), std::move(to));
  t->attach_log();
  HLOG_INFO(log_app, "run {} recorded in {}{}", t->run_id(), t->db_ok() ? t->db_path() : "(no local database)",
            to.server_url.empty() ? "" : " and pushed to the run server");
  return t;
}

/// Runs the commands without a window, then exits. Every step runs as a job
/// on the same session engine as the GUI; each waits for the previous one.
int run_batch(const SessionArgs& a) {
  gui::Session s;
  if (a.threads > 0) s.set_job_threads(a.threads);
  assist::HeadlessView v(s);
  gui::CommandRegistry reg;
  gui::CommandContext ctx{s, v};
  gui::ScriptRunner sr;
  if (const std::string o = open_line(a); !o.empty()) sr.push(o);
  sr.add(a.commands);
  auto tr = start_tracking(a);
  std::signal(SIGINT, on_batch_sigint);
  const double t0 = now_s();
  bool interrupted = false;
  auto echo = [&](const std::string& line, const gui::CommandResult& r) {
    std::printf("hermes> %s\n", line.c_str());
    std::fflush(stdout);
    if (!r.text.empty()) std::fprintf(r.ok ? stdout : stderr, "%s%s\n", r.ok ? "" : "error: ", r.text.c_str());
    std::fflush(stdout);
    if (tr) {
      const auto w = gui::split_command_line(line);
      if (!w.empty()) tr->stage(w[0]);
    }
  };
  while (true) {
    sr.step(ctx, reg, echo);
    if (sr.failed()) break;
    if (sr.idle() && !s.busy()) {
      sr.step(ctx, reg, echo);  // How the last job ended.
      break;
    }
    if (g_interrupts.load() && !interrupted) {
      interrupted = true;
      HLOG_WARN(log_app, "interrupted: cancelling {} and dropping {} command(s)", s.job_kind(), sr.pending());
      s.cancel();
      sr.clear();
    }
    if (v.quit_requested() && !s.busy()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (s.busy()) {
    s.cancel();
    s.wait();
  }
  s.poll();
  const bool failed = sr.failed();
  if (failed) HLOG_ERROR(log_app, "stopped: {}", sr.error());
  HLOG_INFO(log_app, "{} command(s) in {:.3f} s", sr.lines_run(), now_s() - t0);
  if (tr) {
    if (auto snap = s.snapshot()) {
      tr->set_design(snap->name());
      tr->summary("cells", static_cast<double>(snap->num_cells()));
      tr->summary("nets", static_cast<double>(snap->num_nets()));
    }
    tr->summary("total_s", now_s() - t0);
    if (auto p = s.last_placement()) tr->summary("iterations", static_cast<double>(p->iterations));
    if (auto t = s.last_timing()) {
      tr->summary("wns_ps", t->wns_ps);
      tr->summary("tns_ps", t->tns_ps);
    }
    if (auto r = s.last_route()) {
      tr->summary("route_wl", r->stats.wirelength);
      tr->summary("route_overflow", r->stats.total_overflow);
    }
    if (auto q = s.qor()) tr->set_qor(*q);
    else if (auto snap = s.snapshot(); snap && s.last_placement()) tr->summary("hpwl", s.last_placement()->hpwl);
    tr->finish(failed ? "failed" : interrupted ? "cancelled" : "done", failed ? 1 : 0, failed ? sr.error() : "");
  }
  return failed ? 1 : interrupted ? 130 : 0;
}

int run_session(const SessionArgs& a) {
  if (a.no_gui) {
    if (!a.commands.empty()) return run_batch(a);
  } else {
#if HERMES_HAVE_GUI
    if (gui::display_available() || a.hidden) {
      gui::GuiLaunch L;
      L.input = a.input;
      for (const auto& [k, v] : a.opts.all()) L.reader_opts.push_back(k + "=" + v);
      L.synth = a.synth;
      L.commands = a.commands;
      L.exit_after = a.exit_after;
      L.screenshot = a.screenshot;
      L.width = a.width;
      L.height = a.height;
      L.hidden = a.hidden;
      L.threads = a.threads;
      const int rc = gui::run_gui(L);
      if (rc != gui::kGuiUnavailable) return rc;
    }
    HLOG_WARN(log_app, "no window could be opened (no display?); continuing without the GUI");
#else
    HLOG_WARN(log_app, "this hermes was built without the GUI (OpenGL/GLFW not found); continuing without it");
#endif
    if (!a.commands.empty()) return run_batch(a);
  }
  cli::ShellArgs sh;
  sh.input = a.input;
  sh.opts = a.opts;
  sh.threads = a.threads;
  if (!a.synth.empty()) sh.startup.push_back(open_line(a));
  return cli::run_shell(sh);
}

int list_channels() {
  std::printf("%-28s %-8s %s\n", "channel", "level", "set");
  for (const auto& c : log::channels()) {
    std::printf("%-28s %-8s %s\n", c.name.c_str(), std::string(log::to_string(c.level)).c_str(),
                c.explicit_level ? "explicit" : "inherited");
  }
  return 0;
}

/// Takes the log options out of the command line, wherever they are, and
/// applies them. Returns the rest; *list is set by --log-channels.
std::vector<std::string> take_log_options(int argc, char** argv, bool* list) {
  std::vector<std::string> rest;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument(std::string(a) + " needs a value");
      return argv[++i];
    };
    if (a == "--log" || a == "-log") log::configure(value());
    else if (a == "--log-msg" || a == "-log-msg") log::configure("msg:" + value());
    else if (a == "--log-file" || a == "-log-file") log::add_sink(std::make_shared<log::TextSink>(value()));
    else if (a == "--log-json" || a == "-log-json") log::add_sink(std::make_shared<log::JsonLinesSink>(value()));
    else if (a == "--log-time" || a == "-log-time") log::console_sink()->set_format({true, true, false});
    else if (a == "-q" || a == "--quiet") log::console_sink()->set_level(log::Level::Warn);
    else if (a == "--log-channels") *list = true;
    else rest.emplace_back(a);
  }
  return rest;
}

}  // namespace

int main(int argc, char** argv) {
  for (int k = 0; k < argc; ++k) {
    const std::string_view w = argv[k];
    const bool quote = w.empty() || w.find_first_of(" \t\"'") != std::string_view::npos;
    g_cmdline += (k ? " " : "") + (quote ? "'" + std::string(w) + "'" : std::string(w));
  }
  std::vector<std::string> args;
  bool list = false;
  try {
    args = take_log_options(argc, argv, &list);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "hermes: %s\n", e.what());
    return 2;
  }
  if (list) return list_channels();
  if (!args.empty() && (args[0] == "--help" || args[0] == "-h" || args[0] == "-help")) {
    usage();
    return 0;
  }
  if (!args.empty() && (args[0] == "--version" || args[0] == "-version")) {
    std::puts("hermes 0.2.0");
    return 0;
  }
  // Subcommands take argc/argv: rebuild them without the log options.
  std::vector<char*> av{argv[0]};
  for (auto& a : args) av.push_back(a.data());
  av.push_back(nullptr);
  const int ac = static_cast<int>(av.size()) - 1;
  char** v = av.data();
  const std::string cmd = args.empty() ? std::string() : args[0];
  try {
    if (cmd == "formats") return cmd_formats();
    if (cmd == "shell") return cli::cmd_shell(ac, v, 2);
    if (cmd == "mcp") return cli::cmd_mcp(ac, v, 2);
    if (cmd == "ai") return cli::cmd_ai(ac, v, 2);
    if (cmd == "load") return cmd_load(parse_run_args(ac, v, 2, nullptr));
    if (cmd == "convert") return cmd_convert(ac, v, 2);
    if (cmd == "detect" || cmd == "info") {
      if (ac < 3) throw std::invalid_argument(cmd + " needs a path");
      return cmd == "detect" ? cmd_detect(v[2]) : cmd_info(v[2]);
    }
    if (cmd == "run" || cmd == "demo") {
      std::fprintf(stderr,
                   "hermes: '%s' is gone: hermes no longer runs a flow on its own. Open the design and give commands:\n"
                   "  hermes <input>                              GUI; type 'flow' or 'place' in the console\n"
                   "  hermes <input> --no-gui -c \"flow\"           batch\n"
                   "  hermes --synth 20000 --no-gui -c \"flow\"     a synthetic design (was 'hermes demo')\n",
                   cmd.c_str());
      return 2;
    }
    const SessionArgs sa = parse_session_args(args);
    if (!sa.input.empty()) {
      std::error_code ec;
      if (!fs::exists(sa.input, ec)) {
        std::fprintf(stderr, "hermes: unknown command or input not found: %s (see 'hermes --help')\n",
                     sa.input.c_str());
        return 1;
      }
    }
    return run_session(sa);
  } catch (const std::exception& e) {
    HLOG_ERROR(log_app, "{}", e.what());
    return 2;
  }
}
