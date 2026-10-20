// mcp-cpp: Streamable HTTP transport implementation (sogou/workflow).
#include <mcp/protocol.hpp>
#include <mcp/transport_http.hpp>

#include <workflow/HttpUtil.h>
#include <workflow/WFHttpServer.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
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
  // Listen address and port; fixed at run() time.
  std::string host;
  uint16_t port = 0;

  // Message handler (set before run(); immutable afterwards). Owned here so the
  // workflow callback never touches the (possibly destroyed) transport.
  MessageHandler handler;

  // Dispatch probe (optional). Reports how a named tool's handler must run;
  // immutable after run(), read by the process callback.
  StreamableHttpTransport::DispatchProbe dispatch_probe;

  // Guard for sessions and stop.
  std::mutex mutex;
  // session id -> last-seen time point (steady_clock). Sessions expire when
  // idle too long, bounding the set so a malicious client cannot grow it
  // unboundedly (resource-exhaustion guard).
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> sessions;
  std::atomic<bool> stop_requested{false};
  // Set by the first run(); run() is single-shot (second call returns 1).
  std::atomic<bool> started{false};

  // Session idle timeout and sweep cadence.
  static constexpr std::chrono::seconds kSessionTimeout{60 * 30};  // 30 min
  static constexpr std::chrono::seconds kSweepInterval{60};        // every 1 min
  // Reaper wakes every tick to honour stop() promptly (join latency <= tick).
  static constexpr std::chrono::seconds kReaperTick{1};

  // The process callback invoked by workflow for every HTTP request.
  void on_http_request(WFHttpTask* task);

  // Run the handler for one decoded message and write the HTTP response. Used
  // for kInline (process thread) and kThread (compute thread); the handler must
  // finish inside this call. Must not be called twice for the same task.
  void run_handler_and_reply(WFHttpTask* task, const nlohmann::json& message,
                             bool is_initialize, const std::string& sid);

  // Dispatch a tool/call to a workflow compute thread via a go task, so a
  // blocking handler holds at most one worker thread instead of a transport
  // one. The go task runs run_handler_and_reply() and writes the response there.
  void dispatch_async(WFHttpTask* task, const nlohmann::json& message,
                      bool is_initialize, const std::string& sid);

  // Dispatch a tool/call that answers later (kDeferred): the request is parked
  // on a counter gate, so it holds no thread while the tool waits for whatever
  // it is waiting for. The tool calls Done (from any thread) to finish it.
  void dispatch_deferred(WFHttpTask* task, const nlohmann::json& message,
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

// Write the HTTP response for one call. A null reply means the message needs no
// answer (a notification): 202 Accepted with an empty body.
void write_reply(WFHttpTask* task, const nlohmann::json& reply,
                 bool is_initialize, const std::string& sid) {
  HttpResponse* resp = task->get_resp();
  if (reply.is_null()) {
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

// One call whose reply arrives later (kDeferred). The HTTP task stays parked on
// a counter gate until Done is called -- from any thread, possibly long after
// the handler returned -- which costs no thread while waiting.
class DeferredCall : public std::enable_shared_from_this<DeferredCall> {
 public:
  DeferredCall(WFHttpTask* task, bool is_initialize, std::string sid)
      : task_(task), is_initialize_(is_initialize), sid_(std::move(sid)) {}

  // Keep the series -- and with it the connection -- alive. Called from the
  // process callback, before the handler can possibly reply.
  void park(SeriesWork* series) {
    gate_ = WFTaskFactory::create_counter_task(
        1, [self = shared_from_this()](WFCounterTask*) { self->finalize(); });
    series->push_back(gate_);
  }

  Done done_fn() {
    return [self = shared_from_this()](nlohmann::json reply) {
      self->finish(std::move(reply));
    };
  }

 private:
  void finish(nlohmann::json reply) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (replied_) {
        return;  // Done is idempotent: only the first call counts
      }
      replied_ = true;
      reply_ = std::move(reply);
    }
    gate_->count();  // the gate callback writes the response
  }

  void finalize() { write_reply(task_, reply_, is_initialize_, sid_); }

  WFHttpTask* task_;
  bool is_initialize_;
  std::string sid_;
  WFCounterTask* gate_ = nullptr;
  std::mutex mutex_;
  bool replied_ = false;
  nlohmann::json reply_;
};

}  // namespace

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
  // The handler answers within this call, so write the response right here. A
  // completion arriving later (a tool that ignores its Dispatch) is dropped.
  auto replied = std::make_shared<std::atomic<bool>>(false);
  Done done = [task, is_initialize, sid, replied](nlohmann::json reply) {
    if (replied->exchange(true)) {
      return;
    }
    write_reply(task, reply, is_initialize, sid);
  };
  if (handler) {
    handler(message, std::move(done));
  } else {
    done(nullptr);
  }
}

void StreamableHttpTransport::Impl::dispatch_deferred(
    WFHttpTask* task, const nlohmann::json& message, bool is_initialize,
    const std::string& sid) {
  // Park the series first: the handler may call Done from inside this call,
  // from another thread, or much later -- all three are fine, and the response
  // is written once the gate completes.
  auto call = std::make_shared<DeferredCall>(task, is_initialize, sid);
  call->park(series_of(task));
  Done done = call->done_fn();
  if (handler) {
    handler(message, std::move(done));
  } else {
    done(nullptr);
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

  // Dispatch probe: only tool/call is routed by execution model. The probe is
  // cheap and runs on the process thread; session bookkeeping and protocol
  // routing stay synchronous so their result is known before the callback
  // returns.
  Dispatch dispatch = Dispatch::kInline;
  if (method_str == kMethodToolsCall && dispatch_probe) {
    const auto& p = dispatch_message["params"];
    if (p.is_object() && p.contains("name") && p["name"].is_string()) {
      dispatch = dispatch_probe(p["name"].get<std::string>());
    }
  }

  // kDeferred parks the request and replies when the tool is done (no thread
  // held while it waits); kThread moves the handler to a worker thread; kInline
  // answers right here on the process thread.
  auto dispatch_call = [&](bool is_init, const std::string& call_sid) {
    if (dispatch == Dispatch::kDeferred) {
      dispatch_deferred(task, dispatch_message, is_init, call_sid);
    } else if (dispatch == Dispatch::kThread) {
      dispatch_async(task, dispatch_message, is_init, call_sid);
    } else {
      run_handler_and_reply(task, dispatch_message, is_init, call_sid);
    }
  };

  if (is_v2) {
    // Stateless: no session required, no handshake.
    dispatch_call(/*is_init=*/false, "");
    return;
  }

  if (is_initialize) {
    // Assign a new session and hand it back in the response header.
    std::string sid = generate_session_id();
    {
      std::lock_guard<std::mutex> lock(mutex);
      sessions.emplace(sid, std::chrono::steady_clock::now());
    }
    dispatch_call(/*is_init=*/true, sid);
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

  dispatch_call(/*is_init=*/false, sid);
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

void StreamableHttpTransport::set_dispatch_probe(DispatchProbe probe) {
  impl_->dispatch_probe = std::move(probe);
}

int StreamableHttpTransport::run(uint16_t port) {
  return run("127.0.0.1", port);
}

int StreamableHttpTransport::run(const std::string& host, uint16_t port) {
  // run() is single-shot: rejecting re-entry keeps a second listener from being
  // started on this Impl.
  if (impl_->started.exchange(true)) {
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
  // Owned here, not by Impl: as an Impl member the server's copy of the process
  // lambda would keep Impl alive and neither would ever be freed (cycle).
  WFHttpServer server(std::move(process));
  // Bind the caller-supplied host (default run(port) -> 127.0.0.1, loopback
  // only). Pass "0.0.0.0" to accept remote connections.
  if (server.start(host.c_str(), port) != 0) {
    std::fprintf(stderr,
                 "StreamableHttpTransport: failed to listen on %s:%u\n",
                 host.c_str(), static_cast<unsigned>(port));
    return 1;
  }

  // Background reaper: bound the session set by sweeping idle sessions. Wakes
  // every tick so stop() is honoured within ~1s; sweeps when kSweepInterval has
  // elapsed (time-based, so it does not depend on tick/sweep divisibility).
  // Owned and joined here for the same reason as server: it captures Impl, so
  // leaving it running past run() would keep Impl alive.
  std::thread reaper([impl = impl_] {
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
  server.stop();
  reaper.join();
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
