cmake_minimum_required(VERSION 3.24)
project(mcp-cpp VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# mcp-cpp: a high-performance MCP Server framework in C++17.
# JSON-RPC 2.0 protocol layer + stdio + Streamable HTTP transports on
# sogou/workflow (tools/list, tools/call), v1/v2 dual protocol forms.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(MCP_ENABLE_WORKFLOW "Enable Workflow-based Streamable HTTP transport" OFF)
option(MCP_BUILD_EXAMPLES "Build the example stdio server" ON)
option(MCP_BUILD_TESTS "Build unit/integration tests" ON)

# --- Dependencies ----------------------------------------------------------
# nlohmann/json: single-header JSON library (pinned). Workflow is only fetched
# when MCP_ENABLE_WORKFLOW=ON.
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

if(MCP_ENABLE_WORKFLOW)
  # Pin sogou/workflow and build the Streamable HTTP transport.
  # v1.0.1 (2026-06): latest stable; depends on OpenSSL (Dockerfile has libssl-dev).
  # NOTE: workflow's own CMake emits headers/libs into its source tree (_include/_lib);
  # as a FetchContent subproject that is harmless for us (we only link its target).
  FetchContent_Declare(
    workflow
    GIT_REPOSITORY https://github.com/sogou/workflow.git
    GIT_TAG v1.0.1
  )
  FetchContent_MakeAvailable(workflow)
  message(STATUS "MCP_ENABLE_WORKFLOW=ON: building Streamable HTTP transport on sogou/workflow.")
endif()

# --- Core library ----------------------------------------------------------
set(MCP_CORE_SOURCES
  src/jsonrpc.cpp
  src/server.cpp
  src/tool.cpp
  src/transport_stdio.cpp
)
if(MCP_ENABLE_WORKFLOW)
  list(APPEND MCP_CORE_SOURCES src/transport_http.cpp)
endif()
add_library(mcp STATIC ${MCP_CORE_SOURCES})
target_include_directories(mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mcp PUBLIC nlohmann_json::nlohmann_json)
if(MCP_ENABLE_WORKFLOW)
  # workflow v1.0.1's static target is named `workflow-static` (not `workflow`).
  # It pulls OpenSSL transitively; FetchContent also emits `workflow-shared`.
  target_link_libraries(mcp PUBLIC workflow-static)
endif()
target_compile_options(mcp PRIVATE -Wall -Wextra -Wpedantic)

# --- Example ---------------------------------------------------------------
if(MCP_BUILD_EXAMPLES)
  add_executable(mcp_example examples/echo_server.cpp)
  target_link_libraries(mcp_example PRIVATE mcp)
  if(MCP_ENABLE_WORKFLOW)
    add_executable(mcp_http_example examples/http_server.cpp)
    target_link_libraries(mcp_http_example PRIVATE mcp)
  endif()
endif()

# --- Tests ------------------------------------------------------------------
if(MCP_BUILD_TESTS)
  enable_testing()

  add_executable(jsonrpc_test tests/jsonrpc_test.cpp)
  target_link_libraries(jsonrpc_test PRIVATE mcp)
  add_test(NAME jsonrpc_test COMMAND jsonrpc_test)

  add_executable(server_test tests/server_test.cpp)
  target_link_libraries(server_test PRIVATE mcp)
  add_test(NAME server_test COMMAND server_test)

  if(MCP_BUILD_EXAMPLES)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME stdio_integration
               COMMAND ${Python3_EXECUTABLE}
                       ${CMAKE_CURRENT_SOURCE_DIR}/tests/stdio_integration.py
                       $<TARGET_FILE:mcp_example>)
    endif()
  endif()

  if(MCP_ENABLE_WORKFLOW AND MCP_BUILD_EXAMPLES)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME http_integration
               COMMAND ${Python3_EXECUTABLE}
                       ${CMAKE_CURRENT_SOURCE_DIR}/tests/http_integration.py
                       $<TARGET_FILE:mcp_http_example>)
      add_test(NAME http_integration_v2
               COMMAND ${Python3_EXECUTABLE}
                       ${CMAKE_CURRENT_SOURCE_DIR}/tests/http_integration_v2.py
                       $<TARGET_FILE:mcp_http_example>)
    endif()
  endif()
endif()
