---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
## Vulkan Backend — Mapping & Changes

### Current descriptor and pipeline layout (kept initially)
- Descriptor set 0 (global): binding 0 = uniform buffer (view, projection).
- Descriptor set 1 (instance):
  - binding 0 = uniform buffer (per-instance/material)
  - binding 1 = sampled image (slot 0)
  - binding 2 = sampler (slot 0)
- Push constants: model matrix (vertex stage).

### Objectives
- Replace hardcoded UBO struct sizes with config-driven sizes/strides.
- Build shader modules and pipeline stages from `shader_config` (single-file or multi-file).
- Keep entry-points `vertexMain` and `fragmentMain` unless specified.

### Data owned by backend shader object
- Descriptor set layouts and pools (global, instance).
- Per-frame descriptor sets (global) and per-instance descriptor sets.
- Global/instance uniform buffers sized and strided from config.
- Shader modules per stage.
- Pipeline layout with 2 descriptor sets and 1 push constant range (conditional on use flags).

### Required changes
1) Shader modules
   - Accept `VkrShaderObjectDescription` with modules per stage.
   - Support single-file SPIR-V with different `pName` entry points.

2) Uniform buffers and descriptor sets
   - Global UBO: size = `shader.global_ubo_size`, stride = `shader.global_ubo_stride`.
   - Instance UBO: size = `shader.ubo_size`, stride = `shader.ubo_stride`.
   - Allocate buffers with these sizes and per-frame/per-instance counts.
   - Use dynamic offsets if adopting a single large buffer; else per-instance sub-alloc offsets.

3) Pipeline layout
   - Set layouts count: 2 when `use_instances` is enabled; 1 otherwise.
   - Push constant range present only when `use_local` is true, size = `push_constant_size`.

4) Update paths
   - Global apply: bind set 0 for current frame, upload global UBO (projection/view) via buffer write, then `vkUpdateDescriptorSets` if needed.
   - Instance apply: bind instance set 1 for current frame/instance id, upload instance UBO (e.g., diffuse_color), bind sampled image/sampler if enabled.
   - Local apply: `vkCmdPushConstants` with model matrix.

5) Attribute layout
   - Vertex input attributes and bindings are still supplied by pipeline registry; later derive from `shader_config.attributes` per 06-spec.

### Compatibility notes
- Continue writing `vertexMain`/`fragmentMain` entry points for single-file SPIR-V.
- Maintain existing descriptor bindings to avoid shader churn.
- Leave room for future descriptor array expansion for multiple textures.


