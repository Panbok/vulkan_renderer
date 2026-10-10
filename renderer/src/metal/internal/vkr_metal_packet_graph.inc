/* Render-graph resources, dependency lowering, pass descriptors, and capture
 * plans. */
#include "metal/internal/vkr_metal_packet_prepared.h"

vkr_internal MTLPixelFormat
vkr_metal_packet_pixel_format(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM:
    return MTLPixelFormatRGBA8Unorm;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB:
    return MTLPixelFormatRGBA8Unorm_sRGB;
  case VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM:
    return MTLPixelFormatBGRA8Unorm;
  case VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB:
    return MTLPixelFormatBGRA8Unorm_sRGB;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UINT:
    return MTLPixelFormatRGBA8Uint;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SNORM:
    return MTLPixelFormatRGBA8Snorm;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SINT:
    return MTLPixelFormatRGBA8Sint;
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
    return MTLPixelFormatBC7_RGBAUnorm;
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
    return MTLPixelFormatBC7_RGBAUnorm_sRGB;
  case VKR_TEXTURE_FORMAT_BC5_UNORM:
    return MTLPixelFormatBC5_RGUnorm;
  case VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM:
    return MTLPixelFormatEAC_RGBA8;
  case VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB:
    return MTLPixelFormatEAC_RGBA8_sRGB;
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
    return MTLPixelFormatASTC_4x4_LDR;
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return MTLPixelFormatASTC_4x4_sRGB;
  case VKR_TEXTURE_FORMAT_ASTC_6x6_UNORM:
    return MTLPixelFormatASTC_6x6_LDR;
  case VKR_TEXTURE_FORMAT_ASTC_6x6_SRGB:
    return MTLPixelFormatASTC_6x6_sRGB;
  case VKR_TEXTURE_FORMAT_ASTC_4x4_HDR:
    return MTLPixelFormatASTC_4x4_HDR;
  case VKR_TEXTURE_FORMAT_RGB9E5_UFLOAT:
    return MTLPixelFormatRGB9E5Float;
  case VKR_TEXTURE_FORMAT_BC6H_UFLOAT:
    return MTLPixelFormatBC6H_RGBUfloat;
  case VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM:
    return MTLPixelFormatEAC_RG11Unorm;
  case VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT:
    return MTLPixelFormatRGBA16Float;
  case VKR_TEXTURE_FORMAT_R32G32B32A32_SFLOAT:
    return MTLPixelFormatRGBA32Float;
  case VKR_TEXTURE_FORMAT_R8_UNORM:
    return MTLPixelFormatR8Unorm;
  case VKR_TEXTURE_FORMAT_R16_SFLOAT:
    return MTLPixelFormatR16Float;
  case VKR_TEXTURE_FORMAT_R32_SFLOAT:
    return MTLPixelFormatR32Float;
  case VKR_TEXTURE_FORMAT_R32_UINT:
    return MTLPixelFormatR32Uint;
  case VKR_TEXTURE_FORMAT_R8G8_UNORM:
    return MTLPixelFormatRG8Unorm;
  case VKR_TEXTURE_FORMAT_R32G32_UINT:
    return MTLPixelFormatRG32Uint;
  case VKR_TEXTURE_FORMAT_R32G32B32A32_UINT:
    return MTLPixelFormatRGBA32Uint;
  case VKR_TEXTURE_FORMAT_R16G16_SNORM:
    return MTLPixelFormatRG16Snorm;
  case VKR_TEXTURE_FORMAT_R16G16_SFLOAT:
    return MTLPixelFormatRG16Float;
  case VKR_TEXTURE_FORMAT_D16_UNORM:
    return MTLPixelFormatDepth16Unorm;
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
    return MTLPixelFormatDepth32Float;
  default:
    return MTLPixelFormatInvalid;
  }
}

vkr_internal MTLPixelFormat vkr_metal_packet_target_pixel_format_for(
    const VkrMetalPacketRenderer *renderer, bool8_t extended_linear) {
  if (renderer->target_kind == VKR_METAL_PACKET_TARGET_WINDOW) {
    if (extended_linear)
      return MTLPixelFormatRGBA16Float;
    return renderer->srgb_output ? MTLPixelFormatBGRA8Unorm_sRGB
                                 : MTLPixelFormatBGRA8Unorm;
  }
  return renderer->srgb_output ? MTLPixelFormatRGBA8Unorm_sRGB
                               : MTLPixelFormatRGBA8Unorm;
}

vkr_internal MTLPixelFormat
vkr_metal_packet_target_pixel_format(const VkrMetalPacketRenderer *renderer) {
  return vkr_metal_packet_target_pixel_format_for(
      renderer, renderer->display_output.extended_linear != 0u);
}

vkr_internal MTLTextureUsage
vkr_metal_packet_texture_usage(VkrTextureUsageFlags usage) {
  MTLTextureUsage result = MTLTextureUsageUnknown;
  if (usage.set & VKR_TEXTURE_USAGE_SAMPLED)
    result |= MTLTextureUsageShaderRead;
  if (usage.set & VKR_TEXTURE_USAGE_STORAGE)
    result |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  if (usage.set & (VKR_TEXTURE_USAGE_COLOR_ATTACHMENT |
                   VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT))
    result |= MTLTextureUsageRenderTarget;
  return result;
}

vkr_internal bool8_t vkr_metal_packet_image_desc_equal(
    const VkrRgImageDesc *a, const VkrRgImageDesc *b) {
  return a->width == b->width && a->height == b->height &&
         a->depth == b->depth &&
         a->format == b->format && a->usage.set == b->usage.set &&
         a->samples == b->samples && a->layers == b->layers &&
         a->mip_levels == b->mip_levels && a->type == b->type;
}

vkr_internal void
vkr_metal_packet_release_image_views(VkrMetalPacketImageViews *views) {
  if (!views)
    return;
  for (uint32_t mip = 0u; mip < VKR_METAL_PACKET_MAX_TEXTURE_MIPS; ++mip) {
    if (views->mip_views[mip]) {
      [(id<MTLTexture>)views->mip_views[mip] release];
      views->mip_views[mip] = NULL;
    }
  }
  for (uint32_t layer = 0u; layer < VKR_METAL_PACKET_MAX_TEXTURE_LAYERS;
       ++layer) {
    if (views->layer_views[layer]) {
      [(id<MTLTexture>)views->layer_views[layer] release];
      views->layer_views[layer] = NULL;
    }
  }
}

vkr_internal void
vkr_metal_packet_collect_retired_image_views(VkrMetalPacketRenderer *renderer,
                                             uint64_t completed_submit_value) {
  if (!renderer || renderer->retired_image_view_count == 0u)
    return;
  const uint64_t capacity =
      (uint64_t)renderer->max_images * renderer->history_instance_count * 2u;
  for (uint64_t i = 0u; i < capacity; ++i) {
    VkrMetalPacketRetiredImageViews *views = &renderer->retired_image_views[i];
    if (!views->live || views->last_use_submit_value > completed_submit_value)
      continue;
    vkr_metal_packet_release_image_views(&views->views);
    *views = (VkrMetalPacketRetiredImageViews){0};
    renderer->retired_image_view_count--;
  }
}

vkr_internal void
vkr_metal_packet_geometry_ranges_diagnostic(VkrMetalPacketRenderer *renderer,
                                            const char *event) {
  if (!renderer->diagnostics.enabled)
    return;
  VkrGeometryMegabufferMetrics metrics = {
      .vertex_capacity_bytes = renderer->geometry_megabuffer.vertices.length,
      .index_capacity_bytes = renderer->geometry_megabuffer.indices.length,
  };
  vkr_geometry_ranges_metrics(&renderer->geometry_ranges, &metrics);
  VKR_METAL_DIAGNOSTIC(
      renderer, event,
      "live_spans=%llu retired_spans=%llu reusable=%llu retired_bytes=%llu "
      "vertex_capacity=%llu index_capacity=%llu",
      (unsigned long long)metrics.live_range_count,
      (unsigned long long)metrics.retired_range_count,
      (unsigned long long)metrics.reusable_range_bytes,
      (unsigned long long)metrics.retired_range_bytes,
      (unsigned long long)metrics.vertex_capacity_bytes,
      (unsigned long long)metrics.index_capacity_bytes);
}

/* Views retain the same backing allocation. Drop every completed view before
 * returning that allocation's heap charge, using one completion observation. */
vkr_internal VkrMetalMemoryStatus vkr_metal_packet_collect_memory(
    VkrMetalPacketRenderer *renderer, uint64_t completed_submit_value,
    uint32_t *out_collected_count) {
  vkr_metal_packet_collect_retired_image_views(renderer,
                                               completed_submit_value);
  VkrGeometryMegabufferMetrics before = {0}, after = {0};
  if (renderer->diagnostics.enabled)
    vkr_geometry_ranges_metrics(&renderer->geometry_ranges, &before);
  if (!vkr_geometry_ranges_collect(&renderer->geometry_ranges,
                                   completed_submit_value))
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  if (renderer->diagnostics.enabled) {
    vkr_geometry_ranges_metrics(&renderer->geometry_ranges, &after);
    if (after.retired_range_count != before.retired_range_count)
      vkr_metal_packet_geometry_ranges_diagnostic(renderer,
                                                  "geometry_ranges_collected");
  }
  return vkr_metal_memory_device_collect(
      renderer->memory, completed_submit_value, out_collected_count);
}

vkr_internal bool8_t vkr_metal_packet_retire_image_instance(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketImageInstance *instance) {
  if (!instance || !instance->live)
    return true_v;
  VKR_METAL_DIAGNOSTIC(renderer, "graph_image_retire",
                       "resource=%llu retire_after=%llu owned=%u",
                       (unsigned long long)instance->resource.resource_id,
                       (unsigned long long)instance->last_use_submit_value,
                       instance->owned);
  VkrMetalPacketRetiredImageViews *deferred_views = NULL;
  const uint64_t completed = renderer->completion.signaledValue;
  if (instance->owned && instance->last_use_submit_value > completed) {
    const uint64_t capacity =
        (uint64_t)renderer->max_images * renderer->history_instance_count * 2u;
    for (uint64_t i = 0u; i < capacity; ++i) {
      if (!renderer->retired_image_views[i].live) {
        deferred_views = &renderer->retired_image_views[i];
        break;
      }
    }
    if (!deferred_views) {
      log_error("Metal retired image-view capacity exhausted (%llu live)",
                (unsigned long long)renderer->retired_image_view_count);
      return false_v;
    }
  }
  if (instance->owned &&
      vkr_metal_memory_device_retire(
          renderer->memory, instance->resource.handle,
          instance->last_use_submit_value) != VKR_METAL_MEMORY_STATUS_OK)
    return false_v;
  if (deferred_views) {
    deferred_views->views = instance->views;
    deferred_views->last_use_submit_value = instance->last_use_submit_value;
    deferred_views->live = true_v;
    renderer->retired_image_view_count++;
  } else {
    vkr_metal_packet_release_image_views(&instance->views);
  }
  *instance = (VkrMetalPacketImageInstance){0};
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_retire_image_instances(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketImageInstance *instances,
    uint32_t instance_count) {
  if (!renderer || (!instances && instance_count > 0u))
    return false_v;
  uint32_t deferred_view_count = 0u;
  const uint64_t completed = renderer->completion.signaledValue;
  for (uint32_t i = 0u; i < instance_count; ++i) {
    const VkrMetalPacketImageInstance *instance = &instances[i];
    if (instance->live && instance->owned &&
        instance->last_use_submit_value > completed)
      deferred_view_count++;
  }
  const uint64_t capacity =
      (uint64_t)renderer->max_images * renderer->history_instance_count * 2u;
  if (renderer->retired_image_view_count > capacity ||
      (uint64_t)deferred_view_count >
          capacity - renderer->retired_image_view_count) {
    log_error("Metal retired image-view capacity cannot accept %u instances "
              "(%llu/%llu live)",
              deferred_view_count,
              (unsigned long long)renderer->retired_image_view_count,
              (unsigned long long)capacity);
    return false_v;
  }
  for (uint32_t i = 0u; i < instance_count; ++i) {
    if (!vkr_metal_packet_retire_image_instance(renderer, &instances[i]))
      return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_graph_image_format_supported(
    VkrMetalPacketRenderer *renderer, const VkrRgImageDesc *desc) {
  VkrTextureFormatInfo info = {0};
  if (!renderer || !desc ||
      vkr_metal_packet_pixel_format(desc->format) == MTLPixelFormatInvalid ||
      !vkr_texture_format_get_info(desc->format, &info) ||
      ![renderer->device supportsTextureSampleCount:desc->samples])
    return false_v;
  if ((desc->usage.set & VKR_TEXTURE_USAGE_STORAGE) != 0u &&
      (info.is_block_compressed || info.is_depth_stencil))
    return false_v;
  if ((desc->usage.set & VKR_TEXTURE_USAGE_COLOR_ATTACHMENT) != 0u &&
      info.is_depth_stencil)
    return false_v;
  if ((desc->usage.set & VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT) != 0u &&
      !info.is_depth_stencil)
    return false_v;
  return desc->samples == VKR_SAMPLE_COUNT_1 || desc->mip_levels == 1u;
}

/* Executors of the tiled pipeline (ADR-087), the only class Metal runs. */
vkr_internal bool8_t
vkr_metal_packet_executor_supported(VkrRgExecutorKind kind) {
  switch (kind) {
  case VKR_RG_EXECUTOR_SHADOW:
  case VKR_RG_EXECUTOR_LOCAL_SHADOW:
  case VKR_RG_EXECUTOR_LOCAL_SHADOW_COPY:
  case VKR_RG_EXECUTOR_LOCAL_SHADOW_DYNAMIC:
  case VKR_RG_EXECUTOR_PICKING:
  case VKR_RG_EXECUTOR_PICKING_READBACK:
  case VKR_RG_EXECUTOR_IBL_BAKE:
  case VKR_RG_EXECUTOR_GPU_DRAW_UPLOAD:
  case VKR_RG_EXECUTOR_GPU_DRAW_CLASSIFY:
  case VKR_RG_EXECUTOR_GPU_DRAW_PREFIX:
  case VKR_RG_EXECUTOR_GPU_DRAW_ENCODE:
  case VKR_RG_EXECUTOR_SKINNING:
  case VKR_RG_EXECUTOR_SKY_VIEW_LUT:
  case VKR_RG_EXECUTOR_AERIAL_PERSPECTIVE:
  case VKR_RG_EXECUTOR_CLOUD_SHADOW:
  case VKR_RG_EXECUTOR_CLOUD_TRACE:
  case VKR_RG_EXECUTOR_CLOUD_SKY_LIGHT:
  case VKR_RG_EXECUTOR_DIFFUSE_VOLUME_COMPOSE:
  case VKR_RG_EXECUTOR_EXPOSURE_HISTOGRAM:
  case VKR_RG_EXECUTOR_EXPOSURE_RESOLVE:
  case VKR_RG_EXECUTOR_BLOOM_PREFILTER:
  case VKR_RG_EXECUTOR_BLOOM_DOWNSAMPLE:
  case VKR_RG_EXECUTOR_BLOOM_UPSAMPLE:
  case VKR_RG_EXECUTOR_TRANSMISSION_DOWNSAMPLE:
  case VKR_RG_EXECUTOR_HZB_BUILD:
  case VKR_RG_EXECUTOR_TONEMAP:
  case VKR_RG_EXECUTOR_TONEMAP_PREPARE:
  case VKR_RG_EXECUTOR_EDITOR:
  case VKR_RG_EXECUTOR_CLEAR:
  case VKR_RG_EXECUTOR_EDITOR_OVERLAY:
  case VKR_RG_EXECUTOR_EDITOR_OVERLAY_PICKING:
  case VKR_RG_EXECUTOR_EDITOR_SELECTION_MASK:
  case VKR_RG_EXECUTOR_EDITOR_SELECTION_OUTLINE:
  case VKR_RG_EXECUTOR_EDITOR_GRID:
  case VKR_RG_EXECUTOR_ANIMATION_PREVIEW:
  case VKR_RG_EXECUTOR_UI:
  case VKR_RG_EXECUTOR_TILED_OPAQUE:
  case VKR_RG_EXECUTOR_TILED_ATMOSPHERE:
  case VKR_RG_EXECUTOR_TILED_PICKING:
  case VKR_RG_EXECUTOR_TILED_BLEND:
    return true_v;
  default:
    return false_v;
  }
}

/* Rejects a loaded graph naming an executor the Metal backend does not
   implement, such as the desktop pipeline's, before any frame could skip its
   pass. Every pass counts, whatever its condition. */
vkr_internal bool8_t
vkr_metal_packet_validate_graph_executors(const VkrRgJsonGraph *graph) {
  for (uint64_t i = 0u; i < graph->passes.length; ++i) {
    const VkrRgJsonPass *pass = array_get_VkrRgJsonPass(&graph->passes, i);
    if (!pass->executor_resolved || pass->executor_id == 0u ||
        pass->executor_id > VKR_RG_EXECUTOR_COUNT) {
      log_error("Metal graph pass '%.*s' has no executor kind",
                (int)pass->name.length, pass->name.str);
      return false_v;
    }
    const VkrRgExecutorKind kind = (VkrRgExecutorKind)(pass->executor_id - 1u);
    if (!vkr_metal_packet_executor_supported(kind)) {
      log_error("Metal graph pass '%.*s' requires executor '%s', which the "
                "Metal backend does not implement",
                (int)pass->name.length, pass->name.str,
                vkr_render_graph_executor_name(kind));
      return false_v;
    }
  }
  return true_v;
}

vkr_internal VkrRendererError
vkr_metal_packet_allocation_error(VkrMetalMemoryStatus status) {
  switch (status) {
  case VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES:
  case VKR_METAL_MEMORY_STATUS_OUT_OF_HANDLES:
  case VKR_METAL_MEMORY_STATUS_FRAGMENTED:
  case VKR_METAL_MEMORY_STATUS_OUT_OF_RANGE_METADATA:
  case VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED:
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  default:
    return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  }
}

/* Borrows the graph descriptor until this realization attempt is reported. */
typedef struct VkrMetalPacketImageAllocationFailure {
  const VkrRgImage *image;
  VkrMetalMemoryStatus status;
} VkrMetalPacketImageAllocationFailure;

vkr_internal bool8_t vkr_metal_packet_create_image_instance(
    VkrMetalPacketRenderer *renderer, const VkrRgImage *image,
    VkrMetalPacketImageInstance *instance, VkrRendererError *out_error,
    VkrMetalPacketImageAllocationFailure *out_failure) API_AVAILABLE(macos(26.0)) {
  const MTLPixelFormat format =
      vkr_metal_packet_pixel_format(image->desc.format);
  if (!vkr_metal_packet_graph_image_format_supported(renderer, &image->desc) ||
      format == MTLPixelFormatInvalid || image->desc.width == 0 ||
      image->desc.height == 0 || image->desc.depth == 0 ||
      image->desc.layers == 0 ||
      image->desc.mip_levels == 0 ||
      image->desc.mip_levels > VKR_METAL_PACKET_MAX_TEXTURE_MIPS) {
    log_error("Metal graph image '%.*s' has unsupported descriptor: format=%u "
              "extent=%ux%ux%u layers=%u mips=%u samples=%u usage=0x%x",
              (int)image->name.length, image->name.str, image->desc.format,
              image->desc.width, image->desc.height, image->desc.depth,
              image->desc.layers,
              image->desc.mip_levels, image->desc.samples,
              image->desc.usage.set);
    return false_v;
  }

  const bool8_t texture_3d = image->desc.type == VKR_TEXTURE_TYPE_3D;
  const bool8_t array_texture =
      !texture_3d &&
      (image->desc.layers > 1u ||
       (image->desc.flags & VKR_RG_RESOURCE_FLAG_FORCE_ARRAY) != 0u);
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
  descriptor.textureType = texture_3d ? MTLTextureType3D
                                       : (array_texture ? MTLTextureType2DArray
                                                        : MTLTextureType2D);
  descriptor.pixelFormat = format;
  descriptor.width = image->desc.width;
  descriptor.height = image->desc.height;
  descriptor.depth = texture_3d ? image->desc.depth : 1u;
  descriptor.arrayLength = texture_3d ? 1u : image->desc.layers;
  descriptor.mipmapLevelCount = image->desc.mip_levels;
  descriptor.sampleCount = image->desc.samples;
  descriptor.usage = vkr_metal_packet_texture_usage(image->desc.usage);
  VkrMetalTextureResource resource = {0};
  const VkrGpuAllocationOwner owner =
      image->imported && vkr_string8_equals_cstr(&image->name, "swapchain")
          ? VKR_GPU_ALLOCATION_OWNER_SWAPCHAIN
          : VKR_GPU_ALLOCATION_OWNER_RENDER_GRAPH;
  const VkrMetalMemoryStatus status = vkr_metal_memory_device_create_texture(
      renderer->memory, descriptor, owner, &resource);
  [descriptor release];
  if (status != VKR_METAL_MEMORY_STATUS_OK) {
    *out_error = vkr_metal_packet_allocation_error(status);
    *out_failure = (VkrMetalPacketImageAllocationFailure){image, status};
    VKR_METAL_DIAGNOSTIC(
        renderer, "graph_image_allocation_failed",
        "name=%.*s status=%s format=%u extent=%ux%ux%u layers=%u mips=%u usage=0x%x",
        (int)image->name.length, image->name.str,
        vkr_metal_memory_status_string(status), image->desc.format,
        image->desc.width, image->desc.height, image->desc.depth,
        image->desc.layers,
        image->desc.mip_levels, image->desc.usage.set);
    return false_v;
  }

  *instance = (VkrMetalPacketImageInstance){
      .resource = resource,
      .live = true_v,
      .owned = true_v,
  };
  VKR_METAL_DIAGNOSTIC(renderer, "graph_image_create",
                       "name=%.*s resource=%llu extent=%ux%ux%u mips=%u layers=%u",
                       (int)image->name.length, image->name.str,
                       (unsigned long long)resource.resource_id,
                       image->desc.width, image->desc.height,
                       image->desc.depth,
                       image->desc.mip_levels, image->desc.layers);
  id<MTLTexture> texture = (id<MTLTexture>)resource.metal_texture;
  const MTLTextureType view_type = texture_3d
                                       ? MTLTextureType3D
                                       : (array_texture ? MTLTextureType2DArray
                                                        : MTLTextureType2D);
  for (uint32_t mip = 0u; mip < image->desc.mip_levels; ++mip) {
    id<MTLTexture> view = [texture
        newTextureViewWithPixelFormat:format
                          textureType:view_type
                               levels:NSMakeRange(mip, 1u)
                               slices:NSMakeRange(0u,
                                                  texture_3d ? 1u
                                                             : image->desc.layers)];
    if (!view) {
      log_error("Metal graph image '%.*s' mip %u view creation failed",
                (int)image->name.length, image->name.str, mip);
      vkr_metal_packet_release_image_views(&instance->views);
      (void)vkr_metal_memory_device_retire(renderer->memory, resource.handle,
                                           renderer->completion.signaledValue);
      (void)vkr_metal_packet_collect_memory(
          renderer, renderer->completion.signaledValue, NULL);
      *instance = (VkrMetalPacketImageInstance){0};
      return false_v;
    }
    instance->views.mip_views[mip] = view;
  }
  if (array_texture) {
    if (image->desc.layers > VKR_METAL_PACKET_MAX_TEXTURE_LAYERS) {
      log_error("Metal graph image '%.*s' layer count %u exceeds capacity %u",
                (int)image->name.length, image->name.str, image->desc.layers,
                VKR_METAL_PACKET_MAX_TEXTURE_LAYERS);
      vkr_metal_packet_release_image_views(&instance->views);
      (void)vkr_metal_memory_device_retire(renderer->memory, resource.handle,
                                           renderer->completion.signaledValue);
      (void)vkr_metal_packet_collect_memory(
          renderer, renderer->completion.signaledValue, NULL);
      *instance = (VkrMetalPacketImageInstance){0};
      return false_v;
    }
    for (uint32_t layer = 0u; layer < image->desc.layers; ++layer) {
      id<MTLTexture> view =
          [texture newTextureViewWithPixelFormat:format
                                     textureType:MTLTextureType2D
                                          levels:NSMakeRange(0u, 1u)
                                          slices:NSMakeRange(layer, 1u)];
      if (!view) {
        log_error("Metal graph image '%.*s' layer %u view creation failed",
                  (int)image->name.length, image->name.str, layer);
        vkr_metal_packet_release_image_views(&instance->views);
        (void)vkr_metal_memory_device_retire(
            renderer->memory, resource.handle,
            renderer->completion.signaledValue);
        (void)vkr_metal_packet_collect_memory(
            renderer, renderer->completion.signaledValue, NULL);
        *instance = (VkrMetalPacketImageInstance){0};
        return false_v;
      }
      instance->views.layer_views[layer] = view;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_wait(VkrMetalPacketRenderer *renderer,
                                           uint64_t value);

vkr_internal bool8_t vkr_metal_packet_retire_graph_buffer_instances(
    VkrMetalPacketRenderer *renderer,
    VkrMetalPacketGraphBufferInstance *instances, uint32_t instance_count);

/* Allocation recovery keeps the requested extent. Wait for submitted work,
 * including detached old generations from partial realization, then retire
 * mismatched owned targets before allocating replacements. Unchanged retained
 * images remain available; no live resource moves between heaps. */
vkr_internal bool8_t vkr_metal_packet_reclaim_graph_targets(
    VkrMetalPacketRenderer *renderer) {
  if (renderer->graph->images.length > renderer->max_images ||
      renderer->graph->buffers.length > renderer->max_images)
    return false_v;
  if (renderer->submit_value > renderer->completion.signaledValue &&
      !vkr_metal_packet_wait(renderer, renderer->submit_value))
    return false_v;
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image = array_get_VkrRgImage(&renderer->graph->images, i);
    VkrMetalPacketImage *slot = &renderer->images[i];
    if (!image->declared_this_frame || image->imported || !slot->live ||
        slot->external || vkr_metal_packet_image_desc_equal(&slot->desc,
                                                            &image->desc))
      continue;
    if (!vkr_metal_packet_retire_image_instances(renderer, slot->instances,
                                                 slot->instance_count))
      return false_v;
    *slot = (VkrMetalPacketImage){0};
    renderer->resize_count++;
  }
  for (uint64_t i = 0u; i < renderer->graph->buffers.length; ++i) {
    const VkrRgBuffer *buffer =
        array_get_VkrRgBuffer(&renderer->graph->buffers, i);
    VkrMetalPacketGraphBuffer *slot = &renderer->graph_buffers[i];
    if (!buffer->declared_this_frame || buffer->imported || !slot->live ||
        (buffer->desc.flags & VKR_RG_RESOURCE_FLAG_GROW_ONLY) ||
        buffer->desc.size == slot->desc.size)
      continue;
    VkrMetalPacketGraphBufferInstance *instances =
        &renderer->graph_buffer_instances[i * renderer->history_instance_count];
    if (!vkr_metal_packet_retire_graph_buffer_instances(renderer, instances,
                                                        slot->instance_count))
      return false_v;
    *slot = (VkrMetalPacketGraphBuffer){0};
  }
  if (vkr_metal_packet_collect_memory(
          renderer, renderer->completion.signaledValue, NULL) !=
      VKR_METAL_MEMORY_STATUS_OK)
    return false_v;
  renderer->graph_memory_recovery_pending = false_v;
  return true_v;
}

vkr_internal bool8_t
vkr_metal_packet_realize_images(
    VkrMetalPacketRenderer *renderer, VkrRendererError *out_error,
    VkrMetalPacketImageAllocationFailure *out_failure) {
  *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  *out_failure = (VkrMetalPacketImageAllocationFailure){0};
  if (renderer->graph->images.length > renderer->max_images) {
    log_error("Metal graph image count %llu exceeds capacity %u",
              (unsigned long long)renderer->graph->images.length,
              renderer->max_images);
    return false_v;
  }
  for (uint64_t i = 0; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        array_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame)
      continue;
    VkrMetalPacketImage *slot = &renderer->images[i];
    const bool8_t external_drawable =
        renderer->target_kind == VKR_METAL_PACKET_TARGET_WINDOW &&
        image->imported && vkr_string8_equals_cstr(&image->name, "swapchain");
    const VkrRgResourceInstanceDomain domain =
        vkr_rg_resource_instance_domain(image->desc.flags);
    const uint32_t instance_count =
        external_drawable || domain == VKR_RG_RESOURCE_INSTANCE_SINGLE ? 1u
        : (image->desc.flags & VKR_RG_RESOURCE_FLAG_HISTORY) != 0u
            ? renderer->history_instance_count
            : renderer->command_slot_count;
    if (instance_count == 0u ||
        instance_count > VKR_METAL_PACKET_GRAPH_INSTANCE_MAX) {
      log_error("Metal graph image '%.*s' instance count %u exceeds capacity "
                "%u",
                (int)image->name.length, image->name.str, instance_count,
                VKR_METAL_PACKET_GRAPH_INSTANCE_MAX);
      return false_v;
    }
    if (slot->live && slot->graph_generation == image->generation &&
        slot->instance_count == instance_count &&
        slot->external == external_drawable &&
        vkr_metal_packet_image_desc_equal(&slot->desc, &image->desc)) {
      if (external_drawable) {
        if (!renderer->drawable || !renderer->drawable.texture) {
          log_error("Metal graph drawable is unavailable during reuse");
          return false_v;
        }
        slot->instances[0].resource = (VkrMetalTextureResource){
            .metal_texture = renderer->drawable.texture,
            .resource_id = renderer->drawable.texture.gpuResourceID._impl,
        };
      }
      continue;
    }
    VkrMetalPacketImage replacement = {
        .desc = image->desc,
        .graph_generation = image->generation,
        .instance_count = instance_count,
        .live = true_v,
        .external = external_drawable,
    };
    if (external_drawable) {
      if (!renderer->drawable || !renderer->drawable.texture) {
        log_error("Metal graph drawable is unavailable during realization");
        return false_v;
      }
      replacement.instances[0] = (VkrMetalPacketImageInstance){
          .resource =
              {
                  .metal_texture = renderer->drawable.texture,
                  .resource_id = renderer->drawable.texture.gpuResourceID._impl,
              },
          .live = true_v,
          .owned = false_v,
      };
    } else {
      for (uint32_t instance = 0u; instance < instance_count; ++instance) {
        if (!vkr_metal_packet_create_image_instance(
                renderer, image, &replacement.instances[instance], out_error,
                out_failure)) {
          (void)vkr_metal_packet_retire_image_instances(
              renderer, replacement.instances, instance);
          return false_v;
        }
      }
    }
    if (slot->live && !vkr_metal_packet_retire_image_instances(
                          renderer, slot->instances, slot->instance_count)) {
      log_error("Metal graph image '%.*s' replacement retirement failed",
                (int)image->name.length, image->name.str);
      (void)vkr_metal_packet_retire_image_instances(
          renderer, replacement.instances, replacement.instance_count);
      return false_v;
    }
    if (slot->live)
      renderer->resize_count++;
    *slot = replacement;
  }
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal VkrMetalPacketImageInstance *
vkr_metal_packet_image_instance(VkrMetalPacketRenderer *renderer,
                                VkrRgImageHandle handle) {
  if (!vkr_rg_image_handle_valid(handle) || handle.id > renderer->max_images)
    return NULL;
  VkrMetalPacketImage *slot = &renderer->images[handle.id - 1u];
  if (!slot->live || slot->graph_generation != handle.generation ||
      slot->instance_count == 0u)
    return NULL;
  uint32_t instance = 0u;
  const VkrRgResourceInstanceDomain domain =
      vkr_rg_resource_instance_domain(slot->desc.flags);
  if (!slot->external && domain == VKR_RG_RESOURCE_INSTANCE_PER_IMAGE)
    instance = renderer->prepared_frame.image_index % slot->instance_count;
  else if (!slot->external &&
           (slot->desc.flags & VKR_RG_RESOURCE_FLAG_HISTORY) != 0u)
    instance = renderer->history_output_index;
  else if (!slot->external && domain == VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT)
    instance =
        (uint32_t)(renderer->active_command_slot - renderer->command_slots);
  return instance < slot->instance_count && slot->instances[instance].live
             ? &slot->instances[instance]
             : NULL;
}

vkr_internal id<MTLTexture>
vkr_metal_packet_texture(VkrMetalPacketRenderer *renderer,
                         VkrRgImageHandle handle) {
  VkrMetalPacketImageInstance *instance =
      vkr_metal_packet_image_instance(renderer, handle);
  return instance ? (id<MTLTexture>)instance->resource.metal_texture : nil;
}

vkr_internal id<MTLTexture>
vkr_metal_packet_mip_texture(VkrMetalPacketRenderer *renderer,
                             VkrRgImageHandle handle, uint32_t mip) {
  VkrMetalPacketImageInstance *instance =
      vkr_metal_packet_image_instance(renderer, handle);
  return instance && mip < VKR_METAL_PACKET_MAX_TEXTURE_MIPS &&
                 instance->views.mip_views[mip]
             ? (id<MTLTexture>)instance->views.mip_views[mip]
             : nil;
}

vkr_internal VkrMetalPacketImage *
vkr_metal_packet_named_image_slot(VkrMetalPacketRenderer *renderer,
                                  const char *name) {
  if (!renderer || !name)
    return NULL;
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        array_get_VkrRgImage(&renderer->graph->images, i);
    if (image && image->declared_this_frame &&
        vkr_string8_equals_cstr(&image->name, name))
      return &renderer->images[i];
  }
  return NULL;
}

vkr_internal bool8_t
vkr_metal_packet_select_history_output(VkrMetalPacketRenderer *renderer) {
  uint64_t candidate_use[VKR_METAL_PACKET_GRAPH_INSTANCE_MAX] = {0};
  const uint64_t completed = renderer->completion.signaledValue;
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        array_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame ||
        (image->desc.flags & VKR_RG_RESOURCE_FLAG_HISTORY) == 0u)
      continue;
    VkrMetalPacketImage *slot = &renderer->images[i];
    if (!slot->live || slot->instance_count != renderer->history_instance_count)
      return false_v;
    for (uint32_t candidate = 0u; candidate < renderer->history_instance_count;
         ++candidate) {
      const VkrMetalPacketImageInstance *instance = &slot->instances[candidate];
      if (!instance->live)
        return false_v;
      const uint64_t use = MAX(instance->last_use_submit_value,
                               instance->history_producer_submit_value);
      candidate_use[candidate] = MAX(candidate_use[candidate], use);
    }
  }

  for (uint64_t i = 0u; i < renderer->graph->buffers.length; ++i) {
    const VkrRgBuffer *buffer =
        array_get_VkrRgBuffer(&renderer->graph->buffers, i);
    if (!buffer || !buffer->declared_this_frame ||
        (buffer->desc.flags & VKR_RG_RESOURCE_FLAG_HISTORY) == 0u)
      continue;
    VkrMetalPacketGraphBuffer *slot = &renderer->graph_buffers[i];
    if (!slot->live || slot->instance_count != renderer->history_instance_count)
      return false_v;
    VkrMetalPacketGraphBufferInstance *instances =
        &renderer->graph_buffer_instances[i * renderer->history_instance_count];
    for (uint32_t candidate = 0u; candidate < renderer->history_instance_count;
         ++candidate) {
      const VkrMetalPacketGraphBufferInstance *instance = &instances[candidate];
      if (!instance->live)
        return false_v;
      const uint64_t use = MAX(instance->last_use_submit_value,
                               instance->history_producer_submit_value);
      candidate_use[candidate] = MAX(candidate_use[candidate], use);
    }
  }

  uint32_t selected = renderer->history_instance_count;
  uint64_t selected_use = UINT64_MAX;
  for (uint32_t candidate = 0u; candidate < renderer->history_instance_count;
       ++candidate) {
    if (candidate_use[candidate] < selected_use) {
      selected = candidate;
      selected_use = candidate_use[candidate];
    }
  }
  if (selected == renderer->history_instance_count)
    return false_v;
  // Reuse the oldest combined last use after one completion wait when every
  // index in the fixed history ring is still referenced by a submission.
  if (selected_use > completed &&
      (selected_use > renderer->submit_value ||
       !vkr_metal_packet_wait(renderer, selected_use))) {
    log_error("Metal history output wait failed (instance=%u, submit=%llu)",
              selected, (unsigned long long)selected_use);
    return false_v;
  }
  renderer->history_output_index = selected;
  return true_v;
}

vkr_internal bool8_t
vkr_metal_packet_select_hzb_history(VkrMetalPacketRenderer *renderer) {
  if (!renderer || !renderer->active_command_slot)
    return false_v;
  renderer->selected_hzb_history_instance = UINT32_MAX;
  renderer->prepared_frame.hzb_history_valid = false_v;
  renderer->packet_build.hzb_history_checks_valid = true_v;
  if (!renderer->prepared_frame.hzb_build_enabled ||
      (renderer->graph->packet->input.world &&
       renderer->graph->packet->input.world->skinning_count)) {
    renderer->packet_build.hzb_history_rejections[VKR_HZB_HISTORY_DISABLED]++;
    return true_v;
  }
  VkrMetalPacketImage *slot =
      vkr_metal_packet_named_image_slot(renderer, "hzb_history");
  if (!slot || slot->instance_count == 0u)
    return true_v;
  const uint32_t current = renderer->history_output_index;
  const uint64_t completed = renderer->completion.signaledValue;
  uint64_t newest_producer = 0u;
  for (uint32_t i = 0u; i < slot->instance_count; ++i) {
    const VkrMetalPacketImageInstance *instance = &slot->instances[i];
    if (i == current)
      continue;
    VkrHzbHistoryRejection rejection = VKR_HZB_HISTORY_REJECTION_COUNT;
    if (!instance->live || !instance->history_valid)
      rejection = VKR_HZB_HISTORY_INVALID;
    else if (instance->history_producer_submit_value > completed)
      rejection = VKR_HZB_HISTORY_INCOMPLETE;
    else if (instance->history_world_epoch != renderer->current_hzb_world_epoch)
      rejection = VKR_HZB_HISTORY_WORLD_CHANGED;
    else if (instance->history_width != renderer->prepared_frame.viewport_width ||
             instance->history_height != renderer->prepared_frame.viewport_height)
      rejection = VKR_HZB_HISTORY_EXTENT_CHANGED;
    else if (MemCompare(&instance->history_view_projection,
                        &renderer->current_hzb_view_projection, sizeof(Mat4)) != 0)
      rejection = VKR_HZB_HISTORY_CAMERA_CHANGED;
    /* Another jitter phase does not prove coverage at the current sample. */
    else if (MemCompare(&instance->history_raster_view_projection,
                        &renderer->current_hzb_raster_view_projection,
                        sizeof(Mat4)) != 0)
      rejection = VKR_HZB_HISTORY_RASTER_CHANGED;
    if (rejection != VKR_HZB_HISTORY_REJECTION_COUNT) {
      renderer->packet_build.hzb_history_rejections[rejection]++;
      continue;
    }
    if (renderer->selected_hzb_history_instance == UINT32_MAX ||
        instance->history_producer_submit_value > newest_producer) {
      renderer->selected_hzb_history_instance = i;
      newest_producer = instance->history_producer_submit_value;
    }
  }
  renderer->prepared_frame.hzb_history_valid =
      renderer->selected_hzb_history_instance != UINT32_MAX;
  return true_v;
}

vkr_internal const VkrMetalPacketImageInstance *
vkr_metal_packet_selected_hzb_instance(VkrMetalPacketRenderer *renderer) {
  VkrMetalPacketImage *slot =
      vkr_metal_packet_named_image_slot(renderer, "hzb_history");
  const uint32_t selected = renderer->selected_hzb_history_instance;
  return slot && selected < slot->instance_count &&
                 slot->instances[selected].live
             ? &slot->instances[selected]
             : NULL;
}

vkr_internal VkrMetalPacketGraphBufferInstance *
vkr_metal_packet_graph_buffer_instance(VkrMetalPacketRenderer *renderer,
                                       VkrRgBufferHandle handle,
                                       uint32_t instance) {
  if (!vkr_rg_buffer_handle_valid(handle) || handle.id > renderer->max_images)
    return NULL;
  VkrMetalPacketGraphBuffer *slot = &renderer->graph_buffers[handle.id - 1u];
  if (!slot->live || slot->graph_generation != handle.generation ||
      instance >= slot->instance_count)
    return NULL;
  VkrMetalPacketGraphBufferInstance *result =
      &renderer->graph_buffer_instances[(uint64_t)(handle.id - 1u) *
                                            renderer->history_instance_count +
                                        instance];
  return result->live ? result : NULL;
}

/* Cloud radiance history holds camera-independent sky radiance; its stored
   view-projection reprojects it (ADR-074). The tiled pipeline has no temporal
   antialiasing, so the newest completed producer of the same traced layer
   and grid seeds the trace. */
vkr_internal bool8_t
vkr_metal_packet_select_cloud_history(VkrMetalPacketRenderer *renderer) {
  renderer->selected_cloud_history_instance = UINT32_MAX;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  if (!packet || !renderer->prepared_frame.clouds_enabled)
    return true_v;
  VkrMetalPacketImage *history =
      vkr_metal_packet_named_image_slot(renderer, "cloud_history");
  if (!history || history->instance_count != renderer->history_instance_count ||
      history->desc.type != VKR_TEXTURE_TYPE_2D ||
      renderer->history_output_index >= history->instance_count)
    return false_v;
  if (packet->temporal.reset_reasons != VKR_TEMPORAL_RESET_NONE)
    return true_v;

  const uint64_t signature = vkr_cloud_history_signature(
      &packet->sky.clouds, packet->sky.camera_position.w);
  const uint64_t completed = renderer->completion.signaledValue;
  const uint64_t frame_index = packet->input.frame.frame_index;
  uint64_t newest_submit = 0u;
  for (uint32_t i = 0u; i < history->instance_count; ++i) {
    const VkrMetalPacketCloudHistoryState *candidate =
        &renderer->cloud_history[i];
    const VkrMetalPacketImageInstance *image = &history->instances[i];
    const bool8_t source_age_in_ring =
        candidate->frame_index < frame_index &&
        frame_index - candidate->frame_index <= history->instance_count;
    if (i == renderer->history_output_index || !candidate->valid ||
        !image->live || !image->history_valid || !source_age_in_ring ||
        candidate->producer_submit_value > completed ||
        image->history_producer_submit_value !=
            candidate->producer_submit_value ||
        candidate->signature != signature ||
        candidate->graph_generation != history->graph_generation ||
        candidate->width != history->desc.width ||
        candidate->height != history->desc.height)
      continue;
    if (renderer->selected_cloud_history_instance == UINT32_MAX ||
        candidate->producer_submit_value > newest_submit) {
      renderer->selected_cloud_history_instance = i;
      newest_submit = candidate->producer_submit_value;
    }
  }
  return true_v;
}

vkr_internal bool8_t
vkr_metal_packet_select_exposure_history(VkrMetalPacketRenderer *renderer) {
  renderer->selected_exposure_history_instance = UINT32_MAX;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  if (!packet || !renderer->prepared_frame.exposure_automatic ||
      !packet->exposure.history_valid)
    return true_v;
  if (!vkr_rg_buffer_handle_valid(renderer->exposure_state_handle))
    return false_v;
  VkrMetalPacketGraphBuffer *states =
      &renderer->graph_buffers[renderer->exposure_state_handle.id - 1u];
  const uint32_t current = renderer->history_output_index;
  const uint64_t completed = renderer->completion.signaledValue;
  uint64_t newest_producer = 0u;
  for (uint32_t i = 0u; i < states->instance_count; ++i) {
    VkrMetalPacketGraphBufferInstance *candidate =
        vkr_metal_packet_graph_buffer_instance(
            renderer, renderer->exposure_state_handle, i);
    if (i == current || !candidate || !candidate->history_valid ||
        candidate->history_producer_submit_value > completed ||
        candidate->history_scene_generation !=
            packet->input.frame.scene_generation)
      continue;
    if (renderer->selected_exposure_history_instance == UINT32_MAX ||
        candidate->history_producer_submit_value > newest_producer) {
      renderer->selected_exposure_history_instance = i;
      newest_producer = candidate->history_producer_submit_value;
    }
  }
  return true_v;
}

vkr_internal VkrMetalPacketGraphBufferInstance *
vkr_metal_packet_graph_buffer(VkrMetalPacketRenderer *renderer,
                              VkrRgBufferHandle handle) {
  if (!vkr_rg_buffer_handle_valid(handle) || handle.id > renderer->max_images)
    return NULL;
  VkrMetalPacketGraphBuffer *slot = &renderer->graph_buffers[handle.id - 1u];
  if (!slot->live || slot->graph_generation != handle.generation)
    return NULL;
  uint32_t instance = 0u;
  const VkrRgResourceInstanceDomain domain =
      vkr_rg_resource_instance_domain(slot->desc.flags);
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_IMAGE)
    instance = renderer->prepared_frame.image_index % slot->instance_count;
  else if ((slot->desc.flags & VKR_RG_RESOURCE_FLAG_HISTORY) != 0u)
    instance = renderer->history_output_index;
  else if (domain == VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT)
    instance =
        (uint32_t)(renderer->active_command_slot - renderer->command_slots);
  if (instance >= slot->instance_count)
    return NULL;
  VkrMetalPacketGraphBufferInstance *result =
      &renderer->graph_buffer_instances[(uint64_t)(handle.id - 1u) *
                                            renderer->history_instance_count +
                                        instance];
  return result->live ? result : NULL;
}

vkr_internal bool8_t vkr_metal_packet_retire_graph_buffer_instances(
    VkrMetalPacketRenderer *renderer,
    VkrMetalPacketGraphBufferInstance *instances, uint32_t instance_count) {
  if (!renderer || (!instances && instance_count > 0u))
    return false_v;
  for (uint32_t i = 0u; i < instance_count; ++i) {
    VkrMetalPacketGraphBufferInstance *instance = &instances[i];
    if (instance->live &&
        vkr_metal_memory_device_retire(
            renderer->memory, instance->resource.handle,
            instance->last_use_submit_value) != VKR_METAL_MEMORY_STATUS_OK)
      return false_v;
    *instance = (VkrMetalPacketGraphBufferInstance){0};
  }
  return true_v;
}

vkr_internal bool8_t
vkr_metal_packet_realize_buffers(VkrMetalPacketRenderer *renderer,
                                  VkrRendererError *out_error) {
  if (renderer->graph->buffers.length > renderer->max_images) {
    log_error("Metal graph buffer count %llu exceeds capacity %u",
              (unsigned long long)renderer->graph->buffers.length,
              renderer->max_images);
    return false_v;
  }
  renderer->gpu_candidate_buffer_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  renderer->gpu_candidate_instance_buffer_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  renderer->skinning_output_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  renderer->exposure_state_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  for (uint64_t i = 0u; i < renderer->graph->buffers.length; ++i) {
    const VkrRgBuffer *buffer =
        array_get_VkrRgBuffer(&renderer->graph->buffers, i);
    if (!buffer || !buffer->declared_this_frame)
      continue;
    const VkrRgBufferHandle handle = {
        .id = (uint32_t)i + 1u,
        .generation = buffer->generation,
    };
    if (vkr_string8_equals_cstr(&buffer->name, "gpu_draw_candidates"))
      renderer->gpu_candidate_buffer_handle = handle;
    else if (vkr_string8_equals_cstr(&buffer->name, "gpu_draw_instances"))
      renderer->gpu_candidate_instance_buffer_handle = handle;
    else if (vkr_string8_equals_cstr(&buffer->name, "skinning_output"))
      renderer->skinning_output_handle = handle;
    else if (vkr_string8_equals_cstr(&buffer->name, "exposure_state"))
      renderer->exposure_state_handle = handle;
    if (buffer->imported ||
        (buffer->desc.flags & VKR_RG_RESOURCE_FLAG_EXTERNAL)) {
      log_error("Metal graph buffer '%.*s' cannot be external",
                (int)buffer->name.length, buffer->name.str);
      return false_v;
    }
    const VkrRgResourceInstanceDomain domain =
        vkr_rg_resource_instance_domain(buffer->desc.flags);
    const uint32_t instance_count =
        domain == VKR_RG_RESOURCE_INSTANCE_SINGLE ? 1u
        : (buffer->desc.flags & VKR_RG_RESOURCE_FLAG_HISTORY) != 0u
            ? renderer->history_instance_count
            : renderer->command_slot_count;
    VkrMetalPacketGraphBuffer *slot = &renderer->graph_buffers[i];
    const bool8_t grow_only =
        (buffer->desc.flags & VKR_RG_RESOURCE_FLAG_GROW_ONLY) != 0u;
    if (slot->live &&
        (slot->graph_generation == buffer->generation || grow_only) &&
        slot->instance_count == instance_count &&
        (grow_only ? slot->desc.size >= buffer->desc.size
                   : slot->desc.size == buffer->desc.size) &&
        slot->desc.usage.set == buffer->desc.usage.set &&
        slot->desc.flags == buffer->desc.flags) {
      slot->graph_generation = buffer->generation;
      continue;
    }
    VkrMetalPacketGraphBufferInstance *instances =
        &renderer->graph_buffer_instances[i * renderer->history_instance_count];
    if (slot->live && grow_only) {
      uint64_t last_use = 0u;
      for (uint32_t instance = 0u; instance < slot->instance_count; ++instance)
        last_use = Max(last_use, instances[instance].last_use_submit_value);
      if (last_use > renderer->completion.signaledValue &&
          !vkr_metal_packet_wait(renderer, last_use)) {
        if (out_error)
          *out_error = VKR_RENDERER_ERROR_DEVICE_ERROR;
        return false_v;
      }
      if (!vkr_metal_packet_retire_graph_buffer_instances(
              renderer, instances, slot->instance_count))
        return false_v;
      *slot = (VkrMetalPacketGraphBuffer){0};
      if (vkr_metal_packet_collect_memory(
              renderer, renderer->completion.signaledValue, NULL) !=
          VKR_METAL_MEMORY_STATUS_OK)
        return false_v;
    }
    VkrMetalPacketGraphBuffer replacement = {
        .desc = buffer->desc,
        .graph_generation = buffer->generation,
        .instance_count = instance_count,
        .live = true_v,
    };
    VkrMetalPacketGraphBufferInstance
        replacement_instances[VKR_METAL_PACKET_GRAPH_INSTANCE_MAX] = {0};
    for (uint32_t instance = 0u; instance < instance_count; ++instance) {
      const VkrMetalMemoryStatus status = vkr_metal_memory_device_create_buffer(
          renderer->memory, buffer->desc.size,
          VKR_GPU_ALLOCATION_OWNER_RENDER_GRAPH,
          &replacement_instances[instance].resource);
      if (status != VKR_METAL_MEMORY_STATUS_OK) {
        *out_error = vkr_metal_packet_allocation_error(status);
        log_error("Metal graph buffer '%.*s' instance %u allocation of %llu "
                  "bytes failed: %s",
                  (int)buffer->name.length, buffer->name.str, instance,
                  (unsigned long long)buffer->desc.size,
                  vkr_metal_memory_status_string(status));
        (void)vkr_metal_packet_retire_graph_buffer_instances(
            renderer, replacement_instances, instance);
        return false_v;
      }
      replacement_instances[instance].live = true_v;
    }
    if (slot->live && !vkr_metal_packet_retire_graph_buffer_instances(
                          renderer, instances, slot->instance_count)) {
      log_error("Metal graph buffer '%.*s' replacement retirement failed",
                (int)buffer->name.length, buffer->name.str);
      (void)vkr_metal_packet_retire_graph_buffer_instances(
          renderer, replacement_instances, replacement.instance_count);
      return false_v;
    }
    *slot = replacement;
    MemCopy(instances, replacement_instances,
            (uint64_t)replacement.instance_count * sizeof(*instances));
  }
  return true_v;
}

/**
 * Resolves one retained subresource slot, or NULL when the request cannot name
 * a live instance. Shared by the read and commit halves so both agree on
 * bounds and liveness, mirroring the Vulkan implementation.
 */
vkr_internal VkrRgRetainedState *
vkr_metal_packet_retained_slot(VkrMetalPacketRenderer *renderer,
                               uint32_t image_index, uint32_t instance_index,
                               uint32_t subresource) {
  if (image_index >= renderer->graph->images.length)
    return NULL;
  const VkrRgImage *image =
      array_get_VkrRgImage(&renderer->graph->images, image_index);
  VkrMetalPacketImage *slot = &renderer->images[image_index];
  if (!image || !slot->live || slot->graph_generation != image->generation ||
      !vkr_metal_packet_image_desc_equal(&slot->desc, &image->desc) ||
      instance_index >= slot->instance_count)
    return NULL;
  if (subresource >=
      ArrayCount(slot->instances[instance_index].retained_states))
    return NULL;
  return &slot->instances[instance_index].retained_states[subresource];
}

vkr_internal void
vkr_metal_packet_retained_read(void *context, uint32_t image_index,
                               uint32_t instance_index, uint32_t subresource,
                               VkrRgRetainedState *out_state) {
  const VkrRgRetainedState *state =
      vkr_metal_packet_retained_slot((VkrMetalPacketRenderer *)context,
                                     image_index, instance_index, subresource);
  /* An unresolvable slot leaves the caller's invalid default in place, which
     the compiler reads as "must be written this frame". */
  if (state)
    *out_state = *state;
}

vkr_internal void
vkr_metal_packet_retained_commit(void *context, uint32_t image_index,
                                 uint32_t instance_index, uint32_t subresource,
                                 const VkrRgRetainedState *state) {
  VkrRgRetainedState *slot =
      vkr_metal_packet_retained_slot((VkrMetalPacketRenderer *)context,
                                     image_index, instance_index, subresource);
  if (slot)
    *slot = *state;
}

vkr_internal void
vkr_metal_packet_install_retained_provider(VkrMetalPacketRenderer *renderer) {
  const VkrRgRetainedStateProvider provider = {
      .context = renderer,
      .read = vkr_metal_packet_retained_read,
      .commit = vkr_metal_packet_retained_commit,
  };
  vkr_rg_set_retained_state_provider(renderer->graph, &provider);
}

void vkr_metal_packet_renderer_retained_editor_extent(
    VkrMetalPacketRenderer *renderer, uint32_t *out_width, uint32_t *out_height) {
  *out_width = 0u;
  *out_height = 0u;
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        array_get_VkrRgImage(&renderer->graph->images, i);
    if (!vkr_string8_equals_cstr(&image->name, "editor_scene_image"))
      continue;
    const VkrMetalPacketImage *slot = &renderer->images[i];
    if (!slot->live || slot->graph_generation != image->generation ||
        slot->instance_count != 1u || slot->desc.mip_levels != 1u ||
        slot->desc.layers != 1u || !slot->instances[0].live ||
        !slot->instances[0].resource.metal_texture ||
        !slot->instances[0].retained_states[0].content_valid)
      return;
    *out_width = slot->desc.width;
    *out_height = slot->desc.height;
    return;
  }
}

void vkr_metal_packet_renderer_retained_shadow_token(
    VkrMetalPacketRenderer *renderer, uint32_t image_index,
    VkrRetainedShadowToken *out_token) {
  *out_token = (VkrRetainedShadowToken){0};
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        array_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !vkr_string8_equals_cstr(&image->name, "shadow_map"))
      continue;
    VkrMetalPacketImage *slot = &renderer->images[i];
    if (!slot->live || slot->graph_generation != image->generation ||
        slot->instance_count != renderer->command_slot_count ||
        image_index >= slot->instance_count || slot->desc.mip_levels != 1u ||
        slot->desc.samples != VKR_SAMPLE_COUNT_1 ||
        slot->desc.type != VKR_TEXTURE_TYPE_2D ||
        slot->desc.width != renderer->prepared_frame.shadow_map_size ||
        slot->desc.height != renderer->prepared_frame.shadow_map_size ||
        slot->desc.format != renderer->prepared_frame.shadow_depth_format ||
        slot->desc.layers != renderer->prepared_frame.shadow_map_layer_count)
      return;
    const VkrMetalPacketImageInstance *instance = &slot->instances[image_index];
    if (!instance->live)
      return;
    out_token->resource_generation = slot->graph_generation;
    const uint32_t layer_count = MIN(slot->desc.layers, 32u);
    for (uint32_t layer = 0u; layer < layer_count; ++layer) {
      if (instance->retained_states[layer].content_valid)
        out_token->valid_layer_mask |= UINT32_C(1) << layer;
    }
    return;
  }
}

/* Every frame in flight shares one D16 atlas, which reports its own layer
   count. The tiled pipeline draws no transmission layers (ADR-087). */
void vkr_metal_packet_renderer_retained_local_shadow_token(
    VkrMetalPacketRenderer *renderer, VkrRetainedLocalShadowToken *out_token) {
  *out_token = (VkrRetainedLocalShadowToken){0};
  VkrMetalPacketImage *slot = NULL;
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image = array_get_VkrRgImage(&renderer->graph->images, i);
    if (vkr_string8_equals_cstr(&image->name, "local_shadow_map")) {
      VkrMetalPacketImage *candidate = &renderer->images[i];
      if (candidate->graph_generation == image->generation)
        slot = candidate;
      break;
    }
  }
  if (!slot || !slot->live || slot->graph_generation == 0u ||
      slot->instance_count != 1u || slot->desc.mip_levels != 1u ||
      slot->desc.samples != VKR_SAMPLE_COUNT_1 ||
      slot->desc.type != VKR_TEXTURE_TYPE_2D ||
      slot->desc.width != VKR_LOCAL_SHADOW_ATLAS_SIZE ||
      slot->desc.height != VKR_LOCAL_SHADOW_ATLAS_SIZE ||
      slot->desc.format != VKR_TEXTURE_FORMAT_D16_UNORM ||
      slot->desc.layers > VKR_LOCAL_SHADOW_ATLAS_LAYER_COUNT_MAX)
    return;
  const VkrMetalPacketImageInstance *instance = &slot->instances[0];
  if (!instance->live || !instance->resource.metal_texture)
    return;
  uint64_t valid_layers = 0u;
  for (uint32_t layer = 0u; layer < MIN(slot->desc.layers, 64u); ++layer) {
    if (instance->retained_states[layer].content_valid)
      valid_layers |= UINT64_C(1) << layer;
  }
  out_token->resource_generation = slot->graph_generation;
  out_token->atlas_layer_count = slot->desc.layers;
  out_token->valid_layer_mask = valid_layers;
}

vkr_internal void
vkr_metal_packet_mark_graph_images_submitted(VkrMetalPacketRenderer *renderer,
                                             uint64_t submit_value) {
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        array_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame)
      continue;
    VkrMetalPacketImageInstance *instance = vkr_metal_packet_image_instance(
        renderer, (VkrRgImageHandle){.id = (uint32_t)i + 1u,
                                     .generation = image->generation});
    if (instance && instance->owned)
      instance->last_use_submit_value = submit_value;
  }
}

vkr_internal void
vkr_metal_packet_mark_hzb_history_submitted(VkrMetalPacketRenderer *renderer,
                                            uint64_t submit_value) {
  VkrMetalPacketImage *slot =
      vkr_metal_packet_named_image_slot(renderer, "hzb_history");
  if (!slot || !renderer->active_command_slot)
    return;
  const uint32_t current = renderer->history_output_index;
  if (renderer->prepared_frame.hzb_build_enabled &&
      current < slot->instance_count) {
    VkrMetalPacketImageInstance *instance = &slot->instances[current];
    instance->history_producer_submit_value = submit_value;
    instance->history_world_epoch = renderer->current_hzb_world_epoch;
    instance->history_view_projection = renderer->current_hzb_view_projection;
    instance->history_raster_view_projection =
        renderer->current_hzb_raster_view_projection;
    instance->history_width = renderer->prepared_frame.viewport_width;
    instance->history_height = renderer->prepared_frame.viewport_height;
    instance->history_valid = true_v;
  }
  const uint32_t selected = renderer->selected_hzb_history_instance;
  if (selected < slot->instance_count) {
    VkrMetalPacketImageInstance *instance = &slot->instances[selected];
    instance->last_use_submit_value =
        MAX(instance->last_use_submit_value, submit_value);
  }
}

vkr_internal void
vkr_metal_packet_mark_cloud_history_submitted(VkrMetalPacketRenderer *renderer,
                                              uint64_t submit_value) {
  const VkrPreparedFrame *packet = renderer->graph->packet;
  if (!packet || !renderer->prepared_frame.clouds_enabled)
    return;
  VkrMetalPacketImage *history =
      vkr_metal_packet_named_image_slot(renderer, "cloud_history");
  const uint32_t current = renderer->history_output_index;
  if (!history || history->instance_count != renderer->history_instance_count ||
      current >= history->instance_count)
    return;
  if (packet->temporal.reset_reasons != VKR_TEMPORAL_RESET_NONE) {
    for (uint32_t i = 0u; i < history->instance_count; ++i)
      renderer->cloud_history[i].valid = false_v;
  }

  renderer->cloud_history[current] = (VkrMetalPacketCloudHistoryState){
      .view_projection = packet->sky.view_projection,
      .signature = vkr_cloud_history_signature(&packet->sky.clouds,
                                               packet->sky.camera_position.w),
      .producer_submit_value = submit_value,
      .frame_index = packet->input.frame.frame_index,
      .width = history->desc.width,
      .height = history->desc.height,
      .graph_generation = history->graph_generation,
      .valid = true_v,
  };
  VkrMetalPacketImageInstance *output = &history->instances[current];
  output->history_producer_submit_value = submit_value;
  output->history_valid = true_v;

  const uint32_t selected = renderer->selected_cloud_history_instance;
  if (selected < history->instance_count)
    history->instances[selected].last_use_submit_value = MAX(
        history->instances[selected].last_use_submit_value, submit_value);
}

vkr_internal void
vkr_metal_packet_mark_exposure_submitted(VkrMetalPacketRenderer *renderer,
                                         uint64_t submit_value) {
  const VkrPreparedFrame *packet = renderer->graph->packet;
  if (!packet || !renderer->prepared_frame.exposure_automatic ||
      !vkr_rg_buffer_handle_valid(renderer->exposure_state_handle))
    return;
  VkrMetalPacketGraphBuffer *states =
      &renderer->graph_buffers[renderer->exposure_state_handle.id - 1u];
  if (packet->exposure.reset_reasons) {
    for (uint32_t i = 0u; i < states->instance_count; ++i) {
      VkrMetalPacketGraphBufferInstance *instance =
          vkr_metal_packet_graph_buffer_instance(
              renderer, renderer->exposure_state_handle, i);
      if (instance)
        instance->history_valid = false_v;
    }
  }
  VkrMetalPacketGraphBufferInstance *current =
      vkr_metal_packet_graph_buffer_instance(renderer,
                                             renderer->exposure_state_handle,
                                             renderer->history_output_index);
  if (current) {
    renderer->exposure_seconds += packet->exposure.delta_seconds;
    current->history_exposure_seconds = renderer->exposure_seconds;
    current->history_producer_submit_value = submit_value;
    current->history_scene_generation = packet->input.frame.scene_generation;
    current->history_valid = true_v;
  }
  VkrMetalPacketGraphBufferInstance *selected =
      vkr_metal_packet_graph_buffer_instance(
          renderer, renderer->exposure_state_handle,
          renderer->selected_exposure_history_instance);
  if (selected)
    selected->last_use_submit_value =
        MAX(selected->last_use_submit_value, submit_value);
}

vkr_internal void
vkr_metal_packet_mark_graph_buffers_submitted(VkrMetalPacketRenderer *renderer,
                                              uint64_t submit_value) {
  for (uint64_t i = 0u; i < renderer->graph->buffers.length; ++i) {
    const VkrRgBuffer *buffer =
        array_get_VkrRgBuffer(&renderer->graph->buffers, i);
    if (!buffer || !buffer->declared_this_frame)
      continue;
    VkrMetalPacketGraphBufferInstance *instance = vkr_metal_packet_graph_buffer(
        renderer, (VkrRgBufferHandle){.id = (uint32_t)i + 1u,
                                      .generation = buffer->generation});
    if (instance)
      instance->last_use_submit_value = submit_value;
  }
}

vkr_internal id<MTLTexture>
vkr_metal_packet_named_texture(VkrMetalPacketRenderer *renderer,
                               const char *name) {
  for (uint64_t i = 0; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        array_get_VkrRgImage(&renderer->graph->images, i);
    if (image && image->declared_this_frame &&
        vkr_string8_equals_cstr(&image->name, name))
      return vkr_metal_packet_texture(
          renderer, (VkrRgImageHandle){.id = (uint32_t)i + 1,
                                       .generation = image->generation});
  }
  return nil;
}

vkr_internal bool8_t vkr_metal_packet_named_texture_info(
    VkrMetalPacketRenderer *renderer, const char *name,
    id<MTLTexture> *out_texture, VkrTextureFormat *out_format) {
  if (!renderer || !name || !out_texture || !out_format)
    return false_v;
  for (uint64_t i = 0; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        array_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame ||
        !vkr_string8_equals_cstr(&image->name, name))
      continue;
    id<MTLTexture> texture = vkr_metal_packet_texture(
        renderer, (VkrRgImageHandle){.id = (uint32_t)i + 1,
                                     .generation = image->generation});
    if (!texture)
      return false_v;
    *out_texture = texture;
    *out_format = image->desc.format;
    return true_v;
  }
  return false_v;
}

vkr_internal uint64_t vkr_metal_packet_align_up(uint64_t value,
                                                uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

vkr_internal bool8_t vkr_metal_packet_material_info_valid(
    const VkrMetalPacketMaterialCreateInfo *create_info) {
  if (!create_info || create_info->alpha_mode > VKR_MATERIAL_ALPHA_BLEND ||
      !isfinite(create_info->alpha_cutoff) ||
      create_info->alpha_cutoff < 0.0f || create_info->alpha_cutoff > 1.0f)
    return false_v;
  const VkrPbrProperties *pbr = &create_info->pbr;
  const float32_t values[] = {
      pbr->base_color.x,
      pbr->base_color.y,
      pbr->base_color.z,
      pbr->base_color.w,
      pbr->metallic,
      pbr->roughness,
      pbr->normal_scale,
      pbr->occlusion_strength,
      pbr->emissive_factor.x,
      pbr->emissive_factor.y,
      pbr->emissive_factor.z,
      pbr->dielectric_specular.x,
      pbr->dielectric_specular.y,
      pbr->dielectric_specular.z,
      pbr->transmission_factor,
      pbr->ior,
      pbr->thickness_factor,
      pbr->attenuation_color.x,
      pbr->attenuation_color.y,
      pbr->attenuation_color.z,
      pbr->attenuation_distance,
      pbr->temporal_reactivity,
      pbr->clearcoat_factor,
      pbr->clearcoat_roughness,
      pbr->clearcoat_normal_scale,
      pbr->anisotropy_strength,
      pbr->anisotropy_rotation,
      pbr->diffuse_transmission_strength,
      pbr->diffuse_transmission_color.x,
      pbr->diffuse_transmission_color.y,
      pbr->diffuse_transmission_color.z,
  };
  for (uint32_t i = 0; i < ArrayCount(values); ++i) {
    if (!isfinite(values[i]))
      return false_v;
  }
  if (pbr->clearcoat_factor < 0.0f || pbr->clearcoat_factor > 1.0f ||
      pbr->clearcoat_roughness < 0.0f || pbr->clearcoat_roughness > 1.0f ||
      pbr->anisotropy_strength < 0.0f || pbr->anisotropy_strength > 1.0f ||
      pbr->diffuse_transmission_strength < 0.0f ||
      pbr->diffuse_transmission_strength > 1.0f ||
      pbr->diffuse_transmission_color.x < 0.0f ||
      pbr->diffuse_transmission_color.x > 1.0f ||
      pbr->diffuse_transmission_color.y < 0.0f ||
      pbr->diffuse_transmission_color.y > 1.0f ||
      pbr->diffuse_transmission_color.z < 0.0f ||
      pbr->diffuse_transmission_color.z > 1.0f ||
      (pbr->anisotropy_strength > 0.0f && pbr->transmission_factor > 0.0f) ||
      (pbr->diffuse_transmission_strength > 0.0f &&
       (create_info->alpha_mode == VKR_MATERIAL_ALPHA_BLEND ||
        pbr->transmission_factor > 0.0f || pbr->thickness_factor > 0.0f)))
    return false_v;
  if (!isfinite(pbr->subsurface_strength) || pbr->subsurface_strength < 0.0f ||
      pbr->subsurface_strength > 1.0f || pbr->subsurface_profile >= VKR_SUBSURFACE_PROFILE_COUNT ||
      (pbr->subsurface_strength > 0.0f &&
       (create_info->alpha_mode == VKR_MATERIAL_ALPHA_BLEND ||
        pbr->transmission_factor > 0.0f || pbr->thickness_factor > 0.0f ||
        pbr->diffuse_transmission_strength > 0.0f)))
    return false_v;
  for (uint32_t i = 0; i < ArrayCount(create_info->tint); ++i) {
    if (!isfinite(create_info->tint[i]))
      return false_v;
  }
  for (uint32_t i = 0; i < ArrayCount(create_info->textures); ++i) {
    const VkrMetalPacketRgba8TextureCreateInfo *texture =
        &create_info->textures[i];
    if (!texture->pixels || texture->width == 0 || texture->height == 0 ||
        (uint64_t)texture->width >
            UINT64_MAX / ((uint64_t)texture->height * 4u))
      return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_capture_plan(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    uint64_t first_offset, VkrMetalPacketCapturePlan *plans,
    uint32_t *out_count, uint64_t *out_size) {
  const VkrCaptureBatchRequest *request =
      packet->input.debug ? packet->input.debug->capture : NULL;
  if (!request) {
    *out_count = 0;
    *out_size = first_offset;
    return true_v;
  }
  if (request->request_id == 0 || !request->items || request->item_count == 0 ||
      request->item_count > VKR_CAPTURE_MAX_ITEMS)
    return false_v;

  uint64_t seen = 0;
  uint64_t offset = first_offset;
  for (uint32_t i = 0; i < request->item_count; ++i) {
    const VkrCaptureItemRequest *item = &request->items[i];
    const VkrCaptureChannelDescription *channel =
        vkr_renderer_capture_channel_get(item->channel);
    if (!channel || item->channel >= 64 || (seen & (1ull << item->channel)) ||
        item->mip != 0 || item->layer != 0)
      return false_v;
    seen |= 1ull << item->channel;

    const char *source_name = channel->source_name;
    uint32_t source_slice = 0;
    /* The tiled pipeline resolves its scene colour without temporal
       reconstruction (ADR-087). */
    if (string_equals(channel->name, "scene_color") ||
        string_equals(channel->name, "hdr_pre_bloom"))
      source_name = "hdr_scene_color";
    else if (string_equals(channel->name, "depth"))
      // The opaque pass resolves its depth into this image, so both editor
      // and fullscreen capture read it.
      source_name = "opaque_vbuffer_depth";
    else if (string_n_equals(channel->name, "shadow_cascade_", 15))
      source_slice = (uint32_t)(channel->name[15] - '0');
    else if (string_equals(channel->name, "picking_ids") &&
             (!packet->input.picking || !packet->input.picking->pending))
      return false_v;

    id<MTLTexture> texture = nil;
    VkrTextureFormat format = VKR_TEXTURE_FORMAT_COUNT;
    if (!vkr_metal_packet_named_texture_info(renderer, source_name, &texture,
                                             &format) ||
        source_slice >= texture.arrayLength)
      return false_v;

    VkrTextureFormatInfo format_info = {0};
    if (!vkr_texture_format_get_info(format, &format_info) ||
        format_info.block_width != 1 || format_info.block_height != 1 ||
        format_info.bytes_per_block == 0)
      return false_v;
    const bool8_t extended_linear_final =
        string_equals(channel->name, "final_color") &&
        format == VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT &&
        renderer->display_output.extended_linear != 0u;
    if (texture.width > UINT32_MAX || texture.height > UINT32_MAX ||
        texture.width > UINT64_MAX / format_info.bytes_per_block)
      return false_v;
    const uint64_t tight_pitch =
        (uint64_t)texture.width * format_info.bytes_per_block;
    if (tight_pitch > UINT64_MAX - 255u)
      return false_v;
    const uint64_t row_pitch = vkr_metal_packet_align_up(tight_pitch, 256);
    if (texture.height > UINT64_MAX / row_pitch ||
        offset > UINT64_MAX - (VKR_CAPTURE_BUFFER_ALIGNMENT - 1u))
      return false_v;
    const uint64_t data_size = row_pitch * texture.height;
    offset = vkr_metal_packet_align_up(offset, VKR_CAPTURE_BUFFER_ALIGNMENT);
    if (data_size == 0 || offset > UINT64_MAX - data_size)
      return false_v;

    plans[i] = (VkrMetalPacketCapturePlan){
        .texture = texture,
        .result = {.channel = item->channel,
                   .width = (uint32_t)texture.width,
                   .height = (uint32_t)texture.height,
                   .row_pitch = row_pitch,
                   .format = format,
                   .value_kind = channel->value_kind,
                   .color_space = extended_linear_final
                                      ? VKR_CAPTURE_COLOR_SPACE_EXTENDED_LINEAR
                                      : channel->color_space,
                   .display_output = extended_linear_final
                                         ? renderer->display_output
                                         : (VkrDisplayOutputParams){0},
                   .origin = VKR_CAPTURE_ORIGIN_TOP_LEFT,
                   .data_size = data_size,
                   .mip = 0,
                   .layer = source_slice,
                   .display_exposure = packet->exposure.manual,
                   .pre_exposure = packet->exposure.pre_exposure},
        .buffer_offset = offset,
        .source_slice = source_slice,
    };
    string_format(plans[i].result.producer_resource,
                  sizeof(plans[i].result.producer_resource), "%s", source_name);
    offset += data_size;
  }
  *out_count = request->item_count;
  *out_size = offset;
  return true_v;
}

vkr_internal void vkr_metal_packet_decode_gpu_draw_diagnostics(
    VkrMetalPacketResult *result, const uint8_t *camera_and_shadow_readback,
    uint32_t shadow_cascade_count, uint32_t gpu_draw_view_count) {
  if (!result || !camera_and_shadow_readback)
    return;
  const VkrGpuDrawCompactionState *camera =
      (const VkrGpuDrawCompactionState *)camera_and_shadow_readback;
  result->gpu_visible_count = camera->visible_count;
  MemCopy(result->gpu_bucket_counts, camera->bucket_counts,
          sizeof(result->gpu_bucket_counts));
  result->gpu_overflow_count = camera->overflow_count;
  result->gpu_resolve_invalid_count = camera->resolve_invalid_count;
  result->gpu_occlusion_culled_count = camera->occlusion_culled_count;
  for (uint32_t cascade = 0u;
       cascade < Min(shadow_cascade_count, VKR_SHADOW_CASCADE_COUNT_MAX);
       ++cascade) {
    const VkrGpuDrawCompactionState *shadow = camera + cascade + 1u;
    result->shadow_gpu_visible_count[cascade] = shadow->visible_count;
    MemCopy(result->shadow_gpu_bucket_counts[cascade], shadow->bucket_counts,
            sizeof(result->shadow_gpu_bucket_counts[cascade]));
    result->shadow_gpu_overflow_count[cascade] = shadow->overflow_count;
    result->shadow_draw_count += shadow->visible_count;
  }
  // Camera and directional counters retain their existing per-view meaning;
  // the aggregate failure counters also cover every local view.
  for (uint32_t view = 1u + shadow_cascade_count; view < gpu_draw_view_count;
       ++view) {
    result->gpu_overflow_count += camera[view].overflow_count;
    result->gpu_resolve_invalid_count += camera[view].resolve_invalid_count;
  }
  result->opaque_draw_count = result->gpu_visible_count;
  result->indexed_draw_count =
      result->opaque_draw_count + result->blend_draw_count;
  result->has_gpu_draw_diagnostics = true_v;
}

vkr_internal void
vkr_metal_packet_capture_fail_reserved(VkrMetalPacketRenderer *renderer,
                                       const VkrCaptureBatchRequest *request,
                                       VkrRendererError error) {
  if (request) {
    (void)vkr_capture_ring_fail(&renderer->capture_ring, request->request_id,
                                error);
  }
}

vkr_internal MTLLoadAction
vkr_metal_packet_load_action(VkrAttachmentLoadOp operation) {
  switch (operation) {
  case VKR_ATTACHMENT_LOAD_OP_LOAD:
    return MTLLoadActionLoad;
  case VKR_ATTACHMENT_LOAD_OP_CLEAR:
    return MTLLoadActionClear;
  case VKR_ATTACHMENT_LOAD_OP_DONT_CARE:
  default:
    return MTLLoadActionDontCare;
  }
}

vkr_internal MTLStoreAction
vkr_metal_packet_store_action(VkrAttachmentStoreOp operation) {
  return operation == VKR_ATTACHMENT_STORE_OP_STORE ? MTLStoreActionStore
                                                    : MTLStoreActionDontCare;
}

vkr_internal bool8_t vkr_metal_packet_configure_render_pass(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, MTL4RenderPassDescriptor *descriptor)
    API_AVAILABLE(macos(26.0)) {
  (void)packet;
  /* Descriptors are reused across passes and frames; the tiled opaque pass
     adds multisample resolves and a tile size that no other pass may
     inherit. */
  for (uint32_t i = 0; i < VKR_METAL_PACKET_MAX_COLOR_ATTACHMENTS; ++i) {
    descriptor.colorAttachments[i].texture = nil;
    descriptor.colorAttachments[i].resolveTexture = nil;
    descriptor.colorAttachments[i].level = 0;
    descriptor.colorAttachments[i].slice = 0;
  }
  descriptor.depthAttachment.texture = nil;
  descriptor.depthAttachment.resolveTexture = nil;
  descriptor.depthAttachment.depthResolveFilter =
      MTLMultisampleDepthResolveFilterSample0;
  descriptor.tileWidth = 0;
  descriptor.tileHeight = 0;
  descriptor.renderTargetArrayLength = 1;
  descriptor.renderTargetWidth = 0;
  descriptor.renderTargetHeight = 0;
  uint32_t render_target_layers = 0u;

  if (pass->desc.color_attachments.length >
      VKR_METAL_PACKET_MAX_COLOR_ATTACHMENTS)
    return false_v;
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    const VkrRgAttachment *attachment =
        array_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    id<MTLTexture> texture =
        vkr_metal_packet_texture(renderer, attachment->image);
    if (!texture ||
        attachment->desc.slice.mip_level >= texture.mipmapLevelCount ||
        attachment->desc.slice.layer_count == 0u ||
        attachment->desc.slice.base_layer >= texture.arrayLength ||
        attachment->desc.slice.layer_count >
            texture.arrayLength - attachment->desc.slice.base_layer ||
        (render_target_layers != 0u &&
         render_target_layers != attachment->desc.slice.layer_count))
      return false_v;
    MTLRenderPassColorAttachmentDescriptor *color =
        descriptor.colorAttachments[i];
    color.texture = texture;
    if (descriptor.renderTargetWidth == 0) {
      descriptor.renderTargetWidth =
          Max(1u, texture.width >> attachment->desc.slice.mip_level);
      descriptor.renderTargetHeight =
          Max(1u, texture.height >> attachment->desc.slice.mip_level);
    }
    render_target_layers = attachment->desc.slice.layer_count;
    color.level = attachment->desc.slice.mip_level;
    color.slice = attachment->desc.slice.base_layer;
    color.loadAction = vkr_metal_packet_load_action(attachment->desc.load_op);
    color.storeAction =
        vkr_metal_packet_store_action(attachment->desc.store_op);
    const VkrClearValue *clear = &attachment->desc.clear_value;
    color.clearColor =
        MTLClearColorMake(clear->color_f32.r, clear->color_f32.g,
                          clear->color_f32.b, clear->color_f32.a);
  }
  if (pass->desc.has_depth_attachment) {
    const VkrRgAttachment *attachment = &pass->desc.depth_attachment;
    id<MTLTexture> texture =
        vkr_metal_packet_texture(renderer, attachment->image);
    if (!texture ||
        attachment->desc.slice.mip_level >= texture.mipmapLevelCount ||
        attachment->desc.slice.layer_count == 0u ||
        attachment->desc.slice.base_layer >= texture.arrayLength ||
        attachment->desc.slice.layer_count >
            texture.arrayLength - attachment->desc.slice.base_layer ||
        (render_target_layers != 0u &&
         render_target_layers != attachment->desc.slice.layer_count))
      return false_v;
    descriptor.depthAttachment.texture = texture;
    if (descriptor.renderTargetWidth == 0) {
      descriptor.renderTargetWidth =
          Max(1u, texture.width >> attachment->desc.slice.mip_level);
      descriptor.renderTargetHeight =
          Max(1u, texture.height >> attachment->desc.slice.mip_level);
    }
    render_target_layers = attachment->desc.slice.layer_count;
    descriptor.depthAttachment.level = attachment->desc.slice.mip_level;
    descriptor.depthAttachment.slice = attachment->desc.slice.base_layer;
    descriptor.depthAttachment.loadAction =
        vkr_metal_packet_load_action(attachment->desc.load_op);
    descriptor.depthAttachment.storeAction =
        vkr_metal_packet_store_action(attachment->desc.store_op);
    descriptor.depthAttachment.clearDepth =
        attachment->desc.clear_value.depth_stencil.depth;
  }
  descriptor.renderTargetArrayLength = render_target_layers;
  return pass->desc.color_attachments.length > 0 ||
                 pass->desc.has_depth_attachment
             ? true_v
             : false_v;
}

vkr_internal bool8_t vkr_metal_packet_pass_uses_image(const VkrRgPass *pass,
                                                      VkrRgImageHandle image) {
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    const VkrRgAttachment *attachment =
        array_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    if (attachment && attachment->image.id == image.id)
      return true_v;
  }
  if (pass->desc.has_depth_attachment &&
      pass->desc.depth_attachment.image.id == image.id)
    return true_v;
  for (uint64_t i = 0; i < pass->desc.image_reads.length; ++i) {
    const VkrRgImageUse *use =
        array_get_VkrRgImageUse(&pass->desc.image_reads, i);
    if (use && use->image.id == image.id)
      return true_v;
  }
  for (uint64_t i = 0; i < pass->desc.image_writes.length; ++i) {
    const VkrRgImageUse *use =
        array_get_VkrRgImageUse(&pass->desc.image_writes, i);
    if (use && use->image.id == image.id)
      return true_v;
  }
  return false_v;
}

vkr_internal bool8_t vkr_metal_packet_pass_uses_buffer(
    const VkrRgPass *pass, VkrRgBufferHandle buffer) {
  for (uint64_t i = 0; i < pass->desc.buffer_reads.length; ++i) {
    const VkrRgBufferUse *use =
        array_get_VkrRgBufferUse(&pass->desc.buffer_reads, i);
    if (use && use->buffer.id == buffer.id)
      return true_v;
  }
  for (uint64_t i = 0; i < pass->desc.buffer_writes.length; ++i) {
    const VkrRgBufferUse *use =
        array_get_VkrRgBufferUse(&pass->desc.buffer_writes, i);
    if (use && use->buffer.id == buffer.id)
      return true_v;
  }
  return false_v;
}

vkr_internal bool8_t vkr_metal_packet_prepare_dependencies(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketPreparedPass *prepared) {
  const VkrRgPass *pass = prepared->source;
  uint64_t consumer_capacity =
      pass->pre_image_barriers.length + pass->pre_buffer_barriers.length;
  uint64_t producer_capacity = 0u;
  for (uint64_t i = 0u; i < pass->out_edges.length; ++i) {
    const VkrRgPass *consumer =
        &renderer->graph->passes.data[pass->out_edges.data[i]];
    if (!consumer->culled)
      producer_capacity += consumer->pre_image_barriers.length +
                           consumer->pre_buffer_barriers.length;
  }
  if (consumer_capacity > UINT32_MAX || producer_capacity > UINT32_MAX)
    return false_v;
  if (consumer_capacity) {
    prepared->consumers =
        vkr_allocator_alloc(&renderer->graph_frame_allocator,
                            consumer_capacity * sizeof(*prepared->consumers),
                            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!prepared->consumers)
      return false_v;
  }
  if (producer_capacity) {
    prepared->producers =
        vkr_allocator_alloc(&renderer->graph_frame_allocator,
                            producer_capacity * sizeof(*prepared->producers),
                            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!prepared->producers)
      return false_v;
  }
  for (uint64_t i = 0u; i < pass->pre_image_barriers.length; ++i) {
    const VkrRgImageBarrier *barrier = &pass->pre_image_barriers.data[i];
    if (barrier->src_access == VKR_RG_IMAGE_ACCESS_NONE)
      continue;
    if (!vkr_metal_dependency_lower(
            &barrier->dependency,
            &prepared->consumers[prepared->consumer_count++]))
      return false_v;
  }
  for (uint64_t i = 0u; i < pass->pre_buffer_barriers.length; ++i) {
    const VkrRgBufferBarrier *barrier = &pass->pre_buffer_barriers.data[i];
    if (barrier->src_access == VKR_RG_BUFFER_ACCESS_NONE)
      continue;
    if (!vkr_metal_dependency_lower(
            &barrier->dependency,
            &prepared->consumers[prepared->consumer_count++]))
      return false_v;
  }
  for (uint64_t i = 0u; i < pass->out_edges.length; ++i) {
    const VkrRgPass *consumer =
        &renderer->graph->passes.data[pass->out_edges.data[i]];
    if (consumer->culled)
      continue;
    for (uint64_t b = 0u; b < consumer->pre_image_barriers.length; ++b) {
      const VkrRgImageBarrier *barrier = &consumer->pre_image_barriers.data[b];
      if (barrier->src_access == VKR_RG_IMAGE_ACCESS_NONE ||
          !vkr_metal_packet_pass_uses_image(pass, barrier->image))
        continue;
      if (!vkr_metal_dependency_lower(
              &barrier->dependency,
              &prepared->producers[prepared->producer_count++]))
        return false_v;
    }
    for (uint64_t b = 0u; b < consumer->pre_buffer_barriers.length; ++b) {
      const VkrRgBufferBarrier *barrier =
          &consumer->pre_buffer_barriers.data[b];
      if (barrier->src_access == VKR_RG_BUFFER_ACCESS_NONE ||
          !vkr_metal_packet_pass_uses_buffer(pass, barrier->buffer))
        continue;
      if (!vkr_metal_dependency_lower(
              &barrier->dependency,
              &prepared->producers[prepared->producer_count++]))
        return false_v;
    }
  }
  return true_v;
}

vkr_internal void
vkr_metal_packet_record_consumers(id<MTL4CommandEncoder> encoder,
                                  const VkrMetalPacketPreparedPass *prepared)
    API_AVAILABLE(macos(26.0)) {
  for (uint32_t i = 0u; i < prepared->consumer_count; ++i)
    vkr_metal_dependency_encode_consumer(encoder, &prepared->consumers[i]);
}

vkr_internal void
vkr_metal_packet_record_producers(id<MTL4CommandEncoder> encoder,
                                  const VkrMetalPacketPreparedPass *prepared)
    API_AVAILABLE(macos(26.0)) {
  for (uint32_t i = 0u; i < prepared->producer_count; ++i)
    vkr_metal_dependency_encode_producer(encoder, &prepared->producers[i]);
}

/* Copies the static square of dynamic slot `repeat_index`'s face into its
   dynamic square, both on slices of the shared atlas. */
vkr_internal bool8_t vkr_metal_packet_prepare_local_shadow_copy(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared)
    API_AVAILABLE(macos(26.0)) {
  const VkrLocalShadowPassPayload *local = packet->input.local_shadow;
  const VkrRgImageUse *read = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *write = vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  if (!local || pass->desc.repeat_index >= local->dynamic_render_count ||
      !read || !write || !read->has_slice || !write->has_slice ||
      read->image.id != write->image.id)
    return false_v;
  id<MTLTexture> atlas = vkr_metal_packet_texture(renderer, read->image);
  if (!atlas)
    return false_v;
  const uint32_t slot =
      vkr_local_shadow_dynamic_render_first(local) + pass->desc.repeat_index;
  const Vec4 source =
      local->views[local->dynamic_source_views[slot]].atlas_rect;
  const Vec4 destination = local->views[local->render_views[slot]].atlas_rect;
  if ((uint32_t)source.w != read->slice.base_layer ||
      (uint32_t)destination.w != write->slice.base_layer)
    return false_v;
  const float32_t atlas_size = (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE;
  const NSUInteger size = (NSUInteger)(source.z * atlas_size);
  prepared->transfer.texture_copy = (VkrMetalPacketTextureCopy){
      .texture = atlas,
      .source_slice = read->slice.base_layer,
      .destination_slice = write->slice.base_layer,
      .source_origin = MTLOriginMake((NSUInteger)(source.x * atlas_size),
                                     (NSUInteger)(source.y * atlas_size), 0u),
      .destination_origin =
          MTLOriginMake((NSUInteger)(destination.x * atlas_size),
                        (NSUInteger)(destination.y * atlas_size), 0u),
      .size = MTLSizeMake(size, size, 1u),
  };
  prepared->transfer.has_texture_copy = true_v;
  prepared->active = true_v;
  return true_v;
}

/* Copies the frame's packed candidate rows and a zeroed compaction state
   into the culling buffers, or a local face's static square into its
   dynamic square; Cull.Upload and Shadow.Local.Copy are the only transfer
   passes the executor check admits. */
vkr_internal bool8_t vkr_metal_packet_prepare_transfer(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    const VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  prepared->transfer = (VkrMetalPacketTransferPass){0};
  if (prepared->kind == VKR_RG_EXECUTOR_LOCAL_SHADOW_COPY)
    return vkr_metal_packet_prepare_local_shadow_copy(renderer, pass, packet,
                                                      prepared);
  if (prepared->kind != VKR_RG_EXECUTOR_GPU_DRAW_UPLOAD)
    return false_v;
  VkrMetalPacketTransferPass *transfer = &prepared->transfer;
  const VkrRgBufferUse *candidate_use =
      vkr_rg_pass_find_buffer_use(&pass->desc, 0u, 0u);
  const VkrRgBufferUse *state_use =
      vkr_rg_pass_find_buffer_use(&pass->desc, 1u, 0u);
  const VkrRgBufferUse *instance_use =
      vkr_rg_pass_find_buffer_use(&pass->desc, 3u, 0u);
  VkrMetalPacketGraphBufferInstance *candidates =
      candidate_use
          ? vkr_metal_packet_graph_buffer(renderer, candidate_use->buffer)
          : NULL;
  VkrMetalPacketGraphBufferInstance *instances =
      instance_use
          ? vkr_metal_packet_graph_buffer(renderer, instance_use->buffer)
          : NULL;
  VkrMetalPacketGraphBufferInstance *state =
      state_use ? vkr_metal_packet_graph_buffer(renderer, state_use->buffer)
                : NULL;
  const uint64_t state_bytes =
      (uint64_t)(1u + renderer->prepared_frame.shadow_cascade_count +
                 renderer->prepared_frame.local_shadow_render_count) *
      sizeof(VkrGpuDrawCompactionState);
  if (!candidates || !instances || !state ||
      state_bytes > state->resource.length)
    return false_v;
  for (uint32_t i = 0u; i < upload->gpu_draw_candidate_copy_count; ++i) {
    const VkrMetalPacketCandidateCopyRange range =
        upload->gpu_draw_candidate_copies[i];
    const uint64_t candidate_offset =
        (uint64_t)range.destination_first * sizeof(VkrGpuCandidateDrawRow);
    const uint64_t instance_offset =
        (uint64_t)range.destination_first * sizeof(VkrPreparedInstanceGPU);
    const uint64_t candidate_bytes =
        (uint64_t)range.count * sizeof(VkrGpuCandidateDrawRow);
    const uint64_t instance_bytes =
        (uint64_t)range.count * sizeof(VkrPreparedInstanceGPU);
    if (candidate_offset > candidates->resource.length ||
        candidate_bytes > candidates->resource.length - candidate_offset ||
        instance_offset > instances->resource.length ||
        instance_bytes > instances->resource.length - instance_offset)
      return false_v;
    transfer->buffers[transfer->buffer_count++] = (VkrMetalPacketBufferCopy){
        .source = upload->buffer,
        .destination = (id<MTLBuffer>)candidates->resource.metal_buffer,
        .source_offset = range.candidate_source_offset,
        .destination_offset = candidate_offset,
        .size = candidate_bytes,
    };
    transfer->buffers[transfer->buffer_count++] = (VkrMetalPacketBufferCopy){
        .source = upload->buffer,
        .destination = (id<MTLBuffer>)instances->resource.metal_buffer,
        .source_offset = range.instance_source_offset,
        .destination_offset = instance_offset,
        .size = instance_bytes,
    };
  }
  transfer->buffers[transfer->buffer_count++] = (VkrMetalPacketBufferCopy){
      .source = upload->buffer,
      .destination = (id<MTLBuffer>)state->resource.metal_buffer,
      .source_offset = upload->gpu_draw_state_zero_source_offset,
      .size = state_bytes,
  };
  prepared->active = true_v;
  return true_v;
}

vkr_internal void
vkr_metal_packet_record_transfer(id<MTL4ComputeCommandEncoder> encoder,
                                 const VkrMetalPacketTransferPass *transfer)
    API_AVAILABLE(macos(26.0)) {
  for (uint32_t i = 0u; i < transfer->buffer_count; ++i) {
    const VkrMetalPacketBufferCopy *copy = &transfer->buffers[i];
    [encoder copyFromBuffer:copy->source
               sourceOffset:copy->source_offset
                   toBuffer:copy->destination
          destinationOffset:copy->destination_offset
                       size:copy->size];
  }
  if (transfer->has_texture_copy) {
    const VkrMetalPacketTextureCopy *copy = &transfer->texture_copy;
    [encoder copyFromTexture:copy->texture
                 sourceSlice:copy->source_slice
                 sourceLevel:0u
                sourceOrigin:copy->source_origin
                  sourceSize:copy->size
                   toTexture:copy->texture
            destinationSlice:copy->destination_slice
            destinationLevel:0u
           destinationOrigin:copy->destination_origin];
  }
}

vkr_internal bool8_t
vkr_metal_packet_is_picking_readback(const VkrRgPass *pass) {
  return vkr_render_graph_pass_is(pass, VKR_RG_EXECUTOR_PICKING_READBACK);
}

vkr_internal bool8_t vkr_metal_packet_prepare_picking_readback(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    id<MTLBuffer> readback_buffer, uint64_t readback_offset)
    API_AVAILABLE(macos(26.0)) {
  if (!vkr_metal_packet_is_picking_readback(pass))
    return true_v;
  if (!packet || !packet->input.picking)
    return false_v;
  const VkrRgImageUse *read = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  if (!read)
    return false_v;
  id<MTLTexture> source = vkr_metal_packet_texture(renderer, read->image);
  if (!source)
    return false_v;
  const NSUInteger x =
      MIN((NSUInteger)packet->input.picking->x, source.width - 1);
  const NSUInteger y =
      MIN((NSUInteger)packet->input.picking->y, source.height - 1);
  const VkrRgImageUse *depth_read =
      vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  id<MTLTexture> depth_source =
      depth_read ? vkr_metal_packet_texture(renderer, depth_read->image) : nil;
  if (depth_read && (!depth_source || depth_source.width != source.width ||
                     depth_source.height != source.height))
    return false_v;
  prepared->picking_readback.source = source;
  prepared->picking_readback.depth_source = depth_source;
  prepared->picking_readback.destination = readback_buffer;
  prepared->picking_readback.destination_offset = readback_offset;
  prepared->picking_readback.origin = MTLOriginMake(x, y, 0u);
  prepared->active = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_pass_writes_present(
    const VkrMetalPacketRenderer *renderer, const VkrRgPass *pass) {
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    const VkrRgAttachment *attachment =
        array_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    if (attachment && attachment->image.id == renderer->graph->present_image.id)
      return true_v;
  }
  return false_v;
}

vkr_internal bool8_t vkr_metal_packet_is_last_present_writer(
    const VkrMetalPacketRenderer *renderer, uint64_t order_index) {
  const uint32_t *pass_index =
      array_get_uint32_t(&renderer->graph->execution_order, order_index);
  const VkrRgPass *pass =
      array_get_VkrRgPass(&renderer->graph->passes, *pass_index);
  if (!vkr_metal_packet_pass_writes_present(renderer, pass))
    return false_v;
  for (uint64_t i = order_index + 1;
       i < renderer->graph->execution_order.length; ++i) {
    const uint32_t *future_index =
        array_get_uint32_t(&renderer->graph->execution_order, i);
    const VkrRgPass *future =
        array_get_VkrRgPass(&renderer->graph->passes, *future_index);
    if (vkr_metal_packet_pass_writes_present(renderer, future))
      return false_v;
  }
  return true_v;
}
