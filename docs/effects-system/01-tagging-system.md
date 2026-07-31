---
status: proposed
updated: 2026-07-31
authority: design
---
# Phase 1: Entity/Mesh Tagging System

## Overview

Implement a tagging system that allows marking entities and submeshes with effect-related tags. Tags are 64-bit bitmasks enabling efficient filtering and selection during effect application.

## Prerequisites

- Understanding of `vkr_scene_system.h/c` ECS architecture
- Understanding of `vkr_mesh_manager.h/c` submesh structure
- Familiarity with scene JSON format (`assets/scenes/*.scene.json`)

## Design

### Tag Representation

```c
// lib/src/renderer/systems/vkr_tag_system.h

typedef uint64_t VkrTagMask;

// Built-in effect tags (bits 0-15)
#define VKR_TAG_NONE           ((VkrTagMask)0)
#define VKR_TAG_FABRIC         ((VkrTagMask)1 << 0)   // Cloth, curtains, flags
#define VKR_TAG_WATER          ((VkrTagMask)1 << 1)   // Water surfaces
#define VKR_TAG_FOLIAGE        ((VkrTagMask)1 << 2)   // Leaves, grass, plants
#define VKR_TAG_GLASS          ((VkrTagMask)1 << 3)   // Transparent glass
#define VKR_TAG_METAL          ((VkrTagMask)1 << 4)   // Metallic surfaces
#define VKR_TAG_EMISSIVE       ((VkrTagMask)1 << 5)   // Light-emitting
#define VKR_TAG_ANIMATED       ((VkrTagMask)1 << 6)   // Generic animation target
#define VKR_TAG_DESTRUCTIBLE   ((VkrTagMask)1 << 7)   // Can be destroyed
#define VKR_TAG_INTERACTIVE    ((VkrTagMask)1 << 8)   // Player-interactable
#define VKR_TAG_PHYSICS        ((VkrTagMask)1 << 9)   // Physics-simulated
#define VKR_TAG_PARTICLE_EMIT  ((VkrTagMask)1 << 10)  // Particle emitter attachment
#define VKR_TAG_RESERVED_11    ((VkrTagMask)1 << 11)
#define VKR_TAG_RESERVED_12    ((VkrTagMask)1 << 12)
#define VKR_TAG_RESERVED_13    ((VkrTagMask)1 << 13)
#define VKR_TAG_RESERVED_14    ((VkrTagMask)1 << 14)
#define VKR_TAG_RESERVED_15    ((VkrTagMask)1 << 15)

// User-defined tags (bits 16-47) - application defines these
#define VKR_TAG_USER_BASE      ((VkrTagMask)1 << 16)
#define VKR_TAG_USER(n)        ((VkrTagMask)1 << (16 + (n)))  // n in [0, 31]

// System tags (bits 48-63) - reserved for internal use
#define VKR_TAG_SYSTEM_BASE    ((VkrTagMask)1 << 48)
```

### Tag Operations

```c
// lib/src/renderer/systems/vkr_tag_system.h

// Check if mask contains all specified tags
static inline bool8_t vkr_tag_has_all(VkrTagMask mask, VkrTagMask required) {
  return (mask & required) == required ? true_v : false_v;
}

// Check if mask contains any of specified tags
static inline bool8_t vkr_tag_has_any(VkrTagMask mask, VkrTagMask required) {
  return (mask & required) != 0 ? true_v : false_v;
}

// Add tags to mask
static inline VkrTagMask vkr_tag_add(VkrTagMask mask, VkrTagMask tags) {
  return mask | tags;
}

// Remove tags from mask
static inline VkrTagMask vkr_tag_remove(VkrTagMask mask, VkrTagMask tags) {
  return mask & ~tags;
}

// Toggle tags in mask
static inline VkrTagMask vkr_tag_toggle(VkrTagMask mask, VkrTagMask tags) {
  return mask ^ tags;
}
```

### Scene Component

```c
// Addition to lib/src/renderer/systems/vkr_scene_system.h

/**
 * @brief Tag component for effect filtering.
 * @param tags Bitmask of active tags on this entity.
 * @param inherit_parent If true, parent tags are ORed with this entity's tags.
 */
typedef struct SceneTag {
  VkrTagMask tags;
  bool8_t inherit_parent;
} SceneTag;

// Component registration (in vkr_scene_system.c init)
// vkr_world_register_component(world, SceneTag);
```

### SubMesh Extension

```c
// Modification to VkrSubMesh in lib/src/renderer/resources/vkr_resources.h

typedef struct VkrSubMesh {
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  VkrPipelineHandle pipeline;
  VkrRendererInstanceStateHandle instance_state;
  VkrPipelineDomain pipeline_domain;
  String8 shader_override;
  bool8_t pipeline_dirty;
  bool8_t owns_geometry;
  bool8_t owns_material;
  uint64_t last_render_frame;

  // NEW: Effect tagging
  VkrTagMask effect_tags;  // Tags for this specific submesh
} VkrSubMesh;
```

## Implementation

### Step 1: Create Tag System Header

Create `lib/src/renderer/systems/vkr_tag_system.h`:

```c
/**
 * @file vkr_tag_system.h
 * @brief Tag system for entity/mesh effect filtering.
 *
 * Tags are 64-bit bitmasks that can be attached to entities and submeshes.
 * Effects query tags to determine which objects to process.
 */
#pragma once

#include "defines.h"
#include "containers/array.h"

// --- Tag Definitions (see above) ---

// --- Tag Operations (see above) ---

/**
 * @brief Named tag definition for JSON parsing.
 */
typedef struct VkrTagDefinition {
  const char *name;   // e.g., "fabric", "water"
  VkrTagMask mask;    // Corresponding bit(s)
} VkrTagDefinition;

/**
 * @brief Query result for tag-based entity filtering.
 */
typedef struct VkrTagQueryResult {
  uint32_t *entity_ids;   // Array of matching entity IDs
  uint32_t count;         // Number of matches
  uint32_t capacity;      // Allocated capacity
} VkrTagQueryResult;

/**
 * @brief Parse tag name string to bitmask.
 * @param name Tag name (e.g., "fabric", "water|foliage")
 * @param out_mask Output bitmask
 * @return true if parsing succeeded
 *
 * Supports pipe-separated multiple tags: "fabric|animated"
 */
bool8_t vkr_tag_parse_name(const char *name, VkrTagMask *out_mask);

/**
 * @brief Convert tag mask to human-readable string.
 * @param mask Tag bitmask
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of characters written (excluding null terminator)
 */
uint32_t vkr_tag_mask_to_string(VkrTagMask mask, char *buffer, uint32_t buffer_size);

/**
 * @brief Built-in tag name table for parsing/serialization.
 */
extern const VkrTagDefinition VKR_BUILTIN_TAGS[];
extern const uint32_t VKR_BUILTIN_TAG_COUNT;
```

### Step 2: Create Tag System Implementation

Create `lib/src/renderer/systems/vkr_tag_system.c`:

```c
#include "renderer/systems/vkr_tag_system.h"
#include "containers/str.h"
#include <string.h>

const VkrTagDefinition VKR_BUILTIN_TAGS[] = {
    {"fabric",       VKR_TAG_FABRIC},
    {"water",        VKR_TAG_WATER},
    {"foliage",      VKR_TAG_FOLIAGE},
    {"glass",        VKR_TAG_GLASS},
    {"metal",        VKR_TAG_METAL},
    {"emissive",     VKR_TAG_EMISSIVE},
    {"animated",     VKR_TAG_ANIMATED},
    {"destructible", VKR_TAG_DESTRUCTIBLE},
    {"interactive",  VKR_TAG_INTERACTIVE},
    {"physics",      VKR_TAG_PHYSICS},
    {"particle_emit", VKR_TAG_PARTICLE_EMIT},
};

const uint32_t VKR_BUILTIN_TAG_COUNT =
    sizeof(VKR_BUILTIN_TAGS) / sizeof(VKR_BUILTIN_TAGS[0]);

bool8_t vkr_tag_parse_name(const char *name, VkrTagMask *out_mask) {
  if (!name || !out_mask) return false_v;

  *out_mask = VKR_TAG_NONE;

  // Handle empty string
  if (name[0] == '\0') return true_v;

  // Copy for tokenization
  char buffer[256];
  size_t len = strlen(name);
  if (len >= sizeof(buffer)) return false_v;
  memcpy(buffer, name, len + 1);

  // Parse pipe-separated tags
  char *token = buffer;
  char *next = buffer;

  while (*next) {
    // Find next pipe or end
    while (*next && *next != '|') next++;

    bool8_t is_end = (*next == '\0');
    *next = '\0';

    // Skip leading whitespace
    while (*token == ' ' || *token == '\t') token++;

    // Trim trailing whitespace
    char *end = next - 1;
    while (end > token && (*end == ' ' || *end == '\t')) {
      *end = '\0';
      end--;
    }

    // Look up tag name
    bool8_t found = false_v;
    for (uint32_t i = 0; i < VKR_BUILTIN_TAG_COUNT; i++) {
      if (strcmp(token, VKR_BUILTIN_TAGS[i].name) == 0) {
        *out_mask |= VKR_BUILTIN_TAGS[i].mask;
        found = true_v;
        break;
      }
    }

    // Check for user tag format: "user_N" where N is 0-31
    if (!found && strncmp(token, "user_", 5) == 0) {
      int user_id = atoi(token + 5);
      if (user_id >= 0 && user_id < 32) {
        *out_mask |= VKR_TAG_USER((uint32_t)user_id);
        found = true_v;
      }
    }

    if (!found && token[0] != '\0') {
      return false_v;  // Unknown tag
    }

    if (is_end) break;
    next++;
    token = next;
  }

  return true_v;
}

uint32_t vkr_tag_mask_to_string(VkrTagMask mask, char *buffer,
                                 uint32_t buffer_size) {
  if (!buffer || buffer_size == 0) return 0;

  buffer[0] = '\0';
  uint32_t written = 0;
  bool8_t first = true_v;

  for (uint32_t i = 0; i < VKR_BUILTIN_TAG_COUNT; i++) {
    if (mask & VKR_BUILTIN_TAGS[i].mask) {
      const char *name = VKR_BUILTIN_TAGS[i].name;
      size_t name_len = strlen(name);
      size_t need = name_len + (first ? 0 : 1);  // +1 for pipe

      if (written + need + 1 > buffer_size) break;

      if (!first) {
        buffer[written++] = '|';
      }
      memcpy(buffer + written, name, name_len);
      written += (uint32_t)name_len;
      buffer[written] = '\0';
      first = false_v;
    }
  }

  // User tags
  for (uint32_t i = 0; i < 32; i++) {
    if (mask & VKR_TAG_USER(i)) {
      char user_tag[16];
      int len = snprintf(user_tag, sizeof(user_tag), "user_%u", i);
      if (len > 0) {
        size_t need = (size_t)len + (first ? 0 : 1);
        if (written + need + 1 > buffer_size) break;

        if (!first) buffer[written++] = '|';
        memcpy(buffer + written, user_tag, (size_t)len);
        written += (uint32_t)len;
        buffer[written] = '\0';
        first = false_v;
      }
    }
  }

  return written;
}
```

### Step 3: Add SceneTag Component

Modify `lib/src/renderer/systems/vkr_scene_system.h`:

```c
// Add after other component definitions

#include "renderer/systems/vkr_tag_system.h"

/**
 * @brief Tag component for effect filtering.
 * Tags propagate from parent to children when inherit_parent is true.
 */
typedef struct SceneTag {
  VkrTagMask tags;           // Direct tags on this entity
  VkrTagMask effective_tags; // Computed: tags | inherited parent tags
  bool8_t inherit_parent;    // If true, parent tags propagate
} SceneTag;
```

Modify `lib/src/renderer/systems/vkr_scene_system.c`:

```c
// In vkr_scene_init(), add component registration:
vkr_world_register_component(&scene->world, SceneTag);

// Add tag propagation in transform hierarchy update (vkr_scene_update):
// After world matrix computation, propagate tags:

static void vkr__propagate_tags(VkrScene *scene, VkrEntity entity) {
  SceneTag *tag = vkr_world_get_component(&scene->world, entity, SceneTag);
  if (!tag) return;

  if (tag->inherit_parent) {
    SceneTransform *transform =
        vkr_world_get_component(&scene->world, entity, SceneTransform);
    if (transform && transform->parent != VKR_ENTITY_INVALID) {
      SceneTag *parent_tag =
          vkr_world_get_component(&scene->world, transform->parent, SceneTag);
      if (parent_tag) {
        tag->effective_tags = tag->tags | parent_tag->effective_tags;
        return;
      }
    }
  }
  tag->effective_tags = tag->tags;
}
```

### Step 4: Extend Scene JSON Format

Update scene loading in `lib/src/renderer/resources/loaders/scene_loader.c`:

```c
// In entity parsing, after transform:

// Parse tags
cJSON *tags_json = cJSON_GetObjectItem(entity_json, "tags");
if (tags_json) {
  SceneTag tag_component = {
    .tags = VKR_TAG_NONE,
    .effective_tags = VKR_TAG_NONE,
    .inherit_parent = true_v  // Default to inherit
  };

  if (cJSON_IsString(tags_json)) {
    // Single string: "fabric|animated"
    vkr_tag_parse_name(tags_json->valuestring, &tag_component.tags);
  } else if (cJSON_IsArray(tags_json)) {
    // Array: ["fabric", "animated"]
    cJSON *tag_item;
    cJSON_ArrayForEach(tag_item, tags_json) {
      if (cJSON_IsString(tag_item)) {
        VkrTagMask parsed;
        if (vkr_tag_parse_name(tag_item->valuestring, &parsed)) {
          tag_component.tags |= parsed;
        }
      }
    }
  }

  // Check for inherit flag
  cJSON *inherit_json = cJSON_GetObjectItem(entity_json, "tags_inherit");
  if (cJSON_IsBool(inherit_json)) {
    tag_component.inherit_parent = cJSON_IsTrue(inherit_json) ? true_v : false_v;
  }

  vkr_world_add_component(&scene->world, entity, SceneTag, &tag_component);
}
```

### Step 5: Add VkrSubMesh Tags

Modify `lib/src/renderer/resources/vkr_resources.h` (VkrSubMesh already shown above).

Modify mesh loading in `lib/src/renderer/resources/loaders/mesh_loader.c`:

```c
// When creating submesh from material name, apply default tags based on material:

static VkrTagMask vkr__infer_tags_from_material_name(const char *material_name) {
  VkrTagMask tags = VKR_TAG_NONE;

  // Simple heuristics for common material naming conventions
  if (strstr(material_name, "fabric") || strstr(material_name, "cloth") ||
      strstr(material_name, "curtain") || strstr(material_name, "flag")) {
    tags |= VKR_TAG_FABRIC;
  }
  if (strstr(material_name, "water") || strstr(material_name, "liquid")) {
    tags |= VKR_TAG_WATER;
  }
  if (strstr(material_name, "leaf") || strstr(material_name, "plant") ||
      strstr(material_name, "grass") || strstr(material_name, "foliage")) {
    tags |= VKR_TAG_FOLIAGE;
  }
  if (strstr(material_name, "glass") || strstr(material_name, "window")) {
    tags |= VKR_TAG_GLASS;
  }
  if (strstr(material_name, "metal") || strstr(material_name, "iron") ||
      strstr(material_name, "steel") || strstr(material_name, "chrome")) {
    tags |= VKR_TAG_METAL;
  }
  if (strstr(material_name, "light") || strstr(material_name, "lamp") ||
      strstr(material_name, "emissive") || strstr(material_name, "glow")) {
    tags |= VKR_TAG_EMISSIVE;
  }

  return tags;
}

// In submesh creation:
submesh->effect_tags = vkr__infer_tags_from_material_name(material_name);
```

### Step 6: Tag Query API

Add to `lib/src/renderer/systems/vkr_tag_system.h`:

```c
/**
 * @brief Query entities with specific tags from scene.
 * @param scene Scene to query
 * @param required_tags All these tags must be present (AND)
 * @param excluded_tags None of these tags may be present
 * @param allocator Allocator for result array
 * @param result Output query result
 * @return true if query succeeded
 */
bool8_t vkr_tag_query_entities(
    struct VkrScene *scene,
    VkrTagMask required_tags,
    VkrTagMask excluded_tags,
    struct VkrAllocator *allocator,
    VkrTagQueryResult *result);

/**
 * @brief Query submeshes with specific tags from mesh manager.
 * @param manager Mesh manager to query
 * @param required_tags All these tags must be present (AND)
 * @param excluded_tags None of these tags may be present
 * @param out_mesh_indices Output array of mesh indices (caller allocates)
 * @param out_submesh_indices Output array of submesh indices within each mesh
 * @param max_results Maximum results to return
 * @param out_count Actual number of results
 * @return true if query succeeded
 */
bool8_t vkr_tag_query_submeshes(
    struct VkrMeshManager *manager,
    VkrTagMask required_tags,
    VkrTagMask excluded_tags,
    uint32_t *out_mesh_indices,
    uint32_t *out_submesh_indices,
    uint32_t max_results,
    uint32_t *out_count);

/**
 * @brief Free query result allocated memory.
 */
void vkr_tag_query_result_free(VkrTagQueryResult *result,
                                struct VkrAllocator *allocator);
```

## Scene JSON Example

```json
{
  "version": 2,
  "entities": [
    {
      "name": "Sponza",
      "parent": null,
      "transform": { "pos": [0, 0, -15], "rot": [0, 0, 0, 1], "scale": [0.0085, 0.0085, 0.0085] },
      "mesh": {
        "path": "assets/models/sponza.obj",
        "pipeline_domain": "world"
      },
      "tags": ["animated"],
      "tags_inherit": true
    },
    {
      "name": "SponzaCurtains",
      "parent": 0,
      "transform": { "pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1] },
      "tags": "fabric|animated",
      "tags_inherit": false
    }
  ]
}
```

## Testing

### Unit Tests

```c
// tests/test_tag_system.c

void test_tag_parsing(void) {
  VkrTagMask mask;

  // Single tag
  assert(vkr_tag_parse_name("fabric", &mask));
  assert(mask == VKR_TAG_FABRIC);

  // Multiple tags
  assert(vkr_tag_parse_name("fabric|water", &mask));
  assert(mask == (VKR_TAG_FABRIC | VKR_TAG_WATER));

  // With whitespace
  assert(vkr_tag_parse_name("fabric | water", &mask));
  assert(mask == (VKR_TAG_FABRIC | VKR_TAG_WATER));

  // User tag
  assert(vkr_tag_parse_name("user_5", &mask));
  assert(mask == VKR_TAG_USER(5));

  // Unknown tag fails
  assert(!vkr_tag_parse_name("unknown_tag", &mask));
}

void test_tag_operations(void) {
  VkrTagMask mask = VKR_TAG_FABRIC;

  // Has all
  assert(vkr_tag_has_all(mask, VKR_TAG_FABRIC));
  assert(!vkr_tag_has_all(mask, VKR_TAG_FABRIC | VKR_TAG_WATER));

  // Has any
  assert(vkr_tag_has_any(mask, VKR_TAG_FABRIC | VKR_TAG_WATER));
  assert(!vkr_tag_has_any(mask, VKR_TAG_WATER));

  // Add/remove
  mask = vkr_tag_add(mask, VKR_TAG_WATER);
  assert(mask == (VKR_TAG_FABRIC | VKR_TAG_WATER));

  mask = vkr_tag_remove(mask, VKR_TAG_FABRIC);
  assert(mask == VKR_TAG_WATER);
}

void test_tag_to_string(void) {
  char buffer[128];
  VkrTagMask mask = VKR_TAG_FABRIC | VKR_TAG_ANIMATED;

  vkr_tag_mask_to_string(mask, buffer, sizeof(buffer));
  // Should contain "fabric" and "animated" separated by pipe
  assert(strstr(buffer, "fabric") != NULL);
  assert(strstr(buffer, "animated") != NULL);
}
```

### Integration Test

```c
void test_scene_tag_loading(void) {
  VkrScene scene;
  VkrRendererError err;

  // Load scene with tagged entities
  vkr_scene_load(&scene, "assets/scenes/test_tags.scene.json", &err);

  // Query fabric-tagged entities
  VkrTagQueryResult result;
  VkrAllocator temp_alloc = /* ... */;

  vkr_tag_query_entities(&scene, VKR_TAG_FABRIC, VKR_TAG_NONE,
                          &temp_alloc, &result);

  assert(result.count > 0);

  // Verify inheritance
  VkrEntity child = result.entity_ids[0];
  SceneTag *tag = vkr_world_get_component(&scene.world, child, SceneTag);
  assert(vkr_tag_has_all(tag->effective_tags, VKR_TAG_FABRIC));

  vkr_tag_query_result_free(&result, &temp_alloc);
  vkr_scene_shutdown(&scene);
}
```

## Completion Criteria

- [ ] `vkr_tag_system.h/c` compiles without errors
- [ ] `SceneTag` component registered in scene system
- [ ] Tag parsing handles all built-in tags
- [ ] Scene JSON loading parses `tags` and `tags_inherit` fields
- [ ] Tag inheritance propagates correctly in hierarchy
- [ ] `vkr_tag_query_entities()` returns correct entities
- [ ] `vkr_tag_query_submeshes()` returns correct submeshes
- [ ] Mesh loader infers tags from material names
- [ ] All unit tests pass

## Next Steps

After completing this phase, proceed to:
- **02-compute-pipeline-support.md**: Add compute shader infrastructure
- **03-effects-system-design.md**: Build effect registry using tags
