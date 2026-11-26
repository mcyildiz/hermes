#pragma once

// The assistant's tool layer: hermes actions as named tools with a JSON
// schema, a description written for a language model, and an access class.
// One registry serves the MCP server (Claude Code, Claude desktop, any MCP
// client) and the built-in chat (any model provider). Tools act through the
// same Session and console commands as the GUI and `hermes shell`, so an
// assistant can do nothing an engineer at the console could not.
//
// Access classes:
//   Read    reports, queries, logs, run history, view navigation. Never
//           changes the design or writes files.
//   Change  edits the design (load, place, route, move, undo), cancels a
//           job, or writes files (save, screenshot). Hidden in read-only
//           mode; in "ask" mode the engineer confirms every call.
//
// Threading: tools run on the thread that owns the Session (the GUI thread,
// or the MCP/shell main loop). A tool that has to wait (a job, a load)
// returns at once with `wait_until`/`then`; the caller re-checks it while
// keeping its loop (and the GUI) running.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "assist/json.hpp"
#include "gui/scene/commands.hpp"

namespace hermes::gui {
class Session;
}

namespace hermes::assist {

enum class Access { Read, Change };
const char* access_name(Access a);

/// What an assistant may do with Change tools.
///   ReadOnly  Change tools are not offered and are refused (default).
///   Ask       every Change call waits for the engineer's yes/no (chat).
///   Allow     Change calls run (MCP with --allow-edits: the MCP client,
///             e.g. Claude Code, asks the engineer before each call).
enum class EditPolicy { ReadOnly, Ask, Allow };
const char* policy_name(EditPolicy p);
bool parse_policy(std::string_view s, EditPolicy* out);

struct ToolResult {
  bool ok = true;
  std::string text;                      ///< What the model sees.
  /// Waiting tools: poll until true (or timeout_s), then call then().
  std::function<bool()> wait_until;
  std::function<ToolResult()> then;
  double timeout_s = 0.0;
};

struct ToolContext {
  gui::Session& session;
  gui::ViewApi& view;
  const gui::CommandRegistry& commands;
  /// Database the run-history tool reads ("" = track::default_db_path()).
  std::string runs_db;
};

struct Tool {
  std::string name;
  std::string title;
  std::string description;
  Json schema;                           ///< JSON Schema of the arguments (an object schema).
  Access access = Access::Read;
  std::function<ToolResult(ToolContext&, const Json& args)> run;
  /// Access for a particular call (run_command depends on the command). Null = `access`.
  std::function<Access(const ToolContext&, const Json& args)> access_for;
  /// Refuse this call outright (e.g. 'quit' through run_command). Null = allowed.
  std::function<std::string(const ToolContext&, const Json& args)> refuse;

  Access access_of(const ToolContext& c, const Json& a) const { return access_for ? access_for(c, a) : access; }
};

class ToolRegistry {
 public:
  /// With the built-in hermes tools.
  ToolRegistry();
  void add(Tool t);
  const Tool* find(std::string_view name) const;
  /// In registration order. `include_change` = false: read tools only.
  std::vector<const Tool*> list(bool include_change = true) const;

  /// Runs a tool (does not wait). Errors (unknown tool, bad arguments,
  /// exceptions) come back as ok = false with a reason for the model.
  ToolResult call(ToolContext& ctx, std::string_view name, const Json& args) const;

 private:
  std::vector<Tool> tools_;
  std::map<std::string, std::size_t, std::less<>> index_;
};

/// Why `policy` forbids this call ("" = allowed, possibly after confirmation).
std::string policy_refusal(const ToolRegistry& reg, const ToolContext& ctx, std::string_view name, const Json& args,
                           EditPolicy policy);

/// Calls a tool and waits for it in place, polling the session (MCP server,
/// shell, tests). Never use on the GUI thread.
ToolResult call_tool_blocking(const ToolRegistry& reg, ToolContext& ctx, std::string_view name, const Json& args);

/// Finishes a waiting result: true when done (and *r replaced by then()).
/// `started_s` is when the wait began (steady clock seconds).
bool tool_result_ready(ToolResult& r, double started_s);
double steady_seconds();

/// Cuts long tool output for the model (keeps head and tail).
std::string clip_text(std::string s, std::size_t max_chars = 16000);

/// A ViewApi without a window (MCP server, `hermes shell`, Python): keeps a
/// selection by name, refuses rendering requests.
class HeadlessView final : public gui::ViewApi {
 public:
  explicit HeadlessView(gui::Session& s) : s_(s) {}
  void zoom_fit() override {}
  void zoom_to(const Rect&) override {}
  bool set_layer(std::string_view, int, int, std::string* why) override;
  std::string layer_list() const override { return "(no view: headless)"; }
  bool select_cell(std::string_view name, bool zoom, std::string* why) override;
  bool select_net(std::string_view name, bool zoom, std::string* why) override;
  void clear_selection() override { sel_.clear(); }
  bool set_heatmap(std::string_view, std::string* why) override;
  std::string heatmap_list() const override { return "(no view: headless)"; }
  bool screenshot(const std::string&, std::string* why) override;
  void request_quit() override { quit_ = true; }
  bool view_settled() const override { return true; }
  std::string stats() const override;
  std::string selection_text() const override { return sel_; }
  bool quit_requested() const { return quit_; }

 private:
  gui::Session& s_;
  std::string sel_;
  bool quit_ = false;
};

/// Short design summary (counts, die, job, last flow, QoR headline): the
/// design_summary tool and the chat context use it.
Json design_summary(const gui::Session& s);

}  // namespace hermes::assist
