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
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid
from datetime import datetime, timezone
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


def server_instructions():
    source = (ROOT / "Source/MonolithCore/Private/MonolithHttpServer.cpp").read_text(encoding="utf-8")
    block = re.search(r'Result->SetStringField\(TEXT\("instructions"\),(.*?)\);\s*\n',
                      source, re.DOTALL).group(1)
    return "".join(json.loads('"' + part + '"')
                   for part in re.findall(r'TEXT\("((?:\\.|[^"\\])*)"\)', block))


def request(identifier, method="tools/call", **arguments):
    msg = {"jsonrpc": "2.0", "id": identifier, "method": method}
    if method == "tools/call":
        msg["params"] = {"name": "fixture_query", "arguments": arguments}
    return msg


class EditorFixture:
    def __init__(self):
        self.condition = threading.Condition()
        self.received = []
        self.received_headers = []
        self.received_client_headers = []
        self.evidence_payloads = {}
        self.release_attempts = []
        self.release_mode = None
        self.cleanup_stop = threading.Event()
        self.gates = {}
        self.offline = False
        self.tools = TOOLS
        self.instructions = "Fixture instructions: discover schemas before edits."
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
                    owner.received_headers.append({name.lower(): value for name, value in self.headers.items()})
                    owner.received_client_headers.append(self.headers.get_all("X-Monolith-Client", []))
                    owner.condition.notify_all()
                identifier = msg["id"]
                gate = owner.gates.get(identifier)
                if gate is not None:
                    gate.wait(WAIT)
                if owner.offline:
                    self.reply({"offline": True}, 503)
                    return
                if msg.get("params", {}).get("name") == "monolith_coordination":
                    args = msg["params"].get("arguments", {})
                    nested = args.get("params")
                    if isinstance(nested, str):
                        nested = json.loads(nested)
                    if isinstance(nested, dict):
                        args = dict(args, **nested)
                    operation = args.get("operation", "status")
                    token = args.get("_lease_token", "lease-" + str(identifier))
                    shape = args.get("fixture_response", "structured")
                    if operation == "release":
                        with owner.condition:
                            owner.release_attempts.append(token)
                            owner.condition.notify_all()
                        if owner.release_mode == "hang":
                            owner.cleanup_stop.wait(WAIT)
                            return
                        if owner.release_mode == "trickle":
                            try:
                                self.send_response(200)
                                self.send_header("Content-Type", "application/json")
                                self.send_header("Content-Length", "100000")
                                self.end_headers()
                                while not owner.cleanup_stop.is_set():
                                    self.wfile.write(b" ")
                                    self.wfile.flush()
                                    owner.cleanup_stop.wait(0.05)
                            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                                pass
                            return
                    data = {"active": operation != "release", "owner": args.get("owner", "fixture-owner")}
                    if operation in ("acquire", "renew"):
                        data["_lease_token"] = token
                    result = {"isError": False, "content": [{"type": "text", "text": json.dumps(data)}]}
                    if shape != "text_only":
                        result["structuredContent"] = data
                    if shape == "tool_error":
                        result["isError"] = True  # Token-looking error data never establishes ownership.
                    payload = {"jsonrpc": "2.0", "id": identifier, "result": result}
                    if shape == "protocol_error":
                        payload = {"jsonrpc": "2.0", "id": identifier,
                                   "error": {"code": -32020, "message": "acquire rejected", "data": data}}
                    self.reply(payload)
                    return
                mode = msg.get("params", {}).get("arguments", {}).get("fixture_mode")
                if mode in ("evidence_success", "evidence_tool_error", "evidence_tool_error_flat",
                            "evidence_protocol_error"):
                    request_uuid = self.headers.get("X-Monolith-Request-Id")
                    server_instance = "6936d5a3-5c49-42ab-b882-e2c9941d81a5"
                    evidence = {"request_id": request_uuid, "server_instance": server_instance}
                    if mode == "evidence_success":
                        result = {"content": [{"type": "text", "text": "fixture success"}],
                                  "structuredContent": {"value": 42},
                                  "_meta": {"monolith": dict(evidence, lease_owner="fixture-owner")}}
                        payload = {"jsonrpc": "2.0", "id": identifier, "result": result}
                    else:
                        error = {"code": -32003, "message": "Fixture precondition failed",
                                 "data": dict(evidence, executed=False, retryable=False,
                                              **{"class": "precondition_failed"})}
                        if mode in ("evidence_tool_error", "evidence_tool_error_flat"):
                            structured = ({"error": error} if mode == "evidence_tool_error" else
                                          {"error": error["message"], "code": error["code"], "data": error["data"]})
                            payload = {"jsonrpc": "2.0", "id": identifier,
                                       "result": {"isError": True, "structuredContent": structured,
                                                  "content": [{"type": "text", "text": json.dumps(structured)}]}}
                        else:
                            payload = {"jsonrpc": "2.0", "id": identifier, "error": error}
                    with owner.condition:
                        owner.evidence_payloads[identifier] = payload
                    self.reply(payload)
                    return
                if mode and mode.startswith("rejection_"):
                    error = {"code": -32600, "message": "Request rejected before execution",
                             "data": {"executed": False}}
                    payload = {"jsonrpc": "2.0", "id": None, "error": error}
                    if mode == "rejection_wrong_id":
                        payload["id"] = "unrelated-id"
                    elif mode == "rejection_missing_id":
                        payload.pop("id")
                    elif mode == "rejection_wrong_code":
                        error["code"] = -32603
                    elif mode == "rejection_executed_true":
                        error["data"]["executed"] = True
                    elif mode == "rejection_executed_zero":
                        error["data"]["executed"] = 0
                    elif mode == "rejection_missing_evidence":
                        error.pop("data")
                    status = int(mode.rsplit("_", 1)[-1]) if mode in (
                        "rejection_400", "rejection_403", "rejection_413") else 400
                    self.reply(payload, status)
                    return
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
                result = {"content": [{"type": "text", "text": str(identifier)}]}
                if msg["method"] == "tools/list":
                    result = {"tools": owner.tools}
                elif msg["method"] == "initialize":
                    result = {"instructions": owner.instructions,
                              "serverInfo": {"name": "upstream-fixture"}, "capabilities": {}}
                payload = {"jsonrpc": "2.0", "id": identifier, "result": result}
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

    def wait_release_attempts(self, count):
        with self.condition:
            if not self.condition.wait_for(lambda: len(self.release_attempts) >= count, WAIT):
                raise AssertionError("Cleanup release attempt never arrived: %r" % self.received)

    def release(self):
        self.cleanup_stop.set()
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
        for key, value in settings.items():
            if value is None:
                env.pop(key, None)
            else:
                env[key] = str(value)
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
        structured = response["result"]["structuredContent"]
        self.assertEqual(json.loads(response["result"]["content"][0]["text"]), structured)
        self.assertEqual(structured["data"]["class"], "unknown_outcome")
        self.assertEqual(structured["data"]["executed"], "unknown")

    def assert_request_evidence(self, headers, proxy, client_name):
        request_uuid = headers.get("x-monolith-request-id")
        self.assertIsInstance(request_uuid, str)
        self.assertEqual(str(uuid.UUID(request_uuid)), request_uuid)
        self.assertEqual(headers.get("x-monolith-client"), "%s/%d" % (client_name, proxy.process.pid))
        return request_uuid

    def test_request_uuid_headers_and_log_keep_original_rpc_ids(self):
        proxy = self.proxy(MONOLITH_CALL_LOG=1, MONOLITH_CLIENT_NAME="fixture-agent",
                           MONOLITH_PROJECT_ROOT=self.directory.name)
        identifiers = [27, "27", "unicode-ö-雪", 27]
        sent = []
        for identifier in identifiers:
            msg = request(identifier, fixture_mode="evidence_success", request_id="caller-owned-argument")
            sent.append(msg)
            proxy.send(msg)
            response = proxy.receive()
            self.assertEqual(response, self.editor.evidence_payloads[identifier])
            self.assertEqual(type(response["id"]), type(identifier))
        self.assertEqual(self.editor.received, sent)
        uuids = [self.assert_request_evidence(headers, proxy, "fixture-agent")
                 for headers in self.editor.received_headers]
        self.assertEqual(len(set(uuids)), len(identifiers))
        log_path = Path(self.directory.name) / "Saved" / "Logs" / ("MonolithCalls-%d.jsonl" % proxy.process.pid)
        self.assertTrue(log_path.is_file(), "Enabled call log must exist for both proxy flavors")
        records = [json.loads(line) for line in log_path.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(len(records), len(identifiers))
        self.assertEqual([record["request_id"] for record in records], identifiers)
        self.assertEqual([record["request_uuid"] for record in records], uuids)
        self.assertTrue(all(record["client"] == "fixture-agent/%d" % proxy.process.pid for record in records))
        self.assertTrue(all(record["proxy_pid"] == proxy.process.pid for record in records))

    def test_client_header_sanitizes_controls_and_preserves_pid_in_log(self):
        cases = [("agent\r\nX-Injected: yes [bad]\t雪", "agent__X-Injected:_yes__bad___"),
                 ("A" * 200, None), ("", "proxy")]
        for index, (client_name, expected_name) in enumerate(cases):
            with self.subTest(client=client_name):
                proxy = self.proxy(MONOLITH_CALL_LOG=1, MONOLITH_CLIENT_NAME=client_name,
                                   MONOLITH_PROJECT_ROOT=self.directory.name)
                suffix = "/%d" % proxy.process.pid
                if expected_name is None:
                    expected_name = "A" * (128 - len(suffix))
                expected = expected_name + suffix
                proxy.send(request("sanitized-%d" % index))
                self.assertEqual(proxy.receive()["id"], "sanitized-%d" % index)
                headers = self.editor.received_headers[-1]
                request_uuid = self.assert_request_evidence(headers, proxy, expected_name)
                self.assertEqual(self.editor.received_client_headers[-1], [expected])
                self.assertLessEqual(len(expected), 128)
                self.assertNotIn("x-injected", headers)
                log_path = Path(self.directory.name) / "Saved" / "Logs" / ("MonolithCalls-%d.jsonl" % proxy.process.pid)
                records = [json.loads(line) for line in log_path.read_text(encoding="utf-8").splitlines()]
                self.assertEqual(len(records), 1)
                self.assertEqual(records[0]["client"], expected)
                self.assertEqual(records[0]["request_uuid"], request_uuid)

    def test_default_client_and_upstream_error_evidence_are_preserved(self):
        proxy = self.proxy(MONOLITH_CLIENT_NAME=None)
        for identifier, mode in (("tool-error", "evidence_tool_error"), (41, "evidence_protocol_error")):
            proxy.send(request(identifier, fixture_mode=mode))
            response = proxy.receive()
            self.assertEqual(response, self.editor.evidence_payloads[identifier])
            self.assert_request_evidence(self.editor.received_headers[-1], proxy, "proxy")

    def call_log_files(self, proxy, root=None):
        directory = Path(root or self.directory.name) / "Saved" / "Logs"
        active = directory / ("MonolithCalls-%d.jsonl" % proxy.process.pid)
        paths = [active] if active.is_file() else []
        return sorted(paths + list(directory.glob("MonolithCalls-%d-*.jsonl" % proxy.process.pid)))

    def read_call_logs(self, proxy, root=None):
        records = []
        for path in self.call_log_files(proxy, root):
            content = path.read_bytes()
            self.assertTrue(not content or content.endswith(b"\n"), str(path))
            for line in content.splitlines():
                self.assertTrue(line, "Empty JSONL record in " + str(path))
                records.append(json.loads(line))
        return records

    def finish_call_log(self, proxy):
        proxy.eof()
        self.assertEqual(proxy.process.wait(WAIT), 0)

    def test_call_log_rotation_keeps_every_uuid_and_oversized_record(self):
        proxy = self.proxy(MONOLITH_CALL_LOG=1, MONOLITH_CALL_LOG_MAX_MB="0.001",
                           MONOLITH_PROJECT_ROOT=self.directory.name)
        identifiers = ["rotation-%d" % index for index in range(20)]
        for identifier in identifiers:
            proxy.send(request(identifier, action="read"))
        self.assertCountEqual([proxy.receive()["id"] for _ in identifiers], identifiers)
        for identifier in ("oversized-" + "x" * 10240, "after-oversized"):
            identifiers.append(identifier)
            proxy.send(request(identifier, action="read"))
            self.assertEqual(proxy.receive()["id"], identifier)
        self.finish_call_log(proxy)
        paths = self.call_log_files(proxy)
        self.assertGreater(len(paths), 1, "Small log budget must rotate")
        self.assertIn("MonolithCalls-%d.jsonl" % proxy.process.pid, [path.name for path in paths])
        records = self.read_call_logs(proxy)
        self.assertCountEqual([record["request_id"] for record in records], identifiers)
        observed_uuids = [headers["x-monolith-request-id"] for headers in self.editor.received_headers]
        self.assertCountEqual([record["request_uuid"] for record in records], observed_uuids)
        self.assertEqual(len({record["request_uuid"] for record in records}), len(identifiers))
        for path in paths:
            content = path.read_bytes()
            if len(content) > int(0.001 * 1024 * 1024):
                lines = content.splitlines()
                self.assertEqual(len(lines), 1, "Oversized record must remain intact and alone: " + str(path))
                self.assertTrue(json.loads(lines[0])["request_id"].startswith("oversized-"))

    def test_call_log_retention_only_removes_old_matching_regular_files(self):
        directory = Path(self.directory.name) / "Saved" / "Logs"
        directory.mkdir(parents=True)
        old = time.time() - 15 * 86400
        recent = time.time() - 13 * 86400
        paths = {name: directory / name for name in (
            "MonolithCalls-111.jsonl", "MonolithCalls-111-archive.jsonl",
            "MonolithCalls-222.jsonl", "Unrelated.jsonl")}
        for name, path in paths.items():
            path.write_text(name, encoding="utf-8")
            os.utime(path, (old, old))
        os.utime(paths["MonolithCalls-222.jsonl"], (recent, recent))
        matching_directory = directory / "MonolithCalls-directory.jsonl"
        matching_directory.mkdir()
        nested = matching_directory / "MonolithCalls-nested.jsonl"
        nested.write_text("nested", encoding="utf-8")
        os.utime(nested, (old, old))
        os.utime(matching_directory, (old, old))
        proxy = self.proxy(MONOLITH_CALL_LOG=1, MONOLITH_PROJECT_ROOT=self.directory.name)
        proxy.send(request("retention"))
        self.assertEqual(proxy.receive()["id"], "retention")
        self.finish_call_log(proxy)
        self.assertFalse(paths["MonolithCalls-111.jsonl"].exists())
        self.assertFalse(paths["MonolithCalls-111-archive.jsonl"].exists())
        for name in ("MonolithCalls-222.jsonl", "Unrelated.jsonl"):
            self.assertEqual(paths[name].read_text(encoding="utf-8"), name)
        self.assertEqual(nested.read_text(encoding="utf-8"), "nested")

    def test_call_log_retention_preserves_symlink_and_target(self):
        directory = Path(self.directory.name) / "Saved" / "Logs"
        directory.mkdir(parents=True)
        target = directory / "unrelated-target.txt"
        target.write_text("target must survive", encoding="utf-8")
        old = time.time() - 15 * 86400
        os.utime(target, (old, old))
        link = directory / "MonolithCalls-link.jsonl"
        try:
            link.symlink_to(target)
        except OSError as exc:
            self.skipTest("Symlink creation unavailable: " + str(exc))
        proxy = self.proxy(MONOLITH_CALL_LOG=1, MONOLITH_PROJECT_ROOT=self.directory.name)
        proxy.send(request("symlink-retention"))
        self.assertEqual(proxy.receive()["id"], "symlink-retention")
        self.finish_call_log(proxy)
        self.assertTrue(link.is_symlink())
        self.assertEqual(target.read_text(encoding="utf-8"), "target must survive")

    def test_call_log_optout_skips_creation_and_retention(self):
        for existing in (False, True):
            with self.subTest(existing=existing):
                root = Path(self.directory.name) / ("existing" if existing else "empty")
                root.mkdir()
                directory = root / "Saved" / "Logs"
                old_path = directory / "MonolithCalls-111.jsonl"
                if existing:
                    directory.mkdir(parents=True)
                    old_path.write_text("preserve disabled log", encoding="utf-8")
                    old = time.time() - 15 * 86400
                    os.utime(old_path, (old, old))
                proxy = self.proxy(MONOLITH_CALL_LOG=0, MONOLITH_PROJECT_ROOT=root)
                proxy.send(request("disabled"))
                self.assertEqual(proxy.receive()["id"], "disabled")
                self.finish_call_log(proxy)
                self.assertEqual(self.call_log_files(proxy, root), [])
                if existing:
                    self.assertEqual(old_path.read_text(encoding="utf-8"), "preserve disabled log")
                else:
                    self.assertFalse(directory.exists())

    def test_call_log_invalid_rotation_settings_keep_logging(self):
        for index, setting in enumerate((None, "", "0", "-1", "NaN", "inf", "1025", "garbage",
                                         "0.0_001", "0x1p-20")):
            with self.subTest(setting=setting):
                proxy = self.proxy(MONOLITH_CALL_LOG=1, MONOLITH_CALL_LOG_MAX_MB=setting,
                                   MONOLITH_PROJECT_ROOT=self.directory.name)
                identifiers = ["invalid-budget-%d-%d" % (index, item) for item in range(3)]
                for identifier in identifiers:
                    proxy.send(request(identifier))
                    self.assertEqual(proxy.receive()["id"], identifier)
                self.finish_call_log(proxy)
                self.assertEqual(len(self.call_log_files(proxy)), 1)
                self.assertEqual([record["request_id"] for record in self.read_call_logs(proxy)], identifiers)

    def test_call_log_utc_milliseconds_and_response_outcomes(self):
        started = time.time()
        proxy = self.proxy(MONOLITH_CALL_LOG=1, MONOLITH_PROJECT_ROOT=self.directory.name)
        cases = [("success", "evidence_success", "ok", None),
                 ("flat-error", "evidence_tool_error_flat", "error", -32003),
                 ("nested-error", "evidence_tool_error", "error", -32003),
                 ("protocol-error", "evidence_protocol_error", "error", -32003),
                 ("reset", "drop", "unknown", -32603),
                 ("malformed", "invalid", "unknown", -32603)]
        for identifier, mode, outcome, _code in cases:
            proxy.send(request(identifier, fixture_mode=mode))
            response = proxy.receive()
            self.assertEqual(response["id"], identifier)
            if outcome == "unknown":
                self.assert_tool_unknown(response, identifier)
            else:
                self.assertEqual(response, self.editor.evidence_payloads[identifier])
        self.finish_call_log(proxy)
        ended = time.time()
        records = {record["request_id"]: record for record in self.read_call_logs(proxy)}
        self.assertEqual(len(records), len(cases))
        for index, (identifier, _mode, outcome, code) in enumerate(cases):
            record = records[identifier]
            self.assertEqual(record["outcome"], outcome)
            self.assertIs(record["ok"], outcome == "ok")
            self.assertEqual(record["error_code"], code)
            self.assertRegex(record["ts"], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")
            timestamp = datetime.strptime(record["ts"], "%Y-%m-%dT%H:%M:%S.%fZ").replace(tzinfo=timezone.utc).timestamp()
            self.assertGreaterEqual(timestamp, started - 0.001)
            self.assertLessEqual(timestamp, ended)
            self.assertGreaterEqual(record["duration_ms"], 0)
            self.assertEqual(record["request_uuid"], self.editor.received_headers[index]["x-monolith-request-id"])
            self.assertTrue({"proxy_pid", "client", "namespace", "action", "params_hash", "result_bytes"} <= record.keys())

    def test_call_log_refused_connection_is_not_sent(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reserved:
            if os.name == "nt":
                reserved.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            reserved.bind(("127.0.0.1", 0))
            proxy = self.proxy(MONOLITH_CALL_LOG=1, MONOLITH_PROJECT_ROOT=self.directory.name,
                               MONOLITH_URL="http://127.0.0.1:%d/mcp" % reserved.getsockname()[1])
            proxy.send(request("refused-log"))
            response = proxy.receive()
            self.finish_call_log(proxy)
        records = self.read_call_logs(proxy)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["outcome"], "not_sent")
        self.assertIs(records[0]["ok"], False)
        self.assertEqual(records[0]["error_code"], -32603)
        self.assertEqual(records[0]["request_uuid"], response["result"]["structuredContent"]["data"]["request_id"])

    def test_call_log_timeout_is_unknown_and_notification_does_not_cancel(self):
        for identifier, timeout, expected in (("log-timeout", "1", "unknown"),
                                              ("log-cancel-notification", "5", "ok")):
            with self.subTest(identifier=identifier):
                proxy = self.proxy(MONOLITH_CALL_LOG=1, MONOLITH_PROJECT_ROOT=self.directory.name,
                                   MONOLITH_TIMEOUT_SECONDS=timeout)
                gate = self.editor.block(identifier)
                proxy.send(request(identifier))
                self.editor.wait_received([identifier])
                if expected == "ok":
                    proxy.send({"jsonrpc": "2.0", "method": "notifications/cancelled",
                                "params": {"requestId": identifier, "reason": "fixture"}})
                    gate.set()
                response = proxy.receive()
                if expected == "unknown":
                    self.assert_tool_unknown(response, identifier)
                    gate.set()
                else:
                    self.assertFalse(response["result"].get("isError", False))
                self.finish_call_log(proxy)
                records = self.read_call_logs(proxy)
                self.assertEqual(len(records), 1)
                self.assertEqual(records[0]["outcome"], expected)
                self.assertIs(records[0]["ok"], expected == "ok")

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

    def test_initialize_imports_only_server_instructions(self):
        proxy = self.proxy()
        msg = request("init", "initialize")
        msg["params"] = {"protocolVersion": "2025-06-18"}
        proxy.send(msg)
        result = proxy.receive()["result"]
        self.assertEqual(result["instructions"], self.editor.instructions)
        self.assertEqual(result["protocolVersion"], "2025-06-18")
        self.assertEqual(result["capabilities"], {"tools": {"listChanged": True}})
        self.assertNotEqual(result["serverInfo"]["name"], "upstream-fixture")
        proxy.send(request("tools", "tools/list"))
        self.assertEqual(proxy.receive()["result"]["tools"], TOOLS)
        self.assertEqual(len(list(Path(self.directory.name).rglob("*.json"))), 1)
        paths = list(Path(self.directory.name).rglob("*.instructions"))
        self.assertEqual(len(paths), 1)
        self.assertEqual(json.loads(read_cache_snapshot(paths[0])), self.editor.instructions)

    def test_initialize_preserves_cached_instructions_on_invalid_metadata_and_offline(self):
        proxy = self.proxy()
        expected = self.editor.instructions
        proxy.send(request("initial", "initialize"))
        self.assertEqual(proxy.receive()["result"]["instructions"], expected)
        for identifier, value in enumerate((None, [], {"bad": "metadata"})):
            self.editor.instructions = value
            proxy.send(request(identifier, "initialize"))
            self.assertEqual(proxy.receive()["result"]["instructions"], expected)
        self.editor.offline = True
        restarted = self.proxy()
        restarted.send(request("offline", "initialize"))
        self.assertEqual(restarted.receive()["result"]["instructions"], expected)

    def test_offline_initialize_fallback_matches_server_verbatim(self):
        self.editor.offline = True
        proxy = self.proxy()
        proxy.send(request("fallback", "initialize"))
        self.assertEqual(proxy.receive()["result"]["instructions"], server_instructions())

    def test_initialize_does_not_wait_for_saturated_domain_workers(self):
        proxy = self.proxy(MONOLITH_MAX_IN_FLIGHT=2, MONOLITH_MAX_QUEUED=0)
        for identifier in ("slow-one", "slow-two"):
            self.editor.block(identifier)
            proxy.send(request(identifier))
        self.editor.wait_received(["slow-one", "slow-two"])
        proxy.send(request("init", "initialize"))
        response = proxy.receive()
        self.assertEqual(response["id"], "init")
        self.assertEqual(response["result"]["instructions"], self.editor.instructions)
        self.editor.release()
        self.assertEqual({proxy.receive()["id"], proxy.receive()["id"]}, {"slow-one", "slow-two"})

    def test_duplicate_ids_are_rejected_across_metadata_and_domain_pools(self):
        proxy = self.proxy()
        for first_method, second_method in (("initialize", "tools/call"), ("tools/call", "initialize")):
            identifier = "active-" + first_method
            with self.subTest(first_method=first_method):
                gate = self.editor.block(identifier)
                proxy.send(request(identifier, first_method))
                self.editor.wait_received([identifier])
                proxy.send(request(identifier, second_method))
                response = proxy.receive()
                self.assertEqual(response["id"], identifier)
                self.assertEqual(response["error"]["code"], -32600)
                gate.set()
                self.assertIn("result", proxy.receive())
                matches = [m for m in self.editor.received if m["id"] == identifier]
                self.assertEqual(len(matches), 1)
                self.assertEqual(matches[0]["method"], first_method)

    def test_blocked_initialize_keeps_ping_responsive_and_metadata_bounded(self):
        proxy = self.proxy()
        self.editor.block("blocked-init")
        proxy.send(request("blocked-init", "initialize"))
        self.editor.wait_received(["blocked-init"])
        proxy.send(request("ping", "ping"))
        self.assertEqual(proxy.receive()["id"], "ping")
        proxy.send(request("extra-init", "initialize"))
        response = proxy.receive()
        self.assertEqual(response["id"], "extra-init")
        self.assertEqual(response["result"]["instructions"], server_instructions())
        # The one-second metadata timeout returns without releasing the upstream gate.
        response = proxy.receive()
        self.assertEqual(response["id"], "blocked-init")
        self.assertEqual(response["result"]["instructions"], server_instructions())
        self.assertEqual([m["id"] for m in self.editor.received], ["blocked-init"])

    @unittest.skipUnless(os.environ.get("MONOLITH_TEST_NATIVE_PROXY"), "Native proxy required for cache interoperability")
    def test_instructions_cache_is_shared_across_proxy_implementations(self):
        writer = self.proxy()
        writer.send(request("write", "initialize"))
        self.assertEqual(writer.receive()["result"]["instructions"], self.editor.instructions)
        self.editor.offline = True
        command = (NativeTransportTests.command if self.command == PythonTransportTests.command
                   else PythonTransportTests.command)
        reader = ProxyProcess(command, self.editor, self.directory.name)
        self.proxies.append(reader)
        reader.send(request("read", "initialize"))
        self.assertEqual(reader.receive()["result"]["instructions"], self.editor.instructions)

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

    def test_http_rejections_preserve_never_executed_evidence(self):
        proxy = self.proxy()
        for status in (400, 403, 413):
            identifier = "rejected-%d" % status
            proxy.send(request(identifier, fixture_mode="rejection_%d" % status))
            self.assertEqual(proxy.receive(), {"jsonrpc": "2.0", "id": identifier,
                "error": {"code": -32600, "message": "Request rejected before execution",
                          "data": {"executed": False}}})
        self.assertEqual(len(self.editor.received), 3)

    def test_rejection_id_exception_requires_exact_never_executed_contract(self):
        proxy = self.proxy()
        modes = ("wrong_id", "missing_id", "wrong_code", "executed_true",
                 "executed_zero", "missing_evidence")
        for mode in modes:
            proxy.send(request(mode, fixture_mode="rejection_" + mode))
            self.assert_tool_unknown(proxy.receive(), mode)
        self.assertEqual(len(self.editor.received), len(modes))

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
        by_name = {tool["name"]: tool for tool in tools}
        self.assertEqual({name for name in by_name if name.startswith("monolith_")}, {
            "monolith_discover", "monolith_status", "monolith_update", "monolith_reindex",
            "monolith_guide", "monolith_coordination"})
        self.assertEqual(by_name["monolith_guide"]["inputSchema"]["properties"]["section"]["type"], "string")
        coordination = by_name["monolith_coordination"]["inputSchema"]["properties"]
        self.assertEqual(coordination["operation"]["enum"], ["status", "acquire", "renew", "release"])
        self.assertEqual(coordination["operation"]["default"], "status")
        self.assertEqual(coordination["owner"]["type"], "string")
        self.assertEqual((coordination["ttl_seconds"]["minimum"], coordination["ttl_seconds"]["maximum"]), (10, 600))
        self.assertNotIn("default", coordination["ttl_seconds"])
        for name, tool in by_name.items():
            if name.startswith("monolith_"):
                self.assertEqual(tool["inputSchema"]["properties"]["_lease_token"]["type"], "string")

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

    @staticmethod
    def coordination(identifier, operation, shape="flat", **arguments):
        args = dict(arguments, operation=operation)
        if shape == "nested":
            args = {"params": args}
        elif shape == "string":
            args = {"params": json.dumps(args)}
        return {"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                "params": {"name": "monolith_coordination", "arguments": args}}

    def test_connection_refused_is_not_sent(self):
        with socket.socket() as reservation:
            if os.name == "nt":
                reservation.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            reservation.bind(("127.0.0.1", 0))  # Keep the port reserved without listening.
            proxy = self.proxy(MONOLITH_URL="http://127.0.0.1:%d/mcp" % reservation.getsockname()[1])
            proxy.send(request("refused"))
            response = proxy.receive()
            self.assertEqual(response["id"], "refused")
            self.assertIs(response["result"]["isError"], True)
            structured = response["result"]["structuredContent"]
            self.assertEqual(json.loads(response["result"]["content"][0]["text"]), structured)
            self.assertEqual(structured["data"]["class"], "not_sent")
            self.assertIs(structured["data"]["executed"], False)
            self.assertIs(structured["data"]["retryable"], True)
            proxy.send(request("still-alive", "ping"))
            self.assertEqual(proxy.receive()["id"], "still-alive")
            self.assertEqual(self.editor.received, [])

    def test_eof_releases_only_successfully_acquired_tokens(self):
        for index, (shape, response_shape) in enumerate((("flat", "structured"),
                                                       ("nested", "structured"),
                                                       ("string", "text_only"))):
            with self.subTest(shape=shape, response=response_shape):
                proxy = self.proxy()
                identifier = "owned-%d" % index
                proxy.send(self.coordination(identifier, "acquire", shape=shape,
                                             owner="fixture", fixture_response=response_shape))
                self.assertFalse(proxy.receive()["result"]["isError"])
                proxy.eof()
                self.assertEqual(proxy.process.wait(WAIT), 0)
                self.editor.wait_release_attempts(index + 1)
                for reader in proxy.readers:
                    reader.join(WAIT)
                while not proxy.messages.empty():
                    self.assertIn("method", proxy.messages.get_nowait(), "Cleanup must not emit an unsolicited RPC reply")
                self.assertEqual(self.editor.release_attempts, ["lease-owned-%d" % n for n in range(index + 1)])
        for msg, headers in zip(self.editor.received, self.editor.received_headers):
            if msg.get("params", {}).get("name") == "monolith_coordination" and msg["params"].get("arguments", {}).get("operation") == "release":
                self.assertEqual(msg.get("method"), "tools/call")
                self.assertIn("id", msg)  # Tool notifications cannot execute a release.
                self.assertIsInstance(uuid.UUID(headers["x-monolith-request-id"]), uuid.UUID)

    def test_eof_does_not_claim_supplied_renewed_or_failed_tokens(self):
        proxy = self.proxy()
        proxy.send(request("supplied", _lease_token="foreign-supplied"))
        self.assertEqual(proxy.receive()["id"], "supplied")
        proxy.send(self.coordination("renewed", "renew", _lease_token="foreign-renewed"))
        self.assertFalse(proxy.receive()["result"]["isError"])
        for shape in ("tool_error", "protocol_error"):
            proxy.send(self.coordination("failed-" + shape, "acquire", owner="fixture", fixture_response=shape))
            response = proxy.receive()
            self.assertTrue("error" in response or response["result"]["isError"])
        proxy.eof()
        self.assertEqual(proxy.process.wait(WAIT), 0)
        with self.editor.condition:
            self.assertFalse(self.editor.condition.wait_for(lambda: bool(self.editor.release_attempts), 0.2))
        self.assertEqual(self.editor.release_attempts, [])

    def test_successful_explicit_release_removes_owned_token(self):
        proxy = self.proxy()
        proxy.send(self.coordination("explicit-owned", "acquire", owner="fixture"))
        self.assertFalse(proxy.receive()["result"]["isError"])
        proxy.send(self.coordination("explicit-release", "release", _lease_token="lease-explicit-owned"))
        self.assertFalse(proxy.receive()["result"]["isError"])
        proxy.eof()
        self.assertEqual(proxy.process.wait(WAIT), 0)
        with self.editor.condition:
            self.assertFalse(self.editor.condition.wait_for(lambda: len(self.editor.release_attempts) > 1, 0.2))
        self.assertEqual(self.editor.release_attempts, ["lease-explicit-owned"])

    def test_failed_explicit_release_keeps_token_for_cleanup_attempt(self):
        proxy = self.proxy()
        proxy.send(self.coordination("retry-release", "acquire", owner="fixture"))
        self.assertFalse(proxy.receive()["result"]["isError"])
        proxy.send(self.coordination("failed-release", "release", _lease_token="lease-retry-release",
                                     fixture_response="tool_error"))
        self.assertTrue(proxy.receive()["result"]["isError"])
        proxy.eof()
        self.assertEqual(proxy.process.wait(WAIT), 0)
        self.editor.wait_release_attempts(2)
        self.assertEqual(self.editor.release_attempts, ["lease-retry-release", "lease-retry-release"])

    def test_eof_records_acquire_that_finishes_after_eof(self):
        proxy = self.proxy()
        gate = self.editor.block("late-owned")
        proxy.send(self.coordination("late-owned", "acquire", owner="fixture"))
        self.editor.wait_received(["late-owned"])
        proxy.eof()
        self.assertIsNone(proxy.process.poll())
        gate.set()
        self.assertFalse(proxy.receive()["result"]["isError"])
        self.assertEqual(proxy.process.wait(WAIT), 0)
        self.editor.wait_release_attempts(1)
        self.assertEqual(self.editor.release_attempts, ["lease-late-owned"])

    def test_eof_cleanup_waits_for_running_and_queued_work(self):
        proxy = self.proxy(MONOLITH_MAX_IN_FLIGHT=1, MONOLITH_MAX_QUEUED=1)
        proxy.send(self.coordination("drain-owned", "acquire", owner="fixture"))
        self.assertFalse(proxy.receive()["result"]["isError"])
        gate = self.editor.block("leased-running")
        proxy.send(request("leased-running", _lease_token="lease-drain-owned"))
        self.editor.wait_received(["leased-running"])
        proxy.send(request("leased-queued", _lease_token="lease-drain-owned"))
        proxy.send(request("leased-overflow", _lease_token="lease-drain-owned"))
        self.assertEqual(proxy.receive()["error"]["code"], -32001)
        proxy.eof()
        gate.set()
        self.assertEqual({proxy.receive()["id"], proxy.receive()["id"]}, {"leased-running", "leased-queued"})
        self.assertEqual(proxy.process.wait(WAIT), 0)
        self.editor.wait_release_attempts(1)
        self.assertEqual([msg["id"] for msg in self.editor.received[:3]],
                         ["drain-owned", "leased-running", "leased-queued"])
        self.assertEqual(self.editor.release_attempts, ["lease-drain-owned"])
        self.assertEqual(len(self.editor.received), 4)

    def test_eof_cleanup_has_total_deadline_across_leases_and_trickled_body(self):
        for index, mode in enumerate(("hang", "trickle")):
            with self.subTest(cleanup=mode):
                # The previous handler retains its already-signaled Event; never clear it under that waiter.
                self.editor.cleanup_stop = threading.Event()
                proxy = self.proxy()
                for number in range(3):
                    identifier = "bounded-%d-%d" % (index, number)
                    proxy.send(self.coordination(identifier, "acquire", owner="fixture"))
                    self.assertFalse(proxy.receive()["result"]["isError"])
                self.editor.release_mode = mode
                before = len(self.editor.release_attempts)
                started = time.monotonic()
                proxy.eof()
                self.assertEqual(proxy.process.wait(2.75), 0)
                self.assertLess(time.monotonic() - started, 2.75)
                self.editor.wait_release_attempts(before + 1)
                attempts = self.editor.release_attempts[before:]
                self.assertEqual(len(attempts), len(set(attempts)))
                self.assertTrue(all(token.startswith("lease-bounded-%d-" % index) for token in attempts))
                # Receipt is only evidence of an attempt: no confirmed release is claimed.
                self.editor.cleanup_stop.set()
                self.editor.release_mode = None

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

    def test_split_editor_cache_stays_raw_and_rewrites_on_read(self):
        self.editor.tools = [{"name": "editor_query", "description": "editor fixture",
                              "inputSchema": {"type": "object"}}]
        split = self.proxy(MONOLITH_SPLIT_EDITOR_QUERY=1)
        split.send(request("split", "tools/list"))
        self.assertEqual({t["name"] for t in split.receive()["result"]["tools"]},
                         {"editor_read_query", "editor_build_query"})
        self.editor.offline = True
        plain = self.proxy(MONOLITH_SPLIT_EDITOR_QUERY=0)
        plain.send(request("plain", "tools/list"))
        self.assertEqual(plain.receive()["result"]["tools"], self.editor.tools)
        split.send(request("cached-split", "tools/list"))
        self.assertEqual({t["name"] for t in split.receive()["result"]["tools"]},
                         {"editor_read_query", "editor_build_query"})
        python_proxy = ProxyProcess(PythonTransportTests.command, self.editor, self.directory.name)
        self.proxies.append(python_proxy)
        python_proxy.send(request("python-cache", "tools/list"))
        self.assertEqual(python_proxy.receive()["result"]["tools"], self.editor.tools)

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
