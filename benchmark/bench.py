#!/usr/bin/env python3
"""Concurrency benchmark for MCP streamable HTTP servers (tools/call).

Loads a target server with concurrent tools/call requests and reports latency
percentiles and QPS. Speaks raw HTTP (v1 session form) so it can drive both
mcp-cpp (JSON responses) and the Python fastmcp server (SSE-wrapped JSON) for a
fair comparison.

Usage:
    bench.py --url http://127.0.0.1:<port>/mcp \
             --concurrency 100 --requests 2000 [--warmup 100]

Flow (v1): initialize -> Mcp-Session-Id; then emit concurrency waves of
tools/call (batched asyncio.gather, the pattern proven reliable for fastmcp)
and measure wall time per request.
"""

import argparse
import asyncio
import json
import statistics
import sys
import time

import aiohttp


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--url", required=True, help="MCP endpoint URL")
    p.add_argument("--concurrency", type=int, default=100)
    p.add_argument("--requests", type=int, default=2000)
    p.add_argument("--warmup", type=int, default=100, help="warmup requests")
    p.add_argument("--tool", default="add", help="tool name to call")
    return p.parse_args()


async def get_session(session, url):
    body = {"jsonrpc": "2.0", "id": 0, "method": "initialize",
            "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                       "clientInfo": {"name": "bench", "version": "0"}}}
    async with session.post(
        url, json=body,
        headers={"Accept": "application/json, text/event-stream"},
        timeout=aiohttp.ClientTimeout(total=15),
    ) as resp:
        await resp.read()
        return resp.headers.get("Mcp-Session-Id", "")


async def call_tool(session, url, sid, tool, seq):
    body = {"jsonrpc": "2.0", "id": seq, "method": "tools/call",
            "params": {"name": tool, "arguments": {"a": 2, "b": 3}}}
    headers = {"Accept": "application/json, text/event-stream"}
    if sid:
        headers["Mcp-Session-Id"] = sid
    t0 = time.perf_counter()
    async with session.post(
        url, json=body, headers=headers,
        timeout=aiohttp.ClientTimeout(total=30)) as resp:
        await resp.read()
    return time.perf_counter() - t0


async def main() -> int:
    args = parse_args()
    conn = aiohttp.TCPConnector(limit=args.concurrency * 2, force_close=False)
    async with aiohttp.ClientSession(connector=conn) as session:
        sid = await get_session(session, args.url)
        print(f"session: {sid[:8] or '(none)'}  endpoint: {args.url}")

        # Warmup.
        for i in range(args.warmup):
            await call_tool(session, args.url, sid, args.tool, i)

        # Load in waves of `concurrency` concurrent requests (reliable pattern).
        lats = []
        start = time.perf_counter()
        remaining = args.requests
        seq = 0
        while remaining > 0:
            n = min(args.concurrency, remaining)
            lats.extend(await asyncio.gather(
                *[call_tool(session, args.url, sid, args.tool, seq + i)
                  for i in range(n)]))
            seq += n
            remaining -= n
        elapsed = time.perf_counter() - start

    done = len(lats)
    lats.sort()
    qps = done / elapsed
    print(f"\nconcurrency={args.concurrency}  requests={done}  time={elapsed:.2f}s  QPS={qps:.0f}")
    for p in (50, 90, 99, 999):
        idx = min(len(lats) - 1, int(len(lats) * p / 1000))
        print(f"  P{p}: {lats[idx]*1000:.2f} ms")
    print(f"  mean: {statistics.mean(lats)*1000:.2f} ms  max: {max(lats)*1000:.2f} ms")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
