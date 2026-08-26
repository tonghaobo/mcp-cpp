#!/usr/bin/env python3
"""Benchmark reference server: official Python SDK (fastmcp) over streamable
HTTP.

Exposes the same `add` tool as mcp-cpp's http_server so the two can be compared
fairly. Run with uvicorn:

    uvicorn fastmcp_server:app --host 127.0.0.1 --port <port>
"""

from fastmcp import FastMCP

mcp = FastMCP("mcp-cpp-bench-python")


@mcp.tool()
def add(a: int, b: int) -> int:
    """Add two integers."""
    return a + b


app = mcp.http_app()
