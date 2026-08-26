// mcp-cpp: example MCP server on stdio.
//
//   echo      - echoes back the provided text
//   add       - adds two numbers (throws std::invalid_argument on bad input)
//   get_time  - current UTC time in ISO-8601 format
//
// Run with: ./mcp_example   (speaks newline-delimited JSON-RPC on stdin/stdout)
//
// get_time uses gmtime_r(), a POSIX function; expose it even under strict
// -std=c++17 (glibc hides POSIX declarations unless a feature macro is set).
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <mcp/server.hpp>
#include <mcp/transport_stdio.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>

using nlohmann::json;

namespace {

json echo_handler(const json& args) {
  return json{{"text", args.value("text", "")}};
}

json add_handler(const json& args) {
  if (!args.contains("a") || !args.contains("b") || !args["a"].is_number() ||
      !args["b"].is_number()) {
    throw std::invalid_argument("add requires numeric arguments \"a\" and \"b\"");
  }
  if (args["a"].is_number_integer() && args["b"].is_number_integer()) {
    return json{{"sum", args["a"].get<long long>() + args["b"].get<long long>()}};
  }
  return json{{"sum", args["a"].get<double>() + args["b"].get<double>()}};
}

json get_time_handler(const json& /*args*/) {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);  // POSIX; the project only builds on Linux
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return json{{"time", std::string(buf)}};
}

}  // namespace

int main() {
  mcp::McpServer server;
  server.add_tool(mcp::Tool{
      "echo", "Echoes back the provided text.", json::object(), echo_handler});
  server.add_tool(mcp::Tool{
      "add", "Adds two numbers and returns their sum.", json::object(),
      add_handler});
  server.add_tool(mcp::Tool{
      "get_time", "Returns the current UTC time in ISO-8601 format.",
      json::object(), get_time_handler});

  mcp::StdioTransport transport;
  transport.set_message_handler([&server](const json& message) {
    return server.handle_message(message);
  });
  return transport.run();
}
