---
name: niagara-reference
description: Version-aware Niagara scalability and simulation validation references for Unreal Engine.
---

# Niagara reference

Choose CPU versus GPU simulation by required features and measured target-hardware cost. GPU simulation still has CPU system/emitter overhead. There is no universal particle-count threshold that selects the best simulation target. Consider emitter and system instance counts, pooling, culling, and Effect Type scalability together. See [Epic's UE 5.7 Niagara scalability guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/scalability-and-best-practices-for-niagara?application_version=5.7).

Validate bounds across the complete effect lifetime and expected transforms: overly small bounds can cull visible effects, while oversized bounds reduce culling usefulness. Exercise repeated spawning, burst peaks, offscreen behavior and scalability transitions. Check temporal appearance and measured cost in a representative scene.

For data interfaces, event support, GPU read/write features and HLSL, inspect the installed engine's implementation and compile on the shipping RHI. Do not use historical absolute claims about Vulkan crashes or CPU/GPU-only features as current compatibility evidence.

For Monolith work, discover `niagara` and describe the selected action. Compilation and successful API calls do not establish visual quality or performance. The repository does not include the older `Docs/references/niagara/` library; this guide deliberately uses available references.
