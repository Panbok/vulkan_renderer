---
status: implemented
updated: 2026-09-04
authority: design
---
# Editor Transform Gizmo Design Document

## Purpose

Implement an **editor transform gizmo** for interactive manipulation of 3D objects (meshes, lights, cameras, etc.) in the scene. The gizmo provides visual handles for translation, rotation, and scaling operations, following industry-standard conventions (similar to Blender, Unity, Unreal Engine).

This document is **LLM-consumable**: explicit file paths, concrete APIs, data structures, shader code, and a phased implementation plan.

---

## Current State

### Existing Infrastructure

#### Gizmo Model
The gizmo is generated procedurally using geometry helpers in
`lib/src/renderer/systems/vkr_geometry_system.c`:
- Translation arrows, rotation rings, and scale cubes are built from cylinders,
  cones, boxes, and tori (`vkr_geometry_system_create_arrow`,
  `vkr_geometry_system_create_torus`, `vkr_geometry_system_create_box`).
- Axis colors come from built-in gizmo materials created in
  `lib/src/renderer/systems/vkr_material_system.c`:
  `gizmo_axis_x`, `gizmo_axis_y`, `gizmo_axis_z`.
- Submesh order is fixed (arrows X/Y/Z, rings X/Y/Z, cubes X/Y/Z) and used to
  map submesh indices to gizmo handles for picking/highlighting.

#### Picking System (`lib/src/renderer/systems/vkr_picking_system.h`)
- GPU-based pixel-perfect picking with R32_UINT render target
- Async readback via ring buffer architecture
- ID encoding with kind tags (`VkrPickingIdKind`)
- Coordinate mapping from window to render target

#### Picking ID Encoding (`lib/src/renderer/systems/vkr_picking_ids.h`)
```c
typedef enum VkrPickingIdKind {
  VKR_PICKING_ID_KIND_SCENE = 0,      // Scene entities (meshes)
  VKR_PICKING_ID_KIND_UI_TEXT = 1,    // UI text elements
  VKR_PICKING_ID_KIND_WORLD_TEXT = 2, // World-space text
  VKR_PICKING_ID_KIND_LIGHT = 3,      // Light icons
  VKR_PICKING_ID_KIND_GIZMO = 4,      // Gizmo handles
} VkrPickingIdKind;
```

#### Scene System (`lib/src/renderer/systems/vkr_scene_system.h`)
- ECS-based entity management via `VkrWorld`
- Transform component with position/rotation/scale
- Dirty tracking for efficient updates
- Picking result to entity mapping

#### Input System (`lib/src/core/input.h`)
- Mouse position and delta tracking
- Button state (just pressed, held, just released)
- Keyboard modifiers (Shift, Ctrl, Alt)

#### Camera System (`lib/src/renderer/systems/vkr_camera.h`)
- View and projection matrices
- Camera position and orientation
- Near/far clip planes for ray construction

---

## Goals

1. **Combined gizmo**: Translation arrows, rotation rings, and scale cubes
2. **Axis-constrained translation/rotation**: Handle-driven X/Y/Z operations
3. **Uniform scale**: Any cube performs uniform scaling (no per-axis scale)
4. **Screen-space size**: Consistent on-screen size across camera distances
5. **Visual feedback**: Hover/active handle highlighting
6. **Mouse-only workflow**: No keyboard shortcuts required
7. **Single selection**: Operate on one selected entity at a time

### Non-Goals (Initial Implementation)

- Plane handles or plane-constraint shortcuts
- Per-axis scaling
- Keyboard shortcuts (mode/axis locks, cancel/confirm)
- Multi-selection transforms and pivot customization
- Undo/redo integration
- Snapping to grid/objects (future enhancement)
- Gizmo size preferences UI
- Touch/gamepad input

---

## Architecture Overview

### System Components

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                      Transform Gizmo Architecture                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                      VkrGizmoSystem                                  │    │
│  │  - Owns gizmo mesh and materials                                    │    │
│  │  - Manages active mode (translate/rotate/scale)                     │    │
│  │  - Tracks interaction state (idle/hovering/dragging)                │    │
│  │  - Computes gizmo transform (position at selection center)          │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                    │                                         │
│              ┌─────────────────────┼─────────────────────┐                   │
│              ▼                     ▼                     ▼                   │
│  ┌───────────────────┐ ┌───────────────────┐ ┌───────────────────┐          │
│  │  Gizmo Picking    │ │  Gizmo Rendering  │ │  Transform Logic  │          │
│  │  - Handle IDs     │ │  - Axis colors    │ │  - Ray-plane      │          │
│  │  - Hit detection  │ │  - Highlighting   │ │    intersection   │          │
│  │  - Priority over  │ │  - Screen-space   │ │  - Delta calc     │          │
│  │    scene objects  │ │    size           │ │  - Apply to scene │          │
│  └───────────────────┘ └───────────────────┘ └───────────────────┘          │
│                                                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                     Input State Machine                              │    │
│  │  IDLE ──(hover)──► HOVERING ──(click)──► DRAGGING ──(release)──► IDLE│    │
│  │    │                   │                     │                        │    │
│  │    └───(click scene)───┴──(Escape)───────────┘                        │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Data Flow

```
1. User selects object(s) via picking →
2. Gizmo system activates, positions at selection center →
3. Mouse hover triggers gizmo picking →
4. Handle hit → highlight axis →
5. Mouse down + drag → compute ray-plane intersection →
6. Calculate transform delta →
7. Apply delta to selected entities →
8. Mouse up → finalize transform (push to undo stack)
```

---

## Data Structures

### Gizmo Handle Identification

`VkrPickingIdKind` in `lib/src/renderer/systems/vkr_picking_ids.h` includes a
gizmo kind for handle picking:

```c
typedef enum VkrPickingIdKind {
  VKR_PICKING_ID_KIND_SCENE = 0,
  VKR_PICKING_ID_KIND_UI_TEXT = 1,
  VKR_PICKING_ID_KIND_WORLD_TEXT = 2,
  VKR_PICKING_ID_KIND_LIGHT = 3,
  VKR_PICKING_ID_KIND_GIZMO = 4,  // NEW: Gizmo handles
} VkrPickingIdKind;
```

Picking IDs use 3 kind bits to leave room for gizmo handles:

```c
// vkr_picking_ids.h
#define VKR_PICKING_ID_KIND_BITS 3u
#define VKR_PICKING_ID_KIND_SHIFT (32u - VKR_PICKING_ID_KIND_BITS)
#define VKR_PICKING_ID_KIND_MASK                                             \
  (((1u << VKR_PICKING_ID_KIND_BITS) - 1u) << VKR_PICKING_ID_KIND_SHIFT)
```

This reduces the payload bit budget by 1 (from 30 to 29 bits), which is still
more than enough for runtime IDs.

### Gizmo Handle Enum

```c
typedef enum VkrGizmoHandle {
  VKR_GIZMO_HANDLE_NONE = 0,

  // Translation handles
  VKR_GIZMO_HANDLE_TRANSLATE_X = 1,
  VKR_GIZMO_HANDLE_TRANSLATE_Y = 2,
  VKR_GIZMO_HANDLE_TRANSLATE_Z = 3,
  VKR_GIZMO_HANDLE_TRANSLATE_FREE = 4, // Reserved (no geometry in v1)

  // Rotation handles
  VKR_GIZMO_HANDLE_ROTATE_X = 5,
  VKR_GIZMO_HANDLE_ROTATE_Y = 6,
  VKR_GIZMO_HANDLE_ROTATE_Z = 7,

  // Scale handles
  VKR_GIZMO_HANDLE_SCALE_X = 8,
  VKR_GIZMO_HANDLE_SCALE_Y = 9,
  VKR_GIZMO_HANDLE_SCALE_Z = 10,
  VKR_GIZMO_HANDLE_SCALE_UNIFORM = 11, // Reserved (no center handle)
} VkrGizmoHandle;
```

Uniform scaling is handled by the axis cubes themselves (all scale handles map
to uniform scale in UX), so the reserved `SCALE_UNIFORM` handle is unused.

### Submesh-to-Handle Mapping

The procedural gizmo mesh uses a fixed submesh order so picking can map by
index instead of inspecting geometry bounds:

1. `TRANSLATE_X`, `TRANSLATE_Y`, `TRANSLATE_Z`
2. `ROTATE_X`, `ROTATE_Y`, `ROTATE_Z`
3. `SCALE_X`, `SCALE_Y`, `SCALE_Z`

### Gizmo Mode

```c
typedef enum VkrGizmoMode {
  VKR_GIZMO_MODE_NONE = 0,      // Gizmo hidden
  VKR_GIZMO_MODE_TRANSLATE = 1, // Translation mode
  VKR_GIZMO_MODE_ROTATE = 2,    // Rotation mode
  VKR_GIZMO_MODE_SCALE = 3,     // Scale mode
} VkrGizmoMode;
```

Mode is derived from the picked handle during drag (no explicit mode UI in v1).

### Gizmo Orientation Space

```c
typedef enum VkrGizmoSpace {
  VKR_GIZMO_SPACE_WORLD = 0,  // World-aligned axes
  VKR_GIZMO_SPACE_LOCAL = 1,  // Object-aligned axes
  VKR_GIZMO_SPACE_VIEW = 2,   // Camera-aligned axes
} VkrGizmoSpace;
```

### Gizmo Interaction State

```c
typedef enum VkrGizmoState {
  VKR_GIZMO_STATE_HIDDEN = 0,   // No selection, gizmo not shown
  VKR_GIZMO_STATE_IDLE = 1,     // Visible, no interaction
  VKR_GIZMO_STATE_HOVERING = 2, // Mouse over a handle
  VKR_GIZMO_STATE_DRAGGING = 3, // Actively transforming
} VkrGizmoState;
```

Current implementation tracks interaction state in the application layer
instead of `VkrGizmoSystem`.

### Gizmo Configuration

```c
typedef struct VkrGizmoConfig {
  float32_t screen_size;        // Size in screen pixels (default: 150)
} VkrGizmoConfig;

#define VKR_GIZMO_CONFIG_DEFAULT ((VkrGizmoConfig){ \
  .screen_size = 150.0f,                            \
})
```

### Gizmo System State

```c
typedef struct VkrGizmoSystem {
  VkrGizmoConfig config;
  VkrGizmoMode mode;
  VkrGizmoSpace space;
  VkrEntityId selected_entity;
  Vec3 position;
  VkrQuat orientation;
  VkrGizmoHandle hot_handle;
  VkrGizmoHandle active_handle;

  uint32_t gizmo_mesh_index;
  VkrPipelineHandle pipeline;

  bool8_t initialized;
  bool8_t visible;
} VkrGizmoSystem;
```

Interaction state (drag start, axis, pending pick) currently lives in the
editor/application layer instead of the gizmo system.

---

## Picking Integration

### Gizmo Picking IDs

Encode gizmo handles into picking IDs:

```c
// Encode gizmo handle into picking object_id
static inline uint32_t vkr_gizmo_encode_picking_id(VkrGizmoHandle handle) {
  return vkr_picking_encode_id(VKR_PICKING_ID_KIND_GIZMO, (uint32_t)handle);
}

// Decode picking object_id to gizmo handle
static inline VkrGizmoHandle vkr_gizmo_decode_picking_id(uint32_t object_id) {
  VkrPickingDecodedId decoded = vkr_picking_decode_id(object_id);
  if (!decoded.valid || decoded.kind != VKR_PICKING_ID_KIND_GIZMO) {
    return VKR_GIZMO_HANDLE_NONE;
  }
  return (VkrGizmoHandle)decoded.value;
}
```

### Picking Priority

Gizmo handles must be picked before scene objects. Implementation options:

**Option A: Single Picking Pass, Draw Gizmo Last (Recommended)**
Render the gizmo into the existing picking target *after* the scene with
depth-test disabled. Since the gizmo is rendered “on top” in the main view, this
matches user expectations and naturally gives it priority in picking when
overlapping scene geometry.

**Option B: Separate Picking Pass**
```
1. Render gizmo to picking target (front-to-back, no depth test)
2. If gizmo hit → return gizmo handle
3. Else render scene to picking target
4. Return scene entity or nothing
```

**Option C: Combined Pass with Depth Bias**
```
- Render gizmo with negative depth bias
- Gizmo fragments win depth test when overlapping scene
```

### Hover Picking (Highlighting) Without Re-Rendering the Whole Scene

The current picking system is **request-driven** and (for scene selection) it
renders the whole scene into the picking target. Doing that every frame for
hover highlighting is expensive.

Preferred approach for gizmo hover:

1. Reuse the existing picking render target, but do a **gizmo-only picking draw**
   when the mouse moves and the gizmo is visible.
2. Set a **1×1 scissor rect** at the requested pixel.
3. Clear **only that 1×1 region** of the picking color attachment to 0.
4. Draw the gizmo picking geometry with the same 1×1 scissor rect.
5. Read back that one pixel (async is fine; accept 1-frame latency for
   highlight).

This keeps hover feedback cheap: only a handful of triangles are rasterized, and
only one pixel is written and read back.

### Gizmo Picking Render

```c
void vkr_gizmo_render_picking(VkrGizmoSystem *system,
                               RendererFrontend *rf,
                               VkrPickingContext *picking_ctx) {
  if (!system->visible) return;

  // Compute gizmo model matrix
  const VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &rf->camera_system, rf->active_camera);
  Mat4 model = vkr_gizmo_build_model(system, camera, picking_ctx->height);

  // Render each submesh with its encoded handle ID
  VkrMesh *mesh = vkr_mesh_manager_get(&rf->mesh_manager, system->gizmo_mesh_index);
  uint32_t submesh_count = mesh ? vkr_mesh_manager_submesh_count(mesh) : 0;
  for (uint32_t i = 0; i < submesh_count; ++i) {
    VkrSubMesh *submesh =
        vkr_mesh_manager_get_submesh(&rf->mesh_manager, system->gizmo_mesh_index, i);
    if (!submesh) {
      continue;
    }

    VkrGizmoHandle handle = vkr_gizmo_handle_from_submesh(i);
    if (handle == VKR_GIZMO_HANDLE_NONE) {
      continue;
    }
    uint32_t object_id = vkr_gizmo_encode_picking_id(handle);

    // Set push constants
    VkrLocalMaterialState local = {
      .model = model,
      .object_id = object_id,
    };
    vkr_material_system_apply_local(&rf->material_system, &local);

    // Draw geometry (assumes picking pipeline already bound by caller)
    vkr_geometry_system_render(rf, &rf->geometry_system, submesh->geometry, 1);
  }
}
```

**Geometry caveat:** procedural sizing is based on fixed constants (arrow length,
ring radius, cube size). If you change these proportions, keep the arrow tip,
ring radius, and cube offsets consistent so handle picking stays intuitive and
submesh ordering matches the handle mapping.

---

## Ray-Plane Intersection

### Ray Construction from Mouse

```c
typedef struct VkrRay {
  Vec3 origin;
  Vec3 direction;  // Normalized
} VkrRay;

VkrRay vkr_camera_screen_to_ray(const VkrCamera *camera,
                                 float32_t screen_x,
                                 float32_t screen_y,
                                 uint32_t viewport_width,
                                 uint32_t viewport_height) {
  // Convert screen coords to NDC [-1, 1]
  float32_t ndc_x = (2.0f * screen_x / (float32_t)viewport_width) - 1.0f;
  float32_t ndc_y = 1.0f - (2.0f * screen_y / (float32_t)viewport_height);  // Y inverted

  // Clip space points (near and far)
  Vec4 clip_near = vec4_new(ndc_x, ndc_y, 0.0f, 1.0f);  // Vulkan: near = 0
  Vec4 clip_far = vec4_new(ndc_x, ndc_y, 1.0f, 1.0f);   // Vulkan: far = 1

  // Inverse view-projection
  Mat4 vp = mat4_mul(camera->projection, camera->view);
  Mat4 inv_vp = mat4_inverse(vp);

  // Unproject to world space
  Vec4 world_near = mat4_mul_vec4(inv_vp, clip_near);
  Vec4 world_far = mat4_mul_vec4(inv_vp, clip_far);

  world_near = vec4_div_scalar(world_near, world_near.w);
  world_far = vec4_div_scalar(world_far, world_far.w);

  VkrRay ray;
  ray.origin = vec3_from_vec4(world_near);
  ray.direction = vec3_normalize(vec3_sub(vec3_from_vec4(world_far),
                                           vec3_from_vec4(world_near)));
  return ray;
}
```

### Ray-Plane Intersection

```c
typedef struct VkrPlane {
  Vec3 normal;    // Plane normal (normalized)
  Vec3 point;     // Point on plane
} VkrPlane;

bool8_t vkr_ray_plane_intersect(const VkrRay *ray,
                                 const VkrPlane *plane,
                                 float32_t *out_t,
                                 Vec3 *out_point) {
  float32_t denom = vec3_dot(plane->normal, ray->direction);

  // Ray parallel to plane
  if (vkr_abs_f32(denom) < 1e-6f) {
    return false_v;
  }

  Vec3 to_plane = vec3_sub(plane->point, ray->origin);
  float32_t t = vec3_dot(to_plane, plane->normal) / denom;

  // Intersection behind ray origin
  if (t < 0.0f) {
    return false_v;
  }

  if (out_t) *out_t = t;
  if (out_point) {
    *out_point = vec3_add(ray->origin, vec3_mul_scalar(ray->direction, t));
  }
  return true_v;
}
```

### Ray-Axis Closest Point

For axis-constrained translation, find closest point on axis to ray:

```c
bool8_t vkr_ray_axis_closest_point(const VkrRay *ray,
                                    Vec3 axis_origin,
                                    Vec3 axis_direction,
                                    Vec3 *out_point_on_axis,
                                    Vec3 *out_point_on_ray) {
  // Solve: axis_origin + s * axis_direction closest to ray_origin + t * ray_direction
  Vec3 w = vec3_sub(axis_origin, ray->origin);
  float32_t a = vec3_dot(axis_direction, axis_direction);  // = 1 if normalized
  float32_t b = vec3_dot(axis_direction, ray->direction);
  float32_t c = vec3_dot(ray->direction, ray->direction);  // = 1 if normalized
  float32_t d = vec3_dot(axis_direction, w);
  float32_t e = vec3_dot(ray->direction, w);

  float32_t denom = a * c - b * b;
  if (vkr_abs_f32(denom) < 1e-6f) {
    // Lines are parallel
    return false_v;
  }

  float32_t s = (b * e - c * d) / denom;
  float32_t t = (a * e - b * d) / denom;

  if (out_point_on_axis) {
    *out_point_on_axis = vec3_add(axis_origin, vec3_mul_scalar(axis_direction, s));
  }
  if (out_point_on_ray) {
    *out_point_on_ray = vec3_add(ray->origin, vec3_mul_scalar(ray->direction, t));
  }
  return true_v;
}
```

---

## Transform Operations

### Translation

```c
void vkr_gizmo_compute_translation_delta(VkrGizmoSystem *system,
                                          const VkrRay *current_ray,
                                          Vec3 *out_delta) {
  Vec3 current_point;

  switch (system->active_handle) {
    case VKR_GIZMO_HANDLE_TRANSLATE_X:
    case VKR_GIZMO_HANDLE_TRANSLATE_Y:
    case VKR_GIZMO_HANDLE_TRANSLATE_Z: {
      // Axis-constrained translation is numerically unstable when the view ray
      // is nearly parallel to the axis. Prefer a drag plane constructed at
      // drag-start that faces the camera but contains the axis, then project
      // the resulting world delta onto the axis.
      VkrPlane plane = {
        .normal = system->drag_plane_normal,
        .point = system->drag_plane_origin,
      };
      if (!vkr_ray_plane_intersect(current_ray, &plane, NULL, &current_point)) {
        *out_delta = vec3_zero();
        return;
      }
      Vec3 plane_delta = vec3_sub(current_point, system->drag_start_world);
      float32_t axis_dist = vec3_dot(plane_delta, system->drag_axis);
      current_point = vec3_add(system->drag_start_world,
                                vec3_mul_scalar(system->drag_axis, axis_dist));
      break;
    }

    case VKR_GIZMO_HANDLE_TRANSLATE_FREE: {
      // Screen-plane translation (reserved for future handle)
      VkrPlane plane = {
        .normal = system->drag_plane_normal,
        .point = system->drag_plane_origin,
      };
      if (!vkr_ray_plane_intersect(current_ray, &plane, NULL, &current_point)) {
        *out_delta = vec3_zero();
        return;
      }
      break;
    }

    default:
      *out_delta = vec3_zero();
      return;
  }

  *out_delta = vec3_sub(current_point, system->drag_start_world);
}
```

**Drag plane construction (recommended):**
- For axis drag, build `drag_plane_normal` so the plane contains the axis and
  is as “screen-facing” as possible, using the camera forward direction.
- If `axis` is nearly parallel to camera forward, fall back to camera right/up
  to avoid degeneracy.

### Rotation

```c
void vkr_gizmo_compute_rotation_delta(VkrGizmoSystem *system,
                                       const VkrRay *current_ray,
                                       const VkrRay *previous_ray,
                                       VkrQuat *out_delta) {
  // Intersect both rays with rotation plane
  VkrPlane plane = {
    .normal = system->drag_axis,  // Rotation axis is plane normal
    .point = system->position,
  };

  Vec3 current_point, previous_point;
  if (!vkr_ray_plane_intersect(current_ray, &plane, NULL, &current_point) ||
      !vkr_ray_plane_intersect(previous_ray, &plane, NULL, &previous_point)) {
    *out_delta = vkr_quat_identity();
    return;
  }

  // Vectors from gizmo center to intersection points
  Vec3 prev_vec = vec3_sub(previous_point, system->position);
  Vec3 curr_vec = vec3_sub(current_point, system->position);
  if (vec3_length(prev_vec) < 1e-6f || vec3_length(curr_vec) < 1e-6f) {
    *out_delta = vkr_quat_identity();
    return;
  }

  Vec3 v_prev = vec3_normalize(prev_vec);
  Vec3 v_curr = vec3_normalize(curr_vec);

  // Angle between vectors
  float32_t dot = vkr_clamp_f32(vec3_dot(v_prev, v_curr), -1.0f, 1.0f);
  float32_t angle = acosf(dot);

  // Determine rotation direction via cross product
  Vec3 cross = vec3_cross(v_prev, v_curr);
  if (vec3_dot(cross, system->drag_axis) < 0.0f) {
    angle = -angle;
  }

  *out_delta = vkr_quat_from_axis_angle(system->drag_axis, angle);
}
```

### Scale

```c
void vkr_gizmo_compute_scale_delta(VkrGizmoSystem *system,
                                    const VkrRay *current_ray,
                                    float32_t *out_delta_x,
                                    float32_t *out_delta_y,
                                    float32_t *out_delta_z) {
  *out_delta_x = *out_delta_y = *out_delta_z = 1.0f;  // Default: no scale change

  Vec3 current_point;

  // Uniform scale: use distance from gizmo center (all cubes behave uniformly).
  VkrPlane plane = {
    .normal = system->drag_plane_normal,
    .point = system->position,
  };
  if (!vkr_ray_plane_intersect(current_ray, &plane, NULL, &current_point)) {
    return;
  }

  float32_t start_dist = vec3_length(vec3_sub(system->drag_start_world,
                                               system->position));
  float32_t current_dist = vec3_length(vec3_sub(current_point, system->position));

  if (start_dist > 1e-6f) {
    float32_t scale_factor = current_dist / start_dist;
    scale_factor = vkr_max_f32(scale_factor, 1e-3f);
    *out_delta_x = *out_delta_y = *out_delta_z = scale_factor;
  }
}
```

---

## Gizmo Rendering

### Screen-Space Size Calculation

Keep gizmo the same apparent size regardless of distance:

```c
float32_t vkr_gizmo_compute_screen_scale(const VkrGizmoSystem *system,
                                          const VkrCamera *camera,
                                          uint32_t viewport_height) {
  // Distance from camera to gizmo
  float32_t distance = vec3_length(vec3_sub(system->position, camera->position));

  // Desired screen size in world units
  // For perspective: scale proportional to distance
  if (camera->type == VKR_CAMERA_TYPE_PERSPECTIVE) {
    float32_t fov_rad = camera->zoom * (VKR_PI / 180.0f);
    float32_t world_size_per_pixel = (2.0f * distance * tanf(fov_rad * 0.5f))
                                     / (float32_t)viewport_height;
    return system->config.screen_size * world_size_per_pixel;
  } else {
    // Orthographic: fixed world size
    float32_t ortho_height = camera->top_clip - camera->bottom_clip;
    float32_t world_size_per_pixel = ortho_height / (float32_t)viewport_height;
    return system->config.screen_size * world_size_per_pixel;
  }
}

Mat4 vkr_gizmo_build_model(const VkrGizmoSystem *system,
                            const VkrCamera *camera,
                            uint32_t viewport_height) {
  float32_t scale = vkr_gizmo_compute_screen_scale(system, camera, viewport_height);

  Mat4 translation = mat4_translate(system->position);
  Mat4 rotation = vkr_quat_to_mat4(system->orientation);
  Mat4 scale_mat = mat4_scale(vec3_new(scale, scale, scale));

  return mat4_mul(mat4_mul(translation, rotation), scale_mat);
}
```

### Gizmo Shader (Optional)

Create `assets/shaders/gizmo.slang`:

```hlsl
struct GlobalUBO {
  column_major float4x4 projection;
  column_major float4x4 view;
};

struct PushConstants {
  column_major float4x4 model;
  float4 axis_color;      // RGB + alpha
  float4 highlight_color; // RGB + highlight_factor (0-1)
};

[[vk::binding(0, 0)]]
ConstantBuffer<GlobalUBO> g_ubo;

[[vk::push_constant]]
ConstantBuffer<PushConstants> push;

struct VertexInput {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(1)]] float3 normal : NORMAL;
};

struct VertexOutput {
  float4 position : SV_Position;
  float3 normal : NORMAL;
  float3 world_pos : WORLD_POS;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
  VertexOutput output;

  float4 world_pos = mul(push.model, float4(input.position, 1.0));
  output.world_pos = world_pos.xyz;

  float4 view_pos = mul(g_ubo.view, world_pos);
  output.position = mul(g_ubo.projection, view_pos);

  // Transform normal (assuming uniform scale)
  output.normal = normalize(mul((float3x3)push.model, input.normal));

  return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
  // Simple lighting for 3D appearance
  float3 light_dir = normalize(float3(0.5, 1.0, 0.3));
  float ndotl = max(dot(input.normal, light_dir), 0.0);

  // Combine axis color with highlight
  float3 base_color = push.axis_color.rgb;
  float3 highlight = push.highlight_color.rgb;
  float highlight_factor = push.highlight_color.a;

  float3 color = lerp(base_color, highlight, highlight_factor);

  // Apply simple diffuse lighting
  color *= (0.3 + 0.7 * ndotl);

  return float4(color, push.axis_color.a);
}
```

### Optional Gizmo Pipeline Configuration

Current implementation reuses the existing `world_overlay` (fallback `world`)
pipeline and material emission for coloring. A dedicated gizmo shader remains a
future optimization; the config below is optional.

Create `assets/shaders/gizmo.shadercfg` (project shadercfg format):

```ini
version=1.0
name=shader.gizmo
renderpass=Renderpass.Builtin.World
stages=vertex,fragment
stagefiles=assets/shaders/gizmo.spv
use_instance=0
use_local=1

# Match the standard mesh layout for compatibility with existing geometry.
attribute=vec3,in_position
attribute=vec3,in_normal
attribute=vec2,in_texcoord
attribute=vec4,in_color
attribute=vec4,in_tangent

uniform=mat4,0,projection
uniform=mat4,0,view

uniform=mat4,2,model
uniform=vec4,2,axis_color
uniform=vec4,2,highlight_color
```

---

## Input Handling

### Handle-Driven Modes (Current UX)

- **Selection → gizmo appears.**
- The gizmo is **combined**: translation arrows, rotation rings, and scale
  cubes are always visible.
- Clicking a handle implicitly selects the operation (no explicit mode UI).
- Keyboard shortcuts are not required or implemented in the current workflow.

### Mouse Handling

- On mouse move with no buttons: request a pick at the cursor to update the
  hot (hovered) handle.
- On left click: request a pick. If the gizmo is hit, begin a drag using the
  handle’s implied mode; otherwise update selection.
- During drag: update the transform via ray-plane intersection (translation and
  uniform scale) or axis-plane intersection (rotation).
- On mouse release: clear the active handle and end the drag.

---

## Scene Integration

Current implementation supports single-selection only. Multi-selection and
orbiting pivot behavior below are future design notes.

### Multiple Selection Pivot Semantics (Clarify Early)

The system tracks a single gizmo `position` (pivot). For multi-selection, the
expected “editor” behavior is usually:

- **Translate:** all entities shift by the same world-space delta.
- **Rotate around pivot:** entities orbit around the pivot and also rotate.
- **Scale around pivot:** entities move relative to pivot and also scale.

If rotation/scale is implemented as “rotate/scale each object in place”, then
multi-selection will *not* preserve formation (no orbit). That is a valid choice
but must be documented as an explicit behavior.

Recommended v1 behavior:
- Treat **world-space pivot** as `system->position` (selection center).
- Support **Local-space rotation/scale only for single selection** (multi-local
  becomes ill-defined).

### Apply Transform to Entities

```c
void vkr_gizmo_apply_transform_to_selection(VkrGizmoSystem *system,
                                             VkrScene *scene) {
  for (uint32_t i = 0; i < system->selected_count; ++i) {
    VkrEntityId entity = system->selected_entities[i];
    SceneTransform *transform = vkr_scene_get_transform(scene, entity);
    if (!transform) continue;

    switch (system->mode) {
      case VKR_GIZMO_MODE_TRANSLATE: {
        Vec3 new_pos = vec3_add(system->initial_transforms[i].position,
                                 system->accumulated_translation);
        vkr_scene_set_position(scene, entity, new_pos);
        break;
      }

      case VKR_GIZMO_MODE_ROTATE: {
        // Rotation around pivot:
        // - Update rotation
        // - Update position to orbit around pivot (for multi-selection)
        VkrQuat delta = system->accumulated_rotation;

        VkrQuat new_rot =
            (system->space == VKR_GIZMO_SPACE_LOCAL)
                ? vkr_quat_mul(system->initial_transforms[i].rotation, delta)
                : vkr_quat_mul(delta, system->initial_transforms[i].rotation);
        vkr_scene_set_rotation(scene, entity, new_rot);

        Vec3 pivot = system->position;
        Vec3 offset = vec3_sub(system->initial_transforms[i].position, pivot);
        Vec3 rotated_offset = vkr_quat_rotate_vec3(delta, offset);
        vkr_scene_set_position(scene, entity, vec3_add(pivot, rotated_offset));
        break;
      }

      case VKR_GIZMO_MODE_SCALE: {
        Vec3 scale_delta = system->accumulated_scale;
        Vec3 new_scale = vec3_mul(system->initial_transforms[i].scale, scale_delta);
        vkr_scene_set_scale(scene, entity, new_scale);

        Vec3 pivot = system->position;
        Vec3 offset = vec3_sub(system->initial_transforms[i].position, pivot);
        // For v1: interpret scale_delta in world axes (component-wise).
        // Local-space scaling around pivot requires a basis and is out of scope.
        Vec3 scaled_offset = vec3_mul(offset, scale_delta);
        vkr_scene_set_position(scene, entity, vec3_add(pivot, scaled_offset));
        break;
      }

      default:
        break;
    }
  }
}
```

### Selection Management

```c
void vkr_gizmo_set_selection(VkrGizmoSystem *system,
                              VkrScene *scene,
                              const VkrEntityId *entities,
                              uint32_t count) {
  // Cancel any active transform
  if (system->state == VKR_GIZMO_STATE_DRAGGING) {
    vkr_gizmo_cancel_transform(system);
  }

  // Copy selection
  if (count > 0) {
    // Ensure capacity
    if (count > system->initial_transform_capacity) {
      // Reallocate arrays...
    }

    MemCopy(system->selected_entities, entities, sizeof(VkrEntityId) * count);
    system->selected_count = count;

    // Compute gizmo position (selection center)
    Vec3 center = vec3_zero();
    uint32_t valid_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
      SceneTransform *t = vkr_scene_get_transform(scene, entities[i]);
      if (t) {
        center = vec3_add(center, t->position);
        valid_count++;
      }
    }
    if (valid_count > 0) {
      system->position = vec3_div_scalar(center, (float32_t)valid_count);
    }

    system->visible = true_v;
    system->state = VKR_GIZMO_STATE_IDLE;
  } else {
    system->selected_count = 0;
    system->visible = false_v;
    system->state = VKR_GIZMO_STATE_HIDDEN;
  }
}
```

### Text3D Pivot Alignment

World-space text uses a quad whose origin is at the lower-left of the text
plane. For gizmo interaction, the pivot should be centered on the text plane so
the gizmo appears aligned with the visible text. The scene-side `SceneText3D`
stores `world_width/world_height` (derived from text texture size) and the
application uses that to offset the gizmo pivot by
`(world_width * 0.5f, world_height * 0.5f, 0.0f)` in local space.

---

## Gizmo System API

### Public Interface

Create `lib/src/renderer/systems/vkr_gizmo_system.h`:

```c
#pragma once

#include "core/vkr_entity.h"
#include "defines.h"
#include "math/vec.h"
#include "math/vkr_quat.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend;
struct VkrCamera;

// Forward declarations for types defined above
typedef struct VkrGizmoSystem VkrGizmoSystem;
typedef struct VkrGizmoConfig VkrGizmoConfig;

// Lifecycle
bool8_t vkr_gizmo_system_init(VkrGizmoSystem *system,
                               struct s_RendererFrontend *rf,
                               const VkrGizmoConfig *config);
void vkr_gizmo_system_shutdown(VkrGizmoSystem *system,
                                struct s_RendererFrontend *rf);

// Targeting
void vkr_gizmo_system_set_target(VkrGizmoSystem *system,
                                 VkrEntityId entity,
                                 Vec3 position,
                                 VkrQuat orientation);
void vkr_gizmo_system_clear_target(VkrGizmoSystem *system);

// Highlighting
void vkr_gizmo_system_set_hot_handle(VkrGizmoSystem *system,
                                     VkrGizmoHandle handle);
void vkr_gizmo_system_set_active_handle(VkrGizmoSystem *system,
                                        VkrGizmoHandle handle);

// Rendering
void vkr_gizmo_system_render(VkrGizmoSystem *system,
                             struct s_RendererFrontend *rf,
                             const struct VkrCamera *camera,
                             uint32_t viewport_height,
                             VkrPipelineHandle pipeline_override);
void vkr_gizmo_system_render_picking(VkrGizmoSystem *system,
                                     struct s_RendererFrontend *rf,
                                     const struct VkrCamera *camera,
                                     uint32_t viewport_height);
```

Drag state and selection live in the application/editor layer rather than the
gizmo system API.

---

## File Structure

### New Files

| File | Purpose |
|------|---------|
| `lib/src/renderer/systems/vkr_gizmo_system.h` | Gizmo system public API |
| `lib/src/renderer/systems/vkr_gizmo_system.c` | Gizmo system implementation |

Optional future additions:

| File | Purpose |
|------|---------|
| `lib/src/math/vkr_ray.h` | Ray type and ray-intersection utilities |
| `lib/src/math/vkr_ray.c` | Ray intersection implementations |
| `assets/shaders/gizmo.slang` | Gizmo rendering shader |
| `assets/shaders/gizmo.shadercfg` | Gizmo shader configuration |
| `assets/shaders/gizmo_picking/world.slang` | Gizmo picking shader |
| `assets/shaders/gizmo_picking.shadercfg` | Gizmo picking shader config |

### Modified Files

| File | Changes |
|------|---------|
| `lib/src/renderer/systems/vkr_picking_ids.h` | Add `VKR_PICKING_ID_KIND_GIZMO` |
| `lib/src/renderer/systems/vkr_picking_system.c` | Gizmo picking priority handling |
| `lib/src/renderer/renderer_frontend.h` | Add `VkrGizmoSystem` member |
| `lib/src/renderer/renderer_frontend.c` | Initialize/shutdown gizmo system |
| `lib/src/renderer/systems/vkr_geometry_system.h` | Add procedural gizmo shape APIs |
| `lib/src/renderer/systems/vkr_geometry_system.c` | Implement procedural shapes |
| `lib/src/renderer/passes/vkr_pass_editor.c` | Integrate gizmo rendering |
| `runtime/src/vkr_sample_runtime.c` | Wire up gizmo input and selection |

---

## Implementation Phases

### Phase 1: Gizmo Rendering (Foundation)

1. Reuse `world_overlay`/`world` pipeline (dedicated gizmo shader optional)
2. Generate gizmo geometry procedurally (arrows, rings, cubes)
3. Implement screen-space size calculation
4. Render gizmo at hardcoded position
5. Test: Verify gizmo renders correctly, maintains size on zoom

**Validation**: Gizmo visible with correct axis colors

### Phase 2: Gizmo Picking

1. Extend `VkrPickingIdKind` with GIZMO type
2. Create gizmo picking shader
3. Implement gizmo picking in picking pass (draw last, depth test disabled)
4. Implement gizmo-only hover picking (scissored 1×1) OR accept “no hover” for v1
5. Test hover highlighting

**Validation**: Gizmo handles highlight on hover

### Phase 3: Translation Mode

1. Implement ray construction from mouse
2. Implement ray-plane and ray-axis intersection
3. Create drag state management (begin/update/end)
4. Apply translation delta to scene entities
5. Handle axis constraints via handle selection

**Validation**: Objects move when dragging translation handles

### Phase 4: Rotation Mode

1. Implement rotation gizmo geometry (rings)
2. Compute rotation angle from ray intersections
3. Apply rotation to scene entities
4. Handle local vs world space rotation

**Validation**: Objects rotate when dragging rotation handles

### Phase 5: Scale Mode

1. Implement scale handle rendering
2. Compute scale factor from drag distance
3. Apply uniform scale from cube handles
4. Visual feedback for scale direction

**Validation**: Objects scale when dragging scale handles

### Phase 6: Polish and Integration

1. Store initial transforms for undo
2. Implement transform callback for undo system
3. Add toolbar buttons for explicit mode/space switching (optional)

**Validation**: Full workflow with mouse-only handles

---

## Test Plan

### Unit Tests

1. **Ray construction**: Verify rays match expected world positions
2. **Ray-plane intersection**: Test various angles and edge cases
3. **Ray-axis closest point**: Verify accuracy of projection
4. **Picking ID encoding**: Roundtrip encode/decode
5. **Screen-space size**: Verify consistent apparent size

### Integration Tests

1. **Selection binding**: Gizmo appears at selection center
2. **Translation**: Objects move to expected positions
3. **Rotation**: Objects rotate around expected axes
4. **Scale**: Objects scale correctly
5. **Multi-selection**: All selected objects transform together
6. **Undo callback**: Transform records are correct

### Visual Tests

1. Gizmo renders in front of all objects
2. Axis colors are correct (X=red, Y=green, Z=blue)
3. Highlighted handle is clearly visible
4. Gizmo maintains apparent size when zooming
5. Local space rotates gizmo with object

---

## Performance Considerations

- **Picking**: Gizmo picking pass is lightweight (few triangles)
- **Rendering**: Single draw call per axis, no depth test
- **Ray casting**: Simple math, no spatial queries needed
- **Transform application**: Direct component access, O(n) for n selected

---

## Future Enhancements

1. **Snap to grid**: Hold Ctrl for incremental movement/rotation
2. **Custom pivot**: Shift pivot to object center, bounding box corners
3. **Numeric input**: Type values during transform
4. **Gizmo size preference**: User-configurable screen size
5. **Touch input**: Two-finger gestures for transform

---

## References

- [Blender Transform Gizmo](https://docs.blender.org/manual/en/latest/editors/3dview/controls/gizmos.html)
- [Unity Transform Tools](https://docs.unity3d.com/Manual/PositioningGameObjects.html)
- [Real-Time Rendering: Picking and Intersection](https://www.realtimerendering.com/)
