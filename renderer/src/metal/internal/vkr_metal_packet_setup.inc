/* Shader/pipeline creation, device resources, uploads, and renderer startup. */
vkr_internal void vkr_metal_packet_print_error(const char *step,
                                               NSError *error) {
  (void)error;
  fprintf(stderr, "metal.packet.%s: Metal compilation failed\n", step);
}

/* Explicit native resources outside placement heaps share the same managed
 * budget. Opaque driver allocations and externally owned drawables are reported
 * separately. */
vkr_internal void
vkr_metal_packet_release_budgeted_resource(VkrMetalPacketRenderer *renderer,
                                           id<MTLResource> resource) {
  if (!resource)
    return;
  const uint64_t allocated = resource.allocatedSize;
  [resource release];
  vkr_metal_memory_device_release_external(renderer->memory, allocated);
}

vkr_internal id<MTLLibrary>
vkr_metal_packet_create_library(VkrMetalPacketRenderer *renderer,
                                const char *path, const char *metallib_path,
                                NSString *name) API_AVAILABLE(macos(26.0)) {
  NSError *error = nil;
  /* A catalog metallib is compiled offline from this same source by
   * `vkr_bakery shaders`; failure to load it falls back to the source. */
  if (metallib_path && metallib_path[0]) {
    NSURL *url =
        [NSURL fileURLWithPath:[NSString stringWithUTF8String:metallib_path]];
    id<MTLLibrary> precompiled =
        [renderer->device newLibraryWithURL:url error:&error];
    if (precompiled) {
      precompiled.label = name;
      return precompiled;
    }
    vkr_metal_packet_print_error("metallib_load", error);
    error = nil;
  }
  NSString *source =
      [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:path]
                                encoding:NSUTF8StringEncoding
                                   error:&error];
  if (!source) {
    vkr_metal_packet_print_error("shader_read", error);
    return nil;
  }
  MTL4LibraryDescriptor *descriptor = [MTL4LibraryDescriptor new];
  descriptor.name = name;
  descriptor.source = source;
  id<MTLLibrary> library =
      [renderer->compiler newLibraryWithDescriptor:descriptor error:&error];
  [descriptor release];
  if (!library) {
    NSError *diagnostic_error = nil;
    id<MTLLibrary> diagnostic =
        [renderer->device newLibraryWithSource:source
                                       options:nil
                                         error:&diagnostic_error];
    if (!diagnostic && diagnostic_error) {
      fprintf(stderr, "metal.packet.library_diagnostic: %s\n",
              diagnostic_error.localizedDescription.UTF8String);
    }
    [diagnostic release];
    vkr_metal_packet_print_error("library_compile", error);
  }
  return library;
}

vkr_internal uint32_t
vkr_metal_packet_abi_binding_index(VkrMetalPacketAbiRecordId record_id) {
  switch (record_id) {
  case VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT:
  case VKR_METAL_PACKET_ABI_ATMOSPHERE_ROOT:
  case VKR_METAL_PACKET_ABI_PREFILTER_ROOT:
  case VKR_METAL_PACKET_ABI_SH_PROJECT_ROOT:
  case VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT:
  case VKR_METAL_PACKET_ABI_SKINNING_ROOT:
  case VKR_METAL_PACKET_ABI_SKY_VIEW_ROOT:
  case VKR_METAL_PACKET_ABI_AERIAL_PERSPECTIVE_ROOT:
  case VKR_METAL_PACKET_ABI_CLOUD_NOISE_ROOT:
  case VKR_METAL_PACKET_ABI_CLOUD_SHADOW_ROOT:
  case VKR_METAL_PACKET_ABI_CLOUD_TRACE_ROOT:
  case VKR_METAL_PACKET_ABI_CLOUD_SKY_LIGHT_ROOT:
  case VKR_METAL_PACKET_ABI_HZB_BUILD_ROOT:
  case VKR_METAL_PACKET_ABI_EXPOSURE_ROOT:
  case VKR_METAL_PACKET_ABI_BLOOM_ROOT:
    return 0;
  default:
    return 1;
  }
}

/* Slang may append a collision-disambiguation decimal suffix after an
   otherwise exact native field name. Match that emitted spelling only when
   the reflected record contains one unambiguous base_N candidate. */
vkr_internal MTLStructMember *
vkr_metal_packet_reflected_member(MTLStructType *type, const char *base_name)
    API_AVAILABLE(macos(26.0)) {
  if (!type || !base_name)
    return nil;
  NSString *base = [NSString stringWithUTF8String:base_name];
  MTLStructMember *exact = [type memberByName:base];
  if (exact)
    return exact;
  NSString *prefix = [base stringByAppendingString:@"_"];
  const NSUInteger prefix_length = prefix.length;
  MTLStructMember *match = nil;
  for (MTLStructMember *candidate in type.members) {
    NSString *name = candidate.name;
    if (![name hasPrefix:prefix] || name.length == prefix_length)
      continue;
    bool8_t decimal_suffix = true_v;
    for (NSUInteger index = prefix_length; index < name.length; ++index) {
      const unichar character = [name characterAtIndex:index];
      if (character < '0' || character > '9') {
        decimal_suffix = false_v;
        break;
      }
    }
    if (!decimal_suffix)
      continue;
    if (match)
      return nil;
    match = candidate;
  }
  return match;
}

vkr_internal bool8_t vkr_metal_packet_validate_abi_fields(
    NSString *pipeline_label, MTLStructType *shader_type,
    const VkrMetalPacketAbiRecord *record) API_AVAILABLE(macos(26.0)) {
  bool8_t valid = true_v;
  for (uint32_t i = 0; i < record->field_count; ++i) {
    const VkrMetalPacketAbiField *field = &record->fields[i];
    MTLStructMember *member =
        vkr_metal_packet_reflected_member(shader_type, field->shader_name);
    if (!member || member.offset != field->expected_offset) {
      log_error("Metal ABI mismatch for %s/%s.%s: shader offset %lld, host "
                "authority %u",
                pipeline_label.UTF8String, record->shader_name,
                field->shader_name,
                member ? (long long)member.offset : (long long)-1,
                field->expected_offset);
      valid = false_v;
    }
  }
  return valid;
}

vkr_internal bool8_t vkr_metal_packet_validate_abi_type(
    NSString *pipeline_label, MTLStructType *shader_type,
    NSUInteger shader_size, NSUInteger shader_alignment,
    VkrMetalPacketAbiRecordId record_id) API_AVAILABLE(macos(26.0)) {
  const VkrMetalPacketAbiRecord *record =
      vkr_metal_packet_abi_record(record_id);
  if (!record || !shader_type) {
    log_error("Metal ABI type missing for %s/%s", pipeline_label.UTF8String,
              record ? record->shader_name : "unknown");
    return false_v;
  }
  const bool8_t alignment_compatible =
      vkr_metal_packet_abi_alignment_compatible(record_id,
                                                (uint32_t)shader_alignment);
  bool8_t valid = true_v;
  if (shader_size != record->expected_size || !alignment_compatible) {
    log_error(
        "Metal ABI mismatch for %s/%s: shader size/min alignment %llu/%llu, "
        "host authority %u/%u",
        pipeline_label.UTF8String, record->shader_name,
        (unsigned long long)shader_size, (unsigned long long)shader_alignment,
        record->expected_size, record->expected_alignment);
    valid = false_v;
  }
  if (!vkr_metal_packet_validate_abi_fields(pipeline_label, shader_type,
                                            record))
    valid = false_v;
  return valid;
}

vkr_internal bool8_t vkr_metal_packet_validate_nested_abi(
    NSString *pipeline_label, MTLStructType *root_type, const char *member_name,
    VkrMetalPacketAbiRecordId record_id) API_AVAILABLE(macos(26.0)) {
  MTLStructMember *member =
      vkr_metal_packet_reflected_member(root_type, member_name);
  MTLPointerType *pointer = member ? member.pointerType : nil;
  if (pointer)
    return vkr_metal_packet_validate_abi_type(
        pipeline_label, pointer.elementStructType, pointer.dataSize,
        pointer.alignment, record_id);

  const VkrMetalPacketAbiRecord *record =
      vkr_metal_packet_abi_record(record_id);
  MTLStructType *embedded = member ? member.structType : nil;
  if (!record || !embedded) {
    log_error("Metal nested ABI type missing for %s/%s",
              pipeline_label.UTF8String, member_name);
    return false_v;
  }
  return vkr_metal_packet_validate_abi_fields(pipeline_label, embedded, record);
}

vkr_internal bool8_t vkr_metal_packet_validate_nested_gpu_abi(
    NSString *pipeline_label, MTLStructType *root_type, const char *member_name,
    VkrGpuAbiRecordId record_id) API_AVAILABLE(macos(26.0)) {
  MTLStructMember *member =
      vkr_metal_packet_reflected_member(root_type, member_name);
  MTLPointerType *pointer = member ? member.pointerType : nil;
  const VkrGpuAbiRecord *record = vkr_gpu_abi_record(record_id);
  if (!member || !pointer || !record || !pointer.elementStructType ||
      pointer.dataSize != record->expected_size) {
    log_error("Metal shared GPU ABI type missing for %s/%s",
              pipeline_label.UTF8String,
              record ? record->shader_name : "unknown");
    return false_v;
  }
  bool8_t valid = true_v;
  for (uint32_t i = 0u; i < record->field_count; ++i) {
    const VkrGpuAbiField *field = &record->fields[i];
    MTLStructMember *field_member = vkr_metal_packet_reflected_member(
        pointer.elementStructType, field->shader_name);
    if (!field_member || field_member.offset != field->expected_offset) {
      log_error("Metal shared GPU ABI mismatch for %s/%s.%s",
                pipeline_label.UTF8String, record->shader_name,
                field->shader_name);
      valid = false_v;
    }
  }
  return valid;
}

vkr_internal MTLStructType *
vkr_metal_packet_nested_struct(MTLStructType *root_type,
                               const char *member_name)
    API_AVAILABLE(macos(26.0)) {
  MTLStructMember *member =
      vkr_metal_packet_reflected_member(root_type, member_name);
  return member ? member.pointerType.elementStructType : nil;
}

/* The frame-slot sky record and its embedded parameters. */
vkr_internal bool8_t vkr_metal_packet_validate_sky_abi(NSString *pipeline_label,
                                                       MTLStructType *root_type)
    API_AVAILABLE(macos(26.0)) {
  MTLStructType *sky_type = vkr_metal_packet_nested_struct(root_type, "sky");
  /* The parameter record is embedded in the sky record, not addressed. */
  MTLStructMember *params =
      vkr_metal_packet_reflected_member(sky_type, "params");
  return vkr_metal_packet_validate_nested_abi(pipeline_label, root_type, "sky",
                                              VKR_METAL_PACKET_ABI_SKY) &&
         vkr_metal_packet_validate_nested_abi(
             pipeline_label, sky_type, "params",
             VKR_METAL_PACKET_ABI_SKY_PARAMS) &&
         vkr_metal_packet_validate_nested_abi(
             pipeline_label, params ? params.structType : nil, "clouds",
             VKR_METAL_PACKET_ABI_CLOUD_PARAMS);
}

vkr_internal bool8_t vkr_metal_packet_validate_abi_binding(
    NSString *pipeline_label, NSArray<id<MTLBinding>> *bindings,
    VkrMetalPacketAbiRecordId record_id) API_AVAILABLE(macos(26.0)) {
  if (record_id == VKR_METAL_PACKET_ABI_RECORD_COUNT)
    return true_v;
  const uint32_t binding_index = vkr_metal_packet_abi_binding_index(record_id);
  id<MTLBufferBinding> root = nil;
  for (id<MTLBinding> binding in bindings) {
    if (binding.type == MTLBindingTypeBuffer &&
        binding.index == binding_index) {
      root = (id<MTLBufferBinding>)binding;
      break;
    }
  }
  if (!root || !vkr_metal_packet_validate_abi_type(
                   pipeline_label, root.bufferStructType, root.bufferDataSize,
                   root.bufferAlignment, record_id)) {
    if (!root)
      log_error("Metal ABI root binding %u missing for %s", binding_index,
                pipeline_label.UTF8String);
    return false_v;
  }
  if (record_id == VKR_METAL_PACKET_ABI_TONEMAP_ROOT) {
    if (!vkr_metal_packet_validate_nested_gpu_abi(
            pipeline_label, root.bufferStructType, "color_grading",
            VKR_GPU_ABI_COLOR_GRADING) ||
        !vkr_metal_packet_validate_nested_abi(
            pipeline_label, root.bufferStructType, "display_output",
            VKR_METAL_PACKET_ABI_DISPLAY_OUTPUT_PARAMS))
      return false_v;
  }
  if ((record_id == VKR_METAL_PACKET_ABI_UI_ROOT ||
       record_id == VKR_METAL_PACKET_ABI_EDITOR_OVERLAY_ROOT) &&
      !vkr_metal_packet_validate_nested_abi(
          pipeline_label, root.bufferStructType, "display_output",
          VKR_METAL_PACKET_ABI_DISPLAY_OUTPUT_PARAMS))
    return false_v;
  if (record_id == VKR_METAL_PACKET_ABI_UI_ROOT &&
      !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                            root.bufferStructType, "vertices",
                                            VKR_METAL_PACKET_ABI_UI_VERTEX))
    return false_v;
  if (record_id == VKR_METAL_PACKET_ABI_DRAW_ROOT ||
      record_id == VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT) {
    if (!vkr_metal_packet_validate_nested_gpu_abi(
            pipeline_label, root.bufferStructType, "geometry_rows",
            VKR_GPU_ABI_GEOMETRY_ROW) ||
        !vkr_metal_packet_validate_nested_gpu_abi(
            pipeline_label, root.bufferStructType, "visible_rows",
            VKR_GPU_ABI_VISIBLE_DRAW_ROW) ||
        !vkr_metal_packet_validate_nested_gpu_abi(
            pipeline_label, root.bufferStructType, "vertices",
            VKR_GPU_ABI_PACKED_STATIC_VERTEX) ||
        !vkr_metal_packet_validate_nested_abi(
            pipeline_label, root.bufferStructType, "frame",
            VKR_METAL_PACKET_ABI_FRAME_ROOT) ||
        !vkr_metal_packet_validate_nested_abi(
            pipeline_label, root.bufferStructType, "local_shadow_transmission",
            VKR_METAL_PACKET_ABI_LOCAL_SHADOW_TRANSMISSION_DRAW))
      return false_v;
    MTLStructType *frame_type =
        vkr_metal_packet_nested_struct(root.bufferStructType, "frame");
    if (!frame_type ||
        !vkr_metal_packet_validate_nested_abi(
            pipeline_label, frame_type, "diffuse_volume_params",
            VKR_METAL_PACKET_ABI_DIFFUSE_VOLUME) ||
        !vkr_metal_packet_validate_nested_abi(pipeline_label, frame_type,
                                              "lightmap",
                                              VKR_METAL_PACKET_ABI_LIGHTMAP) ||
        !vkr_metal_packet_validate_nested_abi(pipeline_label, frame_type,
                                              "decals",
                                              VKR_METAL_PACKET_ABI_DECALS) ||
        !vkr_metal_packet_validate_nested_abi(
            pipeline_label, frame_type, "fog",
            VKR_METAL_PACKET_ABI_FOG_PARAMS) ||
        !vkr_metal_packet_validate_nested_abi(
            pipeline_label, frame_type, "froxel_fog",
            VKR_METAL_PACKET_ABI_FROXEL_PARAMS) ||
        !vkr_metal_packet_validate_nested_abi(pipeline_label, frame_type, "ltc",
                                              VKR_METAL_PACKET_ABI_LTC) ||
        !vkr_metal_packet_validate_nested_abi(
            pipeline_label, frame_type, "sheen", VKR_METAL_PACKET_ABI_SHEEN) ||
        !vkr_metal_packet_validate_nested_abi(
            pipeline_label, frame_type, "anisotropy",
            VKR_METAL_PACKET_ABI_ANISOTROPY) ||
        !vkr_metal_packet_validate_nested_gpu_abi(
            pipeline_label, vkr_metal_packet_nested_struct(frame_type, "ltc"),
            "lights", VKR_GPU_ABI_RECTANGLE_LIGHT_ROW) ||
        !vkr_metal_packet_validate_nested_gpu_abi(
            pipeline_label, frame_type, "point_light_data",
            VKR_GPU_ABI_POINT_LIGHT_ROW) ||
        !vkr_metal_packet_validate_nested_abi(
            pipeline_label, frame_type, "local_shadow_transmission",
            VKR_METAL_PACKET_ABI_LOCAL_SHADOW_TRANSMISSION) ||
        !vkr_metal_packet_validate_nested_gpu_abi(
            pipeline_label, frame_type, "local_shadow_views",
            VKR_GPU_ABI_LOCAL_SHADOW_VIEW) ||
        !vkr_metal_packet_validate_nested_abi(pipeline_label, frame_type,
                                              "instances",
                                              VKR_METAL_PACKET_ABI_INSTANCE) ||
        !vkr_metal_packet_validate_nested_abi(pipeline_label, frame_type,
                                              "ibl_probes",
                                              VKR_METAL_PACKET_ABI_IBL_PROBE) ||
        !vkr_metal_packet_validate_nested_abi(
            pipeline_label, frame_type, "shadow_cascades",
            VKR_METAL_PACKET_ABI_SHADOW_CASCADE) ||
        !vkr_metal_packet_validate_sky_abi(pipeline_label, frame_type) ||
        (record_id == VKR_METAL_PACKET_ABI_DRAW_ROOT &&
         (!vkr_metal_packet_validate_nested_abi(
              pipeline_label, frame_type, "materials",
              VKR_METAL_PACKET_ABI_MATERIAL) ||
          /* Only MSL reads the decal rows; Slang keeps their address. */
          !vkr_metal_packet_validate_nested_abi(
              pipeline_label,
              vkr_metal_packet_nested_struct(frame_type, "decals"), "rows",
              VKR_METAL_PACKET_ABI_DECAL))))
      return false_v;
  }
  if (record_id == VKR_METAL_PACKET_ABI_EDITOR_OVERLAY_ROOT &&
      (!vkr_metal_packet_validate_nested_gpu_abi(
           pipeline_label, root.bufferStructType, "vertices",
           VKR_GPU_ABI_PACKED_STATIC_VERTEX) ||
       !vkr_metal_packet_validate_nested_gpu_abi(
           pipeline_label, root.bufferStructType, "decode",
           VKR_GPU_ABI_GEOMETRY_DECODE_RECORD)))
    return false_v;
  if (record_id == VKR_METAL_PACKET_ABI_TEXT_ROOT &&
      !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                            root.bufferStructType, "vertices",
                                            VKR_METAL_PACKET_ABI_TEXT_VERTEX))
    return false_v;
  if (record_id == VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT &&
      (!vkr_metal_packet_validate_nested_abi(
           pipeline_label, root.bufferStructType, "views",
           VKR_METAL_PACKET_ABI_GPU_DRAW_VIEW) ||
       !vkr_metal_packet_validate_nested_gpu_abi(
           pipeline_label, root.bufferStructType, "candidates",
           VKR_GPU_ABI_CANDIDATE_DRAW_ROW) ||
       !vkr_metal_packet_validate_nested_gpu_abi(
           pipeline_label, root.bufferStructType, "geometry_rows",
           VKR_GPU_ABI_GEOMETRY_ROW) ||
       !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                             root.bufferStructType, "instances",
                                             VKR_METAL_PACKET_ABI_INSTANCE) ||
       !vkr_metal_packet_validate_nested_gpu_abi(
           pipeline_label, root.bufferStructType, "visible_rows",
           VKR_GPU_ABI_VISIBLE_DRAW_ROW) ||
       !vkr_metal_packet_validate_nested_abi(
           pipeline_label, root.bufferStructType, "draw_roots",
           VKR_METAL_PACKET_ABI_DRAW_ROOT)))
    return false_v;
  if ((record_id == VKR_METAL_PACKET_ABI_SKY_VIEW_ROOT ||
       record_id == VKR_METAL_PACKET_ABI_AERIAL_PERSPECTIVE_ROOT ||
       record_id == VKR_METAL_PACKET_ABI_CLOUD_SHADOW_ROOT ||
       record_id == VKR_METAL_PACKET_ABI_CLOUD_TRACE_ROOT ||
       record_id == VKR_METAL_PACKET_ABI_CLOUD_SKY_LIGHT_ROOT) &&
      !vkr_metal_packet_validate_sky_abi(pipeline_label, root.bufferStructType))
    return false_v;
  if ((record_id == VKR_METAL_PACKET_ABI_CLOUD_TRACE_ROOT ||
       record_id == VKR_METAL_PACKET_ABI_CLOUD_SKY_LIGHT_ROOT) &&
      !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                            root.bufferStructType, "frame",
                                            VKR_METAL_PACKET_ABI_FRAME_ROOT))
    return false_v;
  return true_v;
}

/* How a pipeline's first color target blends. */
typedef enum VkrMetalPacketBlend {
  VKR_METAL_PACKET_BLEND_NONE = 0,
  /* Source alpha over the destination. */
  VKR_METAL_PACKET_BLEND_ALPHA,
  /* The first output plus the destination times the second output. */
  VKR_METAL_PACKET_BLEND_BEHIND,
  /* No blending; the first output's alpha selects the samples written
     (alpha to coverage) and is stored as one. */
  VKR_METAL_PACKET_BLEND_COVERAGE,
} VkrMetalPacketBlend;

vkr_internal id<MTLRenderPipelineState>
vkr_metal_packet_create_pipeline_with_targets(
    VkrMetalPacketRenderer *renderer, id<MTLLibrary> vertex_library,
    const char *vertex_name, id<MTLLibrary> fragment_library,
    const char *fragment_name, MTLPixelFormat color_format,
    VkrMetalPacketBlend blend, VkrMetalPacketAbiRecordId vertex_abi,
    VkrMetalPacketAbiRecordId fragment_abi, bool8_t supports_icb,
    uint32_t sample_count, NSString *label) API_AVAILABLE(macos(26.0)) {
  MTL4LibraryFunctionDescriptor *vertex = [MTL4LibraryFunctionDescriptor new];
  vertex.library = vertex_library;
  vertex.name = [NSString stringWithUTF8String:vertex_name];
  // A null fragment name builds a depth-only pipeline. Metal's tiler takes a
  // materially faster path when there is no fragment stage at all, which a
  // shadow cascade that never shades anything should be using.
  MTL4LibraryFunctionDescriptor *fragment = nil;
  if (fragment_name) {
    fragment = [MTL4LibraryFunctionDescriptor new];
    fragment.library = fragment_library;
    fragment.name = [NSString stringWithUTF8String:fragment_name];
  }
  MTL4RenderPipelineDescriptor *descriptor = [MTL4RenderPipelineDescriptor new];
  descriptor.label = label;
  descriptor.vertexFunctionDescriptor = vertex;
  descriptor.fragmentFunctionDescriptor = fragment;
  MTL4PipelineOptions *options = [MTL4PipelineOptions new];
  options.shaderReflection =
      MTL4ShaderReflectionBindingInfo | MTL4ShaderReflectionBufferTypeInfo;
  descriptor.options = options;
  [options release];
  descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
  descriptor.supportIndirectCommandBuffers =
      supports_icb ? MTL4IndirectCommandBufferSupportStateEnabled
                   : MTL4IndirectCommandBufferSupportStateDisabled;
  descriptor.rasterSampleCount = sample_count;
  if (color_format != MTLPixelFormatInvalid) {
    descriptor.colorAttachments[0].pixelFormat = color_format;
    if (blend == VKR_METAL_PACKET_BLEND_ALPHA) {
      descriptor.colorAttachments[0].blendingState = MTL4BlendStateEnabled;
      descriptor.colorAttachments[0].sourceRGBBlendFactor =
          MTLBlendFactorSourceAlpha;
      descriptor.colorAttachments[0].destinationRGBBlendFactor =
          MTLBlendFactorOneMinusSourceAlpha;
      descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
      descriptor.colorAttachments[0].destinationAlphaBlendFactor =
          MTLBlendFactorOneMinusSourceAlpha;
    } else if (blend == VKR_METAL_PACKET_BLEND_BEHIND) {
      /* Dual-source: the fragment's second output is the factor the
         destination keeps per channel; destination alpha is kept. */
      descriptor.colorAttachments[0].blendingState = MTL4BlendStateEnabled;
      descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
      descriptor.colorAttachments[0].destinationRGBBlendFactor =
          MTLBlendFactorSource1Color;
      descriptor.colorAttachments[0].sourceAlphaBlendFactor =
          MTLBlendFactorZero;
      descriptor.colorAttachments[0].destinationAlphaBlendFactor =
          MTLBlendFactorOne;
    } else if (blend == VKR_METAL_PACKET_BLEND_COVERAGE) {
      descriptor.alphaToCoverageState = MTL4AlphaToCoverageStateEnabled;
      descriptor.alphaToOneState = MTL4AlphaToOneStateEnabled;
    }
  }
  NSError *error = nil;
  id<MTLRenderPipelineState> pipeline = [renderer->compiler
      newRenderPipelineStateWithDescriptor:descriptor
                       compilerTaskOptions:renderer->compiler_options
                                     error:&error];
  if (!pipeline)
    vkr_metal_packet_print_error("pipeline_compile", error);
  if (pipeline && (!vkr_metal_packet_validate_abi_binding(
                       label, pipeline.reflection.vertexBindings, vertex_abi) ||
                   (fragment && !vkr_metal_packet_validate_abi_binding(
                                    label, pipeline.reflection.fragmentBindings,
                                    fragment_abi)))) {
    [pipeline release];
    pipeline = nil;
  }
  [descriptor release];
  [fragment release];
  [vertex release];
  return pipeline;
}

vkr_internal id<MTLRenderPipelineState> vkr_metal_packet_create_pipeline(
    VkrMetalPacketRenderer *renderer, id<MTLLibrary> vertex_library,
    const char *vertex_name, id<MTLLibrary> fragment_library,
    const char *fragment_name, MTLPixelFormat color_format,
    VkrMetalPacketBlend blend, VkrMetalPacketAbiRecordId vertex_abi,
    VkrMetalPacketAbiRecordId fragment_abi, bool8_t supports_icb,
    NSString *label) API_AVAILABLE(macos(26.0)) {
  return vkr_metal_packet_create_pipeline_with_targets(
      renderer, vertex_library, vertex_name, fragment_library, fragment_name,
      color_format, blend, vertex_abi, fragment_abi, supports_icb, 1u, label);
}

vkr_internal id<MTLComputePipelineState>
vkr_metal_packet_create_compute_pipeline(VkrMetalPacketRenderer *renderer,
                                         id<MTLLibrary> library,
                                         const char *function_name,
                                         VkrMetalPacketAbiRecordId abi,
                                         bool8_t supports_icb, NSString *label)
    API_AVAILABLE(macos(26.0)) {
  MTL4LibraryFunctionDescriptor *function = [MTL4LibraryFunctionDescriptor new];
  function.library = library;
  function.name = [NSString stringWithUTF8String:function_name];
  MTL4ComputePipelineDescriptor *descriptor =
      [MTL4ComputePipelineDescriptor new];
  descriptor.label = label;
  descriptor.computeFunctionDescriptor = function;
  descriptor.supportIndirectCommandBuffers =
      supports_icb ? MTL4IndirectCommandBufferSupportStateEnabled
                   : MTL4IndirectCommandBufferSupportStateDisabled;
  MTL4PipelineOptions *options = [MTL4PipelineOptions new];
  options.shaderReflection =
      MTL4ShaderReflectionBindingInfo | MTL4ShaderReflectionBufferTypeInfo;
  descriptor.options = options;
  [options release];
  NSError *error = nil;
  id<MTLComputePipelineState> pipeline = [renderer->compiler
      newComputePipelineStateWithDescriptor:descriptor
                        compilerTaskOptions:renderer->compiler_options
                                      error:&error];
  if (!pipeline)
    vkr_metal_packet_print_error("compute_pipeline_compile", error);
  if (pipeline && !vkr_metal_packet_validate_abi_binding(
                      label, pipeline.reflection.bindings, abi)) {
    [pipeline release];
    pipeline = nil;
  }
  [descriptor release];
  [function release];
  return pipeline;
}

typedef struct VkrMetalPacketPresentationPipelines {
  id<MTLRenderPipelineState> ui;
  id<MTLRenderPipelineState> editor_overlay;
  id<MTLRenderPipelineState> editor_selection_outline;
  id<MTLRenderPipelineState> editor_grid;
  id<MTLRenderPipelineState> tonemap;
} VkrMetalPacketPresentationPipelines;

vkr_internal void vkr_metal_packet_release_presentation_pipelines(
    VkrMetalPacketPresentationPipelines *pipelines) API_AVAILABLE(macos(26.0)) {
  [pipelines->ui release];
  [pipelines->editor_overlay release];
  [pipelines->editor_selection_outline release];
  [pipelines->editor_grid release];
  [pipelines->tonemap release];
  *pipelines = (VkrMetalPacketPresentationPipelines){0};
}

vkr_internal bool8_t vkr_metal_packet_create_presentation_pipelines(
    VkrMetalPacketRenderer *renderer, id<MTLLibrary> fragment,
    MTLPixelFormat target_format,
    VkrMetalPacketPresentationPipelines *out_pipelines)
    API_AVAILABLE(macos(26.0)) {
  *out_pipelines = (VkrMetalPacketPresentationPipelines){0};
  out_pipelines->ui = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_ui_vertex", fragment,
      "vkr_metal_packet_ui_fragment", target_format,
      VKR_METAL_PACKET_BLEND_ALPHA, VKR_METAL_PACKET_ABI_UI_ROOT,
      VKR_METAL_PACKET_ABI_UI_ROOT, false_v, @"VKR Metal packet UI stream");
  out_pipelines->editor_overlay = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_editor_overlay_vertex", fragment,
      "vkr_metal_packet_editor_overlay_fragment", target_format,
      VKR_METAL_PACKET_BLEND_NONE, VKR_METAL_PACKET_ABI_EDITOR_OVERLAY_ROOT,
      VKR_METAL_PACKET_ABI_RECORD_COUNT, false_v, @"VKR Metal editor overlay");
  out_pipelines->editor_selection_outline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_selection_outline_vertex", fragment,
      "vkr_metal_packet_selection_outline_fragment", target_format,
      VKR_METAL_PACKET_BLEND_ALPHA, VKR_METAL_PACKET_ABI_RECORD_COUNT,
      VKR_METAL_PACKET_ABI_SELECTION_OUTLINE_ROOT, false_v,
      @"VKR Metal editor selection outline");
  out_pipelines->editor_grid = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_editor_grid_vertex", fragment,
      "vkr_metal_packet_editor_grid_fragment", target_format,
      VKR_METAL_PACKET_BLEND_ALPHA, VKR_METAL_PACKET_ABI_RECORD_COUNT,
      VKR_METAL_PACKET_ABI_EDITOR_GRID_ROOT, false_v, @"VKR Metal editor grid");
  out_pipelines->tonemap = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_tonemap_vertex", fragment,
      "vkr_metal_packet_tonemap_fragment", target_format,
      VKR_METAL_PACKET_BLEND_NONE, VKR_METAL_PACKET_ABI_RECORD_COUNT,
      VKR_METAL_PACKET_ABI_TONEMAP_ROOT, false_v, @"VKR Metal packet tonemap");
  if (out_pipelines->ui && out_pipelines->editor_overlay &&
      out_pipelines->editor_selection_outline && out_pipelines->editor_grid &&
      out_pipelines->tonemap)
    return true_v;
  vkr_metal_packet_release_presentation_pipelines(out_pipelines);
  return false_v;
}

vkr_internal void vkr_metal_packet_install_presentation_pipelines(
    VkrMetalPacketRenderer *renderer,
    VkrMetalPacketPresentationPipelines *pipelines) API_AVAILABLE(macos(26.0)) {
  [renderer->ui_pipeline release];
  [renderer->editor_overlay_pipeline release];
  [renderer->editor_selection_outline_pipeline release];
  [renderer->editor_grid_pipeline release];
  [renderer->tonemap_pipeline release];
  renderer->ui_pipeline = pipelines->ui;
  renderer->editor_overlay_pipeline = pipelines->editor_overlay;
  renderer->editor_selection_outline_pipeline =
      pipelines->editor_selection_outline;
  renderer->editor_grid_pipeline = pipelines->editor_grid;
  renderer->tonemap_pipeline = pipelines->tonemap;
  *pipelines = (VkrMetalPacketPresentationPipelines){0};
}

vkr_internal VkrDisplayOutputParams vkr_metal_packet_window_output_capability(
    const VkrMetalPacketRenderer *renderer, VkrDisplayOutputParams output)
    API_AVAILABLE(macos(26.0)) {
  if (output.extended_linear == 0u ||
      renderer->target_kind != VKR_METAL_PACKET_TARGET_WINDOW)
    return output;
  CGColorSpaceRef color_space =
      CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
  if (color_space) {
    CGColorSpaceRelease(color_space);
    return output;
  }
  return (VkrDisplayOutputParams){.headroom = 1.0f, .output_scale = 1.0f};
}

vkr_internal bool8_t vkr_metal_packet_configure_window_output(
    VkrMetalPacketRenderer *renderer, VkrDisplayOutputParams output)
    API_AVAILABLE(macos(26.0)) {
  if (renderer->target_kind != VKR_METAL_PACKET_TARGET_WINDOW)
    return output.extended_linear == 0u;
  if (!renderer->layer)
    return false_v;
  if (output.extended_linear != 0u) {
    CGColorSpaceRef color_space =
        CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    if (!color_space)
      return false_v;
    renderer->layer.pixelFormat = MTLPixelFormatRGBA16Float;
    renderer->layer.colorspace = color_space;
    CGColorSpaceRelease(color_space);
    renderer->layer.preferredDynamicRange = CADynamicRangeHigh;
    return true_v;
  }
  renderer->layer.pixelFormat =
      vkr_metal_packet_target_pixel_format_for(renderer, false_v);
  renderer->layer.colorspace = nil;
  renderer->layer.preferredDynamicRange = CADynamicRangeStandard;
  return true_v;
}

/* Creates the shadow, depth-clear and animation preview raster pipelines. A
   failure leaves nil for the startup checks. */
vkr_internal void vkr_metal_packet_create_scene_raster_pipelines(
    VkrMetalPacketRenderer *renderer, id<MTLLibrary> slang,
    id<MTLLibrary> fragment) API_AVAILABLE(macos(26.0)) {
  renderer->animation_preview_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_animation_preview_vertex", fragment,
      "vkr_metal_packet_animation_preview_fragment", MTLPixelFormatRGBA8Unorm,
      VKR_METAL_PACKET_BLEND_NONE, VKR_METAL_PACKET_ABI_ANIMATION_PREVIEW_ROOT,
      VKR_METAL_PACKET_ABI_RECORD_COUNT, false_v, @"VKR animation preview");
  renderer->gpu_shadow_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", fragment,
      "vkr_metal_packet_gpu_shadow_fragment", MTLPixelFormatInvalid,
      VKR_METAL_PACKET_BLEND_NONE, VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
      VKR_METAL_PACKET_ABI_DRAW_ROOT, true_v, @"VKR Metal packet GPU shadow");
  // Opaque cascade geometry never alpha-tests, so it needs no fragment stage.
  renderer->gpu_shadow_opaque_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", nil, NULL,
      MTLPixelFormatInvalid, VKR_METAL_PACKET_BLEND_NONE,
      VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT, VKR_METAL_PACKET_ABI_DRAW_ROOT,
      true_v, @"VKR Metal packet GPU shadow opaque");
  renderer->depth_clear_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_depth_clear_vertex", nil, NULL,
      MTLPixelFormatInvalid, VKR_METAL_PACKET_BLEND_NONE,
      VKR_METAL_PACKET_ABI_RECORD_COUNT, VKR_METAL_PACKET_ABI_RECORD_COUNT,
      false_v, @"VKR Metal packet depth clear");
}

/* The tiled shading variants' entry points. Each frame takes the variant
   for its dynamic lights, decals and reflection probes; the code a frame
   does not need would still cost it registers. Opaque entries are by decal,
   then probe variant; blend entries by probe variant. */
typedef struct VkrMetalTiledEntries {
  const char *forward[VKR_METAL_TILED_DECAL_VARIANT_COUNT]
                     [VKR_METAL_TILED_PROBE_VARIANT_COUNT];
  const char *coverage[VKR_METAL_TILED_DECAL_VARIANT_COUNT]
                      [VKR_METAL_TILED_PROBE_VARIANT_COUNT];
  const char *blend[VKR_METAL_TILED_PROBE_VARIANT_COUNT];
  NSString *forward_label;
  NSString *coverage_label;
  NSString *blend_label;
} VkrMetalTiledEntries;

/* A Custom graph's forward entries are `<function>_` and these names
   (vkr_material_codegen_msl_entries). */
static const VkrMetalTiledEntries
    tiled_lighting[VKR_METAL_TILED_LIGHTING_COUNT] = {
        [VKR_METAL_TILED_LIGHTING_NONE] =
            {
                .forward = {{"vkr_metal_tiled_forward_fragment",
                             "vkr_metal_tiled_forward_probes_fragment"},
                            {"vkr_metal_tiled_forward_decals_fragment",
                             "vkr_metal_tiled_forward_probes_decals_fragment"}},
                .coverage =
                    {{"vkr_metal_tiled_forward_coverage_fragment",
                      "vkr_metal_tiled_forward_coverage_probes_fragment"},
                     {"vkr_metal_tiled_forward_coverage_decals_fragment",
                      "vkr_metal_tiled_forward_coverage_probes_decals_"
                      "fragment"}},
                .blend = {"vkr_metal_tiled_blend_fragment",
                          "vkr_metal_tiled_blend_probes_fragment"},
                .forward_label = @"VKR tiled forward",
                .coverage_label = @"VKR tiled alpha-tested forward",
                .blend_label = @"VKR tiled blend",
            },
        [VKR_METAL_TILED_LIGHTING_PUNCTUAL] =
            {
                .forward =
                    {{"vkr_metal_tiled_forward_punctual_fragment",
                      "vkr_metal_tiled_forward_punctual_probes_fragment"},
                     {"vkr_metal_tiled_forward_punctual_decals_fragment",
                      "vkr_metal_tiled_forward_punctual_probes_decals_"
                      "fragment"}},
                .coverage =
                    {{"vkr_metal_tiled_forward_punctual_coverage_fragment",
                      "vkr_metal_tiled_forward_punctual_coverage_probes_"
                      "fragment"},
                     {"vkr_metal_tiled_forward_punctual_coverage_decals_"
                      "fragment",
                      "vkr_metal_tiled_forward_punctual_coverage_probes_"
                      "decals_fragment"}},
                .blend = {"vkr_metal_tiled_blend_punctual_fragment",
                          "vkr_metal_tiled_blend_punctual_probes_fragment"},
                .forward_label = @"VKR tiled forward with punctual lights",
                .coverage_label =
                    @"VKR tiled alpha-tested forward with punctual lights",
                .blend_label = @"VKR tiled blend with punctual lights",
            },
        [VKR_METAL_TILED_LIGHTING_SHADOWED] =
            {
                .forward =
                    {{"vkr_metal_tiled_forward_shadowed_fragment",
                      "vkr_metal_tiled_forward_shadowed_probes_fragment"},
                     {"vkr_metal_tiled_forward_shadowed_decals_fragment",
                      "vkr_metal_tiled_forward_shadowed_probes_decals_"
                      "fragment"}},
                .coverage =
                    {{"vkr_metal_tiled_forward_shadowed_coverage_fragment",
                      "vkr_metal_tiled_forward_shadowed_coverage_probes_"
                      "fragment"},
                     {"vkr_metal_tiled_forward_shadowed_coverage_decals_"
                      "fragment",
                      "vkr_metal_tiled_forward_shadowed_coverage_probes_"
                      "decals_fragment"}},
                .blend = {"vkr_metal_tiled_blend_shadowed_fragment",
                          "vkr_metal_tiled_blend_shadowed_probes_fragment"},
                .forward_label = @"VKR tiled forward with shadowed lights",
                .coverage_label =
                    @"VKR tiled alpha-tested forward with shadowed lights",
                .blend_label = @"VKR tiled blend with shadowed lights",
            },
        [VKR_METAL_TILED_LIGHTING_ALL] =
            {
                .forward = {{"vkr_metal_tiled_forward_all_fragment",
                             "vkr_metal_tiled_forward_all_probes_fragment"},
                            {"vkr_metal_tiled_forward_all_decals_fragment",
                             "vkr_metal_tiled_forward_all_probes_decals_"
                             "fragment"}},
                .coverage =
                    {{"vkr_metal_tiled_forward_all_coverage_fragment",
                      "vkr_metal_tiled_forward_all_coverage_probes_fragment"},
                     {"vkr_metal_tiled_forward_all_coverage_decals_fragment",
                      "vkr_metal_tiled_forward_all_coverage_probes_decals_"
                      "fragment"}},
                .blend = {"vkr_metal_tiled_blend_all_fragment",
                          "vkr_metal_tiled_blend_all_probes_fragment"},
                .forward_label = @"VKR tiled forward with all lights",
                .coverage_label =
                    @"VKR tiled alpha-tested forward with all lights",
                .blend_label = @"VKR tiled blend with all lights",
            },
        /* Inspection frames always shade the probes and decals. */
        [VKR_METAL_TILED_LIGHTING_INSPECT] =
            {
                .forward = {{"vkr_metal_tiled_forward_inspect_fragment",
                             "vkr_metal_tiled_forward_inspect_fragment"},
                            {"vkr_metal_tiled_forward_inspect_fragment",
                             "vkr_metal_tiled_forward_inspect_fragment"}},
                .coverage =
                    {{"vkr_metal_tiled_forward_inspect_coverage_fragment",
                      "vkr_metal_tiled_forward_inspect_coverage_fragment"},
                     {"vkr_metal_tiled_forward_inspect_coverage_fragment",
                      "vkr_metal_tiled_forward_inspect_coverage_fragment"}},
                .blend = {"vkr_metal_tiled_blend_inspect_fragment",
                          "vkr_metal_tiled_blend_inspect_fragment"},
                .forward_label = @"VKR tiled forward inspection",
                .coverage_label = @"VKR tiled alpha-tested forward inspection",
                .blend_label = @"VKR tiled blend inspection",
            },
};

/* Creates the tiled pipeline's raster pipelines (ADR-087): forward shading
   and the clear sky, multisampled; the atmosphere draw over the resolved
   image, which reads the destination and lays the clouds, aerial perspective
   and fog over it as vkr_cloud_composite composes; and the blend pass's
   glass, blended surfaces and world text. */
vkr_internal void vkr_metal_packet_create_tiled_pipelines(
    VkrMetalPacketRenderer *renderer, id<MTLLibrary> slang,
    id<MTLLibrary> fragment) API_AVAILABLE(macos(26.0)) {
  for (uint32_t decals = 0; decals < VKR_METAL_TILED_DECAL_VARIANT_COUNT;
       ++decals) {
    for (uint32_t probes = 0; probes < VKR_METAL_TILED_PROBE_VARIANT_COUNT;
         ++probes) {
      for (uint32_t i = 0; i < VKR_METAL_TILED_LIGHTING_COUNT; ++i) {
        renderer->tiled_forward_pipelines[decals][probes][i] =
            vkr_metal_packet_create_pipeline_with_targets(
                renderer, fragment, "vkr_metal_tiled_vertex", fragment,
                tiled_lighting[i].forward[decals][probes],
                MTLPixelFormatRGBA16Float, VKR_METAL_PACKET_BLEND_NONE,
                VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
                VKR_METAL_PACKET_ABI_DRAW_ROOT, true_v,
                VKR_METAL_TILED_SAMPLE_COUNT, tiled_lighting[i].forward_label);
        renderer->tiled_coverage_pipelines[decals][probes][i] =
            vkr_metal_packet_create_pipeline_with_targets(
                renderer, fragment, "vkr_metal_tiled_vertex", fragment,
                tiled_lighting[i].coverage[decals][probes],
                MTLPixelFormatRGBA16Float, VKR_METAL_PACKET_BLEND_COVERAGE,
                VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
                VKR_METAL_PACKET_ABI_DRAW_ROOT, true_v,
                VKR_METAL_TILED_SAMPLE_COUNT, tiled_lighting[i].coverage_label);
      }
    }
  }
  /* Without a visibility buffer, a pick replays the opaque draws; their
     alpha-tested surfaces pick whole. */
  renderer->tiled_picking_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", slang,
      "vkr_metal_packet_picking_fragment", MTLPixelFormatR32Uint,
      VKR_METAL_PACKET_BLEND_NONE, VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
      VKR_METAL_PACKET_ABI_RECORD_COUNT, true_v, @"VKR tiled picking");
  renderer->tiled_sky_pipeline = vkr_metal_packet_create_pipeline_with_targets(
      renderer, fragment, "vkr_metal_tiled_sky_vertex", fragment,
      "vkr_metal_tiled_sky_fragment", MTLPixelFormatRGBA16Float,
      VKR_METAL_PACKET_BLEND_NONE, VKR_METAL_PACKET_ABI_RECORD_COUNT,
      VKR_METAL_PACKET_ABI_TILED_SKY_ROOT, false_v,
      VKR_METAL_TILED_SAMPLE_COUNT, @"VKR tiled sky");
  {
    MTL4LibraryFunctionDescriptor *function =
        [MTL4LibraryFunctionDescriptor new];
    function.library = fragment;
    function.name = @"vkr_metal_tiled_resolve_tile";
    MTL4TileRenderPipelineDescriptor *descriptor =
        [MTL4TileRenderPipelineDescriptor new];
    descriptor.label = @"VKR tiled resolve";
    descriptor.tileFunctionDescriptor = function;
    descriptor.rasterSampleCount = VKR_METAL_TILED_SAMPLE_COUNT;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.threadgroupSizeMatchesTileSize = YES;
    NSError *error = nil;
    renderer->tiled_resolve_pipeline = [renderer->compiler
        newRenderPipelineStateWithDescriptor:descriptor
                         compilerTaskOptions:renderer->compiler_options
                                       error:&error];
    if (!renderer->tiled_resolve_pipeline)
      vkr_metal_packet_print_error("tiled_resolve", error);
    [descriptor release];
    [function release];
  }
  /* Glass and alpha-blended surfaces, back to front over the resolved
     image, and world text over them. */
  for (uint32_t probes = 0; probes < VKR_METAL_TILED_PROBE_VARIANT_COUNT;
       ++probes) {
    for (uint32_t i = 0; i < VKR_METAL_TILED_LIGHTING_COUNT; ++i) {
      renderer->tiled_blend_pipelines[probes][i] =
          vkr_metal_packet_create_pipeline(
              renderer, fragment, "vkr_metal_tiled_vertex", fragment,
              tiled_lighting[i].blend[probes], MTLPixelFormatRGBA16Float,
              VKR_METAL_PACKET_BLEND_BEHIND,
              VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
              VKR_METAL_PACKET_ABI_DRAW_ROOT, false_v,
              tiled_lighting[i].blend_label);
    }
  }
  renderer->tiled_text_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_text_vertex", fragment,
      "vkr_metal_packet_text_fragment", MTLPixelFormatRGBA16Float,
      VKR_METAL_PACKET_BLEND_ALPHA, VKR_METAL_PACKET_ABI_TEXT_ROOT,
      VKR_METAL_PACKET_ABI_TEXT_ROOT, false_v, @"VKR tiled world text");
  MTL4LibraryFunctionDescriptor *vertex = [MTL4LibraryFunctionDescriptor new];
  vertex.library = fragment;
  vertex.name = @"vkr_metal_tiled_sky_vertex";
  MTL4LibraryFunctionDescriptor *clouds = [MTL4LibraryFunctionDescriptor new];
  clouds.library = fragment;
  clouds.name = @"vkr_metal_tiled_atmosphere_fragment";
  MTL4RenderPipelineDescriptor *descriptor = [MTL4RenderPipelineDescriptor new];
  descriptor.label = @"VKR tiled atmosphere";
  descriptor.vertexFunctionDescriptor = vertex;
  descriptor.fragmentFunctionDescriptor = clouds;
  MTL4PipelineOptions *options = [MTL4PipelineOptions new];
  options.shaderReflection =
      MTL4ShaderReflectionBindingInfo | MTL4ShaderReflectionBufferTypeInfo;
  descriptor.options = options;
  [options release];
  descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
  descriptor.rasterSampleCount = 1;
  /* The fragment reads the destination through programmable blending and
     writes the composed pixel, so it splits edge pixels by the surface share
     the resolve left in alpha. */
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
  descriptor.colorAttachments[0].blendingState = MTL4BlendStateDisabled;
  NSError *error = nil;
  renderer->tiled_atmosphere_pipeline = [renderer->compiler
      newRenderPipelineStateWithDescriptor:descriptor
                       compilerTaskOptions:renderer->compiler_options
                                     error:&error];
  if (!renderer->tiled_atmosphere_pipeline)
    vkr_metal_packet_print_error("tiled_atmosphere_pipeline", error);
  if (renderer->tiled_atmosphere_pipeline &&
      !vkr_metal_packet_validate_abi_binding(
          descriptor.label,
          renderer->tiled_atmosphere_pipeline.reflection.fragmentBindings,
          VKR_METAL_PACKET_ABI_TILED_SKY_ROOT)) {
    [renderer->tiled_atmosphere_pipeline release];
    renderer->tiled_atmosphere_pipeline = nil;
  }
  [descriptor release];
  [clouds release];
  [vertex release];
}

/* Creates the picking and text raster pipelines. */
vkr_internal void vkr_metal_packet_create_picking_text_pipelines(
    VkrMetalPacketRenderer *renderer, id<MTLLibrary> slang,
    id<MTLLibrary> fragment) API_AVAILABLE(macos(26.0)) {
  renderer->picking_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", slang,
      "vkr_metal_packet_picking_fragment", MTLPixelFormatR32Uint,
      VKR_METAL_PACKET_BLEND_NONE, VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
      VKR_METAL_PACKET_ABI_RECORD_COUNT, false_v,
      @"VKR Metal packet feature picking");
  renderer->editor_overlay_picking_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_editor_overlay_vertex", fragment,
      "vkr_metal_packet_editor_overlay_picking_fragment", MTLPixelFormatR32Uint,
      VKR_METAL_PACKET_BLEND_NONE, VKR_METAL_PACKET_ABI_EDITOR_OVERLAY_ROOT,
      VKR_METAL_PACKET_ABI_RECORD_COUNT, false_v,
      @"VKR Metal editor overlay picking");
  /* Same overlay geometry path; the mask only keeps red. */
  renderer->editor_selection_mask_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_editor_overlay_vertex", fragment,
      "vkr_metal_packet_editor_overlay_fragment", MTLPixelFormatR8Unorm,
      VKR_METAL_PACKET_BLEND_NONE, VKR_METAL_PACKET_ABI_EDITOR_OVERLAY_ROOT,
      VKR_METAL_PACKET_ABI_RECORD_COUNT, false_v,
      @"VKR Metal editor selection mask");
  renderer->picking_text_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_text_vertex", fragment,
      "vkr_metal_packet_text_picking_fragment", MTLPixelFormatR32Uint,
      VKR_METAL_PACKET_BLEND_NONE, VKR_METAL_PACKET_ABI_TEXT_ROOT,
      VKR_METAL_PACKET_ABI_TEXT_ROOT, false_v,
      @"VKR Metal packet text picking");
}

/* Creates the IBL and atmosphere compute pipelines. */
vkr_internal void
vkr_metal_packet_create_environment_pipelines(VkrMetalPacketRenderer *renderer,
                                              id<MTLLibrary> fragment)
    API_AVAILABLE(macos(26.0)) {
  renderer->ibl_prefilter_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_ibl_prefilter",
      VKR_METAL_PACKET_ABI_PREFILTER_ROOT, false_v,
      @"VKR Metal packet IBL prefilter");
  renderer->ibl_sh_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_ibl_sh",
      VKR_METAL_PACKET_ABI_SH_PROJECT_ROOT, false_v,
      @"VKR Metal packet IBL SH projection");
  renderer->atmosphere_transmittance_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment,
          "vkr_metal_packet_atmosphere_transmittance_compute",
          VKR_METAL_PACKET_ABI_ATMOSPHERE_ROOT, false_v,
          @"VKR Metal packet atmosphere transmittance");
  renderer->atmosphere_multiple_scattering_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment,
          "vkr_metal_packet_atmosphere_multiple_scattering_compute",
          VKR_METAL_PACKET_ABI_ATMOSPHERE_ROOT, false_v,
          @"VKR Metal packet atmosphere multiple scattering");
  renderer->atmosphere_source_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_atmosphere_source_compute",
          VKR_METAL_PACKET_ABI_ATMOSPHERE_ROOT, false_v,
          @"VKR Metal packet atmosphere source");
  renderer->sky_view_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_sky_view_compute",
      VKR_METAL_PACKET_ABI_SKY_VIEW_ROOT, false_v,
      @"VKR Metal packet sky view");
  renderer->aerial_perspective_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_aerial_perspective_compute",
          VKR_METAL_PACKET_ABI_AERIAL_PERSPECTIVE_ROOT, false_v,
          @"VKR Metal packet aerial perspective");
  const char *cloud_noise_entries[VKR_METAL_PACKET_CLOUD_NOISE_COUNT] = {
      "vkr_metal_packet_cloud_base_noise",
      "vkr_metal_packet_cloud_detail_noise",
      "vkr_metal_packet_cloud_weather",
  };
  for (uint32_t i = 0u; i < VKR_METAL_PACKET_CLOUD_NOISE_COUNT; ++i) {
    renderer->cloud_noise_pipelines[i] =
        vkr_metal_packet_create_compute_pipeline(
            renderer, fragment, cloud_noise_entries[i],
            VKR_METAL_PACKET_ABI_CLOUD_NOISE_ROOT, false_v,
            @"VKR Metal packet cloud noise");
  }
  renderer->cloud_shadow_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_cloud_shadow",
      VKR_METAL_PACKET_ABI_CLOUD_SHADOW_ROOT, false_v,
      @"VKR Metal packet cloud shadow");
  renderer->cloud_trace_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_cloud_trace",
      VKR_METAL_PACKET_ABI_CLOUD_TRACE_ROOT, false_v,
      @"VKR Metal packet cloud trace");
  renderer->cloud_sky_light_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_cloud_sky_light",
      VKR_METAL_PACKET_ABI_CLOUD_SKY_LIGHT_ROOT, false_v,
      @"VKR Metal packet cloud sky light");
  renderer->cloud_sky_light_sh_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_cloud_sky_light_sh",
          VKR_METAL_PACKET_ABI_CLOUD_SKY_LIGHT_ROOT, false_v,
          @"VKR Metal packet cloud sky-light SH");
  renderer->cloud_sky_light_mips_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_cloud_sky_light_mips",
          VKR_METAL_PACKET_ABI_CLOUD_SKY_LIGHT_ROOT, false_v,
          @"VKR Metal packet cloud sky-light chain");
  renderer->diffuse_volume_compose_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_diffuse_volume_compose",
          VKR_METAL_PACKET_ABI_DIFFUSE_VOLUME_COMPOSE_ROOT, false_v,
          @"VKR Metal packet diffuse volume composition");
}

/* Creates the zeroed SH coefficient pool buffer. */
vkr_internal bool8_t vkr_metal_packet_create_sh_coefficients(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  /* Shared rather than private so the 4 KB pool can be zeroed here without a
     blit: slot 0 must be a valid black sentinel from the first frame, and any
     slot referenced before its projection lands must read black rather than
     stale bytes. On unified memory this costs nothing for a buffer this small
     that lighting reads through the cache. */
  vkr_ibl_sh_pool_init(&renderer->sh_pool);
  const uint64_t sh_reservation =
      [renderer->device
          heapBufferSizeAndAlignWithLength:VKR_SH_BUFFER_BYTES
                                   options:MTLResourceStorageModeShared]
          .size;
  if (!vkr_metal_memory_device_reserve_external(renderer->memory,
                                                sh_reservation))
    return false_v;
  id<MTLBuffer> sh_buffer =
      [renderer->device newBufferWithLength:VKR_SH_BUFFER_BYTES
                                    options:MTLResourceStorageModeShared];
  if (!sh_buffer ||
      !vkr_metal_memory_device_reconcile_external(
          renderer->memory, sh_reservation, sh_buffer.allocatedSize)) {
    [sh_buffer release];
    vkr_metal_memory_device_release_external(renderer->memory, sh_reservation);
    return false_v;
  }
  renderer->sh_coefficients = sh_buffer;
  renderer->sh_coefficients.label = @"VKR Metal SH coefficients";
  if (renderer->sh_coefficients) {
    MemZero(renderer->sh_coefficients.contents, VKR_SH_BUFFER_BYTES);
  }
  return true_v;
}

/* Creates the GPU draw and skinning compute pipelines. */
vkr_internal void vkr_metal_packet_create_scene_compute_pipelines(
    VkrMetalPacketRenderer *renderer, id<MTLLibrary> fragment)
    API_AVAILABLE(macos(26.0)) {
  renderer->gpu_draw_classify_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_gpu_draw_classify",
          VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT, false_v,
          @"VKR Metal packet GPU draw classify");
  renderer->gpu_draw_prefix_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_gpu_draw_prefix",
      VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT, false_v,
      @"VKR Metal packet GPU draw prefix");
  renderer->gpu_draw_encode_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_gpu_draw_encode",
      VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT, true_v,
      @"VKR Metal packet GPU draw encode");
  renderer->skinning_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_skinning",
      VKR_METAL_PACKET_ABI_SKINNING_ROOT, false_v, @"VKR compute skinning");
}

/* Creates the depth-reduction, shadow-moment and exposure compute
   pipelines. */
vkr_internal void vkr_metal_packet_create_frame_compute_pipelines(
    VkrMetalPacketRenderer *renderer, id<MTLLibrary> fragment)
    API_AVAILABLE(macos(26.0)) {
  renderer->hzb_build_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_hzb_build",
      VKR_METAL_PACKET_ABI_HZB_BUILD_ROOT, false_v,
      @"VKR Metal packet HZB build");
  renderer->exposure_clear_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_exposure_clear",
      VKR_METAL_PACKET_ABI_EXPOSURE_ROOT, false_v,
      @"VKR Metal packet exposure histogram clear");
  renderer->exposure_histogram_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_exposure_histogram",
          VKR_METAL_PACKET_ABI_EXPOSURE_ROOT, false_v,
          @"VKR Metal packet exposure histogram");
  renderer->exposure_resolve_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_exposure_resolve",
          VKR_METAL_PACKET_ABI_EXPOSURE_ROOT, false_v,
          @"VKR Metal packet exposure resolve");
}

/* Creates the bloom and display-linear pipelines. */
vkr_internal void
vkr_metal_packet_create_post_pipelines(VkrMetalPacketRenderer *renderer,
                                       id<MTLLibrary> fragment)
    API_AVAILABLE(macos(26.0)) {
  renderer->bloom_prefilter_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_bloom_prefilter",
      VKR_METAL_PACKET_ABI_BLOOM_ROOT, false_v,
      @"VKR Metal packet bloom prefilter");
  /* Both downsample filters are created. Which one a frame binds is cold
     configuration, so the comparison the design asks for is a config change
     rather than a rebuild, and neither is a fallback for the other. */
  renderer->bloom_downsample_tent13_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_bloom_downsample_tent13",
          VKR_METAL_PACKET_ABI_BLOOM_ROOT, false_v,
          @"VKR Metal packet bloom downsample tent13");
  renderer->bloom_downsample_box4_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_bloom_downsample_box4",
          VKR_METAL_PACKET_ABI_BLOOM_ROOT, false_v,
          @"VKR Metal packet bloom downsample box4");
  renderer->bloom_downsample_box2_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_bloom_downsample_box2",
          VKR_METAL_PACKET_ABI_BLOOM_ROOT, false_v,
          @"VKR Metal packet bloom downsample box2");
  renderer->bloom_upsample_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_bloom_upsample",
      VKR_METAL_PACKET_ABI_BLOOM_ROOT, false_v,
      @"VKR Metal packet bloom upsample");
  renderer->display_linear_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_tonemap_vertex", fragment,
      "vkr_metal_packet_tonemap_fragment", MTLPixelFormatRGBA16Float,
      VKR_METAL_PACKET_BLEND_NONE, VKR_METAL_PACKET_ABI_RECORD_COUNT,
      VKR_METAL_PACKET_ABI_TONEMAP_ROOT, false_v,
      @"VKR Metal display-linear preparation");
}

/* Creates the depth-stencil states and the shared argument table. */
vkr_internal void
vkr_metal_packet_create_render_states(VkrMetalPacketRenderer *renderer)
    API_AVAILABLE(macos(26.0)) {
  MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
  depth.depthCompareFunction = MTLCompareFunctionLess;
  depth.depthWriteEnabled = YES;
  renderer->depth_write_state =
      [renderer->device newDepthStencilStateWithDescriptor:depth];
  depth.depthCompareFunction = MTLCompareFunctionAlways;
  renderer->depth_clear_state =
      [renderer->device newDepthStencilStateWithDescriptor:depth];
  depth.depthCompareFunction = MTLCompareFunctionLessEqual;
  depth.depthWriteEnabled = NO;
  renderer->depth_read_state =
      [renderer->device newDepthStencilStateWithDescriptor:depth];
  /* Tiled shading lays its own depth, the later of two coplanar surfaces
     winning; the sky passes where no surface was drawn. */
  depth.depthCompareFunction = MTLCompareFunctionLessEqual;
  depth.depthWriteEnabled = YES;
  renderer->tiled_shade_state =
      [renderer->device newDepthStencilStateWithDescriptor:depth];
  depth.depthWriteEnabled = NO;
  renderer->tiled_sky_state =
      [renderer->device newDepthStencilStateWithDescriptor:depth];
  [depth release];
  MTL4ArgumentTableDescriptor *table = [MTL4ArgumentTableDescriptor new];
  table.maxBufferBindCount = 3;
  table.initializeBindings = YES;
  NSError *error = nil;
  renderer->argument_table =
      [renderer->device newArgumentTableWithDescriptor:table error:&error];
  [table release];
  if (!renderer->argument_table)
    vkr_metal_packet_print_error("argument_table", error);
}

/* Logs the first startup object that is missing. */
vkr_internal bool8_t vkr_metal_packet_startup_objects_ready(
    const VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  const struct {
    id object;
    const char *label;
  } required[] = {
      {renderer->ui_pipeline, "UI stream pipeline"},
      {renderer->picking_pipeline, "picking pipeline"},
      {renderer->tonemap_pipeline, "tonemap pipeline"},
      {renderer->display_linear_pipeline,
       "display-linear preparation pipeline"},
      {renderer->editor_overlay_pipeline, "editor overlay pipeline"},
      {renderer->editor_overlay_picking_pipeline,
       "editor overlay picking pipeline"},
      {renderer->editor_selection_mask_pipeline,
       "editor selection mask pipeline"},
      {renderer->editor_selection_outline_pipeline,
       "editor selection outline pipeline"},
      {renderer->editor_grid_pipeline, "editor grid pipeline"},
      {renderer->picking_text_pipeline, "picking text pipeline"},
      {renderer->animation_preview_pipeline, "animation preview pipeline"},
      {renderer->ibl_prefilter_pipeline, "IBL prefilter pipeline"},
      {renderer->ibl_sh_pipeline, "IBL SH projection pipeline"},
      {renderer->sh_coefficients, "SH coefficient pool"},
      {renderer->atmosphere_transmittance_pipeline,
       "atmosphere transmittance pipeline"},
      {renderer->atmosphere_multiple_scattering_pipeline,
       "atmosphere multiple-scattering pipeline"},
      {renderer->atmosphere_source_pipeline, "atmosphere source pipeline"},
      {renderer->gpu_shadow_pipeline, "GPU shadow pipeline"},
      {renderer->gpu_shadow_opaque_pipeline, "opaque GPU shadow pipeline"},
      {renderer->depth_clear_pipeline, "depth clear pipeline"},
      {renderer->gpu_draw_classify_pipeline, "draw classify pipeline"},
      {renderer->gpu_draw_prefix_pipeline, "draw prefix pipeline"},
      {renderer->gpu_draw_encode_pipeline, "draw encode pipeline"},
      {renderer->gpu_draw_icb_argument_encoder,
       "indirect command argument encoder"},
      {renderer->skinning_pipeline, "skinning pipeline"},
      {renderer->sky_view_pipeline, "Sky-view lookup pipeline"},
      {renderer->aerial_perspective_pipeline, "Aerial perspective pipeline"},
      {renderer->cloud_noise_pipelines[0], "Cloud base-noise pipeline"},
      {renderer->cloud_noise_pipelines[1], "Cloud detail-noise pipeline"},
      {renderer->cloud_noise_pipelines[2], "Cloud weather pipeline"},
      {renderer->cloud_shadow_pipeline, "Cloud shadow pipeline"},
      {renderer->cloud_trace_pipeline, "Cloud trace pipeline"},
      {renderer->hzb_build_pipeline, "HZB build pipeline"},
      {renderer->exposure_clear_pipeline, "exposure clear pipeline"},
      {renderer->exposure_histogram_pipeline, "exposure histogram pipeline"},
      {renderer->exposure_resolve_pipeline, "exposure resolve pipeline"},
      {renderer->bloom_prefilter_pipeline, "bloom prefilter pipeline"},
      {renderer->bloom_downsample_tent13_pipeline,
       "bloom tent13 downsample pipeline"},
      {renderer->bloom_downsample_box4_pipeline,
       "bloom box4 downsample pipeline"},
      {renderer->bloom_downsample_box2_pipeline,
       "bloom box2 downsample pipeline"},
      {renderer->bloom_upsample_pipeline, "bloom upsample pipeline"},
      {renderer->tiled_sky_pipeline, "tiled sky pipeline"},
      {renderer->tiled_atmosphere_pipeline, "tiled atmosphere pipeline"},
      {renderer->tiled_resolve_pipeline, "tiled resolve pipeline"},
      {renderer->tiled_picking_pipeline, "tiled picking pipeline"},
      {renderer->tiled_text_pipeline, "tiled world text pipeline"},
      {renderer->depth_write_state, "depth-write state"},
      {renderer->depth_clear_state, "depth-clear state"},
      {renderer->depth_read_state, "depth-read state"},
      {renderer->tiled_shade_state, "tiled shading depth state"},
      {renderer->tiled_sky_state, "tiled sky depth state"},
      {renderer->argument_table, "argument table"},
  };
  for (uint32_t i = 0u; i < ArrayCount(required); ++i) {
    if (!required[i].object) {
      log_error("Metal renderer startup object unavailable: %s",
                required[i].label);
      return false_v;
    }
  }
  if (renderer->sh_coefficients.gpuAddress == 0) {
    log_error("Metal renderer startup object unavailable: SH coefficient pool "
              "address");
    return false_v;
  }
  for (uint32_t probes = 0; probes < VKR_METAL_TILED_PROBE_VARIANT_COUNT;
       ++probes) {
    for (uint32_t i = 0; i < VKR_METAL_TILED_LIGHTING_COUNT; ++i) {
      if (!renderer->tiled_blend_pipelines[probes][i]) {
        log_error("Metal renderer startup object unavailable: tiled blend "
                  "variant %u with probes %u",
                  i, probes);
        return false_v;
      }
      for (uint32_t decals = 0; decals < VKR_METAL_TILED_DECAL_VARIANT_COUNT;
           ++decals) {
        if (!renderer->tiled_forward_pipelines[decals][probes][i] ||
            !renderer->tiled_coverage_pipelines[decals][probes][i]) {
          log_error("Metal renderer startup object unavailable: tiled "
                    "shading variant %u with probes %u and decals %u",
                    i, probes, decals);
          return false_v;
        }
      }
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_create_pipelines(
    VkrMetalPacketRenderer *renderer, const char *slang_msl_path,
    const char *slang_metallib_path, const char *fragment_msl_path,
    const char *fragment_metallib_path) API_AVAILABLE(macos(26.0)) {
  if (!vkr_metal_packet_abi_validate_host()) {
    log_error("Metal packet host ABI does not match its durable manifest");
    return false_v;
  }
  /* Startup shader cost is the baseline for precompiled libraries. */
  const float64_t library_start = vkr_platform_get_absolute_time();
  id<MTLLibrary> slang = vkr_metal_packet_create_library(
      renderer, slang_msl_path, slang_metallib_path,
      @"VKR Metal packet Slang shaders");
  id<MTLLibrary> fragment = vkr_metal_packet_create_library(
      renderer, fragment_msl_path, fragment_metallib_path,
      @"VKR Metal packet resource shaders");
  const float64_t library_end = vkr_platform_get_absolute_time();
  bool8_t created = false_v;
  if (!slang || !fragment)
    goto cleanup;
  vkr_metal_packet_create_scene_raster_pipelines(renderer, slang, fragment);
  vkr_metal_packet_create_tiled_pipelines(renderer, slang, fragment);
  VkrMetalPacketPresentationPipelines presentation_pipelines;
  if (!vkr_metal_packet_create_presentation_pipelines(
          renderer, fragment, vkr_metal_packet_target_pixel_format(renderer),
          &presentation_pipelines))
    goto cleanup;
  vkr_metal_packet_install_presentation_pipelines(renderer,
                                                  &presentation_pipelines);
  vkr_metal_packet_create_picking_text_pipelines(renderer, slang, fragment);
  vkr_metal_packet_create_environment_pipelines(renderer, fragment);
  if (!vkr_metal_packet_create_sh_coefficients(renderer))
    goto cleanup;
  vkr_metal_packet_create_scene_compute_pipelines(renderer, fragment);
  vkr_metal_packet_create_frame_compute_pipelines(renderer, fragment);
  vkr_metal_packet_create_post_pipelines(renderer, fragment);
  id<MTLFunction> encode_function =
      [fragment newFunctionWithName:@"vkr_metal_packet_gpu_draw_encode"];
  renderer->gpu_draw_icb_argument_encoder =
      [encode_function newArgumentEncoderWithBufferIndex:1u];
  [encode_function release];
  renderer->fragment_library = [fragment retain];
  created = true_v;
  log_info("Metal shader startup: libraries %.1f ms (%s), pipelines %.1f ms "
           "(archive %s)",
           (library_end - library_start) * 1000.0,
           slang_metallib_path && fragment_metallib_path ? "metallib"
                                                         : "source",
           (vkr_platform_get_absolute_time() - library_end) * 1000.0,
           renderer->pipeline_archive_warm ? "warm" : "cold");

cleanup:
  /* Every exit after library creation releases both libraries; the fragment
     library stays alive through the retained reference above. */
  [fragment release];
  [slang release];
  if (!created)
    return false_v;
  vkr_metal_packet_create_render_states(renderer);
  return vkr_metal_packet_startup_objects_ready(renderer);
}

vkr_internal bool8_t vkr_metal_packet_create_gpu_draw_icb_residencies(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  if (!renderer)
    return false_v;
  const char *shader_validation = getenv("MTL_SHADER_VALIDATION");
  const bool8_t validation_enabled = shader_validation &&
                                             shader_validation[0] != '\0' &&
                                             strcmp(shader_validation, "0") != 0
                                         ? true_v
                                         : false_v;
  renderer->gpu_draw_icb_view_group_size =
      validation_enabled ? 1u : VKR_METAL_PACKET_GPU_DRAW_VIEW_COUNT_MAX;
  renderer->gpu_draw_icb_group_count =
      (VKR_METAL_PACKET_GPU_DRAW_VIEW_COUNT_MAX +
       renderer->gpu_draw_icb_view_group_size - 1u) /
      renderer->gpu_draw_icb_view_group_size;
  MTLResidencySetDescriptor *residency_descriptor =
      [MTLResidencySetDescriptor new];
  residency_descriptor.label = @"VKR Metal GPU draw indirect commands";
  bool8_t passed = true_v;
  for (uint32_t i = 0u; passed && i < renderer->command_slot_count; ++i) {
    VkrMetalPacketCommandSlot *slot = &renderer->command_slots[i];
    for (uint32_t group = 0u; group < renderer->gpu_draw_icb_group_count;
         ++group) {
      residency_descriptor.initialCapacity = 1u;
      NSError *residency_error = nil;
      id<MTLResidencySet> residency =
          [renderer->device newResidencySetWithDescriptor:residency_descriptor
                                                    error:&residency_error];
      slot->gpu_draw_icb_residencies[group] = residency;
      if (!residency) {
        (void)residency_error;
        passed = false_v;
        break;
      }
      [residency commit];
      [residency requestResidency];
      slot->gpu_draw_icb_residency_count++;
    }
  }
  [residency_descriptor release];
  if (!passed) {
    log_error("Metal indirect command residency set is unavailable");
    for (uint32_t i = 0u; i < renderer->command_slot_count; ++i) {
      VkrMetalPacketCommandSlot *slot = &renderer->command_slots[i];
      for (uint32_t group = 0u;
           group < VKR_METAL_PACKET_GPU_DRAW_ICB_GROUP_COUNT_MAX; ++group) {
        if (group < slot->gpu_draw_icb_residency_count)
          [slot->gpu_draw_icb_residencies[group] endResidency];
        [slot->gpu_draw_icb_residencies[group] release];
        slot->gpu_draw_icb_residencies[group] = nil;
      }
      slot->gpu_draw_icb_residency_count = 0u;
    }
  }
  return passed;
}

/* Slot acquisition has completed every previous use before this preparation
 * boundary. Releasing the old object first avoids charging both capacities;
 * failure leaves an empty slot that normal frame cancellation can retry. */
vkr_internal bool8_t vkr_metal_packet_reserve_gpu_draw_icb(
    VkrMetalPacketRenderer *renderer, id<MTLResidencySet> residency,
    id<MTLIndirectCommandBuffer> *icb, uint32_t *capacity,
    uint32_t required_commands, VkrRendererError *out_error)
    API_AVAILABLE(macos(26.0)) {
  if (required_commands <= *capacity)
    return true_v;
  if (*icb) {
    [residency removeAllocation:*icb];
    [residency commit];
    vkr_metal_packet_release_budgeted_resource(renderer, *icb);
    *icb = nil;
    *capacity = 0u;
  }
  MTLIndirectCommandBufferDescriptor *descriptor =
      [MTLIndirectCommandBufferDescriptor new];
  descriptor.commandTypes = MTLIndirectCommandTypeDrawIndexed;
  descriptor.inheritPipelineState = YES;
  descriptor.inheritBuffers = NO;
  descriptor.inheritDepthStencilState = YES;
  descriptor.inheritCullMode = YES;
  descriptor.maxVertexBufferBindCount = 1u;
  descriptor.maxFragmentBufferBindCount = 3u;
  /* Metal has no ICB size query. Reject and release a provisional native
     object before residency if its actual allocation cannot enter the cap. */
  *icb = [renderer->device
      newIndirectCommandBufferWithDescriptor:descriptor
                             maxCommandCount:required_commands
                                     options:MTLResourceStorageModePrivate];
  if (*icb && !vkr_metal_memory_device_reserve_external(renderer->memory,
                                                        (*icb).allocatedSize)) {
    [*icb release];
    *icb = nil;
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }
  [descriptor release];
  if (!*icb) {
    log_error("Metal indirect command buffer growth failed: commands=%u",
              required_commands);
    return false_v;
  }
  [residency addAllocation:*icb];
  [residency commit];
  *capacity = required_commands;
  VKR_METAL_DIAGNOSTIC(
      renderer, "icb_capacity", "slot=%u commands=%u bytes=%llu",
      (uint32_t)(renderer->active_command_slot - renderer->command_slots),
      required_commands, (unsigned long long)(*icb).allocatedSize);
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_prepare_gpu_draw_icbs(
    VkrMetalPacketRenderer *renderer, uint32_t candidate_count,
    uint32_t view_count, VkrRendererError *out_error)
    API_AVAILABLE(macos(26.0)) {
  if (candidate_count > VKR_GPU_DRAW_CANDIDATE_CAPACITY ||
      view_count > VKR_METAL_PACKET_GPU_DRAW_VIEW_COUNT_MAX ||
      (candidate_count > 0u && view_count == 0u))
    return false_v;
  VkrMetalPacketCommandSlot *slot = renderer->active_command_slot;
  if (candidate_count > 0u) {
    uint32_t candidate_capacity = 1u;
    while (candidate_capacity < candidate_count)
      candidate_capacity *= 2u;
    const uint32_t group_count =
        (view_count + renderer->gpu_draw_icb_view_group_size - 1u) /
        renderer->gpu_draw_icb_view_group_size;
    for (uint32_t group = 0u; group < group_count; ++group) {
      const uint32_t group_view_count =
          MIN(renderer->gpu_draw_icb_view_group_size,
              view_count - group * renderer->gpu_draw_icb_view_group_size);
      if (!vkr_metal_packet_reserve_gpu_draw_icb(
              renderer, slot->gpu_draw_icb_residencies[group],
              &slot->gpu_draw_icbs[group],
              &slot->gpu_draw_icb_capacities[group],
              candidate_capacity * group_view_count, out_error))
        return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_create_compiler(
    VkrMetalPacketRenderer *renderer, const char *archive_path)
    API_AVAILABLE(macos(26.0)) {
  NSError *error = nil;
  NSURL *archive_url = nil;
  /* Apple's Metal shader validator currently crashes in the Metal 4-to-3
     archive conversion path while installing lookupArchives. Validation runs
     intentionally compile from source, which is also the stronger shader
     validation path; normal runs retain cold/warm archive behavior. */
  const char *shader_validation = getenv("MTL_SHADER_VALIDATION");
  if (shader_validation && shader_validation[0] != '\0' &&
      strcmp(shader_validation, "0") != 0) {
    archive_path = NULL;
    log_info(
        "Metal shader validation enabled; pipeline archive lookup disabled");
  }
  if (archive_path) {
    NSString *path = [NSString stringWithUTF8String:archive_path];
    archive_url = [NSURL fileURLWithPath:path];
    if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
      renderer->pipeline_archive =
          [renderer->device newArchiveWithURL:archive_url error:&error];
      if (!renderer->pipeline_archive) {
        vkr_metal_packet_print_error("archive_load", error);
        return false_v;
      }
      renderer->compiler_options = [MTL4CompilerTaskOptions new];
      renderer->compiler_options.lookupArchives =
          @[ renderer->pipeline_archive ];
      renderer->pipeline_archive_warm = true_v;
    } else {
      MTL4PipelineDataSetSerializerDescriptor *serializer_descriptor =
          [MTL4PipelineDataSetSerializerDescriptor new];
      serializer_descriptor.configuration =
          MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
      renderer->pipeline_serializer = [renderer->device
          newPipelineDataSetSerializerWithDescriptor:serializer_descriptor];
      [serializer_descriptor release];
      if (!renderer->pipeline_serializer)
        return false_v;
    }
  }
  MTL4CompilerDescriptor *compiler_descriptor = [MTL4CompilerDescriptor new];
  compiler_descriptor.label = @"VKR Metal packet renderer";
  compiler_descriptor.pipelineDataSetSerializer = renderer->pipeline_serializer;
  renderer->compiler =
      [renderer->device newCompilerWithDescriptor:compiler_descriptor
                                            error:&error];
  [compiler_descriptor release];
  if (!renderer->compiler)
    vkr_metal_packet_print_error("compiler", error);
  return renderer->compiler ? true_v : false_v;
}

vkr_internal bool8_t vkr_metal_packet_finalize_archive(
    VkrMetalPacketRenderer *renderer, const char *archive_path)
    API_AVAILABLE(macos(26.0)) {
  if (!archive_path || !renderer->pipeline_serializer)
    return true_v;
  NSError *error = nil;
  NSURL *url =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:archive_path]];
  if (![renderer->pipeline_serializer serializeAsArchiveAndFlushToURL:url
                                                                error:&error]) {
    vkr_metal_packet_print_error("archive_write", error);
    return false_v;
  }
  renderer->pipeline_archive =
      [renderer->device newArchiveWithURL:url error:&error];
  if (!renderer->pipeline_archive) {
    vkr_metal_packet_print_error("archive_verify", error);
    return false_v;
  }
  renderer->pipeline_archive_written = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_upload_texture(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketTextureUpload *payload, VkrTextureFormat format,
    id<MTLTexture> destination) API_AVAILABLE(macos(26.0));

vkr_internal bool8_t vkr_metal_packet_create_ibl_targets(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  MTLTextureDescriptor *cube = [MTLTextureDescriptor new];
  cube.textureType = MTLTextureTypeCube;
  cube.pixelFormat = MTLPixelFormatRGBA16Float;
  cube.width = 1u;
  cube.height = 1u;
  cube.mipmapLevelCount = 1u;
  cube.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  const bool8_t black_created =
      vkr_metal_memory_device_create_texture(
          renderer->memory, cube, VKR_GPU_ALLOCATION_OWNER_TEXTURE,
          &renderer->ibl_black_prefilter) == VKR_METAL_MEMORY_STATUS_OK;
  [cube release];
  static const uint64_t black_texels[6] = {0};
  VkrTextureUploadRegion black_regions[6] = {0};
  for (uint32_t face = 0u; face < ArrayCount(black_regions); ++face) {
    black_regions[face] = (VkrTextureUploadRegion){
        .mip_level = 0u,
        .array_layer = face,
        .width = 1u,
        .height = 1u,
        .depth = 1u,
        .byte_offset = (uint64_t)face * sizeof(black_texels[0]),
        .byte_size = sizeof(black_texels[0]),
    };
  }
  const bool8_t black_uploaded =
      black_created &&
      vkr_metal_packet_upload_texture(
          renderer,
          &(VkrMetalPacketTextureUpload){
              .data = (const uint8_t *)black_texels,
              .data_size = sizeof(black_texels),
              .mip_levels = 1u,
              .array_layers = ArrayCount(black_regions),
              .is_compressed = false_v,
              .region_count = ArrayCount(black_regions),
              .regions = black_regions,
          },
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
          (id<MTLTexture>)renderer->ibl_black_prefilter.metal_texture);
  if (!black_uploaded) {
    if (renderer->ibl_black_prefilter.metal_texture)
      (void)vkr_metal_memory_device_retire(
          renderer->memory, renderer->ibl_black_prefilter.handle, 0u);
    (void)vkr_metal_packet_collect_memory(renderer, 0, NULL);
    renderer->ibl_black_prefilter = (VkrMetalTextureResource){0};
    return false_v;
  }
  renderer->ibl_black_live = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_validate_texture_upload(
    const VkrMetalPacketTextureUpload *payload, VkrTextureFormat format,
    id<MTLTexture> destination, VkrTextureFormatInfo *out_info)
    API_AVAILABLE(macos(26.0)) {
  if (!payload || !payload->data || payload->data_size == 0 ||
      !payload->regions || payload->region_count == 0 || !destination ||
      !out_info || !vkr_texture_format_get_info(format, out_info) ||
      out_info->is_depth_stencil ||
      payload->is_compressed != out_info->is_block_compressed ||
      payload->mip_levels != destination.mipmapLevelCount)
    return false_v;
  uint64_t destination_layers = destination.arrayLength;
  if (destination.textureType == MTLTextureTypeCube) {
    destination_layers = 6u;
  } else if (destination.textureType == MTLTextureTypeCubeArray) {
    destination_layers *= 6u;
  }
  if (payload->array_layers != destination_layers)
    return false_v;
  for (uint32_t i = 0; i < payload->region_count; ++i) {
    const VkrTextureUploadRegion *region = &payload->regions[i];
    const uint64_t blocks_x =
        ((uint64_t)region->width + out_info->block_width - 1u) /
        out_info->block_width;
    const uint64_t blocks_y =
        ((uint64_t)region->height + out_info->block_height - 1u) /
        out_info->block_height;
    const uint64_t required =
        blocks_x * out_info->bytes_per_block * blocks_y * region->depth;
    if (region->width == 0 || region->height == 0 || region->depth != 1 ||
        region->mip_level >= payload->mip_levels ||
        region->array_layer >= payload->array_layers ||
        region->byte_offset > payload->data_size ||
        required > payload->data_size - region->byte_offset ||
        required > region->byte_size)
      return false_v;
  }
  return true_v;
}

/* Copies `payload`'s regions from `upload_buffer`, where the payload byte at
   `data_offset` sits at `upload_offset`. */
vkr_internal void vkr_metal_packet_encode_texture_upload(
    const VkrMetalPacketTextureUpload *payload,
    const VkrTextureFormatInfo *info, id<MTLBuffer> upload_buffer,
    uint64_t upload_offset, uint64_t data_offset,
    id<MTL4ComputeCommandEncoder> encoder, id<MTLTexture> destination)
    API_AVAILABLE(macos(26.0)) {
  for (uint32_t i = 0; i < payload->region_count; ++i) {
    const VkrTextureUploadRegion *region = &payload->regions[i];
    const uint64_t blocks_x =
        ((uint64_t)region->width + info->block_width - 1u) / info->block_width;
    const uint64_t blocks_y =
        ((uint64_t)region->height + info->block_height - 1u) /
        info->block_height;
    const uint64_t bytes_per_row = blocks_x * info->bytes_per_block;
    [encoder copyFromBuffer:upload_buffer
               sourceOffset:upload_offset + (region->byte_offset - data_offset)
          sourceBytesPerRow:bytes_per_row
        sourceBytesPerImage:bytes_per_row * blocks_y
                 sourceSize:MTLSizeMake(region->width, region->height, 1)
                  toTexture:destination
           destinationSlice:region->array_layer
           destinationLevel:region->mip_level
          destinationOrigin:MTLOriginMake(0, 0, 0)];
  }
}

vkr_internal bool8_t vkr_metal_packet_upload_texture_batched(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketTextureUpload *payload, VkrTextureFormat format,
    id<MTLTexture> destination) API_AVAILABLE(macos(26.0)) {
  VkrTextureFormatInfo info = {0};
  if (!renderer || !vkr_metal_packet_validate_texture_upload(
                       payload, format, destination, &info))
    return false_v;

  VkrMetalPacketTextureUploadBatch *batch = &renderer->texture_upload_batch;
  uint64_t offset =
      vkr_metal_packet_align_up(batch->used, VKR_METAL_PACKET_ROOT_ALIGNMENT);
  const bool8_t full =
      batch->active &&
      (batch->texture_count == VKR_METAL_PACKET_TEXTURE_UPLOAD_BATCH_MAX ||
       offset > batch->capacity ||
       payload->data_size > batch->capacity - offset);
  if (full && !vkr_metal_packet_texture_upload_batch_flush(renderer, false_v))
    return false_v;

  batch = &renderer->texture_upload_batch;
  if (!batch->active) {
    const uint64_t aligned_size = vkr_metal_packet_align_up(
        payload->data_size, VKR_METAL_PACKET_ROOT_ALIGNMENT);
    const uint64_t capacity =
        aligned_size > VKR_METAL_PACKET_TEXTURE_UPLOAD_BATCH_BYTES
            ? aligned_size
            : VKR_METAL_PACKET_TEXTURE_UPLOAD_BATCH_BYTES;
    if (!vkr_metal_packet_texture_upload_batch_start(renderer, capacity))
      return false_v;
    offset = 0u;
  }

  MemCopy((uint8_t *)batch->upload.cpu_address + offset, payload->data,
          payload->data_size);
  vkr_metal_packet_encode_texture_upload(payload, &info, batch->upload_buffer,
                                         batch->slice.offset + offset, 0u,
                                         batch->encoder, destination);
  batch->used = offset + payload->data_size;
  batch->texture_count++;
  return true_v;
}

/* One past the last region of the upload chunk that starts at region
   `first`: the regions after it that follow it contiguously in the payload,
   together at most VKR_METAL_PACKET_TEXTURE_UPLOAD_CHUNK_BYTES, or the first
   region alone when it is larger. */
vkr_internal uint32_t vkr_metal_packet_texture_upload_chunk_end(
    const VkrMetalPacketTextureUpload *payload, uint32_t first) {
  const uint64_t start = payload->regions[first].byte_offset;
  uint64_t end = start + payload->regions[first].byte_size;
  uint32_t next = first + 1u;
  while (next < payload->region_count) {
    const VkrTextureUploadRegion *region = &payload->regions[next];
    if (region->byte_offset != end ||
        end - start + region->byte_size >
            VKR_METAL_PACKET_TEXTURE_UPLOAD_CHUNK_BYTES)
      break;
    end += region->byte_size;
    ++next;
  }
  return next;
}

/* Payload bytes of the regions [first, end). */
vkr_internal uint64_t vkr_metal_packet_texture_upload_chunk_bytes(
    const VkrMetalPacketTextureUpload *payload, uint32_t first, uint32_t end) {
  const VkrTextureUploadRegion *last = &payload->regions[end - 1u];
  return last->byte_offset + last->byte_size -
         payload->regions[first].byte_offset;
}

/* The largest submission vkr_metal_packet_upload_texture makes for
   `payload`. */
vkr_internal uint64_t vkr_metal_packet_texture_upload_peak_bytes(
    const VkrMetalPacketTextureUpload *payload) {
  if (payload->data_size <= VKR_METAL_PACKET_TEXTURE_UPLOAD_CHUNK_BYTES)
    return payload->data_size;
  uint64_t peak = 0u;
  for (uint32_t first = 0u; first < payload->region_count;) {
    const uint32_t end =
        vkr_metal_packet_texture_upload_chunk_end(payload, first);
    peak = Max(
        peak, vkr_metal_packet_texture_upload_chunk_bytes(payload, first, end));
    first = end;
  }
  return peak;
}

/* A payload larger than VKR_METAL_PACKET_TEXTURE_UPLOAD_CHUNK_BYTES uploads
   in consecutive submissions of whole regions, so a lightmap set's page
   array (ADR-088) needs no publication slot the size of the set. */
vkr_internal bool8_t vkr_metal_packet_upload_texture(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketTextureUpload *payload, VkrTextureFormat format,
    id<MTLTexture> destination) API_AVAILABLE(macos(26.0)) {
  VkrTextureFormatInfo info = {0};
  if (!renderer || !vkr_metal_packet_validate_texture_upload(
                       payload, format, destination, &info))
    return false_v;
  const bool8_t chunked =
      payload->data_size > VKR_METAL_PACKET_TEXTURE_UPLOAD_CHUNK_BYTES;
  for (uint32_t first = 0u; first < payload->region_count;) {
    const uint32_t end =
        chunked ? vkr_metal_packet_texture_upload_chunk_end(payload, first)
                : payload->region_count;
    const uint64_t start = chunked ? payload->regions[first].byte_offset : 0u;
    const uint64_t size =
        chunked
            ? vkr_metal_packet_texture_upload_chunk_bytes(payload, first, end)
            : payload->data_size;
    const VkrMetalPacketTextureUpload chunk = {
        .data = payload->data + start,
        .data_size = size,
        .mip_levels = payload->mip_levels,
        .array_layers = payload->array_layers,
        .is_compressed = payload->is_compressed,
        .region_count = end - first,
        .regions = payload->regions + first,
    };
    VkrMetalRingSlice slice = {0};
    VkrMetalAddressPair upload = {0};
    id<MTLBuffer> upload_buffer = nil;
    id<MTL4ComputeCommandEncoder> encoder = nil;
    if (!vkr_metal_packet_begin_upload(renderer, size, &slice, &upload,
                                       &upload_buffer, &encoder))
      return false_v;
    MemCopy(upload.cpu_address, chunk.data, size);
    vkr_metal_packet_encode_texture_upload(&chunk, &info, upload_buffer,
                                           slice.offset, start, encoder,
                                           destination);
    if (!vkr_metal_packet_submit_upload(renderer, encoder, slice))
      return false_v;
    first = end;
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_create_dfg_lut(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
  descriptor.textureType = MTLTextureType2D;
  descriptor.pixelFormat = MTLPixelFormatRG16Float;
  descriptor.width = VKR_DFG_LUT_SIZE;
  descriptor.height = VKR_DFG_LUT_SIZE;
  descriptor.mipmapLevelCount = 1u;
  descriptor.usage = MTLTextureUsageShaderRead;
  const bool8_t created =
      vkr_metal_memory_device_create_texture(
          renderer->memory, descriptor, VKR_GPU_ALLOCATION_OWNER_SHADER,
          &renderer->dfg_lut) == VKR_METAL_MEMORY_STATUS_OK;
  [descriptor release];
  if (!created)
    return false_v;

  renderer->dfg_lut_live = true_v;
  const VkrTextureUploadRegion region = {
      .mip_level = 0u,
      .array_layer = 0u,
      .width = VKR_DFG_LUT_SIZE,
      .height = VKR_DFG_LUT_SIZE,
      .depth = 1u,
      .byte_offset = 0u,
      .byte_size = sizeof(vkr_dfg_lut_pixels),
  };
  const bool8_t uploaded = vkr_metal_packet_upload_texture(
      renderer,
      &(VkrMetalPacketTextureUpload){
          .data = (const uint8_t *)vkr_dfg_lut_pixels,
          .data_size = sizeof(vkr_dfg_lut_pixels),
          .mip_levels = 1u,
          .array_layers = 1u,
          .is_compressed = false_v,
          .region_count = 1u,
          .regions = &region,
      },
      VKR_TEXTURE_FORMAT_R16G16_SFLOAT,
      (id<MTLTexture>)renderer->dfg_lut.metal_texture);
  renderer->dfg_lut_last_use_submit_value = renderer->submit_value;
  return uploaded;
}

/* Generates the cloud noise once (ADR-074). Frames read it from later command
   buffers, which Metal 4 does not order after this one, so startup waits for
   the generation to complete. */
vkr_internal bool8_t vkr_metal_packet_create_cloud_noise(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  const uint32_t sizes[VKR_METAL_PACKET_CLOUD_NOISE_COUNT] = {
      VKR_CLOUD_BASE_NOISE_SIZE, VKR_CLOUD_DETAIL_NOISE_SIZE,
      VKR_CLOUD_WEATHER_SIZE};
  for (uint32_t i = 0u; i < VKR_METAL_PACKET_CLOUD_NOISE_COUNT; ++i) {
    const bool8_t volume = i < 2u;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = volume ? MTLTextureType3D : MTLTextureType2D;
    descriptor.pixelFormat = MTLPixelFormatR8Unorm;
    descriptor.width = sizes[i];
    descriptor.height = sizes[i];
    descriptor.depth = volume ? sizes[i] : 1u;
    descriptor.mipmapLevelCount = 1u;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    const bool8_t created =
        vkr_metal_memory_device_create_texture(
            renderer->memory, descriptor, VKR_GPU_ALLOCATION_OWNER_SHADER,
            &renderer->cloud_noise[i]) == VKR_METAL_MEMORY_STATUS_OK;
    [descriptor release];
    if (!created)
      return false_v;
    renderer->cloud_noise_live_count = i + 1u;
  }

  VkrMetalRingSlice slice = {0};
  VkrMetalAddressPair upload = {0};
  id<MTLBuffer> upload_buffer = nil;
  id<MTL4ComputeCommandEncoder> encoder = nil;
  if (!vkr_metal_packet_begin_upload(renderer, VKR_METAL_PACKET_ROOT_ALIGNMENT,
                                     &slice, &upload, &upload_buffer, &encoder))
    return false_v;
  const VkrMetalPacketCloudNoiseRoot root = {
      .base_texture_id = renderer->cloud_noise[0].resource_id,
      .detail_texture_id = renderer->cloud_noise[1].resource_id,
      .weather_texture_id = renderer->cloud_noise[2].resource_id,
  };
  MemCopy(upload.cpu_address, &root, sizeof(root));
  [renderer->argument_table setAddress:upload.gpu_address atIndex:0u];
  for (uint32_t i = 0u; i < VKR_METAL_PACKET_CLOUD_NOISE_COUNT; ++i) {
    const bool8_t volume = i < 2u;
    [encoder setComputePipelineState:renderer->cloud_noise_pipelines[i]];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder dispatchThreads:MTLSizeMake(sizes[i], sizes[i],
                                         volume ? sizes[i] : 1u)
        threadsPerThreadgroup:volume ? MTLSizeMake(4u, 4u, 4u)
                                     : MTLSizeMake(8u, 8u, 1u)];
  }
  const bool8_t submitted =
      vkr_metal_packet_submit_upload(renderer, encoder, slice);
  renderer->cloud_noise_last_use_submit_value = renderer->submit_value;
  return submitted && vkr_metal_packet_wait(renderer, renderer->submit_value);
}

vkr_internal bool8_t vkr_metal_packet_create_ltc_luts(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  for (uint32_t i = 0u; i < 2u; ++i) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.width = VKR_LTC_LUT_SIZE;
    descriptor.height = VKR_LTC_LUT_SIZE;
    descriptor.mipmapLevelCount = 1u;
    descriptor.usage = MTLTextureUsageShaderRead;
    const bool8_t created =
        vkr_metal_memory_device_create_texture(
            renderer->memory, descriptor, VKR_GPU_ALLOCATION_OWNER_SHADER,
            &renderer->ltc_luts[i]) == VKR_METAL_MEMORY_STATUS_OK;
    [descriptor release];
    if (!created)
      return false_v;
    ++renderer->ltc_lut_live_count;
    const VkrTextureUploadRegion region = {
        .width = VKR_LTC_LUT_SIZE,
        .height = VKR_LTC_LUT_SIZE,
        .depth = 1u,
        .byte_size = sizeof(vkr_ltc_lut_pixels[i]),
    };
    const bool8_t uploaded = vkr_metal_packet_upload_texture(
        renderer,
        &(VkrMetalPacketTextureUpload){
            .data = (const uint8_t *)vkr_ltc_lut_pixels[i],
            .data_size = sizeof(vkr_ltc_lut_pixels[i]),
            .mip_levels = 1u,
            .array_layers = 1u,
            .region_count = 1u,
            .regions = &region,
        },
        VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
        (id<MTLTexture>)renderer->ltc_luts[i].metal_texture);
    renderer->ltc_last_use_submit_value = renderer->submit_value;
    if (!uploaded)
      return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_create_sheen_luts(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  _Static_assert(sizeof(vkr_sheen_energy_lut_pixels) == 128u * 1024u,
                 "Sheen energy LUT must remain R16F 256x256");
  _Static_assert(sizeof(vkr_sheen_ltc_lut_pixels) == 128u * 1024u,
                 "Sheen LTC LUTs must remain four RGBA16F 64x64 tables");
  MTLTextureDescriptor *energy_descriptor = [MTLTextureDescriptor new];
  energy_descriptor.textureType = MTLTextureType2D;
  energy_descriptor.pixelFormat = MTLPixelFormatR16Float;
  energy_descriptor.width = VKR_SHEEN_ENERGY_LUT_SIZE;
  energy_descriptor.height = VKR_SHEEN_ENERGY_LUT_SIZE;
  energy_descriptor.mipmapLevelCount = 1u;
  energy_descriptor.usage = MTLTextureUsageShaderRead;
  const bool8_t energy_created =
      vkr_metal_memory_device_create_texture(
          renderer->memory, energy_descriptor, VKR_GPU_ALLOCATION_OWNER_SHADER,
          &renderer->sheen_directional_albedo_lut) ==
      VKR_METAL_MEMORY_STATUS_OK;
  [energy_descriptor release];
  if (!energy_created)
    return false_v;
  renderer->sheen_directional_albedo_lut_live = true_v;
  const VkrTextureUploadRegion energy_region = {
      .width = VKR_SHEEN_ENERGY_LUT_SIZE,
      .height = VKR_SHEEN_ENERGY_LUT_SIZE,
      .depth = 1u,
      .byte_size = sizeof(vkr_sheen_energy_lut_pixels),
  };
  if (!vkr_metal_packet_upload_texture(
          renderer,
          &(VkrMetalPacketTextureUpload){
              .data = (const uint8_t *)vkr_sheen_energy_lut_pixels,
              .data_size = sizeof(vkr_sheen_energy_lut_pixels),
              .mip_levels = 1u,
              .array_layers = 1u,
              .region_count = 1u,
              .regions = &energy_region,
          },
          VKR_TEXTURE_FORMAT_R16_SFLOAT,
          (id<MTLTexture>)renderer->sheen_directional_albedo_lut.metal_texture))
    return false_v;
  for (uint32_t table = 0u; table < VKR_SHEEN_LTC_LUT_TABLE_COUNT; ++table) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.width = VKR_SHEEN_LTC_LUT_SIZE;
    descriptor.height = VKR_SHEEN_LTC_LUT_SIZE;
    descriptor.mipmapLevelCount = 1u;
    descriptor.usage = MTLTextureUsageShaderRead;
    const bool8_t created =
        vkr_metal_memory_device_create_texture(
            renderer->memory, descriptor, VKR_GPU_ALLOCATION_OWNER_SHADER,
            &renderer->sheen_ltc_luts[table]) == VKR_METAL_MEMORY_STATUS_OK;
    [descriptor release];
    if (!created)
      return false_v;
    ++renderer->sheen_ltc_lut_live_count;
    const VkrTextureUploadRegion region = {
        .width = VKR_SHEEN_LTC_LUT_SIZE,
        .height = VKR_SHEEN_LTC_LUT_SIZE,
        .depth = 1u,
        .byte_size = sizeof(vkr_sheen_ltc_lut_pixels[table]),
    };
    if (!vkr_metal_packet_upload_texture(
            renderer,
            &(VkrMetalPacketTextureUpload){
                .data = (const uint8_t *)vkr_sheen_ltc_lut_pixels[table],
                .data_size = sizeof(vkr_sheen_ltc_lut_pixels[table]),
                .mip_levels = 1u,
                .array_layers = 1u,
                .region_count = 1u,
                .regions = &region,
            },
            VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
            (id<MTLTexture>)renderer->sheen_ltc_luts[table].metal_texture))
      return false_v;
  }
  renderer->sheen_lut_last_use_submit_value = renderer->submit_value;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_create_anisotropy_luts(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  _Static_assert(sizeof(vkr_anisotropy_lut_pixels) == 6u * 1024u * 1024u,
                 "Anisotropy tables must remain three 2 MiB RGBA16F arrays");
  VkrTextureUploadRegion regions[VKR_ANISOTROPY_LUT_LAYER_COUNT] = {0};
  const uint64_t layer_bytes =
      (uint64_t)VKR_ANISOTROPY_LUT_SIZE * VKR_ANISOTROPY_LUT_SIZE *
      VKR_ANISOTROPY_LUT_CHANNEL_COUNT * sizeof(uint16_t);
  for (uint32_t layer = 0u; layer < ArrayCount(regions); ++layer) {
    regions[layer] = (VkrTextureUploadRegion){
        .array_layer = layer,
        .width = VKR_ANISOTROPY_LUT_SIZE,
        .height = VKR_ANISOTROPY_LUT_SIZE,
        .depth = 1u,
        .byte_offset = layer * layer_bytes,
        .byte_size = layer_bytes,
    };
  }
  for (uint32_t table = 0u; table < VKR_ANISOTROPY_LUT_TABLE_COUNT; ++table) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType2DArray;
    descriptor.pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.width = VKR_ANISOTROPY_LUT_SIZE;
    descriptor.height = VKR_ANISOTROPY_LUT_SIZE;
    descriptor.arrayLength = VKR_ANISOTROPY_LUT_LAYER_COUNT;
    descriptor.mipmapLevelCount = 1u;
    descriptor.usage = MTLTextureUsageShaderRead;
    const bool8_t created =
        vkr_metal_memory_device_create_texture(
            renderer->memory, descriptor, VKR_GPU_ALLOCATION_OWNER_SHADER,
            &renderer->anisotropy_luts[table]) == VKR_METAL_MEMORY_STATUS_OK;
    [descriptor release];
    if (!created)
      return false_v;
    ++renderer->anisotropy_lut_live_count;
    if (!vkr_metal_packet_upload_texture(
            renderer,
            &(VkrMetalPacketTextureUpload){
                .data = (const uint8_t *)vkr_anisotropy_lut_pixels[table],
                .data_size = VKR_ANISOTROPY_LUT_TABLE_BYTE_SIZE,
                .mip_levels = 1u,
                .array_layers = VKR_ANISOTROPY_LUT_LAYER_COUNT,
                .region_count = ArrayCount(regions),
                .regions = regions,
            },
            VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
            (id<MTLTexture>)renderer->anisotropy_luts[table].metal_texture))
      return false_v;
  }
  renderer->anisotropy_lut_last_use_submit_value = renderer->submit_value;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_upload(
    VkrMetalPacketRenderer *renderer, const void *first, uint64_t first_size,
    id<MTLBuffer> first_destination, const void *second, uint64_t second_size,
    id<MTLBuffer> second_destination, id<MTLTexture> texture_destination,
    uint32_t texture_width, uint32_t texture_height)
    API_AVAILABLE(macos(26.0)) {
  const uint64_t total = first_size + second_size;
  if (!renderer || !first || first_size == 0 || total < first_size ||
      (!texture_destination && !first_destination) ||
      (texture_destination &&
       (texture_width == 0 || texture_height == 0 ||
        first_size != (uint64_t)texture_width * texture_height * 4u)) ||
      (second_size > 0 && (!second || !second_destination)))
    return false_v;
  VkrMetalRingSlice slice = {0};
  VkrMetalAddressPair upload = {0};
  id<MTLBuffer> upload_buffer = nil;
  id<MTL4ComputeCommandEncoder> encoder = nil;
  if (!vkr_metal_packet_begin_upload(renderer, total, &slice, &upload,
                                     &upload_buffer, &encoder))
    return false_v;
  MemCopy(upload.cpu_address, first, first_size);
  if (second_size > 0)
    MemCopy((uint8_t *)upload.cpu_address + first_size, second, second_size);
  if (texture_destination) {
    [encoder copyFromBuffer:upload_buffer
               sourceOffset:slice.offset
          sourceBytesPerRow:(uint64_t)texture_width * 4u
        sourceBytesPerImage:first_size
                 sourceSize:MTLSizeMake(texture_width, texture_height, 1)
                  toTexture:texture_destination
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
  } else {
    [encoder copyFromBuffer:upload_buffer
               sourceOffset:slice.offset
                   toBuffer:first_destination
          destinationOffset:0
                       size:first_size];
    if (second_size > 0) {
      [encoder copyFromBuffer:upload_buffer
                 sourceOffset:slice.offset + first_size
                     toBuffer:second_destination
            destinationOffset:0
                         size:second_size];
    }
  }
  return vkr_metal_packet_submit_upload(renderer, encoder, slice);
}

/* Appends `size` bytes for `destination` to the open upload batch, in
   chunks across as many batches as they need. Returns false with
   `*out_batched` clear when no batch could start, so the caller uploads them
   alone. */
vkr_internal bool8_t vkr_metal_packet_upload_buffer_batched(
    VkrMetalPacketRenderer *renderer, const uint8_t *bytes, uint64_t size,
    id<MTLBuffer> destination, uint64_t destination_offset,
    bool8_t *out_batched) API_AVAILABLE(macos(26.0)) {
  *out_batched = false_v;
  VkrMetalPacketTextureUploadBatch *batch = &renderer->texture_upload_batch;
  for (uint64_t offset = 0; offset < size;) {
    uint64_t at =
        vkr_metal_packet_align_up(batch->used, VKR_METAL_PACKET_ROOT_ALIGNMENT);
    if (batch->active && at >= batch->capacity) {
      if (!vkr_metal_packet_texture_upload_batch_flush(renderer, false_v))
        return false_v;
      batch = &renderer->texture_upload_batch;
    }
    if (!batch->active) {
      if (!vkr_metal_packet_texture_upload_batch_start(
              renderer, VKR_METAL_PACKET_TEXTURE_UPLOAD_BATCH_BYTES))
        return false_v;
      at = 0u;
    }
    *out_batched = true_v;
    const uint64_t chunk = Min(size - offset, batch->capacity - at);
    MemCopy((uint8_t *)batch->upload.cpu_address + at, bytes + offset, chunk);
    [batch->encoder copyFromBuffer:batch->upload_buffer
                      sourceOffset:batch->slice.offset + at
                          toBuffer:destination
                 destinationOffset:destination_offset + offset
                              size:chunk];
    batch->used = at + chunk;
    batch->buffer_count++;
    offset += chunk;
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_upload_buffer_chunks_at(
    VkrMetalPacketRenderer *renderer, const void *source, uint64_t size,
    id<MTLBuffer> destination, uint64_t destination_offset)
    API_AVAILABLE(macos(26.0)) {
  if (!renderer || !source || size == 0 || !destination ||
      renderer->upload_slot_size == 0)
    return false_v;
  /* A publication run's buffers share one submission while a batch is
     open, rather than one command slot each. */
  if (renderer->texture_upload_batch.enabled) {
    bool8_t batched = false_v;
    if (vkr_metal_packet_upload_buffer_batched(
            renderer, source, size, destination, destination_offset, &batched))
      return true_v;
    if (batched)
      return false_v;
  }

  const uint8_t *bytes = source;
  for (uint64_t offset = 0; offset < size;) {
    const uint64_t chunk_size = Min(size - offset, renderer->upload_slot_size);
    VkrMetalRingSlice slice = {0};
    VkrMetalAddressPair upload = {0};
    id<MTLBuffer> upload_buffer = nil;
    id<MTL4ComputeCommandEncoder> encoder = nil;
    if (!vkr_metal_packet_begin_upload(renderer, chunk_size, &slice, &upload,
                                       &upload_buffer, &encoder))
      return false_v;
    MemCopy(upload.cpu_address, bytes + offset, chunk_size);
    [encoder copyFromBuffer:upload_buffer
               sourceOffset:slice.offset
                   toBuffer:destination
          destinationOffset:destination_offset + offset
                       size:chunk_size];
    if (!vkr_metal_packet_submit_upload(renderer, encoder, slice))
      return false_v;
    offset += chunk_size;
  }
  return true_v;
}

vkr_internal uint32_t vkr_metal_packet_mip_count(uint32_t extent) {
  uint32_t count = 0;
  while (extent > 0) {
    count++;
    extent >>= 1u;
  }
  return count;
}

/* Validates create arguments before anything is allocated. */
vkr_internal bool8_t vkr_metal_packet_create_arguments_valid(
    const VkrMetalPacketRendererConfig *config,
    VkrMetalPacketRenderer **out_renderer) {
  if (!config || !out_renderer || !config->allocator || !config->graph_path ||
      !config->slang_msl_path || !config->fragment_msl_path ||
      config->heap_chunk_size == 0 || config->upload_ring_size == 0 ||
      config->upload_ring_size > config->upload_ring_max_size ||
      config->frame_slot_count < 2u || config->capture_ring_capacity == 0 ||
      config->capture_ring_capacity > VKR_CAPTURE_RING_CAPACITY_MAX ||
      config->capture_max_batch_bytes == 0 ||
      config->frame_slot_count > UINT32_MAX - 2u ||
      config->frame_slot_count + 2u > VKR_METAL_PACKET_GRAPH_INSTANCE_MAX ||
      config->upload_ring_size % ((uint64_t)config->frame_slot_count * 4u) !=
          0 ||
      config->max_images == 0 || config->max_passes == 0 ||
      config->max_passes > VKR_METAL_PACKET_MAX_PASS_TIMINGS ||
      config->max_material_rows == 0 || config->max_meshes == 0 ||
      config->max_textures == 0 || config->max_draws == 0 ||
      config->max_instances == 0 ||
      (config->upload_ring_size / config->frame_slot_count) %
              VKR_METAL_PACKET_ROOT_ALIGNMENT !=
          0 ||
      config->target_kind > VKR_METAL_PACKET_TARGET_WINDOW ||
      (config->target_kind == VKR_METAL_PACKET_TARGET_WINDOW &&
       !config->metal_layer))
    return false_v;
  return true_v;
}

/* Ring and allocation-record capacities derived from a valid create config. */
typedef struct VkrMetalPacketCreateCapacities {
  uint64_t readback_ring_size;
  uint64_t readback_ring_max_size;
  uint64_t memory_record_capacity;
} VkrMetalPacketCreateCapacities;

vkr_internal bool8_t vkr_metal_packet_create_capacities(
    const VkrMetalPacketRendererConfig *config,
    VkrMetalPacketCreateCapacities *out_capacities) {
  const VkrMetalPacketReadbackLayout maximum_readback =
      vkr_metal_packet_readback_layout(true_v, true_v,
                                       VKR_SHADOW_CASCADE_COUNT_MAX);
  const uint64_t readback_prefix_size = vkr_metal_packet_align_up(
      maximum_readback.prefix_size, VKR_CAPTURE_BUFFER_ALIGNMENT);
  if (config->capture_max_batch_bytes > UINT64_MAX - 3u)
    return false_v;
  const uint64_t aligned_capture_size =
      vkr_metal_packet_align_up(config->capture_max_batch_bytes, 4u);
  if (aligned_capture_size > UINT64_MAX - readback_prefix_size)
    return false_v;
  uint64_t readback_ring_max_size = 0u;
  if (!vkr_metal_submit_ring_total_size(
          readback_prefix_size + aligned_capture_size, MB(48),
          config->frame_slot_count, &readback_ring_max_size))
    return false_v;
  const uint64_t readback_ring_size = KB(64) * config->frame_slot_count;
  /* A replacement is created before the current generation retires. Capacity
     therefore covers both generations of the larger command- or
     history-instance domain. */
  const uint64_t command_instances_per_resource =
      2u * config->frame_slot_count +
      (config->frame_slot_count > 2u ? config->frame_slot_count - 2u : 0u);
  const uint64_t history_instances_per_resource =
      2u * (config->frame_slot_count + 2u);
  const uint64_t graph_instances_per_resource =
      MAX(command_instances_per_resource, history_instances_per_resource);
  const uint64_t graph_allocation_capacity =
      (uint64_t)config->max_images * graph_instances_per_resource * 2u;
  const uint64_t memory_record_capacity =
      graph_allocation_capacity + (uint64_t)config->max_meshes * 2u +
      (uint64_t)config->max_material_rows * 4u + config->max_textures + 4u;
  if (memory_record_capacity >= UINT32_MAX)
    return false_v;
  *out_capacities = (VkrMetalPacketCreateCapacities){
      .readback_ring_size = readback_ring_size,
      .readback_ring_max_size = readback_ring_max_size,
      .memory_record_capacity = memory_record_capacity,
  };
  return true_v;
}

/* Copies create settings into the zeroed renderer, opens optional
 * diagnostics, creates the graph frame arena and resolves the initial display
 * output. */
vkr_internal void
vkr_metal_packet_configure_renderer(VkrMetalPacketRenderer *renderer,
                                    const VkrMetalPacketRendererConfig *config)
    API_AVAILABLE(macos(26.0)) {
  renderer->allocator = config->allocator;
  const char *diagnostics_directory = getenv("VKR_METAL_DIAGNOSTICS_DIR");
  if (diagnostics_directory && diagnostics_directory[0]) {
    (void)vkr_metal_diagnostics_open(&renderer->diagnostics,
                                     diagnostics_directory);
    VKR_METAL_DIAGNOSTIC(renderer, "renderer_create",
                         "target=%u extent=%ux%u slots=%u managed_budget=%llu",
                         config->target_kind, config->target_width,
                         config->target_height, config->frame_slot_count,
                         (unsigned long long)config->managed_budget_size);
    VKR_METAL_DIAGNOSTIC_FLUSH(renderer);
  }
  renderer->exposure_metering = vkr_exposure_metering_config_normalize(NULL);
  renderer->bloom_config = vkr_bloom_config_normalize(&config->bloom);
  renderer->candidate_publication_generation = 1u;
  renderer->graph_frame_arena = arena_create(MB(8), KB(256));
  renderer->graph_frame_allocator =
      (VkrAllocator){.ctx = renderer->graph_frame_arena};
  renderer->max_images = config->max_images;
  renderer->max_passes = config->max_passes;
  renderer->max_meshes = config->max_meshes;
  renderer->max_materials = config->max_material_rows;
  renderer->max_textures = config->max_textures;
  renderer->hzb_enabled = config->hzb_enabled;
  renderer->frustum_enabled = config->frustum_enabled;
  renderer->max_samplers = VKR_METAL_PACKET_SAMPLER_CACHE_CAPACITY;
  renderer->max_draws = config->max_draws;
  renderer->max_instances = config->max_instances;
  renderer->command_slot_count = config->frame_slot_count;
  /* One upload slot per upload ring slot (ring_slot_count below). */
  renderer->upload_slot_count =
      Min(config->frame_slot_count, VKR_METAL_PACKET_UPLOAD_SLOT_COUNT_MAX);
  /* At most N-1 command slots remain in flight. Their distinct outputs plus
     one shared cloud-history input and one shared HZB input can occupy N+1
     instances, so one additional output must remain completion-safe. */
  renderer->history_instance_count = config->frame_slot_count + 2u;
  renderer->upload_slot_size =
      config->upload_ring_max_size / config->frame_slot_count;
  renderer->target_kind = config->target_kind;
  renderer->display_output_mode = config->display_output_mode;
  renderer->display_output_context = config->display_output_context;
  renderer->display_output_snapshot = config->display_output_snapshot;
  const bool8_t query_display_output =
      renderer->display_output_mode ==
          VKR_DISPLAY_OUTPUT_AUTO_EXTENDED_LINEAR &&
      renderer->target_kind == VKR_METAL_PACKET_TARGET_WINDOW &&
      renderer->display_output_snapshot;
  const VkrDisplayOutputSnapshot initial_display_snapshot =
      query_display_output
          ? renderer->display_output_snapshot(renderer->display_output_context)
          : (VkrDisplayOutputSnapshot){0};
  renderer->display_output = vkr_metal_packet_window_output_capability(
      renderer, vkr_display_output_resolve(
                    renderer->display_output_mode, initial_display_snapshot,
                    renderer->target_kind == VKR_METAL_PACKET_TARGET_WINDOW));
  renderer->srgb_output = config->srgb_output;
  renderer->tonemap_enabled = config->tonemap_enabled;
  renderer->dynamic_resolution_enabled = config->dynamic_resolution.enabled;
  renderer->convert_vulkan_clip_y = config->convert_vulkan_clip_y;
  renderer->synchronous_validation_readback =
      config->synchronous_validation_readback;
}

/* Reserves capture-ring storage. A zero requirement reserves nothing. */
vkr_internal bool8_t vkr_metal_packet_create_capture_storage(
    VkrMetalPacketRenderer *renderer, uint64_t capture_storage_size) {
  if (capture_storage_size) {
    /* Own reservation; see capture_storage_memory in the renderer struct.
     */
    if (capture_storage_size > UINT64_MAX - KB(64)) {
      return false_v;
    }
    const uint64_t capture_allocator_size = capture_storage_size + KB(64);
    if (!vkr_dmemory_create(capture_allocator_size, capture_allocator_size,
                            &renderer->capture_storage_memory)) {
      return false_v;
    }
    renderer->capture_storage_allocator =
        (VkrAllocator){.ctx = &renderer->capture_storage_memory};
    vkr_dmemory_allocator_create(&renderer->capture_storage_allocator);
    renderer->capture_storage = vkr_allocator_alloc(
        &renderer->capture_storage_allocator, capture_storage_size,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!renderer->capture_storage) {
      return false_v;
    }
  }
  return true_v;
}

/* Carves every fixed-capacity record array from one tagged reservation. */
vkr_internal bool8_t
vkr_metal_packet_create_record_arrays(VkrMetalPacketRenderer *renderer) {
  /* One reservation for every record array. Each vkr_dmemory_alloc rounds
     up internally, so the total carries slack per array plus a fixed
     margin. */
  const uint64_t record_bytes[] = {
      vkr_metal_packet_images_bytes(renderer),
      vkr_metal_packet_graph_buffers_bytes(renderer),
      vkr_metal_packet_graph_buffer_instances_bytes(renderer),
      vkr_metal_packet_meshes_bytes(renderer),
      vkr_metal_packet_materials_bytes(renderer),
      vkr_metal_packet_textures_bytes(renderer),
      vkr_metal_packet_samplers_bytes(renderer),
      vkr_metal_packet_text_uploads_bytes(renderer),
      vkr_metal_packet_render_passes_bytes(renderer),
      vkr_metal_packet_command_slots_bytes(renderer),
      vkr_metal_packet_timing_results_bytes(renderer),
      vkr_metal_packet_commit_feedback_bytes(renderer),
      vkr_metal_packet_retired_image_views_bytes(renderer),
      vkr_metal_packet_pass_labels_bytes(renderer),
  };
  uint64_t record_total = KB(64);
  bool8_t record_size_valid = true_v;
  for (uint32_t i = 0; record_size_valid && i < ArrayCount(record_bytes); ++i)
    record_size_valid =
        vkr_metal_packet_record_size_add(&record_total, record_bytes[i]);
  if (!record_size_valid || !vkr_dmemory_create(record_total, record_total,
                                                &renderer->record_memory)) {
    return false_v;
  }
  renderer->record_allocator = (VkrAllocator){.ctx = &renderer->record_memory};
  vkr_dmemory_allocator_create(&renderer->record_allocator);
  VkrAllocator *records = &renderer->record_allocator;
  renderer->images =
      vkr_metal_packet_record_alloc(records, record_bytes[0], true_v);
  renderer->graph_buffers =
      vkr_metal_packet_record_alloc(records, record_bytes[1], true_v);
  renderer->graph_buffer_instances =
      vkr_metal_packet_record_alloc(records, record_bytes[2], true_v);
  renderer->meshes =
      vkr_metal_packet_record_alloc(records, record_bytes[3], true_v);
  renderer->packet_materials =
      vkr_metal_packet_record_alloc(records, record_bytes[4], true_v);
  renderer->textures =
      vkr_metal_packet_record_alloc(records, record_bytes[5], true_v);
  renderer->samplers =
      vkr_metal_packet_record_alloc(records, record_bytes[6], false_v);
  renderer->text_uploads =
      vkr_metal_packet_record_alloc(records, record_bytes[7], false_v);
  renderer->render_passes =
      vkr_metal_packet_record_alloc(records, record_bytes[8], true_v);
  renderer->command_slots =
      vkr_metal_packet_record_alloc(records, record_bytes[9], true_v);
  renderer->completed_timing_results =
      vkr_metal_packet_record_alloc(records, record_bytes[10], true_v);
  renderer->commit_feedback_records =
      vkr_metal_packet_record_alloc(records, record_bytes[11], true_v);
  renderer->retired_image_views =
      vkr_metal_packet_record_alloc(records, record_bytes[12], true_v);
  renderer->pass_labels =
      vkr_metal_packet_record_alloc(records, record_bytes[13], true_v);
  if (!renderer->images || !renderer->graph_buffers ||
      !renderer->graph_buffer_instances || !renderer->meshes ||
      !renderer->packet_materials || !renderer->textures ||
      !renderer->samplers || !renderer->text_uploads ||
      !renderer->render_passes || !renderer->command_slots ||
      !renderer->completed_timing_results ||
      !renderer->commit_feedback_records || !renderer->retired_image_views ||
      !renderer->pass_labels) {
    return false_v;
  }
  return true_v;
}

/* Creates each command slot's allocator, command buffer and pass timestamp
 * heap until one is missing, then samples the device timestamp frequency. */
vkr_internal bool8_t vkr_metal_packet_create_command_slots(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketRendererConfig *config) API_AVAILABLE(macos(26.0)) {
  MTL4CounterHeapDescriptor *timestamp_descriptor =
      [MTL4CounterHeapDescriptor new];
  timestamp_descriptor.type = MTL4CounterHeapTypeTimestamp;
  timestamp_descriptor.count = (NSUInteger)config->max_passes * 2u;
  bool8_t command_slots_created = renderer->command_slots != NULL;
  for (uint32_t i = 0;
       command_slots_created && i < renderer->command_slot_count; ++i) {
    VkrMetalPacketCommandSlot *slot = &renderer->command_slots[i];
    slot->allocator = [renderer->device newCommandAllocator];
    slot->buffer = [renderer->device newCommandBuffer];
    slot->timestamp_heap =
        [renderer->device newCounterHeapWithDescriptor:timestamp_descriptor
                                                 error:nil];
    command_slots_created =
        slot->allocator && slot->buffer && slot->timestamp_heap;
  }
  for (uint32_t i = 0; command_slots_created && i < renderer->upload_slot_count;
       ++i) {
    VkrMetalPacketCommandSlot *slot = &renderer->upload_slots[i];
    slot->allocator = [renderer->device newCommandAllocator];
    slot->buffer = [renderer->device newCommandBuffer];
    command_slots_created = slot->allocator && slot->buffer;
  }
  renderer->timestamp_frequency = [renderer->device queryTimestampFrequency];
  [timestamp_descriptor release];
  return command_slots_created;
}

/* Creates each reusable commit option and feedback handler pair until one is
 * missing. */
vkr_internal bool8_t vkr_metal_packet_create_commit_feedback_records(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  bool8_t commit_feedback_created = renderer->commit_feedback_records != NULL;
  for (uint32_t i = 0u;
       commit_feedback_created && i < VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY;
       ++i) {
    VkrMetalPacketCommitFeedbackRecord *record =
        &renderer->commit_feedback_records[i];
    record->options = [MTL4CommitOptions new];
    if (!record->options) {
      commit_feedback_created = false_v;
      break;
    }
    record->ready_handler = Block_copy(^{
      vkr_atomic_bool_store(&record->feedback_ready, true_v,
                            VKR_MEMORY_ORDER_RELEASE);
    });
    record->handler = Block_copy(^(id<MTL4CommitFeedback> feedback) {
      @autoreleasepool {
        record->gpu_start_time = feedback.GPUStartTime;
        record->gpu_end_time = feedback.GPUEndTime;
        record->has_error = feedback.error != nil ? true_v : false_v;
        dispatch_async(vkr_metal_packet_feedback_queue(),
                       record->ready_handler);
      }
    });
    if (!record->handler || !record->ready_handler)
      commit_feedback_created = false_v;
  }
  return commit_feedback_created;
}

/* An explicit request wins. Otherwise two thirds of the recommended working
 * set leaves room for driver and drawable allocations outside the cap and
 * for the rest of the system on unified memory. */
vkr_internal uint64_t vkr_metal_packet_managed_budget(id<MTLDevice> device,
                                                      uint64_t requested) {
  if (requested)
    return requested;
  const uint64_t recommended = device.recommendedMaxWorkingSetSize;
  return recommended ? recommended / 3u * 2u : GB(5);
}

/* Custom material graphs (vkr_metal_packet_custom.inc) build on the tiled
   entry table above. */
vkr_internal bool8_t
vkr_metal_packet_custom_load(VkrMetalPacketRenderer *renderer, const char *path)
    API_AVAILABLE(macos(26.0));

/* Creates the memory device, material table, IBL targets, lookup textures,
 * pipelines, executor registry and render graph, then finalizes the pipeline
 * archive. */
vkr_internal bool8_t vkr_metal_packet_create_device_resources(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketRendererConfig *config,
    const VkrMetalPacketCreateCapacities *capacities)
    API_AVAILABLE(macos(26.0)) {
  return vkr_metal_memory_device_create(
             &(VkrMetalMemoryDeviceConfig){
                 .metal_device = renderer->device,
                 .allocator = renderer->allocator,
                 .diagnostics = &renderer->diagnostics,
                 .managed_budget_size = vkr_metal_packet_managed_budget(
                     renderer->device, config->managed_budget_size),
                 .heap_chunk_size = config->heap_chunk_size,
                 .upload_ring_size = config->upload_ring_size,
                 .upload_ring_max_size = config->upload_ring_max_size,
                 .readback_ring_size = capacities->readback_ring_size,
                 .readback_ring_max_size = capacities->readback_ring_max_size,
                 .ring_slot_count = config->frame_slot_count,
                 .max_allocations =
                     (uint32_t)capacities->memory_record_capacity,
                 .max_retirements =
                     (uint32_t)capacities->memory_record_capacity,
                 .max_free_ranges =
                     (uint32_t)capacities->memory_record_capacity + 1u,
             },
             &renderer->memory) == VKR_METAL_MEMORY_STATUS_OK &&
         vkr_metal_material_table_device_create(
             &(VkrMetalMaterialTableConfig){
                 .max_rows = config->max_material_rows,
                 .max_retirements = config->max_material_rows,
             },
             renderer->device, renderer->memory, renderer->allocator,
             &renderer->materials) == VKR_METAL_MATERIAL_STATUS_OK &&
         vkr_metal_packet_create_ibl_targets(renderer) &&
         vkr_metal_packet_create_dfg_lut(renderer) &&
         vkr_metal_packet_create_ltc_luts(renderer) &&
         vkr_metal_packet_create_sheen_luts(renderer) &&
         vkr_metal_packet_create_anisotropy_luts(renderer) &&
         vkr_metal_packet_create_pipelines(
             renderer, config->slang_msl_path, config->slang_metallib_path,
             config->fragment_msl_path, config->fragment_metallib_path) &&
         vkr_metal_packet_custom_load(renderer, config->custom_library_path) &&
         vkr_metal_packet_create_cloud_noise(renderer) &&
         vkr_rg_executor_registry_init(&renderer->executors,
                                       renderer->allocator) &&
         vkr_render_graph_register_executors(&renderer->executors) &&
         vkr_rg_json_load_file(renderer->allocator, config->graph_path,
                               &renderer->json_graph) &&
         vkr_rg_json_bind_executors(&renderer->json_graph,
                                    &renderer->executors) &&
         vkr_metal_packet_validate_graph_executors(&renderer->json_graph) &&
         (renderer->graph = vkr_rg_create(renderer->allocator)) != NULL &&
         vkr_rg_set_frame_allocator(renderer->graph,
                                    &renderer->graph_frame_allocator) &&
         vkr_metal_packet_finalize_archive(renderer,
                                           config->pipeline_archive_path);
}

bool8_t
vkr_metal_packet_renderer_create(const VkrMetalPacketRendererConfig *config,
                                 VkrMetalPacketRenderer **out_renderer) {
  @autoreleasepool {
    if (!vkr_metal_packet_create_arguments_valid(config, out_renderer)) {
      return false_v;
    }
    VkrMetalPacketCreateCapacities capacities = {0};
    if (!vkr_metal_packet_create_capacities(config, &capacities)) {
      return false_v;
    }
    *out_renderer = NULL;

    if (@available(macOS 26.0, *)) {
      VkrMetalPacketRenderer *renderer =
          vkr_metal_packet_alloc_zeroed(config->allocator, sizeof(*renderer));
      if (!renderer)
        return false_v;
      vkr_metal_packet_configure_renderer(renderer, config);
      const uint64_t capture_storage_size =
          vkr_capture_ring_storage_requirement(config->capture_ring_capacity,
                                               config->capture_max_batch_bytes);
      if (!vkr_metal_packet_create_capture_storage(renderer,
                                                   capture_storage_size)) {
        vkr_metal_packet_renderer_destroy(renderer);
        return false_v;
      }
      if (!vkr_metal_packet_create_record_arrays(renderer)) {
        vkr_metal_packet_renderer_destroy(renderer);
        return false_v;
      }
      if (!vkr_geometry_ranges_create(&renderer->geometry_ranges,
                                      renderer->allocator,
                                      renderer->max_meshes)) {
        vkr_metal_packet_renderer_destroy(renderer);
        return false_v;
      }
      renderer->device = [MTLCreateSystemDefaultDevice() retain];
      if (renderer->device &&
          ![renderer->device supportsFamily:MTLGPUFamilyApple9])
        renderer->max_samplers =
            Min(renderer->max_samplers,
                (uint32_t)(VKR_METAL_PACKET_APPLE7_ARGUMENT_SAMPLER_LIMIT -
                           VKR_METAL_PACKET_CONSTEXPR_SAMPLER_RESERVE));
      if (![renderer->device.name getCString:renderer->device_name
                                   maxLength:sizeof(renderer->device_name)
                                    encoding:NSUTF8StringEncoding])
        snprintf(renderer->device_name, sizeof(renderer->device_name),
                 "unavailable");
      MTL4CommandQueueDescriptor *queue_descriptor =
          [MTL4CommandQueueDescriptor new];
      queue_descriptor.feedbackQueue = vkr_metal_packet_feedback_queue();
      renderer->queue =
          [renderer->device newMTL4CommandQueueWithDescriptor:queue_descriptor
                                                        error:nil];
      [queue_descriptor release];
      renderer->completion = [renderer->device newSharedEvent];
      const bool8_t command_slots_created =
          vkr_metal_packet_create_command_slots(renderer, config);
      const bool8_t commit_feedback_created =
          vkr_metal_packet_create_commit_feedback_records(renderer);
      if (config->target_kind == VKR_METAL_PACKET_TARGET_WINDOW)
        renderer->layer = [(CAMetalLayer *)config->metal_layer retain];
      bool8_t passed =
          renderer->graph_frame_arena &&
          vkr_allocator_arena(&renderer->graph_frame_allocator) &&
          renderer->capture_storage && command_slots_created &&
          vkr_capture_ring_init(
              &renderer->capture_ring, config->capture_ring_capacity,
              config->capture_max_batch_bytes, renderer->capture_storage,
              capture_storage_size) &&
          renderer->device &&
          [renderer->device supportsFamily:MTLGPUFamilyMetal4] &&
          vkr_metal_packet_create_compiler(renderer,
                                           config->pipeline_archive_path) &&
          renderer->queue && renderer->completion && commit_feedback_created &&
          renderer->timestamp_frequency > 0u;
      if (config->target_kind == VKR_METAL_PACKET_TARGET_WINDOW) {
        passed = passed && renderer->layer;
        renderer->actual_present_mode =
            config->requested_present_mode == VKR_PRESENT_MODE_IMMEDIATE
                ? VKR_PRESENT_MODE_IMMEDIATE
                : VKR_PRESENT_MODE_FIFO;
        renderer->layer.device = renderer->device;
        if (!vkr_metal_packet_configure_window_output(renderer,
                                                      renderer->display_output))
          passed = false_v;
        renderer->layer.framebufferOnly = NO;
        renderer->layer.opaque = YES;
        renderer->layer.displaySyncEnabled =
            renderer->actual_present_mode != VKR_PRESENT_MODE_IMMEDIATE;
        if (passed) {
          id<MTLResidencySet> drawable_residency = renderer->layer.residencySet;
          passed = drawable_residency != nil;
          if (passed) {
            /* The layer tracks drawable replacements in this read-only set.
             * Its device stays fixed and it outlives the renderer's queue. */
            [renderer->queue addResidencySet:drawable_residency];
            VKR_METAL_DIAGNOSTIC(renderer, "drawable_residency",
                                 "registered=1");
          } else {
            log_error("Metal window layer has no drawable residency set");
          }
        }
      } else {
        renderer->actual_present_mode = VKR_PRESENT_MODE_DEFAULT;
      }
      for (uint32_t i = 0; passed && i < config->max_passes; ++i) {
        renderer->render_passes[i] = [MTL4RenderPassDescriptor new];
        passed = renderer->render_passes[i] != nil;
      }
      passed = passed && vkr_metal_packet_create_device_resources(
                             renderer, config, &capacities);
      if (passed)
        passed = vkr_metal_packet_create_gpu_draw_icb_residencies(renderer);
      if (!passed) {
        vkr_metal_packet_renderer_destroy(renderer);
        return false_v;
      }
      log_info(
          "Metal managed GPU budget: %llu MiB (demand-created heaps; driver "
          "recommended "
          "working set: %llu MiB)",
          (unsigned long long)(vkr_metal_packet_managed_budget(
                                   renderer->device,
                                   config->managed_budget_size) /
                               MB(1)),
          (unsigned long long)(renderer->device.recommendedMaxWorkingSetSize /
                               MB(1)));
      *out_renderer = renderer;
      return true_v;
    }
    return false_v;
  }
}
