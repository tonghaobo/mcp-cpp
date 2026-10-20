// mcp-cpp: MCP server core: tool registry + JSON-RPC dispatch.
//
// Transport-agnostic: the transport layer decodes wire bytes into a JSON
// message (see jsonrpc::parse) and feeds it to handle_message(). The server
// never touches the transport directly, so StdioTransport and the
// Workflow-based Streamable HTTP transport share the exact same core.
#pragma once

#include <mcp/protocol.hpp>
#include <mcp/tool.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <vector>

namespace mcp {

// Server identity and protocol settings. Defaults match the project baseline;
// override them via the McpServer constructor.
struct ServerOptions {
  std::string name = kServerName;                  // serverInfo.name
  std::string version = kServerVersion;            // serverInfo.version
  std::string protocol_version = kProtocolVersion;  // advertised protocolVersion
};

class McpServer {
 public:
  explicit McpServer(ServerOptions options = ServerOptions{});

  // std::atomic is non-movable, so provide explicit move semantics (the tools
  // vector and options still move; the atomic flag is copied by value).
  McpServer(McpServer&& other) noexcept;
  McpServer& operator=(McpServer&& other) noexcept;
  McpServer(const McpServer&) = delete;
  McpServer& operator=(const McpServer&) = delete;

  // Register a tool. Registering a tool with an existing name replaces it.
  void add_tool(Tool tool);

  // All registered tools, in registration order.
  const std::vector<Tool>& tools() const { return tools_; }

  // Execution model the named tool declared (kInline when it is not
  // registered). The HTTP transport uses this to decide how to run a tool/call.
  Dispatch tool_dispatch(const std::string& name) const;

  // Handle one decoded JSON-RPC message (a request or notification object, as
  // produced by the transport layer). `done` is called exactly once: before
  // returning for every reply the core can produce itself, or later (possibly
  // from another thread) when a kDeferred tool replies. A null json means
  // nothing should be sent back. Never throws.
  void handle_message(const nlohmann::json& message, Done done);

  // Convenience for callers that cannot handle a late reply (unit tests, or a
  // server with no kDeferred tools): returns the reply, or a null json when
  // nothing should be sent. A completion arriving after this returns is dropped.
  nlohmann::json handle_message(const nlohmann::json& message);

  // True once a client has sent notifications/initialized. Atomic so that the
  // HTTP transport (multi-client) may dispatch to handle_message() concurrently.
  bool initialized() const { return initialized_.load(); }

 private:
  nlohmann::json handle_initialize(const nlohmann::json& id);
  nlohmann::json handle_server_discover(const nlohmann::json& id);
  nlohmann::json handle_tools_list(const nlohmann::json& id, bool v2);
  void handle_tools_call(const nlohmann::json& id,
                         const nlohmann::json& message, bool v2, Done done);

  ServerOptions options_;
  std::vector<Tool> tools_;
  std::atomic<bool> initialized_{false};
};

}  // namespace mcp
