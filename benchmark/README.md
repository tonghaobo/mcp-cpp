# mcp-cpp Benchmark

对比 mcp-cpp（C++20 / sogou-workflow）与官方 Python SDK（fastmcp）在 Streamable HTTP
下的**并发 `tools/call`** 延迟与吞吐。

## 方法

- **场景**：单端点 `/mcp`，v1 形态（initialize 握手 → 并发 `tools/call` `add`）。
- **负载**：`benchmark/bench.py` 用 `aiohttp` 并发发原始 HTTP `tools/call`，读完整响应，
  统计每个请求的墙钟延迟；按并发波次（`asyncio.gather`）压测。
- **公平性**：两个服务器注册**完全相同的 `add` 工具**；用同一台容器、同一个压测客户端、
  相同的请求数/并发；先后启动，互不干扰。
- **服务器**：
  - mcp-cpp：`build/mcp_http_example`（C++ / workflow，v1 JSON 响应）
  - Python：`benchmark/fastmcp_server.py` 经 uvicorn 启动（fastmcp，v1 SSE 响应）

## 运行

```bash
# 一键：容器内构建 mcp-cpp + 启动两个服务器 + 压测
REQUESTS=2000 CONCS="100 500 1000" ./scripts/benchmark.sh
```

环境：Linux Docker（`mcp-cpp-builder`），`uvicorn 0.52`，`fastmcp 3.4`，`mcp 2.1.1`，
`python 3.12`。请求走 `127.0.0.1` 回环。

## 结果（2026-08-26，容器内实测）

**P50 / P99 延迟（ms）**

| 并发 | mcp-cpp P50 | mcp-cpp P99 | fastmcp P50 | fastmcp P99 |
|---|---|---|---|---|
| 100  | 1.21  | 1.33  | 23.99 | 26.60 |
| 500  | 7.06  | 8.09  | 141.94 | 155.92 |
| 1000 | 15.35 | 17.38 | 330.66 | 344.89 |

**QPS 吞吐**

| 并发 | mcp-cpp QPS | fastmcp QPS | 倍数 |
|---|---|---|---|
| 100  | 20710 | 1814 | ~11.4x |
| 500  | 16035 | 1658 | ~9.7x |
| 1000 | 14275 | 1609 | ~8.9x |

### 解读

- **延迟**：全并发下 mcp-cpp 的 P50 比 fastmcp 快约 **20 倍**；fastmcp 在 1000 并发下
  P50 劣化到 331ms，mcp-cpp 仍保持 15ms。
- **吞吐**：mcp-cpp 的 QPS 约为 fastmcp 的 **9–11 倍**，且随并发增长下降更缓。
- **并发扩展性**：mcp-cpp 从 100→1000 并发 QPS 仅下降 ~31%；fastmcp 基本持平在 ~1.6k QPS，
  延迟却随并发线性劣化。

> 说明：benchmark 测量的是端到端 `tools/call`（含 HTTP 往返、协议解析、响应组帧）。Python 端
> 使用 SSE 响应组帧、逐请求初始化开销更大，这反映了真实差距。数据随环境/负载变化，仅作相对参考。
