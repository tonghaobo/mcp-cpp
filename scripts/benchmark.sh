#!/usr/bin/env bash
# Benchmark: mcp-cpp (C++/workflow) vs official Python SDK (fastmcp), same
# `add` tool, same v1 tools/call load, same container.
#
# Reports P50/P90/P99/P999 latency and QPS at 100/500/1000 concurrency.
set -euo pipefail

IMAGE="${IMAGE:-mcp-cpp-builder}"
MCPCPP_PORT="${MCPCPP_PORT:-8980}"
PY_PORT="${PY_PORT:-8981}"
REQUESTS="${REQUESTS:-2000}"
CONCS="${CONCS:-100 500 1000}"

echo "==> Benchmark in container: mcp-cpp vs fastmcp (same add tool)"
docker run --rm -e CONCS="$CONCS" -e REQUESTS="$REQUESTS" -v "$PWD:/src" -w /src "$IMAGE" bash -c '
  set -euo pipefail

  echo "### 1. Build mcp-cpp"
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMCP_ENABLE_WORKFLOW=ON >/dev/null
  cmake --build build -j"$(nproc)" --target mcp_http_example >/dev/null

  echo "### 2. mcp-cpp (C++/workflow)"
  ./build/mcp_http_example 127.0.0.1 8980 >/tmp/mcppp.log 2>&1 &
  MC=$!
  sleep 1.5
  for C in $CONCS; do
    echo "--- concurrency $C ---"
    ./.venv/bin/python benchmark/bench.py --url http://127.0.0.1:8980/mcp \
        --concurrency "$C" --requests "$REQUESTS"
  done
  kill $MC 2>/dev/null || true

  echo "### 3. Python SDK (fastmcp)"
  ./.venv/bin/uvicorn benchmark.fastmcp_server:app --host 127.0.0.1 --port 8981 >/tmp/py.log 2>&1 &
  PY=$!
  sleep 2
  for C in $CONCS; do
    echo "--- concurrency $C ---"
    ./.venv/bin/python benchmark/bench.py --url http://127.0.0.1:8981/mcp \
        --concurrency "$C" --requests "$REQUESTS"
  done
  kill $PY 2>/dev/null || true
'
