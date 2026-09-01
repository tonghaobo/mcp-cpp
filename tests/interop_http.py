#!/usr/bin/env python3
"""Interop test: official Python MCP SDK client <-> mcp_http_example over
Streamable HTTP.

Verifies the "official Python SDK streamable-http client interop" (v1 session
form): spawns mcp_http_example, connects via streamable_http_client,
and runs initialize / list_tools / call_tool.

Usage: interop_http.py <path-to-mcp_http_example> [host] [port]   (SDK must be installed)
"""

import asyncio
import importlib.metadata
import json
import subprocess
import sys
import time

from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client


async def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/mcp_http_example"
    host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8931
    url = f"http://{host}:{port}/mcp"

    proc = subprocess.Popen([binary, host, str(port)],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            text=True)
    try:
        # Wait for the server to report listening.
        deadline = time.time() + 10
        while time.time() < deadline:
            line = proc.stdout.readline().strip()
            if "listening on" in line:
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("server did not start listening")

        print(f"SDK version: {importlib.metadata.version('mcp')}")
        async with streamable_http_client(url) as (read, write):
            async with ClientSession(read, write) as session:
                init = await session.initialize()
                print(f"negotiated protocolVersion: {init.protocol_version}")
                print(f"serverInfo: {init.server_info}")
                assert init.protocol_version.startswith("2025-"), init.protocol_version

                tools = await session.list_tools()
                names = sorted(t.name for t in tools.tools)
                print(f"tools: {names}")
                assert {"echo", "add", "get_time"} <= set(names), names

                add = await session.call_tool("add", {"a": 2, "b": 3})
                text = add.content[0].text if add.content else ""
                print(f"add result: {text!r}")
                assert json.loads(text)["sum"] == 5, text

                echo = await session.call_tool("echo", {"text": "hi over http"})
                t = echo.content[0].text if echo.content else ""
                assert json.loads(t)["text"] == "hi over http", t

                print("interop_http: PASS")
                return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
