// mcp-cpp: transport abstraction.
//
// StdioTransport and the Workflow (sogou/workflow) based Streamable HTTP
// transport implement the same ITransport interface so the server core never
// changes.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>

namespace mcp {

class ITransport {
 public:
  // Handler receives one decoded JSON-RPC message (request or notification)
  // and returns the response JSON to send back (null json -> nothing to send).
  using MessageHandler = std::function<nlohmann::json(const nlohmann::json&)>;

  virtual ~ITransport() = default;

  virtual void set_message_handler(MessageHandler handler) = 0;

  // Queue a JSON message for delivery to the peer (encode + write).
  virtual void send(const nlohmann::json& message) = 0;

  // Blocking event loop. Returns the process exit code.
  virtual int run() = 0;
};

}  // namespace mcp
