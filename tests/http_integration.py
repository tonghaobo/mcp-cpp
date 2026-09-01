#!/usr/bin/env python3
"""End-to-end Streamable HTTP integration test for mcp_http_example.

Speaks raw HTTP against the MCP endpoint -- no SDK dependency. Exercises the
happy path (v1 session form) plus error branches:

   1. initialize                       -> 200 JSON + Mcp-Session-Id header
   2. tools/list without session       -> 400 (session required)
   3. tools/list with session          -> 200 JSON, 3 tools
   4. tools/call add                   -> 200 JSON, sum
   5. tools/call unknown tool          -> 200 JSON, -32005
   6. unknown method                   -> 200 JSON, -32601
   7. notification (no id)             -> 202 Accepted, no body
   8. bad JSON body                    -> 200 JSON, -32700
   9. DELETE session                   -> 200; then same session -> 400
  10. GET /mcp                         -> 200 text/event-stream

Usage: http_integration.py <path-to-mcp_http_example> [port]
"""

import http.client
import json
import subprocess
import sys
import time

PATH = "/mcp"
RECV_TIMEOUT = 10


def request(port, body=None, method="POST", headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=RECV_TIMEOUT)
    h = {"Accept": "application/json, text/event-stream",
         "Content-Type": "application/json"}
    if headers:
        h.update(headers)
    data = None if body is None else json.dumps(body)
    conn.request(method, PATH, body=data, headers=h)
    resp = conn.getresponse()
    raw = resp.read().decode()
    conn.close()
    return resp.status, dict(resp.getheaders()), raw


def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/mcp_http_example"
    host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8932

    proc = subprocess.Popen([binary, host, str(port)],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
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

        # 1. initialize -> 200 JSON + Mcp-Session-Id
        status, headers, raw = request(port, {
            "jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-06-18", "capabilities": {},
                "clientInfo": {"name": "http_integration", "version": "0.0.1"}}})
        assert status == 200, (status, raw)
        sid = headers.get("mcp-session-id") or headers.get("Mcp-Session-Id")
        assert sid, headers
        reply = json.loads(raw)
        assert reply["id"] == 1
        assert reply["result"]["protocolVersion"] == "2025-06-18", reply
        assert reply["result"]["serverInfo"]["name"] == "mcp-cpp", reply

        # 2. tools/list without session -> 400
        status, _, raw = request(port, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        assert status == 400, (status, raw)

        # 3. tools/list with session -> 200, 4 tools
        status, _, raw = request(port, {"jsonrpc": "2.0", "id": 3, "method": "tools/list"},
                                 headers={"Mcp-Session-Id": sid})
        assert status == 200, (status, raw)
        names = {t["name"] for t in json.loads(raw)["result"]["tools"]}
        assert names == {"echo", "add", "slow_add", "get_time"}, names

        # 4. tools/call add
        status, _, raw = request(port, {
            "jsonrpc": "2.0", "id": 4, "method": "tools/call",
            "params": {"name": "add", "arguments": {"a": 2, "b": 3}}},
            headers={"Mcp-Session-Id": sid})
        assert status == 200, (status, raw)
        text = json.loads(raw)["result"]["content"][0]["text"]
        assert json.loads(text)["sum"] == 5, text

        # 4b. tools/call slow_add (async tool -> dispatched to a compute thread)
        status, _, raw = request(port, {
            "jsonrpc": "2.0", "id": 40, "method": "tools/call",
            "params": {"name": "slow_add", "arguments": {"a": 4, "b": 6}}},
            headers={"Mcp-Session-Id": sid})
        assert status == 200, (status, raw)
        text = json.loads(raw)["result"]["content"][0]["text"]
        assert json.loads(text)["sum"] == 10, text

        # 5. tools/call unknown tool -> -32005
        status, _, raw = request(port, {
            "jsonrpc": "2.0", "id": 5, "method": "tools/call",
            "params": {"name": "nope"}},
            headers={"Mcp-Session-Id": sid})
        assert status == 200, (status, raw)
        assert json.loads(raw)["error"]["code"] == -32005, raw

        # 6. unknown method -> -32601
        status, _, raw = request(port, {"jsonrpc": "2.0", "id": 6, "method": "no/such"},
                                 headers={"Mcp-Session-Id": sid})
        assert status == 200, (status, raw)
        assert json.loads(raw)["error"]["code"] == -32601, raw

        # 7. notification -> 202, empty body
        status, _, raw = request(port, {"jsonrpc": "2.0", "method": "notifications/initialized"},
                                 headers={"Mcp-Session-Id": sid})
        assert status == 202, (status, raw)
        assert raw == "", raw

        # 8. bad JSON -> -32700
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=RECV_TIMEOUT)
        conn.request("POST", PATH, body="{not json",
                     headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        raw = resp.read().decode()
        conn.close()
        assert resp.status == 200, resp.status
        assert json.loads(raw)["error"]["code"] == -32700, raw

        # 9. DELETE session; then reuse -> 400
        status, _, _ = request(port, method="DELETE", headers={"Mcp-Session-Id": sid})
        assert status == 200, (status,)
        status, _, _ = request(port, {"jsonrpc": "2.0", "id": 7, "method": "ping"},
                               headers={"Mcp-Session-Id": sid})
        assert status == 400, (status,)

        # 10. GET /mcp -> SSE
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=RECV_TIMEOUT)
        conn.request("GET", PATH, headers={"Accept": "text/event-stream"})
        resp = conn.getresponse()
        ctype = resp.getheader("Content-Type", "")
        body = resp.read().decode()
        conn.close()
        assert resp.status == 200, resp.status
        assert ctype.startswith("text/event-stream"), ctype
        assert body.startswith(": keep-alive"), body

        print("http_integration: all 10 steps passed")
        return 0
    except Exception as exc:  # noqa: BLE001 - report and fail
        proc.kill()
        stderr = ""
        try:
            stderr = proc.stderr.read() if proc.stderr else ""
        except Exception:  # noqa: BLE001 - best-effort diagnostics
            pass
        print(f"http_integration FAILED: {exc}", file=sys.stderr)
        if stderr:
            print(f"server stderr:\n{stderr}", file=sys.stderr)
        return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    sys.exit(main())
