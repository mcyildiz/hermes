#include "assist/mcp_server.hpp"

#include <istream>
#include <ostream>

#include "gui/scene/session.hpp"
#include "hermes/track/run_tracker.hpp"

namespace hermes::assist {

namespace {

Json rpc_error(const Json& id, int code, std::string msg) {
  return Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"error", Json::object({{"code", code}, {"message", std::move(msg)}})}});
}

Json rpc_result(const Json& id, Json result) {
  return Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

}  // namespace

McpServer::McpServer(const ToolRegistry& tools, ToolContext& ctx, McpOptions opts)
    : tools_(tools), ctx_(ctx), opts_(std::move(opts)) {
  if (opts_.edits == EditPolicy::Ask) opts_.edits = EditPolicy::Allow;  // the client asks
}

Json McpServer::tools_list() const {
  Json arr = Json::array();
  for (const Tool* t : tools_.list(opts_.edits != EditPolicy::ReadOnly)) {
    const bool read = t->access == Access::Read && !t->access_for;
    Json ann = Json::object({{"title", t->title},
                             {"readOnlyHint", t->access == Access::Read && !t->access_for},
                             {"openWorldHint", false}});
    if (!read) ann.set("destructiveHint", t->access == Access::Change);
    std::string desc = t->description;
    if (t->access_for)
      desc += opts_.edits == EditPolicy::ReadOnly ? " (This server is read-only: commands that change the design are refused.)"
                                                  : "";
    arr.push_back(Json::object({{"name", t->name},
                                {"title", t->title},
                                {"description", desc},
                                {"inputSchema", t->schema},
                                {"annotations", std::move(ann)}}));
  }
  return Json::object({{"tools", std::move(arr)}});
}

Json McpServer::tools_call(const Json& params) {
  const std::string name = params["name"].as_string();
  const Json& args = params["arguments"];
  ToolResult r;
  if (std::string why = policy_refusal(tools_, ctx_, name, args, opts_.edits); !why.empty()) {
    r = {false,
         why + " (the hermes MCP server was started read-only; restart it with `hermes mcp --allow-edits` to allow "
               "changes)"};
  } else {
    r = call_tool_blocking(tools_, ctx_, name, args);
  }
  return Json::object({{"content", Json::array({Json::object({{"type", "text"}, {"text", clip_text(r.text, 60000)}})})},
                       {"isError", !r.ok}});
}

Json McpServer::handle(const Json& msg, bool* is_notification) {
  *is_notification = false;
  if (!msg.is_object() || msg["jsonrpc"].as_string() != "2.0") return rpc_error(msg["id"], -32600, "invalid request");
  const std::string method = msg["method"].as_string();
  if (!msg.contains("id")) {  // notification (initialized, cancelled, ...): no reply
    *is_notification = true;
    return {};
  }
  const Json& id = msg["id"];
  const Json& params = msg["params"];
  ctx_.session.poll();
  if (method == "initialize") {
    std::string ver = params["protocolVersion"].as_string(kProtocolVersion);
    if (ver != "2024-11-05" && ver != "2025-03-26" && ver != "2025-06-18") ver = kProtocolVersion;
    std::string instr =
        "hermes: placement/routing engine with embedded STA. Tools inspect the design open in this session (start "
        "with design_summary), compute and read QoR/congestion/timing reports, query cells and nets, read the log "
        "and past runs. Lengths are in um. Several engines are placeholders (router stub, wire-delay STA); reports "
        "say what is estimated.";
    instr += opts_.edits == EditPolicy::ReadOnly
                 ? " This server is READ-ONLY: it cannot load designs, run flows, move cells or write files. The "
                   "engineer can restart it with `hermes mcp --allow-edits`."
                 : " Edits are enabled: load_design, run_flow, move_cell, save_design, run_command, etc. change the "
                   "design or write files; explain before calling them.";
    return rpc_result(id, Json::object({{"protocolVersion", ver},
                                        {"capabilities", Json::object({{"tools", Json::object({{"listChanged", false}})}})},
                                        {"serverInfo", Json::object({{"name", opts_.server_name},
                                                                     {"title", "hermes placement & routing"},
                                                                     {"version", track::build_version()}})},
                                        {"instructions", instr}}));
  }
  if (method == "ping") return rpc_result(id, Json::object());
  if (method == "tools/list") return rpc_result(id, tools_list());
  if (method == "tools/call") {
    if (!params["name"].is_string()) return rpc_error(id, -32602, "tools/call needs a tool name");
    if (!tools_.find(params["name"].str_ref())) return rpc_error(id, -32602, "unknown tool: " + params["name"].str_ref());
    return rpc_result(id, tools_call(params));
  }
  if (method == "resources/list") return rpc_result(id, Json::object({{"resources", Json::array()}}));
  if (method == "prompts/list") return rpc_result(id, Json::object({{"prompts", Json::array()}}));
  return rpc_error(id, -32601, "method not found: " + method);
}

std::optional<std::string> McpServer::handle_line(const std::string& line) {
  if (line.find_first_not_of(" \t\r\n") == std::string::npos) return std::nullopt;
  std::string err;
  const Json msg = Json::parse(line, &err);
  if (msg.is_null()) return rpc_error(Json(), -32700, "parse error: " + err).dump();
  bool note = false;
  Json reply = handle(msg, &note);
  if (note) return std::nullopt;
  return reply.dump();
}

int McpServer::serve(std::istream& in, std::ostream& out) {
  std::string line;
  while (std::getline(in, line)) {
    if (auto r = handle_line(line)) {
      out << *r << '\n';
      out.flush();
    }
    if (auto* hv = dynamic_cast<HeadlessView*>(&ctx_.view); hv && hv->quit_requested()) break;
  }
  return 0;
}

}  // namespace hermes::assist
