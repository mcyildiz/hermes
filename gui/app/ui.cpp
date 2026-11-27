// hermes_gui panels (Dear ImGui): top bar, layers, inspector, console,
// assistant chat, status bar, toasts. Panels float over the full-window design canvas.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "gui/app/app.hpp"
#include "hermes/par/parallel.hpp"
#include "imgui.h"
#include "imgui_internal.h"

namespace hermes::gui {

namespace {

ImVec4 rgba(std::uint32_t rgb, float a = 1.0f) {
  return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, a};
}
const ImVec4 kAccent = rgba(0x5b8cff);
const ImVec4 kMuted = rgba(0x8a93a6);
const ImVec4 kOk = rgba(0x5fd38d);
const ImVec4 kWarn = rgba(0xf2c14e);
const ImVec4 kErr = rgba(0xff6b6b);

constexpr ImGuiWindowFlags kPanel = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoFocusOnAppearing;

void chip(const char* text, ImVec4 col) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col.x, col.y, col.z, 0.18f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x, col.y, col.z, 0.18f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(col.x, col.y, col.z, 0.18f));
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7, 2));
  ImGui::SmallButton(text);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);
}

void section(const char* title) {
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextUnformatted(title);
  ImGui::PopStyleColor();
  ImGui::Spacing();
}

std::string count_str(std::size_t n) {
  if (n >= 10'000'000) return std::format("{:.0f}M", n / 1e6);
  if (n >= 1'000'000) return std::format("{:.2f}M", n / 1e6);
  if (n >= 10'000) return std::format("{:.0f}k", n / 1e3);
  if (n >= 1'000) return std::format("{:.1f}k", n / 1e3);
  return std::to_string(n);
}

// Same stops as the renderer's colormap (gl_renderer.cpp make_lut).
ImU32 heat_color(float t) {
  struct S {
    float t, r, g, b;
  } st[] = {{0.00f, 0.10f, 0.16f, 0.45f}, {0.20f, 0.13f, 0.42f, 0.86f}, {0.40f, 0.12f, 0.75f, 0.80f},
            {0.60f, 0.55f, 0.90f, 0.35f}, {0.75f, 0.98f, 0.82f, 0.22f}, {0.88f, 0.98f, 0.48f, 0.14f},
            {1.00f, 0.86f, 0.10f, 0.18f}};
  t = std::clamp(t, 0.0f, 1.0f);
  int k = 0;
  while (k < 5 && t > st[k + 1].t) ++k;
  const float u = (t - st[k].t) / (st[k + 1].t - st[k].t);
  auto l = [&](float a, float b) { return a + (b - a) * u; };
  return ImGui::ColorConvertFloat4ToU32(ImVec4(l(st[k].r, st[k + 1].r), l(st[k].g, st[k + 1].g), l(st[k].b, st[k + 1].b), 1));
}

void legend(float vmin, float vmax, const std::string& unit) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = 10;
  const int n = 24;
  for (int i = 0; i < n; ++i) {
    const float x0 = p.x + w * i / n, x1 = p.x + w * (i + 1) / n;
    dl->AddRectFilledMultiColor({x0, p.y}, {x1, p.y + h}, heat_color(float(i) / n), heat_color(float(i + 1) / n),
                                heat_color(float(i + 1) / n), heat_color(float(i) / n));
  }
  ImGui::Dummy({w, h + 2});
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::Text("%.3g", vmin);
  const std::string right = std::format("{:.3g} {}", vmax, unit);
  ImGui::SameLine(std::max(0.0f, w - ImGui::CalcTextSize(right.c_str()).x + ImGui::GetStyle().WindowPadding.x));
  ImGui::TextUnformatted(right.c_str());
  ImGui::PopStyleColor();
}

bool color_swatch(const char* id, Color& c) {
  float col[4] = {c.r, c.g, c.b, c.a};
  const bool ch = ImGui::ColorEdit4(id, col,
                                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                                        ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoBorder);
  if (ch) c = {col[0], col[1], col[2], col[3]};
  return ch;
}

char g_open_path[1024] = "";
int g_synth_cells = 200000;
char g_find[256] = "";
char g_console[1024] = "";
int g_history_pos = -1;
char g_chat[4096] = "";
bool g_chat_privacy = false;

// ---------------------------------------------------------------------------

void top_bar(App& app, float W) {
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize({W, 48});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(0x12151c, 0.96f));
  ImGui::Begin("##top", nullptr, kPanel | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  ImGui::SetCursorPosY(10);
  if (app.font_big) ImGui::PushFont(app.font_big);
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::TextUnformatted("hermes");
  ImGui::PopStyleColor();
  if (app.font_big) ImGui::PopFont();
  ImGui::SameLine(0, 18);
  ImGui::SetCursorPosY(14);
  Session& s = app.session;
  auto snap = s.snapshot();
  if (snap) {
    ImGui::TextUnformatted(snap->name().c_str());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::Text("%s cells  ·  %s nets  ·  v%llu", count_str(snap->num_cells()).c_str(),
                count_str(snap->num_nets()).c_str(), static_cast<unsigned long long>(snap->version()));
    ImGui::PopStyleColor();
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted("no design");
    ImGui::PopStyleColor();
  }
  ImGui::SameLine(0, 24);
  ImGui::SetCursorPosY(10);
  const bool busy = s.busy();
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Open")) ImGui::OpenPopup("open");
  ImGui::SameLine();
  ImGui::BeginDisabled(!s.has_design());
  if (ImGui::Button("Place")) app.run_command("place");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Global placement with timing and routability feedback, then legalization (command: place)");
  ImGui::SameLine();
  if (ImGui::Button("Route")) app.run_command("global_route");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Global route the current placement; incremental when routes exist (command: global_route)");
  ImGui::SameLine();
  if (ImGui::Button("Timing")) app.run_command("timing");
  ImGui::SameLine();
  if (ImGui::Button("Flow")) app.run_command("flow");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Place, route, timing and QoR in one background job (command: flow)");
  ImGui::SameLine();
  if (ImGui::Button("QoR")) app.run_command("qor");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Compute the QoR report: density, congestion and wirelength maps");
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  if (busy) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, rgba(0x5a2a2a));
    if (ImGui::Button("Cancel")) app.run_command("cancel");
    ImGui::PopStyleColor();
    const JobProgress p = s.job_progress();
    ImGui::SameLine(0, 14);
    ImGui::SetCursorPosY(14);
    const std::string label = std::format("{} {:.0f}%  {:.1f}s", p.stage.empty() ? s.job_kind() : p.stage,
                                          p.fraction * 100, p.elapsed_s);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
    ImGui::ProgressBar(static_cast<float>(p.fraction), {240, 20}, label.c_str());
    ImGui::PopStyleColor();
  }
  if (ImGui::BeginPopup("open")) {
    ImGui::TextUnformatted("Open a design directory or file");
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted("LEF/DEF dir, Bookshelf .aux or dir, .hdb (drag & drop works too)");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(420);
    bool go = ImGui::InputTextWithHint("##path", "/path/to/design", g_open_path, sizeof g_open_path,
                                       ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    go |= ImGui::Button("Open##go");
    if (go && g_open_path[0]) {
      app.run_command(std::string("open \"") + g_open_path + "\"");
      ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Or generate a synthetic design");
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt("cells", &g_synth_cells, 10000, 100000);
    ImGui::SameLine();
    if (ImGui::Button("Generate")) {
      app.run_command(std::format("synth {}", std::max(1, g_synth_cells)));
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Generate unplaced")) {
      app.run_command(std::format("synth {} unplaced", std::max(1, g_synth_cells)));
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  // Right side.
  const float right_w = 330;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10, W - right_w));
  ImGui::SetCursorPosY(10);
  auto toggle = [&](const char* label, bool& v) {
    if (v) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f));
    if (ImGui::Button(label)) v = !v;
    if (v) ImGui::PopStyleColor();
    ImGui::SameLine();
  };
  toggle("Layers", app.show_layers);
  toggle("Inspect", app.show_inspector);
  toggle("Console", app.show_console);
  toggle("Assistant", app.show_chat);
  ImGui::End();
}

void layers_panel(App& app, float x, float y, float h) {
  ImGui::SetNextWindowPos({x, y});
  ImGui::SetNextWindowSize({270, h});
  ImGui::Begin("##layers", nullptr, kPanel);
  if (app.font_big) ImGui::PushFont(app.font_big);
  ImGui::TextUnformatted("Layers");
  if (app.font_big) ImGui::PopFont();
  LayerSet& L = app.layers;
  auto scene = app.scene();
  auto det = app.detail();
  auto count_of = [&](int i) -> std::string {
    if (!scene) return "";
    switch (L[i].kind) {
      case LayerKind::StdCells:
        return count_str(scene->num_std - scene->num_fixed_std);
      case LayerKind::FixedCells:
        return count_str(scene->num_fixed_std);
      case LayerKind::Macros:
        return count_str(scene->num_macros);
      case LayerKind::IoPins:
        return count_str(scene->io_pins.size());
      case LayerKind::Rows:
        return count_str(scene->rows.size());
      case LayerKind::Pins:
        return det && app.detail_wanted() ? count_str(det->pins.size()) : "";
      case LayerKind::Vias:
        return det && app.detail_wanted() ? count_str(det->vias.size()) : "";
      case LayerKind::Metal:
        if (det && app.detail_wanted() && static_cast<std::size_t>(i) + 1 < det->layer_begin.size())
          return count_str(det->layer_begin[static_cast<std::size_t>(i) + 1] - det->layer_begin[static_cast<std::size_t>(i)]);
        return "";
      default:
        return "";
    }
  };
  auto group_table = [&](const char* title, const char* group) {
    ImGui::Spacing();
    if (!ImGui::BeginTable(group, 4, ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed, 44);
    ImGui::TableSetupColumn("show", ImGuiTableColumnFlags_WidthFixed, 26);
    ImGui::TableSetupColumn("pick", ImGuiTableColumnFlags_WidthFixed, 26);
    ImGui::TableNextRow();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(title);
    ImGui::PushFont(nullptr);
    ImGui::SetWindowFontScale(0.85f);
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted("show");
    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted("pick");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    for (int i = 0; i < L.size(); ++i) {
      DisplayLayer& l = L[i];
      if (l.group != group) continue;
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      color_swatch("##c", l.color);
      ImGui::SameLine();
      if (!l.visible) ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
      ImGui::TextUnformatted(l.name.c_str());
      if (!l.visible) ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      const std::string n = count_of(i);
      ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(n.c_str()).x);
      ImGui::TextUnformatted(n.c_str());
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(2);
      ImGui::Checkbox("##v", &l.visible);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show %s", l.name.c_str());
      ImGui::TableSetColumnIndex(3);
      if (l.can_select) {
        ImGui::BeginDisabled(!l.visible);
        ImGui::Checkbox("##s", &l.selectable);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Clicks select %s", l.name.c_str());
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  };
  group_table("DESIGN", "Design");
  group_table("ROUTING", "Routing");
  if (auto t = app.session.tech()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("Wires are estimated (router stub).");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t->wires_source.c_str());
  }
  if (scene && !app.detail_wanted() && L.any_metal_visible()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("Zoom in to see pins and wires.");
    ImGui::PopStyleColor();
  }

  section("HEATMAP");
  const char* cur = app.heatmap >= 0 && app.heatmap < static_cast<int>(app.heatmaps.size())
                        ? app.heatmaps[static_cast<std::size_t>(app.heatmap)].name.c_str()
                        : "Off";
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##heat", cur)) {
    if (ImGui::Selectable("Off", app.heatmap < 0)) app.heatmap = -1;
    for (std::size_t i = 0; i < app.heatmaps.size(); ++i) {
      if (i == 2) ImGui::Separator();
      if (ImGui::Selectable(app.heatmaps[i].name.c_str(), app.heatmap == static_cast<int>(i))) {
        std::string why;
        app.set_heatmap(app.heatmaps[i].key, &why);
      }
    }
    if (app.heatmaps.size() <= 2) {
      ImGui::Separator();
      ImGui::BeginDisabled();
      ImGui::Selectable("QoR maps: run QoR or a flow");
      ImGui::EndDisabled();
    }
    ImGui::EndCombo();
  }
  if (app.heatmap >= 0) {
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##alpha", &app.heat_alpha, 0.05f, 1.0f, "opacity %.2f");
    legend(app.heat_vmin, app.heat_vmax, app.heatmaps[static_cast<std::size_t>(app.heatmap)].unit);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloatRange2("##range", &app.heat_vmin, &app.heat_vmax, 0.01f, -1e9f, 1e9f, "min %.3g", "max %.3g"))
      app.heat_auto_range = false;
    if (app.heatmaps[static_cast<std::size_t>(app.heatmap)].live) {
      ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
      ImGui::TextWrapped("Live: follows the design while engines run.");
      ImGui::PopStyleColor();
    } else if (app.session.qor_version() && app.session.snapshot() &&
               app.session.qor_version() != app.session.snapshot()->version()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
      ImGui::TextWrapped("Design changed since this QoR report.");
      ImGui::PopStyleColor();
    }
  }
  ImGui::End();
}

void cell_details(App& app, const DesignSnapshot& s, Id id) {
  const Cell& c = s.cell(id);
  if (app.font_big) ImGui::PushFont(app.font_big);
  ImGui::TextWrapped("%s", std::string(s.cell_name(id)).c_str());
  if (app.font_big) ImGui::PopFont();
  if (!s.cell_master(id).empty()) {
    chip(std::string(s.cell_master(id)).c_str(), kAccent);
    ImGui::SameLine();
  }
  if (c.is_macro) {
    chip("macro", rgba(0xd08bff));
    ImGui::SameLine();
  }
  if (c.fixed) {
    chip("fixed", kMuted);
    ImGui::SameLine();
  }
  chip(c.placed ? "placed" : "unplaced", c.placed ? kOk : kWarn);
  ImGui::Spacing();
  if (ImGui::BeginTable("props", 2)) {
    auto row = [&](const char* k, const std::string& v) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
      ImGui::TextUnformatted(k);
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(v.c_str());
    };
    const char* unit = app.dbu_per_um() > 0 ? " um" : " dbu";
    row("Location", app.um(c.loc.x) + ", " + app.um(c.loc.y) + unit);
    row("Size", app.um(c.width) + " x " + app.um(c.height) + unit);
    static const char* orients[] = {"N", "S", "E", "W", "FN", "FS", "FE", "FW"};
    row("Orient", orients[static_cast<int>(c.orient) & 7]);
    row("Id", std::to_string(id));
    ImGui::EndTable();
  }
  const auto pins = s.cell_pins(id);
  section(std::format("PINS ({})", pins.size()).c_str());
  if (ImGui::BeginChild("pins", {0, std::min(260.0f, 26.0f * pins.size() + 8)}, ImGuiChildFlags_None)) {
    if (ImGui::BeginTable("pt", 3, ImGuiTableFlags_SizingStretchProp)) {
      static const char* dirs[] = {"in", "out", "inout"};
      for (Id p : pins) {
        ImGui::PushID(p);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(std::string(s.pin_name(p)).c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextUnformatted(dirs[static_cast<int>(s.pin(p).dir) % 3]);
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2);
        const Id n = s.pin(p).net;
        if (n != kInvalidId) {
          if (ImGui::Selectable(std::string(s.net_name(n)).c_str())) app.select({PickResult::Kind::Net, n});
        } else {
          ImGui::TextDisabled("unconnected");
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }
  ImGui::EndChild();
  if (!c.fixed) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped(app.session.can_edit() ? "Ctrl/Cmd-drag the cell to move it, Ctrl/Cmd-Z to undo."
                                              : "Editing is off while a job runs.");
    ImGui::PopStyleColor();
  }
}

void net_details(App& app, const DesignSnapshot& s, Id id) {
  if (app.font_big) ImGui::PushFont(app.font_big);
  ImGui::TextWrapped("%s", std::string(s.net_name(id)).c_str());
  if (app.font_big) ImGui::PopFont();
  chip("net", rgba(0x39e5ff));
  const auto pins = s.net_pins(id);
  const Rect bb = s.net_bbox(id);
  if (ImGui::BeginTable("props", 2)) {
    auto row = [&](const char* k, const std::string& v) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
      ImGui::TextUnformatted(k);
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(v.c_str());
    };
    const char* unit = app.dbu_per_um() > 0 ? " um" : " dbu";
    row("Pins", std::to_string(pins.size()));
    row("HPWL", app.um(bb.width() + bb.height()) + unit);
    row("Box", app.um(bb.width()) + " x " + app.um(bb.height()) + unit);
    row("Weight", std::format("{:.3g}", s.net(id).weight));
    if (auto t = app.session.tech()) {
      std::vector<Point> pts;
      for (Id p : pins) pts.push_back(s.pin_location(p));
      std::vector<WireSegment> segs;
      t->routes.route_net(pts, segs);
      Dbu wl = 0;
      for (const auto& w : segs) wl += std::abs(w.b.x - w.a.x) + std::abs(w.b.y - w.a.y);
      row("Est. wire", app.um(wl) + unit);
    }
    ImGui::EndTable();
  }
  section(std::format("PINS ({})", pins.size()).c_str());
  if (ImGui::BeginChild("pins", {0, std::min(300.0f, 26.0f * pins.size() + 8)})) {
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(pins.size()));
    while (clip.Step())
      for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
        const Id p = pins[static_cast<std::size_t>(k)];
        const Pin& pp = s.pin(p);
        const std::string label = pp.cell == kInvalidId ? std::format("I/O {}", s.pin_name(p))
                                                        : std::format("{} / {}", s.cell_name(pp.cell), s.pin_name(p));
        ImGui::PushID(k);
        if (ImGui::Selectable(label.c_str()) && pp.cell != kInvalidId) app.select({PickResult::Kind::Cell, pp.cell});
        ImGui::PopID();
      }
  }
  ImGui::EndChild();
}

void pin_details(App& app, const DesignSnapshot& s, Id id) {
  const Pin& p = s.pin(id);
  if (app.font_big) ImGui::PushFont(app.font_big);
  ImGui::TextWrapped("%s", std::string(s.pin_name(id)).c_str());
  if (app.font_big) ImGui::PopFont();
  chip(p.cell == kInvalidId ? "I/O pin" : "pin", rgba(0xffd166));
  const Point q = s.pin_location(id);
  ImGui::Text("at %s, %s", app.um(q.x).c_str(), app.um(q.y).c_str());
  if (p.cell != kInvalidId && ImGui::Button(std::format("Cell {}", s.cell_name(p.cell)).c_str()))
    app.select({PickResult::Kind::Cell, p.cell});
  if (p.net != kInvalidId && ImGui::Button(std::format("Net {}", s.net_name(p.net)).c_str()))
    app.select({PickResult::Kind::Net, p.net});
}

void inspector_panel(App& app, float x, float y, float h) {
  ImGui::SetNextWindowPos({x, y});
  ImGui::SetNextWindowSize({330, h});
  ImGui::Begin("##inspector", nullptr, kPanel);
  Session& s = app.session;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputTextWithHint("##find", "Find cell or net...", g_find, sizeof g_find,
                               ImGuiInputTextFlags_EnterReturnsTrue) &&
      g_find[0]) {
    app.run_command(std::string("find \"") + g_find + "\"");
  }
  if (s.busy()) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.14f));
    ImGui::BeginChild("busy", {0, 54}, ImGuiChildFlags_None);
    ImGui::SetCursorPos({10, 8});
    ImGui::TextColored(kAccent, "%s running", s.job_kind().c_str());
    ImGui::SetCursorPosX(10);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted("Live view. Editing is off until it ends.");
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }
  ImGui::Spacing();
  auto snap = s.snapshot();
  const Selection sel = app.selection;
  if (!snap || sel.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped(snap ? "Click a cell, macro, pin or wire to inspect it. Shift-drag to zoom to a box, F to fit."
                            : "Open a design to start.");
    ImGui::PopStyleColor();
  } else if (sel.kind == PickResult::Kind::Cell && static_cast<std::size_t>(sel.id) < snap->num_cells()) {
    cell_details(app, *snap, sel.id);
  } else if (sel.kind == PickResult::Kind::Net && static_cast<std::size_t>(sel.id) < snap->num_nets()) {
    net_details(app, *snap, sel.id);
  } else if (sel.kind == PickResult::Kind::Pin && static_cast<std::size_t>(sel.id) < snap->num_pins()) {
    pin_details(app, *snap, sel.id);
  }
  if (snap && !sel.empty()) {
    if (ImGui::Button("Zoom to  Z")) app.zoom_to_selection();
    ImGui::SameLine();
    if (ImGui::Button("Clear  Esc")) app.clear_selection();
  }

  // Results of the last steps (place, global_route, timing, or a flow).
  if (s.last_placement() || s.last_route() || s.last_timing() || s.last_legalize()) {
    ImGui::Spacing();
    ImGui::Separator();
    section("RESULTS");
    const char* unit = app.dbu_per_um() > 0 ? " um" : "";
    if (auto p = s.last_placement())
      ImGui::Text("Placement HPWL %.6g%s", p->hpwl, p->cancelled ? " (cancelled)" : "");
    if (auto l = s.last_legalize())
      ImGui::Text("Legalized %d cells, %s", l->cells.legalized, l->legal ? "legal" : "NOT legal");
    if (auto r = s.last_route())
      ImGui::Text("Route WL %s%s  overflow %.0f%s", app.um(static_cast<Dbu>(r->stats.wirelength)).c_str(), unit,
                  r->stats.total_overflow, r->incremental ? " (incr.)" : "");
    if (auto t = s.last_timing()) ImGui::Text("WNS %.1f ps   TNS %.1f ps", t->wns_ps, t->tns_ps);
    if (auto f = s.last_flow()) {
      for (const auto& [stage, sec] : f->stage_seconds) {
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::Text("%-16s %7.2f s", stage.c_str(), sec);
        ImGui::PopStyleColor();
      }
    }
  }
  if (auto q = s.qor()) {
    ImGui::Spacing();
    ImGui::Separator();
    section("QUALITY OF RESULTS");
    const char* unit = app.dbu_per_um() > 0 ? " um" : "";
    ImGui::Text("HPWL %s%s", app.um(static_cast<Dbu>(q->wirelength.hpwl)).c_str(), unit);
    ImGui::Text("Density overflow %.3f   peak %.2f", q->density.overflow, q->density.peak_util);
    ImGui::Text("Congestion overflow %.0f   max util %.2f", q->congestion.total_overflow, q->congestion.max_util);
    ImGui::Text("Vias %llu  (layers %s)", static_cast<unsigned long long>(q->wirelength.total_vias),
                q->wirelength.layers_estimated ? "estimated" : "routed");
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted("Show map:");
    ImGui::PopStyleColor();
    int k = 0;
    for (const QorMap& m : q->maps) {
      if (k++ % 2) ImGui::SameLine();
      if (ImGui::SmallButton(m.name.c_str())) {
        std::string why;
        app.set_heatmap("qor:" + m.name, &why);
      }
    }
  }
  ImGui::End();
}

int console_callback(ImGuiInputTextCallbackData* d) {
  App& app = *static_cast<App*>(d->UserData);
  if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory && !app.history.empty()) {
    const int n = static_cast<int>(app.history.size());
    if (d->EventKey == ImGuiKey_UpArrow) g_history_pos = g_history_pos < 0 ? n - 1 : std::max(0, g_history_pos - 1);
    else if (d->EventKey == ImGuiKey_DownArrow) g_history_pos = g_history_pos < 0 ? -1 : g_history_pos + 1;
    if (g_history_pos >= n) g_history_pos = -1;
    d->DeleteChars(0, d->BufTextLen);
    if (g_history_pos >= 0) d->InsertChars(0, app.history[static_cast<std::size_t>(g_history_pos)].c_str());
  } else if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
    std::string word(d->Buf, d->Buf + d->BufTextLen);
    if (word.find(' ') == std::string::npos) {
      const auto c = app.commands.complete(word);
      if (c.size() == 1) {
        d->DeleteChars(0, d->BufTextLen);
        d->InsertChars(0, (c[0] + " ").c_str());
      } else if (c.size() > 1) {
        std::string all;
        for (const auto& x : c) all += x + "  ";
        app.console.push_back({all, 0});
      }
    }
  }
  return 0;
}

void console_panel(App& app, float x, float y, float w, float h) {
  ImGui::SetNextWindowPos({x, y});
  ImGui::SetNextWindowSize({w, h});
  ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(0x0d1016, 0.97f));
  ImGui::Begin("##console", nullptr, kPanel);
  ImGui::PopStyleColor();
  const float input_h = ImGui::GetFrameHeightWithSpacing() + 4;
  if (app.font_mono) ImGui::PushFont(app.font_mono);
  ImGui::BeginChild("lines", {0, -input_h});
  ImGuiListClipper clip;
  clip.Begin(static_cast<int>(app.console.size()));
  while (clip.Step())
    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
      const auto& l = app.console[static_cast<std::size_t>(i)];
      const ImVec4 col = l.kind == 1 ? kAccent : l.kind == 2 || l.kind == 5 ? kErr : l.kind == 4 ? kWarn
                                                                                  : l.kind == 3 ? kMuted
                                                                                                : rgba(0xe6e9ef);
      ImGui::PushStyleColor(ImGuiCol_Text, col);
      ImGui::TextUnformatted(l.text.c_str());
      ImGui::PopStyleColor();
    }
  static std::size_t last_n = 0;
  if (app.console.size() != last_n) {
    ImGui::SetScrollHereY(1.0f);
    last_n = app.console.size();
  }
  ImGui::EndChild();
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::TextUnformatted(">");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1);
  static bool refocus = true;
  if (refocus) {
    ImGui::SetKeyboardFocusHere();
    refocus = false;
  }
  const char* hint = app.session.busy() ? "command (a job is running: view commands run now, steps are queued)"
                                        : "command (help, place, global_route, timing, qor, save, source, ...)";
  if (ImGui::InputTextWithHint("##in", hint, g_console, sizeof g_console,
                               ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
                                   ImGuiInputTextFlags_CallbackCompletion,
                               console_callback, &app)) {
    std::string line = g_console;
    if (!line.empty()) {
      app.history.push_back(line);
      app.run_command(line);
    }
    g_console[0] = 0;
    g_history_pos = -1;
    refocus = true;
  }
  if (app.font_mono) ImGui::PopFont();
  ImGui::End();
}


// --- Assistant chat ------------------------------------------------------------

void chat_panel(App& app, float x, float y, float w, float h) {
  ImGui::SetNextWindowPos({x, y});
  ImGui::SetNextWindowSize({w, h});
  ImGui::Begin("##chat", nullptr, kPanel);
  assist::AiHost& ai = app.ai;
  assist::Assistant* a = ai.assistant();
  ImGui::PushFont(app.font_big);
  ImGui::TextUnformatted("Assistant");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextUnformatted(assist::policy_name(ai.config().edits));
  ImGui::PopStyleColor();
  ImGui::SameLine(w - 150);
  if (ImGui::SmallButton(g_chat_privacy ? "Chat" : "What is sent")) g_chat_privacy = !g_chat_privacy;
  ImGui::SameLine();
  if (a && a->busy()) {
    if (ImGui::SmallButton("Stop")) a->cancel();
  } else if (a && ImGui::SmallButton("Clear")) {
    a->reset();
  }
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextWrapped("%s", assist::status_line(ai.config()).c_str());
  ImGui::PopStyleColor();
  ImGui::Separator();

  std::string why;
  const bool ready = a || assist::assistant_ready(ai.config(), &why);
  if (g_chat_privacy || !ready) {
    if (!ready) {
      ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
      ImGui::TextWrapped("%s", why.c_str());
      ImGui::PopStyleColor();
      ImGui::Spacing();
      ImGui::TextWrapped("Quick try without a model: type 'ai set provider mock' then 'ai on' in the console.");
      ImGui::Spacing();
    }
    std::string ep;
    if (a) {
      ep = a->provider().endpoint();
    } else if (auto p = assist::make_provider(ai.config().provider, {}, &why)) {
      ep = p->endpoint();
    }
    ImGui::BeginChild("privacy", {0, 0});
    ImGui::TextWrapped("%s", assist::sharing_text(ai.config(), ep).c_str());
    ImGui::EndChild();
    ImGui::End();
    return;
  }

  // Transcript (copied only when it changed).
  static std::vector<assist::Assistant::Entry> entries;
  static std::uint64_t seen_version = ~0ull;
  static const void* seen_owner = nullptr;
  if (a && (a->version() != seen_version || seen_owner != a)) {
    entries = a->transcript();
    seen_version = a->version();
    seen_owner = a;
  } else if (!a) {
    entries.clear();
  }
  const auto pending = a ? a->pending_confirmation() : std::nullopt;
  const float input_h = ImGui::GetFrameHeightWithSpacing() * 2 + 6;
  const float confirm_h = pending ? 110.0f : 0.0f;
  ImGui::BeginChild("chat_lines", {0, -(input_h + confirm_h)});
  using K = assist::Assistant::Entry::Kind;
  if (entries.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("Ask about the design: congestion hot spots, QoR, timing, a cell or net, past runs. "
                       "The assistant uses hermes tools and the current selection. Right-click a message to copy it.");
    ImGui::PopStyleColor();
  }
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    ImGui::PushID(static_cast<int>(i));
    switch (e.kind) {
      case K::User:
        ImGui::Spacing();
        ImGui::TextColored(kAccent, "You");
        ImGui::TextWrapped("%s", e.text.c_str());
        break;
      case K::Assistant:
        ImGui::Spacing();
        ImGui::TextColored(kOk, "Assistant");
        ImGui::TextWrapped("%s", e.text.c_str());
        break;
      case K::ToolCall:
        if (app.font_mono) ImGui::PushFont(app.font_mono);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("> %s", e.text.c_str());
        ImGui::PopStyleColor();
        if (app.font_mono) ImGui::PopFont();
        break;
      case K::ToolResult:
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.text.c_str());
        break;
      case K::Error:
        ImGui::PushStyleColor(ImGuiCol_Text, kErr);
        ImGui::TextWrapped("%s", e.text.c_str());
        ImGui::PopStyleColor();
        break;
      case K::Info:
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("(%s)", e.text.c_str());
        ImGui::PopStyleColor();
        break;
    }
    if (e.kind != K::ToolResult && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      ImGui::SetClipboardText(e.text.c_str());
      app.toast("copied");
    }
    ImGui::PopID();
  }
  if (a && a->busy() && !pending) {
    const char* dots[] = {".", "..", "..."};
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::Text("working%s", dots[static_cast<int>(ImGui::GetTime() * 3) % 3]);
    ImGui::PopStyleColor();
  }
  static std::size_t last_n = 0;
  if (entries.size() != last_n) {
    ImGui::SetScrollHereY(1.0f);
    last_n = entries.size();
  }
  ImGui::EndChild();

  if (pending) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(kWarn.x, kWarn.y, kWarn.z, 0.14f));
    ImGui::BeginChild("confirm", {0, confirm_h - 6});
    ImGui::SetCursorPos({8, 6});
    ImGui::TextColored(kWarn, "The assistant wants to: %s", pending->title.c_str());
    if (app.font_mono) ImGui::PushFont(app.font_mono);
    ImGui::SetCursorPosX(8);
    ImGui::TextWrapped("%s %s", pending->tool.c_str(), pending->args.c_str());
    if (app.font_mono) ImGui::PopFont();
    ImGui::SetCursorPosX(8);
    if (ImGui::Button("Allow")) a->confirm(true);
    ImGui::SameLine();
    if (ImGui::Button("Deny")) a->confirm(false);
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }

  // Input.
  ImGui::SetNextItemWidth(-1);
  static bool refocus = true;
  if (refocus && app.show_chat) {
    ImGui::SetKeyboardFocusHere();
    refocus = false;
  }
  const bool busy = a && a->busy();
  bool send = ImGui::InputTextWithHint("##ask", busy ? "answering... (Stop to interrupt)" : "Ask the assistant...",
                                       g_chat, sizeof g_chat, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::BeginDisabled(busy || !g_chat[0]);
  send |= ImGui::Button("Send");
  ImGui::EndDisabled();
  if (send && g_chat[0] && !busy) {
    std::string err;
    if (!ai.ask(app.session, app, g_chat, &err)) {
      app.toast(err, 2);
    } else {
      g_chat[0] = 0;
    }
    refocus = true;
  }
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  if (a) ImGui::Text("%s  tokens %lld / %lld", a->provider().model().c_str(), a->input_tokens(), a->output_tokens());
  ImGui::PopStyleColor();
  ImGui::End();
}

void status_bar(App& app, float W, float H) {
  const float h = 26;
  ImGui::SetNextWindowPos({0, H - h});
  ImGui::SetNextWindowSize({W, h});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 4});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(0x12151c, 0.96f));
  ImGui::Begin("##status", nullptr, kPanel | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  if (auto sc = app.scene()) {
    const Point w = app.screen_to_world(app.mouse_x, app.mouse_y);
    const double d = app.dbu_per_um();
    if (d > 0)
      ImGui::Text("x %.3f  y %.3f um", w.x / d, w.y / d);
    else
      ImGui::Text("x %lld  y %lld", static_cast<long long>(w.x), static_cast<long long>(w.y));
    ImGui::SameLine(0, 24);
    const double upp = app.view().world_per_px() / (d > 0 ? d : 1.0);
    ImGui::Text("%.4g %s/px", upp, d > 0 ? "um" : "dbu");
    ImGui::SameLine(0, 24);
    ImGui::Text("%s", app.density_mode() ? "coarse view (density)" : app.detail_wanted() ? "cells + pins + wires" : "cells");
    ImGui::SameLine(0, 24);
    ImGui::Text("%s cells in view", count_str(app.fstats.visible_cells).c_str());
    ImGui::SameLine(0, 24);
    ImGui::Text("scene %.0f ms", sc->build_ms);
    if (auto det = app.detail(); det && app.detail_wanted()) {
      ImGui::SameLine(0, 24);
      ImGui::Text("detail %.0f ms%s", det->build_ms, det->truncated ? " (capped)" : "");
    }
  } else {
    ImGui::TextUnformatted("ready");
  }
  const std::string right =
      std::format("frame {:.1f} ms  ·  gpu {:.2f} ms  ·  {:.0f} fps  ·  jobs {}/{} threads", app.fstats.cpu_ms,
                  app.fstats.gpu_ms, app.fstats.frame_ms > 0 ? 1000.0 / app.fstats.frame_ms : 0.0,
                  app.session.job_threads(), par::hardware_threads());
  ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, W - ImGui::CalcTextSize(right.c_str()).x - 14));
  ImGui::TextUnformatted(right.c_str());
  ImGui::PopStyleColor();
  ImGui::End();
}

void empty_state(App& app, float W, float H) {
  const bool loading = app.session.busy() && !app.session.has_design();
  const float w = 520, h = loading ? 120 : 230;
  ImGui::SetNextWindowPos({(W - w) / 2, (H - h) / 2});
  ImGui::SetNextWindowSize({w, h});
  ImGui::Begin("##empty", nullptr, kPanel);
  if (app.font_big) ImGui::PushFont(app.font_big);
  ImGui::TextUnformatted(loading ? "Loading..." : "Open a design");
  if (app.font_big) ImGui::PopFont();
  if (loading) {
    const JobProgress p = app.session.job_progress();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
    ImGui::ProgressBar(static_cast<float>(p.fraction), {-1, 22}, std::format("{} {:.1f}s", p.stage, p.elapsed_s).c_str());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted("The window stays responsive while the design loads.");
    ImGui::PopStyleColor();
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("A LEF/DEF or Bookshelf directory, a .aux, .def or .hdb file. You can also drop it on the window.");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-90);
    bool go = ImGui::InputTextWithHint("##path0", "/path/to/design", g_open_path, sizeof g_open_path,
                                       ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    go |= ImGui::Button("Open", {-1, 0});
    if (go && g_open_path[0]) app.run_command(std::string("open \"") + g_open_path + "\"");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Or try a synthetic design");
    ImGui::SetNextItemWidth(180);
    ImGui::InputInt("cells##s", &g_synth_cells, 10000, 100000);
    ImGui::SameLine();
    if (ImGui::Button("Generate")) app.run_command(std::format("synth {}", std::max(1, g_synth_cells)));
    ImGui::SameLine();
    if (ImGui::Button("Unplaced")) app.run_command(std::format("synth {} unplaced", std::max(1, g_synth_cells)));
  }
  ImGui::End();
}

}  // namespace

void apply_theme(float) {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 10;
  s.ChildRounding = 8;
  s.FrameRounding = 6;
  s.PopupRounding = 8;
  s.GrabRounding = 6;
  s.ScrollbarRounding = 8;
  s.TabRounding = 6;
  s.WindowBorderSize = 1;
  s.FrameBorderSize = 0;
  s.PopupBorderSize = 1;
  s.WindowPadding = {14, 12};
  s.FramePadding = {9, 5};
  s.ItemSpacing = {8, 6};
  s.ItemInnerSpacing = {6, 4};
  s.CellPadding = {4, 3};
  s.ScrollbarSize = 10;
  s.GrabMinSize = 10;
  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = rgba(0xe6e9ef);
  c[ImGuiCol_TextDisabled] = rgba(0x6b7385);
  c[ImGuiCol_WindowBg] = rgba(0x161a22, 0.94f);
  c[ImGuiCol_ChildBg] = rgba(0x000000, 0.0f);
  c[ImGuiCol_PopupBg] = rgba(0x1a1f29, 0.98f);
  c[ImGuiCol_Border] = rgba(0x2a303c, 0.9f);
  c[ImGuiCol_FrameBg] = rgba(0x222836);
  c[ImGuiCol_FrameBgHovered] = rgba(0x2b3346);
  c[ImGuiCol_FrameBgActive] = rgba(0x323b52);
  c[ImGuiCol_TitleBg] = rgba(0x161a22);
  c[ImGuiCol_TitleBgActive] = rgba(0x161a22);
  c[ImGuiCol_Button] = rgba(0x262d3c);
  c[ImGuiCol_ButtonHovered] = rgba(0x323c52);
  c[ImGuiCol_ButtonActive] = rgba(0x3f4d6d);
  c[ImGuiCol_Header] = rgba(0x262d3c);
  c[ImGuiCol_HeaderHovered] = rgba(0x303a50);
  c[ImGuiCol_HeaderActive] = rgba(0x3a4660);
  c[ImGuiCol_CheckMark] = kAccent;
  c[ImGuiCol_SliderGrab] = kAccent;
  c[ImGuiCol_SliderGrabActive] = rgba(0x7aa2ff);
  c[ImGuiCol_Separator] = rgba(0x2a303c);
  c[ImGuiCol_ScrollbarBg] = rgba(0x000000, 0.0f);
  c[ImGuiCol_ScrollbarGrab] = rgba(0x2c3342);
  c[ImGuiCol_TableRowBg] = rgba(0x000000, 0.0f);
  c[ImGuiCol_TableBorderLight] = rgba(0x2a303c);
  c[ImGuiCol_PlotHistogram] = kAccent;
  c[ImGuiCol_NavCursor] = kAccent;
}

void draw_ui(App& app) {
  ImGuiIO& io = ImGui::GetIO();
  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  top_bar(app, W);
  const float top = 58, bottom = 26 + 10;
  const float console_h = app.show_console ? std::max(160.0f, H * 0.32f) : 0.0f;
  const float panel_h = H - top - bottom - (app.show_console ? console_h + 10 : 0);
  if (!app.session.has_design()) {
    empty_state(app, W, H);
  } else {
    if (app.show_layers) layers_panel(app, 12, top, panel_h);
    if (app.show_inspector) inspector_panel(app, W - 342, top, panel_h);
  }
  if (app.show_chat) {
    const float cw = std::min(420.0f, std::max(300.0f, W * 0.28f));
    const float cx = app.session.has_design() && app.show_inspector ? W - 342 - 10 - cw : W - 12 - cw;
    chat_panel(app, cx, top, cw, panel_h);
  }
  if (app.show_console) console_panel(app, 12, H - bottom - console_h, W - 24, console_h);
  status_bar(app, W, H);

  // Hover tooltip and box-zoom rubber band.
  if (!app.hover.empty() && !io.WantCaptureMouse && !(app.hover == app.selection)) {
    const std::string t = app.describe(app.hover);
    if (!t.empty()) ImGui::SetTooltip("%s", t.c_str());
  }
  if (io.KeyShift && io.MouseDown[0] && !io.WantCaptureMouse) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRect(io.MouseClickedPos[0], io.MousePos, ImGui::ColorConvertFloat4ToU32(kAccent), 3, 0, 1.5f);
    dl->AddRectFilled(io.MouseClickedPos[0], io.MousePos, ImGui::ColorConvertFloat4ToU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.08f)));
  }
  // Toasts.
  float ty = 62;
  for (std::size_t i = 0; i < app.toasts.size(); ++i) {
    const auto& t = app.toasts[i];
    const ImVec2 sz = ImGui::CalcTextSize(t.text.c_str(), nullptr, false, 460);
    ImGui::SetNextWindowPos({(W - sz.x) * 0.5f - 14, ty});
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::Begin(std::format("##toast{}", i).c_str(), nullptr,
                 kPanel | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
    ImGui::PushTextWrapPos(460);
    ImGui::TextColored(t.kind == 2 ? kErr : t.kind == 1 ? kWarn : kOk, "%s", t.text.c_str());
    ImGui::PopTextWrapPos();
    ty += ImGui::GetWindowHeight() + 6;
    ImGui::End();
  }
}

}  // namespace hermes::gui
