// mcp-cpp: JSON-RPC 2.0 message layer implementation.
//
// Contract (see include/mcp/jsonrpc.hpp):
//   * parse() never throws: invalid JSON -> kParseError; anything that is not a
//     well-formed request/notification/response -> kInvalidRequest. For
//     kInvalidRequest the id is echoed back when it can be identified.
//   * to_json() / factories produce spec-shaped objects:
//     { "jsonrpc":"2.0", ... }.
#include <mcp/jsonrpc.hpp>

#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <utility>

namespace mcp::jsonrpc {

namespace {

// A JSON-RPC id must be a string, a number, or null (spec §4.3).
bool is_valid_id(const nlohmann::json& id) {
  return id.is_string() || id.is_number() || id.is_null();
}

Error invalid_request_error() {
  return Error{kInvalidRequest, "Invalid Request"};
}

}  // namespace

// --- Serialization ----------------------------------------------------------

nlohmann::json Error::to_json() const {
  nlohmann::json j = {{"code", code}, {"message", message}};
  if (!data.is_null()) {
    j["data"] = data;
  }
  return j;
}

nlohmann::json Request::to_json() const {
  nlohmann::json j = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
  if (has_params) {
    j["params"] = params;
  }
  return j;
}

nlohmann::json Notification::to_json() const {
  nlohmann::json j = {{"jsonrpc", "2.0"}, {"method", method}};
  if (has_params) {
    j["params"] = params;
  }
  return j;
}

nlohmann::json Response::to_json() const {
  nlohmann::json j = {{"jsonrpc", "2.0"}, {"id", id}};
  if (has_result) {
    j["result"] = result;
  }
  if (has_error) {
    j["error"] = error.to_json();
  }
  return j;
}

// --- Parsing ----------------------------------------------------------------

ParsedMessage parse(const std::string& raw) {
  ParsedMessage pm;

  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(raw);
  } catch (const std::exception&) {
    // Malformed JSON: cannot even identify the request, so the id is null.
    pm.kind = ParsedMessage::Kind::kParseError;
    pm.error = Error{kParseError, "Parse error"};
    return pm;
  }

  if (!doc.is_object()) {
    pm.kind = ParsedMessage::Kind::kInvalidRequest;
    pm.error = invalid_request_error();
    return pm;
  }

  const bool has_method = doc.contains("method") && doc["method"].is_string();
  if (has_method) {
    // Request or notification: notifications carry no id.
    if (doc.contains("id")) {
      const auto& id = doc["id"];
      if (!is_valid_id(id)) {
        pm.kind = ParsedMessage::Kind::kInvalidRequest;
        pm.error = invalid_request_error();
        return pm;
      }
      pm.kind = ParsedMessage::Kind::kRequest;
      pm.request.id = id;
      pm.request.method = doc["method"].get<std::string>();
      if (doc.contains("params")) {
        pm.request.params = doc["params"];
        pm.request.has_params = true;
      }
      return pm;
    }
    pm.kind = ParsedMessage::Kind::kNotification;
    pm.notification.method = doc["method"].get<std::string>();
    if (doc.contains("params")) {
      pm.notification.params = doc["params"];
      pm.notification.has_params = true;
    }
    return pm;
  }

  // No method: only valid as a response (id + result/error). We are a server,
  // so responses are unexpected, but parse them anyway (transport-agnostic).
  if (doc.contains("id") && (doc.contains("result") || doc.contains("error"))) {
    pm.kind = ParsedMessage::Kind::kResponse;
    pm.response.id = doc["id"];
    if (doc.contains("result")) {
      pm.response.has_result = true;
      pm.response.result = doc["result"];
    }
    if (doc.contains("error")) {
      pm.response.has_error = true;
      if (doc["error"].is_object()) {
        const auto& err = doc["error"];
        if (err.contains("code") && err["code"].is_number_integer()) {
          pm.response.error.code = err["code"].get<int>();
        }
        if (err.contains("message") && err["message"].is_string()) {
          pm.response.error.message = err["message"].get<std::string>();
        }
        if (err.contains("data")) {
          pm.response.error.data = err["data"];
        }
      }
    }
    return pm;
  }

  // Neither a method nor a response shape: invalid request. Echo the id when
  // we can identify one so the client can correlate the error.
  pm.kind = ParsedMessage::Kind::kInvalidRequest;
  pm.error = invalid_request_error();
  if (doc.contains("id") && is_valid_id(doc["id"])) {
    pm.error_id = doc["id"];
  }
  return pm;
}

// --- Factories --------------------------------------------------------------

nlohmann::json make_result_response(const nlohmann::json& id,
                                    nlohmann::json result) {
  return nlohmann::json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", std::move(result)}};
}

nlohmann::json make_error_response(const nlohmann::json& id, int code,
                                   const std::string& message,
                                   nlohmann::json data) {
  nlohmann::json error = {{"code", code}, {"message", message}};
  if (!data.is_null()) {
    error["data"] = std::move(data);
  }
  return nlohmann::json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"error", std::move(error)}};
}

}  // namespace mcp::jsonrpc
