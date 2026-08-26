// mcp-cpp: Tool serialization.
#include <mcp/tool.hpp>

#include <nlohmann/json.hpp>

namespace mcp {

nlohmann::json Tool::to_json() const {
  nlohmann::json entry;
  entry["name"] = name;
  if (!description.empty()) {
    entry["description"] = description;
  }
  if (input_schema.is_null() || input_schema.empty()) {
    // No schema provided: advertise an anything-goes object schema.
    entry["inputSchema"] = {{"type", "object"},
                            {"properties", nlohmann::json::object()}};
  } else {
    entry["inputSchema"] = input_schema;
  }
  return entry;
}

}  // namespace mcp
