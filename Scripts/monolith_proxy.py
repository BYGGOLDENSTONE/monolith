#!/usr/bin/env python3
"""
Monolith MCP stdio-to-HTTP proxy.

Sits between Claude Code (stdio) and Monolith (HTTP on localhost).
Handles initialize locally, forwards tool calls to Monolith.
Survives editor restarts — proxy process never dies.
Background health poll auto-detects when the editor comes online.

Usage (in .mcp.json):
  {"mcpServers": {"monolith": {"command": "python", "args": ["Plugins/Monolith/Scripts/monolith_proxy.py"]}}}

Requirements: Python 3.8+ (stdlib only, no pip install needed)
"""

# PEP 563: defer annotation evaluation so PEP 604 unions (`str | None`) below
# parse on Python 3.8/3.9 too (macOS ships 3.9 by default via Xcode).
from __future__ import annotations

import hashlib
import http.client
import selectors
import socket
import ssl
import errno
import json
import os
import sys
import threading
import time
import tempfile
import math
import uuid
from concurrent.futures import ThreadPoolExecutor
import urllib.error
import urllib.request
import urllib.parse
from datetime import datetime, timezone
from io import TextIOWrapper
from pathlib import Path

MONOLITH_URL = os.environ.get("MONOLITH_URL", "http://localhost:9316/mcp")
MONOLITH_HEALTH = MONOLITH_URL.rsplit("/", 1)[0] + "/health"
PROXY_NAME = "monolith-proxy"
PROXY_VERSION = "1.2.0"


def _client_identity() -> str:
    suffix = f"/{os.getpid()}"
    name = os.environ.get("MONOLITH_CLIENT_NAME") or "proxy"
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:/"
    safe_name = "".join(char if char in allowed else "_" for char in name)
    return safe_name[:128 - len(suffix)] + suffix


def _env_number(name, default, minimum, maximum):
    try:
        value = float(os.environ.get(name, default))
        if math.isfinite(value) and minimum <= value <= maximum:
            return value
    except ValueError:
        pass
    return default


TIMEOUT = _env_number("MONOLITH_TIMEOUT_SECONDS", 120, 1, 3600)
MAX_IN_FLIGHT = int(_env_number("MONOLITH_MAX_IN_FLIGHT", 8, 1, 32))
MAX_QUEUED = int(_env_number("MONOLITH_MAX_QUEUED", 64, 0, 1024))
POLL_INTERVAL = 5.0
POLL_START_DELAY = 3.0

# Track Monolith availability for list_changed notifications
_monolith_was_up = None
_stdout_lock = threading.Lock()
_stop_poll = threading.Event()

# Call-log state (Phase 4 / survivor F)
#
# NOTE: Saved/Logs/MonolithCalls-<pid>.jsonl is project-root-relative and excluded
# from crash zip generation by UE's crash reporter (Saved/Logs/ tail capture
# only includes editor logs, not arbitrary jsonl). If a crash collector pattern
# elsewhere DOES sweep Saved/Logs/*, the user should add MonolithCalls-<pid>.jsonl to
# the exclusion list. Single-user local dev tool; no phone-home.
_call_log_enabled = False           # resolved once at startup
_call_log_handle = None             # binary append-mode file handle
_call_log_lock = threading.Lock()

_owned_leases = {}  # Tokens observed in successful acquire responses only.
_owned_leases_lock = threading.Lock()

CORE_QUERY_TOOLS = [
    "blueprint_query",
    "material_query",
    "animation_query",
    "niagara_query",
    "editor_query",
    "config_query",
    "project_query",
    "source_query",
    "ui_query",
    "mesh_query",
    "gas_query",
    "combograph_query",
    "ai_query",
    "logicdriver_query",
    "audio_query",
    "level_sequence_query",
]


def _log(msg: str) -> None:
    """Log to stderr (visible in Claude Code debug mode, never interferes with stdio)."""
    print(f"[monolith-proxy] {msg}", file=sys.stderr, flush=True)


# ----------------------------------------------------------------------------
# JSONL call log (Phase 4 / survivor F)
#
# One line per upstream HTTP roundtrip:
#   {"ts":"2026-05-27T18:14:56Z","namespace":"editor","action":"get_build_errors",
#    "params_hash":"<40-char-sha1-hex>","duration_ms":42.5,"ok":true,
#    "error_code":null,"result_bytes":1834}
#
# Path: <project-root>/Saved/Logs/MonolithCalls-<pid>.jsonl
# Opt-out: env var MONOLITH_CALL_LOG=0
# Atomicity: open(..., "ab") + threading.Lock around write+flush is sufficient
# for single-process emission. POSIX O_APPEND would give kernel-level atomicity
# for writes < PIPE_BUF (~4KB), but the lock makes the choice moot for our
# line sizes.
# ----------------------------------------------------------------------------


def _resolve_call_log_path() -> Path:
    """Resolve <project-root>/Saved/Logs/MonolithCalls-<pid>.jsonl.

    Priority:
      1. MONOLITH_PROJECT_ROOT env var (explicit override).
      2. Current working directory (proxy CWD is the project root when launched
         by Claude Code's MCP config).
    """
    root = os.environ.get("MONOLITH_PROJECT_ROOT") or os.getcwd()
    logs_dir = Path(root) / "Saved" / "Logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    return logs_dir / f"MonolithCalls-{os.getpid()}.jsonl"


def _init_call_log() -> None:
    """Open the call-log file handle once at startup. Default-enabled."""
    global _call_log_enabled, _call_log_handle

    _call_log_enabled = os.environ.get("MONOLITH_CALL_LOG", "1") != "0"
    if not _call_log_enabled:
        _log("Call log disabled (MONOLITH_CALL_LOG=0)")
        return

    try:
        path = _resolve_call_log_path()
        # Append-binary mode; OS handles end-of-file positioning. We flush after
        # each write so a crash leaves complete lines on disk.
        _call_log_handle = open(path, "ab")
        _log(f"Call log: {path}")
    except OSError as e:
        _log(f"Failed to open call log: {e} -- logging disabled")
        _call_log_enabled = False
        _call_log_handle = None


def _canonical_json(value) -> str:
    """sort_keys + tightest separators -- matches the cpp proxy."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def _extract_namespace_action(msg: dict) -> tuple[str, str]:
    """Mirror the cpp proxy's extraction logic for (namespace, action)."""
    method = msg.get("method", "") or ""
    if method != "tools/call":
        return method, ""

    params = msg.get("params") or {}
    if not isinstance(params, dict):
        return "tools/call", ""

    name = params.get("name", "") or ""
    if name.endswith("_query"):
        ns = name[: -len("_query")]
        args = params.get("arguments") or {}
        action = args.get("action", "") if isinstance(args, dict) else ""
        return ns, action or ""

    if name.startswith("monolith_"):
        return "monolith", name[len("monolith_"):]

    return name, ""


def _extract_params_for_hash(msg: dict):
    """For tools/call, hash arguments; for other methods, hash the whole params."""
    method = msg.get("method", "") or ""
    params = msg.get("params") or {}
    if not isinstance(params, dict):
        return {}

    if method == "tools/call":
        args = params.get("arguments") or {}
        return args if isinstance(args, dict) else {}
    return params


def _inspect_response(resp: str | None) -> tuple[bool, int | None, int]:
    """Returns (ok, error_code, result_bytes)."""
    if not resp:
        return False, None, 0
    try:
        parsed = json.loads(resp)
    except (json.JSONDecodeError, ValueError):
        return False, None, len(resp.encode("utf-8")) if resp else 0

    error = parsed.get("error") if isinstance(parsed, dict) else None
    if isinstance(error, dict):
        code = error.get("code")
        error_code = int(code) if isinstance(code, int) else None
        result = parsed.get("result")
        if result is not None:
            result_bytes = len(_canonical_json(result).encode("utf-8"))
        else:
            result_bytes = len(resp.encode("utf-8"))
        return False, error_code, result_bytes

    result = parsed.get("result") if isinstance(parsed, dict) else None
    if result is not None:
        result_bytes = len(_canonical_json(result).encode("utf-8"))
    else:
        result_bytes = len(resp.encode("utf-8"))
    ok = (isinstance(parsed, dict) and parsed.get("jsonrpc") == "2.0"
          and "result" in parsed and not (isinstance(result, dict) and result.get("isError")))
    return ok, None, result_bytes


def _write_call_log_line(msg: dict, resp: str | None, duration_ms: float,
                         request_uuid: str | None = None, client: str | None = None) -> None:
    """Append one JSONL line describing an upstream HTTP roundtrip."""
    if not _call_log_enabled or _call_log_handle is None:
        return
    try:
        ns, action = _extract_namespace_action(msg)
        params_for_hash = _extract_params_for_hash(msg)
        canonical = _canonical_json(params_for_hash)
        params_hash = hashlib.sha1(canonical.encode("utf-8")).hexdigest()

        ok, error_code, result_bytes = _inspect_response(resp)

        # Second-precision ISO-8601 UTC (matches cpp proxy)
        ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

        line = {
            "proxy_pid": os.getpid(),
            "request_id": msg.get("id"),
            "request_uuid": request_uuid,
            "client": client,
            "ts": ts,
            "namespace": ns,
            "action": action,
            "params_hash": params_hash,
            "duration_ms": round(duration_ms, 3),
            "ok": ok,
            "error_code": error_code,
            "result_bytes": result_bytes,
        }
        payload = (json.dumps(line, separators=(",", ":")) + "\n").encode("utf-8")
        with _call_log_lock:
            _call_log_handle.write(payload)
            _call_log_handle.flush()
    except Exception as e:
        # Never let logging crash the proxy.
        _log(f"Call-log write failed: {e}")


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class _TransportTrace:
    def __init__(self):
        self.may_have_written = False
        self.peer = None
        self.tls_context = None


class _ObservedConnection:
    def __init__(self, *args, trace, **kwargs):
        self.trace = trace
        super().__init__(*args, **kwargs)

    def send(self, data):
        # HTTPConnection.send otherwise hides connect and sendall in one call.
        # A failed connect/TLS handshake cannot have written the HTTP request.
        if self.sock is None:
            self.connect()
        self.trace.peer = (self.sock.family, self.sock.getpeername())
        self.trace.tls_context = getattr(self, "_context", None)
        # sendall may write a prefix before raising; conservatively unknown.
        self.trace.may_have_written = True
        return super().send(data)


class _ObservedHTTPConnection(_ObservedConnection, http.client.HTTPConnection):
    pass


class _ObservedHTTPSConnection(_ObservedConnection, http.client.HTTPSConnection):
    pass


class _ObservedHTTPHandler(urllib.request.HTTPHandler):
    def __init__(self, trace):
        super().__init__()
        self.trace = trace

    def http_open(self, req):
        def connection(host, **kwargs):
            return _ObservedHTTPConnection(host, trace=self.trace, **kwargs)
        return self.do_open(connection, req)


class _ObservedHTTPSHandler(urllib.request.HTTPSHandler):
    def __init__(self, trace):
        super().__init__()
        self.trace = trace

    def https_open(self, req):
        def connection(host, **kwargs):
            return _ObservedHTTPSConnection(host, trace=self.trace, **kwargs)
        return self.do_open(connection, req, context=self._context)


def _direct_opener(trace=None):
    handlers = [urllib.request.ProxyHandler({}), _NoRedirect()]
    if trace is not None:
        handlers.extend((_ObservedHTTPHandler(trace), _ObservedHTTPSHandler(trace)))
    return urllib.request.build_opener(*handlers)


def _post_monolith(body: str, timeout: float = TIMEOUT,
                   request_uuid: str | None = None, client: str | None = None,
                   trace: _TransportTrace | None = None) -> str | None:
    """POST JSON-RPC to Monolith. Returns response body or None on failure."""
    try:
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                   "MCP-Protocol-Version": "2025-03-26"}
        if request_uuid is not None:
            headers["X-Monolith-Request-Id"] = request_uuid
            headers["X-Monolith-Client"] = client
        req = urllib.request.Request(
            MONOLITH_URL,
            data=body.encode("utf-8"),
            headers=headers,
            method="POST",
        )
        try:
            upstream = _direct_opener(trace).open(req, timeout=timeout)
        except urllib.error.HTTPError as rejection:
            # HTTP rejection bodies still carry JSON-RPC errors. Validate them
            # exactly like successful HTTP responses rather than losing evidence.
            upstream = rejection
        with upstream as resp:
            response = resp.read().decode("utf-8")
            parsed = json.loads(response)
            request = json.loads(body)
            if (not isinstance(parsed, dict) or parsed.get("jsonrpc") != "2.0"
                    or isinstance(parsed.get("id"), bool)
                    or "id" not in parsed
                    or ("result" in parsed) == ("error" in parsed)):
                raise ValueError("Invalid upstream JSON-RPC response")
            error = parsed.get("error")
            if "error" in parsed and (not isinstance(error, dict)
                    or isinstance(error.get("code"), bool) or not isinstance(error.get("code"), int)
                    or not isinstance(error.get("message"), str)):
                raise ValueError("Malformed upstream error")
            if parsed["id"] != request.get("id"):
                # Pre-parse rejections cannot recover the request ID. Only the
                # explicit invalid-request / never-executed contract can rebind it.
                data = error.get("data") if isinstance(error, dict) else None
                if (parsed["id"] is not None or not isinstance(error, dict)
                        or error.get("code") != -32600 or not isinstance(data, dict)
                        or data.get("executed") is not False):
                    raise ValueError("Mismatched upstream response id")
                parsed["id"] = request.get("id")
                return json.dumps(parsed)
            return response
    except (urllib.error.URLError, OSError, TimeoutError, ValueError, http.client.HTTPException) as e:
        _log(f"Monolith unreachable: {e}")
        return None


def _write(stdout, msg: str) -> None:
    """Write a JSON-RPC message to stdout (thread-safe)."""
    with _stdout_lock:
        stdout.write(msg + "\n")
        stdout.flush()


def _result(id, result: dict) -> str:
    return json.dumps({"jsonrpc": "2.0", "id": id, "result": result})


def _tool_error(id, message: str) -> str:
    """Return a tool result with isError=true (graceful failure, not protocol error)."""
    return json.dumps({
        "jsonrpc": "2.0",
        "id": id,
        "result": {
            "content": [{"type": "text", "text": message}],
            "isError": True,
        },
    })


def _transport_error(identifier, request_uuid, may_have_written, tool_name):
    if may_have_written:
        error_class = "unknown_outcome"
        message = (f"Monolith transport failed for '{tool_name}'. Execution outcome is unknown. "
                   "Inspect editor state before retrying a mutation; no automatic retry was sent.")
    else:
        error_class = "not_sent"
        message = (f"Monolith transport failed before sending '{tool_name}'. The request did not execute; "
                   "retry when the editor connection is available. No automatic retry was sent.")
    structured = {"error": message, "code": -32603,
                  "data": {"class": error_class, "executed": "unknown" if may_have_written else False,
                           "retryable": not may_have_written, "request_id": request_uuid}}
    return _result(identifier, {"isError": True, "structuredContent": structured,
                               "content": [{"type": "text", "text": json.dumps(structured)}]})


def _observe_lease(msg, response, trace):
    params = msg.get("params", {})
    if params.get("name") != "monolith_coordination":
        return
    args = params.get("arguments", {})
    if not isinstance(args, dict):
        return
    nested = args.get("params")
    if isinstance(nested, str):
        try:
            nested = json.loads(nested)
        except (ValueError, TypeError, RecursionError):
            nested = None
    if isinstance(nested, dict):
        # Core tools overlay nested object/string params on top-level extras.
        args = dict(args, **nested)
    operation = args.get("operation")
    if operation not in ("acquire", "release"):
        return
    payload = json.loads(response)
    result = payload.get("result")
    if "error" in payload or not isinstance(result, dict) or result.get("isError"):
        return
    if operation == "release":
        token = args.get("_lease_token")
        if isinstance(token, str):
            with _owned_leases_lock:
                _owned_leases.pop(token, None)
        return
    values = [result.get("structuredContent")]
    # Older/text-only successful responses still carry the action's JSON object.
    content_items = result.get("content", [])
    for content in content_items if isinstance(content_items, list) else []:
        if isinstance(content, dict) and content.get("type") == "text":
            try:
                values.append(json.loads(content.get("text", "")))
            except (ValueError, TypeError):
                pass
    for value in values:
        token = value.get("_lease_token") if isinstance(value, dict) else None
        if isinstance(token, str) and token and trace.peer is not None:
            with _owned_leases_lock:
                _owned_leases[token] = (trace.peer, trace.tls_context)
            return


def _wait_socket(sock, events, deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("EOF release deadline expired")
    with selectors.DefaultSelector() as selector:
        selector.register(sock, events)
        if not selector.select(remaining):
            raise TimeoutError("EOF release deadline expired")


def _send_eof_release(body, peer, tls_context, request_uuid, client, deadline, trace):
    # Reuse the successful acquire's numeric peer: no uncancellable DNS at EOF.
    # All connect/TLS/send operations are nonblocking and share one deadline.
    family, address = peer
    parts = urllib.parse.urlsplit(MONOLITH_URL)
    host = parts.hostname.encode("idna").decode("ascii")
    host_header = f"[{host}]" if ":" in host else host
    if parts.port is not None:
        host_header += f":{parts.port}"
    target = urllib.parse.urlunsplit(("", "", parts.path or "/", parts.query, ""))
    encoded_body = body.encode("utf-8")
    headers = (f"POST {target} HTTP/1.1\r\nHost: {host_header}\r\n"
               "Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n"
               "MCP-Protocol-Version: 2025-03-26\r\nConnection: close\r\n"
               f"X-Monolith-Request-Id: {request_uuid}\r\nX-Monolith-Client: {client}\r\n"
               f"Content-Length: {len(encoded_body)}\r\n\r\n").encode("ascii")
    sock = socket.socket(family, socket.SOCK_STREAM)
    try:
        sock.setblocking(False)
        error = sock.connect_ex(address)
        pending = {0, errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EALREADY,
                   getattr(errno, "WSAEWOULDBLOCK", 10035)}
        if error not in pending:
            raise OSError(error, "EOF release connect failed")
        if error:
            _wait_socket(sock, selectors.EVENT_WRITE, deadline)
            error = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
            if error:
                raise OSError(error, "EOF release connect failed")
        if parts.scheme == "https":
            # Context was created and used by the successful HTTPS acquire.
            # Preserve certificate validation and the original hostname/SNI.
            if tls_context is None:
                raise ValueError("No validated TLS context for EOF release")
            sock = tls_context.wrap_socket(sock, server_hostname=host, do_handshake_on_connect=False)
            while True:
                if time.monotonic() >= deadline:
                    raise TimeoutError("EOF release deadline expired")
                try:
                    sock.do_handshake()
                    break
                except ssl.SSLWantReadError:
                    _wait_socket(sock, selectors.EVENT_READ, deadline)
                except ssl.SSLWantWriteError:
                    _wait_socket(sock, selectors.EVENT_WRITE, deadline)
        pending_data = memoryview(headers + encoded_body)
        while pending_data:
            if time.monotonic() >= deadline:
                raise TimeoutError("EOF release deadline expired")
            try:
                # Even a failed write may have submitted a prefix.
                trace.may_have_written = True
                written = sock.send(pending_data)
                if written == 0:
                    raise ConnectionError("EOF release socket closed during send")
                pending_data = pending_data[written:]
            except ssl.SSLWantReadError:
                _wait_socket(sock, selectors.EVENT_READ, deadline)
            except (ssl.SSLWantWriteError, BlockingIOError):
                _wait_socket(sock, selectors.EVENT_WRITE, deadline)
        # Best-effort submission only; no response wait or success assertion.
    finally:
        sock.close()


def _release_owned_leases():
    with _owned_leases_lock:
        leases = list(_owned_leases.items())
        _owned_leases.clear()
    deadline = time.monotonic() + 2.0
    for token, (peer, tls_context) in leases:
        if time.monotonic() >= deadline:
            break
        request_uuid = str(uuid.uuid4())
        client = _client_identity()
        msg = {"jsonrpc": "2.0", "id": "proxy-eof-" + request_uuid, "method": "tools/call",
               "params": {"name": "monolith_coordination",
                          "arguments": {"operation": "release", "_lease_token": token}}}
        trace = _TransportTrace()
        started = time.perf_counter()
        try:
            _send_eof_release(json.dumps(msg), peer, tls_context, request_uuid, client, deadline, trace)
        except (OSError, ValueError) as exc:
            _log(f"Best-effort EOF release transport failed: {type(exc).__name__}")
        # No response was read, so a submitted release always remains unknown.
        response = _transport_error(msg["id"], request_uuid, trace.may_have_written, "monolith_coordination")
        _write_call_log_line(msg, response, (time.perf_counter() - started) * 1000.0, request_uuid, client)


def _jsonrpc_error(id, code: int, message: str) -> str:
    """Return a JSON-RPC protocol-level error."""
    return json.dumps({
        "jsonrpc": "2.0",
        "id": id,
        "error": {"code": code, "message": message},
    })


def _sanitize_cache_part(value: str) -> str:
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in value)


def _tools_cache_path() -> Path:
    base = Path(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir())
    cache_dir = base / "Monolith"
    cache_dir.mkdir(parents=True, exist_ok=True)

    host_port = MONOLITH_HEALTH.replace("http://", "").replace("https://", "")
    host_port = host_port.split("/", 1)[0]
    scope = MONOLITH_URL + "|" + os.path.abspath(os.environ.get("MONOLITH_PROJECT_ROOT") or os.getcwd())
    digest = hashlib.sha1(scope.encode("utf-8")).hexdigest()[:16]
    return cache_dir / f"monolith_proxy_tools_{_sanitize_cache_part(host_port)}_{digest}.json"


def _query_tool_schema() -> dict:
    return {
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "description": "The action to execute. Use monolith_discover first when the editor is available.",
            },
            "params": {
                "type": "object",
                "description": "Parameters for the selected action.",
            },
            "_fields": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
            },
            "_omit": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
            },
            "_compact_json": {
                "type": "boolean",
                "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
            },
        },
        "required": ["action"],
    }


def _empty_object_schema() -> dict:
    return {
        "type": "object",
        "properties": {
            "_fields": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
            },
            "_omit": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
            },
            "_compact_json": {
                "type": "boolean",
                "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
            },
        },
    }


def _make_tool(name: str, description: str, schema: dict) -> dict:
    return {"name": name, "description": description, "inputSchema": schema}


def _seed_tools() -> list[dict]:
    tools = []
    for name in CORE_QUERY_TOOLS:
        domain = name[:-6] if name.endswith("_query") else name
        tools.append(_make_tool(
            name,
            f"Query the {domain} domain. The editor may be offline at session start; retry after Monolith is healthy.",
            _query_tool_schema(),
        ))

    tools.append(_make_tool(
        "monolith_discover",
        "List available tool namespaces and their actions. Pass namespace and optional category to filter.",
        {
            "type": "object",
            "properties": {
                "namespace": {"type": "string", "description": "Optional: filter to a specific namespace"},
                "category": {"type": "string", "description": "Optional: filter actions within the namespace by category"},
                "include_action_names": {"type": "boolean", "description": "Optional: include action-name arrays in the namespace inventory (default false)."},
                "_fields": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
                },
                "_omit": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
                },
                "_compact_json": {
                    "type": "boolean",
                    "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
                },
            },
        },
    ))
    tools.append(_make_tool(
        "monolith_status",
        "Get Monolith server health: version, uptime, port, registered action count, and module status.",
        _empty_object_schema(),
    ))
    tools.append(_make_tool(
        "monolith_update",
        "Check for or install Monolith updates from GitHub Releases.",
        {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "description": "'check' to compare versions, 'install' to download and stage update",
                    "default": "check",
                },
                "_fields": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
                },
                "_omit": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
                },
                "_compact_json": {
                    "type": "boolean",
                    "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
                },
            },
        },
    ))
    tools.append(_make_tool(
        "monolith_reindex",
        "Re-index the Monolith project database. Requires the editor-side Monolith server.",
        _empty_object_schema(),
    ))
    tools.append(_make_tool(
        "monolith_guide",
        "Read Monolith workflow recipes, onboarding, decisions, and recovery guidance.",
        {"type": "object", "properties": {
            "section": {"type": "string", "description": "Optional section: onboarding, recipes, decisions, errors, skills_map, gotchas. Omit for all sections."},
        }},
    ))
    tools.append(_make_tool(
        "monolith_coordination",
        "Acquire, renew, release, or inspect an editor-wide workflow lease.",
        {"type": "object", "properties": {
            "operation": {"type": "string", "enum": ["status", "acquire", "renew", "release"], "default": "status"},
            "owner": {"type": "string", "description": "Required for acquire: nonempty owner label, at most 128 characters."},
            "ttl_seconds": {"type": "number", "minimum": 10, "maximum": 600,
                            "description": "Acquire defaults to 120 seconds; renew retains the current duration when omitted."},
            "_lease_token": {"type": "string", "description": "Token returned by acquire; required for renew and release."},
        }},
    ))
    for tool in tools:
        if tool["name"].startswith("monolith_"):
            tool["inputSchema"]["properties"].setdefault("_lease_token", {
                "type": "string", "description": "Optional owner token for a protected core tool call.",
            })
    return tools


def _write_tools_cache(resp: str) -> None:
    try:
        payload = json.loads(resp)
        tools = payload.get("result", {}).get("tools", [])
        if isinstance(tools, list) and tools:
            path = _tools_cache_path()
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                             prefix=path.name, delete=False) as temp:
                temp.write(json.dumps(tools))
            try:
                os.replace(temp.name, path)
            finally:
                Path(temp.name).unlink(missing_ok=True)
    except Exception as e:
        _log(f"Failed to write tools/list cache: {e}")


def _read_tools_cache() -> list[dict] | None:
    try:
        path = _tools_cache_path()
        if not path.exists():
            return None
        # Windows CRT readers do not share DELETE access. A rename can publish
        # a complete snapshot before MoveFileEx releases its DELETE handle.
        # Retry only that transient local open failure, never JSON parsing or
        # the upstream editor operation (maximum extra wait: 20 ms).
        # https://devblogs.microsoft.com/oldnewthing/20211022-00/?p=105822
        for attempt in range(3):
            try:
                contents = path.read_text(encoding="utf-8")
                break
            except PermissionError:
                if attempt == 2:
                    raise
                time.sleep(0.01)
        tools = json.loads(contents)
        if isinstance(tools, list) and tools:
            return tools
    except Exception as e:
        _log(f"Failed to read tools/list cache: {e}")
    return None


def _fallback_tools_list(msg: dict) -> str:
    cached = _read_tools_cache()
    if cached:
        _log("Monolith down during tools/list — returning cached tools")
        return _result(msg.get("id"), {"tools": cached})

    _log("Monolith down during tools/list — returning seed tools")
    return _result(msg.get("id"), {"tools": _seed_tools()})


def _check_monolith_up() -> bool | None:
    """Return up/refused, or None when busy or otherwise indeterminate."""
    try:
        req = urllib.request.Request(MONOLITH_HEALTH, method="GET")
        with _direct_opener().open(req, timeout=3) as resp:
            return True if resp.status == 200 else None
    except (urllib.error.URLError, OSError) as exc:
        reason = getattr(exc, "reason", exc)
        if isinstance(reason, ConnectionRefusedError) or getattr(reason, "errno", None) == errno.ECONNREFUSED:
            return False
        return None


def _send_list_changed(stdout) -> bool:
    """Send tools/list_changed notification. Returns False if stdout is broken."""
    try:
        _write(stdout, json.dumps({
            "jsonrpc": "2.0",
            "method": "notifications/tools/list_changed",
        }))
        return True
    except (BrokenPipeError, OSError):
        return False


def check_monolith_state_change(stdout) -> None:
    """Check for state transition and notify if changed."""
    global _monolith_was_up
    is_up = _check_monolith_up()
    if is_up is None:
        return

    if _monolith_was_up is not None and is_up != _monolith_was_up:
        direction = "online" if is_up else "offline"
        _log(f"Monolith went {direction} — sending tools/list_changed")
        _send_list_changed(stdout)

    _monolith_was_up = is_up


def _health_poll_thread(stdout) -> None:
    """Background thread that polls Monolith and sends list_changed on state transitions."""
    if _stop_poll.wait(POLL_START_DELAY):
        return
    _log(f"Health poll started (interval={POLL_INTERVAL}s)")

    while not _stop_poll.is_set():
        try:
            check_monolith_state_change(stdout)
        except (BrokenPipeError, OSError):
            _log("stdout broken, health poll exiting")
            return
        except Exception as e:
            _log(f"Health poll error: {e}")

        _stop_poll.wait(POLL_INTERVAL)


DEFAULT_INSTRUCTIONS = (
    "Monolith MCP server for Unreal Engine. "
    "Before calling a domain action, check its schema instead of guessing: "
    "monolith_discover() lists namespaces, monolith_discover('<namespace>') lists a "
    "namespace's action names + descriptions (terse by default — pass detail=true to "
    "inline param schemas), and describe_query('action_schema', ...) returns one action's "
    "exact parameter schema. monolith_guide(section='recipes') gives cross-namespace "
    "workflows, decision matrices, and gotchas. For multi-agent edits acquire monolith_coordination "
    "and pass _lease_token on domain calls; renew before expiry. Transport timeouts have unknown execution outcome."
)


def _read_instructions_cache() -> str | None:
    try:
        path = Path(str(_tools_cache_path()) + ".instructions")
        for attempt in range(3):
            try:
                contents = path.read_text(encoding="utf-8")
                break
            except PermissionError:
                if attempt == 2:
                    raise
                time.sleep(0.01)
        value = json.loads(contents)
        return value if isinstance(value, str) else None
    except (OSError, ValueError):
        return None


def _write_instructions_cache(value: str) -> None:
    try:
        path = Path(str(_tools_cache_path()) + ".instructions")
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=path.name, delete=False) as temp:
            temp.write(json.dumps(value))
        try:
            os.replace(temp.name, path)
        finally:
            Path(temp.name).unlink(missing_ok=True)
    except OSError as exc:
        _log(f"Failed to write instructions cache: {exc}")


def handle_initialize(msg: dict, fetch_instructions: bool = True) -> str:
    """Negotiate locally, importing only server instructions on the metadata worker."""
    client_version = msg.get("params", {}).get("protocolVersion", "2025-11-25")
    supported = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}
    version = client_version if isinstance(client_version, str) and client_version in supported else "2025-11-25"

    instructions = _read_instructions_cache()
    if fetch_instructions:
        upstream = dict(msg, params={
            "protocolVersion": version, "capabilities": {},
            "clientInfo": {"name": PROXY_NAME, "version": PROXY_VERSION},
        })
        response = _post_monolith(json.dumps(upstream), timeout=1.0)
        try:
            result = json.loads(response or "null")
            result = result.get("result") if isinstance(result, dict) else None
            value = result.get("instructions") if isinstance(result, dict) else None
            if isinstance(value, str):
                instructions = value
                _write_instructions_cache(value)
        except (ValueError, TypeError):
            pass  # Keep the previous complete snapshot on malformed upstream metadata.

    return _result(msg.get("id"), {
        "protocolVersion": version,
        "capabilities": {
            "tools": {"listChanged": True},
        },
        "serverInfo": {"name": PROXY_NAME, "version": PROXY_VERSION},
        "instructions": instructions if instructions is not None else DEFAULT_INSTRUCTIONS,
    })


def handle_ping(msg: dict) -> str:
    return _result(msg.get("id"), {})


def handle_tools_list(msg: dict) -> str:
    """Forward tools/list to Monolith. Stable cached/seed list if down."""
    t0 = time.perf_counter()
    resp = _post_monolith(json.dumps(msg))
    duration_ms = (time.perf_counter() - t0) * 1000.0
    _write_call_log_line(msg, resp, duration_ms)

    if resp:
        _write_tools_cache(resp)
        return resp
    return _fallback_tools_list(msg)


def handle_tools_call(msg: dict) -> str:
    """Forward tools/call to Monolith. Graceful error if down."""
    request_uuid = str(uuid.uuid4())
    client = _client_identity()
    trace = _TransportTrace()
    t0 = time.perf_counter()
    resp = _post_monolith(json.dumps(msg), request_uuid=request_uuid, client=client, trace=trace)
    duration_ms = (time.perf_counter() - t0) * 1000.0
    if resp:
        _observe_lease(msg, resp, trace)
    else:
        resp = _transport_error(msg.get("id"), request_uuid, trace.may_have_written,
                                msg.get("params", {}).get("name", "unknown"))
    _write_call_log_line(msg, resp, duration_ms, request_uuid, client)
    return resp


def main() -> None:
    # Use binary-safe IO for Windows compatibility
    stdin = TextIOWrapper(sys.stdin.buffer, encoding="utf-8", newline="\n")
    stdout = TextIOWrapper(sys.stdout.buffer, encoding="utf-8", newline="\n")

    _log(f"Started. Forwarding to {MONOLITH_URL}")

    _init_call_log()

    # Start background health poller
    poller = threading.Thread(
        target=_health_poll_thread,
        args=(stdout,),
        daemon=True,
        name="monolith-health-poll",
    )
    poller.start()

    # Bound queued + running work. Never block stdin on an HTTP call: ping and
    # initialize stay responsive even when all editor workers are occupied.
    slots = threading.BoundedSemaphore(MAX_IN_FLIGHT + MAX_QUEUED)
    metadata_slots = threading.BoundedSemaphore(1)
    active = set()
    active_lock = threading.Lock()

    def dispatch(msg, metadata=False):
        try:
            method = msg["method"]
            if method == "tools/list":
                response = handle_tools_list(msg)
            elif method == "tools/call":
                response = handle_tools_call(msg)
            elif method == "initialize":
                response = handle_initialize(msg)
            else:
                response = _jsonrpc_error(msg["id"], -32601, f"Method not found: {method}")
            _write(stdout, response)
        except Exception as exc:
            _log(f"Request failed: {exc}")
            _write(stdout, _jsonrpc_error(msg["id"], -32603, "Internal proxy error; execution outcome unknown"))
        finally:
            with active_lock:
                active.discard(_canonical_json(msg["id"]))
            (metadata_slots if metadata else slots).release()

    try:
        with ThreadPoolExecutor(max_workers=MAX_IN_FLIGHT, thread_name_prefix="monolith") as pool, \
                ThreadPoolExecutor(max_workers=1, thread_name_prefix="monolith-metadata") as metadata_pool:
            for line in stdin:
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line)
                except ValueError:
                    _write(stdout, _jsonrpc_error(None, -32700, "Invalid JSON"))
                    continue
                if (not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0"
                        or not isinstance(msg.get("method"), str)
                        or ("id" in msg and (isinstance(msg["id"], bool)
                            or not isinstance(msg["id"], (str, int, float))))):
                    _write(stdout, _jsonrpc_error(None, -32600, "Invalid JSON-RPC request"))
                    continue
                # Notifications must not execute tools or emit responses. Cancellation
                # cannot roll back an already-dispatched editor action.
                if "id" not in msg:
                    continue
                if not isinstance(msg.get("params", {}), dict):
                    _write(stdout, _jsonrpc_error(msg["id"], -32602, "params must be an object"))
                    continue
                method = msg["method"]
                if method == "ping":
                    _write(stdout, handle_ping(msg))
                    continue
                if method == "tools/call":
                    params = msg.get("params", {})
                    if (not isinstance(params.get("name"), str)
                            or not isinstance(params.get("arguments", {}), dict)):
                        _write(stdout, _jsonrpc_error(msg["id"], -32602, "name must be a string; arguments must be an object"))
                        continue
                metadata = method == "initialize"
                request_slots = metadata_slots if metadata else slots
                key = _canonical_json(msg["id"])
                with active_lock:
                    duplicate = key in active
                    accepted = not duplicate and request_slots.acquire(blocking=False)
                    if accepted:
                        active.add(key)
                if not accepted:
                    if metadata and not duplicate:
                        _write(stdout, handle_initialize(msg, fetch_instructions=False))
                        continue
                    code = -32600 if duplicate else -32001
                    message = "Request id already in flight" if duplicate else "Proxy queue full; request not executed. Retry with backoff."
                    _write(stdout, _jsonrpc_error(msg["id"], code, message))
                    continue
                (metadata_pool if metadata else pool).submit(dispatch, msg, metadata)
            # EOF: stop new health polls before accepted workers finish draining.
            _stop_poll.set()
    finally:
        _stop_poll.set()
        _release_owned_leases()
        poller.join(timeout=4)
        if _call_log_handle is not None:
            _call_log_handle.close()


if __name__ == "__main__":
    main()
