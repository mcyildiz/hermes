#pragma once

// Assistant configuration: off unless the engineer turns it on.
//
// Read from $HERMES_AI_CONFIG, else ~/.hermes/assistant.conf (key = value
// lines, # comments; `hermes ai init` writes a commented template), then
// HERMES_AI_* environment variables override single keys. The API key is
// never stored in the config: it comes from an environment variable
// (ANTHROPIC_API_KEY / OPENAI_API_KEY / HERMES_AI_API_KEY, or the variable
// named by api_key_env) or from a file (api_key_file).
//
// Nothing is sent anywhere until the assistant is enabled, has a key (or a
// local endpoint), and the engineer sends a message. sharing_text() says
// exactly what goes out.

#include <string>
#include <vector>

#include "assist/provider.hpp"
#include "assist/tools.hpp"

namespace hermes::assist {

struct AssistantConfig {
  bool enabled = false;
  ProviderConfig provider;          ///< kind, model, base_url, effort, timeout...
  std::string api_key_env;          ///< "" = the provider's usual variable.
  std::string api_key_file;
  EditPolicy edits = EditPolicy::ReadOnly;
  // Context attached to each message (all visible with `ai privacy`).
  bool send_design_summary = true;  ///< Name, counts, die size, job state, last flow/QoR headline numbers.
  bool send_selection = true;       ///< The selected object's name.
  bool send_qor = true;             ///< QoR headline numbers (part of the summary).
  int send_log_lines = 0;           ///< Recent log lines (0 = none).
  int max_tool_calls = 25;          ///< Per question.
  int max_tokens = 8192;            ///< Per model answer.
  std::string runs_db;              ///< Run-history database ("" = default).

  std::string source = "defaults";  ///< Config file that was read.
  std::vector<std::string> warnings;
};

/// $HERMES_AI_CONFIG, else ~/.hermes/assistant.conf.
std::string default_config_path();
/// Defaults, then the file (missing file = defaults), then the environment.
AssistantConfig load_assistant_config(const std::string& path = "");
/// Sets one key ("provider", "model", "edits", ...). False and *why if
/// the key or value is not valid.
bool set_config_value(AssistantConfig& c, const std::string& key, const std::string& value, std::string* why);
/// A commented config file to start from.
std::string config_template();

/// Fills provider.api_key from the environment / key file. True if a key was found.
bool resolve_api_key(AssistantConfig& c);
/// True when the endpoint is on this machine (no key needed, nothing leaves it).
bool endpoint_is_local(const AssistantConfig& c);
/// Ready to send: enabled, provider known, key present or local endpoint.
bool assistant_ready(const AssistantConfig& c, std::string* why);

/// What leaves the machine, and where, for this config (shown before the
/// first message and by `ai privacy`).
std::string sharing_text(const AssistantConfig& c, const std::string& endpoint);
/// One-line status ("anthropic claude-opus-5-5, read-only, sends: summary, selection").
std::string status_line(const AssistantConfig& c);

}  // namespace hermes::assist
