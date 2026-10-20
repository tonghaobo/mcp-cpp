#!/usr/bin/env python3
"""Concurrency benchmark for MCP streamable HTTP servers (tools/call).

Scenarios mirror the three execution models of mcp-cpp, plus a fixed mix:
  inline    add                             (no waiting)
  thread    slow_add  {delay_ms}            (handler blocks a worker)
  deferred  slow_query {delay}              (waiting, holds no thread)
  mixed     inline / thread / deferred at a fixed ratio (default 70/20/10)

Fairness rules:
  * the tool sequence is deterministic (round-robin by weight), so both servers
    receive the identical workload;
  * every reply is validated (HTTP 200, JSON-RPC result, no error), and failures
    are reported as an error rate rather than counted as successes.

The load is closed-loop (each wave waits for its own requests), so latency is
subject to coordinated omission. Treat the numbers as server-to-server
comparisons, not as open-loop capacity planning.

Usage:
    bench.py --url http://127.0.0.1:<port>/mcp --concurrency 100 \
             --requests 2000 [--scenario inline|thread|deferred|mixed] \
             [--mix inline=70,thread=20,deferred=10] [--rounds 1]

Flow (v1): initialize -> Mcp-Session-Id; warmup; then waves of `tools/call`
(asyncio.gather) measuring wall time per request.
"""

import argparse
import asyncio
import json
import statistics
import sys
import time

import aiohttp

# kind -> (tool name, arguments). Keep these in sync with the servers under test.
PROFILES = {
    "inline": ("add", {"a": 2, "b": 3}),
    "thread": ("slow_add", {"a": 2, "b": 3, "delay_ms": 10}),
    "deferred": ("slow_query", {"rows": 5, "delay": 1}),
}
DEFAULT_MIX = "inline=70,thread=20,deferred=10"


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--url", required=True, help="MCP endpoint URL")
    p.add_argument("--concurrency", type=int, default=100)
    p.add_argument("--requests", type=int, default=2000)
    p.add_argument("--warmup", type=int, default=100, help="warmup requests")
    p.add_argument("--scenario", default="inline",
                   choices=["inline", "thread", "deferred", "mixed"])
    p.add_argument("--mix", default=DEFAULT_MIX,
                   help="ratio for --scenario mixed, e.g. inline=70,thread=20,deferred=10")
    p.add_argument("--rounds", type=int, default=1, help="repeat the load N times")
    # Backward-compatible single-tool mode: --tool overrides --scenario.
    p.add_argument("--tool", default="", help="call this tool only (overrides --scenario)")
    p.add_argument("--args", default="", help="JSON arguments for --tool")
    return p.parse_args()


def build_plan(args):
    if args.tool:
        tool_args = json.loads(args.args) if args.args else {"a": 2, "b": 3}
        return [("custom", args.tool, tool_args)]

    if args.scenario != "mixed":
        tool, tool_args = PROFILES[args.scenario]
        return [(args.scenario, tool, tool_args)]

    cycle = []
    for item in args.mix.split(","):
        kind, _, weight = item.partition("=")
        kind = kind.strip()
        if kind not in PROFILES:
            raise SystemExit(f"unknown mix kind: {kind!r} (use {', '.join(PROFILES)})")
        cycle.extend([kind] * int(weight))
    if not cycle:
        raise SystemExit("empty --mix")
    return [(kind, *PROFILES[kind]) for kind in cycle]


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


def parse_reply(raw: str) -> dict:
    """Accept a plain JSON body or an SSE-wrapped one (fastmcp)."""
    text = raw.strip()
    if text.startswith("{"):
        return json.loads(text)
    payload = None
    for line in text.splitlines():
        if line.startswith("data:"):
            payload = line[5:].strip()
    if payload is None:
        raise ValueError("no JSON payload in reply")
    return json.loads(payload)


async def call_tool(session, url, sid, kind, tool, tool_args, seq):
    body = {"jsonrpc": "2.0", "id": seq, "method": "tools/call",
            "params": {"name": tool, "arguments": tool_args}}
    headers = {"Accept": "application/json, text/event-stream"}
    if sid:
        headers["Mcp-Session-Id"] = sid
    t0 = time.perf_counter()
    ok = False
    try:
        async with session.post(
            url, json=body, headers=headers,
            timeout=aiohttp.ClientTimeout(total=120),
        ) as resp:
            raw = await resp.text()
            reply = parse_reply(raw)
            ok = resp.status == 200 and "result" in reply and "error" not in reply
    except Exception:  # noqa: BLE001 - failures are data, not crashes
        ok = False
    return kind, time.perf_counter() - t0, ok


def pct(lats, p):
    if not lats:
        return 0.0
    idx = min(len(lats) - 1, int(len(lats) * p / 1000))
    return lats[idx] * 1000


def summarize(records, elapsed, label):
    lats = sorted(r[1] for r in records)
    ok = sum(1 for r in records if r[2])
    errors = len(records) - ok
    qps = len(records) / elapsed if elapsed > 0 else 0.0
    print(f"\n{label}  requests={len(records)}  time={elapsed:.2f}s  "
          f"QPS={qps:.0f}  errors={errors} ({100.0 * errors / max(1, len(records)):.2f}%)")
    if lats:
        print(f"  P50: {pct(lats, 500):.2f} ms  P90: {pct(lats, 900):.2f} ms  "
              f"P99: {pct(lats, 990):.2f} ms  P999: {pct(lats, 999):.2f} ms")
        print(f"  mean: {statistics.mean(lats) * 1000:.2f} ms  "
              f"max: {max(lats) * 1000:.2f} ms")

    by_kind = {}
    for kind, latency, _ in records:
        by_kind.setdefault(kind, []).append(latency)
    if len(by_kind) > 1:
        print("  per kind:")
        for kind in sorted(by_kind):
            arr = sorted(by_kind[kind])
            print(f"    {kind:<9} n={len(arr):<6} P50={pct(arr, 500):7.2f}  "
                  f"P90={pct(arr, 900):7.2f}  P99={pct(arr, 990):7.2f}  "
                  f"max={arr[-1] * 1000:8.2f}")
    return qps


async def main() -> int:
    args = parse_args()
    plan = build_plan(args)
    conn = aiohttp.TCPConnector(limit=args.concurrency * 2, force_close=False)
    async with aiohttp.ClientSession(connector=conn) as session:
        sid = await get_session(session, args.url)
        print(f"session: {sid[:8] or '(none)'}  endpoint: {args.url}")
        print(f"scenario: {args.scenario}  plan: " + ", ".join(
            f"{k}:{t}" for k, t, _ in plan[:3]) + (" ..." if len(plan) > 3 else ""))

        # Warmup uses the same plan, so JIT/cache effects are equal across kinds.
        for i in range(args.warmup):
            kind, tool, tool_args = plan[i % len(plan)]
            await call_tool(session, args.url, sid, kind, tool, tool_args, i)

        seq = 0
        all_records = []
        total_elapsed = 0.0
        for r in range(args.rounds):
            records = []
            remaining = args.requests
            round_start = time.perf_counter()
            while remaining > 0:
                n = min(args.concurrency, remaining)
                batch = []
                for _ in range(n):
                    kind, tool, tool_args = plan[seq % len(plan)]
                    batch.append(call_tool(session, args.url, sid, kind, tool,
                                           tool_args, seq))
                    seq += 1
                records.extend(await asyncio.gather(*batch))
                remaining -= n
            elapsed = time.perf_counter() - round_start
            total_elapsed += elapsed
            all_records.extend(records)
            summarize(records, elapsed,
                      f"round {r + 1}/{args.rounds} concurrency={args.concurrency}")

        if args.rounds > 1:
            print("\n=== all rounds combined ===")
            summarize(all_records, total_elapsed, f"aggregate rounds={args.rounds}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
