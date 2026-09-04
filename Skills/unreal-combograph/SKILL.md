---
name: unreal-combograph
description: Use Monolith MCP for ComboGraph attack graphs and related ability scaffolds. Applies when working in an Unreal project with Monolith available.
---

# unreal-combograph

Read [the domain reference](unreal-combograph.md) only for the requested operation. Its static action tables are examples; installed tools and per-action schemas are authoritative.

1. Call `monolith_status` to identify the connected project and engine. Discover `combograph` with `monolith_discover({"namespace":"combograph"})`.
2. Resolve the intended action and parameters with `describe_query({"action":"action_schema","params":{"target_namespace":"combograph","target_action":"ACTION"}})`. Use returned asset paths and IDs; do not invent signatures or treat a missing optional namespace as a transport failure.
3. For a shared editor, one agent owns each asset and one agent holds the global editor lease. Acquire using `monolith_coordination({"operation":"acquire","owner":"task/agent","ttl_seconds":120})`; attach the returned `_lease_token` inside `params` on every domain call. Renew before expiry, keep ownership through compile/save/readback, then release. Never share the token with concurrent workers. If coordination is unavailable, use one designated editor agent.
4. Wait for each dependent action's result. A lease prevents interleaving from other MCP clients; it does not provide rollback, cancellation, or control of manual editor input. Stop on unknown mutation outcomes and inspect current state before retrying.

Check the optional ComboGraph plugin is available; validate connections and test actual combo timing in PIE.

If a subagent cannot see Monolith tools, that is a host tool-visibility issue. Let it research or prepare independent source changes; have the tool-enabled agent execute the editor steps. Do not claim that installing a skill grants tool access.

