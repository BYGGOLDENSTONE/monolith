---
name: material-reference
description: Consult version-aware Unreal reference guidance for material/PBR validation and measured shader cost. Use for technical validation, not as a Monolith action catalog.
---

# material-reference

Read [the focused reference](material-reference.md). Verify engine-dependent details against the installed UE source or the linked official documentation, and measure performance on the target hardware. Project-specific budgets are not engine limits.

For editor mutations, discover the live domain schema first. When multiple agents share an editor, use one lease holder and attach its `_lease_token` inside each domain call's `params`; renew through compile/save/readback. If coordination is unavailable, use one designated editor agent. A failed transport does not prove a mutation was rolled back.

