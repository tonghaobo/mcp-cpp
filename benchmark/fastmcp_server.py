#!/usr/bin/env python3
"""Benchmark reference server: official Python SDK (fastmcp) over streamable
HTTP.

Exposes the same tools as mcp-cpp's http_server, so the three execution models
can be compared one to one:

    add         pure computation            <-> mcp-cpp kInline
    slow_add    time.sleep in a sync tool   <-> mcp-cpp kThread
    slow_query  await asyncio.sleep         <-> mcp-cpp kDeferred

The sync tool runs in fastmcp's worker-thread pool; the async one holds no
thread while waiting. Run with uvicorn:

    uvicorn fastmcp_server:app --host 127.0.0.1 --port <port>
"""

import asyncio
import time

from fastmcp import FastMCP

mcp = FastMCP("mcp-cpp-bench-python")


@mcp.tool()
def add(a: int, b: int) -> int:
    """Add two integers."""
    return a + b


@mcp.tool()
def slow_add(a: int, b: int, delay_ms: int = 10) -> int:
    """Add two integers after blocking a worker thread (kThread analogue)."""
    time.sleep(max(0, delay_ms) / 1000.0)
    return a + b


@mcp.tool()
async def slow_query(rows: int = 5, delay: int = 1) -> dict:
    """Wait without holding a thread (kDeferred analogue)."""
    await asyncio.sleep(max(0, delay))
    return {"rows": rows, "delay": delay}


app = mcp.http_app()
