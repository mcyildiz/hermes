// AI assistant integration tests. No network and no API key: providers run
// against a fake HTTP client or a scripted mock model.
//
// Covers: JSON, the tool layer (results, read-only refusals), the MCP server
// (handshake, listing, calls, read-only), Anthropic and OpenAI-compatible
// request/response translation, the assistant loop (tool round trips,
// confirmations, read-only, cancel, non-blocking), configuration and what
// gets sent, run history, and the `ai` console command.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "assist/ai_host.hpp"
#include "assist/assistant.hpp"
#include "assist/config.hpp"
#include "assist/json.hpp"
#include "assist/mcp_server.hpp"
#include "assist/provider.hpp"
#include "assist/run_history.hpp"
#include "assist/tools.hpp"
#include "gui/scene/session.hpp"
#include "hermes/db/synthetic.hpp"
#include "hermes/log/log.hpp"
#include "hermes/log/sinks.hpp"
#include "hermes/track/run_tracker.hpp"

using namespace hermes;
using namespace hermes::assist;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Design small(int n = 3000) {
  SyntheticOptions o;
  o.cells = n;
  return make_synthetic_design(o);
}

struct Rig {
  gui::Session session;
  HeadlessView view{session};
  gui::CommandRegistry commands;
  ToolRegistry tools;
  ToolContext ctx{session, view, commands, "-"};
  long long n = 0;  // live cells
  explicit Rig(int cells = 3000) {
    session.adopt(small(cells), "synthetic");
    auto s = session.snapshot();
    for (std::size_t i = 0; i < s->num_cells(); ++i) n += !s->cell(static_cast<Id>(i)).removed;
  }
  std::string cell0() const { return std::string(session.snapshot()->cell_name(0)); }
  std::string net0() const { return std::string(session.snapshot()->net_name(0)); }
};

// Pumps an assistant until it is idle (or a confirmation is pending).
bool drive(Assistant& a, ToolContext& ctx, double timeout_ms = 20000, bool stop_at_confirm = false) {
  const double t0 = now_ms();
  while (now_ms() - t0 < timeout_ms) {
    a.pump(ctx);
    ctx.session.poll();
    if (stop_at_confirm && a.pending_confirmation()) return true;
    if (!a.busy()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

ChatResponse text_reply(std::string t) {
  ChatResponse r;
  r.ok = true;
  r.text = std::move(t);
  r.stop_reason = "end_turn";
  return r;
}

ChatResponse call_reply(std::string tool, Json args, std::string id = "c1") {
  ChatResponse r;
  r.ok = true;
  r.calls.push_back({std::move(id), std::move(tool), std::move(args)});
  r.stop_reason = "tool_use";
  return r;
}

// Fake HTTP: records the request, returns a canned answer.
struct FakeHttp final : HttpClient {
  std::string url, body;
  std::vector<std::string> headers;
  HttpResponse answer;
  HttpResponse post(const std::string& u, const std::vector<std::string>& h, const std::string& b, double,
                    const std::atomic<bool>*) override {
    url = u;
    headers = h;
    body = b;
    return answer;
  }
  bool header(const std::string& h) const {
    for (const auto& x : headers)
      if (x == h) return true;
    return false;
  }
};

// ---------------------------------------------------------------------------

void test_json() {
  std::string err;
  Json j = Json::parse(R"({"a": [1, 2.5, -3e2, true, null], "s": "x\"y\\né😀", "o": {}})", &err);
  CHECK(err.empty());
  CHECK(j["a"].size() == 5);
  CHECK(j["a"][1].as_number() == 2.5);
  CHECK(j["a"][2].as_int() == -300);
  CHECK(j["a"][3].as_bool());
  CHECK(j["a"][4].is_null());
  CHECK(j["s"].as_string() == "x\"y\\n\xc3\xa9\xf0\x9f\x98\x80");
  CHECK(j["o"].is_object() && j["o"].size() == 0);
  CHECK(j["missing"]["deeper"].is_null());
  // Round trip keeps member order and values.
  Json k = Json::parse(j.dump(), &err);
  CHECK(err.empty() && k == j);
  CHECK(Json::parse(j.dump(2)) == j);
  CHECK(Json::object({{"b", 1}, {"a", 2}}).dump() == R"({"b":1,"a":2})");
  CHECK(Json(3.0).dump() == "3" && Json(0.25).dump() == "0.25");
  // Errors.
  CHECK(Json::parse("{\"a\":", &err).is_null() && !err.empty());
  CHECK(Json::parse("[1,2] x", &err).is_null() && !err.empty());
  CHECK(Json::parse("nul", &err).is_null() && !err.empty());
}

void test_tools_read() {
  Rig r;
  ToolResult s = call_tool_blocking(r.tools, r.ctx, "design_summary", Json::object());
  CHECK(s.ok);
  Json j = Json::parse(s.text);
  CHECK(j["cells"].as_int() == r.n);
  CHECK(j["die_um"].size() == 4);
  CHECK(j["job"]["busy"].as_bool(true) == false);

  ToolResult c = call_tool_blocking(r.tools, r.ctx, "query_cell", Json::object({{"name", r.cell0()}}));
  CHECK(c.ok);
  Json cj = Json::parse(c.text);
  CHECK(cj["name"].as_string() == r.cell0());
  CHECK(cj["pin_count"].as_int() > 0);
  CHECK(!call_tool_blocking(r.tools, r.ctx, "query_cell", Json::object({{"name", "no_such_cell"}})).ok);
  CHECK(!call_tool_blocking(r.tools, r.ctx, "query_cell", Json::object()).ok);  // missing required arg

  ToolResult n = call_tool_blocking(r.tools, r.ctx, "query_net", Json::object({{"name", r.net0()}}));
  CHECK(n.ok && Json::parse(n.text)["pin_count"].as_int() >= 1);

  ToolResult f = call_tool_blocking(r.tools, r.ctx, "find_objects",
                                    Json::object({{"pattern", r.cell0()}, {"kind", "cell"}, {"limit", 5}}));
  CHECK(f.ok);
  Json fj = Json::parse(f.text);
  CHECK(fj["cells"]["total"].as_int() >= 1 && !fj.contains("nets"));
  ToolResult g = call_tool_blocking(r.tools, r.ctx, "find_objects", Json::object({{"pattern", "*"}, {"limit", 3}}));
  CHECK(Json::parse(g.text)["cells"]["total"].as_int() == r.n);
  CHECK(Json::parse(g.text)["cells"]["names"].size() == 3);

  // Region query over the whole die sees every cell.
  Json die = Json::parse(s.text)["die_um"];
  ToolResult reg = call_tool_blocking(r.tools, r.ctx, "cells_in_region",
                                      Json::object({{"x0", die[0].as_number() - 1}, {"y0", die[1].as_number() - 1},
                                                    {"x1", die[2].as_number() + 1}, {"y1", die[3].as_number() + 1}}));
  CHECK(reg.ok && Json::parse(reg.text)["cells"].as_int() == r.n);

  // No QoR yet; compute it (waits for the background job), then the reports work.
  CHECK(!call_tool_blocking(r.tools, r.ctx, "qor_report", Json::object()).ok);
  ToolResult q = call_tool_blocking(r.tools, r.ctx, "compute_qor", Json::object());
  CHECK(q.ok);
  CHECK(Json::parse(q.text).contains("hpwl_um"));
  CHECK(call_tool_blocking(r.tools, r.ctx, "qor_report", Json::object()).ok);
  ToolResult cg = call_tool_blocking(r.tools, r.ctx, "congestion_report", Json::object({{"hot_spots", 3}}));
  CHECK(cg.ok && Json::parse(cg.text)["layers"].size() > 0);
  CHECK(Json::parse(cg.text)["hot_spots"].size() <= 3);
  ToolResult t = call_tool_blocking(r.tools, r.ctx, "timing_report", Json::object({{"nets", 4}}));
  CHECK(t.ok && Json::parse(t.text)["longest_nets"].size() == 4);
  CHECK(call_tool_blocking(r.tools, r.ctx, "read_log", Json::object({{"lines", 5}})).ok);
  CHECK(has(call_tool_blocking(r.tools, r.ctx, "list_commands", Json::object()).text, "heatmap"));
  // View: headless selection is what get_selection reports.
  CHECK(call_tool_blocking(r.tools, r.ctx, "show_in_view", Json::object({{"name", r.cell0()}})).ok);
  CHECK(has(call_tool_blocking(r.tools, r.ctx, "get_selection", Json::object()).text, r.cell0()));
  CHECK(!call_tool_blocking(r.tools, r.ctx, "no_such_tool", Json::object()).ok);
}

void test_tools_change_and_policy() {
  Rig r;
  auto snap = r.session.snapshot();
  const std::string name = r.cell0();
  // Read-only refuses every change tool, and run_command lines that change the design.
  CHECK(!policy_refusal(r.tools, r.ctx, "move_cell", Json::object(), EditPolicy::ReadOnly).empty());
  CHECK(!policy_refusal(r.tools, r.ctx, "run_flow", Json::object(), EditPolicy::ReadOnly).empty());
  CHECK(!policy_refusal(r.tools, r.ctx, "save_design", Json::object(), EditPolicy::ReadOnly).empty());
  CHECK(!policy_refusal(r.tools, r.ctx, "run_command", Json::object({{"line", "run iters=5"}}), EditPolicy::ReadOnly).empty());
  CHECK(!policy_refusal(r.tools, r.ctx, "run_command", Json::object({{"line", "save x.hdb"}}), EditPolicy::ReadOnly).empty());
  CHECK(policy_refusal(r.tools, r.ctx, "run_command", Json::object({{"line", "help"}}), EditPolicy::ReadOnly).empty());
  CHECK(policy_refusal(r.tools, r.ctx, "design_summary", Json::object(), EditPolicy::ReadOnly).empty());
  CHECK(policy_refusal(r.tools, r.ctx, "move_cell", Json::object(), EditPolicy::Ask).empty());
  // The read-only list has no change tools.
  for (const Tool* t : r.tools.list(false)) CHECK(t->access == Access::Read || t->access_for);
  // quit/ai are never available through run_command.
  CHECK(!r.tools.call(r.ctx, "run_command", Json::object({{"line", "quit"}})).ok);
  CHECK(!r.tools.call(r.ctx, "run_command", Json::object({{"line", "ai hello"}})).ok);

  // Change tools do what they say.
  ToolResult m = call_tool_blocking(r.tools, r.ctx, "move_cell", Json::object({{"name", name}, {"x_um", 1.0}, {"y_um", 2.0}}));
  CHECK(m.ok);
  CHECK(r.session.undo_depth() == 1);
  CHECK(call_tool_blocking(r.tools, r.ctx, "undo", Json::object()).ok);
  CHECK(r.session.undo_depth() == 0);
  ToolResult f = call_tool_blocking(r.tools, r.ctx, "run_flow",
                                    Json::object({{"route", false}, {"options", Json::object({{"iters", 5}})}, {"wait", true}}));
  CHECK(f.ok);
  CHECK(r.session.last_flow() != nullptr);
  CHECK(Json::parse(f.text).contains("last_flow"));
  CHECK(!call_tool_blocking(r.tools, r.ctx, "run_flow", Json::object({{"options", Json::object({{"bogus", 1}})}})).ok);
  const fs::path out = fs::temp_directory_path() / "hermes_assist_test.hdb";
  fs::remove(out);
  CHECK(call_tool_blocking(r.tools, r.ctx, "save_design", Json::object({{"path", out.string()}})).ok);
  CHECK(fs::exists(out));
  fs::remove(out);
}

void test_mcp() {
  Rig r;
  McpServer ro(r.tools, r.ctx);
  auto rpc = [](McpServer& s, const std::string& line) {
    auto out = s.handle_line(line);
    return out ? Json::parse(*out) : Json();
  };
  Json init = rpc(ro, R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})");
  CHECK(init["result"]["protocolVersion"].as_string() == "2025-06-18");
  CHECK(init["result"]["capabilities"]["tools"].is_object());
  CHECK(init["result"]["serverInfo"]["name"].as_string() == "hermes");
  CHECK(has(init["result"]["instructions"].as_string(), "READ-ONLY"));
  // Unknown versions get ours.
  CHECK(rpc(ro, R"({"jsonrpc":"2.0","id":9,"method":"initialize","params":{"protocolVersion":"1999-01-01"}})")["result"]["protocolVersion"].as_string() == McpServer::kProtocolVersion);
  // Notifications get no reply.
  CHECK(!ro.handle_line(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
  CHECK(rpc(ro, R"({"jsonrpc":"2.0","id":2,"method":"ping"})")["result"].is_object());

  Json list = rpc(ro, R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})");
  bool has_summary = false, has_flow = false;
  for (const auto& t : list["result"]["tools"].items()) {
    has_summary |= t["name"].as_string() == "design_summary";
    has_flow |= t["name"].as_string() == "run_flow";
    CHECK(t["inputSchema"]["type"].as_string() == "object");
    CHECK(t["annotations"].contains("readOnlyHint"));
  }
  CHECK(has_summary && !has_flow);

  Json call = rpc(ro, R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"design_summary","arguments":{}}})");
  CHECK(call["result"]["isError"].as_bool(true) == false);
  CHECK(Json::parse(call["result"]["content"][0]["text"].as_string())["cells"].as_int() == r.n);
  // Read-only: a change through run_command is refused with a hint.
  Json refused = rpc(ro, R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"run_command","arguments":{"line":"run"}}})");
  CHECK(refused["result"]["isError"].as_bool());
  CHECK(has(refused["result"]["content"][0]["text"].as_string(), "--allow-edits"));
  CHECK(!r.session.busy());
  CHECK(rpc(ro, R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"move_cell","arguments":{}}})")["result"]["isError"].as_bool());
  // Errors.
  CHECK(rpc(ro, R"({"jsonrpc":"2.0","id":7,"method":"nope"})")["error"]["code"].as_int() == -32601);
  CHECK(rpc(ro, R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"nope"}})")["error"]["code"].as_int() == -32602);
  CHECK(rpc(ro, "{not json")["error"]["code"].as_int() == -32700);

  // With edits: change tools are listed (destructive), and run.
  McpOptions o;
  o.edits = EditPolicy::Allow;
  McpServer rw(r.tools, r.ctx, o);
  Json l2 = rpc(rw, R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})");
  bool flow_destructive = false;
  for (const auto& t : l2["result"]["tools"].items())
    if (t["name"].as_string() == "run_flow")
      flow_destructive = t["annotations"]["destructiveHint"].as_bool() && !t["annotations"]["readOnlyHint"].as_bool(true);
  CHECK(flow_destructive);
  const std::string mv = std::string(R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"move_cell","arguments":{"name":")") +
                         r.cell0() + R"(","x_um":0,"y_um":0}}})";
  CHECK(rpc(rw, mv)["result"]["isError"].as_bool(true) == false);
  CHECK(r.session.undo_depth() == 1);

  // serve(): line in, line out.
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"ping"})" "\n"
                        R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n"
                        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" "\n");
  std::ostringstream out;
  ro.serve(in, out);
  int lines = 0;
  std::istringstream rd(out.str());
  for (std::string l; std::getline(rd, l);) {
    ++lines;
    CHECK(!Json::parse(l).is_null());
  }
  CHECK(lines == 2);
}

void test_anthropic_adapter() {
  ProviderConfig cfg;
  cfg.kind = "anthropic";
  cfg.api_key = "sk-test";
  ChatRequest req;
  req.system = "sys";
  req.tools.push_back({"design_summary", "d", Json::object({{"type", "object"}})});
  req.messages.push_back({ChatMessage::Role::User, "hi", {}, {}, {}});
  Json raw = Json::array({Json::object({{"type", "thinking"}, {"thinking", ""}, {"signature", "sig"}}),
                          Json::object({{"type", "tool_use"}, {"id", "t1"}, {"name", "design_summary"}, {"input", Json::object()}})});
  req.messages.push_back({ChatMessage::Role::Assistant, "", {{"t1", "design_summary", Json::object()}}, {}, raw});
  ChatMessage res{ChatMessage::Role::ToolResults, {}, {}, {{"t1", "design_summary", "{\"cells\":5}", false}}, {}};
  req.messages.push_back(res);

  Json b = anthropic_request_body(cfg, req);
  CHECK(b["model"].as_string() == "claude-opus-5-5");
  CHECK(b["system"].as_string() == "sys");
  CHECK(b["tools"][0]["input_schema"]["type"].as_string() == "object");
  CHECK(b["messages"].size() == 3);
  CHECK(b["messages"][1]["content"] == raw);  // replayed unchanged (thinking kept)
  CHECK(b["messages"][2]["role"].as_string() == "user");
  CHECK(b["messages"][2]["content"][0]["type"].as_string() == "tool_result");
  CHECK(b["messages"][2]["content"][0]["tool_use_id"].as_string() == "t1");
  CHECK(b["fallbacks"].as_string() == "default");
  CHECK(!b.contains("output_config"));
  CHECK(!has(b.dump(), "sk-test"));  // the key never goes into the body
  cfg.effort = "high";
  cfg.model = "claude-haiku-5-5";
  Json b2 = anthropic_request_body(cfg, req);
  CHECK(b2["output_config"]["effort"].as_string() == "high");
  CHECK(!b2.contains("fallbacks"));

  // Through the provider with a fake HTTP client.
  auto http = std::make_shared<FakeHttp>();
  http->answer = {200, R"({"type":"message","content":[{"type":"text","text":"Looking."},{"type":"tool_use","id":"tu_1","name":"qor_report","input":{"x":1}}],"stop_reason":"tool_use","usage":{"input_tokens":10,"output_tokens":5,"cache_read_input_tokens":3}})", ""};
  cfg = {};
  cfg.api_key = "sk-test";
  std::string why;
  auto p = make_provider(cfg, http, &why);
  ChatResponse r = p->chat(req, nullptr);
  CHECK(r.ok);
  CHECK(http->url == "https://api.anthropic.com/v1/messages");
  CHECK(http->header("x-api-key: sk-test"));
  CHECK(http->header("anthropic-version: 2026-10-21"));
  CHECK(http->header("anthropic-beta: server-side-fallback-2026-07-01"));
  CHECK(r.text == "Looking.");
  CHECK(r.calls.size() == 1 && r.calls[0].id == "tu_1" && r.calls[0].args["x"].as_int() == 1);
  CHECK(r.input_tokens == 13 && r.output_tokens == 5);
  CHECK(r.raw.is_array() && r.raw.size() == 2);
  // Errors and refusals.
  http->answer = {401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})", ""};
  ChatResponse e = p->chat(req, nullptr);
  CHECK(!e.ok && has(e.error, "401") && has(e.error, "invalid x-api-key"));
  ChatResponse rf = anthropic_parse_response(Json::parse(
      R"({"content":[],"stop_reason":"refusal","stop_details":{"type":"refusal","category":"cyber"}})"));
  CHECK(rf.ok && rf.calls.empty() && has(rf.text, "declined") && has(rf.text, "cyber"));
  http->answer = {0, "", "could not connect"};
  CHECK(!p->chat(req, nullptr).ok);
}

void test_openai_adapter() {
  ProviderConfig cfg;
  cfg.kind = "openai";
  cfg.base_url = "http://localhost:11434/v1/";
  cfg.model = "qwen3";
  ChatRequest req;
  req.system = "sys";
  req.tools.push_back({"query_cell", "q", Json::object({{"type", "object"}})});
  req.messages.push_back({ChatMessage::Role::User, "hi", {}, {}, {}});
  req.messages.push_back({ChatMessage::Role::Assistant, "", {{"call_1", "query_cell", Json::object({{"name", "u1"}})}}, {}, {}});
  req.messages.push_back({ChatMessage::Role::ToolResults, {}, {}, {{"call_1", "query_cell", "no such cell", true}}, {}});
  Json b = openai_request_body(cfg, req);
  CHECK(b["messages"][0]["role"].as_string() == "system");
  CHECK(b["messages"][2]["tool_calls"][0]["function"]["name"].as_string() == "query_cell");
  CHECK(Json::parse(b["messages"][2]["tool_calls"][0]["function"]["arguments"].as_string())["name"].as_string() == "u1");
  CHECK(b["messages"][2]["content"].is_null());
  CHECK(b["messages"][3]["role"].as_string() == "tool" && b["messages"][3]["tool_call_id"].as_string() == "call_1");
  CHECK(has(b["messages"][3]["content"].as_string(), "ERROR"));
  CHECK(b["tools"][0]["type"].as_string() == "function");
  CHECK(b["max_tokens"].is_number());

  auto http = std::make_shared<FakeHttp>();
  http->answer = {200, R"({"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[{"id":"c9","type":"function","function":{"name":"design_summary","arguments":"{}"}},{"id":"c10","type":"function","function":{"name":"x","arguments":"{bad"}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":7,"completion_tokens":2}})", ""};
  std::string why;
  auto p = make_provider(cfg, http, &why);
  ChatResponse r = p->chat(req, nullptr);
  CHECK(r.ok);
  CHECK(http->url == "http://localhost:11434/v1/chat/completions");
  for (const auto& h : http->headers) CHECK(!has(h, "authorization"));  // local server, no key
  CHECK(r.calls.size() == 2 && r.calls[0].name == "design_summary");
  CHECK(r.calls[1].args.contains("_unparsed_arguments"));
  CHECK(r.input_tokens == 7);
  cfg.api_key = "k";
  make_provider(cfg, http, &why)->chat(req, nullptr);
  CHECK(http->header("authorization: Bearer k"));
  CHECK(!make_provider(ProviderConfig{"nope"}, http, &why) && !why.empty());
}

AssistantConfig test_config(EditPolicy p) {
  AssistantConfig c;
  c.enabled = true;
  c.provider.kind = "mock";
  c.edits = p;
  return c;
}

void test_assistant_tool_round_trip() {
  Rig r;
  auto mock = std::make_unique<MockProvider>([](const ChatRequest& req) {
    const ChatMessage& last = req.messages.back();
    if (last.role == ChatMessage::Role::User) return call_reply("design_summary", Json::object());
    CHECK(last.role == ChatMessage::Role::ToolResults);
    const Json s = Json::parse(last.results.at(0).content);
    return text_reply("The design has " + std::to_string(s["cells"].as_int()) + " cells.");
  });
  MockProvider* m = mock.get();
  Assistant a(test_config(EditPolicy::ReadOnly), std::move(mock), r.tools);
  CHECK(a.ask("how big is it?", build_context(a.config(), r.session, r.view)));
  CHECK(drive(a, r.ctx));
  CHECK(m->calls() == 2);
  auto t = a.transcript();
  CHECK(!t.empty() && t.back().kind == Assistant::Entry::Kind::Assistant);
  CHECK(!t.empty() && t.back().text == "The design has " + std::to_string(r.n) + " cells.");
  const ChatRequest last = m->last_request();
  CHECK(has(last.system, "READ-ONLY"));
  CHECK(has(last.messages[0].text, "<hermes_context>"));
  CHECK(has(last.messages[0].text, "how big is it?"));
  // Read-only: no change tool is offered.
  for (const auto& tool : last.tools) CHECK(tool.name != "run_flow" && tool.name != "move_cell" && tool.name != "save_design");
  // A second question continues the conversation.
  CHECK(a.ask("again"));
  CHECK(drive(a, r.ctx));
  CHECK(m->last_request().messages.size() == 7);  // u, a(call), results, a, u, a(call), results
  CHECK(a.reset() && a.transcript_size() == 0);
}

void test_assistant_read_only_refuses() {
  Rig r;
  int step = 0;
  auto mock = std::make_unique<MockProvider>([&](const ChatRequest& req) {
    if (step++ == 0) return call_reply("move_cell", Json::object({{"name", "x"}, {"x_um", 0}, {"y_um", 0}}));
    CHECK(req.messages.back().results.at(0).is_error);
    CHECK(has(req.messages.back().results.at(0).content, "read-only"));
    return text_reply("I cannot.");
  });
  Assistant a(test_config(EditPolicy::ReadOnly), std::move(mock), r.tools);
  a.ask("move it");
  CHECK(drive(a, r.ctx));
  CHECK(r.session.undo_depth() == 0);
  CHECK(!a.pending_confirmation());
}

void test_assistant_confirmations() {
  Rig r;
  const std::string cell = r.cell0();
  for (bool yes : {false, true}) {
    bool saw_error = false;
    auto mock = std::make_unique<MockProvider>([&](const ChatRequest& req) {
      if (req.messages.back().role == ChatMessage::Role::User)
        return call_reply("move_cell", Json::object({{"name", cell}, {"x_um", 3.0}, {"y_um", 4.0}}));
      saw_error = req.messages.back().results.at(0).is_error;
      return text_reply("done");
    });
    Assistant a(test_config(EditPolicy::Ask), std::move(mock), r.tools);
    a.ask("move the cell");
    CHECK(drive(a, r.ctx, 20000, true));
    auto c = a.pending_confirmation();
    CHECK(c.has_value());
    if (!c) continue;
    CHECK(c->tool == "move_cell" && has(c->args, cell));
    // Nothing happens while the engineer has not answered.
    for (int i = 0; i < 20; ++i) a.pump(r.ctx);
    CHECK(r.session.undo_depth() == (yes ? 0u : 0u));
    a.confirm(yes);
    CHECK(drive(a, r.ctx));
    CHECK(saw_error == !yes);
    CHECK(r.session.undo_depth() == (yes ? 1u : 0u));
  }
  // Read tools never ask.
  auto mock = std::make_unique<MockProvider>([&](const ChatRequest& req) {
    if (req.messages.back().role == ChatMessage::Role::User) return call_reply("design_summary", Json::object());
    return text_reply("ok");
  });
  Assistant a(test_config(EditPolicy::Ask), std::move(mock), r.tools);
  a.ask("summary");
  CHECK(drive(a, r.ctx, 20000, true));
  CHECK(!a.pending_confirmation() && !a.busy());
}

void test_assistant_nonblocking_and_cancel() {
  Rig r;
  // A slow model: ask() returns at once and the owner loop keeps turning.
  auto slow = std::make_unique<MockProvider>([](const ChatRequest&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return text_reply("slow answer");
  });
  Assistant a(test_config(EditPolicy::ReadOnly), std::move(slow), r.tools);
  const double t0 = now_ms();
  CHECK(a.ask("q"));
  CHECK(now_ms() - t0 < 50);
  CHECK(!a.ask("second"));  // one question at a time
  int frames = 0;
  double worst = 0;
  while (a.busy()) {
    const double f0 = now_ms();
    a.pump(r.ctx);
    worst = std::max(worst, now_ms() - f0);
    ++frames;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(frames > 20);
  CHECK(worst < 20);
  std::printf("  slow model: %d frames while waiting, worst pump %.3f ms\n", frames, worst);

  // Cancel: a model that blocks until cancelled; the conversation stays valid.
  std::atomic<bool> entered{false};
  auto stuck = std::make_unique<MockProvider>();
  struct Blocking final : Provider {
    std::atomic<bool>* entered;
    std::string name() const override { return "block"; }
    std::string model() const override { return "block"; }
    std::string endpoint() const override { return "-"; }
    ChatResponse chat(const ChatRequest&, const std::atomic<bool>* cancel) override {
      *entered = true;
      while (!cancel->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return {false, "cancelled"};
    }
  };
  auto b = std::make_unique<Blocking>();
  b->entered = &entered;
  Assistant c(test_config(EditPolicy::ReadOnly), std::move(b), r.tools);
  c.ask("q");
  while (!entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const double c0 = now_ms();
  c.cancel();
  while (c.busy() && now_ms() - c0 < 2000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(!c.busy());
  CHECK(c.transcript().back().text == "stopped");

  // Cancel while a tool waits on a job: the job keeps running, the answer stops.
  auto waiting = std::make_unique<MockProvider>([](const ChatRequest& req) {
    if (req.messages.back().role == ChatMessage::Role::User)
      return call_reply("wait_for_job", Json::object({{"timeout_s", 60}}));
    return text_reply("after wait");
  });
  Assistant w(test_config(EditPolicy::ReadOnly), std::move(waiting), r.tools);
  FlowOptions fo;
  fo.placer.max_iterations = 100000;
  fo.placer.stop_overflow = 0.0;  // Keep the job running: fast machines converge in < 200 ms.
  fo.run_routing = false;
  CHECK(r.session.start_flow(fo));
  w.ask("wait for it");
  const double w0 = now_ms();
  while (now_ms() - w0 < 200) {
    w.pump(r.ctx);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(w.busy());
  w.cancel();
  CHECK(drive(w, r.ctx, 2000));
  CHECK(r.session.busy());  // the engineer's job is not cancelled by stopping the assistant
  r.session.cancel();
  r.session.wait();
}

void test_provider_error_keeps_history_valid() {
  Rig r;
  int n = 0;
  auto mock = std::make_unique<MockProvider>([&](const ChatRequest& req) {
    if (n++ == 0) return ChatResponse{false, "HTTP 529: overloaded"};
    // The failed question was dropped: the history alternates correctly.
    CHECK(req.messages.size() == 1 && req.messages[0].role == ChatMessage::Role::User);
    return text_reply("fine now");
  });
  Assistant a(test_config(EditPolicy::ReadOnly), std::move(mock), r.tools);
  a.ask("first");
  CHECK(drive(a, r.ctx));
  CHECK(a.transcript().back().kind == Assistant::Entry::Kind::Error);
  a.ask("second");
  CHECK(drive(a, r.ctx));
  CHECK(a.transcript().back().text == "fine now");
}

void test_tool_call_limit() {
  Rig r;
  auto mock = std::make_unique<MockProvider>([](const ChatRequest&) { return call_reply("job_status", Json::object()); });
  AssistantConfig cfg = test_config(EditPolicy::ReadOnly);
  cfg.max_tool_calls = 3;
  Assistant a(cfg, std::move(mock), r.tools);
  a.ask("loop forever");
  CHECK(drive(a, r.ctx));
  int calls = 0;
  for (const auto& e : a.transcript()) calls += e.kind == Assistant::Entry::Kind::ToolCall;
  CHECK(calls == 3);
  CHECK(has(a.transcript().back().text, "max_tool_calls"));
}

void test_config_and_privacy() {
  const fs::path p = fs::temp_directory_path() / "hermes_assistant_test.conf";
  {
    std::ofstream f(p);
    f << "# test\nenabled = true\nprovider = ollama\nmodel = qwen3\nedits = ask\nsend_selection = false\n"
         "send_log_lines = 5\nbogus = 1\n";
  }
  unsetenv("HERMES_AI_ENABLED");
  unsetenv("HERMES_AI_PROVIDER");
  unsetenv("HERMES_AI_API_KEY");
  AssistantConfig c = load_assistant_config(p.string());
  CHECK(c.enabled && c.provider.kind == "openai" && c.provider.model == "qwen3");
  CHECK(c.provider.base_url == "http://localhost:11434/v1");
  CHECK(c.edits == EditPolicy::Ask);
  CHECK(!c.send_selection && c.send_log_lines == 5);
  CHECK(c.warnings.size() == 1);
  std::string why;
  CHECK(endpoint_is_local(c));
  CHECK(assistant_ready(c, &why));  // local model: no key needed
  const std::string share = sharing_text(c, "http://localhost:11434/v1/chat/completions");
  CHECK(has(share, "nothing leaves it"));
  CHECK(!has(share, "selected"));
  CHECK(has(share, "5 log lines"));
  // Environment overrides.
  setenv("HERMES_AI_PROVIDER", "anthropic", 1);
  setenv("HERMES_AI_ENABLED", "0", 1);
  AssistantConfig e = load_assistant_config(p.string());
  CHECK(e.provider.kind == "anthropic" && !e.enabled);
  CHECK(!assistant_ready(e, &why) && has(why, "off"));
  e.enabled = true;
  e.provider.base_url.clear();
  setenv("ANTHROPIC_API_KEY", "", 1);
  resolve_api_key(e);
  CHECK(!assistant_ready(e, &why) && has(why, "ANTHROPIC_API_KEY"));
  setenv("HERMES_AI_API_KEY", "sk-secret-123", 1);
  CHECK(resolve_api_key(e) && e.provider.api_key == "sk-secret-123");
  CHECK(assistant_ready(e, &why));
  CHECK(!has(sharing_text(e, "https://api.anthropic.com/v1/messages"), "sk-secret"));
  CHECK(!has(status_line(e), "sk-secret"));
  unsetenv("HERMES_AI_API_KEY");
  unsetenv("HERMES_AI_PROVIDER");
  unsetenv("HERMES_AI_ENABLED");
  unsetenv("ANTHROPIC_API_KEY");
  // Defaults: off and read-only.
  AssistantConfig d = load_assistant_config((fs::temp_directory_path() / "no_such_hermes.conf").string());
  CHECK(!d.enabled && d.edits == EditPolicy::ReadOnly && d.source == "defaults");
  // The template parses cleanly and stays off.
  {
    std::ofstream f(p);
    f << config_template();
  }
  AssistantConfig t = load_assistant_config(p.string());
  CHECK(t.warnings.empty() && !t.enabled && t.edits == EditPolicy::ReadOnly);
  fs::remove(p);
}

void test_context_respects_config() {
  Rig r;
  r.view.select_cell(r.cell0(), false, nullptr);
  AssistantConfig c;
  std::string ctx = build_context(c, r.session, r.view);
  CHECK(has(ctx, "\"cells\":" + std::to_string(r.n)) && has(ctx, r.cell0()));
  c.send_selection = false;
  CHECK(!has(build_context(c, r.session, r.view), r.cell0()));
  c.send_design_summary = false;
  CHECK(build_context(c, r.session, r.view).empty());
}

void test_run_history() {
#if HERMES_HAVE_SQLITE
  const fs::path db = fs::temp_directory_path() / "hermes_assist_runs.db";
  fs::remove(db);
  {
    track::RunInfo info;
    info.design = "adder";
    info.name = "try-1";
    track::TrackerOptions o;
    o.db_path = db.string();
    track::RunTracker t(info, o);
    t.summary("hpwl", 123.0);
    t.finish("done");
  }
  std::string err;
  Json l = list_runs(db.string(), "", 10, &err);
  CHECK(err.empty());
  CHECK(l["runs"].size() == 1);
  CHECK(l["runs"][0]["name"].as_string() == "try-1" && l["runs"][0]["status"].as_string() == "done");
  const std::string id = l["runs"][0]["id"].as_string();
  Json d = run_details(db.string(), id.substr(0, 6), &err);
  CHECK(err.empty() && d["design"].as_string() == "adder");
  CHECK(list_runs(db.string(), "other", 10, &err)["runs"].size() == 0);
  run_details(db.string(), "zzzz", &err);
  CHECK(!err.empty());
  // Through the tool.
  Rig r;
  r.ctx.runs_db = db.string();
  ToolResult tr = call_tool_blocking(r.tools, r.ctx, "run_history", Json::object({{"design", "adder"}}));
  CHECK(tr.ok && has(tr.text, "try-1"));
  fs::remove(db);
#endif
}

void test_ai_command() {
  Rig r;
  AssistantConfig cfg;  // off
  AiHost host(cfg);
  host.register_commands(r.commands);
  gui::CommandContext cc{r.session, r.view};
  gui::CommandResult off = r.commands.run(cc, "ai what is this?");
  CHECK(!off.ok && has(off.text, "off"));
  CHECK(r.commands.run(cc, "ai status").ok);
  CHECK(has(r.commands.run(cc, "ai privacy").text, "Nothing is sent until"));
  CHECK(has(r.commands.run(cc, "ai tools").text, "design_summary"));
  CHECK(r.commands.run(cc, "ai set provider mock").ok);
  CHECK(!r.commands.run(cc, "ai set edits sometimes").ok);
  CHECK(r.commands.run(cc, "ai on").ok);
  // The built-in mock (offline demo) answers with a tool.
  gui::CommandResult q = r.commands.run(cc, "ai show me the congestion");
  CHECK(q.ok);
  const double t0 = now_ms();
  while (host.assistant() && host.assistant()->busy() && now_ms() - t0 < 20000) {
    host.pump(r.session, r.view);
    r.session.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  bool called = false;
  for (const auto& e : host.assistant()->transcript()) called |= e.kind == Assistant::Entry::Kind::ToolCall && has(e.text, "congestion_report");
  CHECK(called);
  CHECK(r.commands.run(cc, "ai reset").ok);
  CHECK(!r.commands.run(cc, "ai yes").ok);  // nothing pending
}

}  // namespace

int main() {
  log::configure("warn");
  log::console_sink()->set_level(log::Level::Off);
  struct {
    const char* name;
    void (*fn)();
  } tests[] = {
      {"json", test_json},
      {"tools_read", test_tools_read},
      {"tools_change_and_policy", test_tools_change_and_policy},
      {"mcp", test_mcp},
      {"anthropic_adapter", test_anthropic_adapter},
      {"openai_adapter", test_openai_adapter},
      {"assistant_tool_round_trip", test_assistant_tool_round_trip},
      {"assistant_read_only_refuses", test_assistant_read_only_refuses},
      {"assistant_confirmations", test_assistant_confirmations},
      {"assistant_nonblocking_and_cancel", test_assistant_nonblocking_and_cancel},
      {"provider_error_keeps_history_valid", test_provider_error_keeps_history_valid},
      {"tool_call_limit", test_tool_call_limit},
      {"config_and_privacy", test_config_and_privacy},
      {"context_respects_config", test_context_respects_config},
      {"run_history", test_run_history},
      {"ai_command", test_ai_command},
  };
  for (const auto& t : tests) {
    const int before = g_failures;
    t.fn();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
  }
  std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "all passed", g_failures);
  return g_failures ? 1 : 0;
}
