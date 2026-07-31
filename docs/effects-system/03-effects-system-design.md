---
status: proposed
updated: 2026-07-31
authority: design
---
# Phase 3: Effects System Core Design

## Overview

The effects system manages GPU compute effects that modify mesh vertex data before rendering. Effects are registered by name, bound to entities/submeshes via tags, and executed during the render frame before graphics passes.

## Prerequisites

- **Phase 1**: Tagging system implemented (`vkr_tag_system.h/c`)
- **Phase 2**: Compute pipeline support (`vulkan_compute.h/c`)
- Understanding of `vkr_mesh_manager.h` and `vkr_geometry_system.h`

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        VkrEffectSystem                              │
├─────────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │
│  │ Effect Defs  │  │ Effect Inst  │  │ Deformed     │              │
│  │ (registry)   │  │ (per entity) │  │ Buffers      │              │
│  └──────────────┘  └──────────────┘  └──────────────┘              │
│         │                  │                  │                     │
│         ▼                  ▼                  ▼                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                    Effect Execution                          │  │
│  │  1. Query tagged entities/submeshes                          │  │
│  │  2. For each: copy original → deformed buffer (if needed)    │  │
│  │  3. Dispatch compute shader with deformed buffer             │  │
│  │  4. Insert barrier (compute → vertex)                        │  │
│  │  5. Render uses deformed buffer as vertex source             │  │
│  └──────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

## Data Structures

### Effect Definition

```c
// lib/src/renderer/systems/vkr_effect_system.h

/**
 * @brief Unique handle to an effect definition.
 */
typedef struct VkrEffectHandle {
  uint32_t id;
  uint32_t generation;
} VkrEffectHandle;

#define VKR_EFFECT_HANDLE_INVALID ((VkrEffectHandle){0, 0})

/**
 * @brief Effect execution phase within the frame.
 */
typedef enum VkrEffectPhase {
  VKR_EFFECT_PHASE_PRE_RENDER,   // Before any graphics rendering
  VKR_EFFECT_PHASE_POST_WORLD,   // After world pass, before UI
  VKR_EFFECT_PHASE_POST_RENDER,  // After all rendering (for readback)
} VkrEffectPhase;

/**
 * @brief Effect parameter type for uniform data.
 */
typedef enum VkrEffectParamType {
  VKR_EFFECT_PARAM_FLOAT,
  VKR_EFFECT_PARAM_FLOAT2,
  VKR_EFFECT_PARAM_FLOAT3,
  VKR_EFFECT_PARAM_FLOAT4,
  VKR_EFFECT_PARAM_INT,
  VKR_EFFECT_PARAM_UINT,
  VKR_EFFECT_PARAM_MAT4,
} VkrEffectParamType;

/**
 * @brief Effect parameter definition.
 */
typedef struct VkrEffectParamDef {
  const char *name;
  VkrEffectParamType type;
  uint32_t offset;          // Offset in parameter block
  uint32_t size;            // Size in bytes
  union {
    float default_float[4];
    int32_t default_int;
    uint32_t default_uint;
  } default_value;
} VkrEffectParamDef;

/**
 * @brief Effect definition describing a compute-based effect.
 *
 * @param name Unique effect name for lookup
 * @param compute_shader_path Path to compiled compute shader
 * @param target_tags Tags that this effect applies to (OR match)
 * @param exclude_tags Tags that exclude meshes from this effect
 * @param phase When to execute this effect in the frame
 * @param priority Execution order within phase (lower = earlier)
 * @param workgroup_size Compute shader workgroup dimensions
 * @param param_defs Array of parameter definitions
 * @param param_count Number of parameters
 * @param param_block_size Total size of parameter uniform block
 */
typedef struct VkrEffectDefinition {
  const char *name;
  String8 compute_shader_path;
  String8 compute_entry_point;

  VkrTagMask target_tags;
  VkrTagMask exclude_tags;

  VkrEffectPhase phase;
  int32_t priority;

  uint32_t workgroup_size[3];  // [x, y, z]

  const VkrEffectParamDef *param_defs;
  uint32_t param_count;
  uint32_t param_block_size;

  // Optional: custom dispatch callback for complex effects
  void (*custom_dispatch)(struct VkrEffectSystem *system,
                          VkrEffectHandle effect,
                          struct VkrEffectInstance *instance,
                          void *user_data);
  void *custom_dispatch_data;
} VkrEffectDefinition;
```

### Effect Instance

```c
/**
 * @brief Per-entity effect instance with parameters and state.
 */
typedef struct VkrEffectInstance {
  VkrEffectHandle effect;       // Which effect definition
  VkrEntity entity;             // Which entity (or VKR_ENTITY_INVALID for global)
  uint32_t submesh_index;       // Which submesh (UINT32_MAX for all)

  void *param_data;             // Allocated parameter block
  uint32_t param_data_size;

  // Runtime state
  bool8_t enabled;
  bool8_t first_frame;          // True on first execution (for initialization)
  float32_t elapsed_time;       // Time since effect started

  // Deformed buffer reference (managed by effect system)
  VkrStorageBufferHandle deformed_buffer;
  uint32_t vertex_count;
} VkrEffectInstance;
```

### Effect System

```c
/**
 * @brief Configuration for effect system initialization.
 */
typedef struct VkrEffectSystemConfig {
  uint32_t max_effect_definitions;
  uint32_t max_effect_instances;
  uint32_t max_deformed_buffers;
  uint64_t deformed_buffer_pool_size;  // Total bytes for deformed buffers
} VkrEffectSystemConfig;

#define VKR_EFFECT_SYSTEM_CONFIG_DEFAULT \
  ((VkrEffectSystemConfig){              \
      .max_effect_definitions = 32,      \
      .max_effect_instances = 256,       \
      .max_deformed_buffers = 128,       \
      .deformed_buffer_pool_size = MB(64) \
  })

/**
 * @brief Deformed buffer entry for vertex modification.
 */
typedef struct VkrDeformedBuffer {
  VkrStorageBufferHandle buffer;
  VkrGeometryHandle source_geometry;   // Original geometry
  uint32_t vertex_count;
  uint32_t vertex_stride;
  uint64_t size;
  bool8_t in_use;
  uint64_t last_frame_used;
} VkrDeformedBuffer;

/**
 * @brief Effect execution batch for efficient dispatch.
 */
typedef struct VkrEffectBatch {
  VkrEffectHandle effect;
  VkrEffectInstance **instances;
  uint32_t instance_count;
} VkrEffectBatch;

/**
 * @brief Main effect system state.
 */
typedef struct VkrEffectSystem {
  Arena *arena;
  Arena *scratch_arena;
  VkrAllocator allocator;
  VkrAllocator scratch_allocator;

  VkrRendererFrontendHandle renderer;
  VkrMeshManager *mesh_manager;
  VkrGeometrySystem *geometry_system;
  struct VkrScene *scene;

  VkrEffectSystemConfig config;

  // Effect definitions (registry)
  Array_VkrEffectDef definitions;
  VkrHashTable_VkrEffectDefEntry definitions_by_name;
  Array_uint32_t definition_free_list;
  uint32_t definition_count;
  uint32_t next_definition_generation;

  // Effect instances
  Array_VkrEffectInstance instances;
  Array_uint32_t instance_free_list;
  uint32_t instance_count;

  // Deformed buffer pool
  Array_VkrDeformedBuffer deformed_buffers;
  uint64_t deformed_buffer_used;

  // Per-frame execution state
  Array_VkrEffectBatch batches;
  bool8_t in_effect_pass;

  // Statistics
  struct {
    uint32_t total_dispatches;
    uint32_t total_vertices_processed;
    uint32_t buffers_allocated;
    uint32_t buffers_reused;
  } stats;
} VkrEffectSystem;
```

## API

### System Lifecycle

```c
// lib/src/renderer/systems/vkr_effect_system.h

/**
 * @brief Initialize the effect system.
 * @param system Effect system to initialize
 * @param renderer Renderer handle
 * @param mesh_manager Mesh manager for geometry access
 * @param geometry_system Geometry system for buffer operations
 * @param config Configuration (NULL for defaults)
 * @return true on success
 */
bool8_t vkr_effect_system_init(
    VkrEffectSystem *system,
    VkrRendererFrontendHandle renderer,
    VkrMeshManager *mesh_manager,
    VkrGeometrySystem *geometry_system,
    const VkrEffectSystemConfig *config);

/**
 * @brief Shutdown effect system and release resources.
 */
void vkr_effect_system_shutdown(VkrEffectSystem *system);

/**
 * @brief Set scene for tag queries (call when scene changes).
 */
void vkr_effect_system_set_scene(VkrEffectSystem *system, struct VkrScene *scene);

/**
 * @brief Update effect system (call once per frame before rendering).
 * @param system Effect system
 * @param delta_time Frame delta time in seconds
 */
void vkr_effect_system_update(VkrEffectSystem *system, float32_t delta_time);

/**
 * @brief Execute effects for a phase.
 * @param system Effect system
 * @param phase Which phase to execute
 *
 * Called by renderer at appropriate points in the frame.
 */
void vkr_effect_system_execute(VkrEffectSystem *system, VkrEffectPhase phase);
```

### Effect Registration

```c
/**
 * @brief Register an effect definition.
 * @param system Effect system
 * @param def Effect definition
 * @param out_handle Output handle
 * @return true on success
 */
bool8_t vkr_effect_system_register(
    VkrEffectSystem *system,
    const VkrEffectDefinition *def,
    VkrEffectHandle *out_handle);

/**
 * @brief Unregister an effect definition.
 * Also removes all instances using this effect.
 */
bool8_t vkr_effect_system_unregister(
    VkrEffectSystem *system,
    VkrEffectHandle handle);

/**
 * @brief Find effect by name.
 * @param system Effect system
 * @param name Effect name
 * @return Effect handle or VKR_EFFECT_HANDLE_INVALID if not found
 */
VkrEffectHandle vkr_effect_system_find(
    VkrEffectSystem *system,
    const char *name);
```

### Effect Instances

```c
/**
 * @brief Create effect instance for an entity.
 * @param system Effect system
 * @param effect Effect definition handle
 * @param entity Target entity (VKR_ENTITY_INVALID for tag-based matching)
 * @param submesh_index Target submesh (UINT32_MAX for all submeshes)
 * @param out_instance Output instance pointer
 * @return true on success
 */
bool8_t vkr_effect_system_instantiate(
    VkrEffectSystem *system,
    VkrEffectHandle effect,
    VkrEntity entity,
    uint32_t submesh_index,
    VkrEffectInstance **out_instance);

/**
 * @brief Create effect instances for all entities matching tags.
 * @param system Effect system
 * @param effect Effect definition handle
 * @param out_count Number of instances created
 * @return true on success
 *
 * Uses effect definition's target_tags and exclude_tags.
 */
bool8_t vkr_effect_system_instantiate_by_tags(
    VkrEffectSystem *system,
    VkrEffectHandle effect,
    uint32_t *out_count);

/**
 * @brief Destroy effect instance.
 */
void vkr_effect_system_destroy_instance(
    VkrEffectSystem *system,
    VkrEffectInstance *instance);

/**
 * @brief Set effect parameter by name.
 * @param instance Effect instance
 * @param param_name Parameter name
 * @param value Pointer to value data
 * @return true if parameter found and set
 */
bool8_t vkr_effect_instance_set_param(
    VkrEffectInstance *instance,
    const char *param_name,
    const void *value);

/**
 * @brief Set effect parameter by index (faster).
 */
bool8_t vkr_effect_instance_set_param_index(
    VkrEffectInstance *instance,
    uint32_t param_index,
    const void *value);

/**
 * @brief Enable/disable effect instance.
 */
void vkr_effect_instance_set_enabled(
    VkrEffectInstance *instance,
    bool8_t enabled);
```

### Deformed Buffer Management

```c
/**
 * @brief Get or create deformed buffer for geometry.
 * @param system Effect system
 * @param geometry Source geometry to deform
 * @param out_buffer Output deformed buffer handle
 * @return true on success
 *
 * If buffer already exists for this geometry, returns existing.
 * Otherwise allocates new buffer and copies original vertex data.
 */
bool8_t vkr_effect_system_get_deformed_buffer(
    VkrEffectSystem *system,
    VkrGeometryHandle geometry,
    VkrDeformedBuffer **out_buffer);

/**
 * @brief Reset deformed buffer to original geometry data.
 * Call at start of frame if effect needs clean state.
 */
void vkr_effect_system_reset_deformed_buffer(
    VkrEffectSystem *system,
    VkrDeformedBuffer *buffer);

/**
 * @brief Bind deformed buffer as vertex source for rendering.
 * @param system Effect system
 * @param buffer Deformed buffer
 * @param command_buffer Vulkan command buffer
 *
 * Should be called instead of normal vertex buffer bind.
 */
void vkr_effect_system_bind_deformed_buffer(
    VkrEffectSystem *system,
    VkrDeformedBuffer *buffer,
    VkCommandBuffer command_buffer);
```

## Implementation

### Step 1: Create Effect System Header

Create `lib/src/renderer/systems/vkr_effect_system.h` with types above.

### Step 2: Effect System Initialization

Create `lib/src/renderer/systems/vkr_effect_system.c`:

```c
#include "renderer/systems/vkr_effect_system.h"
#include "core/logger.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/systems/vkr_tag_system.h"

bool8_t vkr_effect_system_init(
    VkrEffectSystem *system,
    VkrRendererFrontendHandle renderer,
    VkrMeshManager *mesh_manager,
    VkrGeometrySystem *geometry_system,
    const VkrEffectSystemConfig *config) {

  assert_log(system != NULL, "System is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(mesh_manager != NULL, "Mesh manager is NULL");
  assert_log(geometry_system != NULL, "Geometry system is NULL");

  MemZero(system, sizeof(*system));
  system->renderer = renderer;
  system->mesh_manager = mesh_manager;
  system->geometry_system = geometry_system;
  system->config = config ? *config : VKR_EFFECT_SYSTEM_CONFIG_DEFAULT;

  // Create arenas
  ArenaFlags flags = bitset8_create();
  system->arena = arena_create(MB(4), MB(1), flags);
  system->scratch_arena = arena_create(MB(1), MB(1), flags);
  if (!system->arena || !system->scratch_arena) {
    log_fatal("Failed to create effect system arenas");
    vkr_effect_system_shutdown(system);
    return false_v;
  }

  system->allocator = (VkrAllocator){.ctx = system->arena};
  vkr_allocator_arena(&system->allocator);
  system->scratch_allocator = (VkrAllocator){.ctx = system->scratch_arena};
  vkr_allocator_arena(&system->scratch_allocator);

  // Initialize arrays
  system->definitions = array_create_VkrEffectDef(
      &system->allocator, system->config.max_effect_definitions);
  system->definition_free_list = array_create_uint32_t(
      &system->allocator, system->config.max_effect_definitions);
  system->definitions_by_name = vkr_hash_table_create_VkrEffectDefEntry(
      &system->allocator, system->config.max_effect_definitions * 2);

  system->instances = array_create_VkrEffectInstance(
      &system->allocator, system->config.max_effect_instances);
  system->instance_free_list = array_create_uint32_t(
      &system->allocator, system->config.max_effect_instances);

  system->deformed_buffers = array_create_VkrDeformedBuffer(
      &system->allocator, system->config.max_deformed_buffers);

  system->batches = array_create_VkrEffectBatch(
      &system->allocator, system->config.max_effect_definitions);

  system->next_definition_generation = 1;

  return true_v;
}

void vkr_effect_system_shutdown(VkrEffectSystem *system) {
  if (!system) return;

  // Destroy all deformed buffers
  for (uint32_t i = 0; i < system->deformed_buffers.length; i++) {
    VkrDeformedBuffer *buf = &system->deformed_buffers.data[i];
    if (buf->buffer) {
      vkr_renderer_destroy_storage_buffer(system->renderer, buf->buffer);
    }
  }

  // Destroy compute pipelines for effects
  for (uint32_t i = 0; i < system->definitions.length; i++) {
    VkrEffectDefInternal *def = &system->definitions.data[i];
    if (def->compute_pipeline) {
      vkr_renderer_destroy_compute_pipeline(system->renderer,
                                             def->compute_pipeline);
    }
  }

  if (system->arena) arena_destroy(system->arena);
  if (system->scratch_arena) arena_destroy(system->scratch_arena);
  MemZero(system, sizeof(*system));
}
```

### Step 3: Effect Registration

```c
// Internal definition storage
typedef struct VkrEffectDefInternal {
  VkrEffectHandle handle;
  VkrEffectDefinition def;
  VkrComputePipelineHandle compute_pipeline;
  char *name_storage;  // Owned copy of name
  bool8_t valid;
} VkrEffectDefInternal;

bool8_t vkr_effect_system_register(
    VkrEffectSystem *system,
    const VkrEffectDefinition *def,
    VkrEffectHandle *out_handle) {

  assert_log(system != NULL, "System is NULL");
  assert_log(def != NULL, "Definition is NULL");
  assert_log(def->name != NULL, "Effect name is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");

  *out_handle = VKR_EFFECT_HANDLE_INVALID;

  // Check if name already registered
  if (vkr_hash_table_get_VkrEffectDefEntry(&system->definitions_by_name,
                                            def->name)) {
    log_error("Effect '%s' already registered", def->name);
    return false_v;
  }

  // Find free slot
  uint32_t slot;
  if (system->definition_free_list.length > 0) {
    slot = system->definition_free_list.data[--system->definition_free_list.length];
  } else {
    if (system->definition_count >= system->definitions.length) {
      log_error("Effect definition limit reached");
      return false_v;
    }
    slot = system->definition_count++;
  }

  VkrEffectDefInternal *internal = &system->definitions.data[slot];
  internal->handle.id = slot + 1;
  internal->handle.generation = system->next_definition_generation++;
  internal->def = *def;
  internal->valid = true_v;

  // Copy name
  size_t name_len = strlen(def->name) + 1;
  internal->name_storage = vkr_allocator_alloc(
      &system->allocator, name_len, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  memcpy(internal->name_storage, def->name, name_len);
  internal->def.name = internal->name_storage;

  // Create compute pipeline
  VkrComputePipelineDescription pipeline_desc = {
      .shader_path = def->compute_shader_path,
      .entry_point = def->compute_entry_point.str
                         ? def->compute_entry_point
                         : string8_lit("computeMain"),
      .storage_buffer_count = 1,  // Deformed vertex buffer
      .uniform_buffer_count = 1,  // Effect parameters
      .push_constant_size = sizeof(uint32_t) * 2,  // vertex_count, frame_index
  };

  VkrRendererError err;
  internal->compute_pipeline = vkr_renderer_create_compute_pipeline(
      system->renderer, &pipeline_desc, &err);

  if (!internal->compute_pipeline) {
    log_error("Failed to create compute pipeline for effect '%s': %d",
              def->name, err);
    internal->valid = false_v;
    system->definition_free_list.data[system->definition_free_list.length++] = slot;
    return false_v;
  }

  // Add to name lookup
  VkrEffectDefEntry entry = {.slot = slot};
  vkr_hash_table_insert_VkrEffectDefEntry(&system->definitions_by_name,
                                           internal->name_storage, entry);

  *out_handle = internal->handle;
  log_debug("Registered effect '%s' with handle %u:%u",
            def->name, internal->handle.id, internal->handle.generation);

  return true_v;
}

VkrEffectHandle vkr_effect_system_find(
    VkrEffectSystem *system,
    const char *name) {

  VkrEffectDefEntry *entry = vkr_hash_table_get_VkrEffectDefEntry(
      &system->definitions_by_name, name);
  if (!entry) return VKR_EFFECT_HANDLE_INVALID;

  VkrEffectDefInternal *def = &system->definitions.data[entry->slot];
  if (!def->valid) return VKR_EFFECT_HANDLE_INVALID;

  return def->handle;
}
```

### Step 4: Effect Instantiation

```c
bool8_t vkr_effect_system_instantiate(
    VkrEffectSystem *system,
    VkrEffectHandle effect,
    VkrEntity entity,
    uint32_t submesh_index,
    VkrEffectInstance **out_instance) {

  assert_log(system != NULL, "System is NULL");
  assert_log(out_instance != NULL, "Out instance is NULL");

  *out_instance = NULL;

  // Validate effect handle
  if (effect.id == 0 || effect.id - 1 >= system->definitions.length) {
    return false_v;
  }
  VkrEffectDefInternal *def = &system->definitions.data[effect.id - 1];
  if (!def->valid || def->handle.generation != effect.generation) {
    return false_v;
  }

  // Find free instance slot
  uint32_t slot;
  if (system->instance_free_list.length > 0) {
    slot = system->instance_free_list.data[--system->instance_free_list.length];
  } else {
    if (system->instance_count >= system->instances.length) {
      log_error("Effect instance limit reached");
      return false_v;
    }
    slot = system->instance_count++;
  }

  VkrEffectInstance *instance = &system->instances.data[slot];
  MemZero(instance, sizeof(*instance));

  instance->effect = effect;
  instance->entity = entity;
  instance->submesh_index = submesh_index;
  instance->enabled = true_v;
  instance->first_frame = true_v;
  instance->elapsed_time = 0.0f;

  // Allocate parameter block
  if (def->def.param_block_size > 0) {
    instance->param_data = vkr_allocator_alloc(
        &system->allocator, def->def.param_block_size,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    instance->param_data_size = def->def.param_block_size;

    // Initialize with defaults
    for (uint32_t i = 0; i < def->def.param_count; i++) {
      const VkrEffectParamDef *param = &def->def.param_defs[i];
      void *dest = (uint8_t *)instance->param_data + param->offset;
      memcpy(dest, &param->default_value, param->size);
    }
  }

  *out_instance = instance;
  return true_v;
}

bool8_t vkr_effect_system_instantiate_by_tags(
    VkrEffectSystem *system,
    VkrEffectHandle effect,
    uint32_t *out_count) {

  assert_log(system != NULL, "System is NULL");
  assert_log(out_count != NULL, "Out count is NULL");

  *out_count = 0;

  if (!system->scene) {
    log_warn("No scene set for tag-based effect instantiation");
    return false_v;
  }

  // Validate effect
  if (effect.id == 0 || effect.id - 1 >= system->definitions.length) {
    return false_v;
  }
  VkrEffectDefInternal *def = &system->definitions.data[effect.id - 1];
  if (!def->valid) return false_v;

  // Query entities with matching tags
  VkrAllocatorScope scope = vkr_allocator_begin_scope(&system->scratch_allocator);
  VkrTagQueryResult query;

  if (!vkr_tag_query_entities(system->scene, def->def.target_tags,
                               def->def.exclude_tags, &system->scratch_allocator,
                               &query)) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    return false_v;
  }

  // Create instance for each matching entity
  for (uint32_t i = 0; i < query.count; i++) {
    VkrEntity entity = (VkrEntity)query.entity_ids[i];
    VkrEffectInstance *instance;

    if (vkr_effect_system_instantiate(system, effect, entity, UINT32_MAX,
                                       &instance)) {
      (*out_count)++;
    }
  }

  vkr_tag_query_result_free(&query, &system->scratch_allocator);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);

  return *out_count > 0;
}
```

### Step 5: Effect Execution

```c
void vkr_effect_system_update(VkrEffectSystem *system, float32_t delta_time) {
  assert_log(system != NULL, "System is NULL");

  // Update elapsed time for all instances
  for (uint32_t i = 0; i < system->instance_count; i++) {
    VkrEffectInstance *instance = &system->instances.data[i];
    if (instance->enabled && instance->effect.id != 0) {
      instance->elapsed_time += delta_time;
    }
  }

  // Reset deformed buffer usage flags
  for (uint32_t i = 0; i < system->deformed_buffers.length; i++) {
    system->deformed_buffers.data[i].in_use = false_v;
  }
}

void vkr_effect_system_execute(VkrEffectSystem *system, VkrEffectPhase phase) {
  assert_log(system != NULL, "System is NULL");

  if (system->instance_count == 0) return;

  // Begin compute pass
  if (!vkr_renderer_begin_compute(system->renderer)) {
    log_error("Failed to begin compute pass");
    return;
  }
  system->in_effect_pass = true_v;

  // Build batches by effect (same compute pipeline)
  // For simplicity, process one effect at a time
  for (uint32_t def_idx = 0; def_idx < system->definition_count; def_idx++) {
    VkrEffectDefInternal *def = &system->definitions.data[def_idx];
    if (!def->valid || def->def.phase != phase) continue;

    // Find instances using this effect
    for (uint32_t inst_idx = 0; inst_idx < system->instance_count; inst_idx++) {
      VkrEffectInstance *instance = &system->instances.data[inst_idx];

      if (!instance->enabled || instance->effect.id != def->handle.id ||
          instance->effect.generation != def->handle.generation) {
        continue;
      }

      // Execute effect for this instance
      vkr__execute_effect_instance(system, def, instance);
      instance->first_frame = false_v;
    }
  }

  // End compute pass
  vkr_renderer_end_compute(system->renderer);
  system->in_effect_pass = false_v;
}

vkr_internal void vkr__execute_effect_instance(
    VkrEffectSystem *system,
    VkrEffectDefInternal *def,
    VkrEffectInstance *instance) {

  // Get geometry for this entity
  VkrGeometryHandle geometry = {0};
  uint32_t vertex_count = 0;

  if (instance->entity != VKR_ENTITY_INVALID) {
    // Get mesh renderer component
    SceneMeshRenderer *mesh_comp = vkr_world_get_component(
        &system->scene->world, instance->entity, SceneMeshRenderer);
    if (!mesh_comp) return;

    VkrMesh *mesh = vkr_mesh_manager_get(system->mesh_manager,
                                          mesh_comp->mesh_index);
    if (!mesh) return;

    // Get first matching submesh geometry
    uint32_t submesh_idx = instance->submesh_index;
    if (submesh_idx == UINT32_MAX) submesh_idx = 0;

    if (submesh_idx < mesh->submeshes.length) {
      VkrSubMesh *submesh = &mesh->submeshes.data[submesh_idx];
      geometry = submesh->geometry;
    }
  }

  if (geometry.id == 0) return;

  // Get geometry details
  VkrGeometry *geo = vkr_geometry_system_get_by_handle(
      system->geometry_system, geometry);
  if (!geo) return;

  vertex_count = geo->vertex_count;
  if (vertex_count == 0) return;

  // Get or create deformed buffer
  VkrDeformedBuffer *deformed;
  if (!vkr_effect_system_get_deformed_buffer(system, geometry, &deformed)) {
    log_error("Failed to get deformed buffer for effect");
    return;
  }

  // Copy original data on first frame
  if (instance->first_frame) {
    vkr_effect_system_reset_deformed_buffer(system, deformed);
  }

  instance->deformed_buffer = deformed->buffer;
  instance->vertex_count = vertex_count;
  deformed->in_use = true_v;

  // Bind compute pipeline
  vkr_renderer_bind_compute_pipeline(system->renderer, def->compute_pipeline);

  // Bind storage buffer (deformed vertices)
  vkr_renderer_bind_storage_buffer(system->renderer, def->compute_pipeline,
                                    0, deformed->buffer);

  // Update uniform buffer with parameters + time
  struct {
    float time;
    uint32_t vertex_count;
    // Rest of param_data follows
  } uniform_header = {
      .time = instance->elapsed_time,
      .vertex_count = vertex_count,
  };

  // TODO: Combine with instance->param_data and upload

  // Push constants
  struct {
    uint32_t vertex_count;
    uint32_t frame_index;
  } push_data = {
      .vertex_count = vertex_count,
      .frame_index = 0,  // TODO: get from renderer
  };

  // Calculate dispatch size
  uint32_t workgroup_x = def->def.workgroup_size[0];
  if (workgroup_x == 0) workgroup_x = 64;  // Default

  uint32_t group_count = (vertex_count + workgroup_x - 1) / workgroup_x;

  VkrComputeDispatch dispatch = {
      .group_count_x = group_count,
      .group_count_y = 1,
      .group_count_z = 1,
  };

  vkr_renderer_dispatch_compute(system->renderer, def->compute_pipeline,
                                 &dispatch, &push_data, sizeof(push_data));

  system->stats.total_dispatches++;
  system->stats.total_vertices_processed += vertex_count;
}
```

### Step 6: Deformed Buffer Management

```c
bool8_t vkr_effect_system_get_deformed_buffer(
    VkrEffectSystem *system,
    VkrGeometryHandle geometry,
    VkrDeformedBuffer **out_buffer) {

  assert_log(system != NULL, "System is NULL");
  assert_log(out_buffer != NULL, "Out buffer is NULL");

  *out_buffer = NULL;

  // Look for existing buffer
  for (uint32_t i = 0; i < system->deformed_buffers.length; i++) {
    VkrDeformedBuffer *buf = &system->deformed_buffers.data[i];
    if (buf->source_geometry.id == geometry.id &&
        buf->source_geometry.generation == geometry.generation) {
      *out_buffer = buf;
      system->stats.buffers_reused++;
      return true_v;
    }
  }

  // Get geometry info
  VkrGeometry *geo = vkr_geometry_system_get_by_handle(
      system->geometry_system, geometry);
  if (!geo) return false_v;

  uint64_t size = (uint64_t)geo->vertex_count * (uint64_t)geo->vertex_size;

  // Find free slot
  VkrDeformedBuffer *slot = NULL;
  for (uint32_t i = 0; i < system->deformed_buffers.length; i++) {
    if (system->deformed_buffers.data[i].buffer == NULL) {
      slot = &system->deformed_buffers.data[i];
      break;
    }
  }

  if (!slot) {
    log_error("Deformed buffer limit reached");
    return false_v;
  }

  // Check pool size limit
  if (system->deformed_buffer_used + size >
      system->config.deformed_buffer_pool_size) {
    log_error("Deformed buffer pool exhausted");
    return false_v;
  }

  // Create storage buffer
  VkrRendererError err;
  VkrStorageBufferHandle buffer = vkr_renderer_create_storage_buffer(
      system->renderer, size,
      VKR_BUFFER_USAGE_COMPUTE_STORAGE | VKR_BUFFER_USAGE_VERTEX_SOURCE |
          VKR_BUFFER_USAGE_TRANSFER_DST,
      VKR_BUFFER_FLAG_DEVICE_LOCAL, &err);

  if (!buffer) {
    log_error("Failed to create deformed buffer: %d", err);
    return false_v;
  }

  slot->buffer = buffer;
  slot->source_geometry = geometry;
  slot->vertex_count = geo->vertex_count;
  slot->vertex_stride = geo->vertex_size;
  slot->size = size;
  slot->in_use = false_v;
  slot->last_frame_used = 0;

  system->deformed_buffer_used += size;
  system->stats.buffers_allocated++;

  *out_buffer = slot;
  return true_v;
}

void vkr_effect_system_reset_deformed_buffer(
    VkrEffectSystem *system,
    VkrDeformedBuffer *buffer) {

  if (!buffer || !buffer->buffer) return;

  // Copy original vertex data to deformed buffer
  VkrGeometry *geo = vkr_geometry_system_get_by_handle(
      system->geometry_system, buffer->source_geometry);
  if (!geo) return;

  // Use staging buffer and transfer command
  // TODO: Implement via renderer copy API
  vkr_renderer_copy_vertex_to_storage(system->renderer,
                                       geo->vertex_buffer,
                                       buffer->buffer,
                                       buffer->size);
}
```

## Integration Points

### Renderer Integration

In `lib/src/renderer/renderer_frontend.c`:

```c
// During packet construction, publish the typed effects payload for this frame.
vkr_effect_system_update(&frontend->effect_system, delta_time);

// Declare named effects passes in the JSON graph. A pre-world executor consumes
// the packet payload, declares/writes the deformed buffers, and runs before
// pass.world through resource dependencies. A post-world effect is a separate
// declared pass before pass.ui.
```

### Mesh Manager Integration

When binding vertex buffer for drawing, check for deformed buffer:

```c
// In world pass rendering (vkr_pass_world.c):
VkrDeformedBuffer *deformed = vkr_effect_system_get_deformed_if_exists(
    effect_system, submesh->geometry);

if (deformed && deformed->in_use) {
  // Bind deformed buffer instead of original
  vkr_effect_system_bind_deformed_buffer(effect_system, deformed, cmd_buffer);
} else {
  // Bind original vertex buffer
  vkr_renderer_bind_vertex_buffer(renderer, geo->vertex_buffer);
}
```

## Testing

### Unit Test

```c
void test_effect_registration(void) {
  VkrEffectSystem system;
  // Initialize...

  VkrEffectParamDef params[] = {
      {.name = "amplitude",
       .type = VKR_EFFECT_PARAM_FLOAT,
       .offset = 0,
       .size = sizeof(float),
       .default_value.default_float = {0.1f}},
      {.name = "frequency",
       .type = VKR_EFFECT_PARAM_FLOAT,
       .offset = 4,
       .size = sizeof(float),
       .default_value.default_float = {1.0f}},
  };

  VkrEffectDefinition def = {
      .name = "test_wave",
      .compute_shader_path = string8_lit("assets/shaders/effects/wave.comp.spv"),
      .target_tags = VKR_TAG_FABRIC,
      .phase = VKR_EFFECT_PHASE_PRE_RENDER,
      .workgroup_size = {64, 1, 1},
      .param_defs = params,
      .param_count = 2,
      .param_block_size = 8,
  };

  VkrEffectHandle handle;
  assert(vkr_effect_system_register(&system, &def, &handle));
  assert(handle.id != 0);

  VkrEffectHandle found = vkr_effect_system_find(&system, "test_wave");
  assert(found.id == handle.id);
  assert(found.generation == handle.generation);
}

void test_effect_instantiation(void) {
  VkrEffectSystem system;
  VkrEffectHandle effect;
  // Initialize and register...

  VkrEffectInstance *instance;
  assert(vkr_effect_system_instantiate(&system, effect, VKR_ENTITY_INVALID,
                                        UINT32_MAX, &instance));
  assert(instance != NULL);
  assert(instance->enabled);

  // Set parameter
  float amplitude = 0.5f;
  assert(vkr_effect_instance_set_param(instance, "amplitude", &amplitude));
}
```

## Completion Criteria

- [ ] `VkrEffectSystem` struct defined with all fields
- [ ] Effect system initializes and shuts down cleanly
- [ ] Effect definitions register with compute pipeline creation
- [ ] Effect instances create with parameter blocks
- [ ] Tag-based instantiation finds matching entities
- [ ] Effect execution dispatches compute shaders
- [ ] Deformed buffers allocated and managed
- [ ] Memory barriers inserted correctly
- [ ] Integration hooks in renderer frontend

## Next Steps

After completing this phase, proceed to:
- **04-wave-effect-demo.md**: Implement concrete wave effect
- **05-scene-integration.md**: Add scene JSON support for effects
