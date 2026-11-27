#pragma once

// Console commands. The same registry serves the GUI console, scripts
// (hermes_gui --script / -c) and, later, an automation API: every action
// is a named command with help text and a flag saying whether it changes
// the design (refused while a job runs, so the GUI stays consistent).

#include <cstdint>
#include <deque>
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
  /// A screenshot was requested and is not written yet (scripts wait).
  virtual bool screenshot_pending() const { return false; }
};

struct CommandResult {
  bool ok = true;
  std::string text;
  /// Scripts wait until this returns true before the next command.
  std::function<bool()> wait_until;
  /// Lines to run next, before the rest of the script ('source').
  std::vector<std::string> insert_lines;
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
  /// Runs at once even while a job runs (view commands, cancel, wait).
  /// Every other command waits for the running job first: in scripts the
  /// runner waits, and the GUI console queues it.
  bool immediate = false;
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

/// Runs command lines in order, the same way in the GUI (startup commands,
/// --script, console input typed while a job runs), the batch mode and the
/// shell:
///  * a command that is not Command::immediate first waits until no job
///    runs, so "place" then "global_route" run one after the other;
///  * a result's wait_until is honoured ('wait', 'sleep', 'settle');
///  * a failing command, or a job that fails, stops the script (the rest
///    is dropped; failed() / error() say why). A cancelled job does not.
class ScriptRunner {
 public:
  using Echo = std::function<void(const std::string& line, const CommandResult& result)>;

  void add(const std::vector<std::string>& lines);
  void push(std::string line) { lines_.push_back(std::move(line)); }
  /// Nothing queued and nothing to wait for (a job may still run).
  bool idle() const { return lines_.empty() && !wait_; }
  std::size_t pending() const { return lines_.size(); }
  void clear();
  /// Runs lines until one has to wait or the queue is empty. `echo` sees
  /// each line run and its result. Never blocks.
  void step(CommandContext& ctx, const CommandRegistry& reg, const Echo& echo = {});
  /// What the runner waits for ("" = nothing).
  std::string waiting_for(const CommandContext& ctx) const;
  bool failed() const { return failed_; }
  const std::string& error() const { return error_; }
  /// Lines run so far (for error messages).
  std::size_t lines_run() const { return run_; }

 private:
  bool check_job(CommandContext& ctx);
  std::deque<std::string> lines_;
  std::function<bool()> wait_;
  bool watch_job_ = false;
  std::uint64_t job_serial_ = 0;
  bool failed_ = false;
  std::string error_;
  std::size_t run_ = 0;
};
/// Reads a command file: one command per line, '#' comments;
/// Reads a command file: one command per line, '#' comments, a trailing
/// a trailing backslash continues a line. Throws std::runtime_error if it cannot be read.
std::vector<std::string> read_command_file(const std::string& path);

/// Splits "cmd1; cmd2" (from -c) into lines.
std::vector<std::string> split_commands(std::string_view text);

}  // namespace hermes::gui
