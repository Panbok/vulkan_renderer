---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Shader System Specification

## Overview

This directory contains the complete specification for the **config-driven shader system** for the Vulkan renderer. The system replaces hardcoded shader definitions with flexible, text-based configuration files.

**Status**: ✅ M5 Complete — Terminology cleanup (local → instance)
**Reviewed**: 2025-10-18
**Rating**: 8.5/10

---

## Quick Start

1. **Start with the Review**: Read `10-review-and-recommendations.md` for a comprehensive analysis
2. **Understand the Architecture**: Read `01-architecture.md` for high-level design
3. **Review Examples**: Check `/assets/shaders/*.shadercfg` for config file examples
4. **Follow Migration Plan**: Implement in phases per `08-migration-plan.md`
5. **Track Progress**: Use `11-implementation-checklist.md` to track completion

---

## Document Index

| File | Purpose | Status |
|------|---------|--------|
| `00-index.md` | Table of contents and glossary | ✅ Updated |
| `01-architecture.md` | System design, lifecycle, scope mapping | ✅ Updated |
| `02-config-format.md` | `.shadercfg` file format and examples | ✅ Updated |
| `03-frontend-api.md` | Public shader system API | ✅ Updated |
| `04-loader.md` | Config parser and layout computation | ✅ Updated |
| `05-backend-vulkan.md` | Vulkan backend integration | ✅ Updated |
| `06-pipeline-registry-integration.md` | Pipeline system bridge | ✅ Updated |
| `07-material-integration.md` | Material ↔ shader mapping | ✅ Updated |
| `08-migration-plan.md` | Phased rollout (M1-M6) | ✅ Updated |
| `09-validation.md` | Testing strategy and validation | ✅ Updated |
| `10-review-and-recommendations.md` | **⭐ Comprehensive review** | ✅ Complete |
| `11-implementation-checklist.md` | Detailed task tracking | ✅ Updated |

---

## Key Design Decisions

### ✅ Strengths
- **Config-driven**: Shaders defined by `.shadercfg` files, minimal C code changes
- **Phased migration**: 6 incremental milestones (M1-M6), low risk
- **Backward compatible**: Existing materials/pipelines continue working
- **std140 layout**: Explicit UBO packing rules documented
- **Extensible**: Easy to add new uniforms, attributes, textures

### ⚠️ Important Notes
- **Terminology change**: Backend "local" → "instance" (M5 renames everything)
- **Alignment critical**: std140 rules must be followed (vec3 wastes 4 bytes!)
- **Testing required**: Visual regression tests ensure no rendering changes
- **Renderpass resolution**: Happens at shader init, not load

---

## Critical Changes from Original Draft

The spec has been **significantly improved** based on the review:

1. **Added std140 alignment details** (02-config-format.md, 04-loader.md)
   - Explicit rules for vec3, vec4, mat4 packing
   - Helper functions for offset computation
   - Examples with calculated layouts

2. **Clarified sampler handling** (02-config-format.md)
   - Samplers don't occupy UBO space
   - Descriptor bindings explained (image + sampler separate)
   - Texture slot indexing documented

3. **Error handling** (04-loader.md, 03-frontend-api.md)
   - `shader_config_parse_result` with error messages
   - Validation of limits (uniform count, UBO size, etc.)
   - Example error messages

4. **Renderpass resolution** (04-loader.md)
   - Clarified load vs init timing
   - Default renderpass fallback per domain

5. **Hot reload lifecycle** (01-architecture.md)
   - Full reload workflow documented
   - Compatibility checks defined
   - Marked as post-M6 optional

6. **Terminology cleanup** (01-architecture.md, 08-migration-plan.md)
   - Renamed "local" → "instance" in backend (M5)
   - Clear scope definitions: Global/Instance/Local
   - Renaming checklist in M5

7. **Merged M1+M2** (08-migration-plan.md)
   - Combined config loader + pipeline integration
   - End-to-end testing in M1
   - Reduced milestones from 7 to 6

8. **Regression tests** (09-validation.md)
   - Framebuffer comparison tests
   - Stress tests for max uniforms/textures/shaders
   - UBO alignment edge case tests

---

## Implementation Timeline

| Milestone | Duration | Risk | Deliverable |
|-----------|----------|------|-------------|
| **M1**: Config + Loader + Pipeline | 2-3 weeks | Low | Config-driven loading works |
| **M2**: Config-driven sizes | 1 week | Low | Dynamic UBO sizes |
| **M3**: Attributes from config | 2 weeks | Medium | Optional vertex layouts |
| **M4**: Material → Shader mapping | 1 week | Low | Decoupled materials |
| **M5**: Terminology cleanup | 1 week | Low | Consistent naming |
| **M6**: Final cleanup | 1 week | Low | Production-ready |
| **Total** | **8-10 weeks** | **Low-Medium** | Shipped! |

---

## Review Sign-off (M4)

**Scope delivered:**
- Materials explicitly reference shaders via `shader_name` field
- Material loader parses `shader=` key from `.mt` files with `pipeline=` fallback
- Pipeline registry resolves shader→pipeline mapping with caching
- Application rendering loop uses material's shader name for pipeline resolution
- Added duplicate creation guards to prevent extra pipeline allocations

**Conventions:** All shader loader types use Vkr-prefixed PascalCase; functions use `vkr_`.

**Parity:** Visual output matches pre-migration (manual check). Materials now explicitly control shader selection.

**Known deferrals:** Automated tests and framebuffer regression; broader device capability matrix.

**Next milestone:** M5 — terminology cleanup (local → instance).

---

## Review Sign-off (M3)

**Scope delivered:**
- Config flag to derive vertex layout from config
- Registry builds vertex input from config locations while matching geometry stride/offsets
- Added compatibility warnings for count/stride mismatches
- Added integer vertex formats mapping

**Conventions:** Loader structs/enums use Vkr-prefixed PascalCase; functions use `vkr_`.

**Parity:** Visual output matches pre-migration (manual check). Tests are deferred by agreement.

**Known deferrals:** Automated tests and framebuffer regression; broader device capability matrix.

**Next milestone:** M4 — material → shader mapping.

---

## Example Usage

### Define a Shader Config
```ini
# assets/shaders/my_shader.shadercfg
version=1.0
name=shader.my_shader
renderpass=renderpass.default.world
stages=vertex,fragment
stagefiles=assets/shaders/my_shader.vert.spv,assets/shaders/my_shader.frag.spv
use_instance=1
use_local=1

attribute=vec3,in_position
attribute=vec2,in_texcoord

uniform=mat4,0,projection
uniform=mat4,0,view
uniform=vec4,1,diffuse_color
uniform=samp,1,diffuse_texture
uniform=mat4,2,model
```

### Load and Use (M1 onwards)
```c
// Load shader from config
shader_config_parse_result result = shader_loader_parse(
    string8_from_cstr("assets/shaders/my_shader.shadercfg"),
    temp_arena
);

if (result.is_valid) {
    shader_system_create(&result.config);
}

// Render with shader (M4 onwards)
shader_system_use("shader.my_shader");

// Global uniforms (once per frame)
shader_system_uniform_set("projection", &projection_matrix);
shader_system_uniform_set("view", &view_matrix);
shader_system_apply_global();

// Instance uniforms (per material)
u32 instance_id;
shader_acquire_instance_resources(shader, &instance_id);
shader_system_bind_instance(instance_id);
shader_system_uniform_set("diffuse_color", &color);
shader_system_sampler_set("diffuse_texture", texture);
shader_system_apply_instance();

// Local uniforms (per object, via push constants)
vkCmdPushConstants(cmd_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                   0, sizeof(Mat4), &model_matrix);
```

---

## Questions & Support

For questions about the spec, see:
- **Design questions**: Read `10-review-and-recommendations.md` § "Questions for Clarification"
- **Implementation details**: Check `04-loader.md` and `05-backend-vulkan.md`
- **Migration plan**: See `08-migration-plan.md` for phase-by-phase breakdown
- **Testing**: See `09-validation.md` for test strategies

---

## Next Steps

1. ✅ **M1 complete** — Config loader + pipeline integration shipped
2. ✅ **M2 complete** — Backend uses config-driven sizes/strides
3. ✅ **M3 complete** — Optional vertex layouts from config
4. ✅ **M4 complete** — Material-to-shader mapping implemented
5. ✅ **M5 complete** — Terminology cleanup across codebase

---

**Last Updated**: 2025-10-18
**Review Author**: AI Code Review
**Status**: M4 Shipped

