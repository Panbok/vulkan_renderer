#include "script/vkr_script_internal.h"

#include "core/vkr_hash.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_types.h"

/* Tick hashes for re-simulation (docs/proposals/recordings.md, "Drive and
   re-simulation"): after each fixed tick, CRC-32s of the session's
   transforms, physics bodies and each module's state, which a recording
   keeps and a re-simulation compares tick by tick. The hashes read values,
   never addresses, so two runs of one build that simulate the same ticks
   hash alike. */

/* Entities whose typed components one call reads at a time. */
#define SCRIPT_HASH_TYPED_BATCH 256u

/* An entity as a hash sees it: its index and world without the generation,
   which grows each time a session reset frees and reuses the slot, so a
   re-simulation's entities hash like the recorded session's. */
vkr_internal uint64_t script_hash_id(VkrEntityId entity) {
  entity.parts.generation = 0u;
  return entity.u64;
}

/* Continues `state` over `size` bytes of script data, with every aligned
   64-bit word that names a live entity of an attached container hashed
   without its generation. */
vkr_internal uint32_t script_hash_data(const VkrScriptHost *host,
                                       uint32_t state, const void *data,
                                       uint64_t size) {
  const uint8_t *bytes = data;
  const uint64_t words =
      (uintptr_t)bytes % sizeof(uint64_t) == 0u ? size / sizeof(uint64_t) : 0u;
  for (uint64_t w = 0; w < words; ++w) {
    VkrEntityId value;
    MemCopy(&value.u64, bytes + w * sizeof(uint64_t), sizeof(value.u64));
    for (uint32_t c = 0;
         value.parts.generation != 0u && c < host->container_count; ++c) {
      const VkrScriptContainer *container = &host->containers[c];
      if (container->world_id == value.parts.world &&
          vkr_entity_is_alive(container->scene->world, value)) {
        value.u64 = script_hash_id(value);
        break;
      }
    }
    state = vkr_crc32_update(state, &value.u64, sizeof(value.u64));
  }
  const uint64_t tail = words * sizeof(uint64_t);
  return tail < size ? vkr_crc32_update(state, bytes + tail, size - tail)
                     : state;
}

typedef struct ScriptHashTransforms {
  VkrComponentTypeId transform;
  uint32_t *buckets;
} ScriptHashTransforms;

/* One chunk of transforms: each entity's id, position, rotation and scale,
   into the bucket its index falls in. */
vkr_internal void script_hash_transform_chunk(const VkrArchetype *archetype,
                                              VkrChunk *chunk, void *user) {
  (void)archetype;
  ScriptHashTransforms *hash = user;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  const VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  const SceneTransform *transforms =
      vkr_entity_chunk_column_const(chunk, hash->transform);
  for (uint32_t i = 0; i < count; ++i) {
    const SceneTransform *transform = &transforms[i];
    /* Vector members are read one by one: their SIMD padding is not
       state. */
    const float32_t values[10] = {transform->position.x, transform->position.y,
                                  transform->position.z, transform->rotation.x,
                                  transform->rotation.y, transform->rotation.z,
                                  transform->rotation.w, transform->scale.x,
                                  transform->scale.y,    transform->scale.z};
    const uint32_t bucket =
        entities[i].parts.index % VKR_RECORDING_HASH_BUCKETS;
    const uint64_t id = script_hash_id(entities[i]);
    uint32_t state = vkr_crc32_update(hash->buckets[bucket], &id, sizeof(id));
    hash->buckets[bucket] = vkr_crc32_update(state, values, sizeof(values));
  }
}

/* The scene's transforms in buckets by entity index. */
vkr_internal void script_hash_transforms(VkrScene *scene, uint32_t *buckets) {
  for (uint32_t b = 0; b < VKR_RECORDING_HASH_BUCKETS; ++b) {
    buckets[b] = VKR_CRC32_INITIAL;
  }
  ScriptHashTransforms hash = {.transform = scene->comp_transform,
                               .buckets = buckets};
  VkrQuery query;
  vkr_entity_query_build(scene->world, &scene->comp_transform, 1u, NULL, 0u,
                         &query);
  vkr_entity_query_each_chunk(scene->world, &query, script_hash_transform_chunk,
                              &hash);
}

/* The scene's physics bodies: each body's entity and its pose and
   velocities. */
vkr_internal uint32_t script_hash_bodies(const VkrScene *scene) {
  uint32_t state = VKR_CRC32_INITIAL;
  const uint32_t count = vkr_scene_physics_body_count(scene);
  for (uint32_t i = 0; i < count; ++i) {
    const VkrEntityId entity = vkr_scene_physics_body_at(scene, i);
    VkrPhysicsPose pose;
    if (!vkr_scene_physics_get_pose(scene, entity, &pose)) {
      continue;
    }
    float32_t values[13];
    MemCopy(values, pose.position, sizeof(pose.position));
    MemCopy(values + 3, pose.rotation, sizeof(pose.rotation));
    MemCopy(values + 7, pose.linear_velocity, sizeof(pose.linear_velocity));
    MemCopy(values + 10, pose.angular_velocity, sizeof(pose.angular_velocity));
    const uint64_t id = script_hash_id(entity);
    state = vkr_crc32_update(state, &id, sizeof(id));
    state = vkr_crc32_update(state, values, sizeof(values));
  }
  return state;
}

typedef struct ScriptHashState {
  const VkrScriptHost *host;
  VkrComponentTypeId id;
  uint32_t size;
  uint32_t state;
} ScriptHashState;

/* One chunk of a per-entity state type: each entity and its bytes. */
vkr_internal void script_hash_state_chunk(const VkrArchetype *archetype,
                                          VkrChunk *chunk, void *user) {
  (void)archetype;
  ScriptHashState *hash = user;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  const VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  const uint8_t *values = vkr_entity_chunk_column_const(chunk, hash->id);
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t id = script_hash_id(entities[i]);
    hash->state = vkr_crc32_update(hash->state, &id, sizeof(id));
    hash->state = script_hash_data(
        hash->host, hash->state, values + (uint64_t)i * hash->size, hash->size);
  }
}

/* Module `m`'s state: its instances' generators and what they fed
   vkr_sim_hash this tick, its components' values and its per-entity state,
   in every attached container. Instance data is not hashed whole: it holds
   input bookkeeping, frame-time presentation and pointers that a replay of
   the same input does not reproduce. */
vkr_internal uint32_t script_hash_module(VkrScriptHost *host, uint32_t m) {
  uint32_t state = VKR_CRC32_INITIAL;
  for (uint32_t i = 0; i < host->instance_count; ++i) {
    const VkrScriptInstance *instance = host->instances[i];
    if (instance->module != m) {
      continue;
    }
    state = vkr_crc32_update(state, &instance->random_state,
                             sizeof(instance->random_state));
    state = vkr_crc32_update(state, &instance->sim_hash,
                             sizeof(instance->sim_hash));
  }
  const VkrScriptModule *module = &host->modules[m];
  for (uint32_t c = 0; c < host->container_count; ++c) {
    VkrScene *scene = host->containers[c].scene;
    for (uint32_t t = 0; t < module->type_count; ++t) {
      const VkrTypeDesc *type = module->types[t];
      VkrEntityId entities[SCRIPT_HASH_TYPED_BATCH];
      const uint32_t found = Min(
          vkr_scene_find_typed(scene, type, entities, SCRIPT_HASH_TYPED_BATCH),
          SCRIPT_HASH_TYPED_BATCH);
      for (uint32_t e = 0; e < found; ++e) {
        const void *value = vkr_scene_get_typed(scene, entities[e], type);
        const uint64_t id = script_hash_id(entities[e]);
        state = vkr_crc32_update(state, &id, sizeof(id));
        if (value) {
          state = script_hash_data(host, state, value, type->size);
        }
      }
    }
    for (uint32_t s = 0; s < host->state_type_count; ++s) {
      const VkrScriptStateType *type = &host->state_types[s];
      if (type->module != m || type->ids[c] == VKR_COMPONENT_TYPE_INVALID) {
        continue;
      }
      ScriptHashState hash = {
          .host = host, .id = type->ids[c], .size = type->size, .state = state};
      VkrQuery query;
      vkr_entity_query_build(scene->world, &type->ids[c], 1u, NULL, 0u, &query);
      vkr_entity_query_each_chunk(scene->world, &query, script_hash_state_chunk,
                                  &hash);
      state = hash.state;
    }
  }
  return state;
}

void vkr_script_host_tick_hash(VkrScriptHost *host, VkrScene *scene,
                               uint64_t tick, VkrRecordingTickHash *out,
                               const char **out_names) {
  MemZero(out, sizeof(*out));
  out->tick = tick;
  script_hash_transforms(scene, out->transforms);
  out->bodies = script_hash_bodies(scene);
  for (uint32_t m = 0; m < host->module_count &&
                       out->module_count < VKR_RECORDING_HASH_MODULE_MAX;
       ++m) {
    if (host->modules[m].retired) {
      continue;
    }
    out_names[out->module_count] = host->modules[m].name;
    out->modules[out->module_count++] = script_hash_module(host, m);
  }
}

void vkr_script_host_tick_hash_restart(VkrScriptHost *host) {
  for (uint32_t i = 0; i < host->instance_count; ++i) {
    host->instances[i]->sim_hash = VKR_CRC32_INITIAL;
  }
}
