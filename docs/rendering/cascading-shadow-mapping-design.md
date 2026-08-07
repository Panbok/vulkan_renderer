---
status: partial
updated: 2026-07-31
authority: design
---
# Cascading Shadow Mapping (CSM) Design Document

**Legacy note:** This document references the deprecated view/layer system
(`view system (removed)`) which has been removed. Render orchestration now uses
the render graph; shadow state lives in `VkrShadowSystem` and is consumed by
pass executors. CSM ships, but the historical integration examples have not
been fully reconciled with current code.

## Purpose

Implement **Cascading Shadow Mapping** for directional light shadows in the Vulkan renderer. CSM provides high-quality shadows across large view distances by splitting the view frustum into multiple cascades, each with its own shadow map at appropriate resolution.

This document is **LLM-consumable**: explicit file paths, concrete APIs, data structures, shader code, and a phased implementation plan.

---

## Current State

### Shadow Domain Infrastructure

The renderer already defines a shadow pipeline domain:
- `VKR_PIPELINE_DOMAIN_SHADOW` in `lib/src/renderer/vkr_renderer.h`
- Shadow render pass exists in `lib/src/renderer/vulkan/vulkan_renderpass.c`:
  - Depth-only attachment
  - `loadOp = CLEAR`, `storeOp = STORE`
  - Final layout: `DEPTH_STENCIL_READ_ONLY_OPTIMAL` (ready for sampling)

**Important:** the Vulkan backend supports the SHADOW *domain*, but the app does
not currently register a built-in named renderpass for it (unlike
`Renderpass.Builtin.World`, `Renderpass.Builtin.UI`, etc.). The CSM
implementation must create a named renderpass (e.g. `Renderpass.CSM.Shadow`)
via `vkr_renderer_renderpass_create()` or extend renderer initialization to
include it.

### Directional Light (Scene-Driven)

Directional light data is already driven by the scene + lighting system:
- `lib/src/renderer/systems/vkr_lighting_system.c` caches a single directional
  light (enabled, direction, color*intensity) and applies it to shader uniforms.
- `assets/shaders/default.world.shadercfg` / `lib/src/renderer/vulkan/shaders/world/default.slang`
  expose:
  - `dir_enabled` (uint32)
  - `dir_direction` (vec3)
  - `dir_color` (vec4)

CSM should consume the same direction as the world shader uses (the cached
direction in `VkrLightingSystem`), so shadows stay consistent with lighting.

Direction convention (important for correctness):
- `SceneDirectionalLight.direction_local` / `dir_direction` represent the **light ray direction**
  (the direction light travels in world space), with a typical “sun from above” default of `{0, -1, 0}`.
- Shaders compute `N·L` using `L = normalize(-dir_direction)` and should use the same convention for
  shadow bias and cascade fitting.
  - Example: `assets/scenes/default.scene.json` uses `direction_local = {-0.57735, -0.57735, -0.57735}`.

### Shader Binding Constraint (Affects Design)

The shader system currently updates only **instance** samplers (set 1); global
samplers (set 0) are not wired end-to-end yet (`vkr_shader_system_sampler_set()`
warns for non-instance samplers). The initial CSM design below therefore binds
shadow maps as additional **instance** samplers for the world shader. This is
simple to implement but it requires updating the world shader’s sampler binding
numbers because the backend derives sampler bindings from
`instance_texture_count`.

### Shader Packing Constraint (Must Match CPU Offsets)

Uniform offsets computed from `*.shadercfg` assume **DX-style cbuffer packing**
(not GLSL std140): a `vec3` occupies 12 bytes and a following scalar can pack
into the remaining 4 bytes of the same 16-byte slot. This is relied on by
existing uniforms (e.g. `view_position` + `render_mode`).

Slang must therefore compile SPIR-V with DX layout enabled:
- `build.sh` uses `slangc -fvk-use-dx-layout ...`

If shaders are compiled without `-fvk-use-dx-layout`, global UBO reads become
misaligned (lights/shadow uniforms appear “random” or zero), which often shows
up as a black scene or shadows that only appear in a tiny moving patch.

### Tradeoffs (Accepted for First Implementation)

- **More instance sampler slots**: the world shader uses more texture slots, and
  its sampler bindings must be renumbered to match the backend’s descriptor
  layout convention.
- **No texture arrays/atlas**: allocating one depth texture per cascade keeps
  the renderer changes small but costs memory and requires binding multiple
  textures in the world shader.
- **No cascade blending**: there may be a visible transition at split planes.
  We rely on reasonable split placement + stabilization to keep this acceptable
  initially.

### Frustum Culling System

`lib/src/math/vkr_frustum.h` provides:
- `VkrFrustum vkr_frustum_from_view_projection(Mat4 view, Mat4 projection)`
- `bool8_t vkr_frustum_test_sphere(const VkrFrustum *frustum, Vec3 center, float32_t radius)`

These can be extended for cascade frustum fitting.

---

## Goals

1. **Multi-cascade shadow maps**: 3-4 cascades covering near-to-far view frustum
2. **Stable cascade transitions**: Minimize shadow shimmering during camera movement
3. **PCF filtering**: Percentage-closer filtering for soft shadow edges
4. **Integration with existing lighting**: Extend `GlobalUniformBufferObject` with shadow data
5. **Debug visualization**: Optional cascade coloring for debugging

### Non-Goals (Initial Implementation)

- Point light shadows (omnidirectional shadow maps)
- Spot light shadows
- Variance shadow maps (VSM)
- Shadow cascades for multiple directional lights
- Transparent object shadows

---

## Architecture Overview

### Cascade System Components

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Per-Frame Flow                               │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  1. Cascade Fitting                                                  │
│     ┌──────────────────────────────────────────────────────────┐    │
│     │ For each cascade i:                                       │    │
│     │   - Compute sub-frustum corners (near_i, far_i)          │    │
│     │   - Transform to world space                              │    │
│     │   - Fit light-space ortho projection                      │    │
│     │   - Snap to texel grid (stabilization)                    │    │
│     │   - Store view-proj matrix + split distance               │    │
│     └──────────────────────────────────────────────────────────┘    │
│                              │                                       │
│                              ▼                                       │
│  2. Shadow Pass (for each cascade)                                   │
│     ┌──────────────────────────────────────────────────────────┐    │
│     │ - Begin pass for cascade i                                │    │
│     │ - Use cascade render target (per-frame, per-cascade)      │    │
│     │ - Render scene depth from light's view                    │    │
│     │ - Optional: frustum cull per cascade                      │    │
│     └──────────────────────────────────────────────────────────┘    │
│                              │                                       │
│                              ▼                                       │
│  3. Main World Pass                                                  │
│     ┌──────────────────────────────────────────────────────────┐    │
│     │ - Sample shadow map with cascade selection                │    │
│     │ - Apply PCF filtering                                     │    │
│     │ - Modulate lighting by shadow factor                      │    │
│     └──────────────────────────────────────────────────────────┘    │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Data Structures

### CPU-Side Types

Add to `lib/src/renderer/systems/vkr_shadow_system.h`:

```c
#define VKR_SHADOW_CASCADE_COUNT_MAX 4
#define VKR_SHADOW_MAP_SIZE_DEFAULT 1024

typedef struct VkrCascadeData {
  Mat4 view_projection;    // Light-space view-projection matrix
  float32_t split_far;     // Far split distance in front of the camera (positive view depth)
  float32_t world_units_per_texel; // For stabilization diagnostics (XY)
  Vec3 bounds_center;              // World-space cascade bounds center
  float32_t bounds_radius;         // World-space cascade bounds radius
} VkrCascadeData;

typedef struct VkrShadowConfig {
  uint32_t cascade_count;              // 1-4 cascades
  uint32_t shadow_map_size;            // Per-cascade resolution
  float32_t cascade_split_lambda;      // Logarithmic/linear blend (0.0-1.0)
  float32_t max_shadow_distance;       // Clamp far split (view space units)
  float32_t shadow_bias;               // Depth bias to prevent acne
  float32_t normal_bias;               // Normal-based offset
  float32_t pcf_radius;                // PCF radius in texels (typically 1)
  bool8_t stabilize_cascades;          // Snap to texel grid
  bool8_t debug_show_cascades;         // Color cascades for debugging
} VkrShadowConfig;

typedef struct VkrShadowFrameResources {
  // Per-swapchain-image resources to avoid frame-in-flight hazards.
  VkrTextureOpaqueHandle shadow_maps[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrRenderTargetHandle shadow_targets[VKR_SHADOW_CASCADE_COUNT_MAX];
} VkrShadowFrameResources;

typedef struct VkrShadowSystem {
  // Configuration
  VkrShadowConfig config;

  // Per-cascade data (updated each frame)
  VkrCascadeData cascades[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t cascade_splits[VKR_SHADOW_CASCADE_COUNT_MAX + 1]; // [0]=near, [count]=far

  // GPU resources
  uint32_t frame_resource_count; // = vkr_renderer_window_attachment_count()
  VkrShadowFrameResources *frames; // [frame_resource_count]
  VkrRenderPassHandle shadow_renderpass;
  VkrPipelineHandle shadow_pipeline; // VKR_PIPELINE_DOMAIN_SHADOW

  // Directional light state
  Vec3 light_direction;
  Vec4 light_color;
  bool8_t light_enabled;

  // State
  bool8_t initialized;
} VkrShadowSystem;
```

### GPU-Side Structures

#### Shadow Uniform Buffer Extension

Extend `GlobalUniformBufferObject` in `lib/src/renderer/vulkan/shaders/world/default.slang`.

Design goals for the uniform layout:
- Reuse the existing directional light uniforms (`dir_*`) for bias direction.
- Keep cascade data **contiguous and fixed-size** (max 4) to avoid dynamic
  descriptor/UBO layouts in the first implementation.
- Encode view-space split distances (positive “distance along forward”) so the
  fragment shader can select cascades using `view_depth = -view_pos.z`.

```hlsl
struct GlobalUniformBufferObject
{
    column_major float4x4 projection;
    column_major float4x4 view;
    float4 ambient_color;
    float3 view_position;
    uint32_t render_mode;

    // Directional light (already present today)
    uint32_t dir_enabled;
    float3 dir_direction;
    float4 dir_color;

    uint32_t point_light_count;
    float4 point_light_data[MAX_POINT_LIGHTS * 3];

    // --------------------------------------------------------------------
    // CSM (new)
    // --------------------------------------------------------------------
    uint32_t shadow_enabled;          // 0/1, separate from dir_enabled
    uint32_t shadow_cascade_count;    // 1..4
    float shadow_map_inv_size;        // 1.0 / shadow_map_size (square maps)
    float shadow_pcf_radius;          // radius in texels (typically 1.0)

    float4 shadow_split_far;          // far split distances for cascades 0..3 (view space)
    float shadow_bias;                // constant bias in shadow depth units
    float shadow_normal_bias;         // scales with (1 - N·L)
    uint32_t shadow_debug_cascades;   // 0/1
    uint32_t shadow_debug_mode;       // 0=off, 1=cascades, 2=factor, 3=depth, 4..7=maps

    column_major float4x4 shadow_view_projection[4];
};
```

---

## Cascade Fitting Algorithm

### Split Distance Calculation

Use a practical split scheme combining logarithmic and linear distributions:

```c
void vkr_shadow_compute_cascade_splits(
    VkrShadowSystem *system,
    float32_t near_clip,
    float32_t far_clip,
    float32_t lambda)  // 0.0 = linear, 1.0 = logarithmic
{
  uint32_t count = system->config.cascade_count;
  if (count == 0) {
    system->cascade_splits[0] = near_clip;
    return;
  }

  // Clamp shadow distance to keep far cascades from wasting resolution.
  float32_t far_for_shadows = far_clip;
  if (system->config.max_shadow_distance > 0.0f) {
    far_for_shadows = vkr_min_f32(far_for_shadows, system->config.max_shadow_distance);
  }
  far_for_shadows = vkr_max_f32(far_for_shadows, near_clip + 0.001f);

  for (uint32_t i = 0; i <= count; ++i) {
    float32_t p = (float32_t)i / (float32_t)count;

    // Logarithmic distribution (better near detail)
    float32_t log_split = near_clip * powf(far_for_shadows / near_clip, p);

    // Linear distribution
    float32_t linear_split = near_clip + (far_for_shadows - near_clip) * p;

    // Blend between schemes
    system->cascade_splits[i] = lambda * log_split + (1.0f - lambda) * linear_split;
  }
}
```

### Frustum Corner Computation

Implementation note: the current implementation in
`lib/src/renderer/systems/vkr_shadow_system.c` computes cascade frustum corners
from the camera basis vectors (`camera->forward/right/up`) and frustum
parameters, instead of inverting the view-projection matrix. This avoids an
inverse on the hot path and matches how `VkrCamera` is already managed.

The design alternative below (inverse view-projection) is still valid, but the
aspect ratio used must match the camera projection used for the world pass.

```c
void vkr_shadow_compute_frustum_corners(
    const Mat4 *view,
    const Mat4 *projection,
    float32_t camera_near_clip,
    float32_t camera_far_clip,
    float32_t near_split,
    float32_t far_split,
    Vec3 out_corners[8])  // [0-3]=near plane, [4-7]=far plane
{
  // Compute inverse view-projection
  Mat4 vp = mat4_mul(*projection, *view);
  Mat4 inv_vp = mat4_inverse(vp);

  // NDC corners (Vulkan: Y inverted, Z in [0,1])
  Vec4 ndc_corners[8] = {
    // Near plane (z=0 in Vulkan)
    {-1.0f,  1.0f, 0.0f, 1.0f},  // top-left
    { 1.0f,  1.0f, 0.0f, 1.0f},  // top-right
    { 1.0f, -1.0f, 0.0f, 1.0f},  // bottom-right
    {-1.0f, -1.0f, 0.0f, 1.0f},  // bottom-left
    // Far plane (z=1)
    {-1.0f,  1.0f, 1.0f, 1.0f},
    { 1.0f,  1.0f, 1.0f, 1.0f},
    { 1.0f, -1.0f, 1.0f, 1.0f},
    {-1.0f, -1.0f, 1.0f, 1.0f},
  };

  // Transform to world space and interpolate to split distances
  Vec3 full_frustum_near[4], full_frustum_far[4];

  for (int i = 0; i < 4; ++i) {
    Vec4 world_near = mat4_mul_vec4(inv_vp, ndc_corners[i]);
    Vec4 world_far = mat4_mul_vec4(inv_vp, ndc_corners[i + 4]);

    world_near = vec4_div_scalar(world_near, world_near.w);
    world_far = vec4_div_scalar(world_far, world_far.w);

    full_frustum_near[i] = vec3_from_vec4(world_near);
    full_frustum_far[i] = vec3_from_vec4(world_far);
  }

  // Interpolate along frustum edges to get cascade-specific corners
  float32_t denom = (camera_far_clip - camera_near_clip);
  if (denom <= 0.0f) {
    // Invalid camera; return full frustum as a safe fallback.
    for (int i = 0; i < 4; ++i) {
      out_corners[i] = full_frustum_near[i];
      out_corners[i + 4] = full_frustum_far[i];
    }
    return;
  }

  float32_t near_t = (near_split - camera_near_clip) / denom;
  float32_t far_t = (far_split - camera_near_clip) / denom;

  // Clamp to avoid numerical issues when splits exceed camera range.
  near_t = vkr_clamp_f32(near_t, 0.0f, 1.0f);
  far_t = vkr_clamp_f32(far_t, 0.0f, 1.0f);

  for (int i = 0; i < 4; ++i) {
    out_corners[i] = vec3_lerp(full_frustum_near[i], full_frustum_far[i], near_t);
    out_corners[i + 4] = vec3_lerp(full_frustum_near[i], full_frustum_far[i], far_t);
  }
}
```

### Light-Space Matrix Construction

#### `mat4_ortho_vulkan()` (Required Helper)

`lib/src/math/mat.h` currently has `mat4_ortho()`, but its Z mapping is the
classic OpenGL-style `[-1, 1]`. For shadow maps we want to stay consistent with
`mat4_perspective()` which is already “Vulkan-flavored” (Y inverted, Z in
`[0, 1]`).

Add:

```c
static INLINE Mat4 mat4_ortho_vulkan(float32_t left, float32_t right,
                                     float32_t bottom, float32_t top,
                                     float32_t near_clip, float32_t far_clip) {
  float32_t rl = (right - left);
  float32_t tb = (top - bottom);
  float32_t fn = (far_clip - near_clip);

  // Note: Y is negated to match the engine's Vulkan clip-space convention.
  // Right-handed view (mat4_look_at): points in front have negative Z. Map
  // z=-near→0 and z=-far→1 for Vulkan's [0,1] depth range.
  return mat4_new(2.0f / rl, 0.0f, 0.0f, 0.0f,
                  0.0f, -2.0f / tb, 0.0f, 0.0f,
                  0.0f, 0.0f, -1.0f / fn, 0.0f,
                  -((right + left) / rl),
                  -((top + bottom) / tb),
                  -(near_clip / fn),
                  1.0f);
}
```

```c
void vkr_shadow_compute_cascade_matrix(
    const Vec3 *light_direction,
    const Vec3 frustum_corners[8],
    uint32_t shadow_map_size,
    bool8_t stabilize,
    Mat4 *out_view_projection,
    float32_t *out_world_units_per_texel)
{
  // Compute frustum center
  Vec3 center = vec3_zero();
  for (int i = 0; i < 8; ++i) {
    center = vec3_add(center, frustum_corners[i]);
  }
  center = vec3_div_scalar(center, 8.0f);

  // Light view matrix (directional light): position is arbitrary as long as the
  // frustum fits in the orthographic projection. We construct a view that looks
  // at the cascade center along the light direction.
  Vec3 dir = vec3_normalize(*light_direction);
  Vec3 up =
      (vkr_abs_f32(dir.y) > 0.99f) ? vec3_new(0, 0, 1) : vec3_new(0, 1, 0);
  Vec3 light_pos = vec3_sub(center, vec3_mul_scalar(dir, 1.0f));
  Mat4 light_view = mat4_look_at(light_pos, center, up);

  // Transform corners to light space
  Vec3 ls_min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
  Vec3 ls_max = vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);

  for (int i = 0; i < 8; ++i) {
    Vec4 ls_corner = mat4_mul_vec4(light_view, vec4_from_vec3(frustum_corners[i], 1.0f));
    ls_min = vec3_min(ls_min, vec3_from_vec4(ls_corner));
    ls_max = vec3_max(ls_max, vec3_from_vec4(ls_corner));
  }

  // Pad Z to reduce clipping of off-frustum casters.
  float32_t z_range = (ls_max.z - ls_min.z);
  float32_t z_pad = vkr_max_f32(5.0f, z_range * 0.05f);
  ls_min.z -= z_pad;
  ls_max.z += z_pad;

  float32_t width = ls_max.x - ls_min.x;
  float32_t height = ls_max.y - ls_min.y;
  float32_t extent = vkr_max_f32(width, height);
  if (extent < 0.001f) {
    extent = 0.001f;
  }

  float32_t left = 0.0f;
  float32_t right = 0.0f;
  float32_t bottom = 0.0f;
  float32_t top = 0.0f;

  // Stabilization: snap the cascade projection center to the texel grid and
  // use a fixed square span derived from a bounding sphere. This reduces
  // “breathing” when frustum AABB extrema jitter with camera movement/rotation.
  if (stabilize && shadow_map_size > 0) {
    float32_t span = radius * 2.0f;
    float32_t snap_units = span / (float32_t)shadow_map_size;
    Vec4 center_ls = mat4_mul_vec4(light_view, vec4_from_vec3(center, 1.0f));
    float32_t center_x = floorf(center_ls.x / snap_units + 0.5f) * snap_units;
    float32_t center_y = floorf(center_ls.y / snap_units + 0.5f) * snap_units;
    float32_t half = span * 0.5f;

    left = center_x - half;
    right = center_x + half;
    bottom = center_y - half;
    top = center_y + half;
    *out_world_units_per_texel = snap_units;
  } else {
    float32_t center_x = (ls_min.x + ls_max.x) * 0.5f;
    float32_t center_y = (ls_min.y + ls_max.y) * 0.5f;
    float32_t half = extent * 0.5f;
    left = center_x - half;
    right = center_x + half;
    bottom = center_y - half;
    top = center_y + half;
    *out_world_units_per_texel = extent / (float32_t)shadow_map_size;
  }

  // Orthographic projection for directional light
  Mat4 light_projection = mat4_ortho_vulkan(
      left, right,
      bottom, top,
      -ls_max.z, -ls_min.z  // View space looks down -Z; convert to + distances.
  );

  *out_view_projection = mat4_mul(light_projection, light_view);
}
```

---

## Shadow Render Pass

### Named Renderpass Creation

The Vulkan backend already implements the SHADOW domain renderpass, but CSM
needs a **named** renderpass so it can be referenced by shader configs and view
passes.

Create it once (e.g. in `vkr_shadow_system_init()`):

```c
VkrRenderPassHandle pass =
    vkr_renderer_renderpass_get(rf, string8_lit("Renderpass.CSM.Shadow"));
if (!pass) {
  VkrRenderPassConfig cfg = {
      .name = string8_lit("Renderpass.CSM.Shadow"),
      .clear_flags = VKR_RENDERPASS_CLEAR_DEPTH,
      .domain = VKR_PIPELINE_DOMAIN_SHADOW,
  };
  pass = vkr_renderer_renderpass_create(rf, &cfg);
}
system->shadow_renderpass = pass;
```

### Shadow Pipeline Configuration

Add `assets/shaders/shadow.shadercfg`:

```ini
version=1.0
name=shader.shadow
renderpass=Renderpass.CSM.Shadow
stages=vertex,fragment
stagefiles=assets/shaders/shadow.spv
use_instance=0
use_local=1

# Minimal vertex input for depth-only rendering.
attribute=vec3,in_position

# Global: per-cascade light VP (changes once per cascade pass).
uniform=mat4,0,light_view_projection

# Local: per-draw model matrix (push constants).
uniform=mat4,2,model

# Common shadow defaults:
cull_mode=front
```

### Shadow Vertex Shader

Create `lib/src/renderer/vulkan/shaders/shadow/cutout.slang`:

```hlsl
struct GlobalUniformBufferObject {
  column_major float4x4 light_view_projection;
};

[[vk::binding(0, 0)]]
ConstantBuffer<GlobalUniformBufferObject> g_ubo;

struct PushConstantsObject {
  column_major float4x4 model;
};

[[vk::push_constant]]
ConstantBuffer<PushConstantsObject> push_constants;

struct VertexInput {
  [[vk::location(0)]] float3 position : POSITION;
};

struct VertexOutput {
  float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
  VertexOutput output;

  float4 world_pos = mul(push_constants.model, float4(input.position, 1.0));
  output.position = mul(g_ubo.light_view_projection, world_pos);

  return output;
}

// Empty fragment shader (depth-only pass)
[shader("fragment")]
void fragmentMain() {
  // No color output needed - only depth is written
}
```

### Shadow Map Textures (Per Cascade, Per Frame)

To keep the first implementation small (no array textures, no atlas), allocate
one depth texture per cascade **per swapchain image**. This matches how other
offscreen passes (e.g. UI offscreen, picking) avoid “frame-in-flight” hazards.

This requires a small renderer API addition because the existing
`vkr_renderer_create_depth_attachment()` creates a depth image without
`SAMPLED` usage and without a sampler (so it cannot be sampled in the world
shader).

**Proposed new API (frontend):**
- `VkrTextureOpaqueHandle vkr_renderer_create_sampled_depth_attachment(VkrRendererFrontendHandle renderer, uint32_t width, uint32_t height, VkrRendererError *out_error);`

Backend behavior:
- Create a depth image with `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`.
- Create a sampler (at minimum: clamp-to-border, border=white, min/mag=nearest).

```c
bool8_t vkr_shadow_create_shadow_maps(VkrShadowSystem *system,
                                      RendererFrontend *rf) {
  uint32_t size = system->config.shadow_map_size;
  uint32_t cascades = system->config.cascade_count;
  uint32_t frames = vkr_renderer_window_attachment_count(rf);

  system->frame_resource_count = frames;
  system->frames = vkr_allocator_alloc(
      &rf->allocator, sizeof(VkrShadowFrameResources) * (uint64_t)frames,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!system->frames) {
    return false_v;
  }
  MemZero(system->frames, sizeof(VkrShadowFrameResources) * (uint64_t)frames);

  for (uint32_t f = 0; f < frames; ++f) {
    for (uint32_t c = 0; c < cascades; ++c) {
      VkrRendererError tex_err = VKR_RENDERER_ERROR_NONE;
      system->frames[f].shadow_maps[c] =
          vkr_renderer_create_sampled_depth_attachment(rf, size, size, &tex_err);
      if (!system->frames[f].shadow_maps[c]) {
        return false_v;
      }

      VkrTextureOpaqueHandle attachments[1] = {system->frames[f].shadow_maps[c]};
      VkrRenderTargetDesc rt_desc = {
          .sync_to_window_size = false_v,
          .attachment_count = 1,
          .attachments = attachments,
          .width = size,
          .height = size,
      };

      system->frames[f].shadow_targets[c] =
          vkr_renderer_render_target_create(rf, &rt_desc, system->shadow_renderpass);
      if (!system->frames[f].shadow_targets[c]) {
        return false_v;
      }
    }
  }

  return true_v;
}
```

---

## Shadow Sampling Shader

### Bindings and ShaderCFG Updates

The world shader uses **instance** textures (set 1). Adding 4 shadow maps means
`instance_texture_count` becomes `3 + 4 = 7`, so the sampler bindings must move
to `binding = 1 + instance_texture_count` and up.

Update `assets/shaders/default.world.shadercfg`:
- Add global uniforms:
  - `uniform=uint32,0,shadow_enabled`
  - `uniform=uint32,0,shadow_cascade_count`
  - `uniform=float,0,shadow_map_inv_size`
  - `uniform=float,0,shadow_pcf_radius`
  - `uniform=vec4,0,shadow_split_far`
  - `uniform=float,0,shadow_bias`
  - `uniform=float,0,shadow_normal_bias`
  - `uniform=uint32,0,shadow_debug_cascades`
  - `uniform=mat4[4],0,shadow_view_projection`
- Add instance samplers (append after existing 3):
  - `uniform=samp,1,shadow_map_0`
  - `uniform=samp,1,shadow_map_1`
  - `uniform=samp,1,shadow_map_2`
  - `uniform=samp,1,shadow_map_3`

Update `lib/src/renderer/vulkan/shaders/world/default.slang` to match the new binding layout:

```hlsl
// Existing textures (set 1)
[[vk::binding(1, 1)]] Texture2D<float4> diffuse_texture;
[[vk::binding(2, 1)]] Texture2D<float4> specular_texture;
[[vk::binding(3, 1)]] Texture2D<float4> normal_texture;

// New shadow maps (set 1, appended)
[[vk::binding(4, 1)]] Texture2D<float> shadow_map_0;
[[vk::binding(5, 1)]] Texture2D<float> shadow_map_1;
[[vk::binding(6, 1)]] Texture2D<float> shadow_map_2;
[[vk::binding(7, 1)]] Texture2D<float> shadow_map_3;

// Samplers: start at binding = 1 + instance_texture_count = 8
[[vk::binding(8, 1)]]  SamplerState diffuse_sampler;
[[vk::binding(9, 1)]]  SamplerState specular_sampler;
[[vk::binding(10, 1)]] SamplerState normal_sampler;
[[vk::binding(11, 1)]] SamplerState shadow_sampler_0;
[[vk::binding(12, 1)]] SamplerState shadow_sampler_1;
[[vk::binding(13, 1)]] SamplerState shadow_sampler_2;
[[vk::binding(14, 1)]] SamplerState shadow_sampler_3;
```

### PCF (Manual Compare, Depth Texture)

For simplicity and portability, use **manual depth compares** instead of a
comparison sampler (`SamplerComparisonState`). This only requires that the
shadow depth textures are created with `SAMPLED` usage and a sampler.

Add to `lib/src/renderer/vulkan/shaders/world/default.slang`:

```hlsl
uint select_cascade(float view_depth)
{
    uint count = max(g_ubo.shadow_cascade_count, 1u);
    count = min(count, 4u);

    // shadow_split_far packs far split distances for cascades 0..3
    float4 far_splits = g_ubo.shadow_split_far;

    if (count <= 1u) return 0u;
    if (view_depth < far_splits.x) return 0u;
    if (count == 2u) return 1u;
    if (view_depth < far_splits.y) return 1u;
    if (count == 3u) return 2u;
    if (view_depth < far_splits.z) return 2u;
    return min(count - 1u, 3u);
}

float sample_shadow_pcf(Texture2D<float> map, SamplerState samp,
                        float2 uv, float compare_depth, float bias)
{
    float2 inv_size = float2(g_ubo.shadow_map_inv_size, g_ubo.shadow_map_inv_size);
    int radius = (int)round(max(g_ubo.shadow_pcf_radius, 0.0f));
    radius = clamp(radius, 0, 2); // keep kernel small (0=hard,1=3x3,2=5x5)

    float sum = 0.0f;
    int taps = 0;

    [unroll]
    for (int y = -radius; y <= radius; ++y)
    {
        [unroll]
        for (int x = -radius; x <= radius; ++x)
        {
            float2 o = float2(x, y) * inv_size;
            float d = map.SampleLevel(samp, uv + o, 0).r;
            sum += (compare_depth - bias <= d) ? 1.0f : 0.0f;
            taps++;
        }
    }

    return (taps > 0) ? (sum / (float)taps) : 1.0f;
}

float calculate_shadow(float3 world_pos, float3 normal, float view_depth)
{
    if (g_ubo.shadow_enabled == 0u || g_ubo.dir_enabled == 0u)
    {
        return 1.0f;
    }

    uint cascade = select_cascade(view_depth);

    float4 clip = mul(g_ubo.shadow_view_projection[cascade], float4(world_pos, 1.0f));
    clip.xyz /= clip.w;

    float2 uv = clip.xy * 0.5f + 0.5f;
    float z = clip.z; // Vulkan depth in [0,1] for our projection helpers

    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || z < 0.0f || z > 1.0f)
    {
        return 1.0f;
    }

    // Normal-based bias using the same dir_direction used for lighting.
    float3 light_dir = normalize(-g_ubo.dir_direction);
    float ndotl = saturate(dot(normal, light_dir));
    float bias = g_ubo.shadow_bias + g_ubo.shadow_normal_bias * (1.0f - ndotl);

    // Dispatch to the correct bound texture/sampler.
    if (cascade == 0u) return sample_shadow_pcf(shadow_map_0, shadow_sampler_0, uv, z, bias);
    if (cascade == 1u) return sample_shadow_pcf(shadow_map_1, shadow_sampler_1, uv, z, bias);
    if (cascade == 2u) return sample_shadow_pcf(shadow_map_2, shadow_sampler_2, uv, z, bias);
    return sample_shadow_pcf(shadow_map_3, shadow_sampler_3, uv, z, bias);
}
```

### Directional Light Integration (Shadow Only Diffuse+Specular)

Modify `calculate_directional_light()` in `lib/src/renderer/vulkan/shaders/world/default.slang`
to accept `shadow` and apply it only to the non-ambient terms:

```hlsl
float4 calculate_directional_light(DirectionalLight light, float3 normal,
                                   float3 view_direction, float4 ambient_color,
                                   uint32_t render_mode, float4 diffuse_sample,
                                   float specular_mask, float shadow)
{
    float diffuse_factor = max(dot(normal, -light.direction), 0.0f);
    float3 half_direction = normalize(view_direction - light.direction);
    float specular_factor =
        pow(max(dot(half_direction, normal), 0.0f), local_ubo.shininess);

    float4 ambient =
        float4(ambient_color.rgb * local_ubo.diffuse_color.rgb, diffuse_sample.a);
    float4 diffuse =
        float4(light.color.rgb * diffuse_factor, diffuse_sample.a) * shadow;
    float4 specular =
        float4(light.color.rgb * specular_factor, diffuse_sample.a) * shadow;
    specular *= local_ubo.specular_color * specular_mask;

    if (render_mode == RENDER_MODE_DEFAULT)
    {
        ambient *= diffuse_sample;
        diffuse *= diffuse_sample;
        specular.a = diffuse_sample.a;
    }
    else
    {
        diffuse *= local_ubo.diffuse_color;
    }

    return ambient + diffuse + specular;
}
```

In `fragmentMain()`, compute `shadow` once (view-depth based) and pass it:

```hlsl
float4 view_pos = mul(g_ubo.view, float4(input.frag_position, 1.0));
float view_depth = -view_pos.z;
float shadow = calculate_shadow(input.frag_position, normal, view_depth);

color += calculate_directional_light(dir_light, normal, view_direction,
                                     input.ambient_color, input.render_mode,
                                     diffuse_sample, specular_sample, shadow);
```

---

## Shadow System API

### Public API

Add `lib/src/renderer/systems/vkr_shadow_system.h`:

```c
#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "memory/vkr_allocator.h"

struct s_RendererFrontend;
struct VkrCamera;

#define VKR_SHADOW_CASCADE_COUNT_MAX 4
#define VKR_SHADOW_MAP_SIZE_DEFAULT 1024

// Configuration
typedef struct VkrShadowConfig {
  uint32_t cascade_count;
  uint32_t shadow_map_size;
  float32_t cascade_split_lambda;
  float32_t max_shadow_distance;
  float32_t shadow_bias;
  float32_t normal_bias;
  float32_t pcf_radius;
  bool8_t stabilize_cascades;
  bool8_t debug_show_cascades;
} VkrShadowConfig;

#define VKR_SHADOW_CONFIG_DEFAULT ((VkrShadowConfig){ \
  .cascade_count = 3,                                 \
  .shadow_map_size = 1024,                            \
  .cascade_split_lambda = 0.75f,                      \
  .max_shadow_distance = 120.0f,                      \
  .shadow_bias = 0.001f,                              \
  .normal_bias = 0.01f,                               \
  .pcf_radius = 1.0f,                                 \
  .stabilize_cascades = true_v,                       \
  .debug_show_cascades = false_v,                     \
})

typedef struct VkrShadowFrameData {
  bool8_t enabled;
  uint32_t cascade_count; // 1..4
  float32_t shadow_map_inv_size;
  float32_t pcf_radius;
  float32_t shadow_bias;
  float32_t normal_bias;
  bool8_t debug_show_cascades;

  float32_t split_far[VKR_SHADOW_CASCADE_COUNT_MAX];
  Mat4 view_projection[VKR_SHADOW_CASCADE_COUNT_MAX];

  // Depth textures to bind for this frame (index = swapchain image index).
  VkrTextureOpaqueHandle shadow_maps[VKR_SHADOW_CASCADE_COUNT_MAX];
} VkrShadowFrameData;

// Forward declarations
typedef struct VkrShadowSystem VkrShadowSystem;

// Lifecycle
bool8_t vkr_shadow_system_init(VkrShadowSystem *system,
                                struct s_RendererFrontend *rf,
                                const VkrShadowConfig *config);
void vkr_shadow_system_shutdown(VkrShadowSystem *system);

// Per-frame update
void vkr_shadow_system_update(VkrShadowSystem *system,
                              const struct VkrCamera *camera,
                              bool8_t light_enabled,
                              Vec3 light_direction);

// Accessors for view integration
VkrRenderTargetHandle
vkr_shadow_system_get_render_target(const VkrShadowSystem *system,
                                    uint32_t frame_index,
                                    uint32_t cascade_index);

void vkr_shadow_system_get_frame_data(const VkrShadowSystem *system,
                                      uint32_t frame_index,
                                      VkrShadowFrameData *out_data);
```

---

## Integration Points

### Shadow View Layer

Create `lib/src/renderer/passes/vkr_pass_shadow.c`:

Key integration details with the render graph:
- The JSON graph repeats the shadow pass per cascade.
- Each pass owns a depth attachment slice of the layered shadow map.
- The pass executor forwards execution to the shadow system helper with the
  correct cascade index and render target.

```c
typedef struct VkrShadowSystemState {
  VkrShadowSystem shadow_system;
  uint64_t last_frame_updated;
} VkrShadowSystemState;

// One executor, multiple passes:
// - user_data == cascade_index
// - render graph begins/ends the renderpass around execute()
static void vkr_pass_shadow_execute(VkrRgPassContext *ctx, void *user_data) {
  RendererFrontend *rf = (RendererFrontend *)ctx->renderer;
  uint32_t cascade_index = (uint32_t)(uintptr_t)user_data;
  vkr_pass_shadow_execute(rf, ctx->image_index, ctx->delta_time, cascade_index,
                         ctx->render_target);

  // Shadow view handles per-cascade update + draw.
}
```

### World Layer Integration

Modify `lib/src/renderer/passes/vkr_pass_world.c`:

0. Query shadow data produced by the shadow system for the current frame index.
   Use the direct view API (no message layer):

   ```c
   // vkr_pass_world.c (inside render helper)
   VkrShadowFrameData shadow_data = {0};
   bool8_t have_shadow_data =
       vkr_shadow_system_get_frame_data(rf, image_index, &shadow_data);
   if (have_shadow_data) {
     // use shadow_data
   }
   ```

1. **Before the first world material global apply**, push CSM uniforms into the
   shader staging buffer (then `vkr_material_system_apply_global()` will upload
   everything):
   - `shadow_enabled`, `shadow_cascade_count`
   - `shadow_map_inv_size`, `shadow_pcf_radius`
   - `shadow_split_far`, `shadow_bias`, `shadow_normal_bias`, `shadow_debug_cascades`
   - `shadow_view_projection[4]`

2. **Bind per-frame shadow map textures** (for `info->image_index`) to the world
   shader instance sampler names (`shadow_map_0..3`). The least-invasive way is
   to extend `vkr_material_system_apply_instance()` (world path) to also bind
   the shadow maps using the `VkrShadowFrameData.shadow_maps[]` returned by the
   shadow layer.

3. Shadowing is applied only to the directional light term (ambient stays
   unshadowed).

---

## Implementation Notes (Common Pitfalls)

These are issues that are easy to miss in this codebase because shader resource
binding and descriptor updates are cached aggressively for performance.

- **Descriptor generation tracking**: Any texture that can be destroyed/recreated
  (scene reloads, hot reload, etc.) must have a unique `VkrTextureDescription.generation`
  **before** calling `vkr_renderer_create_texture()`. Descriptor updates compare the
  backend texture generation to decide whether to rewrite descriptors; a constant
  generation value can leave descriptor sets pointing at destroyed image views/samplers.
- **Transparency flagging**: Do not infer `VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT`
  from channel count alone. Many textures are uploaded as RGBA with an all-255 alpha;
  incorrectly flagging them as transparent pushes submeshes into the transparent path
  and can also force unnecessary alpha-tested shadow work. Detect transparency from
  actual alpha data during load.
- **Scene reload safety**: Shadow rendering is now packet-driven and does not
  rely on view-owned instance caches. Scene unload should still destroy textures
  and materials cleanly, but no shadow-specific invalidation hook is required.
- **Debug logging**: Avoid per-frame logging of shadow globals/splits; it can
  dominate frame time and hide real GPU costs. Prefer a debug toggle and on-screen UI.

## File Changes Summary

### New Files

| File | Purpose |
|------|---------|
| `lib/src/renderer/systems/vkr_shadow_system.h` | Shadow system public API |
| `lib/src/renderer/systems/vkr_shadow_system.c` | Shadow system implementation |
| `lib/src/renderer/passes/vkr_pass_shadow.h` | Shadow pass executor header |
| `lib/src/renderer/passes/vkr_pass_shadow.c` | Shadow pass executor |
| `lib/src/renderer/vulkan/shaders/shadow/cutout.slang` | Shadow pass shader |
| `assets/shaders/shadow.shadercfg` | Shadow shader configuration |

### Modified Files

| File | Changes |
|------|---------|
| `lib/src/renderer/vkr_renderer.h` | Add `vkr_renderer_create_sampled_depth_attachment()` |
| `lib/src/renderer/renderer_frontend.c` | Wire new sampled depth attachment API |
| `lib/src/renderer/vulkan/vulkan_backend.c` | Implement sampled depth attachment creation (usage+sampler) |
| `lib/src/renderer/renderer_frontend.h` | Store shadow system state pointer |
| `lib/src/renderer/systems/vkr_material_system.h` | Optionally add “bind shadow maps” hook for world instances |
| `lib/src/renderer/systems/vkr_material_system.c` | Bind `shadow_map_0..3` for world shader instances |
| `lib/src/renderer/vulkan/shaders/world/default.slang` | Add shadow sampling |
| `assets/shaders/default.world.shadercfg` | Add shadow map binding |
| `lib/src/math/mat.h` | Add `mat4_ortho_vulkan()` for light projection |

---

## Implementation Phases

### Phase 1: Shadow Infrastructure

1. Add `mat4_ortho_vulkan()` and cascade fitting helpers
2. Add `vkr_renderer_create_sampled_depth_attachment()` (sampled depth textures)
3. Create `Renderpass.CSM.Shadow` + `shader.shadow` pipeline
4. Add `Layer.Shadow` with `pass_count = cascade_count`

**Validation**: Shadow layer renders without errors and produces non-empty depth

### Phase 2: Shadow Sampling

1. Extend world shadercfg with CSM uniforms + `shadow_map_0..3` samplers
2. Update world shader bindings (sampler binding renumbering)
3. Implement hard shadow compare (no PCF)
4. Apply shadow only to directional diffuse+specular

**Validation**: Basic hard shadows visible in world

### Phase 3: Quality Improvements

1. Add small-kernel PCF (3x3)
2. Enable cascade stabilization (texel snapping)
3. Tune bias (constant + normal-based)
4. Debug cascade visualization toggle

**Validation**: Soft shadows, no obvious artifacts

### Phase 4: Optimization

1. Add per-cascade frustum culling
2. Optimize shadow pass (single pipeline bind)
3. Profile and tune cascade splits
4. Consider shadow map caching for static scenes

**Validation**: Acceptable performance with many meshes

---

## Debug Visualization

Add cascade coloring for debugging in fragment shader:

```hlsl
uint cascade = select_cascade(view_depth);

// Debug: color fragments by cascade
if (g_ubo.shadow_debug_cascades != 0u) {
  static const float4 cascade_colors[4] = {
    float4(1, 0, 0, 1),  // Red - cascade 0 (nearest)
    float4(0, 1, 0, 1),  // Green - cascade 1
    float4(0, 0, 1, 1),  // Blue - cascade 2
    float4(1, 1, 0, 1),  // Yellow - cascade 3 (farthest)
  };
  color.rgb = lerp(color.rgb, cascade_colors[cascade].rgb, 0.3);
}
```

---

## Test Plan

### Unit Tests

1. **Cascade split calculation**: Verify logarithmic/linear blend
2. **Frustum corner computation**: Test against known camera configurations
3. **Light matrix construction**: Verify orthographic bounds contain frustum

### Integration Tests

1. **Shadow rendering**: Verify shadow maps contain depth data
2. **Cascade selection**: Verify correct cascade used at different depths
3. **Shadow sampling**: Verify shadows appear on lit surfaces
4. **PCF quality**: Verify soft shadow edges
5. **Stabilization**: Verify minimal shimmer during camera movement

### Visual Tests

1. Move camera near/far to verify cascade transitions
2. Rotate light direction to verify shadow direction
3. Enable cascade debug coloring to verify coverage
4. Test with various mesh complexities

---

## Performance Considerations

- **Shadow map size**: 1024x1024 per cascade is a good default; 2048x2048 for higher-end quality
- **Cascade count**: 3 cascades is a good default; 4 if you need more far detail
- **PCF samples**: 9 (3x3) balances quality and performance
- **Culling**: Per-cascade culling reduces shadow pass vertex count
- **Static shadows**: Cache shadow maps for static lights/scenes

---

## References

- [Practical Split Scheme for Cascaded Shadow Maps](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus)
- [Vulkan Shadow Mapping Tutorial](https://www.saschawillems.de/blog/2016/08/14/vulkan-based-shadow-mapping/)
- [Microsoft CSM Documentation](https://docs.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps)
