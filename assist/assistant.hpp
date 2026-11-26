#pragma once

// The built-in chat assistant: a model (any Provider) answering the
// engineer's questions by calling hermes tools.
//
// Non-blocking by construction: ask() returns at once; the model request
// runs on the assistant's own thread. Tool calls are handed back to the
// thread that owns the Session (the GUI thread), which runs them in pump()
// once per frame, so the Design is never touched from two threads and the
// GUI keeps drawing while the model thinks or a flow runs.
//
// Permissions: with EditPolicy::ReadOnly (default) the model gets only read
// tools and any change is refused. With Ask, every change waits in
// pending_confirmation() until the engineer answers confirm(yes/no).
//
//   Assistant a(cfg, make_provider(cfg.provider, {}, &why), tools);
//   a.ask("why is M3 congested?", build_context(cfg, session, view));
//   every frame:  a.pump(ctx);  if (auto c = a.pending_confirmation()) ...;

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "assist/config.hpp"
#include "assist/provider.hpp"
#include "assist/tools.hpp"

namespace hermes::assist {

class Assistant {
 public:
  struct Entry {
    enum class Kind { User, Assistant, ToolCall, ToolResult, Error, Info };
    Kind kind = Kind::Info;
    std::string text;
  };
  struct Confirmation {
    std::string tool;    ///< Tool name.
    std::string title;   ///< Human title ("Run flow").
    std::string args;    ///< Arguments as JSON text.
  };

  Assistant(AssistantConfig cfg, std::unique_ptr<Provider> provider, const ToolRegistry& tools);
  ~Assistant();
  Assistant(const Assistant&) = delete;
  Assistant& operator=(const Assistant&) = delete;

  /// Starts answering `question` in the background. `context` (from
  /// build_context) is attached to the message. False (and *why) while a
  /// previous question is still being answered.
  bool ask(std::string question, std::string context = {}, std::string* why = nullptr);
  /// Runs pending tool calls on the calling (Session-owning) thread. Cheap
  /// when there is nothing to do; call once per frame / loop iteration.
  void pump(ToolContext& ctx);
  bool busy() const { return busy_.load(); }
  /// Stops the current answer (an in-flight request is aborted; a running
  /// hermes job is not cancelled).
  void cancel();
  /// Forgets the conversation (not while busy).
  bool reset();

  std::optional<Confirmation> pending_confirmation() const;
  /// The engineer's answer to pending_confirmation().
  void confirm(bool yes);

  std::vector<Entry> transcript(std::size_t from = 0) const;
  std::size_t transcript_size() const;
  /// Changes whenever the transcript or state changes.
  std::uint64_t version() const { return version_.load(); }
  long long input_tokens() const { return in_tok_.load(); }
  long long output_tokens() const { return out_tok_.load(); }
  const AssistantConfig& config() const { return cfg_; }
  const Provider& provider() const { return *provider_; }
  /// Called from the assistant thread when something changed (e.g. wake the GUI).
  void set_on_update(std::function<void()> fn);

  /// The system prompt (stable for a given policy, so providers can cache it).
  static std::string system_prompt(EditPolicy policy);

 private:
  struct Job;
  void worker(std::string user_text);
  void add(Entry::Kind k, std::string text);
  void notify();
  ToolOutput run_call(const ToolCall& call);

  AssistantConfig cfg_;
  std::unique_ptr<Provider> provider_;
  const ToolRegistry& tools_;
  std::vector<ChatMessage> messages_;  // owned by the worker while busy

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::shared_ptr<Job> job_;           // tool call waiting for pump()
  std::vector<Entry> transcript_;
  std::function<void()> on_update_;
  std::thread thread_;
  std::atomic<bool> busy_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<std::uint64_t> version_{0};
  std::atomic<long long> in_tok_{0}, out_tok_{0};
};

/// Context for one message, limited to what `cfg` allows: design summary
/// (with or without QoR), selection, recent log lines. Call on the
/// Session-owning thread.
std::string build_context(const AssistantConfig& cfg, const gui::Session& session, const gui::ViewApi& view);

}  // namespace hermes::assist
