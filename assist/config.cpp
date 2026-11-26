#include "assist/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace hermes::assist {

namespace {

std::string trim(std::string s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool parse_bool(const std::string& v, bool* out) {
  if (v == "1" || v == "true" || v == "yes" || v == "on") *out = true;
  else if (v == "0" || v == "false" || v == "no" || v == "off") *out = false;
  else return false;
  return true;
}

const char* env(const char* k) {
  const char* v = std::getenv(k);
  return v && *v ? v : nullptr;
}

std::string usual_key_env(const std::string& kind) {
  if (kind == "anthropic" || kind == "claude") return "ANTHROPIC_API_KEY";
  if (kind == "mock") return "";
  return "OPENAI_API_KEY";
}

}  // namespace

std::string default_config_path() {
  if (const char* p = env("HERMES_AI_CONFIG")) return p;
  const char* home = env("HOME");
  return std::string(home ? home : ".") + "/.hermes/assistant.conf";
}

bool set_config_value(AssistantConfig& c, const std::string& key, const std::string& value, std::string* why) {
  auto bad = [&](const char* what) {
    if (why) *why = std::format("{}: {} (got '{}')", key, what, value);
    return false;
  };
  auto num = [&](double* out) {
    char* e = nullptr;
    *out = std::strtod(value.c_str(), &e);
    return e && *e == 0 && !value.empty();
  };
  double d = 0;
  if (key == "enabled") {
    if (!parse_bool(value, &c.enabled)) return bad("expected true or false");
  } else if (key == "provider") {
    if (value != "anthropic" && value != "claude" && value != "openai" && value != "openai-compatible" &&
        value != "ollama" && value != "llamacpp" && value != "mock")
      return bad("expected anthropic, openai (any OpenAI-compatible endpoint) or mock");
    c.provider.kind = value == "claude" ? "anthropic" : (value == "anthropic" || value == "mock") ? value : "openai";
    if (value == "ollama" && c.provider.base_url.empty()) c.provider.base_url = "http://localhost:11434/v1";
    if (value == "llamacpp" && c.provider.base_url.empty()) c.provider.base_url = "http://localhost:8080/v1";
  } else if (key == "model") {
    c.provider.model = value;
  } else if (key == "base_url") {
    c.provider.base_url = value;
  } else if (key == "api_key_env") {
    c.api_key_env = value;
  } else if (key == "api_key_file") {
    c.api_key_file = value;
  } else if (key == "edits") {
    if (!parse_policy(value, &c.edits)) return bad("expected read-only, ask or allow");
  } else if (key == "effort") {
    if (!value.empty() && value != "low" && value != "medium" && value != "high" && value != "xhigh" && value != "max")
      return bad("expected low, medium, high, xhigh, max or empty");
    c.provider.effort = value;
  } else if (key == "fallbacks") {
    if (value != "auto" && value != "default" && value != "off") return bad("expected auto, default or off");
    c.provider.fallbacks = value;
  } else if (key == "max_tokens_field") {
    c.provider.max_tokens_field = value;
  } else if (key == "timeout_s") {
    if (!num(&d) || d <= 0) return bad("expected seconds");
    c.provider.timeout_s = d;
  } else if (key == "send_design_summary") {
    if (!parse_bool(value, &c.send_design_summary)) return bad("expected true or false");
  } else if (key == "send_selection") {
    if (!parse_bool(value, &c.send_selection)) return bad("expected true or false");
  } else if (key == "send_qor") {
    if (!parse_bool(value, &c.send_qor)) return bad("expected true or false");
  } else if (key == "send_log_lines") {
    if (!num(&d) || d < 0) return bad("expected a line count");
    c.send_log_lines = static_cast<int>(d);
  } else if (key == "max_tool_calls") {
    if (!num(&d) || d < 1) return bad("expected a count");
    c.max_tool_calls = static_cast<int>(d);
  } else if (key == "max_tokens") {
    if (!num(&d) || d < 256) return bad("expected a token count >= 256");
    c.max_tokens = static_cast<int>(d);
  } else if (key == "runs_db") {
    c.runs_db = value;
  } else {
    if (why) *why = "unknown key '" + key + "'";
    return false;
  }
  return true;
}

AssistantConfig load_assistant_config(const std::string& path_in) {
  AssistantConfig c;
  const std::string path = path_in.empty() ? default_config_path() : path_in;
  std::ifstream in(path);
  if (in) {
    c.source = path;
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
      ++n;
      if (const auto h = line.find('#'); h != std::string::npos) line.resize(h);
      line = trim(line);
      if (line.empty()) continue;
      const auto eq = line.find('=');
      std::string why;
      if (eq == std::string::npos) {
        c.warnings.push_back(std::format("{}:{}: expected key = value", path, n));
      } else if (!set_config_value(c, trim(line.substr(0, eq)), trim(line.substr(eq + 1)), &why)) {
        c.warnings.push_back(std::format("{}:{}: {}", path, n, why));
      }
    }
  }
  static const std::pair<const char*, const char*> envs[] = {
      {"HERMES_AI_ENABLED", "enabled"},   {"HERMES_AI_PROVIDER", "provider"}, {"HERMES_AI_MODEL", "model"},
      {"HERMES_AI_BASE_URL", "base_url"}, {"HERMES_AI_EDITS", "edits"},       {"HERMES_AI_EFFORT", "effort"},
      {"HERMES_AI_API_KEY_ENV", "api_key_env"}};
  for (const auto& [e, k] : envs)
    if (const char* v = env(e)) {
      std::string why;
      if (!set_config_value(c, k, v, &why)) c.warnings.push_back(std::string(e) + ": " + why);
    }
  resolve_api_key(c);
  return c;
}

bool resolve_api_key(AssistantConfig& c) {
  c.provider.api_key.clear();
  if (const char* k = env("HERMES_AI_API_KEY")) {
    c.provider.api_key = k;
  } else if (!c.api_key_file.empty()) {
    std::string p = c.api_key_file;
    if (p.starts_with("~/"))
      if (const char* home = env("HOME")) p = std::string(home) + p.substr(1);
    std::ifstream f(p);
    std::string k;
    if (f && std::getline(f, k)) c.provider.api_key = trim(k);
  } else {
    const std::string var = c.api_key_env.empty() ? usual_key_env(c.provider.kind) : c.api_key_env;
    if (!var.empty())
      if (const char* k = env(var.c_str())) c.provider.api_key = k;
  }
  return !c.provider.api_key.empty();
}

bool endpoint_is_local(const AssistantConfig& c) {
  if (c.provider.kind == "mock") return true;
  const std::string& u = c.provider.base_url;
  for (const char* h : {"http://localhost", "http://127.", "http://[::1]", "https://localhost", "https://127."})
    if (u.starts_with(h)) return true;
  return false;
}

bool assistant_ready(const AssistantConfig& c, std::string* why) {
  auto no = [&](std::string m) {
    if (why) *why = std::move(m);
    return false;
  };
  if (!c.enabled)
    return no("the AI assistant is off. Turn it on with `enabled = true` in " + default_config_path() +
              " (run `hermes ai init` for a template) or HERMES_AI_ENABLED=1, and give an API key.");
  if (c.provider.api_key.empty() && !endpoint_is_local(c)) {
    const std::string var = !c.api_key_env.empty() ? c.api_key_env : usual_key_env(c.provider.kind);
    return no("no API key: set " + (var.empty() ? std::string("HERMES_AI_API_KEY") : var) +
              " (or api_key_file in the config), or point base_url at a local model server");
  }
  if (c.provider.kind != "mock" && !have_http_client())
    return no("this hermes build has no HTTP client (libcurl); only provider = mock works");
  return true;
}

std::string status_line(const AssistantConfig& c) {
  std::string sends;
  auto add = [&](bool on, const char* w) {
    if (on) sends += (sends.empty() ? "" : ", ") + std::string(w);
  };
  add(c.send_design_summary, "design summary");
  add(c.send_qor && c.send_design_summary, "QoR headline");
  add(c.send_selection, "selection");
  if (c.send_log_lines > 0) sends += std::format("{}{} log lines", sends.empty() ? "" : ", ", c.send_log_lines);
  return std::format("{} {}, {}{}, context: {}", c.provider.kind,
                     c.provider.model.empty() ? default_model(c.provider.kind) : c.provider.model,
                     policy_name(c.edits), c.enabled ? "" : " (off)", sends.empty() ? "none" : sends);
}

std::string sharing_text(const AssistantConfig& c, const std::string& endpoint) {
  std::ostringstream s;
  if (c.provider.kind == "mock") {
    s << "provider = mock: the offline demo assistant answers inside hermes; nothing is sent anywhere.\n"
      << "With a real provider, this is what each message would send:\n";
  } else if (endpoint_is_local(c)) {
    s << "Requests go to " << endpoint << " on this machine; nothing leaves it.\n";
  } else {
    s << "Requests go to " << endpoint << " (" << c.provider.kind << ", model "
      << (c.provider.model.empty() ? default_model(c.provider.kind) : c.provider.model) << ").\n";
  }
  if (c.provider.kind != "mock") s << "Nothing is sent until you send a message. Each message sends:\n";
  s << "  - your text and the conversation so far (questions, answers, tool results)\n"
    << "  - the list of hermes tools (names, descriptions; no design data)\n";
  if (c.send_design_summary)
    s << "  - design summary: name, input path, cell/net/pin counts, die size, layer names, job state, last flow"
      << (c.send_qor ? " and QoR headline numbers\n" : "\n");
  if (c.send_selection) s << "  - the name of the selected cell/net/pin\n";
  if (c.send_log_lines > 0) s << "  - the last " << c.send_log_lines << " log lines\n";
  s << "  - the output of each tool the model calls: reports and the names/coordinates of the cells and nets it "
       "asks about (never the whole design, never files)\n"
    << "Edits: " << policy_name(c.edits)
    << (c.edits == EditPolicy::ReadOnly ? " (the model cannot change the design or write files)\n"
        : c.edits == EditPolicy::Ask   ? " (you confirm every change)\n"
                                       : " (changes run without asking)\n")
    << "Change what is sent in " << default_config_path() << " (send_* keys).";
  return s.str();
}

std::string config_template() {
  return R"(# hermes AI assistant (built-in chat in hermes_gui and `hermes shell`).
# Off unless enabled. The API key is NOT stored here: it is read from the
# environment (ANTHROPIC_API_KEY, OPENAI_API_KEY or HERMES_AI_API_KEY, or the
# variable named by api_key_env) or from api_key_file.

enabled = false

# anthropic (Claude) | openai (any OpenAI-compatible endpoint) | ollama |
# llamacpp | mock (offline demo, sends nothing)
provider = anthropic
# model = claude-opus-5-5        # anthropic default
# model = gpt-5                  # openai default; for Ollama e.g. qwen3:32b
# base_url = http://localhost:11434/v1   # Ollama; llama.cpp: http://localhost:8080/v1
# api_key_env = MY_KEY_VARIABLE
# api_key_file = ~/.hermes/ai_key
# effort = medium                # Anthropic: low | medium | high | xhigh | max
# fallbacks = auto               # Anthropic server-side refusal fallbacks: auto | default | off
# max_tokens_field = max_tokens  # OpenAI-compatible: some servers want max_completion_tokens
# timeout_s = 180

# What the model may do: read-only (default) | ask (you confirm each change) | allow
edits = read-only

# Context attached to every message (see `ai privacy`).
send_design_summary = true   # name, counts, die size, job, last flow, QoR headline
send_qor = true
send_selection = true
send_log_lines = 0

max_tool_calls = 25          # per question
max_tokens = 8192            # per answer
)";
}

}  // namespace hermes::assist
