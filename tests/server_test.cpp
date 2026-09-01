// mcp-cpp: unit tests for the McpServer core (mcp/server.hpp).
#include <mcp/jsonrpc.hpp>
#include <mcp/protocol.hpp>
#include <mcp/server.hpp>
#include <mcp/tool.hpp>

#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

using nlohmann::json;
using mcp::jsonrpc::kInvalidParams;
using mcp::jsonrpc::kInvalidRequest;
using mcp::jsonrpc::kInternalError;
using mcp::jsonrpc::kMethodNotFound;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL (line " << line << "): " << what << '\n';
  }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

json echo_handler(const json& args) {
  return json{{"text", args.value("text", "")}};
}

json add_handler(const json& args) {
  if (!args.contains("a") || !args.contains("b") || !args["a"].is_number() ||
      !args["b"].is_number()) {
    throw std::invalid_argument("a and b must be numbers");
  }
  return json{{"sum", args["a"].get<long long>() + args["b"].get<long long>()}};
}

json boom_handler(const json& /*args*/) {
  throw std::runtime_error("boom");
}

mcp::McpServer make_server() {
  mcp::McpServer server;
  server.add_tool(mcp::Tool{.name = "echo",
                            .description = "Echoes text back.",
                            .input_schema = json::object(),
                            .handler = echo_handler});
  server.add_tool(mcp::Tool{.name = "add",
                            .description = "Adds two numbers.",
                            .input_schema = json::object(),
                            .handler = add_handler});
  server.add_tool(mcp::Tool{.name = "boom",
                            .description = "Always throws.",
                            .input_schema = json::object(),
                            .handler = boom_handler});
  return server;
}

// --- initialize -------------------------------------------------------------

void test_initialize_handshake_fields() {
  mcp::McpServer server = make_server();
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{
         "protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}}})");
  json reply = server.handle_message(msg);

  CHECK(reply["id"] == 1);
  CHECK(reply["result"]["protocolVersion"] == mcp::kProtocolVersion);
  CHECK(reply["result"]["capabilities"]["tools"]["listChanged"] == false);
  CHECK(reply["result"]["serverInfo"]["name"] == mcp::kServerName);
  CHECK(reply["result"]["serverInfo"]["version"] == mcp::kServerVersion);
}

void test_initialize_honors_server_options() {
  mcp::McpServer server(
      mcp::ServerOptions{"custom-name", "9.9.9", "2026-07-28"});
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{}})");
  json reply = server.handle_message(msg);

  CHECK(reply["result"]["protocolVersion"] == "2026-07-28");
  CHECK(reply["result"]["serverInfo"]["name"] == "custom-name");
  CHECK(reply["result"]["serverInfo"]["version"] == "9.9.9");
}

// --- ping / list ------------------------------------------------------------

void test_ping_returns_empty_object() {
  mcp::McpServer server = make_server();
  json reply = server.handle_message(
      json::parse(R"({"jsonrpc":"2.0","id":3,"method":"ping"})"));
  CHECK(reply["id"] == 3);
  CHECK(reply["result"].is_object());
  CHECK(reply["result"].empty());
}

void test_tools_list_shape() {
  mcp::McpServer server = make_server();
  json reply = server.handle_message(
      json::parse(R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})"));
  CHECK(reply["result"]["tools"].is_array());
  CHECK(reply["result"]["tools"].size() == 3);

  json names;
  for (const auto& t : reply["result"]["tools"]) {
    CHECK(t.contains("name"));
    CHECK(t.contains("inputSchema"));
    CHECK(t["inputSchema"]["type"] == "object");
    names.push_back(t["name"]);
  }
  CHECK(names.size() == 3);
  bool has_echo = false, has_add = false, has_boom = false;
  for (const auto& n : names) {
    has_echo = has_echo || n == "echo";
    has_add = has_add || n == "add";
    has_boom = has_boom || n == "boom";
  }
  CHECK(has_echo && has_add && has_boom);
}

void test_tools_list_explicit_schema_and_description() {
  mcp::McpServer server;
  json schema = {{"type", "object"},
                 {"properties", {{"text", {{"type", "string"}}}}},
                 {"required", json::array({"text"})}};
  server.add_tool(mcp::Tool{.name = "say",
                            .description = "Say something.",
                            .input_schema = schema,
                            .handler = echo_handler});

  json reply = server.handle_message(
      json::parse(R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})"));
  const json& tool = reply["result"]["tools"][0];
  CHECK(tool["description"] == "Say something.");
  CHECK(tool["inputSchema"]["required"][0] == "text");
}

// --- tools/call -------------------------------------------------------------

void test_call_echo_success() {
  mcp::McpServer server = make_server();
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":6,"method":"tools/call",)"
      R"("params":{"name":"echo","arguments":{"text":"hi there"}}})");
  json reply = server.handle_message(msg);
  CHECK(reply["id"] == 6);
  CHECK(reply["result"]["content"][0]["type"] == "text");
  // echo returns {"text": "hi there"} (no "content") -> text-wrapped.
  CHECK(reply["result"]["content"][0]["text"] == R"({"text":"hi there"})");
  CHECK(reply["result"]["isError"] == false);
}

void test_call_add_success() {
  mcp::McpServer server = make_server();
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":7,"method":"tools/call",)"
      R"("params":{"name":"add","arguments":{"a":2,"b":3}}})");
  json reply = server.handle_message(msg);
  CHECK(reply["result"]["content"][0]["text"] == R"({"sum":5})");
}

void test_call_without_arguments_defaults_to_empty_object() {
  mcp::McpServer server = make_server();
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"echo"}})");
  json reply = server.handle_message(msg);
  CHECK(reply["result"]["content"][0]["text"] == R"({"text":""})");
}

void test_call_unknown_tool() {
  mcp::McpServer server = make_server();
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":9,"method":"tools/call",)"
      R"("params":{"name":"nope"}})");
  json reply = server.handle_message(msg);
  CHECK(reply["error"]["code"] == mcp::kToolNotFound);
  CHECK(reply["id"] == 9);
}

void test_call_missing_name() {
  mcp::McpServer server = make_server();
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{}})");
  json reply = server.handle_message(msg);
  CHECK(reply["error"]["code"] == kInvalidParams);
}

void test_call_arguments_not_object() {
  mcp::McpServer server = make_server();
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":11,"method":"tools/call",)"
      R"("params":{"name":"echo","arguments":"oops"}})");
  json reply = server.handle_message(msg);
  CHECK(reply["error"]["code"] == kInvalidParams);
}

void test_call_params_not_object() {
  mcp::McpServer server = make_server();
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":[]})");
  json reply = server.handle_message(msg);
  CHECK(reply["error"]["code"] == kInvalidParams);
}

void test_call_handler_invalid_argument() {
  mcp::McpServer server = make_server();
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":13,"method":"tools/call",)"
      R"("params":{"name":"add","arguments":{"a":"x","b":1}}})");
  json reply = server.handle_message(msg);
  CHECK(reply["error"]["code"] == kInvalidParams);
}

void test_call_handler_other_exception() {
  mcp::McpServer server = make_server();
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":14,"method":"tools/call",)"
      R"("params":{"name":"boom"}})");
  json reply = server.handle_message(msg);
  CHECK(reply["error"]["code"] == kInternalError);
}

void test_call_handler_result_with_content_passthrough() {
  mcp::McpServer server;
  server.add_tool(mcp::Tool{.name = "raw", .input_schema = json::object(),
                            .handler = [](const json&) {
                              json content_item = {{"type", "text"},
                                                   {"text", "raw"}};
                              return json{{"content", json::array({content_item})},
                                          {"isError", true}};
                            }});
  json msg = json::parse(
      R"({"jsonrpc":"2.0","id":15,"method":"tools/call",)"
      R"("params":{"name":"raw"}})");
  json reply = server.handle_message(msg);
  CHECK(reply["result"]["content"][0]["text"] == "raw");
  CHECK(reply["result"]["isError"] == true);
}

// --- misc dispatch ----------------------------------------------------------

void test_unknown_method() {
  mcp::McpServer server = make_server();
  json msg = json::parse(R"({"jsonrpc":"2.0","id":16,"method":"bogus"})");
  json reply = server.handle_message(msg);
  CHECK(reply["error"]["code"] == kMethodNotFound);
  CHECK(reply["id"] == 16);
}

void test_initialized_notification_sets_state() {
  mcp::McpServer server = make_server();
  CHECK(!server.initialized());
  json notif = json::parse(
      R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  json reply = server.handle_message(notif);
  CHECK(reply.is_null());  // nothing to send back
  CHECK(server.initialized());
}

void test_unknown_notification_ignored() {
  mcp::McpServer server = make_server();
  json notif = json::parse(R"({"jsonrpc":"2.0","method":"notifications/x"})");
  json reply = server.handle_message(notif);
  CHECK(reply.is_null());
}

void test_non_object_message() {
  mcp::McpServer server = make_server();
  json reply = server.handle_message(json::array({1, 2}));
  CHECK(reply["error"]["code"] == kInvalidRequest);
}

void test_add_tool_replaces_same_name() {
  mcp::McpServer server;
  server.add_tool(mcp::Tool{.name = "echo",
                            .description = "v1",
                            .input_schema = json::object(),
                            .handler = echo_handler});
  server.add_tool(mcp::Tool{.name = "echo",
                            .description = "v2",
                            .input_schema = json::object(),
                            .handler = echo_handler});
  CHECK(server.tools().size() == 1);
  CHECK(server.tools()[0].description == "v2");
}

}  // namespace

int main() {
  test_initialize_handshake_fields();
  test_initialize_honors_server_options();
  test_ping_returns_empty_object();
  test_tools_list_shape();
  test_tools_list_explicit_schema_and_description();
  test_call_echo_success();
  test_call_add_success();
  test_call_without_arguments_defaults_to_empty_object();
  test_call_unknown_tool();
  test_call_missing_name();
  test_call_arguments_not_object();
  test_call_params_not_object();
  test_call_handler_invalid_argument();
  test_call_handler_other_exception();
  test_call_handler_result_with_content_passthrough();
  test_unknown_method();
  test_initialized_notification_sets_state();
  test_unknown_notification_ignored();
  test_non_object_message();
  test_add_tool_replaces_same_name();

  if (g_failures == 0) {
    std::cout << "server_test: all tests passed\n";
    return 0;
  }
  std::cerr << "server_test: " << g_failures << " check(s) failed\n";
  return 1;
}
