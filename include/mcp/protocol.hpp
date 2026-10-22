// mcp-cpp: MCP protocol constants (method names, error codes, versions).
#pragma once

#include <string>

namespace mcp {

// Protocol version advertised by the server in the v1 `initialize` response
// (v2 uses kProtocolVersionModern). Verified against the official Python SDK
// during interop testing.
inline constexpr const char* kProtocolVersion = "2025-06-18";

inline constexpr const char* kServerName = "mcp-cpp";
inline constexpr const char* kServerVersion = "0.1.0";

// --- MCP method names ------------------------------------------------------
inline constexpr const char* kMethodInitialize = "initialize";
inline constexpr const char* kMethodInitialized = "notifications/initialized";
inline constexpr const char* kMethodPing = "ping";
inline constexpr const char* kMethodToolsList = "tools/list";
inline constexpr const char* kMethodToolsCall = "tools/call";
// 2026-07-28 stateless discovery (no handshake/session). Returns the same
// protocolVersion/capabilities/serverInfo shape as initialize.
inline constexpr const char* kMethodServerDiscover = "server/discover";
// 2026-07-28 modern protocol version (stateless, no Mcp-Session-Id).
inline constexpr const char* kProtocolVersionModern = "2026-07-28";
// Key inside params._meta used by modern clients to declare the protocol
// version in the request envelope.
inline constexpr const char* kMetaProtocolVersionKey =
    "io.modelcontextprotocol/protocolVersion";

// --- Project-defined error codes -------------------------------------------
// JSON-RPC reserves -32000..-32099 for implementation-defined server errors, so
// this code is protocol-legal. MCP itself defines no "tool not found" code; the
// official tools spec example answers an unknown tool with -32602 instead. We
// keep a distinct code so clients can tell a typo'd tool name from bad params.
inline constexpr int kToolNotFound = -32005;

// --- Codes defined by MCP 2026-07-28 (per-request envelope era) -------------
// A modern request must mirror its envelope metadata into the HTTP headers; a
// mismatch (or a missing routing header) is -32020, an unserved revision is
// -32022, and a capability the request needs but did not declare is -32021.
inline constexpr int kHeaderMismatch = -32020;
inline constexpr int kMissingRequiredClientCapability = -32021;
inline constexpr int kUnsupportedProtocolVersion = -32022;

// Required _meta key holding the client's capabilities, next to
// kMetaProtocolVersionKey, on every 2026-07-28 request.
inline constexpr const char* kMetaClientCapabilitiesKey =
    "io.modelcontextprotocol/clientCapabilities";

}  // namespace mcp
