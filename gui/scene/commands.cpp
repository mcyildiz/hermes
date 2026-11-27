#include "gui/scene/commands.hpp"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <type_traits>

#include "gui/scene/session.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/qor/qor.hpp"

namespace hermes::gui {

std::vector<std::string> split_command_line(std::string_view line) {
  std::vector<std::string> out;
  std::string cur;
  bool quote = false, any = false;
  for (char c : line) {
    if (c == '"') {
      quote = !quote;
      any = true;
    } else if (!quote && (c == ' ' || c == '\t')) {
      if (any) out.push_back(std::move(cur));
      cur.clear();
      any = false;
    } else {
      cur += c;
      any = true;
    }
  }
  if (any) out.push_back(std::move(cur));
  return out;
}

namespace {

CommandResult fail(std::string t) { return {false, std::move(t), {}, {}}; }
CommandResult ok(std::string t = {}) { return {true, std::move(t), {}, {}}; }

double dbu_per_um(const Session& s) {
  auto snap = s.snapshot();
  return snap && snap->dbu_per_micron() > 0 ? snap->dbu_per_micron() : 1.0;
}

bool parse_double(const std::string& s, double& v) {
  char* e = nullptr;
  v = std::strtod(s.c_str(), &e);
  return e && *e == 0 && !s.empty();
}

// k=v flow options: density=0.7 iters=300 route=0 dp=0 legalize=0 threads=N
bool apply_flow_opt(FlowOptions& f, const std::string& kv, std::string* why) {
  const auto eq = kv.find('=');
  if (eq == std::string::npos) {
    *why = "expected key=value, got '" + kv + "'";
    return false;
  }
  const std::string k = kv.substr(0, eq);
  double v = 0;
  if (!parse_double(kv.substr(eq + 1), v)) {
    *why = "bad number in '" + kv + "'";
    return false;
  }
  if (k == "density") f.placer.target_density = v;
  else if (k == "iters") f.placer.max_iterations = static_cast<int>(v);
  else if (k == "overflow") f.placer.stop_overflow = v;
  else if (k == "route") f.run_routing = v != 0;
  else if (k == "dr") f.run_detailed_route = v != 0;
  else if (k == "dp") f.placer.detailed_placement = v != 0;
  else if (k == "legalize") f.placer.legalize = v != 0;
  else if (k == "keep_placed") f.placer.keep_placed = v != 0;
  else if (k == "timing") f.placer.timing_interval = static_cast<int>(v);
  else if (k == "routability") f.placer.routability_interval = static_cast<int>(v);
  else if (k == "grid") f.router.grid_x = f.router.grid_y = static_cast<int>(v);
  else if (k == "qor") f.report_qor = v != 0;
  else if (k == "clock") f.clock_period_ps = v;
  else if (k == "refine") f.placer.refine_iterations = static_cast<int>(v);
  else {
    *why = "unknown flow option '" + k +
           "' (density iters overflow route dr dp legalize refine keep_placed timing routability grid clock qor)";
    return false;
  }
  return true;
}

/// key=value arguments of a step command. Values stay text; each command
/// takes the keys it knows and reports the rest.
struct Kv {
  std::map<std::string, std::string, std::less<>> m;
  bool has(std::string_view k) const { return m.find(k) != m.end(); }
};

bool parse_kv(const std::vector<std::string>& a, std::size_t from, Kv& kv, std::string* why) {
  for (std::size_t i = from; i < a.size(); ++i) {
    const auto eq = a[i].find('=');
    if (eq == std::string::npos || eq == 0) {
      *why = "expected key=value, got '" + a[i] + "'";
      return false;
    }
    kv.m[a[i].substr(0, eq)] = a[i].substr(eq + 1);
  }
  return true;
}

bool only_keys(const Kv& kv, std::initializer_list<std::string_view> keys, std::string* why) {
  for (const auto& [k, v] : kv.m) {
    if (std::find(keys.begin(), keys.end(), k) != keys.end()) continue;
    std::string list;
    for (auto x : keys) list += (list.empty() ? "" : " ") + std::string(x);
    *why = "unknown option '" + k + "' (" + list + ")";
    return false;
  }
  return true;
}

bool kv_num(const Kv& kv, std::string_view k, double& v, std::string* why) {
  auto it = kv.m.find(k);
  if (it == kv.m.end()) return true;
  if (!parse_double(it->second, v)) {
    *why = "bad number in '" + std::string(k) + "=" + it->second + "'";
    return false;
  }
  return true;
}

template <class T>
bool kv_set(const Kv& kv, std::string_view k, T& field, std::string* why) {
  double v = static_cast<double>(field);
  if (!kv_num(kv, k, v, why)) return false;
  if constexpr (std::is_same_v<T, bool>) field = v != 0;
  else if constexpr (std::is_integral_v<T>) field = static_cast<T>(std::llround(v));
  else field = static_cast<T>(v);
  return true;
}

/// region=x0,y0,x1,y1 in microns.
bool kv_region(const Kv& kv, double dbu_per_um, Rect& r, std::string* why) {
  auto it = kv.m.find("region");
  if (it == kv.m.end()) return true;
  double v[4];
  std::string t = it->second;
  for (char& ch : t)
    if (ch == ',') ch = ' ';
  const auto w = split_command_line(t);
  bool good = w.size() == 4;
  for (int i = 0; good && i < 4; ++i) good = parse_double(w[static_cast<std::size_t>(i)], v[i]);
  if (!good || v[2] <= v[0] || v[3] <= v[1]) {
    *why = "region must be x0,y0,x1,y1 in microns with x1>x0, y1>y0";
    return false;
  }
  r = {static_cast<Dbu>(std::llround(v[0] * dbu_per_um)), static_cast<Dbu>(std::llround(v[1] * dbu_per_um)),
       static_cast<Dbu>(std::llround(v[2] * dbu_per_um)), static_cast<Dbu>(std::llround(v[3] * dbu_per_um))};
  return true;
}

const char* kPlaceKeys = "density iters overflow legalize refine dp keep_placed timing routability seed clock route";

bool placer_kv(const Kv& kv, PlacerOptions& p, std::string* why) {
  return kv_set(kv, "density", p.target_density, why) && kv_set(kv, "iters", p.max_iterations, why) &&
         kv_set(kv, "overflow", p.stop_overflow, why) && kv_set(kv, "legalize", p.legalize, why) &&
         kv_set(kv, "refine", p.refine_iterations, why) && kv_set(kv, "dp", p.detailed_placement, why) &&
         kv_set(kv, "keep_placed", p.keep_placed, why) && kv_set(kv, "timing", p.timing_interval, why) &&
         kv_set(kv, "routability", p.routability_interval, why) && kv_set(kv, "seed", p.seed, why);
}

/// Router option keys; true in *changed if any was given.
bool router_kv(const Kv& kv, GlobalRouterOptions& o, bool* changed, std::string* why) {
  *changed = kv.has("grid") || kv.has("gcell_rows") || kv.has("rrr_iters") || kv.has("layer_assignment");
  if (kv.has("grid")) {
    double g = 0;
    if (!kv_num(kv, "grid", g, why)) return false;
    o.grid_x = o.grid_y = static_cast<int>(g);
  }
  return kv_set(kv, "gcell_rows", o.gcell_rows, why) && kv_set(kv, "rrr_iters", o.max_iterations, why) &&
         kv_set(kv, "layer_assignment", o.layer_assignment, why);
}

std::string um_text(double dbu, double dbu_per_um) {
  return dbu_per_um > 1.0 ? std::format("{:.6g} um", dbu / dbu_per_um) : std::format("{:.6g}", dbu);
}

std::string placement_text(const PlacementResult& r, bool legalized, double dbu_per_um) {
  return std::format("placement{}: {} iterations, HPWL {}, density overflow {:.4f}, est. routing overflow {:.4g}{}",
                     r.cancelled ? " (cancelled)" : "", r.iterations, um_text(r.hpwl, dbu_per_um), r.density_overflow,
                     r.overflow,
                     legalized ? std::format(", legalized in {:.0f} ms", r.legalize_ms) : ", not legalized");
}

std::string route_text(const RouteStepResult& r, double dbu_per_um) {
  const GlobalRouteResult& g = r.stats;
  std::string head = r.incremental ? std::format("global route (incremental, {} nets rerouted, {} repaired)",
                                                 r.reroute.nets_rerouted, r.reroute.nets_repaired)
                                   : std::format("global route ({} nets, {} RRR iterations)", g.nets_routed, g.iterations);
  return std::format("{}{}: WL {}, overflow {:.4g} on {} edges, max util {:.2f}, {} vias, {:.3f} s", head,
                     g.cancelled ? " (cancelled)" : "", um_text(g.wirelength, dbu_per_um), g.total_overflow,
                     g.overflow_edges, g.max_utilization, g.vias, r.seconds);
}

std::string timing_text(const TimingStepResult& t) {
  return std::format("timing: WNS {:.2f} ps, TNS {:.2f} ps (clock {:g} ps; placeholder wire-delay STA)", t.wns_ps,
                     t.tns_ps, t.clock_period_ps);
}

std::string legalize_text(const LegalizeStepResult& r, double dbu_per_um) {
  const double d = r.hpwl_before > 0 ? 100.0 * (r.hpwl_after / r.hpwl_before - 1.0) : 0.0;
  return std::format("legalize: {} cells ({} failed), {} macros, HPWL {} -> {} ({:+.2f}%), {}, {:.3f} s",
                     r.cells.legalized, r.cells.failed, r.macros.moved, um_text(r.hpwl_before, dbu_per_um),
                     um_text(r.hpwl_after, dbu_per_um), d,
                     r.legal ? "legal" : "NOT legal: " + r.violation, r.seconds);
}

}  // namespace

CommandRegistry::CommandRegistry() {
  add({"help", "[command]", "List commands, or show one command's help", false,
       [this](CommandContext&, const std::vector<std::string>& a) {
         if (a.size() > 1) {
           const Command* c = find(a[1]);
           if (!c) return fail("no command '" + a[1] + "'");
           return ok(c->name + " " + c->args + "\n  " + c->help + (c->edits_design ? "\n  (changes the design)" : ""));
         }
         return ok(help());
       }});
  add({"open", "<path> [key=value ...]", "Open a directory or file (LEF/DEF, Bookshelf, .hdb) in the background",
       true, [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() < 2) return fail("usage: open <path> [key=value ...]");
         io::Options o;
         for (std::size_t i = 2; i < a.size(); ++i) o.set(a[i]);
         std::string why;
         if (!c.session.open(a[1], o, &why)) return fail(why);
         return ok("loading " + a[1] + " ...");
       }});
  add({"synth", "<cells> [ordered|random|unplaced]", "Generate a synthetic design (benchmarks, demos)", true,
       [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() < 2) return fail("usage: synth <cells> [ordered|random|unplaced]");
         SyntheticOptions o;
         double n = 0;
         if (!parse_double(a[1], n) || n < 1) return fail("bad cell count '" + a[1] + "'");
         o.cells = static_cast<int>(n);
         if (a.size() > 2) {
           if (a[2] == "random") o.placement = SyntheticOptions::Placement::Random;
           else if (a[2] == "unplaced") o.placement = SyntheticOptions::Placement::Unplaced;
           else if (a[2] != "ordered") return fail("placement must be ordered, random or unplaced");
         }
         std::string why;
         if (!c.session.open_synthetic(o, &why)) return fail(why);
         return ok(std::format("generating {} cells ...", o.cells));
       }});
  add({"flow", "[key=value ...]",
       "Run the whole flow as one background job: place, global route, timing, QoR. Options: density iters "
       "overflow route dr dp legalize refine keep_placed timing routability grid clock qor",
       true, [](CommandContext& c, const std::vector<std::string>& a) {
         FlowOptions f;
         std::string why;
         for (std::size_t i = 1; i < a.size(); ++i)
           if (!apply_flow_opt(f, a[i], &why)) return fail(why);
         if (!c.session.start_flow(f, &why)) return fail(why);
         return ok(std::string(f.run_routing ? "flow" : "flow (no routing)") +
                   " started in the background; the view updates live");
       }});
  add({"run", "[key=value ...]", "Same as 'flow'", true,
       [this](CommandContext& c, const std::vector<std::string>& a) { return find("flow")->run(c, a); }});

  add({"place", "[key=value ...]",
       std::string("Mixed-size placement: global placement with timing and routability feedback (the global router "
                   "is the congestion oracle), then legalization. route=1 also global-routes the result. Options: ") +
           kPlaceKeys,
       true, [](CommandContext& c, const std::vector<std::string>& a) {
         Kv kv;
         PlacerOptions p;
         std::string why;
         double clock = -1;
         bool route = false;
         if (!parse_kv(a, 1, kv, &why) ||
             !only_keys(kv, {"density", "iters", "overflow", "legalize", "refine", "dp", "keep_placed", "timing",
                             "routability", "seed", "clock", "route"},
                        &why) ||
             !placer_kv(kv, p, &why) || !kv_num(kv, "clock", clock, &why) || !kv_set(kv, "route", route, &why))
           return fail(why);
         const double s = dbu_per_um(c.session);
         if (!c.session.start_task(
                 route ? "placement + route" : "placement",
                 [p, clock, route, s](Design& d, FlowEngines& eng, RunControl& ctl, TaskOutput& out) {
                   if (clock > 0) eng.clock_period_ps = clock;
                   ctl.begin_stage("placement", 0.0, route ? 0.85 : 1.0);
                   out.placement = place_design(d, eng, p, &ctl);
                   out.summary = placement_text(*out.placement, p.legalize, s);
                   if (!route || out.placement->cancelled || ctl.cancelled()) return;
                   ctl.begin_stage("global route", 0.85, 1.0);
                   out.route = global_route_design(d, eng, RouteMode::Full, &ctl);
                   out.summary += "\n" + route_text(*out.route, s);
                 },
                 true, &why))
           return fail(why);
         return ok(std::string(route ? "placement + global route" : "placement") + " started in the background");
       }});

  add({"global_place", "[key=value ...]",
       "Global placement only (no legalization), with timing and routability feedback. Options: density iters "
       "overflow keep_placed timing routability seed clock",
       true, [](CommandContext& c, const std::vector<std::string>& a) {
         Kv kv;
         PlacerOptions p;
         std::string why;
         double clock = -1;
         if (!parse_kv(a, 1, kv, &why) ||
             !only_keys(kv, {"density", "iters", "overflow", "keep_placed", "timing", "routability", "seed", "clock"},
                        &why) ||
             !placer_kv(kv, p, &why) || !kv_num(kv, "clock", clock, &why))
           return fail(why);
         const double s = dbu_per_um(c.session);
         if (!c.session.start_task(
                 "global placement",
                 [p, clock, s](Design& d, FlowEngines& eng, RunControl& ctl, TaskOutput& out) {
                   if (clock > 0) eng.clock_period_ps = clock;
                   ctl.begin_stage("global placement", 0.0, 1.0);
                   out.placement = global_place_design(d, eng, p, &ctl);
                   out.summary = placement_text(*out.placement, false, s);
                 },
                 true, &why))
           return fail(why);
         return ok("global placement started in the background");
       }});

  add({"legalize", "[macros=0|1] [cells=0|1] [region=x0,y0,x1,y1]",
       "Legalize the current placement: macros (then fixed), then std cells into rows and sites (Abacus). With "
       "region (microns) only std cells inside it move. Keeps global routes; the next global_route is incremental",
       true, [](CommandContext& c, const std::vector<std::string>& a) {
         Kv kv;
         LegalizeStepOptions o;
         std::string why;
         if (!parse_kv(a, 1, kv, &why) || !only_keys(kv, {"macros", "cells", "region"}, &why) ||
             !kv_set(kv, "macros", o.macros, &why) || !kv_set(kv, "cells", o.cells, &why) ||
             !kv_region(kv, dbu_per_um(c.session), o.fence, &why))
           return fail(why);
         const double s = dbu_per_um(c.session);
         if (!c.session.start_task(
                 "legalization",
                 [o, s](Design& d, FlowEngines&, RunControl& ctl, TaskOutput& out) {
                   ctl.begin_stage("legalization", 0.0, 1.0);
                   out.legalize = legalize_design(d, o);
                   out.summary = legalize_text(*out.legalize, s);
                 },
                 true, &why))
           return fail(why);
         return ok("legalization started in the background");
       }});

  add({"global_route", "[mode=auto|full|incremental] [region=x0,y0,x1,y1] [grid=N gcell_rows=R rrr_iters=N]",
       "Global routing on the current placement (any placed design, e.g. one loaded from DEF). auto: incremental "
       "when routes exist (only nets whose pins changed GCell), else full. region (microns): reroute only there. "
       "Router options force a full route",
       false, [](CommandContext& c, const std::vector<std::string>& a) {
         Kv kv;
         std::string why;
         if (!parse_kv(a, 1, kv, &why) ||
             !only_keys(kv, {"mode", "region", "grid", "gcell_rows", "rrr_iters", "layer_assignment"}, &why))
           return fail(why);
         RouteMode mode = RouteMode::Auto;
         if (auto it = kv.m.find("mode"); it != kv.m.end()) {
           if (it->second == "full") mode = RouteMode::Full;
           else if (it->second == "incremental" || it->second == "incr") mode = RouteMode::Incremental;
           else if (it->second != "auto") return fail("mode must be auto, full or incremental");
         }
         Rect region{};
         const double s = dbu_per_um(c.session);
         if (!kv_region(kv, s, region, &why)) return fail(why);
         const bool has_region = kv.has("region");
         const Kv opts = kv;
         if (!c.session.start_task(
                 "global route",
                 [opts, mode, region, has_region, s](Design& d, FlowEngines& eng, RunControl& ctl, TaskOutput& out) {
                   GlobalRouterOptions ro = eng.router.options();
                   bool changed = false;
                   std::string why;
                   if (!router_kv(opts, ro, &changed, &why)) throw std::invalid_argument(why);
                   RouteMode m = mode;
                   if (changed) {
                     eng.router.set_options(ro);
                     if (m == RouteMode::Auto) m = RouteMode::Full;
                   }
                   ctl.begin_stage("global route", 0.0, 1.0);
                   out.route = has_region ? reroute_region_design(d, eng, region, &ctl)
                                          : global_route_design(d, eng, m, &ctl);
                   out.summary = route_text(*out.route, s);
                 },
                 false, &why))
           return fail(why);
         return ok("global route started in the background");
       }});

  add({"timing", "[clock=PS]",
       "Static timing on the current placement; prints WNS/TNS. clock sets the clock period for this and later "
       "steps (placeholder wire-delay STA until a Liberty engine exists)",
       false, [](CommandContext& c, const std::vector<std::string>& a) {
         Kv kv;
         std::string why;
         double clock = -1, wd = -1;
         if (!parse_kv(a, 1, kv, &why) || !only_keys(kv, {"clock", "wire_delay"}, &why) ||
             !kv_num(kv, "clock", clock, &why) || !kv_num(kv, "wire_delay", wd, &why))
           return fail(why);
         if (!c.session.start_task(
                 "timing",
                 [clock, wd](Design& d, FlowEngines& eng, RunControl& ctl, TaskOutput& out) {
                   if (clock > 0) eng.clock_period_ps = clock;
                   if (wd > 0) eng.wire_delay_ps_per_dbu = wd;
                   ctl.begin_stage("timing", 0.0, 1.0);
                   out.timing = time_design(d, eng);
                   out.summary = timing_text(*out.timing);
                 },
                 false, &why))
           return fail(why);
         return ok("timing started in the background");
       }});

  add({"cancel", "", "Cancel the running job at its next safe point", false,
       [](CommandContext& c, const std::vector<std::string>&) {
         if (!c.session.busy()) return ok("no job running");
         c.session.cancel();
         return ok("cancel requested");
       },
       false, true});
  add({"wait", "", "Wait for the running job to finish (scripts wait on their own before each step)", false,
       [](CommandContext& c, const std::vector<std::string>&) {
         CommandResult r = ok();
         Session* s = &c.session;
         r.wait_until = [s] { return !s->busy(); };
         return r;
       },
       false, true});
  add({"settle", "", "Wait until the view shows the latest design with full detail (scripts)", false,
       [](CommandContext& c, const std::vector<std::string>&) {
         CommandResult r = ok();
         ViewApi* v = &c.view;
         r.wait_until = [v] { return v->view_settled(); };
         return r;
       }});
  add({"qor", "", "Compute the QoR report (density, congestion from the global routes if any, wirelength) in the "
       "background; 'report' prints it", false,
       [](CommandContext& c, const std::vector<std::string>&) {
         std::string why;
         if (!c.session.start_qor(&why)) return fail(why);
         return ok("computing QoR ...");
       }});
  add({"report", "[file.txt|file.json]", "Print the last QoR report; with a file, also write it there", false,
       [](CommandContext& c, const std::vector<std::string>& a) {
         auto q = c.session.qor();
         if (!q) return fail("no QoR report yet (run 'qor' or 'flow')");
         const std::string text = format_qor(*q);
         if (a.size() > 1) {
           const bool json = std::filesystem::path(a[1]).extension() == ".json";
           std::ofstream f(a[1]);
           if (!f) return fail("cannot write " + a[1]);
           f << (json ? qor_to_json(*q) : text);
           return ok(text + "\nwrote " + a[1]);
         }
         return ok(text);
       },
       true});
  add({"results", "", "One line per step: the last placement, legalization, global route, timing and QoR results",
       false, [](CommandContext& c, const std::vector<std::string>&) {
         const Session& s = c.session;
         std::string t;
         if (auto p = s.last_placement()) t += placement_text(*p, p->legalize_ms > 0, dbu_per_um(s)) + "\n";
         if (auto l = s.last_legalize()) t += legalize_text(*l, dbu_per_um(s)) + "\n";
         if (auto r = s.last_route()) t += route_text(*r, dbu_per_um(s)) + "\n";
         if (auto tm = s.last_timing()) t += timing_text(*tm) + "\n";
         if (auto q = s.qor())
           t += std::format("qor: HPWL {}, density overflow {:.4f}, congestion overflow {:.4g} ({})\n",
                            um_text(q->wirelength.hpwl, dbu_per_um(s)), q->density.overflow, q->congestion.total_overflow,
                            q->congestion.routed ? "global routes" : "estimate");
         if (t.empty()) return ok("no results yet");
         t.pop_back();
         return ok(t);
       }});
  add({"save", "<file.hdb|.def|.pl|.aux> [key=value ...]",
       "Save the design (.hdb: native, fast) or write it as DEF / Bookshelf, in the background", false,
       [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() < 2) return fail("usage: save <file.hdb|.def|.pl|.aux> [key=value ...]");
         io::Options o;
         for (std::size_t i = 2; i < a.size(); ++i) o.set(a[i]);
         std::string why;
         if (!c.session.save(a[1], &why, o)) return fail(why);
         return ok("saving " + a[1] + " ...");
       },
       true});
  add({"write", "<file> [key=value ...]", "Same as 'save'", false,
       [this](CommandContext& c, const std::vector<std::string>& a) { return find("save")->run(c, a); }, true});
  add({"source", "<file>", "Run the commands in a file (one per line, # comments)", false,
       [](CommandContext&, const std::vector<std::string>& a) {
         if (a.size() < 2) return fail("usage: source <file>");
         CommandResult r = ok();
         r.insert_lines = read_command_file(a[1]);
         return r;
       }});
  add({"select", "cell|net <name>", "Select a cell or net by name and zoom to it", false,
       [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() < 3) return fail("usage: select cell|net <name>");
         std::string why;
         bool r = a[1] == "net" ? c.view.select_net(a[2], true, &why) : c.view.select_cell(a[2], true, &why);
         return r ? ok() : fail(why);
       }});
  add({"find", "<name>", "Select a cell, else a net, by name and zoom to it", false,
       [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() < 2) return fail("usage: find <name>");
         std::string why;
         if (c.view.select_cell(a[1], true, &why)) return ok();
         if (c.view.select_net(a[1], true, &why)) return ok();
         return fail("no cell or net named '" + a[1] + "'");
       }});
  add({"clear", "", "Clear the selection", false, [](CommandContext& c, const std::vector<std::string>&) {
         c.view.clear_selection();
         return ok();
       }});
  add({"zoom", "fit | <x0> <y0> <x1> <y1> (um)", "Zoom to the whole die or a window in microns", false,
       [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() == 2 && a[1] == "fit") {
           c.view.zoom_fit();
           return ok();
         }
         if (a.size() != 5) return fail("usage: zoom fit | zoom <x0> <y0> <x1> <y1>");
         double v[4];
         for (int i = 0; i < 4; ++i)
           if (!parse_double(a[static_cast<std::size_t>(i) + 1], v[i])) return fail("bad number");
         const double s = dbu_per_um(c.session);
         c.view.zoom_to({static_cast<Dbu>(std::llround(v[0] * s)), static_cast<Dbu>(std::llround(v[1] * s)),
                         static_cast<Dbu>(std::llround(v[2] * s)), static_cast<Dbu>(std::llround(v[3] * s))});
         return ok();
       }});
  add({"layer", "<name> on|off|select|noselect", "Show, hide, or make a layer (not) selectable", false,
       [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() < 3) return fail("usage: layer <name> on|off|select|noselect");
         int vis = -1, sel = -1;
         for (std::size_t i = 2; i < a.size(); ++i) {
           if (a[i] == "on") vis = 1;
           else if (a[i] == "off") vis = 0;
           else if (a[i] == "select") sel = 1;
           else if (a[i] == "noselect") sel = 0;
           else return fail("expected on|off|select|noselect, got '" + a[i] + "'");
         }
         std::string why;
         if (!c.view.set_layer(a[1], vis, sel, &why)) return fail(why);
         return ok();
       }});
  add({"layers", "", "List display layers", false,
       [](CommandContext& c, const std::vector<std::string>&) { return ok(c.view.layer_list()); }});
  add({"heatmap", "<name>|off", "Show a heatmap overlay (see 'heatmaps')", false,
       [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() < 2) return fail("usage: heatmap <name>|off");
         std::string why;
         if (!c.view.set_heatmap(a[1], &why)) return fail(why);
         return ok();
       }});
  add({"heatmaps", "", "List available heatmaps", false,
       [](CommandContext& c, const std::vector<std::string>&) { return ok(c.view.heatmap_list()); }});
  add({"move", "<cell> <x_um> <y_um>", "Move a cell (lower-left corner, microns); undo with 'undo'", true,
       [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() != 4) return fail("usage: move <cell> <x_um> <y_um>");
         auto snap = c.session.snapshot();
         if (!snap) return fail("no design loaded");
         const Id id = snap->find_cell(a[1]);
         if (id == kInvalidId) return fail("no cell named '" + a[1] + "'");
         double x, y;
         if (!parse_double(a[2], x) || !parse_double(a[3], y)) return fail("bad number");
         const double s = dbu_per_um(c.session);
         std::string why;
         if (!c.session.move_cell(id, {static_cast<Dbu>(std::llround(x * s)), static_cast<Dbu>(std::llround(y * s))},
                                  &why))
           return fail(why);
         return ok();
       }});
  add({"undo", "", "Undo the last edit made in the GUI", true, [](CommandContext& c, const std::vector<std::string>&) {
         std::string why;
         if (!c.session.undo(&why)) return fail(why);
         return ok();
       }});
  add({"info", "", "Design, job and view statistics", false,
       [](CommandContext& c, const std::vector<std::string>&) { return ok(c.view.stats()); }});
  add({"threads", "[n]", "Show or set the threads background jobs use", false,
       [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() > 1) {
           double n = 0;
           if (!parse_double(a[1], n) || n < 0) return fail("bad thread count");
           c.session.set_job_threads(static_cast<int>(n));
         }
         return ok(std::format("jobs use {} of {} hardware threads", c.session.job_threads(), par::hardware_threads()));
       }});
  add({"screenshot", "<file.png>", "Save the current view as PNG", false,
       [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() < 2) return fail("usage: screenshot <file.png>");
         std::string why;
         if (!c.view.screenshot(a[1], &why)) return fail(why);
         CommandResult r = ok();
         ViewApi* v = &c.view;
         r.wait_until = [v] { return !v->screenshot_pending(); };
         return r;
       },
       true});
  add({"echo", "<text>", "Print text (scripts)", false, [](CommandContext&, const std::vector<std::string>& a) {
         std::string t;
         for (std::size_t i = 1; i < a.size(); ++i) t += (i > 1 ? " " : "") + a[i];
         return ok(t);
       }});
  add({"quit", "", "Close the GUI (cancels a running job)", false,
       [](CommandContext& c, const std::vector<std::string>&) {
         c.view.request_quit();
         return ok();
       }});
}

void CommandRegistry::add(Command c) {
  if (c.name == "help" || c.name == "select" || c.name == "find" || c.name == "clear" || c.name == "zoom" ||
      c.name == "layer" || c.name == "layers" || c.name == "heatmap" || c.name == "heatmaps" || c.name == "info" ||
      c.name == "threads" || c.name == "ai" || c.name == "frames" ||
      c.name == "sleep" || c.name == "bench_frames" || c.name == "zoomfrac" || c.name == "chat" || c.name == "panel")
    c.immediate = true;
  std::string n = c.name;
  cmds_[std::move(n)] = std::move(c);
}

const Command* CommandRegistry::find(std::string_view name) const {
  auto it = cmds_.find(name);
  return it == cmds_.end() ? nullptr : &it->second;
}

std::vector<const Command*> CommandRegistry::list() const {
  std::vector<const Command*> v;
  for (const auto& [k, c] : cmds_) v.push_back(&c);
  return v;
}

CommandResult CommandRegistry::run(CommandContext& ctx, std::string_view line) const {
  const auto words = split_command_line(line);
  if (words.empty() || words[0].starts_with("#")) return ok();
  const Command* c = find(words[0]);
  if (!c) return fail("unknown command '" + words[0] + "' (try 'help')");
  if (c->edits_design && ctx.session.busy())
    return fail("'" + c->name + "' changes the design; not available while " + ctx.session.job_kind() +
                " runs (cancel it or wait)");
  try {
    return c->run(ctx, words);
  } catch (const std::exception& e) {
    return fail(e.what());
  }
}

std::vector<std::string> CommandRegistry::complete(std::string_view prefix) const {
  std::vector<std::string> v;
  for (const auto& [k, c] : cmds_)
    if (k.starts_with(prefix)) v.push_back(k);
  return v;
}

std::string CommandRegistry::help() const {
  std::string s;
  for (const auto& [k, c] : cmds_) {
    std::string head = c.name + (c.args.empty() ? "" : " " + c.args);
    if (head.size() < 34) head.resize(34, ' ');
    s += head + " " + c.help + (c.edits_design ? " *" : "") + "\n";
  }
  s += "(* = changes the design: not available while a job runs)";
  return s;
}

// --- Scripts -------------------------------------------------------------------

void ScriptRunner::add(const std::vector<std::string>& lines) {
  for (const auto& l : lines) lines_.push_back(l);
}

void ScriptRunner::clear() {
  lines_.clear();
  wait_ = nullptr;
}

bool ScriptRunner::check_job(CommandContext& ctx) {
  if (!watch_job_) return true;
  if (ctx.session.busy()) return false;
  ctx.session.poll();
  watch_job_ = false;
  const JobEnd* e = ctx.session.last_end();
  if (e && e->serial > job_serial_ && e->state == JobState::Failed) {
    failed_ = true;
    error_ = e->kind + " failed: " + e->error;
    lines_.clear();
  }
  return true;
}

void ScriptRunner::step(CommandContext& ctx, const CommandRegistry& reg, const Echo& echo) {
  while (true) {
    if (wait_) {
      if (!wait_()) return;
      wait_ = nullptr;
    }
    if (lines_.empty()) {
      check_job(ctx);  // Report a failure of the last job.
      return;
    }
    const auto words = split_command_line(lines_.front());
    if (words.empty() || words[0].starts_with("#")) {
      lines_.pop_front();
      continue;
    }
    const Command* c = reg.find(words[0]);
    const bool immediate = c && c->immediate;
    if (!immediate) {
      if (ctx.session.busy()) return;  // Wait for the job before this step.
      check_job(ctx);
      if (failed_) return;
    }
    const std::string line = std::move(lines_.front());
    lines_.pop_front();
    const std::uint64_t serial = ctx.session.last_end() ? ctx.session.last_end()->serial : 0;
    const bool was_busy = ctx.session.busy();
    CommandResult r = reg.run(ctx, line);
    ++run_;
    if (echo) echo(line, r);
    if (!r.ok) {
      failed_ = true;
      error_ = r.text;
      lines_.clear();
      return;
    }
    for (auto it = r.insert_lines.rbegin(); it != r.insert_lines.rend(); ++it) lines_.push_front(*it);
    if (r.wait_until) wait_ = std::move(r.wait_until);
    if (!was_busy && ctx.session.busy()) {  // The command started a job: watch how it ends.
      watch_job_ = true;
      job_serial_ = serial;
    }
  }
}

std::string ScriptRunner::waiting_for(const CommandContext& ctx) const {
  if (wait_) return "wait";
  if (!lines_.empty() && ctx.session.busy()) return ctx.session.job_kind();
  return {};
}

std::vector<std::string> read_command_file(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot read command file " + path);
  std::vector<std::string> out;
  std::string cont;
  for (std::string line; std::getline(f, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && line.back() == '\\') {
      line.pop_back();
      cont += line + " ";
      continue;
    }
    out.push_back(cont + line);
    cont.clear();
  }
  if (!cont.empty()) out.push_back(cont);
  return out;
}

std::vector<std::string> split_commands(std::string_view text) {
  std::vector<std::string> out;
  std::string cur;
  bool quote = false;
  auto push = [&] {
    const auto b = cur.find_first_not_of(" \t");
    if (b != std::string::npos) out.push_back(cur.substr(b));
    cur.clear();
  };
  for (char ch : text) {
    if (ch == '"') quote = !quote;
    if ((ch == ';' || ch == '\n') && !quote) push();
    else cur += ch;
  }
  push();
  return out;
}

}  // namespace hermes::gui
