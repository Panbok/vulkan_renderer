---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
## Pipeline Registry — Integration Plan

### Current behavior (summary)
- Builds `VkrShaderObjectDescription` with single-file SPIR-V and two entry points.
- Uses a hardcoded path for world domain (`assets/deafult.world.spv`).
- Vertex input descriptions are produced from a `VkrGeometryVertexLayoutType`.

### Objectives
- Source stage file paths from `shader_config` instead of hardcoded defaults.
- Allow both single-file (multi-entry) and per-stage files.
- Map material pipeline/domain → shader selection via config (name/id).

### Changes
1) Resolve shader by material
   - If material specifies `shader=name`, resolve that; else map `pipeline` to a default shader for the domain.

2) Build shader description from config
   - Convert `shader_config.stages` and `stage_filenames` to `VkrShaderObjectDescription` modules.
   - Set entry points to `vertexMain`/`fragmentMain` unless overridden by config.

3) Vertex input
   - Derive attribute descriptions from `shader_config.attributes` (location, format, stride) or keep current geometry layout until M4.

4) Remove hardcoded path
   - Replace `assets/deafult.world.spv` with a lookup from config or a domain default table.

### Error handling
- If a shader config is missing for a requested material/domain, fall back to a built-in default shader and log a warning.
- Validate stage counts and entry point presence.


