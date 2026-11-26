#pragma once

// Console commands. The same registry serves the GUI console, scripts
// (hermes_gui --script / -c) and, later, an automation API: every action
// is a named command with help text and a flag saying whether it changes
// the design (refused while a job runs, so the GUI stays consistent).

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "hermes/core/types.hpp"

namespace hermes::gui {

class Session;

/// What commands may ask of the viewer (implemented by the app; a headless
/// stub in tests).
class ViewApi {
 public:
  virtual ~ViewApi() = default;
  virtual void zoom_fit() = 0;
  virtual void zoom_to(const Rect& world) = 0;
  virtual bool set_layer(std::string_view name, int visible, int selectable, std::string* why) = 0;
  virtual std::string layer_list() const = 0;
  virtual bool select_cell(std::string_view name, bool zoom, std::string* why) = 0;
  virtual bool select_net(std::string_view name, bool zoom, std::string* why) = 0;
  virtual void clear_selection() = 0;
  virtual bool set_heatmap(std::string_view name, std::string* why) = 0;
  virtual std::string heatmap_list() const = 0;
  virtual bool screenshot(const std::string& path, std::string* why) = 0;
  virtual void request_quit() = 0;
  /// True when the view shows the latest snapshot with full detail.
  virtual bool view_settled() const = 0;
  virtual std::string stats() const = 0;
  /// One-line description of the current selection ("" = nothing selected).
  /// Used as context for the AI assistant.
  virtual std::string selection_text() const { return {}; }
};

struct CommandResult {
  bool ok = true;
  std::string text;
  /// Scripts wait until this returns true before the next command.
  std::function<bool()> wait_until;
};

struct CommandContext {
  Session& session;
  ViewApi& view;
};

struct Command {
  std::string name;
  std::string args;   ///< e.g. "<path>"
  std::string help;
  bool edits_design = false;  ///< Refused while a job runs.
  std::function<CommandResult(CommandContext&, const std::vector<std::string>&)> run;
  /// Writes files (save, screenshot). The AI assistant treats these like
  /// design edits: never in read-only mode, confirmed by the engineer.
  bool writes_files = false;
};

class CommandRegistry {
 public:
  /// With the built-in commands.
  CommandRegistry();
  void add(Command c);
  const Command* find(std::string_view name) const;
  std::vector<const Command*> list() const;
  /// Runs one line ("open ~/designs/aes", "layer M3 off"). Splits on
  /// whitespace; "quotes" group words.
  CommandResult run(CommandContext& ctx, std::string_view line) const;
  /// Names starting with `prefix` (tab completion).
  std::vector<std::string> complete(std::string_view prefix) const;
  std::string help() const;

 private:
  std::map<std::string, Command, std::less<>> cmds_;
};

std::vector<std::string> split_command_line(std::string_view line);

}  // namespace hermes::gui
