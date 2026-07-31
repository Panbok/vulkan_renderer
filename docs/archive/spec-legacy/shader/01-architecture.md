---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
## Shader System — Architecture & Lifecycle

### High-level goals
- Config-driven definition of stages, attributes, and uniforms.
- Preserve current Vulkan descriptor layout while removing hardcoding.
- Clear separation between front-end API, resource loading, and backend binding.

### Components
- `shader_system`: manages creation/lookup/use of shaders defined by config.
- `shader`: front-end object holding sizes/strides, uniform/attribute metadata, lookups, and an opaque backend `internal_data`.
- `shader_config`: parsed representation of a `.shadercfg` file (attributes/uniforms/stages/renderpass flags).
- `shader_loader`: parses `.shadercfg`, computes offsets/strides, validates, and returns a `shader_config`.
- Vulkan backend integration: extends `vulkan_shaders.c` and `vulkan_pipeline.c` to consume sizes/strides and stage info from `shader_config`.

### Lifecycle
1) Create: allocate/initialize `shader` and assign static fields from `shader_config`.
2) Define: add attributes/uniforms from config, compute offsets, sizes, and strides using device alignment.
3) Initialize: create descriptor set layouts, descriptor pools/sets, and UBOs sized from config; create shader modules and pipeline layout.
4) Per-frame:
   - Bind globals (set 0), upload global UBO and bind descriptor set.
   - Iterate instances: bind instance id, upload instance UBO and bind descriptor set.
   - Iterate locals: push constants for each draw (model matrix).

### Scope mapping (doc ↔ current backend)
**IMPORTANT TERMINOLOGY NOTE**:
The current Vulkan backend uses "local" to refer to what this spec calls "instance". We will rename the backend implementation to match this spec's terminology for consistency.

- **Global** (Scope 0) → Descriptor set 0 UBO (view/projection)
  - Updated once per frame
  - Shared across all instances of a shader
  - Example: projection matrix, view matrix, time, camera position

- **Instance** (Scope 1) → Descriptor set 1 UBO and samplers
  - Current backend calls this "local" (will be renamed)
  - Updated per material/object instance
  - Each instance gets its own descriptor set and UBO slot
  - Example: diffuse_color, textures (one per material)

- **Local** (Scope 2) → Push constants
  - Updated per draw call (fastest)
  - No descriptor set needed
  - Example: model matrix (unique per object in the world)

**Backend Renaming Required** (M6):
```
VulkanShaderObjectLocalState        → VulkanShaderObjectInstanceState
local_descriptor_set_layout         → instance_descriptor_set_layout
local_uniform_buffer                → instance_uniform_buffer
local_descriptor_pool               → instance_descriptor_pool
VkrLocalUniformObject               → VkrInstanceUniformObject
vulkan_shader_update_state          → vulkan_shader_update_instance
shader_acquire_local_resources      → shader_acquire_instance_resources
```

### Alignment and stride
- `shader.required_ubo_alignment` populated from device limits.
- For each scope with a UBO, compute `size` (sum of added uniform sizes) and `stride` (round up to next multiple of alignment).
- Push constants are aligned to 4 bytes. Track `push_constant_size` and `push_constant_stride`.

### Name lookup and indices
- Uniforms and attributes are indexed by add order.
- Hashtable maps string name → uniform index (and sampler slot for textures).

### Instance resource management
- `shader_acquire_instance_resources(shader*, u32* out_id)` reserves a slot in the instance UBO and allocates a descriptor set if needed.
- `shader_release_instance_resources(shader*, u32 id)` frees descriptor set slot and marks UBO range reusable.

### Concurrency
- Creation/destruction happen on init/shutdown paths (single-threaded).
- Per-frame updates occur on the render thread; data upload and descriptor writes are per-frame-indexed.
- No thread synchronization needed in MVP; future multi-threaded rendering will require per-thread command buffers.

### Hot Reload (Post-M6, Optional)
Hot reload allows shaders to be updated without restarting the application:

1. **File Watcher** detects `.shadercfg` or `.spv` file change
2. **Validation Phase**:
   - Parse new config
   - Validate uniform/attribute compatibility with existing usage
   - If validation fails: log error, keep old shader, abort reload
3. **Reload Phase** (if validation passes):
   - Mark shader state as `SHADER_STATE_RELOADING`
   - Wait for in-flight frames to complete (fence wait)
   - Destroy old Vulkan resources:
     - Shader modules
     - Pipeline layouts
     - Pipelines using this shader (tracked via dependency list)
   - Create new resources from updated config
   - Transition to `SHADER_STATE_INITIALIZED`
4. **Materiał Compatibility**:
   - Materials using the shader don't need updates (handle remains valid)
   - Instance resources are preserved if compatible
   - New uniforms get default values; removed uniforms are ignored

**Limitations**:
- Incompatible changes (removing required uniforms, changing types) require app restart
- Descriptor layout changes (adding/removing bindings) require pipeline recreation
- Best for tweaking uniform values, fixing shader code, adjusting attributes

**Implementation Note**: Defer until stable; requires careful state management.

### Current backend touchpoints
- `lib/src/renderer/vulkan/vulkan_shaders.c`: descriptor set creation, buffers, updates.
- `lib/src/renderer/vulkan/vulkan_pipeline.c`: pipeline layout: set layouts and push constants.
- `lib/src/renderer/systems/vkr_pipeline_registry.c`: shader module description consumed at pipeline creation.


