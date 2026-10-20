// mcp-cpp: execution-model regression test for StreamableHttpTransport.
//
// The worker pool is deliberately tiny (one compute thread), so a long call
// that occupies a worker would stall every other call that needs one. Asserts
// that:
//   * kInline is not blocked by a pending kThread call (separate pools), and
//   * kInline is not blocked by a pending kDeferred call, whose reply still
//     arrives with the right content.
//
// Requires MCP_ENABLE_WORKFLOW=ON (sogou/workflow).
#include <mcp/dispatch.hpp>
#include <mcp/server.hpp>
#include <mcp/transport_http.hpp>

#include <workflow/WFGlobal.h>

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint16_t kPort = 8957;
constexpr int kHoldMs = 2000;
// A blocked kInline call would take kHoldMs; a healthy one takes ~1ms.
constexpr double kFastLimitMs = 300.0;

int g_failures = 0;

void check(bool condition, const char* what, int line) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL (line " << line << "): " << what << '\n';
  }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

struct Reply {
  double ms = 0.0;
  int status = 0;
  std::string body;
};

// One stateless (2026-07-28) POST /mcp: the protocol version header means no
// session is required, which keeps the test free of handshake bookkeeping.
Reply post(const std::string& payload, int timeout_s = 30) {
  Reply reply;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return reply;
  }
  timeval tv{};
  tv.tv_sec = timeout_s;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  const auto t0 = Clock::now();
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    const std::string req =
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Type: application/json\r\nAccept: application/json\r\n"
        "MCP-Protocol-Version: 2026-07-28\r\nContent-Length: " +
        std::to_string(payload.size()) +
        "\r\nConnection: close\r\n\r\n" + payload;
    ::send(fd, req.data(), req.size(), 0);
    char buf[4096];
    ssize_t n;
    std::string raw;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
      raw.append(buf, static_cast<size_t>(n));
    }
    reply.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      const std::string status_line = raw.substr(0, raw.find("\r\n"));
      const size_t sp = status_line.find(' ');
      if (sp != std::string::npos) {
        reply.status = std::atoi(status_line.c_str() + sp + 1);
      }
      reply.body = raw.substr(header_end + 4);
    }
  }
  ::close(fd);
  return reply;
}

std::string call_payload(int id, const std::string& tool) {
  return json{{"jsonrpc", "2.0"},
              {"id", id},
              {"method", "tools/call"},
              {"params", {{"name", tool}, {"arguments", json::object()}}}}
      .dump();
}

// The tool result is wrapped as content[0].text, i.e. the JSON the handler
// returned, dumped.
json tool_text(const Reply& reply) {
  const json envelope = json::parse(reply.body);
  return json::parse(envelope["result"]["content"][0]["text"].get<std::string>());
}

json fast_handler(const json& /*args*/) { return json{{"fast", true}}; }

json hold_thread_handler(const json& /*args*/) {
  std::this_thread::sleep_for(std::chrono::milliseconds(kHoldMs));
  return json{{"held", "thread"}};
}

void hold_deferred_handler(const json& /*args*/, mcp::Done done) {
  std::thread([done = std::move(done)]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(kHoldMs));
    done(json{{"held", "deferred"}});
  }).detach();
}

void wait_until_listening() {
  for (int i = 0; i < 100; ++i) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(kPort);
      ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      const bool ok =
          ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
      ::close(fd);
      if (ok) {
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::cerr << "server did not start listening\n";
  std::exit(1);
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // One compute thread: a call that occupies it must not affect kInline calls,
  // and a kDeferred call must not need it at all.
  struct WFGlobalSettings settings = GLOBAL_SETTINGS_DEFAULT;
  settings.compute_threads = 1;
  settings.handler_threads = 4;
  WORKFLOW_library_init(&settings);

  mcp::McpServer server;
  server.add_tool(mcp::Tool{.name = "fast",
                            .description = "Returns at once.",
                            .input_schema = json::object(),
                            .handler = fast_handler});
  server.add_tool(mcp::Tool{.name = "hold_thread",
                            .description = "Occupies a worker thread.",
                            .input_schema = json::object(),
                            .handler = hold_thread_handler,
                            .dispatch = mcp::Dispatch::kThread});
  server.add_tool(mcp::Tool{.name = "hold_deferred",
                            .description = "Replies from another thread.",
                            .input_schema = json::object(),
                            .dispatch = mcp::Dispatch::kDeferred,
                            .deferred = hold_deferred_handler});

  mcp::StreamableHttpTransport transport;
  transport.set_message_handler([&server](const json& message, mcp::Done done) {
    server.handle_message(message, std::move(done));
  });
  transport.set_dispatch_probe(
      [&server](const std::string& name) { return server.tool_dispatch(name); });

  std::thread runner([&transport] { transport.run("127.0.0.1", kPort); });
  wait_until_listening();

  // 1. kThread holds the only compute thread; a kInline call must not care.
  std::thread held([&] { post(call_payload(1, "hold_thread")); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  Reply fast = post(call_payload(2, "fast"));
  CHECK(fast.status == 200);
  CHECK(fast.ms < kFastLimitMs);
  std::cout << "kThread pending -> kInline call took " << fast.ms << "ms\n";
  held.join();

  // 2. A kDeferred call waits 2s without holding a thread; kInline stays fast
  //    and the deferred reply still arrives, with the handler's output.
  std::thread pending([&] { post(call_payload(3, "hold_deferred")); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  fast = post(call_payload(4, "fast"));
  CHECK(fast.status == 200);
  CHECK(fast.ms < kFastLimitMs);
  std::cout << "kDeferred pending -> kInline call took " << fast.ms << "ms\n";
  pending.join();

  Reply deferred = post(call_payload(5, "hold_deferred"));
  CHECK(deferred.status == 200);
  CHECK(deferred.ms >= kHoldMs - 100);
  CHECK(tool_text(deferred)["held"] == "deferred");
  std::cout << "kDeferred reply arrived after " << deferred.ms << "ms\n";

  transport.stop();
  runner.join();

  if (g_failures == 0) {
    std::cout << "http_dispatch_test: all tests passed\n";
    return 0;
  }
  std::cerr << "http_dispatch_test: " << g_failures << " check(s) failed\n";
  return 1;
}
