#include "vkr_harness.h"

/**
 * Every capture channel a case may name. The first block reads a backend
 * resource directly under default renderer state; the rest re-render the scene
 * with the state their `replay_mode` selects, which is why each distinct mode
 * costs one more child process.
 */
static const VkrHarnessCaptureChannelDescription s_channels[] = {
    {"final_color", "final_color", "direct", VKR_RENDER_MODE_DEFAULT, 0u},
    {"scene_color", "scene_color", "direct", VKR_RENDER_MODE_DEFAULT, 0u},
    {"depth", "depth", "direct", VKR_RENDER_MODE_DEFAULT, 0u},
    {"shadow_cascade_0", "shadow_cascade_0", "direct", VKR_RENDER_MODE_DEFAULT,
     0u},
    {"shadow_cascade_1", "shadow_cascade_1", "direct", VKR_RENDER_MODE_DEFAULT,
     0u},
    {"shadow_cascade_2", "shadow_cascade_2", "direct", VKR_RENDER_MODE_DEFAULT,
     0u},
    {"shadow_cascade_3", "shadow_cascade_3", "direct", VKR_RENDER_MODE_DEFAULT,
     0u},
    {"picking_ids", "picking_ids", "direct", VKR_RENDER_MODE_DEFAULT, 0u},
    {"visibility_ids", "visibility_ids", "direct", VKR_RENDER_MODE_DEFAULT, 0u},
    {"visibility_primitives", "visibility_primitives", "direct",
     VKR_RENDER_MODE_DEFAULT, 0u},
    {"gbuffer_diffuse", "gbuffer_diffuse", "direct", VKR_RENDER_MODE_DEFAULT,
     0u},
    {"gbuffer_specular", "gbuffer_specular", "direct", VKR_RENDER_MODE_DEFAULT,
     0u},
    {"gbuffer_normal", "gbuffer_normal", "direct", VKR_RENDER_MODE_DEFAULT, 0u},
    {"deferred_emissive", "deferred_emissive", "direct",
     VKR_RENDER_MODE_DEFAULT, 0u},
    {"resolve_barycentric_lod", "resolve_barycentric_lod", "direct",
     VKR_RENDER_MODE_DEFAULT, 0u},
    {"transmission_visibility_ids", "transmission_visibility_ids", "direct",
     VKR_RENDER_MODE_DEFAULT, 0u},
    {"transmission_visibility_primitives", "transmission_visibility_primitives",
     "direct", VKR_RENDER_MODE_DEFAULT, 0u},
    {"normals", "final_color", "normals", VKR_RENDER_MODE_NORMAL, 0u},
    {"unlit", "final_color", "unlit", VKR_RENDER_MODE_UNLIT, 0u},
    {"lighting", "final_color", "lighting", VKR_RENDER_MODE_LIGHTING, 0u},
    {"direct_diffuse", "final_color", "direct_diffuse",
     VKR_RENDER_MODE_DIRECT_DIFFUSE, 0u},
    {"direct_specular", "final_color", "direct_specular",
     VKR_RENDER_MODE_DIRECT_SPECULAR, 0u},
    {"material_params", "final_color", "material_params",
     VKR_RENDER_MODE_MATERIAL_PARAMS, 0u},
    {"temporal_motion", "final_color", "temporal_motion",
     VKR_RENDER_MODE_TEMPORAL_MOTION, 0u},
    {"temporal_history", "final_color", "temporal_history",
     VKR_RENDER_MODE_TEMPORAL_HISTORY, 0u},
    {"shadow_debug_cascades", "final_color", "shadow_debug_cascades",
     VKR_RENDER_MODE_DEFAULT, 1u},
    {"shadow_debug_factor", "final_color", "shadow_debug_factor",
     VKR_RENDER_MODE_DEFAULT, 2u},
    {"shadow_debug_depth", "final_color", "shadow_debug_depth",
     VKR_RENDER_MODE_DEFAULT, 3u},
};

const VkrHarnessCaptureChannelDescription *
vkr_harness_capture_channel_description(const char *name) {
  for (uint32_t i = 0; i < ArrayCount(s_channels); ++i) {
    if (string_equals(s_channels[i].name, name)) {
      return &s_channels[i];
    }
  }
  return NULL;
}

static const VkrHarnessCaptureChannelDescription *
vkr_harness_replay_channel(const VkrHarnessCapture *capture, uint32_t index,
                           VkrHarnessError *out_error) {
  const VkrHarnessCaptureChannelDescription *channel =
      vkr_harness_capture_channel_description(capture->channels[index]);
  if (!channel) {
    vkr_harness_error_set(
        out_error, "capture.channel_unknown", "$.captures[].channels[]",
        "Unknown capture channel '%s'", capture->channels[index]);
  }
  return channel;
}

/**
 * Adds one logical channel to a replay. Channels sharing a replay agree on
 * renderer state by construction, so restating it per channel is idempotent.
 */
static bool8_t vkr_harness_replay_add_channel(
    VkrHarnessCaptureReplay *replay,
    const VkrHarnessCaptureChannelDescription *channel,
    VkrHarnessError *out_error) {
  if (replay->channel_count >= VKR_HARNESS_MAX_CAPTURE_CHANNELS) {
    vkr_harness_error_set(out_error, "capture.replay_channels",
                          "$.captures[].channels[]",
                          "Replay '%s' exceeds %u channels", replay->mode,
                          VKR_HARNESS_MAX_CAPTURE_CHANNELS);
    return false_v;
  }
  const uint32_t slot = replay->channel_count++;
  string_format(replay->logical_channels[slot],
                sizeof(replay->logical_channels[slot]), "%s", channel->name);
  string_format(replay->direct_channels[slot],
                sizeof(replay->direct_channels[slot]), "%s",
                channel->direct_channel);
  replay->render_mode = channel->render_mode;
  replay->shadow_debug_mode = channel->shadow_debug_mode;
  return true_v;
}

static VkrHarnessCaptureReplay *
vkr_harness_replay_get(VkrHarnessCaptureReplay *replays, uint32_t *count,
                       uint32_t capacity, uint32_t capture_index,
                       const VkrHarnessCaptureChannelDescription *channel,
                       VkrHarnessError *error) {
  for (uint32_t i = 0; i < *count; ++i) {
    if (replays[i].capture_index == capture_index &&
        string_equals(replays[i].mode, channel->replay_mode)) {
      return &replays[i];
    }
  }
  if (*count >= capacity) {
    vkr_harness_error_set(error, "capture.replay_capacity", "$.captures",
                          "Capture replay count exceeds %u", capacity);
    return NULL;
  }
  VkrHarnessCaptureReplay *replay = &replays[(*count)++];
  MemZero(replay, sizeof(*replay));
  replay->capture_index = capture_index;
  string_format(replay->mode, sizeof(replay->mode), "%s", channel->replay_mode);
  return replay;
}

bool8_t vkr_harness_capture_replays_build(const VkrHarnessCase *case_manifest,
                                          VkrHarnessCaptureReplay *out_replays,
                                          uint32_t capacity,
                                          uint32_t *out_count,
                                          VkrHarnessError *out_error) {
  if (!case_manifest || !out_replays || !out_count) {
    return false_v;
  }
  *out_count = 0u;
  for (uint32_t capture_index = 0; capture_index < case_manifest->capture_count;
       ++capture_index) {
    const VkrHarnessCapture *capture = &case_manifest->captures[capture_index];
    for (uint32_t i = 0; i < capture->channel_count; ++i) {
      const VkrHarnessCaptureChannelDescription *channel =
          vkr_harness_replay_channel(capture, i, out_error);
      VkrHarnessCaptureReplay *replay =
          channel ? vkr_harness_replay_get(out_replays, out_count, capacity,
                                           capture_index, channel, out_error)
                  : NULL;
      if (!replay ||
          !vkr_harness_replay_add_channel(replay, channel, out_error)) {
        return false_v;
      }
    }
  }
  return true_v;
}

bool8_t vkr_harness_capture_replay_find(const VkrHarnessCase *case_manifest,
                                        uint32_t capture_index,
                                        const char *mode,
                                        VkrHarnessCaptureReplay *out_replay,
                                        VkrHarnessError *out_error) {
  if (!case_manifest || capture_index >= case_manifest->capture_count ||
      !mode || !out_replay) {
    return false_v;
  }
  MemZero(out_replay, sizeof(*out_replay));
  out_replay->capture_index = capture_index;
  string_format(out_replay->mode, sizeof(out_replay->mode), "%s", mode);
  const VkrHarnessCapture *capture = &case_manifest->captures[capture_index];
  for (uint32_t i = 0; i < capture->channel_count; ++i) {
    const VkrHarnessCaptureChannelDescription *channel =
        vkr_harness_replay_channel(capture, i, out_error);
    if (!channel) {
      return false_v;
    }
    if (string_equals(channel->replay_mode, mode) &&
        !vkr_harness_replay_add_channel(out_replay, channel, out_error)) {
      return false_v;
    }
  }
  if (out_replay->channel_count == 0u) {
    vkr_harness_error_set(out_error, "capture.replay_unknown", "$.captures",
                          "Replay mode '%s' is not requested", mode);
    return false_v;
  }
  return true_v;
}
