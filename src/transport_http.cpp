// mcp-cpp: Streamable HTTP transport implementation (sogou/workflow).
#include <mcp/protocol.hpp>
#include <mcp/transport_http.hpp>

#include <workflow/HttpUtil.h>
#include <workflow/WFHttpServer.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace mcp {

namespace {

// MCP Streamable HTTP header names.
constexpr const char* kSessionHeader = "Mcp-Session-Id";
constexpr const char* kProtocolVersionHeader = "MCP-Protocol-Version";

// Mime types.
constexpr const char* kContentTypeJson = "application/json";
constexpr const char* kContentTypeSse = "text/event-stream";

// Base MCP endpoint path; the SDK connects to <scheme>://host:port/mcp.
constexpr const char* kMcpPath = "/mcp";

// Generate a random session id: 32 lowercase hex chars (128 bits). Session ids
// must be "visible ASCII"; hex is safe and collision-free for our purposes.
std::string generate_session_id() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uint64_t hi = rng();
  std::uint64_t lo = rng();
  char buf[33];
  std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                static_cast<unsigned long long>(hi),
                static_cast<unsigned long long>(lo));
  return std::string(buf);
}

}  // namespace

struct StreamableHttpTransport::Impl
    : std::enable_shared_from_this<StreamableHttpTransport::Impl> {
  // Workflow HTTP server. Created in run(); never moved.
  std::unique_ptr<WFHttpServer> server;

  // Listen address and port; fixed at run() time.
  std::string host;
  uint16_t port = 0;

  // Message handler (set before run(); immutable afterwards). Owned here so the
  // workflow callback never touches the (possibly destroyed) transport.
  MessageHandler handler;

  // Async probe (optional). Reports whether a named tool's handler should run
  // off the transport thread. Immutable after run(); read by the process
  // callback to decide whether a tool/call may be dispatched asynchronously.
  StreamableHttpTransport::AsyncProbe async_probe;

  // Guard for sessions and stop.
  std::mutex mutex;
  // session id -> last-seen time point (steady_clock). Sessions expire when
  // idle too long, bounding the set so a malicious client cannot grow it
  // unboundedly (resource-exhaustion guard).
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> sessions;
  std::atomic<bool> stop_requested{false};
  // Reaper thread id; joined in stop(). Idle-session sweep.
  std::thread reaper;

  // Session idle timeout and sweep cadence.
  static constexpr std::chrono::seconds kSessionTimeout{60 * 30};  // 30 min
  static constexpr std::chrono::seconds kSweepInterval{60};        // every 1 min
  // Reaper wakes every tick to honour stop() promptly (join latency <= tick).
  static constexpr std::chrono::seconds kReaperTick{1};

  ~Impl();

  // The process callback invoked by workflow for every HTTP request.
  void on_http_request(WFHttpTask* task);

  // Run the handler for one decoded message and write the HTTP response. Shared
  // by the synchronous path (called on the process thread) and the async path
  // (called inside a go task's compute-thread function). Must not be called
  // twice for the same task.
  void run_handler_and_reply(WFHttpTask* task, const nlohmann::json& message,
                             bool is_initialize, const std::string& sid);

  // Dispatch a tool/call to a workflow compute thread via a go task. The go
  // task runs run_handler_and_reply() off the transport thread; the process
  // callback returns immediately. Requires async_probe to be set.
  void dispatch_async(WFHttpTask* task, const nlohmann::json& message,
                      bool is_initialize, const std::string& sid);

  void sweep_expired_sessions();
};

// --------------------------------------------------------------------------
// HTTP request dispatch
// --------------------------------------------------------------------------

namespace {

using protocol::HttpRequest;
using protocol::HttpResponse;

bool get_header(HttpRequest* req, const char* name, std::string* out) {
  protocol::HttpHeaderCursor cursor(req);
  std::string key, value;
  while (cursor.next(key, value)) {
    // Header names are case-insensitive.
    if (key.size() == std::strlen(name) &&
        strncasecmp(key.c_str(), name, key.size()) == 0) {
      *out = value;
      return true;
    }
  }
  return false;
}

// Read the request body as a string. If the body was not fully parsed (returns
// false), treat it as empty rather than reading garbage.
std::string read_body(HttpRequest* req) {
  const void* body = nullptr;
  size_t size = 0;
  if (req->get_parsed_body(&body, &size)) {
    return std::string(static_cast<const char*>(body), size);
  }
  return std::string();
}

void set_content_type(HttpResponse* resp, const char* type) {
  resp->add_header_pair("Content-Type", type);
}

// Write a plain JSON HTTP response with the given status code.
void respond_json(HttpResponse* resp, int status, const nlohmann::json& payload) {
  resp->set_status_code(std::to_string(status).c_str());
  set_content_type(resp, kContentTypeJson);
  const std::string body = payload.dump();
  resp->append_output_body(body.c_str(), body.size());
}

}  // namespace

StreamableHttpTransport::Impl::~Impl() {
  if (reaper.joinable()) {
    reaper.join();
  }
}

void StreamableHttpTransport::Impl::sweep_expired_sessions() {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex);
  for (auto it = sessions.begin(); it != sessions.end();) {
    if (now - it->second > kSessionTimeout) {
      it = sessions.erase(it);
    } else {
      ++it;
    }
  }
}

void StreamableHttpTransport::Impl::run_handler_and_reply(
    WFHttpTask* task, const nlohmann::json& message, bool is_initialize,
    const std::string& sid) {
  HttpResponse* resp = task->get_resp();
  const nlohmann::json reply = handler ? handler(message)
                                       : nlohmann::json(nullptr);
  if (reply.is_null()) {
    // Notification (no id): 202 Accepted, no body.
    resp->set_status_code("202");
    resp->set_reason_phrase("Accepted");
  } else {
    respond_json(resp, 200, reply);
  }
  if (is_initialize) {
    // Hand the freshly assigned session back in the response header.
    resp->add_header_pair(kSessionHeader, sid.c_str());
  }
}

void StreamableHttpTransport::Impl::dispatch_async(
    WFHttpTask* task, const nlohmann::json& message, bool is_initialize,
    const std::string& sid) {
  // Move the handler execution off the transport thread. The go task runs
  // run_handler_and_reply() on a workflow compute thread, then writes the
  // response there. Capture a shared_ptr<Impl> so the handler/sessions outlive
  // a concurrently destroyed transport. The WFHttpTask* stays valid because
  // the go task is pushed onto the same series as the HTTP task: the response
  // is written (and the HTTP task sent) only after the go task completes.
  std::shared_ptr<Impl> impl = shared_from_this();
  WFGoTask* go = WFTaskFactory::create_go_task(
      "mcp", [impl, task, message, is_initialize, sid]() {
        impl->run_handler_and_reply(task, message, is_initialize, sid);
      });
  series_of(task)->push_back(go);
}

void StreamableHttpTransport::Impl::on_http_request(WFHttpTask* task) {
  HttpRequest* req = task->get_req();
  HttpResponse* resp = task->get_resp();

  // Only serve the single MCP endpoint.
  const std::string uri = req->get_request_uri();
  if (uri != kMcpPath) {
    respond_json(resp, 404, nlohmann::json{{"error", "not found"}});
    return;
  }

  const std::string method = req->get_method();

  if (method == "GET") {
    // SSE stream: we only send an opening keep-alive frame in single-shot mode.
    resp->set_status_code("200");
    set_content_type(resp, kContentTypeSse);
    resp->add_header_pair("Cache-Control", "no-cache");
    const std::string frame = ": keep-alive\n\n";
    resp->append_output_body(frame.c_str(), frame.size());
    return;
  }

  if (method == "DELETE") {
    std::string sid;
    get_header(req, kSessionHeader, &sid);
    {
      std::lock_guard<std::mutex> lock(mutex);
      sessions.erase(sid);
    }
    respond_json(resp, 200, nlohmann::json::object());
    return;
  }

  if (method != "POST") {
    respond_json(resp, 405, nlohmann::json{{"error", "method not allowed"}});
    return;
  }

  // ---- POST: one JSON-RPC message per request ----------------------------
  const std::string raw = read_body(req);
  const jsonrpc::ParsedMessage pm = jsonrpc::parse(raw);

  // A request/notification must carry a "method".
  if (pm.kind != jsonrpc::ParsedMessage::Kind::kRequest &&
      pm.kind != jsonrpc::ParsedMessage::Kind::kNotification) {
    // Parse or invalid request: reply with the JSON-RPC error (id null / echoed).
    respond_json(resp, 200,
                 jsonrpc::make_error_response(pm.error_id, pm.error.code,
                                              pm.error.message));
    return;
  }

  const bool is_initialize = (pm.kind == jsonrpc::ParsedMessage::Kind::kRequest &&
                              pm.request.method == kMethodInitialize);

  const nlohmann::json message =
      (pm.kind == jsonrpc::ParsedMessage::Kind::kRequest)
          ? pm.request.to_json()
          : pm.notification.to_json();

  // Modern (2026-07-28) stateless requests carry MCP-Protocol-Version:
  // 2026-07-28 and/or a _meta.protocolVersion envelope. server/discover is also
  // inherently modern. These have no session and no initialize handshake; the
  // transport passes the JSON-RPC method straight to the core (which now
  // handles server/discover too). The core ignores _meta.
  std::string proto_version_header;
  const bool has_proto_header = get_header(req, kProtocolVersionHeader,
                                           &proto_version_header);
  const std::string method_str =
      (pm.kind == jsonrpc::ParsedMessage::Kind::kRequest)
          ? pm.request.method
          : pm.notification.method;
  bool is_v2 = has_proto_header &&
               proto_version_header == kProtocolVersionModern;
  if (!is_v2 && pm.kind == jsonrpc::ParsedMessage::Kind::kRequest &&
      pm.request.has_params && message.contains("params") &&
      message["params"].is_object() && message["params"].contains("_meta") &&
      message["params"]["_meta"].is_object()) {
    const auto& meta = message["params"]["_meta"];
    const auto it = meta.find(kMetaProtocolVersionKey);
    if (it != meta.end() && it->is_string() &&
        it->get<std::string>() == kProtocolVersionModern) {
      is_v2 = true;
    }
  }
  // server/discover is always a stateless (v2) request.
  if (!is_v2 && pm.kind == jsonrpc::ParsedMessage::Kind::kRequest &&
      pm.request.method == kMethodServerDiscover) {
    is_v2 = true;
  }

  // Build the exact message to dispatch. For v2, stamp the modern protocol
  // version into the message's _meta envelope so the transport-agnostic core
  // can reliably detect a 2026-07-28 request (it routes off _meta, not the
  // HTTP header). This covers requests that carry the version header but no
  // _meta body envelope.
  nlohmann::json dispatch_message = message;
  if (is_v2) {
    if (!dispatch_message.contains("params") ||
        !dispatch_message["params"].is_object()) {
      dispatch_message["params"] = nlohmann::json::object();
    }
    if (!dispatch_message["params"].contains("_meta") ||
        !dispatch_message["params"]["_meta"].is_object()) {
      dispatch_message["params"]["_meta"] = nlohmann::json::object();
    }
    dispatch_message["params"]["_meta"][kMetaProtocolVersionKey] =
        kProtocolVersionModern;
  }

  // Async probe: a tool/call targeting an async tool may be handed to a
  // workflow compute thread so the (potentially slow) handler does not block
  // the transport event loop. The probe is cheap and runs on the process
  // thread; only the handler execution moves off-thread. Everything else
  // (session bookkeeping, protocol routing) stays synchronous so its result is
  // observed before the process callback returns.
  bool want_async = false;
  if (method_str == kMethodToolsCall && async_probe) {
    const auto& p = dispatch_message["params"];
    if (p.is_object() && p.contains("name") && p["name"].is_string()) {
      want_async = async_probe(p["name"].get<std::string>());
    }
  }

  if (is_v2) {
    // Stateless: no session required, no handshake.
    if (want_async) {
      dispatch_async(task, dispatch_message, /*is_initialize=*/false, "");
      return;
    }
    run_handler_and_reply(task, dispatch_message, /*is_initialize=*/false, "");
    return;
  }

  if (is_initialize) {
    // Assign a new session and hand it back in the response header.
    std::string sid = generate_session_id();
    {
      std::lock_guard<std::mutex> lock(mutex);
      sessions.emplace(sid, std::chrono::steady_clock::now());
    }
    if (want_async) {
      dispatch_async(task, dispatch_message, /*is_initialize=*/true, sid);
      return;
    }
    run_handler_and_reply(task, dispatch_message, /*is_initialize=*/true, sid);
    return;
  }

  // All non-initialize (v1) requests require a valid session.
  std::string sid;
  if (!get_header(req, kSessionHeader, &sid)) {
    respond_json(resp, 400, nlohmann::json{{"error", "missing Mcp-Session-Id"}});
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = sessions.find(sid);
    if (it == sessions.end()) {
      respond_json(resp, 400,
                   nlohmann::json{{"error", "unknown or expired session"}});
      return;
    }
    it->second = std::chrono::steady_clock::now();  // refresh last-seen
  }

  if (want_async) {
    dispatch_async(task, dispatch_message, /*is_initialize=*/false, sid);
    return;
  }
  run_handler_and_reply(task, dispatch_message, /*is_initialize=*/false, sid);
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

StreamableHttpTransport::StreamableHttpTransport()
    : impl_(std::make_shared<Impl>()) {}

StreamableHttpTransport::~StreamableHttpTransport() {
  stop();
}

void StreamableHttpTransport::set_message_handler(MessageHandler handler) {
  impl_->handler = std::move(handler);
}

void StreamableHttpTransport::set_async_probe(AsyncProbe probe) {
  impl_->async_probe = std::move(probe);
}

int StreamableHttpTransport::run(uint16_t port) {
  return run("127.0.0.1", port);
}

int StreamableHttpTransport::run(const std::string& host, uint16_t port) {
  // run() is single-shot: rejecting re-entry avoids a std::terminate if a stale
  // reaper thread were reassigned.
  if (impl_->server) {
    std::fprintf(stderr,
                 "StreamableHttpTransport: run() is not re-entrant\n");
    return 1;
  }

  // The workflow process callback runs on workflow's own threads. Capture a
  // shared_ptr<Impl> so an in-flight callback keeps Impl alive even if this
  // transport is destroyed first (no use-after-free on handler/sessions).
  auto process = [impl = impl_](WFHttpTask* task) {
    impl->on_http_request(task);
  };

  impl_->host = host;
  impl_->port = port;
  impl_->server = std::make_unique<WFHttpServer>(std::move(process));
  // Bind the caller-supplied host (default run(port) -> 127.0.0.1, loopback
  // only). Pass "0.0.0.0" to accept remote connections.
  if (impl_->server->start(host.c_str(), port) != 0) {
    std::fprintf(stderr,
                 "StreamableHttpTransport: failed to listen on %s:%u\n",
                 host.c_str(), static_cast<unsigned>(port));
    return 1;
  }

  // Background reaper: bound the session set by sweeping idle sessions. Wakes
  // every tick so stop() is honoured within ~1s; sweeps when kSweepInterval has
  // elapsed (time-based, so it does not depend on tick/sweep divisibility).
  impl_->reaper = std::thread([impl = impl_] {
    auto deadline = std::chrono::steady_clock::now() + Impl::kSweepInterval;
    while (!impl->stop_requested.load()) {
      std::this_thread::sleep_for(Impl::kReaperTick);
      if (std::chrono::steady_clock::now() >= deadline) {
        impl->sweep_expired_sessions();
        deadline = std::chrono::steady_clock::now() + Impl::kSweepInterval;
      }
    }
  });

  // Emit a readiness line only after the listener is up, so integration tests
  // and tooling can reliably detect "server is accepting connections".
  std::printf("StreamableHttpTransport: listening on %s:%u\n", host.c_str(),
              static_cast<unsigned>(port));
  std::fflush(stdout);

  // workflow runs a background scheduler; block until stop() is requested.
  while (!impl_->stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  impl_->server->stop();
  return 0;
}

void StreamableHttpTransport::stop() {
  impl_->stop_requested.store(true);
}

size_t StreamableHttpTransport::session_count() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->sessions.size();
}

}  // namespace mcp
