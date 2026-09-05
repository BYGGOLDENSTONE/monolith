# Monolith with multiple AI agents

Monolith supports multiple clients sharing an editor through bounded proxy concurrency and a cooperative editor lease. Unreal actions still execute on the game thread. Parallelize planning, research and independent source changes; serialize editor workflows and shared-file integration.

## Diagnose the actual failure

| Symptom | Check | Action |
|---|---|---|
| Lead sees tools, worker cannot call them | The worker's host-provided tool inventory | Configure the host's MCP/tool inheritance, or route editor operations through the lead. A skill cannot grant MCP access. |
| All clients fail to connect | Editor process, enabled plugin, project identity, URL/port, logs | Correct the endpoint and use `monolith_status` when reachable. A timeout is not evidence that the editor is closed. |
| Slow call blocks unrelated request delivery | Proxy version and execution limits | Use the updated proxy. UObject operations still queue on the game thread. |
| Duplicate identical operations behave incorrectly | Separate intent versus retry; request IDs | Give each request its own ID. The proxy does not deduplicate calls by arguments. |
| Workflows overwrite each other's intermediate changes | Asset ownership and coordination status | Acquire a lease for the complete read/change/compile/save/readback sequence. |
| Namespace/action missing | `monolith_discover` and optional modules | Inspect live schemas and module availability; an offline fallback catalog is not proof of installed functionality. |

## Connection examples

The portable Python proxy accepts stdio MCP and forwards to a loopback HTTP endpoint. Configure the actual absolute plugin path; workers can have separate proxy processes pointing to the same editor. See [generic JSON](../Templates/.mcp.json.proxy.example) and [Codex TOML](../Templates/codex-config.toml.example). The TOML fields follow the [official MCP configuration documentation](https://learn.chatgpt.com/docs/extend/mcp?surface=cli). These are templates, not changes to the user's installed configuration. Restart/reconnect the host's MCP server after updating its proxy. Rebuild the plugin and restart the editor to load C++ changes.

| Environment variable | Default | Accepted range | Meaning |
|---|---:|---:|---|
| `MONOLITH_MAX_IN_FLIGHT` | 8 | 1–32 | Concurrent forwarded requests per proxy process |
| `MONOLITH_MAX_QUEUED` | 64 | 0–1024 | Additional pending requests per proxy process |
| `MONOLITH_TIMEOUT_SECONDS` | 120 | 1–3600 | HTTP request timeout in seconds |
| `MONOLITH_URL` | `http://localhost:9316/mcp` | URL | Editor endpoint; select the intended project |
| `MONOLITH_CLIENT_NAME` | `proxy` | Diagnostic name | Prefix for the HTTP client identity `<name>/<pid>` |
| `MONOLITH_CALL_LOG` | 1 | `0` disables | Enable call logs and startup retention cleanup |
| `MONOLITH_CALL_LOG_MAX_MB` | 16 | Greater than 0, at most 1024 | Log rotation threshold in MiB; finite fractional values are accepted |
| `MONOLITH_PROJECT_ROOT` | Working directory | Directory | Root containing the proxy's `Saved/Logs` directory |

These variables apply to both the Python and rebuilt native proxies. Limits are per process, not a cluster scheduler. Extra workers do not make game-thread actions execute simultaneously. A native proxy build must be rebuilt to include its source changes; a downloaded older executable does not acquire new behavior by updating Python files. Use `Scripts/build_proxy.ps1` to build the native version. Call logs use `MonolithCalls-<pid>.jsonl` so multiple proxies do not append to one file; cached tool catalogs are scoped to endpoint and project.

Each forwarded `tools/call` gets a UUID in `X-Monolith-Request-Id`. The same value
appears as `request_uuid` in the proxy call log and `request_id` in the server's
response metadata and per-call log line. The proxy log's existing `request_id`
still holds the original JSON-RPC ID, which a client may reuse after completion.
Client names retain ASCII letters, digits, `-_.:/`; other characters become `_`,
and long names are bounded so the `/pid` suffix fits within 128 characters.

Successful MCP results include
`_meta.monolith.{request_id,server_instance,lease_owner}`. Tool failures preserve
the existing `structuredContent` error object and add `request_id` and
`server_instance` to its `data`, also mirrored in the JSON text content. Protocol
errors carry these fields in `error.data`. `server_instance` identifies the
module/editor session and is also available through `monolith_status` and
`/health`; restarting only the HTTP listener keeps it stable. Request UUIDs
provide correlation, without adding request deduplication or replay behavior.

Transport failures are MCP tool errors with matching JSON text and
`structuredContent.data` evidence. A proven failure before any request write is
`class:"not_sent", executed:false, retryable:true`. Once a write may have begun,
a timeout, reset or invalid response is
`class:"unknown_outcome", executed:"unknown", retryable:false`. Neither proxy
automatically retries. The native proxy caps connection attempts at five seconds
(or the configured timeout when shorter); normal send/receive operations use
the configured timeout. When the transport cannot prove whether a write began,
it conservatively reports `unknown_outcome`.

Both proxies log UTC timestamps with millisecond precision, elapsed
`duration_ms`, `client`, `request_uuid`, and an `outcome`: `ok` for success,
`error` for a returned error, `not_sent` for proven pre-send failure, or `unknown`
when execution cannot be established. `error_code` includes MCP tool errors as
well as protocol errors. `cancelled` is reserved for confirmed cancellation;
the current proxies ignore cancellation notifications and never infer it from
EOF or a timeout. Logs contain an argument hash, without raw arguments or lease
tokens.

Before an append would cross the configured threshold, the nonempty active log
is closed and archived as `MonolithCalls-<pid>-<UTC timestamp>-<counter>.jsonl`.
A single oversized entry stays intact. Archive failures preserve the existing
file, and logging failures do not alter tool responses. At startup, each proxy
deletes regular `MonolithCalls-*.jsonl` files older than 14 days in its log
directory. Directories, symlinks and unrelated files are preserved. Disabling
logging also disables this cleanup. Invalid size settings fall back to 16 MiB.

## Ownership and leases

The lead assigns each `.uasset`, `.umap`, shared header, Build.cs and configuration file to one owner. Use separate source worktrees or distinct files where practical. Asset ownership is a team convention in addition to the global server lease; the lease does not implement per-asset locks.

Examples show tool argument objects, not raw JSON-RPC envelopes:

```json
{"operation":"acquire","owner":"combat/blueprint-worker","ttl_seconds":120}
```

Send this to `monolith_coordination`. Retain the `_lease_token` returned on successful acquisition. `owner` is a diagnostic label, not authentication. While held, include the token in `params` for **every domain call**, including reads:

```json
{
  "action":"get_blueprint_info",
  "params":{
    "asset_path":"/Game/Combat/BP_Weapon",
    "_lease_token":"TOKEN_FROM_ACQUIRE"
  }
}
```

Send the example to `blueprint_query` only after inspecting its live schema. Wait for each dependent result. Use `monolith_discover` and `describe_query` action `action_schema` to avoid relying on static action tables.

Core management tools such as `monolith_reindex` receive `_lease_token` directly in their argument object, alongside their other arguments. Their live input schemas advertise this optional field. Domain tools receive it inside `params`, as above.

Renew before expiry and retain the lease through asynchronous compile completion, save and readback:

```json
{"operation":"renew","_lease_token":"TOKEN_FROM_ACQUIRE","ttl_seconds":120}
```

Release after verification:

Legacy HTTP JSON-RPC batches pin a validated lease through the remaining items. Every protected item still needs its own token. Send `release` after the batch response arrives; release inside a pinned batch is rejected as executing.

```json
{"operation":"release","_lease_token":"TOKEN_FROM_ACQUIRE"}
```

Inspect without a token using `{"operation":"status"}`. TTL is 10–600 seconds. Do not share the token with concurrent workers; ownership of a token permits calls but does not order that owner's requests. Choose a TTL appropriate to the operation, renew with headroom, and avoid handing ownership to another agent while an asynchronous operation is still running.

On stdin EOF, proxies drain accepted requests, then spend at most two seconds
total attempting releases for tokens they observed in successful `acquire`
responses. Merely passing or renewing a token does not transfer cleanup
ownership; an explicitly released token is removed. These final releases are
best-effort sends and do not wait for confirmation. A crash or forced process
termination may prevent cleanup, so explicit release and lease expiry remain
necessary. EOF cleanup does not cancel or roll back accepted calls.

While a lease is active, other clients' unowned calls are rejected. Discovery, status, guide, and `describe_query`'s `action_schema` are exempt from lease ownership for planning; a busy game thread can still delay them. Search across namespaces with `monolith_discover({"filter":"search term"})`. This is a cooperative guard for local clients. It is process-local, disappears on editor restart, and does not control manual editor input, shell writes or other plugins. A lease does not promise rollback, cancellation, atomic multi-call transactions or exactly-once execution. Without an active lease, legacy clients may still operate. To use older Monolith builds without coordination, designate one editor worker.

On busy responses, do independent work or use bounded backoff. Do not spin on acquire. After a timeout, disconnect, cancellation or editor restart, a submitted mutation may have completed or partially completed. Inspect asset state, compilation and logs before retrying; never blindly replay a non-idempotent call. If an outcome cannot be determined, preserve the affected paths and failure context for the lead rather than continuing dependent writes.

## Install discoverable skills

The original `Skills/<name>/<name>.md` references remain at their existing paths. Each now has a `SKILL.md` entrypoint that loads details on demand. The installer is opt-in, validates the full selection before copying, and refuses existing skill directories unless `--update` is supplied. Updates overwrite bundled filenames and preserve unrelated local files; they are not an atomic package transaction.

```powershell
python Scripts/install_skills.py --list
python Scripts/install_skills.py monolith-multi-agent unreal-blueprints unreal-build --dry-run
python Scripts/install_skills.py monolith-multi-agent unreal-blueprints unreal-build
```

Default destination is `$CODEX_HOME/skills`, or `~/.codex/skills` when unset. `--target` selects another host's skill directory. Use `--all` only when all domains are useful. Review local customizations before `--update`. Reload the host's skill discovery as needed. [AGENTS.md example](../Templates/AGENTS.md.example) supplies project-specific workflow conventions without requiring all skills to be loaded.

## Four-worker pattern

1. Lead records intended behavior, affected assets, owner and verification criteria; integrates results and owns builds.
2. Gameplay worker prepares independent C++ changes or a Blueprint edit plan.
3. Content worker prepares material/animation/VFX specifications and references.
4. Review worker inspects schemas, source, diffs and test evidence.

Grant the editor lease to one tool-enabled worker at a time. Each handoff reports changed asset paths, compile and save results, readback/runtime evidence, outstanding background operations and unresolved failures. Run representative gameplay and packaging checks after integration. A tool success or a generated scaffold does not establish production quality; see [UE 5.7 audit and release gates](AUDIT_UE57.md).

Coordination errors use `-32020` for busy/executing (`retryable:true`) and `-32021` for an invalid or stale lease (`retryable:false`). Both include `executed:false`. Optional-dependency errors retain `-32010`.

Omitting `ttl_seconds` on renewal retains the last acquired or explicitly renewed duration. When a leased action or batch finishes past its deadline, the owner gets `min(30, TTL/4)` seconds to release or renew cleanly. That grace is granted once per deadline: a second overrun inside the grace window does not extend the lease again, so keep renewing on a timer instead of relying on it. Idle leases still expire at their deadline.
