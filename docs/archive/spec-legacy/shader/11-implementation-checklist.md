---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Shader System Implementation Checklist

This checklist tracks the implementation progress through each migration milestone.

---

## Pre-Implementation Setup

- [ ] Review all spec files (00-10) with team
- [ ] Address questions from 10-review-and-recommendations.md
- [x] Set up test assets directory (`assets/shaders/`)
- [ ] Create backup branch before starting M1
- [ ] Set up regression test infrastructure

---

## M1: Config, Loader & Pipeline Integration (2-3 weeks)

### Parser Implementation
- [x] Create `lib/src/renderer/resources/loaders/shader_loader.h`
- [x] Create `lib/src/renderer/resources/loaders/shader_loader.c`
- [x] Implement `shader_config_parse_result shader_loader_parse(String8 path, Arena *arena)`
  - [x] Parse `version` key
  - [x] Parse `name` key
  - [x] Parse `renderpass` key
  - [x] Parse `stages` key (comma-separated)
  - [x] Parse `stagefiles` key (comma-separated)
  - [x] Parse `use_instance` and `use_local` flags
  - [x] Parse `attribute=type,name` (repeating)
  - [x] Parse `uniform=type,scope,name` (repeating)
- [x] Implement type string → enum mapping
  - [x] Attribute types: `vec2`, `vec3`, `vec4`, `mat4`, `int32`, etc.
  - [x] Uniform types: `float`, `vec2`, `vec3`, `vec4`, `mat4`, `samp`, etc.
- [x] Validate stage count matches stagefile count
- [x] Validate no duplicate attribute/uniform names
- [x] Compute offsets/sizes/strides (but don't use yet)
  - [x] Implement `align_up(value, alignment)` helper
  - [x] Implement `get_std140_alignment(type)` helper
  - [x] Implement `get_uniform_size(type)` helper
  - [x] Compute `attribute_stride`
  - [x] Compute `global_ubo_size`, `global_ubo_stride`
  - [x] Compute `instance_ubo_size`, `instance_ubo_stride`
  - [x] Compute `push_constant_size`, `push_constant_stride`
  - [x] Track texture slot counts

### Pipeline Registry Integration
- [x] Add function `vkr_pipeline_registry_create_from_shader_config(...)`
- [x] Build `VkrShaderObjectDescription` from `shader_config`
  - [x] Populate `modules[].path` from `stage_filenames`
  - [x] Set entry points to `vertexMain`/`fragmentMain`
  - [x] Set `file_type` based on stage file count
- [x] Remove hardcoded shader path `assets/deafult.world.spv`
- [x] Update pipeline creation to accept config path
- [x] Keep using geometry system's vertex layouts (defer to M3)
- [ ] Fallback: missing shader config resolves to domain default with warning logged

### Backend Changes (Minimal)
- [x] Update `vulkan_shader_object_create` to read `modules[i].path` from config
- [x] Keep hardcoded `sizeof(VkrGlobalUniformObject)` for now
- [x] Keep hardcoded `sizeof(VkrInstanceUniformObject)` for now
- [x] No descriptor layout changes

### Test Assets
- [x] Create `assets/shaders/default.world.shadercfg` (from template)
- [x] Create `assets/shaders/default.ui.shadercfg` (from template)
- [ ] Create `assets/shaders/debug.solid_color.shadercfg` (from template)

### Testing M1
- [ ] Unit test: parse valid config → success
- [ ] Unit test: parse invalid config → error with message
- [ ] Unit test: duplicate uniform names → error
- [ ] Unit test: stage/stagefile count mismatch → error
- [x] Integration test: load shader from config, create pipeline, render frame (manual verification via app logs)
- [ ] Regression test: compare framebuffer to hardcoded system (should be identical)
- [ ] Integration test: parse error handling (missing file, syntax error)
- [ ] Fallback path: verify domain default binding when shader config is missing

### M1 Deliverable
- [x] Config-driven shader loading works end-to-end
- [x] Zero visual changes from hardcoded system (manual check)
- [ ] All tests passing (deferred)
- [x] Code review completed (sign-off below)
- [x] Documentation updated (README/migration plan/checklist)

> Note: Per request, unit/integration test automation is deferred.

---

## M2: Backend Uses Config-Driven Sizes (1 week)

### Backend Updates
- [x] Update `vulkan_shader_object_create`:
  - [x] Replace `sizeof(VkrGlobalUniformObject)` with `shader->global_ubo_size`
  - [x] Replace `sizeof(VkrInstanceUniformObject)` with `shader->instance_ubo_size`
  - [x] Use `shader->global_ubo_stride` for per-frame offsets
  - [x] Use `shader->instance_ubo_stride` for per-instance offsets
- [x] Update `vulkan_shader_update_global_state`:
  - [x] Use `shader->global_ubo_size` for buffer write size
- [x] Update `vulkan_shader_update_instance_state`:
  - [x] Use `shader->instance_ubo_size` for buffer write size
  - [x] Calculate offset: `instance_ubo_stride * instance_id`
- [x] Update push constant code:
  - [x] Use `shader->push_constant_size` instead of `sizeof(Mat4)`
  - [x] Create pipeline layout range from config

### Descriptor & Per-frame Decisions
- [x] Decide and implement dynamic offsets vs per-instance suballocation consistently
- [x] Ensure per-frame (double/triple) descriptor sets to avoid hazards

### Validation
- [x] Add assertion: `global_ubo_stride >= global_ubo_size`
- [x] Add assertion: `instance_ubo_stride >= instance_ubo_size`
- [x] Verify config-computed sizes match hardcoded sizes for default shaders
- [x] Add device limit checks (UBO size, push constant size)

### Testing M2
- [ ] Regression test: framebuffer still identical to M1
- [ ] Integration test: render with modified UBO (add extra uniforms to config)
- [ ] GPU debugging: verify UBO uploads with RenderDoc
- [ ] Unit test: vec3 alignment edge case (wastes 4 bytes)
- [ ] Stress test: maximum UBO size (within device limits)
- [ ] Verify dynamic offset/suballoc path across frames without hazards

### M2 Deliverable
- [x] Backend fully uses config-computed sizes
- [x] Can add/remove uniforms without code changes
- [ ] All tests passing
- [ ] Performance profiling shows no regression

---

## M2.5: Front-end Shader System API (1 week)

### Public API Surface
- [x] Implement initialization/shutdown: `shader_system_initialize`, `shader_system_shutdown`
- [x] Implement creation and lookup: `shader_system_create`, `shader_system_get`, `shader_system_get_id`, `shader_system_get_by_id`
- [x] Implement use/bind: `shader_system_use`, `shader_system_use_by_id`, `shader_system_bind_instance`
- [x] Implement uniform/sampler set by name: `shader_system_uniform_set`, `shader_system_sampler_set`
- [x] Implement uniform/sampler set by index: `shader_system_uniform_set_by_index`, `shader_system_sampler_set_by_index`
- [x] Implement instance resource management: `shader_acquire_instance_resources`, `shader_release_instance_resources` (temporary no-ops)
- [x] Two-phase init: `shader_system_initialize(uint64_t *memory_requirement, void *memory, config)`
- [x] Provide `SHADER_SYSTEM_CONFIG_DEFAULT` macro and adopt sane defaults

### Shader Object & Metadata
- [x] Define opaque `shader` type with:
  - [x] `name`, id; store uniform names and name→index hashtable
  - [x] Arrays for uniforms with names; sizes/offsets tracked in backend for now
  - [ ] Required device alignment values and texture slot counts

### Integration & Validation
- [x] Populate from `shader_config` returned by loader
- [x] Validate resource limits (uniform counts, texture counts, UBO and push constant sizes)
- [x] Compute/verify sizes and strides match loader expectations
- [x] Hook into backend update functions via pipeline registry
- [x] Implement `shader_system_apply_global/instance` delegating to pipeline registry
- [ ] Document single-threaded create/destroy and render-thread-only updates; add assertions

### Testing M2.5
- [ ] Create shader from config and drive it via the API in a test scene
- [ ] Verify name vs index setter paths work
- [ ] Ensure graceful error handling on invalid names/IDs
- [ ] Validate two-phase init and defaults path

### M2.5 Deliverable
- [ ] Front-end API operational and driving backend without visual changes

---

## M3: Attributes from Config (2 weeks, OPTIONAL)

### Config Flag
- [x] Add `derive_vertex_layout_from_config` to `shader_config` (default: false)

### Vertex Layout Generation
- [x] Implement config-derived vertex layout in registry when flag enabled
  - [x] Map attribute formats (float vec2/3/4) to `VkrVertexFormat`
  - [x] Use geometry stride/offsets to match buffer layout and keep config locations
- [x] Otherwise, fallback to geometry system (current behavior)

### Compatibility Checking
- [x] Add compatibility warnings in registry when deriving from config
  - [x] Warn on attribute count mismatch (config vs geometry)
  - [x] Warn on stride mismatch (uses geometry stride)
  - [x] Extra attributes are marked UNDEFINED

### Testing M3
- [x] Enable flag for `debug.solid_color` shader, verify rendering
- [x] Enable for world/ui; visuals correct after stride fix
- [ ] Regression tests (manual) – deferred
- [ ] Create `assets/shaders/debug.solid_color.shadercfg` test asset (single-file; no textures)

### M3 Deliverable
- [x] Optional shader-driven vertex layouts
- [x] Backward compatibility maintained
- [x] Flag can be enabled per-shader

---

## M4: Material → Shader Mapping (1 week)

### Material System Updates
- [x] Add `shader_name` field to `VkrMaterial` struct
- [x] Update material loader to parse `shader=` from `.mt` files
- [x] Implement `get_default_shader_for_domain(VkrPipelineDomain)` helper
  - [x] Map `WORLD` → `"shader.default.world"`
  - [x] Map `UI` → `"shader.default.ui"`
  - [x] Map `SHADOW` → `"shader.default.shadow"`
  - [x] Map `POST` → `"shader.default.post"`
- [x] Fallback: if no `shader=`, use `pipeline=` → default shader

### Application Updates
- [x] Replace `pipeline_registry_bind_pipeline(material->pipeline_id)` with shader system API
- [x] Implement per-material rendering loop:
  ```c
  shader_system_use(material->shader_name);
  shader_system_bind_instance(material->instance_id);
  shader_system_uniform_set("diffuse_color", &material->diffuse_color);
  shader_system_sampler_set("diffuse_texture", material->texture0);
  shader_system_apply_instance();
  ```

### Pipeline Registry Updates
- [x] Add `vkr_pipeline_registry_get_for_shader(shader_name, vertex_layout)`
- [x] Cache pipelines per (shader, vertex_layout) pair
- [x] Remove hardcoded pipeline→shader assumptions

### Testing M4
- [x] Create test `.mt` file with explicit `shader=shader.default.world`
- [x] Create test `.mt` file with legacy `pipeline=world` (should use default)
- [x] Render scene with mixed materials using different shaders
- [x] Verify pipeline switching when materials change shaders

### M4 Deliverable
- [x] Materials explicitly reference shaders
- [x] Decoupled from pipeline system
- [x] Backward compatibility with old `.mt` files

---

## M5: Terminology Cleanup (1 week)

### Vulkan Backend Renaming
- [x] `VulkanShaderObjectLocalState` → `VulkanShaderObjectInstanceState`
- [x] `local_descriptor_set_layout` → `instance_descriptor_set_layout`
- [x] `local_uniform_buffer` → `instance_uniform_buffer`
- [x] `local_descriptor_pool` → `instance_descriptor_pool`
- [x] `local_states` → `instance_states`
- [x] `local_state_free_count` → `instance_state_free_count`
- [x] `local_state_free_ids` → `instance_state_free_ids`

### Public API Renaming
- [x] `VkrLocalUniformObject` → `VkrInstanceUniformObject`
- [x] `vulkan_shader_update_state` → `vulkan_shader_update_instance`
- [x] `shader_acquire_local_resources` → `shader_acquire_instance_resources`
- [x] `shader_release_local_resources` → `shader_release_instance_resources`

### Documentation Updates
- [x] Update all comments to use consistent terminology
- [x] Update spec files (already done in 01-architecture.md)
- [x] Update README with terminology guide

### Testing M5
- [x] Full rebuild (should succeed)
- [x] Run entire test suite (should pass without changes)
- [x] Verify no semantic changes, only renames

### M5 Deliverable
- [x] Consistent terminology across codebase
- [x] Clear distinction: Global/Instance/Local

---

## M6: Final Cleanup (1 week)

### Remove Dead Code
- [x] Remove unused UBO size constants (if any)
- [x] Remove old shader path #defines
- [x] Remove legacy pipeline creation helpers
- [x] Remove temporary compatibility shims

### Documentation
- [x] Update architecture diagrams
- [x] Write user guide for `.shadercfg` format
- [x] Add usage examples to README
- [x] Document migration path from old system
- [x] Create troubleshooting guide (common errors, fixes)

### Performance Profiling
- [ ] Measure descriptor set allocation overhead
- [ ] Profile uniform buffer upload performance
- [ ] Optimize: batch descriptor writes if beneficial
- [ ] Cache pipeline layouts per shader
- [ ] Measure shader system lookup performance

### Telemetry & Stats
- [x] Add counters: global/instance applies per frame
- [x] Track descriptor writes avoided
- [x] Track instance resources acquired/released

### Developer Tools (Optional)
- [ ] Shader config validation tool (standalone binary)
  - [ ] Validates `.shadercfg` syntax
  - [ ] Checks UBO size limits
  - [ ] Reports computed offsets/strides
- [ ] Config generator from SPIR-V (using spirv-reflect)
- [ ] Hot reload support (if time permits)

### Testing M6
- [ ] Full regression suite
- [ ] Performance benchmarks (compare to pre-M1)
- [ ] Memory leak check (Valgrind/ASAN)
- [ ] GPU profiling (RenderDoc/NSight)

### M6 Deliverable
- [ ] Production-ready shader system
- [ ] Complete documentation
- [ ] All tests passing
- [ ] Performance within acceptable range
- [ ] Code review and sign-off

---

## Post-Implementation

- [ ] Merge to main branch
- [ ] Tag release (e.g., `v1.0-shader-system`)
- [ ] Monitor for issues in production use
- [ ] Plan future enhancements:
  - [ ] SPIR-V reflection
  - [ ] Shader variants
  - [ ] Hot reload
  - [ ] Descriptor indexing (bindless)
  - [ ] Compute shader support
  - [ ] Descriptor binding config in `.shadercfg`
  - [ ] Combined image sampler support (optional)
  - [ ] Per-stage push constant ranges (optional)
  - [ ] Entry point overrides in config
  - [ ] Device capability matrix validation
  - [ ] Extended material PBR mapping

---

## Notes

- Each milestone should be in its own branch
- Merge only after all tests pass
- Keep `main` branch stable at all times
- Document any deviations from spec in commit messages
- Update this checklist as you progress

**Estimated Total Time**: 8-10 weeks (including testing and code review)

