#include "assist/tools.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <stdexcept>
#include <thread>

#include "assist/run_history.hpp"
#include "gui/scene/session.hpp"
#include "hermes/qor/qor.hpp"

namespace hermes::assist {

using gui::Session;

const char* access_name(Access a) { return a == Access::Read ? "read" : "change"; }

const char* policy_name(EditPolicy p) {
  switch (p) {
    case EditPolicy::ReadOnly: return "read-only";
    case EditPolicy::Ask: return "ask";
    default: return "allow";
  }
}

bool parse_policy(std::string_view s, EditPolicy* out) {
  if (s == "read-only" || s == "readonly" || s == "read" || s == "off" || s == "never") *out = EditPolicy::ReadOnly;
  else if (s == "ask" || s == "confirm") *out = EditPolicy::Ask;
  else if (s == "allow" || s == "yes") *out = EditPolicy::Allow;
  else return false;
  return true;
}

double steady_seconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string clip_text(std::string s, std::size_t max_chars) {
  if (s.size() <= max_chars) return s;
  const std::size_t head = max_chars * 3 / 4, tail = max_chars - head;
  return s.substr(0, head) + std::format("\n... [{} characters cut] ...\n", s.size() - head - tail) +
         s.substr(s.size() - tail);
}

namespace {

ToolResult ok(std::string t) { return {true, std::move(t), {}, {}, 0.0}; }
ToolResult fail(std::string t) { return {false, std::move(t), {}, {}, 0.0}; }
ToolResult ok_json(const Json& j) { return ok(j.dump(1)); }

// --- schema helpers -----------------------------------------------------------
Json prop(const char* type, const char* desc) { return Json::object({{"type", type}, {"description", desc}}); }
Json schema(std::initializer_list<Json::Member> props, std::initializer_list<const char*> required = {}) {
  Json s = Json::object({{"type", "object"}});
  Json p = Json::object();
  for (const auto& m : props) p.set(m.first, m.second);
  s.set("properties", std::move(p));
  if (required.size()) {
    Json r = Json::array();
    for (const char* k : required) r.push_back(k);
    s.set("required", std::move(r));
  }
  s.set("additionalProperties", false);
  return s;
}

std::string need_string(const Json& a, const char* key) {
  const Json& v = a[key];
  if (!v.is_string() || v.str_ref().empty()) throw std::invalid_argument(std::format("argument '{}' (string) is required", key));
  return v.str_ref();
}
double num_arg(const Json& a, const char* key, double def) {
  const Json& v = a[key];
  if (v.is_null()) return def;
  if (!v.is_number()) throw std::invalid_argument(std::format("argument '{}' must be a number", key));
  return v.as_number();
}
bool bool_arg(const Json& a, const char* key, bool def) {
  const Json& v = a[key];
  if (v.is_null()) return def;
  if (!v.is_bool()) throw std::invalid_argument(std::format("argument '{}' must be true or false", key));
  return v.as_bool();
}

double um_per_dbu(const DesignSnapshot& s) { return s.dbu_per_micron() > 0 ? 1.0 / s.dbu_per_micron() : 1.0; }
Json um(const DesignSnapshot& s, Dbu v) { return Json(std::round(static_cast<double>(v) * um_per_dbu(s) * 1e4) / 1e4); }
Json rect_um(const DesignSnapshot& s, const Rect& r) { return Json::array({um(s, r.xlo), um(s, r.ylo), um(s, r.xhi), um(s, r.yhi)}); }
Dbu to_dbu(const DesignSnapshot& s, double v_um) {
  return static_cast<Dbu>(std::llround(v_um * (s.dbu_per_micron() > 0 ? s.dbu_per_micron() : 1.0)));
}

std::shared_ptr<const DesignSnapshot> need_snapshot(const Session& s) {
  auto snap = s.snapshot();
  if (!snap) throw std::runtime_error("no design loaded (use load_design, or open one in the GUI)");
  return snap;
}

const char* dir_name(PinDir d) {
  switch (d) {
    case PinDir::Input: return "input";
    case PinDir::Output: return "output";
    default: return "inout";
  }
}

// Glob with '*' and '?'; without wildcards, a case-sensitive substring match.
bool name_matches(std::string_view name, std::string_view pat) {
  if (pat.find_first_of("*?") == std::string_view::npos) return name.find(pat) != std::string_view::npos;
  std::size_t n = 0, p = 0, star = std::string_view::npos, mark = 0;
  while (n < name.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == name[n])) {
      ++n;
      ++p;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = n;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      n = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

Json job_json(const Session& s) {
  Json j = Json::object({{"busy", s.busy()}});
  if (s.busy()) {
    const JobProgress p = s.job_progress();
    j.set("job", s.job_kind());
    j.set("stage", p.stage);
    j.set("fraction", std::round(p.fraction * 1000) / 1000);
    j.set("message", p.message);
    j.set("elapsed_s", std::round(p.elapsed_s * 100) / 100);
  }
  return j;
}

Json flow_json(const FlowReport& f) {
  Json j = Json::object();
  j.set("cancelled", f.cancelled);
  j.set("placement", Json::object({{"iterations", f.placement.iterations},
                                   {"hpwl_dbu", f.placement.hpwl},
                                   {"global_hpwl_dbu", f.placement.global_hpwl},
                                   {"density_overflow", f.placement.density_overflow},
                                   {"wns_ps", f.placement.wns_ps},
                                   {"tns_ps", f.placement.tns_ps}}));
  j.set("global_route", Json::object({{"wirelength_dbu", f.global_route.wirelength},
                                      {"total_overflow", f.global_route.total_overflow},
                                      {"max_utilization", f.global_route.max_utilization}}));
  j.set("final_wns_ps", f.final_wns_ps);
  j.set("final_tns_ps", f.final_tns_ps);
  Json st = Json::array();
  for (const auto& [n, t] : f.stage_seconds) st.push_back(Json::object({{"stage", n}, {"seconds", std::round(t * 1000) / 1000}}));
  j.set("stage_seconds", std::move(st));
  return j;
}

Json qor_headline(const QorReport& q) {
  const double u = q.dbu_per_micron > 0 ? 1.0 / q.dbu_per_micron : 1.0;
  return Json::object({{"hpwl_um", std::round(q.wirelength.hpwl * u)},
                       {"routed_wl_um", std::round(q.wirelength.routed * u)},
                       {"vias", q.wirelength.total_vias},
                       {"density_overflow", q.density.overflow},
                       {"peak_bin_util", q.density.peak_util},
                       {"congestion_total_overflow", q.congestion.total_overflow},
                       {"congestion_max_util", q.congestion.max_util},
                       {"overflow_gcells", q.congestion.overflow_gcells},
                       {"rc", q.congestion.rc}});
}

std::string level_name(log::Level l) {
  switch (l) {
    case log::Level::Error: return "error";
    case log::Level::Warn: return "warn";
    case log::Level::Info: return "info";
    default: return "debug";
  }
}

// Waits for the current job, then reports `done()`.
ToolResult wait_then(Session& s, double timeout_s, std::function<ToolResult()> done, std::string started) {
  ToolResult r = ok(std::move(started));
  Session* sp = &s;
  r.wait_until = [sp] {
    sp->poll();
    return !sp->busy();
  };
  r.then = std::move(done);
  r.timeout_s = timeout_s;
  return r;
}

void add_builtin_tools(ToolRegistry& R) {
  // ---------------------------------------------------------------- read
  R.add({"design_summary", "Design summary",
         "Summary of the loaded design: name, source, cell/macro/net/pin counts, die size (um), rows, routing "
         "layers, the running job, the last flow result and the latest QoR headline. Start here.",
         schema({}), Access::Read,
         [](ToolContext& c, const Json&) { return ok_json(design_summary(c.session)); }});

  R.add({"job_status", "Job status",
         "Whether a background job (load, flow, placement, QoR, save) is running, with stage, progress fraction "
         "and latest message.",
         schema({}), Access::Read, [](ToolContext& c, const Json&) {
           c.session.poll();
           return ok_json(job_json(c.session));
         }});

  R.add({"wait_for_job", "Wait for job",
         "Wait until the running job finishes (or timeout_s passes, default 60, max 900), then report the job state "
         "and the last flow result.",
         schema({{"timeout_s", prop("number", "Seconds to wait at most (default 60, max 900)")}}), Access::Read,
         [](ToolContext& c, const Json& a) {
           const double t = std::clamp(num_arg(a, "timeout_s", 60), 0.0, 900.0);
           Session* s = &c.session;
           if (!s->busy()) return ok_json(design_summary(*s));
           return wait_then(*s, t, [s] {
             Json j = design_summary(*s);
             if (s->busy()) j.set("note", "still running after the timeout; call wait_for_job again or cancel_job");
             return ok_json(j);
           }, "waiting");
         }});

  R.add({"qor_report", "QoR report",
         "The latest QoR report (density, 3-D track congestion per layer, hot spots, wirelength, vias, model notes) as "
         "text. If there is none, call compute_qor first.",
         schema({}), Access::Read, [](ToolContext& c, const Json&) {
           auto q = c.session.qor();
           if (!q) return fail("no QoR report yet: call compute_qor (or run_flow, which ends with one)");
           return ok(clip_text(format_qor(*q)));
         }});

  R.add({"compute_qor", "Compute QoR",
         "Compute a fresh QoR report for the current design as a background job (does not change the design) and "
         "return its headline numbers when done.",
         schema({{"timeout_s", prop("number", "Seconds to wait (default 300)")}}), Access::Read,
         [](ToolContext& c, const Json& a) {
           std::string why;
           if (!c.session.start_qor(&why)) return fail(why);
           Session* s = &c.session;
           return wait_then(*s, std::clamp(num_arg(a, "timeout_s", 300), 1.0, 1800.0), [s] {
             auto q = s->qor();
             if (s->busy()) return fail("QoR still running; call wait_for_job");
             if (!q) return fail("QoR job ended without a report (see read_log)");
             Json j = qor_headline(*q);
             j.set("note", "full report: qor_report; per-layer congestion: congestion_report");
             return ok_json(j);
           }, "computing QoR");
         }});

  R.add({"congestion_report", "Congestion report",
         "Routing congestion from the latest QoR report: per-layer capacity/demand/overflow/utilization, the worst "
         "GCells (hot spots, coordinates in um), ACE/RC and RUDY. Optional 'hot_spots' (default 10).",
         schema({{"hot_spots", prop("integer", "Number of hot spots to list (default 10)")}}), Access::Read,
         [](ToolContext& c, const Json& a) {
           auto q = c.session.qor();
           if (!q) return fail("no QoR report yet: call compute_qor first");
           const double u = q->dbu_per_micron > 0 ? 1.0 / q->dbu_per_micron : 1.0;
           const auto& cg = q->congestion;
           Json j = Json::object({{"model", cg.model},
                                  {"layers_from", cg.layer_source},
                                  {"usage_from", cg.usage_source},
                                  {"routed", cg.routed},
                                  {"grid", Json::array({cg.nx, cg.ny})},
                                  {"total_overflow", cg.total_overflow},
                                  {"h_overflow", cg.h_overflow},
                                  {"v_overflow", cg.v_overflow},
                                  {"overflow_gcells", cg.overflow_gcells},
                                  {"max_util", cg.max_util},
                                  {"ace_0.5_1_2_5", Json::array({cg.ace[0], cg.ace[1], cg.ace[2], cg.ace[3]})},
                                  {"rc", cg.rc},
                                  {"rudy", Json::object({{"h_overflow", cg.rudy_h_overflow},
                                                         {"v_overflow", cg.rudy_v_overflow},
                                                         {"max_util", cg.rudy_max_util}})}});
           Json layers = Json::array();
           for (const auto& l : cg.layers)
             layers.push_back(Json::object({{"name", l.name},
                                            {"dir", l.dir == LayerDir::Horizontal ? "H" : "V"},
                                            {"capacity", l.capacity},
                                            {"demand", l.demand},
                                            {"overflow", l.overflow},
                                            {"avg_util", l.avg_util},
                                            {"max_util", l.max_util},
                                            {"overflow_gcells", l.overflow_gcells},
                                            {"wirelength_um", std::round(l.wirelength * u)}}));
           j.set("layers", std::move(layers));
           const auto n = static_cast<std::size_t>(std::clamp<std::int64_t>(a["hot_spots"].as_int(10), 0, 100));
           Json hs = Json::array();
           for (std::size_t i = 0; i < std::min(n, cg.hot_spots.size()); ++i) {
             const auto& h = cg.hot_spots[i];
             hs.push_back(Json::object({{"gcell", Json::array({h.ix, h.iy})},
                                        {"box_um", Json::array({h.box.xlo * u, h.box.ylo * u, h.box.xhi * u, h.box.yhi * u})},
                                        {"util", h.util},
                                        {"overflow", h.overflow},
                                        {"worst_layer", h.worst_layer}}));
           }
           j.set("hot_spots", std::move(hs));
           Json notes = Json::array();
           for (const auto& s : q->notes) notes.push_back(s);
           j.set("notes", std::move(notes));
           return ok_json(j);
         }});

  R.add({"timing_report", "Timing report",
         "Timing from the last flow (WNS/TNS after placement and at the end) and the longest nets by HPWL with their "
         "estimated wire delay. The current STA is a placeholder wire-delay model (delay = HPWL * k), not Liberty-based.",
         schema({{"nets", prop("integer", "Number of longest nets to list (default 10)")},
                 {"ps_per_um", prop("number", "Wire delay per um for the estimate (default: the flow's 0.001 ps/DBU)")}}),
         Access::Read, [](ToolContext& c, const Json& a) {
           auto snap = need_snapshot(c.session);
           Json j = Json::object({{"model", "WireDelaySta: delay = HPWL * ps_per_dbu vs one clock period (placeholder)"}});
           if (auto f = c.session.last_flow()) {
             j.set("placement_wns_ps", f->placement.wns_ps);
             j.set("placement_tns_ps", f->placement.tns_ps);
             j.set("final_wns_ps", f->final_wns_ps);
             j.set("final_tns_ps", f->final_tns_ps);
           } else {
             j.set("note", "no flow has run in this session; slack numbers need run_flow");
           }
           const std::size_t n = static_cast<std::size_t>(std::clamp<std::int64_t>(a["nets"].as_int(10), 0, 200));
           const double ps_per_dbu = a["ps_per_um"].is_number() ? a["ps_per_um"].as_number() * um_per_dbu(*snap) : 0.001;
           std::vector<std::pair<double, Id>> top;
           for (std::size_t i = 0; i < snap->num_nets(); ++i) {
             const Id id = static_cast<Id>(i);
             if (snap->net(id).removed || snap->net_pins(id).size() < 2) continue;
             const Rect b = snap->net_bbox(id);
             const double h = static_cast<double>(b.width() + b.height());
             if (top.size() < n) {
               top.emplace_back(h, id);
               std::push_heap(top.begin(), top.end(), std::greater<>());
             } else if (n && h > top.front().first) {
               std::pop_heap(top.begin(), top.end(), std::greater<>());
               top.back() = {h, id};
               std::push_heap(top.begin(), top.end(), std::greater<>());
             }
           }
           std::sort(top.begin(), top.end(), std::greater<>());
           Json nets = Json::array();
           for (const auto& [h, id] : top)
             nets.push_back(Json::object({{"net", snap->net_name(id)},
                                          {"pins", snap->net_pins(id).size()},
                                          {"hpwl_um", um(*snap, static_cast<Dbu>(h))},
                                          {"est_delay_ps", std::round(h * ps_per_dbu * 1000) / 1000}}));
           j.set("longest_nets", std::move(nets));
           return ok_json(j);
         }});

  R.add({"query_cell", "Query cell",
         "Details of one cell by exact name: master, location and size (um), orientation, macro/fixed/placed flags and "
         "its pins with net names (up to 'max_pins', default 50).",
         schema({{"name", prop("string", "Cell instance name")}, {"max_pins", prop("integer", "Pins to list (default 50)")}},
                {"name"}),
         Access::Read, [](ToolContext& c, const Json& a) {
           auto snap = need_snapshot(c.session);
           const std::string name = need_string(a, "name");
           const Id id = snap->find_cell(name);
           if (id == kInvalidId) return fail("no cell named '" + name + "' (find_objects searches by pattern)");
           const Cell& cl = snap->cell(id);
           Json j = Json::object({{"name", name},
                                  {"master", snap->cell_master(id)},
                                  {"loc_um", Json::array({um(*snap, cl.loc.x), um(*snap, cl.loc.y)})},
                                  {"size_um", Json::array({um(*snap, cl.width), um(*snap, cl.height)})},
                                  {"bbox_um", rect_um(*snap, cl.bbox())},
                                  {"orient", static_cast<int>(cl.orient)},
                                  {"macro", cl.is_macro},
                                  {"fixed", cl.fixed},
                                  {"placed", cl.placed},
                                  {"removed", cl.removed}});
           const auto pins = snap->cell_pins(id);
           const std::size_t mp = static_cast<std::size_t>(std::clamp<std::int64_t>(a["max_pins"].as_int(50), 0, 1000));
           Json pj = Json::array();
           for (std::size_t i = 0; i < std::min(mp, pins.size()); ++i) {
             const Pin& p = snap->pin(pins[i]);
             pj.push_back(Json::object({{"pin", snap->pin_name(pins[i])},
                                        {"dir", dir_name(p.dir)},
                                        {"net", p.net == kInvalidId ? std::string_view("") : snap->net_name(p.net)}}));
           }
           j.set("pin_count", pins.size());
           j.set("pins", std::move(pj));
           return ok_json(j);
         }});

  R.add({"query_net", "Query net",
         "Details of one net by exact name: pin count, bounding box and HPWL (um), weight, and the connected cell pins "
         "(up to 'max_pins', default 50).",
         schema({{"name", prop("string", "Net name")}, {"max_pins", prop("integer", "Pins to list (default 50)")}}, {"name"}),
         Access::Read, [](ToolContext& c, const Json& a) {
           auto snap = need_snapshot(c.session);
           const std::string name = need_string(a, "name");
           const Id id = snap->find_net(name);
           if (id == kInvalidId) return fail("no net named '" + name + "' (find_objects searches by pattern)");
           const Rect b = snap->net_bbox(id);
           Json j = Json::object({{"name", name},
                                  {"weight", snap->net(id).weight},
                                  {"bbox_um", rect_um(*snap, b)},
                                  {"hpwl_um", um(*snap, b.width() + b.height())}});
           const auto pins = snap->net_pins(id);
           const std::size_t mp = static_cast<std::size_t>(std::clamp<std::int64_t>(a["max_pins"].as_int(50), 0, 1000));
           Json pj = Json::array();
           for (std::size_t i = 0; i < std::min(mp, pins.size()); ++i) {
             const Pin& p = snap->pin(pins[i]);
             const Point l = snap->pin_location(pins[i]);
             pj.push_back(Json::object({{"cell", p.cell == kInvalidId ? std::string_view("(I/O)") : snap->cell_name(p.cell)},
                                        {"pin", snap->pin_name(pins[i])},
                                        {"dir", dir_name(p.dir)},
                                        {"at_um", Json::array({um(*snap, l.x), um(*snap, l.y)})}}));
           }
           j.set("pin_count", pins.size());
           j.set("pins", std::move(pj));
           return ok_json(j);
         }});

  R.add({"find_objects", "Find cells or nets",
         "Find cells and/or nets whose name matches a pattern ('*' and '?' wildcards; without them, a substring). "
         "Returns up to 'limit' names (default 20) and the total match count.",
         schema({{"pattern", prop("string", "Name pattern, e.g. 'u_core/*reg*' or 'clk'")},
                 {"kind", Json::object({{"type", "string"}, {"enum", Json::array({"cell", "net", "any"})}, {"description", "What to search (default any)"}})},
                 {"limit", prop("integer", "Names to return (default 20, max 500)")}},
                {"pattern"}),
         Access::Read, [](ToolContext& c, const Json& a) {
           auto snap = need_snapshot(c.session);
           const std::string pat = need_string(a, "pattern");
           const std::string kind = a["kind"].as_string("any");
           const std::size_t limit = static_cast<std::size_t>(std::clamp<std::int64_t>(a["limit"].as_int(20), 1, 500));
           Json j = Json::object();
           auto scan = [&](bool cells) {
             Json names = Json::array();
             std::size_t total = 0;
             const std::size_t n = cells ? snap->num_cells() : snap->num_nets();
             for (std::size_t i = 0; i < n; ++i) {
               const Id id = static_cast<Id>(i);
               if (cells ? snap->cell(id).removed : snap->net(id).removed) continue;
               const std::string_view nm = cells ? snap->cell_name(id) : snap->net_name(id);
               if (!name_matches(nm, pat)) continue;
               if (++total <= limit) names.push_back(nm);
             }
             j.set(cells ? "cells" : "nets", Json::object({{"total", total}, {"names", std::move(names)}}));
           };
           if (kind != "net") scan(true);
           if (kind != "cell") scan(false);
           return ok_json(j);
         }});

  R.add({"cells_in_region", "Cells in a region",
         "Cells whose lower-left corner lies inside a window (um): count, std-cell/macro split, area utilization of the "
         "window and up to 'limit' cell names (default 20). Useful to inspect hot spots.",
         schema({{"x0", prop("number", "Left (um)")},
                 {"y0", prop("number", "Bottom (um)")},
                 {"x1", prop("number", "Right (um)")},
                 {"y1", prop("number", "Top (um)")},
                 {"limit", prop("integer", "Names to return (default 20)")}},
                {"x0", "y0", "x1", "y1"}),
         Access::Read, [](ToolContext& c, const Json& a) {
           auto snap = need_snapshot(c.session);
           const Rect w{to_dbu(*snap, num_arg(a, "x0", 0)), to_dbu(*snap, num_arg(a, "y0", 0)),
                        to_dbu(*snap, num_arg(a, "x1", 0)), to_dbu(*snap, num_arg(a, "y1", 0))};
           if (w.width() <= 0 || w.height() <= 0) return fail("empty window: need x1 > x0 and y1 > y0");
           const std::size_t limit = static_cast<std::size_t>(std::clamp<std::int64_t>(a["limit"].as_int(20), 0, 500));
           std::size_t n = 0, macros = 0, fixed = 0;
           double area = 0;
           Json names = Json::array();
           for (std::size_t i = 0; i < snap->num_cells(); ++i) {
             const Id id = static_cast<Id>(i);
             const Cell& cl = snap->cell(id);
             if (cl.removed || cl.loc.x < w.xlo || cl.loc.x >= w.xhi || cl.loc.y < w.ylo || cl.loc.y >= w.yhi) continue;
             ++n;
             macros += cl.is_macro;
             fixed += cl.fixed;
             area += static_cast<double>(cl.width) * static_cast<double>(cl.height);
             if (names.size() < limit) names.push_back(snap->cell_name(id));
           }
           return ok_json(Json::object({{"window_um", rect_um(*snap, w)},
                                        {"cells", n},
                                        {"macros", macros},
                                        {"fixed", fixed},
                                        {"cell_area_over_window", area / (static_cast<double>(w.width()) * static_cast<double>(w.height()))},
                                        {"names", std::move(names)}}));
         }});

  R.add({"read_log", "Read log",
         "Recent hermes log messages (info, warnings, errors) from this session, newest last. Optional 'lines' (default "
         "50), 'min_level' (error|warn|info), 'contains' (substring filter).",
         schema({{"lines", prop("integer", "How many (default 50, max 1000)")},
                 {"min_level", Json::object({{"type", "string"}, {"enum", Json::array({"error", "warn", "info"})}, {"description", "Lowest level shown (default info)"}})},
                 {"contains", prop("string", "Only messages containing this text")}}),
         Access::Read, [](ToolContext& c, const Json& a) {
           const std::size_t n = static_cast<std::size_t>(std::clamp<std::int64_t>(a["lines"].as_int(50), 1, 1000));
           const std::string lv = a["min_level"].as_string("info");
           const log::Level min = lv == "error" ? log::Level::Error : lv == "warn" ? log::Level::Warn : log::Level::Info;
           const std::string sub = a["contains"].as_string();
           std::vector<std::string> out;
           const auto lines = c.session.log_lines(5000);
           for (auto it = lines.rbegin(); it != lines.rend() && out.size() < n; ++it) {
             if (static_cast<int>(it->level) > static_cast<int>(min)) continue;
             if (!sub.empty() && it->text.find(sub) == std::string::npos) continue;
             out.push_back(std::format("[{:8.3f}] {} {}: {}", it->elapsed_s, level_name(it->level), it->channel, it->text));
           }
           std::string s;
           for (auto it = out.rbegin(); it != out.rend(); ++it) s += *it + "\n";
           return ok(s.empty() ? "(no matching log messages)" : clip_text(s));
         }});

  R.add({"run_history", "Run history",
         "Past hermes runs from the run-tracking database (~/.hermes/runs.db): id, name, status, design, start time, "
         "duration, threads, version and summary metrics. With 'run_id', the details of one run (settings, stages, "
         "results, warnings/errors). Filters: 'design', 'limit' (default 10).",
         schema({{"run_id", prop("string", "Show one run (prefix is enough)")},
                 {"design", prop("string", "Only runs of this design")},
                 {"limit", prop("integer", "Runs to list (default 10, max 100)")}}),
         Access::Read, [](ToolContext& c, const Json& a) {
           std::string err;
           Json j = a["run_id"].is_string()
                        ? run_details(c.runs_db, a["run_id"].str_ref(), &err)
                        : list_runs(c.runs_db, a["design"].as_string(),
                                    static_cast<int>(std::clamp<std::int64_t>(a["limit"].as_int(10), 1, 100)), &err);
           if (!err.empty()) return fail(err);
           return ok(clip_text(j.dump(1)));
         }});

  R.add({"list_commands", "List console commands",
         "The hermes console commands (the same as in hermes_gui and `hermes shell`) with their arguments. "
         "Commands marked * change the design. Use run_command to run one.",
         schema({}), Access::Read, [](ToolContext& c, const Json&) { return ok(c.commands.help()); }});

  R.add({"get_selection", "Current selection",
         "What the engineer has selected in the viewer (cell, net or pin), or nothing.", schema({}), Access::Read,
         [](ToolContext& c, const Json&) {
           const std::string s = c.view.selection_text();
           return ok(s.empty() ? "nothing selected" : s);
         }});

  R.add({"show_in_view", "Show in viewer",
         "Select a cell or net in the viewer and zoom to it, so the engineer sees what you are talking about. "
         "Does not change the design.",
         schema({{"name", prop("string", "Cell or net name")},
                 {"kind", Json::object({{"type", "string"}, {"enum", Json::array({"cell", "net", "any"})}, {"description", "default any"}})}},
                {"name"}),
         Access::Read, [](ToolContext& c, const Json& a) {
           const std::string n = need_string(a, "name");
           const std::string k = a["kind"].as_string("any");
           std::string why;
           if (k != "net" && c.view.select_cell(n, true, &why)) return ok("showing cell " + n);
           if (k != "cell" && c.view.select_net(n, true, &why)) return ok("showing net " + n);
           return fail(why.empty() ? "no such object" : why);
         }});

  // ---------------------------------------------------------------- change
  R.add({"load_design", "Load design",
         "Load a design directory or file (LEF/DEF, Bookshelf, .hdb; what `hermes <path>` reads), replacing the "
         "current one, and wait until it is loaded. 'options': reader options as key=value strings.",
         schema({{"path", prop("string", "Directory or file on the machine running hermes")},
                 {"options", Json::object({{"type", "array"}, {"items", Json::object({{"type", "string"}})}, {"description", "Reader options, key=value"}})},
                 {"timeout_s", prop("number", "Seconds to wait (default 600)")}},
                {"path"}),
         Access::Change, [](ToolContext& c, const Json& a) {
           const std::string p = need_string(a, "path");
           io::Options o;
           for (const auto& v : a["options"].items()) o.set(v.as_string());
           std::string why;
           if (!c.session.open(p, o, &why)) return fail(why);
           Session* s = &c.session;
           return wait_then(*s, std::clamp(num_arg(a, "timeout_s", 600), 1.0, 3600.0), [s] {
             if (s->busy()) return fail("still loading; call wait_for_job");
             if (!s->has_design()) return fail("load failed (see read_log)");
             return ok_json(design_summary(*s));
           }, "loading");
         }});

  R.add({"run_flow", "Run flow",
         "Run the hermes flow on the current design as a background job: placement, then (route=true, default) global "
         "and detailed routing, STA and a QoR report. Changes cell locations. 'options' holds flow settings: density, "
         "iters, overflow, dr, dp, legalize, keep_placed, timing, routability, grid, qor (numbers; booleans as 0/1). "
         "With wait=true it waits (timeout_s, default 600) and returns the result; otherwise use job_status/wait_for_job.",
         schema({{"route", prop("boolean", "Also route (default true); false = placement only")},
                 {"options", Json::object({{"type", "object"}, {"description", "Flow settings, e.g. {\"density\": 0.7, \"iters\": 300}"},
                                           {"additionalProperties", Json::object({{"type", "number"}})}})},
                 {"wait", prop("boolean", "Wait for the result (default false)")},
                 {"timeout_s", prop("number", "Seconds to wait when wait=true (default 600)")}}),
         Access::Change, [](ToolContext& c, const Json& a) {
           const bool route = bool_arg(a, "route", true);
           std::string line = route ? "run" : "place";
           for (const auto& [k, v] : a["options"].members()) {
             if (!v.is_number() && !v.is_bool()) return fail("option '" + k + "' must be a number");
             line += std::format(" {}={}", k, v.is_bool() ? (v.as_bool() ? 1.0 : 0.0) : v.as_number());
           }
           gui::CommandContext cc{c.session, c.view};
           gui::CommandResult r = c.commands.run(cc, line);
           if (!r.ok) return fail(r.text);
           if (!bool_arg(a, "wait", false)) return ok(r.text + " (" + line + ")");
           Session* s = &c.session;
           return wait_then(*s, std::clamp(num_arg(a, "timeout_s", 600), 1.0, 7200.0), [s] {
             Json j = design_summary(*s);
             if (s->busy()) j.set("note", "still running after the timeout; call wait_for_job or cancel_job");
             return ok_json(j);
           }, r.text);
         }});

  R.add({"cancel_job", "Cancel job", "Cancel the running background job at its next safe point.", schema({}),
         Access::Change, [](ToolContext& c, const Json&) {
           if (!c.session.busy()) return ok("no job running");
           c.session.cancel();
           return ok("cancel requested for " + c.session.job_kind());
         }});

  R.add({"move_cell", "Move cell",
         "Move one cell's lower-left corner to (x_um, y_um). The engineer can undo it with undo.",
         schema({{"name", prop("string", "Cell name")}, {"x_um", prop("number", "X (um)")}, {"y_um", prop("number", "Y (um)")}},
                {"name", "x_um", "y_um"}),
         Access::Change, [](ToolContext& c, const Json& a) {
           auto snap = need_snapshot(c.session);
           const std::string n = need_string(a, "name");
           const Id id = snap->find_cell(n);
           if (id == kInvalidId) return fail("no cell named '" + n + "'");
           std::string why;
           if (!c.session.move_cell(id, {to_dbu(*snap, num_arg(a, "x_um", 0)), to_dbu(*snap, num_arg(a, "y_um", 0))}, &why))
             return fail(why);
           return ok("moved " + n);
         }});

  R.add({"undo", "Undo", "Undo the last interactive edit (move).", schema({}), Access::Change,
         [](ToolContext& c, const Json&) {
           std::string why;
           if (!c.session.undo(&why)) return fail(why);
           return ok("undone");
         }});

  R.add({"save_design", "Save design",
         "Save the current design as a native .hdb snapshot (writes a file; waits until written).",
         schema({{"path", prop("string", "Output file, e.g. runs/try1.hdb")}}, {"path"}), Access::Change,
         [](ToolContext& c, const Json& a) {
           const std::string p = need_string(a, "path");
           std::string why;
           if (!c.session.save(p, &why)) return fail(why);
           Session* s = &c.session;
           return wait_then(*s, 600, [s, p] {
             if (s->busy()) return fail("still saving; call wait_for_job");
             return ok("saved " + p);
           }, "saving");
         }});

  {
    Tool t{"run_command", "Run console command",
           "Run one hermes console command line (see list_commands), exactly as the engineer would type it, e.g. "
           "'heatmap congestion.overflow' or 'zoom 0 0 200 200'. Commands that change the design or write files "
           "need the same permission as the dedicated tools.",
           schema({{"line", prop("string", "The command line")}}, {"line"}), Access::Change,
           [](ToolContext& c, const Json& a) {
             gui::CommandContext cc{c.session, c.view};
             gui::CommandResult r = c.commands.run(cc, need_string(a, "line"));
             ToolResult t2{r.ok, r.text.empty() ? (r.ok ? "ok" : "failed") : r.text, {}, {}, 0.0};
             return t2;
           }};
    t.access_for = [](const ToolContext& c, const Json& a) {
      const auto w = gui::split_command_line(a["line"].as_string());
      if (w.empty()) return Access::Read;
      const gui::Command* cmd = c.commands.find(w[0]);
      if (!cmd) return Access::Read;  // fails as unknown
      return cmd->edits_design || cmd->writes_files || w[0] == "cancel" ? Access::Change : Access::Read;
    };
    t.refuse = [](const ToolContext&, const Json& a) -> std::string {
      const auto w = gui::split_command_line(a["line"].as_string());
      if (!w.empty() && (w[0] == "quit" || w[0] == "wait" || w[0] == "settle" || w[0] == "ai"))
        return "'" + w[0] + "' is not available to the assistant (use wait_for_job to wait)";
      return {};
    };
    R.add(std::move(t));
  }
}

}  // namespace

ToolRegistry::ToolRegistry() { add_builtin_tools(*this); }

void ToolRegistry::add(Tool t) {
  if (auto it = index_.find(t.name); it != index_.end()) {
    tools_[it->second] = std::move(t);
    return;
  }
  index_[t.name] = tools_.size();
  tools_.push_back(std::move(t));
}

const Tool* ToolRegistry::find(std::string_view name) const {
  auto it = index_.find(name);
  return it == index_.end() ? nullptr : &tools_[it->second];
}

std::vector<const Tool*> ToolRegistry::list(bool include_change) const {
  std::vector<const Tool*> v;
  for (const auto& t : tools_)
    if (include_change || t.access == Access::Read || t.access_for) v.push_back(&t);
  return v;
}

ToolResult ToolRegistry::call(ToolContext& ctx, std::string_view name, const Json& args) const {
  const Tool* t = find(name);
  if (!t) return fail("unknown tool '" + std::string(name) + "'");
  const Json a = args.is_null() ? Json::object() : args;
  if (!a.is_object()) return fail("arguments must be a JSON object");
  try {
    if (t->refuse) {
      if (std::string why = t->refuse(ctx, a); !why.empty()) return fail(why);
    }
    return t->run(ctx, a);
  } catch (const std::exception& e) {
    return fail(e.what());
  }
}

std::string policy_refusal(const ToolRegistry& reg, const ToolContext& ctx, std::string_view name, const Json& args,
                           EditPolicy policy) {
  const Tool* t = reg.find(name);
  if (!t || policy != EditPolicy::ReadOnly) return {};
  if (t->access_of(ctx, args.is_object() ? args : Json::object()) == Access::Change)
    return "'" + std::string(name) +
           "' changes the design or writes files, and the assistant is read-only. Tell the engineer what you would "
           "run; they can run it themselves or enable edits.";
  return {};
}

bool tool_result_ready(ToolResult& r, double started_s) {
  if (!r.wait_until) return true;
  const bool done = r.wait_until();
  if (!done && steady_seconds() - started_s < r.timeout_s) return false;
  auto then = std::move(r.then);
  r = then ? then() : ToolResult{r.ok, r.text, {}, {}, 0.0};
  if (r.wait_until) return tool_result_ready(r, steady_seconds());
  return true;
}

ToolResult call_tool_blocking(const ToolRegistry& reg, ToolContext& ctx, std::string_view name, const Json& args) {
  ToolResult r = reg.call(ctx, name, args);
  const double t0 = steady_seconds();
  while (!tool_result_ready(r, t0)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return r;
}

// --- HeadlessView -----------------------------------------------------------------

bool HeadlessView::set_layer(std::string_view, int, int, std::string* why) {
  if (why) *why = "no viewer in this session (headless)";
  return false;
}
bool HeadlessView::set_heatmap(std::string_view, std::string* why) {
  if (why) *why = "no viewer in this session (headless)";
  return false;
}
bool HeadlessView::screenshot(const std::string&, std::string* why) {
  if (why) *why = "no viewer in this session (headless): use hermes_gui --screenshot";
  return false;
}
bool HeadlessView::select_cell(std::string_view name, bool, std::string* why) {
  auto snap = s_.snapshot();
  if (!snap || snap->find_cell(name) == kInvalidId) {
    if (why) *why = "no cell named '" + std::string(name) + "'";
    return false;
  }
  sel_ = "cell " + std::string(name) + " (" + std::string(snap->cell_master(snap->find_cell(name))) + ")";
  return true;
}
bool HeadlessView::select_net(std::string_view name, bool, std::string* why) {
  auto snap = s_.snapshot();
  if (!snap || snap->find_net(name) == kInvalidId) {
    if (why) *why = "no net named '" + std::string(name) + "'";
    return false;
  }
  sel_ = std::format("net {} ({} pins)", name, snap->net_pins(snap->find_net(name)).size());
  return true;
}
std::string HeadlessView::stats() const { return design_summary(s_).dump(1); }

// --- summary ------------------------------------------------------------------------

Json design_summary(const Session& s) {
  Json j = Json::object();
  auto snap = s.snapshot();
  if (!snap) {
    j.set("design", nullptr);
    j.set("note", "no design loaded");
  } else {
    std::size_t macros = 0, fixed = 0, placed = 0, live = 0;
    for (std::size_t i = 0; i < snap->num_cells(); ++i) {
      const Cell& c = snap->cell(static_cast<Id>(i));
      if (c.removed) continue;
      ++live;
      macros += c.is_macro;
      fixed += c.fixed;
      placed += c.placed;
    }
    j.set("design", snap->name());
    j.set("source", s.source());
    j.set("cells", live);
    j.set("macros", macros);
    j.set("fixed_cells", fixed);
    j.set("placed_cells", placed);
    j.set("nets", snap->num_nets());
    j.set("pins", snap->num_pins());
    j.set("io_pins", snap->io_pins().size());
    j.set("die_um", rect_um(*snap, snap->die()));
    j.set("rows", snap->rows().size());
    j.set("row_height_um", um(*snap, snap->row_height()));
    j.set("dbu_per_micron", snap->dbu_per_micron());
    Json layers = Json::array();
    for (const auto& l : snap->routing_layers()) layers.push_back(l.name);
    j.set("routing_layers", std::move(layers));
    j.set("undo_depth", s.undo_depth());
  }
  j.set("job", job_json(s));
  if (auto f = s.last_flow()) j.set("last_flow", flow_json(*f));
  if (auto q = s.qor()) j.set("qor", qor_headline(*q));
  return j;
}

}  // namespace hermes::assist
