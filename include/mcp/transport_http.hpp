// mcp-cpp: Streamable HTTP transport (MCP 2025-06-18 spec) built on
// sogou/workflow.
//
// This transport exposes a single MCP endpoint over HTTP that speaks the
// Streamable HTTP wire protocol:
//
//   POST /mcp   - client sends one JSON-RPC message (request or notification).
//                 Requests (with id) get an application/json reply; a JSON-RPC
//                 error for parse/invalid requests, or a 400 when a required
//                 session id is missing. Notifications (no id) get 202 Accepted
//                 with no body.
//   GET  /mcp   - opens a text/event-stream (SSE) channel for server-initiated
//                 messages and keep-alive. Clients may keep it open; the server
//                 pushes nothing but heartbeats in the single-shot mode.
//   DELETE /mcp - terminates a session (optional per spec).
//
// Sessions: the server assigns a cryptographically random Mcp-Session-Id in the
// initialize response and requires it on all subsequent requests. Missing or
// unknown sessions yield 400 Bad Request. This matches the 2025-06-18 spec and
// the official Python SDK streamable_http_client behaviour (verified by probe).
//
// Unlike the stdio transport (one peer, blocking), this is a multi-client async
// model, so it is a distinct class rather than an ITransport implementation.
// It reuses McpServer::handle_message unchanged for JSON-RPC dispatch.
#pragma once

#include <mcp/dispatch.hpp>
#include <mcp/jsonrpc.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

namespace mcp {

class StreamableHttpTransport {
 public:
  // Handler decodes one JSON-RPC message and calls `done` with the reply to
  // send, or with a null json when nothing should be sent back (notifications).
  // For a request owned by a kDeferred tool, `done` may be called later and
  // from another thread; the connection stays open until then.
  //
  // NOTE: the handler (and the user tool handlers it dispatches to) may be
  // invoked concurrently from multiple workflow threads. Tool handlers must be
  // thread-safe; McpServer is safe to dispatch concurrently (tools_ is read-only
  // after registration, initialized_ is atomic).
  using MessageHandler = std::function<void(const nlohmann::json&, Done)>;

  StreamableHttpTransport();
  ~StreamableHttpTransport();

  StreamableHttpTransport(const StreamableHttpTransport&) = delete;
  StreamableHttpTransport& operator=(const StreamableHttpTransport&) = delete;

  void set_message_handler(MessageHandler handler);

  // Optional: given a tool name, reports how its handler must run. kThread
  // hands the call to a framework worker thread (the process thread returns at
  // once); kDeferred parks the request until the tool replies through Done,
  // holding no thread while it waits. Without a probe (or for kInline) the call
  // is answered synchronously on the transport thread.
  using DispatchProbe = std::function<Dispatch(const std::string& tool_name)>;
  void set_dispatch_probe(DispatchProbe probe);

  // Start the HTTP server on the given port and block until stop() is called.
  // Returns the process exit code (0 on a clean stop). Listens on 127.0.0.1.
  // run() is single-shot and not re-entrant: calling it twice returns 1.
  int run(uint16_t port);

  // Like run(port) but binds the given host/address explicitly, so the server
  // can listen on a specific interface (e.g. "0.0.0.0" to accept remote
  // connections) instead of loopback-only. host is passed to workflow's
  // WFHttpServer::start(host, port); use a numeric address or resolvable name.
  int run(const std::string& host, uint16_t port);

  // Ask the server to stop. Safe to call from another thread; run() returns.
  void stop();

  // Number of live sessions (used by tests).
  size_t session_count() const;

 private:
  // Internal state is shared with the workflow callback via shared_ptr so the
  // callback never dereferences a freed transport (see S1 in review).
  struct Impl;

  std::shared_ptr<Impl> impl_;
};

}  // namespace mcp
