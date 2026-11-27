#pragma once

// Opens the hermes GUI window and runs it until it is closed. Used by
// `hermes <design>` (the default mode) and by the hermes_gui viewer.

#include <string>
#include <vector>

namespace hermes::gui {

struct GuiLaunch {
  std::string input;                  ///< Design to open ("" = none).
  std::vector<std::string> reader_opts;  ///< key=value reader options for 'open'.
  std::string synth;                  ///< "N[:ordered|random|unplaced]": a synthetic design.
  /// Commands run once the design is shown (from -c and --script, in order).
  std::vector<std::string> commands;
  bool exit_after = false;            ///< Quit when the commands are done.
  std::string screenshot;             ///< Save a PNG once the view is complete, then quit.
  bool bench = false;
  int bench_cells = 1000000;
  int width = 1600, height = 1000;
  bool hidden = false;
  bool vsync = true;
  int threads = 0;                    ///< Job threads (0 = all but one).
};

/// Returned by run_gui() when no window could be opened (no display, no
/// OpenGL 3.3): the caller may fall back to the headless console.
constexpr int kGuiUnavailable = 75;

/// Runs the GUI; returns the process exit code.
int run_gui(const GuiLaunch& launch);

/// True if a window can probably be opened (Linux: DISPLAY or
/// WAYLAND_DISPLAY is set; macOS and Windows: always).
bool display_available();

}  // namespace hermes::gui
