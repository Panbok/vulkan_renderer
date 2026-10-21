#include "application/vkr_standard_scene_runtime.h"

#include "containers/str.h"
#include "containers/vkr_hashtable.h"
#include "core/logger.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_animation.h"
#include "renderer/systems/vkr_scene_model.h"
#include "renderer/systems/vkr_scene_partition.h"

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
  /* Its own visibility: a hidden parent hides it without a change here. */
  const SceneVisibility *visibility = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_visibility);
  if (visibility && !visibility->visible) {
    pose.flags |= VKR_RECORDING_POSE_FLAG_HIDDEN;
  }
  /* Relative to its parent, unless Watch's hierarchy may differ from this
     one: a spawn's stand-in is a root, and a reparented entity's parent is
     no longer the one the document gave it. */
  const SceneTransform *parent =
      transform->parent.u64 != VKR_ENTITY_ID_INVALID.u64 &&
              !(transform->flags & SCENE_TRANSFORM_REPARENTED) &&
              !(pose.kind == VKR_RECORDING_POSE_KIND_SPAWN &&
                pose.node == VKR_RECORDING_POSE_NODE_NONE)
          ? vkr_scene_get_transform(scene, transform->parent)
          : NULL;
  Mat4 matrix = transform->world;
  Vec3 offset = scene->origin_offset;
  if (parent) {
    /* Its local matrix when nothing overrides its world, so Watch finds it
       unchanged and leaves it to the scene. */
    const Mat4 carried = mat4_mul(parent->world, transform->local);
    matrix = MemCompare(&carried, &transform->world, sizeof(carried)) == 0
                 ? transform->local
                 : mat4_mul(mat4_inverse(parent->world), transform->world);
    offset = vec3_zero();
    pose.flags |= VKR_RECORDING_POSE_FLAG_PARENTED;
  }
  pose.translation[0] = (float64_t)matrix.m03 + (float64_t)offset.x;
  pose.translation[1] = (float64_t)matrix.m13 + (float64_t)offset.y;
  pose.translation[2] = (float64_t)matrix.m23 + (float64_t)offset.z;
  for (uint32_t column = 0u; column < 3u; ++column) {
    for (uint32_t row = 0u; row < 3u; ++row) {
      pose.linear[column * 3u + row] = matrix.elements[column * 4u + row];
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

/* A recorded pose's matrix: its world matrix in `scene`'s space, or, for
   a parented pose, its matrix in its parent's space. */
vkr_internal Mat4 vkr_standard_scene_recording_pose_matrix(
    const VkrScene *scene, const VkrRecordingPoseSample *sample) {
  Mat4 matrix = mat4_identity();
  for (uint32_t column = 0u; column < 3u; ++column) {
    for (uint32_t row = 0u; row < 3u; ++row) {
      matrix.elements[column * 4u + row] = sample->linear[column * 3u + row];
    }
  }
  const bool8_t parented =
      (sample->flags & VKR_RECORDING_POSE_FLAG_PARENTED) != 0u;
  const Vec3 offset = parented || !scene ? vec3_zero() : scene->origin_offset;
  matrix.m03 = (float32_t)(sample->translation[0] - (float64_t)offset.x);
  matrix.m13 = (float32_t)(sample->translation[1] - (float64_t)offset.y);
  matrix.m23 = (float32_t)(sample->translation[2] - (float64_t)offset.z);
  return matrix;
}

/* The world pose key `k`'s entity has at `frame`: its recorded world pose,
   or its parent's world pose times its recorded pose relative to it, or,
   before its first pose, its live local one. Computed once per frame. */
vkr_internal Mat4 vkr_standard_scene_recording_key_world(
    VkrStandardSceneRuntime *runtime, uint32_t k, uint64_t frame) {
  VkrRecordingPlayback *playback = &runtime->playback;
  if (playback->pose_world_frames[k] == frame) {
    return playback->pose_worlds[k];
  }
  VkrScene *scene = vkr_standard_scene_recording_container(
      runtime, playback->pose_keys[k].container);
  const VkrEntityId entity = {.u64 = playback->pose_entities[k]};
  const SceneTransform *transform =
      scene && entity.u64 != VKR_ENTITY_ID_INVALID.u64
          ? vkr_scene_get_transform(scene, entity)
          : NULL;
  const uint32_t sample = vkr_recording_playback_pose_at(playback, k, frame);
  Mat4 world = transform ? transform->world : mat4_identity();
  if (sample != UINT32_MAX && !(playback->pose_samples[sample].flags &
                                VKR_RECORDING_POSE_FLAG_PARENTED)) {
    world = vkr_standard_scene_recording_pose_matrix(
        scene, &playback->pose_samples[sample]);
  } else if (sample != UINT32_MAX || playback->pose_parents[k] != UINT32_MAX) {
    const Mat4 relative =
        sample != UINT32_MAX ? vkr_standard_scene_recording_pose_matrix(
                                   scene, &playback->pose_samples[sample])
                             : (transform ? transform->local : mat4_identity());
    const SceneTransform *parent =
        transform && transform->parent.u64 != VKR_ENTITY_ID_INVALID.u64
            ? vkr_scene_get_transform(scene, transform->parent)
            : NULL;
    if (playback->pose_parents[k] != UINT32_MAX) {
      world = mat4_mul(vkr_standard_scene_recording_key_world(
                           runtime, playback->pose_parents[k], frame),
                       relative);
    } else {
      world = parent ? mat4_mul(parent->world, relative) : relative;
    }
  }
  playback->pose_worlds[k] = world;
  playback->pose_world_frames[k] = frame;
  return world;
}

/* Finds each bound key's parent key through its entity's live parent, and
   forgets the world poses composed so far. */
vkr_internal bool8_t
vkr_standard_scene_recording_bind_parents(VkrStandardSceneRuntime *runtime) {
  VkrRecordingPlayback *playback = &runtime->playback;
  const uint32_t keys = playback->pose_key_count;
  uint32_t slot_count = 16u;
  while (slot_count < keys * 2u) {
    slot_count *= 2u;
  }
  uint32_t *slots =
      vkr_allocator_alloc(&playback->allocator, sizeof(uint32_t) * slot_count,
                          VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  if (!slots) {
    return false_v;
  }
  /* Slots hold key + 1 by entity and container; zero is empty. */
  MemZero(slots, sizeof(uint32_t) * slot_count);
  const uint32_t mask = slot_count - 1u;
  for (uint32_t k = 0u; k < keys; ++k) {
    playback->pose_parents[k] = UINT32_MAX;
    playback->pose_world_frames[k] = UINT64_MAX;
    const uint64_t entity = playback->pose_entities[k];
    if (entity == VKR_ENTITY_ID_INVALID.u64) {
      continue;
    }
    uint32_t probe =
        (uint32_t)vkr_hash_table_finalize(
            entity ^ ((uint64_t)playback->pose_keys[k].container << 56u)) &
        mask;
    while (slots[probe] != 0u) {
      probe = (probe + 1u) & mask;
    }
    slots[probe] = k + 1u;
  }
  for (uint32_t k = 0u; k < keys; ++k) {
    const uint8_t container = playback->pose_keys[k].container;
    VkrScene *scene =
        vkr_standard_scene_recording_container(runtime, container);
    const VkrEntityId entity = {.u64 = playback->pose_entities[k]};
    const SceneTransform *transform =
        scene && entity.u64 != VKR_ENTITY_ID_INVALID.u64
            ? vkr_scene_get_transform(scene, entity)
            : NULL;
    if (!transform || transform->parent.u64 == VKR_ENTITY_ID_INVALID.u64) {
      continue;
    }
    const uint64_t parent = transform->parent.u64;
    uint32_t probe = (uint32_t)vkr_hash_table_finalize(
                         parent ^ ((uint64_t)container << 56u)) &
                     mask;
    while (slots[probe] != 0u) {
      const uint32_t candidate = slots[probe] - 1u;
      if (playback->pose_entities[candidate] == parent &&
          playback->pose_keys[candidate].container == container) {
        playback->pose_parents[k] = candidate;
        break;
      }
      probe = (probe + 1u) & mask;
    }
  }
  vkr_allocator_free(&playback->allocator, slots, sizeof(uint32_t) * slot_count,
                     VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  return true_v;
}

/* The world matrix `blend` of the way from `from` to `to`, the world poses
   an entity had at a frame and at the next, as the playback camera moves
   between frames: the turn between their bases slerped and the origin
   lerped, both poses being rigid motions of one rest pose. */
vkr_internal Mat4 vkr_standard_scene_recording_pose_blend(Mat4 from, Mat4 to,
                                                          float32_t blend) {
  Mat4 from_basis = from;
  Mat4 to_basis = to;
  from_basis.m03 = from_basis.m13 = from_basis.m23 = 0.0f;
  to_basis.m03 = to_basis.m13 = to_basis.m23 = 0.0f;
  Mat4 world = from_basis;
  if (MemCompare(&from_basis, &to_basis, sizeof(from_basis)) != 0) {
    const VkrQuat turn = vkr_quat_normalize(
        mat4_to_quat(mat4_mul(to_basis, mat4_inverse_affine(from_basis))));
    world = mat4_mul(
        vkr_quat_to_mat4(vkr_quat_slerp(vkr_quat_identity(), turn, blend)),
        from_basis);
  }
  world.m03 = from.m03 + (to.m03 - from.m03) * blend;
  world.m13 = from.m13 + (to.m13 - from.m13) * blend;
  world.m23 = from.m23 + (to.m23 - from.m23) * blend;
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
   poses_pending while some wait for a model. Returns how many it bound. */
vkr_internal uint32_t
vkr_standard_scene_recording_bind_spawns(VkrStandardSceneRuntime *runtime) {
  VkrRecordingPlayback *playback = &runtime->playback;
  playback->poses_pending = false_v;
  uint32_t bound = 0u;
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
      ++bound;
      continue;
    }
    const VkrEntityId node = vkr_scene_model_node(scene, proxy, key->node);
    if (node.u64 != VKR_ENTITY_ID_INVALID.u64) {
      playback->pose_entities[k] = node.u64;
      ++bound;
    } else if (vkr_scene_model_status(scene, proxy, NULL) ==
               VKR_SCENE_MODEL_LOADING) {
      playback->poses_pending = true_v;
    }
  }
  return bound;
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
    const Mat4 inverse = mat4_inverse(
        vkr_standard_scene_recording_key_world(runtime, self, frame));
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
      Mat4 global = mat4_mul(
          inverse, vkr_standard_scene_recording_key_world(runtime, k, frame));
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

/* Finds each pose key's live entity with one walk over the containers and
   its parent's key, and marks a key whose only pose matches its entity
   already as static: the same world pose, or the same local pose under its
   parent. */
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
    playback->pose_hidden =
        vkr_allocator_alloc(&playback->allocator, sizeof(uint8_t) * keys,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    playback->pose_parents =
        vkr_allocator_alloc(&playback->allocator, sizeof(uint32_t) * keys,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    playback->pose_worlds =
        vkr_allocator_alloc(&playback->allocator, sizeof(Mat4) * keys,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    playback->pose_world_frames =
        vkr_allocator_alloc(&playback->allocator, sizeof(uint64_t) * keys,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  uint32_t slot_count = 16u;
  while (slot_count < keys * 2u) {
    slot_count *= 2u;
  }
  uint32_t *slots =
      vkr_allocator_alloc(&playback->allocator, sizeof(uint32_t) * slot_count,
                          VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  if (!playback->pose_entities || !playback->pose_applied ||
      !playback->pose_hidden || !playback->pose_parents ||
      !playback->pose_worlds || !playback->pose_world_frames || !slots ||
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
    playback->pose_hidden[k] = 0u;
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
                key->kind == VKR_RECORDING_POSE_KIND_DOCUMENT &&
                !(playback->pose_samples[key->first].flags &
                  VKR_RECORDING_POSE_FLAG_HIDDEN)
            ? vkr_scene_get_transform(scene, entity)
            : NULL;
    if (!transform) {
      continue;
    }
    const VkrRecordingPoseSample *pose = &playback->pose_samples[key->first];
    const Mat4 matrix = vkr_standard_scene_recording_pose_matrix(scene, pose);
    const Mat4 *live = (pose->flags & VKR_RECORDING_POSE_FLAG_PARENTED)
                           ? &transform->local
                           : &transform->world;
    if (MemCompare(&matrix, live, sizeof(matrix)) == 0) {
      playback->pose_applied[k] = VKR_RECORDING_POSE_APPLIED_STATIC;
    }
  }
  vkr_allocator_free(&playback->allocator, slots, sizeof(uint32_t) * slot_count,
                     VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  vkr_standard_scene_recording_bind_spawns(runtime);
  if (!vkr_standard_scene_recording_bind_parents(runtime)) {
    return false_v;
  }
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

/* Poses each bound entity as the recording had it at the playhead: its
   last pose until the playhead's frame as an evaluated override, or its own
   transform before the recording first moved it. An entity that moved into
   the next frame, or an overridden child whose parent may have, is blended
   toward its world pose at that frame, as the camera is. A pose relative to
   its parent that is its own local pose needs no override: the parent
   carries it. */
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
  if (playback->poses_pending &&
      vkr_standard_scene_recording_bind_spawns(runtime) > 0u &&
      !vkr_standard_scene_recording_bind_parents(runtime)) {
    return;
  }
  const VkrRecordingPlaybackPose at = vkr_recording_playback_pose(playback);
  const uint64_t frame = at.frame;
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
    VkrScene *scene = vkr_standard_scene_recording_container(
        runtime, playback->pose_keys[k].container);
    if (!scene) {
      continue;
    }
    const VkrRecordingPoseSample *pose =
        sample != UINT32_MAX ? &playback->pose_samples[sample] : NULL;
    /* The playhead lies between `frame` and the next one. */
    const bool8_t between =
        at.blend > 0.0f && frame + 1u < playback->frame_count;
    /* The next frame's sample, when the entity moved into that frame. */
    const VkrRecordingPoseKey *key = &playback->pose_keys[k];
    const uint32_t next =
        between && sample != UINT32_MAX &&
                sample + 1u < key->first + key->count &&
                playback->pose_samples[sample + 1u].frame == frame + 1u
            ? sample + 1u
            : UINT32_MAX;
    const SceneTransform *transform =
        pose && (pose->flags & VKR_RECORDING_POSE_FLAG_PARENTED)
            ? vkr_scene_get_transform(scene, entity)
            : NULL;
    if (next != UINT32_MAX) {
      /* World poses composed through the parents at both frames. */
      const Mat4 world = vkr_standard_scene_recording_pose_blend(
          vkr_standard_scene_recording_key_world(runtime, k, frame),
          vkr_standard_scene_recording_key_world(runtime, k, frame + 1u),
          at.blend);
      (void)vkr_scene_set_evaluated_transform(scene, entity, &world);
      playback->pose_applied[k] = VKR_RECORDING_POSE_APPLIED_BLENDED;
    } else if (transform) {
      const Mat4 relative =
          vkr_standard_scene_recording_pose_matrix(scene, pose);
      if (MemCompare(&relative, &transform->local, sizeof(relative)) == 0) {
        /* Its parent carries it as the recording had it. */
        if (applied < VKR_RECORDING_POSE_APPLIED_FOLLOWS) {
          (void)vkr_scene_set_evaluated_transform(scene, entity, NULL);
        }
        playback->pose_applied[k] = VKR_RECORDING_POSE_APPLIED_FOLLOWS;
      } else {
        /* Its parent may move under the same relative pose, so it blends
           with its parent. */
        const Mat4 at_frame =
            vkr_standard_scene_recording_key_world(runtime, k, frame);
        Mat4 world = at_frame;
        if (between && playback->pose_parents[k] != UINT32_MAX) {
          world = vkr_standard_scene_recording_pose_blend(
              at_frame,
              vkr_standard_scene_recording_key_world(runtime, k, frame + 1u),
              at.blend);
        }
        if (sample != applied ||
            MemCompare(&world, &transform->world, sizeof(world)) != 0) {
          (void)vkr_scene_set_evaluated_transform(scene, entity, &world);
          playback->pose_applied[k] = sample;
        }
      }
    } else if (sample != applied) {
      if (sample == UINT32_MAX) {
        (void)vkr_scene_set_evaluated_transform(scene, entity, NULL);
      } else {
        const Mat4 world =
            vkr_standard_scene_recording_pose_matrix(scene, pose);
        (void)vkr_scene_set_evaluated_transform(scene, entity, &world);
      }
      playback->pose_applied[k] = sample;
    }
    /* Hidden as the recording had it, such as a first-person player's body
       around the camera. A stand-in its spawn's lifetime showed again is
       hidden again. */
    const uint8_t hidden =
        sample != UINT32_MAX && (playback->pose_samples[sample].flags &
                                 VKR_RECORDING_POSE_FLAG_HIDDEN)
            ? 1u
            : 0u;
    const bool8_t reshown =
        hidden &&
        playback->pose_keys[k].kind == VKR_RECORDING_POSE_KIND_SPAWN &&
        vkr_scene_entity_visible(scene, entity);
    if (hidden != playback->pose_hidden[k] || reshown) {
      vkr_scene_set_visibility(scene, entity, !hidden, true_v);
      playback->pose_hidden[k] = hidden;
    }
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
        applied < VKR_RECORDING_POSE_APPLIED_FOLLOWS) {
      (void)vkr_scene_set_evaluated_transform(scene, entity, NULL);
    }
    if (scene && entity.u64 != VKR_ENTITY_ID_INVALID.u64 &&
        playback->pose_hidden[k]) {
      vkr_scene_set_visibility(scene, entity, true_v, true_v);
    }
    playback->pose_applied[k] = VKR_RECORDING_POSE_APPLIED_NONE;
    playback->pose_hidden[k] = 0u;
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
  /* Clouds and their shadows stand where the session had them; a recording
     without them leaves the clouds drifting. */
  if (pose.has_clouds) {
    runtime->cloud_wind_offset_m[0] = pose.cloud_wind_offset_m[0];
    runtime->cloud_wind_offset_m[1] = pose.cloud_wind_offset_m[1];
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
  /* VKR_RECORDING_ZONES=1 (profile --record-zones) adds every thread's CPU
     zones (ADR-126). */
  char zones[8] = {0};
  const bool8_t record_zones =
      vkr_platform_environment_variable("VKR_RECORDING_ZONES", zones,
                                        sizeof(zones)) &&
      zones[0] == '1';
  const VkrRecordingConfig config = {
      .bundle_path = path,
      .name = "run",
      .author = "environment",
      .metrics = runtime->metrics,
      .zones = record_zones,
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

/* Appends one hardware fact; an empty value says the API reports none. */
vkr_internal void
vkr_standard_scene_recording_fact(VkrRecordingHardwareFact *facts,
                                  uint32_t *count, uint32_t capacity,
                                  const char *key, const char *value) {
  if (*count >= capacity || !value || !value[0]) {
    return;
  }
  VkrRecordingHardwareFact *fact = &facts[(*count)++];
  string_format(fact->key, sizeof(fact->key), "%s", key);
  string_format(fact->value, sizeof(fact->value), "%s", value);
}

/* A byte count as the HW tab shows it: GiB, MiB or KiB. */
vkr_internal void vkr_standard_scene_recording_bytes(char *out,
                                                     uint64_t capacity,
                                                     uint64_t bytes) {
  if (bytes == 0u) {
    out[0] = '\0';
  } else if (bytes >= GB(1)) {
    string_format(out, capacity, "%.2f GiB",
                  (float64_t)bytes / (float64_t)GB(1));
  } else if (bytes >= MB(1)) {
    string_format(out, capacity, "%.1f MiB",
                  (float64_t)bytes / (float64_t)MB(1));
  } else {
    string_format(out, capacity, "%llu KiB",
                  (unsigned long long)(bytes / KB(1)));
  }
}

/* The device the recording runs on, as facts the HW tab lists
   (ADR-130): what the graphics API reports, then what the OS does. */
vkr_internal uint32_t vkr_standard_scene_recording_hardware_facts(
    VkrStandardSceneRuntime *runtime, VkrRecordingHardwareFact *facts,
    uint32_t capacity) {
  VkrDeviceInformation device = {0};
  vkr_renderer_get_device_information(&runtime->renderer, &device,
                                      runtime->frame_arenas[0]);
  uint32_t count = 0u;
  char value[VKR_RECORDING_HARDWARE_VALUE_MAX + 1u];
  string_format(value, sizeof(value), "%.*s",
                (int32_t)device.device_name.length,
                (const char *)device.device_name.str);
  vkr_standard_scene_recording_fact(facts, &count, capacity, "GPU", value);
  /* The HW tab offers GPU counters by maker (ADR-130). */
  string_format(value, sizeof(value), "%.*s",
                (int32_t)device.vendor_name.length,
                (const char *)device.vendor_name.str);
  vkr_standard_scene_recording_fact(facts, &count, capacity, "Vendor", value);
  vkr_standard_scene_recording_fact(facts, &count, capacity, "Architecture",
                                    device.gpu_architecture);
  /* The OS names the GPU's cores; it walks the IORegistry, once per
     recording. */
  VkrPlatformGpuInfo gpu = {0};
  if (vkr_platform_get_gpu_info(&gpu) && gpu.core_count_valid) {
    string_format(value, sizeof(value), "%u", gpu.core_count);
    vkr_standard_scene_recording_fact(facts, &count, capacity, "GPU cores",
                                      value);
  }
  string_format(value, sizeof(value), "%.*s",
                (int32_t)device.api_version.length,
                (const char *)device.api_version.str);
  vkr_standard_scene_recording_fact(facts, &count, capacity, "API", value);
  /* Metal names no driver beside its API. */
  if (!string8_equals(&device.driver_version, &device.api_version)) {
    string_format(value, sizeof(value), "%.*s",
                  (int32_t)device.driver_version.length,
                  (const char *)device.driver_version.str);
    vkr_standard_scene_recording_fact(facts, &count, capacity, "Driver", value);
  }
  vkr_standard_scene_recording_fact(
      facts, &count, capacity, "Pipeline",
      device.graphics_pipeline == VKR_GRAPHICS_PIPELINE_TILED ? "Tiled"
                                                              : "Desktop");
  vkr_standard_scene_recording_fact(facts, &count, capacity, "Unified memory",
                                    device.unified_memory ? "Yes" : "No");
  vkr_standard_scene_recording_bytes(value, sizeof(value),
                                     device.working_set_bytes);
  vkr_standard_scene_recording_fact(facts, &count, capacity,
                                    "Recommended working set", value);
  vkr_standard_scene_recording_bytes(value, sizeof(value),
                                     device.vram_local_size);
  vkr_standard_scene_recording_fact(facts, &count, capacity, "Device memory",
                                    value);
  vkr_standard_scene_recording_bytes(value, sizeof(value),
                                     device.max_threadgroup_memory);
  vkr_standard_scene_recording_fact(facts, &count, capacity,
                                    "Threadgroup memory", value);
  if (device.max_threads_per_threadgroup > 0u) {
    string_format(value, sizeof(value), "%u",
                  device.max_threads_per_threadgroup);
    vkr_standard_scene_recording_fact(facts, &count, capacity,
                                      "Threads per threadgroup", value);
  }
  if (device.simd_width_max > 0u) {
    if (device.simd_width_min == device.simd_width_max) {
      string_format(value, sizeof(value), "%u", device.simd_width_max);
    } else {
      string_format(value, sizeof(value), "%u to %u", device.simd_width_min,
                    device.simd_width_max);
    }
    vkr_standard_scene_recording_fact(facts, &count, capacity, "SIMD width",
                                      value);
  }
  if (device.graphics_pipeline != VKR_GRAPHICS_PIPELINE_TILED) {
    vkr_standard_scene_recording_fact(
        facts, &count, capacity, "Shader statistics",
        device.shader_statistics_captured ? "Captured" : "Off");
  }
  return count;
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
  /* A change of passes asks the render thread for the graph's shape,
     which a later frame hands over. */
  if (vkr_recording_graph_wanted(&runtime->recording)) {
    (void)vkr_renderer_request_graph_snapshot(&runtime->renderer);
  }
  vkr_recording_graph(&runtime->recording, frame,
                      vkr_renderer_take_graph_snapshot(&runtime->renderer));
  vkr_recording_allocators(&runtime->recording, frame);
  /* The device once, then each pipeline the renderer creates. */
  if (runtime->recording.passes_enabled &&
      !runtime->recording.hardware_written) {
    VkrRecordingHardwareFact facts[24];
    const uint32_t fact_count = vkr_standard_scene_recording_hardware_facts(
        runtime, facts, ArrayCount(facts));
    vkr_recording_hardware(&runtime->recording, frame, facts, fact_count);
  }
  vkr_recording_pipelines(&runtime->recording, frame,
                          vkr_renderer_pipeline_registry(&runtime->renderer));
  /* Hardware counters the renderer sampled (ADR-130), once a session
     finished on the GPU. */
  if (vkr_renderer_gpu_counters_supported(&runtime->renderer)) {
    VkrRendererImplGpuCounters *session =
        vkr_recording_gpu_counter_buffer(&runtime->recording);
    if (session &&
        vkr_renderer_take_gpu_counters(&runtime->renderer, session)) {
      vkr_recording_gpu_counters(&runtime->recording, session);
    }
  }
  /* The census, like the pass table, belongs to this publication only when
     both name the same renderer frame. */
  const VkrRendererMetricsDrawCensus *census =
      vkr_renderer_metrics_get_draw_census(&runtime->renderer_metrics);
  if (census && census->cpu_frame_index == view.frame->cpu_frame_index) {
    vkr_recording_draws(&runtime->recording, census);
  }
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

void vkr_standard_scene_recording_draw_bounds(
    VkrStandardSceneRuntime *runtime, uint64_t frame,
    const VkrWorldPassPayload *world) {
  VkrRecording *recording = &runtime->recording;
  if (!recording->active || !world || world->gpu_candidate_count == 0u) {
    return;
  }
  const uint64_t generation = vkr_world_object_bounds_generation(world);
  if (!vkr_recording_draw_bounds_wanted(recording, generation)) {
    return;
  }
  /* Rows for every candidate: the helper also sorts in them. */
  const uint32_t capacity = world->gpu_candidate_count;
  const uint64_t bytes = sizeof(VkrWorldObjectBounds) * (uint64_t)capacity;
  VkrWorldObjectBounds *rows = vkr_allocator_alloc(
      &recording->allocator, bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!rows) {
    return;
  }
  const uint32_t count = vkr_world_object_bounds(world, rows, capacity);
  /* The ground's cells, where the scene streams by world partition. */
  const VkrScene *scene =
      runtime->world_scene ? runtime->world_scene : runtime->active_scene;
  SceneWorldPartition partition = {0};
  const float32_t cell_size =
      scene && vkr_scene_partition_settings(scene, &partition)
          ? partition.cell_size
          : 0.0f;
  vkr_recording_draw_bounds(recording, frame, generation, rows, count,
                            cell_size);
  vkr_allocator_free(&recording->allocator, rows, bytes,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
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
