#include "vkr_sample_recording.h"

#include "application/vkr_standard_scene_runtime.h"

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_content_codec.h"
#include "core/vkr_json_writer.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_physics.h"

#define VKR_SAMPLE_RECORDING_IDENTITY_MAX 2048u
/* Tier 1 on the tiled pipeline takes per-pass GPU stamps in one frame of
   this many. */
#define VKR_SAMPLE_RECORDING_TILED_PASS_PERIOD 8u

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
      .metrics =
          request->tier != VKR_RECORDING_TIER_NONE || request->all_measurements
              ? runtime->metrics
              : NULL,
      .poses = true_v,
      .passes =
          request->tier == VKR_RECORDING_TIER_1 || request->all_measurements,
      .all_measurements = request->all_measurements,
      .zones = request->zones,
      /* Per-pass stamps split the tiled pipeline's shared compute encoder
         (about 0.5 ms per Bistro frame), so Tier 1 samples them there;
         desktop stamps are cheap and run every frame (D2). All
         measurements take them every frame on both. */
      .pass_gpu_period = request->all_measurements ||
                                 runtime->metrics->config.pass_gpu_timings ||
                                 runtime->renderer.graphics_pipeline !=
                                     VKR_GRAPHICS_PIPELINE_TILED
                             ? 1u
                             : VKR_SAMPLE_RECORDING_TILED_PASS_PERIOD,
  };
  if (!vkr_recording_start(&runtime->recording, &config)) {
    char message[VKR_RECORDING_PATH_MAX + 64u];
    string_format(message, sizeof(message),
                  "Could not create the recording '%s'.", request->path);
    vkr_sample_recording_result(state, request->token, false_v, message);
    return;
  }
  /* Tier 0 carries the frame's GPU time: whole-frame timing from command
     completion. The recording sets per-pass stamps frame by frame
     (vkr_recording_pass_gpu_frame); both return at stop. */
  if (config.metrics) {
    state->restore_gpu_timing = true_v;
    state->submission_timing = runtime->metrics->config.submission_gpu_timings;
    state->pass_timing = runtime->metrics->config.pass_gpu_timings;
    state->event_subjects = runtime->metrics->config.event_subjects;
    runtime->metrics->config.submission_gpu_timings = true_v;
    /* All measurements also name each event's subject, such as the asset
       a load read. */
    if (request->all_measurements) {
      runtime->metrics->config.event_subjects = true_v;
    }
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
  if (state->restore_gpu_timing) {
    runtime->metrics->config.submission_gpu_timings = state->submission_timing;
    runtime->metrics->config.pass_gpu_timings = state->pass_timing;
    runtime->metrics->config.event_subjects = state->event_subjects;
    state->restore_gpu_timing = false_v;
  }
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
  vkr_standard_scene_recording_close_world(runtime);
  vkr_recording_playback_close(&runtime->playback);
  char error[192];
  if (!vkr_recording_playback_open(&runtime->playback, request->path,
                                   vkr_content_codec_zstd_decoder(), error,
                                   sizeof(error))) {
    vkr_sample_recording_result(state, request->token, false_v, error);
    return;
  }
  ++runtime->playback_serial;
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
  vkr_standard_scene_recording_close_world(runtime);
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
    vkr_recording_playback_set_loop_range(playback, request->loop_start,
                                          request->loop_end);
    break;
  default:
    break;
  }
  vkr_sample_recording_result(state, request->token, true_v, "");
}

vkr_internal void
vkr_sample_recording_queue(VkrSampleRecordingState *state,
                           VkrSampleRecordingTransport transport) {
  if (state->transport_count < ArrayCount(state->transport)) {
    state->transport[state->transport_count++] = transport;
  }
}

/* Copies what the open drive measured into the status. */
vkr_internal void
vkr_sample_recording_drive_result(const VkrRecordingDrive *drive,
                                  VkrSampleRecordingDriveResult *out) {
  const uint64_t total = drive->last_tick - drive->first_tick + 1u;
  const uint64_t done = Min(drive->ticks_driven, total);
  string_format(out->path, sizeof(out->path), "%s", drive->bundle_path);
  out->valid = true_v;
  out->ticks = done;
  out->ticks_total = total;
  out->ticks_compared = drive->ticks_compared;
  out->distance = drive->distance;
  out->max_distance = drive->max_distance;
  out->path_length = drive->path_length;
  out->divergent_tick = drive->divergent_tick;
  out->divergent_frame = drive->divergent_frame;
  out->resim = drive->resim;
  out->hash_diverged = drive->hash_diverged;
  out->ticks_hashed = drive->ticks_hashed;
  out->hash_tick = drive->hash_divergent_tick;
  out->hash_frame = drive->hash_divergent_frame;
  string_format(out->subsystems, sizeof(out->subsystems), "%s",
                drive->hash_subsystems);
}

/* Names the simulated entities, bodies and characters, whose stable key
   falls in a transform bucket that differed, as candidates for what moved. */
vkr_internal void
vkr_sample_recording_resim_entities(VkrStandardSceneRuntime *runtime,
                                    const VkrRecordingDrive *drive, char *out,
                                    uint64_t capacity) {
  out[0] = '\0';
  VkrScene *scene = runtime->active_scene;
  if (!scene || !drive->hash_buckets) {
    return;
  }
  VkrEntityId candidates[64];
  uint32_t count = 0u;
  const uint32_t bodies = vkr_scene_physics_body_count(scene);
  for (uint32_t i = 0u; i < bodies && count < ArrayCount(candidates); ++i) {
    candidates[count++] = vkr_scene_physics_body_at(scene, i);
  }
  VkrPhysicsCharacterState states[ArrayCount(candidates)];
  count += vkr_scene_characters(scene, candidates + count, states,
                                ArrayCount(candidates) - count);
  uint64_t length = 0u;
  uint32_t named = 0u;
  for (uint32_t i = 0u; i < count && named < 6u; ++i) {
    const uint32_t bucket =
        (uint32_t)(vkr_scene_entity_stable_key(scene, candidates[i]) %
                   VKR_RECORDING_HASH_BUCKETS);
    if (!(drive->hash_buckets & (1u << bucket))) {
      continue;
    }
    String8 name = vkr_scene_get_name(scene, candidates[i]);
    const int32_t written = string_format(
        out + length, capacity - length, "%s%.*s", named ? ", " : "",
        (int32_t)Min(name.length, (uint64_t)48u), (const char *)name.str);
    if (written < 0 || (uint64_t)written >= capacity - length) {
      break;
    }
    length += (uint64_t)written;
    ++named;
  }
}

/* Ends the open drive with its measurement as the request's outcome. */
vkr_internal void
vkr_sample_recording_drive_end(VkrStandardSceneRuntime *runtime,
                               VkrSampleRecordingState *state, uint64_t token,
                               const char *why) {
  VkrSampleRecordingDriveResult *result = &state->status.drive;
  vkr_sample_recording_drive_result(&runtime->drive, result);
  char message[256];
  if (result->resim) {
    vkr_sample_recording_resim_entities(
        runtime, &runtime->drive, result->entities, sizeof(result->entities));
  }
  if (result->resim && result->hash_diverged) {
    string_format(message, sizeof(message),
                  "Re-simulate %s at recorded frame %llu, tick %llu: %s "
                  "differ%s%s.",
                  why, (unsigned long long)result->hash_frame,
                  (unsigned long long)result->hash_tick, result->subsystems,
                  result->entities[0] ? "; near " : "", result->entities);
  } else if (result->resim) {
    string_format(message, sizeof(message),
                  "Re-simulate %s: %llu ticks matched the recording.", why,
                  (unsigned long long)result->ticks_hashed);
  } else if (result->divergent_tick == UINT64_MAX) {
    string_format(message, sizeof(message),
                  "Drive %s: %llu ticks compared, the path stayed within "
                  "%.2f m (max %.3f m).",
                  why, (unsigned long long)result->ticks_compared,
                  VKR_RECORDING_DRIVE_TOLERANCE, result->max_distance);
  } else {
    string_format(message, sizeof(message),
                  "Drive %s: the path left %.2f m at recorded frame %llu "
                  "(tick %llu); max %.2f m over %llu ticks.",
                  why, VKR_RECORDING_DRIVE_TOLERANCE,
                  (unsigned long long)result->divergent_frame,
                  (unsigned long long)result->divergent_tick,
                  result->max_distance,
                  (unsigned long long)result->ticks_compared);
  }
  log_info("%s", message);
  vkr_sample_recording_result(state, token, true_v, message);
  vkr_recording_drive_close(&runtime->drive);
  state->drive_started = false_v;
}

vkr_internal void
vkr_sample_recording_drive(VkrStandardSceneRuntime *runtime,
                           VkrSampleRecordingState *state,
                           const VkrSampleRecordingRequest *request) {
  if (runtime->drive.open) {
    vkr_sample_recording_result(state, request->token, false_v,
                                "A drive is already running.");
    return;
  }
  char error[192];
  if (!vkr_recording_drive_open(&runtime->drive, request->path,
                                vkr_content_codec_zstd_decoder(), error,
                                sizeof(error))) {
    vkr_sample_recording_result(state, request->token, false_v, error);
    return;
  }
  runtime->drive.follow = request->follow;
  const bool8_t no_actions = runtime->drive.action_count == 0u;
  if (!request->resim && (no_actions || runtime->drive.player_count == 0u)) {
    vkr_recording_drive_close(&runtime->drive);
    vkr_sample_recording_result(
        state, request->token, false_v,
        no_actions ? "The recording has no actions to drive with; Re-simulate "
                     "replays a session without input."
                   : "The recording has no player track to drive with.");
    return;
  }
  if (request->resim &&
      (!runtime->drive.has_session || runtime->drive.hash_count == 0u)) {
    vkr_recording_drive_close(&runtime->drive);
    vkr_sample_recording_result(
        state, request->token, false_v,
        "Re-simulate needs a recording that began before Play started, so "
        "it holds the session's seed, clock and tick hashes.");
    return;
  }
  /* Sim frames of another layout are skipped when the drive loads. */
  if (request->resim && runtime->drive.sim_frame_count == 0u) {
    vkr_recording_drive_close(&runtime->drive);
    vkr_sample_recording_result(
        state, request->token, false_v,
        "Re-simulate cannot replay this recording's session clock: an "
        "older build wrote it. Record the session again.");
    return;
  }
  runtime->drive.resim = request->resim;
  /* Drive plays the session, which Playback keeps stopped. */
  if (runtime->playback.open) {
    vkr_sample_recording_close(runtime, state, 0u);
  }
  /* The request answers now; the measurement ends in the status. */
  state->drive_token = 0u;
  state->drive_started = false_v;
  MemZero(&state->status.drive, sizeof(state->status.drive));
  vkr_sample_recording_queue(state, VKR_SAMPLE_RECORDING_TRANSPORT_RESET);
  vkr_sample_recording_queue(state, VKR_SAMPLE_RECORDING_TRANSPORT_PLAY);
  char message[VKR_RECORDING_PATH_MAX + 96u];
  string_format(message, sizeof(message),
                "%s %s: %llu actions over %llu ticks.",
                request->resim ? "Re-simulating" : "Driving", request->path,
                (unsigned long long)runtime->drive.action_count,
                (unsigned long long)(runtime->drive.last_tick -
                                     runtime->drive.first_tick + 1u));
  log_info("%s", message);
  vkr_sample_recording_result(state, request->token, true_v, message);
}

bool8_t vkr_sample_recording_take_transport(VkrSampleRecordingState *state,
                                            VkrSampleRecordingTransport *out) {
  if (state->transport_count == 0u) {
    return false_v;
  }
  *out = state->transport[0];
  state->transport_count--;
  MemCopy(state->transport, state->transport + 1u,
          sizeof(state->transport[0]) * state->transport_count);
  return true_v;
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
  case VKR_SAMPLE_RECORDING_DRIVE:
    vkr_sample_recording_drive(runtime, state, request);
    return;
  case VKR_SAMPLE_RECORDING_DRIVE_STOP:
    if (!runtime->drive.open) {
      vkr_sample_recording_result(state, request->token, false_v,
                                  "No drive is running.");
      return;
    }
    vkr_sample_recording_queue(state, VKR_SAMPLE_RECORDING_TRANSPORT_PAUSE);
    vkr_sample_recording_drive_end(runtime, state, request->token, "stopped");
    return;
  }
}

void vkr_sample_recording_publish(VkrStandardSceneRuntime *runtime,
                                  VkrSampleRecordingState *state,
                                  uint64_t edit_revision,
                                  uint64_t edit_sequence) {
  VkrSampleRecordingStatus *status = &state->status;
  if (!runtime->recording.active) {
    state->edit_start_written = false_v;
  } else if (!state->edit_start_written) {
    state->edit_start_written = true_v;
    vkr_recording_edit(&runtime->recording, edit_sequence);
  } else if (edit_revision != state->edit_revision) {
    (void)vkr_recording_mark(&runtime->recording, VKR_RECORDING_MARKER_EDIT,
                             "scene edited");
    vkr_recording_edit(&runtime->recording, edit_sequence);
  }
  state->edit_revision = edit_revision;
  status->recording = runtime->recording.active;
  status->recording_all_measurements =
      runtime->recording.active && runtime->recording.measurement_count > 0u;
  if (status->recording) {
    string_format(status->recording_path, sizeof(status->recording_path), "%s",
                  runtime->recording.bundle_path);
    status->frames = runtime->recording.frame;
    status->seconds = runtime->recording.time;
    status->markers = runtime->recording.marker_count;
    vkr_recording_stats(&runtime->recording, &status->stats);
  }
  status->playback = runtime->playback.open ? &runtime->playback : NULL;
  /* A drive ends when its recorded ticks run out or the session stops. */
  if (runtime->drive.open) {
    const bool8_t simulating = runtime->editor_viewport.simulation_running;
    state->drive_started = state->drive_started || simulating;
    vkr_sample_recording_drive_result(&runtime->drive, &status->drive);
    if (runtime->drive.hash_diverged) {
      vkr_sample_recording_queue(state, VKR_SAMPLE_RECORDING_TRANSPORT_PAUSE);
      vkr_sample_recording_drive_end(runtime, state, state->drive_token,
                                     "stopped");
    } else if (runtime->drive.finished) {
      vkr_sample_recording_queue(state, VKR_SAMPLE_RECORDING_TRANSPORT_PAUSE);
      vkr_sample_recording_drive_end(runtime, state, state->drive_token,
                                     "finished");
    } else if (state->drive_started && !simulating) {
      vkr_sample_recording_drive_end(runtime, state, state->drive_token,
                                     "ended when the session stopped");
    }
  }
  status->driving = runtime->drive.open;
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
