#include "application/vkr_standard_scene_runtime.h"

#include "containers/str.h"
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

bool8_t
vkr_standard_scene_recording_drive_camera(VkrStandardSceneRuntime *runtime,
                                          VkrCamera *camera, float64_t delta) {
  VkrRecordingPlayback *playback = &runtime->playback;
  if (!playback->open || !camera) {
    return false_v;
  }
  vkr_recording_playback_advance(playback, delta);
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
  vkr_metrics_snapshot_release(runtime->metrics, &view);
}

void vkr_standard_scene_recording_capture(VkrStandardSceneRuntime *runtime,
                                          const VkrCamera *camera,
                                          float64_t delta) {
  if (!runtime->recording_environment_read) {
    vkr_standard_scene_recording_from_environment(runtime);
  }
  if (!runtime->recording.active || !camera) {
    return;
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
}
