#pragma once

// hermes as an MCP server (Model Context Protocol, JSON-RPC 2.0 over stdio,
// one message per line). Any MCP client can drive hermes with it: Claude
// Code, Claude desktop, or any other agent / model that speaks MCP.
//
//   hermes mcp [<design>] [--allow-edits]
//
// Read-only by default: only read tools are listed and change tools are
// refused. With --allow-edits the change tools are listed too, marked with
// readOnlyHint=false / destructiveHint, so the client asks the engineer
// before each call (Claude Code does this unless the engineer pre-approved
// the tool).
//
// stdout carries protocol messages only; hermes log output goes to stderr.

#include <iosfwd>
#include <optional>
#include <string>

#include "assist/json.hpp"
#include "assist/tools.hpp"

namespace hermes::assist {

struct McpOptions {
  EditPolicy edits = EditPolicy::ReadOnly;  ///< ReadOnly or Allow.
  std::string server_name = "hermes";
};

class McpServer {
 public:
  McpServer(const ToolRegistry& tools, ToolContext& ctx, McpOptions opts = {});
  /// Handles one JSON-RPC message; returns the reply line (none for notifications).
  std::optional<std::string> handle_line(const std::string& line);
  Json handle(const Json& msg, bool* is_notification);
  /// Reads messages from `in` until EOF, writes replies to `out`.
  int serve(std::istream& in, std::ostream& out);

  static constexpr const char* kProtocolVersion = "2025-06-18";

 private:
  Json tools_list() const;
  Json tools_call(const Json& params);

  const ToolRegistry& tools_;
  ToolContext& ctx_;
  McpOptions opts_;
};

}  // namespace hermes::assist
