#include "gui/scene/commands.hpp"

#include <cmath>
#include <cstdlib>
#include <format>

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

CommandResult fail(std::string t) { return {false, std::move(t), {}}; }
CommandResult ok(std::string t = {}) { return {true, std::move(t), {}}; }

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
  else {
    *why = "unknown flow option '" + k +
           "' (density iters overflow route dr dp legalize keep_placed timing routability grid qor)";
    return false;
  }
  return true;
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
  auto run_cmd = [](bool route) {
    return [route](CommandContext& c, const std::vector<std::string>& a) {
      FlowOptions f;
      f.run_routing = route;
      std::string why;
      for (std::size_t i = 1; i < a.size(); ++i)
        if (!apply_flow_opt(f, a[i], &why)) return fail(why);
      if (!c.session.start_flow(f, &why)) return fail(why);
      return ok(std::string(route ? "flow" : "placement") + " started in the background; the view updates live");
    };
  };
  add({"run", "[key=value ...]",
       "Run the full flow (place, route, STA, QoR) as a background job. Options: density iters overflow route dr dp "
       "legalize keep_placed timing routability grid qor",
       true, run_cmd(true)});
  add({"place", "[key=value ...]", "Run placement only as a background job (same options as run)", true,
       run_cmd(false)});
  add({"cancel", "", "Cancel the running job at its next safe point", false,
       [](CommandContext& c, const std::vector<std::string>&) {
         if (!c.session.busy()) return ok("no job running");
         c.session.cancel();
         return ok("cancel requested");
       }});
  add({"wait", "", "Wait for the running job to finish (scripts)", false,
       [](CommandContext& c, const std::vector<std::string>&) {
         CommandResult r = ok();
         Session* s = &c.session;
         r.wait_until = [s] { return !s->busy(); };
         return r;
       }});
  add({"settle", "", "Wait until the view shows the latest design with full detail (scripts)", false,
       [](CommandContext& c, const std::vector<std::string>&) {
         CommandResult r = ok();
         ViewApi* v = &c.view;
         r.wait_until = [v] { return v->view_settled(); };
         return r;
       }});
  add({"qor", "", "Compute the QoR report (density, congestion, wirelength maps) in the background", false,
       [](CommandContext& c, const std::vector<std::string>&) {
         std::string why;
         if (!c.session.start_qor(&why)) return fail(why);
         return ok("computing QoR ...");
       }});
  add({"report", "", "Print the last QoR report", false, [](CommandContext& c, const std::vector<std::string>&) {
         auto q = c.session.qor();
         if (!q) return fail("no QoR report yet (run 'qor' or a flow)");
         return ok(format_qor(*q));
       }});
  add({"save", "<file.hdb>", "Save the design (native .hdb) in the background", false,
       [](CommandContext& c, const std::vector<std::string>& a) {
         if (a.size() < 2) return fail("usage: save <file.hdb>");
         std::string why;
         if (!c.session.save(a[1], &why)) return fail(why);
         return ok("saving " + a[1] + " ...");
       },
       true});
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
         return ok("saved " + a[1]);
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

}  // namespace hermes::gui
