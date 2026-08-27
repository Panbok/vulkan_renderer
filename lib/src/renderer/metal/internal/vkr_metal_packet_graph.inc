/* Render-graph resources, dependency lowering, pass descriptors, and capture
 * plans. */
vkr_internal void vkr_metal_packet_noop(VkrRgPassContext *ctx,
                                        void *user_data) {
  (void)ctx;
  (void)user_data;
}

typedef enum VkrMetalPacketGraphExecutorKind {
  VKR_METAL_PACKET_GRAPH_EXECUTOR_SHADOW = 0,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_PICKING,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_PICKING_DEPTH_SEED,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_PICKING_RESOLVE,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_PICKING_READBACK,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_IBL_BAKE,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_UPLOAD,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_CLASSIFY,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_PREFIX,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_ENCODE,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_TEMPORAL_TRANSFORM,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_UPLOAD,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_CLASSIFY,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_PREFIX,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_ENCODE,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_DEPTH_SEED,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_VBUFFER_OPAQUE,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_GBUFFER_RESOLVE,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_DEFERRED_LIGHTING,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_TEMPORAL_RESOLVE,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_SHADE,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_COVERAGE,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_COMPACT,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_SDSM_REDUCE,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_HZB_BUILD,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_WORLD_BLEND,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_TONEMAP,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_EDITOR,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_UI,
  VKR_METAL_PACKET_GRAPH_EXECUTOR_COUNT,
} VkrMetalPacketGraphExecutorKind;

typedef struct VkrMetalPacketGraphExecutorSpec {
  const char *name;
  VkrRgPassType type;
} VkrMetalPacketGraphExecutorSpec;

vkr_global const VkrMetalPacketGraphExecutorSpec
    s_metal_packet_graph_executors[] = {
        {"pass.shadow.cascade", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.picking", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.picking.depth_seed", VKR_RG_PASS_TYPE_TRANSFER},
        {"pass.picking.resolve", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.picking.readback", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.ibl_bake", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.gpu_draw_upload", VKR_RG_PASS_TYPE_TRANSFER},
        {"pass.gpu_draw_classify", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.gpu_draw_prefix", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.gpu_draw_encode", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.temporal.transform_history", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.transmission.gpu_draw_upload", VKR_RG_PASS_TYPE_TRANSFER},
        {"pass.transmission.gpu_draw_classify", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.transmission.gpu_draw_prefix", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.transmission.gpu_draw_encode", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.transmission.depth_seed", VKR_RG_PASS_TYPE_TRANSFER},
        {"pass.vbuffer.opaque", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.vbuffer.transmission", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.gbuffer.resolve", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.lighting.deferred", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.temporal.resolve", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.transmission.shade", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.transmission.coverage", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.transmission.compact", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.sdsm.reduce", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.hzb.build", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.copy.pre_transmission.fullscreen", VKR_RG_PASS_TYPE_TRANSFER},
        {"pass.copy.pre_transmission.editor", VKR_RG_PASS_TYPE_TRANSFER},
        {"pass.world.blend", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.tonemap", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.editor", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.ui", VKR_RG_PASS_TYPE_GRAPHICS},
};
_Static_assert(ArrayCount(s_metal_packet_graph_executors) ==
                   VKR_METAL_PACKET_GRAPH_EXECUTOR_COUNT,
               "Metal graph executor table is incomplete");

vkr_internal bool8_t vkr_metal_packet_graph_executor_kind(
    const VkrRgPass *pass, VkrMetalPacketGraphExecutorKind *out_kind) {
  if (!pass || !out_kind || pass->desc.executor_id == 0u ||
      pass->desc.executor_id > VKR_METAL_PACKET_GRAPH_EXECUTOR_COUNT)
    return false_v;
  *out_kind = (VkrMetalPacketGraphExecutorKind)(pass->desc.executor_id - 1u);
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_graph_pass_is(
    const VkrRgPass *pass, VkrMetalPacketGraphExecutorKind kind) {
  return pass && pass->desc.executor_id == (uint32_t)kind + 1u;
}

vkr_internal bool8_t
vkr_metal_packet_register_executors(VkrMetalPacketRenderer *renderer) {
  for (uint32_t i = 0; i < ArrayCount(s_metal_packet_graph_executors); ++i) {
    const VkrMetalPacketGraphExecutorSpec *spec =
        &s_metal_packet_graph_executors[i];
    VkrRgPassExecutor entry = {
        .name = string8_create_from_cstr((const uint8_t *)spec->name,
                                         string_length(spec->name)),
        .id = i + 1u,
        .type = spec->type,
        .execute = vkr_metal_packet_noop,
    };
    if (!vkr_rg_executor_registry_register(&renderer->executors, &entry))
      return false_v;
  }
  return true_v;
}

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
  case VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM:
    return MTLPixelFormatEAC_RG11Unorm;
  case VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT:
    return MTLPixelFormatRGBA16Float;
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

vkr_internal MTLPixelFormat
vkr_metal_packet_target_pixel_format(const VkrMetalPacketRenderer *renderer) {
  if (renderer->target_kind == VKR_METAL_PACKET_TARGET_WINDOW)
    return renderer->srgb_output ? MTLPixelFormatBGRA8Unorm_sRGB
                                 : MTLPixelFormatBGRA8Unorm;
  return renderer->srgb_output ? MTLPixelFormatRGBA8Unorm_sRGB
                               : MTLPixelFormatRGBA8Unorm;
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

vkr_internal bool8_t vkr_metal_packet_retire_image_instance(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketImageInstance *instance) {
  if (!instance || !instance->live)
    return true_v;
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

vkr_internal bool8_t vkr_metal_packet_create_image_instance(
    VkrMetalPacketRenderer *renderer, const VkrRgImage *image,
    VkrMetalPacketImageInstance *instance) API_AVAILABLE(macos(26.0)) {
  const MTLPixelFormat format =
      vkr_metal_packet_pixel_format(image->desc.format);
  if (!vkr_metal_packet_graph_image_format_supported(renderer, &image->desc) ||
      format == MTLPixelFormatInvalid || image->desc.width == 0 ||
      image->desc.height == 0 || image->desc.layers == 0 ||
      image->desc.mip_levels == 0 ||
      image->desc.mip_levels > VKR_METAL_PACKET_MAX_TEXTURE_MIPS) {
    log_error("Metal graph image '%.*s' has unsupported descriptor: format=%u "
              "extent=%ux%u layers=%u mips=%u samples=%u usage=0x%x",
              (int)image->name.length, image->name.str, image->desc.format,
              image->desc.width, image->desc.height, image->desc.layers,
              image->desc.mip_levels, image->desc.samples,
              image->desc.usage.set);
    return false_v;
  }

  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
  descriptor.textureType =
      image->desc.layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
  descriptor.pixelFormat = format;
  descriptor.width = image->desc.width;
  descriptor.height = image->desc.height;
  descriptor.depth = 1;
  descriptor.arrayLength = image->desc.layers;
  descriptor.mipmapLevelCount = image->desc.mip_levels;
  descriptor.sampleCount = image->desc.samples;
  descriptor.usage = vkr_metal_packet_texture_usage(image->desc.usage);
  VkrMetalTextureResource resource = {0};
  const VkrMetalMemoryStatus status = vkr_metal_memory_device_create_texture(
      renderer->memory, descriptor, &resource);
  [descriptor release];
  if (status != VKR_METAL_MEMORY_STATUS_OK) {
    log_error("Metal graph image '%.*s' allocation failed: %s, format=%u "
              "extent=%ux%u layers=%u mips=%u usage=0x%x",
              (int)image->name.length, image->name.str,
              vkr_metal_memory_status_string(status), image->desc.format,
              image->desc.width, image->desc.height, image->desc.layers,
              image->desc.mip_levels, image->desc.usage.set);
    return false_v;
  }

  *instance = (VkrMetalPacketImageInstance){
      .resource = resource,
      .live = true_v,
      .owned = true_v,
  };
  id<MTLTexture> texture = (id<MTLTexture>)resource.metal_texture;
  const MTLTextureType view_type =
      image->desc.layers > 1u ? MTLTextureType2DArray : MTLTextureType2D;
  for (uint32_t mip = 0u; mip < image->desc.mip_levels; ++mip) {
    id<MTLTexture> view = [texture
        newTextureViewWithPixelFormat:format
                          textureType:view_type
                               levels:NSMakeRange(mip, 1u)
                               slices:NSMakeRange(0u, image->desc.layers)];
    if (!view) {
      log_error("Metal graph image '%.*s' mip %u view creation failed",
                (int)image->name.length, image->name.str, mip);
      vkr_metal_packet_release_image_views(&instance->views);
      (void)vkr_metal_memory_device_retire(renderer->memory, resource.handle,
                                           renderer->completion.signaledValue);
      (void)vkr_metal_memory_device_collect(
          renderer->memory, renderer->completion.signaledValue, NULL);
      *instance = (VkrMetalPacketImageInstance){0};
      return false_v;
    }
    instance->views.mip_views[mip] = view;
  }
  if (image->desc.layers > 1u) {
    if (image->desc.layers > VKR_METAL_PACKET_MAX_TEXTURE_LAYERS) {
      log_error("Metal graph image '%.*s' layer count %u exceeds capacity %u",
                (int)image->name.length, image->name.str, image->desc.layers,
                VKR_METAL_PACKET_MAX_TEXTURE_LAYERS);
      vkr_metal_packet_release_image_views(&instance->views);
      (void)vkr_metal_memory_device_retire(renderer->memory, resource.handle,
                                           renderer->completion.signaledValue);
      (void)vkr_metal_memory_device_collect(
          renderer->memory, renderer->completion.signaledValue, NULL);
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
        (void)vkr_metal_memory_device_collect(
            renderer->memory, renderer->completion.signaledValue, NULL);
        *instance = (VkrMetalPacketImageInstance){0};
        return false_v;
      }
      instance->views.layer_views[layer] = view;
    }
  }
  return true_v;
}

vkr_internal bool8_t
vkr_metal_packet_realize_images(VkrMetalPacketRenderer *renderer) {
  if (renderer->graph->images.length > renderer->max_images) {
    log_error("Metal graph image count %llu exceeds capacity %u",
              (unsigned long long)renderer->graph->images.length,
              renderer->max_images);
    return false_v;
  }
  for (uint64_t i = 0; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
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
                renderer, image, &replacement.instances[instance])) {
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
        vector_get_VkrRgImage(&renderer->graph->images, i);
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
        vector_get_VkrRgImage(&renderer->graph->images, i);
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
        vector_get_VkrRgBuffer(&renderer->graph->buffers, i);
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
    if (candidate_use[candidate] <= completed &&
        candidate_use[candidate] < selected_use) {
      selected = candidate;
      selected_use = candidate_use[candidate];
    }
  }
  if (selected == renderer->history_instance_count) {
    for (uint32_t candidate = 0u; candidate < renderer->history_instance_count;
         ++candidate)
      log_error("Metal history instance %u is busy through submit %llu",
                candidate, (unsigned long long)candidate_use[candidate]);
    log_error("Metal has no completion-safe history output instance "
              "(completed %llu)",
              (unsigned long long)completed);
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
  if (!renderer->hzb_enabled)
    return true_v;
  VkrMetalPacketImage *slot =
      vkr_metal_packet_named_image_slot(renderer, "hzb_history");
  if (!slot || slot->instance_count == 0u)
    return true_v;
  const uint32_t current = renderer->history_output_index;
  const uint64_t completed = renderer->completion.signaledValue;
  uint64_t newest_producer = 0u;
  for (uint32_t i = 0u; i < slot->instance_count; ++i) {
    const VkrMetalPacketImageInstance *instance = &slot->instances[i];
    if (i == current || !instance->live || !instance->history_valid ||
        instance->history_producer_submit_value > completed ||
        instance->history_width != renderer->prepared_frame.viewport_width ||
        instance->history_height != renderer->prepared_frame.viewport_height ||
        instance->history_world_epoch != renderer->current_hzb_world_epoch ||
        MemCompare(&instance->history_view_projection,
                   &renderer->current_hzb_view_projection, sizeof(Mat4)) != 0)
      continue;
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

vkr_internal id<MTLTexture>
vkr_metal_packet_selected_hzb_texture(VkrMetalPacketRenderer *renderer) {
  VkrMetalPacketImage *slot =
      vkr_metal_packet_named_image_slot(renderer, "hzb_history");
  const uint32_t selected = renderer->selected_hzb_history_instance;
  return slot && selected < slot->instance_count &&
                 slot->instances[selected].live
             ? (id<MTLTexture>)slot->instances[selected].resource.metal_texture
             : nil;
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

vkr_internal bool8_t
vkr_metal_packet_select_temporal_history(VkrMetalPacketRenderer *renderer) {
  renderer->selected_temporal_history_instance = UINT32_MAX;
  const VkrRenderPacket *packet = renderer->graph->packet;
  if (!packet || !packet->globals.temporal.history_valid)
    return true_v;
  VkrMetalPacketImage *colors =
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_color");
  VkrMetalPacketImage *depths =
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_depth");
  VkrMetalPacketImage *identities =
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_identity");
  VkrMetalPacketImage *primitives =
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_primitive");
  if (!colors || !depths || !identities || !primitives ||
      !vkr_rg_buffer_handle_valid(
          renderer->temporal_transform_history_handle) ||
      colors->instance_count != renderer->history_instance_count ||
      depths->instance_count != colors->instance_count ||
      identities->instance_count != colors->instance_count ||
      primitives->instance_count != colors->instance_count)
    return false_v;
  const uint32_t current = renderer->history_output_index;
  const uint64_t completed = renderer->completion.signaledValue;
  uint64_t newest_producer = 0u;
  for (uint32_t i = 0u; i < colors->instance_count; ++i) {
    VkrMetalPacketImageInstance *color = &colors->instances[i];
    VkrMetalPacketGraphBufferInstance *transform =
        vkr_metal_packet_graph_buffer_instance(
            renderer, renderer->temporal_transform_history_handle, i);
    if (i == current || !color->live || !color->history_valid ||
        color->history_producer_submit_value > completed ||
        color->history_scene_generation != packet->frame.scene_generation ||
        color->history_width != renderer->prepared_frame.viewport_width ||
        color->history_height != renderer->prepared_frame.viewport_height ||
        !depths->instances[i].live || !identities->instances[i].live ||
        !primitives->instances[i].live || !transform ||
        !transform->history_valid)
      continue;
    if (renderer->selected_temporal_history_instance == UINT32_MAX ||
        color->history_producer_submit_value > newest_producer) {
      renderer->selected_temporal_history_instance = i;
      newest_producer = color->history_producer_submit_value;
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
vkr_metal_packet_realize_buffers(VkrMetalPacketRenderer *renderer) {
  if (renderer->graph->buffers.length > renderer->max_images) {
    log_error("Metal graph buffer count %llu exceeds capacity %u",
              (unsigned long long)renderer->graph->buffers.length,
              renderer->max_images);
    return false_v;
  }
  renderer->gpu_candidate_buffer_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  renderer->gpu_candidate_instance_buffer_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  renderer->transmission_gpu_candidate_instance_buffer_handle =
      VKR_RG_BUFFER_HANDLE_INVALID;
  renderer->temporal_transform_history_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  for (uint64_t i = 0u; i < renderer->graph->buffers.length; ++i) {
    const VkrRgBuffer *buffer =
        vector_get_VkrRgBuffer(&renderer->graph->buffers, i);
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
    else if (vkr_string8_equals_cstr(&buffer->name,
                                     "transmission_gpu_draw_instances"))
      renderer->transmission_gpu_candidate_instance_buffer_handle = handle;
    else if (vkr_string8_equals_cstr(&buffer->name,
                                     "temporal_transform_history"))
      renderer->temporal_transform_history_handle = handle;
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
    if (slot->live && slot->graph_generation == buffer->generation &&
        slot->instance_count == instance_count &&
        slot->desc.size == buffer->desc.size &&
        slot->desc.usage.set == buffer->desc.usage.set &&
        slot->desc.flags == buffer->desc.flags)
      continue;
    VkrMetalPacketGraphBufferInstance *instances =
        &renderer->graph_buffer_instances[i * renderer->history_instance_count];
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
          &replacement_instances[instance].resource);
      if (status != VKR_METAL_MEMORY_STATUS_OK) {
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
      vector_get_VkrRgImage(&renderer->graph->images, image_index);
  VkrMetalPacketImage *slot = &renderer->images[image_index];
  if (!image || !slot->live || slot->graph_generation != image->generation ||
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

void vkr_metal_packet_renderer_retained_shadow_token(
    VkrMetalPacketRenderer *renderer, uint32_t image_index,
    VkrRetainedShadowToken *out_token) {
  *out_token = (VkrRetainedShadowToken){0};
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
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

vkr_internal void
vkr_metal_packet_mark_graph_images_submitted(VkrMetalPacketRenderer *renderer,
                                             uint64_t submit_value) {
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
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
  if (current < slot->instance_count) {
    VkrMetalPacketImageInstance *instance = &slot->instances[current];
    instance->history_producer_submit_value = submit_value;
    instance->history_world_epoch = renderer->current_hzb_world_epoch;
    instance->history_view_projection = renderer->current_hzb_view_projection;
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
vkr_metal_packet_mark_temporal_submitted(VkrMetalPacketRenderer *renderer,
                                         uint64_t submit_value) {
  const VkrRenderPacket *packet = renderer->graph->packet;
  if (!packet || !renderer->active_command_slot)
    return;
  VkrMetalPacketImage *slots[] = {
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_color"),
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_depth"),
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_identity"),
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_primitive"),
  };
  const uint32_t current = renderer->history_output_index;
  const uint32_t selected = renderer->selected_temporal_history_instance;
  for (uint32_t i = 0u; i < ArrayCount(slots); ++i) {
    if (!slots[i] || current >= slots[i]->instance_count)
      continue;
    VkrMetalPacketImageInstance *instance = &slots[i]->instances[current];
    instance->history_producer_submit_value = submit_value;
    instance->history_frame_index = packet->frame.frame_index;
    instance->history_scene_generation = packet->frame.scene_generation;
    instance->history_view_projection = vkr_metal_packet_clip_matrix(
        renderer->convert_vulkan_clip_y,
        packet->globals.temporal.current_view_projection);
    instance->history_width = renderer->prepared_frame.viewport_width;
    instance->history_height = renderer->prepared_frame.viewport_height;
    instance->history_valid = true_v;
  }
  VkrMetalPacketGraphBufferInstance *current_transform =
      vkr_metal_packet_graph_buffer_instance(
          renderer, renderer->temporal_transform_history_handle, current);
  if (current_transform) {
    current_transform->history_producer_submit_value = submit_value;
    current_transform->history_frame_index = packet->frame.frame_index;
    current_transform->history_scene_generation =
        packet->frame.scene_generation;
    current_transform->history_valid = true_v;
  }

  if (selected == UINT32_MAX)
    return;
  for (uint32_t i = 0u; i < ArrayCount(slots); ++i) {
    if (slots[i] && selected < slots[i]->instance_count)
      slots[i]->instances[selected].last_use_submit_value = MAX(
          slots[i]->instances[selected].last_use_submit_value, submit_value);
  }
  VkrMetalPacketGraphBufferInstance *selected_transform =
      vkr_metal_packet_graph_buffer_instance(
          renderer, renderer->temporal_transform_history_handle, selected);
  if (selected_transform)
    selected_transform->last_use_submit_value =
        MAX(selected_transform->last_use_submit_value, submit_value);
}

vkr_internal void
vkr_metal_packet_mark_graph_buffers_submitted(VkrMetalPacketRenderer *renderer,
                                              uint64_t submit_value) {
  for (uint64_t i = 0u; i < renderer->graph->buffers.length; ++i) {
    const VkrRgBuffer *buffer =
        vector_get_VkrRgBuffer(&renderer->graph->buffers, i);
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
        vector_get_VkrRgImage(&renderer->graph->images, i);
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
        vector_get_VkrRgImage(&renderer->graph->images, i);
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
  };
  for (uint32_t i = 0; i < ArrayCount(values); ++i) {
    if (!isfinite(values[i]))
      return false_v;
  }
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

vkr_internal void vkr_metal_packet_decode_half4(const void *source,
                                                float32_t destination[4]) {
  uint16_t encoded[4] = {0};
  MemCopy(encoded, source, sizeof(encoded));
  for (uint32_t i = 0; i < ArrayCount(encoded); ++i)
    destination[i] = vkr_float16_to_float32(encoded[i]);
}

vkr_internal bool8_t vkr_metal_packet_capture_plan(
    VkrMetalPacketRenderer *renderer, const VkrRenderPacket *packet,
    uint64_t first_offset, VkrMetalPacketCapturePlan *plans,
    uint32_t *out_count, uint64_t *out_size) {
  const VkrCaptureBatchRequest *request =
      packet->debug ? packet->debug->capture : NULL;
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
    if (string_equals(channel->name, "scene_color"))
      source_name = "temporal_history_color";
    else if (string_equals(channel->name, "depth"))
      // The deferred topology writes depth only in the visibility raster, so
      // both editor and fullscreen capture read the same image. The retired
      // forward attachments are gone; naming them captured nothing.
      source_name = "opaque_vbuffer_depth";
    else if (string_n_equals(channel->name, "shadow_cascade_", 15))
      source_slice = (uint32_t)(channel->name[15] - '0');
    else if (string_equals(channel->name, "picking_ids") &&
             (!packet->picking || !packet->picking->pending))
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
                   .color_space = channel->color_space,
                   .origin = VKR_CAPTURE_ORIGIN_TOP_LEFT,
                   .data_size = data_size,
                   .mip = 0,
                   .layer = source_slice,
                   .display_exposure = packet->globals.exposure},
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
    uint32_t shadow_cascade_count, const uint8_t *transmission_readback) {
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
  if (transmission_readback) {
    const VkrMetalPacketTransmissionDiagnostics *transmission =
        (const VkrMetalPacketTransmissionDiagnostics *)transmission_readback;
    result->transmission_gpu_visible_count =
        transmission->compaction.visible_count;
    MemCopy(result->transmission_gpu_bucket_counts,
            transmission->compaction.bucket_counts,
            sizeof(result->transmission_gpu_bucket_counts));
    result->transmission_gpu_overflow_count =
        transmission->compaction.overflow_count;
    result->transmission_gpu_occlusion_culled_count =
        transmission->compaction.occlusion_culled_count;
  }
  result->opaque_draw_count = result->gpu_visible_count;
  result->transmission_draw_count = result->transmission_gpu_visible_count;
  result->indexed_draw_count = result->opaque_draw_count +
                               result->transmission_draw_count +
                               result->blend_draw_count;
  result->has_gpu_draw_diagnostics = true_v;
}

vkr_internal void vkr_metal_packet_decode_sdsm(VkrMetalPacketResult *result,
                                               const uint8_t *readback) {
  if (!result || !readback)
    return;
  VkrMetalPacketSdsmState state = {0};
  MemCopy(&state, readback, sizeof(state));
  VkrShadowDepthRangeSample *sample = &result->shadow_depth_range;
  sample->occupied_count = state.occupied_count;
  if (state.occupied_count == 0u) {
    sample->valid = true_v;
    return;
  }
  if (state.min_device_z_bits == UINT32_MAX)
    return;
  MemCopy(&sample->min_device_z, &state.min_device_z_bits, sizeof(float32_t));
  MemCopy(&sample->max_device_z, &state.max_device_z_bits, sizeof(float32_t));
  sample->valid =
      isfinite(sample->min_device_z) && isfinite(sample->max_device_z) &&
              sample->min_device_z >= 0.0f && sample->max_device_z <= 1.0f &&
              sample->min_device_z <= sample->max_device_z
          ? true_v
          : false_v;
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
    const VkrRenderPacket *packet, MTL4RenderPassDescriptor *descriptor)
    API_AVAILABLE(macos(26.0)) {
  (void)packet;
  for (uint32_t i = 0; i < VKR_METAL_PACKET_MAX_COLOR_ATTACHMENTS; ++i) {
    descriptor.colorAttachments[i].texture = nil;
    descriptor.colorAttachments[i].level = 0;
    descriptor.colorAttachments[i].slice = 0;
  }
  descriptor.depthAttachment.texture = nil;
  descriptor.renderTargetArrayLength = 1;
  descriptor.renderTargetWidth = 0;
  descriptor.renderTargetHeight = 0;
  uint32_t render_target_layers = 0u;

  if (pass->desc.color_attachments.length >
      VKR_METAL_PACKET_MAX_COLOR_ATTACHMENTS)
    return false_v;
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    const VkrRgAttachment *attachment =
        vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
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

vkr_internal bool8_t vkr_metal_packet_encode_dependency(
    id<MTL4CommandEncoder> encoder, const VkrGpuDependency *dependency,
    bool8_t producer) API_AVAILABLE(macos(26.0)) {
  VkrMetalDependency lowered = {0};
  if (!vkr_metal_dependency_lower(dependency, &lowered))
    return false_v;
  return producer ? vkr_metal_dependency_encode_producer(encoder, &lowered)
                  : vkr_metal_dependency_encode_consumer(encoder, &lowered);
}

vkr_internal bool8_t vkr_metal_packet_encode_consumers(
    id<MTL4CommandEncoder> encoder, const VkrRgPass *pass,
    uint32_t *dependency_count) API_AVAILABLE(macos(26.0)) {
  for (uint64_t i = 0; i < pass->pre_image_barriers.length; ++i) {
    const VkrRgImageBarrier *barrier =
        vector_get_VkrRgImageBarrier(&pass->pre_image_barriers, i);
    if (barrier->src_access == VKR_RG_IMAGE_ACCESS_NONE)
      continue;
    if (!vkr_metal_packet_encode_dependency(encoder, &barrier->dependency,
                                            false_v))
      return false_v;
    (*dependency_count)++;
  }
  for (uint64_t i = 0; i < pass->pre_buffer_barriers.length; ++i) {
    const VkrRgBufferBarrier *barrier =
        vector_get_VkrRgBufferBarrier(&pass->pre_buffer_barriers, i);
    if (barrier->src_access == VKR_RG_BUFFER_ACCESS_NONE)
      continue;
    if (!vkr_metal_packet_encode_dependency(encoder, &barrier->dependency,
                                            false_v))
      return false_v;
    (*dependency_count)++;
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_has_in_edge(const VkrRgPass *pass,
                                                  uint32_t producer_index) {
  for (uint64_t i = 0; i < pass->in_edges.length; ++i) {
    const uint32_t *edge = vector_get_uint32_t(&pass->in_edges, i);
    if (edge && *edge == producer_index)
      return true_v;
  }
  return false_v;
}

vkr_internal bool8_t vkr_metal_packet_pass_uses_image(const VkrRgPass *pass,
                                                      VkrRgImageHandle image) {
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    const VkrRgAttachment *attachment =
        vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    if (attachment && attachment->image.id == image.id)
      return true_v;
  }
  if (pass->desc.has_depth_attachment &&
      pass->desc.depth_attachment.image.id == image.id)
    return true_v;
  for (uint64_t i = 0; i < pass->desc.image_reads.length; ++i) {
    const VkrRgImageUse *use =
        vector_get_VkrRgImageUse(&pass->desc.image_reads, i);
    if (use && use->image.id == image.id)
      return true_v;
  }
  for (uint64_t i = 0; i < pass->desc.image_writes.length; ++i) {
    const VkrRgImageUse *use =
        vector_get_VkrRgImageUse(&pass->desc.image_writes, i);
    if (use && use->image.id == image.id)
      return true_v;
  }
  return false_v;
}

vkr_internal bool8_t vkr_metal_packet_pass_uses_buffer(
    const VkrRgPass *pass, VkrRgBufferHandle buffer) {
  for (uint64_t i = 0; i < pass->desc.buffer_reads.length; ++i) {
    const VkrRgBufferUse *use =
        vector_get_VkrRgBufferUse(&pass->desc.buffer_reads, i);
    if (use && use->buffer.id == buffer.id)
      return true_v;
  }
  for (uint64_t i = 0; i < pass->desc.buffer_writes.length; ++i) {
    const VkrRgBufferUse *use =
        vector_get_VkrRgBufferUse(&pass->desc.buffer_writes, i);
    if (use && use->buffer.id == buffer.id)
      return true_v;
  }
  return false_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_producers(
    VkrMetalPacketRenderer *renderer, id<MTL4CommandEncoder> encoder,
    uint32_t producer_index, uint32_t *dependency_count)
    API_AVAILABLE(macos(26.0)) {
  const VkrRgPass *producer =
      vector_get_VkrRgPass(&renderer->graph->passes, producer_index);
  for (uint64_t i = 0; i < renderer->graph->passes.length; ++i) {
    const VkrRgPass *consumer =
        vector_get_VkrRgPass(&renderer->graph->passes, i);
    if (!consumer || consumer->culled ||
        !vkr_metal_packet_has_in_edge(consumer, producer_index))
      continue;
    for (uint64_t b = 0; b < consumer->pre_image_barriers.length; ++b) {
      const VkrRgImageBarrier *barrier =
          vector_get_VkrRgImageBarrier(&consumer->pre_image_barriers, b);
      if (barrier->src_access == VKR_RG_IMAGE_ACCESS_NONE ||
          !vkr_metal_packet_pass_uses_image(producer, barrier->image))
        continue;
      if (!vkr_metal_packet_encode_dependency(encoder, &barrier->dependency,
                                              true_v))
        return false_v;
      (*dependency_count)++;
    }
    for (uint64_t b = 0; b < consumer->pre_buffer_barriers.length; ++b) {
      const VkrRgBufferBarrier *barrier =
          vector_get_VkrRgBufferBarrier(&consumer->pre_buffer_barriers, b);
      if (barrier->src_access == VKR_RG_BUFFER_ACCESS_NONE ||
          !vkr_metal_packet_pass_uses_buffer(producer, barrier->buffer))
        continue;
      if (!vkr_metal_packet_encode_dependency(encoder, &barrier->dependency,
                                              true_v))
        return false_v;
      (*dependency_count)++;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_transfer(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    id<MTL4ComputeCommandEncoder> encoder,
    const VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  const bool8_t opaque_gpu_upload = vkr_metal_packet_graph_pass_is(
      pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_UPLOAD);
  const bool8_t transmission_gpu_upload = vkr_metal_packet_graph_pass_is(
      pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_UPLOAD);
  if (opaque_gpu_upload || transmission_gpu_upload) {
    if (!upload)
      return false_v;
    const VkrRgBufferUse *candidate_use =
        vkr_rg_pass_find_buffer_use(&pass->desc, 0u, 0u);
    const VkrRgBufferUse *state_use =
        vkr_rg_pass_find_buffer_use(&pass->desc, 1u, 0u);
    const uint32_t instance_binding = transmission_gpu_upload ? 2u : 3u;
    const VkrRgBufferUse *instance_use =
        vkr_rg_pass_find_buffer_use(&pass->desc, instance_binding, 0u);
    const bool8_t sdsm_enabled =
        opaque_gpu_upload && renderer->prepared_frame.sdsm_enabled;
    const VkrRgBufferUse *sdsm_use =
        sdsm_enabled ? vkr_rg_pass_find_buffer_use(&pass->desc, 2u, 0u) : NULL;
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
    VkrMetalPacketGraphBufferInstance *sdsm_state =
        sdsm_use ? vkr_metal_packet_graph_buffer(renderer, sdsm_use->buffer)
                 : NULL;
    const uint64_t state_source_offset =
        transmission_gpu_upload
            ? upload->transmission_gpu_draw_state_zero_source_offset
            : upload->gpu_draw_state_zero_source_offset;
    const uint64_t state_bytes =
        transmission_gpu_upload
            ? sizeof(VkrMetalPacketTransmissionDiagnostics)
            : (uint64_t)(1u + renderer->prepared_frame.shadow_cascade_count) *
                  sizeof(VkrGpuDrawCompactionState);
    if (!candidates || !instances || !state || (sdsm_enabled && !sdsm_state) ||
        state_bytes > state->resource.length)
      return false_v;
    if (transmission_gpu_upload) {
      const uint64_t count = renderer->transmission_gpu_draw_count;
      const uint64_t candidate_bytes = count * sizeof(VkrGpuCandidateDrawRow);
      const uint64_t instance_bytes = count * sizeof(VkrInstanceDataGPU);
      if (candidate_bytes > candidates->resource.length ||
          instance_bytes > instances->resource.length)
        return false_v;
      if (count > 0u) {
        [encoder copyFromBuffer:upload->buffer
                   sourceOffset:
                       upload->transmission_gpu_draw_candidate_source_offset
                       toBuffer:(id<MTLBuffer>)candidates->resource.metal_buffer
              destinationOffset:0u
                           size:candidate_bytes];
        [encoder
               copyFromBuffer:upload->buffer
                 sourceOffset:upload
                                  ->transmission_gpu_draw_instance_source_offset
                     toBuffer:(id<MTLBuffer>)instances->resource.metal_buffer
            destinationOffset:0u
                         size:instance_bytes];
      }
    } else {
      for (uint32_t i = 0u; i < upload->gpu_draw_candidate_copy_count; ++i) {
        const VkrMetalPacketCandidateCopyRange *range =
            &upload->gpu_draw_candidate_copies[i];
        const uint64_t candidate_destination =
            (uint64_t)range->destination_first * sizeof(VkrGpuCandidateDrawRow);
        const uint64_t instance_destination =
            (uint64_t)range->destination_first * sizeof(VkrInstanceDataGPU);
        const uint64_t candidate_bytes =
            (uint64_t)range->count * sizeof(VkrGpuCandidateDrawRow);
        const uint64_t instance_bytes =
            (uint64_t)range->count * sizeof(VkrInstanceDataGPU);
        if (candidate_destination > candidates->resource.length ||
            candidate_bytes >
                candidates->resource.length - candidate_destination ||
            instance_destination > instances->resource.length ||
            instance_bytes > instances->resource.length - instance_destination)
          return false_v;
        [encoder copyFromBuffer:upload->buffer
                   sourceOffset:range->candidate_source_offset
                       toBuffer:(id<MTLBuffer>)candidates->resource.metal_buffer
              destinationOffset:candidate_destination
                           size:candidate_bytes];
        [encoder copyFromBuffer:upload->buffer
                   sourceOffset:range->instance_source_offset
                       toBuffer:(id<MTLBuffer>)instances->resource.metal_buffer
              destinationOffset:instance_destination
                           size:instance_bytes];
      }
    }
    [encoder copyFromBuffer:upload->buffer
               sourceOffset:state_source_offset
                   toBuffer:(id<MTLBuffer>)state->resource.metal_buffer
          destinationOffset:0u
                       size:state_bytes];
    if (sdsm_state) {
      [encoder copyFromBuffer:upload->buffer
                 sourceOffset:upload->sdsm_state_reset_source_offset
                     toBuffer:(id<MTLBuffer>)sdsm_state->resource.metal_buffer
            destinationOffset:0u
                         size:sizeof(VkrMetalPacketSdsmState)];
    }
    return true_v;
  }
  const VkrRgImageUse *read = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *write = vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  if (!read && !write)
    return true_v;
  if (!read || !write)
    return false_v;
  id<MTLTexture> source = vkr_metal_packet_texture(renderer, read->image);
  id<MTLTexture> destination = vkr_metal_packet_texture(renderer, write->image);
  if (!source || !destination)
    return false_v;
  const NSUInteger width = MIN(source.width, destination.width);
  const NSUInteger height = MIN(source.height, destination.height);
  [encoder copyFromTexture:source
               sourceSlice:read->has_slice ? read->slice.base_layer : 0
               sourceLevel:read->has_slice ? read->slice.mip_level : 0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(width, height, 1)
                 toTexture:destination
          destinationSlice:write->has_slice ? write->slice.base_layer : 0
          destinationLevel:write->has_slice ? write->slice.mip_level : 0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
  return true_v;
}

vkr_internal bool8_t
vkr_metal_packet_is_picking_readback(const VkrRgPass *pass) {
  return vkr_metal_packet_graph_pass_is(
      pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_PICKING_READBACK);
}

vkr_internal bool8_t vkr_metal_packet_encode_picking_readback(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    id<MTLBuffer> readback_buffer, uint64_t readback_offset)
    API_AVAILABLE(macos(26.0)) {
  if (!vkr_metal_packet_is_picking_readback(pass))
    return true_v;
  if (!packet || !packet->picking)
    return false_v;
  const VkrRgImageUse *read = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  if (!read)
    return false_v;
  id<MTLTexture> source = vkr_metal_packet_texture(renderer, read->image);
  if (!source)
    return false_v;
  const NSUInteger x = MIN((NSUInteger)packet->picking->x, source.width - 1);
  const NSUInteger y = MIN((NSUInteger)packet->picking->y, source.height - 1);
  [encoder copyFromTexture:source
                   sourceSlice:0
                   sourceLevel:0
                  sourceOrigin:MTLOriginMake(x, y, 0)
                    sourceSize:MTLSizeMake(1, 1, 1)
                      toBuffer:readback_buffer
             destinationOffset:readback_offset
        destinationBytesPerRow:sizeof(uint32_t)
      destinationBytesPerImage:sizeof(uint32_t)
                       options:MTLBlitOptionNone];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_pass_writes_present(
    const VkrMetalPacketRenderer *renderer, const VkrRgPass *pass) {
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    const VkrRgAttachment *attachment =
        vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    if (attachment && attachment->image.id == renderer->graph->present_image.id)
      return true_v;
  }
  return false_v;
}

vkr_internal bool8_t vkr_metal_packet_is_last_present_writer(
    const VkrMetalPacketRenderer *renderer, uint64_t order_index) {
  const uint32_t *pass_index =
      vector_get_uint32_t(&renderer->graph->execution_order, order_index);
  const VkrRgPass *pass =
      vector_get_VkrRgPass(&renderer->graph->passes, *pass_index);
  if (!vkr_metal_packet_pass_writes_present(renderer, pass))
    return false_v;
  for (uint64_t i = order_index + 1;
       i < renderer->graph->execution_order.length; ++i) {
    const uint32_t *future_index =
        vector_get_uint32_t(&renderer->graph->execution_order, i);
    const VkrRgPass *future =
        vector_get_VkrRgPass(&renderer->graph->passes, *future_index);
    if (vkr_metal_packet_pass_writes_present(renderer, future))
      return false_v;
  }
  return true_v;
}
