# Monolith stdio proxy

Build on Windows from the plugin root:

```powershell
powershell -NoProfile -File Scripts/build_proxy.ps1
```

The script discovers Visual Studio CMake, checks configure/build results, and
copies the executable to `Binaries/monolith_proxy.exe`. `build.bat` and
`build_proxy.bat` are compatibility wrappers. On macOS/Linux use the standard
library Python proxy at `Scripts/monolith_proxy.py`.

Configure the actual absolute executable/script path in your MCP host. Choose
project or user configuration deliberately; duplicating servers in both scopes
is unnecessary. [Multi-agent setup](../../Docs/MULTI_AGENT.md) includes examples.

Both implementations accept distinct requests concurrently with bounded queues,
keep ping local, and drain accepted requests on EOF. Same-argument calls are
independent operations, not inferred retries. Use editor leases for multi-call
changes; a timeout cannot prove whether an action executed. Failed requests are
never replayed and redirects are not followed.

`MONOLITH_MAX_IN_FLIGHT=8`, `MONOLITH_MAX_QUEUED=64`, and
`MONOLITH_TIMEOUT_SECONDS=120` are defaults. Native split-editor mode limits
`editor_read_query` to an explicit diagnostic allowlist; it is not a security
sandbox for other tools. Both implementations validate response IDs and JSON-RPC
error shape. Offline tool caches are atomically replaced and scoped to endpoint
and project. An offline catalog does not prove the editor is ready.

## Local call logs

Logs are written to `<project-root>/Saved/Logs/MonolithCalls-<pid>.jsonl`.
`MONOLITH_PROJECT_ROOT` selects the project, otherwise CWD is used.
`MONOLITH_CALL_LOG=0` disables logging. Each record contains `proxy_pid`,
`request_id`, UTC timestamp, namespace/action, argument hash, duration, success,
error code and response size. MCP `isError` counts as failure. Raw arguments and
lease tokens are not logged. Hashes support correlation; they are not encryption
or a confidentiality guarantee for predictable arguments. Log rotation and
retention are user-managed. Each proxy owns a separate file, avoiding cross-process
append contention.

## Verification

```powershell
$env:MONOLITH_TEST_NATIVE_PROXY = (Resolve-Path Binaries/monolith_proxy.exe).Path
python -m unittest discover -s Scripts/tests -v
```

Live editor tests are opt-in through `MONOLITH_LIVE_URL`; see
[validation evidence](../../Docs/VALIDATION_UE57.md).
