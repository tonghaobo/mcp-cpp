// mcp-cpp: example MCP server on stdio.
//
//   echo       - echoes back the provided text
//   add        - adds two numbers (throws std::invalid_argument on bad input)
//   get_time   - current UTC time in ISO-8601 format
//   slow_query - deferred tool: replies from another thread after a delay
//
// Run with: ./mcp_example   (speaks newline-delimited JSON-RPC on stdin/stdout)
//
// get_time uses gmtime_r(), a POSIX function; expose it even under strict
// -std=c++20 (glibc hides POSIX declarations unless a feature macro is set).
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <mcp/server.hpp>
#include <mcp/transport_stdio.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

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

// Deferred handler: returns at once, replies from a detached thread. Standing
// in for a query against a slow external system.
void slow_query_handler(const json& args, mcp::Done done) {
  std::thread([args, done = std::move(done)]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    done(json{{"rows", args.value("rows", 0)}});
  }).detach();
}

}  // namespace

int main() {
  mcp::McpServer server;
  server.add_tool(mcp::Tool{
      .name = "echo", .description = "Echoes back the provided text.",
      .input_schema = json::object(), .handler = echo_handler});
  server.add_tool(mcp::Tool{
      .name = "add", .description = "Adds two numbers and returns their sum.",
      .input_schema = json::object(), .handler = add_handler});
  server.add_tool(mcp::Tool{
      .name = "get_time",
      .description = "Returns the current UTC time in ISO-8601 format.",
      .input_schema = json::object(), .handler = get_time_handler});
  server.add_tool(mcp::Tool{
      .name = "slow_query",
      .description = "Queries a slow external system; replies from another thread.",
      .input_schema = json::object(),
      .dispatch = mcp::Dispatch::kDeferred,
      .deferred = slow_query_handler});

  mcp::StdioTransport transport;
  transport.set_message_handler(
      [&server](const json& message, mcp::Done done) {
        server.handle_message(message, std::move(done));
      });
  return transport.run();
}
