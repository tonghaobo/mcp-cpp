// mcp-cpp: Tool definition and registration API.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace mcp {

// One registered tool.
//
// `handler` is synchronous and runs on the transport thread. It is also an
// extension point for concurrent execution: the server may run handlers on a
// thread pool; the Handler signature stays the same, only the executor changes.
struct Tool {
  using Handler = std::function<nlohmann::json(const nlohmann::json& args)>;

  std::string name;
  std::string description;                     // optional; empty -> omitted from tools/list
  nlohmann::json input_schema;                 // JSON Schema for `arguments`
  Handler handler;                             // required; tools/call fails otherwise

  // Serialize into a tools/list result entry.
  nlohmann::json to_json() const;
};

}  // namespace mcp
