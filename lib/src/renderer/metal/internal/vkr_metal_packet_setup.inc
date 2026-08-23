/* Shader/pipeline creation, device resources, uploads, and renderer startup. */
vkr_internal void vkr_metal_packet_print_error(const char *step,
                                               NSError *error) {
  (void)error;
  fprintf(stderr, "metal.packet.%s: Metal compilation failed\n", step);
}

vkr_internal id<MTLLibrary>
vkr_metal_packet_create_library(VkrMetalPacketRenderer *renderer,
                                const char *path, NSString *name)
    API_AVAILABLE(macos(26.0)) {
  NSError *error = nil;
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
  if (record_id == VKR_METAL_PACKET_ABI_TRANSMISSION_PEEL_ROOT)
    return 2;
  switch (record_id) {
  case VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT:
  case VKR_METAL_PACKET_ABI_EQUIRECT_ROOT:
  case VKR_METAL_PACKET_ABI_IRRADIANCE_ROOT:
  case VKR_METAL_PACKET_ABI_PREFILTER_ROOT:
  case VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT:
  case VKR_METAL_PACKET_ABI_GBUFFER_RESOLVE_ROOT:
  case VKR_METAL_PACKET_ABI_DEFERRED_LIGHTING_ROOT:
  case VKR_METAL_PACKET_ABI_TRANSMISSION_SHADE_ROOT:
  case VKR_METAL_PACKET_ABI_TRANSMISSION_COVERAGE_ROOT:
  case VKR_METAL_PACKET_ABI_TRANSMISSION_COMPACT_ROOT:
  case VKR_METAL_PACKET_ABI_PICKING_RESOLVE_ROOT:
  case VKR_METAL_PACKET_ABI_HZB_BUILD_ROOT:
    return 0;
  default:
    return 1;
  }
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
  for (uint32_t i = 0; i < record->field_count; ++i) {
    const VkrMetalPacketAbiField *field = &record->fields[i];
    MTLStructMember *member = [shader_type
        memberByName:[NSString stringWithUTF8String:field->shader_name]];
    for (uint32_t suffix = 0; !member && suffix < 4; ++suffix) {
      NSString *slang_name =
          [NSString stringWithFormat:@"%s_%u", field->shader_name, suffix];
      member = [shader_type memberByName:slang_name];
    }
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

vkr_internal bool8_t vkr_metal_packet_validate_nested_abi(
    NSString *pipeline_label, MTLStructType *root_type, const char *member_name,
    VkrMetalPacketAbiRecordId record_id) API_AVAILABLE(macos(26.0)) {
  MTLStructMember *member =
      [root_type memberByName:[NSString stringWithUTF8String:member_name]];
  if (!member) {
    NSString *slang_name = [NSString stringWithFormat:@"%s_0", member_name];
    member = [root_type memberByName:slang_name];
  }
  MTLPointerType *pointer = member ? member.pointerType : nil;
  return member && pointer &&
                 vkr_metal_packet_validate_abi_type(
                     pipeline_label, pointer.elementStructType,
                     pointer.dataSize, pointer.alignment, record_id)
             ? true_v
             : false_v;
}

vkr_internal bool8_t vkr_metal_packet_validate_nested_gpu_abi(
    NSString *pipeline_label, MTLStructType *root_type, const char *member_name,
    VkrGpuAbiRecordId record_id) API_AVAILABLE(macos(26.0)) {
  MTLStructMember *member =
      [root_type memberByName:[NSString stringWithUTF8String:member_name]];
  if (!member) {
    NSString *slang_name = [NSString stringWithFormat:@"%s_0", member_name];
    member = [root_type memberByName:slang_name];
  }
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
    MTLStructMember *field_member = [pointer.elementStructType
        memberByName:[NSString stringWithUTF8String:field->shader_name]];
    for (uint32_t suffix = 0; !field_member && suffix < 4; ++suffix) {
      NSString *slang_name =
          [NSString stringWithFormat:@"%s_%u", field->shader_name, suffix];
      field_member = [pointer.elementStructType memberByName:slang_name];
    }
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
      [root_type memberByName:[NSString stringWithUTF8String:member_name]];
  if (!member) {
    NSString *slang_name = [NSString stringWithFormat:@"%s_0", member_name];
    member = [root_type memberByName:slang_name];
  }
  return member ? member.pointerType.elementStructType : nil;
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
  if (record_id == VKR_METAL_PACKET_ABI_DRAW_ROOT ||
      record_id == VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT) {
    if (!vkr_metal_packet_validate_nested_gpu_abi(
            pipeline_label, root.bufferStructType, "geometry_rows",
            VKR_GPU_ABI_GEOMETRY_ROW) ||
        !vkr_metal_packet_validate_nested_gpu_abi(
            pipeline_label, root.bufferStructType, "visible_rows",
            VKR_GPU_ABI_VISIBLE_DRAW_ROW) ||
        !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                              root.bufferStructType, "vertices",
                                              VKR_METAL_PACKET_ABI_VERTEX) ||
        !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                              root.bufferStructType, "frame",
                                              VKR_METAL_PACKET_ABI_FRAME_ROOT))
      return false_v;
    MTLStructType *frame_type =
        vkr_metal_packet_nested_struct(root.bufferStructType, "frame");
    if (!frame_type ||
        !vkr_metal_packet_validate_nested_abi(pipeline_label, frame_type,
                                              "instances",
                                              VKR_METAL_PACKET_ABI_INSTANCE) ||
        !vkr_metal_packet_validate_nested_abi(pipeline_label, frame_type,
                                              "ibl_probes",
                                              VKR_METAL_PACKET_ABI_IBL_PROBE) ||
        !vkr_metal_packet_validate_nested_abi(
            pipeline_label, frame_type, "shadow_cascades",
            VKR_METAL_PACKET_ABI_SHADOW_CASCADE) ||
        (record_id == VKR_METAL_PACKET_ABI_DRAW_ROOT &&
         !vkr_metal_packet_validate_nested_abi(pipeline_label, frame_type,
                                               "materials",
                                               VKR_METAL_PACKET_ABI_MATERIAL)))
      return false_v;
  }
  if (record_id == VKR_METAL_PACKET_ABI_TEXT_ROOT &&
      !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                            root.bufferStructType, "vertices",
                                            VKR_METAL_PACKET_ABI_TEXT_VERTEX))
    return false_v;
  if (record_id == VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT &&
      (!vkr_metal_packet_validate_nested_gpu_abi(
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
  if (record_id == VKR_METAL_PACKET_ABI_GBUFFER_RESOLVE_ROOT &&
      (!vkr_metal_packet_validate_nested_gpu_abi(
           pipeline_label, root.bufferStructType, "visible_rows",
           VKR_GPU_ABI_VISIBLE_DRAW_ROW) ||
       !vkr_metal_packet_validate_nested_gpu_abi(
           pipeline_label, root.bufferStructType, "geometry_rows",
           VKR_GPU_ABI_GEOMETRY_ROW) ||
       !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                             root.bufferStructType, "instances",
                                             VKR_METAL_PACKET_ABI_INSTANCE) ||
       !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                             root.bufferStructType, "materials",
                                             VKR_METAL_PACKET_ABI_MATERIAL)))
    return false_v;
  return true_v;
}

vkr_internal id<MTLRenderPipelineState> vkr_metal_packet_create_pipeline(
    VkrMetalPacketRenderer *renderer, id<MTLLibrary> vertex_library,
    const char *vertex_name, id<MTLLibrary> fragment_library,
    const char *fragment_name, MTLPixelFormat color_format, bool8_t blend,
    VkrMetalPacketAbiRecordId vertex_abi,
    VkrMetalPacketAbiRecordId fragment_abi, bool8_t supports_icb,
    NSString *label) API_AVAILABLE(macos(26.0)) {
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
  descriptor.rasterSampleCount = 1;
  if (color_format != MTLPixelFormatInvalid) {
    descriptor.colorAttachments[0].pixelFormat = color_format;
    if (blend) {
      descriptor.colorAttachments[0].blendingState = MTL4BlendStateEnabled;
      descriptor.colorAttachments[0].sourceRGBBlendFactor =
          MTLBlendFactorSourceAlpha;
      descriptor.colorAttachments[0].destinationRGBBlendFactor =
          MTLBlendFactorOneMinusSourceAlpha;
      descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
      descriptor.colorAttachments[0].destinationAlphaBlendFactor =
          MTLBlendFactorOneMinusSourceAlpha;
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

vkr_internal bool8_t vkr_metal_packet_create_pipelines(
    VkrMetalPacketRenderer *renderer, const char *slang_msl_path,
    const char *fragment_msl_path) API_AVAILABLE(macos(26.0)) {
  if (!vkr_metal_packet_abi_validate_host()) {
    log_error("Metal packet host ABI does not match its durable manifest");
    return false_v;
  }
  id<MTLLibrary> slang = vkr_metal_packet_create_library(
      renderer, slang_msl_path, @"VKR Metal packet Slang shaders");
  id<MTLLibrary> fragment = vkr_metal_packet_create_library(
      renderer, fragment_msl_path, @"VKR Metal packet resource shaders");
  if (!slang || !fragment) {
    [fragment release];
    [slang release];
    return false_v;
  }
  renderer->gpu_shadow_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", fragment,
      "vkr_metal_packet_gpu_shadow_fragment", MTLPixelFormatInvalid, false_v,
      VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT, VKR_METAL_PACKET_ABI_DRAW_ROOT,
      true_v, @"VKR Metal packet GPU shadow");
  // Opaque cascade geometry never alpha-tests, so it needs no fragment stage.
  renderer->gpu_shadow_opaque_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", nil, NULL,
      MTLPixelFormatInvalid, false_v, VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
      VKR_METAL_PACKET_ABI_DRAW_ROOT, true_v,
      @"VKR Metal packet GPU shadow opaque");
  renderer->vbuffer_opaque_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", fragment,
      "vkr_metal_packet_vbuffer_opaque_fragment", MTLPixelFormatRG32Uint,
      false_v, VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
      // The opaque variant binds no fragment root buffer, so there is no
      // fragment-side ABI record to validate.
      VKR_METAL_PACKET_ABI_RECORD_COUNT, true_v,
      @"VKR Metal packet vbuffer opaque");
  renderer->vbuffer_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", fragment,
      "vkr_metal_packet_vbuffer_fragment", MTLPixelFormatRG32Uint, false_v,
      VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT, VKR_METAL_PACKET_ABI_DRAW_ROOT,
      true_v, @"VKR Metal packet opaque visibility buffer");
  renderer->transmission_vbuffer_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", fragment,
      "vkr_metal_packet_transmission_vbuffer_fragment", MTLPixelFormatRG32Uint,
      false_v, VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
      VKR_METAL_PACKET_ABI_DRAW_ROOT, true_v,
      @"VKR Metal packet transmission peel visibility buffer");
  if (renderer->transmission_vbuffer_pipeline &&
      !vkr_metal_packet_validate_abi_binding(
          @"VKR Metal packet transmission peel visibility buffer",
          renderer->transmission_vbuffer_pipeline.reflection.fragmentBindings,
          VKR_METAL_PACKET_ABI_TRANSMISSION_PEEL_ROOT)) {
    [renderer->transmission_vbuffer_pipeline release];
    renderer->transmission_vbuffer_pipeline = nil;
  }
  renderer->blend_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", fragment,
      "vkr_metal_packet_opaque_fragment", MTLPixelFormatRGBA16Float, true_v,
      VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT, VKR_METAL_PACKET_ABI_DRAW_ROOT,
      false_v, @"VKR Metal packet alpha blend");
  renderer->overlay_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", fragment,
      "vkr_metal_packet_opaque_fragment",
      vkr_metal_packet_target_pixel_format(renderer), true_v,
      VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT, VKR_METAL_PACKET_ABI_DRAW_ROOT,
      false_v, @"VKR Metal packet UI/editor overlay");
  renderer->picking_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", slang,
      "vkr_metal_packet_picking_fragment", MTLPixelFormatR32Uint, false_v,
      VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT, VKR_METAL_PACKET_ABI_RECORD_COUNT,
      false_v, @"VKR Metal packet feature picking");
  renderer->tonemap_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_tonemap_vertex", fragment,
      "vkr_metal_packet_tonemap_fragment",
      vkr_metal_packet_target_pixel_format(renderer), false_v,
      VKR_METAL_PACKET_ABI_RECORD_COUNT, VKR_METAL_PACKET_ABI_TONEMAP_ROOT,
      false_v, @"VKR Metal packet tonemap");
  renderer->world_text_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_text_vertex", fragment,
      "vkr_metal_packet_text_fragment", MTLPixelFormatRGBA16Float, true_v,
      VKR_METAL_PACKET_ABI_TEXT_ROOT, VKR_METAL_PACKET_ABI_TEXT_ROOT, false_v,
      @"VKR Metal packet world text");
  renderer->ui_text_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_text_vertex", fragment,
      "vkr_metal_packet_text_fragment",
      vkr_metal_packet_target_pixel_format(renderer), true_v,
      VKR_METAL_PACKET_ABI_TEXT_ROOT, VKR_METAL_PACKET_ABI_TEXT_ROOT, false_v,
      @"VKR Metal packet UI text");
  renderer->picking_text_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_text_vertex", fragment,
      "vkr_metal_packet_text_picking_fragment", MTLPixelFormatR32Uint, false_v,
      VKR_METAL_PACKET_ABI_TEXT_ROOT, VKR_METAL_PACKET_ABI_TEXT_ROOT, false_v,
      @"VKR Metal packet text picking");
  renderer->ibl_equirect_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_ibl_equirect",
      VKR_METAL_PACKET_ABI_EQUIRECT_ROOT, false_v,
      @"VKR Metal packet IBL equirect to cube");
  renderer->ibl_irradiance_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_ibl_irradiance",
      VKR_METAL_PACKET_ABI_IRRADIANCE_ROOT, false_v,
      @"VKR Metal packet IBL irradiance");
  renderer->ibl_prefilter_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_ibl_prefilter",
      VKR_METAL_PACKET_ABI_PREFILTER_ROOT, false_v,
      @"VKR Metal packet IBL prefilter");
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
  renderer->gbuffer_resolve_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_gbuffer_resolve",
      VKR_METAL_PACKET_ABI_GBUFFER_RESOLVE_ROOT, false_v,
      @"VKR Metal packet G-buffer resolve");
  renderer->deferred_lighting_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_deferred_lighting",
          VKR_METAL_PACKET_ABI_DEFERRED_LIGHTING_ROOT, false_v,
          @"VKR Metal packet deferred lighting");
  renderer->transmission_shade_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_transmission_shade",
          VKR_METAL_PACKET_ABI_TRANSMISSION_SHADE_ROOT, false_v,
          @"VKR Metal packet transmission shade");
  renderer->transmission_coverage_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_transmission_coverage",
          VKR_METAL_PACKET_ABI_TRANSMISSION_COVERAGE_ROOT, false_v,
          @"VKR Metal packet transmission coverage");
  renderer->transmission_compact_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_transmission_compact",
          VKR_METAL_PACKET_ABI_TRANSMISSION_COMPACT_ROOT, false_v,
          @"VKR Metal packet transmission compact");
  renderer->transmission_compact_finalize_pipeline =
      vkr_metal_packet_create_compute_pipeline(
          renderer, fragment, "vkr_metal_packet_transmission_compact_finalize",
          VKR_METAL_PACKET_ABI_TRANSMISSION_COMPACT_ROOT, false_v,
          @"VKR Metal packet transmission compact finalize");
  renderer->picking_resolve_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_picking_resolve",
      VKR_METAL_PACKET_ABI_PICKING_RESOLVE_ROOT, false_v,
      @"VKR Metal packet deferred picking resolve");
  renderer->hzb_build_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_hzb_build",
      VKR_METAL_PACKET_ABI_HZB_BUILD_ROOT, false_v,
      @"VKR Metal packet HZB build");
  id<MTLFunction> encode_function =
      [fragment newFunctionWithName:@"vkr_metal_packet_gpu_draw_encode"];
  renderer->gpu_draw_icb_argument_encoder =
      [encode_function newArgumentEncoderWithBufferIndex:1u];
  [encode_function release];
  if (!renderer->gpu_draw_classify_pipeline ||
      !renderer->gpu_draw_prefix_pipeline ||
      !renderer->gpu_draw_encode_pipeline ||
      !renderer->gbuffer_resolve_pipeline ||
      !renderer->deferred_lighting_pipeline ||
      !renderer->transmission_shade_pipeline ||
      !renderer->transmission_coverage_pipeline ||
      !renderer->transmission_compact_pipeline ||
      !renderer->transmission_compact_finalize_pipeline ||
      !renderer->picking_resolve_pipeline || !renderer->hzb_build_pipeline ||
      !renderer->gpu_shadow_pipeline || !renderer->gpu_shadow_opaque_pipeline ||
      !renderer->vbuffer_pipeline || !renderer->vbuffer_opaque_pipeline ||
      !renderer->transmission_vbuffer_pipeline ||
      !renderer->gpu_draw_icb_argument_encoder) {
    log_error("Metal deferred pipelines or indirect argument encoder are "
              "unavailable");
    [fragment release];
    [slang release];
    return false_v;
  }
  [fragment release];
  [slang release];
  MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
  depth.depthCompareFunction = MTLCompareFunctionLess;
  depth.depthWriteEnabled = YES;
  renderer->depth_write_state =
      [renderer->device newDepthStencilStateWithDescriptor:depth];
  depth.depthCompareFunction = MTLCompareFunctionLessEqual;
  depth.depthWriteEnabled = NO;
  renderer->depth_read_state =
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
  return renderer->blend_pipeline && renderer->overlay_pipeline &&
                 renderer->picking_pipeline && renderer->tonemap_pipeline &&
                 renderer->world_text_pipeline && renderer->ui_text_pipeline &&
                 renderer->picking_text_pipeline &&
                 renderer->ibl_equirect_pipeline &&
                 renderer->ibl_irradiance_pipeline &&
                 renderer->ibl_prefilter_pipeline &&
                 renderer->gpu_shadow_pipeline &&
                 renderer->gpu_shadow_opaque_pipeline &&
                 renderer->vbuffer_pipeline &&
                 renderer->vbuffer_opaque_pipeline &&
                 renderer->transmission_vbuffer_pipeline &&
                 renderer->gpu_draw_classify_pipeline &&
                 renderer->gpu_draw_prefix_pipeline &&
                 renderer->gpu_draw_encode_pipeline &&
                 renderer->gbuffer_resolve_pipeline &&
                 renderer->deferred_lighting_pipeline &&
                 renderer->transmission_shade_pipeline &&
                 renderer->transmission_coverage_pipeline &&
                 renderer->transmission_compact_pipeline &&
                 renderer->transmission_compact_finalize_pipeline &&
                 renderer->picking_resolve_pipeline &&
                 renderer->hzb_build_pipeline && renderer->depth_write_state &&
                 renderer->depth_read_state && renderer->argument_table
             ? true_v
             : false_v;
}

vkr_internal bool8_t vkr_metal_packet_create_gpu_draw_icbs(
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
  MTLIndirectCommandBufferDescriptor *descriptor =
      [MTLIndirectCommandBufferDescriptor new];
  descriptor.commandTypes = MTLIndirectCommandTypeDrawIndexed;
  descriptor.inheritPipelineState = YES;
  descriptor.inheritBuffers = NO;
  descriptor.inheritDepthStencilState = YES;
  descriptor.inheritCullMode = YES;
  descriptor.maxVertexBufferBindCount = 1u;
  descriptor.maxFragmentBufferBindCount = 3u;
  MTLResidencySetDescriptor *residency_descriptor =
      [MTLResidencySetDescriptor new];
  residency_descriptor.label = @"VKR Metal GPU draw indirect commands";
  bool8_t passed = true_v;
  for (uint32_t i = 0u; passed && i < renderer->command_slot_count; ++i) {
    VkrMetalPacketCommandSlot *slot = &renderer->command_slots[i];
    for (uint32_t group = 0u; group < renderer->gpu_draw_icb_group_count;
         ++group) {
      residency_descriptor.initialCapacity = group == 0u ? 2u : 1u;
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
      const uint32_t first_view =
          group * renderer->gpu_draw_icb_view_group_size;
      const uint32_t group_view_count =
          MIN(renderer->gpu_draw_icb_view_group_size,
              VKR_METAL_PACKET_GPU_DRAW_VIEW_COUNT_MAX - first_view);
      id<MTLIndirectCommandBuffer> icb = [renderer->device
          newIndirectCommandBufferWithDescriptor:descriptor
                                 maxCommandCount:
                                     VKR_GPU_DRAW_CANDIDATE_CAPACITY *
                                     group_view_count
                                         options:MTLResourceStorageModePrivate];
      slot->gpu_draw_icbs[group] = icb;
      if (!icb) {
        passed = false_v;
        break;
      }
      [residency addAllocation:icb];
      if (group == 0u) {
        id<MTLIndirectCommandBuffer> transmission_icb = [renderer->device
            newIndirectCommandBufferWithDescriptor:descriptor
                                   maxCommandCount:
                                       VKR_GPU_DRAW_CANDIDATE_CAPACITY
                                           options:
                                               MTLResourceStorageModePrivate];
        slot->transmission_gpu_draw_icb = transmission_icb;
        if (!transmission_icb) {
          passed = false_v;
          break;
        }
        [residency addAllocation:transmission_icb];
      }
      [residency commit];
      [residency requestResidency];
      slot->gpu_draw_icb_residency_count++;
    }
  }
  [descriptor release];
  [residency_descriptor release];
  if (!passed) {
    log_error("Metal deferred indirect command buffer capacity is "
              "unavailable");
    for (uint32_t i = 0u; i < renderer->command_slot_count; ++i) {
      VkrMetalPacketCommandSlot *slot = &renderer->command_slots[i];
      for (uint32_t group = 0u;
           group < VKR_METAL_PACKET_GPU_DRAW_ICB_GROUP_COUNT_MAX; ++group) {
        if (group < slot->gpu_draw_icb_residency_count)
          [slot->gpu_draw_icb_residencies[group] endResidency];
        [slot->gpu_draw_icbs[group] release];
        slot->gpu_draw_icbs[group] = nil;
        [slot->gpu_draw_icb_residencies[group] release];
        slot->gpu_draw_icb_residencies[group] = nil;
      }
      [slot->transmission_gpu_draw_icb release];
      slot->transmission_gpu_draw_icb = nil;
      slot->gpu_draw_icb_residency_count = 0u;
    }
  }
  return passed;
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

vkr_internal bool8_t vkr_metal_packet_create_ibl_targets(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  MTLTextureDescriptor *cube = [MTLTextureDescriptor new];
  cube.textureType = MTLTextureTypeCube;
  cube.pixelFormat = MTLPixelFormatRGBA16Float;
  cube.width = VKR_IBL_IRRADIANCE_SIZE;
  cube.height = VKR_IBL_IRRADIANCE_SIZE;
  cube.mipmapLevelCount = 1;
  cube.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  const bool8_t passed =
      vkr_metal_memory_device_create_texture(renderer->memory, cube,
                                             &renderer->ibl_irradiance) ==
      VKR_METAL_MEMORY_STATUS_OK;
  cube.width = VKR_IBL_PREFILTER_SIZE;
  cube.height = VKR_IBL_PREFILTER_SIZE;
  cube.mipmapLevelCount = VKR_IBL_PREFILTER_MIP_COUNT;
  const bool8_t targets_passed =
      passed && vkr_metal_memory_device_create_texture(
                    renderer->memory, cube, &renderer->ibl_prefilter) ==
                    VKR_METAL_MEMORY_STATUS_OK;
  [cube release];
  if (!targets_passed) {
    if (renderer->ibl_irradiance.metal_texture)
      (void)vkr_metal_memory_device_retire(renderer->memory,
                                           renderer->ibl_irradiance.handle, 0);
    if (renderer->ibl_prefilter.metal_texture)
      (void)vkr_metal_memory_device_retire(renderer->memory,
                                           renderer->ibl_prefilter.handle, 0);
    (void)vkr_metal_memory_device_collect(renderer->memory, 0, NULL);
    renderer->ibl_irradiance = (VkrMetalTextureResource){0};
    renderer->ibl_prefilter = (VkrMetalTextureResource){0};
    return false_v;
  }
  renderer->ibl_live = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_upload_texture(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketTextureUpload *payload, VkrTextureFormat format,
    id<MTLTexture> destination) API_AVAILABLE(macos(26.0)) {
  VkrTextureFormatInfo info = {0};
  if (!renderer || !payload || !payload->data || payload->data_size == 0 ||
      !payload->regions || payload->region_count == 0 || !destination ||
      !vkr_texture_format_get_info(format, &info) || info.is_depth_stencil ||
      payload->is_compressed != info.is_block_compressed ||
      payload->mip_levels != destination.mipmapLevelCount ||
      payload->array_layers != destination.arrayLength)
    return false_v;
  for (uint32_t i = 0; i < payload->region_count; ++i) {
    const VkrTextureUploadRegion *region = &payload->regions[i];
    const uint64_t blocks_x =
        ((uint64_t)region->width + info.block_width - 1u) / info.block_width;
    const uint64_t blocks_y =
        ((uint64_t)region->height + info.block_height - 1u) / info.block_height;
    const uint64_t bytes_per_row = blocks_x * info.bytes_per_block;
    const uint64_t required = bytes_per_row * blocks_y * region->depth;
    if (region->width == 0 || region->height == 0 || region->depth != 1 ||
        region->mip_level >= payload->mip_levels ||
        region->array_layer >= payload->array_layers ||
        region->byte_offset > payload->data_size ||
        required > payload->data_size - region->byte_offset ||
        required > region->byte_size)
      return false_v;
  }
  VkrMetalRingSlice slice = {0};
  VkrMetalAddressPair upload = {0};
  id<MTLBuffer> upload_buffer = nil;
  id<MTL4ComputeCommandEncoder> encoder = nil;
  if (!vkr_metal_packet_begin_upload(renderer, payload->data_size, &slice,
                                     &upload, &upload_buffer, &encoder))
    return false_v;
  MemCopy(upload.cpu_address, payload->data, payload->data_size);
  for (uint32_t i = 0; i < payload->region_count; ++i) {
    const VkrTextureUploadRegion *region = &payload->regions[i];
    const uint64_t blocks_x =
        ((uint64_t)region->width + info.block_width - 1u) / info.block_width;
    const uint64_t blocks_y =
        ((uint64_t)region->height + info.block_height - 1u) / info.block_height;
    const uint64_t bytes_per_row = blocks_x * info.bytes_per_block;
    [encoder copyFromBuffer:upload_buffer
               sourceOffset:slice.offset + region->byte_offset
          sourceBytesPerRow:bytes_per_row
        sourceBytesPerImage:bytes_per_row * blocks_y
                 sourceSize:MTLSizeMake(region->width, region->height, 1)
                  toTexture:destination
           destinationSlice:region->array_layer
           destinationLevel:region->mip_level
          destinationOrigin:MTLOriginMake(0, 0, 0)];
  }
  return vkr_metal_packet_submit_upload(renderer, encoder, slice);
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

vkr_internal bool8_t vkr_metal_packet_upload_buffer_chunks_at(
    VkrMetalPacketRenderer *renderer, const void *source, uint64_t size,
    id<MTLBuffer> destination, uint64_t destination_offset)
    API_AVAILABLE(macos(26.0)) {
  if (!renderer || !source || size == 0 || !destination ||
      renderer->upload_slot_size == 0)
    return false_v;

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

vkr_internal bool8_t vkr_metal_packet_upload_buffer_chunks(
    VkrMetalPacketRenderer *renderer, const void *source, uint64_t size,
    id<MTLBuffer> destination) API_AVAILABLE(macos(26.0)) {
  return vkr_metal_packet_upload_buffer_chunks_at(renderer, source, size,
                                                  destination, 0u);
}

vkr_internal bool8_t vkr_metal_packet_upload_cubemap(
    VkrMetalPacketRenderer *renderer, const uint8_t *rgba8_faces,
    uint32_t extent, id<MTLTexture> destination) API_AVAILABLE(macos(26.0)) {
  if (!renderer || !rgba8_faces || extent == 0 || !destination ||
      (uint64_t)extent > UINT64_MAX / ((uint64_t)extent * 24u))
    return false_v;
  const uint64_t bytes_per_row = (uint64_t)extent * 4u;
  const uint64_t bytes_per_face = bytes_per_row * extent;
  const uint64_t total = bytes_per_face * 6u;
  VkrMetalRingSlice slice = {0};
  VkrMetalAddressPair upload = {0};
  id<MTLBuffer> upload_buffer = nil;
  id<MTL4ComputeCommandEncoder> encoder = nil;
  if (!vkr_metal_packet_begin_upload(renderer, total, &slice, &upload,
                                     &upload_buffer, &encoder))
    return false_v;
  MemCopy(upload.cpu_address, rgba8_faces, total);
  for (uint32_t face = 0; face < 6; ++face) {
    [encoder copyFromBuffer:upload_buffer
               sourceOffset:slice.offset + bytes_per_face * face
          sourceBytesPerRow:bytes_per_row
        sourceBytesPerImage:bytes_per_face
                 sourceSize:MTLSizeMake(extent, extent, 1)
                  toTexture:destination
           destinationSlice:face
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
  }
  return vkr_metal_packet_submit_upload(renderer, encoder, slice);
}

vkr_internal uint32_t vkr_metal_packet_mip_count(uint32_t extent) {
  uint32_t count = 0;
  while (extent > 0) {
    count++;
    extent >>= 1u;
  }
  return count;
}

vkr_internal bool8_t vkr_metal_packet_convert_equirect(
    VkrMetalPacketRenderer *renderer, const uint16_t *rgba16, uint32_t width,
    uint32_t height, uint32_t cube_extent,
    const VkrMetalTextureResource *source,
    const VkrMetalTextureResource *destination) API_AVAILABLE(macos(26.0)) {
  if (!renderer || !rgba16 || width == 0 || height == 0 || cube_extent == 0 ||
      !source || !source->metal_texture || !destination ||
      !destination->metal_texture ||
      (uint64_t)width > UINT64_MAX / ((uint64_t)height * 8u))
    return false_v;
  const uint64_t image_size = (uint64_t)width * height * 8u;
  const uint64_t root_offset =
      vkr_metal_packet_align_up(image_size, VKR_METAL_PACKET_ROOT_ALIGNMENT);
  if (root_offset > UINT64_MAX - VKR_METAL_PACKET_ROOT_ALIGNMENT)
    return false_v;
  VkrMetalRingSlice slice = {0};
  VkrMetalAddressPair upload = {0};
  id<MTLBuffer> upload_buffer = nil;
  id<MTL4ComputeCommandEncoder> encoder = nil;
  if (!vkr_metal_packet_begin_upload(
          renderer, root_offset + VKR_METAL_PACKET_ROOT_ALIGNMENT, &slice,
          &upload, &upload_buffer, &encoder))
    return false_v;
  MemCopy(upload.cpu_address, rgba16, image_size);
  VkrMetalPacketEquirectRoot *root =
      (VkrMetalPacketEquirectRoot *)((uint8_t *)upload.cpu_address +
                                     root_offset);
  MemZero(root, VKR_METAL_PACKET_ROOT_ALIGNMENT);
  *root = (VkrMetalPacketEquirectRoot){
      .source_texture_id = source->resource_id,
      .target_texture_id = destination->resource_id,
      .target_size = cube_extent,
  };
  [encoder copyFromBuffer:upload_buffer
             sourceOffset:slice.offset
        sourceBytesPerRow:(uint64_t)width * 8u
      sourceBytesPerImage:image_size
               sourceSize:MTLSizeMake(width, height, 1)
                toTexture:(id<MTLTexture>)source->metal_texture
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
  [encoder barrierAfterEncoderStages:MTLStageBlit
                 beforeEncoderStages:MTLStageDispatch
                   visibilityOptions:MTL4VisibilityOptionDevice];
  [renderer->argument_table setAddress:upload.gpu_address + root_offset
                               atIndex:0];
  [encoder setComputePipelineState:renderer->ibl_equirect_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(cube_extent, cube_extent, 6)
      threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
  [encoder barrierAfterEncoderStages:MTLStageDispatch
                 beforeEncoderStages:MTLStageBlit
                   visibilityOptions:MTL4VisibilityOptionDevice];
  [encoder
      generateMipmapsForTexture:(id<MTLTexture>)destination->metal_texture];
  return vkr_metal_packet_submit_upload(renderer, encoder, slice);
}

bool8_t
vkr_metal_packet_renderer_create(const VkrMetalPacketRendererConfig *config,
                                 VkrMetalPacketRenderer **out_renderer) {
  if (!config || !out_renderer || !config->allocator || !config->graph_path ||
      !config->slang_msl_path || !config->fragment_msl_path ||
      config->heap_size == 0 || config->upload_ring_size == 0 ||
      config->readback_ring_size == 0 || config->frame_slot_count == 0 ||
      config->capture_ring_capacity == 0 ||
      config->capture_ring_capacity > VKR_CAPTURE_RING_CAPACITY_MAX ||
      config->capture_max_batch_bytes == 0 ||
      config->frame_slot_count > UINT32_MAX / 4u ||
      config->frame_slot_count > VKR_METAL_PACKET_GRAPH_INSTANCE_MAX ||
      config->upload_ring_size % ((uint64_t)config->frame_slot_count * 4u) !=
          0 ||
      config->readback_ring_size % ((uint64_t)config->frame_slot_count * 4u) !=
          0 ||
      config->max_images == 0 || config->max_passes == 0 ||
      config->max_passes > VKR_METAL_PACKET_MAX_PASS_TIMINGS ||
      config->max_material_rows == 0 || config->max_meshes == 0 ||
      config->max_submeshes_per_mesh == 0 ||
      (uint64_t)config->max_meshes * config->max_submeshes_per_mesh >
          SIZE_MAX / sizeof(VkrMetalPacketSubmeshCreateInfo) ||
      config->max_textures == 0 || config->max_draws == 0 ||
      config->max_instances == 0 ||
      (config->upload_ring_size / config->frame_slot_count) %
              VKR_METAL_PACKET_ROOT_ALIGNMENT !=
          0 ||
      config->target_kind > VKR_METAL_PACKET_TARGET_WINDOW ||
      (config->target_kind == VKR_METAL_PACKET_TARGET_WINDOW &&
       !config->metal_layer))
    return false_v;
  /* A replacement is created before the current generation retires. With
     three command slots, one still-in-flight instance from the preceding
     generation can also survive until that slot is reacquired. */
  const uint64_t graph_instances_per_resource =
      2u * config->frame_slot_count +
      (config->frame_slot_count > 2u ? config->frame_slot_count - 2u : 0u);
  const uint64_t graph_allocation_capacity =
      (uint64_t)config->max_images * graph_instances_per_resource * 2u;
  const uint64_t memory_record_capacity =
      graph_allocation_capacity + (uint64_t)config->max_meshes * 2u +
      (uint64_t)config->max_material_rows * 4u + config->max_textures + 3u;
  if (memory_record_capacity >= UINT32_MAX)
    return false_v;
  *out_renderer = NULL;

  if (@available(macOS 26.0, *)) {
    VkrMetalPacketRenderer *renderer =
        vkr_metal_packet_alloc_zeroed(config->allocator, sizeof(*renderer));
    if (!renderer)
      return false_v;
    renderer->allocator = config->allocator;
    renderer->graph_frame_arena = arena_create(MB(8), KB(256));
    renderer->graph_frame_allocator =
        (VkrAllocator){.ctx = renderer->graph_frame_arena};
    renderer->max_images = config->max_images;
    renderer->max_passes = config->max_passes;
    renderer->max_meshes = config->max_meshes;
    renderer->max_submeshes_per_mesh = config->max_submeshes_per_mesh;
    renderer->max_materials = config->max_material_rows;
    renderer->max_textures = config->max_textures;
    renderer->transmission_compact_enabled =
        config->transmission_compact_enabled;
    renderer->hzb_enabled = config->hzb_enabled;
    renderer->max_samplers = VKR_METAL_PACKET_SAMPLER_CACHE_CAPACITY;
    renderer->max_draws = config->max_draws;
    renderer->max_instances = config->max_instances;
    renderer->command_slot_count = config->frame_slot_count;
    renderer->upload_slot_size =
        config->upload_ring_size / config->frame_slot_count;
    renderer->target_kind = config->target_kind;
    renderer->srgb_output = config->srgb_output;
    renderer->convert_vulkan_clip_y = config->convert_vulkan_clip_y;
    renderer->synchronous_validation_readback =
        config->synchronous_validation_readback;
    const uint64_t capture_storage_size = vkr_capture_ring_storage_requirement(
        config->capture_ring_capacity, config->capture_max_batch_bytes);
    if (capture_storage_size) {
      /* Own reservation; see capture_storage_memory in the renderer struct. */
      if (capture_storage_size > UINT64_MAX - KB(64)) {
        vkr_metal_packet_renderer_destroy(renderer);
        return false_v;
      }
      const uint64_t capture_allocator_size = capture_storage_size + KB(64);
      if (!vkr_dmemory_create(capture_allocator_size, capture_allocator_size,
                              &renderer->capture_storage_memory)) {
        vkr_metal_packet_renderer_destroy(renderer);
        return false_v;
      }
      renderer->capture_storage_allocator =
          (VkrAllocator){.ctx = &renderer->capture_storage_memory};
      vkr_dmemory_allocator_create(&renderer->capture_storage_allocator);
      renderer->capture_storage = vkr_allocator_alloc(
          &renderer->capture_storage_allocator, capture_storage_size,
          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      if (!renderer->capture_storage) {
        vkr_metal_packet_renderer_destroy(renderer);
        return false_v;
      }
    }
    /* One reservation for every record array. Each vkr_dmemory_alloc rounds up
       internally, so the total carries slack per array plus a fixed margin. */
    const uint64_t record_bytes[] = {
        vkr_metal_packet_images_bytes(renderer),
        vkr_metal_packet_graph_buffers_bytes(renderer),
        vkr_metal_packet_graph_buffer_instances_bytes(renderer),
        vkr_metal_packet_meshes_bytes(renderer),
        vkr_metal_packet_submeshes_bytes(renderer),
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
      vkr_metal_packet_renderer_destroy(renderer);
      return false_v;
    }
    renderer->record_allocator =
        (VkrAllocator){.ctx = &renderer->record_memory};
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
    /* Submesh entries are read only below a live mesh's initialized count, so
       touching the entire 96 MiB worst-case array at startup is unnecessary. */
    renderer->submeshes =
        vkr_metal_packet_record_alloc(records, record_bytes[4], false_v);
    renderer->packet_materials =
        vkr_metal_packet_record_alloc(records, record_bytes[5], true_v);
    renderer->textures =
        vkr_metal_packet_record_alloc(records, record_bytes[6], true_v);
    renderer->samplers =
        vkr_metal_packet_record_alloc(records, record_bytes[7], false_v);
    renderer->text_uploads =
        vkr_metal_packet_record_alloc(records, record_bytes[8], false_v);
    renderer->render_passes =
        vkr_metal_packet_record_alloc(records, record_bytes[9], true_v);
    renderer->command_slots =
        vkr_metal_packet_record_alloc(records, record_bytes[10], true_v);
    renderer->completed_timing_results =
        vkr_metal_packet_record_alloc(records, record_bytes[11], true_v);
    renderer->commit_feedback_records =
        vkr_metal_packet_record_alloc(records, record_bytes[12], true_v);
    renderer->retired_image_views =
        vkr_metal_packet_record_alloc(records, record_bytes[13], true_v);
    renderer->pass_labels =
        vkr_metal_packet_record_alloc(records, record_bytes[14], true_v);
    if (!renderer->images || !renderer->graph_buffers ||
        !renderer->graph_buffer_instances || !renderer->meshes ||
        !renderer->submeshes || !renderer->packet_materials ||
        !renderer->textures || !renderer->samplers || !renderer->text_uploads ||
        !renderer->render_passes || !renderer->command_slots ||
        !renderer->completed_timing_results ||
        !renderer->commit_feedback_records || !renderer->retired_image_views ||
        !renderer->pass_labels) {
      vkr_metal_packet_renderer_destroy(renderer);
      return false_v;
    }
    renderer->device = [MTLCreateSystemDefaultDevice() retain];
    MTL4CommandQueueDescriptor *queue_descriptor =
        [MTL4CommandQueueDescriptor new];
    queue_descriptor.feedbackQueue = vkr_metal_packet_feedback_queue();
    renderer->queue =
        [renderer->device newMTL4CommandQueueWithDescriptor:queue_descriptor
                                                      error:nil];
    [queue_descriptor release];
    renderer->completion = [renderer->device newSharedEvent];
    bool8_t command_slots_created = renderer->command_slots != NULL;
    for (uint32_t i = 0;
         command_slots_created && i < renderer->command_slot_count; ++i) {
      VkrMetalPacketCommandSlot *slot = &renderer->command_slots[i];
      slot->allocator = [renderer->device newCommandAllocator];
      slot->buffer = [renderer->device newCommandBuffer];
      command_slots_created = slot->allocator && slot->buffer;
    }
    bool8_t commit_feedback_created = renderer->commit_feedback_records != NULL;
    for (uint32_t i = 0u; commit_feedback_created &&
                          i < VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY;
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
        record->gpu_start_time = feedback.GPUStartTime;
        record->gpu_end_time = feedback.GPUEndTime;
        record->has_error = feedback.error != nil ? true_v : false_v;
        dispatch_async(vkr_metal_packet_feedback_queue(),
                       record->ready_handler);
      });
      if (!record->handler || !record->ready_handler)
        commit_feedback_created = false_v;
    }
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
        renderer->queue && renderer->completion && commit_feedback_created;
    if (config->target_kind == VKR_METAL_PACKET_TARGET_WINDOW) {
      passed = passed && renderer->layer;
      renderer->actual_present_mode =
          config->requested_present_mode == VKR_PRESENT_MODE_IMMEDIATE
              ? VKR_PRESENT_MODE_IMMEDIATE
              : VKR_PRESENT_MODE_FIFO;
      renderer->layer.device = renderer->device;
      renderer->layer.pixelFormat =
          vkr_metal_packet_target_pixel_format(renderer);
      renderer->layer.framebufferOnly = NO;
      renderer->layer.opaque = YES;
      renderer->layer.displaySyncEnabled =
          renderer->actual_present_mode != VKR_PRESENT_MODE_IMMEDIATE;
    } else {
      renderer->actual_present_mode = VKR_PRESENT_MODE_DEFAULT;
    }
    for (uint32_t i = 0; passed && i < config->max_passes; ++i) {
      renderer->render_passes[i] = [MTL4RenderPassDescriptor new];
      passed = renderer->render_passes[i] != nil;
    }
    passed = passed &&
             vkr_metal_memory_device_create(
                 &(VkrMetalMemoryDeviceConfig){
                     .metal_device = renderer->device,
                     .allocator = renderer->allocator,
                     .heap_size = config->heap_size,
                     .upload_ring_size = config->upload_ring_size,
                     .readback_ring_size = config->readback_ring_size,
                     .ring_slot_count = config->frame_slot_count,
                     .max_allocations = (uint32_t)memory_record_capacity,
                     .max_retirements = (uint32_t)memory_record_capacity,
                     .max_free_ranges = (uint32_t)memory_record_capacity + 1u,
                 },
                 &renderer->memory) == VKR_METAL_MEMORY_STATUS_OK &&
             vkr_metal_material_table_device_create(
                 &(VkrMetalMaterialTableConfig){
                     .max_rows = config->max_material_rows,
                     .max_retirements = config->max_material_rows,
                 },
                 renderer->device, renderer->allocator,
                 &renderer->materials) == VKR_METAL_MATERIAL_STATUS_OK &&
             vkr_metal_packet_create_ibl_targets(renderer) &&
             vkr_metal_packet_create_pipelines(renderer, config->slang_msl_path,
                                               config->fragment_msl_path) &&
             vkr_rg_executor_registry_init(&renderer->executors,
                                           renderer->allocator) &&
             vkr_metal_packet_register_executors(renderer) &&
             vkr_rg_json_load_file(renderer->allocator, config->graph_path,
                                   &renderer->json_graph) &&
             vkr_rg_json_bind_executors(&renderer->json_graph,
                                        &renderer->executors) &&
             (renderer->graph = vkr_rg_create(renderer->allocator)) != NULL &&
             vkr_rg_set_frame_allocator(renderer->graph,
                                        &renderer->graph_frame_allocator) &&
             vkr_metal_packet_finalize_archive(renderer,
                                               config->pipeline_archive_path);
    if (passed)
      passed = vkr_metal_packet_create_gpu_draw_icbs(renderer);
    if (!passed) {
      vkr_metal_packet_renderer_destroy(renderer);
      return false_v;
    }
    log_info(
        "Metal placement heap capacity: %llu MiB (driver recommended "
        "working set: %llu MiB)",
        (unsigned long long)(config->heap_size / MB(1)),
        (unsigned long long)(renderer->device.recommendedMaxWorkingSetSize /
                             MB(1)));
    *out_renderer = renderer;
    return true_v;
  }
  return false_v;
}
