---
name: monolith-multi-agent
description: Coordinate multiple AI agents using one Unreal Editor through Monolith MCP, including tool visibility diagnosis, editor leases, asset ownership, and verified handoffs.
---

# Monolith multi-agent coordination

Separate host tool visibility from server concurrency. Each worker must have callable Monolith tools to use the editor directly; a prompt, copied skill, or parallel proxy does not grant them. If a worker lacks tools, assign offline research or independent source files and route its editor work through the tool-enabled lead.

Establish the connected project and engine with `monolith_status`. Discover only needed namespaces; use `describe_query` action `action_schema` with `target_namespace` and `target_action` for exact parameters. Do not load every domain reference.

Give each asset, shared header, Build.cs, and project configuration file one owner. Independent research and source edits can run concurrently. One editor workflow runs at a time, including reads that must observe a consistent state:

1. Acquire `monolith_coordination({"operation":"acquire","owner":"task/agent","ttl_seconds":120})` and keep its returned `_lease_token` private to the executing worker.
2. Add `params._lease_token` to every domain call, including reads. Execute dependent changes sequentially.
3. Renew using `monolith_coordination({"operation":"renew","_lease_token":"TOKEN","ttl_seconds":120})` before expiry. Preserve the lease through compilation, saving and readback; monitor long asynchronous operations before releasing.
4. Release using `monolith_coordination({"operation":"release","_lease_token":"TOKEN"})` after verification. Handoff records include changed paths, compile/save results, remaining asynchronous work, and unresolved failures.

Status uses `monolith_coordination({"operation":"status"})`. A busy lease is a scheduling signal: continue independent work or use a bounded wait, not a tight retry loop. Never reuse an expired token or share one across concurrent workers. If this core tool is absent, use a single designated editor agent until the plugin is updated.

The lease is cooperative, global and process-local. It does not lock files against external tools or manual editor input. It does not imply transaction rollback, idempotency, cancellation, or exactly-once execution. A transport timeout or disconnect after submission has an unknown outcome: inspect editor state and logs before deciding whether a mutation may be repeated. Separate asset ownership still matters after lease expiry or editor restart.

The proxy accepts concurrent requests with distinct IDs, but Unreal UObject work stays on the game thread. Parallel planning improves throughput; simultaneous binary asset editing does not. Tool-level success can still contain partial or stub domain results. Verify the changed behavior in the editor and representative gameplay before calling the handoff complete.

Coordination errors use `-32020` for busy/executing (`retryable:true`) and `-32021` for an invalid or stale lease (`retryable:false`). Both include `executed:false`. Optional-dependency errors retain `-32010`.
