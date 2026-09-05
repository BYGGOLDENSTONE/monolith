"""Real stdio/HTTP integration checks; no editor, pip packages, or live assets needed.

Run: python -m unittest discover -s Scripts/tests -v
Set MONOLITH_TEST_NATIVE_PROXY to a built Windows proxy to run the same cases
against it. Every test owns its server, subprocesses, and temporary cache.
"""

import json
import importlib.util
import os
from pathlib import Path
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def windows_cache_handle(path, access):
    """Open a rename-compatible Win32 handle; caller must close or transfer it."""
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    create = kernel.CreateFileW
    create.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    create.restype = wintypes.HANDLE
    close = kernel.CloseHandle
    close.argtypes = (wintypes.HANDLE,)
    close.restype = wintypes.BOOL
    handle = create(str(path), access, 1 | 2 | 4, None, 3, 0x80, None)
    if handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    return handle, close


def read_cache_snapshot(path):
    """Observe atomic replacement using Windows rename-compatible sharing.

    CRT open() denies DELETE sharing and can fail even after a complete snapshot
    has been renamed into place. This reader does not retry or ignore invalid JSON.
    """
    if os.name != "nt":
        return path.read_text(encoding="utf-8")
    import msvcrt
    handle, close = windows_cache_handle(path, 0x80000000)  # GENERIC_READ
    try:
        descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
    except Exception:
        close(handle)
        raise
    with os.fdopen(descriptor, "r", encoding="utf-8") as stream:
        return stream.read()


ROOT = Path(__file__).resolve().parents[2]
WAIT = 10
TOOLS = [{"name": "fixture_query", "description": "transport fixture",
          "inputSchema": {"type": "object"}}]


def request(identifier, method="tools/call", **arguments):
    msg = {"jsonrpc": "2.0", "id": identifier, "method": method}
    if method == "tools/call":
        msg["params"] = {"name": "fixture_query", "arguments": arguments}
    return msg


class EditorFixture:
    def __init__(self):
        self.condition = threading.Condition()
        self.received = []
        self.gates = {}
        self.offline = False
        self.health_entered = threading.Event()
        self.health_gate = threading.Event()
        self.health_gate.set()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def reply(self, payload, status=200):
                body = json.dumps(payload).encode("utf-8")
                try:
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass  # Expected after a transport timeout.

            def do_GET(self):
                owner.health_entered.set()
                owner.health_gate.wait(WAIT)
                self.reply({"status": "ok"})

            def do_POST(self):
                msg = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                with owner.condition:
                    owner.received.append(msg)
                    owner.condition.notify_all()
                identifier = msg["id"]
                gate = owner.gates.get(identifier)
                if gate is not None:
                    gate.wait(WAIT)
                if owner.offline:
                    self.reply({"offline": True}, 503)
                    return
                mode = msg.get("params", {}).get("arguments", {}).get("fixture_mode")
                if mode == "redirect" and "replayed" not in self.path:
                    self.send_response(307)
                    self.send_header("Location", owner.url + "?replayed=1")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if mode == "drop":
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
                    return
                if mode == "wrong_id":
                    identifier = "stale-unrelated-id"
                if mode == "wrong_id_type":
                    identifier = True  # Python True == 1 must not bypass ID validation.
                if mode == "invalid":
                    self.reply(["not a JSON-RPC response"])
                    return
                payload = {"jsonrpc": "2.0", "id": identifier,
                           "result": {"tools": TOOLS} if msg["method"] == "tools/list"
                           else {"content": [{"type": "text", "text": str(identifier)}]}}
                if mode == "both_result_error":
                    payload["error"] = {"code": -32603, "message": "contradictory"}
                if mode in ("invalid_error", "valid_error"):
                    payload.pop("result")
                    payload["error"] = ("not an error object" if mode == "invalid_error" else
                                        {"code": -32001, "message": "Editor queue full"})
                self.reply(payload)

        class FixtureHTTPServer(ThreadingHTTPServer):
            request_queue_size = 64
            daemon_threads = True

        self.server = FixtureHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = "http://127.0.0.1:%d/mcp" % self.server.server_port

    def block(self, identifier):
        self.gates[identifier] = threading.Event()
        return self.gates[identifier]

    def wait_received(self, identifiers):
        with self.condition:
            ok = self.condition.wait_for(
                lambda: set(identifiers).issubset({m["id"] for m in self.received}), WAIT)
        if not ok:
            raise AssertionError("Upstream requests never arrived: %r; received %r" %
                                 (identifiers, self.received))

    def release(self):
        self.health_gate.set()
        for gate in self.gates.values():
            gate.set()

    def close(self):
        self.release()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(WAIT)


class ProxyProcess:
    def __init__(self, command, editor, directory, **settings):
        env = os.environ.copy()
        env.update({"MONOLITH_URL": editor.url, "MONOLITH_CALL_LOG": "0",
                    "PYTHONIOENCODING": "utf-8",
                    "MONOLITH_MAX_IN_FLIGHT": "8", "MONOLITH_MAX_QUEUED": "64",
                    "MONOLITH_TIMEOUT_SECONDS": "5", "LOCALAPPDATA": directory,
                    "TMPDIR": directory, "TEMP": directory, "TMP": directory})
        env.update({k: str(v) for k, v in settings.items()})
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, encoding="utf-8",
                                        env=env, cwd=directory)
        self.messages = queue.Queue()
        self.stderr = []
        self.readers = [threading.Thread(target=self.read_stdout, daemon=True),
                        threading.Thread(target=self.read_stderr, daemon=True)]
        for reader in self.readers:
            reader.start()

    def read_stdout(self):
        for line in self.process.stdout:
            try:
                self.messages.put(json.loads(line))
            except ValueError:
                self.messages.put({"invalid_stdout": line})

    def read_stderr(self):
        self.stderr.extend(self.process.stderr)

    def send(self, msg):
        self.raw(json.dumps(msg))

    def raw(self, line):
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def receive(self):
        try:
            while True:
                message = self.messages.get(timeout=WAIT)
                if "method" not in message:  # health notifications are out of band
                    return message
        except queue.Empty:
            raise AssertionError("Proxy response timed out; exit=%r; stderr=%s" %
                                 (self.process.poll(), "".join(self.stderr)))

    def eof(self):
        if not self.process.stdin.closed:
            self.process.stdin.close()

    def finish(self):
        self.eof()
        try:
            code = self.process.wait(WAIT)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(WAIT)
            raise AssertionError("Proxy did not exit after EOF: " + "".join(self.stderr))
        finally:
            for reader in self.readers:
                reader.join(WAIT)
            self.process.stdout.close()
            self.process.stderr.close()
        if code:
            raise AssertionError("Proxy exited %d: %s" % (code, "".join(self.stderr)))


class TransportContract:
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="monolith-transport-")
        self.editor = EditorFixture()
        self.proxies = []

    def tearDown(self):
        self.editor.release()
        failures = []
        try:
            for proxy in self.proxies:
                try:
                    proxy.finish()
                except Exception as exc:
                    failures.append(exc)
        finally:
            self.editor.close()
            self.directory.cleanup()
        if failures:
            raise failures[0]

    def proxy(self, **settings):
        result = ProxyProcess(self.command, self.editor, self.directory.name, **settings)
        self.proxies.append(result)
        return result

    def assert_tool_unknown(self, response, identifier):
        self.assertEqual(response["id"], identifier)
        self.assertTrue(response.get("result", {}).get("isError"), response)
        self.assertIn("unknown", json.dumps(response).lower())

    def test_identical_calls_keep_every_request_and_id(self):
        proxy = self.proxy()
        identifiers = list(range(16)) + ["unicode-ö-雪", "1"]
        for identifier in identifiers:
            proxy.send(request(identifier, action="same_mutation", value=42))
        responses = [proxy.receive() for _ in identifiers]
        self.assertEqual({m["id"] for m in responses}, set(identifiers))
        self.assertTrue(all("result" in m for m in responses), responses)
        self.assertTrue(all(not m["result"].get("isError") for m in responses), responses)
        self.assertCountEqual([m["id"] for m in self.editor.received], identifiers)

    def test_slow_request_does_not_block_fast_request_or_ping(self):
        proxy = self.proxy(MONOLITH_MAX_IN_FLIGHT=2)
        slow = self.editor.block("slow")
        proxy.send(request("slow"))
        self.editor.wait_received(["slow"])
        proxy.send(request("fast"))
        self.assertEqual(proxy.receive()["id"], "fast")
        proxy.send(request("ping", "ping"))
        self.assertEqual(proxy.receive(), {"jsonrpc": "2.0", "id": "ping", "result": {}})
        slow.set()
        self.assertEqual(proxy.receive()["id"], "slow")

    def test_bounded_capacity_rejects_without_execution(self):
        proxy = self.proxy(MONOLITH_MAX_IN_FLIGHT=2, MONOLITH_MAX_QUEUED=0)
        for identifier in (1, 2):
            self.editor.block(identifier)
            proxy.send(request(identifier))
        self.editor.wait_received([1, 2])
        proxy.send(request(3))
        response = proxy.receive()
        self.assertEqual((response["id"], response["error"]["code"]), (3, -32001))
        proxy.send(request(4, "ping"))
        self.assertEqual(proxy.receive()["id"], 4)
        self.editor.release()
        self.assertEqual({proxy.receive()["id"], proxy.receive()["id"]}, {1, 2})
        self.assertEqual({m["id"] for m in self.editor.received}, {1, 2})

    def test_duplicate_active_id_is_rejected_without_execution(self):
        proxy = self.proxy()
        gate = self.editor.block("same-id")
        proxy.send(request("same-id"))
        self.editor.wait_received(["same-id"])
        proxy.send(request("same-id"))
        self.assertEqual(proxy.receive()["error"]["code"], -32600)
        gate.set()
        self.assertIn("result", proxy.receive())
        self.assertEqual(len(self.editor.received), 1)

    def test_bad_input_does_not_kill_stdin_loop(self):
        proxy = self.proxy()
        cases = [("{broken", -32700), ("[]", -32600), ("null", -32600),
                 ("17", -32600), (json.dumps({"method": "ping", "id": 1}), -32600)]
        for line, code in cases:
            proxy.raw(line)
            self.assertEqual(proxy.receive()["error"]["code"], code)
        for params in ([], None, {"name": 17}, {"name": "x", "arguments": []},
                       {"name": "x", "arguments": None}):
            proxy.send({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": params})
            self.assertEqual(proxy.receive()["error"]["code"], -32602)
        proxy.send(request("alive", "ping"))
        self.assertEqual(proxy.receive()["id"], "alive")
        self.assertEqual(self.editor.received, [])

    def test_malformed_initialize_does_not_crash(self):
        proxy = self.proxy()
        for version in ({}, [], None, 12):
            proxy.send({"jsonrpc": "2.0", "id": "init", "method": "initialize",
                        "params": {"protocolVersion": version}})
            response = proxy.receive()
            self.assertEqual(response["id"], "init")
            if "error" in response:
                self.assertEqual(response["error"]["code"], -32602)
            else:
                self.assertIsInstance(response["result"]["protocolVersion"], str)
        proxy.send(request("alive", "ping"))
        self.assertEqual(proxy.receive()["id"], "alive")

    def test_notifications_are_silent_and_never_execute_tools(self):
        proxy = self.proxy()
        for method in ("tools/call", "tools/list", "ping", "notifications/cancelled"):
            msg = request(1, method)
            msg.pop("id")
            proxy.send(msg)
        proxy.send(request("barrier", "ping"))
        self.assertEqual(proxy.receive()["id"], "barrier")
        proxy.eof()
        proxy.process.wait(WAIT)
        for reader in proxy.readers:
            reader.join(WAIT)
        self.assertTrue(proxy.messages.empty())
        self.assertEqual(self.editor.received, [])

    def test_bad_upstream_responses_are_unknown_outcomes_without_replay(self):
        proxy = self.proxy()
        for index, mode in enumerate(("wrong_id", "invalid", "both_result_error", "invalid_error")):
            proxy.send(request(index, fixture_mode=mode))
            self.assert_tool_unknown(proxy.receive(), index)
        self.assertEqual(len(self.editor.received), 4)

    def test_boolean_upstream_id_cannot_match_integer_request(self):
        proxy = self.proxy()
        proxy.send(request(1, fixture_mode="wrong_id_type"))
        self.assert_tool_unknown(proxy.receive(), 1)
        self.assertEqual(len(self.editor.received), 1)

    def test_valid_upstream_protocol_error_is_preserved(self):
        proxy = self.proxy()
        proxy.send(request("upstream-error", fixture_mode="valid_error"))
        self.assertEqual(proxy.receive(), {"jsonrpc": "2.0", "id": "upstream-error",
                                          "error": {"code": -32001, "message": "Editor queue full"}})
        self.assertEqual(len(self.editor.received), 1)

    def test_dropped_connection_is_unknown_outcome_without_replay(self):
        proxy = self.proxy()
        proxy.send(request("drop", fixture_mode="drop"))
        self.assert_tool_unknown(proxy.receive(), "drop")
        proxy.send(request("still-alive", "ping"))
        self.assertEqual(proxy.receive()["id"], "still-alive")
        self.assertEqual(len(self.editor.received), 1)

    def test_post_redirect_is_not_followed_or_replayed(self):
        proxy = self.proxy()
        proxy.send(request("redirect", fixture_mode="redirect"))
        self.assert_tool_unknown(proxy.receive(), "redirect")
        self.assertEqual(len(self.editor.received), 1)

    def test_timeout_is_unknown_outcome_without_replay(self):
        proxy = self.proxy(MONOLITH_TIMEOUT_SECONDS=1)
        self.editor.block("timeout")
        proxy.send(request("timeout"))
        self.editor.wait_received(["timeout"])
        self.assert_tool_unknown(proxy.receive(), "timeout")
        self.assertEqual(len(self.editor.received), 1)

    def test_offline_seed_tools(self):
        self.editor.offline = True
        proxy = self.proxy()
        proxy.send(request("offline", "tools/list"))
        tools = proxy.receive()["result"]["tools"]
        self.assertIn("monolith_discover", {tool["name"] for tool in tools})
        self.assertTrue(all(isinstance(tool.get("inputSchema"), dict) for tool in tools))

    def test_concurrent_tools_cache_is_complete_and_survives_restart(self):
        proxies = [self.proxy(), self.proxy()]
        proxies[0].send(request("seed-cache", "tools/list"))
        self.assertEqual(proxies[0].receive()["result"]["tools"], TOOLS)
        cache_files = list(Path(self.directory.name).rglob("*.json"))
        self.assertTrue(cache_files, "No persistent tools/list cache written")
        observed = threading.Event()
        stop = threading.Event()
        failures = []

        def inspect_cache():
            while not stop.is_set():
                try:
                    for path in cache_files:
                        self.assertEqual(json.loads(read_cache_snapshot(path)), TOOLS)
                    observed.set()
                except Exception as exc:
                    failures.append(exc)
                    return
                stop.wait(0.001)

        reader = threading.Thread(target=inspect_cache, daemon=True)
        reader.start()
        try:
            self.assertTrue(observed.wait(WAIT), "Cache reader never observed a valid cache")
            for index, proxy in enumerate(proxies):
                for number in range(12):
                    proxy.send(request("%d-%d" % (index, number), "tools/list"))
            for proxy in proxies:
                for _ in range(12):
                    self.assertEqual(proxy.receive()["result"]["tools"], TOOLS)
        finally:
            stop.set()
            reader.join(WAIT)
        self.assertEqual(failures, [], "Cache readers observed a partial/invalid document")
        self.editor.offline = True
        restarted = self.proxy()
        restarted.send(request("cached", "tools/list"))
        self.assertEqual(restarted.receive()["result"]["tools"], TOOLS)

    def test_eof_drains_accepted_workers(self):
        proxy = self.proxy(MONOLITH_MAX_IN_FLIGHT=2)
        gate = self.editor.block("drain")
        proxy.send(request("drain"))
        self.editor.wait_received(["drain"])
        proxy.eof()
        self.assertIsNone(proxy.process.poll())
        gate.set()
        self.assertEqual(proxy.receive()["id"], "drain")
        self.assertEqual(proxy.process.wait(WAIT), 0)

    def test_eof_drains_queued_work_and_queue_bound_includes_running(self):
        proxy = self.proxy(MONOLITH_MAX_IN_FLIGHT=1, MONOLITH_MAX_QUEUED=1)
        gate = self.editor.block("running")
        proxy.send(request("running"))
        self.editor.wait_received(["running"])
        proxy.send(request("queued"))
        proxy.send(request("overflow"))
        response = proxy.receive()
        self.assertEqual((response["id"], response["error"]["code"]), ("overflow", -32001))
        proxy.eof()
        gate.set()
        self.assertEqual({proxy.receive()["id"], proxy.receive()["id"]}, {"running", "queued"})
        self.assertEqual(proxy.process.wait(WAIT), 0)
        self.assertCountEqual([m["id"] for m in self.editor.received], ["running", "queued"])

    def test_eof_stops_active_health_poll(self):
        self.editor.health_gate.clear()
        proxy = self.proxy()
        proxy.send(request("alive", "ping"))
        self.assertEqual(proxy.receive()["id"], "alive")
        self.assertTrue(self.editor.health_entered.wait(WAIT), "Health poll never started")
        proxy.eof()
        self.editor.health_gate.set()
        self.assertEqual(proxy.process.wait(WAIT), 0)


class PythonTransportTests(TransportContract, unittest.TestCase):
    command = [sys.executable, str(ROOT / "Scripts" / "monolith_proxy.py")]


@unittest.skipUnless(os.environ.get("MONOLITH_TEST_NATIVE_PROXY"),
                     "Set MONOLITH_TEST_NATIVE_PROXY to run native transport parity")
class NativeTransportTests(TransportContract, unittest.TestCase):
    command = [os.environ.get("MONOLITH_TEST_NATIVE_PROXY", "monolith_proxy.exe")]

    @unittest.skipUnless(os.name == "nt", "Win32 DELETE sharing contract")
    def test_cache_can_be_read_while_rename_delete_handle_is_open(self):
        proxy = self.proxy()
        proxy.send(request("seed", "tools/list"))
        self.assertEqual(proxy.receive()["result"]["tools"], TOOLS)
        cache_files = list(Path(self.directory.name).rglob("*.json"))
        self.assertEqual(len(cache_files), 1)
        # Deterministically hold the access right MoveFileEx retains after rename.
        # A CRT ifstream reader fails for the whole interval; the shared reader
        # must succeed immediately, without waiting for this test to close it.
        handle, close = windows_cache_handle(cache_files[0], 0x00010000)  # DELETE
        try:
            self.editor.offline = True
            proxy.send(request("held-delete-handle", "tools/list"))
            self.assertEqual(proxy.receive()["result"]["tools"], TOOLS)
            self.assertEqual(json.loads(read_cache_snapshot(cache_files[0])), TOOLS)
        finally:
            close(handle)

    def test_split_editor_read_tool_blocks_mutations_but_allows_diagnostics(self):
        proxy = self.proxy(MONOLITH_SPLIT_EDITOR_QUERY=1)
        for action in ("spawn_actor", "trigger_build"):
            msg = request(action, action=action)
            msg["params"]["name"] = "editor_read_query"
            proxy.send(msg)
            response = proxy.receive()
            self.assertEqual(response["id"], action)
            self.assertTrue(response["result"].get("isError"), response)
        self.assertEqual(self.editor.received, [])
        msg = request("diagnostic", action="get_build_status")
        msg["params"]["name"] = "editor_read_query"
        proxy.send(msg)
        response = proxy.receive()
        self.assertEqual(response["id"], "diagnostic")
        self.assertFalse(response["result"].get("isError"), response)
        self.assertEqual(len(self.editor.received), 1)
        self.assertEqual(self.editor.received[0]["params"]["name"], "editor_query")


class PythonCacheReadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("monolith_cache_contract",
                                                     ROOT / "Scripts" / "monolith_proxy.py")
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def test_health_ignores_environment_proxy(self):
        editor = EditorFixture()
        self.addCleanup(editor.close)
        with mock.patch.dict(os.environ, {"HTTP_PROXY": "http://127.0.0.1:1",
                                          "HTTPS_PROXY": "http://127.0.0.1:1",
                                          "NO_PROXY": "", "no_proxy": ""}), \
                mock.patch.object(self.module, "MONOLITH_HEALTH", editor.url.replace("/mcp", "/health")):
            self.assertTrue(self.module._check_monolith_up())

    def test_health_timeout_preserves_state_and_refusal_marks_down(self):
        import io
        import urllib.error
        for error, expected in ((TimeoutError(), None),
                                (urllib.error.URLError(TimeoutError()), None),
                                (urllib.error.URLError(ConnectionRefusedError()), False)):
            opener = mock.Mock()
            opener.open.side_effect = error
            with mock.patch.object(self.module, "_direct_opener", return_value=opener):
                self.assertIs(self.module._check_monolith_up(), expected)
        output = io.StringIO()
        with mock.patch.object(self.module, "_monolith_was_up", True), \
                mock.patch.object(self.module, "_check_monolith_up", return_value=None):
            self.module.check_monolith_state_change(output)
            self.assertTrue(self.module._monolith_was_up)
            self.assertEqual(output.getvalue(), "")

    def test_health_redirect_is_not_followed(self):
        import urllib.request
        handler = self.module._NoRedirect()
        self.assertIsNone(handler.redirect_request(
            urllib.request.Request("http://localhost/health"), None, 302, "redirect", {},
            "http://elsewhere/health"))

    def cache_read(self, outcomes):
        path = mock.Mock()
        path.exists.return_value = True
        path.read_text.side_effect = outcomes
        with mock.patch.object(self.module, "_tools_cache_path", return_value=path), \
                mock.patch.object(self.module.time, "sleep") as sleep, \
                mock.patch.object(self.module, "_log"):
            result = self.module._read_tools_cache()
        return result, path.read_text.call_count, sleep.call_count

    def test_transient_permission_failure_retries_only_cache_read(self):
        result, reads, sleeps = self.cache_read([PermissionError("sharing violation"), json.dumps(TOOLS)])
        self.assertEqual((result, reads, sleeps), (TOOLS, 2, 1))

    def test_permission_retry_is_bounded(self):
        result, reads, sleeps = self.cache_read([PermissionError("denied")] * 3)
        self.assertEqual((result, reads, sleeps), (None, 3, 2))

    def test_invalid_json_is_not_retried(self):
        result, reads, sleeps = self.cache_read(["{truncated"])
        self.assertEqual((result, reads, sleeps), (None, 1, 0))


if __name__ == "__main__":
    unittest.main()
