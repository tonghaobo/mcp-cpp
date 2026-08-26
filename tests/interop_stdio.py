#!/usr/bin/env python3
"""Interop test: official Python MCP SDK client <-> mcp_example over stdio.

Verifies the "official Python SDK stdio client interop": initialize /
list_tools / call_tool all succeed, and prints the SDK version + negotiated
protocol version so the pinned kProtocolVersion can be
confirmed against reality.

Usage: interop_stdio.py <path-to-mcp_example>   (SDK must be installed)
"""

import asyncio
import importlib.metadata
import json
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/mcp_example"
    server_params = StdioServerParameters(command=binary, args=[], env=None)

    print(f"SDK version: {importlib.metadata.version('mcp')}")
    async with stdio_client(server_params) as (read, write):
        async with ClientSession(read, write) as session:
            # mcp SDK 2.x exposes snake_case fields (protocol_version, ...).
            init = await session.initialize()
            print(f"negotiated protocolVersion: {init.protocol_version}")
            print(f"serverInfo: {init.server_info}")

            tools = await session.list_tools()
            names = [t.name for t in tools.tools]
            print(f"tools ({len(names)}): {sorted(names)}")
            assert {"echo", "add", "get_time"} <= set(names), names

            echo = await session.call_tool("echo", {"text": "hello from SDK"})
            text = echo.content[0].text if echo.content else ""
            print(f"echo result: {text!r}")
            # The echo handler returns {"text": ...} (no "content"), so the
            # server text-wraps it: content[0].text == '{"text":"hello from SDK"}'.
            assert json.loads(text)["text"] == "hello from SDK", text

            add = await session.call_tool("add", {"a": 2, "b": 3})
            print(f"add result: {add}")
            assert add.is_error is False, add

            print("interop_stdio: PASS")
            return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
