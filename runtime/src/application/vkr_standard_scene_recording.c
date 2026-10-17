#include "application/vkr_standard_scene_runtime.h"

#include "containers/str.h"
#include "containers/vkr_hashtable.h"
#include "core/logger.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_animation.h"
#include "renderer/systems/vkr_scene_model.h"

/* The frame's part of recordings (docs/proposals/recordings.md): what the
   recording in progress writes after the camera update, and how an open
   playback places the camera before it. The client starts, stops, opens and
   closes them (vkr_sample_runtime.c). */

vkr_internal Vec3
vkr_standard_scene_recording_origin(VkrStandardSceneRuntime *runtime) {
  VkrScene *level = vkr_standard_scene_runtime_level_scene(runtime);
  return level ? level->origin_offset : vec3_zero();
}

/* ------------------------------------------------------------------------ */
/* Entity poses (docs/proposals/recordings.md, "Recorded state")             */

#define VKR_RECORDING_CONTAINER_COUNT                                          \
  (VKR_RECORDING_CONTAINER_ADDITIVE + VKR_SCENE_ADDITIVE_MAX)

/* The scene at VKR_RECORDING_CONTAINER_* index `index`, or NULL. The World
   counts only apart from the active scene. */
vkr_internal VkrScene *
vkr_standard_scene_recording_container(VkrStandardSceneRuntime *runtime,
                                       uint32_t index) {
  if (index == VKR_RECORDING_CONTAINER_ACTIVE) {
    return runtime->active_scene;
  }
  if (index == VKR_RECORDING_CONTAINER_WORLD) {
    return runtime->world_scene != runtime->active_scene ? runtime->world_scene
                                                         : NULL;
  }
  const uint32_t additive = index - VKR_RECORDING_CONTAINER_ADDITIVE;
  return additive < runtime->additive_count ? runtime->additive_scenes[additive]
                                            : NULL;
}

/* How a recording names `entity` (VkrRecordingPose): its own ref; for a
   model node, the nearest ancestor's ref and the node; without refs, its
   document index. False for an entity it cannot name, such as a script
   spawn. */
vkr_internal bool8_t vkr_standard_scene_recording_identity(
    VkrScene *scene, VkrEntityId entity, VkrEntityRef *out_ref,
    uint32_t *out_node, uint32_t *out_document) {
  *out_node = VKR_RECORDING_POSE_NODE_NONE;
  *out_document = VKR_RECORDING_POSE_DOCUMENT_NONE;
  if (vkr_scene_entity_ref(scene, entity, out_ref)) {
    return true_v;
  }
  MemZero(out_ref, sizeof(*out_ref));
  const SceneSourceIdentity *identity = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_source_identity);
  if (!identity || identity->scene_entity_index == UINT32_MAX) {
    return false_v;
  }
  if (identity->gltf_node_index == UINT32_MAX) {
    *out_document = identity->scene_entity_index;
    return true_v;
  }
  *out_node = identity->gltf_node_index;
  const SceneTransform *transform = vkr_scene_get_transform(scene, entity);
  VkrEntityId parent = transform ? transform->parent : VKR_ENTITY_ID_INVALID;
  while (parent.u64 != VKR_ENTITY_ID_INVALID.u64) {
    if (vkr_scene_entity_ref(scene, parent, out_ref)) {
      return true_v;
    }
    transform = vkr_scene_get_transform(scene, parent);
    parent = transform ? transform->parent : VKR_ENTITY_ID_INVALID;
  }
  /* A node of a model in a document without refs: the wrapper's document
     index, which its nodes carry. */
  *out_document = identity->scene_entity_index;
  return true_v;
}

/* Names a script spawn, or a node of a model spawned under one, by spawn
   id (VKR_RECORDING_POSE_KIND_SPAWN). False for anything else or when the
   recording's spawn table is full. */
vkr_internal bool8_t vkr_standard_scene_recording_spawn_name(
    VkrStandardSceneRuntime *runtime, VkrScene *scene, uint8_t container,
    VkrEntityId entity, VkrRecordingPose *pose) {
  VkrEntityId named = entity;
  pose->node = VKR_RECORDING_POSE_NODE_NONE;
  const SceneSourceIdentity *identity = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_source_identity);
  if (identity && identity->gltf_node_index != UINT32_MAX) {
    /* A model node: the wrapper above it that holds the spawned model. */
    const SceneTransform *transform = vkr_scene_get_transform(scene, entity);
    named = transform ? transform->parent : VKR_ENTITY_ID_INVALID;
    while (named.u64 != VKR_ENTITY_ID_INVALID.u64 &&
           vkr_scene_model_status(scene, named, NULL) == VKR_SCENE_MODEL_NONE) {
      transform = vkr_scene_get_transform(scene, named);
      named = transform ? transform->parent : VKR_ENTITY_ID_INVALID;
    }
    pose->node = identity->gltf_node_index;
  }
  if (named.u64 == VKR_ENTITY_ID_INVALID.u64 ||
      !vkr_scene_entity_transient(scene, named)) {
    return false_v;
  }
  const uint32_t id =
      vkr_recording_spawn_id(&runtime->recording, named.u64, container);
  if (id == UINT32_MAX) {
    return false_v;
  }
  MemZero(&pose->ref, sizeof(pose->ref));
  pose->kind = VKR_RECORDING_POSE_KIND_SPAWN;
  pose->document = id;
  return true_v;
}

/* Writes one changed entity's world pose; skips one it cannot name. */
vkr_internal void
vkr_standard_scene_recording_pose(VkrStandardSceneRuntime *runtime,
                                  VkrScene *scene, uint8_t container,
                                  VkrEntityId entity) {
  const SceneTransform *transform = vkr_scene_get_transform(scene, entity);
  VkrRecordingPose pose = {.container = container};
  if (!transform ||
      (!vkr_standard_scene_recording_identity(scene, entity, &pose.ref,
                                              &pose.node, &pose.document) &&
       !vkr_standard_scene_recording_spawn_name(runtime, scene, container,
                                                entity, &pose))) {
    return;
  }
  const Mat4 *world = &transform->world;
  pose.translation[0] =
      (float64_t)world->m03 + (float64_t)scene->origin_offset.x;
  pose.translation[1] =
      (float64_t)world->m13 + (float64_t)scene->origin_offset.y;
  pose.translation[2] =
      (float64_t)world->m23 + (float64_t)scene->origin_offset.z;
  for (uint32_t column = 0u; column < 3u; ++column) {
    for (uint32_t row = 0u; row < 3u; ++row) {
      pose.linear[column * 3u + row] = world->elements[column * 4u + row];
    }
  }
  vkr_recording_pose(&runtime->recording, &pose);
}

/* Writes the despawns of spawns that went away and the models spawns got
   since the last frame. */
vkr_internal void
vkr_standard_scene_recording_spawns(VkrStandardSceneRuntime *runtime) {
  VkrRecording *recording = &runtime->recording;
  for (uint32_t id = 0u; id < recording->spawn_count; ++id) {
    const uint8_t flags = recording->spawn_flags[id];
    if (!(flags & VKR_RECORDING_SPAWN_ALIVE)) {
      continue;
    }
    VkrScene *scene = vkr_standard_scene_recording_container(
        runtime, recording->spawn_containers[id]);
    const VkrEntityId entity = {.u64 = recording->spawn_entities[id]};
    if (!scene || !vkr_scene_entity_alive(scene, entity)) {
      vkr_recording_despawn(recording, id);
      continue;
    }
    if (flags & VKR_RECORDING_SPAWN_LOOK) {
      continue;
    }
    String8 mesh;
    String8 animation;
    const SceneShape *shape = vkr_entity_get_component_if_alive_const(
        scene->world, entity, scene->comp_shape);
    if (vkr_scene_model_paths(scene, entity, &mesh, &animation)) {
      vkr_recording_spawn_look(recording, id, mesh, animation, NULL);
    } else if (shape) {
      const VkrRecordingSpawnShape look = {
          .type = (uint32_t)shape->type,
          .size = {shape->dimensions.x, shape->dimensions.y,
                   shape->dimensions.z},
          .color = {shape->color.x, shape->color.y, shape->color.z,
                    shape->color.w},
      };
      vkr_recording_spawn_look(recording, id, (String8){0}, (String8){0},
                               &look);
    }
  }
}

/* Writes the poses of entities that changed since the last frame, at most
   VKR_RECORDING_POSE_FRAME_MAX; a container seen for the first time lists
   all its entities, which the next frames continue. */
vkr_internal void
vkr_standard_scene_recording_poses(VkrStandardSceneRuntime *runtime) {
  uint32_t budget = VKR_RECORDING_POSE_FRAME_MAX;
  for (uint32_t c = 0u; c < VKR_RECORDING_CONTAINER_COUNT; ++c) {
    VkrScene *scene = vkr_standard_scene_recording_container(runtime, c);
    /* A new container, or one reloaded in place, starts with all its
       entities. */
    if (runtime->recording_tracked[c] != scene ||
        (scene && !scene->changes_tracked)) {
      runtime->recording_tracked[c] = scene;
      if (scene) {
        (void)vkr_scene_track_changes(scene, true_v);
      }
    }
    if (!scene) {
      continue;
    }
    uint32_t count = 0u;
    bool8_t overflow = false_v;
    const VkrEntityId *changed =
        vkr_scene_changed_entities(scene, &count, &overflow);
    if (overflow) {
      /* The list lost entries, so every entity is written again. */
      for (uint32_t i = 0u; i < scene->topo_count; ++i) {
        vkr_standard_scene_recording_pose(runtime, scene, (uint8_t)c,
                                          scene->topo_order[i]);
      }
      vkr_scene_changed_entities_consume(scene, count);
      continue;
    }
    const uint32_t taken = Min(count, budget);
    for (uint32_t i = 0u; i < taken; ++i) {
      vkr_standard_scene_recording_pose(runtime, scene, (uint8_t)c, changed[i]);
    }
    vkr_scene_changed_entities_consume(scene, taken);
    budget -= taken;
  }
  vkr_recording_poses_flush(&runtime->recording);
  vkr_standard_scene_recording_spawns(runtime);
}

/* Stops tracking the containers a finished recording tracked. */
vkr_internal void
vkr_standard_scene_recording_untrack(VkrStandardSceneRuntime *runtime) {
  for (uint32_t c = 0u; c < VKR_RECORDING_CONTAINER_COUNT; ++c) {
    VkrScene *scene = runtime->recording_tracked[c];
    /* A container that went away took its tracking with it. */
    if (scene && scene == vkr_standard_scene_recording_container(runtime, c)) {
      (void)vkr_scene_track_changes(scene, false_v);
    }
    runtime->recording_tracked[c] = NULL;
  }
}

/* The world matrix of a recorded pose in `scene`'s space. */
vkr_internal Mat4 vkr_standard_scene_recording_pose_world(
    const VkrScene *scene, const VkrRecordingPoseSample *sample) {
  Mat4 world = mat4_identity();
  for (uint32_t column = 0u; column < 3u; ++column) {
    for (uint32_t row = 0u; row < 3u; ++row) {
      world.elements[column * 4u + row] = sample->linear[column * 3u + row];
    }
  }
  world.m03 =
      (float32_t)(sample->translation[0] - (float64_t)scene->origin_offset.x);
  world.m13 =
      (float32_t)(sample->translation[1] - (float64_t)scene->origin_offset.y);
  world.m23 =
      (float32_t)(sample->translation[2] - (float64_t)scene->origin_offset.z);
  return world;
}

/* A hash of a pose key's name. */
vkr_internal uint64_t vkr_standard_scene_recording_key_hash(
    uint8_t container, uint8_t kind, const VkrEntityRef *ref, uint32_t node,
    uint32_t document) {
  uint8_t bytes[sizeof(VkrEntityRef) + 10u];
  bytes[0] = container;
  bytes[1] = kind;
  MemCopy(bytes + 2u, ref, sizeof(*ref));
  MemCopy(bytes + 2u + sizeof(*ref), &node, sizeof(node));
  MemCopy(bytes + 6u + sizeof(*ref), &document, sizeof(document));
  return vkr_hash_table_key_string8(
             string8_create_from_cstr(bytes, sizeof(bytes)))
      .hash;
}

/* Creates the transient stand-ins of spawns that had a model or a shape,
   hidden until their first frame, and loads their models. */
vkr_internal bool8_t
vkr_standard_scene_recording_spawn_proxies(VkrStandardSceneRuntime *runtime) {
  VkrRecordingPlayback *playback = &runtime->playback;
  const uint64_t count = (uint64_t)playback->spawn_count + 1u;
  playback->spawn_proxies =
      vkr_allocator_alloc(&playback->allocator, sizeof(uint64_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->spawn_shown = vkr_allocator_alloc(&playback->allocator, count,
                                              VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!playback->spawn_proxies || !playback->spawn_shown) {
    return false_v;
  }
  MemZero(playback->spawn_shown, count);
  /* Node keys grouped by spawn, for the animated stand-ins' poses. */
  playback->spawn_node_first =
      vkr_allocator_alloc(&playback->allocator, sizeof(uint32_t) * (count + 1u),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->spawn_node_keys = vkr_allocator_alloc(
      &playback->allocator,
      sizeof(uint32_t) * ((uint64_t)playback->pose_key_count + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->spawn_self_key =
      vkr_allocator_alloc(&playback->allocator, sizeof(uint32_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!playback->spawn_node_first || !playback->spawn_node_keys ||
      !playback->spawn_self_key) {
    return false_v;
  }
  for (uint32_t id = 0u; id < count; ++id) {
    playback->spawn_self_key[id] = UINT32_MAX;
  }
  MemZero(playback->spawn_node_first, sizeof(uint32_t) * (count + 1u));
  for (uint32_t k = 0u; k < playback->pose_key_count; ++k) {
    const VkrRecordingPoseKey *key = &playback->pose_keys[k];
    if (key->kind != VKR_RECORDING_POSE_KIND_SPAWN ||
        key->document >= playback->spawn_count) {
      continue;
    }
    if (key->node == VKR_RECORDING_POSE_NODE_NONE) {
      playback->spawn_self_key[key->document] = k;
    } else {
      ++playback->spawn_node_first[key->document + 1u];
    }
  }
  for (uint32_t id = 0u; id < playback->spawn_count; ++id) {
    playback->spawn_node_first[id + 1u] += playback->spawn_node_first[id];
  }
  uint32_t *cursor = playback->spawn_node_keys;
  for (uint32_t id = 0u; id < playback->spawn_count; ++id) {
    for (uint32_t k = 0u; k < playback->pose_key_count; ++k) {
      const VkrRecordingPoseKey *key = &playback->pose_keys[k];
      if (key->kind == VKR_RECORDING_POSE_KIND_SPAWN &&
          key->node != VKR_RECORDING_POSE_NODE_NONE && key->document == id) {
        *cursor++ = k;
      }
    }
  }
  playback->spawn_pose_frame = UINT64_MAX;
  uint32_t created = 0u;
  for (uint32_t id = 0u; id < playback->spawn_count; ++id) {
    playback->spawn_proxies[id] = VKR_ENTITY_ID_INVALID.u64;
    const VkrRecordingSpawn *spawn = &playback->spawns[id];
    VkrScene *scene =
        vkr_standard_scene_recording_container(runtime, spawn->container);
    String8 mesh;
    String8 animation;
    const bool8_t model =
        vkr_recording_playback_spawn_model(playback, id, &mesh, &animation);
    if (!scene || (!model && !spawn->has_shape)) {
      continue;
    }
    const VkrEntityId proxy = vkr_scene_create_entity(scene, NULL);
    const char *error = NULL;
    bool8_t made = proxy.u64 &&
                   vkr_scene_set_transform(scene, proxy, vec3_zero(),
                                           vkr_quat_identity(), vec3_one()) &&
                   vkr_scene_set_transient(scene, proxy);
    if (made && model) {
      VkrSceneModelDesc desc = {.mesh_path = mesh,
                                .animation_path = animation,
                                .animation =
                                    VKR_SCENE_ANIMATION_CONFIG_DEFAULT};
      made = vkr_scene_request_model(scene, &runtime->assets, proxy, &desc,
                                     &error);
    } else if (made) {
      VkrSceneShapeConfig shape = VKR_SCENE_SHAPE_CONFIG_DEFAULT;
      shape.type = (SceneShapeType)spawn->shape.type;
      shape.dimensions = vec3_new(spawn->shape.size[0], spawn->shape.size[1],
                                  spawn->shape.size[2]);
      shape.color = vec4_new(spawn->shape.color[0], spawn->shape.color[1],
                             spawn->shape.color[2], spawn->shape.color[3]);
      made = vkr_scene_set_shape(scene, &runtime->assets, proxy, &shape, NULL);
    }
    if (!made) {
      if (proxy.u64) {
        vkr_scene_destroy_entity(scene, proxy);
      }
      log_warn("Playback could not stand in for spawn %u: %s", id,
               error ? error : "entity creation failed");
      continue;
    }
    vkr_scene_set_visibility(scene, proxy, false_v, true_v);
    playback->spawn_proxies[id] = proxy.u64;
    ++created;
  }
  if (created > 0u) {
    log_info("Playback stands in for %u of %u spawns", created,
             playback->spawn_count);
  }
  return true_v;
}

/* Binds the spawn keys still unbound: a spawn's own key to its stand-in,
   a node's key to the node once the stand-in's model is ready. Sets
   poses_pending while some wait for a model. */
vkr_internal void
vkr_standard_scene_recording_bind_spawns(VkrStandardSceneRuntime *runtime) {
  VkrRecordingPlayback *playback = &runtime->playback;
  playback->poses_pending = false_v;
  for (uint32_t k = 0u; k < playback->pose_key_count; ++k) {
    const VkrRecordingPoseKey *key = &playback->pose_keys[k];
    if (key->kind != VKR_RECORDING_POSE_KIND_SPAWN ||
        playback->pose_entities[k] != VKR_ENTITY_ID_INVALID.u64 ||
        key->document >= playback->spawn_count) {
      continue;
    }
    const VkrEntityId proxy = {.u64 = playback->spawn_proxies[key->document]};
    VkrScene *scene =
        vkr_standard_scene_recording_container(runtime, key->container);
    if (!scene || proxy.u64 == VKR_ENTITY_ID_INVALID.u64) {
      continue;
    }
    if (key->node == VKR_RECORDING_POSE_NODE_NONE) {
      playback->pose_entities[k] = proxy.u64;
      continue;
    }
    const VkrEntityId node = vkr_scene_model_node(scene, proxy, key->node);
    if (node.u64 != VKR_ENTITY_ID_INVALID.u64) {
      playback->pose_entities[k] = node.u64;
    } else if (vkr_scene_model_status(scene, proxy, NULL) ==
               VKR_SCENE_MODEL_LOADING) {
      playback->poses_pending = true_v;
    }
  }
}

/* An animated stand-in draws its skin from its player's pose, not from its
   node entities: the recorded node poses at `frame`, relative to the
   stand-in's recorded pose of that frame, become that pose. With the scene
   stopped the player does not advance, so the pose stays. */
vkr_internal void
vkr_standard_scene_recording_pose_skins(VkrStandardSceneRuntime *runtime,
                                        uint64_t frame) {
  VkrRecordingPlayback *playback = &runtime->playback;
  if (playback->spawn_pose_frame == frame) {
    return;
  }
  /* A stand-in whose model still loads takes its pose once it is ready. */
  playback->spawn_pose_frame = playback->poses_pending ? UINT64_MAX : frame;
  for (uint32_t id = 0u; id < playback->spawn_count; ++id) {
    const VkrEntityId proxy = {.u64 = playback->spawn_proxies[id]};
    VkrScene *scene = vkr_standard_scene_recording_container(
        runtime, playback->spawns[id].container);
    VkrAnimationPlayer *player =
        scene && proxy.u64 != VKR_ENTITY_ID_INVALID.u64
            ? vkr_scene_animation_get_player(scene, proxy)
            : NULL;
    const uint32_t self = playback->spawn_self_key[id];
    const uint32_t self_sample =
        self == UINT32_MAX
            ? UINT32_MAX
            : vkr_recording_playback_pose_at(playback, self, frame);
    if (!player || self_sample == UINT32_MAX) {
      continue;
    }
    const Mat4 inverse = mat4_inverse(vkr_standard_scene_recording_pose_world(
        scene, &playback->pose_samples[self_sample]));
    vkr_animation_player_set_playing(player, false_v);
    uint32_t nodes[256];
    Mat4 globals[256];
    uint32_t count = 0u;
    for (uint32_t i = playback->spawn_node_first[id];
         i < playback->spawn_node_first[id + 1u] && count < ArrayCount(nodes);
         ++i) {
      const uint32_t k = playback->spawn_node_keys[i];
      const uint32_t sample =
          vkr_recording_playback_pose_at(playback, k, frame);
      if (sample == UINT32_MAX) {
        continue;
      }
      nodes[count] = playback->pose_keys[k].node;
      Mat4 global =
          mat4_mul(inverse, vkr_standard_scene_recording_pose_world(
                                scene, &playback->pose_samples[sample]));
      /* Both are affine; the inverse's rounding must not leave the bottom
         row the player checks. */
      global.elements[3] = 0.0f;
      global.elements[7] = 0.0f;
      global.elements[11] = 0.0f;
      global.elements[15] = 1.0f;
      globals[count++] = global;
    }
    if (count > 0u &&
        !vkr_animation_player_override_globals(player, nodes, globals, count)) {
      log_warn("Playback could not pose spawn %u", id);
    }
  }
}

/* Shows each stand-in from its spawn's first frame until it went away. */
vkr_internal void
vkr_standard_scene_recording_show_spawns(VkrStandardSceneRuntime *runtime,
                                         uint64_t frame) {
  VkrRecordingPlayback *playback = &runtime->playback;
  for (uint32_t id = 0u; id < playback->spawn_count; ++id) {
    const VkrEntityId proxy = {.u64 = playback->spawn_proxies[id]};
    const VkrRecordingSpawn *spawn = &playback->spawns[id];
    VkrScene *scene =
        vkr_standard_scene_recording_container(runtime, spawn->container);
    if (!scene || proxy.u64 == VKR_ENTITY_ID_INVALID.u64) {
      continue;
    }
    const uint8_t shown =
        frame >= spawn->first_frame && frame < spawn->last_frame ? 1u : 0u;
    if (shown != playback->spawn_shown[id]) {
      vkr_scene_set_visibility(scene, proxy, shown != 0u, true_v);
      playback->spawn_shown[id] = shown;
    }
  }
}

/* Finds each pose key's live entity with one walk over the containers, and
   marks a key whose only pose matches its entity already as static. */
vkr_internal bool8_t
vkr_standard_scene_recording_bind_poses(VkrStandardSceneRuntime *runtime) {
  VkrRecordingPlayback *playback = &runtime->playback;
  const uint32_t keys = playback->pose_key_count;
  if (!playback->pose_entities) {
    playback->pose_entities =
        vkr_allocator_alloc(&playback->allocator, sizeof(uint64_t) * keys,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    playback->pose_applied =
        vkr_allocator_alloc(&playback->allocator, sizeof(uint32_t) * keys,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  uint32_t slot_count = 16u;
  while (slot_count < keys * 2u) {
    slot_count *= 2u;
  }
  uint32_t *slots =
      vkr_allocator_alloc(&playback->allocator, sizeof(uint32_t) * slot_count,
                          VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  if (!playback->pose_entities || !playback->pose_applied || !slots ||
      (!playback->spawn_proxies &&
       !vkr_standard_scene_recording_spawn_proxies(runtime))) {
    return false_v;
  }
  /* Slots hold key + 1; zero is empty. */
  MemZero(slots, sizeof(uint32_t) * slot_count);
  const uint32_t mask = slot_count - 1u;
  for (uint32_t k = 0u; k < keys; ++k) {
    const VkrRecordingPoseKey *key = &playback->pose_keys[k];
    uint32_t probe =
        (uint32_t)vkr_standard_scene_recording_key_hash(
            key->container, key->kind, &key->ref, key->node, key->document) &
        mask;
    while (slots[probe] != 0u) {
      probe = (probe + 1u) & mask;
    }
    slots[probe] = k + 1u;
    playback->pose_entities[k] = VKR_ENTITY_ID_INVALID.u64;
    playback->pose_applied[k] = VKR_RECORDING_POSE_APPLIED_NONE;
  }
  for (uint32_t c = 0u; c < VKR_RECORDING_CONTAINER_COUNT; ++c) {
    VkrScene *scene = vkr_standard_scene_recording_container(runtime, c);
    for (uint32_t i = 0u; scene && i < scene->topo_count; ++i) {
      const VkrEntityId entity = scene->topo_order[i];
      VkrEntityRef ref;
      uint32_t node = 0u;
      uint32_t document = 0u;
      if (!vkr_standard_scene_recording_identity(scene, entity, &ref, &node,
                                                 &document)) {
        continue;
      }
      uint32_t probe = (uint32_t)vkr_standard_scene_recording_key_hash(
                           (uint8_t)c, VKR_RECORDING_POSE_KIND_DOCUMENT, &ref,
                           node, document) &
                       mask;
      while (slots[probe] != 0u) {
        const uint32_t k = slots[probe] - 1u;
        const VkrRecordingPoseKey *key = &playback->pose_keys[k];
        if (key->container == c &&
            key->kind == VKR_RECORDING_POSE_KIND_DOCUMENT &&
            key->node == node && key->document == document &&
            MemCompare(&key->ref, &ref, sizeof(ref)) == 0) {
          playback->pose_entities[k] = entity.u64;
          break;
        }
        probe = (probe + 1u) & mask;
      }
    }
  }
  for (uint32_t k = 0u; k < keys; ++k) {
    const VkrRecordingPoseKey *key = &playback->pose_keys[k];
    VkrScene *scene =
        vkr_standard_scene_recording_container(runtime, key->container);
    const VkrEntityId entity = {.u64 = playback->pose_entities[k]};
    const SceneTransform *transform =
        scene && key->count == 1u &&
                key->kind == VKR_RECORDING_POSE_KIND_DOCUMENT
            ? vkr_scene_get_transform(scene, entity)
            : NULL;
    if (!transform) {
      continue;
    }
    const Mat4 world = vkr_standard_scene_recording_pose_world(
        scene, &playback->pose_samples[key->first]);
    if (MemCompare(&world, &transform->world, sizeof(world)) == 0) {
      playback->pose_applied[k] = VKR_RECORDING_POSE_APPLIED_STATIC;
    }
  }
  vkr_allocator_free(&playback->allocator, slots, sizeof(uint32_t) * slot_count,
                     VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  vkr_standard_scene_recording_bind_spawns(runtime);
  uint32_t bound = 0u;
  uint32_t still = 0u;
  for (uint32_t k = 0u; k < keys; ++k) {
    bound += playback->pose_entities[k] != VKR_ENTITY_ID_INVALID.u64 ? 1u : 0u;
    still += playback->pose_applied[k] == VKR_RECORDING_POSE_APPLIED_STATIC
                 ? 1u
                 : 0u;
  }
  log_info("Playback poses: %u entities, %u found in the scene, %u already "
           "in place",
           keys, bound, still);
  playback->poses_bound = true_v;
  return true_v;
}

/* Poses each bound entity as the recording had it at the playhead's frame:
   its last pose until then as an evaluated override, or its own transform
   before the recording first moved it. */
vkr_internal void
vkr_standard_scene_recording_apply_poses(VkrStandardSceneRuntime *runtime) {
  VkrRecordingPlayback *playback = &runtime->playback;
  if (playback->pose_key_count == 0u) {
    return;
  }
  if (!playback->poses_bound ||
      runtime->playback_pose_generation != runtime->scene_generation) {
    /* A reloaded scene took the stand-ins with it. */
    if (runtime->playback_pose_generation != runtime->scene_generation) {
      playback->spawn_proxies = NULL;
    }
    runtime->playback_pose_generation = runtime->scene_generation;
    if (!vkr_standard_scene_recording_bind_poses(runtime)) {
      return;
    }
  }
  if (playback->poses_pending) {
    vkr_standard_scene_recording_bind_spawns(runtime);
  }
  const uint64_t frame =
      vkr_recording_playback_frame_at(playback, playback->time);
  vkr_standard_scene_recording_show_spawns(runtime, frame);
  vkr_standard_scene_recording_pose_skins(runtime, frame);
  for (uint32_t k = 0u; k < playback->pose_key_count; ++k) {
    const VkrEntityId entity = {.u64 = playback->pose_entities[k]};
    const uint32_t applied = playback->pose_applied[k];
    if (entity.u64 == VKR_ENTITY_ID_INVALID.u64 ||
        applied == VKR_RECORDING_POSE_APPLIED_STATIC) {
      continue;
    }
    const uint32_t sample = vkr_recording_playback_pose_at(playback, k, frame);
    if (sample == applied) {
      continue;
    }
    VkrScene *scene = vkr_standard_scene_recording_container(
        runtime, playback->pose_keys[k].container);
    if (!scene) {
      continue;
    }
    if (sample == UINT32_MAX) {
      (void)vkr_scene_set_evaluated_transform(scene, entity, NULL);
    } else {
      const Mat4 world = vkr_standard_scene_recording_pose_world(
          scene, &playback->pose_samples[sample]);
      (void)vkr_scene_set_evaluated_transform(scene, entity, &world);
    }
    playback->pose_applied[k] = sample;
  }
}

void vkr_standard_scene_recording_release_poses(
    VkrStandardSceneRuntime *runtime) {
  VkrRecordingPlayback *playback = &runtime->playback;
  if (!playback->open || !playback->poses_bound ||
      runtime->playback_pose_generation != runtime->scene_generation) {
    return;
  }
  for (uint32_t k = 0u; k < playback->pose_key_count; ++k) {
    const VkrEntityId entity = {.u64 = playback->pose_entities[k]};
    const uint32_t applied = playback->pose_applied[k];
    VkrScene *scene = vkr_standard_scene_recording_container(
        runtime, playback->pose_keys[k].container);
    if (scene && entity.u64 != VKR_ENTITY_ID_INVALID.u64 &&
        applied != VKR_RECORDING_POSE_APPLIED_NONE &&
        applied != VKR_RECORDING_POSE_APPLIED_STATIC) {
      (void)vkr_scene_set_evaluated_transform(scene, entity, NULL);
    }
    playback->pose_applied[k] = VKR_RECORDING_POSE_APPLIED_NONE;
  }
  playback->poses_bound = false_v;
}

void vkr_standard_scene_recording_close_world(
    VkrStandardSceneRuntime *runtime) {
  vkr_standard_scene_recording_release_poses(runtime);
  VkrRecordingPlayback *playback = &runtime->playback;
  if (!playback->open || !playback->spawn_proxies) {
    return;
  }
  for (uint32_t id = 0u; id < playback->spawn_count; ++id) {
    const VkrEntityId proxy = {.u64 = playback->spawn_proxies[id]};
    VkrScene *scene = vkr_standard_scene_recording_container(
        runtime, playback->spawns[id].container);
    if (scene && proxy.u64 != VKR_ENTITY_ID_INVALID.u64 &&
        vkr_scene_entity_alive(scene, proxy)) {
      vkr_scene_destroy_entity(scene, proxy);
    }
    playback->spawn_proxies[id] = VKR_ENTITY_ID_INVALID.u64;
  }
  playback->spawn_proxies = NULL;
}

void vkr_standard_scene_recording_advance(VkrStandardSceneRuntime *runtime,
                                          float64_t delta) {
  if (!runtime->playback.open) {
    return;
  }
  vkr_recording_playback_advance(&runtime->playback, delta);
  vkr_standard_scene_recording_apply_poses(runtime);
}

bool8_t
vkr_standard_scene_recording_drive_camera(VkrStandardSceneRuntime *runtime,
                                          VkrCamera *camera) {
  VkrRecordingPlayback *playback = &runtime->playback;
  if (!playback->open || !camera) {
    return false_v;
  }
  const VkrRecordingPlaybackPose pose = vkr_recording_playback_pose(playback);
  const Vec3 origin = vkr_standard_scene_recording_origin(runtime);
  const Vec3 position =
      vec3_new((float32_t)(pose.camera.position[0] - (float64_t)origin.x),
               (float32_t)(pose.camera.position[1] - (float64_t)origin.y),
               (float32_t)(pose.camera.position[2] - (float64_t)origin.z));
  vkr_camera_set_pose(camera, position, pose.camera.yaw, pose.camera.pitch);
  if (camera->zoom != pose.camera.fov ||
      camera->near_clip != pose.camera.near_clip ||
      camera->far_clip != pose.camera.far_clip) {
    camera->zoom = pose.camera.fov;
    camera->near_clip = pose.camera.near_clip;
    camera->far_clip = pose.camera.far_clip;
    camera->projection_dirty = true_v;
  }
  VkrScene *level = vkr_standard_scene_runtime_level_scene(runtime);
  if (level && level->clock.hour != (float64_t)pose.time_of_day) {
    (void)vkr_scene_set_time_of_day_hour(level, pose.time_of_day);
  }
  return true_v;
}

/* With $VKR_RECORDING_DIRECTORY set, the run records from its first frame
   into <directory>/run-<pid>-<unix time>.vkrrec, raw because the runtime
   carries no compressor; the harness passes it to its child, so a timing case
   measures a recorded run. */
vkr_internal void vkr_standard_scene_recording_from_environment(
    VkrStandardSceneRuntime *runtime) {
  runtime->recording_environment_read = true_v;
  char directory[VKR_RECORDING_PATH_MAX - 64u];
  if (!vkr_platform_environment_variable("VKR_RECORDING_DIRECTORY", directory,
                                         sizeof(directory)) ||
      !directory[0] || runtime->recording.active) {
    return;
  }
  char path[VKR_RECORDING_PATH_MAX];
  string_format(path, sizeof(path), "%s/run-%u-%lld%s", directory,
                vkr_platform_get_process_id(),
                (long long)vkr_platform_get_unix_time(),
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {
      .bundle_path = path,
      .name = "run",
      .author = "environment",
      .metrics = runtime->metrics,
      .identity_json =
          runtime->renderer.graphics_pipeline == VKR_GRAPHICS_PIPELINE_TILED
              ? "{\"source\":\"VKR_RECORDING_DIRECTORY\",\"pipeline\":"
                "\"tiled\"}"
              : "{\"source\":\"VKR_RECORDING_DIRECTORY\",\"pipeline\":"
                "\"desktop\"}",
  };
  if (vkr_recording_start(&runtime->recording, &config)) {
    log_info("Recording the run to %s", path);
  } else {
    log_error("VKR_RECORDING_DIRECTORY: could not create %s", path);
  }
}

void vkr_standard_scene_recording_metrics(VkrStandardSceneRuntime *runtime,
                                          uint64_t frame,
                                          const VkrRecordingMetricsLoop *loop,
                                          bool8_t published) {
  /* The frame rendered before the recording began publishes the update of
     the recording's first frame-loop iteration. */
  const bool8_t loop_only =
      frame == UINT64_MAX && loop && loop->frame != UINT64_MAX;
  if (!runtime->recording.active || (frame == UINT64_MAX && !loop_only)) {
    return;
  }
  VkrMetricsSnapshotView view = {0};
  if (!published || !vkr_metrics_snapshot_acquire(runtime->metrics, &view)) {
    if (!loop_only) {
      vkr_recording_metrics(&runtime->recording, frame, NULL, NULL);
    }
    return;
  }
  vkr_recording_metrics(&runtime->recording, frame, view.frame, loop);
  if (loop_only) {
    vkr_metrics_snapshot_release(runtime->metrics, &view);
    return;
  }
  /* The pass table is single-buffered: it belongs to this publication only
     when both name the same renderer frame. */
  const VkrRendererMetricsPassTable *passes =
      vkr_renderer_metrics_get_pass_table(&runtime->renderer_metrics);
  vkr_recording_passes(
      &runtime->recording, frame, view.frame->cpu_frame_index,
      passes && passes->cpu_frame_index == view.frame->cpu_frame_index ? passes
                                                                       : NULL);
  /* The published gpu.submission measures an earlier frame; the sample
     beside it names which, so the recording files it there. */
  const VkrRendererMetricsSubmission *submission =
      vkr_renderer_metrics_get_submission(&runtime->renderer_metrics);
  if (submission && submission->valid &&
      submission->cpu_frame_index == view.frame->cpu_frame_index) {
    vkr_recording_gpu_submission(&runtime->recording,
                                 submission->source_frame_index,
                                 (float64_t)submission->gpu_ns * 1.0e-6);
  }
  vkr_metrics_snapshot_release(runtime->metrics, &view);
}

void vkr_standard_scene_recording_capture(VkrStandardSceneRuntime *runtime,
                                          const VkrCamera *camera,
                                          float64_t delta) {
  if (!runtime->recording_environment_read) {
    vkr_standard_scene_recording_from_environment(runtime);
  }
  if (!runtime->recording.active) {
    vkr_standard_scene_recording_untrack(runtime);
    return;
  }
  if (!camera) {
    return;
  }
  /* Tier 1 takes per-pass GPU timestamps in the frames the recording
     samples; the metrics config stays the authority the frame reads. */
  bool8_t pass_timed = false_v;
  if (vkr_recording_pass_gpu_frame(&runtime->recording, &pass_timed)) {
    runtime->metrics->config.pass_gpu_timings = pass_timed;
  }
  const VkrStandardSceneRuntimeEditorViewport *viewport =
      &runtime->editor_viewport;
  VkrScene *level = vkr_standard_scene_runtime_level_scene(runtime);
  uint32_t flags = 0u;
  if (viewport->simulation_running) {
    flags |= VKR_RECORDING_FRAME_SIMULATING;
  }
  if (viewport->scripts_own_camera) {
    flags |= VKR_RECORDING_FRAME_SCRIPT_CAMERA;
  }
  if (vkr_standard_scene_runtime_editor_scene_rendering_stopped(runtime)) {
    flags |= VKR_RECORDING_FRAME_RENDERING_STOPPED;
  }
  if (runtime->metrics->config.pass_gpu_timings) {
    flags |= VKR_RECORDING_FRAME_PASS_TIMING;
  }
  const VkrRecordingFrame frame = {
      .display_dt = delta,
      .simulation_time = viewport->simulation_time,
      .time_of_day = level ? (float32_t)level->clock.hour : 12.0f,
      .flags = flags,
  };
  const Vec3 origin = vkr_standard_scene_recording_origin(runtime);
  const VkrRecordingCamera record = {
      .position = {(float64_t)camera->position.x + (float64_t)origin.x,
                   (float64_t)camera->position.y + (float64_t)origin.y,
                   (float64_t)camera->position.z + (float64_t)origin.z},
      .yaw = camera->yaw,
      .pitch = camera->pitch,
      .fov = camera->zoom,
      .near_clip = camera->near_clip,
      .far_clip = camera->far_clip,
      .projection = (uint32_t)camera->type,
  };
  vkr_recording_frame(&runtime->recording, &frame, &record);
  if (runtime->recording.poses_enabled) {
    vkr_standard_scene_recording_poses(runtime);
  }
}
