---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Shader System Review & Recommendations

## Executive Summary

This document reviews the proposed shader system specification against the current implementation and the draft in `new_shader_system.txt`. Overall, the spec is **well-designed and addresses the core requirements**, but several important details need clarification or extension.

**Status**: ✅ Ready for implementation with recommended adjustments
**Risk Level**: Low - The phased migration plan minimizes disruption

---

## Current State Analysis

### What Exists Today

1. **Hardcoded Descriptor Layouts**
   - Set 0 (global): 1 UBO binding (view/projection)
   - Set 1 (local/instance): 1 UBO + 1 sampled image + 1 sampler
   - Push constants: `Mat4 model` (64 bytes)

2. **Hardcoded Uniform Structures**
   ```c
   // VkrGlobalUniformObject: 256 bytes (128 bytes padding for alignment)
   Mat4 view, projection

   // VkrLocalUniformObject: 256 bytes (252 bytes padding)
   Vec4 diffuse_color
   ```

3. **Hardcoded Shader Loading**
   - Single-file SPIR-V with `vertexMain`/`fragmentMain` entry points
   - Path hardcoded: `assets/deafult.world.spv` (note the typo)
   - Multi-file support exists but incomplete

4. **Material System**
   - Materials have: `pipeline_id`, phong properties, texture slots
   - No `shader=` field currently
   - Textures use slot-based system (`VKR_TEXTURE_SLOT_DIFFUSE`, etc.)

5. **Pipeline Registry**
   - Manages pipeline creation from `VkrGraphicsPipelineDescription`
   - Has domain-based organization (WORLD, UI, SHADOW, etc.)
   - Lacks automatic shader→pipeline mapping

---

## Specification Review: Strengths

### ✅ Excellent Design Choices

1. **Phased Migration Plan** (08-migration-plan.md)
   - M1-M6 breakdown is realistic and incremental
   - Each phase is independently testable
   - Rollback strategy exists

2. **Config Format** (02-config-format.md)
   - Simple key=value format, easy to parse
   - Supports both single-file and multi-file shaders
   - Scope encoding (0=global, 1=instance, 2=local) is clear

3. **Frontend API** (03-frontend-api.md)
   - Clean separation between system API and per-shader API
   - Follows existing patterns (`shader_system_get`, `shader_system_use`)
   - Instance resource management is well-defined

4. **Vulkan Backend Integration** (05-backend-vulkan.md)
   - Preserves existing descriptor layout initially
   - Config-driven sizes replace hardcoding
   - Clear path to dynamic layouts later

5. **Validation Plan** (09-validation.md)
   - Unit tests for parser and stride computation
   - Integration tests with actual rendering
   - Runtime assertions for safety

---

## Critical Gaps & Recommendations

### 🔴 HIGH PRIORITY

#### 1. **Descriptor Binding Flexibility** (Missing from spec)

**Problem**: The spec assumes a fixed descriptor layout:
- Set 0: binding 0 (global UBO)
- Set 1: binding 0 (instance UBO), binding 1 (image), binding 2 (sampler)

**Reality**: Different shaders may need:
- Multiple textures per instance (normal map, roughness, etc.)
- No textures at all (solid color shaders)
- Storage buffers (for compute or advanced effects)

**Recommendation**:
```
Add to 02-config-format.md:

# Descriptor bindings (optional, defaults to current layout)
# Format: binding=set,binding,type,count
# Types: ubo, image, sampler, storage, combined_image_sampler
binding=0,0,ubo,1        # Set 0, binding 0: global UBO
binding=1,0,ubo,1        # Set 1, binding 0: instance UBO
binding=1,1,image,1      # Set 1, binding 1: diffuse texture
binding=1,2,sampler,1    # Set 1, binding 2: sampler

# For now, keep this OPTIONAL and default to current layout
# Future: auto-derive from uniform declarations
```

**Migration**: M1 can skip this; add in M7 (post-cleanup)

---

#### 2. **UBO Alignment Rules** (Underspecified)

**Problem**: The spec mentions "alignment" but doesn't specify:
- GLSL std140 vs std430 layout rules
- Per-member alignment (vec3 → 16-byte aligned, mat4 → 16-byte columns)
- GPU-specific `minUniformBufferOffsetAlignment` (256 bytes on some GPUs)

**Current Code**: Hardcoded 256-byte padding
```c
typedef struct VkrGlobalUniformObject {
  Mat4 view;        // 64 bytes
  Mat4 projection;  // 64 bytes
  uint8_t padding[128];  // Total: 256 bytes
} VkrGlobalUniformObject;
```

**Recommendation**:
```
Add to 04-loader.md "Size, offset, stride computation":

### UBO Member Alignment (std140 rules)
- Scalars (float, int): 4-byte aligned
- vec2: 8-byte aligned
- vec3: 16-byte aligned (wastes 4 bytes!)
- vec4: 16-byte aligned
- mat4: treated as array of 4 vec4s (16-byte aligned per column)

Example calculation:
  uniform=mat4,0,projection  → offset=0,   size=64
  uniform=mat4,0,view        → offset=64,  size=64
  → global_ubo_size = 128
  → global_ubo_stride = align_up(128, required_ubo_alignment)
                      = align_up(128, 256) = 256

### Device-specific alignment
Query `VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment` and store as
`shader.required_ubo_alignment`. Use this for stride computation.
```

**Code Addition**: Add `align_offset` function to loader:
```c
static inline u64 align_offset(u64 offset, u64 alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
}

// When adding uniforms:
u64 member_alignment = get_std140_alignment(uniform_type);
uniform.offset = align_offset(current_offset, member_alignment);
current_offset = uniform.offset + uniform.size;
```

---

#### 3. **Sampler Uniform Handling** (Inconsistent)

**Problem**:
- `new_shader_system.txt` says samplers have `size=0` and increment a slot counter
- Spec 02-config-format.md says `samp` maps to texture slots "in declaration order"
- Current code separates image and sampler into two bindings

**Reality**: Vulkan supports:
1. **Combined image sampler** (single binding, image+sampler together)
2. **Separate image/sampler** (current approach, 2 bindings)

**Recommendation**:
```
Clarify in 02-config-format.md:

### Sampler Uniforms
- `uniform=samp,1,diffuse_texture` allocates:
  - A texture slot in `shader.instance_textures[slot_index]`
  - Two descriptor bindings (current approach):
    * binding N: SAMPLED_IMAGE
    * binding N+1: SAMPLER
- `shader_uniform.location` = texture slot index (0, 1, 2...)
- `shader_uniform.size` = 0 (not stored in UBO)

Future: Support `uniform=combined_image_sampler,1,name` for single-binding approach.
```

**Update `shader_uniform` struct**:
```c
typedef struct shader_uniform {
  u64 offset;        // UBO offset (0 for samplers)
  u16 location;      // Texture slot OR UBO binding index
  u16 index;         // Index in uniforms array
  u16 size;          // 0 for samplers
  u8 set_index;      // 0=global, 1=instance, INVALID_ID=local
  u8 descriptor_binding;  // NEW: actual descriptor binding number
  shader_scope scope;
  shader_uniform_type type;
} shader_uniform;
```

---

#### 4. **Attribute Offset Computation** (Missing)

**Problem**: Spec says attributes have offsets, but doesn't specify:
- Packed layout vs aligned layout
- Padding between attributes
- Interleaved vs separate buffers

**Current Code**: Uses hardcoded `VkrVertexInputAttributeDescription` from geometry system

**Recommendation**:
```
Add to 04-loader.md:

### Attribute Packing
Attributes are tightly packed (no padding) in the order declared:

attribute=vec3,in_position  → location=0, offset=0,  size=12
attribute=vec2,in_texcoord  → location=1, offset=12, size=8
→ attribute_stride = 20 bytes

Future: Support explicit offsets or multiple bindings for advanced layouts.
```

**Note**: This assumes single interleaved buffer (binding 0). Leave multi-buffer support for later.

---

### 🟡 MEDIUM PRIORITY

#### 5. **Renderpass Binding** (Underspecified)

**Spec says**: `renderpass=renderpass.default.world`

**Questions**:
- How are renderpasses identified? By name string or numeric ID?
- What if renderpass doesn't exist at shader load time?
- Does shader store renderpass ID or name?

**Recommendation**:
```
Add to 04-loader.md:

### Renderpass Resolution
- Shader config stores `renderpass_name` as string
- At shader initialization (not load), resolve to numeric ID via:
  `renderpass_id = renderpass_system_get_id(shader_config.renderpass_name)`
- If renderpass not found, log error and use default for domain:
  * WORLD → "renderpass.default.world"
  * UI → "renderpass.default.ui"
```

**Update `shader` struct**:
```c
typedef struct shader {
  u32 id;
  char *name;
  char *renderpass_name;  // From config
  u32 renderpass_id;       // Resolved at init
  // ...
} shader;
```

---

#### 6. **Hot Reload Support** (Future-proofing)

**Spec mentions**: "Optionally support hot-reload" in 04-loader.md

**Recommendation**: Make this **explicit** in the lifecycle:
```
Add to 01-architecture.md:

### Hot Reload (Optional, post-M6)
1. File watcher detects `.shadercfg` change
2. Reload config, validate
3. If validation passes:
   - Mark shader as `SHADER_STATE_RELOADING`
   - Wait for current frame to finish
   - Destroy old pipeline(s) using this shader
   - Recreate shader resources
   - Recreate pipelines
   - Transition to `SHADER_STATE_INITIALIZED`
4. If validation fails, keep old shader and log error

Note: Materials using the shader don't need to change (handle remains valid).
```

---

#### 7. **Error Handling in Loader** (Underspecified)

**Problem**: Config parse errors aren't detailed:
- What if `stages` count ≠ `stagefiles` count?
- What if unknown uniform type?
- What if UBO size exceeds GPU limit?

**Recommendation**:
```
Add to 04-loader.md "Validation":

### Parse Error Handling
Return `shader_config` with `is_valid=false` and populate error details:

typedef struct shader_config_error {
  bool is_valid;
  const char *error_message;  // Human-readable
  u32 line_number;            // For file errors
} shader_config_error;

Common errors:
- "stages count (2) != stagefiles count (1)"
- "Unknown uniform type 'vec5' at line 23"
- "UBO size 65536 exceeds device limit 65536"
- "Attribute 'in_position' declared twice"

Log error and return NULL from `shader_system_create`.
```

---

#### 8. **Push Constant Range Calculation** (Incomplete)

**Spec says**: `push_constant_size = sum of local uniforms`

**Problem**: Vulkan requires:
- Per-stage ranges (vertex vs fragment)
- 4-byte alignment
- Max size check (usually 128 bytes minimum, but can be 256+)

**Current Code**: Single range, vertex stage only:
```c
vkCmdPushConstants(command_buffer->handle, pipeline_layout,
                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &data->model);
```

**Recommendation**:
```
Update 02-config-format.md:

### Push Constants (Local Uniforms)
- Declare scope 2 uniforms with stage hint:
  uniform=mat4,2,model,vertex
  uniform=vec4,2,debug_color,fragment

- If no stage specified, default to VERTEX for compatibility
- Loader groups by stage and creates ranges:
  push_constant_ranges[0] = {offset=0,  size=64, stage=VERTEX}
  push_constant_ranges[1] = {offset=64, size=16, stage=FRAGMENT}

- Total push_constant_size = 80, stride = align_up(80, 4) = 80
```

**Note**: This is complex - consider deferring to M7. For M1-M6, keep single vertex-stage range.

---

### 🟢 LOW PRIORITY (Nice to Have)

#### 9. **Shader Variants** (Future Work)

Shaders often need variants (e.g., with/without normal mapping). Not in scope now, but design should allow:
```
variant=WITH_NORMAL_MAP
uniform=samp,1,normal_texture,#ifdef WITH_NORMAL_MAP
```

**Defer to post-M6.**

---

#### 10. **Reflection from SPIR-V** (Future Work)

Spec correctly lists this as non-goal. In future, could auto-generate config from SPIR-V using tools like `spirv-reflect`.

**Keep as documentation note.**

---

## Terminology Alignment

### ⚠️ Naming Inconsistency: "Local" vs "Instance"

**Problem**: The spec uses confusing terminology:

| Term       | Spec Says           | Current Code Uses   |
|------------|---------------------|---------------------|
| Global     | Set 0, per-frame    | Set 0, global       |
| Instance   | Set 1, per-material | Set 1, **local**    |
| Local      | Push constants      | Push constants      |

**Current Vulkan code**:
```c
// descriptor set 1 is called "local" in implementation:
VkDescriptorSetLayout local_descriptor_set_layout;
VkDescriptorSet local_descriptor_sets[...];
```

**But the spec says**:
> Instance → descriptor set 1 UBO and samplers (current "local").

**Recommendation**: **Unify on "Instance"** everywhere:

1. Rename in Vulkan backend:
   - `local_descriptor_set_layout` → `instance_descriptor_set_layout`
   - `local_uniform_buffer` → `instance_uniform_buffer`
   - `VkrLocalUniformObject` → `VkrInstanceUniformObject`

2. Use "Local" exclusively for push constants

3. Update docs and comments consistently

**Impact**: Medium (lots of renaming), but **critical for clarity**

---

## Additional Recommendations

### A. **Example Shader Configs in Assets**

Create actual `.shadercfg` files for testing:
```
assets/shaders/default.world.shadercfg
assets/shaders/default.ui.shadercfg
assets/shaders/debug.solid_color.shadercfg  (no textures)
```

Include comments explaining each field.

---

### B. **Shader System Config Defaults**

The spec defines `shader_system_config` but doesn't give defaults:

```c
#define SHADER_SYSTEM_CONFIG_DEFAULT { \
  .max_shader_count = 512, \
  .max_uniform_count = 32, \
  .max_global_textures = 8, \
  .max_instance_textures = 8 \
}
```

**Rationale**:
- 512 shaders is plenty for a game
- 32 uniforms covers most cases
- 8 textures per scope allows PBR (albedo, normal, metallic, roughness, AO, emissive, +2 extra)

---

### C. **Material → Shader Default Mapping**

Spec 07-material-integration.md says:
> If `shader=` is absent, use default shader for domain

Define this mapping explicitly:
```c
const char* get_default_shader_for_domain(VkrPipelineDomain domain) {
  switch(domain) {
    case VKR_PIPELINE_DOMAIN_WORLD: return "shader.default.world";
    case VKR_PIPELINE_DOMAIN_UI:    return "shader.default.ui";
    case VKR_PIPELINE_DOMAIN_SHADOW: return "shader.default.shadow";
    default: return "shader.default.world";
  }
}
```

---

### D. **Loader File Location**

Spec says: `lib/src/renderer/resources/loaders/shader_loader.c`

But also need header: `lib/src/renderer/resources/loaders/shader_loader.h`

**Public API** (exposed to resource system):
```c
bool shader_loader_supports(const char *path);
bool shader_loader_load(String8 path, shader_config *out_config);
void shader_loader_unload(shader_config *config);
```

---

## Migration Plan Adjustments

### Recommended Milestone Changes

**Current M1**: Config & Loader (no backend change)
**Issue**: Can't test without integration

**Recommendation**: Merge M1+M2:
```
M1 (Combined): Config, Loader, and Pipeline Integration
- Implement parser
- Pipeline registry uses shader_config for stage filenames
- Sizes still hardcoded, but config is loaded and validated
- Can test end-to-end: load config → create pipeline → render
```

**Current M4**: Attributes from config
**Issue**: This is complex and can break vertex layouts

**Recommendation**: Make M4 **optional**:
```
M4 (Optional): Attributes from Config
- Only enable if `config.derive_attributes_from_shader = true`
- Otherwise, use geometry system's vertex layouts
- Allows gradual migration
```

---

## Testing Strategy Enhancement

Add to 09-validation.md:

### Regression Tests
```c
// Ensure new system produces identical rendering to old hardcoded system
void test_world_shader_compatibility() {
  // 1. Render frame with old hardcoded shader
  // 2. Render frame with config-driven shader (same .shadercfg as old)
  // 3. Compare framebuffer bytes (should be identical)
}
```

### Stress Tests
```c
void test_max_shaders() {
  // Create 512 shaders (max_shader_count)
  // Ensure no allocation failures
}

void test_max_uniforms() {
  // Create shader with 32 uniforms
  // Set all uniforms, apply, render
}
```

---

## Summary of Required Changes to Spec

### Must Add
1. ✅ UBO alignment rules (std140) → **04-loader.md**
2. ✅ Sampler/descriptor binding clarification → **02-config-format.md**
3. ✅ Attribute offset computation → **04-loader.md**
4. ✅ Error handling details → **04-loader.md**
5. ✅ Renderpass resolution → **04-loader.md**, **01-architecture.md**
6. ✅ Terminology fix (Instance vs Local) → **All files**

### Should Add
7. ✅ Descriptor binding config (optional, future) → **02-config-format.md**
8. ✅ Hot reload lifecycle → **01-architecture.md**
9. ✅ Push constant range per-stage → **02-config-format.md**
10. ✅ Default config values → **03-frontend-api.md**

### Nice to Have
11. ✅ Example `.shadercfg` files → **New directory: assets/shaders/**
12. ✅ Domain→shader default mapping → **07-material-integration.md**
13. ✅ Regression tests → **09-validation.md**

---

## Final Verdict

### Overall Assessment: **8.5/10** ⭐⭐⭐⭐

**Strengths**:
- Solid architecture with clear separation of concerns
- Phased migration minimizes risk
- Config format is simple and extensible
- Aligns well with current Vulkan backend

**Weaknesses**:
- Missing low-level details (alignment, packing, error handling)
- Terminology inconsistency (Instance/Local)
- Some edge cases not covered (multi-file, no-texture shaders)

**Recommendation**: **Proceed with implementation** after addressing high-priority gaps (1-4) and terminology fix.

### Estimated Implementation Effort

| Phase | Estimated Time | Risk |
|-------|---------------|------|
| M1 (Parser + Pipeline Integration) | 2-3 weeks | Low |
| M2 (Removed - merged with M1) | - | - |
| M3 (Config-driven sizes) | 1 week | Low |
| M4 (Attributes from config) | 2 weeks | Medium |
| M5 (Material→Shader mapping) | 1 week | Low |
| M6 (Cleanup) | 1 week | Low |
| **Total** | **7-9 weeks** | **Low-Medium** |

### Next Steps

1. ✅ Update spec docs with recommendations from this review
2. Create example `.shadercfg` files
3. Implement M1 (parser + basic integration)
4. Write unit tests for parser
5. Integrate with pipeline registry
6. Proceed through M3-M6

---

## Questions for Clarification

1. **Descriptor indexing**: Will you support dynamic descriptor indexing in future (for bindless textures)?
2. **Compute shaders**: Should compute shader support be considered now, or defer?
3. **Material texture mapping**: When a material has `diffuse_color` + `base_color` texture, how do they combine? (Multiply? Override?)
4. **Shader compilation**: Will you compile from GLSL/HLSL to SPIR-V at runtime, or pre-compile?

---

**Document Version**: 1.0
**Author**: AI Code Review
**Date**: 2025-10-14

