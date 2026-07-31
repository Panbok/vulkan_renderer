---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Camera Registry Refactor Plan

## Goals

- Add a camera registry to manage multiple cameras concurrently.
- Provide acquire/release semantics with ref counting and auto-release (similar to texture system).
- Introduce stable handles (id + generation) for cameras.
- Keep current single-camera APIs working (backwards compatibility) while adding new registry APIs.

## Current State (reference)

```49:82:lib/src/renderer/systems/vkr_camera.c
void vkr_camera_system_perspective_create(VkrCamera *camera, VkrWindow *window,
                                          float32_t zoom, float32_t near_clip,
                                          float32_t far_clip) {
  assert_log(window != NULL, "Window is NULL");

  camera->window = window;

  camera->type = VKR_CAMERA_TYPE_PERSPECTIVE;

  camera->speed = VKR_DEFAULT_CAMERA_SPEED;
  camera->sensitivity = VKR_DEFAULT_CAMERA_SENSITIVITY;
  camera->yaw = VKR_DEFAULT_CAMERA_YAW;
  camera->pitch = VKR_DEFAULT_CAMERA_PITCH;

  camera->position = VKR_DEFAULT_CAMERA_POSITION;
  // ...
}
```
```126:150:lib/src/renderer/systems/vkr_camera.c
void vkr_camera_system_update(VkrCamera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  assert_log(camera->window != NULL, "Camera window is NULL");
  assert_log(camera->type != VKR_CAMERA_TYPE_NONE, "Camera type is NONE");

  VkrWindowPixelSize window_size = vkr_window_get_pixel_size(camera->window);
  // ...
}
```

These APIs operate on a single `VkrCamera` instance; we will keep them, but add a registry that holds many `VkrCamera` and exposes handle-based operations.

## Data Structures

- VkrCameraHandle: stable reference to a camera
  - `uint32_t id` (index + 1)
  - `uint32_t generation` (increments when a slot is reused)
- VkrCameraEntry: hash value stored per name
  - `uint32_t index` (into cameras array)
  - `uint32_t ref_count`
  - `bool8_t auto_release`
- VkrCameraSystemConfig
  - `uint32_t max_camera_count`
  - `uint64_t arena_reserve`, `uint64_t arena_commit` (defaults similar to texture system)
- VkrCameraSystem
  - `Arena *arena`
  - `Array_VkrCamera cameras` (fixed capacity = max_camera_count)
  - `VkrHashTable_VkrCameraEntry camera_map` (string key -> entry)
  - `uint32_t next_free_index`
  - `uint32_t generation_counter` (starts at 1)
  - `VkrCameraHandle default_camera`
  - `VkrCameraHandle active_camera`

## Public API (new)

```c
// Types
typedef struct VkrCameraHandle { uint32_t id, generation; } VkrCameraHandle;

typedef struct VkrCameraEntry {
  uint32_t index;
  uint32_t ref_count;
  bool8_t  auto_release;
} VkrCameraEntry;

typedef struct VkrCameraSystemConfig {
  uint32_t max_camera_count;
  uint64_t arena_reserve;
  uint64_t arena_commit;
} VkrCameraSystemConfig;

typedef struct VkrCameraSystem {
  Arena *arena;
  Array_VkrCamera cameras;
  VkrHashTable_VkrCameraEntry camera_map;
  uint32_t next_free_index;
  uint32_t generation_counter;
  VkrCameraHandle default_camera;
  VkrCameraHandle active_camera;
} VkrCameraSystem;

// Init/Shutdown
bool8_t vkr_camera_registry_init(const VkrCameraSystemConfig *config,
                                VkrCameraSystem *out_system);
void    vkr_camera_registry_shutdown(VkrCameraSystem *system);

// Creation (unique by name)
bool8_t vkr_camera_registry_create_perspective(
    VkrCameraSystem *system, String8 name, VkrWindow *window,
    float32_t zoom, float32_t near_clip, float32_t far_clip,
    VkrCameraHandle *out_handle);

bool8_t vkr_camera_registry_create_orthographic(
    VkrCameraSystem *system, String8 name, VkrWindow *window,
    float32_t left, float32_t right, float32_t bottom, float32_t top,
    float32_t near_clip, float32_t far_clip,
    VkrCameraHandle *out_handle);

// Acquire/Release (ref counting)
VkrCameraHandle vkr_camera_registry_acquire(VkrCameraSystem *system,
                                           String8 name,
                                           bool8_t auto_release,
                                           bool8_t *out_ok);
void vkr_camera_registry_release(VkrCameraSystem *system, String8 name);
void vkr_camera_registry_release_by_handle(VkrCameraSystem *system,
                                          VkrCameraHandle handle);

// Update and Getters
void vkr_camera_registry_update(VkrCameraSystem *system, VkrCameraHandle h);
void vkr_camera_registry_update_all(VkrCameraSystem *system);
Mat4 vkr_camera_registry_get_view(VkrCameraSystem *system, VkrCameraHandle h);
Mat4 vkr_camera_registry_get_projection(VkrCameraSystem *system,
                                        VkrCameraHandle h);

// Active camera (incremental migration aids)
void            vkr_camera_registry_set_active(VkrCameraSystem *system,
                                              VkrCameraHandle h);
VkrCameraHandle vkr_camera_registry_get_active(VkrCameraSystem *system);
Mat4            vkr_camera_registry_get_active_view(VkrCameraSystem *system);
Mat4            vkr_camera_registry_get_active_projection(
               VkrCameraSystem *system);

// Window interaction
void vkr_camera_registry_on_window_resize(VkrCameraSystem *system,
                                          VkrWindow *window);

// Lookups (advanced)
VkrCamera *vkr_camera_registry_get_by_handle(VkrCameraSystem *system,
                                            VkrCameraHandle h);
VkrCamera *vkr_camera_registry_get_by_index(VkrCameraSystem *system,
                                           uint32_t index);
```

Notes:

- Generation increments only when a slot is (re)created/unloaded (slot reuse), not on per-frame updates or param changes, to keep handles stable for the camera’s lifetime.
- `get_view`/`get_projection` assert that the camera is updated (same behavior as current functions) to retain clear contracts.
- `on_window_resize` marks matching cameras’ projections dirty, mirroring current per-camera logic.

## Backward Compatibility

- Keep existing single-camera helpers:
  - `vkr_camera_system_perspective_create`, `vkr_camera_system_orthographic_create`, `vkr_camera_system_update`, `vkr_camera_translate/rotate/zoom`, `vkr_camera_system_get_view_matrix`, `vkr_camera_system_get_projection_matrix`.
- No signature changes to existing functions. They remain valid on raw `VkrCamera*` for local/stack cameras.
- New registry lives alongside; renderer code can incrementally adopt handle-based APIs.
- Provide convenience wrappers that operate on `active_camera` for quick drop-in migration if needed.

## Behavior Parity with Texture System

- Name uniqueness constraint (per registry) and stable C-string keys allocated in the arena.
- `acquire(name, auto_release)` increases ref_count; first acquire sets auto_release.
- `release(name|handle)` decrements ref_count; if it hits 0 and `auto_release == true`, the camera is destroyed, slot cleared, name removed.
- `id = slot + 1`, `generation` increases on (re)create to prevent stale-handle reuse.

## Implementation Steps (high-level)

1. Add types to header (`VkrCameraHandle`, `VkrCameraEntry`, `VkrCameraSystemConfig`, `VkrCameraSystem`) and public API prototypes.
2. Implement registry memory + containers in `vkr_camera.c` (arena, array, hash table; `find_free_slot`, stable key copy, destroy helpers).
3. Implement `vkr_camera_registry_init/shutdown`, including default camera creation (slot 0) and `active_camera = default`.
4. Implement `create_perspective/orthographic` using existing camera init routines; assign id/generation; insert hash entry with ref_count = 0 (like textures).
5. Implement `acquire/release/release_by_handle` with ref counting + auto-release.
6. Implement `get_by_handle/get_by_index`, `update`, `update_all`, view/projection getters (assert not dirty).
7. Add `set_active/get_active` and active getters for migration.
8. Add `on_window_resize` to mark cameras bound to `window` as projection-dirty.
9. Keep existing single-camera API unchanged; document migration path.
10. Compile and test basic flows; migrate one call-site in renderer as smoke test (optional follow-up task).

## Risks & Mitigations

- Risk: Name collisions across registries.
  - Mitigation: One registry per renderer/app; name uniqueness enforced within the registry’s map.
- Risk: Stale handles used after auto-release.
  - Mitigation: Generation check returns NULL quickly; add clear logs on invalid handle.
- Risk: Inconsistent update before read.
  - Mitigation: Retain assert-on-dirty contract; provide `update_all` convenience.

## Deliverables

- Updated `lib/src/renderer/systems/vkr_camera.h` and `lib/src/renderer/systems/vkr_camera.c` with registry.
- Back-compat preserved; optional convenience wrappers for active camera.
- Minimal docs in a `.spec/` markdown file describing the registry API and migration.

## Registry API (implemented)

- Types added: `VkrCameraHandle` (id+generation), `VkrCameraEntry`, `VkrCameraSystemConfig` (max count + arena sizes), and `VkrCameraSystem` (arena, camera array, name map, next_free_index/generation_counter, default/active handles).
- Init/shutdown: `vkr_camera_registry_init` allocates an arena (defaults to 16MB/4MB reserve/commit if not provided), builds camera array/hash table, and sets handles invalid; shutdown destroys the arena and clears the system.
- Creation: `vkr_camera_registry_create_perspective/orthographic` use existing camera initializers, assign `generation` and handle (`id = slot + 1`), insert map entry with `ref_count = 0`, and set the first camera as default+active. Names are copied into the arena for stability.
- Acquire/Release: `acquire(name, auto_release)` bumps ref_count and latches auto_release on first acquire; `release(name|handle)` decrements and auto-releases when ref_count hits 0 (slot cleared, map entry removed, default/active fall back to another valid camera if available).
- Updates/Getters: `update(handle)`, `update_all`, `get_view/get_projection`, and active-camera wrappers. Invalid handles log and return identity matrices; view/projection still assert if called while dirty to preserve the old contract.
- Window resize: `vkr_camera_registry_on_window_resize` marks cameras bound to the resized window as projection-dirty and refreshes cached dimensions.
- Backward compatibility: All single-camera helpers remain unchanged for stack/local usage; registry is additive for handle-based workflows. First created registry camera becomes the default active camera; callers can override via `set_active`.
