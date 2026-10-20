// mcp-cpp: MCP server core implementation.
#include <mcp/server.hpp>

#include <mcp/jsonrpc.hpp>

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace mcp {

namespace {

// Wrap a tool output into an MCP call result: a handler that already produced
// a full result (has "content") passes through, anything else becomes one text
// content item. 2026-07-28 requires resultType.
nlohmann::json tool_result(nlohmann::json output, bool v2) {
  if (output.is_object() && output.contains("content")) {
    if (v2 && !output.contains("resultType")) {
      output["resultType"] = "complete";
    }
    return output;
  }
  std::string text = output.is_string() ? output.get<std::string>()
                                        : output.dump();
  nlohmann::json content_item = {{"type", "text"}, {"text", std::move(text)}};
  nlohmann::json result = {{"content", nlohmann::json::array({content_item})},
                           {"isError", false}};
  if (v2) {
    result["resultType"] = "complete";
  }
  return result;
}

}  // namespace

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

Dispatch McpServer::tool_dispatch(const std::string& name) const {
  for (const auto& tool : tools_) {
    if (tool.name == name) {
      return tool.dispatch;
    }
  }
  return Dispatch::kInline;
}

nlohmann::json McpServer::handle_message(const nlohmann::json& message) {
  // The guard keeps a late completion (a kDeferred tool answering after this
  // call returned) from writing into a reply nobody owns any more.
  auto returned = std::make_shared<bool>(false);
  nlohmann::json reply;
  handle_message(message, [&reply, returned](nlohmann::json response) {
    if (!*returned) {
      reply = std::move(response);
    }
  });
  *returned = true;
  return reply;
}

void McpServer::handle_message(const nlohmann::json& message, Done done) {
  try {
    // Only requests and notifications reach the server core.
    if (!message.is_object() || !message.contains("method") ||
        !message["method"].is_string()) {
      done(jsonrpc::make_error_response(nullptr, jsonrpc::kInvalidRequest,
                                        "Invalid Request"));
      return;
    }

    const std::string method = message["method"].get<std::string>();
    const bool is_request = message.contains("id");

    if (!is_request) {
      // Notification: never replied to; unknown ones are silently ignored.
      if (method == kMethodInitialized) {
        initialized_.store(true);
      }
      done(nullptr);  // nothing to send back
      return;
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
      done(handle_initialize(id));
      return;
    }
    // 2026-07-28 stateless discovery: no handshake, single exchange. Response
    // shape differs from initialize (capabilities/cacheScope/resultType/
    // supportedVersions/ttlMs).
    if (method == kMethodServerDiscover) {
      done(handle_server_discover(id));
      return;
    }
    if (method == kMethodPing) {
      // ping result: v1 is an empty object; v2 requires resultType.
      done(jsonrpc::make_result_response(
          id, is_v2 ? nlohmann::json{{"resultType", "complete"}}
                    : nlohmann::json::object()));
      return;
    }
    if (method == kMethodToolsList) {
      done(handle_tools_list(id, is_v2));
      return;
    }
    if (method == kMethodToolsCall) {
      // Passed by value: a tool that replies late needs `done` alive past this
      // frame, and the catch handlers below still want their own copy.
      handle_tools_call(id, message, is_v2, done);
      return;
    }
    done(jsonrpc::make_error_response(id, jsonrpc::kMethodNotFound,
                                      "Method not found"));
  } catch (const std::exception& e) {
    done(jsonrpc::make_error_response(
        message.is_object() && message.contains("id") ? message["id"]
                                                      : nlohmann::json(nullptr),
        jsonrpc::kInternalError, e.what()));
  } catch (...) {
    done(jsonrpc::make_error_response(
        message.is_object() && message.contains("id") ? message["id"]
                                                      : nlohmann::json(nullptr),
        jsonrpc::kInternalError, "Internal error"));
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

void McpServer::handle_tools_call(const nlohmann::json& id,
                                  const nlohmann::json& message, bool v2,
                                  Done done) {
  if (!message.contains("params") || !message["params"].is_object()) {
    done(jsonrpc::make_error_response(id, jsonrpc::kInvalidParams,
                                      "Invalid params: expected an object"));
    return;
  }
  const auto& params = message["params"];

  if (!params.contains("name") || !params["name"].is_string()) {
    done(jsonrpc::make_error_response(
        id, jsonrpc::kInvalidParams, "Invalid params: missing string \"name\""));
    return;
  }
  const std::string name = params["name"].get<std::string>();

  if (params.contains("arguments") && !params["arguments"].is_object()) {
    done(jsonrpc::make_error_response(
        id, jsonrpc::kInvalidParams,
        "Invalid params: \"arguments\" must be an object"));
    return;
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
    done(jsonrpc::make_error_response(id, kToolNotFound,
                                      "Tool not found: " + name));
    return;
  }

  if (tool->dispatch == Dispatch::kDeferred) {
    if (!tool->deferred) {
      done(jsonrpc::make_error_response(id, jsonrpc::kInternalError,
                                        "Tool has no deferred handler"));
      return;
    }
    // The core owns the JSON-RPC envelope, so the handler only has to produce
    // the tool output a synchronous handler would have returned.
    try {
      tool->deferred(args, [raw = done, id, v2](nlohmann::json output) {
        raw(jsonrpc::make_result_response(id, tool_result(std::move(output), v2)));
      });
    } catch (const std::invalid_argument& e) {
      done(jsonrpc::make_error_response(id, jsonrpc::kInvalidParams, e.what()));
    } catch (const std::exception& e) {
      done(jsonrpc::make_error_response(id, jsonrpc::kInternalError, e.what()));
    } catch (...) {
      done(jsonrpc::make_error_response(id, jsonrpc::kInternalError,
                                        "Internal error"));
    }
    return;
  }

  try {
    done(jsonrpc::make_result_response(
        id, tool_result(tool->handler(args), v2)));
  } catch (const std::invalid_argument& e) {
    // Handler reported bad arguments: -32602 per the JSON-RPC contract.
    done(jsonrpc::make_error_response(id, jsonrpc::kInvalidParams, e.what()));
  } catch (const std::exception& e) {
    done(jsonrpc::make_error_response(id, jsonrpc::kInternalError, e.what()));
  } catch (...) {
    done(jsonrpc::make_error_response(id, jsonrpc::kInternalError,
                                      "Internal error"));
  }
}

}  // namespace mcp
