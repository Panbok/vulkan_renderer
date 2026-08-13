#include "vkr_capture.h"

#include "renderer/renderer_frontend.h"

enum {
  VKR_CAPTURE_CHANNEL_FINAL_COLOR = 0,
  VKR_CAPTURE_CHANNEL_SCENE_COLOR,
  VKR_CAPTURE_CHANNEL_DEPTH,
  VKR_CAPTURE_CHANNEL_SHADOW_0,
  VKR_CAPTURE_CHANNEL_SHADOW_1,
  VKR_CAPTURE_CHANNEL_SHADOW_2,
  VKR_CAPTURE_CHANNEL_SHADOW_3,
  VKR_CAPTURE_CHANNEL_PICKING_IDS,
  VKR_CAPTURE_CHANNEL_VISIBILITY_IDS,
  VKR_CAPTURE_CHANNEL_VISIBILITY_PRIMITIVES,
  VKR_CAPTURE_CHANNEL_GBUFFER_DIFFUSE,
  VKR_CAPTURE_CHANNEL_GBUFFER_SPECULAR,
  VKR_CAPTURE_CHANNEL_GBUFFER_NORMAL,
  VKR_CAPTURE_CHANNEL_DEFERRED_EMISSIVE,
  VKR_CAPTURE_CHANNEL_RESOLVE_BARYCENTRIC_LOD,
};

static const VkrCaptureChannelDescription s_capture_channels[] = {
    {VKR_CAPTURE_CHANNEL_FINAL_COLOR, "final_color", "swapchain",
     VKR_RENDERER_SUBSYSTEM_COUNT, VKR_CAPTURE_ASPECT_COLOR,
     VKR_CAPTURE_VALUE_COLOR, VKR_CAPTURE_COLOR_SPACE_SRGB, "RGBA8_SRGB_PNG",
     1},
    {VKR_CAPTURE_CHANNEL_SCENE_COLOR, "scene_color", "scene_color",
     VKR_RENDERER_SUBSYSTEM_COUNT, VKR_CAPTURE_ASPECT_COLOR,
     VKR_CAPTURE_VALUE_COLOR, VKR_CAPTURE_COLOR_SPACE_SRGB, "RGBA8_SRGB_PNG",
     2},
    {VKR_CAPTURE_CHANNEL_DEPTH, "depth", "depth", VKR_RENDERER_SUBSYSTEM_COUNT,
     VKR_CAPTURE_ASPECT_DEPTH, VKR_CAPTURE_VALUE_DEPTH,
     VKR_CAPTURE_COLOR_SPACE_NONE, "R32_FLOAT_LE", 1},
    {VKR_CAPTURE_CHANNEL_SHADOW_0, "shadow_cascade_0", "shadow_map",
     VKR_RENDERER_SUBSYSTEM_SHADOWS, VKR_CAPTURE_ASPECT_DEPTH,
     VKR_CAPTURE_VALUE_DEPTH, VKR_CAPTURE_COLOR_SPACE_NONE, "R32_FLOAT_LE", 1},
    {VKR_CAPTURE_CHANNEL_SHADOW_1, "shadow_cascade_1", "shadow_map",
     VKR_RENDERER_SUBSYSTEM_SHADOWS, VKR_CAPTURE_ASPECT_DEPTH,
     VKR_CAPTURE_VALUE_DEPTH, VKR_CAPTURE_COLOR_SPACE_NONE, "R32_FLOAT_LE", 1},
    {VKR_CAPTURE_CHANNEL_SHADOW_2, "shadow_cascade_2", "shadow_map",
     VKR_RENDERER_SUBSYSTEM_SHADOWS, VKR_CAPTURE_ASPECT_DEPTH,
     VKR_CAPTURE_VALUE_DEPTH, VKR_CAPTURE_COLOR_SPACE_NONE, "R32_FLOAT_LE", 1},
    {VKR_CAPTURE_CHANNEL_SHADOW_3, "shadow_cascade_3", "shadow_map",
     VKR_RENDERER_SUBSYSTEM_SHADOWS, VKR_CAPTURE_ASPECT_DEPTH,
     VKR_CAPTURE_VALUE_DEPTH, VKR_CAPTURE_COLOR_SPACE_NONE, "R32_FLOAT_LE", 1},
    {VKR_CAPTURE_CHANNEL_PICKING_IDS, "picking_ids", "picking_color",
     VKR_RENDERER_SUBSYSTEM_PICKING, VKR_CAPTURE_ASPECT_COLOR,
     VKR_CAPTURE_VALUE_UINT, VKR_CAPTURE_COLOR_SPACE_NONE, "R32_UINT_LE", 1},
    {VKR_CAPTURE_CHANNEL_VISIBILITY_IDS, "visibility_ids", "opaque_vbuffer",
     VKR_RENDERER_SUBSYSTEM_COUNT, VKR_CAPTURE_ASPECT_COLOR,
     VKR_CAPTURE_VALUE_UINT, VKR_CAPTURE_COLOR_SPACE_NONE, "R32_UINT_LE", 1},
    {VKR_CAPTURE_CHANNEL_VISIBILITY_PRIMITIVES, "visibility_primitives",
     "opaque_vbuffer", VKR_RENDERER_SUBSYSTEM_COUNT, VKR_CAPTURE_ASPECT_COLOR,
     VKR_CAPTURE_VALUE_UINT, VKR_CAPTURE_COLOR_SPACE_NONE, "R32_UINT_LE", 1},
    {VKR_CAPTURE_CHANNEL_GBUFFER_DIFFUSE, "gbuffer_diffuse", "gbuffer_albedo",
     VKR_RENDERER_SUBSYSTEM_COUNT, VKR_CAPTURE_ASPECT_COLOR,
     VKR_CAPTURE_VALUE_COLOR, VKR_CAPTURE_COLOR_SPACE_LINEAR, "RGBA8_UNORM", 1},
    {VKR_CAPTURE_CHANNEL_GBUFFER_SPECULAR, "gbuffer_specular",
     "gbuffer_specular", VKR_RENDERER_SUBSYSTEM_COUNT, VKR_CAPTURE_ASPECT_COLOR,
     VKR_CAPTURE_VALUE_COLOR, VKR_CAPTURE_COLOR_SPACE_LINEAR, "RGBA8_UNORM", 1},
    {VKR_CAPTURE_CHANNEL_GBUFFER_NORMAL, "gbuffer_normal", "gbuffer_normal",
     VKR_RENDERER_SUBSYSTEM_COUNT, VKR_CAPTURE_ASPECT_COLOR,
     VKR_CAPTURE_VALUE_COLOR, VKR_CAPTURE_COLOR_SPACE_LINEAR, "RG16_SNORM", 1},
    {VKR_CAPTURE_CHANNEL_DEFERRED_EMISSIVE, "deferred_emissive",
     "deferred_emissive_seed", VKR_RENDERER_SUBSYSTEM_COUNT,
     VKR_CAPTURE_ASPECT_COLOR, VKR_CAPTURE_VALUE_COLOR,
     VKR_CAPTURE_COLOR_SPACE_LINEAR, "RGBA16_FLOAT_LE", 1},
    {VKR_CAPTURE_CHANNEL_RESOLVE_BARYCENTRIC_LOD, "resolve_barycentric_lod",
     "deferred_resolve_debug", VKR_RENDERER_SUBSYSTEM_COUNT,
     VKR_CAPTURE_ASPECT_COLOR, VKR_CAPTURE_VALUE_COLOR,
     VKR_CAPTURE_COLOR_SPACE_LINEAR, "RGBA16_FLOAT_LE", 1},
};

/* Duplicate requests are rejected with one 64-bit mask keyed by channel id. */
_Static_assert(ArrayCount(s_capture_channels) <= 64,
               "Capture channel ids must fit the duplicate-request bitmask");

uint32_t vkr_renderer_capture_channel_count(void) {
  return (uint32_t)ArrayCount(s_capture_channels);
}

const VkrCaptureChannelDescription *
vkr_renderer_capture_channel_get(uint32_t index) {
  return index < vkr_renderer_capture_channel_count()
             ? &s_capture_channels[index]
             : NULL;
}

VkrCaptureChannelId vkr_renderer_capture_channel_from_name(const char *name) {
  if (!name) {
    return VKR_CAPTURE_CHANNEL_INVALID;
  }
  for (uint32_t i = 0; i < vkr_renderer_capture_channel_count(); ++i) {
    if (string_equals(name, s_capture_channels[i].name)) {
      return s_capture_channels[i].id;
    }
  }
  return VKR_CAPTURE_CHANNEL_INVALID;
}

VkrCaptureStatus vkr_renderer_capture_poll(VkrRendererFrontendHandle renderer,
                                           VkrCaptureRequestId request_id,
                                           VkrCapturePollResult *out_result) {
  if (!renderer || !renderer->impl.ops || !renderer->impl.ops->capture_poll) {
    if (out_result) {
      MemZero(out_result, sizeof(*out_result));
      out_result->error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    }
    return VKR_CAPTURE_STATUS_NOT_FOUND;
  }
  return renderer->impl.ops->capture_poll(renderer->impl.state, request_id,
                                          out_result);
}

bool8_t vkr_renderer_capture_release(VkrRendererFrontendHandle renderer,
                                     VkrCaptureRequestId request_id) {
  if (renderer && renderer->impl.ops && renderer->impl.ops->capture_release) {
    return renderer->impl.ops->capture_release(renderer->impl.state,
                                               request_id);
  }
  return false_v;
}
