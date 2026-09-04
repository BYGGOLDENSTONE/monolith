---
name: material-reference
description: Version-aware PBR and material performance validation references for Unreal Engine.
---

# Material reference

Choose material inputs from measured or suitable artistic references and verify their appearance under representative lighting. Unreal's physically based material model exposes base color, roughness, metallic and specular inputs; use [Epic's UE 5.7 material guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/physically-based-materials-in-unreal-engine?application_version=5.7) for their meaning and measured examples.

There is no universal shader-instruction, arithmetic-cycle or GPU-millisecond budget for every material. Record target hardware, shader platform, resolution, blend mode and workload. Inspect compiled shader statistics and compare representative frame captures; graph expression count is not an instruction count.

For custom HLSL, verify helpers, precision types, derivative support and shader pass constraints against the installed engine source and target compiler. Compile on each shipping RHI. Do not infer portability from a single editor preview.

For Monolith work, discover `material` and describe the selected action before using it. Use live compiled stats where available, then evaluate overdraw, scene usage and runtime cost. The repository does not include the older `Docs/references/materials/` library; this guide deliberately uses available references.
