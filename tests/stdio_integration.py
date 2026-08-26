#!/usr/bin/env python3
"""End-to-end stdio integration test for mcp_example.

Speaks raw newline-delimited JSON-RPC 2.0 over stdin/stdout -- no SDK
dependency. Exercises the full happy path plus every error branch:

   1. initialize                -> result with protocolVersion/capabilities/serverInfo
   2. notifications/initialized -> silent (no reply; verified via the next ping)
   3. ping                      -> empty result
   4. tools/list                -> 3 tools
   5. tools/call echo           -> text echoed
   6. tools/call add            -> sum
   7. tools/call unknown tool   -> -32005
   8. unknown method            -> -32601
   9. tools/call bad arguments  -> -32602
  10. invalid JSON              -> -32700 (id null)
  11. stdin EOF                 -> process exits 0

Usage: stdio_integration.py <path-to-mcp_example>
"""

import json
import select
import subprocess
import sys

RECV_TIMEOUT = 10  # seconds


class IntegrationError(AssertionError):
    pass


def send(proc, payload):
    line = payload if isinstance(payload, str) else json.dumps(payload)
    proc.stdin.write(line + "\n")
    proc.stdin.flush()


def recv(proc):
    """Read one JSON line from the server, with a timeout."""
    ready, _, _ = select.select([proc.stdout], [], [], RECV_TIMEOUT)
    if not ready:
        raise IntegrationError("timed out waiting for a response")
    line = proc.stdout.readline()
    if not line:
        raise IntegrationError("server closed stdout unexpectedly")
    return json.loads(line)


def expect_response(reply, req_id):
    assert reply.get("id") == req_id, f"id mismatch: {reply} != {req_id}"
    assert "error" not in reply, f"unexpected error: {reply}"


def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/mcp_example"
    proc = subprocess.Popen(
        [binary],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )

    try:
        # 1. initialize
        send(proc, {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "stdio_integration", "version": "0.0.1"},
            },
        })
        reply = recv(proc)
        expect_response(reply, 1)
        result = reply["result"]
        assert isinstance(result["protocolVersion"], str), result
        assert result["capabilities"]["tools"]["listChanged"] is False, result
        assert result["serverInfo"]["name"] == "mcp-cpp", result
        assert result["serverInfo"]["version"] == "0.1.0", result

        # 2. notifications/initialized must be silent: the very next line we
        #    read must be the response to the ping we send right after.
        send(proc, {"jsonrpc": "2.0", "method": "notifications/initialized"})
        send(proc, {"jsonrpc": "2.0", "id": 2, "method": "ping"})

        # 3. ping
        reply = recv(proc)
        expect_response(reply, 2)
        assert reply["result"] == {}, reply

        # 4. tools/list
        send(proc, {"jsonrpc": "2.0", "id": 3, "method": "tools/list"})
        reply = recv(proc)
        expect_response(reply, 3)
        names = {t["name"] for t in reply["result"]["tools"]}
        assert names == {"echo", "add", "get_time"}, names

        # 5. tools/call echo
        send(proc, {
            "jsonrpc": "2.0",
            "id": 4,
            "method": "tools/call",
            "params": {"name": "echo", "arguments": {"text": "hello"}},
        })
        reply = recv(proc)
        expect_response(reply, 4)
        # The echo handler returns {"text": "hello"} (no "content"), so the
        # server text-wraps it: content[0].text == '{"text":"hello"}'.
        text = reply["result"]["content"][0]["text"]
        assert json.loads(text)["text"] == "hello", text
        assert reply["result"]["isError"] is False, reply

        # 6. tools/call add
        send(proc, {
            "jsonrpc": "2.0",
            "id": 5,
            "method": "tools/call",
            "params": {"name": "add", "arguments": {"a": 2, "b": 3}},
        })
        reply = recv(proc)
        expect_response(reply, 5)
        text = reply["result"]["content"][0]["text"]
        assert json.loads(text)["sum"] == 5, text

        # 7. tools/call unknown tool -> -32005
        send(proc, {
            "jsonrpc": "2.0",
            "id": 6,
            "method": "tools/call",
            "params": {"name": "does_not_exist"},
        })
        reply = recv(proc)
        assert reply["id"] == 6, reply
        assert reply["error"]["code"] == -32005, reply

        # 8. unknown method -> -32601
        send(proc, {"jsonrpc": "2.0", "id": 7, "method": "no/such/method"})
        reply = recv(proc)
        assert reply["id"] == 7, reply
        assert reply["error"]["code"] == -32601, reply

        # 9. tools/call bad arguments -> -32602
        send(proc, {
            "jsonrpc": "2.0",
            "id": 8,
            "method": "tools/call",
            "params": {"name": "add", "arguments": {"a": "not-a-number"}},
        })
        reply = recv(proc)
        assert reply["id"] == 8, reply
        assert reply["error"]["code"] == -32602, reply

        # 10. invalid JSON -> -32700, id null
        send(proc, "{this is not json")
        reply = recv(proc)
        assert reply["id"] is None, reply
        assert reply["error"]["code"] == -32700, reply

        # 11. EOF -> graceful exit with code 0
        proc.stdin.close()
        returncode = proc.wait(timeout=RECV_TIMEOUT)
        assert returncode == 0, f"expected exit 0, got {returncode}"

    except Exception as exc:  # noqa: BLE001 - report and fail the test
        # Kill the server first: stderr.read() only reaches EOF once the
        # process is dead, otherwise it blocks forever on the open pipe.
        proc.kill()
        stderr = ""
        try:
            stderr = proc.stderr.read() if proc.stderr else ""
        except Exception:  # noqa: BLE001 - best-effort diagnostics only
            pass
        print(f"stdio_integration FAILED: {exc}", file=sys.stderr)
        if stderr:
            print(f"server stderr:\n{stderr}", file=sys.stderr)
        return 1

    print("stdio_integration: all 11 steps passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
