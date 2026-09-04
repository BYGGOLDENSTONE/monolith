"""Opt-in contracts against a running, freshly built Unreal Editor MCP server.

PowerShell (run from the repository root):
  $env:MONOLITH_LIVE_URL = 'http://127.0.0.1:19316/mcp'
  python -m unittest discover -s Scripts/tests -p test_live_mcp.py -v

These tests read diagnostics and exercise temporary coordination leases only.
They never create, edit, save, compile, or delete game assets. Lease tests skip
if another workflow already owns the editor. Do not run this suite concurrently
with another live-suite invocation. The regular offline CI leaves it disabled.
"""

from contextlib import contextmanager
import http.client
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from urllib.parse import urlsplit
import uuid

try:
    from .test_proxy_transport import ProxyProcess
except ImportError:
    from test_proxy_transport import ProxyProcess


LIVE_URL = os.environ.get("MONOLITH_LIVE_URL")
ROOT = Path(__file__).resolve().parents[2]


def rpc(identifier, method, params=None):
    payload = {"jsonrpc": "2.0", "id": identifier, "method": method}
    if params is not None:
        payload["params"] = params
    return payload


def tool(identifier, name, **arguments):
    return rpc(identifier, "tools/call", {"name": name, "arguments": arguments})


@unittest.skipUnless(LIVE_URL, "Set MONOLITH_LIVE_URL to test a running Unreal Editor")
class LiveMcpTests(unittest.TestCase):
    def setUp(self):
        self.url = urlsplit(LIVE_URL)
        self.assertIn(self.url.scheme, ("http", "https"))
        self.owner = "live-contract-" + uuid.uuid4().hex
        self.next_id = 0

    def http(self, payload=None, method="POST", headers=None):
        connection_type = (http.client.HTTPSConnection if self.url.scheme == "https"
                           else http.client.HTTPConnection)
        connection = connection_type(self.url.hostname, self.url.port, timeout=30)
        request_headers = {"Content-Type": "application/json", "Accept": "application/json"}
        request_headers.update(headers or {})
        path = self.url.path or "/mcp"
        if self.url.query:
            path += "?" + self.url.query
        body = None if payload is None else json.dumps(payload).encode("utf-8")
        try:
            # http.client preserves header spelling, including mixed-case test headers.
            connection.request(method, path, body=body, headers=request_headers)
            response = connection.getresponse()
            content = response.read()
            return response.status, dict(response.getheaders()), content
        finally:
            connection.close()

    def call(self, name, **arguments):
        self.next_id += 1
        status, _headers, body = self.http(tool(self.next_id, name, **arguments))
        self.assertEqual(status, 200, body)
        response = json.loads(body)
        self.assertEqual(response["id"], self.next_id)
        self.assertNotIn("error", response, response)
        return response["result"]

    def data(self, result):
        self.assertFalse(result.get("isError"), result)
        texts = [part["text"] for part in result["content"] if part["type"] == "text"]
        self.assertEqual(len(texts), 1, result)
        return json.loads(texts[0])

    def coordination(self, operation="status", **arguments):
        return self.call("monolith_coordination", operation=operation, **arguments)

    def require_free_editor(self):
        status = self.data(self.coordination())
        if status["active"]:
            self.skipTest("Editor already leased by %s; existing workflow left untouched" % status["owner"])

    @contextmanager
    def lease(self):
        self.require_free_editor()
        acquired = self.coordination("acquire", owner=self.owner, ttl_seconds=30)
        # Capture ownership before assertions so a failed assertion still releases.
        token = self.data(acquired)["_lease_token"]
        try:
            self.assertIsInstance(token, str)
            self.assertTrue(token)
            yield token
        finally:
            released = self.coordination("release", _lease_token=token)
            self.assertFalse(self.data(released)["active"], released)

    def assert_lease_error(self, result, code, reason):
        self.assertTrue(result.get("isError"), result)
        structured = result["structuredContent"]
        self.assertEqual(structured["code"], code)
        self.assertEqual(structured["data"]["reason"], reason)
        self.assertFalse(structured["data"]["executed"])
        self.assertNotIn("_lease_token", structured["data"])
        text = next(part["text"] for part in result["content"] if part["type"] == "text")
        self.assertEqual(json.loads(text), structured)

    def test_initialize_negotiates_latest_supported_protocol(self):
        status, _headers, body = self.http(rpc("init", "initialize", {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": "monolith-live-contract", "version": "1"}}))
        self.assertEqual(status, 200, body)
        response = json.loads(body)
        self.assertEqual(response["id"], "init")
        self.assertEqual(response["result"]["protocolVersion"], "2025-11-25")
        self.assertIn("tools", response["result"]["capabilities"])

    def test_discovery_exposes_coordination_and_project_stats(self):
        status, _headers, body = self.http(rpc("list", "tools/list"))
        self.assertEqual(status, 200, body)
        names = {item["name"] for item in json.loads(body)["result"]["tools"]}
        self.assertTrue({"monolith_coordination", "monolith_discover", "project_query"} <= names)
        discovered = self.data(self.call("monolith_discover", namespace="project"))
        self.assertIn("get_stats", json.dumps(discovered))

    def test_competing_acquisition_fails_and_public_status_never_leaks_token(self):
        with self.lease() as token:
            for contender in (self.owner, self.owner + "-competitor"):
                result = self.coordination("acquire", owner=contender, ttl_seconds=30)
                self.assert_lease_error(result, -32010, "lease_busy")
                self.assertTrue(result["structuredContent"]["data"]["retryable"])
            public = self.data(self.coordination())
            self.assertTrue(public["active"])
            self.assertEqual(public["owner"], self.owner)
            self.assertNotIn("_lease_token", public)
            self.assertNotIn(token, json.dumps(public))

    def test_domain_requires_current_lease_and_stale_token_fails_after_release(self):
        with self.lease() as token:
            denied = self.call("project_query", action="get_stats", params={})
            self.assert_lease_error(denied, -32010, "lease_busy")
            stats = self.data(self.call("project_query", action="get_stats",
                                        params={"_lease_token": token}))
            self.assertIsInstance(stats, dict)
        stale = self.call("project_query", action="get_stats", params={"_lease_token": token})
        self.assert_lease_error(stale, -32011, "invalid_lease")
        self.assertFalse(self.data(self.coordination())["active"])

    def test_notification_does_not_acquire_a_lease_or_emit_response(self):
        self.require_free_editor()
        message = tool("unused", "monolith_coordination", operation="acquire",
                       owner=self.owner, ttl_seconds=10)
        message.pop("id")
        try:
            status, _headers, body = self.http(message)
            self.assertEqual(status, 202, body)
            self.assertEqual(body, b"")
            self.assertFalse(self.data(self.coordination())["active"])
        finally:
            # A regression that executes the notification cannot return its token.
            # Never reclaim by owner label: allow only this short test lease to expire.
            deadline = time.monotonic() + 15
            while True:
                current = self.data(self.coordination())
                if not current["active"] or current["owner"] != self.owner:
                    break
                if time.monotonic() >= deadline:
                    self.fail("Erroneously acquired notification lease did not expire")
                threading.Event().wait(0.1)

    def test_untrusted_mixed_case_origin_header_is_rejected(self):
        status, headers, _body = self.http(rpc("origin", "ping"),
                                           headers={"oRiGiN": "https://localhost.evil.example"})
        self.assertEqual(status, 403)
        self.assertNotIn("access-control-allow-origin", {key.lower(): value for key, value in headers.items()})

    def test_get_mcp_returns_method_not_allowed(self):
        status, headers, _body = self.http(method="GET")
        self.assertEqual(status, 405)
        self.assertIn("POST", {key.lower(): value for key, value in headers.items()}.get("allow", ""))

    def test_legacy_batch_stays_an_array_and_omits_notifications(self):
        payload = [rpc("one", "ping"), {"jsonrpc": "2.0", "method": "notifications/initialized"}]
        status, _headers, body = self.http(payload)
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body), [{"jsonrpc": "2.0", "id": "one", "result": {}}])

    def test_invalid_mixed_case_protocol_header_returns_bad_request(self):
        status, _headers, _body = self.http(rpc("version", "ping"),
                                           headers={"mCp-PrOtOcOl-VeRsIoN": "unsupported"})
        self.assertEqual(status, 400)

    def test_independent_stdio_clients_reuse_ids_without_crossing_responses(self):
        commands = [[sys.executable, str(ROOT / "Scripts" / "monolith_proxy.py")]]
        if os.environ.get("MONOLITH_TEST_NATIVE_PROXY"):
            commands.append([os.environ["MONOLITH_TEST_NATIVE_PROXY"]])
        for command in commands:
            with self.subTest(command=command), tempfile.TemporaryDirectory(prefix="monolith-live-") as directory:
                proxies = []
                try:
                    for _ in range(2):
                        proxies.append(ProxyProcess(command, SimpleNamespace(url=LIVE_URL), directory))
                    # Both clients have outstanding requests using the same IDs.
                    for identifier in (1, 2, "same-id"):
                        proxies[0].send(tool(identifier, "monolith_status"))
                        proxies[1].send(tool(identifier, "monolith_discover", namespace="project"))
                    for index, proxy in enumerate(proxies):
                        responses = [proxy.receive() for _ in range(3)]
                        self.assertEqual({item["id"] for item in responses}, {1, 2, "same-id"})
                        for item in responses:
                            data = self.data(item["result"])
                            if index == 0:
                                self.assertIn("version", data)
                                self.assertTrue(data["server_running"])
                                self.assertTrue(data["capabilities"]["editor_workflow_leases"])
                            else:
                                self.assertIn("get_stats", json.dumps(data))
                finally:
                    failures = []
                    for proxy in proxies:
                        try:
                            proxy.finish()
                        except Exception as exc:
                            failures.append(exc)
                    if failures:
                        raise failures[0]


if __name__ == "__main__":
    unittest.main()
