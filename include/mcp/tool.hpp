// mcp-cpp: Tool definition and registration API.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace mcp {

// One registered tool.
//
// `handler` returns the tool result synchronously. Its execution thread depends
// on `async`:
//   - async == false (default): the handler runs inline on the transport
//     (process) thread; the request is fully handled before it returns.
//   - async == true: the transport hands the tool/call to a workflow compute
//     thread (go task); the process thread returns immediately without
//     blocking, and the response is written once the handler completes. Use
//     this for slow handlers so they do not stall the HTTP event loop.
// Either way the Handler signature is the same; only the executor changes.
//
// Initialize with C++20 designated initializers so field order is never an
// issue when adding new fields:
//
//   Tool{.name = "add", .description = "Adds.", .handler = add_handler};
struct Tool {
  using Handler = std::function<nlohmann::json(const nlohmann::json& args)>;

  std::string name;
  std::string description;                     // optional; empty -> omitted from tools/list
  nlohmann::json input_schema;                 // JSON Schema for `arguments`
  Handler handler;                             // required; tools/call fails otherwise
  bool async = false;                          // true -> run handler off the transport thread

  // Serialize into a tools/list result entry.
  nlohmann::json to_json() const;
};

}  // namespace mcp
