#pragma once

// hermes_gui application state: camera, layers, selection, heatmaps, the
// scene worker and the session. Platform code (GLFW, ImGui backends) lives
// in main.cpp; panels in ui.cpp. The App implements ViewApi so console
// commands and scripts drive the same code paths as the mouse.

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "assist/ai_host.hpp"
#include "gui/app/renderer.hpp"
#include "gui/scene/commands.hpp"
#include "gui/scene/layers.hpp"
#include "gui/scene/scene.hpp"
#include "gui/scene/session.hpp"
#include "gui/scene/worker.hpp"

struct ImFont;

namespace hermes::gui {

struct Selection {
  PickResult::Kind kind = PickResult::Kind::None;
  Id id = kInvalidId;
  bool empty() const { return kind == PickResult::Kind::None; }
  bool operator==(const Selection&) const = default;
};

struct HeatmapEntry {
  std::string name;   ///< Shown in the UI ("Cell density (live)", "congestion.M3.util", ...).
  std::string key;    ///< Command name ("density", "pins", "qor:congestion.M3.util").
  std::string unit;
  bool live = false;  ///< Follows the design while engines run.
};

struct FrameStats {
  double cpu_ms = 0;       ///< App update + draw submission (no vsync wait).
  double frame_ms = 0;     ///< Wall time between frames.
  double gpu_ms = -1;
  std::size_t visible_cells = 0;
  std::size_t drawn = 0;
};

class App final : public ViewApi {
 public:
  App();
  ~App() override;

  // Platform hooks.
  void set_wakeup(std::function<void()> fn) {
    wakeup_ = std::move(fn);
    ai.set_on_update(wakeup_);
  }
  bool init_renderer(std::string* why);
  /// Per frame, before ImGui: picks up results, uploads, handles input.
  void update(int fb_w, int fb_h, float fb_scale);
  /// Draws the design (call after update, before ImGui renders).
  void render();
  /// After ImGui rendered: screenshot capture, script step, stats.
  void after_frame();
  bool wants_quit() const { return quit_; }
  /// True while something changes on screen (keep rendering).
  bool animating() const;

  // ViewApi
  void zoom_fit() override;
  void zoom_to(const Rect& world) override;
  bool set_layer(std::string_view name, int visible, int selectable, std::string* why) override;
  std::string layer_list() const override;
  bool select_cell(std::string_view name, bool zoom, std::string* why) override;
  bool select_net(std::string_view name, bool zoom, std::string* why) override;
  void clear_selection() override;
  bool set_heatmap(std::string_view name, std::string* why) override;
  std::string heatmap_list() const override;
  bool screenshot(const std::string& path, std::string* why) override;
  void request_quit() override { quit_ = true; }
  bool view_settled() const override;
  std::string stats() const override;
  std::string selection_text() const override;

  // Commands / scripts.
  /// Console input, buttons, drops: view commands (and cancel) run at once;
  /// a step that needs the design waits behind the running job (queued).
  void run_command(const std::string& line, bool echo = true);
  void add_script(const std::vector<std::string>& lines);
  bool script_running() const { return !script_.idle(); }
  std::size_t queued_commands() const { return script_.pending(); }
  /// With exit: quit once the startup commands are done and no job runs
  /// (on a failed command: exit code 1).
  void set_exit_after_script(bool on) { exit_after_script_ = on; }
  int exit_code() const { return exit_code_; }
  bool screenshot_pending() const override { return !screenshot_path_.empty(); }

  // --- state the UI reads/writes (ui.cpp) --------------------------------
  Session session;
  CommandRegistry commands;
  /// Optional AI assistant (off unless configured; see assist/config.hpp).
  assist::AiHost ai;
  bool show_chat = false;
  std::size_t chat_seen = 0;       ///< Transcript entries mirrored to the console.
  LayerSet layers;
  Selection selection, hover;
  std::vector<HeatmapEntry> heatmaps;
  int heatmap = -1;               ///< Index into heatmaps, -1 = off.
  float heat_alpha = 0.65f;
  float heat_vmin = 0, heat_vmax = 1;
  bool heat_auto_range = true;
  bool show_layers = true, show_inspector = true, show_console = false, show_stats = false;
  std::string console_input;
  std::vector<std::string> history;
  struct ConsoleLine {
    std::string text;
    int kind = 0;  // 0 output, 1 command, 2 error, 3 log info, 4 log warn, 5 log error
  };
  std::deque<ConsoleLine> console;
  std::uint64_t log_seen = 0;
  struct Toast {
    std::string text;
    int kind = 0;
    double until = 0;
  };
  std::vector<Toast> toasts;
  FrameStats fstats;
  double ttff_ms = -1, t_scene_ms = -1, t_detail_ms = -1;  ///< From process start.
  ImFont* font_ui = nullptr;
  ImFont* font_mono = nullptr;
  ImFont* font_big = nullptr;
  float ui_scale = 1.0f;          ///< Framebuffer pixels per UI point.
  bool mouse_over_ui = false;

  std::shared_ptr<const Scene> scene() const { return scene_; }
  std::shared_ptr<const Detail> detail() const { return detail_; }
  const ViewXform& view() const { return view_; }
  bool density_mode() const { return density_mode_; }
  bool detail_wanted() const { return detail_wanted_; }
  Point screen_to_world(double sx, double sy) const;  ///< Framebuffer pixels.
  void zoom_at(double sx, double sy, double factor);
  void pan_px(double dx, double dy);
  void select(Selection s, bool zoom = false);
  void zoom_to_selection();
  std::string describe(const Selection& s) const;
  double now_ms() const;
  void toast(std::string text, int kind = 0);
  double dbu_per_um() const;
  std::string um(Dbu v) const;  ///< "12.345 um" (or DBU without units).
  Renderer* renderer() { return renderer_.get(); }

  // Mouse/keyboard (from main.cpp, in framebuffer pixels).
  void on_mouse_button(int button, bool down, double x, double y, int mods);
  void on_mouse_move(double x, double y);
  void on_scroll(double dy, double x, double y);
  void on_key(int key, int mods);
  double mouse_x = 0, mouse_y = 0;
  bool dragging_cell() const { return drag_cell_ != kInvalidId; }

 private:
  void sync_design();
  void sync_heatmaps();
  void upload_scene();
  void upload_detail();
  void update_selection_geometry();
  void request_detail_if_needed();
  void step_script();
  void pull_log();
  void pull_assistant();
  void draw_heatmap_image();

  std::unique_ptr<Renderer> renderer_;
  std::unique_ptr<SceneWorker> worker_;
  std::function<void()> wakeup_;
  ViewXform view_;
  bool have_view_ = false;
  bool fit_pending_ = true;
  std::uint64_t generation_ = 0;
  std::shared_ptr<const Scene> scene_;
  std::shared_ptr<const Detail> detail_;
  const void* heat_src_ = nullptr;  // source of the uploaded heatmap image
  int heat_idx_ = -2;
  std::uint64_t requested_scene_version_ = ~std::uint64_t{0};
  double last_scene_request_ms_ = 0, last_detail_request_ms_ = 0;
  bool density_mode_ = false, detail_wanted_ = false;
  Rect detail_window_;
  DetailOptions pending_detail_;
  bool detail_pending_ = false;
  std::uint64_t detail_layers_key_ = 0;
  bool pin_density_wanted_ = false;
  // Selection geometry.
  Selection sel_built_;
  std::uint64_t sel_version_ = 0;
  // Interaction.
  bool left_down_ = false, panning_ = false, box_zoom_ = false;
  double down_x_ = 0, down_y_ = 0, last_x_ = 0, last_y_ = 0;
  double last_move_ms_ = 0;
  bool hover_done_ = true;
  Id drag_cell_ = kInvalidId;
  Point drag_offset_;
  // Screenshot / script.
  std::string screenshot_path_;
  void execute(const std::string& line, bool echo);
  void show_result(const std::string& line, const CommandResult& r, bool echo);
  ScriptRunner script_;
  bool exit_after_script_ = false;
  int exit_code_ = 0;
  bool quit_ = false;
  std::chrono::steady_clock::time_point last_frame_;
  double update_ms_ = 0;
};

/// ImGui panels (ui.cpp).
void draw_ui(App& app);
void apply_theme(float scale);

}  // namespace hermes::gui
