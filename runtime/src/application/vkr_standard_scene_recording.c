#include "application/vkr_standard_scene_runtime.h"

#include "containers/str.h"
#include "containers/vkr_hashtable.h"
#include "core/logger.h"
#include "platform/vkr_platform.h"

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

/* Writes one changed entity's world pose; skips one it cannot name. */
vkr_internal void
vkr_standard_scene_recording_pose(VkrStandardSceneRuntime *runtime,
                                  VkrScene *scene, uint8_t container,
                                  VkrEntityId entity) {
  const SceneTransform *transform = vkr_scene_get_transform(scene, entity);
  VkrRecordingPose pose = {.container = container};
  if (!transform || !vkr_standard_scene_recording_identity(
                        scene, entity, &pose.ref, &pose.node, &pose.document)) {
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
    uint8_t container, const VkrEntityRef *ref, uint32_t node,
    uint32_t document) {
  uint8_t bytes[sizeof(VkrEntityRef) + 9u];
  bytes[0] = container;
  MemCopy(bytes + 1u, ref, sizeof(*ref));
  MemCopy(bytes + 1u + sizeof(*ref), &node, sizeof(node));
  MemCopy(bytes + 5u + sizeof(*ref), &document, sizeof(document));
  return vkr_hash_table_key_string8(
             string8_create_from_cstr(bytes, sizeof(bytes)))
      .hash;
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
  if (!playback->pose_entities || !playback->pose_applied || !slots) {
    return false_v;
  }
  /* Slots hold key + 1; zero is empty. */
  MemZero(slots, sizeof(uint32_t) * slot_count);
  const uint32_t mask = slot_count - 1u;
  for (uint32_t k = 0u; k < keys; ++k) {
    const VkrRecordingPoseKey *key = &playback->pose_keys[k];
    uint32_t probe = (uint32_t)vkr_standard_scene_recording_key_hash(
                         key->container, &key->ref, key->node, key->document) &
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
                           (uint8_t)c, &ref, node, document) &
                       mask;
      while (slots[probe] != 0u) {
        const uint32_t k = slots[probe] - 1u;
        const VkrRecordingPoseKey *key = &playback->pose_keys[k];
        if (key->container == c && key->node == node &&
            key->document == document &&
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
        scene && key->count == 1u ? vkr_scene_get_transform(scene, entity)
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
    runtime->playback_pose_generation = runtime->scene_generation;
    if (!vkr_standard_scene_recording_bind_poses(runtime)) {
      return;
    }
  }
  const uint64_t frame =
      vkr_recording_playback_frame_at(playback, playback->time);
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
                                          uint64_t frame, bool8_t published) {
  if (!runtime->recording.active || frame == UINT64_MAX) {
    return;
  }
  VkrMetricsSnapshotView view = {0};
  if (!published || !vkr_metrics_snapshot_acquire(runtime->metrics, &view)) {
    vkr_recording_metrics(&runtime->recording, frame, NULL);
    return;
  }
  vkr_recording_metrics(&runtime->recording, frame, view.frame);
  /* The pass table is single-buffered: it belongs to this publication only
     when both name the same renderer frame. */
  const VkrRendererMetricsPassTable *passes =
      vkr_renderer_metrics_get_pass_table(&runtime->renderer_metrics);
  vkr_recording_passes(
      &runtime->recording, frame, view.frame->cpu_frame_index,
      passes && passes->cpu_frame_index == view.frame->cpu_frame_index ? passes
                                                                       : NULL);
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
