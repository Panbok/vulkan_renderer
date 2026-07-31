---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
## Shader Loader — Design

### Files
- `lib/src/renderer/resources/loaders/shader_loader.h`
- `lib/src/renderer/resources/loaders/shader_loader.c`

### Responsibilities
- Read `.shadercfg` text files.
- Parse keys in any order; ignore unknown keys with a warning.
- Build `shader_config` with:
  - `name`, `renderpass_name`
  - `use_instances`, `use_local`
  - Darray `attributes` (name, type, size)
  - Darray `uniforms` (name, type, scope, size, location)
  - `stage_count`, `stages[]`, `stage_names[]`, `stage_filenames[]`
- Compute offsets, total sizes, and strides per scope:
  - Global UBO size/stride
  - Instance UBO size/stride
  - Push constants size/stride
- Build name→index hashtable for uniforms.

### Parsing rules
- `key=value` with optional spaces.
- `attribute=type,name`
- `uniform=type,scope,name` with `scope` in `{0,1,2}`.
- `stages` comma-separated → map to enum bits and ordered array.
- `stagefiles` either single path (single-file) or N paths aligned to `stages`.

### Type mapping
- Attribute types: `vec2,vec3,vec4,mat4,int8,uint8,int16,uint16,int32,uint32` → sizes consistent with backend vertex formats.
- Uniform types: `float,vec2,vec3,vec4,int8,uint8,int16,uint16,int32,uint32,mat4,samp` → sizes for UBO computation; `samp` size=0 and increments sampler slot.

### Size, offset, stride computation

#### Helper Functions
```c
// Align value to next multiple of alignment
static inline u64 align_up(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Get std140 alignment for a uniform type
static inline u64 get_std140_alignment(shader_uniform_type type) {
    switch(type) {
        case SHADER_UNIFORM_TYPE_FLOAT32:   return 4;
        case SHADER_UNIFORM_TYPE_INT32:     return 4;
        case SHADER_UNIFORM_TYPE_UINT32:    return 4;
        case SHADER_UNIFORM_TYPE_FLOAT32_2: return 8;
        case SHADER_UNIFORM_TYPE_FLOAT32_3: return 16;  // Note: wastes 4 bytes!
        case SHADER_UNIFORM_TYPE_FLOAT32_4: return 16;
        case SHADER_UNIFORM_TYPE_MATRIX_4:  return 16;  // Treated as 4 vec4s
        case SHADER_UNIFORM_TYPE_SAMPLER:   return 0;   // Not in UBO
        default: return 4;
    }
}

// Get size of a uniform type
static inline u64 get_uniform_size(shader_uniform_type type) {
    switch(type) {
        case SHADER_UNIFORM_TYPE_FLOAT32:   return 4;
        case SHADER_UNIFORM_TYPE_FLOAT32_2: return 8;
        case SHADER_UNIFORM_TYPE_FLOAT32_3: return 12;
        case SHADER_UNIFORM_TYPE_FLOAT32_4: return 16;
        case SHADER_UNIFORM_TYPE_MATRIX_4:  return 64;
        case SHADER_UNIFORM_TYPE_SAMPLER:   return 0;
        // ... etc
        default: return 0;
    }
}
```

#### UBO Layout Computation (std140 rules)
For each scope (global, instance):
1. Initialize `current_offset = 0`, `texture_slot_count = 0`
2. For each uniform in declaration order:
   - If sampler: `uniform.location = texture_slot_count++`, `uniform.offset = 0`, `uniform.size = 0`
   - Else (UBO member):
     - `alignment = get_std140_alignment(uniform.type)`
     - `uniform.offset = align_up(current_offset, alignment)`
     - `uniform.size = get_uniform_size(uniform.type)`
     - `current_offset = uniform.offset + uniform.size`
3. After all uniforms: `ubo_size = current_offset`
4. Query device `minUniformBufferOffsetAlignment` (store as `required_ubo_alignment`, typically 256)
5. Compute `ubo_stride = align_up(ubo_size, required_ubo_alignment)`

Example (global scope):
```
uniform=mat4,0,projection:
  offset = align_up(0, 16) = 0
  size = 64
  current_offset = 64

uniform=mat4,0,view:
  offset = align_up(64, 16) = 64
  size = 64
  current_offset = 128

→ global_ubo_size = 128
→ global_ubo_stride = align_up(128, 256) = 256
```

#### Push Constants
- No std140 alignment needed (memcpy'd directly)
- Align to 4 bytes as required by Vulkan spec
- Track per-stage ranges if needed (future)

```c
push_constant_size = sum of all local uniform sizes
push_constant_stride = align_up(push_constant_size, 4)

// Validate against device limit
if (push_constant_size > device_max_push_constants_size) {
    log_error("Push constant size %u exceeds device limit %u",
              push_constant_size, device_max_push_constants_size);
    return false;
}
```

### Validation

#### Config Parse Validation
```c
typedef struct shader_config_parse_result {
    bool is_valid;
    shader_config config;  // Populated if valid
    const char *error_message;  // Human-readable error
    u32 line_number;  // Line where error occurred (0 if N/A)
} shader_config_parse_result;
```

**Common Parse Errors**:
- `"stages count (2) != stagefiles count (1)"`
- `"Unknown uniform type 'vec5' at line 23"`
- `"Unknown attribute type 'int64' at line 15"`
- `"Duplicate attribute name 'in_position' at line 18"`
- `"Duplicate uniform name 'projection' at line 25"`
- `"Invalid scope value '3' (must be 0, 1, or 2) at line 27"`
- `"Empty shader name"`
- `"Empty renderpass name"`

#### Resource Limit Validation
After parsing, before returning config:
```c
// Uniform count
if (global_uniform_count > max_uniform_count) {
    return error("Global uniform count %u exceeds limit %u", ...);
}
if (instance_uniform_count > max_uniform_count) {
    return error("Instance uniform count %u exceeds limit %u", ...);
}

// Texture counts
if (global_texture_count > max_global_textures) {
    return error("Global texture count %u exceeds limit %u", ...);
}
if (instance_texture_count > max_instance_textures) {
    return error("Instance texture count %u exceeds limit %u", ...);
}

// UBO size
if (global_ubo_size > device_max_ubo_size) {
    return error("Global UBO size %llu exceeds device limit %llu", ...);
}
if (instance_ubo_size > device_max_ubo_size) {
    return error("Instance UBO size %llu exceeds device limit %llu", ...);
}

// Push constants
if (push_constant_size > device_max_push_constants_size) {
    return error("Push constant size %llu exceeds device limit %u", ...);
}

// Stage count
if (stage_count == 0) {
    return error("No shader stages defined");
}
if (stage_count > VKR_SHADER_STAGE_COUNT) {
    return error("Too many shader stages (%u, max %u)", ...);
}
```

#### Renderpass Resolution
- Shader config stores `renderpass_name` as string during load
- At shader **initialization** (not load), resolve to ID:
  ```c
  u32 renderpass_id = renderpass_system_get_id(shader->renderpass_name);
  if (renderpass_id == INVALID_ID) {
      log_warn("Renderpass '%s' not found, using default for domain",
               shader->renderpass_name);
      renderpass_id = get_default_renderpass_for_domain(shader->domain);
  }
  shader->renderpass_id = renderpass_id;
  ```

### Resource integration
- Expose loader via resource system to allow `shader` resources alongside `material`.
- Optionally support hot-reload (invalidate pipeline and recreate on next use).

### Outputs to backend
- Build a `VkrShaderObjectDescription`:
  - File format: SPIR-V
  - File type: `single` or `multi`
  - Modules: per-stage `path` and `entry_point` (`vertexMain`, `fragmentMain`)
- Provide sizes/strides for globals/instances/push constants to the backend initialization path.


