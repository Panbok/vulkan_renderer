#include "vkr_sample_recording.h"

#include "application/vkr_standard_scene_runtime.h"

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_content_codec.h"
#include "core/vkr_json_writer.h"
#include "platform/vkr_platform.h"

#define VKR_SAMPLE_RECORDING_IDENTITY_MAX 2048u

typedef struct VkrSampleRecordingText {
  char *data;
  uint64_t capacity;
  uint64_t length;
} VkrSampleRecordingText;

vkr_internal bool8_t vkr_sample_recording_text_sink(void *context,
                                                    const uint8_t *data,
                                                    uint64_t length) {
  VkrSampleRecordingText *text = context;
  if (text->length + length >= text->capacity) {
    return false_v;
  }
  MemCopy(text->data + text->length, data, length);
  text->length += length;
  text->data[text->length] = '\0';
  return true_v;
}

vkr_internal void vkr_sample_recording_field(VkrJsonWriter *json,
                                             const char *name, String8 value) {
  vkr_json_writer_name(json, string8_create_from_cstr((const uint8_t *)name,
                                                      string_length(name)));
  vkr_json_writer_string(json, value);
}

/* The IDENTITY chunk: what playback compares before it starts. */
vkr_internal bool8_t vkr_sample_recording_identity(
    VkrStandardSceneRuntime *runtime, String8 scene_path, char *out,
    uint64_t capacity) {
  VkrSampleRecordingText text = {.data = out, .capacity = capacity};
  VkrJsonWriter json;
  vkr_json_writer_init(&json, vkr_sample_recording_text_sink, &text);
  vkr_json_writer_begin_object(&json);
  vkr_sample_recording_field(&json, "scene", scene_path);
  vkr_sample_recording_field(&json, "pipeline",
                             runtime->renderer.graphics_pipeline ==
                                     VKR_GRAPHICS_PIPELINE_TILED
                                 ? string8_lit("tiled")
                                 : string8_lit("desktop"));
#if defined(PLATFORM_APPLE)
  vkr_sample_recording_field(&json, "os", string8_lit("macos"));
#elif defined(PLATFORM_WINDOWS)
  vkr_sample_recording_field(&json, "os", string8_lit("windows"));
#endif
  vkr_json_writer_name(&json, string8_lit("output_width"));
  vkr_json_writer_u64(&json, runtime->editor_viewport.output_width);
  vkr_json_writer_name(&json, string8_lit("output_height"));
  vkr_json_writer_u64(&json, runtime->editor_viewport.output_height);
  vkr_json_writer_name(&json, string8_lit("started_unix"));
  vkr_json_writer_i64(&json, vkr_platform_get_unix_time());
  vkr_json_writer_end_object(&json);
  return vkr_json_writer_complete(&json);
}

vkr_internal void vkr_sample_recording_result(VkrSampleRecordingState *state,
                                              uint64_t token, bool8_t ok,
                                              const char *message) {
  state->status.token = token;
  state->status.ok = ok;
  string_format(state->status.message, sizeof(state->status.message), "%s",
                message);
}

vkr_internal void vkr_sample_recording_start(
    VkrStandardSceneRuntime *runtime, VkrSampleRecordingState *state,
    const VkrSampleRecordingRequest *request, String8 scene_path) {
  if (runtime->recording.active || runtime->playback.open) {
    vkr_sample_recording_result(state, request->token, false_v,
                                runtime->recording.active
                                    ? "A recording is already in progress."
                                    : "Close the playback before recording.");
    return;
  }
  char identity[VKR_SAMPLE_RECORDING_IDENTITY_MAX];
  char scene[VKR_RECORDING_PATH_MAX];
  string_format(scene, sizeof(scene), "%.*s", (int)scene_path.length,
                (const char *)scene_path.str);
  if (!vkr_sample_recording_identity(runtime, scene_path, identity,
                                     sizeof(identity))) {
    identity[0] = '\0';
  }
  const VkrRecordingConfig config = {
      .bundle_path = request->path,
      .codec = request->codec,
      .name = request->name,
      .scene = scene,
      .author = request->author[0] ? request->author : "user",
      .identity_json = identity[0] ? identity : NULL,
  };
  if (!vkr_recording_start(&runtime->recording, &config)) {
    char message[VKR_RECORDING_PATH_MAX + 64u];
    string_format(message, sizeof(message),
                  "Could not create the recording '%s'.", request->path);
    vkr_sample_recording_result(state, request->token, false_v, message);
    return;
  }
  log_info("Recording to %s", request->path);
  vkr_sample_recording_result(state, request->token, true_v, "Recording.");
}

vkr_internal void
vkr_sample_recording_stop(VkrStandardSceneRuntime *runtime,
                          VkrSampleRecordingState *state,
                          const VkrSampleRecordingRequest *request) {
  if (!runtime->recording.active) {
    vkr_sample_recording_result(state, request->token, false_v,
                                "No recording is in progress.");
    return;
  }
  const uint64_t frames = runtime->recording.frame;
  const float64_t seconds = runtime->recording.time;
  char path[VKR_RECORDING_PATH_MAX];
  string_format(path, sizeof(path), "%s", runtime->recording.bundle_path);
  VkrRecorderStats stats;
  const bool8_t ok = vkr_recording_stop(&runtime->recording, &stats);
  char message[VKR_RECORDING_PATH_MAX + 96u];
  if (ok) {
    string_format(message, sizeof(message), "Saved %llu frames (%.1f s) to %s.",
                  (unsigned long long)frames, seconds, path);
  } else {
    string_format(message, sizeof(message),
                  "The recording %s did not save cleanly.", path);
  }
  log_info("%s", message);
  vkr_sample_recording_result(state, request->token, ok, message);
}

/* Entering Playback saves the editor camera and hour; leaving restores them. */
vkr_internal void
vkr_sample_recording_open(VkrStandardSceneRuntime *runtime,
                          VkrSampleRecordingState *state,
                          const VkrSampleRecordingRequest *request) {
  if (runtime->recording.active ||
      runtime->editor_viewport.simulation_running) {
    vkr_sample_recording_result(
        state, request->token, false_v,
        runtime->recording.active
            ? "Stop the recording before opening a playback."
            : "Stop the simulation before opening a playback.");
    return;
  }
  vkr_recording_playback_close(&runtime->playback);
  char error[192];
  if (!vkr_recording_playback_open(&runtime->playback, request->path,
                                   vkr_content_codec_zstd_decoder(), error,
                                   sizeof(error))) {
    vkr_sample_recording_result(state, request->token, false_v, error);
    return;
  }
  const VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &runtime->camera_system, runtime->active_camera);
  VkrScene *level = vkr_standard_scene_runtime_level_scene(runtime);
  if (!state->saved && camera) {
    state->saved = true_v;
    state->position = camera->position;
    state->yaw = camera->yaw;
    state->pitch = camera->pitch;
    state->zoom = camera->zoom;
    state->near_clip = camera->near_clip;
    state->far_clip = camera->far_clip;
    state->hour = level ? level->clock.hour : 12.0;
  }
  char message[256];
  string_format(message, sizeof(message), "Opened %llu frames (%.1f s)%s.",
                (unsigned long long)runtime->playback.frame_count,
                runtime->playback.duration,
                runtime->playback.truncated ? ", cut off at its end" : "");
  vkr_sample_recording_result(state, request->token, true_v, message);
}

vkr_internal void vkr_sample_recording_close(VkrStandardSceneRuntime *runtime,
                                             VkrSampleRecordingState *state,
                                             uint64_t token) {
  vkr_recording_playback_close(&runtime->playback);
  VkrCamera *camera = vkr_camera_registry_get_by_handle(&runtime->camera_system,
                                                        runtime->active_camera);
  VkrScene *level = vkr_standard_scene_runtime_level_scene(runtime);
  if (state->saved && camera) {
    vkr_camera_set_pose(camera, state->position, state->yaw, state->pitch);
    camera->zoom = state->zoom;
    camera->near_clip = state->near_clip;
    camera->far_clip = state->far_clip;
    camera->projection_dirty = true_v;
  }
  if (state->saved && level) {
    (void)vkr_scene_set_time_of_day_hour(level, state->hour);
  }
  state->saved = false_v;
  vkr_sample_recording_result(state, token, true_v, "Playback closed.");
}

vkr_internal void
vkr_sample_recording_transport(VkrStandardSceneRuntime *runtime,
                               VkrSampleRecordingState *state,
                               const VkrSampleRecordingRequest *request) {
  VkrRecordingPlayback *playback = &runtime->playback;
  if (!playback->open) {
    vkr_sample_recording_result(state, request->token, false_v,
                                "No playback is open.");
    return;
  }
  switch (request->command) {
  case VKR_SAMPLE_RECORDING_PLAY:
    if (playback->time >= playback->duration && !playback->loop) {
      playback->time = 0.0;
    }
    playback->playing = true_v;
    break;
  case VKR_SAMPLE_RECORDING_PAUSE:
    playback->playing = false_v;
    break;
  case VKR_SAMPLE_RECORDING_SEEK_TIME:
    vkr_recording_playback_seek_time(playback, request->time);
    break;
  case VKR_SAMPLE_RECORDING_SEEK_FRAME:
    vkr_recording_playback_seek_frame(playback, request->frame);
    break;
  case VKR_SAMPLE_RECORDING_SETTINGS:
    playback->speed = Min(Max(request->speed, 0.1f), 4.0f);
    playback->loop = request->loop;
    break;
  default:
    break;
  }
  vkr_sample_recording_result(state, request->token, true_v, "");
}

void vkr_sample_recording_apply(VkrStandardSceneRuntime *runtime,
                                VkrSampleRecordingState *state,
                                const VkrSampleRecordingRequest *request,
                                String8 scene_path) {
  vkr_assert(runtime != NULL && state != NULL && request != NULL);
  switch (request->command) {
  case VKR_SAMPLE_RECORDING_NONE:
    return;
  case VKR_SAMPLE_RECORDING_START:
    vkr_sample_recording_start(runtime, state, request, scene_path);
    return;
  case VKR_SAMPLE_RECORDING_STOP:
    vkr_sample_recording_stop(runtime, state, request);
    return;
  case VKR_SAMPLE_RECORDING_MARK:
    if (!runtime->recording.active) {
      vkr_sample_recording_result(state, request->token, false_v,
                                  "No recording is in progress.");
      return;
    }
    vkr_sample_recording_result(
        state, request->token,
        vkr_recording_mark(&runtime->recording,
                           (VkrRecordingMarkerKind)request->marker_kind,
                           request->label),
        "Marked.");
    return;
  case VKR_SAMPLE_RECORDING_OPEN:
    vkr_sample_recording_open(runtime, state, request);
    return;
  case VKR_SAMPLE_RECORDING_CLOSE:
    vkr_sample_recording_close(runtime, state, request->token);
    return;
  case VKR_SAMPLE_RECORDING_PLAY:
  case VKR_SAMPLE_RECORDING_PAUSE:
  case VKR_SAMPLE_RECORDING_SEEK_TIME:
  case VKR_SAMPLE_RECORDING_SEEK_FRAME:
  case VKR_SAMPLE_RECORDING_SETTINGS:
    vkr_sample_recording_transport(runtime, state, request);
    return;
  }
}

void vkr_sample_recording_publish(VkrStandardSceneRuntime *runtime,
                                  VkrSampleRecordingState *state) {
  VkrSampleRecordingStatus *status = &state->status;
  status->recording = runtime->recording.active;
  if (status->recording) {
    string_format(status->recording_path, sizeof(status->recording_path), "%s",
                  runtime->recording.bundle_path);
    status->frames = runtime->recording.frame;
    status->seconds = runtime->recording.time;
    status->markers = runtime->recording.marker_count;
    vkr_recording_stats(&runtime->recording, &status->stats);
  }
  status->playback = runtime->playback.open ? &runtime->playback : NULL;
  const VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &runtime->camera_system, runtime->active_camera);
  VkrScene *level = vkr_standard_scene_runtime_level_scene(runtime);
  const Vec3 origin = level ? level->origin_offset : vec3_zero();
  status->camera_valid = camera != NULL;
  if (camera) {
    status->camera_position[0] =
        (float64_t)camera->position.x + (float64_t)origin.x;
    status->camera_position[1] =
        (float64_t)camera->position.y + (float64_t)origin.y;
    status->camera_position[2] =
        (float64_t)camera->position.z + (float64_t)origin.z;
    status->camera_yaw = camera->yaw;
    status->camera_pitch = camera->pitch;
  }
}
