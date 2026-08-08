/* Render-graph resources, dependency lowering, pass descriptors, and capture
 * plans. */
static void vkr_metal_packet_noop(VkrRgPassContext *ctx, void *user_data) {
  (void)ctx;
  (void)user_data;
}

static bool8_t
vkr_metal_packet_register_executors(VkrMetalPacketRenderer *renderer) {
  static const char *names[] = {
      "pass.shadow.cascade",
      "pass.picking",
      "pass.picking.readback",
      "pass.ibl_bake",
      "pass.skybox",
      "pass.world.opaque",
      "pass.copy.pre_transmission.fullscreen",
      "pass.copy.pre_transmission.editor",
      "pass.world.transmission",
      "pass.world.blend",
      "pass.tonemap",
      "pass.editor",
      "pass.ui",
  };
  for (uint32_t i = 0; i < ArrayCount(names); ++i) {
    VkrRgPassExecutor entry = {
        .name = string8_create_from_cstr((const uint8_t *)names[i],
                                         string_length(names[i])),
        .execute = vkr_metal_packet_noop,
    };
    if (!vkr_rg_executor_registry_register(&renderer->executors, &entry))
      return false_v;
  }
  return true_v;
}

static MTLPixelFormat vkr_metal_packet_pixel_format(VkrTextureFormat format) {
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
  case VKR_TEXTURE_FORMAT_D16_UNORM:
    return MTLPixelFormatDepth16Unorm;
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
    return MTLPixelFormatDepth32Float;
  default:
    return MTLPixelFormatInvalid;
  }
}

static MTLPixelFormat
vkr_metal_packet_target_pixel_format(const VkrMetalPacketRenderer *renderer) {
  if (renderer->target_kind == VKR_METAL_PACKET_TARGET_WINDOW)
    return renderer->srgb_output ? MTLPixelFormatBGRA8Unorm_sRGB
                                 : MTLPixelFormatBGRA8Unorm;
  return renderer->srgb_output ? MTLPixelFormatRGBA8Unorm_sRGB
                               : MTLPixelFormatRGBA8Unorm;
}

static MTLTextureUsage
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

static bool8_t vkr_metal_packet_image_desc_equal(const VkrRgImageDesc *a,
                                                 const VkrRgImageDesc *b) {
  return a->width == b->width && a->height == b->height &&
         a->format == b->format && a->usage.set == b->usage.set &&
         a->samples == b->samples && a->layers == b->layers &&
         a->mip_levels == b->mip_levels && a->type == b->type;
}

static bool8_t vkr_metal_packet_create_image(VkrMetalPacketRenderer *renderer,
                                             const VkrRgImage *image,
                                             VkrMetalPacketImage *slot)
    API_AVAILABLE(macos(26.0)) {
  const MTLPixelFormat format =
      vkr_metal_packet_pixel_format(image->desc.format);
  if (format == MTLPixelFormatInvalid || image->desc.width == 0 ||
      image->desc.height == 0 || image->desc.layers == 0 ||
      image->desc.mip_levels == 0)
    return false_v;

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
  if (status != VKR_METAL_MEMORY_STATUS_OK)
    return false_v;

  *slot = (VkrMetalPacketImage){
      .resource = resource,
      .desc = image->desc,
      .graph_generation = image->generation,
      .live = true_v,
      .owned = true_v,
  };
  return true_v;
}

static bool8_t
vkr_metal_packet_realize_images(VkrMetalPacketRenderer *renderer) {
  if (renderer->graph->images.length > renderer->max_images)
    return false_v;
  for (uint64_t i = 0; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame)
      continue;
    VkrMetalPacketImage *slot = &renderer->images[i];
    if (renderer->target_kind == VKR_METAL_PACKET_TARGET_WINDOW &&
        image->imported && vkr_string8_equals_cstr(&image->name, "swapchain")) {
      if (!renderer->drawable || !renderer->drawable.texture)
        return false_v;
      *slot = (VkrMetalPacketImage){
          .resource =
              {
                  .metal_texture = renderer->drawable.texture,
                  .resource_id = renderer->drawable.texture.gpuResourceID._impl,
              },
          .desc = image->desc,
          .graph_generation = image->generation,
          .live = true_v,
          .owned = false_v,
      };
      continue;
    }
    if (slot->live && slot->graph_generation == image->generation &&
        vkr_metal_packet_image_desc_equal(&slot->desc, &image->desc))
      continue;
    if (slot->live && slot->owned) {
      if (vkr_metal_memory_device_retire(
              renderer->memory, slot->resource.handle,
              renderer->submit_value) != VKR_METAL_MEMORY_STATUS_OK)
        return false_v;
      slot->live = false_v;
      renderer->resize_count++;
    }
    slot->live = false_v;
    if (!vkr_metal_packet_create_image(renderer, image, slot))
      return false_v;
  }
  return true_v;
}

static id<MTLTexture> vkr_metal_packet_texture(VkrMetalPacketRenderer *renderer,
                                               VkrRgImageHandle handle) {
  if (!vkr_rg_image_handle_valid(handle) || handle.id > renderer->max_images)
    return nil;
  VkrMetalPacketImage *slot = &renderer->images[handle.id - 1];
  return slot->live && slot->graph_generation == handle.generation
             ? (id<MTLTexture>)slot->resource.metal_texture
             : nil;
}

static id<MTLTexture>
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

static bool8_t vkr_metal_packet_named_texture_info(
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

static uint64_t vkr_metal_packet_align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static bool8_t
vkr_metal_packet_lighting_valid(const VkrFrameLighting *lighting) {
  if (!lighting)
    return true_v;
  if (!(isfinite(lighting->directional_direction.x) &&
        isfinite(lighting->directional_direction.y) &&
        isfinite(lighting->directional_direction.z) &&
        isfinite(lighting->directional_color.x) &&
        isfinite(lighting->directional_color.y) &&
        isfinite(lighting->directional_color.z) &&
        isfinite(lighting->directional_intensity) &&
        isfinite(lighting->ibl_intensity) &&
        isfinite(lighting->ibl_diffuse_intensity) &&
        isfinite(lighting->ibl_specular_intensity) &&
        lighting->directional_color.x >= 0.0f &&
        lighting->directional_color.y >= 0.0f &&
        lighting->directional_color.z >= 0.0f &&
        lighting->directional_intensity >= 0.0f &&
        lighting->ibl_intensity >= 0.0f &&
        lighting->ibl_diffuse_intensity >= 0.0f &&
        lighting->ibl_specular_intensity >= 0.0f))
    return false_v;
  if (lighting->point_light_count > VKR_MAX_SCENE_POINT_LIGHTS ||
      (lighting->point_light_count > 0 &&
       (!lighting->point_lights || !lighting->point_light_grid)))
    return false_v;
  if (lighting->ibl_source.id != 0 &&
      lighting->ibl_source.generation == VKR_INVALID_ID)
    return false_v;
  if (lighting->ibl_probe_count > VKR_FRAME_IBL_PROBE_MAX ||
      (lighting->ibl_probe_count > 0 && !lighting->ibl_probes))
    return false_v;
  for (uint32_t i = 0; i < lighting->ibl_probe_count; ++i) {
    const VkrFrameIblProbe *probe = &lighting->ibl_probes[i];
    const float32_t values[] = {
        probe->center.x,           probe->center.y,
        probe->center.z,           probe->extents.x,
        probe->extents.y,          probe->extents.z,
        probe->blend_distance,     probe->weight,
        probe->intensity,          probe->diffuse_intensity,
        probe->specular_intensity,
    };
    for (uint32_t value = 0; value < ArrayCount(values); ++value) {
      if (!isfinite(values[value]))
        return false_v;
    }
    if (probe->irradiance.id == 0 || probe->prefilter.id == 0 ||
        probe->irradiance.generation == VKR_INVALID_ID ||
        probe->prefilter.generation == VKR_INVALID_ID ||
        probe->extents.x < 0.0f || probe->extents.y < 0.0f ||
        probe->extents.z < 0.0f || probe->blend_distance < 0.0f ||
        probe->weight < 0.0f || probe->intensity < 0.0f ||
        probe->diffuse_intensity < 0.0f || probe->specular_intensity < 0.0f)
      return false_v;
  }
  if (lighting->point_light_count == 0)
    return true_v;
  const VkrPointLightGrid *grid = lighting->point_light_grid;
  const uint64_t grid_cells =
      (uint64_t)grid->dimensions[0] * grid->dimensions[1] * grid->dimensions[2];
  if (!isfinite(grid->origin.x) || !isfinite(grid->origin.y) ||
      !isfinite(grid->origin.z) || !isfinite(grid->cell_size) ||
      grid->cell_size < 0.0f ||
      grid->cell_count > VKR_POINT_LIGHT_GRID_MAX_CELLS ||
      grid_cells != grid->cell_count)
    return false_v;
  for (uint32_t i = 0; i < lighting->point_light_count; ++i) {
    const VkrPointLight *light = &lighting->point_lights[i];
    const float32_t values[] = {
        light->position.x,       light->position.y,  light->position.z,
        light->color.x,          light->color.y,     light->color.z,
        light->intensity,        light->constant,    light->linear,
        light->quadratic,        light->range,       light->direction.x,
        light->direction.y,      light->direction.z, light->inner_cone_angle,
        light->outer_cone_angle,
    };
    for (uint32_t value = 0; value < ArrayCount(values); ++value) {
      if (!isfinite(values[value]))
        return false_v;
    }
    if (light->color.x < 0.0f || light->color.y < 0.0f ||
        light->color.z < 0.0f || light->intensity < 0.0f ||
        light->range < 0.0f || light->kind > VKR_POINT_LIGHT_KIND_GLTF_SPOT)
      return false_v;
  }
  return true_v;
}

static bool8_t
vkr_metal_packet_text_draws_valid(const VkrPreparedTextDraw *draws,
                                  uint32_t draw_count) {
  if (draw_count > 0 && !draws)
    return false_v;
  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrPreparedTextDraw *draw = &draws[i];
    if (!draw->vertices || draw->vertex_count == 0 || !draw->indices ||
        draw->index_count == 0 || draw->max_index >= draw->vertex_count ||
        draw->atlas.id == 0 || draw->atlas.generation == VKR_INVALID_ID ||
        !isfinite(draw->screen_px_range) || draw->screen_px_range < 0.0f ||
        draw->font_mode > 1u)
      return false_v;
    for (uint32_t element = 0; element < ArrayCount(draw->model.elements);
         ++element) {
      if (!isfinite(draw->model.elements[element]))
        return false_v;
    }
  }
  return true_v;
}

static bool8_t vkr_metal_packet_material_info_valid(
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

static void vkr_metal_packet_decode_half4(const void *source,
                                          float32_t destination[4]) {
  uint16_t encoded[4] = {0};
  MemCopy(encoded, source, sizeof(encoded));
  for (uint32_t i = 0; i < ArrayCount(encoded); ++i)
    destination[i] = vkr_float16_to_float32(encoded[i]);
}

static bool8_t vkr_metal_packet_capture_plan(VkrMetalPacketRenderer *renderer,
                                             const VkrRenderPacket *packet,
                                             uint64_t first_offset,
                                             VkrMetalPacketCapturePlan *plans,
                                             uint32_t *out_count,
                                             uint64_t *out_size) {
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
      source_name =
          packet->frame.editor_enabled ? "scene_color" : "hdr_scene_color";
    else if (string_equals(channel->name, "depth"))
      source_name =
          packet->frame.editor_enabled ? "scene_depth" : "swapchain_depth";
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

static void vkr_metal_packet_collect_pass_timings(
    VkrMetalPacketRenderer *renderer, id<MTL4CounterHeap> timestamp_heap,
    uint32_t entry_count, VkrMetalPacketResult *result)
    API_AVAILABLE(macos(26.0)) {
  if (!renderer || !timestamp_heap || !result || entry_count == 0)
    return;
  NSData *resolved =
      [timestamp_heap resolveCounterRange:NSMakeRange(0, entry_count)];
  if (!resolved ||
      resolved.length < entry_count * sizeof(MTL4TimestampHeapEntry))
    return;
  const MTL4TimestampHeapEntry *entries = resolved.bytes;
  for (uint32_t i = 0; i < result->pass_timing_count; ++i) {
    VkrMetalPacketPassTiming *timing = &result->pass_timings[i];
    const uint32_t begin_index = timing->pass_index * 2u;
    if (begin_index + 1u >= entry_count)
      continue;
    const uint64_t begin = entries[begin_index].timestamp;
    const uint64_t end = entries[begin_index + 1u].timestamp;
    timing->valid = begin > 0 && end >= begin;
    if (timing->valid) {
      timing->gpu_ms = (float64_t)(end - begin) * 1000.0 /
                       (float64_t)renderer->timestamp_frequency;
    }
  }
}

static void
vkr_metal_packet_capture_fail_reserved(VkrMetalPacketRenderer *renderer,
                                       const VkrCaptureBatchRequest *request,
                                       VkrRendererError error) {
  if (request) {
    (void)vkr_metal_capture_ring_fail(&renderer->capture_ring,
                                      request->request_id, error);
  }
}

static MTLLoadAction
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

static MTLStoreAction
vkr_metal_packet_store_action(VkrAttachmentStoreOp operation) {
  return operation == VKR_ATTACHMENT_STORE_OP_STORE ? MTLStoreActionStore
                                                    : MTLStoreActionDontCare;
}

static bool8_t vkr_metal_packet_configure_render_pass(
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

  if (pass->desc.color_attachments.length >
      VKR_METAL_PACKET_MAX_COLOR_ATTACHMENTS)
    return false_v;
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    const VkrRgAttachment *attachment =
        vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    id<MTLTexture> texture =
        vkr_metal_packet_texture(renderer, attachment->image);
    if (!texture)
      return false_v;
    MTLRenderPassColorAttachmentDescriptor *color =
        descriptor.colorAttachments[i];
    color.texture = texture;
    if (descriptor.renderTargetWidth == 0) {
      descriptor.renderTargetWidth = texture.width;
      descriptor.renderTargetHeight = texture.height;
    }
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
    if (!texture)
      return false_v;
    descriptor.depthAttachment.texture = texture;
    if (descriptor.renderTargetWidth == 0) {
      descriptor.renderTargetWidth = texture.width;
      descriptor.renderTargetHeight = texture.height;
    }
    descriptor.depthAttachment.level = attachment->desc.slice.mip_level;
    descriptor.depthAttachment.slice = attachment->desc.slice.base_layer;
    descriptor.depthAttachment.loadAction =
        vkr_metal_packet_load_action(attachment->desc.load_op);
    descriptor.depthAttachment.storeAction =
        vkr_metal_packet_store_action(attachment->desc.store_op);
    descriptor.depthAttachment.clearDepth =
        attachment->desc.clear_value.depth_stencil.depth;
  }
  return pass->desc.color_attachments.length > 0 ||
                 pass->desc.has_depth_attachment
             ? true_v
             : false_v;
}

static bool8_t vkr_metal_packet_encode_dependency(
    id<MTL4CommandEncoder> encoder, const VkrGpuDependency *dependency,
    bool8_t producer) API_AVAILABLE(macos(26.0)) {
  VkrMetalDependency lowered = {0};
  if (!vkr_metal_dependency_lower(dependency, &lowered))
    return false_v;
  return producer ? vkr_metal_dependency_encode_producer(encoder, &lowered)
                  : vkr_metal_dependency_encode_consumer(encoder, &lowered);
}

static bool8_t vkr_metal_packet_encode_consumers(id<MTL4CommandEncoder> encoder,
                                                 const VkrRgPass *pass,
                                                 uint32_t *dependency_count)
    API_AVAILABLE(macos(26.0)) {
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

static bool8_t vkr_metal_packet_has_in_edge(const VkrRgPass *pass,
                                            uint32_t producer_index) {
  for (uint64_t i = 0; i < pass->in_edges.length; ++i) {
    const uint32_t *edge = vector_get_uint32_t(&pass->in_edges, i);
    if (edge && *edge == producer_index)
      return true_v;
  }
  return false_v;
}

static bool8_t vkr_metal_packet_pass_uses_image(const VkrRgPass *pass,
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

static bool8_t vkr_metal_packet_pass_uses_buffer(const VkrRgPass *pass,
                                                 VkrRgBufferHandle buffer) {
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

static bool8_t vkr_metal_packet_encode_producers(
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

static bool8_t vkr_metal_packet_encode_transfer(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    id<MTL4ComputeCommandEncoder> encoder) API_AVAILABLE(macos(26.0)) {
  if (pass->desc.image_reads.length == 0 || pass->desc.image_writes.length == 0)
    return true_v;
  const VkrRgImageUse *read =
      vector_get_VkrRgImageUse(&pass->desc.image_reads, 0);
  const VkrRgImageUse *write =
      vector_get_VkrRgImageUse(&pass->desc.image_writes, 0);
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

static bool8_t vkr_metal_packet_is_picking_readback(const VkrRgPass *pass) {
  return vkr_string8_equals_cstr(&pass->desc.execute_name,
                                 "pass.picking.readback");
}

static bool8_t vkr_metal_packet_encode_picking_readback(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    id<MTLBuffer> readback_buffer, uint64_t readback_offset)
    API_AVAILABLE(macos(26.0)) {
  if (!vkr_metal_packet_is_picking_readback(pass))
    return true_v;
  if (pass->desc.image_reads.length == 0 || !packet || !packet->picking)
    return false_v;
  const VkrRgImageUse *read =
      vector_get_VkrRgImageUse(&pass->desc.image_reads, 0);
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

static bool8_t
vkr_metal_packet_pass_writes_present(const VkrMetalPacketRenderer *renderer,
                                     const VkrRgPass *pass) {
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    const VkrRgAttachment *attachment =
        vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    if (attachment && attachment->image.id == renderer->graph->present_image.id)
      return true_v;
  }
  return false_v;
}

static bool8_t
vkr_metal_packet_is_last_present_writer(const VkrMetalPacketRenderer *renderer,
                                        uint64_t order_index) {
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
