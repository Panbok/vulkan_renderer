---
status: partial
updated: 2026-07-31
authority: design
---
# Scene System Extension: 3D Text and Primitive Shapes

**Legacy note:** This document references the deprecated view/layer system
(`view system (removed)`) for cross-system messages. Render orchestration now uses
the render graph; stateless pass executors and world resources own rendering.
The feature ships, but historical examples and the unchecked checklist have not
been fully reconciled with current code.

## Purpose

Extend the existing scene system to support:
1. **3D Text entities** - Text rendered in world space using the existing `VkrText3D` system
2. **Primitive shape entities** - Cube geometry (expandable to other primitives) using `VkrGeometrySystem`

This document is written to be **LLM-consumable**: explicit file paths, data flow, JSON schemas, and implementation checklist.

---

## Table of Contents

1. [Current State Analysis](#1-current-state-analysis)
2. [Goals and Constraints](#2-goals-and-constraints)
3. [Data Model Extensions](#3-data-model-extensions)
4. [JSON Schema Extensions](#4-json-schema-extensions)
5. [Implementation Architecture](#5-implementation-architecture)
6. [Picking and Selection](#6-picking-and-selection)
7. [Lifecycle Management](#7-lifecycle-management)
8. [Runtime Updates](#8-runtime-updates)
9. [API Extensions](#9-api-extensions)
10. [Implementation Plan](#10-implementation-plan)
11. [File Changes Summary](#11-file-changes-summary)

---

## 1. Current State Analysis

### 1.1 Scene System (`lib/src/renderer/systems/vkr_scene_system.h/.c`)

The scene system currently supports:
- ECS-based entity management via `VkrWorld`
- Components: `SceneName`, `SceneTransform`, `SceneMeshRenderer`, `SceneVisibility`, `SceneRenderId`
- Transform hierarchy with topological sorting
- Dirty tracking for efficient render sync
- Mesh ownership tracking for cleanup

**Key structures:**
```c
typedef struct VkrScene {
  VkrWorld *world;
  VkrComponentTypeId comp_name;
  VkrComponentTypeId comp_transform;
  VkrComponentTypeId comp_mesh_renderer;
  VkrComponentTypeId comp_visibility;
  VkrComponentTypeId comp_render_id;
  // ... hierarchy, dirty tracking, mesh ownership
} VkrScene;
```

### 1.2 Scene Loader (`lib/src/renderer/resources/loaders/scene_loader.c`)

Current JSON format supports:
```json
{
  "version": 1,
  "entities": [
    {
      "name": "EntityName",
      "parent": 0,
      "transform": { "pos": [...], "rot": [...], "scale": [...] },
      "mesh": { "path": "...", "pipeline_domain": "world" }
    }
  ]
}
```

**Note:** Line 15 of `scene_loader.h` contains: `// todo: add support for text and geometry entities`

### 1.3 VkrText3D (`lib/src/renderer/resources/world/vkr_text_3d.h/.c`)

Provides 3D text rendering with:
- Font system integration (`VkrFontHandle`)
- Configurable font size, color, texture dimensions
- Transform support (`VkrTransform`)
- Pipeline and instance state management
- Vertex/index buffer generation from glyph layouts

**Key config:**
```c
typedef struct VkrText3DConfig {
  String8 text;
  VkrFontHandle font;
  float32_t font_size;
  Vec4 color;
  uint32_t texture_width;
  uint32_t texture_height;
  float32_t uv_inset_px;
  VkrPipelineHandle pipeline;
} VkrText3DConfig;
```

### 1.4 VkrGeometrySystem (`lib/src/renderer/systems/vkr_geometry_system.h/.c`)

Provides primitive geometry creation:
```c
VkrGeometryHandle vkr_geometry_system_create_cube(
    VkrGeometrySystem *system,
    float32_t width, float32_t height, float32_t depth,
    const char *name,
    VkrRendererError *out_error);
```

Geometry includes:
- Vertex/index buffers
- Reference counting with acquire/release
- Name-based lookup

---

## 2. Goals and Constraints

### 2.1 Goals

1. **Declarative scene definition** - 3D text and shapes defined in `.scene.json` files
2. **Transform hierarchy** - Text and shapes participate in scene transform hierarchy
3. **Picking support** - Text and shapes are pickable in editor mode
4. **Lifecycle management** - Scene owns created resources, cleans up on destroy
5. **Minimal renderer changes** - Use existing `VkrText3D` and `VkrGeometrySystem` APIs

### 2.2 Constraints

1. **No new render passes** - Render through existing world pass executor
2. **Picking ID space** - Use scene render IDs with kind tags (already designed)
3. **Memory ownership** - Scene allocator owns component data; resources cleaned up on scene destroy
4. **Incremental loading** - Support adding text/shapes without breaking existing mesh loading

### 2.3 Non-Goals (v1)

- Runtime text modification API (text is static after load)
- Other primitive shapes (sphere, cylinder, etc.) - only cube for v1
- Material override for shapes (use default material)
- Text billboarding (always face camera)

---

## 3. Data Model Extensions

### 3.1 New Components

#### `SceneText3D` - 3D Text Renderer Component

Stores configuration for a 3D text entity. The actual `VkrText3D` instance is stored in a scene-owned array, not directly in ECS (to avoid complex ECS storage for large structs with internal pointers).

```c
// lib/src/renderer/systems/vkr_scene_system.h

/**
 * @brief 3D text renderer component.
 * Links entity to a scene-owned VkrText3D instance.
 */
typedef struct SceneText3D {
  uint32_t text_index;  // Index into scene's text3d_instances array
} SceneText3D;
```

#### `SceneShape` - Primitive Shape Renderer Component

Stores configuration for a primitive shape entity.

```c
// lib/src/renderer/systems/vkr_scene_system.h

typedef enum SceneShapeType {
  SCENE_SHAPE_CUBE = 0,
  // Future: SCENE_SHAPE_SPHERE, SCENE_SHAPE_CYLINDER, etc.
  SCENE_SHAPE_COUNT
} SceneShapeType;

/**
 * @brief Primitive shape renderer component.
 * Links entity to geometry system handle and material.
 */
typedef struct SceneShape {
  SceneShapeType type;
  VkrGeometryHandle geometry;     // Handle to geometry system
  VkrMaterialHandle material;     // Material handle (or invalid for default)
  Vec3 dimensions;                // Cube: width/height/depth
  Vec4 color;                     // Base color (for default material)
} SceneShape;
```

### 3.2 Extended VkrScene Structure

```c
// lib/src/renderer/systems/vkr_scene_system.h

typedef struct VkrScene {
  // ... existing fields ...

  // New component type IDs
  VkrComponentTypeId comp_text3d;
  VkrComponentTypeId comp_shape;

  // 3D text instance storage (scene-owned)
  VkrText3D *text3d_instances;
  uint32_t text3d_count;
  uint32_t text3d_capacity;

  // Shape geometry handles (for cleanup)
  VkrGeometryHandle *owned_shapes;
  uint32_t owned_shape_count;
  uint32_t owned_shape_capacity;

  // Query for text entities
  VkrQueryCompiled query_text3d;
  VkrQueryCompiled query_shapes;
} VkrScene;
```

---

## 4. JSON Schema Extensions

### 4.1 Extended Schema (Version 2)

```json
{
  "version": 2,
  "entities": [
    {
      "name": "EntityName",
      "parent": null,
      "transform": {
        "pos": [0.0, 0.0, 0.0],
        "rot": [0.0, 0.0, 0.0, 1.0],
        "scale": [1.0, 1.0, 1.0]
      },
      "mesh": { ... },
      "text3d": { ... },
      "shape": { ... }
    }
  ]
}
```

**Entity type is determined by which renderer component is present:**
- `mesh` -> Mesh entity (existing)
- `text3d` -> 3D text entity (new)
- `shape` -> Primitive shape entity (new)

**Mutually exclusive:** An entity can have at most ONE of `mesh`, `text3d`, or `shape`.

### 4.2 Text3D Component Schema

```json
{
  "text3d": {
    "text": "Hello World",
    "font_size": 32.0,
    "color": [1.0, 1.0, 1.0, 1.0],
    "font": "default",
    "texture_width": 512,
    "texture_height": 256,
    "world_width": 2.0,
    "world_height": 1.0
  }
}
```

**Field descriptions:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `text` | string | (required) | Text content to render |
| `font_size` | float | 32.0 | Font size in points |
| `color` | [r,g,b,a] | [1,1,1,1] | Text color (RGBA) |
| `font` | string | "default" | Font name or "default" for MTSDF default |
| `texture_width` | int | 512 | Internal texture width |
| `texture_height` | int | 256 | Internal texture height |
| `world_width` | float | 1.0 | Width in world units |
| `world_height` | float | auto | Height in world units (auto = maintain aspect) |

### 4.3 Shape Component Schema

```json
{
  "shape": {
    "type": "cube",
    "dimensions": [1.0, 1.0, 1.0],
    "color": [1.0, 0.5, 0.2, 1.0],
    "material": null
  }
}
```

**Field descriptions:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `type` | string | (required) | Shape type: "cube" |
| `dimensions` | [w,h,d] | [1,1,1] | Cube dimensions |
| `color` | [r,g,b,a] | [1,1,1,1] | Base color |
| `material` | string/null | null | Material path or null for default |

### 4.4 Example Scene File

```json
{
  "version": 2,
  "entities": [
    {
      "name": "SceneRoot",
      "parent": null,
      "transform": {
        "pos": [0.0, 0.0, 0.0],
        "rot": [0.0, 0.0, 0.0, 1.0],
        "scale": [1.0, 1.0, 1.0]
      }
    },
    {
      "name": "Sponza",
      "parent": 0,
      "transform": {
        "pos": [0.0, 0.0, -15.0],
        "rot": [0.0, 0.0, 0.0, 1.0],
        "scale": [0.0085, 0.0085, 0.0085]
      },
      "mesh": {
        "path": "assets/models/sponza.obj",
        "pipeline_domain": "world"
      }
    },
    {
      "name": "WelcomeText",
      "parent": 0,
      "transform": {
        "pos": [0.0, 5.0, -10.0],
        "rot": [0.0, 0.0, 0.0, 1.0],
        "scale": [1.0, 1.0, 1.0]
      },
      "text3d": {
        "text": "Welcome to the Scene",
        "font_size": 48.0,
        "color": [1.0, 0.8, 0.2, 1.0],
        "world_width": 4.0
      }
    },
    {
      "name": "MarkerCube",
      "parent": 0,
      "transform": {
        "pos": [5.0, 1.0, -10.0],
        "rot": [0.0, 0.0, 0.0, 1.0],
        "scale": [1.0, 1.0, 1.0]
      },
      "shape": {
        "type": "cube",
        "dimensions": [1.0, 2.0, 1.0],
        "color": [0.2, 0.6, 1.0, 1.0]
      }
    }
  ]
}
```

---

## 5. Implementation Architecture

### 5.1 Component Flow Diagram

```
Scene JSON
    |
    v
[scene_loader.c]
    |
    +-- mesh entity --> VkrMeshManager --> SceneMeshRenderer
    |
    +-- text3d entity --> VkrText3D (scene-owned) --> SceneText3D
    |
    +-- shape entity --> VkrGeometrySystem --> SceneShape
    |
    v
[VkrScene ECS World]
    |
    v
[vkr_scene_update()]
    |
    +-- Transform update (hierarchy)
    +-- Text3D transform sync
    +-- Shape transform sync (via mesh manager OR direct draw)
    |
    v
[vkr_pass_world_execute()]
    |
    +-- Mesh rendering (existing)
    +-- Text3D rendering (new callback)
    +-- Shape rendering (new callback OR mesh manager)
```

### 5.2 Rendering Strategy

**Option A: Shapes via Mesh Manager (Recommended)**

Shapes create a mesh entry in `VkrMeshManager` just like loaded meshes. This:
- Reuses existing frustum culling
- Reuses existing picking pipeline
- No changes to `vkr_pass_world`

```c
// In scene_loader.c, for shape entities:
uint32_t mesh_index = create_shape_mesh(rf, shape_config);
vkr_scene_set_mesh_renderer(scene, entity, mesh_index);
```

**Option B: Direct Draw for Text3D**

`VkrText3D` has its own draw function that binds the correct pipeline. Add a hook in `vkr_pass_world` to iterate text entities:

```c
// In vkr_pass_world_execute():
// After mesh rendering, before post-effects:
vkr_scene_render_text3d(scene, rf);
```

### 5.3 Recommended Approach

1. **Shapes**: Use Mesh Manager (Option A)
   - Create geometry via `vkr_geometry_system_create_cube()`
   - Create a single-submesh "pseudo-mesh" entry in mesh manager
   - Link entity via `SceneMeshRenderer` component
   - Picking works automatically

2. **Text3D**: Direct Draw (Option B)
   - Store `VkrText3D` instances in scene
   - Link entity via `SceneText3D` component
   - Add explicit draw call in world pass executor
   - Add picking support for text (deferred or via separate picking kind)

---

## 6. Picking and Selection

### 6.1 Existing Picking Infrastructure

The picking system uses object IDs encoded with a 2-bit kind tag:
- `VKR_PICKING_ID_KIND_SCENE` (0) - Scene entities (render_id + 1)
- `VKR_PICKING_ID_KIND_UI_TEXT` (1) - UI text
- `VKR_PICKING_ID_KIND_WORLD_TEXT` (2) - World text

### 6.2 Shapes Picking

Shapes rendered via mesh manager automatically participate in picking:
- They have a `SceneRenderId` component
- The mesh manager entry gets `render_id` set during scene sync
- Picking decode uses `VKR_PICKING_ID_KIND_SCENE`

### 6.3 Text3D Picking (v1: Deferred)

For v1, text entities are **not pickable**. Adding text picking requires:
1. Text3D rendering to picking buffer with object IDs
2. Using `VKR_PICKING_ID_KIND_WORLD_TEXT` kind tag
3. Mapping text slot index to entity

**Implementation deferred to Phase 2.**

---

## 7. Lifecycle Management

### 7.1 Creation Flow

```c
// scene_loader.c: scene_json_parse_entity()

// For text3d entity:
if (has_text3d) {
  VkrText3DConfig config = parse_text3d_config(json);
  uint32_t text_index = vkr_scene_create_text3d(scene, rf, &config);
  vkr_scene_set_text3d(scene, entity, text_index);
  vkr_scene_ensure_render_id(scene, entity, NULL);  // For future picking
}

// For shape entity:
if (has_shape) {
  SceneShapeConfig config = parse_shape_config(json);
  uint32_t mesh_index = vkr_scene_create_shape(scene, rf, &config);
  vkr_scene_set_mesh_renderer(scene, entity, mesh_index);
}
```

### 7.2 Update Flow

```c
// vkr_scene_update()

// After transform hierarchy update:
vkr_scene_update_text3d_transforms(scene);  // Sync Text3D transforms from ECS
// Shapes use mesh manager, updated via existing render bridge sync
```

### 7.3 Destruction Flow

```c
// vkr_scene_shutdown()

// Destroy owned text instances
for (uint32_t i = 0; i < scene->text3d_count; i++) {
  vkr_text_3d_destroy(&scene->text3d_instances[i]);
}

// Destroy owned shape geometries
for (uint32_t i = 0; i < scene->owned_shape_count; i++) {
  vkr_geometry_system_release(&rf->geometry_system, scene->owned_shapes[i]);
}

// Existing: destroy owned meshes
```

---

## 8. Runtime Updates

This section describes how scene-loaded text and shapes can be modified at runtime, following the existing patterns established for world text updates in `app/src/main.c`.

### 8.1 Existing World Text Update Pattern

The current world text (clock display) demonstrates the update flow:

```c
// app/src/main.c:817-852 - application_update_world_text()

// 1. Check update interval
if (!vkr_clock_interval_elapsed(&state->world_text_update_clock,
                                VKR_WORLD_TIME_UPDATE_INTERVAL)) {
  return;
}

// 2. Create scoped allocator for temporary string
VkrAllocatorScope scope = vkr_allocator_begin_scope(&application->app_allocator);

// 3. Format new text content
String8 time_text = string8_create_formatted(&application->app_allocator,
                                              "%02d:%02d:%02d", ...);

// 4. Update the world text instance directly (render graph path)
vkr_world_resources_text_update(&application->renderer,
                                &application->renderer.world_resources,
                                state->world_text_id, time_text);

// 5. End scope (free temporary allocations)
vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
```

**Key characteristics:**
- Updates sent via world resources API
- Text identified by `text_id` (slot index in world resources)
- Temporary allocations cleaned up after message sent
- Polling/interval-based updates (not continuous)

### 8.2 Scene Entity Update Approaches

For scene-loaded entities, we have two update paths:

#### Option A: Direct Scene API (Recommended)

Updates go through scene system API, which manages dirty tracking and renderer sync:

```c
// Update text content
vkr_scene_update_text3d(scene, entity, new_text);

// Update transform (existing API - works for all entity types)
vkr_scene_set_position(scene, entity, new_position);
vkr_scene_set_rotation(scene, entity, new_rotation);
vkr_scene_set_scale(scene, entity, new_scale);
```

**Advantages:**
- Unified API for all scene entities
- Automatic dirty tracking
- Transform hierarchy properly propagated
- No need to know internal text_id/slot indices

#### Option B: Direct World-Resources API (For Special Cases)

Direct world-resources calls bypass the scene system - useful for non-scene text or when
scene handle is unavailable:

```c
// Direct update (bypasses scene system)
vkr_world_resources_text_update(rf, &rf->world_resources, text_id, text);
```

**Use cases:**
- Editor-only overlays not part of scene
- Debug text that shouldn't be in scene graph

### 8.3 Update Flow for Scene Text3D

```
User Code                    Scene System                 World Resources
    |                             |                              |
    | vkr_scene_update_text3d()                                  |
    |---------------------------->|                              |
    |                             | Mark text dirty              |
    |                             |----------------------------->|
    |                             |                              |
    | vkr_scene_update()          |                              |
    |---------------------------->|                              |
    |                             | For each dirty text:         |
    |                             |   vkr_text_3d_set_text()     |
    |                             |----------------------------->|
    |                             |   Sync transform from ECS    |
    |                             |----------------------------->|
    |                             | Clear dirty flags            |
    |                             |                              |
```

### 8.4 Update Flow for Scene Shapes

Shapes use the mesh manager, so transform updates follow the existing scene sync path:

```
User Code                    Scene System                 Mesh Manager
    |                             |                              |
    | vkr_scene_set_position()    |                              |
    |---------------------------->|                              |
    |                             | Mark transform DIRTY_LOCAL   |
    |                             | Mark render dirty            |
    |                             |                              |
    | vkr_scene_update()          |                              |
    |---------------------------->|                              |
    |                             | Recompute world matrix       |
    |                             |                              |
    | vkr_scene_handle_sync()     |                              |
    |---------------------------->|                              |
    |                             | vkr_mesh_manager_set_model() |
    |                             |----------------------------->|
    |                             |                              |
```

### 8.5 Text3D Dirty Tracking

Extend the scene dirty tracking for text-specific changes:

```c
// lib/src/renderer/systems/vkr_scene_system.h

// Text dirty flags (separate from transform dirty flags)
#define SCENE_TEXT3D_DIRTY_CONTENT   0x01  // Text string changed
#define SCENE_TEXT3D_DIRTY_COLOR     0x02  // Color changed
#define SCENE_TEXT3D_DIRTY_FONT      0x04  // Font/size changed
#define SCENE_TEXT3D_DIRTY_TRANSFORM 0x08  // Transform needs sync to VkrText3D

typedef struct SceneText3D {
  uint32_t text_index;  // Index into scene's text3d_instances array
  uint8_t dirty_flags;  // Bitmask of SCENE_TEXT3D_DIRTY_* flags
} SceneText3D;
```

### 8.6 Per-Frame Update Implementation

```c
// lib/src/renderer/systems/vkr_scene_system.c

/**
 * @brief Update text3d instances from ECS state.
 * Called from vkr_scene_update() after transform hierarchy update.
 */
vkr_internal void vkr_scene_update_text3d(VkrScene *scene) {
  if (!scene->queries_valid) return;

  // Iterate all entities with SceneText3D and SceneTransform
  vkr_entity_query_compiled_each_chunk(&scene->query_text3d,
                                        scene_update_text3d_cb, scene);
}

vkr_internal void scene_update_text3d_cb(const VkrArchetype *arch,
                                          VkrChunk *chunk, void *user) {
  VkrScene *scene = (VkrScene *)user;
  uint32_t count = vkr_entity_chunk_count(chunk);

  SceneText3D *text_comps = vkr_entity_chunk_column(chunk, scene->comp_text3d);
  SceneTransform *transforms = vkr_entity_chunk_column(chunk, scene->comp_transform);

  for (uint32_t i = 0; i < count; i++) {
    SceneText3D *text_comp = &text_comps[i];
    SceneTransform *transform = transforms ? &transforms[i] : NULL;

    if (text_comp->text_index >= scene->text3d_count) continue;
    VkrText3D *text3d = &scene->text3d_instances[text_comp->text_index];

    // Sync transform if changed
    if (transform && (text_comp->dirty_flags & SCENE_TEXT3D_DIRTY_TRANSFORM)) {
      VkrTransform vkr_transform = vkr_transform_from_matrix(transform->world);
      vkr_text_3d_set_transform(text3d, vkr_transform);
    }

    // Content/color updates already applied by setter functions
    // Just clear dirty flags
    text_comp->dirty_flags = 0;
  }
}
```

### 8.7 Shape Runtime Updates

Shapes have fewer runtime-modifiable properties (geometry is immutable after creation):

| Property | Update Method | Notes |
|----------|---------------|-------|
| Position | `vkr_scene_set_position()` | Via transform hierarchy |
| Rotation | `vkr_scene_set_rotation()` | Via transform hierarchy |
| Scale | `vkr_scene_set_scale()` | Via transform hierarchy |
| Visibility | `vkr_scene_set_visibility()` | Existing API |
| Color | `vkr_scene_set_shape_color()` | New - updates material |
| Dimensions | Not supported | Requires geometry recreation |

**Color updates for shapes:**

```c
void vkr_scene_set_shape_color(VkrScene *scene, VkrEntityId entity, Vec4 color) {
  SceneShape *shape = vkr_entity_get_component_mut(scene->world, entity,
                                                    scene->comp_shape);
  if (!shape) return;

  shape->color = color;
  // Mark for material update on next sync
  scene_mark_render_dirty(scene, entity);
}
```

### 8.8 Example: Updating Scene Text at Runtime

```c
// app/src/main.c - Example usage pattern

vkr_internal void application_update_scene_text(Application *application,
                                                 float64_t delta_time) {
  VkrSceneHandle scene_handle = state->scene_resource.as.scene;
  VkrScene *scene = vkr_scene_handle_get_scene(scene_handle);
  if (!scene) return;

  // Find the text entity by name (or cache the entity ID)
  VkrEntityId text_entity = vkr_scene_find_entity_by_name(scene,
                                string8_lit("WelcomeText"));
  if (text_entity.u64 == VKR_ENTITY_ID_INVALID.u64) return;

  // Update text content periodically
  if (vkr_clock_interval_elapsed(&state->scene_text_clock, 1.0)) {
    VkrAllocatorScope scope = vkr_allocator_begin_scope(&application->app_allocator);

    VkrTime time = vkr_platform_get_local_time();
    String8 new_text = string8_create_formatted(&application->app_allocator,
                                                 "Time: %02d:%02d:%02d",
                                                 time.hours, time.minutes, time.seconds);

    vkr_scene_set_text3d_content(scene, text_entity, new_text);

    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  // Animate rotation
  static float32_t angle = 0.0f;
  angle += (float32_t)delta_time * 0.5f;
  VkrQuat rotation = vkr_quat_from_euler(0.0f, angle, 0.0f);
  vkr_scene_set_rotation(scene, text_entity, rotation);
}
```

### 8.9 Example: Animating Scene Shapes

```c
// app/src/main.c - Example usage pattern

vkr_internal void application_update_scene_shapes(Application *application,
                                                   float64_t delta_time) {
  VkrSceneHandle scene_handle = state->scene_resource.as.scene;
  VkrScene *scene = vkr_scene_handle_get_scene(scene_handle);
  if (!scene) return;

  // Find the cube entity
  VkrEntityId cube_entity = vkr_scene_find_entity_by_name(scene,
                                string8_lit("MarkerCube"));
  if (cube_entity.u64 == VKR_ENTITY_ID_INVALID.u64) return;

  // Rotate the cube
  static float32_t angle = 0.0f;
  angle += (float32_t)delta_time * 1.0f;
  VkrQuat rotation = vkr_quat_from_euler(angle * 0.5f, angle, angle * 0.3f);
  vkr_scene_set_rotation(scene, cube_entity, rotation);

  // Bob up and down
  SceneTransform *transform = vkr_scene_get_transform(scene, cube_entity);
  if (transform) {
    float32_t y_offset = sinf(angle * 2.0f) * 0.5f;
    Vec3 base_pos = vec3_new(5.0f, 1.0f + y_offset, -10.0f);
    vkr_scene_set_position(scene, cube_entity, base_pos);
  }

  // Pulse color based on time
  float32_t pulse = (sinf(angle * 3.0f) + 1.0f) * 0.5f;
  Vec4 color = vec4_new(0.2f + pulse * 0.8f, 0.6f, 1.0f - pulse * 0.5f, 1.0f);
  vkr_scene_set_shape_color(scene, cube_entity, color);
}
```

### 8.10 Entity Lookup Helper

For runtime updates, we need to find entities by name:

```c
// lib/src/renderer/systems/vkr_scene_system.h

/**
 * @brief Find entity by name.
 * @param scene Scene to search.
 * @param name Entity name to find.
 * @return Entity ID or VKR_ENTITY_ID_INVALID if not found.
 *
 * Note: O(n) search through all named entities. Cache the result
 * if calling frequently.
 */
VkrEntityId vkr_scene_find_entity_by_name(const VkrScene *scene, String8 name);
```

### 8.11 Performance Considerations

**Text updates:**
- Text content changes trigger layout recomputation and buffer regeneration
- Minimize update frequency (use interval-based updates like world text clock)
- Consider pre-allocating buffer capacity for texts that change frequently

**Transform updates:**
- Transform changes are cheap (just flag setting)
- Actual matrix computation happens in `vkr_scene_update()`
- Dirty tracking ensures only changed transforms are synced to renderer

**Shape color updates:**
- Requires material property update
- May trigger descriptor set updates
- Less expensive than text content changes

---

## 9. API Extensions

### 9.1 Scene System API

```c
// lib/src/renderer/systems/vkr_scene_system.h

// ============================================================================
// Text3D Component
// ============================================================================

/**
 * @brief Create a 3D text instance owned by the scene.
 * @param scene Scene to create text in.
 * @param rf Renderer frontend.
 * @param config Text configuration.
 * @param out_error Optional error output.
 * @return Text instance index, or UINT32_MAX on failure.
 */
uint32_t vkr_scene_create_text3d(VkrScene *scene, struct s_RendererFrontend *rf,
                                  const VkrText3DConfig *config,
                                  VkrSceneError *out_error);

/**
 * @brief Add SceneText3D component to entity.
 * @param scene Scene containing the entity.
 * @param entity Entity to modify.
 * @param text_index Index from vkr_scene_create_text3d.
 * @return true on success.
 */
bool8_t vkr_scene_set_text3d(VkrScene *scene, VkrEntityId entity,
                              uint32_t text_index);

/**
 * @brief Get Text3D instance for entity.
 * @return VkrText3D pointer or NULL if entity lacks text component.
 */
VkrText3D *vkr_scene_get_text3d(VkrScene *scene, VkrEntityId entity);

/**
 * @brief Update text content for a text entity.
 * @param scene Scene containing the entity.
 * @param entity Entity with SceneText3D component.
 * @param text New text content.
 */
void vkr_scene_set_text3d_content(VkrScene *scene, VkrEntityId entity,
                                   String8 text);

// ============================================================================
// Shape Component
// ============================================================================

/**
 * @brief Configuration for creating a primitive shape.
 */
typedef struct SceneShapeConfig {
  SceneShapeType type;
  Vec3 dimensions;        // Cube: width/height/depth
  Vec4 color;             // Base color
  String8 material_path;  // Optional material path (empty for default)
} SceneShapeConfig;

/**
 * @brief Create a shape and add to mesh manager.
 * @param scene Scene to create shape in.
 * @param rf Renderer frontend.
 * @param config Shape configuration.
 * @param out_mesh_index Output mesh manager index.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_create_shape(VkrScene *scene, struct s_RendererFrontend *rf,
                                const SceneShapeConfig *config,
                                uint32_t *out_mesh_index,
                                VkrSceneError *out_error);

/**
 * @brief Add SceneShape component to entity (uses mesh renderer internally).
 */
bool8_t vkr_scene_set_shape(VkrScene *scene, VkrEntityId entity,
                             const SceneShapeConfig *config,
                             uint32_t mesh_index);

/**
 * @brief Get SceneShape component for entity.
 */
const SceneShape *vkr_scene_get_shape(const VkrScene *scene, VkrEntityId entity);
```

### 9.2 Scene Loader Extensions

```c
// lib/src/renderer/resources/loaders/scene_loader.h

typedef struct VkrSceneLoadResult {
  uint32_t entity_count;
  uint32_t mesh_count;
  uint32_t text3d_count;   // New
  uint32_t shape_count;    // New
} VkrSceneLoadResult;
```

### 9.3 Internal Parsing Structures

```c
// scene_loader.c (internal)

typedef struct SceneText3DImport {
  String8 text;
  float32_t font_size;
  Vec4 color;
  String8 font_name;
  uint32_t texture_width;
  uint32_t texture_height;
  float32_t world_width;
  float32_t world_height;
} SceneText3DImport;

typedef struct SceneShapeImport {
  SceneShapeType type;
  Vec3 dimensions;
  Vec4 color;
  String8 material_path;
} SceneShapeImport;

typedef struct SceneEntityImport {
  // ... existing fields ...
  bool8_t has_text3d;
  SceneText3DImport text3d;
  bool8_t has_shape;
  SceneShapeImport shape;
} SceneEntityImport;
```

---

## 10. Implementation Plan

### Phase 1: Data Model and Components

**Step 1.1:** Add new components to scene system header

- Define `SceneText3D` component struct
- Define `SceneShape` component struct and `SceneShapeType` enum
- Add component type IDs to `VkrScene`
- Add text/shape instance storage arrays

**Step 1.2:** Register components in scene init

- Register `comp_text3d` and `comp_shape` in `vkr_scene_init()`
- Build compiled queries for text and shape entities

**Step 1.3:** Implement storage management

- `vkr_scene_create_text3d()` - Create and store VkrText3D instance
- Text3D instance array management (ensure capacity, cleanup)
- Shape geometry handle tracking

### Phase 2: JSON Parser Extensions

**Step 2.1:** Add parsing for text3d objects

- `scene_json_parse_text3d()` - Parse text3d JSON object
- `scene_json_parse_vec4()` - Parse color arrays (already have vec3)
- Handle font name lookup (map "default" to system default)

**Step 2.2:** Add parsing for shape objects

- `scene_json_parse_shape()` - Parse shape JSON object
- `scene_json_parse_shape_type()` - Parse type string to enum

**Step 2.3:** Update entity parsing

- Extend `SceneEntityImport` struct
- Add text3d/shape parsing to `scene_json_parse_entity()`
- Validate mutual exclusivity of renderer components

### Phase 3: Resource Creation

**Step 3.1:** Implement text3d creation in loader

- Create `VkrText3DConfig` from `SceneText3DImport`
- Call `vkr_scene_create_text3d()`
- Add `SceneText3D` component to entity
- Assign render ID for future picking

**Step 3.2:** Implement shape creation in loader

- For cube: call `vkr_geometry_system_create_cube()`
- Create mesh manager entry from geometry
- Track geometry handle for cleanup
- Add `SceneMeshRenderer` and `SceneShape` components

### Phase 4: Rendering Integration

**Step 4.1:** Add text3d transform sync

- In `vkr_scene_update()`: sync ECS transforms to VkrText3D instances
- Handle world matrix from transform hierarchy

**Step 4.2:** Add text3d drawing hook

- Add `vkr_scene_render_text3d()` function
- Call from `vkr_pass_world_execute()` after mesh pass
- Iterate text entities with visibility check

**Step 4.3:** Verify shape rendering

- Shapes go through mesh manager - should work automatically
- Verify frustum culling and picking

### Phase 5: Cleanup and Testing

**Step 5.1:** Implement destruction

- Extend `vkr_scene_shutdown()` to destroy text instances
- Release shape geometry handles

**Step 5.2:** Update load result reporting

- Track text3d_count and shape_count in loader
- Log summary on scene load

**Step 5.3:** Create test scene

- Update `assets/scenes/default.scene.json` with text and cube entities
- Verify loading, rendering, transform hierarchy

---

## 11. File Changes Summary

### New Files

None - all changes are extensions to existing files.

### Modified Files

**`lib/src/renderer/systems/vkr_scene_system.h`**
- Add `SceneText3D` component struct
- Add `SceneShape` component struct and `SceneShapeType` enum
- Add `SceneShapeConfig` struct
- Add component type IDs to `VkrScene` struct
- Add text3d instance storage to `VkrScene`
- Add text3d/shape API function declarations

**`lib/src/renderer/systems/vkr_scene_system.c`**
- Register new components in `vkr_scene_init()`
- Implement `vkr_scene_create_text3d()`
- Implement `vkr_scene_set_text3d()`
- Implement `vkr_scene_get_text3d()`
- Implement `vkr_scene_set_text3d_content()`
- Implement `vkr_scene_create_shape()`
- Implement `vkr_scene_set_shape()`
- Implement text3d transform sync in `vkr_scene_update()`
- Add text3d cleanup in `vkr_scene_shutdown()`

**`lib/src/renderer/resources/loaders/scene_loader.h`**
- Extend `VkrSceneLoadResult` with text3d_count, shape_count

**`lib/src/renderer/resources/loaders/scene_loader.c`**
- Add `SceneText3DImport` and `SceneShapeImport` structs
- Extend `SceneEntityImport` struct
- Add `scene_json_parse_text3d()`
- Add `scene_json_parse_shape()`
- Add `scene_json_parse_vec4()` if not present
- Update `scene_json_parse_entity()` to handle new components
- Add text3d/shape creation in `vkr_scene_load_from_json()`

**`lib/src/renderer/passes/vkr_pass_world.c`**
- Add call to `vkr_scene_render_text3d()` after mesh rendering

**`assets/scenes/default.scene.json`**
- Update to version 2
- Add example text3d entity
- Add example cube shape entity

---

## Appendix A: Error Handling

### New Error Codes

```c
typedef enum VkrSceneError {
  // ... existing ...
  VKR_SCENE_ERROR_TEXT3D_CREATE_FAILED,
  VKR_SCENE_ERROR_SHAPE_CREATE_FAILED,
  VKR_SCENE_ERROR_GEOMETRY_CREATE_FAILED,
  VKR_SCENE_ERROR_INVALID_SHAPE_TYPE,
} VkrSceneError;
```

---

## Appendix B: Future Extensions

### Additional Shape Types

```c
typedef enum SceneShapeType {
  SCENE_SHAPE_CUBE = 0,
  SCENE_SHAPE_SPHERE,      // Future
  SCENE_SHAPE_CYLINDER,    // Future
  SCENE_SHAPE_PLANE,       // Future
  SCENE_SHAPE_CAPSULE,     // Future
  SCENE_SHAPE_COUNT
} SceneShapeType;
```

### Text Picking (Phase 2)

```c
// In picking pass:
// Render text quads with object_id = encode(KIND_WORLD_TEXT, text_slot_index)

// In scene bridge:
VkrEntityId vkr_scene_handle_entity_from_text_picking_id(
    VkrSceneHandle handle, uint32_t text_slot_index);
```

### Dynamic Text Updates

```c
// Runtime API for text modification:
void vkr_scene_set_text3d_color(VkrScene *scene, VkrEntityId entity, Vec4 color);
void vkr_scene_set_text3d_font_size(VkrScene *scene, VkrEntityId entity, float32_t size);
```

---

## Appendix C: Implementation Checklist

```
[ ] Phase 1: Data Model
    [ ] Add SceneText3D component to vkr_scene_system.h
    [ ] Add SceneShape component and enum to vkr_scene_system.h
    [ ] Add component type IDs to VkrScene struct
    [ ] Add text3d instance array to VkrScene struct
    [ ] Register components in vkr_scene_init()
    [ ] Implement text3d array management functions

[ ] Phase 2: JSON Parsing
    [ ] Add scene_json_parse_vec4() function
    [ ] Add scene_json_parse_text3d() function
    [ ] Add scene_json_parse_shape() function
    [ ] Add scene_json_parse_shape_type() function
    [ ] Extend SceneEntityImport struct
    [ ] Update scene_json_parse_entity() for text3d/shape
    [ ] Add mutual exclusivity check for renderer components

[ ] Phase 3: Resource Creation
    [ ] Implement vkr_scene_create_text3d()
    [ ] Implement vkr_scene_set_text3d()
    [ ] Implement vkr_scene_create_shape() (cube only)
    [ ] Add text3d creation to scene loader
    [ ] Add shape creation to scene loader
    [ ] Track geometry handles for cleanup

[ ] Phase 4: Rendering
    [ ] Implement vkr_scene_update_text3d_transforms()
    [ ] Implement vkr_scene_render_text3d()
    [ ] Add text3d draw call to vkr_pass_world
    [ ] Verify shape rendering through mesh manager

[ ] Phase 5: Cleanup
    [ ] Add text3d cleanup to vkr_scene_shutdown()
    [ ] Add shape geometry release to vkr_scene_shutdown()
    [ ] Update VkrSceneLoadResult with counts
    [ ] Update load logging

[ ] Phase 6: Testing
    [ ] Update default.scene.json with examples
    [ ] Test text3d loading and rendering
    [ ] Test shape loading and rendering
    [ ] Test transform hierarchy with text/shapes
    [ ] Test scene reload (no memory growth)
```
