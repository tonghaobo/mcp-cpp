// mcp-cpp: Tool definition and registration API.
#pragma once

#include <mcp/dispatch.hpp>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace mcp {

// One registered tool.
//
// The handler shape follows `dispatch`: `handler` returns the result
// synchronously (kInline / kThread), `deferred` receives a Done and may reply
// at once or hand it to another thread (kDeferred). Set the one you declared.
//
// Initialize with C++20 designated initializers so field order is never an
// issue when adding new fields:
//
//   Tool{.name = "add", .description = "Adds.", .handler = add_handler};
struct Tool {
  using Handler = std::function<nlohmann::json(const nlohmann::json& args)>;

  std::string name;
  std::string description;                 // optional; empty -> omitted from tools/list
  nlohmann::json input_schema;             // JSON Schema for `arguments`
  Handler handler;                         // required for kInline / kThread
  Dispatch dispatch = Dispatch::kInline;   // execution model; see dispatch.hpp
  DeferredHandler deferred;                // required for kDeferred

  // Serialize into a tools/list result entry.
  nlohmann::json to_json() const;
};

}  // namespace mcp
