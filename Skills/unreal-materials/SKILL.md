---
name: unreal-materials
description: Use Monolith MCP for materials, material instances, graph expressions, textures, and custom HLSL. Applies when working in an Unreal project with Monolith available.
---

# unreal-materials

Read [the domain reference](unreal-materials.md) only for the requested operation. Its static action tables are examples; installed tools and per-action schemas are authoritative.

1. Call `monolith_status` to identify the connected project and engine. Discover `material` with `monolith_discover({"namespace":"material"})`.
2. Resolve the intended action and parameters with `describe_query({"action":"action_schema","params":{"target_namespace":"material","target_action":"ACTION"}})`. Use returned asset paths and IDs; do not invent signatures or treat a missing optional namespace as a transport failure.
3. For a shared editor, one agent owns each asset and one agent holds the global editor lease. Acquire using `monolith_coordination({"operation":"acquire","owner":"task/agent","ttl_seconds":120})`; attach the returned `_lease_token` inside `params` on every domain call. Renew before expiry, keep ownership through compile/save/readback, then release. Never share the token with concurrent workers. If coordination is unavailable, use one designated editor agent.
4. Wait for each dependent action's result. A lease prevents interleaving from other MCP clients; it does not provide rollback, cancellation, or control of manual editor input. Stop on unknown mutation outcomes and inspect current state before retrying.

Compile for the target shader platform; inspect actual previews and platform shader stats. Graph node counts are not compiled instruction counts.

If a subagent cannot see Monolith tools, that is a host tool-visibility issue. Let it research or prepare independent source changes; have the tool-enabled agent execute the editor steps. Do not claim that installing a skill grants tool access.

