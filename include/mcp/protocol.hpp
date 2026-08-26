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

// --- MCP-specific error codes (JSON-RPC codes live in jsonrpc.hpp) ---------
inline constexpr int kToolNotFound = -32005;

}  // namespace mcp
