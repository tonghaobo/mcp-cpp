// mcp-cpp: per-tool execution model.
//
// Choose by what the handler spends its time on, not by what it does: waiting
// work costs nothing while it waits, computing work must own a thread.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>

namespace mcp {

// kInline   ms-level work: runs inline on the transport thread (default).
// kThread   blocking or CPU-bound work: runs on a framework worker thread, so
//           it holds one thread for its whole duration (queues when they are
//           all busy).
// kDeferred waiting on something else (remote system, IO, timer): returns at
//           once and replies later through Done, holding no thread meanwhile.
enum class Dispatch { kInline, kThread, kDeferred };

// Completion of one call: exactly once, from any thread, possibly long after
// the handler returned. A null json means "nothing to send back".
using Done = std::function<void(nlohmann::json response)>;

// Deferred tool handler: `done` takes the same value a synchronous handler
// would return (a tool output, or a full MCP call result).
using DeferredHandler =
    std::function<void(const nlohmann::json& args, Done done)>;

}  // namespace mcp
