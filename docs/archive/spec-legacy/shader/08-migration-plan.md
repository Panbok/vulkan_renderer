---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
## Migration Plan — Phased Rollout

### M1 — Config, Loader & Pipeline Integration (COMBINED M1+M2)
**Goal**: End-to-end pipeline using config files, but with hardcoded UBO sizes for safety.

**Tasks**:
1. Implement `.shadercfg` parser:
   - `shader_loader.c/h` with `shader_loader_load(path, *out_config)`
   - Parse all keys (version, name, renderpass, stages, attributes, uniforms)
   - Compute offsets/sizes/strides (but don't use them yet)
   - Validate and return `shader_config_parse_result`

2. Integrate with pipeline registry:
   - `vkr_pipeline_registry_create_from_shader_config(registry, config, *out_handle)`
   - Build `VkrShaderObjectDescription` from `config.stages` and `config.stage_filenames`
   - Remove hardcoded path `assets/deafult.world.spv` (note the typo)
   - Still use hardcoded vertex layouts from geometry system
   - Fallback: if shader config missing/unloadable for requested material/domain, bind default shader for the domain and log a warning

3. Backend uses config for stage files only:
   - `vulkan_shader_object_create` reads `desc->modules[i].path` from config
   - Still allocates `sizeof(VkrGlobalUniformObject)` and `sizeof(VkrInstanceUniformObject)` (hardcoded)
   - Descriptor layout unchanged

4. Create example `.shadercfg` files:
   - `assets/shaders/default.world.shadercfg`
   - `assets/shaders/default.ui.shadercfg`

**Testing**:
- Load shader from config → create pipeline → render frame
- Compare output to hardcoded version (should be identical)
- Parse error handling (invalid config files)
 - Missing shader config fallback path binds domain default and logs warning

**Deliverable**: Config-driven shader loading with zero visual changes.
**Status**: ✅ Completed (2025-10-18)

**Estimated Time**: 2-3 weeks

### M2 — Backend uses config-driven sizes/strides (formerly M3)
**Goal**: Replace hardcoded struct sizes with computed values from config.

**Tasks**:
1. Update `vulkan_shader_object_create`:
   - Replace `sizeof(VkrGlobalUniformObject)` with `shader->global_ubo_size`
   - Replace `sizeof(VkrInstanceUniformObject)` with `shader->instance_ubo_size`
   - Use `shader->global_ubo_stride` and `shader->instance_ubo_stride` for buffer offsets

2. Update uniform buffer uploads:
   - `vulkan_shader_update_global_state` uses `shader->global_ubo_size` for buffer write
   - `vulkan_shader_update_instance_state` uses `shader->instance_ubo_size * instance_id` for offset

3. Push constants:
   - Use `shader->push_constant_size` instead of `sizeof(Mat4)`
   - Create pipeline layout with range size from config

4. Validation:
   - Assert that config-computed sizes match current hardcoded sizes for default shaders
   - Add runtime check: `assert(global_ubo_stride >= global_ubo_size)`

5. Descriptor buffer model decision:
   - Decide between dynamic offsets vs per-instance suballocation and implement the chosen approach consistently across global/instance updates
   - Ensure per-frame descriptor sets and multi-buffering are used to avoid hazards (double/triple buffering as configured)

**Testing**:
- Render with config-driven sizes
- Verify UBO uploads are correct (GPU debugging with RenderDoc)
- Test shader with different UBO sizes (add extra uniforms to config)
 - Verify dynamic offset/suballoc path works across frames without hazards

**Deliverable**: Backend fully uses config-computed sizes, enabling arbitrary uniform layouts.
**Status**: ✅ Completed (2025-10-18)

**Estimated Time**: 1 week

### M2.5 — Front-end Shader System API (NEW)
**Goal**: Implement the public shader system front-end API to manage shader creation, lookup, usage, and uniform/sampler application.

**Tasks**:
1. Define public API (see 03-frontend-api.md):
   - `shader_system_initialize`, `shader_system_shutdown`
   - `shader_system_create`, `shader_system_get/get_by_id/get_id`
   - `shader_system_use(_by_id)`; `shader_system_bind_instance`
   - Uniform/sampler setters by name and by index
   - `shader_acquire_instance_resources`, `shader_release_instance_resources`
   - Two-phase init pattern: implement `shader_system_initialize(uint64_t *memory_requirement, void *memory, config)`
   - Provide `SHADER_SYSTEM_CONFIG_DEFAULT` macro and adopt sane defaults
2. Implement internal `shader` object:
   - Store `name`, `renderpass_name`, resolved `renderpass_id`
   - Uniform/attribute metadata arrays and name→index hashtable
   - Computed sizes/strides and required device alignments
   - Texture slot counts per scope
3. Integrate with loader output:
   - Accept `shader_config` from loader and populate front-end `shader`
   - Validate limits and compute/verify sizes/strides
4. Hook to backend:
   - Create/destroy backend shader object on initialize/shutdown of each `shader`
   - Call backend apply functions from `shader_system_apply_global/instance`
5. Error handling & logging:
   - Graceful failures with context-rich messages
   - Return codes per API spec; no crashes on invalid names
6. Concurrency constraints:
   - Document single-threaded creation/destruction and render-thread requirement for per-frame updates; add assertions where helpful

**Testing**:
- Create a shader from config and exercise the full API in a test scene
- Verify name and index-based uniform/sampler paths
 - Validate two-phase init and defaults path

**Deliverable**: Usable front-end API that drives backend without changing visuals.
**Status**: ✅ Core API integrated (2025-10-19)
**Estimated Time**: 1 week

### M3 — Attributes from config (formerly M4, OPTIONAL)
**Goal**: Vertex layouts defined by shader config instead of geometry system.

**Tasks**:
1. Add config flag: `shader_config.derive_vertex_layout_from_config` (default: false)

2. If enabled, generate `VkrVertexInputAttributeDescription` from `config.attributes`:
```c
for (u32 i = 0; i < config.attribute_count; i++) {
  attributes[i] = (VkrVertexInputAttributeDescription){
    .location = i,
    .binding = 0,  // Single interleaved buffer
    .format = map_attribute_type_to_vk_format(config.attributes[i].type),
    .offset = config.attributes[i].offset
  };
}

bindings[0] = (VkrVertexInputBindingDescription){
  .binding = 0,
  .stride = shader->attribute_stride,
  .input_rate = VKR_VERTEX_INPUT_RATE_VERTEX
};
```

3. Otherwise (default), use geometry system's vertex layout as before

4. Update geometry system to check compatibility:
   - Warn if geometry vertex layout doesn't match shader attributes
   - Allow subset (shader uses fewer attributes than geometry provides)
5. Test assets:
   - Create `assets/shaders/debug.solid_color.shadercfg` (single-file; no textures)

**Testing**:
- Enable flag for one shader, verify vertex data is read correctly
- Disable flag, verify fallback to geometry system
- Test mismatched layouts (should log warning)

**Deliverable**: Optional shader-driven vertex layouts with backward compatibility.
**Status**: ✅ Completed (2025-10-18)

**Estimated Time**: 2 weeks

**Risk**: Medium (vertex layout bugs can cause crashes or corruption)

### M4 — Material → Shader mapping (formerly M5)
**Goal**: Materials explicitly reference shaders, replacing implicit pipeline→shader mapping.

**Tasks**:
1. Add `shader_name` field to `VkrMaterial`:
```c
typedef struct VkrMaterial {
  // ... existing fields ...
  const char *shader_name;  // NEW: "shader.default.world"
} VkrMaterial;
```

2. Update material loader to parse `shader=` from `.mt` files:
```
shader=shader.default.world  # Explicit
# OR fallback:
pipeline=world  → default shader = "shader.default.world"
```

3. Update application rendering loop:
```c
// OLD:
pipeline_registry_bind_pipeline(registry, material->pipeline_id);

// NEW:
shader *s = shader_system_get(material->shader_name);
shader_system_use(material->shader_name);
shader_system_bind_instance(material->instance_id);
shader_system_uniform_set("diffuse_color", &material->diffuse_color);
shader_system_sampler_set("diffuse_texture", material->texture0);
shader_system_apply_instance();
```

4. Pipeline registry resolves shader→pipeline mapping:
   - `pipeline = pipeline_registry_get_for_shader(shader_name, vertex_layout)`
   - Caches pipeline per (shader, vertex_layout) pair

**Testing**:
- Render with explicit `shader=` in `.mt` file
- Render with legacy `pipeline=` field (should use default shader)
- Switch materials with different shaders (verify pipeline changes)

**Deliverable**: Decoupled materials and shaders, explicit control over shader selection.
**Status**: ✅ Completed (2025-10-18)

**Estimated Time**: 1 week

### M5 — Terminology Cleanup (formerly M6)
**Goal**: Rename "local" to "instance" in Vulkan backend for consistency.

**Tasks**:
1. Global find-replace in `lib/src/renderer/vulkan/`:
   - `VulkanShaderObjectLocalState` → `VulkanShaderObjectInstanceState`
   - `local_descriptor_set_layout` → `instance_descriptor_set_layout`
   - `local_uniform_buffer` → `instance_uniform_buffer`
   - `local_descriptor_pool` → `instance_descriptor_pool`
   - `local_states` → `instance_states`

2. Rename structs in `vkr_renderer.h`:
   - `VkrLocalUniformObject` → `VkrInstanceUniformObject`

3. Update function names:
   - `vulkan_shader_update_state` → `vulkan_shader_update_instance`
   - `shader_acquire_local_resources` → `shader_acquire_instance_resources`
   - `shader_release_local_resources` → `shader_release_instance_resources`

4. Update all comments and docs to use consistent terminology:
   - **Global** = per-frame, set 0
   - **Instance** = per-material, set 1
   - **Local** = per-object, push constants

**Testing**:
- Full rebuild and test suite
- Verify no semantic changes, only renames

**Deliverable**: Consistent terminology across codebase.

**Estimated Time**: 1 week

**Risk**: Low (mechanical refactoring)

**Status**: ✅ Completed (2025-10-19)

### M6 — Final Cleanup
**Goal**: Remove legacy code and document final system.

**Tasks**:
1. Remove dead code:
   - Hardcoded UBO size constants (if unused)
   - Old shader path defines
   - Legacy pipeline creation helpers

2. Document final system:
   - Update architecture diagrams
   - Add usage examples to README
   - Document shader config format in user guide

3. Performance profiling:
   - Measure descriptor set allocation overhead
   - Optimize uniform buffer uploads (batch writes)
   - Cache pipeline layouts per shader
4. Instrumentation & stats:
   - Add shader-system telemetry counters (global/instance applies per frame, descriptor writes avoided, instance resources acquired/released)

5. Add developer tools:
   - Shader validation tool (standalone binary)
   - Config generator from existing shaders
   - Hot reload support (optional)

**Deliverable**: Production-ready shader system with documentation.

**Estimated Time**: 1 week

**Status**: ✅ Completed (legacy paths removed; docs updated)

### M7 — Extended Features (Optional, Post-Cleanup)
**Goal**: Add optional advanced capabilities to future-proof the system.

**Tasks**:
1. Optional descriptor binding config in `.shadercfg` (keep current layout as default)
2. Clarify and optionally support combined image samplers
3. Per-stage push constant ranges (defer actual multi-stage usage if not needed)
4. Hot reload lifecycle (file watcher, validation, safe re-init)
5. Entry point overrides in `.shadercfg` (optional): allow `entry_points=vertexMain,fragmentMain`
6. Device capability matrix: collect limits across supported GPUs and validate configs against them; add fallbacks where reasonable
7. Compute shader scaffolding (optional): minimal support for `stages=compute` path
8. Extended material mapping (optional): map additional PBR fields to uniforms/samplers

**Testing**:
- Unit tests for new parsing and validation
- Integration tests for hot reload and per-stage push constants (if enabled)

**Deliverable**: Extended, optional capabilities without breaking existing shaders.

**Estimated Time**: 1-2 weeks (optional)

### Rollback strategy
- Each milestone is individually revertible.
- Maintain default shaders and legacy paths until the final cleanup.

### Notes on Architecture Coverage
- Front-end API (M2.5) implements `shader_system` lifecycle and maps `shader_config` to runtime `shader` objects.
- Backend Vulkan (M2) consumes sizes/strides and stage info from `shader_config` via the front-end.
- Pipeline registry integration (M1) bridges shader configs to `VkrShaderObjectDescription`.
- Material mapping (M4) exercises front-end API in the render loop.


