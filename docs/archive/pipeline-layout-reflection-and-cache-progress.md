---
status: superseded
updated: 2026-07-31
authority: progress
---

> **Archived.** Superseded by [`../rendering/pipeline-layout-reflection-and-cache-spec.md`](../rendering/pipeline-layout-reflection-and-cache-spec.md). Retained for history; do not treat as current.
# Pipeline Layout Reflection + Cache Implementation Progress

Spec: `pipeline-layout-reflection-and-cache-spec.md`
Last updated: 2026-02-06

This document tracks implementation progress for the reflection-driven pipeline
layout migration and pipeline cache integration. Update this file at the end of
each phase with what landed, file touches, and validation results.

## Status Board

- Phase 1: Reflection Module Skeleton + Error Surface - `completed`
- Phase 2: Descriptor/Push Constant/Vertex Reflection Core - `completed`
- Phase 3: Shader Program Integration + Metadata JSON - `completed`
- Phase 4: Pipeline Layout Build from Reflection (Sparse Sets Included) - `completed`
- Phase 5: Generic Descriptor Set Acquire/Update/Bind Path - `completed`
- Phase 6: Descriptor Pool Policy + Exhaustion Handling - `completed`
- Phase 7: Pipeline Cache Load/Use/Save - `completed`
- Phase 8: Legacy Removal + Cleanup Pass - `completed`
- Phase 9: Validation, Tests, and Performance Checks - `completed`

---

## Phase 1: Reflection Module Skeleton + Error Surface
Status: completed

Goal:
- Introduce reflection module files and error/reporting primitives without
  changing runtime behavior yet.

Implementation details (completed):
- Added `vulkan_spirv_reflection.c/h` module shell with explicit ownership
  contract for reflected entry-point data lifetime.
- Added renderer-level reflection error enum and context structure in Vulkan
  types for deterministic diagnostics propagation.
- Added reflection error context reset helper and stable error-to-string helper.
- Added minimal module parse utility:
  - validates SPIR-V code size constraints
  - creates `SpvReflectShaderModule`
  - resolves/canonicalizes entry point (`main` fallback)
  - validates optional expected stage against reflected stage
  - reports structured failure context on error
- Added module destroy utility that owns `spvReflectDestroyShaderModule()` and
  zeroes the wrapper object on teardown.

Expected files:
- `lib/src/renderer/vulkan/vulkan_spirv_reflection.c`
- `lib/src/renderer/vulkan/vulkan_spirv_reflection.h`
- `lib/src/renderer/vulkan/vulkan_types.h`

Exit criteria:
- Module compiles and is linkable.
- Reflection errors can be propagated to caller with shader/stage context.

Files touched:
- `lib/src/renderer/vulkan/vulkan_spirv_reflection.c`
- `lib/src/renderer/vulkan/vulkan_spirv_reflection.h`
- `lib/src/renderer/vulkan/vulkan_types.h`

Validation run:
- `./build.sh Debug` (success; renderer library and app linked, new reflection
  module compiled as part of the build)

Notes / Deferred:
- Reflection output extraction/normalization (descriptor merges, push constant
  interval normalization, vertex input extraction) is deferred to Phase 2.
- Runtime integration into shader program creation is deferred to Phase 3.

---

## Phase 2: Descriptor/Push Constant/Vertex Reflection Core
Status: completed

Goal:
- Implement canonical reflection extraction and normalization.

Implementation details (completed):
- Added phase-2 reflection data model to Vulkan types:
  - `VkrShaderStageModuleDesc`
  - `VkrSpirvReflectionCreateInfo`
  - `VkrDescriptorBindingDesc`
  - `VkrDescriptorSetDesc` / `VkrDescriptorSetRole`
  - `VkrPushConstantRangeDesc`
  - `VkrVertexInputBindingDesc`
  - `VkrVertexInputAttributeDesc`
  - `VkrUniformMemberDesc` / `VkrUniformBlockDesc`
  - `VkrShaderReflection`
- Added high-level reflection API:
  - `vulkan_spirv_shader_reflection_create()`
  - `vulkan_spirv_shader_reflection_destroy()`
- Implemented descriptor reflection merge across stages by `(set,binding)`:
  - deterministic sort by set then binding
  - stage flag OR merge
  - fatal mismatch checks for descriptor type/count
  - runtime-sized/static-invalid descriptor array rejection
  - sparse set support via `layout_set_count = max_set + 1`
- Implemented push constant extraction and normalization:
  - entry-point block enumeration per stage
  - alignment validation (`offset/size` 4-byte aligned)
  - device-limit validation via `max_push_constant_size`
  - boundary interval splitting + adjacent-range merge for identical stage flags
- Implemented vertex input reflection from VS:
  - built-ins skipped
  - fatal checks for component decoration (`component != 0`)
  - fatal checks for arrays/matrices/64-bit/unsupported formats
  - duplicate location detection
  - deterministic sort by location
  - packed 4-byte aligned offsets and stride generation (phase 2 default: binding 0, vertex rate)
- Implemented reflection name ownership for output descriptors/attributes using
  allocator-backed string duplication, and symmetric destroy-path cleanup.

Expected files:
- `lib/src/renderer/vulkan/vulkan_spirv_reflection.c`
- `lib/src/renderer/vulkan/vulkan_spirv_reflection.h`
- `lib/src/renderer/vulkan/vulkan_types.h`

Exit criteria:
- `VkrShaderReflection` can be produced for built-in VS+FS programs.
- Fatal reflection cases produce deterministic errors.

Files touched:
- `lib/src/renderer/vulkan/vulkan_spirv_reflection.c`
- `lib/src/renderer/vulkan/vulkan_spirv_reflection.h`
- `lib/src/renderer/vulkan/vulkan_types.h`

Validation run:
- `./build.sh Debug` (success)

Notes / Deferred:
- Instance-rate vertex binding assignment from metadata is deferred to Phase 3
  (`instance_rate_locations` JSON integration).
- Uniform block/member extraction is represented in the data model but remains
  optional and not populated yet.

---

## Phase 3: Shader Program Integration + Metadata JSON
Status: completed

Goal:
- Move shader program creation to reflection-backed descriptions and optional
  metadata JSON.

Implementation details (completed):
- Extended shader config ingestion and shader object description to carry
  optional reflection metadata path:
  - `metadata_path`/`metadata` keys parsed in shader config loader
  - propagated through pipeline registry into shader object description
- Extended reflection create input with `metadata_path` and added metadata JSON
  load/apply flow in reflection module:
  - parse `dynamic_descriptors`
  - parse `instance_rate_locations` (plus compatibility alias
    `instance_rate_inputs[].location`)
  - parse `set_roles` with strict role validation (`frame/material/draw/feature`)
  - validate all metadata references against reflected sets/bindings/locations
  - convert UBO/SSBO bindings marked dynamic into Vulkan dynamic descriptor
    types
  - apply per-set role annotations and reject conflicting role overrides
  - rebuild vertex bindings/offsets/strides after instance-rate remapping
- Integrated reflection lifecycle into Vulkan shader object:
  - load stage SPIR-V modules for reflection input
  - create `VkrShaderReflection` during shader object creation
  - store reflection in `VulkanShaderObject` (`has_reflection + reflection`)
  - release reflection memory in shader object destroy
- Linked vendored SPIR-V reflection implementation in `renderer_lib`
  (`vendor/spirv_reflect.c`) to satisfy new reflection symbols at link time.

Expected files:
- `lib/src/renderer/renderer_frontend.h`
- `lib/src/renderer/renderer_frontend.c`
- `lib/src/renderer/systems/vkr_shader_system.c`
- `lib/src/renderer/vulkan/vulkan_shaders.c`
- `lib/src/renderer/vulkan/vulkan_types.h`

Exit criteria:
- Shader program creation path produces reflection + metadata-augmented layout
  descriptors.
- Invalid metadata fails program creation with actionable diagnostics.

Files touched:
- `lib/src/renderer/resources/loaders/shader_loader.c`
- `lib/src/renderer/resources/vkr_resources.h`
- `lib/src/renderer/systems/vkr_pipeline_registry.c`
- `lib/src/renderer/vkr_renderer.h`
- `lib/src/renderer/vulkan/vulkan_types.h`
- `lib/src/renderer/vulkan/vulkan_spirv_reflection.c`
- `lib/src/renderer/vulkan/vulkan_shaders.c`
- `lib/CMakeLists.txt`

Validation run:
- `./build.sh Debug` (success; reflection module compiled and linked, app linked)

Notes / Deferred:
- Runtime pipeline layout/descriptor set layout generation still uses legacy
  fixed global/instance assumptions; reflection-driven layout creation begins in
  Phase 4.

---

## Phase 4: Pipeline Layout Build from Reflection (Sparse Sets Included)
Status: completed

Goal:
- Make reflection the only source of descriptor set layout, push constants, and
  vertex input for pipeline creation.

Implementation details (completed):
- Refactored graphics pipeline creation to consume `VkrShaderReflection`
  generated in Phase 3 instead of `VkrGraphicsPipelineDescription` vertex
  bindings/attributes for Vulkan vertex input state.
- Added reflected descriptor set layout construction at pipeline creation:
  - create exactly `layout_set_count` layouts (`0..layout_set_count-1`)
  - locate reflected set descriptors by set index
  - create empty layouts for sparse holes
  - preserve reflected binding order (canonicalized in Phase 2)
- Switched pipeline layout push constant setup to reflected ranges
  (`push_constant_ranges[]`) instead of a single hard-coded vertex-only range.
- Added explicit cleanup symmetry in pipeline creation:
  - deterministic teardown for temporary reflected set layouts on all paths
  - cleanup label now releases shader object/pipeline layout/pipeline resources
    on partial failure instead of leaking.

Expected files:
- `lib/src/renderer/vulkan/vulkan_pipeline.c`
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_backend.h`
- `lib/src/renderer/vulkan/vulkan_types.h`

Exit criteria:
- Graphics pipelines are creatable for built-in shaders with reflection-only
  layout derivation.
- No hard-coded push constant stage flags remain in pipeline setup.

Files touched:
- `lib/src/renderer/vulkan/vulkan_pipeline.c`

Validation run:
- `./build.sh Debug` (success)

Notes / Deferred:
- Runtime descriptor set update/bind still uses fixed global/instance code
  paths and assumes set usage contracts; generic arbitrary set update/bind
  remains Phase 5 work.

---

## Phase 5: Generic Descriptor Set Acquire/Update/Bind Path
Status: completed

Goal:
- Replace fixed global/instance update assumptions with generic set/binding
  operations validated against reflection.

Implementation details (completed):
- Added reflection-driven descriptor set contract resolution at shader-object
  creation time:
  - resolves runtime frame/draw set indices from set roles (with deterministic
    fallback)
  - resolves runtime binding indices for frame UBO/storage and draw
    UBO/sampled-image/sampler slots
  - precomputes per-set dynamic descriptor element counts for bind-time offset
    handling
- Replaced hard-coded frame/draw descriptor set layout creation with
  reflection-derived layout creation for runtime descriptor pools/sets.
- Added generic reflection-backed descriptor write validation helper:
  - validates `(set,binding)` existence
  - validates descriptor type match
  - validates array bounds (`array_element + count`)
- Updated frame descriptor update/bind path to:
  - use resolved reflection set index instead of fixed set `0`
  - use resolved reflected binding indices/types instead of fixed bindings
  - bind with dynamic offset array sized from reflected dynamic descriptors
- Updated draw/instance descriptor update/bind path to:
  - use resolved reflection set index instead of fixed set `1`
  - validate sampled image/sampler/uniform writes against reflection before
    `vkUpdateDescriptorSets`
  - bind draw set with dynamic offsets count derived from reflection
- Updated push constant upload path to use reflected normalized push constant
  ranges and stage masks instead of hard-coded vertex-stage-only push.
- Kept existing frontend usage stable while introducing generic reflection-aware
  backend behavior; acquire/release paths now safely no-op descriptor allocation
  when no draw descriptor set exists.

Expected files:
- `lib/src/renderer/renderer_frontend.h`
- `lib/src/renderer/renderer_frontend.c`
- `lib/src/renderer/vkr_renderer.h`
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_types.h`

Exit criteria:
- Descriptor update/bind calls work for any reflected set index.
- Invalid writes are rejected early with clear error logs.

Files touched:
- `lib/src/renderer/vulkan/vulkan_types.h`
- `lib/src/renderer/vulkan/vulkan_shaders.c`

Validation run:
- `./build.sh Debug` (success)

Notes / Deferred:
- Public frontend descriptor set acquire/update APIs by arbitrary set index are
  still not exposed; this phase completed generic backend behavior under
  existing frontend entry points.

---

## Phase 6: Descriptor Pool Policy + Exhaustion Handling
Status: completed

Goal:
- Implement role/lifetime-driven descriptor pool sizing with overflow fallback.

Implementation details (completed):
- Added descriptor pool sizing policy driven by reflected set role defaults:
  - frame set default capacity: `swapchain_image_count`
  - material set default capacity: `256`
  - draw set default capacity: `1024`
  - feature/none default capacity: `64`
  - draw-set capacity is clamped to
    `VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT`.
- Added per-shader-object instance descriptor pool tracking:
  - primary pool + overflow pool array
  - per-pool instance capacity tracking
  - per-instance descriptor-pool ownership (`VkDescriptorPool`) so frees are
    symmetric even after overflow fallback.
- Added overflow pool creation/retry path in descriptor set allocation:
  - on `VK_ERROR_OUT_OF_POOL_MEMORY` or `VK_ERROR_FRAGMENTED_POOL`, allocate a
    new overflow pool at `2x` previous instance capacity (clamped), then retry
    allocation once from the new pool.
  - maintain fallback/overflow counters and emit warning logs on overflow use.
- Added teardown coverage for all overflow pools during shader object destroy,
  plus debug telemetry summary (`overflow_pools`, `fallback_allocations`).

Expected files:
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_types.h`
- `lib/src/renderer/systems/vkr_pipeline_registry.c`

Exit criteria:
- Pool exhaustion path recovers with overflow pool retry.
- Program teardown releases all pools symmetrically.

Files touched:
- `lib/src/renderer/vulkan/vulkan_types.h`
- `lib/src/renderer/vulkan/vulkan_shaders.c`

Validation run:
- `./build.sh Debug` (success)

Notes / Deferred:
- Pool policy and overflow handling were implemented in Vulkan shader-object
  runtime (`vulkan_shaders.c`) rather than backend/pipeline-registry files,
  because descriptor set pools are owned and consumed at shader-object scope.

---

## Phase 7: Pipeline Cache Load/Use/Save
Status: completed

Goal:
- Add robust cache persistence across runs with non-fatal failure behavior.

Implementation details (completed):
- Added pipeline cache lifecycle wiring in Vulkan backend:
  - resolved `VKR_PIPELINE_CACHE_PATH` override first
  - added platform defaults:
    - macOS: `~/Library/Caches/VulkanRenderer/pipeline_cache_v1.bin`
    - Linux/other POSIX: `$XDG_CACHE_HOME/vulkan_renderer/pipeline_cache_v1.bin`
      with `~/.cache/vulkan_renderer/pipeline_cache_v1.bin` fallback
    - Windows: `%LOCALAPPDATA%\\VulkanRenderer\\pipeline_cache_v1.bin`
- Added cache load on backend initialization:
  - reads existing cache bytes when file exists
  - passes bytes into `vkCreatePipelineCache`
  - retries with empty cache on incompatible/corrupt data
  - keeps init non-fatal if cache load/create fails
- Added cache save on backend shutdown:
  - queries data size and payload via `vkGetPipelineCacheData`
  - ensures parent directory exists
  - writes temp file in same directory and promotes via rename
  - uses remove+rename retry for platforms that reject overwrite
  - keeps shutdown non-fatal on any cache write/promote failure
- Updated graphics pipeline creation to use backend cache handle:
  - `vkCreateGraphicsPipelines(..., state->pipeline_cache, ...)`

Expected files:
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_pipeline.c`
- `lib/src/renderer/vulkan/vulkan_types.h`

Files touched:
- `lib/src/renderer/vulkan/vulkan_types.h`
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_pipeline.c`

Validation run:
- `./build.sh Debug` (success)

Exit criteria:
- Cache integration is active for runtime pipeline creation.
- Cache failures do not abort renderer init or shutdown.

Notes / Deferred:
- Warm-restart performance delta and cache reuse telemetry verification are
  deferred to Phase 9 performance checks.
- Post-validation fixes applied after real-scene runs:
  - Restored shader-config vertex input layout precedence temporarily after
    early reflection-only bring-up to mitigate CPU vertex-struct padding
    mismatches (for example `Vec3` SIMD/padded offsets). This temporary bridge
    is superseded by the ABI-driven reflection vertex layout path added in
    Phase 8.
  - Fixed shader-object teardown bug in `vulkan_shaders.c` where
    `global_descriptor_pool` destruction was guarded by the wrong handle check.
    This now destroys global pool/layout handles symmetrically and prevents
    leaked descriptor sets at `vkDestroyDevice`.

---

## Phase 8: Legacy Removal + Cleanup Pass
Status: completed

Goal:
- Remove obsolete shader config and descriptor assumptions, then collapse
  duplicated transitional code.

Implementation details (completed):
- Switched Vulkan shader-object sizing/count initialization to reflection-only:
  - frame/draw UBO byte sizes derive from reflected binding metadata
    (`VkrDescriptorBindingDesc.byte_size`)
  - UBO strides derive from device `minUniformBufferOffsetAlignment`
  - push constant size derives from reflected push-constant ranges
  - material sampler slot count derives from reflected sampled-image/sampler
    descriptor counts instead of shader config values
- Added create-time validation for reflected runtime assumptions:
  - reflected UBO sizes must be non-zero when uniform bindings exist
  - reflected UBO/push-constant sizes are validated against device limits
  - draw sampled-image/sampler slots must map to contiguous single-descriptor
    bindings required by the current update path
- Removed remaining shader-config layout-size assignments from pipeline registry
  shader object description wiring; these values are now ignored and runtime
  layout sizing is reflection-driven.
- Removed explicit metadata-driven vertex layout overrides from runtime
  reflection binding rebuild. Vertex input layout now derives from reflection +
  host ABI profile only.
- Added vertex ABI profile propagation from pipeline registry to reflection:
  - ABI profiles: `3D`, `2D`, `Text2D`
  - reflected locations now map to `offsetof(...)` in packed host vertex
    structs (for example `VkrVertex3d`, `VkrVertex2d`, `VkrTextVertex`)
  - binding stride is fixed by ABI profile (`sizeof(vertex_struct)`), which
    preserves compatibility for shaders that reflect only a subset of
    attributes while sharing a full interleaved vertex buffer.
- Removed runtime metadata parsing/application entirely (`dynamic_descriptors`,
  `instance_rate_locations`, `set_roles` are no longer consumed by reflection).
- Applied practical host-ABI alignment for 3D vertices to reduce vertex layout
  fragility:
  - `VkrVertex3d.position`/`normal` migrated from SIMD `Vec3` storage to packed
    3-float fields (`VkrPackedVec3`) with explicit ABI assertions
    (`sizeof(VkrVertex3d)==64`, fixed offsets).
  - Geometry and mesh-loader CPU math paths now convert through
    `vkr_vertex_pack_vec3`/`vkr_vertex_unpack_vec3`, preserving math semantics
    while keeping deterministic GPU-facing memory layout.
  - 3D vertex layout uses packed offsets/stride (`0/12/24/32/48`, stride `64`)
    instead of prior SIMD-padded layout.
- Removed `metadata=` layout references from built-in `.shadercfg` files.
- Added explicit `vertex_abi` shader-config contract (`3d`, `2d`, `text2d`) and
  removed vertex-ABI heuristic inference in pipeline registry.
- Added backend runtime layout query path for reflected shader sizes:
  - backend now exposes pipeline runtime layout sizes (global/instance UBO,
    push constants, sampler slot counts)
  - pipeline registry snapshots reflected runtime sizes into the stored pipeline
    description at creation time
  - shader system staging/apply paths now consume these runtime sizes instead of
    legacy `VkrShaderConfig` propagated size fields
- Removed legacy shadercfg attribute-layout inference/validation from loader:
  - deleted canonical host-vertex offset assignment pass
  - removed vertex-type-required validation gate from shadercfg validation
  - made `metadata_path`/`metadata` keys hard-fail parse errors (no deprecated
    compatibility parsing)
  - default render pass fallback remains world-domain when explicit field is absent

Expected files:
- `lib/src/renderer/resources/` (shader config loaders)
- `lib/src/renderer/systems/vkr_shader_system.*`
- `lib/src/renderer/systems/vkr_pipeline_registry.*`
- `assets/shaders/` (legacy config asset references)

Exit criteria:
- No runtime layout decisions depend on shader config assets.
- Reflection-only path is the sole live path for program/pipeline layout data.

Files touched:
- `lib/src/renderer/vulkan/vulkan_shaders.c`
- `lib/src/renderer/systems/vkr_pipeline_registry.c`
- `lib/src/renderer/vulkan/vulkan_spirv_reflection.c`
- `lib/src/renderer/vulkan/vulkan_pipeline.c`
- `lib/src/renderer/renderer_frontend.c`
- `lib/src/renderer/vkr_renderer.h`
- `lib/src/renderer/vulkan/vulkan_backend.h`
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/systems/vkr_shader_system.c`
- `lib/src/renderer/resources/loaders/shader_loader.c`
- `assets/shaders/*.shadercfg`

Validation run:
- `./build.sh Debug` (success)
- `./build_test.sh` (success)

Notes / Deferred:
- `test.sh` runtime smoke script cannot be executed end-to-end in this sandbox
  due environment constraints around local launch services, but local build/tests
  succeeded and user-validated runtime scenarios passed during this phase.

---

## Phase 9: Validation, Tests, and Performance Checks
Status: completed

Goal:
- Validate correctness, safety, and performance expectations from the spec.

Implemented:
- Added a dedicated reflection validation suite:
  - success path for built-in world shader reflection
  - duplicate-stage rejection
  - missing vertex ABI rejection
  - repeated reflection create/destroy loop (64 cycles) to validate allocator
    symmetry under repeated lifecycle churn
- Wired the new reflection test suite into the global test runner.
- Added debug-time reflection layout logging at shader-object creation:
  - reflected set/binding layout summary
  - reflected push-constant range summary
- Added descriptor-bind safety check that enforces dynamic-offset count symmetry
  before issuing `vkCmdBindDescriptorSets`.
- Added pipeline creation timing telemetry:
  - per-pipeline `Pipeline create time: ... ms` log emitted around
    `vkCreateGraphicsPipelines`
  - extended `test.sh` parsing to summarize pipeline creation timing samples.

Expected files:
- `tests/src/reflection_pipeline_test.c`
- `tests/src/reflection_pipeline_test.h`
- `tests/src/test_main.c`
- `tests/src/test_main.h`
- `lib/src/renderer/vulkan/vulkan_shaders.c`
- `lib/src/renderer/vulkan/vulkan_pipeline.c`
- `test.sh`

Exit criteria:
- Test suite passes for new coverage.
- Acceptance criteria in spec section 19 are satisfied and documented.

Validation run:
- `./build_test.sh` (success; reflection suite included and passing)
- `./build.sh Debug` (success)

Notes / Deferred:
- Full app launch + warm-cache smoke timing from `test.sh` cannot be executed
  end-to-end inside this sandbox due host launch-service restrictions for GUI
  app startup and transient vcpkg lock constraints in nested script rebuilds.
  Unit/integration coverage and compile validation succeeded locally in this
  environment.

---

## Phase Log Template (Use Per Completed Phase)

Copy this block under the phase when completed:

- Implemented:
  - ...
- Files touched:
  - ...
- Validation run:
  - ...
- Notes / Deferred:
  - ...
