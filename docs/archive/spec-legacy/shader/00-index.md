---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
## Shader System Specs — Index & Glossary

### Contents
- **01-architecture.md** — High-level design and lifecycle
- **02-config-format.md** — `.shadercfg` schema and examples
- **03-frontend-api.md** — Public API surface and data structures
- **04-loader.md** — Resource loader design and offset/stride rules
- **05-backend-vulkan.md** — Vulkan mapping and required changes
- **06-pipeline-registry-integration.md** — Pipeline bridge
- **07-material-integration.md** — Material ↔ Shader mapping
- **08-migration-plan.md** — Incremental rollout (updated: M1+M2 merged)
- **09-validation.md** — Testing & runtime validation (updated: regression tests)
- **10-review-and-recommendations.md** — ⭐ Comprehensive review and required changes
- **11-implementation-checklist.md** — Detailed implementation tracking

### Goals
- Config-driven shaders: minimal hardcoding in C, defined by config files.
- Extensible & maintainable: adding new uniforms/attributes without code churn.
- Align with current Vulkan backend: descriptor sets and push constants remain intact initially.

### Non-goals
- Full reflection from SPIR-V (future work).
- Dynamic descriptor indexing or bindless (future work).
- Breaking existing materials/UI/world rendering.

### Constraints & current state mapping
- Descriptor sets (current):
  - Set 0 (global): UBO with view/projection.
  - Set 1 (instance, currently called "local" in code): UBO + sampled image + sampler.
  - Push constants: model matrix (vertex stage) - these are the true "local" uniforms.
- **Terminology Note**: Backend will be renamed to match spec (see 01-architecture.md)
- Packaging: single-file SPIR-V with `vertexMain` and `fragmentMain` entry points already supported; multi-file support to be added.

### Glossary
- **Global** (Scope 0): Per-frame data (e.g., projection/view) → descriptor set 0 UBO. Updated once per frame, shared across all instances.
- **Instance** (Scope 1): Per-material/per-instance data (e.g., diffuse color/texture) → descriptor set 1 UBO and samplers. Each material gets its own descriptor set.
- **Local** (Scope 2): Per-draw/per-object data (e.g., model matrix) → push constants. Fastest, no descriptor set needed.
- **UBO stride**: Total bytes between consecutive UBO entries, aligned to `required_ubo_alignment`.
- **Required alignment**: Device `minUniformBufferOffsetAlignment` (typically 256 bytes); exposed as `shader.required_ubo_alignment`.
- **std140 layout**: GLSL UBO packing rules with specific alignment requirements (vec3 → 16-byte aligned, wastes 4 bytes).
- **Shader config**: A `.shadercfg` text file defining shader stages, attributes, and uniforms.

### Example Shader Configs
See `/assets/shaders/` for complete examples:
- `default.world.shadercfg` — 3D world rendering with diffuse texture
- `default.ui.shadercfg` — 2D UI rendering with orthographic projection
- `debug.solid_color.shadercfg` — No-texture debug shader


