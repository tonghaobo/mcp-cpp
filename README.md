# mcp-cpp

> A high-performance [MCP (Model Context Protocol)](https://modelcontextprotocol.io) server framework implemented in C++20 from scratch. Engineered for high-concurrency tool calls — with benchmark data against the official Python SDK — while the mainstream ecosystem trades performance for features. Clean layered architecture (JSON-RPC → transport → server), transport-agnostic core, and a thread-pool extension point.

用 C++20 从零实现的高性能 MCP（Model Context Protocol）Server 框架。

针对 MCP 生态主流实现"功能优先、性能随缘"的现状，本框架把重点放在**高并发工具调用**的延迟与吞吐上，并附 benchmark 数据证明。当前支持两种传输、双协议形态：
- **stdio**（`StdioTransport`）：最小闭环，JSON-RPC 2.0 协议层 + `initialize` / `tools/list` / `tools/call`。
- **Streamable HTTP**（`StreamableHttpTransport`，基于 [sogou/workflow](https://github.com/sogou/workflow)）：单端点 `/mcp`，官方 Python SDK 直接互通。
  - **v1**（`2025-06-18`）：`Mcp-Session-Id` 会话管理（传统握手）。
  - **v2**（`2026-07-28`，stateless）：**无 session-id、无握手**，`server/discover` 探测 + `_meta` 信封。同一端点自动分流。

## 快速开始

### 本机编译

本项目是标准 CMake 工程，任何装有 CMake ≥ 3.24 与 C++20 编译器的环境都可直接编译，**无需 Docker**。构建时自动通过 FetchContent 拉取 `nlohmann/json`；若开启 HTTP 传输还需 `sogou/workflow`（其依赖 OpenSSL 开发头）。

**依赖安装**（仅构建，按需选择平台）：

```bash
# Ubuntu / Debian
sudo apt update && sudo apt install -y build-essential cmake git libssl-dev
# macOS（Homebrew）
brew install cmake git openssl
# 测试 stdio/HTTP 集成用例还需 Python 3
```

**编译**（stdio 形态）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

**编译并运行 HTTP 形态**（Streamable HTTP，需 workflow + OpenSSL）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMCP_ENABLE_WORKFLOW=ON
cmake --build build -j"$(nproc)"
./build/mcp_http_example 127.0.0.1 8931   # 默认 http://127.0.0.1:8931/mcp
./build/mcp_http_example 0.0.0.0 8931     # 绑定所有接口，接受远程连接
```

> `mcp_http_example` 接受 `[host] [port]`：`host` 为监听地址（`127.0.0.1` 仅本机、`0.0.0.0` 对外），默认 `127.0.0.1`；`port` 默认 `8931`。

产物：静态库 `libmcp.a`、示例 server `build/mcp_example`（stdio）/ `build/mcp_http_example`（HTTP）、单元测试 `jsonrpc_test` / `server_test`。

### Docker 编译

本仓库开发机为 macOS（有意不装本地 C++ 工具链），所有编译验证统一走 Linux Docker 容器：

```bash
# 一键：构建镜像 + 容器内 cmake configure/build/ctest
./scripts/build_docker.sh

# 官方 Python SDK 互通测试（stdio + Streamable HTTP，首次会建 .venv 装 mcp）
./scripts/interop.sh
```

手动等价命令：

```bash
docker build -t mcp-cpp-builder -f Dockerfile .
docker run --rm -v "$PWD:/src" -w /src mcp-cpp-builder bash -c \
  'cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
   cmake --build build -j"$(nproc)" && \
   ctest --test-dir build --output-on-failure'
```

> Streamable HTTP 需在 CMake 配置时加 `-DMCP_ENABLE_WORKFLOW=ON`（拉取 sogou/workflow + OpenSSL），
> 产物含 `build/mcp_http_example`。默认（不带该开关）只构建 stdio 形态。

### 与 MCP 客户端交互

**stdio**：`mcp_example` 运行在 stdio 上（换行分隔的 JSON-RPC）。任何 MCP client 都可以用它：

```python
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

async with stdio_client(StdioServerParameters(command=["./build/mcp_example"])) as (r, w):
    async with ClientSession(r, w) as s:
        await s.initialize()
        tools = await s.list_tools()
        await s.call_tool("add", {"a": 2, "b": 3})
```

**Streamable HTTP**：`mcp_http_example`（需 `MCP_ENABLE_WORKFLOW=ON`）暴露单端点，官方 SDK 的 `streamable_http_client` 直接连：

```python
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client

async with streamable_http_client("http://127.0.0.1:8931/mcp") as (r, w):
    async with ClientSession(r, w) as s:
        await s.initialize()
        tools = await s.list_tools()
        await s.call_tool("add", {"a": 2, "b": 3})
```

> 上例需在 Python 环境安装官方 SDK：`pip install "mcp"`（若环境限制，用 `python3 -m venv .venv && .venv/bin/pip install mcp`）。
> `./scripts/interop.sh` 默认在 Docker 容器内跑互通；本机已装 SDK 时可手动运行 `python tests/interop_stdio.py ./build/mcp_example` 与 `python tests/interop_http_v2.py ./build/mcp_http_example`。

也可以直接手工喂 JSON-RPC：

```bash
$ ./build/mcp_example
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}         # -> 握手结果
{"jsonrpc":"2.0","method":"notifications/initialized"}              # -> 无响应
{"jsonrpc":"2.0","id":2,"method":"tools/list"}                      # -> 工具列表
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"add","arguments":{"a":1,"b":2}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}}
^D  # EOF -> 进程以 0 退出
```

### 最小使用示例

```cpp
#include <mcp/server.hpp>
#include <mcp/transport_stdio.hpp>

int main() {
  mcp::McpServer server;
  server.add_tool(mcp::Tool{
      "add", "Adds two numbers.", json::object(),
      [](const nlohmann::json& args) {
        return nlohmann::json{{"sum", args["a"].get<long long>() + args["b"].get<long long>()}};
      }});
  mcp::StdioTransport transport;
  transport.set_message_handler([&server](const nlohmann::json& msg) {
    return server.handle_message(msg);
  });
  return transport.run();
}
```

> 更完整的使用示例见 `examples/echo_server.cpp`（含 `echo` / `add` / `get_time` 三个工具）。

## 架构

```
┌────────────────────────────────────────────────────────────────┐
│  jsonrpc 层（transport-agnostic）      transport 层    server 层 │
│  ParsedMessage{Kind, req, notif,       StdioTransport   McpServer│
│   resp, error, error_id};              (行 framing)     handle_  │
│  parse() 永不抛异常                     StreamableHttp   message( │
│  Error/Request/Notification/            Transport        json)   │
│   Response::to_json() + 工厂函数         (workflow)       → json │
│                                      (多客户端异步)   (null=不回复)│
└────────────────────────────────────────────────────────────────┘
```

- **协议层** `include/mcp/jsonrpc.hpp` + `src/jsonrpc.cpp`：JSON-RPC 2.0 消息编解码。`parse(raw)` 永不抛异常：非法 JSON → `-32700`；非对象 / 缺 method 且非 response → `-32600`（能识别 id 时回显）；有 method+id → Request；有 method 无 id → Notification；有 id+result/error → Response。
- **传输层**：
  - `StdioTransport`（`transport_stdio.cpp`）：按行读 stdin → `jsonrpc::parse` → 调 handler → 写 stdout 并 flush；忽略 SIGPIPE；EOF 优雅退出。
  - `StreamableHttpTransport`（`transport_http.cpp`，需 `MCP_ENABLE_WORKFLOW=ON`）：基于 sogou/workflow，单端点 `/mcp` POST/GET/DELETE；多客户端异步，JSON-RPC 分发复用 `handle_message`。同一端点**自动分流 v1/v2**：v1（`Mcp-Session-Id` 会话）+ v2（`2026-07-28` stateless，无 session / `server/discover`）。监听地址可配（`run(host, port)`，默认 `127.0.0.1`；传 `0.0.0.0` 对外暴露）。支持**异步 tool handler**：将某 tool 标记为 `async` 并注册 async probe 后，其 `tools/call` 被派发到 workflow compute 线程（go task）执行，transport 线程不被慢 handler 阻塞。
- **服务端核心** `include/mcp/server.hpp` + `src/server.cpp`：工具注册 + 分发。`initialize`（回 `protocolVersion`/`capabilities`/`serverInfo`）、`ping`、`tools/list`、`tools/call`、`notifications/initialized`；未知方法 `-32601`；工具缺失/参数非法 `-32602`；工具不存在 `-32005`；handler 抛 `std::invalid_argument` → `-32602`，其他异常 → `-32603`。handler 返回值若已是含 `content` 的对象则透传，否则包装为 `{content:[{type:"text",text}], isError:false}`。

## 协议版本

**双形态**（`include/mcp/protocol.hpp`）：
- **v1** `kProtocolVersion = "2025-06-18"`：传统握手 + `Mcp-Session-Id` 会话。
- **v2** `kProtocolVersionModern = "2026-07-28"`：stateless，无握手/无 session，`server/discover` + `_meta` 信封。

已用官方 Python SDK **mcp 2.x** 实测互通：legacy 客户端协商 `2025-06-18`（stdio / `streamable_http_client`）；新 `Client` 客户端协商 `2026-07-28`（`Client` auto 与 pinned）。同一 HTTP 端点自动分流，互不干扰。

## 测试

| 测试 | 类型 | 覆盖 |
|---|---|---|
| `tests/jsonrpc_test.cpp` | 单元 | parse 各类请求/通知/响应/错误、round-trip、`to_json`、工厂函数 |
| `tests/server_test.cpp` | 单元 | 握手字段、`-32601/-32602/-32005/-32603`、ping、list、通知、透传 |
| `tests/stdio_integration.py` | 集成（无 SDK 依赖） | spawn `mcp_example` 走 11 步原始 JSON-RPC：握手→通知→ping→list→call echo/add→未知工具→未知方法→坏参数→坏 JSON→EOF 退出码 0 |
| `tests/interop_stdio.py` | 互通 | 官方 Python SDK `ClientSession` stdio 连接 + `initialize` / `list_tools` / `call_tool` |
| `tests/http_integration.py` | 集成（无 SDK 依赖） | spawn `mcp_http_example` 走 11 步原始 HTTP（v1）：握手+会话、无会话 400、list/call（含异步 tool `slow_add`）、错误分支、202 通知、DELETE、GET SSE |
| `tests/interop_http.py` | 互通 | 官方 SDK `streamable_http_client`（v1） + `initialize` / `list_tools` / `call_tool` |
| `tests/http_integration_v2.py` | 集成（无 SDK 依赖，v2） | spawn `mcp_http_example` 走 5 步原始 HTTP stateless：server/discover、无 session 的 list/call/ping、错误分支 |
| `tests/interop_http_v2.py` | 互通（v2） | 官方 SDK `Client`（auto + pinned `2026-07-28`）+ `list_tools` / `call_tool` |

## 性能基准（Benchmark）

对比 mcp-cpp（C++/workflow）与官方 Python SDK（fastmcp），单端点 Streamable HTTP，
v1 形态并发 `tools/call` `add`（相同工具、同容器、同压测客户端）。详细方法与复现见
[benchmark/README.md](benchmark/README.md)。

**P50 延迟（ms）与 QPS**

| 并发 | mcp-cpp P50 | mcp-cpp QPS | fastmcp P50 | fastmcp QPS | 延迟倍数 | QPS 倍数 |
|---|---|---|---|---|---|---|
| 100  | 1.21  | 20710 | 24.0  | 1814 | ~20x | ~11x |
| 500  | 7.06  | 16035 | 141.9 | 1658 | ~20x | ~10x |
| 1000 | 15.35 | 14275 | 330.7 | 1609 | ~21x | ~9x |

> mcp-cpp 的 P50 延迟比 fastmcp 快约 **20 倍**，QPS 约 **9–11 倍**；高并发下 fastmcp 延迟急剧
> 劣化（1000 并发 P50 331ms），mcp-cpp 仍保持 15ms。复现：`REQUESTS=2000 CONCS="100 500 1000" ./scripts/benchmark.sh`。

## 目录结构

```
.
├── CMakeLists.txt              # C++20；mcp 静态库 + mcp_example + mcp_http_example + ctest；MCP_ENABLE_WORKFLOW 开关
├── Dockerfile                  # ubuntu:24.04 + build-essential/cmake/git/libssl-dev/python3-venv
├── include/mcp/                # 公共头文件（jsonrpc / protocol / tool / transport / transport_stdio / transport_http / server）
├── src/                        # 实现（jsonrpc / server / tool / transport_stdio / transport_http）
├── examples/                   # echo_server.cpp（stdio）· http_server.cpp（Streamable HTTP）
├── tests/                      # 单元 + 集成 + SDK 互通测试
├── benchmark/                  # 性能基准：bench.py + fastmcp_server.py + README
└── scripts/                    # build_docker.sh / interop.sh / benchmark.sh
```

## License

MIT — 见 [LICENSE](LICENSE)。
