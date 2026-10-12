#!/usr/bin/env bash
# Benchmark on the host, without Docker: mcp-cpp (C++/workflow) vs official
# Python SDK (fastmcp), same `add` tool, same v1 tools/call load.
#
# Host twin of scripts/benchmark.sh: builds build/mcp_http_example locally and
# drives it (and the fastmcp reference server) with benchmark/bench.py at
# 100/500/1000 concurrency. Unlike the container script it relies on a local
# toolchain and a local venv (created on demand for bench.py / uvicorn).
#
# Usage: scripts/benchmark_local.sh
# Env:   REQUESTS (2000) · CONCS ("100 500 1000")
#        MCPCPP_PORT (8980) · PY_PORT (8981) · PYTHON (<repo>/.venv/bin/python)
#
# CAVEAT: the server and the load client share this machine's CPU. On a 4-core
# host the client saturates before the server at high concurrency, so these
# numbers are NOT comparable with the container-measured table in
# benchmark/README.md; use them for smoke tests and relative trends only.
set -euo pipefail
cd "$(dirname "$0")/.."

MCPCPP_PORT="${MCPCPP_PORT:-8980}"
PY_PORT="${PY_PORT:-8981}"
REQUESTS="${REQUESTS:-2000}"
CONCS="${CONCS:-100 500 1000}"

# Interpreter for bench.py / uvicorn. Defaults to the repo venv, created on
# demand: Ubuntu 24.04 is PEP 668 externally-managed, so a venv is mandatory.
PYTHON="${PYTHON:-}"
if [ -z "$PYTHON" ]; then
  PYTHON="$PWD/.venv/bin/python"
  if [ ! -x "$PYTHON" ]; then
    echo "==> creating .venv"
    python3 -m venv .venv
  fi
fi

MCPCPP_PID=""
PY_PID=""
cleanup() {
  if [ -n "$MCPCPP_PID" ]; then kill "$MCPCPP_PID" 2>/dev/null || true; fi
  if [ -n "$PY_PID" ]; then kill "$PY_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT

# Poll until something accepts TCP on the port. Both servers signal readiness
# asynchronously, so a fixed sleep is flaky on a cold start.
wait_for_port() {
  for _ in $(seq 1 150); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  echo "ERROR: $2 did not listen on port $1 (see /tmp/mcppp.log, /tmp/py.log)" >&2
  return 1
}

run_waves() {  # run_waves <port>
  for C in $CONCS; do
    echo "--- concurrency $C ---"
    "$PYTHON" benchmark/bench.py --url "http://127.0.0.1:$1/mcp" \
        --concurrency "$C" --requests "$REQUESTS"
  done
}

echo "==> Benchmark on host: mcp-cpp vs fastmcp (same add tool)"

echo "### 1. Build mcp-cpp"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMCP_ENABLE_WORKFLOW=ON >/dev/null
cmake --build build -j"$(nproc)" --target mcp_http_example >/dev/null

echo "### 2. Ensure bench dependencies"
"$PYTHON" -m pip install -q aiohttp fastmcp uvicorn

echo "### 3. mcp-cpp (C++/workflow)"
./build/mcp_http_example 127.0.0.1 "$MCPCPP_PORT" >/tmp/mcppp.log 2>&1 &
MCPCPP_PID=$!
wait_for_port "$MCPCPP_PORT" "mcp_http_example"
run_waves "$MCPCPP_PORT"
kill "$MCPCPP_PID" 2>/dev/null || true
MCPCPP_PID=""

echo "### 4. Python SDK (fastmcp)"
"$PYTHON" -m uvicorn benchmark.fastmcp_server:app \
    --host 127.0.0.1 --port "$PY_PORT" >/tmp/py.log 2>&1 &
PY_PID=$!
wait_for_port "$PY_PORT" "uvicorn"
run_waves "$PY_PORT"
kill "$PY_PID" 2>/dev/null || true
PY_PID=""

echo "==> benchmark_local.sh: OK"
