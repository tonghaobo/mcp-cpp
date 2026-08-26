// mcp-cpp: stdio transport (MCP spec: newline-delimited JSON-RPC over
// stdin/stdout).
//
// Reads one JSON message per line from stdin, decodes it with jsonrpc::parse,
// forwards requests/notifications to the handler, and writes any reply as a
// single JSON line to stdout (flushed). SIGPIPE is ignored so a client that
// closes the pipe cannot kill the process; stdin EOF ends the loop cleanly.
#include <mcp/jsonrpc.hpp>
#include <mcp/transport_stdio.hpp>

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <csignal>
#endif

namespace mcp {

void StdioTransport::set_message_handler(MessageHandler handler) {
  handler_ = std::move(handler);
}

void StdioTransport::send(const nlohmann::json& message) {
  try {
    std::cout << message.dump() << '\n';
    std::cout.flush();
  } catch (const std::exception&) {
    // A message that cannot be serialized (e.g. a non-finite number) must not
    // kill the transport loop; report it as a JSON-RPC internal error instead.
    std::cout << R"({"jsonrpc":"2.0","id":null,)"
                 R"("error":{"code":-32603,"message":"Failed to serialize response"}})"
              << '\n';
    std::cout.flush();
  }
}

int StdioTransport::run() {
#if !defined(_WIN32)
  // Writing to a pipe whose reader has gone away must not terminate us.
  std::signal(SIGPIPE, SIG_IGN);
#endif

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;  // blank lines carry no message
    }
    if (line.back() == '\r') {
      line.pop_back();  // tolerate CRLF line endings
    }

    const jsonrpc::ParsedMessage pm = jsonrpc::parse(line);
    switch (pm.kind) {
      case jsonrpc::ParsedMessage::Kind::kRequest: {
        if (handler_) {
          const nlohmann::json reply = handler_(pm.request.to_json());
          if (!reply.is_null()) {
            send(reply);
          }
        }
        break;
      }
      case jsonrpc::ParsedMessage::Kind::kNotification: {
        // Notifications never produce a reply, but must still be processed
        // (e.g. notifications/initialized updates server state).
        if (handler_) {
          handler_(pm.notification.to_json());
        }
        break;
      }
      case jsonrpc::ParsedMessage::Kind::kResponse:
        // We are a server; a response from the peer is unexpected. Ignore it.
        break;
      case jsonrpc::ParsedMessage::Kind::kParseError:
      case jsonrpc::ParsedMessage::Kind::kInvalidRequest:
        // Report malformed input to the peer (echoing the id when known).
        send(jsonrpc::make_error_response(pm.error_id, pm.error.code,
                                          pm.error.message));
        break;
    }
  }

  // stdin EOF: graceful shutdown.
  return 0;
}

}  // namespace mcp
