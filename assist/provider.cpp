#include "assist/provider.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <mutex>

namespace hermes::assist {

std::string default_model(const std::string& kind) {
  if (kind == "anthropic") return "claude-opus-5-5";
  if (kind == "openai") return "gpt-5";  // Any model name the endpoint serves (e.g. "qwen3" on Ollama).
  return "mock";
}

namespace {

std::string trim_slash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

std::string http_error(const HttpResponse& r) {
  if (r.status == 0) return r.error.empty() ? "no answer from the model endpoint" : r.error;
  // Both APIs answer {"error": {"message": ...}}.
  const Json j = Json::parse(r.body);
  std::string msg = j["error"]["message"].as_string();
  if (msg.empty()) msg = j["error"].as_string();
  if (msg.empty()) msg = r.body.substr(0, 300);
  return std::format("HTTP {}: {}", r.status, msg);
}

// ---------------------------------------------------------------------------
// Anthropic Messages API
// ---------------------------------------------------------------------------

bool anthropic_wants_fallbacks(const ProviderConfig& cfg, const std::string& model) {
  if (cfg.fallbacks == "off") return false;
  if (cfg.fallbacks == "default") return true;
  // auto: the models that support server-side refusal fallbacks.
  for (const char* m : {"claude-fable-5-2", "claude-fable-5-1", "claude-opus-5-5", "claude-sonnet-5-5"})
    if (model == m) return true;
  return model == "claude-opus-5";
}

class AnthropicProvider final : public Provider {
 public:
  AnthropicProvider(ProviderConfig cfg, std::shared_ptr<HttpClient> http) : cfg_(std::move(cfg)), http_(std::move(http)) {
    if (cfg_.model.empty()) cfg_.model = default_model("anthropic");
    if (cfg_.base_url.empty()) cfg_.base_url = "https://api.anthropic.com";
  }
  std::string name() const override { return "anthropic"; }
  std::string model() const override { return cfg_.model; }
  std::string endpoint() const override { return trim_slash(cfg_.base_url) + "/v1/messages"; }
  ChatResponse chat(const ChatRequest& req, const std::atomic<bool>* cancel) override {
    std::vector<std::string> h = {"content-type: application/json", "anthropic-version: 2026-10-21",
                                  "x-api-key: " + cfg_.api_key};
    if (anthropic_wants_fallbacks(cfg_, cfg_.model)) h.push_back("anthropic-beta: server-side-fallback-2026-07-01");
    const HttpResponse r = http_->post(endpoint(), h, anthropic_request_body(cfg_, req).dump(), cfg_.timeout_s, cancel);
    if (r.status < 200 || r.status >= 300) return {false, http_error(r)};
    std::string err;
    const Json body = Json::parse(r.body, &err);
    if (body.is_null()) return {false, "bad JSON from the model endpoint: " + err};
    return anthropic_parse_response(body);
  }

 private:
  ProviderConfig cfg_;
  std::shared_ptr<HttpClient> http_;
};

// ---------------------------------------------------------------------------
// OpenAI-compatible chat completions
// ---------------------------------------------------------------------------

class OpenAIProvider final : public Provider {
 public:
  OpenAIProvider(ProviderConfig cfg, std::shared_ptr<HttpClient> http) : cfg_(std::move(cfg)), http_(std::move(http)) {
    if (cfg_.model.empty()) cfg_.model = default_model("openai");
    if (cfg_.base_url.empty()) cfg_.base_url = "https://api.openai.com/v1";
  }
  std::string name() const override { return "openai"; }
  std::string model() const override { return cfg_.model; }
  std::string endpoint() const override { return trim_slash(cfg_.base_url) + "/chat/completions"; }
  ChatResponse chat(const ChatRequest& req, const std::atomic<bool>* cancel) override {
    std::vector<std::string> h = {"content-type: application/json"};
    if (!cfg_.api_key.empty()) h.push_back("authorization: Bearer " + cfg_.api_key);
    const HttpResponse r = http_->post(endpoint(), h, openai_request_body(cfg_, req).dump(), cfg_.timeout_s, cancel);
    if (r.status < 200 || r.status >= 300) return {false, http_error(r)};
    std::string err;
    const Json body = Json::parse(r.body, &err);
    if (body.is_null()) return {false, "bad JSON from the model endpoint: " + err};
    return openai_parse_response(body);
  }

 private:
  ProviderConfig cfg_;
  std::shared_ptr<HttpClient> http_;
};

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

Json anthropic_request_body(const ProviderConfig& cfg, const ChatRequest& req) {
  const std::string model = cfg.model.empty() ? default_model("anthropic") : cfg.model;
  Json b = Json::object({{"model", model}, {"max_tokens", req.max_tokens}});
  if (!req.system.empty()) b.set("system", req.system);
  // Automatic prompt caching: the tools + system prefix and the growing
  // conversation are reused between the turns of one chat.
  b.set("cache_control", Json::object({{"type", "ephemeral"}}));
  Json msgs = Json::array();
  for (const auto& m : req.messages) {
    switch (m.role) {
      case ChatMessage::Role::User:
        msgs.push_back(Json::object({{"role", "user"}, {"content", m.text}}));
        break;
      case ChatMessage::Role::Assistant: {
        Json content;
        if (m.raw.is_array()) {
          content = m.raw;  // replay unchanged (thinking blocks included)
        } else {
          content = Json::array();
          if (!m.text.empty()) content.push_back(Json::object({{"type", "text"}, {"text", m.text}}));
          for (const auto& c : m.calls)
            content.push_back(Json::object({{"type", "tool_use"}, {"id", c.id}, {"name", c.name},
                                            {"input", c.args.is_object() ? c.args : Json::object()}}));
        }
        msgs.push_back(Json::object({{"role", "assistant"}, {"content", std::move(content)}}));
        break;
      }
      case ChatMessage::Role::ToolResults: {
        Json content = Json::array();
        for (const auto& r : m.results) {
          Json t = Json::object({{"type", "tool_result"}, {"tool_use_id", r.call_id}, {"content", r.content}});
          if (r.is_error) t.set("is_error", true);
          content.push_back(std::move(t));
        }
        msgs.push_back(Json::object({{"role", "user"}, {"content", std::move(content)}}));
        break;
      }
    }
  }
  b.set("messages", std::move(msgs));
  if (!req.tools.empty()) {
    Json tools = Json::array();
    for (const auto& t : req.tools)
      tools.push_back(Json::object({{"name", t.name}, {"description", t.description}, {"input_schema", t.schema}}));
    b.set("tools", std::move(tools));
  }
  if (!cfg.effort.empty()) b.set("output_config", Json::object({{"effort", cfg.effort}}));
  if (anthropic_wants_fallbacks(cfg, model)) b.set("fallbacks", "default");
  return b;
}

ChatResponse anthropic_parse_response(const Json& body) {
  ChatResponse r;
  if (body["type"].as_string() == "error") {
    r.error = body["error"]["message"].as_string("model endpoint error");
    return r;
  }
  r.ok = true;
  r.raw = body["content"];
  r.stop_reason = body["stop_reason"].as_string();
  for (const auto& blk : body["content"].items()) {
    const std::string t = blk["type"].as_string();
    if (t == "text") {
      if (!r.text.empty()) r.text += "\n";
      r.text += blk["text"].as_string();
    } else if (t == "tool_use") {
      r.calls.push_back({blk["id"].as_string(), blk["name"].as_string(), blk["input"]});
    }
  }
  if (r.stop_reason == "refusal") {
    const std::string cat = body["stop_details"]["category"].as_string();
    r.text += (r.text.empty() ? "" : "\n") +
              std::string("(The model declined to answer this request") + (cat.empty() ? "" : " [" + cat + "]") + ".)";
    r.calls.clear();
  } else if (r.stop_reason == "max_tokens") {
    r.text += "\n(The answer was cut at the max_tokens limit.)";
  }
  r.input_tokens = body["usage"]["input_tokens"].as_int() + body["usage"]["cache_read_input_tokens"].as_int() +
                   body["usage"]["cache_creation_input_tokens"].as_int();
  r.output_tokens = body["usage"]["output_tokens"].as_int();
  return r;
}

Json openai_request_body(const ProviderConfig& cfg, const ChatRequest& req) {
  Json b = Json::object({{"model", cfg.model.empty() ? default_model("openai") : cfg.model}});
  b.set(cfg.max_tokens_field.empty() ? "max_tokens" : cfg.max_tokens_field, req.max_tokens);
  Json msgs = Json::array();
  if (!req.system.empty()) msgs.push_back(Json::object({{"role", "system"}, {"content", req.system}}));
  for (const auto& m : req.messages) {
    switch (m.role) {
      case ChatMessage::Role::User:
        msgs.push_back(Json::object({{"role", "user"}, {"content", m.text}}));
        break;
      case ChatMessage::Role::Assistant: {
        Json a = Json::object({{"role", "assistant"}, {"content", m.text.empty() ? Json() : Json(m.text)}});
        if (!m.calls.empty()) {
          Json tc = Json::array();
          for (const auto& c : m.calls)
            tc.push_back(Json::object({{"id", c.id},
                                       {"type", "function"},
                                       {"function", Json::object({{"name", c.name},
                                                                  {"arguments", (c.args.is_object() ? c.args : Json::object()).dump()}})}}));
          a.set("tool_calls", std::move(tc));
        }
        msgs.push_back(std::move(a));
        break;
      }
      case ChatMessage::Role::ToolResults:
        for (const auto& r : m.results)
          msgs.push_back(Json::object({{"role", "tool"},
                                       {"tool_call_id", r.call_id},
                                       {"content", r.is_error ? "ERROR: " + r.content : r.content}}));
        break;
    }
  }
  b.set("messages", std::move(msgs));
  if (!req.tools.empty()) {
    Json tools = Json::array();
    for (const auto& t : req.tools)
      tools.push_back(Json::object({{"type", "function"},
                                    {"function", Json::object({{"name", t.name},
                                                               {"description", t.description},
                                                               {"parameters", t.schema}})}}));
    b.set("tools", std::move(tools));
  }
  return b;
}

ChatResponse openai_parse_response(const Json& body) {
  ChatResponse r;
  if (body.contains("error")) {
    r.error = body["error"]["message"].as_string("model endpoint error");
    return r;
  }
  const Json& ch = body["choices"][0];
  if (!ch.is_object()) {
    r.error = "no choices in the model's answer";
    return r;
  }
  r.ok = true;
  const Json& msg = ch["message"];
  r.text = msg["content"].as_string();
  r.stop_reason = ch["finish_reason"].as_string();
  int n = 0;
  for (const auto& tc : msg["tool_calls"].items()) {
    ToolCall c;
    c.id = tc["id"].as_string(std::format("call_{}", n++));
    c.name = tc["function"]["name"].as_string();
    const Json& args = tc["function"]["arguments"];
    if (args.is_string()) {
      std::string err;
      c.args = Json::parse(args.str_ref().empty() ? "{}" : args.str_ref(), &err);
      if (!err.empty()) c.args = Json::object({{"_unparsed_arguments", args.str_ref()}});
    } else {
      c.args = args;  // some local servers send an object
    }
    r.calls.push_back(std::move(c));
  }
  r.raw = msg;
  r.input_tokens = body["usage"]["prompt_tokens"].as_int();
  r.output_tokens = body["usage"]["completion_tokens"].as_int();
  return r;
}

// ---------------------------------------------------------------------------
// Mock
// ---------------------------------------------------------------------------

ChatRequest MockProvider::last_request() const {
  std::lock_guard lk(mu_);
  return last_;
}

ChatResponse MockProvider::chat(const ChatRequest& req, const std::atomic<bool>*) {
  ++calls_;
  {
    std::lock_guard lk(mu_);
    last_ = req;
  }
  if (fn_) return fn_(req);
  // Offline demo: one tool picked by keyword, then a summary of its output.
  ChatResponse r;
  r.ok = true;
  if (req.messages.empty()) return r;
  const ChatMessage& last = req.messages.back();
  if (last.role == ChatMessage::Role::ToolResults) {
    r.text = "(mock assistant, no model) Tool output:\n";
    for (const auto& o : last.results)
      r.text += (o.is_error ? "[error] " : "") + o.content.substr(0, 700) + (o.content.size() > 700 ? " ..." : "") + "\n";
    r.stop_reason = "end_turn";
    return r;
  }
  std::string q = lower(last.text);
  if (const auto p = q.rfind("</hermes_context>"); p != std::string::npos) q = q.substr(p);
  struct Rule {
    const char* word;
    const char* tool;
  };
  static const Rule rules[] = {{"place", "run_flow"},           {"run the flow", "run_flow"},
                               {"congest", "congestion_report"}, {"qor", "qor_report"},     {"timing", "timing_report"},
                               {"slack", "timing_report"},       {"log", "read_log"},       {"history", "run_history"},
                               {"runs", "run_history"},          {"status", "job_status"},  {"command", "list_commands"},
                               {"select", "get_selection"},      {"summary", "design_summary"}};
  const char* tool = "design_summary";
  for (const auto& rule : rules)
    if (q.find(rule.word) != std::string::npos) {
      tool = rule.tool;
      break;
    }
  bool offered = false;
  for (const auto& t : req.tools) offered |= t.name == tool;
  if (!offered) {
    r.text = "(mock assistant) I would call " + std::string(tool) + ", but it is not offered.";
    return r;
  }
  Json args = Json::object();
  if (std::string_view(tool) == "run_flow") args = Json::object({{"route", q.find("place") == std::string::npos}});
  r.calls.push_back({std::format("mock_{}", calls_.load()), tool, std::move(args)});
  r.stop_reason = "tool_use";
  return r;
}

std::unique_ptr<Provider> make_provider(const ProviderConfig& cfg, std::shared_ptr<HttpClient> http,
                                        std::string* why) {
  if (!http) http = default_http_client();
  if (cfg.kind == "anthropic" || cfg.kind == "claude") return std::make_unique<AnthropicProvider>(cfg, http);
  if (cfg.kind == "openai" || cfg.kind == "openai-compatible" || cfg.kind == "ollama" || cfg.kind == "llamacpp")
    return std::make_unique<OpenAIProvider>(cfg, http);
  if (cfg.kind == "mock") return std::make_unique<MockProvider>();
  if (why) *why = "unknown provider '" + cfg.kind + "' (anthropic, openai, mock)";
  return nullptr;
}

}  // namespace hermes::assist
