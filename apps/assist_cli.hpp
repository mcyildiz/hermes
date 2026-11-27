#pragma once

#include <string>
#include <vector>

#include "hermes/io/common.hpp"

namespace hermes::cli {

int cmd_shell(int argc, char** argv, int i);
int cmd_mcp(int argc, char** argv, int i);
int cmd_ai(int argc, char** argv, int i);

struct ShellArgs {
  std::string input;               ///< Design to open first ("" = none).
  io::Options opts;                ///< Reader options.
  std::vector<std::string> startup;  ///< Commands run before the prompt.
  std::string runs_db, ai_config;
  int threads = 0;                 ///< Job threads (0 = default).
};

/// Interactive console on stdin (same commands as the GUI console, plus
/// 'ai'). Steps run as background jobs; a step that needs the design waits
/// for the running one (Ctrl-C cancels it).
int run_shell(const ShellArgs& a);

}  // namespace hermes::cli
