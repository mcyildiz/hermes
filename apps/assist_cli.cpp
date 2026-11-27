// hermes CLI commands for the AI integration:
//
//   hermes shell [<design>]           interactive console (same commands as hermes_gui) + `ai` chat
//   hermes mcp [<design>] [--allow-edits]   MCP server on stdio (Claude Code, Claude desktop, any MCP client)
//   hermes ai init|status|privacy|mcp-config

#include "apps/assist_cli.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "assist/ai_host.hpp"
#include "assist/mcp_server.hpp"
#include "gui/scene/session.hpp"
#include "hermes/log/log.hpp"
#include "hermes/log/sinks.hpp"
#include "hermes/par/parallel.hpp"

namespace fs = std::filesystem;

namespace hermes::cli {

using namespace hermes::assist;

namespace {

std::atomic<int> g_interrupts{0};
extern "C" void on_shell_sigint(int) { g_interrupts.fetch_add(1); }

struct Common {
  std::string input;
  io::Options opts;
  bool allow_edits = false;
  std::string runs_db;
  std::string config;
};

Common parse_common(int argc, char** argv, int i, bool mcp) {
  Common c;
  for (; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument(a + " needs a value");
      return argv[++i];
    };
    if (a == "--opt") c.opts.set(value());
    else if (a == "--threads") par::set_num_threads(std::stoi(value()));
    else if (a == "--runs-db" || a == "--track-db") c.runs_db = value();
    else if (a == "--ai-config") c.config = value();
    else if (mcp && a == "--allow-edits") c.allow_edits = true;
    else if (!a.empty() && a[0] == '-') throw std::invalid_argument("unknown option " + a);
    else if (c.input.empty()) c.input = a;
    else throw std::invalid_argument("more than one input: " + a);
  }
  return c;
}

bool load(gui::Session& s, const Common& c) {
  if (c.input.empty()) return true;
  std::string why;
  if (!s.open(c.input, c.opts, &why)) {
    std::fprintf(stderr, "hermes: %s\n", why.c_str());
    return false;
  }
  s.wait();
  if (!s.has_design()) {
    std::fprintf(stderr, "hermes: could not load %s (see the log above)\n", c.input.c_str());
    return false;
  }
  return true;
}

const char* prefix(Assistant::Entry::Kind k) {
  switch (k) {
    case Assistant::Entry::Kind::User: return "you> ";
    case Assistant::Entry::Kind::Assistant: return "ai> ";
    case Assistant::Entry::Kind::ToolCall: return "    -> ";
    case Assistant::Entry::Kind::ToolResult: return "    <- ";
    case Assistant::Entry::Kind::Error: return "ai error: ";
    default: return "ai: ";
  }
}

// Drives one answer: runs tools, prints the transcript, asks for confirmations.
void drive(AiHost& ai, gui::Session& s, gui::ViewApi& v, std::size_t& shown) {
  Assistant* a = ai.assistant();
  if (!a) return;
  g_interrupts = 0;
  bool stop_sent = false;
  while (true) {
    ai.pump(s, v);
    s.poll();
    for (const auto& e : a->transcript(shown)) {
      ++shown;
      if (e.kind == Assistant::Entry::Kind::User) continue;
      std::string t = e.text;
      if (e.kind == Assistant::Entry::Kind::ToolResult && t.size() > 240) t = t.substr(0, 240) + " ...";
      std::printf("%s%s\n", prefix(e.kind), t.c_str());
    }
    if (auto c = a->pending_confirmation()) {
      std::printf("ai wants to run %s %s\nallow? [y/N] ", c->tool.c_str(), c->args.c_str());
      std::fflush(stdout);
      std::string ans;
      if (!std::getline(std::cin, ans)) ans = "n";
      a->confirm(!ans.empty() && (ans[0] == 'y' || ans[0] == 'Y'));
      continue;
    }
    if (!a->busy() && a->transcript_size() == shown) break;
    if (g_interrupts.load() && !stop_sent) {
      a->cancel();
      stop_sent = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
}

}  // namespace

int cmd_shell(int argc, char** argv, int i) {
  Common c = parse_common(argc, argv, i, false);
  ShellArgs a;
  a.input = c.input;
  a.opts = c.opts;
  a.runs_db = c.runs_db;
  a.ai_config = c.config;
  return run_shell(a);
}

int run_shell(const ShellArgs& args) {
  gui::Session s;
  if (args.threads > 0) s.set_job_threads(args.threads);
  HeadlessView v(s);
  gui::CommandRegistry reg;
  AiHost ai(load_assistant_config(args.ai_config));
  if (!args.runs_db.empty()) ai.config().runs_db = args.runs_db;
  ai.register_commands(reg);
  std::signal(SIGINT, on_shell_sigint);
  const bool tty = isatty(0);
  gui::CommandContext ctx{s, v};
  gui::ScriptRunner sr;
  bool said_waiting = false;
  auto echo = [&](const std::string&, const gui::CommandResult& r) {
    if (!r.text.empty()) std::fprintf(r.ok ? stdout : stderr, "%s\n", r.text.c_str());
    std::fflush(stdout);
  };
  // Runs queued lines until the queue is empty (a job may still run, unless
  // `drain`: then until it ends too). Ctrl-C cancels the job and the queue.
  auto pump = [&](bool drain) {
    g_interrupts = 0;
    said_waiting = false;
    while (true) {
      sr.step(ctx, reg, echo);
      if (sr.failed()) {
        std::fprintf(stderr, "stopped: %s\n", sr.error().c_str());
        sr = gui::ScriptRunner{};
        break;
      }
      if (sr.idle() && (!drain || !s.busy())) {
        if (drain) sr.step(ctx, reg, echo);
        break;
      }
      if (!said_waiting && s.busy()) {
        std::printf("(waiting for %s; Ctrl-C cancels)\n", s.job_kind().c_str());
        std::fflush(stdout);
        said_waiting = true;
      }
      if (g_interrupts.exchange(0)) {
        s.cancel();
        if (sr.pending()) std::printf("cancelled; dropped %zu queued command(s)\n", sr.pending());
        sr.clear();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    s.poll();
  };
  if (!args.input.empty()) {
    std::string line = "open \"" + args.input + "\"";
    for (const auto& [k, val] : args.opts.all()) line += " " + k + "=" + val;
    sr.push(line);
  }
  sr.add(args.startup);
  pump(true);
  if (tty)
    std::printf("hermes shell. 'help' lists commands; steps run in the background and the next step waits for "
                "them. 'ai <question>' or '? <question>' asks the assistant (%s).\n",
                ai.config().enabled ? "on" : "off; see 'ai status'");
  std::size_t shown = 0;
  std::string line;
  while (true) {
    if (tty) {
      std::printf("hermes> ");
      std::fflush(stdout);
    }
    if (!std::getline(std::cin, line)) {
      if (g_interrupts.exchange(0) && std::cin.eof() == false) {
        std::cin.clear();
        std::printf("\n(Ctrl-D or 'quit' to leave)\n");
        continue;
      }
      break;
    }
    s.poll();
    if (line == "quit" || line == "exit") break;
    if (line.starts_with("?")) line = "ai " + line.substr(1);
    sr.push(line);
    pump(false);
    const auto w = gui::split_command_line(line);
    if (!w.empty() && w[0] == "ai") drive(ai, s, v, shown);
    if (v.quit_requested()) break;
  }
  // End of input: finish what was started (Ctrl-C cancels).
  pump(true);
  if (s.busy()) {
    s.cancel();
    s.wait();
  }
  const gui::JobEnd* e = s.last_end();
  return e && e->state == JobState::Failed ? 1 : 0;
}

int cmd_mcp(int argc, char** argv, int i) {
  // stdout is the protocol channel: all log output goes to stderr.
  log::remove_sink(log::kConsoleSinkId);
  auto err = std::make_shared<log::TextSink>(stderr);
  err->set_level(log::Level::Info);
  log::add_sink(err);
  Common c = parse_common(argc, argv, i, true);
  gui::Session s;
  HeadlessView v(s);
  gui::CommandRegistry reg;
  ToolRegistry tools;
  if (!c.input.empty() && !load(s, c)) std::fprintf(stderr, "hermes mcp: serving without a design\n");
  ToolContext ctx{s, v, reg, c.runs_db};
  McpOptions o;
  o.edits = c.allow_edits ? EditPolicy::Allow : EditPolicy::ReadOnly;
  McpServer server(tools, ctx, o);
  std::fprintf(stderr, "hermes mcp: %s, %s; waiting for an MCP client on stdin\n",
               s.has_design() ? ("design " + s.snapshot()->name()).c_str() : "no design",
               c.allow_edits ? "edits allowed (the client confirms)" : "read-only");
  std::ios::sync_with_stdio(false);
  const int rc = server.serve(std::cin, std::cout);
  if (s.busy()) {
    s.cancel();
    s.wait();
  }
  return rc;
}

int cmd_ai(int argc, char** argv, int i) {
  const std::string sub = i < argc ? argv[i] : "status";
  AssistantConfig cfg = load_assistant_config();
  if (sub == "init") {
    const std::string p = i + 1 < argc ? argv[i + 1] : default_config_path();
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::fprintf(stderr, "%s exists; not overwritten\n", p.c_str());
      return 1;
    }
    fs::create_directories(fs::path(p).parent_path(), ec);
    std::ofstream(p) << config_template();
    std::printf("wrote %s (the assistant stays off until you set enabled = true)\n", p.c_str());
    return 0;
  }
  if (sub == "status") {
    AiHost h(cfg);
    std::printf("%s\n", h.status().c_str());
    return 0;
  }
  if (sub == "privacy") {
    std::string why;
    auto p = make_provider(cfg.provider, {}, &why);
    std::printf("%s\n", sharing_text(cfg, p ? p->endpoint() : "?").c_str());
    return 0;
  }
  if (sub == "mcp-config") {
    std::error_code ec;
#ifdef __APPLE__
    char buf[4096];
    std::uint32_t sz = sizeof buf;
    std::string exe = _NSGetExecutablePath(buf, &sz) == 0 ? fs::canonical(buf, ec).string() : std::string();
#else
    std::string exe = fs::canonical("/proc/self/exe", ec).string();
#endif
    if (exe.empty()) exe = fs::absolute(argv[0], ec).string();
    std::string args = "\"mcp\"";
    for (int k = i + 1; k < argc; ++k) args += ", \"" + std::string(argv[k]) + "\"";
    std::printf(
        "Claude Code:\n  claude mcp add hermes -- %s mcp%s\n\n"
        "Claude desktop (claude_desktop_config.json) or any MCP client:\n"
        "{\n  \"mcpServers\": {\n    \"hermes\": { \"command\": \"%s\", \"args\": [%s] }\n  }\n}\n",
        exe.c_str(), [&] {
          std::string s;
          for (int k = i + 1; k < argc; ++k) s += " " + std::string(argv[k]);
          return s;
        }().c_str(),
        exe.c_str(), args.c_str());
    return 0;
  }
  std::fprintf(stderr, "usage: hermes ai init [path] | status | privacy | mcp-config [<design>] [--allow-edits]\n");
  return 1;
}

}  // namespace hermes::cli
