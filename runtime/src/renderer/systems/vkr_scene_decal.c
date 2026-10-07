#include "renderer/systems/vkr_scene_decal.h"

#include "core/logger.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_types.h"

#include <math.h>

#define DECAL_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY

typedef struct DecalRecord {
  VkrEntityId entity;
  VkrMaterialHandle material;
  /* The record holds one reference; false while it has no material. */
  bool8_t owned;
  /* The component changed since its material loaded. */
  bool8_t dirty;
} DecalRecord;

/* Records in the order their components were added; that order breaks
   sort-order ties. */
struct s_VkrSceneDecals {
  DecalRecord *records;
  uint32_t count;
  uint32_t capacity;
};

// =============================================================================
// Records
// =============================================================================

static VkrSceneDecals *decal_state(VkrScene *scene, bool8_t create) {
  if (scene->decals || !create) {
    return scene->decals;
  }
  VkrSceneDecals *state = vkr_allocator_alloc(scene->alloc, sizeof(*state),
                                              VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (state) {
    MemZero(state, sizeof(*state));
  }
  scene->decals = state;
  return state;
}

static int32_t decal_find(const VkrSceneDecals *state, VkrEntityId entity) {
  for (uint32_t i = 0u; i < state->count; ++i) {
    if (state->records[i].entity.u64 == entity.u64) {
      return (int32_t)i;
    }
  }
  return -1;
}

static void decal_release(VkrScene *scene, DecalRecord *record) {
  if (record->owned && scene->assets) {
    vkr_material_system_release(&scene->assets->material_system,
                                record->material);
  }
  record->owned = false_v;
  record->material = (VkrMaterialHandle){0};
}

/* Releases a record and closes the gap, keeping the others' order. */
static void decal_remove(VkrScene *scene, VkrSceneDecals *state,
                         uint32_t index) {
  decal_release(scene, &state->records[index]);
  MemCopy(&state->records[index], &state->records[index + 1u],
          (uint64_t)(state->count - index - 1u) * sizeof(*state->records));
  state->count--;
}

void vkr_scene_decal_changed(VkrScene *scene, VkrEntityId entity) {
  VkrSceneDecals *state = decal_state(scene, true_v);
  if (!state) {
    return;
  }
  const int32_t index = decal_find(state, entity);
  if (index >= 0) {
    state->records[index].dirty = true_v;
    return;
  }
  if (state->count == state->capacity) {
    const uint32_t capacity = Max(16u, state->capacity * 2u);
    DecalRecord *grown = vkr_allocator_realloc(
        scene->alloc, state->records,
        (uint64_t)state->capacity * sizeof(*state->records),
        (uint64_t)capacity * sizeof(*state->records), DECAL_TAG);
    if (!grown) {
      log_error("Scene decals: no memory for the decal of entity %llu",
                (unsigned long long)entity.u64);
      return;
    }
    state->records = grown;
    state->capacity = capacity;
  }
  state->records[state->count++] = (DecalRecord){
      .entity = entity,
      .dirty = true_v,
  };
}

void vkr_scene_decal_entity_destroying(VkrScene *scene, VkrEntityId entity) {
  VkrSceneDecals *state = scene ? decal_state(scene, false_v) : NULL;
  if (!state) {
    return;
  }
  const int32_t index = decal_find(state, entity);
  if (index >= 0) {
    decal_remove(scene, state, (uint32_t)index);
  }
}

void vkr_scene_decal_shutdown(VkrScene *scene) {
  VkrSceneDecals *state = scene ? scene->decals : NULL;
  if (!state) {
    return;
  }
  for (uint32_t i = 0u; i < state->count; ++i) {
    decal_release(scene, &state->records[i]);
  }
  if (state->records) {
    vkr_allocator_free(scene->alloc, state->records,
                       (uint64_t)state->capacity * sizeof(*state->records),
                       DECAL_TAG);
  }
  vkr_allocator_free(scene->alloc, state, sizeof(*state),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  scene->decals = NULL;
}

/* Loads the material a changed component names. A file that does not load
   leaves the decal without a material, so it draws nothing rather than the
   default surface material over its whole box. */
static void decal_load(VkrScene *scene, DecalRecord *record,
                       const SceneDecal *value) {
  decal_release(scene, record);
  const char *path =
      value->material[0] ? value->material : VKR_SCENE_DECAL_DEFAULT_MATERIAL;
  bool8_t owned = false_v;
  const VkrMaterialHandle material =
      vkr_scene_material_load(scene, path, &owned);
  if (!owned) {
    log_warn("Scene decals: material '%s' did not load; the decal draws "
             "nothing",
             path);
    return;
  }
  record->material = material;
  record->owned = true_v;
}

// =============================================================================
// Frame
// =============================================================================

/* Lowers one decal whose box `world` places; false for a box with no
   volume. */
static bool8_t decal_lower(const Mat4 *world, const SceneDecal *value,
                           VkrMaterialHandle material, VkrDecal *out) {
  *out = (VkrDecal){
      .material = material,
      .opacity = Clamp(value->opacity, 0.0f, 1.0f),
      .fade_cos_start = cosf(value->fade_angle_start),
      .fade_cos_end = cosf(value->fade_angle_end),
      .depth_fade = Clamp(value->depth_fade, 0.0f, 1.0f),
  };
  return vkr_decal_set_box(out, *world) && vkr_decal_valid(out);
}

static float32_t decal_distance_squared(const VkrDecal *decal, Vec3 point) {
  const Vec3 closest =
      vec3_new(Clamp(point.x, decal->bounds_min.x, decal->bounds_max.x),
               Clamp(point.y, decal->bounds_min.y, decal->bounds_max.y),
               Clamp(point.z, decal->bounds_min.z, decal->bounds_max.z));
  return vec3_length_squared(vec3_sub(point, closest));
}

static void decal_frame_move(VkrSceneDecalFrame *frame, uint32_t to,
                             uint32_t from) {
  frame->decals[to] = frame->decals[from];
  frame->distances[to] = frame->distances[from];
  frame->sort_orders[to] = frame->sort_orders[from];
  frame->sequences[to] = frame->sequences[from];
}

/* Inserts a decal by distance, nearest first; once the table is full, a
   decal nearer than the farthest replaces it. */
static void decal_frame_keep(VkrSceneDecalFrame *frame, const VkrDecal *decal,
                             float32_t distance, int32_t sort_order) {
  const uint32_t sequence = frame->next_sequence++;
  uint32_t slot = frame->count;
  if (frame->count == VKR_MAX_FRAME_DECALS) {
    frame->dropped_count++;
    if (distance >= frame->distances[frame->count - 1u]) {
      return;
    }
    slot = frame->count - 1u;
  } else {
    frame->count++;
  }
  while (slot > 0u && frame->distances[slot - 1u] > distance) {
    decal_frame_move(frame, slot, slot - 1u);
    slot--;
  }
  frame->decals[slot] = *decal;
  frame->distances[slot] = distance;
  frame->sort_orders[slot] = sort_order;
  frame->sequences[slot] = sequence;
}

void vkr_scene_decal_frame_begin(VkrSceneDecalFrame *frame) {
  frame->count = 0u;
  frame->dropped_count = 0u;
  frame->next_sequence = 0u;
}

void vkr_scene_decal_frame_append(VkrSceneDecalFrame *frame, VkrScene *scene,
                                  Vec3 camera_position) {
  VkrSceneDecals *state = scene ? decal_state(scene, false_v) : NULL;
  if (!state || !scene->assets) {
    return;
  }
  const VkrComponentTypeId id = vkr_scene_type_id(scene, &vkr_scene_decal_type);
  uint32_t i = 0u;
  while (i < state->count) {
    DecalRecord *record = &state->records[i];
    const SceneDecal *value =
        id != VKR_COMPONENT_TYPE_INVALID &&
                vkr_entity_is_alive(scene->world, record->entity)
            ? vkr_entity_get_component(scene->world, record->entity, id)
            : NULL;
    if (!value) {
      decal_remove(scene, state, i);
      continue;
    }
    i++;
    if (record->dirty) {
      decal_load(scene, record, value);
      record->dirty = false_v;
    }
    if (!record->owned || !value->enabled || !(value->opacity > 0.0f) ||
        !vkr_scene_entity_visible(scene, record->entity)) {
      continue;
    }
    const SceneTransform *transform = vkr_entity_get_component(
        scene->world, record->entity, scene->comp_transform);
    VkrDecal decal;
    if (!transform ||
        !decal_lower(&transform->world, value, record->material, &decal)) {
      continue;
    }
    decal_frame_keep(frame, &decal,
                     decal_distance_squared(&decal, camera_position),
                     value->sort_order);
  }
}

void vkr_scene_decal_frame_finish(VkrSceneDecalFrame *frame,
                                  VkrMaterialSystem *materials) {
  /* Compositing order: ascending sort order, then collection order. At most
     VKR_MAX_FRAME_DECALS rows, so insertion sort. */
  for (uint32_t i = 1u; i < frame->count; ++i) {
    const VkrDecal decal = frame->decals[i];
    const float32_t distance = frame->distances[i];
    const int32_t sort_order = frame->sort_orders[i];
    const uint32_t sequence = frame->sequences[i];
    uint32_t slot = i;
    while (slot > 0u && (frame->sort_orders[slot - 1u] > sort_order ||
                         (frame->sort_orders[slot - 1u] == sort_order &&
                          frame->sequences[slot - 1u] > sequence))) {
      decal_frame_move(frame, slot, slot - 1u);
      slot--;
    }
    frame->decals[slot] = decal;
    frame->distances[slot] = distance;
    frame->sort_orders[slot] = sort_order;
    frame->sequences[slot] = sequence;
  }

  /* Draws name the material's live generation, as world draws do. */
  for (uint32_t i = 0u; i < frame->count; ++i) {
    VkrDecal *decal = &frame->decals[i];
    const VkrMaterial *material =
        vkr_material_system_get_live(materials, decal->material);
    if (material) {
      decal->material = (VkrMaterialHandle){.id = material->id,
                                            .generation = material->generation};
    }
    vkr_material_system_touch_texture_residency(materials, decal->material);
  }
  vkr_decal_grid_build(frame->decals, frame->count, &frame->grid);
}
