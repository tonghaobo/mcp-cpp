// mcp-cpp: a deliberately slow HTTP service, used to exercise async clients.
//
// Every request parks on a framework timer for `delay` seconds and only then
// answers. The timer sits on the HTTP task's own series, so a waiting request
// holds no compute thread and no connection thread -- many 5s/30s/50s calls
// can be in flight at once. Same series+task shape the MCP transport uses.
//
//   Run:   ./delay_server [port]            (default 9100)
//   Call:  curl "http://127.0.0.1:9100/delay?name=job1&delay=30"
//   Reply: {"name":"job1","delay":30,"ok":true,"waited_ms":30002}
//
// Build requires -DMCP_ENABLE_WORKFLOW=ON.
#include <workflow/WFHttpServer.h>
#include <workflow/HttpMessage.h>
#include <workflow/WFTaskFactory.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

using nlohmann::json;

namespace {

constexpr int kDefaultDelaySeconds = 30;
constexpr int kMaxDelaySeconds = 600;

volatile std::sig_atomic_t g_stop = 0;
std::atomic<int> g_inflight{0};
std::atomic<unsigned> g_sequence{0};

std::string query_value(const std::string& uri, const std::string& key) {
  const size_t question = uri.find('?');
  if (question == std::string::npos) {
    return {};
  }
  const std::string query = uri.substr(question + 1);
  size_t pos = 0;
  while (pos < query.size()) {
    const size_t amp = query.find('&', pos);
    const std::string pair =
        query.substr(pos, amp == std::string::npos ? std::string::npos
                                                   : amp - pos);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == key) {
      return pair.substr(eq + 1);
    }
    if (amp == std::string::npos) {
      break;
    }
    pos = amp + 1;
  }
  return {};
}

// Returns -1 for a malformed or out-of-range value.
int parse_delay(const std::string& raw) {
  if (raw.empty()) {
    return kDefaultDelaySeconds;
  }
  try {
    const int delay = std::stoi(raw);
    if (delay < 0 || delay > kMaxDelaySeconds) {
      return -1;
    }
    return delay;
  } catch (const std::exception&) {
    return -1;
  }
}

void reply_json(WFHttpTask* task, const char* status, const char* reason,
                const json& body) {
  auto* resp = task->get_resp();
  resp->set_status_code(status);
  resp->set_reason_phrase(reason);
  resp->add_header_pair("Content-Type", "application/json");
  const std::string text = body.dump();
  resp->append_output_body(text.c_str(), text.size());
}

void process(WFHttpTask* task) {
  const std::string uri = task->get_req()->get_request_uri();

  if (uri.rfind("/delay", 0) != 0) {
    reply_json(task, "404", "Not Found", json{{"error", "not found"}});
    return;
  }

  const int delay = parse_delay(query_value(uri, "delay"));
  if (delay < 0) {
    reply_json(task, "400", "Bad Request",
               json{{"error", "delay must be 0..600 seconds"}});
    return;
  }

  std::string name = query_value(uri, "name");
  if (name.empty()) {
    name = "job-" + std::to_string(++g_sequence);
  }

  const int inflight = ++g_inflight;
  std::printf("[delay] start %-10s delay=%-3ds inflight=%d\n", name.c_str(),
              delay, inflight);

  const auto started = std::chrono::steady_clock::now();
  WFTimerTask* timer = WFTaskFactory::create_timer_task(
      delay, 0, [task, name, delay, started](WFTimerTask*) {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        const int left = --g_inflight;
        reply_json(task, "200", "OK",
                   json{{"name", name},
                        {"delay", delay},
                        {"ok", true},
                        {"waited_ms", waited.count()}});
        std::printf("[delay] done  %-10s waited=%lldms inflight=%d\n",
                    name.c_str(), static_cast<long long>(waited.count()), left);
      });

  // Same series as the HTTP task: the response is sent only after the timer
  // fires, and no thread is held in between.
  series_of(task)->push_back(timer);
}

void on_signal(int) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const unsigned port =
      argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 9100u;

  WFHttpServer server(process);
  if (server.start(port) != 0) {
    std::fprintf(stderr, "delay_server: cannot listen on port %u\n", port);
    return 1;
  }
  std::printf("delay_server: listening on 127.0.0.1:%u\n", port);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  server.stop();
  return 0;
}
