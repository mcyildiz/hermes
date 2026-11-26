#pragma once

// Model providers for the built-in chat. The assistant talks to models only
// through Provider, so any model works: adapters exist for the Anthropic
// Messages API (Claude), for OpenAI-compatible chat-completions endpoints
// (OpenAI, Azure/OpenRouter/vLLM gateways, and local models served by
// Ollama, llama.cpp `llama-server`, LM Studio), and a mock provider for tests
// and offline demos. Adding a provider = one class with chat().
//
// Every request is a plain HTTPS/HTTP POST through HttpClient (libcurl when
// the build has it); tests inject a fake HttpClient, so they need no
// network and no key.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "assist/json.hpp"

namespace hermes::assist {

struct ToolCall {
  std::string id;
  std::string name;
  Json args;
};

struct ToolOutput {
  std::string call_id;
  std::string name;
  std::string content;
  bool is_error = false;
};

struct ChatMessage {
  enum class Role { User, Assistant, ToolResults };
  Role role = Role::User;
  std::string text;                  ///< User / assistant text.
  std::vector<ToolCall> calls;       ///< Assistant tool calls.
  std::vector<ToolOutput> results;   ///< ToolResults.
  /// The provider's own form of an assistant turn (e.g. Anthropic content
  /// blocks incl. thinking), replayed unchanged on the next request.
  Json raw;
};

struct ToolSpec {
  std::string name;
  std::string description;
  Json schema;
};

struct ChatRequest {
  std::string system;
  std::vector<ChatMessage> messages;
  std::vector<ToolSpec> tools;
  int max_tokens = 8192;
};

struct ChatResponse {
  bool ok = false;
  std::string error;                 ///< When !ok.
  std::string text;
  std::vector<ToolCall> calls;
  Json raw;                          ///< See ChatMessage::raw.
  std::string stop_reason;
  long long input_tokens = 0, output_tokens = 0;
};

// --- HTTP ----------------------------------------------------------------------

struct HttpResponse {
  int status = 0;                    ///< 0 = transport error.
  std::string body;
  std::string error;
};

class HttpClient {
 public:
  virtual ~HttpClient() = default;
  /// Blocking POST. `cancel` (optional) aborts the transfer when set.
  virtual HttpResponse post(const std::string& url, const std::vector<std::string>& headers, const std::string& body,
                            double timeout_s, const std::atomic<bool>* cancel) = 0;
};

/// libcurl when available, else a client that reports why it cannot connect.
std::shared_ptr<HttpClient> default_http_client();
bool have_http_client();

// --- Providers -----------------------------------------------------------------

struct ProviderConfig {
  std::string kind = "anthropic";    ///< anthropic | openai | mock
  std::string model;                 ///< "" = the provider's default.
  std::string base_url;              ///< "" = the provider's public endpoint.
  std::string api_key;               ///< Never logged or shown.
  double timeout_s = 180.0;
  // Anthropic only.
  std::string effort;                ///< low|medium|high|xhigh|max ("" = model default).
  std::string fallbacks = "auto";    ///< auto | default | off (server-side refusal fallbacks).
  // OpenAI-compatible only.
  std::string max_tokens_field = "max_tokens";  ///< Some servers want "max_completion_tokens".
};

class Provider {
 public:
  virtual ~Provider() = default;
  virtual std::string name() const = 0;       ///< e.g. "anthropic"
  virtual std::string model() const = 0;
  virtual std::string endpoint() const = 0;   ///< Where requests go (shown to the engineer).
  virtual ChatResponse chat(const ChatRequest& req, const std::atomic<bool>* cancel) = 0;
};

std::string default_model(const std::string& kind);

/// Builds the provider for `cfg` (null and *why when the kind is unknown).
std::unique_ptr<Provider> make_provider(const ProviderConfig& cfg, std::shared_ptr<HttpClient> http,
                                        std::string* why);

/// Scripted provider: each chat() call is answered by `fn` (tests). Without
/// a function it is the offline demo: picks one read tool from keywords in
/// the question, then summarizes the tool output.
class MockProvider final : public Provider {
 public:
  using Fn = std::function<ChatResponse(const ChatRequest&)>;
  explicit MockProvider(Fn fn = {}) : fn_(std::move(fn)) {}
  std::string name() const override { return "mock"; }
  std::string model() const override { return "mock"; }
  std::string endpoint() const override { return "(local, nothing is sent)"; }
  ChatResponse chat(const ChatRequest& req, const std::atomic<bool>* cancel) override;
  int calls() const { return calls_.load(); }
  /// Last request seen (tests check what would be sent).
  ChatRequest last_request() const;

 private:
  Fn fn_;
  std::atomic<int> calls_{0};
  mutable std::mutex mu_;
  ChatRequest last_;
};

// Request/response translation, exposed for tests.
Json anthropic_request_body(const ProviderConfig& cfg, const ChatRequest& req);
ChatResponse anthropic_parse_response(const Json& body);
Json openai_request_body(const ProviderConfig& cfg, const ChatRequest& req);
ChatResponse openai_parse_response(const Json& body);

}  // namespace hermes::assist
