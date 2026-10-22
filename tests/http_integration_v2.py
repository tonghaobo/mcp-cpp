#!/usr/bin/env python3
"""End-to-end v2 (2026-07-28, stateless / no session-id) HTTP integration test.

Speaks raw HTTP against mcp_http_example, exercising the modern stateless path:
requests carry the per-request envelope (params._meta) plus the mirrored
MCP-Protocol-Version / Mcp-Method / Mcp-Name headers, but NO Mcp-Session-Id and
no initialize handshake. Verifies:

   1. server/discover -> 200, capabilities/cacheScope/resultType/
      supportedVersions/ttlMs (v2 shape, no session required)
   2. tools/list -> 200 with cacheScope/resultType/ttlMs (no session)
   3. tools/call add -> 200 with resultType (no session)
   4. ping -> 200 with resultType
   5. unknown method -> 404 JSON-RPC -32601 (status table maps -32601 to 404)
   6. unknown tool -> 200 -32005 (a code outside the status table stays 200)
   7. missing _meta envelope -> 400 -32602
   8. Mcp-Method header disagreeing with the body -> 400 -32020
   9. unserved revision named by the envelope -> 400 -32022
  10. a routing header supplied twice -> 400 -32020

Usage: http_integration_v2.py <path-to-mcp_http_example> [port]
"""

import http.client
import json
import subprocess
import sys
import time

PATH = "/mcp"
V2 = "2026-07-28"
META_VERSION = "io.modelcontextprotocol/protocolVersion"
META_CAPABILITIES = "io.modelcontextprotocol/clientCapabilities"


def request(port, body, headers=None, version=V2, envelope=True):
    """POST one message with the v2 envelope and mirrored routing headers."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    h = {"Accept": "application/json, text/event-stream",
         "Content-Type": "application/json",
         "MCP-Protocol-Version": version}
    if envelope:
        params = body.setdefault("params", {})
        meta = params.setdefault("_meta", {})
        meta[META_VERSION] = version
        meta[META_CAPABILITIES] = {}
        h["Mcp-Method"] = body["method"]
        if isinstance(params.get("name"), str):
            h["Mcp-Name"] = params["name"]
    if headers:
        h.update(headers)
    conn.request("POST", PATH, body=json.dumps(body), headers=h)
    resp = conn.getresponse()
    raw = resp.read().decode()
    conn.close()
    return resp.status, raw


def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/mcp_http_example"
    host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8946

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

        # 1. server/discover (no session, no handshake).
        status, raw = request(port, {
            "jsonrpc": "2.0", "id": 1, "method": "server/discover", "params": {}})
        print(f"[debug] discover status={status} raw={raw!r}", file=sys.stderr)
        assert status == 200, (status, raw)
        result = json.loads(raw)["result"]
        # v2 DiscoverResult has no protocolVersion/serverInfo fields.
        assert "protocolVersion" not in result, result
        assert result["cacheScope"] in ("private", "public"), result
        assert result["resultType"] == "complete", result
        assert "supportedVersions" in result and V2 in result["supportedVersions"], result
        assert result["ttlMs"] == 0, result
        assert result["capabilities"]["tools"] is not None, result

        # 2. tools/list (no session).
        status, raw = request(port, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        assert status == 200, (status, raw)
        r = json.loads(raw)["result"]
        names = {t["name"] for t in r["tools"]}
        assert names == {"echo", "add", "slow_add", "slow_query", "get_time"}, names
        assert r["cacheScope"] in ("private", "public"), r
        assert r["resultType"] == "complete", r
        assert r["ttlMs"] == 0, r

        # 3. tools/call add (no session).
        status, raw = request(port, {
            "jsonrpc": "2.0", "id": 3, "method": "tools/call",
            "params": {"name": "add", "arguments": {"a": 2, "b": 3}}})
        assert status == 200, (status, raw)
        r = json.loads(raw)["result"]
        assert r["resultType"] == "complete", r
        text = r["content"][0]["text"]
        assert json.loads(text)["sum"] == 5, text

        # 4. ping (no session).
        status, raw = request(port, {"jsonrpc": "2.0", "id": 4, "method": "ping"})
        assert status == 200, (status, raw)
        assert json.loads(raw)["result"] == {"resultType": "complete"}, raw

        # 5. unknown method -> -32601 carries HTTP 404 on the modern wire.
        status, raw = request(port, {"jsonrpc": "2.0", "id": 5, "method": "no/such"})
        assert status == 404, (status, raw)
        assert json.loads(raw)["error"]["code"] == -32601, raw

        # 6. a code outside the status table (unknown tool) stays 200.
        status, raw = request(port, {
            "jsonrpc": "2.0", "id": 6, "method": "tools/call",
            "params": {"name": "no_such_tool", "arguments": {}}})
        assert status == 200, (status, raw)
        assert json.loads(raw)["error"]["code"] == -32005, raw

        # 7. missing _meta envelope.
        status, raw = request(port, {"jsonrpc": "2.0", "id": 7, "method": "ping"},
                              envelope=False)
        assert status == 400, (status, raw)
        assert json.loads(raw)["error"]["code"] == -32602, raw

        # 8. Mcp-Method disagreeing with the body's method.
        status, raw = request(port, {"jsonrpc": "2.0", "id": 8, "method": "ping"},
                              headers={"Mcp-Method": "tools/list"})
        assert status == 400, (status, raw)
        assert json.loads(raw)["error"]["code"] == -32020, raw

        # 9. envelope naming a revision this server does not serve.
        status, raw = request(port, {"jsonrpc": "2.0", "id": 9, "method": "ping"},
                              version="2027-01-01")
        assert status == 400, (status, raw)
        err = json.loads(raw)["error"]
        assert err["code"] == -32022, raw
        assert err["data"]["supported"] == [V2], raw

        # 10. routing header supplied twice (raw putheader: dict headers cannot
        # repeat a name).
        payload = json.dumps({
            "jsonrpc": "2.0", "id": 10, "method": "ping",
            "params": {"_meta": {META_VERSION: V2, META_CAPABILITIES: {}}}})
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        conn.putrequest("POST", PATH)
        conn.putheader("Content-Type", "application/json")
        conn.putheader("Content-Length", str(len(payload)))
        conn.putheader("MCP-Protocol-Version", V2)
        conn.putheader("Mcp-Method", "ping")
        conn.putheader("Mcp-Method", "ping")
        conn.endheaders(payload.encode())
        resp = conn.getresponse()
        raw = resp.read().decode()
        conn.close()
        assert resp.status == 400, (resp.status, raw)
        err = json.loads(raw)["error"]
        assert err["code"] == -32020, raw
        assert "mcp-method" in err["message"], raw

        print("http_integration_v2: all 10 steps passed")
        return 0
    except Exception as exc:  # noqa: BLE001
        proc.kill()
        print(f"http_integration_v2 FAILED: {exc}", file=sys.stderr)
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
