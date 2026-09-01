#!/usr/bin/env python3
"""Interop test: modern (v2, 2026-07-28) Python SDK client <-> mcp_http_example.

Verifies the "stateless / no session-id" (v2) support: the modern `Client` API
in both `auto` mode (default) and pinned `mode="2026-07-28"` connects without
a session id or initialize handshake (it uses server/discover), and tool calls
complete. The server's responses carry the v2 cache/resultType fields.

Usage: interop_http_v2.py <path-to-mcp_http_example> [port]
"""

import asyncio
import importlib.metadata
import json
import subprocess
import sys
import time

from mcp import Client


async def check(client, tag: str) -> None:
    print(f"[{tag}] negotiated protocol_version = {client.protocol_version}")
    assert client.protocol_version == "2026-07-28", client.protocol_version
    tools = await client.list_tools()
    names = sorted(t.name for t in tools.tools)
    print(f"[{tag}] tools = {names}")
    assert {"echo", "add", "get_time"} <= set(names), names
    add = await client.call_tool("add", {"a": 2, "b": 3})
    text = add.content[0].text if add.content else ""
    print(f"[{tag}] add result = {text!r}")
    assert json.loads(text)["sum"] == 5, text


async def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/mcp_http_example"
    host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8945
    url = f"http://{host}:{port}/mcp"

    proc = subprocess.Popen([binary, host, str(port)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)
    try:
        deadline = time.time() + 10
        while time.time() < deadline:
            line = proc.stdout.readline().strip()
            if "listening on" in line:
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("server did not start listening")

        print(f"SDK version: {importlib.metadata.version('mcp')}")
        # auto mode: should negotiate to 2026-07-28 now.
        async with Client(url, read_timeout_seconds=15) as client:
            await check(client, "auto")
        # pinned v2 mode.
        async with Client(url, mode="2026-07-28", read_timeout_seconds=15) as client:
            await check(client, "pinned-2026-07-28")
        print("interop_http_v2: PASS")
        return 0
    except Exception as exc:  # noqa: BLE001
        proc.kill()
        print(f"interop_http_v2 FAILED: {type(exc).__name__}: {exc}",
              file=sys.stderr)
        return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
