---
status: partial
updated: 2026-07-31
authority: design
---
# Uniform Buffer Layout Migration: DX cbuffer to std430 (and what that means here)

## Executive Summary

This document describes a plan to migrate the renderer away from **HLSL/DX cbuffer packing** (previously forced via `slangc -fvk-use-dx-layout`) toward **standard, backend-friendly layouts**. Phase 0 is now implemented: all shader build scripts compile UBOs with Slang’s default `std140` layout.

Important nuance: **“std430” is not the default layout for uniform buffers.** In Vulkan/SPIR-V it is the standard layout for *storage buffers* (SSBOs). For *uniform buffers* (UBOs) you normally get `std140`-style layout unless you opt into Vulkan 1.2 features and emit the matching SPIR-V capabilities. (We target Vulkan 1.2 on MoltenVK, but these features are still opt-in at device creation.) This doc therefore splits the migration into:
- **Baseline (recommended)**: UBOs use `std140` (Slang’s default for `ConstantBuffer<T>`), SSBOs use `std430`.
- **Optional tightening**: use Vulkan 1.2 `scalarBlockLayout` (Slang `-fvk-use-scalar-layout`) if we want “tighter-than-std430” packing on Vulkan.

**Key Benefits (realistic in this codebase):**
- Removes the DX-only packing dependency (unblocks Metal and other backends).
- Makes layouts **spec-defined** (`std140` for UBO, `std430` for SSBO) instead of “engine-defined”.
- Reduces wasted space for *large arrays* once moved to SSBOs (where `std430` is actually applicable).
- Enables stronger validation (SPIR-V decoration offsets become meaningful ground truth).

**Non-benefits / common misconceptions:**
- UBO memory savings may be limited by `minUniformBufferOffsetAlignment` (often 256 bytes) when using dynamic offsets; shrinking a 160B UBO to 120B still costs a 256B stride.
- “CPU structs match shader layout” is only true if we use **packed GPU-facing types**. This engine’s `Vec3` is SIMD (`VKR_SIMD_F32X4`) and is **16 bytes**, not a 12-byte `float3`.

---

## Implementation Status (as of now)

- Toolchain: DX packing removed from all shader build scripts (std140 UBOs by default).
- Loader: `std140` alignment rules fixed; DX register packing removed.
- Shaders: padding-only fields removed from `default.text.slang` (others still need audit under std140).

Pending:
- Recompile `.spv` with the new flags.
- Audit UBO struct field order for std140 (especially `scalar → float3` sequences).
- Refresh or remove stale “computed layout” comments in `.shadercfg` files.

---

## Toolchain Reality (Slang)

Shader compilation flags are now consistent across scripts (DX packing removed in all build entry points), so `.spv` layout is stable across build paths.

Slang supports these relevant modes for SPIR-V:

- **DX cbuffer packing**: `-fvk-use-dx-layout` (legacy; no longer used).
- **Default UBO packing**: *no flag* → Slang emits `std140`-style layout for `ConstantBuffer<T>`.
- **Scalar block layout**: `-force-glsl-scalar-layout` / `-fvk-use-scalar-layout` → emits SPIR-V using the scalar/natural rules (requires Vulkan feature enablement; see Appendix B).

Note: Slang also exposes `-fvk-use-gl-layout`, but it is documented as affecting **raw buffer load/store** layout, not `ConstantBuffer<T>` member packing.

There is no single “flip” that makes `ConstantBuffer<T>` become `std430` everywhere; **`std430` applies naturally to SSBOs**, and UBO “std430-like” rules require `uniformBufferStandardLayout` plus toolchain support to emit the correct SPIR-V capability.

---

## Current Architecture Analysis

### Layout System Location

The uniform buffer layout calculation is centralized in the shader loader:

| File | Purpose |
|------|---------|
| `lib/src/renderer/resources/loaders/shader_loader.c` | Layout computation (`vkr_compute_uniform_layout`) |
| `lib/src/renderer/resources/vkr_resources.h` | Shader config structs (`VkrShaderConfig`, `VkrShaderUniformDesc`) |
| `assets/shaders/*.slang` | GPU-side struct definitions using Slang `ConstantBuffer<T>` |

### Current Constants (shader_loader.c:18-21)

```c
#define VKR_SHADER_UBO_ALIGNMENT 256              // Assumed minUniformBufferOffsetAlignment (device property)
#define VKR_SHADER_PUSH_CONSTANT_ALIGNMENT 4      // Push constant alignment
#define VKR_SHADER_STD140_BASE_ALIGNMENT 16       // std140 base alignment for arrays/structs
```

`VKR_SHADER_UBO_ALIGNMENT` is a portability footgun. Vulkan requires dynamic UBO offsets to be aligned to `minUniformBufferOffsetAlignment` queried from the physical device; **it is not guaranteed to be 256**. The migration work is a good time to either:
- rename this constant to reflect that it is an *assumption*, or
- plumb the real value from the Vulkan backend into the shader system.

### Current Alignment Functions (shader_loader.c:228-260)

```c
// Returns std140 base alignment for each type.
vkr_internal INLINE uint64_t vkr_std140_alignment(VkrShaderUniformType type) {
  switch (type) {
  case SHADER_UNIFORM_TYPE_FLOAT32:
  case SHADER_UNIFORM_TYPE_INT32:
  case SHADER_UNIFORM_TYPE_UINT32:
    return sizeof(float32_t);           // 4-byte alignment
  case SHADER_UNIFORM_TYPE_FLOAT32_2:
    return sizeof(float32_t) * 2;       // 8-byte alignment
  case SHADER_UNIFORM_TYPE_FLOAT32_3:
  case SHADER_UNIFORM_TYPE_FLOAT32_4:
  case SHADER_UNIFORM_TYPE_MATRIX_4:
    return sizeof(float32_t) * 4;       // 16-byte alignment
  // ...
  }
}
```

### Current Layout Computation (shader_loader.c:526-610)

The `vkr_compute_uniform_layout()` function applies these rules:
1. Arrays: std140 array stride = `align_up(size, max(align, 16))`
2. Scalars/vectors: Align to std140 base alignment (no DX register packing)
3. Final UBO size: Rounded up to the max alignment seen in the block

```c
if (ud->array_count > 1) {
  if (element_align < VKR_SHADER_STD140_BASE_ALIGNMENT) {
    element_align = VKR_SHADER_STD140_BASE_ALIGNMENT;
  }
  uint64_t element_stride = vkr_align_up_u64(element_size, element_align);
  total_size = element_stride * ud->array_count;
}
```

### Shader-Side Current Patterns (Slang)

#### default.world.slang (Lines 1-30)
```slang
struct GlobalUniformBufferObject {
    column_major float4x4 projection;  // offset 0,   size 64
    column_major float4x4 view;        // offset 64,  size 64
    float4 ambient_color;              // offset 128, size 16
    float3 view_position;              // offset 144, size 12
    uint32_t render_mode;              // offset 156, size 4 (fills the 16B slot)

    uint32_t dir_enabled;              // offset 160, size 4
    float3 dir_direction;              // std140: aligns to 16B (12B padding after dir_enabled)
    float4 dir_color;                  // offset 176, size 16 (aligns to register 11)

    uint32_t point_light_count;        // offset 192, size 4
    float4 point_light_data[48];       // offset 208, size 768 (16*48, each vec4 = 16 bytes)
};
```

#### default.text.slang (Lines 8-13)
```slang
struct LocalUniformObject {
    float4 diffuse_color;     // 16 bytes
    float  screen_px_range;   // 4 bytes
    float  font_mode;         // 4 bytes
};
```

### CPU-Side Structures

#### vkr_renderer.h:521-534 (VkrGlobalMaterialState)
```c
typedef struct VkrGlobalMaterialState {
  Mat4 projection;        // 64 bytes
  Mat4 view;              // 64 bytes
  Mat4 ui_projection;     // 64 bytes (NOT in shader - separate UBO)
  Mat4 ui_view;           // 64 bytes (NOT in shader - separate UBO)
  Vec4 ambient_color;     // 16 bytes
  Vec3 view_position;     // 16 bytes (SIMD Vec4 storage; shader often uses float3)
  VkrRenderMode render_mode;  // 4 bytes (enum = uint32_t)
} VkrGlobalMaterialState;
```

#### vkr_resources.h:112-117 (VkrPhongProperties)
```c
typedef struct VkrPhongProperties {
  Vec4 diffuse_color;        // 16 bytes
  Vec4 specular_color;       // 16 bytes
  float32_t shininess;       // 4 bytes
  Vec3 emission_color;       // 16 bytes (SIMD); do not assume shader float3 size
} VkrPhongProperties;
```

**Key takeaway:** the engine’s math types are optimized for CPU SIMD, not GPU byte-exact packing. Avoid “memcpy a struct into a UBO” unless the struct is explicitly designed for GPU layout. The renderer already uploads most material/light values via per-uniform writes, which sidesteps this class of mismatch.

---

## Legacy DX Layout (Background / Why We Removed It)

The issues below describe the DX packing model that was previously enforced by the build scripts. Phase 0 removed DX packing from the toolchain; these remain as rationale and as anti-patterns to avoid in new shader structs.

### 1. Array Element Waste

Current behavior for `float point_light_data[48]` (if it were floats, not vec4s):
```
DX Layout:  48 elements * 16-byte stride = 768 bytes
std430:     48 elements * 4-byte stride  = 192 bytes  (SSBO, or UBO w/ opt-in feature)
Waste:      576 bytes (75% overhead!)
```

For vec4 arrays this is fine, but for scalar/vec2 arrays the waste is significant. Note that `vec3` arrays still have a 16-byte stride in both `std140` and `std430` (because `vec3` base alignment is 16).

Also note: if the engine uses dynamic UBO offsets, the **per-instance stride** is still aligned up to `minUniformBufferOffsetAlignment` (often 256 bytes), so savings only show up when data is moved into an SSBO or otherwise not forced into 256-byte slots.

### 2. Register Boundary Padding

```
struct Example {
    float  a;      // offset 0
    float3 b;      // DX: offset 4 (fits in remaining 12 bytes of the first 16B row)
};
// DX: 16 bytes total
// std140/std430: 32 bytes total (float3 requires 16-byte alignment)
```

### 3. CPU/GPU Structure Mismatch

DX packing makes it very easy for shader structs to “accidentally” become DX-only:
- scalars can precede `float3` without forcing a 16-byte alignment
- arrays default to 16-byte stride even for scalars/vec2

This matters because the renderer’s CPU-side layout computation in `shader_loader.c` must match the SPIR-V member offsets exactly. If we want portability, we need to pick a standard layout (baseline: `std140`) and adjust shader structs accordingly.

As an example, `assets/shaders/default.world.slang` uses:

```c
// GPU (shader)
struct LocalUniformObject {
    float4 diffuse_color;     // offset 0
    float4 specular_color;    // offset 16
    float3 emission_color;    // offset 32 (12 bytes)
    float shininess;          // offset 44 (uses the remaining 4 bytes of the 16B slot)
    uint32_t texture_flags;   // offset 48
};                            // size/stride rules depend on chosen layout + engine stride policy
```

The renderer avoids direct-struct memcpy for most material data (it writes each uniform by name/type), which is good. The risky cases are “bulk copy” paths (e.g. building instance UBO blocks as raw bytes) and any future code that assumes `sizeof(Vec3)==12`.

### 4. Explicit Padding Requirements

Shaders sometimes add manual padding (see `float2 _padding` in `assets/shaders/default.text.slang`) to make the *struct size/stride* a clean multiple of 16 bytes. This is a symptom of two separate issues:
- DX packing: many layouts “work” until a field order changes and crosses a 16B boundary.
- Engine stride: even if the last member ends at byte 24, the next per-instance block must start aligned for its first member (often 16, and then rounded up to `minUniformBufferOffsetAlignment` if using dynamic offsets).

---

## Layout Rules Reference (std140 vs std430 vs scalar)

### std140/std430 (GLSL spec 4.60, Section 7.6.2.2)

| Type | Alignment | Size |
|------|-----------|------|
| `float`, `int`, `uint` | 4 | 4 |
| `vec2`, `ivec2`, `uvec2` | 8 | 8 |
| `vec3`, `ivec3`, `uvec3` | 16 | 12 |
| `vec4`, `ivec4`, `uvec4` | 16 | 16 |
| `mat4` (column_major) | 16 | 64 |
| Array of T | align(T) | N * stride(T), where stride(T) is: std140 → `max(align(T),16)`; std430 → `align_up(size(T), align(T))` |
| Struct | max(member alignments) | size rounded up to struct alignment |

### Key Differences (what changes vs DX packing)

| Aspect | DX cbuffer packing | std140/std430 |
|--------|---------------------|--------|
| `float` followed by `float3` | Can pack `float3` at offset 4 | `float3` requires 16-byte alignment |
| Array stride (`float[N]`, `vec2[N]`) | 16-byte stride | std140: 16-byte stride, std430: tight stride |
| “No cross 16B row” rule | Yes | No (std layouts are expressed via explicit member offsets) |

**Note:** `vec3` is awkward in std layouts (16-byte aligned, 12-byte size). Best practice: avoid `float3` in blocks that need portability and predictable packing; prefer `float4` or pack scalars into the “.w”.

---

## Proposed Changes

### Phase 0: Pick a Layout Contract (UBO vs SSBO)

Before touching CPU layout code, decide what the *contract* is:

1. **Baseline contract (recommended):**
   - UBOs: `std140` (Slang default for `ConstantBuffer<T>` when *not* using `-fvk-use-dx-layout`)
   - SSBOs: `std430` (natural fit; also aligns with “Phase 4 (SSBO)” in `docs/rendering/lighting-system-design-plan.md`)

2. **Optional tightening on Vulkan:**
   - UBO/SSBO: scalar block layout (`slangc -fvk-use-scalar-layout`)
   - Requires enabling Vulkan feature `scalarBlockLayout` at device creation time.

3. **UBO “std430-like” (only if supported end-to-end):**
   - Vulkan feature: `uniformBufferStandardLayout`
   - Toolchain: must emit SPIR-V using the matching capability; verify Slang support before committing to this path.

### Phase 1: CPU-Side Layout Computation (match the contract)

#### 1.1 Fix std140 alignment (shader_loader.c)

The current `vkr_std140_alignment()` is a hybrid and is not actually `std140` (e.g. it returns 4-byte alignment for `vec2`/`vec3`). For the baseline contract, replace it with correct `std140` base alignment:

```c
// std140 base alignment (no DX "row packing" rule).
// Array element stride is handled separately (std140 forces 16B stride).
vkr_internal INLINE uint64_t vkr_std140_base_alignment(VkrShaderUniformType type) {
  switch (type) {
  case SHADER_UNIFORM_TYPE_FLOAT32:
  case SHADER_UNIFORM_TYPE_INT32:
  case SHADER_UNIFORM_TYPE_UINT32:
    return 4;
  case SHADER_UNIFORM_TYPE_FLOAT32_2:
    return 8;
  case SHADER_UNIFORM_TYPE_FLOAT32_3:
  case SHADER_UNIFORM_TYPE_FLOAT32_4:
  case SHADER_UNIFORM_TYPE_MATRIX_4:
    return 16;
  default:
    return 4;
  }
}
```

#### 1.2 Remove DX register packing

Delete `vkr_apply_uniform_register_packing()` and any “don’t cross 16B” logic. `std140`/`std430` layouts are defined by explicit offsets that follow base-alignment rules (plus array/struct stride rules).

#### 1.3 Push constants: offsets vs API alignment

Push-constant **member offsets** should follow the same layout rules as other blocks (base alignment per type, plus array rules if used). Separately, Vulkan requires `vkCmdPushConstants` `offset` and `size` to be multiples of 4. Do not treat “push constants = 4-byte alignment for everything” as a layout rule.

### Phase 2: Shader Compilation + Shader Struct Updates

#### 2.1 Remove DX layout from SPIR-V builds (baseline std140)

Update the SPIR-V compilation path to stop forcing DX packing:
- `build.sh`, `build.bat`, and release scripts that pass `-fvk-use-dx-layout`

After this change, Slang will emit `std140`-style layout for `ConstantBuffer<T>` by default.

#### 2.2 Fix shader structs that relied on DX packing

Any pattern like “`uint` then `float3`” will change offsets under `std140`/`std430` because `float3` requires 16-byte alignment. Prefer to repack into 16-byte-friendly fields:

```slang
struct GlobalUniformBufferObject {
    column_major float4x4 projection;  // offset 0
    column_major float4x4 view;        // offset 64
    float4 ambient_color;              // offset 128
    float4 view_position_and_mode;     // xyz = position, w = render_mode (store as float if possible, or keep render_mode separate and reorder to avoid scalar→float3 sequences)
    // ... etc
};
```

#### 2.3 Remove “dummy padding” members that are not part of the shadercfg contract

If a padding field exists only to make the struct “look aligned”, but it is not declared in the `.shadercfg`, it becomes a maintenance hazard (it can silently drift from what the CPU thinks exists).

Example: `assets/shaders/default.text.slang` previously included `float2 _padding;` as the last member (now removed). It can be removed if:
- the engine’s instance UBO stride is already aligned (it is, via `minUniformBufferOffsetAlignment`), and
- no shader code reads the padding field.

```slang
struct LocalUniformObject {
    float4 diffuse_color;     // offset 0
    float  screen_px_range;   // offset 16
    float  font_mode;         // offset 20
};
```

### Phase 3: CPU-Side Data Packing (optional)

If we keep the current “write each uniform by name/type” API (`vkr_shader_system_uniform_set()`), we do **not** need to reorder engine structs like `VkrPhongProperties` for correctness.

If we ever want to bulk-upload an entire UBO block as a blob (faster, fewer calls), introduce a dedicated GPU-facing packed struct that:
- uses plain scalar arrays (not SIMD `Vec3`/`Vec4`)
- matches the chosen shader layout (`std140` baseline) exactly
- is validated with `static_assert(sizeof/offsetof)` and/or reflection checks

Example sketch for a world material block (std140-friendly):
```c
typedef struct VkrWorldMaterialUboPacked {
  float32_t diffuse_color[4];
  float32_t specular_color[4];
  float32_t emission_color[3];
  float32_t shininess;
  uint32_t texture_flags;
  uint32_t _pad0[3];
} VkrWorldMaterialUboPacked;
```

### Phase 4: Shader Config (“single source of truth”) updates

#### 4.1 Make the chosen layout explicit

Add a `layout=` field to `.shadercfg` (recommended) so the loader can select the correct algorithm and error out early on mismatches:
- `layout=dx_cbuffer` (legacy)
- `layout=std140` (baseline)
- `layout=scalar` (optional Vulkan tightening)

#### 4.2 Fix/regen “computed layout” comments

Many `.shadercfg` files contain “computed layout” comments that can drift from reality. Either regenerate them from the loader, or delete them to avoid false confidence.

---

## Implementation Checklist

### File Changes Summary

| File | Changes |
|------|---------|
| `build.sh`, `build.bat`, release scripts | Remove `-fvk-use-dx-layout` (baseline `std140`) or replace with `-fvk-use-scalar-layout` (optional) |
| `lib/src/renderer/resources/loaders/shader_loader.c` | Implement correct `std140` layout and delete DX register packing |
| `assets/shaders/default.text.slang` | Remove padding-only member (`_padding`) |
| `assets/shaders/*.slang` | Repack structs that relied on DX packing (avoid `uint` → `float3` patterns) |
| `assets/shaders/*.spv` | Recompile all shaders |

### Step-by-Step Implementation

1. **Phase 0: Toolchain** (CRITICAL)
   - [x] Stop compiling SPIR-V with `-fvk-use-dx-layout`
   - [ ] Recompile all shaders and verify SPIR-V member offsets (`spirv-dis`)

2. **Phase 1: Loader** (HIGH)
   - [x] Fix `std140` alignment rules in `shader_loader.c`
   - [x] Remove DX register packing logic
   - [x] Ensure push-constant member offsets respect base alignment (not “always 4”)

3. **Phase 2: Shader structs** (HIGH)
   - [ ] Fix any UBO structs that depended on DX packing to keep the layout stable under `std140`
   - [x] Remove “padding-only” members that aren’t part of the `.shadercfg` contract (e.g. `default.text.slang`)

4. **Phase 3: Validation** (CRITICAL)
   - [ ] Run the app and check Vulkan validation output
   - [ ] Verify world materials, text rendering, and lighting correctness
   - [ ] If memory reduction is a goal, move large arrays to SSBOs (true `std430`) and measure again

### Validation Commands

```bash
# Recompile shaders
./build.sh

# Dump SPIR-V to verify layout (requires spirv-tools)
spirv-dis assets/shaders/default.world.spv | rg -n "OpMemberDecorate .* Offset"

# Run with Vulkan validation
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./build/app/vulkan_renderer
```

---

## Risk Assessment

### Low Risk
- CPU-side layout changes are isolated to `shader_loader.c`
- Can be tested incrementally shader-by-shader

### Medium Risk
- Shader recompilation required for all `.slang` files
- Potential for subtle layout mismatches if any uniform missed

### Mitigation
- Add debug logging of computed offsets vs expected
- Compare computed sizes against shader reflection (if available)
- Create test shader that validates layout at runtime

---

## Future Considerations

### 1. Avoid vec3 in UBOs
Even in standard layouts, `float3` is awkward (16-byte aligned, 12-byte size). Consider:
- Prefer `float4` and use `.w` for an extra scalar (or leave it unused).
- If you keep `float3`, order fields so a scalar can occupy the trailing 4 bytes (e.g. `float3 + float`).
- For large arrays of scalars/vec2, prefer SSBOs (`std430`) to avoid `std140`’s 16-byte array stride.

### 2. Push Constants
Vulkan only requires the *API call* `offset`/`size` to be multiples of 4, but push-constant **member offsets still have to satisfy the shader’s layout rules**. Treat push constants like any other block when computing offsets.

### 3. Shader Reflection
Consider adding reflection-based validation to automatically compare:
- loader-computed offsets/sizes (from `.shadercfg`) vs
- SPIR-V member offsets (from `OpMemberDecorate Offset`)

Slang can also emit additional reflection decorations for SPIR-V (`slangc -fspv-reflect`), which may help if/when we add runtime layout checks.

---

## Appendix A: Layout Comparison Examples

### Example 1: DX-only packing that breaks std140/std430

```
struct BadUnderStd140 {
  uint   enabled;    // offset 0
  float3 direction;  // DX: offset 4, std140/std430: offset 16
};
```

This is the main class of change when dropping `-fvk-use-dx-layout`: any `scalar → float3` sequence will shift.

### Example 2: Why SSBOs matter for “std430”

```
float weights[48];

UBO (std140): 48 * 16B stride = 768B
SSBO (std430): 48 *  4B stride = 192B
```

If memory reduction is the goal, the win comes from moving large scalar/vec2 arrays into SSBOs, not from trying to “make UBOs std430”.

### Example 3: “Removing padding” usually doesn’t change runtime memory

Even if a block’s “logical size” goes from 32B → 24B, dynamic UBO allocations often round up to a 256B stride (`minUniformBufferOffsetAlignment`). Removing padding fields is still useful (less drift, easier validation), but don’t expect a measurable memory win unless the data moves out of dynamic UBO slots.

---

## Appendix B: Slang Scalar Layout Reference

Slang supports scalar block layout for SPIR-V. This is **not** `std430`; it is a different (tighter) set of rules that aligns vectors like scalars.

You can enable it globally with `slangc -fvk-use-scalar-layout` (recommended if you choose this path), or on a per-declaration basis using `[[vk::scalar_layout]]`:

```slang
// Force scalar block layout
[[vk::binding(0, 0)]]
[[vk::scalar_layout]]
ConstantBuffer<MyStruct> ubo;

// Or use scalar block layout (GLSL-style)
[[vk::binding(0, 0)]]
[[vk::scalar_layout]]
cbuffer MyBuffer {
    float4x4 matrix;
    float3 position;
    float scale;
};
```

This generates SPIR-V with the `ScalarBlockLayout` capability, which requires enabling Vulkan feature `scalarBlockLayout` (core in Vulkan 1.2, originally `VK_EXT_scalar_block_layout`).

### Vulkan Extension Requirement

Enable scalar block layout at device creation:

```c
// During device creation (vulkan_device.c)
VkPhysicalDeviceScalarBlockLayoutFeatures scalar_layout = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
    .scalarBlockLayout = VK_TRUE,
};
// Chain to VkDeviceCreateInfo.pNext
```

Or if targeting Vulkan 1.2+, use `VkPhysicalDeviceVulkan12Features.scalarBlockLayout`.

---

## Appendix C: Vulkan “std430-like UBO” (Uniform Buffer Standard Layout)

Vulkan 1.2 also exposes `uniformBufferStandardLayout`, which allows uniform buffers to use a standard layout that is closer to `std430` than classic `std140`. This is core in Vulkan 1.2, but still opt-in and not guaranteed on every device/driver (verify on MoltenVK targets).

Practical constraints:
- The device feature must be enabled (`VkPhysicalDeviceVulkan12Features.uniformBufferStandardLayout = VK_TRUE`).
- The SPIR-V must use the matching capability (`UniformBufferStandardLayout`).
- The shader toolchain must be able to emit that capability for UBOs; verify Slang support before committing to this path.

If the goal is portability (including Metal), it is usually simpler to keep UBOs on `std140` and move large, array-heavy data into SSBOs (`std430`), where the win is unambiguous.
