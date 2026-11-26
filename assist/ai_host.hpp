#pragma once

// Glue that puts the assistant into a console (hermes_gui, `hermes shell`):
// owns config, tools and the Assistant, and adds the `ai` command:
//
//   ai <question>       ask (answer arrives in the chat panel / console)
//   ai status           provider, model, edit policy, token use
//   ai privacy          exactly what is sent, and where
//   ai yes | ai no      answer a pending confirmation
//   ai stop | ai reset  stop the answer / forget the conversation
//   ai tools            the tools the model can use
//   ai on | ai off      enable / disable for this session
//   ai set <key> <value>  change a config key for this session
//
// The host is created disabled-safe: nothing is contacted until the
// assistant is enabled and the engineer asks something.

#include <memory>
#include <string>

#include "assist/assistant.hpp"
#include "assist/config.hpp"
#include "assist/tools.hpp"
#include "gui/scene/commands.hpp"

namespace hermes::assist {

class AiHost {
 public:
  explicit AiHost(AssistantConfig cfg = load_assistant_config(), std::shared_ptr<HttpClient> http = {});
  ~AiHost();

  /// Adds the `ai` command to `reg` (and remembers it for tool calls).
  void register_commands(gui::CommandRegistry& reg);
  /// Runs pending tool calls (owner thread, every frame / loop turn).
  void pump(gui::Session& s, gui::ViewApi& v);

  /// Creates the assistant if the config allows (false and *why if not).
  bool ensure(std::string* why);
  bool ask(gui::Session& s, const gui::ViewApi& v, const std::string& question, std::string* why);
  Assistant* assistant() { return assistant_.get(); }
  AssistantConfig& config() { return cfg_; }
  const ToolRegistry& tools() const { return tools_; }
  /// For tests: use this provider instead of building one from the config.
  void set_provider_override(std::function<std::unique_ptr<Provider>()> fn) { provider_override_ = std::move(fn); }
  std::string status() const;
  void set_on_update(std::function<void()> fn);

 private:
  AssistantConfig cfg_;
  std::shared_ptr<HttpClient> http_;
  ToolRegistry tools_;
  const gui::CommandRegistry* commands_ = nullptr;
  std::unique_ptr<Assistant> assistant_;
  std::function<std::unique_ptr<Provider>()> provider_override_;
  std::function<void()> on_update_;
};

}  // namespace hermes::assist
