#include "script/vkr_script_internal.h"

#include "core/vkr_hash.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_types.h"

/* Tick hashes for re-simulation (docs/proposals/recordings.md, "Drive and
   re-simulation"): after each fixed tick, CRC-32s of the session's
   transforms, physics bodies and each module's state, which a recording
   keeps and a re-simulation compares tick by tick. The hashes read values,
   never addresses, so two runs of one build that simulate the same ticks
   hash alike.

   Entities are named by their stable key (vkr_scene_entity_stable_key),
   never their ECS slot, and each entity's hash is summed into its bucket, so
   neither the slots nor the chunk order that a session's load and unload
   history leaves (world partition cells) changes a hash. */

/* Entities whose typed components one call reads at a time. */
#define SCRIPT_HASH_TYPED_BATCH 256u
/* Slots a container's key cache grows to first. */
#define SCRIPT_HASH_KEY_CACHE_MIN 1024u

/* Grows container `c`'s key cache to hold slot `index`; false when it
   could not, and the key is then computed without the cache. */
vkr_internal bool8_t script_hash_key_cache_fit(VkrScriptHost *host, uint32_t c,
                                               uint32_t index) {
  VkrScriptKeyCache *cache = &host->key_caches[c];
  if (index < cache->capacity) {
    return true_v;
  }
  uint32_t capacity = Max(cache->capacity, SCRIPT_HASH_KEY_CACHE_MIN);
  while (capacity <= index) {
    capacity *= 2u;
  }
  uint64_t *keys = vkr_allocator_alloc(&host->instance_allocator,
                                       sizeof(uint64_t) * (uint64_t)capacity,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *generations = vkr_allocator_alloc(
      &host->instance_allocator, sizeof(uint32_t) * (uint64_t)capacity,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!keys || !generations) {
    if (keys) {
      vkr_allocator_free(&host->instance_allocator, keys,
                         sizeof(uint64_t) * (uint64_t)capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    if (generations) {
      vkr_allocator_free(&host->instance_allocator, generations,
                         sizeof(uint32_t) * (uint64_t)capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    return false_v;
  }
  MemZero(generations, sizeof(uint32_t) * (uint64_t)capacity);
  if (cache->capacity) {
    MemCopy(keys, cache->keys, sizeof(uint64_t) * (uint64_t)cache->capacity);
    MemCopy(generations, cache->generations,
            sizeof(uint32_t) * (uint64_t)cache->capacity);
    vkr_allocator_free(&host->instance_allocator, cache->keys,
                       sizeof(uint64_t) * (uint64_t)cache->capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_free(&host->instance_allocator, cache->generations,
                       sizeof(uint32_t) * (uint64_t)cache->capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  cache->keys = keys;
  cache->generations = generations;
  cache->capacity = capacity;
  return true_v;
}

/* Entity `entity` of container `c` as a hash sees it: its stable key,
   looked up once per slot and generation. */
vkr_internal uint64_t script_hash_key(VkrScriptHost *host, uint32_t c,
                                      VkrEntityId entity) {
  const uint32_t index = entity.parts.index;
  const uint32_t generation = (uint32_t)entity.parts.generation + 1u;
  VkrScriptKeyCache *cache = &host->key_caches[c];
  if (index < cache->capacity && cache->generations[index] == generation) {
    return cache->keys[index];
  }
  const uint64_t key =
      vkr_scene_entity_stable_key(host->containers[c].scene, entity);
  if (script_hash_key_cache_fit(host, c, index)) {
    cache->keys[index] = key;
    cache->generations[index] = generation;
  }
  return key;
}

/* One entity's contribution to a sum: its key and the hash of its values. */
vkr_internal uint32_t script_hash_entity(uint64_t key, uint32_t values) {
  uint32_t state = vkr_crc32_update(VKR_CRC32_INITIAL, &key, sizeof(key));
  return vkr_crc32_update(state, &values, sizeof(values));
}

/* Continues `state` over `size` bytes of script data, with every aligned
   64-bit word that names a live entity of an attached container hashed as
   that entity's stable key. */
vkr_internal uint32_t script_hash_data(VkrScriptHost *host, uint32_t state,
                                       const void *data, uint64_t size) {
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
        value.u64 = script_hash_key(host, c, value);
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
  VkrScriptHost *host;
  uint32_t container;
  VkrComponentTypeId transform;
  uint32_t *buckets;
} ScriptHashTransforms;

/* One chunk of transforms: each entity's position, rotation and scale,
   added to the bucket its stable key falls in. */
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
    const uint64_t key =
        script_hash_key(hash->host, hash->container, entities[i]);
    hash->buckets[key % VKR_RECORDING_HASH_BUCKETS] += script_hash_entity(
        key, vkr_crc32_update(VKR_CRC32_INITIAL, values, sizeof(values)));
  }
}

/* Container `c`'s transforms in buckets by stable key. */
vkr_internal void script_hash_transforms(VkrScriptHost *host, uint32_t c,
                                         uint32_t *buckets) {
  VkrScene *scene = host->containers[c].scene;
  ScriptHashTransforms hash = {.host = host,
                               .container = c,
                               .transform = scene->comp_transform,
                               .buckets = buckets};
  VkrQuery query;
  vkr_entity_query_build(scene->world, &scene->comp_transform, 1u, NULL, 0u,
                         &query);
  vkr_entity_query_each_chunk(scene->world, &query, script_hash_transform_chunk,
                              &hash);
}

/* Container `c`'s physics bodies: each body's pose and velocities, summed
   by stable key. */
vkr_internal uint32_t script_hash_bodies(VkrScriptHost *host, uint32_t c) {
  const VkrScene *scene = host->containers[c].scene;
  uint32_t sum = 0u;
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
    sum += script_hash_entity(
        script_hash_key(host, c, entity),
        vkr_crc32_update(VKR_CRC32_INITIAL, values, sizeof(values)));
  }
  return sum;
}

typedef struct ScriptHashState {
  VkrScriptHost *host;
  uint32_t container;
  VkrComponentTypeId id;
  uint32_t size;
  uint32_t sum;
} ScriptHashState;

/* One chunk of a per-entity state type: each entity's bytes, summed by
   stable key. */
vkr_internal void script_hash_state_chunk(const VkrArchetype *archetype,
                                          VkrChunk *chunk, void *user) {
  (void)archetype;
  ScriptHashState *hash = user;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  const VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  const uint8_t *values = vkr_entity_chunk_column_const(chunk, hash->id);
  for (uint32_t i = 0; i < count; ++i) {
    hash->sum += script_hash_entity(
        script_hash_key(hash->host, hash->container, entities[i]),
        script_hash_data(hash->host, VKR_CRC32_INITIAL,
                         values + (uint64_t)i * hash->size, hash->size));
  }
}

/* Module `m`'s state: its instances' generators and what they fed
   vkr_sim_hash this tick, in instance order, then its components' values
   and its per-entity state in every attached container, summed by stable
   key. Instance data is not hashed whole: it holds input bookkeeping,
   frame-time presentation and pointers that a replay of the same input does
   not reproduce. */
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
    uint32_t sum = 0u;
    for (uint32_t t = 0; t < module->type_count; ++t) {
      const VkrTypeDesc *type = module->types[t];
      VkrEntityId entities[SCRIPT_HASH_TYPED_BATCH];
      const uint32_t found = Min(
          vkr_scene_find_typed(scene, type, entities, SCRIPT_HASH_TYPED_BATCH),
          SCRIPT_HASH_TYPED_BATCH);
      for (uint32_t e = 0; e < found; ++e) {
        const void *value = vkr_scene_get_typed(scene, entities[e], type);
        sum += script_hash_entity(
            script_hash_key(host, c, entities[e]) ^ (uint64_t)t,
            value ? script_hash_data(host, VKR_CRC32_INITIAL, value, type->size)
                  : 0u);
      }
    }
    for (uint32_t s = 0; s < host->state_type_count; ++s) {
      const VkrScriptStateType *type = &host->state_types[s];
      if (type->module != m || type->ids[c] == VKR_COMPONENT_TYPE_INVALID) {
        continue;
      }
      ScriptHashState hash = {
          .host = host, .container = c, .id = type->ids[c], .size = type->size};
      VkrQuery query;
      vkr_entity_query_build(scene->world, &type->ids[c], 1u, NULL, 0u, &query);
      vkr_entity_query_each_chunk(scene->world, &query, script_hash_state_chunk,
                                  &hash);
      sum += hash.sum * (2u * s + 1u);
    }
    state = vkr_crc32_update(state, &sum, sizeof(sum));
  }
  return state;
}

void vkr_script_host_tick_hash(VkrScriptHost *host, VkrScene *scene,
                               uint64_t tick, VkrRecordingTickHash *out,
                               const char **out_names) {
  MemZero(out, sizeof(*out));
  out->tick = tick;
  uint32_t c = 0u;
  while (c < host->container_count && host->containers[c].scene != scene) {
    ++c;
  }
  if (c < host->container_count) {
    script_hash_transforms(host, c, out->transforms);
    out->bodies = script_hash_bodies(host, c);
  }
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
