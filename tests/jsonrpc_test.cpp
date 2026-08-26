// mcp-cpp: unit tests for the JSON-RPC 2.0 message layer (mcp/jsonrpc.hpp).
#include <mcp/jsonrpc.hpp>

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

using mcp::jsonrpc::ParsedMessage;
using mcp::jsonrpc::parse;
using mcp::jsonrpc::make_error_response;
using mcp::jsonrpc::make_result_response;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL (line " << line << "): " << what << '\n';
  }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// --- parse: requests --------------------------------------------------------

void test_request_string_id() {
  auto pm = parse(R"({"jsonrpc":"2.0","id":"abc","method":"tools/list"})");
  CHECK(pm.kind == ParsedMessage::Kind::kRequest);
  CHECK(pm.request.id == "abc");
  CHECK(pm.request.method == "tools/list");
  CHECK(!pm.request.has_params);
}

void test_request_number_id_with_params() {
  auto pm = parse(
      R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"x"}})");
  CHECK(pm.kind == ParsedMessage::Kind::kRequest);
  CHECK(pm.request.id == 7);
  CHECK(pm.request.method == "tools/call");
  CHECK(pm.request.has_params);
  CHECK(pm.request.params["name"] == "x");
}

void test_request_null_id() {
  auto pm = parse(R"({"jsonrpc":"2.0","id":null,"method":"ping"})");
  CHECK(pm.kind == ParsedMessage::Kind::kRequest);
  CHECK(pm.request.id.is_null());
}

void test_request_invalid_id_is_invalid_request() {
  auto pm = parse(R"({"jsonrpc":"2.0","id":{"oops":1},"method":"ping"})");
  CHECK(pm.kind == ParsedMessage::Kind::kInvalidRequest);
  CHECK(pm.error.code == mcp::jsonrpc::kInvalidRequest);
}

// --- parse: notifications ---------------------------------------------------

void test_notification() {
  auto pm = parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  CHECK(pm.kind == ParsedMessage::Kind::kNotification);
  CHECK(pm.notification.method == "notifications/initialized");
  CHECK(!pm.notification.has_params);
}

void test_notification_with_params() {
  auto pm = parse(R"({"jsonrpc":"2.0","method":"m","params":[1,2]})");
  CHECK(pm.kind == ParsedMessage::Kind::kNotification);
  CHECK(pm.notification.has_params);
  CHECK(pm.notification.params.is_array());
  CHECK(pm.notification.params.size() == 2);
}

// --- parse: responses -------------------------------------------------------

void test_response_with_result() {
  auto pm = parse(R"({"jsonrpc":"2.0","id":3,"result":{"ok":true}})");
  CHECK(pm.kind == ParsedMessage::Kind::kResponse);
  CHECK(pm.response.id == 3);
  CHECK(pm.response.has_result);
  CHECK(!pm.response.has_error);
  CHECK(pm.response.result["ok"] == true);
}

void test_response_with_error() {
  auto pm = parse(
      R"({"jsonrpc":"2.0","id":9,"error":{"code":-32005,"message":"nope","data":{"x":1}}})");
  CHECK(pm.kind == ParsedMessage::Kind::kResponse);
  CHECK(pm.response.id == 9);
  CHECK(!pm.response.has_result);
  CHECK(pm.response.has_error);
  CHECK(pm.response.error.code == -32005);
  CHECK(pm.response.error.message == "nope");
  CHECK(pm.response.error.data["x"] == 1);
}

// --- parse: errors ----------------------------------------------------------

void test_bad_json_is_parse_error() {
  auto pm = parse("this is not json");
  CHECK(pm.kind == ParsedMessage::Kind::kParseError);
  CHECK(pm.error.code == mcp::jsonrpc::kParseError);
  CHECK(pm.error_id.is_null());
}

void test_empty_input_is_parse_error() {
  auto pm = parse("");
  CHECK(pm.kind == ParsedMessage::Kind::kParseError);
}

void test_non_object_is_invalid_request() {
  for (const auto& raw : {"[1,2,3]", "\"hello\"", "42", "null", "true"}) {
    auto pm = parse(raw);
    CHECK(pm.kind == ParsedMessage::Kind::kInvalidRequest);
    CHECK(pm.error.code == mcp::jsonrpc::kInvalidRequest);
  }
}

void test_object_without_method_or_response_shape() {
  auto pm = parse(R"({"jsonrpc":"2.0"})");
  CHECK(pm.kind == ParsedMessage::Kind::kInvalidRequest);
}

void test_invalid_request_echoes_id() {
  auto pm = parse(R"({"jsonrpc":"2.0","id":"k","foo":1})");
  CHECK(pm.kind == ParsedMessage::Kind::kInvalidRequest);
  CHECK(pm.error_id == "k");
}

// --- round-trip -------------------------------------------------------------

void test_request_round_trip() {
  mcp::jsonrpc::Request req;
  req.id = "r1";
  req.method = "tools/call";
  req.params = {{"name", "echo"}, {"arguments", {{"text", "hi"}}}};
  req.has_params = true;

  auto pm = parse(req.to_json().dump());
  CHECK(pm.kind == ParsedMessage::Kind::kRequest);
  CHECK(pm.request.id == "r1");
  CHECK(pm.request.method == "tools/call");
  CHECK(pm.request.params["name"] == "echo");
  CHECK(pm.request.params["arguments"]["text"] == "hi");
}

void test_notification_round_trip() {
  mcp::jsonrpc::Notification notif;
  notif.method = "notifications/initialized";

  auto pm = parse(notif.to_json().dump());
  CHECK(pm.kind == ParsedMessage::Kind::kNotification);
  CHECK(pm.notification.method == "notifications/initialized");
}

void test_error_to_json() {
  mcp::jsonrpc::Error err;
  err.code = -32602;
  err.message = "bad params";
  err.data = {{"detail", "a must be a number"}};

  nlohmann::json j = err.to_json();
  CHECK(j["code"] == -32602);
  CHECK(j["message"] == "bad params");
  CHECK(j["data"]["detail"] == "a must be a number");
}

void test_error_to_json_omits_null_data() {
  mcp::jsonrpc::Error err;
  err.code = -32601;
  err.message = "nope";

  nlohmann::json j = err.to_json();
  CHECK(j["code"] == -32601);
  CHECK(!j.contains("data"));
}

// --- factories --------------------------------------------------------------

void test_make_result_response() {
  nlohmann::json j = make_result_response(1, {{"ok", true}});
  CHECK(j["jsonrpc"] == "2.0");
  CHECK(j["id"] == 1);
  CHECK(j["result"]["ok"] == true);
  CHECK(!j.contains("error"));
}

void test_make_error_response() {
  nlohmann::json j = make_error_response("id2", -32700, "Parse error");
  CHECK(j["jsonrpc"] == "2.0");
  CHECK(j["id"] == "id2");
  CHECK(j["error"]["code"] == -32700);
  CHECK(j["error"]["message"] == "Parse error");
  CHECK(!j["error"].contains("data"));
}

void test_make_error_response_with_data() {
  nlohmann::json j =
      make_error_response(nullptr, -32602, "Invalid params", {{"n", 3}});
  CHECK(j["id"].is_null());
  CHECK(j["error"]["data"]["n"] == 3);
}

}  // namespace

int main() {
  test_request_string_id();
  test_request_number_id_with_params();
  test_request_null_id();
  test_request_invalid_id_is_invalid_request();
  test_notification();
  test_notification_with_params();
  test_response_with_result();
  test_response_with_error();
  test_bad_json_is_parse_error();
  test_empty_input_is_parse_error();
  test_non_object_is_invalid_request();
  test_object_without_method_or_response_shape();
  test_invalid_request_echoes_id();
  test_request_round_trip();
  test_notification_round_trip();
  test_error_to_json();
  test_error_to_json_omits_null_data();
  test_make_result_response();
  test_make_error_response();
  test_make_error_response_with_data();

  if (g_failures == 0) {
    std::cout << "jsonrpc_test: all tests passed\n";
    return 0;
  }
  std::cerr << "jsonrpc_test: " << g_failures << " check(s) failed\n";
  return 1;
}
