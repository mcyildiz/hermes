#include "gui/app/app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <numeric>

#include "hermes/par/parallel.hpp"
#include "imgui.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_OVERRIDE
#include "stb_image_write.h"

namespace hermes::gui {

namespace {

// Process start (static init runs before main): time-to-first-frame base.
const auto g_start = std::chrono::steady_clock::now();

constexpr Color kBackground{0.059f, 0.067f, 0.082f, 1.0f};  // #0f1115
constexpr Color kSelect{1.0f, 0.82f, 0.30f, 1.0f};         // amber
constexpr Color kNetHi{0.22f, 0.90f, 1.0f, 1.0f};          // cyan
constexpr Color kHover{1.0f, 1.0f, 1.0f, 0.55f};

double pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const std::size_t k = std::min(v.size() - 1, static_cast<std::size_t>(p * (v.size() - 1) + 0.5));
  return v[k];
}

}  // namespace

// --- Benchmark recorder (bench_frames command) ------------------------------
struct BenchRun {
  std::string label;
  int frames = 0, done = 0;
  std::string motion;  // "", "pan", "zoom"
  std::vector<double> cpu, gpu, wall;
};
static std::vector<BenchRun> g_bench_done;
static BenchRun g_bench;
static bool g_bench_active = false;

App::App() {
  worker_ = std::make_unique<SceneWorker>([this] {
    if (wakeup_) wakeup_();
  });
  last_frame_ = std::chrono::steady_clock::now();
  ai.register_commands(commands);
  // App-only commands.
  commands.add({"chat", "[on|off]", "Show or hide the assistant panel (key A)", false,
                [this](CommandContext&, const std::vector<std::string>& a) {
                  show_chat = a.size() > 1 ? a[1] != "off" : !show_chat;
                  return CommandResult{true, {}, {}};
                }});
  commands.add({"frames", "<n>", "Render n frames before the next script command", false,
                [](CommandContext&, const std::vector<std::string>& a) {
                  CommandResult r;
                  auto left = std::make_shared<int>(a.size() > 1 ? std::max(1, std::atoi(a[1].c_str())) : 1);
                  r.wait_until = [left] { return --*left <= 0; };
                  return r;
                }});
  commands.add({"sleep", "<ms>", "Wait (scripts)", false, [](CommandContext&, const std::vector<std::string>& a) {
                  CommandResult r;
                  const auto until = std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(a.size() > 1 ? std::atoi(a[1].c_str()) : 100);
                  r.wait_until = [until] { return std::chrono::steady_clock::now() >= until; };
                  return r;
                }});
  commands.add({"panel", "layers|inspector|console|stats on|off", "Show or hide a panel", false,
                [this](CommandContext&, const std::vector<std::string>& a) {
                  CommandResult r;
                  if (a.size() < 3) return CommandResult{false, "usage: panel layers|inspector|console|stats on|off", {}};
                  const bool on = a[2] == "on";
                  if (a[1] == "layers") show_layers = on;
                  else if (a[1] == "inspector") show_inspector = on;
                  else if (a[1] == "console") show_console = on;
                  else if (a[1] == "stats") show_stats = on;
                  else return CommandResult{false, "no panel '" + a[1] + "'", {}};
                  return r;
                }});
  commands.add({"zoomfrac", "<f> [cx cy]", "Zoom to a window f of the die side, centered (or at cx, cy in 0..1)",
                false, [this](CommandContext&, const std::vector<std::string>& a) {
                  if (a.size() < 2 || !scene_) return CommandResult{false, "usage: zoomfrac <f> (needs a design)", {}};
                  const double f = std::atof(a[1].c_str());
                  const double cx = a.size() > 3 ? std::atof(a[2].c_str()) : 0.5;
                  const double cy = a.size() > 3 ? std::atof(a[3].c_str()) : 0.5;
                  const Rect d = scene_->die;
                  const double w = d.width() * f, h = d.height() * f;
                  const double x = d.xlo + d.width() * cx, y = d.ylo + d.height() * cy;
                  zoom_to({static_cast<Dbu>(x - w / 2), static_cast<Dbu>(y - h / 2), static_cast<Dbu>(x + w / 2),
                           static_cast<Dbu>(y + h / 2)});
                  return CommandResult{};
                }});
  commands.add({"bench_frames", "<label> <n> [pan|zoom]", "Record frame times over n frames (benchmarks)", false,
                [](CommandContext&, const std::vector<std::string>& a) {
                  CommandResult r;
                  if (a.size() < 3) return CommandResult{false, "usage: bench_frames <label> <n> [pan|zoom]", {}};
                  g_bench = {};
                  g_bench.label = a[1];
                  g_bench.frames = std::max(1, std::atoi(a[2].c_str()));
                  g_bench.motion = a.size() > 3 ? a[3] : "";
                  g_bench_active = true;
                  r.wait_until = [] { return !g_bench_active; };
                  return r;
                }});
  commands.add({"bench_report", "[file.json]", "Print (and save) recorded timings", false,
                [this](CommandContext&, const std::vector<std::string>& a) {
                  std::string t = std::format("time to first frame {:.0f} ms, first scene {:.0f} ms, first detail {:.0f} ms\n",
                                              ttff_ms, t_scene_ms, t_detail_ms);
                  t += std::format("{:<28} {:>7} {:>9} {:>9} {:>9} {:>9} {:>9}\n", "run", "frames", "cpu p50", "cpu p95",
                                   "gpu p50", "gpu p95", "wall p95");
                  std::string json = std::format("{{\"ttff_ms\":{:.1f},\"first_scene_ms\":{:.1f},\"first_detail_ms\":{:.1f},"
                                                 "\"renderer\":\"{}\",\"runs\":[",
                                                 ttff_ms, t_scene_ms, t_detail_ms, renderer_ ? renderer_->describe() : "");
                  bool first = true;
                  for (const BenchRun& b : g_bench_done) {
                    t += std::format("{:<28} {:>7} {:>9.2f} {:>9.2f} {:>9.2f} {:>9.2f} {:>9.2f}\n", b.label, b.done,
                                     pct(b.cpu, 0.5), pct(b.cpu, 0.95), pct(b.gpu, 0.5), pct(b.gpu, 0.95),
                                     pct(b.wall, 0.95));
                    json += std::format("{}{{\"label\":\"{}\",\"frames\":{},\"cpu_p50\":{:.3f},\"cpu_p95\":{:.3f},"
                                        "\"gpu_p50\":{:.3f},\"gpu_p95\":{:.3f},\"wall_p50\":{:.3f},\"wall_p95\":{:.3f},"
                                        "\"wall_max\":{:.3f}}}",
                                        first ? "" : ",", b.label, b.done, pct(b.cpu, 0.5), pct(b.cpu, 0.95),
                                        pct(b.gpu, 0.5), pct(b.gpu, 0.95), pct(b.wall, 0.5), pct(b.wall, 0.95),
                                        pct(b.wall, 1.0));
                    first = false;
                  }
                  json += "]}\n";
                  if (a.size() > 1) {
                    std::ofstream(a[1]) << json;
                    t += "saved " + a[1];
                  }
                  return CommandResult{true, t, {}};
                }});
}

App::~App() {
  session.cancel();
  worker_.reset();
}

double App::now_ms() const {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
}

bool App::init_renderer(std::string* why) {
  renderer_ = make_gl_renderer();
  return renderer_->init(why);
}

void App::toast(std::string text, int kind) { toasts.push_back({std::move(text), kind, now_ms() + 3500}); }

double App::dbu_per_um() const {
  auto s = scene_ ? scene_->snap : nullptr;
  return s && s->dbu_per_micron() > 0 ? s->dbu_per_micron() : 0.0;
}

std::string App::um(Dbu v) const {
  const double d = dbu_per_um();
  if (d <= 0) return std::format("{}", v);
  return std::format("{:.3f}", static_cast<double>(v) / d);
}

// --- Commands / script --------------------------------------------------------

void App::show_result(const std::string& line, const CommandResult& r, bool echo) {
  if (echo) console.push_back({"> " + line, 1});
  if (!r.text.empty()) {
    std::size_t b = 0;
    while (b <= r.text.size()) {
      std::size_t e = r.text.find('\n', b);
      if (e == std::string::npos) e = r.text.size();
      console.push_back({r.text.substr(b, e - b), r.ok ? 0 : 2});
      b = e + 1;
    }
    if (exit_after_script_ && !line.starts_with("screenshot"))
      std::printf("%s%s\n", r.ok ? "" : "error: ", r.text.c_str());
  }
  if (!r.ok) toast(r.text, 2);
  while (console.size() > 4000) console.pop_front();
}

void App::execute(const std::string& line, bool echo) {
  CommandContext ctx{session, *this};
  const CommandResult r = commands.run(ctx, line);
  show_result(line, r, echo);
  if (r.ok && !r.insert_lines.empty()) script_.add(r.insert_lines);
}

void App::run_command(const std::string& line, bool echo) {
  const auto w = split_command_line(line);
  if (w.empty()) return;
  const Command* c = commands.find(w[0]);
  if (!c || c->immediate || w[0] == "quit") {
    execute(line, echo);
    if (w[0] == "cancel" && script_.pending()) {
      console.push_back({std::format("dropped {} queued command(s)", script_.pending()), 0});
      script_.clear();
    }
    return;
  }
  if (session.busy() || !script_.idle()) {
    if (echo) console.push_back({"> " + line, 1});
    console.push_back({"queued: runs when " + (session.busy() ? session.job_kind() : std::string("the queue")) +
                           " is done ('cancel' drops the queue)",
                       0});
    script_.push(line + " ");  // trailing space: already echoed
    return;
  }
  script_.push(line);
  step_script();
}

void App::add_script(const std::vector<std::string>& lines) { script_.add(lines); }

void App::step_script() {
  if (!screenshot_path_.empty()) return;
  CommandContext ctx{session, *this};
  script_.step(ctx, commands, [this](const std::string& line, const CommandResult& r) {
    const bool echoed = !line.empty() && line.back() == ' ';
    if (exit_after_script_) std::printf("> %s\n", line.c_str());
    show_result(line, r, !echoed);
  });
  if (script_.failed()) {
    const std::string msg = "commands stopped: " + script_.error();
    console.push_back({msg, 2});
    toast(msg, 2);
    if (exit_after_script_) {
      std::fprintf(stderr, "hermes: %s\n", msg.c_str());
      exit_code_ = 1;
      quit_ = true;
    }
    script_ = ScriptRunner{};
    return;
  }
  if (exit_after_script_ && script_.idle() && !session.busy() && screenshot_path_.empty()) quit_ = true;
}

void App::pull_log() {
  if (session.log_count() == log_seen) return;
  const std::uint64_t now = session.log_count();
  const auto lines = session.log_lines(static_cast<std::size_t>(std::min<std::uint64_t>(now - log_seen, 2000)));
  for (const LogLine& l : lines) {
    const int kind = l.level == log::Level::Error ? 5 : l.level == log::Level::Warn ? 4 : 3;
    console.push_back({std::format("[{}] {}", l.channel, l.text), kind});
    if (l.channel == "gui" && kind >= 4) toast(l.text, kind == 5 ? 2 : 1);
    else if (l.channel == "gui" && (l.text.find("finished") != std::string::npos ||
                                     l.text.find("ready") != std::string::npos))
      toast(l.text, 0);
  }
  log_seen = now;
  while (console.size() > 4000) console.pop_front();
}

// --- ViewApi --------------------------------------------------------------------

void App::zoom_fit() {
  if (!scene_) {
    fit_pending_ = true;
    return;
  }
  const Rect d = scene_->die;
  const double m = 0.04;
  zoom_to({d.xlo - static_cast<Dbu>(d.width() * m), d.ylo - static_cast<Dbu>(d.height() * m),
           d.xhi + static_cast<Dbu>(d.width() * m), d.yhi + static_cast<Dbu>(d.height() * m)});
  fit_pending_ = false;
}

void App::zoom_to(const Rect& r) {
  const double w = std::max<double>(1.0, static_cast<double>(r.width()));
  const double h = std::max<double>(1.0, static_cast<double>(r.height()));
  view_.cx = 0.5 * (static_cast<double>(r.xlo) + static_cast<double>(r.xhi));
  view_.cy = 0.5 * (static_cast<double>(r.ylo) + static_cast<double>(r.yhi));
  // Leave room for the side panels when they are shown.
  const double usable_w = std::max(100.0, view_.fb_w - (show_layers ? 290.0 * ui_scale : 0.0) -
                                              (show_inspector ? 350.0 * ui_scale : 0.0));
  const double usable_h = std::max(100.0, view_.fb_h - 90.0 * ui_scale);
  view_.scale = std::min(usable_w / w, usable_h / h);
  view_.cx -= ((show_layers ? 290.0 * ui_scale : 0.0) - (show_inspector ? 350.0 * ui_scale : 0.0)) * 0.5 / view_.scale;
  have_view_ = true;
  fit_pending_ = false;
}

bool App::set_layer(std::string_view name, int visible, int selectable, std::string* why) {
  const int i = layers.find(name);
  if (i < 0) {
    if (why) *why = "no layer '" + std::string(name) + "' (see 'layers')";
    return false;
  }
  if (visible >= 0) layers[i].visible = visible != 0;
  if (selectable >= 0) layers[i].selectable = selectable != 0;
  return true;
}

std::string App::layer_list() const {
  std::string s;
  for (int i = 0; i < layers.size(); ++i)
    s += std::format("{:<12} {:<8} {}{}\n", layers[i].name, layers[i].group, layers[i].visible ? "shown" : "hidden",
                     layers[i].can_select ? (layers[i].selectable ? ", selectable" : ", not selectable") : "");
  if (!s.empty()) s.pop_back();
  return s;
}

bool App::select_cell(std::string_view name, bool zoom, std::string* why) {
  auto snap = session.snapshot();
  if (!snap) {
    if (why) *why = "no design loaded";
    return false;
  }
  const Id id = snap->find_cell(name);
  if (id == kInvalidId) {
    if (why) *why = "no cell named '" + std::string(name) + "'";
    return false;
  }
  select({PickResult::Kind::Cell, id}, zoom);
  return true;
}

bool App::select_net(std::string_view name, bool zoom, std::string* why) {
  auto snap = session.snapshot();
  if (!snap) {
    if (why) *why = "no design loaded";
    return false;
  }
  const Id id = snap->find_net(name);
  if (id == kInvalidId) {
    if (why) *why = "no net named '" + std::string(name) + "'";
    return false;
  }
  select({PickResult::Kind::Net, id}, zoom);
  return true;
}

void App::clear_selection() { selection = {}; }

void App::select(Selection s, bool zoom) {
  selection = s;
  if (zoom) zoom_to_selection();
}

void App::zoom_to_selection() {
  auto snap = session.snapshot();
  if (!snap || selection.empty()) return;
  Rect r;
  if (selection.kind == PickResult::Kind::Cell) {
    r = snap->cell(selection.id).bbox();
    const Dbu m = std::max<Dbu>(r.width(), r.height()) * 6;
    r = {r.xlo - m, r.ylo - m, r.xhi + m, r.yhi + m};
  } else if (selection.kind == PickResult::Kind::Net) {
    r = snap->net_bbox(selection.id);
    const Dbu m = std::max<Dbu>({r.width(), r.height(), snap->row_height() * 4}) / 4;
    r = {r.xlo - m, r.ylo - m, r.xhi + m, r.yhi + m};
  } else {
    const Point p = snap->pin_location(selection.id);
    const Dbu m = std::max<Dbu>(snap->row_height() * 6, 100);
    r = {p.x - m, p.y - m, p.x + m, p.y + m};
  }
  zoom_to(r);
}

void App::sync_heatmaps() {
  heatmaps.clear();
  heatmaps.push_back({"Cell density (live)", "density", "area ratio", true});
  heatmaps.push_back({"Pin density (live)", "pins", "pins/um^2", true});
  if (auto q = session.qor())
    for (const QorMap& m : q->maps) heatmaps.push_back({m.name, "qor:" + m.name, m.unit, false});
}

bool App::set_heatmap(std::string_view name, std::string* why) {
  sync_heatmaps();
  if (name == "off" || name == "none") {
    heatmap = -1;
    return true;
  }
  for (std::size_t i = 0; i < heatmaps.size(); ++i) {
    const HeatmapEntry& h = heatmaps[i];
    if (h.key == name || h.name == name || h.key == "qor:" + std::string(name)) {
      heatmap = static_cast<int>(i);
      heat_auto_range = true;
      return true;
    }
  }
  if (why) *why = "no heatmap '" + std::string(name) + "' (see 'heatmaps'; QoR maps appear after 'qor' or a flow)";
  return false;
}

std::string App::heatmap_list() const {
  std::string s;
  for (const HeatmapEntry& h : heatmaps) s += std::format("{:<34} {}{}\n", h.key, h.name, h.live ? "" : " (QoR)");
  if (!session.qor()) s += "(QoR maps: run 'qor' or a flow)";
  else if (!s.empty()) s.pop_back();
  return s;
}

bool App::screenshot(const std::string& path, std::string* why) {
  if (path.empty()) {
    if (why) *why = "empty path";
    return false;
  }
  screenshot_path_ = path;
  return true;
}

bool App::view_settled() const {
  if (!session.has_design()) return !session.busy();
  if (session.busy()) return false;
  auto snap = session.snapshot();
  if (!scene_ || !snap || scene_->version != snap->version()) return false;
  if (!worker_->idle()) return false;
  if (detail_wanted_) {
    if (!detail_ || detail_->version != scene_->version) return false;
  }
  return true;
}

std::string App::stats() const {
  std::string s;
  auto snap = session.snapshot();
  if (snap) {
    s += std::format("design {} ({}): {} cells, {} nets, {} pins, version {}\n", snap->name(), session.source(),
                     snap->num_cells(), snap->num_nets(), snap->num_pins(), snap->version());
    s += std::format("loaded in {:.3f} s\n", session.load_seconds());
  } else {
    s += "no design\n";
  }
  if (scene_)
    s += std::format("scene: {} std cells, {} macros, {}x{} tiles, built in {:.1f} ms (density {:.1f} ms)\n",
                     scene_->num_std, scene_->num_macros, scene_->tiles_x, scene_->tiles_y, scene_->build_ms,
                     scene_->density_ms);
  if (detail_)
    s += std::format("detail: {} pins, {} nets, {} wires, {} vias in {:.1f} ms{}\n", detail_->pins.size(), detail_->nets,
                     detail_->wires.size(), detail_->vias.size(), detail_->build_ms,
                     detail_->truncated ? " (truncated)" : "");
  s += std::format("view: {:.4g} um/px, mode {}, {} cells in view\n", view_.world_per_px() / std::max(1e-12, dbu_per_um() > 0 ? dbu_per_um() : 1.0),
                   density_mode_ ? "density" : detail_wanted_ ? "cells+detail" : "cells", fstats.visible_cells);
  s += std::format("frame: cpu {:.2f} ms, gpu {:.2f} ms, wall {:.1f} ms; time to first frame {:.0f} ms, first scene {:.0f} ms\n",
                   fstats.cpu_ms, fstats.gpu_ms, fstats.frame_ms, ttff_ms, t_scene_ms);
  if (renderer_) s += std::format("renderer: {}, {:.1f} MB on GPU\n", renderer_->describe(), renderer_->gpu_bytes() / 1e6);
  s += std::format("jobs: {} ({} threads of {})", session.busy() ? session.job_kind() : "idle", session.job_threads(),
                   par::hardware_threads());
  return s;
}

std::string App::selection_text() const {
  const std::string d = describe(selection);
  if (d.empty()) return {};
  return selection.kind == PickResult::Kind::Cell ? "cell " + d : d;
}

void App::pull_assistant() {
  assist::Assistant* a = ai.assistant();
  if (!a) return;
  if (a->transcript_size() < chat_seen) chat_seen = 0;  // reset
  for (const auto& e : a->transcript(chat_seen)) {
    ++chat_seen;
    using K = assist::Assistant::Entry::Kind;
    switch (e.kind) {
      case K::Assistant:
        console.push_back({"ai> " + e.text, 0});
        break;
      case K::ToolCall:
        console.push_back({"    ai runs " + e.text, 3});
        break;
      case K::Error:
        console.push_back({"ai: " + e.text, 2});
        toast("Assistant: " + e.text, 2);
        break;
      case K::Info:
        console.push_back({"ai: " + e.text, 3});
        break;
      default:
        break;
    }
  }
  if (auto c = a->pending_confirmation(); c && !show_chat) {
    show_chat = true;
    toast("The assistant asks to run " + c->title + ": confirm in the Assistant panel", 1);
  }
}

std::string App::describe(const Selection& sel) const {
  auto snap = session.snapshot();
  if (!snap || sel.empty()) return {};
  switch (sel.kind) {
    case PickResult::Kind::Cell: {
      const Cell& c = snap->cell(sel.id);
      return std::format("{} ({}{})", snap->cell_name(sel.id), c.is_macro ? "macro " : "", snap->cell_master(sel.id));
    }
    case PickResult::Kind::Net:
      return std::format("net {} ({} pins)", snap->net_name(sel.id), snap->net_pins(sel.id).size());
    case PickResult::Kind::Pin: {
      const Pin& p = snap->pin(sel.id);
      return p.cell == kInvalidId ? std::format("I/O pin {}", snap->pin_name(sel.id))
                                  : std::format("pin {}/{}", snap->cell_name(p.cell), snap->pin_name(sel.id));
    }
    default:
      return {};
  }
}

// --- Frame ------------------------------------------------------------------------

Point App::screen_to_world(double sx, double sy) const {
  return {static_cast<Dbu>(std::llround(view_.cx + (sx - view_.fb_w * 0.5) / view_.scale)),
          static_cast<Dbu>(std::llround(view_.cy - (sy - view_.fb_h * 0.5) / view_.scale))};
}

void App::zoom_at(double sx, double sy, double f) {
  const double wx = view_.cx + (sx - view_.fb_w * 0.5) / view_.scale;
  const double wy = view_.cy - (sy - view_.fb_h * 0.5) / view_.scale;
  double s = view_.scale * f;
  if (scene_) {
    const double die = std::max<double>(scene_->die.width(), scene_->die.height());
    s = std::clamp(s, 0.02 * view_.fb_w / std::max(1.0, die), 64.0);  // at most 64 px per DBU
  }
  view_.scale = s;
  view_.cx = wx - (sx - view_.fb_w * 0.5) / s;
  view_.cy = wy + (sy - view_.fb_h * 0.5) / s;
}

void App::pan_px(double dx, double dy) {
  view_.cx -= dx / view_.scale;
  view_.cy += dy / view_.scale;
}

void App::sync_design() {
  if (session.generation() == generation_) return;
  generation_ = session.generation();
  worker_->reset();
  scene_.reset();
  detail_.reset();
  selection = hover = {};
  sel_built_ = {};
  requested_scene_version_ = ~std::uint64_t{0};
  if (auto t = session.tech()) layers.set_metals(t->routes.stack());
  fit_pending_ = true;
  heatmap = -1;
  heat_src_ = nullptr;
  heat_idx_ = -2;
  if (renderer_)
    for (int b = 0; b < static_cast<int>(BufferId::Count); ++b) renderer_->upload(static_cast<BufferId>(b), {});
  sync_heatmaps();
}

void App::upload_scene() {
  Renderer& r = *renderer_;
  r.upload(BufferId::Cells, scene_->cells);
  r.upload(BufferId::Macros, scene_->macros);
  r.upload(BufferId::IoPins, scene_->io_pins);
  r.upload(BufferId::Rows, scene_->rows);
  r.upload_image(ImageId::Density, scene_->density);
  const float w = static_cast<float>(scene_->die.width()), h = static_cast<float>(scene_->die.height());
  const std::uint32_t st = style::make(LayerSet::kDie);
  const Instance die[] = {{0, 0, w, 0, st}, {w, 0, w, h, st}, {w, h, 0, h, st}, {0, h, 0, 0, st}};
  r.upload(BufferId::Die, die);
  // GCell grid lines.
  if (auto t = session.tech()) {
    const BinGrid& g = t->routes.grid();
    std::vector<Instance> lines;
    const std::uint32_t gs = style::make(LayerSet::kGcellGrid);
    const float gx0 = scene_->fx(g.region().xlo), gy0 = scene_->fy(g.region().ylo);
    const float gx1 = scene_->fx(g.region().xhi), gy1 = scene_->fy(g.region().yhi);
    for (int ix = 0; ix <= g.nx(); ++ix) {
      const float x = ix == g.nx() ? gx1 : scene_->fx(g.bin_rect(ix, 0).xlo);
      lines.push_back({x, gy0, x, gy1, gs});
    }
    for (int iy = 0; iy <= g.ny(); ++iy) {
      const float y = iy == g.ny() ? gy1 : scene_->fy(g.bin_rect(0, iy).ylo);
      lines.push_back({gx0, y, gx1, y, gs});
    }
    r.upload(BufferId::Grid, lines);
  }
  sel_version_ = 0;     // selection geometry follows the scene
}

void App::upload_detail() {
  renderer_->upload(BufferId::Pins, detail_->pins);
  renderer_->upload(BufferId::Wires, detail_->wires);
  renderer_->upload(BufferId::Vias, detail_->vias);
}

void App::request_detail_if_needed() {
  detail_wanted_ = false;
  auto tech = session.tech();
  if (!scene_ || !tech || density_mode_) return;
  // Wires and vias only once cells are clearly visible (else they cover
  // everything); pins a bit later still.
  const double cell_h_px = scene_->avg_cell_h * view_.scale;
  const bool close = cell_h_px >= 16.0 || fstats.visible_cells <= 4000;
  const bool pins = layers.visible(LayerSet::kPins) && cell_h_px >= 10.0;
  const bool wires = close && layers.any_metal_visible();
  const bool vias = close && layers.visible(LayerSet::kVias);
  if (!pins && !wires && !vias) return;
  if (fstats.visible_cells > 300000) return;  // wires would be a carpet; density/cells say more
  detail_wanted_ = true;
  const double hw = view_.fb_w * 0.5 / view_.scale, hh = view_.fb_h * 0.5 / view_.scale;
  const Rect vis{static_cast<Dbu>(view_.cx - hw), static_cast<Dbu>(view_.cy - hh), static_cast<Dbu>(view_.cx + hw),
                 static_cast<Dbu>(view_.cy + hh)};
  const std::uint64_t key = (pins ? 1 : 0) | (wires ? 2 : 0) | (vias ? 4 : 0);
  auto covers = [&](const Rect& w) {
    return w.xlo <= vis.xlo && w.ylo <= vis.ylo && w.xhi >= vis.xhi && w.yhi >= vis.yhi;
  };
  const bool have = detail_ && detail_->version == scene_->version && covers(detail_->window) &&
                    detail_layers_key_ == key;
  const bool asked = detail_pending_ && covers(pending_detail_.window) && detail_layers_key_ == key &&
                     requested_scene_version_ == scene_->version;
  if (have || asked) return;
  // While a job publishes, keep a stale detail a little longer (saves work).
  if (detail_ && session.busy() && covers(detail_->window) && now_ms() - last_detail_request_ms_ < 500) return;
  DetailOptions o;
  o.window = {vis.xlo - static_cast<Dbu>(hw), vis.ylo - static_cast<Dbu>(hh), vis.xhi + static_cast<Dbu>(hw),
              vis.yhi + static_cast<Dbu>(hh)};
  o.pins = pins;
  o.wires = wires;
  o.vias = vias;
  pending_detail_ = o;
  detail_pending_ = true;
  detail_layers_key_ = key;
  last_detail_request_ms_ = now_ms();
  worker_->request_detail(scene_, tech, o);
}

void App::update_selection_geometry() {
  auto snap = scene_ ? scene_->snap : nullptr;
  auto tech = session.tech();
  if (!snap || !tech) return;
  if (sel_built_ == selection && sel_version_ == snap->version()) return;
  sel_built_ = selection;
  sel_version_ = snap->version();
  std::vector<Instance> rects, wires, marks;
  const std::uint32_t rs = style::make(LayerSet::kStdCells);
  auto add_cell = [&](Id c) {
    const Rect b = snap->cell(c).bbox();
    rects.push_back({scene_->fx(b.xlo), scene_->fy(b.ylo), scene_->fx(b.xhi), scene_->fy(b.yhi), rs});
  };
  auto add_pin = [&](Id p) {
    const Point q = snap->pin_location(p);
    marks.push_back({scene_->fx(q.x), scene_->fy(q.y), 3.0f, 0, style::make(LayerSet::kPins, style::kMarker)});
  };
  auto add_net = [&](Id n) {
    if (snap->net_pins(n).size() > 5000) return;  // huge nets: pins only
    net_wires(*snap, *tech, n, *scene_, wires);
  };
  if (selection.kind == PickResult::Kind::Cell && static_cast<std::size_t>(selection.id) < snap->num_cells()) {
    add_cell(selection.id);
    int nets = 0;
    for (Id p : snap->cell_pins(selection.id)) {
      add_pin(p);
      const Id n = snap->pin(p).net;
      if (n != kInvalidId && nets++ < 200) add_net(n);
    }
  } else if (selection.kind == PickResult::Kind::Net && static_cast<std::size_t>(selection.id) < snap->num_nets()) {
    add_net(selection.id);
    for (Id p : snap->net_pins(selection.id)) add_pin(p);
  } else if (selection.kind == PickResult::Kind::Pin && static_cast<std::size_t>(selection.id) < snap->num_pins()) {
    add_pin(selection.id);
    if (snap->pin(selection.id).net != kInvalidId) add_net(snap->pin(selection.id).net);
  }
  for (Instance& w : wires) w.style = style::make(LayerSet::kStdCells);  // drawn with override color
  renderer_->upload(BufferId::SelRects, rects);
  renderer_->upload(BufferId::SelWires, wires);
  renderer_->upload(BufferId::SelMarkers, marks);
}

void App::update(int fb_w, int fb_h, float fb_scale) {
  const auto t0 = std::chrono::steady_clock::now();
  view_.fb_w = std::max(1, fb_w);
  view_.fb_h = std::max(1, fb_h);
  ui_scale = fb_scale;
  session.poll();
  pull_log();
  ai.pump(session, *this);
  pull_assistant();
  sync_design();
  if (session.qor() && heatmaps.size() <= 2) sync_heatmaps();
  if (!session.qor() && heatmaps.size() > 2) sync_heatmaps();

  // Scene: rebuild whenever a newer snapshot is out (latest wins).
  if (auto snap = session.snapshot()) {
    const bool want_pd = heatmap == 1;
    const bool pd_missing = want_pd && scene_ && scene_->pin_density.empty();
    if (snap->version() != requested_scene_version_ || (pd_missing && !pin_density_wanted_)) {
      pin_density_wanted_ = want_pd;
      SceneOptions o;
      o.pin_density = want_pd;
      worker_->request_scene(snap, o);
      requested_scene_version_ = snap->version();
      last_scene_request_ms_ = now_ms();
    }
    if (!want_pd) pin_density_wanted_ = false;
  }
  if (auto sc = worker_->scene(); sc && sc != scene_) {
    scene_ = sc;
    view_.origin = scene_->origin;
    upload_scene();
    if (t_scene_ms < 0) t_scene_ms = now_ms();
  }
  if (scene_ && (fit_pending_ || !have_view_)) zoom_fit();
  view_.origin = scene_ ? scene_->origin : Point{};

  // Level of detail.
  fstats.visible_cells = 0;
  if (scene_) {
    density_mode_ = scene_->num_std > 0 && scene_->avg_cell_h * view_.scale < 2.5;
    const double hw = view_.fb_w * 0.5 / view_.scale, hh = view_.fb_h * 0.5 / view_.scale;
    int ix0, iy0, ix1, iy1;
    scene_->tiles_for({static_cast<Dbu>(view_.cx - hw), static_cast<Dbu>(view_.cy - hh), static_cast<Dbu>(view_.cx + hw),
                       static_cast<Dbu>(view_.cy + hh)},
                      ix0, iy0, ix1, iy1);
    for (int iy = iy0; iy <= iy1; ++iy) {
      const auto [b, e] = scene_->tile_row_range(iy, ix0, ix1);
      fstats.visible_cells += e - b;
    }
  }
  request_detail_if_needed();
  if (auto d = worker_->detail(); d && d != detail_) {
    detail_ = d;
    detail_pending_ = false;
    upload_detail();
    if (t_detail_ms < 0) t_detail_ms = now_ms();
  }
  update_selection_geometry();

  // Heatmap image: upload when the selection or its source changes.
  if (heatmap >= 0 && heatmap < static_cast<int>(heatmaps.size()) && scene_) {
    const HeatmapEntry& h = heatmaps[static_cast<std::size_t>(heatmap)];
    const ScalarImage* img = nullptr;
    const void* src = nullptr;
    ScalarImage tmp;
    std::shared_ptr<const QorReport> q;
    if (h.key == "density") {
      img = &scene_->density;
      src = scene_.get();
    } else if (h.key == "pins") {
      if (!scene_->pin_density.empty()) {
        img = &scene_->pin_density;
        src = scene_.get();
      }
    } else if ((q = session.qor())) {
      src = q.get();
      if (src != heat_src_ || heatmap != heat_idx_) {
        if (const QorMap* m = q->map(h.key.substr(4))) {
          tmp.w = m->nx;
          tmp.h = m->ny;
          tmp.region = m->region;
          tmp.values = m->values;
          tmp.vmin = tmp.values.empty() ? 0 : *std::min_element(tmp.values.begin(), tmp.values.end());
          tmp.vmax = tmp.values.empty() ? 1 : *std::max_element(tmp.values.begin(), tmp.values.end());
          img = &tmp;
        }
      }
    }
    if (img && (src != heat_src_ || heatmap != heat_idx_)) {
      renderer_->upload_image(ImageId::Heatmap, *img);
      if (heat_auto_range || heatmap != heat_idx_) {
        heat_vmin = std::min(0.0f, img->vmin);
        heat_vmax = img->vmax > heat_vmin ? img->vmax : heat_vmin + 1.0f;
      }
      heat_src_ = src;
      heat_idx_ = heatmap;
    }
  }

  // Hover tooltip target (after the mouse rests).
  if (!hover_done_ && now_ms() - last_move_ms_ > 250 && scene_ && !mouse_over_ui && !script_running()) {
    PickContext ctx{scene_.get(), detail_wanted_ ? detail_.get() : nullptr, &layers, view_.world_per_px(), 4.0 * std::max(1.0f, ui_scale)};
    const PickResult r = pick(ctx, screen_to_world(mouse_x, mouse_y));
    hover = {r.kind, r.id};
    hover_done_ = true;
  }

  // Benchmark motion.
  if (g_bench_active) {
    if (g_bench.motion == "pan") pan_px(view_.fb_w * 0.004, 0);
    if (g_bench.motion == "zoom") zoom_at(view_.fb_w * 0.5, view_.fb_h * 0.5, (g_bench.done / 30) % 2 ? 1.03 : 1 / 1.03);
  }
  update_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void App::render() {
  const auto t0 = std::chrono::steady_clock::now();
  Renderer& r = *renderer_;
  r.begin_frame(view_, kBackground);
  if (scene_) {
    DrawStyle st;
    for (int i = 0; i < layers.size() && i < LayerSet::kMaxLayers; ++i) st.palette[static_cast<std::size_t>(i)] = layers[i].color;
    st.visible_mask = layers.visible_mask();
    const double row_px = scene_->snap->row_height() * view_.scale;
    const double cell_w_px = scene_->avg_cell_w * view_.scale;

    // Rows (faint, only when they are a few pixels apart).
    if (layers.visible(LayerSet::kRows) && row_px > 5) {
      DrawStyle s = st;
      s.line_px = 1.0f;
      s.alpha = static_cast<float>(std::clamp((row_px - 5) / 10, 0.0, 1.0));
      r.draw_segments(BufferId::Rows, 0, r.size(BufferId::Rows), s);
    }
    // Std cells: density image when cells are below ~2 px, else rectangles of the tiles in view.
    std::size_t drawn = 0;
    if (density_mode_) {
      if (layers.visible(LayerSet::kStdCells)) {
        ImageStyle is;
        is.mono = true;
        is.color = layers[LayerSet::kStdCells].color;
        is.vmin = 0;
        is.vmax = std::max(1.0f, scene_->density.vmax);
        is.alpha = 0.95f;
        is.linear = false;
        r.draw_image(ImageId::Density, is);
      }
    } else {
      DrawStyle s = st;
      s.fill_alpha = cell_w_px > 8 ? 0.42f : 0.8f;
      s.border_px = cell_w_px > 8 ? 1.0f : 0.0f;
      s.min_px = 1.0f;
      const double hw = view_.fb_w * 0.5 / view_.scale + scene_->max_cell_w;
      const double hh = view_.fb_h * 0.5 / view_.scale + scene_->max_cell_h;
      int ix0, iy0, ix1, iy1;
      scene_->tiles_for({static_cast<Dbu>(view_.cx - hw), static_cast<Dbu>(view_.cy - hh),
                         static_cast<Dbu>(view_.cx + hw), static_cast<Dbu>(view_.cy + hh)},
                        ix0, iy0, ix1, iy1);
      for (int iy = iy0; iy <= iy1; ++iy) {
        const auto [b, e] = scene_->tile_row_range(iy, ix0, ix1);
        r.draw_rects(BufferId::Cells, b, e - b, s);
        drawn += e - b;
      }
    }
    // Macros.
    {
      DrawStyle s = st;
      s.fill_alpha = 0.28f;
      s.border_px = 1.5f;
      s.min_px = 2.0f;
      r.draw_rects(BufferId::Macros, 0, r.size(BufferId::Macros), s);
    }
    // Heatmap overlay.
    if (heatmap >= 0 && heatmap == heat_idx_ && r.has_image(ImageId::Heatmap)) {
      ImageStyle is;
      is.vmin = heat_vmin;
      is.vmax = heat_vmax;
      is.alpha = heat_alpha;
      r.draw_image(ImageId::Heatmap, is);
    }
    // Detail: wires bottom-up, vias, pins.
    if (detail_wanted_ && detail_) {
      DrawStyle s = st;
      s.line_px = static_cast<float>(std::clamp(row_px / 40.0, 1.0, 2.5) * std::max(1.0f, ui_scale * 0.75f));
      s.alpha = 0.85f;
      const auto& lb = detail_->layer_begin;
      for (std::size_t l = 0; l + 1 < lb.size(); ++l)
        if (lb[l + 1] > lb[l] && layers.visible(static_cast<int>(l)))
          r.draw_segments(BufferId::Wires, lb[l], lb[l + 1] - lb[l], s);
      DrawStyle m = st;
      m.alpha = 0.9f;
      r.draw_rects(BufferId::Vias, 0, r.size(BufferId::Vias), m);
      if (row_px >= 10) r.draw_rects(BufferId::Pins, 0, r.size(BufferId::Pins), m);
    }
    // I/O pins, die outline, GCell grid.
    {
      DrawStyle m = st;
      r.draw_rects(BufferId::IoPins, 0, r.size(BufferId::IoPins), m);
      DrawStyle s = st;
      s.line_px = 1.5f * std::max(1.0f, ui_scale * 0.75f);
      r.draw_segments(BufferId::Die, 0, 4, s);
      s.line_px = 1.0f;
      s.alpha = 0.8f;
      r.draw_segments(BufferId::Grid, 0, r.size(BufferId::Grid), s);
    }
    // Selection on top.
    if (!selection.empty()) {
      DrawStyle s = st;
      s.visible_mask = 0xffffffffu;
      s.override_color = true;
      const float lw = 2.0f * std::max(1.0f, ui_scale * 0.75f);
      s.color = kBackground;  // dark casing for contrast over busy areas
      s.line_px = lw + 2.0f;
      r.draw_segments(BufferId::SelWires, 0, r.size(BufferId::SelWires), s);
      s.color = kNetHi;
      s.line_px = lw;
      r.draw_segments(BufferId::SelWires, 0, r.size(BufferId::SelWires), s);
      s.color = kSelect;
      s.outline_only = true;
      s.border_px = 2.0f;
      s.grow_px = 2.0f;
      s.min_px = 6.0f;
      r.draw_rects(BufferId::SelRects, 0, r.size(BufferId::SelRects), s);
      s.outline_only = false;
      s.grow_px = 0.5f;
      r.draw_rects(BufferId::SelMarkers, 0, r.size(BufferId::SelMarkers), s);
    }
    // Hover outline / drag ghost.
    {
      std::vector<Instance> ghost;
      auto snap = scene_->snap;
      if (drag_cell_ != kInvalidId) {
        const Cell& c = snap->cell(drag_cell_);
        const Point at = screen_to_world(mouse_x, mouse_y) - drag_offset_;
        ghost.push_back({scene_->fx(at.x), scene_->fy(at.y), scene_->fx(at.x + c.width), scene_->fy(at.y + c.height),
                         style::make(LayerSet::kStdCells)});
      } else if (hover.kind == PickResult::Kind::Cell && !(hover == selection)) {
        const Rect b = snap->cell(hover.id).bbox();
        ghost.push_back({scene_->fx(b.xlo), scene_->fy(b.ylo), scene_->fx(b.xhi), scene_->fy(b.yhi),
                         style::make(LayerSet::kStdCells)});
      }
      if (!ghost.empty()) {
        r.upload(BufferId::Ghost, ghost);
        DrawStyle s = st;
        s.visible_mask = 0xffffffffu;
        s.override_color = true;
        s.color = kHover;
        s.outline_only = true;
        s.border_px = 1.5f;
        s.grow_px = 1.0f;
        s.min_px = 4.0f;
        r.draw_rects(BufferId::Ghost, 0, ghost.size(), s);
      }
    }
    fstats.drawn = drawn;
  }
  r.end_frame();
  fstats.cpu_ms = update_ms_ + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void App::after_frame() {
  const auto now = std::chrono::steady_clock::now();
  fstats.frame_ms = std::chrono::duration<double, std::milli>(now - last_frame_).count();
  last_frame_ = now;
  fstats.gpu_ms = renderer_->gpu_ms();
  if (ttff_ms < 0) ttff_ms = now_ms();
  if (!screenshot_path_.empty()) {
    std::vector<std::uint8_t> px;
    renderer_->read_pixels(view_.fb_w, view_.fb_h, px);
    const bool ok = stbi_write_png(screenshot_path_.c_str(), view_.fb_w, view_.fb_h, 4, px.data(), view_.fb_w * 4) != 0;
    const std::string msg = ok ? "saved " + screenshot_path_ : "could not write " + screenshot_path_;
    console.push_back({msg, ok ? 0 : 2});
    if (exit_after_script_) std::printf("%s\n", msg.c_str());
    screenshot_path_.clear();
  }
  if (g_bench_active) {
    if (g_bench.done > 0) {  // the first frame of a run absorbs the switch
      g_bench.cpu.push_back(fstats.cpu_ms);
      g_bench.gpu.push_back(fstats.gpu_ms);
      g_bench.wall.push_back(fstats.frame_ms);
    }
    if (++g_bench.done > g_bench.frames) {
      g_bench.done = g_bench.frames;
      g_bench_done.push_back(std::move(g_bench));
      g_bench = {};
      g_bench_active = false;
    }
  }
  step_script();
  toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& t) { return t.until < now_ms(); }),
               toasts.end());
}

bool App::animating() const {
  return session.busy() || !worker_->idle() || script_running() || g_bench_active || !screenshot_path_.empty() ||
         !toasts.empty() || left_down_ || !hover_done_ || detail_pending_ || session.has_design() != (scene_ != nullptr);
}

// --- Input -----------------------------------------------------------------------

void App::on_mouse_move(double x, double y) {
  if (left_down_ && !box_zoom_ && drag_cell_ == kInvalidId) {
    if (!panning_ && std::hypot(x - down_x_, y - down_y_) > 4) panning_ = true;
    if (panning_) pan_px(x - last_x_, y - last_y_);
  }
  last_x_ = x;
  last_y_ = y;
  mouse_x = x;
  mouse_y = y;
  last_move_ms_ = now_ms();
  hover_done_ = false;
  if (!left_down_) hover = {};
}

void App::on_mouse_button(int button, bool down, double x, double y, int mods) {
  const bool shift = mods & ImGuiMod_Shift, ctrl = (mods & ImGuiMod_Ctrl) || (mods & ImGuiMod_Super);
  if (button == 0 && down) {
    left_down_ = true;
    panning_ = false;
    box_zoom_ = shift;
    down_x_ = last_x_ = x;
    down_y_ = last_y_ = y;
    // Ctrl/Cmd + drag on the selected cell moves it (when no job runs).
    if (ctrl && selection.kind == PickResult::Kind::Cell && scene_) {
      auto snap = scene_->snap;
      const Cell& c = snap->cell(selection.id);
      const Point w = screen_to_world(x, y);
      if (c.bbox().contains(w)) {
        std::string why;
        if (!session.can_edit()) {
          toast(session.busy() ? "Editing is off while " + session.job_kind() + " runs" : "Nothing to edit", 1);
        } else if (c.fixed) {
          toast("Cell is fixed", 1);
        } else {
          drag_cell_ = selection.id;
          drag_offset_ = w - c.loc;
        }
      }
    }
  } else if (button == 0 && !down && left_down_) {
    left_down_ = false;
    if (drag_cell_ != kInvalidId) {
      Point at = screen_to_world(x, y) - drag_offset_;
      auto snap = scene_->snap;
      // Snap to the row grid and site step.
      const auto rows = snap->rows();
      if (!rows.empty()) {
        const Row& r0 = rows.front();
        const Dbu rh = std::max<Dbu>(1, snap->row_height());
        at.y = r0.origin.y + (at.y - r0.origin.y + rh / 2) / rh * rh;
        const Dbu step = std::max<Dbu>(1, r0.site_step);
        at.x = r0.origin.x + (at.x - r0.origin.x + step / 2) / step * step;
      }
      std::string why;
      if (!session.move_cell(drag_cell_, at, &why)) toast(why, 2);
      drag_cell_ = kInvalidId;
    } else if (box_zoom_) {
      if (std::hypot(x - down_x_, y - down_y_) > 6) {
        const Point a = screen_to_world(down_x_, down_y_), b = screen_to_world(x, y);
        zoom_to({std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)});
      }
    } else if (!panning_ && scene_) {
      PickContext ctx{scene_.get(), detail_wanted_ ? detail_.get() : nullptr, &layers, view_.world_per_px(), 4.0 * std::max(1.0f, ui_scale)};
      const PickResult r = pick(ctx, screen_to_world(x, y));
      selection = {r.kind, r.id};
    }
    panning_ = box_zoom_ = false;
  }
}

void App::on_scroll(double dy, double x, double y) {
  if (dy == 0) return;
  zoom_at(x, y, std::pow(1.0015, dy * 120.0));
}

void App::on_key(int key, int mods) {
  const bool ctrl = (mods & ImGuiMod_Ctrl) || (mods & ImGuiMod_Super);
  switch (key) {
    case ImGuiKey_F:
      zoom_fit();
      break;
    case ImGuiKey_Escape:
      clear_selection();
      break;
    case ImGuiKey_GraveAccent:
      show_console = !show_console;
      break;
    case ImGuiKey_L:
      show_layers = !show_layers;
      break;
    case ImGuiKey_I:
      show_inspector = !show_inspector;
      break;
    case ImGuiKey_A:
      show_chat = !show_chat;
      break;
    case ImGuiKey_Z:
      if (ctrl) {
        std::string why;
        if (!session.undo(&why)) toast(why, 1);
      } else {
        zoom_to_selection();
      }
      break;
    case ImGuiKey_Equal:
    case ImGuiKey_KeypadAdd:
      zoom_at(view_.fb_w * 0.5, view_.fb_h * 0.5, 1.25);
      break;
    case ImGuiKey_Minus:
    case ImGuiKey_KeypadSubtract:
      zoom_at(view_.fb_w * 0.5, view_.fb_h * 0.5, 0.8);
      break;
    case ImGuiKey_LeftArrow:
      pan_px(view_.fb_w * 0.1, 0);
      break;
    case ImGuiKey_RightArrow:
      pan_px(-view_.fb_w * 0.1, 0);
      break;
    case ImGuiKey_UpArrow:
      pan_px(0, view_.fb_h * 0.1);
      break;
    case ImGuiKey_DownArrow:
      pan_px(0, -view_.fb_h * 0.1);
      break;
    default:
      break;
  }
}

}  // namespace hermes::gui
