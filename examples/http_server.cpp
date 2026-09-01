// mcp-cpp: example MCP server over Streamable HTTP.
//
// Same tools as the stdio example (echo / add / get_time) plus an async tool
// (slow_add, a stand-in for a slow handler), served over a single HTTP endpoint
// using sogou/workflow. The MCP client connects via the official Python SDK's
// streamable_http_client, or any Streamable HTTP client.
//
// Build requires -DMCP_ENABLE_WORKFLOW=ON (drags in sogou/workflow + OpenSSL).
//
//   Run with: ./mcp_http_example [host] [port]
//             host defaults to 127.0.0.1 (loopback); use 0.0.0.0 to accept
//             remote connections. port defaults to 8931.
//   Connect:  streamable_http_client(url="http://127.0.0.1:8931/mcp")
//
// get_time uses gmtime_r(), a POSIX function; expose it even under strict
// -std=c++20 (glibc hides POSIX declarations unless a feature macro is set).
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <mcp/server.hpp>
#include <mcp/transport_http.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>

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

// A deliberately slow handler to demonstrate async execution. Registered with
// async = true so the transport runs it on a compute thread and the HTTP event
// loop is not blocked while it sleeps.
json slow_add_handler(const json& args) {
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  return add_handler(args);
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

int main(int argc, char** argv) {
  const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
  const unsigned port =
      argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 8931u;

  mcp::McpServer server;
  server.add_tool(mcp::Tool{
      .name = "echo", .description = "Echoes back the provided text.",
      .input_schema = json::object(), .handler = echo_handler});
  server.add_tool(mcp::Tool{
      .name = "add", .description = "Adds two numbers and returns their sum.",
      .input_schema = json::object(), .handler = add_handler});
  // slow_add is async: its handler runs on a workflow compute thread.
  server.add_tool(mcp::Tool{
      .name = "slow_add",
      .description = "Adds two numbers after a short delay (async demo).",
      .input_schema = json::object(), .handler = slow_add_handler,
      .async = true});
  server.add_tool(mcp::Tool{
      .name = "get_time",
      .description = "Returns the current UTC time in ISO-8601 format.",
      .input_schema = json::object(), .handler = get_time_handler});

  mcp::StreamableHttpTransport transport;
  transport.set_message_handler([&server](const json& message) {
    return server.handle_message(message);
  });
  // Tell the transport which tools must run off the transport thread.
  transport.set_async_probe(
      [&server](const std::string& name) { return server.is_tool_async(name); });

  // The transport prints a readiness line once the listener is up.
  return transport.run(host, port);
}
