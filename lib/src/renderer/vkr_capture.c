#include "vkr_capture.h"

#if defined(PLATFORM_APPLE)
#include "renderer/metal/vkr_metal_packet_renderer.h"
#endif

enum {
  VKR_CAPTURE_CHANNEL_FINAL_COLOR = 0,
  VKR_CAPTURE_CHANNEL_SCENE_COLOR,
  VKR_CAPTURE_CHANNEL_DEPTH,
  VKR_CAPTURE_CHANNEL_SHADOW_0,
  VKR_CAPTURE_CHANNEL_SHADOW_1,
  VKR_CAPTURE_CHANNEL_SHADOW_2,
  VKR_CAPTURE_CHANNEL_SHADOW_3,
  VKR_CAPTURE_CHANNEL_PICKING_IDS,
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
  if (renderer && renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
#if defined(PLATFORM_APPLE)
    return vkr_metal_packet_renderer_capture_poll(renderer->metal_renderer,
                                                  request_id, out_result);
#else
    if (out_result) {
      MemZero(out_result, sizeof(*out_result));
      out_result->error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    }
    return VKR_CAPTURE_STATUS_NOT_FOUND;
#endif
  }
  if (!renderer || !renderer->backend.capture_poll) {
    if (out_result) {
      MemZero(out_result, sizeof(*out_result));
      out_result->error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    }
    return VKR_CAPTURE_STATUS_NOT_FOUND;
  }
  return renderer->backend.capture_poll(renderer->backend_state, request_id,
                                        out_result);
}

bool8_t vkr_renderer_capture_release(VkrRendererFrontendHandle renderer,
                                     VkrCaptureRequestId request_id) {
  if (renderer && renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
#if defined(PLATFORM_APPLE)
    return vkr_metal_packet_renderer_capture_release(renderer->metal_renderer,
                                                     request_id);
#else
    return false_v;
#endif
  }
  if (renderer && renderer->backend.capture_release) {
    return renderer->backend.capture_release(renderer->backend_state,
                                             request_id);
  }
  return false_v;
}

vkr_internal uint64_t vkr_capture_align(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

vkr_internal VkrRendererError vkr_capture_reject(VkrValidationError *validation,
                                                 VkrRendererError code,
                                                 const char *message) {
  if (validation) {
    *validation = (VkrValidationError){
        .code = code, .field_path = "debug.capture", .message = message};
  }
  return code;
}

/**
 * `depth` and `scene_color` have configuration-dependent producers because the
 * editor target renders into viewport attachments. Resolving them here keeps
 * the reserved plan, graph overlay, and reported `producer_resource` aligned.
 */
vkr_internal const char *
vkr_capture_source_name(const VkrCaptureChannelDescription *channel,
                        bool8_t editor_enabled) {
  if (channel->id == VKR_CAPTURE_CHANNEL_DEPTH) {
    return editor_enabled ? "scene_depth" : "swapchain_depth";
  }
  if (channel->id == VKR_CAPTURE_CHANNEL_SCENE_COLOR) {
    return editor_enabled ? "scene_color" : "hdr_scene_color";
  }
  return channel->source_name;
}

typedef struct VkrCaptureSourceLayout {
  uint32_t width;
  uint32_t height;
  uint32_t layer;
  VkrTextureFormat format;
} VkrCaptureSourceLayout;

/**
 * Describes the subresource a channel copies from. Extent and format are the
 * producer's, not the request's: a shadow cascade is an array layer of the
 * fixed-size shadow map, while `final_color` is the whole present image rather
 * than the editor viewport.
 */
vkr_internal VkrCaptureSourceLayout vkr_capture_resolve_source(
    RendererFrontend *rf, const VkrCaptureChannelDescription *channel,
    const VkrRenderPacket *packet, uint32_t shadow_map_size) {
  VkrCaptureSourceLayout layout = {
      .width = packet->frame.viewport_width ? packet->frame.viewport_width
                                            : packet->frame.window_width,
      .height = packet->frame.viewport_height ? packet->frame.viewport_height
                                              : packet->frame.window_height,
      .layer = 0,
      .format = vkr_renderer_present_target_format(
          rf, VKR_PRESENT_TARGET_ATTACHMENT_COLOR),
  };
  switch (channel->id) {
  case VKR_CAPTURE_CHANNEL_FINAL_COLOR:
    layout.width = packet->frame.window_width;
    layout.height = packet->frame.window_height;
    break;
  case VKR_CAPTURE_CHANNEL_SCENE_COLOR:
    layout.format = VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT;
    break;
  case VKR_CAPTURE_CHANNEL_DEPTH:
    layout.format = vkr_renderer_present_target_format(
        rf, VKR_PRESENT_TARGET_ATTACHMENT_DEPTH);
    break;
  case VKR_CAPTURE_CHANNEL_SHADOW_0:
  case VKR_CAPTURE_CHANNEL_SHADOW_1:
  case VKR_CAPTURE_CHANNEL_SHADOW_2:
  case VKR_CAPTURE_CHANNEL_SHADOW_3:
    layout.width = layout.height = shadow_map_size;
    layout.layer = channel->id - VKR_CAPTURE_CHANNEL_SHADOW_0;
    layout.format = vkr_renderer_get_shadow_depth_format(rf);
    break;
  case VKR_CAPTURE_CHANNEL_PICKING_IDS:
    layout.format = VKR_TEXTURE_FORMAT_R32_UINT;
    break;
  default:
    break;
  }
  return layout;
}

VkrRendererError vkr_capture_frame_reserve(RendererFrontend *rf,
                                           const VkrRenderPacket *packet,
                                           uint32_t shadow_map_size,
                                           uint32_t shadow_cascade_count,
                                           VkrValidationError *validation) {
  vkr_capture_frame_clear(rf);
  const VkrCaptureBatchRequest *request =
      packet->debug ? packet->debug->capture : NULL;
  if (!request) {
    return VKR_RENDERER_ERROR_NONE;
  }
  if (!rf->backend.capture_reserve || request->request_id == 0 ||
      !request->items || request->item_count == 0 ||
      request->item_count > VKR_CAPTURE_MAX_ITEMS) {
    return vkr_capture_reject(validation, VKR_RENDERER_ERROR_INVALID_PARAMETER,
                              "capture request shape is invalid or "
                              "unsupported");
  }

  uint64_t seen = 0;
  uint64_t offset = 0;
  VkrCaptureFrameState *frame = &rf->capture_frame;
  for (uint32_t i = 0; i < request->item_count; ++i) {
    const VkrCaptureItemRequest *item = &request->items[i];
    const VkrCaptureChannelDescription *channel =
        vkr_renderer_capture_channel_get(item->channel);
    if (!channel) {
      return vkr_capture_reject(validation,
                                VKR_RENDERER_ERROR_INVALID_PARAMETER,
                                "capture channel does not exist");
    }
    if (seen & (1ull << item->channel)) {
      return vkr_capture_reject(validation,
                                VKR_RENDERER_ERROR_INVALID_PARAMETER,
                                "capture channel is requested twice");
    }
    seen |= 1ull << item->channel;
    if (item->mip != 0) {
      return vkr_capture_reject(validation,
                                VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE,
                                "only mip level 0 is capturable");
    }
    if (channel->required_subsystem != VKR_RENDERER_SUBSYSTEM_COUNT &&
        !vkr_renderer_subsystem_plan_includes(
            &rf->subsystem_plan,
            (VkrRendererSubsystem)channel->required_subsystem)) {
      return vkr_capture_reject(validation,
                                VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE,
                                "capture channel needs an omitted subsystem");
    }
    /* An identifier image with no producer is an unavailable channel, never a
       transparent capture that would silently compare as "no objects". */
    if (channel->id == VKR_CAPTURE_CHANNEL_PICKING_IDS &&
        (!packet->picking || !packet->picking->pending)) {
      return vkr_capture_reject(validation,
                                VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE,
                                "picking_ids needs a pending picking pass");
    }

    const VkrCaptureSourceLayout source =
        vkr_capture_resolve_source(rf, channel, packet, shadow_map_size);
    const bool8_t cascade = channel->id >= VKR_CAPTURE_CHANNEL_SHADOW_0 &&
                            channel->id <= VKR_CAPTURE_CHANNEL_SHADOW_3;
    if (cascade && (source.layer >= shadow_cascade_count || item->layer != 0)) {
      return vkr_capture_reject(validation,
                                VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE,
                                "shadow cascade layer is out of range");
    }
    if (!cascade && item->layer != 0) {
      return vkr_capture_reject(validation,
                                VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE,
                                "only array layer 0 is capturable");
    }
    /* The canonical depth converter implements exactly these two formats; a
       combined depth/stencil source has no defined canonicalization yet. */
    if (channel->value_kind == VKR_CAPTURE_VALUE_DEPTH &&
        source.format != VKR_TEXTURE_FORMAT_D32_SFLOAT &&
        source.format != VKR_TEXTURE_FORMAT_D16_UNORM) {
      return vkr_capture_reject(validation,
                                VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE,
                                "depth source format is not capturable");
    }
    if (source.width == 0 || source.height == 0) {
      return vkr_capture_reject(validation,
                                VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE,
                                "capture source has an empty extent");
    }

    VkrTextureFormatInfo format_info = {0};
    if (!vkr_texture_format_get_info(source.format, &format_info) ||
        format_info.block_width != 1u || format_info.block_height != 1u ||
        format_info.bytes_per_block == 0u) {
      return vkr_capture_reject(validation,
                                VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE,
                                "capture source format has no linear texel "
                                "encoding");
    }

    const uint64_t bytes_per_pixel = format_info.bytes_per_block;
    const uint64_t row_pitch = (uint64_t)source.width * bytes_per_pixel;
    const uint64_t data_size = vkr_texture_format_region_size(
        source.format, source.width, source.height);
    const uint64_t aligned_offset =
        vkr_capture_align(offset, VKR_CAPTURE_BUFFER_ALIGNMENT);
    if (data_size == 0u || aligned_offset < offset ||
        data_size > UINT64_MAX - aligned_offset) {
      return vkr_capture_reject(validation,
                                VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE,
                                "capture source byte size overflows");
    }
    offset = aligned_offset;
    frame->plans[i] = (VkrCaptureBackendItemPlan){
        .result = {.channel = item->channel,
                   .width = source.width,
                   .height = source.height,
                   .row_pitch = row_pitch,
                   .format = source.format,
                   .value_kind = channel->value_kind,
                   .color_space = channel->color_space,
                   .origin = VKR_CAPTURE_ORIGIN_TOP_LEFT,
                   .data_size = data_size,
                   .mip = item->mip,
                   .layer = source.layer,
                   .display_exposure = packet->globals.exposure},
        .buffer_offset = offset};
    string_format(
        frame->plans[i].result.producer_resource,
        sizeof(frame->plans[i].result.producer_resource), "%s",
        vkr_capture_source_name(channel, packet->frame.editor_enabled));
    offset += data_size;
  }

  const VkrRendererError error =
      rf->backend.capture_reserve(rf->backend_state, request, frame->plans,
                                  packet->frame.frame_index, &frame->buffer);
  if (error != VKR_RENDERER_ERROR_NONE) {
    return error == VKR_RENDERER_ERROR_CAPTURE_BUSY
               ? error
               : vkr_capture_reject(validation, error,
                                    "capture batch reservation failed");
  }
  frame->active = true_v;
  frame->request_id = request->request_id;
  frame->item_count = request->item_count;
  frame->editor_enabled = packet->frame.editor_enabled;
  return VKR_RENDERER_ERROR_NONE;
}

vkr_internal void vkr_capture_execute(VkrRgPassContext *ctx, void *user_data) {
  VkrCaptureFrameState *frame = user_data;
  if (!ctx || !ctx->renderer || !frame || !frame->active ||
      !vkr_rg_pass_get_buffer_handle(ctx, frame->graph_buffer)) {
    if (ctx) {
      ctx->error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    }
    return;
  }
  for (uint32_t i = 0; i < frame->item_count; ++i) {
    VkrTextureOpaqueHandle texture =
        vkr_rg_pass_get_image_texture(ctx, frame->images[i]);
    if (!texture) {
      ctx->error = VKR_RENDERER_ERROR_INVALID_HANDLE;
      return;
    }
    ctx->error = ctx->renderer->backend.capture_record_item(
        ctx->renderer->backend_state, frame->request_id, i,
        (VkrBackendResourceHandle){.ptr = texture});
    if (ctx->error != VKR_RENDERER_ERROR_NONE) {
      return;
    }
  }
}

bool8_t vkr_capture_graph_overlay_build(RendererFrontend *rf) {
  VkrCaptureFrameState *frame = &rf->capture_frame;
  if (!frame->active) {
    return true_v;
  }
  frame->graph_buffer = vkr_rg_import_buffer(
      rf->render_graph, string8_lit("capture.batch.buffer"),
      (VkrBufferHandle)frame->buffer.ptr, VKR_RG_BUFFER_ACCESS_NONE);
  if (!vkr_rg_buffer_handle_valid(frame->graph_buffer)) {
    return false_v;
  }
  if (!vkr_rg_imported_buffer_add_usage(rf->render_graph, frame->graph_buffer,
                                        VKR_BUFFER_USAGE_TRANSFER_DST)) {
    return false_v;
  }
  VkrRgPassBuilder pass =
      vkr_rg_add_pass(rf->render_graph, VKR_RG_PASS_TYPE_TRANSFER,
                      string8_lit("Capture.Readback"));
  vkr_rg_pass_set_flags(&pass, VKR_RG_PASS_FLAG_NO_CULL);
  vkr_rg_pass_set_execute(&pass, vkr_capture_execute, frame);
  vkr_rg_pass_write_buffer(&pass, frame->graph_buffer,
                           VKR_RG_BUFFER_ACCESS_TRANSFER_DST, 0, 0);
  for (uint32_t i = 0; i < frame->item_count; ++i) {
    const VkrCaptureChannelDescription *channel =
        vkr_renderer_capture_channel_get(frame->plans[i].result.channel);
    const char *source =
        vkr_capture_source_name(channel, frame->editor_enabled);
    frame->images[i] = vkr_rg_find_image(
        rf->render_graph, string8_create_from_cstr((const uint8_t *)source,
                                                   string_length(source)));
    if (!vkr_rg_image_handle_valid(frame->images[i])) {
      return false_v;
    }
    /* Graph-owned resources declare TRANSFER_SRC in the authored JSON. Present
       images are external, so the request-specific overlay is the only place
       their capture capability — already validated against the surface at
       initialization — enters this graph. */
    const bool8_t external_capture_source =
        channel->id == VKR_CAPTURE_CHANNEL_FINAL_COLOR ||
        (channel->id == VKR_CAPTURE_CHANNEL_DEPTH && !frame->editor_enabled);
    if (external_capture_source &&
        !vkr_rg_imported_image_add_usage(rf->render_graph, frame->images[i],
                                         VKR_TEXTURE_USAGE_TRANSFER_SRC)) {
      return false_v;
    }
    vkr_rg_pass_read_image_slice(
        &pass, frame->images[i], VKR_RG_IMAGE_ACCESS_TRANSFER_SRC, i, 0,
        (VkrRgImageSlice){.mip_level = frame->plans[i].result.mip,
                          .base_layer = frame->plans[i].result.layer,
                          .layer_count = 1});
  }
  return true_v;
}

void vkr_capture_frame_clear(RendererFrontend *rf) {
  MemZero(&rf->capture_frame, sizeof(rf->capture_frame));
}
