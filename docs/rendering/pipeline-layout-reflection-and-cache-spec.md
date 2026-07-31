---
status: partial
updated: 2026-07-31
authority: spec
---
# Reflection-Driven Pipeline Layout + Cache Spec (LLM-Consumable)

## 1. Purpose

Replace the shader config system with **mandatory SPIR-V reflection** that derives:
- descriptor set layouts and pipeline layouts
- push constant ranges
- vertex input layout
- optional uniform block/member metadata (for tooling)

Also add **Vulkan pipeline cache** persistence and a **generic descriptor update path**.

This is a **clean break** migration:
- reflection is the only layout source of truth
- no legacy `.shadercfg` layout ingestion remains
- no fallback path to manual layout declarations

---

## 2. Non-Goals (This Phase)

- Compute pipelines.
- Descriptor indexing features (`VK_EXT_descriptor_indexing` style features).
- Runtime-sized descriptor arrays.
- Matrix/array vertex input expansion.
- Shader hot reload.
- Any backward-compatible bridge to `VkrShaderConfig`.

---

## 3. Normative Language

- **MUST / MUST NOT**: required for correctness.
- **SHOULD / SHOULD NOT**: strong default, deviation must be intentional.
- **MAY**: optional.

---

## 4. Assumptions and Constraints

- Use the already vendored SPIR-V reflection library under `vendor/`.
- Reflection is per module + entry point from `VkrShaderStageModuleDesc.entry_point`.
  - Empty entry point defaults to `"main"`.
- One module per stage per program. Duplicate stage modules are invalid.
- Graphics stages only in this spec (`VS/FS/GS/TES/TCS`).
- Descriptor array counts must be static and `> 0`.
- Sparse descriptor set indices are valid and MUST be preserved in pipeline layout.
- `VkrShaderReflection` lifetime is intentionally decoupled from `VkPipeline` lifetime so hot reload can be added later without reworking reflection data ownership.

---

## 5. Scope of Changes

- Delete shader config ingestion for descriptor layouts, vertex attributes, and UBO sizes.
- Add a reflection module that produces canonical `VkrShaderReflection`.
- Replace fixed global/instance descriptor assumptions with arbitrary set/binding handling.
- Build `VkDescriptorSetLayout` + `VkPipelineLayout` from reflection only.
- Add pipeline cache load/use/save.

---

## 6. Architecture Overview

1. **Shader Program Load**
   - Load SPIR-V bytes for each stage module.
   - Reflect descriptors, push constants, and vertex inputs.
   - Validate explicit shader `vertex_abi` contract (`3d`/`2d`/`text2d`) for
     host-side vertex layout mapping.
   - Produce immutable `VkrShaderReflection` stored with the shader program.

2. **Pipeline Creation**
   - Build descriptor set layouts from reflected sets/bindings.
   - Build pipeline layout from reflected set layouts + reflected push constants.
   - Build vertex input state from reflected vertex stage inputs.
   - Pass backend pipeline cache to `vkCreateGraphicsPipelines`.

3. **Runtime Descriptor Use**
   - Allocate descriptor sets using reflection-derived layout identity.
   - Update descriptors through generic write API validated against reflection.
   - Bind any reflected set index; no implicit `set=0/1` contract.

---

## 7. Key Invariants (Must-Haves)

- Reflection failure is fatal for shader program creation.
- Unsupported reflection features are fatal (no silent fallback).
- Descriptor set/binding compatibility across stages is validated.
- Push constant ranges come from reflection (never hard-coded).
- Sparse set indices are preserved in pipeline layout by inserting empty set layouts.
- All reflected names copied out of reflection-library-owned memory before module destruction.

---

## 8. Deterministic Canonicalization Rules

Canonicalization exists to make results stable across runs and suitable for hashing/caching.

- Sort reflected sets by ascending set index.
- Sort bindings inside each set by ascending binding index.
- Merge descriptor binding stage flags across all stages.
- Sort vertex attributes by ascending location.
- Canonicalize missing names to empty `String8` (length 0, non-owning `NULL` pointer allowed only if length is 0).

---

## 9. Reflection Algorithms

### 9.1 Stage Module Parsing

For each `VkrShaderStageModuleDesc`:
1. Load SPIR-V bytes.
2. Build reflection module using vendored API (`spvReflectCreateShaderModule2()` when available, otherwise equivalent API in current vendored version).
3. Resolve entry point (`desc.entry_point` or `"main"`).
4. Reflect descriptors, push constants, and stage interface data from that entry point.
5. Destroy reflection module only after all reflected strings needed by engine
   are copied.

### 9.2 Descriptor Merge Across Stages

Use a map keyed by `(set, binding)`.

For each reflected binding from each stage:
- Convert to canonical `VkDescriptorType`.
- Compute canonical descriptor count (product of static array dimensions).
- Validate count is static and `> 0`.
- Insert or merge into map:
  - same key + same type + same count: OR stage flag
  - same key + different type/count: **fatal error**

Output `VkrDescriptorSetDesc[]` from canonicalized map.

### 9.3 Descriptor Type Mapping

Required mapping:
- Uniform buffer block -> `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER`
- Storage buffer block -> `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`
- Sampled image -> `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`
- Separate sampler -> `VK_DESCRIPTOR_TYPE_SAMPLER`
- Combined image sampler -> `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`
- Storage image -> `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE`
- Input attachment -> `VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT`

Fatal in this phase:
- Uniform/storage texel buffers
- Acceleration structures
- Inline uniform blocks
- Runtime-sized arrays

### 9.4 Push Constant Normalization (Deterministic + Minimal)

Input: reflected push constant ranges `(offset, size, stages)` from all stages.

Validation:
- `offset % 4 == 0`
- `size % 4 == 0`
- `size > 0`
- `offset + size <= device_limits.maxPushConstantsSize`

Normalization algorithm (interval splitting):
1. Collect boundaries `{offset, offset+size}` from all ranges.
2. Sort and deduplicate boundaries.
3. For each adjacent interval `[b[i], b[i+1])`, compute union of stage flags for source ranges covering the interval.
4. Drop intervals with empty stage flags.
5. Merge adjacent intervals only when stage flags are identical.

Output is a minimal list of non-overlapping ranges valid for `VkPipelineLayoutCreateInfo`.

### 9.5 Vertex Input Reflection

Only vertex-stage user inputs participate.

Supported:
- 32-bit scalar/vector float/int/uint types.

Fatal in this phase:
- matrices
- arrays
- 64-bit attributes
- `Component` decoration with value != 0
- duplicate user `Location`
- missing user `Location`
- formats not representable as supported `VkFormat`

Algorithm:
1. Enumerate VS input variables.
2. Skip built-ins.
3. Validate and map each variable to `VkFormat`.
4. Require explicit shader `vertex_abi` contract.
5. Map reflected locations to fixed host offsets/stride from ABI profile.
6. Sort by location.
7. Compute binding 1 offsets/stride only for explicitly reflected instance-rate
   attributes (if any); binding 0 stride is profile-defined.

Notes:
- Location gaps are allowed.
- If there are no user vertex inputs: `vertex_binding_count = 0`, `vertex_attribute_count = 0`.

### 9.6 Uniform Block Member Reflection (Optional)

Member reflection is optional and not required for pipeline creation.

If enabled:
- Copy per-member name/offset/size/stride/matrix info for tooling.

If disabled:
- set `member_count = 0`
- set `members = NULL`

---

## 10. Shader ABI Contract

Runtime shader layout metadata JSON is removed.

Vertex layout compatibility is now controlled by explicit shader config key:
- `vertex_abi=3d`
- `vertex_abi=2d`
- `vertex_abi=text2d`

Rules:
- If a shader declares vertex attributes, `vertex_abi` MUST be present.
- Unknown `vertex_abi` values are fatal during shader config parsing.
- `metadata_path`/`metadata` keys are no longer supported and are fatal.
- Reflection uses ABI profile mapping for binding 0 offsets/stride; it does not
  accept runtime JSON overrides.

---

## 11. Recommended Descriptor Set Role Conventions

These are optimization hints, not hard constraints:
- set 0: frame/global
- set 1: material/static resources
- set 2: draw/instance
- set 3+: feature-specific

If no role is provided, default to `VKR_DESCRIPTOR_SET_ROLE_NONE`.

---

## 12. Descriptor Allocation and Pool Policy

### 12.1 Default Lifetime Policy

- `FRAME` lifetime for frequently changing data.
- `PROGRAM` lifetime for mostly static resources.
- If role is `NONE`, default to `PROGRAM`.

```c
typedef enum VkrDescriptorSetLifetime {
  VKR_DESCRIPTOR_SET_LIFETIME_FRAME = 0,
  VKR_DESCRIPTOR_SET_LIFETIME_PROGRAM,
  VKR_DESCRIPTOR_SET_LIFETIME_COUNT
} VkrDescriptorSetLifetime;
```

### 12.2 Initial Pool Sizing (Simple Baseline)

Per program, estimate required set allocations by role:
- `frame`: `swapchain_image_count`
- `material`: `max_materials_per_program` (default 256)
- `draw`: `max_draws_per_program` (default 1024)
- `feature`/`none`: `default_set_allocation_count` (default 64)

For each reflected set:
1. count descriptors by `VkDescriptorType`
2. multiply by expected set allocations for that role
3. apply 20% slack

### 12.3 Exhaustion Policy

If `vkAllocateDescriptorSets` returns `VK_ERROR_OUT_OF_POOL_MEMORY` or `VK_ERROR_FRAGMENTED_POOL`:
1. log warning with shader/set context
2. allocate overflow pool with 2x previous capacity
3. retry allocation once
4. retain overflow pool for teardown at program destruction

---

## 13. Data Structures (Proposed)

```c
typedef struct VkrShaderStageModuleDesc {
  VkShaderStageFlagBits stage;  // exactly one stage
  String8 path;
  String8 entry_point;          // empty => "main"
} VkrShaderStageModuleDesc;

typedef struct VkrShaderProgramDesc {
  uint32_t module_count;
  const VkrShaderStageModuleDesc *modules;
  VkrVertexAbiProfile vertex_abi_profile;
} VkrShaderProgramDesc;

typedef enum VkrDescriptorSetRole {
  VKR_DESCRIPTOR_SET_ROLE_NONE = 0,
  VKR_DESCRIPTOR_SET_ROLE_FRAME,
  VKR_DESCRIPTOR_SET_ROLE_MATERIAL,
  VKR_DESCRIPTOR_SET_ROLE_DRAW,
  VKR_DESCRIPTOR_SET_ROLE_FEATURE,
  VKR_DESCRIPTOR_SET_ROLE_COUNT
} VkrDescriptorSetRole;

typedef struct VkrDescriptorBindingDesc {
  uint32_t binding;
  VkDescriptorType type;
  uint32_t count;
  VkShaderStageFlags stages;
  String8 name;
} VkrDescriptorBindingDesc;

typedef struct VkrDescriptorSetDesc {
  uint32_t set;
  VkrDescriptorSetRole role;
  uint32_t binding_count;
  VkrDescriptorBindingDesc *bindings;
} VkrDescriptorSetDesc;

typedef struct VkrPushConstantRangeDesc {
  uint32_t offset;
  uint32_t size;
  VkShaderStageFlags stages;
} VkrPushConstantRangeDesc;

typedef struct VkrVertexInputBindingDesc {
  uint32_t binding;
  uint32_t stride;
  VkVertexInputRate rate;
} VkrVertexInputBindingDesc;

typedef struct VkrVertexInputAttributeDesc {
  uint32_t location;
  uint32_t binding;
  VkFormat format;
  uint32_t offset;
  String8 name;
} VkrVertexInputAttributeDesc;

typedef struct VkrUniformMemberDesc {
  String8 name;
  uint32_t offset;
  uint32_t size;
  uint32_t array_stride;
  uint32_t matrix_stride;
  uint32_t columns;
  uint32_t rows;
} VkrUniformMemberDesc;

typedef struct VkrUniformBlockDesc {
  String8 name;
  uint32_t set;
  uint32_t binding;
  uint32_t size;
  uint32_t member_count;
  VkrUniformMemberDesc *members;
} VkrUniformBlockDesc;

typedef struct VkrShaderReflection {
  uint32_t set_count;                 // number of non-empty reflected sets
  VkrDescriptorSetDesc *sets;         // sorted by set index
  uint32_t layout_set_count;          // max_set_index + 1, includes sparse holes
  uint32_t push_constant_range_count;
  VkrPushConstantRangeDesc *push_constant_ranges;
  uint32_t vertex_binding_count;
  VkrVertexInputBindingDesc *vertex_bindings;
  uint32_t vertex_attribute_count;
  VkrVertexInputAttributeDesc *vertex_attributes;
  uint32_t uniform_block_count;
  VkrUniformBlockDesc *uniform_blocks;
} VkrShaderReflection;
```

Ownership and lifetime:
- All arrays/strings in `VkrShaderReflection` are owned by shader-program lifetime allocator.
- Reflection library memory is never stored directly.

---

## 14. API Surface (High-Level)

```c
VkrShaderProgramHandle vkr_renderer_shader_program_create(
    VkrRendererFrontendHandle renderer,
    const VkrShaderProgramDesc *desc,
    VkrRendererError *out_error);

void vkr_renderer_shader_program_destroy(
    VkrRendererFrontendHandle renderer,
    VkrShaderProgramHandle program);

VkrPipelineHandle vkr_renderer_pipeline_create(
    VkrRendererFrontendHandle renderer,
    const VkrPipelineFixedState *fixed_state,
    VkrShaderProgramHandle program,
    VkrRenderPassHandle render_pass,
    VkrRendererError *out_error);

VkrDescriptorSetHandle vkr_renderer_descriptor_set_acquire(
    VkrRendererFrontendHandle renderer,
    VkrShaderProgramHandle program,
    uint32_t set_index,
    VkrDescriptorSetLifetime lifetime,
    VkrRendererError *out_error);

bool8_t vkr_renderer_descriptor_set_update(
    VkrRendererFrontendHandle renderer,
    VkrDescriptorSetHandle set,
    const VkrDescriptorWrite *writes,
    uint32_t write_count);

void vkr_renderer_descriptor_set_release(
    VkrRendererFrontendHandle renderer,
    VkrDescriptorSetHandle set);

void vkr_renderer_bind_pipeline(
    VkrRendererFrontendHandle renderer,
    VkrPipelineHandle pipeline);

void vkr_renderer_bind_descriptor_set(
    VkrRendererFrontendHandle renderer,
    VkrPipelineHandle pipeline,
    VkrDescriptorSetHandle set,
    uint32_t set_index,
    const uint32_t *dynamic_offsets,
    uint32_t dynamic_offset_count);
```

Generic descriptor write:

```c
typedef struct VkrDescriptorImageInfo {
  VkSampler sampler;
  VkImageView image_view;
  VkImageLayout image_layout;
} VkrDescriptorImageInfo;

typedef struct VkrDescriptorBufferInfo {
  VkBuffer buffer;
  VkDeviceSize offset;
  VkDeviceSize range;
} VkrDescriptorBufferInfo;

typedef struct VkrDescriptorWrite {
  uint32_t binding;
  uint32_t array_element;
  VkDescriptorType type;
  uint32_t count;
  const VkrDescriptorImageInfo *images;
  const VkrDescriptorBufferInfo *buffers;
} VkrDescriptorWrite;
```

Update validation rules:
- `(set,binding)` exists in reflection.
- `type` matches reflected type.
- `array_element + count` is in bounds.
- buffer-backed types require `buffers != NULL`.
- image/sampler-backed types require `images != NULL`.

Dynamic offset rules (current single-set bind API):
- offsets are consumed by dynamic descriptors in this set only
- ordering: ascending binding index, then ascending array element
- `dynamic_offset_count` must match reflected dynamic element count for the bound set

---

## 15. Pipeline Layout Rules

- Create exactly `layout_set_count` `VkDescriptorSetLayout` objects (`0..layout_set_count-1`).
- Missing reflected set indices use empty layouts.
- Binding order inside each set layout is ascending binding index.
- Store `layout_set_count` with pipeline/program so runtime can validate `set_index` quickly.

---

## 16. Pipeline Cache

### 16.1 Backend State

- `VkPipelineCache pipeline_cache;`
- `String8 pipeline_cache_path;`

### 16.2 Cache Path Policy

- Override: `VKR_PIPELINE_CACHE_PATH`.
- Default paths:
  - macOS: `~/Library/Caches/VulkanRenderer/pipeline_cache_v1.bin`
  - Linux: `$XDG_CACHE_HOME/vulkan_renderer/pipeline_cache_v1.bin` (fallback `~/.cache/`)
  - Windows: `%LOCALAPPDATA%/VulkanRenderer/pipeline_cache_v1.bin`

`_v1` is an engine-controlled cache epoch suffix. Bump when intentionally invalidating old cache files.

### 16.3 Initialization

1. Resolve path and create parent directory if needed.
2. If file exists, read bytes.
3. Call `vkCreatePipelineCache` with initial data when available.
4. If creation returns incompatible/invalid cache error, retry with empty cache and log warning.
5. Never fail renderer initialization solely because cache load failed.

### 16.4 Shutdown

1. Call `vkGetPipelineCacheData` twice (size query + write).
2. Write to temp file in same directory.
3. Atomically rename temp file to final path.
4. Destroy pipeline cache.

If any step fails, log warning and continue shutdown.

---

## 17. Reflection Error Handling

### 17.1 Fatal Errors

- SPIR-V parse/module creation failure.
- Duplicate shader stage module in one program.
- Missing entry point.
- Descriptor `(set,binding)` type mismatch across stages.
- Descriptor `(set,binding)` count mismatch across stages.
- Unsupported descriptor kind.
- Runtime descriptor array.
- Vertex input missing `Location`.
- Vertex input `Component != 0`.
- Duplicate vertex location.
- Unsupported vertex attribute type/format.
- Missing/unknown vertex ABI profile for shaders with vertex inputs.
- Push constant alignment/size/limit violation.

### 17.2 Error Enum (Proposed)

```c
typedef enum VkrReflectionError {
  VKR_REFLECTION_OK = 0,
  VKR_REFLECTION_ERROR_PARSE_FAILED,
  VKR_REFLECTION_ERROR_DUPLICATE_STAGE,
  VKR_REFLECTION_ERROR_ENTRY_POINT_NOT_FOUND,
  VKR_REFLECTION_ERROR_BINDING_TYPE_MISMATCH,
  VKR_REFLECTION_ERROR_BINDING_COUNT_MISMATCH,
  VKR_REFLECTION_ERROR_UNSUPPORTED_DESCRIPTOR,
  VKR_REFLECTION_ERROR_RUNTIME_ARRAY,
  VKR_REFLECTION_ERROR_MISSING_LOCATION,
  VKR_REFLECTION_ERROR_VERTEX_COMPONENT_DECORATION,
  VKR_REFLECTION_ERROR_DUPLICATE_VERTEX_LOCATION,
  VKR_REFLECTION_ERROR_UNSUPPORTED_VERTEX_INPUT,
  VKR_REFLECTION_ERROR_PUSH_CONSTANT_ALIGNMENT,
  VKR_REFLECTION_ERROR_PUSH_CONSTANT_LIMIT,
} VkrReflectionError;
```

### 17.3 Error Context Contract

Every reflection error MUST include:
- shader program identifier/path
- stage (if stage-specific)
- entry point
- set/binding or location when relevant
- concise human-readable description

---

## 18. Vertex Format Compatibility Policy

Current mesh layouts (`VkrVertex3d`, `VkrVertex2d`, etc.) MUST match reflected layout for each pipeline.

If a shader's reflected vertex layout does not match the mesh format bound for that draw path, pipeline creation or draw setup MUST fail with a clear mismatch message.

This keeps incompatibility explicit and prevents silent attribute corruption.

---

## 19. Implementation Plan

1. Remove `VkrShaderConfig` layout-dependent logic and delete `.shadercfg` asset usage.
2. Add `lib/src/renderer/vulkan/vulkan_spirv_reflection.c/h`.
3. Add `VkrShaderProgramDesc`/`VkrShaderProgramHandle` and persist `VkrShaderReflection` in program state.
4. Implement descriptor/push-constant/vertex reflection with deterministic canonicalization.
5. Enforce explicit shader `vertex_abi` contract and remove runtime metadata
   JSON ingestion.
6. Build descriptor set layouts and pipeline layout from reflection only (including sparse-set empty layouts).
7. Replace fixed descriptor update/bind paths with generic set/binding APIs.
8. Implement per-program descriptor pool policy with overflow handling.
9. Add pipeline cache load/use/save and wire into graphics pipeline creation.
10. Remove legacy code paths and dead data structures in cleanup pass.

Acceptance criteria:
- no shader-config layout path remains
- built-in shaders produce valid reflection-driven pipelines
- arbitrary set/binding updates work
- cache file persists and is reused across runs

---

## 20. Validation and Testing

- **Unit tests (reflection):**
  - stage merge mismatch detection
  - sparse set handling
  - push constant interval normalization
  - vertex format rejection cases
  - missing/invalid vertex ABI rejection

- **Integration tests (renderer):**
  - create/destroy programs repeatedly without leaks
  - descriptor set allocate/update/bind across multiple sets
  - pipeline creation with cache cold/warm runs

- **Debug checks:**
  - print reflected set layouts/push constants in debug builds
  - log dynamic offset expected count vs supplied count at bind time

- **Performance check:**
  - compare pipeline creation time first run vs warm cache run
  - collect per-pipeline timing around `vkCreateGraphicsPipelines` and
    summarize cold/warm runs from runtime logs

---

## 21. Risk Notes and Mitigations

- Vertex packing mismatch risk:
  - mitigate with strict reflection-vs-mesh validation and clear diagnostics.

- Descriptor pool pressure risk:
  - mitigate with role-based defaults + overflow pool fallback telemetry.

- ABI contract drift risk:
  - mitigate with strict `vertex_abi` validation and fail-fast errors.

- Reflection string lifetime risk:
  - mitigate by immediate deep-copy of all names before module destruction.

---

## 22. Expected File Touch Map

- `lib/src/renderer/vulkan/`
  - new: `vulkan_spirv_reflection.c/h`
  - update: `vulkan_shaders.c`, `vulkan_pipeline.c`, `vulkan_backend.c/h`
- `lib/src/renderer/systems/`
  - update/remove: `vkr_shader_system.*`, `vkr_pipeline_registry.*`
- `lib/src/renderer/resources/`
  - remove: shader config loader(s)
- `docs/rendering/`
  - update: this spec
