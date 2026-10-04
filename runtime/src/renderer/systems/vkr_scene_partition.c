#include "renderer/systems/vkr_scene_partition.h"

#include "core/vkr_type_desc.h"
#include "renderer/systems/vkr_scene_types.h"

#include <math.h>
#include <stdlib.h>

#define PARTITION_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
/* Cells stay loaded this far past the load radius, in cells, so a source
   moving along a cell edge does not churn them. */
#define PARTITION_KEEP_CELLS 1.0f

struct s_VkrScenePartition {
  VkrScenePartitionCellRecord *records;
  uint32_t record_count;
  uint32_t record_capacity;
  /* Open addressing over the records: index + 1, zero for empty. */
  uint32_t *slots;
  uint32_t slot_capacity;
};

static uint32_t partition_hash(VkrScenePartitionCell cell) {
  const uint64_t key = ((uint64_t)(uint32_t)cell.x << 32u) | (uint32_t)cell.z;
  return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32u);
}

static struct s_VkrScenePartition *partition_state(VkrScene *scene,
                                                   bool8_t create) {
  if (!scene->partition && create) {
    scene->partition =
        vkr_allocator_alloc(scene->alloc, sizeof(*scene->partition),
                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (scene->partition) {
      MemZero(scene->partition, sizeof(*scene->partition));
    }
  }
  return scene->partition;
}

/* The slot holding `cell`, or the empty slot where it would go. */
static uint32_t partition_slot(const struct s_VkrScenePartition *state,
                               VkrScenePartitionCell cell) {
  const uint32_t mask = state->slot_capacity - 1u;
  uint32_t slot = partition_hash(cell) & mask;
  while (state->slots[slot]) {
    const VkrScenePartitionCell at =
        state->records[state->slots[slot] - 1u].cell;
    if (at.x == cell.x && at.z == cell.z) {
      break;
    }
    slot = (slot + 1u) & mask;
  }
  return slot;
}

/* Rehashes into twice the slots once the table is half full. */
static bool8_t partition_grow(VkrScene *scene,
                              struct s_VkrScenePartition *state) {
  if (state->record_count == state->record_capacity) {
    const uint32_t capacity = Max(64u, state->record_capacity * 2u);
    VkrScenePartitionCellRecord *records = vkr_allocator_realloc(
        scene->alloc, state->records, state->record_capacity * sizeof(*records),
        capacity * sizeof(*records), PARTITION_TAG);
    if (!records) {
      return false_v;
    }
    state->records = records;
    state->record_capacity = capacity;
  }
  if ((state->record_count + 1u) * 2u <= state->slot_capacity) {
    return true_v;
  }
  const uint32_t old_capacity = state->slot_capacity;
  const uint32_t capacity = Max(128u, old_capacity * 2u);
  uint32_t *slots = vkr_allocator_alloc(scene->alloc, capacity * sizeof(*slots),
                                        PARTITION_TAG);
  if (!slots) {
    return false_v;
  }
  if (state->slots) {
    vkr_allocator_free(scene->alloc, state->slots,
                       old_capacity * sizeof(*slots), PARTITION_TAG);
  }
  MemZero(slots, capacity * sizeof(*slots));
  state->slots = slots;
  state->slot_capacity = capacity;
  for (uint32_t i = 0; i < state->record_count; ++i) {
    state->slots[partition_slot(state, state->records[i].cell)] = i + 1u;
  }
  return true_v;
}

bool8_t vkr_scene_partition_settings(const VkrScene *scene,
                                     SceneWorldPartition *out) {
  VkrEntityId entity = VKR_ENTITY_ID_INVALID;
  if (!scene || !vkr_scene_find_typed(scene, &vkr_scene_world_partition_type,
                                      &entity, 1u)) {
    return false_v;
  }
  const SceneWorldPartition *partition =
      vkr_scene_get_typed(scene, entity, &vkr_scene_world_partition_type);
  if (!partition || !(partition->cell_size >= 1.0f) ||
      !isfinite(partition->cell_size) || !isfinite(partition->load_radius) ||
      !isfinite(partition->proxy_radius)) {
    return false_v;
  }
  *out = *partition;
  out->load_radius = Max(0.0f, out->load_radius);
  out->proxy_radius = Max(0.0f, out->proxy_radius);
  out->cell_budget = Max(1u, out->cell_budget);
  return true_v;
}

VkrScenePartitionCell
vkr_scene_partition_cell_at(const SceneWorldPartition *settings, Vec3 world) {
  return (VkrScenePartitionCell){
      .x = (int32_t)floorf(world.x / settings->cell_size),
      .z = (int32_t)floorf(world.z / settings->cell_size)};
}

/* Whether root `entity` belongs to the persistent layer: a document entity,
   a runtime-only one, one without a place, or one carrying a terrain, a
   singleton or an enabled `always_loaded`. */
static bool8_t partition_persistent(const VkrScene *scene, VkrEntityId entity) {
  const VkrWorld *world = scene->world;
  if (vkr_entity_get_component(world, entity, scene->comp_source_identity) ||
      !vkr_entity_get_component(world, entity, scene->comp_transform) ||
      vkr_scene_entity_transient(scene, entity) ||
      vkr_scene_get_typed(scene, entity, &vkr_scene_terrain_type)) {
    return true_v;
  }
  const SceneAlwaysLoaded *always =
      vkr_scene_get_typed(scene, entity, &vkr_scene_always_loaded_type);
  if (always && always->enabled) {
    return true_v;
  }
  for (uint32_t i = 0; i < scene->type_count; ++i) {
    if ((scene->types[i].type->flags & VKR_TYPE_FLAG_SINGLETON) &&
        vkr_entity_get_component(world, entity, scene->types[i].id)) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_scene_partition_entity_cell(const VkrScene *scene,
                                        const SceneWorldPartition *settings,
                                        VkrEntityId entity,
                                        VkrScenePartitionCell *out) {
  /* Children stream with their root. */
  VkrEntityId root = entity;
  for (uint32_t depth = 0; depth < 256u; ++depth) {
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, root, scene->comp_transform);
    if (!transform || !transform->parent.u64 ||
        !vkr_scene_entity_alive(scene, transform->parent)) {
      break;
    }
    root = transform->parent;
  }
  if (partition_persistent(scene, root)) {
    return false_v;
  }
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, root, scene->comp_transform);
  *out = vkr_scene_partition_cell_at(settings, mat4_position(transform->world));
  return true_v;
}

const VkrScenePartitionCellRecord *
vkr_scene_partition_cells(const VkrScene *scene, uint32_t *out_count) {
  const struct s_VkrScenePartition *state = scene ? scene->partition : NULL;
  *out_count = state ? state->record_count : 0u;
  return state ? state->records : NULL;
}

VkrScenePartitionCellRecord *
vkr_scene_partition_cell(VkrScene *scene, VkrScenePartitionCell cell,
                         bool8_t create) {
  struct s_VkrScenePartition *state = partition_state(scene, create);
  if (!state) {
    return NULL;
  }
  if (state->slot_capacity) {
    const uint32_t slot = partition_slot(state, cell);
    if (state->slots[slot]) {
      return &state->records[state->slots[slot] - 1u];
    }
  }
  if (!create || !partition_grow(scene, state)) {
    return NULL;
  }
  const uint32_t index = state->record_count++;
  state->records[index] = (VkrScenePartitionCellRecord){.cell = cell};
  state->slots[partition_slot(state, cell)] = index + 1u;
  return &state->records[index];
}

void vkr_scene_partition_reset(VkrScene *scene) {
  struct s_VkrScenePartition *state = scene ? scene->partition : NULL;
  if (!state) {
    return;
  }
  state->record_count = 0u;
  if (state->slots) {
    MemZero(state->slots, state->slot_capacity * sizeof(*state->slots));
  }
}

typedef struct PartitionCandidate {
  float32_t distance;
  VkrScenePartitionCell cell;
} PartitionCandidate;

static int partition_candidate_compare(const void *a, const void *b) {
  const float32_t da = ((const PartitionCandidate *)a)->distance;
  const float32_t db = ((const PartitionCandidate *)b)->distance;
  return da < db ? -1 : da > db ? 1 : 0;
}

/* Metres from the nearest streaming source to the cell's square. */
static float32_t partition_distance(const VkrScene *scene,
                                    const SceneWorldPartition *settings,
                                    VkrScenePartitionCell cell) {
  const float32_t size = settings->cell_size;
  const float32_t x0 = (float32_t)cell.x * size;
  const float32_t z0 = (float32_t)cell.z * size;
  float32_t best = INFINITY;
  for (uint32_t i = 0; i < scene->stream_source_count; ++i) {
    const Vec3 at = scene->stream_sources[i];
    const float32_t dx = Max(0.0f, Max(x0 - at.x, at.x - (x0 + size)));
    const float32_t dz = Max(0.0f, Max(z0 - at.z, at.z - (z0 + size)));
    best = Min(best, sqrtf(dx * dx + dz * dz));
  }
  return best;
}

void vkr_scene_partition_plan(const VkrScene *scene,
                              const SceneWorldPartition *settings,
                              VkrScenePartitionPlan *out) {
  MemZero(out, sizeof(*out));
  const struct s_VkrScenePartition *state = scene->partition;
  if (!state || !scene->stream_source_count) {
    return;
  }
  const float32_t keep =
      settings->load_radius + PARTITION_KEEP_CELLS * settings->cell_size;
  PartitionCandidate loads[VKR_SCENE_PARTITION_PLAN_MAX];
  PartitionCandidate unloads[VKR_SCENE_PARTITION_PLAN_MAX];
  uint32_t load_count = 0u;
  uint32_t unload_count = 0u;
  uint32_t loaded = 0u;
  for (uint32_t i = 0; i < state->record_count; ++i) {
    const VkrScenePartitionCellRecord *record = &state->records[i];
    const float32_t distance =
        partition_distance(scene, settings, record->cell);
    const PartitionCandidate candidate = {distance, record->cell};
    if (record->flags & VKR_SCENE_PARTITION_CELL_LOADED) {
      loaded++;
      if (!(record->flags & VKR_SCENE_PARTITION_CELL_PINNED) &&
          distance > keep) {
        /* The farthest go first when the list is full. */
        if (unload_count < VKR_SCENE_PARTITION_PLAN_MAX) {
          unloads[unload_count++] = candidate;
        } else {
          for (uint32_t u = 0; u < unload_count; ++u) {
            if (unloads[u].distance < distance) {
              unloads[u] = candidate;
              break;
            }
          }
        }
      }
    } else if ((record->flags & VKR_SCENE_PARTITION_CELL_ON_DISK) &&
               distance <= settings->load_radius) {
      if (load_count < VKR_SCENE_PARTITION_PLAN_MAX) {
        loads[load_count++] = candidate;
      } else {
        for (uint32_t l = 0; l < load_count; ++l) {
          if (loads[l].distance > distance) {
            loads[l] = candidate;
            break;
          }
        }
      }
    }
  }
  qsort(loads, load_count, sizeof(*loads), partition_candidate_compare);
  /* A load past the budget waits until farther cells leave. */
  const uint32_t staying = loaded - Min(loaded, unload_count);
  const uint32_t room =
      staying < settings->cell_budget ? settings->cell_budget - staying : 0u;
  load_count = Min(load_count, room);
  for (uint32_t i = 0; i < load_count; ++i) {
    out->load[out->load_count++] = loads[i].cell;
  }
  qsort(unloads, unload_count, sizeof(*unloads), partition_candidate_compare);
  for (uint32_t i = unload_count; i > 0u; --i) {
    out->unload[out->unload_count++] = unloads[i - 1u].cell;
  }
  /* Over the budget, the farthest loaded cells inside the radius go too. */
  if (staying > settings->cell_budget) {
    uint32_t excess = staying - settings->cell_budget;
    PartitionCandidate far[VKR_SCENE_PARTITION_PLAN_MAX];
    uint32_t far_count = 0u;
    for (uint32_t i = 0; i < state->record_count; ++i) {
      const VkrScenePartitionCellRecord *record = &state->records[i];
      const float32_t distance =
          partition_distance(scene, settings, record->cell);
      if ((record->flags & VKR_SCENE_PARTITION_CELL_LOADED) &&
          !(record->flags & VKR_SCENE_PARTITION_CELL_PINNED) &&
          distance <= keep && far_count < VKR_SCENE_PARTITION_PLAN_MAX) {
        far[far_count++] = (PartitionCandidate){distance, record->cell};
      }
    }
    qsort(far, far_count, sizeof(*far), partition_candidate_compare);
    for (uint32_t i = far_count;
         i > 0u && excess && out->unload_count < VKR_SCENE_PARTITION_PLAN_MAX;
         --i, --excess) {
      out->unload[out->unload_count++] = far[i - 1u].cell;
    }
  }
}

void vkr_scene_partition_shutdown(VkrScene *scene) {
  struct s_VkrScenePartition *state = scene ? scene->partition : NULL;
  if (!state) {
    return;
  }
  if (state->records) {
    vkr_allocator_free(scene->alloc, state->records,
                       state->record_capacity * sizeof(*state->records),
                       PARTITION_TAG);
  }
  if (state->slots) {
    vkr_allocator_free(scene->alloc, state->slots,
                       state->slot_capacity * sizeof(*state->slots),
                       PARTITION_TAG);
  }
  vkr_allocator_free(scene->alloc, state, sizeof(*state),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  scene->partition = NULL;
}
