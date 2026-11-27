// hermes_gui: interactive viewer for hermes designs.
//
//   hermes_gui [<design dir | file>] [options]
//
//   --synth N[:ordered|random|unplaced]  generate a synthetic design
//   --opt k=v           reader option (as `hermes --opt`)
//   -c "cmd; cmd"       run console commands after start
//   --script FILE       run a command file (one command per line)
//   --exit              quit when the commands are done (with -c/--script)
//   --screenshot FILE   wait until the view is complete, save a PNG, quit
//   --bench [N]         measure frame times on a design (default synthetic 1M), print a table
//   --size WxH          window size in points (default 1600x1000)
//   --hidden            no visible window (benchmarks, screenshots)
//   --no-vsync          render as fast as possible
//   --threads N         threads for background jobs (default: all but one)
//   --log SPEC          log levels (as `hermes --log`)
//
// The window opens before anything is loaded; loading, scene building and
// engines run in the background, so the UI stays responsive.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "gui/app/launch.hpp"
#include "gui/scene/commands.hpp"
#include "hermes/log/log.hpp"

using namespace hermes;
using namespace hermes::gui;

namespace {

void usage() {
  std::puts(
      "usage: hermes_gui [<design dir | file>] [options]\n"
      "  --synth N[:ordered|random|unplaced]  generate a synthetic design\n"
      "  --opt k=v           reader option (as `hermes --opt`)\n"
      "  -c \"cmd; cmd\"       run console commands after start\n"
      "  --script FILE       run a command file (one command per line)\n"
      "  --exit              quit when the commands are done\n"
      "  --screenshot FILE   wait until the view is complete, save a PNG, quit\n"
      "  --bench [N]         frame-time benchmark (synthetic N cells, default 1000000, or the given design)\n"
      "  --size WxH          window size (default 1600x1000)\n"
      "  --hidden            no visible window\n"
      "  --no-vsync          render as fast as possible\n"
      "  --threads N         threads for background jobs (default: all but one)\n"
      "  --log SPEC          log levels, e.g. warn,gui=info\n"
      "keys: F fit  Z zoom to selection  Esc clear  ` console  L layers  I inspector  A assistant  +/- zoom  arrows pan\n"
      "mouse: drag pan, wheel zoom, click select, shift-drag box zoom, Ctrl/Cmd-drag move cell");
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  GuiLaunch L;
  std::string& input = L.input;
  std::string& synth = L.synth;
  std::string& screenshot = L.screenshot;
  std::string script_file;
  std::vector<std::string> commands;
  std::vector<std::string>& opts = L.reader_opts;
  bool& exit_after = L.exit_after;
  bool& hidden = L.hidden;
  bool& vsync = L.vsync;
  bool& bench = L.bench;
  int& bench_cells = L.bench_cells;
  int& win_w = L.width;
  int& win_h = L.height;
  int& threads = L.threads;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "hermes_gui: %s needs a value\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (a == "--synth") {
      synth = value();
    } else if (a == "--opt") {
      opts.push_back(value());
    } else if (a == "-c") {
      for (auto& c : split_commands(value())) commands.push_back(std::move(c));
    } else if (a == "--script") {
      script_file = value();
    } else if (a == "--exit") {
      exit_after = true;
    } else if (a == "--screenshot") {
      screenshot = value();
    } else if (a == "--bench") {
      bench = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') bench_cells = std::atoi(argv[++i]);
    } else if (a == "--size") {
      const std::string v = value();
      if (std::sscanf(v.c_str(), "%dx%d", &win_w, &win_h) != 2) {
        std::fprintf(stderr, "hermes_gui: --size WxH\n");
        return 2;
      }
    } else if (a == "--hidden") {
      hidden = true;
    } else if (a == "--no-vsync") {
      vsync = false;
    } else if (a == "--threads") {
      threads = std::atoi(value().c_str());
    } else if (a == "--log") {
      log::configure(value());
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "hermes_gui: unknown option %s\n", a.c_str());
      usage();
      return 2;
    } else {
      input = a;
    }
  }
  if (!script_file.empty()) {
    try {
      for (auto& l : read_command_file(script_file)) L.commands.push_back(std::move(l));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "hermes_gui: %s\n", e.what());
      return 2;
    }
  }
  L.commands.insert(L.commands.end(), commands.begin(), commands.end());
  return run_gui(L);
}
