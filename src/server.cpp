// mcp-cpp: MCP server core implementation.
#include <mcp/server.hpp>

#include <mcp/jsonrpc.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace mcp {

McpServer::McpServer(ServerOptions options) : options_(std::move(options)) {}

McpServer::McpServer(McpServer&& other) noexcept
    : options_(std::move(other.options_)),
      tools_(std::move(other.tools_)),
      initialized_(other.initialized_.load()) {}

McpServer& McpServer::operator=(McpServer&& other) noexcept {
  if (this != &other) {
    options_ = std::move(other.options_);
    tools_ = std::move(other.tools_);
    initialized_.store(other.initialized_.load());
  }
  return *this;
}

void McpServer::add_tool(Tool tool) {
  for (auto& existing : tools_) {
    if (existing.name == tool.name) {
      existing = std::move(tool);
      return;
    }
  }
  tools_.push_back(std::move(tool));
}

nlohmann::json McpServer::handle_message(const nlohmann::json& message) {
  try {
    // Only requests and notifications reach the server core.
    if (!message.is_object() || !message.contains("method") ||
        !message["method"].is_string()) {
      return jsonrpc::make_error_response(nullptr, jsonrpc::kInvalidRequest,
                                          "Invalid Request");
    }

    const std::string method = message["method"].get<std::string>();
    const bool is_request = message.contains("id");

    if (!is_request) {
      // Notification: never replied to; unknown ones are silently ignored.
      if (method == kMethodInitialized) {
        initialized_.store(true);
      }
      return nullptr;  // nothing to send back
    }

    // Detect a 2026-07-28 (stateless) request. The transport routes by the
    // MCP-Protocol-Version header; here we additionally trust the _meta
    // envelope so the core stays correct regardless of transport.
    bool is_v2 = (method == kMethodServerDiscover);
    if (!is_v2 && message.contains("params") && message["params"].is_object() &&
        message["params"].contains("_meta") &&
        message["params"]["_meta"].is_object()) {
      const auto& meta = message["params"]["_meta"];
      const auto it = meta.find(kMetaProtocolVersionKey);
      if (it != meta.end() && it->is_string() &&
          it->get<std::string>() == kProtocolVersionModern) {
        is_v2 = true;
      }
    }

    const nlohmann::json id = message["id"];
    if (method == kMethodInitialize) {
      return handle_initialize(id);
    }
    // 2026-07-28 stateless discovery: no handshake, single exchange. Response
    // shape differs from initialize (capabilities/cacheScope/resultType/
    // supportedVersions/ttlMs).
    if (method == kMethodServerDiscover) {
      return handle_server_discover(id);
    }
    if (method == kMethodPing) {
      // ping result: v1 is an empty object; v2 requires resultType.
      return jsonrpc::make_result_response(
          id, is_v2 ? nlohmann::json{{"resultType", "complete"}}
                    : nlohmann::json::object());
    }
    if (method == kMethodToolsList) {
      return handle_tools_list(id, is_v2);
    }
    if (method == kMethodToolsCall) {
      return handle_tools_call(id, message, is_v2);
    }
    return jsonrpc::make_error_response(id, jsonrpc::kMethodNotFound,
                                        "Method not found");
  } catch (const std::exception& e) {
    return jsonrpc::make_error_response(
        message.is_object() && message.contains("id") ? message["id"]
                                                      : nlohmann::json(nullptr),
        jsonrpc::kInternalError, e.what());
  } catch (...) {
    return jsonrpc::make_error_response(
        message.is_object() && message.contains("id") ? message["id"]
                                                      : nlohmann::json(nullptr),
        jsonrpc::kInternalError, "Internal error");
  }
}

nlohmann::json McpServer::handle_initialize(const nlohmann::json& id) {
  // Advertise the configured protocol version without negotiating; the client's
  // requested version is ignored (recorded by the transport only).
  nlohmann::json result = {
      {"protocolVersion", options_.protocol_version},
      {"capabilities", {{"tools", {{"listChanged", false}}}}},
      {"serverInfo", {{"name", options_.name}, {"version", options_.version}}}};
  return jsonrpc::make_result_response(id, std::move(result));
}

nlohmann::json McpServer::handle_server_discover(const nlohmann::json& id) {
  // 2026-07-28 stateless discovery. Shape differs from initialize:
  // capabilities / cacheScope / resultType / supportedVersions / ttlMs
  // (no protocolVersion/serverInfo). Older clients ignore unknown keys.
  nlohmann::json result = {
      {"capabilities", {{"tools", {{"listChanged", false}}}}},
      {"cacheScope", "private"},
      {"resultType", "complete"},
      {"supportedVersions",
       nlohmann::json::array({kProtocolVersionModern, kProtocolVersion})},
      {"ttlMs", 0}};
  return jsonrpc::make_result_response(id, std::move(result));
}

nlohmann::json McpServer::handle_tools_list(const nlohmann::json& id,
                                            bool v2) {
  nlohmann::json tools = nlohmann::json::array();
  for (const auto& tool : tools_) {
    tools.push_back(tool.to_json());
  }
  if (v2) {
    // cacheScope/resultType/ttlMs are required on 2026-07-28.
    return jsonrpc::make_result_response(
        id, {{"tools", std::move(tools)},
             {"cacheScope", "private"},
             {"resultType", "complete"},
             {"ttlMs", 0}});
  }
  return jsonrpc::make_result_response(id, {{"tools", std::move(tools)}});
}

nlohmann::json McpServer::handle_tools_call(const nlohmann::json& id,
                                            const nlohmann::json& message,
                                            bool v2) {
  if (!message.contains("params") || !message["params"].is_object()) {
    return jsonrpc::make_error_response(id, jsonrpc::kInvalidParams,
                                        "Invalid params: expected an object");
  }
  const auto& params = message["params"];

  if (!params.contains("name") || !params["name"].is_string()) {
    return jsonrpc::make_error_response(
        id, jsonrpc::kInvalidParams, "Invalid params: missing string \"name\"");
  }
  const std::string name = params["name"].get<std::string>();

  if (params.contains("arguments") && !params["arguments"].is_object()) {
    return jsonrpc::make_error_response(
        id, jsonrpc::kInvalidParams,
        "Invalid params: \"arguments\" must be an object");
  }
  nlohmann::json args =
      params.contains("arguments") ? params["arguments"] : nlohmann::json::object();

  const Tool* tool = nullptr;
  for (const auto& candidate : tools_) {
    if (candidate.name == name) {
      tool = &candidate;
      break;
    }
  }
  if (tool == nullptr) {
    return jsonrpc::make_error_response(id, kToolNotFound,
                                        "Tool not found: " + name);
  }

  try {
    nlohmann::json output = tool->handler(args);
    if (output.is_object() && output.contains("content")) {
      // The handler already produced an MCP call result: pass it through. On
      // v2, ensure resultType is present (required on 2026-07-28).
      if (v2 && !output.contains("resultType")) {
        output["resultType"] = "complete";
      }
      return jsonrpc::make_result_response(id, std::move(output));
    }
    std::string text = output.is_string() ? output.get<std::string>()
                                          : output.dump();
    nlohmann::json content_item = {{"type", "text"}, {"text", std::move(text)}};
    nlohmann::json result = {{"content", nlohmann::json::array({content_item})},
                             {"isError", false}};
    if (v2) {
      result["resultType"] = "complete";
    }
    return jsonrpc::make_result_response(id, std::move(result));
  } catch (const std::invalid_argument& e) {
    // Handler reported bad arguments: -32602 per the JSON-RPC contract.
    return jsonrpc::make_error_response(id, jsonrpc::kInvalidParams, e.what());
  } catch (const std::exception& e) {
    return jsonrpc::make_error_response(id, jsonrpc::kInternalError, e.what());
  } catch (...) {
    return jsonrpc::make_error_response(id, jsonrpc::kInternalError,
                                        "Internal error");
  }
}

}  // namespace mcp
