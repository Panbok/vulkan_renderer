---
status: proposed
updated: 2026-07-31
authority: design
---
# Phase 5: Scene Integration

## Overview

Extend the scene JSON format and loader to support effect definitions and assignments. Effects are loaded with the scene, applied to tagged entities, and destroyed on scene unload.

## Prerequisites

- **Phase 1-4**: All previous phases implemented
- Understanding of `scene_loader.c` JSON parsing
- Understanding of `vkr_scene_system.c` lifecycle

## Scene JSON Schema Extension

### Effects Array (Scene Level)

Define global effects available in the scene:

```json
{
  "version": 3,
  "effects": [
    {
      "name": "curtain_wave",
      "type": "wave",
      "target_tags": "fabric",
      "exclude_tags": "",
      "phase": "pre_render",
      "priority": 0,
      "auto_instantiate": true,
      "params": {
        "amplitude": 0.12,
        "frequency": 2.5,
        "speed": 0.8,
        "phase_offset": 0.0,
        "wave_direction": [0.0, 0.0, 1.0],
        "wind_direction": [1.0, 0.0, 0.0],
        "wind_strength": 0.4,
        "vertical_bias": 0.5,
        "damping": 1.0,
        "noise_scale": 0.2
      }
    },
    {
      "name": "water_ripple",
      "type": "wave",
      "target_tags": "water",
      "params": {
        "amplitude": 0.05,
        "frequency": 4.0,
        "speed": 1.5,
        "vertical_bias": 1.0
      }
    }
  ],
  "entities": [
    // ...
  ]
}
```

### Per-Entity Effect Override

Override effect parameters per entity:

```json
{
  "name": "SpecialCurtain",
  "parent": 0,
  "transform": { ... },
  "mesh": { ... },
  "tags": "fabric|animated",
  "effect_overrides": [
    {
      "effect": "curtain_wave",
      "submesh_filter": "*",
      "params": {
        "amplitude": 0.25,
        "speed": 1.2
      }
    }
  ]
}
```

### Submesh-Specific Effects

Apply effect to specific submesh by index or material pattern:

```json
{
  "name": "ComplexMesh",
  "mesh": { "path": "assets/models/complex.obj" },
  "effect_overrides": [
    {
      "effect": "wave",
      "submesh_filter": "material:fabric*",
      "params": { "amplitude": 0.1 }
    },
    {
      "effect": "wave",
      "submesh_filter": "index:2,3,5",
      "params": { "amplitude": 0.2 }
    }
  ]
}
```

## Implementation

### Step 1: Scene Effect Definition Structure

Add to `lib/src/renderer/resources/loaders/scene_loader.h`:

```c
/**
 * @brief Parsed effect definition from scene JSON.
 */
typedef struct VkrSceneEffectDef {
  char *name;               // Effect instance name (unique within scene)
  char *type;               // Effect type (e.g., "wave", "water")
  VkrTagMask target_tags;
  VkrTagMask exclude_tags;
  VkrEffectPhase phase;
  int32_t priority;
  bool8_t auto_instantiate;

  // Parsed parameter key-value pairs
  struct {
    char *key;
    VkrEffectParamType type;
    union {
      float f[4];
      int32_t i;
      uint32_t u;
    } value;
  } *params;
  uint32_t param_count;
} VkrSceneEffectDef;

/**
 * @brief Per-entity effect override from scene JSON.
 */
typedef struct VkrSceneEffectOverride {
  char *effect_name;
  char *submesh_filter;  // "*", "material:pattern", "index:1,2,3"

  // Override parameters
  struct {
    char *key;
    VkrEffectParamType type;
    union {
      float f[4];
      int32_t i;
      uint32_t u;
    } value;
  } *params;
  uint32_t param_count;
} VkrSceneEffectOverride;
```

### Step 2: Parse Effects Array

Add to `lib/src/renderer/resources/loaders/scene_loader.c`:

```c
/**
 * @brief Parse effect phase from string.
 */
static VkrEffectPhase vkr__parse_effect_phase(const char *str) {
  if (!str) return VKR_EFFECT_PHASE_PRE_RENDER;
  if (strcmp(str, "pre_render") == 0) return VKR_EFFECT_PHASE_PRE_RENDER;
  if (strcmp(str, "post_world") == 0) return VKR_EFFECT_PHASE_POST_WORLD;
  if (strcmp(str, "post_render") == 0) return VKR_EFFECT_PHASE_POST_RENDER;
  return VKR_EFFECT_PHASE_PRE_RENDER;
}

/**
 * @brief Parse parameter value based on JSON type.
 */
static bool8_t vkr__parse_effect_param_value(cJSON *json, VkrEffectParamType *out_type,
                                              void *out_value) {
  if (cJSON_IsNumber(json)) {
    *out_type = VKR_EFFECT_PARAM_FLOAT;
    *(float *)out_value = (float)json->valuedouble;
    return true_v;
  }
  if (cJSON_IsArray(json)) {
    int count = cJSON_GetArraySize(json);
    if (count >= 2 && count <= 4) {
      float *f = (float *)out_value;
      for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(json, i);
        f[i] = cJSON_IsNumber(item) ? (float)item->valuedouble : 0.0f;
      }
      *out_type = (count == 2) ? VKR_EFFECT_PARAM_FLOAT2
                               : (count == 3) ? VKR_EFFECT_PARAM_FLOAT3
                                              : VKR_EFFECT_PARAM_FLOAT4;
      return true_v;
    }
  }
  return false_v;
}

/**
 * @brief Parse scene-level effects array.
 */
static bool8_t vkr__parse_scene_effects(cJSON *effects_json,
                                         VkrAllocator *allocator,
                                         VkrSceneEffectDef **out_defs,
                                         uint32_t *out_count) {
  if (!cJSON_IsArray(effects_json)) {
    *out_defs = NULL;
    *out_count = 0;
    return true_v;
  }

  uint32_t count = (uint32_t)cJSON_GetArraySize(effects_json);
  if (count == 0) {
    *out_defs = NULL;
    *out_count = 0;
    return true_v;
  }

  VkrSceneEffectDef *defs = vkr_allocator_alloc(
      allocator, sizeof(VkrSceneEffectDef) * count,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!defs) return false_v;

  MemZero(defs, sizeof(VkrSceneEffectDef) * count);

  uint32_t valid_count = 0;
  cJSON *effect_json;
  cJSON_ArrayForEach(effect_json, effects_json) {
    VkrSceneEffectDef *def = &defs[valid_count];

    // Required: name, type
    cJSON *name_json = cJSON_GetObjectItem(effect_json, "name");
    cJSON *type_json = cJSON_GetObjectItem(effect_json, "type");
    if (!cJSON_IsString(name_json) || !cJSON_IsString(type_json)) {
      log_warn("Effect missing name or type, skipping");
      continue;
    }

    def->name = vkr__strdup(allocator, name_json->valuestring);
    def->type = vkr__strdup(allocator, type_json->valuestring);

    // Optional: target_tags
    cJSON *tags_json = cJSON_GetObjectItem(effect_json, "target_tags");
    if (cJSON_IsString(tags_json)) {
      vkr_tag_parse_name(tags_json->valuestring, &def->target_tags);
    }

    // Optional: exclude_tags
    cJSON *exclude_json = cJSON_GetObjectItem(effect_json, "exclude_tags");
    if (cJSON_IsString(exclude_json)) {
      vkr_tag_parse_name(exclude_json->valuestring, &def->exclude_tags);
    }

    // Optional: phase
    cJSON *phase_json = cJSON_GetObjectItem(effect_json, "phase");
    def->phase = vkr__parse_effect_phase(
        cJSON_IsString(phase_json) ? phase_json->valuestring : NULL);

    // Optional: priority
    cJSON *priority_json = cJSON_GetObjectItem(effect_json, "priority");
    def->priority = cJSON_IsNumber(priority_json) ? priority_json->valueint : 0;

    // Optional: auto_instantiate
    cJSON *auto_json = cJSON_GetObjectItem(effect_json, "auto_instantiate");
    def->auto_instantiate = !cJSON_IsBool(auto_json) || cJSON_IsTrue(auto_json);

    // Optional: params object
    cJSON *params_json = cJSON_GetObjectItem(effect_json, "params");
    if (cJSON_IsObject(params_json)) {
      uint32_t param_count = (uint32_t)cJSON_GetArraySize(params_json);
      if (param_count > 0) {
        def->params = vkr_allocator_alloc(
            allocator, sizeof(def->params[0]) * param_count,
            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
        if (def->params) {
          uint32_t parsed_params = 0;
          cJSON *param;
          cJSON_ArrayForEach(param, params_json) {
            def->params[parsed_params].key =
                vkr__strdup(allocator, param->string);
            if (vkr__parse_effect_param_value(
                    param, &def->params[parsed_params].type,
                    &def->params[parsed_params].value)) {
              parsed_params++;
            }
          }
          def->param_count = parsed_params;
        }
      }
    }

    valid_count++;
  }

  *out_defs = defs;
  *out_count = valid_count;
  return true_v;
}
```

### Step 3: Parse Entity Effect Overrides

```c
/**
 * @brief Parse per-entity effect overrides.
 */
static bool8_t vkr__parse_entity_effect_overrides(
    cJSON *overrides_json, VkrAllocator *allocator,
    VkrSceneEffectOverride **out_overrides, uint32_t *out_count) {

  if (!cJSON_IsArray(overrides_json)) {
    *out_overrides = NULL;
    *out_count = 0;
    return true_v;
  }

  uint32_t count = (uint32_t)cJSON_GetArraySize(overrides_json);
  if (count == 0) {
    *out_overrides = NULL;
    *out_count = 0;
    return true_v;
  }

  VkrSceneEffectOverride *overrides = vkr_allocator_alloc(
      allocator, sizeof(VkrSceneEffectOverride) * count,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!overrides) return false_v;

  MemZero(overrides, sizeof(VkrSceneEffectOverride) * count);

  uint32_t valid_count = 0;
  cJSON *override_json;
  cJSON_ArrayForEach(override_json, overrides_json) {
    VkrSceneEffectOverride *ovr = &overrides[valid_count];

    // Required: effect
    cJSON *effect_json = cJSON_GetObjectItem(override_json, "effect");
    if (!cJSON_IsString(effect_json)) continue;

    ovr->effect_name = vkr__strdup(allocator, effect_json->valuestring);

    // Optional: submesh_filter
    cJSON *filter_json = cJSON_GetObjectItem(override_json, "submesh_filter");
    ovr->submesh_filter = cJSON_IsString(filter_json)
                              ? vkr__strdup(allocator, filter_json->valuestring)
                              : vkr__strdup(allocator, "*");

    // Optional: params
    cJSON *params_json = cJSON_GetObjectItem(override_json, "params");
    if (cJSON_IsObject(params_json)) {
      uint32_t param_count = (uint32_t)cJSON_GetArraySize(params_json);
      if (param_count > 0) {
        ovr->params = vkr_allocator_alloc(
            allocator, sizeof(ovr->params[0]) * param_count,
            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
        if (ovr->params) {
          uint32_t parsed = 0;
          cJSON *param;
          cJSON_ArrayForEach(param, params_json) {
            ovr->params[parsed].key = vkr__strdup(allocator, param->string);
            if (vkr__parse_effect_param_value(param, &ovr->params[parsed].type,
                                               &ovr->params[parsed].value)) {
              parsed++;
            }
          }
          ovr->param_count = parsed;
        }
      }
    }

    valid_count++;
  }

  *out_overrides = overrides;
  *out_count = valid_count;
  return true_v;
}
```

### Step 4: Apply Effects After Scene Load

Add to `lib/src/renderer/systems/vkr_scene_system.c`:

```c
/**
 * @brief Apply parsed effect definitions to effect system.
 */
bool8_t vkr_scene_apply_effects(VkrScene *scene, VkrEffectSystem *effect_system,
                                 const VkrSceneEffectDef *defs, uint32_t count) {
  if (!scene || !effect_system || !defs || count == 0) return true_v;

  for (uint32_t i = 0; i < count; i++) {
    const VkrSceneEffectDef *def = &defs[i];

    // Find effect type
    VkrEffectHandle effect = vkr_effect_system_find(effect_system, def->type);
    if (effect.id == 0) {
      log_warn("Unknown effect type '%s', skipping '%s'", def->type, def->name);
      continue;
    }

    // Auto-instantiate by tags if requested
    if (def->auto_instantiate && def->target_tags != VKR_TAG_NONE) {
      uint32_t inst_count = 0;

      // Create instances for matching entities
      VkrAllocatorScope scope = vkr_allocator_begin_scope(&scene->scratch_allocator);
      VkrTagQueryResult query;

      if (vkr_tag_query_entities(scene, def->target_tags, def->exclude_tags,
                                  &scene->scratch_allocator, &query)) {
        for (uint32_t j = 0; j < query.count; j++) {
          VkrEffectInstance *instance;
          if (vkr_effect_system_instantiate(effect_system, effect,
                                             (VkrEntity)query.entity_ids[j],
                                             UINT32_MAX, &instance)) {
            // Apply parameters
            vkr__apply_scene_params_to_instance(instance, def->params,
                                                 def->param_count);
            inst_count++;
          }
        }
        vkr_tag_query_result_free(&query, &scene->scratch_allocator);
      }

      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      log_info("Effect '%s' created %u instances", def->name, inst_count);
    }
  }

  return true_v;
}

/**
 * @brief Apply per-entity effect overrides.
 */
bool8_t vkr_scene_apply_entity_effects(VkrScene *scene,
                                        VkrEffectSystem *effect_system,
                                        VkrEntity entity,
                                        const VkrSceneEffectOverride *overrides,
                                        uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    const VkrSceneEffectOverride *ovr = &overrides[i];

    VkrEffectHandle effect = vkr_effect_system_find(effect_system,
                                                     ovr->effect_name);
    if (effect.id == 0) {
      log_warn("Unknown effect '%s' in override", ovr->effect_name);
      continue;
    }

    // Parse submesh filter
    uint32_t submesh_index = UINT32_MAX;  // All by default
    if (ovr->submesh_filter) {
      if (strcmp(ovr->submesh_filter, "*") == 0) {
        submesh_index = UINT32_MAX;
      } else if (strncmp(ovr->submesh_filter, "index:", 6) == 0) {
        // Parse comma-separated indices
        // For simplicity, just take first index
        submesh_index = (uint32_t)atoi(ovr->submesh_filter + 6);
      } else if (strncmp(ovr->submesh_filter, "material:", 9) == 0) {
        // Pattern match - would need material system access
        // For now, apply to all
        submesh_index = UINT32_MAX;
      }
    }

    // Create or find existing instance
    VkrEffectInstance *instance;
    if (vkr_effect_system_instantiate(effect_system, effect, entity,
                                       submesh_index, &instance)) {
      // Apply override parameters
      vkr__apply_scene_params_to_instance(instance, ovr->params,
                                           ovr->param_count);
    }
  }

  return true_v;
}

/**
 * @brief Helper to apply parsed params to effect instance.
 */
static void vkr__apply_scene_params_to_instance(
    VkrEffectInstance *instance, const void *params_array, uint32_t count) {
  // Type punning for the param struct
  typedef struct {
    char *key;
    VkrEffectParamType type;
    union {
      float f[4];
      int32_t i;
      uint32_t u;
    } value;
  } ParsedParam;

  const ParsedParam *params = params_array;

  for (uint32_t i = 0; i < count; i++) {
    const ParsedParam *p = &params[i];
    vkr_effect_instance_set_param(instance, p->key, &p->value);
  }
}
```

### Step 5: Scene Loader Integration

Update main scene loading function:

```c
// In vkr_scene_load() or equivalent

bool8_t vkr_scene_load(VkrScene *scene, const char *path,
                        VkrEffectSystem *effect_system,
                        VkrRendererError *out_error) {
  // ... existing load code ...

  // Parse JSON
  cJSON *root = cJSON_Parse(json_content);
  if (!root) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  // Check version (now requires v3 for effects)
  cJSON *version_json = cJSON_GetObjectItem(root, "version");
  int version = cJSON_IsNumber(version_json) ? version_json->valueint : 1;

  // Parse effects (v3+)
  VkrSceneEffectDef *effect_defs = NULL;
  uint32_t effect_def_count = 0;
  if (version >= 3) {
    cJSON *effects_json = cJSON_GetObjectItem(root, "effects");
    vkr__parse_scene_effects(effects_json, &scene->allocator,
                              &effect_defs, &effect_def_count);
  }

  // Parse entities
  cJSON *entities_json = cJSON_GetObjectItem(root, "entities");
  // ... existing entity parsing ...

  // For each entity, also parse effect_overrides
  cJSON *entity_json;
  cJSON_ArrayForEach(entity_json, entities_json) {
    VkrEntity entity = /* created entity */;

    cJSON *overrides_json = cJSON_GetObjectItem(entity_json, "effect_overrides");
    if (overrides_json && cJSON_IsArray(overrides_json)) {
      VkrSceneEffectOverride *overrides = NULL;
      uint32_t override_count = 0;
      vkr__parse_entity_effect_overrides(overrides_json, &scene->allocator,
                                          &overrides, &override_count);

      if (overrides && override_count > 0) {
        // Store for later application (after all entities loaded)
        vkr__store_entity_overrides(scene, entity, overrides, override_count);
      }
    }
  }

  // Apply scene effects
  if (effect_system && effect_def_count > 0) {
    vkr_scene_apply_effects(scene, effect_system, effect_defs, effect_def_count);
  }

  // Apply per-entity overrides
  vkr__apply_stored_entity_overrides(scene, effect_system);

  cJSON_Delete(root);
  return true_v;
}
```

### Step 6: Scene Unload Cleanup

```c
void vkr_scene_shutdown(VkrScene *scene, VkrEffectSystem *effect_system) {
  if (!scene) return;

  // Destroy effect instances for this scene
  if (effect_system) {
    vkr_effect_system_destroy_scene_instances(effect_system, scene);
  }

  // ... existing shutdown code ...
}

// In vkr_effect_system.c:
void vkr_effect_system_destroy_scene_instances(VkrEffectSystem *system,
                                                 VkrScene *scene) {
  // Find and destroy all instances belonging to entities in this scene
  for (uint32_t i = 0; i < system->instance_count; i++) {
    VkrEffectInstance *instance = &system->instances.data[i];
    if (instance->effect.id == 0) continue;

    // Check if entity belongs to scene
    if (vkr_world_entity_exists(&scene->world, instance->entity)) {
      vkr_effect_system_destroy_instance(system, instance);
    }
  }

  // Clear deformed buffer references that may be stale
  vkr_effect_system_flush_deformed_buffers(system);
}
```

## Complete Scene Example

```json
{
  "version": 3,
  "effects": [
    {
      "name": "sponza_curtains",
      "type": "wave",
      "target_tags": "fabric",
      "phase": "pre_render",
      "auto_instantiate": true,
      "params": {
        "amplitude": 0.12,
        "frequency": 2.5,
        "speed": 0.8,
        "wave_direction": [0.0, 0.0, 1.0],
        "wind_direction": [1.0, 0.0, 0.0],
        "wind_strength": 0.35,
        "vertical_bias": 0.6,
        "damping": 0.8,
        "noise_scale": 0.15
      }
    }
  ],
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
      "parent": null,
      "transform": {
        "pos": [0.0, 0.0, -15.0],
        "rot": [0.0, 0.0, 0.0, 1.0],
        "scale": [0.0085, 0.0085, 0.0085]
      },
      "mesh": {
        "path": "assets/models/sponza.obj",
        "pipeline_domain": "world"
      },
      "tags": ["animated"]
    },
    {
      "name": "Falcon",
      "parent": 0,
      "transform": {
        "pos": [0.0, 0.2, -15.0],
        "rot": [0.0, 0.0, 0.0, 1.0],
        "scale": [0.2, 0.2, 0.2]
      },
      "mesh": {
        "path": "assets/models/falcon.obj",
        "pipeline_domain": "world"
      }
    },
    {
      "name": "Gizmo",
      "parent": 0,
      "transform": {
        "pos": [0.0, 3.0, -15.0],
        "rot": [0.0, 0.0, 0.0, 1.0],
        "scale": [1.0, 1.0, 1.0]
      },
      "mesh": {
        "path": "assets/models/gizmo.obj",
        "pipeline_domain": "world"
      }
    },
    {
      "name": "WorldClock",
      "parent": 0,
      "transform": {
        "pos": [7.0, 3.0, -11.5],
        "rot": [0.0, 0.7071068, 0.0, 0.7071068],
        "scale": [8.0, 8.0, 1.0]
      },
      "text3d": {
        "text": "00:00:00",
        "font": "UbuntuMono-3d",
        "font_size": 64.0,
        "color": [1.0, 0.95, 0.85, 1.0],
        "texture_width": 1024,
        "texture_height": 256,
        "inset": 1.0
      }
    }
  ]
}
```

## Testing

### Scene Load Test

```c
void test_scene_with_effects(void) {
  VkrScene scene;
  VkrEffectSystem effect_system;

  // Initialize effect system with wave effect registered
  vkr_effect_system_init(&effect_system, ...);
  vkr_effect_wave_register(&effect_system, NULL);

  // Load scene with effects
  VkrRendererError err;
  bool8_t result = vkr_scene_load(&scene,
                                   "assets/scenes/default.scene.json",
                                   &effect_system, &err);
  assert(result);
  assert(err == VKR_RENDERER_ERROR_NONE);

  // Verify effect instances created
  uint32_t instance_count = vkr_effect_system_instance_count(&effect_system);
  assert(instance_count > 0);

  // Unload and verify cleanup
  vkr_scene_shutdown(&scene, &effect_system);
  instance_count = vkr_effect_system_instance_count(&effect_system);
  assert(instance_count == 0);
}
```

### Version Migration

```c
void test_scene_version_upgrade(void) {
  // v2 scene (no effects section)
  const char *v2_scene = "{ \"version\": 2, \"entities\": [] }";

  VkrScene scene;
  VkrRendererError err;

  // Should load without errors
  bool8_t result = vkr_scene_load_from_string(&scene, v2_scene,
                                               NULL, &err);
  assert(result);

  vkr_scene_shutdown(&scene, NULL);
}
```

## Completion Criteria

- [ ] Scene JSON v3 schema defined with effects array
- [ ] Effect definitions parsed from scene JSON
- [ ] Per-entity effect overrides parsed
- [ ] Effects auto-instantiate by tags on scene load
- [ ] Effect parameters applied from JSON
- [ ] Effect instances destroyed on scene unload
- [ ] Backward compatible with v2 scenes (no effects)
- [ ] Tests pass for load/unload/reload cycles

## Next Steps

After completing this phase:
- **06-implementation-checklist.md**: Final verification checklist
- Add editor UI for runtime effect parameter editing
- Implement additional effect types (water, foliage, particles)
- Add effect preview in editor
