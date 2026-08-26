// mcp-cpp: JSON-RPC 2.0 message layer.
//
// Transport-agnostic by design: both stdio and Streamable HTTP transports
// convert wire bytes into jsonrpc::ParsedMessage and back.
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace mcp::jsonrpc {

// --- JSON-RPC 2.0 error codes (spec §5.1) ---------------------------------
inline constexpr int kParseError = -32700;      // Invalid JSON was received by the server.
inline constexpr int kInvalidRequest = -32600;  // The JSON sent is not a valid Request object.
inline constexpr int kMethodNotFound = -32601;  // The method does not exist / is not available.
inline constexpr int kInvalidParams = -32602;   // Invalid method parameter(s).
inline constexpr int kInternalError = -32603;   // Internal JSON-RPC error.

// Error object: { code, message, data? }
struct Error {
  int code = kInternalError;
  std::string message;
  nlohmann::json data = nlohmann::json();  // null when absent
  nlohmann::json to_json() const;
};

// Request object: { jsonrpc, id, method, params? }
struct Request {
  nlohmann::json id = nullptr;            // string | number | null
  std::string method;
  nlohmann::json params = nlohmann::json();  // object | array; null when absent
  bool has_params = false;
  nlohmann::json to_json() const;
};

// Notification object: { jsonrpc, method, params? }  (no id -> no response)
struct Notification {
  std::string method;
  nlohmann::json params = nlohmann::json();
  bool has_params = false;
  nlohmann::json to_json() const;
};

// Response object: { jsonrpc, id, result } or { jsonrpc, id, error }
struct Response {
  nlohmann::json id = nullptr;
  bool has_result = false;
  nlohmann::json result = nlohmann::json();
  bool has_error = false;
  Error error;
  nlohmann::json to_json() const;
};

// Result of decoding one wire message (one stdio line / one HTTP body).
struct ParsedMessage {
  enum class Kind { kRequest, kNotification, kResponse, kParseError, kInvalidRequest };
  Kind kind = Kind::kInvalidRequest;
  Request request;
  Notification notification;
  Response response;
  Error error;                     // populated for kParseError / kInvalidRequest
  nlohmann::json error_id = nullptr;  // id to echo back in error responses
};

// Parse a raw JSON-RPC message. Never throws.
ParsedMessage parse(const std::string& raw);

// Convenience factories for building response JSON.
nlohmann::json make_result_response(const nlohmann::json& id, nlohmann::json result);
nlohmann::json make_error_response(const nlohmann::json& id, int code,
                                   const std::string& message,
                                   nlohmann::json data = nlohmann::json());

}  // namespace mcp::jsonrpc
