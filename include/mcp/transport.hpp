// mcp-cpp: transport abstraction.
//
// StdioTransport and the Workflow (sogou/workflow) based Streamable HTTP
// transport implement the same ITransport interface so the server core never
// changes.
#pragma once

#include <mcp/dispatch.hpp>

#include <nlohmann/json.hpp>

#include <functional>

namespace mcp {

class ITransport {
 public:
  // Handler receives one decoded JSON-RPC message (request or notification) and
  // calls `done` with the response to send back (null json -> nothing to send).
  // For a request owned by a kDeferred tool, `done` may be called later and from
  // another thread; the transport must be ready for that.
  using MessageHandler = std::function<void(const nlohmann::json&, Done)>;

  virtual ~ITransport() = default;

  virtual void set_message_handler(MessageHandler handler) = 0;

  // Queue a JSON message for delivery to the peer (encode + write).
  virtual void send(const nlohmann::json& message) = 0;

  // Blocking event loop. Returns the process exit code.
  virtual int run() = 0;
};

}  // namespace mcp
