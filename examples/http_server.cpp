// mcp-cpp: example MCP server over Streamable HTTP.
//
// Same tools as the stdio example (echo / add / get_time) plus one tool per
// execution model, served over a single HTTP endpoint using sogou/workflow:
//   slow_add   - kThread: blocks a worker thread for 200ms
//   slow_query - kDeferred: replies after 2s on a framework timer, holding no
//                thread while it waits
//
// Build requires -DMCP_ENABLE_WORKFLOW=ON (drags in sogou/workflow + OpenSSL).
//
//   Run with: ./mcp_http_example [host] [port] [--query-url URL]
//             host defaults to 127.0.0.1 (loopback); use 0.0.0.0 to accept
//             remote connections. port defaults to 8931. --query-url points
//             slow_query at a slow HTTP service (examples/delay_server.cpp);
//             without it slow_query falls back to a local timer.
//   Connect:  streamable_http_client(url="http://127.0.0.1:8931/mcp")
//
// get_time uses gmtime_r(), a POSIX function; expose it even under strict
// -std=c++20 (glibc hides POSIX declarations unless a feature macro is set).
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <mcp/server.hpp>
#include <mcp/transport_http.hpp>

#include <workflow/WFTaskFactory.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using nlohmann::json;

namespace {

json echo_handler(const json& args) {
  return json{{"text", args.value("text", "")}};
}

json add_handler(const json& args) {
  if (!args.contains("a") || !args.contains("b") || !args["a"].is_number() ||
      !args["b"].is_number()) {
    throw std::invalid_argument("add requires numeric arguments \"a\" and \"b\"");
  }
  if (args["a"].is_number_integer() && args["b"].is_number_integer()) {
    return json{{"sum", args["a"].get<long long>() + args["b"].get<long long>()}};
  }
  return json{{"sum", args["a"].get<double>() + args["b"].get<double>()}};
}

json get_time_handler(const json& /*args*/) {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);  // POSIX; the project only builds on Linux
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return json{{"time", std::string(buf)}};
}

// kThread: occupy a worker thread for the whole call (registered with
// dispatch = kThread). A stand-in for a blocking SDK or CPU-bound work.
json slow_add_handler(const json& args) {
  const int delay_ms = args.value("delay_ms", 200);
  if (delay_ms < 0 || delay_ms > 60000) {
    throw std::invalid_argument("delay_ms must be 0..60000");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  return add_handler(args);
}

// Set by --query-url. Empty -> slow_query uses a local timer instead.
std::string g_query_url;
std::atomic<unsigned> g_query_sequence{0};

int query_delay(const json& args) {
  const int delay = args.value("delay", 2);
  if (delay < 0 || delay > 600) {
    throw std::invalid_argument("delay must be 0..600 seconds");
  }
  return delay;
}

// MCP two-tier errors: a tool that ran but failed reports that inside a normal
// result (isError=true), not as a JSON-RPC error -- the model can read the text
// and retry. Protocol errors (unknown tool, bad params) stay JSON-RPC errors.
json tool_error(const std::string& text) {
  return json{{"content", json::array({json{{"type", "text"}, {"text", text}}})},
              {"isError", true}};
}

// kDeferred: reply when the HTTP service answers (create_http_task), or after
// a local timer when no --query-url was given. No thread is held while waiting.
void slow_query_handler(const json& args, mcp::Done done) {
  const int delay = query_delay(args);
  const int rows = args.value("rows", 0);

  if (g_query_url.empty()) {
    WFTimerTask* timer = WFTaskFactory::create_timer_task(
        delay, 0, [done = std::move(done), rows, delay](WFTimerTask*) mutable {
          done(json{{"rows", rows}, {"delay", delay}, {"source", "timer"}});
        });
    timer->start();
    return;
  }

  const std::string url = g_query_url + "/delay?name=query-" +
                          std::to_string(++g_query_sequence) +
                          "&delay=" + std::to_string(delay);
  auto* task = WFTaskFactory::create_http_task(
      url, /*redirect_max=*/0, /*retry_max=*/1,
      [done = std::move(done)](WFHttpTask* t) mutable {
        const void* body = nullptr;
        size_t len = 0;
        const char* status = t->get_resp()->get_status_code();
        if (t->get_state() != WFT_STATE_SUCCESS || status == nullptr ||
            std::strcmp(status, "200") != 0 ||
            !t->get_resp()->get_parsed_body(&body, &len)) {
          done(tool_error("upstream unavailable"));   // 失败也必须回包
          return;
        }
        try {
          done(json::parse(static_cast<const char*>(body),
                           static_cast<const char*>(body) + len));
        } catch (const std::exception&) {
          done(tool_error("response is not JSON"));
        }
      });
  task->set_receive_timeout((delay + 15) * 1000);   // ms
  task->start();
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  unsigned port = 8931u;
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--query-url" && i + 1 < argc) {
      g_query_url = argv[++i];
      while (!g_query_url.empty() && g_query_url.back() == '/') {
        g_query_url.pop_back();
      }
    } else {
      positional.push_back(arg);
    }
  }
  if (!positional.empty()) {
    host = positional[0];
  }
  if (positional.size() > 1) {
    port = static_cast<unsigned>(std::atoi(positional[1].c_str()));
  }

  mcp::McpServer server;
  server.add_tool(mcp::Tool{
      .name = "echo", .description = "Echoes back the provided text.",
      .input_schema = json::object(), .handler = echo_handler});
  server.add_tool(mcp::Tool{
      .name = "add", .description = "Adds two numbers and returns their sum.",
      .input_schema = json::object(), .handler = add_handler});
  // slow_add needs a thread of its own: kThread hands it to a worker thread.
  server.add_tool(mcp::Tool{
      .name = "slow_add",
      .description = "Adds two numbers after a short delay (kThread demo).",
      .input_schema = json::object(),
      .handler = slow_add_handler,
      .dispatch = mcp::Dispatch::kThread});
  // slow_query waits on the HTTP service (or a timer): kDeferred costs no thread.
  server.add_tool(mcp::Tool{
      .name = "slow_query",
      .description = "Queries a slow HTTP service; `delay` sets the wait (kDeferred demo).",
      .input_schema = json::object(),
      .dispatch = mcp::Dispatch::kDeferred,
      .deferred = slow_query_handler});
  server.add_tool(mcp::Tool{
      .name = "get_time",
      .description = "Returns the current UTC time in ISO-8601 format.",
      .input_schema = json::object(), .handler = get_time_handler});

  mcp::StreamableHttpTransport transport;
  transport.set_message_handler(
      [&server](const json& message, mcp::Done done) {
        server.handle_message(message, std::move(done));
      });
  // Tell the transport which execution model each tool declared.
  transport.set_dispatch_probe([&server](const std::string& name) {
    return server.tool_dispatch(name);
  });

  // The transport prints a readiness line once the listener is up.
  return transport.run(host, port);
}
