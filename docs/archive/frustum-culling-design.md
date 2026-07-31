---
status: superseded
updated: 2026-07-31
authority: design
---
# Frustum Culling Design + Integration Plan

> **Archived.** Superseded by
> [ADR-013: Measured Draw Submission](../architecture/adr/013-draw-submission-strategy.md).
> This plan targets the removed view/layer path and places culling in the world
> pass. The current direction culls during packet extraction, before world
> payload materialization, while keeping camera and shadow visibility separate.

## Goal

Add **CPU frustum culling** to reduce world draw calls by skipping meshes outside the camera frustum, while fitting the current renderer architecture (frontend/backend split + view/layer system).

This document is written to be **LLM-consumable**: explicit file paths, data flow, algorithms, and step-by-step implementation plan.

## Non-goals (initial version)

- Occlusion culling (HZB, occlusion queries).
- GPU-driven culling / indirect draws.
- Per-triangle/per-cluster culling.
- Perfectly-tight bounds (we’ll start conservative).

## Current Architecture (relevant pieces)

### World rendering path

- **Frame loop updates camera + globals**: `lib/src/application.h`
  - Updates `application->renderer.globals.view` / `.projection` / `.view_position`.
  - Calls `vkr_mesh_manager_update_model()` for all mesh slots each frame.
- **World layer draws meshes**: `lib/src/renderer/passes/vkr_pass_world.c`
  - Iterates `mesh_index in [0..vkr_mesh_manager_capacity())`
  - For each mesh:
    - Iterates submeshes
    - Opaque submeshes draw immediately
    - Transparent submeshes are collected + distance-sorted, then drawn

### What we already have that enables culling

- **Camera matrices**: `lib/src/renderer/systems/vkr_camera.c`
  - View: `mat4_look_at`
  - Projection: `mat4_perspective` (Vulkan clip conventions)
- **Global shader state** includes camera matrices and view position:
  - `VkrGlobalMaterialState` in `lib/src/renderer/vkr_renderer.h`
- **Per-geometry local bounds exist today**:
  - `VkrGeometry` has `.center`, `.min_extents`, `.max_extents` in `lib/src/renderer/resources/vkr_resources.h`
  - These are populated by mesh loading:
    - `vkr_mesh_loader_compute_bounds()` and assignment to `VkrGeometryConfig` in
      `lib/src/renderer/resources/loaders/mesh_loader.c`

### Matrix / coordinate conventions (important for frustum extraction)

`lib/src/math/mat.h` documents:

- Right-handed
- Column-major
- `mat4_perspective` is **Vulkan-oriented**: **Y inverted**, **Z in [0, 1]**

This affects **near plane extraction** (see below).

## Design Overview

### Culling granularity

Start with **per-mesh culling** (cheap + large win). Optional later: **per-submesh** refinement.

### Bounding volume choice

Use a **bounding sphere** for the first iteration:

- Very cheap to test against 6 planes.
- Conservative under rotation (no false negatives if computed correctly).

We will derive it from existing geometry AABBs.

### Where culling runs

In the **World view render callback**:

- `lib/src/renderer/passes/vkr_pass_world.c` → `vkr_pass_world_execute()`

This keeps culling **view-specific** (each layer can have its own camera), and avoids backend coupling.

## Data Model Changes

### Add mesh-level local bounds (computed once when mesh/submeshes are set)

Add fields to `VkrMesh` in `lib/src/renderer/resources/vkr_resources.h`:

- `bool8_t bounds_valid`
- `Vec3 bounds_local_min`
- `Vec3 bounds_local_max`
- `Vec3 bounds_local_center`
- `float32_t bounds_local_radius`

Optional (nice-to-have) cached world bounds:

- `Vec3 bounds_world_center`
- `float32_t bounds_world_radius`
- `bool8_t bounds_world_dirty` (or reuse `transform.is_dirty`)

Rationale:

- Geometry already has local AABB; mesh bounds are the union across submeshes.
- Local sphere radius can be derived from local AABB half-diagonal.
- Cached world bounds make per-frame culling cheaper and de-couple bounds updates
  from render loop (but can be added later).

### Where bounds are computed/maintained

**Compute local bounds on mesh creation / load** in `lib/src/renderer/systems/vkr_mesh_manager.c`:

- In `vkr_mesh_manager_add()` after submeshes are built and stored into the mesh:
  - Resolve each submesh `VkrGeometry` via `vkr_geometry_system_get_by_handle()`
  - Union `geometry->min_extents/max_extents` into mesh-local min/max
  - Compute:
    - `center = (min + max) * 0.5`
    - `radius = length(max - center)` (equivalently `0.5 * length(max - min)`)
  - Set `bounds_valid=true`

**Update world bounds when model changes** in `vkr_mesh_manager_update_model()`:

- After `mesh->model = vkr_transform_get_world(&mesh->transform)`:
  - `bounds_world_center = mat4_mul_vec3(mesh->model, bounds_local_center)`
  - Compute non-uniform scale factor:
    - \(s_x = ||(m00, m10, m20)||\)
    - \(s_y = ||(m01, m11, m21)||\)
    - \(s_z = ||(m02, m12, m22)||\)
    - `s = max(sx, sy, sz)`
  - `bounds_world_radius = bounds_local_radius * s`

Notes:

- This is conservative even with rotation + non-uniform scale.
- Negative scale is handled by using vector lengths (always ≥ 0).

## New Math/Utility Module

### Add a frustum helper

Add new files:

- `lib/src/math/vkr_frustum.h`
- `lib/src/math/vkr_frustum.c`

Suggested types:

- `typedef struct VkrPlane { Vec3 n; float32_t d; } VkrPlane;` (plane equation: `dot(n, x) + d >= 0` is “inside”)
- `typedef struct VkrFrustum { VkrPlane planes[6]; } VkrFrustum;`
- `typedef enum VkrFrustumPlaneIndex { LEFT, RIGHT, BOTTOM, TOP, NEAR, FAR } ...`

Suggested functions:

- `VkrFrustum vkr_frustum_from_view_projection(Mat4 view, Mat4 projection);`
- `bool8_t vkr_frustum_test_sphere(const VkrFrustum *f, Vec3 center, float32_t radius);`
- (Optional) `bool8_t vkr_frustum_test_aabb(const VkrFrustum *f, Vec3 aabb_min, Vec3 aabb_max);`

### Plane extraction algorithm (clip-space correct)

Let:

- `Mat4 vp = mat4_mul(projection, view);` (standard: MVP = P * V * M)
- `Vec4 r0 = mat4_row(vp, 0)`
- `Vec4 r1 = mat4_row(vp, 1)`
- `Vec4 r2 = mat4_row(vp, 2)`
- `Vec4 r3 = mat4_row(vp, 3)`

Planes (before normalization), based on clip inequalities:

- Left:   `r3 + r0`
- Right:  `r3 - r0`
- Bottom: `r3 + r1`
- Top:    `r3 - r1`

Near/Far depend on the projection’s Z clip range:

- For **Vulkan-style Z in [0, w]** (our `mat4_perspective()`):
  - Near: `r2`          (z >= 0)
  - Far:  `r3 - r2`     (z <= w)
- For **OpenGL-style Z in [-w, w]** (our current `mat4_ortho()`):
  - Near: `r3 + r2`
  - Far:  `r3 - r2`

Robust selection:

- If `projection.m33 == 0.0f` and `projection.m32 != 0.0f` → treat as perspective Vulkan (near = r2)
- Else → treat as “[-w,w] depth” (near = r3 + r2)

### Plane normalization

Normalize each plane to reduce numeric issues:

- `len = length(n)`
- `n /= len; d /= len` (skip if len is ~0)

### Sphere vs frustum test

For each plane:

- `dist = dot(plane.n, center) + plane.d`
- If `dist < -radius` → **outside**, cull
- If all planes pass → **visible**

This is conservative and fast.

## Integration into World View

### Main integration point

Modify:

- `lib/src/renderer/passes/vkr_pass_world.c` → `vkr_pass_world_execute()`

Proposed per-frame flow:

1. Build frustum from the layer camera:
   - `const Mat4 *view = vkr_layer_context_get_view(ctx);`
   - `const Mat4 *proj = vkr_layer_context_get_projection(ctx);`
   - `VkrFrustum fr = vkr_frustum_from_view_projection(*view, *proj);`
2. Iterate meshes:
   - Skip null / not-loaded meshes (existing behavior)
   - If mesh has no valid bounds, treat as visible (safe fallback)
   - Otherwise test mesh world sphere vs frustum
   - If culled: skip all submeshes (also skip adding transparent entries)
3. Keep existing opaque pass + transparent collection/sort/draw

### Transparent sorting input

Current code uses mesh world translation from `vkr_transform_get_world(&mesh->transform)` to compute distance.

After culling exists, prefer:

- Use `mesh->bounds_world_center` (or `mat4_mul_vec3(mesh->model, bounds_local_center)`)

This makes sorting more stable for meshes whose local origin is far from their visible geometry.

### Optional refinement: per-submesh culling (future)

Once mesh-level culling works, optionally cull each submesh using its geometry bounds:

- `VkrGeometry` already has local AABB; you can compute a per-submesh sphere:
  - center = geometry.center, radius from (max-min)/2
- World transform is the same mesh `model`

This reduces work for meshes with many submeshes where only a portion is visible.

## Editor / Offscreen / Picking Considerations

- World pass renders either to swapchain or to offscreen targets (editor mode), but **camera matrices still come from the active camera**.
- Culling should be applied identically in both modes (no special casing needed).
- Picking:
  - `VkrLocalMaterialState.object_id = mesh_index + 1` is already set in world drawing.
  - When a dedicated picking pass/layer is introduced (see `VKR_PIPELINE_DOMAIN_PICKING`), reuse the same visibility logic to ensure “pickable == visible”.

## Debuggability and Controls

Add a simple runtime toggle (optional but recommended):

- Add a world-pass culling toggle (renderer frontend/world resources config), default `true`.

Add basic metrics (cheap, helps validate):

- meshes_total, meshes_culled, meshes_drawn
- submeshes_total, submeshes_drawn

Surface via logging (debug builds) or future UI overlay.

## Performance Plan (Incremental)

### Phase 1 (single-threaded, mesh-level sphere cull)

- Complexity: O(meshes * 6 planes)
- Expected to remove most off-screen draw calls quickly.

### Phase 2 (avoid redundant recomputation)

Today the app loop calls `vkr_mesh_manager_update_model()` for all mesh slots every frame.

Recommended improvement (separate from culling, but synergistic):

- Only update model/bounds when `mesh->transform.is_dirty` or parent changed.
- Keep `mesh->model` + `bounds_world_*` stable otherwise.

### Phase 3 (parallel culling using job system)

If mesh count grows (10k+):

- Split mesh indices into chunks and run culling in `VkrJobSystem` (type `VKR_JOB_TYPE_RESOURCE` is used elsewhere; consider a dedicated culling job type if you add one).
- Each job writes visible mesh indices into a thread-local buffer.
- Merge buffers into a single visible list for the render thread.

Keep actual rendering on the main thread (current architecture).

## Implementation Steps (Suggested PR breakdown)

1. **Add mesh bounds storage**
   - Update `VkrMesh` in `lib/src/renderer/resources/vkr_resources.h`
   - Compute bounds in `vkr_mesh_manager_add()`
   - Update world bounds in `vkr_mesh_manager_update_model()`
2. **Add frustum math module**
   - `lib/src/math/vkr_frustum.h/.c`
   - Ensure the new `.c` file is compiled (update `CMakeLists.txt` if the project does not glob sources)
   - Unit-test plane extraction + sphere test under both perspective and ortho
3. **Integrate into world render loop**
   - `vkr_pass_world_execute()` culls meshes before iterating submeshes
   - Track stats; verify visually and via counts
4. **(Optional) Add toggle + debug reporting**
   - Layer message to enable/disable culling quickly during debugging
5. **(Optional) Per-submesh refinement**
6. **(Optional) Parallelize culling**

## Test Plan

### Unit tests (math-level)

Add tests under `tests/`:

- Frustum creation sanity:
  - Build view/projection from known camera (identity view, simple perspective)
  - Verify points known to be inside/outside pass/fail
- Sphere tests:
  - Sphere fully inside near center → visible
  - Sphere far outside to +X → culled by right plane
  - Sphere intersecting a plane → visible (conservative)

### Integration tests (renderer-level)

- Load a scene with many meshes.
- Move camera so that large groups go in/out of view.
- Compare:
  - Draw call counts (via logs/telemetry hooks)
  - Visual correctness (no popping for on-screen objects)

## Risks / Edge Cases

- **Projection differences**: near plane extraction must respect Vulkan `[0,1]` depth for perspective; ortho currently behaves like `[-1,1]`. This doc’s extraction handles both.
- **Bad/zero bounds**: for degenerate geometry (single point/line), radius may be ~0; must not produce NaNs.
- **Transforms with parents**: `vkr_transform_get_world()` can return identity if parent chain cycles (depth limit). Bounds will then be wrong; but that’s already an engine-level error case.
