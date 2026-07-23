# Monolith MCP Proxy Configuration

## Current Configuration

The Monolith MCP is configured to use the C++ proxy executable:

```json
{
  "command": "<project-root>/Plugins/Monolith/Binaries/monolith_proxy.exe",
  "args": []
}
```

This configuration is set in:
- `.mcp.json` (project-level)
- `~/.claude.json` (user-level)

## Rollback to Python Proxy (if needed)

If the C++ proxy encounters issues, you can revert to the Python proxy by updating both config files to:

```json
{
  "command": "python",
  "args": ["<project-root>/Scripts/monolith_proxy.py"]
}
```

Update the monolith entry in:
1. `<project-root>/.mcp.json`
2. `%USERPROFILE%\.claude.json` (Windows) or `~/.claude.json` (macOS / Linux)

Then restart Claude Code.

## Proxy Details

- **Python proxy:** `Scripts/monolith_proxy.py` — Stdio-to-HTTP proxy, survives editor restarts via background health polling
- **C++ proxy:** `Plugins/Monolith/Binaries/monolith_proxy.exe` — Native executable, faster startup
- **Backend:** Both connect to the same Monolith HTTP server running in the Unreal Editor
- **Editor-down startup:** Both proxies return a cached Monolith tool list when available, or a stable seed list of namespace/meta tools. This prevents MCP clients that do not fully refresh on `tools/list_changed` from starting with an empty Monolith catalog.

## Timeouts and the honest-timeout contract

Both proxies wait **30 seconds** for the editor to answer a request. Two
environment variables tune this; both are read once at proxy startup, so
changing them requires a proxy restart.

| Variable | Default | Meaning |
|---|---|---|
| `MONOLITH_TIMEOUT` | `30` | Seconds to wait for an upstream HTTP response. Invalid or non-positive values fall back to the default (with a stderr warning). |
| `MONOLITH_TIMEOUT_RESEND_GUARD` | `60` | Seconds during which an *identical* `tools/call` that already timed out is refused instead of forwarded. Set to `0` to disable the guard. |

### Why 30 s is still the default

The editor's MCP server executes every action on the game thread, one at a
time. Waiting longer does not make the work finish sooner — it just blocks
every other caller for longer. The supported answer for long work is the job
system (`jobs` namespace), not a bigger wall-clock budget. `MONOLITH_TIMEOUT`
exists for actions that have not been converted to jobs yet.

### Timeout is not "the editor is down"

A timeout and a dead socket are different events and the proxies now report
them differently:

- **Timeout** — the editor is probably still running and still executing the
  request; its result will be discarded when it finishes. The message says so,
  points at `jobs_query` (`list` to find a running job's `job_id`, then `poll`),
  notes that `animation_query action='rebuild_pose_search_index'` is async by
  default, and tells the caller not to retry. No `job_id` is invented — the
  proxy does not know one.
- **Connection refused / closed** — nothing answered the socket, so the request
  never executed. This still reports as "Monolith MCP is not available (Unreal
  Editor not running)".

### Retry behaviour

Neither proxy has ever retried a failed request, and neither does now — one
request, one attempt. What is new is the **resend guard**: after a timeout the
call's `(name, arguments)` signature is remembered, and an identical call
arriving within `MONOLITH_TIMEOUT_RESEND_GUARD` seconds is answered with an
explanation instead of being forwarded. This stops a *client* retry from
queueing a duplicate copy of work that is still running. The C++ proxy's
unrelated 3-second repeat-call dedup window is untouched; it is far too short to
cover a call that already burned the whole timeout budget.

The user-visible message text is kept word-for-word identical between
`Tools/MonolithProxy/monolith_proxy.cpp` and `Scripts/monolith_proxy.py`. Edit
both in the same commit.

## Call Log

Both proxies append one JSONL line per upstream MCP roundtrip to:

```
<project-root>/Saved/Logs/MonolithCalls.jsonl
```

Path resolution: `MONOLITH_PROJECT_ROOT` env var if set, otherwise the proxy's
current working directory (Claude Code launches the proxy with the project root
as CWD). The `Saved/Logs/` parent directories are created on first run.

### Schema (one object per line, terminated by `\n`)

```json
{"ts":"2026-05-27T18:14:56Z","namespace":"editor","action":"get_build_errors","params_hash":"da39a3ee5e6b4b0d3255bfef95601890afd80709","duration_ms":42.5,"ok":true,"error_code":null,"result_bytes":1834}
```

| Field | Type | Notes |
|---|---|---|
| `ts` | string | ISO-8601 UTC, second precision. |
| `namespace` | string | For `*_query` tools: the prefix (e.g. `editor`). For `monolith_*` tools: `monolith`. For non-`tools/call` methods: the method name (`initialize`, `tools/list`, `ping`). |
| `action` | string | For `*_query`: the `action` argument. For `monolith_*`: the suffix (`discover`, `status`, etc.). Empty otherwise. |
| `params_hash` | string | 40-char hex SHA-1 over canonicalised JSON of the params dict (`sort_keys=True`, tightest separators). Used to recognise repeat calls without storing arguments. NOT 32-bit FCrc — collision-safe. |
| `duration_ms` | number | Wall time between sending the upstream HTTP request and receiving the response. |
| `ok` | bool | `true` iff the response has no JSON-RPC `error` field. |
| `error_code` | int or null | The JSON-RPC `error.code` when `ok` is false; null otherwise. |
| `result_bytes` | int | Byte length of the serialised `result` payload (or the full response body if no `result`). |

### Opt-out

Set the environment variable `MONOLITH_CALL_LOG=0` before launching the proxy.
This disables emission entirely — no file handle is opened, no writes are made.
Any other value (including unset) leaves logging enabled. The env var is read
once at startup; toggling it mid-session requires a proxy restart.

### Use cases

- Post-hoc grep / analysis of which actions an agent session called.
- Spot the silent retries hidden by the dedup window.
- Pipe through your own log-tailing tool for live tailing.
- Cheap input to future Markov-style breadcrumb analytics (deferred — substrate
  ships first, consumers later).

### Privacy

Local-only. Nothing is uploaded. The `params_hash` is one-way — the original
parameter values cannot be recovered from the log. Filenames and asset paths
inside the hashed JSON are not extractable from the line.

### Rotation / reset

User-managed. Delete the file to start fresh; the proxies recreate it on the
next call. No automatic rotation in v1 — the file is small (≈200 bytes/line) and
single-user.

### Crash-reporter exclusion

UE's crash reporter sweeps editor logs from `Saved/Logs/`, not arbitrary JSONL.
If a downstream crash collector pattern is added that sweeps `Saved/Logs/*`,
add `MonolithCalls.jsonl` to its exclusion list. The file is intentionally
local-only.
