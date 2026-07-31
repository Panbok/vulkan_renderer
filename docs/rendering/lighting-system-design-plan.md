---
status: partial
updated: 2026-07-31
authority: design
---
# Lighting System Design Plan (Scene + ECS + Picking)

## Implementation Status

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 0 | Shader system array support | **COMPLETED** |
| Phase 1 | ECS lights drive world shading | **COMPLETED** |
| Phase 2 | Light picking (proxy geometry) | **COMPLETED** |
| Phase 3 | Scene authoring (JSON loader) | **COMPLETED** |
| Phase 4 | Scale-up path (SSBO) | Not started |

---

## Purpose

Turn the current hardcoded lighting in `assets/shaders/default.world.slang` into
**ECS-driven light entities** that:

- Live in the Scene's ECS (`VkrWorld`), not in the renderer as ad-hoc globals.
- Affect world shading (Phong today).
- Can be **picked** (selected) as objects and mapped back to `VkrEntityId`.
- Are integrated into the Scene update flow described in `docs/scene/scene-system-design.md`.

This document is **LLM-consumable**: explicit file paths, concrete APIs, data
flow, and a phased checklist. It is a follow-up to the "basic scene" work and
assumes we have an active `VkrScene` with ECS, and a renderer-side bridge that
can consume ECS.

---

## Current State (Post-Implementation)

### Dynamic ECS-driven lights (IMPLEMENTED)

`assets/shaders/default.world.slang` now receives light data from the UBO:

- Directional light: `gUBO.dir_enabled`, `gUBO.dir_direction`, `gUBO.dir_color`
- Point lights: `gUBO.point_light_count`, `gUBO.point_light_data[48]` (packed vec4 array)

The static `const` light declarations have been removed. Lighting is computed
dynamically in `fragmentMain()` by looping through `point_light_count` lights.

### Lighting system (IMPLEMENTED)

`lib/src/renderer/systems/vkr_lighting_system.h/.c` provides:

- `VkrLightingSystem` struct with cached GPU-ready light data
- `vkr_lighting_system_sync_from_scene()` - queries ECS for light entities
- `vkr_lighting_system_apply_uniforms()` - sets shader uniforms
- Dirty tracking for efficient per-frame updates
- Integrated into `RendererFrontend` and called from `vkr_pass_world.c`

### Light picking (IMPLEMENTED)

`lib/src/renderer/systems/vkr_picking_system.c` now includes:

- `vkr_picking_render_light_gizmos()` - renders 0.25 unit cubes at light positions
- Light gizmo cube geometry created on init
- Picking uses `VKR_PICKING_ID_KIND_SCENE` with entity's `SceneRenderId`
- `vkr_scene_handle_entity_from_picking_id()` handles lookup

### Light gizmo visualization (IMPLEMENTED)

`lib/src/renderer/passes/vkr_pass_world.c` now renders point light
gizmos in the world pass:

- Sphere geometry (`vkr_geometry_system_create_sphere`) with per-light instance
  states to avoid color bleeding.
- Depth-tested (world pipeline), so gizmos are occluded by geometry.
- Uses `VKR_RENDER_MODE_UNLIT` to render emissive-only colors for accurate tint.

### Scene authoring (IMPLEMENTED)

`lib/src/renderer/resources/loaders/scene_loader.c` now parses light components:

- `point_light` and `directional_light` objects are supported per entity.
- Defaults match current engine expectations (enabled, color/intensity, attenuation).
- `assets/scenes/default.scene.json` includes authored lights.

### Light component helpers (IMPLEMENTED)

`lib/src/renderer/systems/vkr_scene_system.h/.c` provides:

- `vkr_scene_set_point_light()` - adds point light with automatic render ID
- `vkr_scene_get_point_light()` - retrieves point light component
- `vkr_scene_set_directional_light()` / `vkr_scene_get_directional_light()`

### Shader system array support (IMPLEMENTED)

`lib/src/renderer/resources/loaders/shader_loader.c` now supports:

```
uniform=type[count],scope,name
```

- `VkrShaderUniformDesc.array_count` field added
- Layout computation uses 16-byte element stride for arrays

### Important constraint: our UBO packing is not std140

The shadercfg loader computes offsets/sizes with a **16-byte register packing**
rule (see `vkr_apply_uniform_register_packing()` in
`lib/src/renderer/resources/loaders/shader_loader.c`). The comments in some
`.shadercfg` files call this “std140”, but the implementation is closer to
HLSL/Slang constant-buffer packing:

- Small scalars/vectors can share a 16-byte register row.
- Arrays should treat each element as consuming whole 16-byte registers
  (i.e. element stride is typically 16 bytes, or `align_up(element_size, 16)`).

Design implications:
- Avoid relying on std140-only offset math in docs/implementations.
- For v1, prefer **packed `vec4` arrays** for light data to keep layout simple
  and unambiguous under this packing model.

### Render mode extensions (IMPLEMENTED)

- Added `VKR_RENDER_MODE_UNLIT` to `VkrRenderMode` (C enum + shader constant).
- Used for unlit gizmo rendering so light colors are not affected by lighting.

---

## Goals / Non-goals

### Goals

1. **ECS light entities**:
   - A light is an entity with components (e.g. `SceneTransform` + `ScenePointLight`).
2. **Replace shader constants**:
   - Remove `static const ...` lights from `assets/shaders/default.world.slang`.
3. **Picking support**:
   - Lights render into `Renderpass.Builtin.Picking` with stable object IDs and
     map back to the owning `VkrEntityId`.
4. **Scene integration**:
   - Lights update order is defined relative to scene transform evaluation and
     rendering.
5. **Array-based uniform support**:
   - Extend shadercfg to support arrays (v1 uses packed `vec4[]`; v2 can move to `PointLight[]`).

### Non-goals (deferred unless explicitly needed later)

- Expanding picking beyond existing mesh/text support (except adding light gizmos).
- Shadow mapping.
- Clustered/tiled/forward+ lighting.
- PBR shading (we will extend `VkrMaterial` later; not part of this plan).

---

## High-level Architecture

### Ownership boundaries (important)

- **Scene** owns entity/component state only (`VkrWorld`).
- **Renderer systems** consume ECS state and update GPU resources.
- Scene code must not directly "own" GPU buffers/pipelines.

Concrete modules:

- Scene-level components live with the scene system (initially in scene module).
- Renderer-level lighting sync lives in a dedicated system:
  - `lib/src/renderer/systems/vkr_lighting_system.h/.c` (extend existing stubs)

Picking integration is implemented as a renderer concern (because it draws):

- Extend `lib/src/renderer/systems/vkr_picking_system.h/.c` to accept additional
  "pick draw" providers (meshes + lights).

---

## ECS Data Model (Scene Components)

These are registered into the scene's `VkrWorld`.

### `SceneDirectionalLight`

Used for a single (optional) sun-like light. World direction is computed as
`rotation * direction_local` from the entity's transform.

```c
typedef struct SceneDirectionalLight {
  Vec3 color;            // linear RGB
  float32_t intensity;
  Vec3 direction_local;  // local-space direction (default: {0, -1, 0})
  bool8_t enabled;
} SceneDirectionalLight;
```

**Direction computation**:
- Entity has `SceneTransform` with rotation quaternion.
- World direction = `quat_rotate(transform.rotation, direction_local)`.
- If entity has no transform, use `direction_local` directly.

### `ScenePointLight`

Matches the current Phong attenuation model already present in the shader.

```c
typedef struct ScenePointLight {
  Vec3 color;             // linear RGB
  float32_t intensity;
  float32_t constant;
  float32_t linear;
  float32_t quadratic;
  bool8_t enabled;
} ScenePointLight;
```

**Position**: Derived from entity's `SceneTransform.world` translation.

### `SceneRenderId` (existing)

Already defined in `vkr_scene_system.h`. Used for picking. Light entities
should have this component assigned via `vkr_scene_ensure_render_id()`.

---

## GPU / Shader Data Model (Phong v1)

### Shader system array support (prerequisite)

To avoid repetitive `point_light_0`, `point_light_1` fields, we extend the
shadercfg format to support arrays:

**New shadercfg format**:
```
uniform=type[count],scope,name
```

**Examples**:
```
uniform=vec4[16],0,point_light_positions
uniform=vec4[16],0,point_light_colors
```

**Implementation changes** (Phase 0, minimal v1 scope):

1. **shader_loader.c**: Update `vkr_parse_uniform_line()` to parse `type[count]`:
   - Detect `[` in type token
   - Extract base type and count
   - Store count in new `VkrShaderUniformDesc.array_count` field (default 1)

2. **shader_loader.c**: Update `vkr_compute_uniform_layout()` to size arrays using
   the engine’s 16-byte register packing:
   - `element_size = vkr_uniform_type_size(base_type)`
   - `element_stride = align_up(element_size, VKR_SHADER_UNIFORM_REGISTER_SIZE)`
   - `uniform.size = element_stride * array_count`
   - Arrays should start on a register boundary (equivalent to aligning to 16).

3. (Optional) **vkr_shader_system.c**: Add an element setter for convenience:
   ```c
   bool8_t vkr_shader_system_uniform_set_element(VkrShaderSystem *state,
                                                  const char *uniform_name,
                                                  uint32_t index,
                                                  const void *value);
   ```

In v1, this element setter is not strictly required if the engine packs the
entire array on the CPU and calls `vkr_shader_system_uniform_set()` once for the
whole array uniform (recommended for `point_light_data`).

### Packed `vec4` arrays (recommended for v1)

If full struct-array support (and/or struct layout rules) is too invasive for
v1, pack light data into `vec4` arrays with an explicit, engine-owned layout:

```c
// In shader:
float4 point_light_data[MAX_POINT_LIGHTS * 3];  // 3 vec4s per light
// [i*3+0] = {position.xyz, constant}
// [i*3+1] = {color.rgb, linear}
// [i*3+2] = {intensity, quadratic, 0, 0}
```

In shadercfg:
```
uniform=vec4[48],0,point_light_data
uniform=uint32,0,point_light_count
```

This avoids struct support but requires manual packing/unpacking.

### GPU struct layout (reference only)

This is provided as a conceptual reference. Do not assume these offsets match
our current shadercfg layout logic unless/until we explicitly implement std140
struct layout for uniform blocks.

```c
// PointLight GPU representation (std140)
struct PointLight {
    float3 position;     // offset 0,  size 12, padded to 16
    float constant;      // offset 12, size 4
    float3 color;        // offset 16, size 12, padded to 16
    float linear;        // offset 28, size 4
    float intensity;     // offset 32, size 4
    float quadratic;     // offset 36, size 4
    float _pad[2];       // offset 40, size 8 (pad to 48 = 16*3)
};  // Total: 48 bytes per light
```

### Shader changes (replace static const lights)

Update `assets/shaders/default.world.slang`:

1. Remove the `static const` light declarations.
2. Extend `GlobalUniformBufferObject` to include:
   - Directional light fields (enabled, direction, color*intensity)
   - Point light array via packed vec4s or struct array
   - `point_light_count` for loop bounds

Example shape (packed approach):

```c
struct GlobalUniformBufferObject {
    column_major float4x4 projection;
    column_major float4x4 view;
    float4 ambient_color;
    float3 view_position;
    uint32_t render_mode;

    // Directional light
    uint32_t dir_enabled;
    float3 dir_direction;    // world space, normalized
    float4 dir_color;        // rgb = color * intensity, a = 1

    // Point lights (packed vec4 array)
    uint32_t point_light_count;
    float4 point_light_data[MAX_POINT_LIGHTS * 3];
};
```

Shader logic becomes:

```c
// Directional
if (gUBO.dir_enabled != 0) {
    result += calculate_directional_light_from_ubo(...);
}

// Point lights
for (uint i = 0; i < gUBO.point_light_count; i++) {
    float4 d0 = gUBO.point_light_data[i * 3 + 0];
    float4 d1 = gUBO.point_light_data[i * 3 + 1];
    float4 d2 = gUBO.point_light_data[i * 3 + 2];
    // Unpack and compute...
}
```

### Shader config changes

Update `assets/shaders/default.world.shadercfg` to match UBO layout order:

```
# Existing globals
uniform=mat4,0,projection
uniform=mat4,0,view
uniform=vec4,0,ambient_color
uniform=vec3,0,view_position
uniform=uint32,0,render_mode

# Directional light
uniform=uint32,0,dir_enabled
uniform=vec3,0,dir_direction
uniform=vec4,0,dir_color

# Point lights
uniform=uint32,0,point_light_count
uniform=vec4[48],0,point_light_data
```

Notes:
- `48` is `VKR_MAX_POINT_LIGHTS * 3` for the packed representation (3 `vec4`s
  per light). The shadercfg format is not expression-aware, so this is written
  as a literal.
- When implementing, update any “Computed Layout” comments in the shadercfg
  file to avoid drifting documentation (the current default world shadercfg
  layout comments are already out of date).

---

## Renderer Lighting System (ECS → UBO)

### Extend existing files

- `lib/src/renderer/systems/vkr_lighting_system.h` (extend)
- `lib/src/renderer/systems/vkr_lighting_system.c` (implement)

### Updated API

```c
#define VKR_MAX_POINT_LIGHTS 16

typedef struct VkrLightingSystem {
  // Cached GPU-ready data (updated from ECS each frame)
  struct {
    bool8_t enabled;
    Vec3 direction;     // world space
    Vec3 color;
    float32_t intensity;
  } directional;

  struct {
    Vec3 position;      // world space
    Vec3 color;
    float32_t intensity;
    float32_t constant;
    float32_t linear;
    float32_t quadratic;
  } point_lights[VKR_MAX_POINT_LIGHTS];
  uint32_t point_light_count;

  // Dirty tracking
  bool8_t dirty;
} VkrLightingSystem;

// Initialize/shutdown
bool8_t vkr_lighting_system_init(VkrLightingSystem *system);
void vkr_lighting_system_shutdown(VkrLightingSystem *system);

// Sync from ECS (call before rendering)
void vkr_lighting_system_sync_from_scene(VkrLightingSystem *system,
                                          const VkrScene *scene);

// Apply to shader uniforms (call when the target shader is bound/current)
void vkr_lighting_system_apply_uniforms(VkrLightingSystem *system);

// Mark dirty (call when lights change outside normal sync)
void vkr_lighting_system_mark_dirty(VkrLightingSystem *system);

// Check if any updates pending
bool8_t vkr_lighting_system_is_dirty(const VkrLightingSystem *system);
```

Recommended cleanup (to avoid two sources of truth):
- The existing manual APIs (`vkr_lighting_system_add_point_light()`, etc.) are
  redundant once ECS is the source of truth. Prefer removing or deprecating
  them during implementation, keeping `sync_from_scene/apply_uniforms` as the
  primary API.
- The ECS component types (`SceneDirectionalLight`, `ScenePointLight`) currently
  live in `lib/src/renderer/systems/vkr_lighting_system.h`. Move these into the
  scene module (e.g. `lib/src/renderer/systems/vkr_scene_system.h`) so the scene
  owns component definitions and the renderer only consumes them.

### Responsibilities

1. Query the active scene's ECS for light entities:
   - `(SceneDirectionalLight, SceneTransform?)`
   - `(ScenePointLight, SceneTransform)`
2. Transform local directions/positions to world space using `SceneTransform.world`.
3. Choose which lights are "active" when exceeding slot limits.
4. Pack data into GPU-ready format.
5. Apply uniforms when shader is bound.

### Slot selection policy (when too many lights exist)

When more than `VKR_MAX_POINT_LIGHTS` exist, select deterministically:

1. Filter to only enabled lights.
2. Sort by `SceneRenderId.id` (ascending) for stable ordering.
3. Take first N lights.

This avoids flicker when entity iteration order changes.

For v2, consider distance-based or importance-based selection.

### Integration point

In `lib/src/renderer/passes/vkr_pass_world.c`:

```c
void vkr_pass_world_execute(...) {
    // Before any mesh rendering
    vkr_lighting_system_sync_from_scene(&rf->lighting_system, scene);

    // Shader is bound for mesh rendering...
    // After shader bind, before first draw:
    if (vkr_lighting_system_is_dirty(&rf->lighting_system)) {
        vkr_lighting_system_apply_uniforms(&rf->lighting_system);
    }

    // ... rest of rendering
}
```

Note: The call sites should match the actual `vkr_lighting_system_apply_uniforms()`
signature. Prefer storing a `VkrShaderSystem*` inside `VkrLightingSystem` (as the
current stub does) and keeping `apply_uniforms()` parameterless.

---

## Picking Integration (Lights as pickable objects)

### Problem

`lib/src/renderer/systems/vkr_picking_system.c` only renders meshes, so light
entities are not pickable.

### Approach (v1: proxy geometry in picking pass)

Render a small proxy mesh (cube or billboard) at each pickable light's
world position into the picking target:

- **Geometry**: Unit cube via `vkr_geometry_system_create_cube()`.
- **Model matrix**: `translate(light_position) * scale(gizmo_size)`
- **Pipeline**: Existing picking pipeline (`assets/shaders/picking.slang`)
- **object_id**: passed via picking shader push constant (`PushConstants.object_id`)

### Required changes

1. **Light entities need SceneRenderId**:
   - Call `vkr_scene_ensure_render_id()` when adding light component.

2. **Picking system needs light provider**:
   Add callback or iterate lights after meshes in `vkr_picking_render()`:

   ```c
   // After mesh rendering in picking pass...
   vkr_picking_render_light_gizmos(rf, ctx, scene);
   ```

3. **Entity lookup uses existing infrastructure**:
   - `vkr_scene_handle_entity_from_picking_id()` already maps render_id → entity.
   - No new mapping table needed.

Picking kind decision (important):
- For v1, encode light gizmos using `VKR_PICKING_ID_KIND_SCENE` with the entity’s
  `SceneRenderId.id` to reuse `vkr_scene_handle_entity_from_picking_id()` as-is.
- `VKR_PICKING_ID_KIND_LIGHT` can remain reserved for future cases where the
  payload is not a `SceneRenderId` (e.g. non-entity light handles).

### Gizmo rendering helper

```c
void vkr_picking_render_light_gizmos(RendererFrontend *rf,
                                      VkrPickingContext *ctx,
                                      const VkrScene *scene) {
    // Query scene for (ScenePointLight, SceneTransform, SceneRenderId)
    // For each light:
    //   1. Get world position from transform
    //   2. Build model matrix with gizmo scale
    //   3. Set push constants: {model, object_id, alpha_cutoff=0}
    //   4. Draw cube geometry
}
```

### Async readback safety

The picking system already handles async readback via
`vkr_renderer_request_pixel_readback()`. Because `SceneRenderId.id` is never
reused (monotonic allocator in scene), there's no stale-pick hazard.

If render_id recycling is added later, use generation encoding or delay
reuse until no pick is pending (`vkr_picking_is_pending()`).

---

## Scene Integration and Update Order

To make light transforms correct, ordering must be explicit:

1. `vkr_scene_update(scene, dt)` updates `SceneTransform.world`.
2. `vkr_lighting_system_sync_from_scene(system, scene)` reads `SceneTransform.world`.
3. `vkr_pass_world_execute(...)` binds shader and applies lighting uniforms.
4. Picking pass renders proxy meshes for light entities using `SceneRenderId`.

If the engine moves scene updates into renderer internals later, preserve this
ordering.

---

## Implementation Phases (Recommended)

### Phase 0 — Shader system array support (prerequisite) ✅ COMPLETED

**Goal**: Enable `uniform=type[count],scope,name` in shadercfg.

**Files modified**:
- `lib/src/renderer/resources/loaders/shader_loader.c`
  - Updated `vkr_parse_uniform_line()` to detect `[count]` suffix
  - Updated `vkr_compute_uniform_layout()` to size/align array uniforms with
    16-byte element stride
- `lib/src/renderer/resources/vkr_resources.h`
  - Added `uint32_t array_count` to `VkrShaderUniformDesc`

**Acceptance criteria**: ✅ All met
- Can declare `uniform=vec4[48],0,point_light_data` in shadercfg
- Layout computation produces correct offsets for arrays (16-byte element stride)

### Phase 1 — ECS lights drive world shading (no picking yet) ✅ COMPLETED

**Goal**: Lights are entities; shader receives light data from ECS.

**Implementation summary**:

1. **ECS components registered** in `vkr_scene_system.h/.c`:
   - `SceneDirectionalLight`, `ScenePointLight` component types
   - `comp_directional_light`, `comp_point_light` type IDs in `VkrScene`
   - Compiled queries: `query_directional_light`, `query_point_lights`

2. **Lighting system sync** in `vkr_lighting_system.c`:
   - `vkr_lighting_system_sync_from_scene()` queries ECS via chunk callbacks
   - Transforms positions/directions to world space using `SceneTransform.world`
   - Takes first N enabled lights when count exceeds `VKR_MAX_POINT_LIGHTS`

3. **Uniform application** in `vkr_lighting_system.c`:
   - `vkr_lighting_system_apply_uniforms()` packs light data into vec4 arrays
   - Sets `point_light_data` as single bulk write (3 vec4s per light)

4. **Shader + shadercfg updated**:
   - `assets/shaders/default.world.slang`: Removed static lights, added UBO fields
   - `assets/shaders/default.world.shadercfg`: Added light uniforms

5. **Per-frame wiring** in `vkr_pass_world.c`:
   - `vkr_lighting_system_sync_from_scene()` called at start of render
   - `vkr_lighting_system_apply_uniforms()` called after global material state

6. **Frontend integration** in `renderer_frontend.h/.c`:
   - Added `VkrLightingSystem lighting_system` to `RendererFrontend`
   - Added `VkrScene *active_scene` for lighting sync
   - Init/shutdown calls in renderer lifecycle

**Acceptance criteria**: ✅ All met
- Can create light entities in scene
- Moving a point light entity changes world lighting
- Shader no longer has hardcoded lights

### Phase 2 — Light picking (proxy geometry) ✅ COMPLETED

**Goal**: Clicking on light gizmo returns entity ID.

**Implementation summary**:

1. **Render IDs for lights**:
   - `vkr_scene_set_point_light()` automatically calls `vkr_scene_ensure_render_id()`
   - Light entities are pickable via `SceneRenderId` component

2. **Light gizmo rendering** in `vkr_picking_system.c`:
   - Added `VkrGeometryHandle light_gizmo_cube` to `VkrPickingContext`
   - Created 1.0 unit cube on init, scaled to 0.25 units at render time
   - `vkr_picking_render_light_gizmos()` iterates `query_point_lights`
   - Encodes picking ID using `VKR_PICKING_ID_KIND_SCENE`
   - Called in `vkr_picking_render()` after gizmo rendering, before text

3. **Scene helpers** in `vkr_scene_system.h/.c`:
   - `vkr_scene_set_point_light()` / `vkr_scene_get_point_light()`
   - `vkr_scene_set_directional_light()` / `vkr_scene_get_directional_light()`

**Acceptance criteria**: ✅ All met
- Clicking light gizmo returns correct entity ID via existing mapping
- Multiple lights distinguishable via picking
- Gizmo size is 0.25 world units (configurable via `VKR_LIGHT_GIZMO_SIZE`)

### Phase 3 — Scene authoring (JSON loader) ✅ COMPLETED

**Goal**: Author lights in `assets/scenes/*.scene.json` and load them via the
existing scene JSON loader (`lib/src/renderer/resources/loaders/scene_loader.c/.h`).

#### 3.1 Scene JSON schema extension (additive)

Extend the per-entity schema with optional light component objects (matching the
existing `mesh`, `text3d`, `shape` pattern):

```json
{
  "name": "MyLight",
  "parent": 0,
  "transform": {
    "pos": [0.0, 1.0, -10.0],
    "rot": [0.0, 0.0, 0.0, 1.0],
    "scale": [1.0, 1.0, 1.0]
  },
  "point_light": {
    "enabled": true,
    "color": [1.0, 0.9, 0.8],
    "intensity": 1.0,
    "attenuation": {
      "constant": 1.0,
      "linear": 0.35,
      "quadratic": 0.44
    }
  }
}
```

Directional light uses the entity rotation to define world direction:

```json
{
  "name": "Sun",
  "parent": 0,
  "transform": {
    "pos": [0.0, 0.0, 0.0],
    "rot": [0.0, 0.7071068, 0.0, 0.7071068],
    "scale": [1.0, 1.0, 1.0]
  },
  "directional_light": {
    "enabled": true,
    "color": [1.0, 1.0, 1.0],
    "intensity": 1.0
  }
}
```

Schema rules/invariants (implemented):
- `point_light` position is taken from `transform.pos` (world-space after parent
  evaluation).
- `directional_light` direction is computed by the lighting system as
  `quat_rotate(transform.rotation, direction_local)`; loader defaults
  `direction_local = [0, -1, 0]` unless overridden.
- Multiple directional lights are allowed in the scene file, but the runtime
  lighting system selects a single directional light deterministically (smallest
  `SceneRenderId` when present).
- This change is additive; keep `"version": 2` in `.scene.json` files (no format
  break).

#### 3.2 Loader integration (implemented)

Update `lib/src/renderer/resources/loaders/scene_loader.c`:

- Extend `SceneEntityImport`:
  - `bool8_t has_point_light; ScenePointLightImport point_light;`
  - `bool8_t has_directional_light; SceneDirectionalLightImport directional_light;`
- Add parsing helpers (mirroring the existing `text3d`/`shape` parsing style):
  - `scene_json_parse_point_light(...)` reads:
    - `enabled` (default `true`)
    - `color` as vec3 (default `[1,1,1]`)
    - `intensity` (default `1.0`)
    - `attenuation` object (defaults: `constant=1.0, linear=0.35, quadratic=0.44`)
  - `scene_json_parse_directional_light(...)` reads:
    - `enabled` (default `true`)
    - `color` as vec3 (default `[1,1,1]`)
    - `intensity` (default `1.0`)
    - optional `direction_local` vec3 (default `[0,-1,0]`, normalized)

Update `vkr_scene_load_from_json()` load steps (implemented):

1. Create entities + set `SceneName`, `SceneTransform`, parent links (existing).
2. Load meshes/text/shapes (existing).
3. Apply lights (new):
   - If `has_point_light`: call `vkr_scene_set_point_light(scene, entity, &light)`
     (this already ensures `SceneRenderId` so picking works immediately).
   - If `has_directional_light`: call `vkr_scene_set_directional_light(scene, entity, &light)`.

Update `lib/src/renderer/resources/loaders/scene_loader.h`:
- Extend `VkrSceneLoadResult` with `directional_light_count`, `point_light_count`.

#### 3.3 Default scene update (authoring example)

Update `assets/scenes/default.scene.json` to include:
- One `directional_light` entity (sun)
- 1–2 `point_light` entities positioned near the existing meshes

This replaces “magic defaults” and makes lighting reproducible across reloads.

#### 3.4 Acceptance criteria

**Acceptance criteria**: ✅ All met
- Loading a scene with `point_light` entities affects world shading immediately.
- Loaded `point_light` entities are pickable (picking gizmo cube maps back to
  the correct `VkrEntityId`).
- Loader tolerates missing/invalid fields without crashing (consistent with
  existing mesh/text/shape parsing behavior).

### Phase 4 — Scale-up path (deferred)

When fixed-slot UBO becomes limiting:

- Move to SSBO with truly dynamic light count
- Consider light culling (frustum, distance-based)
- Keep `SceneRenderId`/picking mapping unchanged

---

## File Changes Summary

**Modified** (Phases 0-3):

- `lib/src/renderer/resources/loaders/shader_loader.c` — array parsing (`type[count]` syntax)
- `lib/src/renderer/resources/vkr_resources.h` — added `array_count` field to `VkrShaderUniformDesc`
- `lib/src/renderer/systems/vkr_lighting_system.c` — full implementation of sync + apply
- `lib/src/renderer/systems/vkr_lighting_system.h` — updated API, removed duplicate component types
- `lib/src/renderer/passes/vkr_pass_world.c` — lighting sync/apply calls
- `lib/src/renderer/systems/vkr_picking_system.c` — light gizmo rendering, cube geometry
- `lib/src/renderer/systems/vkr_picking_system.h` — `light_gizmo_cube` field, `vkr_picking_render_light_gizmos()` API
- `lib/src/renderer/systems/vkr_scene_system.c` — light component registration, setter/getter functions
- `lib/src/renderer/systems/vkr_scene_system.h` — light component types, compiled queries, helper APIs
- `lib/src/renderer/renderer_frontend.c` — lighting system init/shutdown
- `lib/src/renderer/renderer_frontend.h` — `VkrLightingSystem` and `active_scene` fields
- `assets/shaders/default.world.slang` — removed static lights, added UBO fields for dynamic lights
- `assets/shaders/default.world.shadercfg` — added light uniforms
- `lib/src/renderer/passes/vkr_pass_world.c` — light gizmo visualization (spheres, depth-tested)
- `lib/src/renderer/systems/vkr_geometry_system.c` — UV sphere generator
- `lib/src/renderer/systems/vkr_geometry_system.h` — sphere API
- `lib/src/renderer/vkr_renderer.h` — `VKR_RENDER_MODE_UNLIT`

**New** (none - built on existing stubs)

**Implemented** (Phase 3):

- `lib/src/renderer/resources/loaders/scene_loader.c` — parse `point_light`/`directional_light`, apply via `vkr_scene_set_*_light()`
- `lib/src/renderer/resources/loaders/scene_loader.h` — extend `VkrSceneLoadResult` with light counts
- `assets/scenes/default.scene.json` — add authored light entities

---

## Validation / Test Plan

Minimal "fast confidence" checks:

1. **Visual**: Moving a point light entity changes lighting in-world without shader edits.
2. **Determinism**: With >N point lights, the chosen active lights do not flicker when creation order is stable.
3. **Picking**: Clicking a light gizmo returns its `VkrEntityId` via `SceneRenderId` mapping.
4. **Async safety**: Repeatedly click + destroy/create lights does not produce stale picks.
5. **Array uniforms**: Setting elements of a vec4 array works correctly.
6. **Scene load**: Loading `assets/scenes/default.scene.json` produces the expected lights without post-load scripting.

Unit-test candidates (if we add tests in `tests/src/` later):

- Shader loader parses `type[count]` format correctly
- Array uniform layout matches shadercfg register packing rules (including array stride)
- Slot selection ordering is deterministic
- World direction computation from transform rotation

---

## Open Questions / Decisions Made

1. **Packed vec4 vs struct arrays**: ✅ **DECIDED: Packed vec4**
   - Implemented with packed vec4 arrays (3 vec4s per light)
   - Layout: `[i*3+0]={pos.xyz,constant}`, `[i*3+1]={color*intensity,linear}`, `[i*3+2]={intensity,quadratic,0,0}`
   - Struct arrays can be considered for v2 if cleaner API is needed.

2. **Light gizmo appearance**: ✅ **DECIDED: Sphere (visual) + cube (picking)**
   - World view renders depth-tested spheres tinted to light color.
   - Picking pass keeps a unit cube for stable selection behavior.

3. **Gizmo scale**: ✅ **DECIDED: Fixed world-space**
   - Picking: `VKR_LIGHT_GIZMO_SIZE` (0.25 units) in `vkr_picking_system.c`
   - Visualization: `VKR_VIEW_WORLD_LIGHT_GIZMO_SIZE` (0.5 units) in
     `vkr_pass_world.c`
   - Screen-space sizing can be added later if needed.

4. **Directional light gizmo**: ✅ **DECIDED: Not pickable in v1**
   - Directional lights can be edited via properties panel.
   - Can add infinite-distance gizmo rendering in future editor work.

5. **Scene authoring format (Phase 3)**: ✅ **DECIDED/IMPLEMENTED**
   - `point_light` and `directional_light` objects under `entities[]` (mirrors `mesh`/`text3d`/`shape`).
   - Scene `"version": 2` preserved since this is additive.
