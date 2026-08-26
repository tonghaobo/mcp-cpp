// mcp-cpp: stdio transport (MCP spec: newline-delimited JSON-RPC over stdin/stdout).
#pragma once

#include <mcp/transport.hpp>

namespace mcp {

class StdioTransport : public ITransport {
 public:
  void set_message_handler(MessageHandler handler) override;
  void send(const nlohmann::json& message) override;
  int run() override;

 private:
  MessageHandler handler_;
};

}  // namespace mcp
