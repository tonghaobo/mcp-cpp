# mcp-cpp Benchmark

对比 mcp-cpp（C++20 / sogou-workflow）与官方 Python SDK（fastmcp）在 Streamable HTTP
下的**并发 `tools/call`** 延迟与吞吐，覆盖三种执行模型与混合负载。

## 场景

三种任务分别对应 mcp-cpp 的一档执行模型，Python 端用语义等价的写法：

| 场景 | 工具 | 参数 | mcp-cpp 语义 | fastmcp 对照实现 |
|---|---|---|---|---|
| `inline` | `add` | `a=2, b=3` | `kInline`：纯计算，无等待 | 同步 `def add` |
| `thread` | `slow_add` | `delay_ms=10` | `kThread`：阻塞一个 worker 线程 | 同步 `def` + `time.sleep`（anyio 线程池） |
| `deferred` | `slow_query` | `rows=5, delay=1` | `kDeferred`：等待期间不占线程 | `async def` + `asyncio.sleep` |
| `mixed` | 三者轮转 | 默认 70/20/10 | 混合画像 | 同样的轮转比例 |

## 方法

- **负载**：`bench.py` 用 `aiohttp` 发原始 HTTP `tools/call`（v1 会话），按并发波次
  （`asyncio.gather`）压测，读完整响应后计时。
- **确定性序列**：混合场景按权重生成固定轮转序列（`inline×70 → thread×20 → deferred×10`
  循环），两个服务器收到的请求序列完全一致，避免随机波动带来的偏差。
- **正确性校验**：每个响应必须 `HTTP 200` + 含 `result` + 不含 `error`；否则计入 `errors`
  并输出错误率。失败请求同样计入延迟样本。
- **延迟口径**：单个请求的端到端墙钟时间（HTTP 往返 + 协议解析 + 响应组帧）。
  负载是 closed-loop（每波等自己的请求），存在 coordinated omission；
  数值适合服务器间相对比较，不作为开环容量规划依据。
- **预热**：默认 100 个请求，沿同一混合序列，避免某一档吃到冷启动优势。
- **线程池前提**（`thread` 场景必读）：
  - mcp-cpp：workflow `compute_threads` 默认 `-1`，运行时取 CPU 核数（本机 4）；`handler_threads` 默认 20。
  - fastmcp：同步工具跑在 anyio 的 worker 线程池（默认上限 40），异步工具不占线程。
  - 因此 `thread` 场景比较的是**两个大小不同的池子**：C++ 的 4 线程在 100 并发下已排队，
    fastmcp 的 40 线程没有打满。这组数字只代表"各自默认配置下的表现"，不是同池对比。
- **公平性**：同机、同客户端、同请求数/并发；两个服务器先后启动，互不干扰。

## 运行

```bash
# 一键：容器内构建 mcp-cpp + 启动两个服务器 + 三档 + 混合
REQUESTS=2000 CONCS="100 500 1000" ./scripts/benchmark.sh

# 只跑部分场景 / 单档并发
SCENARIOS="inline mixed" REQUESTS=2000 CONCS="100" ./scripts/benchmark.sh

# 本机（不用 Docker）：本机编译产物 + 本地 venv，同样跑场景
REQUESTS=1000 CONCS="100" ./scripts/benchmark_local.sh
```

> `SCENARIOS` 默认 `"inline thread deferred mixed"`。`deferred` 的等待是 1s，
> 该场景耗时约 `请求数 / 并发` 秒，跑全量时总时长会明显长于纯 `add`。

环境：Linux Docker（`mcp-cpp-builder`），`uvicorn 0.52`，`fastmcp 3.4`，`mcp 2.1.1`，
`python 3.12`。请求走 `127.0.0.1` 回环。

## 结果

### A. 三档 + 混合（2026-10-20，本机 4 核，并发 100 / 1000 请求）

> 本机模式下 server 与压测客户端**争抢同一台 4 核机器**，数值**不可与 B 表（容器内）对比**；
> 这组数据用于验证三档差异，以及混合负载下各档是否相互拖累。

| 场景 | mcp-cpp QPS | fastmcp QPS | mcp-cpp P50 | fastmcp P50 | mcp-cpp P99 | fastmcp P99 |
|---|---|---|---|---|---|---|
| `inline` | 4083 | 451 | 13.73 | 177.14 | 32.17 | 238.95 |
| `thread` | 373 | 405 | 130.25 | 197.67 | 259.18 | 301.42 |
| `deferred` | 98 | 85 | 1000.88 | 1129.96 | 1023.14 | 1218.83 |
| `mixed` | 98 | 83 | 17.93 | 183.70 | 1013.16 | 1237.12 |

`mixed` 的整体分位受 70/20/10 比例影响，按类型拆分更有意义（P50 / P99，ms）：

| 类型 | mcp-cpp | fastmcp |
|---|---|---|
| `inline` | 14.91 / 34.01 | 172.84 / 252.82 |
| `thread` | 31.59 / 65.81 | 218.81 / 258.64 |
| `deferred` | 1001.05 / 1015.18 | 1190.67 / 1250.57 |

两种服务器的错误率均为 0%。

### B. 纯 `add`（2026-08-26，容器内实测）

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

## 解读

- **`inline`**：mcp-cpp 的 QPS 约为 fastmcp 的 9 倍，P50 低一个数量级（13.73ms vs 177.14ms）。
- **`thread`**：两组 QPS 接近（373 vs 405），但这是不同池子的结果——C++ 的 4 个 compute 线程
  在 10ms 任务、100 并发下理论上限约 400 QPS，已经打满；fastmcp 的 40 线程池未打满，
  被 HTTP 开销限住。P50 上 C++ 仍低约 1.5 倍（130ms vs 198ms）。要同池对比，
  需要把两侧线程数显式对齐后再跑。
- **`deferred`**：两边 QPS 都收敛到 `并发 / 等待时间`（≈100 vs ≈85），说明两者都做到了
  "等待期间不占线程"；差异在固定开销：C++ P50 1000.88ms 几乎就是等待时间本身，
  fastmcp 多出约 130ms。
- **`mixed`**：mcp-cpp 的 700 次 `inline` 调用 P50 为 14.91ms，与单独压测的 13.73ms 基本一致
  —— `kDeferred` 的长等待没有拖慢同进程的短任务。fastmcp 的 `inline` 在混合下仍是约 173ms，
  主要受自身请求处理开销限制。
- **扩展性（B 表）**：纯 `add` 下 mcp-cpp 从 100→1000 并发 QPS 仅下降 ~31%，
  fastmcp 基本持平在 ~1.6k QPS，延迟随并发线性劣化。

> 说明：数据随环境/负载变化，仅作相对参考。Python 端使用 SSE 响应组帧、逐请求开销更大，
> 这反映了真实差距，而非人为放大。
