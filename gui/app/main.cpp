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
#include <fstream>
#include <string>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "gui/app/app.hpp"
#include "hermes/log/log.hpp"
#include "hermes/log/sinks.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "font_mono.inc"
#include "font_ui.inc"

using namespace hermes;
using namespace hermes::gui;

namespace {

App* g_app = nullptr;

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

std::string quote(const std::string& s) { return "\"" + s + "\""; }

void handle_input(App& app, float sc) {
  ImGuiIO& io = ImGui::GetIO();
  app.mouse_over_ui = io.WantCaptureMouse;
  const double mx = io.MousePos.x * sc, my = io.MousePos.y * sc;
  static double px = -1, py = -1;
  if (ImGui::IsMousePosValid() && (mx != px || my != py)) {
    app.on_mouse_move(mx, my);
    px = mx;
    py = my;
  }
  const int mods = io.KeyMods;
  if (!io.WantCaptureMouse) {
    if (ImGui::IsMouseClicked(0)) app.on_mouse_button(0, true, mx, my, mods);
    if (ImGui::IsMouseDoubleClicked(0)) app.zoom_to_selection();
    if (io.MouseWheel != 0) app.on_scroll(io.MouseWheel, mx, my);
    for (int b : {1, 2})
      if (ImGui::IsMouseDragging(b, 0.0f)) {
        const ImVec2 d = io.MouseDelta;
        app.pan_px(d.x * sc, d.y * sc);
      }
  }
  if (ImGui::IsMouseReleased(0)) app.on_mouse_button(0, false, mx, my, mods);
  if (!io.WantCaptureKeyboard) {
    static const ImGuiKey keys[] = {ImGuiKey_F,     ImGuiKey_Escape,     ImGuiKey_GraveAccent, ImGuiKey_L,
                                    ImGuiKey_I,     ImGuiKey_A,     ImGuiKey_Z,          ImGuiKey_Equal,       ImGuiKey_KeypadAdd,
                                    ImGuiKey_Minus, ImGuiKey_KeypadSubtract, ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
                                    ImGuiKey_UpArrow, ImGuiKey_DownArrow};
    for (ImGuiKey k : keys)
      if (ImGui::IsKeyPressed(k)) app.on_key(k, mods);
  } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) && app.show_console) {
    app.show_console = false;
  }
}

void load_fonts(App& app, float scale) {
  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  cfg.OversampleH = 2;
  cfg.PixelSnapH = false;
  app.font_ui = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kFontUi), static_cast<int>(kFontUi_size),
                                               15.0f * scale, &cfg);
  app.font_big = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kFontUi), static_cast<int>(kFontUi_size),
                                                19.0f * scale, &cfg);
  app.font_mono = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kFontMono),
                                                 static_cast<int>(kFontMono_size), 14.0f * scale, &cfg);
  io.FontGlobalScale = 1.0f / scale;
  io.FontDefault = app.font_ui;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string input, synth, script_file, screenshot;
  std::vector<std::string> commands, opts;
  bool exit_after = false, hidden = false, vsync = true, bench = false;
  int bench_cells = 1000000, win_w = 1600, win_h = 1000, threads = 0;
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
      std::string s = value(), cur;
      for (char ch : s + ";") {
        if (ch == ';') {
          if (cur.find_first_not_of(' ') != std::string::npos) commands.push_back(cur.substr(cur.find_first_not_of(' ')));
          cur.clear();
        } else {
          cur += ch;
        }
      }
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

  glfwSetErrorCallback([](int code, const char* msg) { std::fprintf(stderr, "glfw error %d: %s\n", code, msg); });
  if (!glfwInit()) {
    std::fprintf(stderr, "hermes_gui: cannot initialize GLFW (no display?)\n");
    return 1;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif
  if (hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  GLFWwindow* win = glfwCreateWindow(win_w, win_h, "hermes", nullptr, nullptr);
  if (!win) {
    std::fprintf(stderr, "hermes_gui: cannot create an OpenGL 3.3 window\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(win);
  glfwSwapInterval(vsync && !bench ? 1 : 0);

  int ret = 0;
  {
    App app;
    g_app = &app;
    app.set_wakeup([] { glfwPostEmptyEvent(); });
    if (threads > 0) app.session.set_job_threads(threads);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    int fbw, fbh, ww, wh;
    glfwGetFramebufferSize(win, &fbw, &fbh);
    glfwGetWindowSize(win, &ww, &wh);
    const float fb_scale = ww > 0 ? static_cast<float>(fbw) / ww : 1.0f;
    load_fonts(app, fb_scale);
    apply_theme(fb_scale);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    std::string why;
    if (!app.init_renderer(&why)) {
      std::fprintf(stderr, "hermes_gui: %s\n", why.c_str());
      return 1;
    }
    glfwSetDropCallback(win, [](GLFWwindow*, int n, const char** paths) {
      if (g_app && n > 0) g_app->run_command(std::string("open \"") + paths[0] + "\"");
    });

    // Startup commands: the input first, then -c / --script, then shortcuts.
    std::vector<std::string> script;
    std::string reader_opts;
    for (const auto& o : opts) reader_opts += " " + o;
    if (!input.empty()) script.push_back("open " + quote(input) + reader_opts);
    if (!synth.empty()) {
      const auto colon = synth.find(':');
      script.push_back("synth " + synth.substr(0, colon) + (colon == std::string::npos ? "" : " " + synth.substr(colon + 1)));
    }
    if (bench) {
      if (input.empty() && synth.empty()) script.push_back("synth " + std::to_string(bench_cells));
      script.insert(script.end(), {"wait", "settle", "bench_frames open_fit 120", "bench_frames fit_pan 120 pan",
                                   "bench_frames fit_zoom 120 zoom", "zoomfrac 0.1", "settle",
                                   "bench_frames detail_1pct 120", "bench_frames detail_1pct_pan 120 pan",
                                   "zoomfrac 0.01", "settle", "bench_frames closeup 120", "zoom fit", "settle",
                                   "place iters=60", "sleep 1500", "bench_frames during_placement_fit 120",
                                   "bench_frames during_placement_pan 120 pan", "cancel", "wait", "settle",
                                   "bench_report hermes_gui_bench.json", "info"});
      exit_after = true;
    }
    std::vector<std::string> file_lines;
    if (!script_file.empty()) {
      std::ifstream f(script_file);
      if (!f) {
        std::fprintf(stderr, "hermes_gui: cannot read %s\n", script_file.c_str());
        return 2;
      }
      for (std::string line; std::getline(f, line);) file_lines.push_back(line);
    }
    // Commands given on the command line start once the design is shown.
    if ((!input.empty() || !synth.empty()) && (!commands.empty() || !script_file.empty())) {
      script.push_back("wait");
      script.push_back("settle");
    }
    script.insert(script.end(), file_lines.begin(), file_lines.end());
    script.insert(script.end(), commands.begin(), commands.end());
    if (!screenshot.empty()) {
      script.insert(script.end(), {"wait", "settle", "frames 3", "screenshot " + quote(screenshot)});
      exit_after = true;
    }
    app.add_script(script);
    app.set_exit_after_script(exit_after);

    bool first = true;
    while (!glfwWindowShouldClose(win) && !app.wants_quit()) {
      if (first || !vsync || bench || app.animating())
        glfwPollEvents();
      else
        glfwWaitEventsTimeout(0.25);
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      glfwGetFramebufferSize(win, &fbw, &fbh);
      glfwGetWindowSize(win, &ww, &wh);
      const float sc = ww > 0 ? static_cast<float>(fbw) / ww : 1.0f;
      handle_input(app, sc);
      app.update(fbw, fbh, sc);
      draw_ui(app);
      ImGui::Render();
      app.render();
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      app.after_frame();
      glfwSwapBuffers(win);
      if (first) {
        app.ttff_ms = app.now_ms();
        if (exit_after) std::printf("time to first frame: %.0f ms\n", app.ttff_ms);
        first = false;
      }
      const std::string title = app.session.snapshot() ? "hermes - " + app.session.snapshot()->name() : "hermes";
      static std::string last_title;
      if (title != last_title) {
        glfwSetWindowTitle(win, title.c_str());
        last_title = title;
      }
    }
    app.session.cancel();
    app.session.wait();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    g_app = nullptr;
  }
  glfwDestroyWindow(win);
  glfwTerminate();
  return ret;
}
