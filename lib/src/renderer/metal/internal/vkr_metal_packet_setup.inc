/* Shader/pipeline creation, device resources, uploads, and renderer startup. */
static void vkr_metal_packet_print_error(const char *step, NSError *error) {
  (void)error;
  fprintf(stderr, "metal.packet.%s: Metal compilation failed\n", step);
}

static id<MTLLibrary>
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

static uint32_t
vkr_metal_packet_abi_binding_index(VkrMetalPacketAbiRecordId record_id) {
  switch (record_id) {
  case VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT:
  case VKR_METAL_PACKET_ABI_EQUIRECT_ROOT:
  case VKR_METAL_PACKET_ABI_IRRADIANCE_ROOT:
  case VKR_METAL_PACKET_ABI_PREFILTER_ROOT:
  case VKR_METAL_PACKET_ABI_BRDF_ROOT:
    return 0;
  default:
    return 1;
  }
}

static bool8_t vkr_metal_packet_validate_abi_type(
    NSString *pipeline_label, MTLStructType *shader_type,
    NSUInteger shader_size, NSUInteger shader_alignment,
    VkrMetalPacketAbiRecordId record_id) API_AVAILABLE(macos(26.0)) {
  const VkrMetalPacketAbiRecord *record =
      vkr_metal_packet_abi_record(record_id);
  if (!record || !shader_type || shader_size != record->expected_size ||
      shader_alignment != record->expected_alignment) {
    log_error(
        "Metal ABI mismatch for %s/%s: shader size/alignment %llu/%llu, "
        "host authority %u/%u",
        pipeline_label.UTF8String, record ? record->shader_name : "unknown",
        (unsigned long long)shader_size, (unsigned long long)shader_alignment,
        record ? record->expected_size : 0,
        record ? record->expected_alignment : 0);
    return false_v;
  }
  for (uint32_t i = 0; i < record->field_count; ++i) {
    const VkrMetalPacketAbiField *field = &record->fields[i];
    MTLStructMember *member = [shader_type
        memberByName:[NSString stringWithUTF8String:field->shader_name]];
    if (!member && record_id == VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT) {
      NSString *slang_name =
          [NSString stringWithFormat:@"%s_0", field->shader_name];
      member = [shader_type memberByName:slang_name];
    }
    if (!member || member.offset != field->expected_offset) {
      log_error("Metal ABI mismatch for %s/%s.%s: shader offset %lld, host "
                "authority %u",
                pipeline_label.UTF8String, record->shader_name,
                field->shader_name,
                member ? (long long)member.offset : (long long)-1,
                field->expected_offset);
      return false_v;
    }
  }
  return true_v;
}

static bool8_t vkr_metal_packet_validate_nested_abi(
    NSString *pipeline_label, MTLStructType *root_type, const char *member_name,
    VkrMetalPacketAbiRecordId record_id) API_AVAILABLE(macos(26.0)) {
  MTLStructMember *member =
      [root_type memberByName:[NSString stringWithUTF8String:member_name]];
  MTLPointerType *pointer = member.pointerType;
  return member && pointer &&
                 vkr_metal_packet_validate_abi_type(
                     pipeline_label, pointer.elementStructType,
                     pointer.dataSize, pointer.alignment, record_id)
             ? true_v
             : false_v;
}

static bool8_t vkr_metal_packet_validate_abi_binding(
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
  if (record_id == VKR_METAL_PACKET_ABI_DRAW_ROOT &&
      (!vkr_metal_packet_validate_nested_abi(pipeline_label,
                                             root.bufferStructType, "vertices",
                                             VKR_METAL_PACKET_ABI_VERTEX) ||
       !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                             root.bufferStructType, "instances",
                                             VKR_METAL_PACKET_ABI_INSTANCE) ||
       !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                             root.bufferStructType, "materials",
                                             VKR_METAL_PACKET_ABI_MATERIAL) ||
       !vkr_metal_packet_validate_nested_abi(
           pipeline_label, root.bufferStructType, "ibl_probes",
           VKR_METAL_PACKET_ABI_IBL_PROBE) ||
       !vkr_metal_packet_validate_nested_abi(
           pipeline_label, root.bufferStructType, "shadow_cascades",
           VKR_METAL_PACKET_ABI_SHADOW_CASCADE)))
    return false_v;
  if (record_id == VKR_METAL_PACKET_ABI_TEXT_ROOT &&
      !vkr_metal_packet_validate_nested_abi(pipeline_label,
                                            root.bufferStructType, "vertices",
                                            VKR_METAL_PACKET_ABI_TEXT_VERTEX))
    return false_v;
  return true_v;
}

static id<MTLRenderPipelineState> vkr_metal_packet_create_pipeline(
    VkrMetalPacketRenderer *renderer, id<MTLLibrary> vertex_library,
    const char *vertex_name, id<MTLLibrary> fragment_library,
    const char *fragment_name, MTLPixelFormat color_format, bool8_t blend,
    VkrMetalPacketAbiRecordId vertex_abi,
    VkrMetalPacketAbiRecordId fragment_abi, NSString *label)
    API_AVAILABLE(macos(26.0)) {
  MTL4LibraryFunctionDescriptor *vertex = [MTL4LibraryFunctionDescriptor new];
  vertex.library = vertex_library;
  vertex.name = [NSString stringWithUTF8String:vertex_name];
  MTL4LibraryFunctionDescriptor *fragment = [MTL4LibraryFunctionDescriptor new];
  fragment.library = fragment_library;
  fragment.name = [NSString stringWithUTF8String:fragment_name];
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
  if (pipeline &&
      (!vkr_metal_packet_validate_abi_binding(
           label, pipeline.reflection.vertexBindings, vertex_abi) ||
       !vkr_metal_packet_validate_abi_binding(
           label, pipeline.reflection.fragmentBindings, fragment_abi))) {
    [pipeline release];
    pipeline = nil;
  }
  [descriptor release];
  [fragment release];
  [vertex release];
  return pipeline;
}

static id<MTLComputePipelineState> vkr_metal_packet_create_compute_pipeline(
    VkrMetalPacketRenderer *renderer, id<MTLLibrary> library,
    const char *function_name, VkrMetalPacketAbiRecordId abi, NSString *label)
    API_AVAILABLE(macos(26.0)) {
  MTL4LibraryFunctionDescriptor *function = [MTL4LibraryFunctionDescriptor new];
  function.library = library;
  function.name = [NSString stringWithUTF8String:function_name];
  MTL4ComputePipelineDescriptor *descriptor =
      [MTL4ComputePipelineDescriptor new];
  descriptor.label = label;
  descriptor.computeFunctionDescriptor = function;
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

static bool8_t vkr_metal_packet_create_pipelines(
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
  renderer->shadow_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", slang,
      "vkr_metal_packet_shadow_fragment", MTLPixelFormatInvalid, false_v,
      VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT, VKR_METAL_PACKET_ABI_RECORD_COUNT,
      @"VKR Metal packet shadow");
  renderer->skybox_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_skybox_vertex", fragment,
      "vkr_metal_packet_skybox_fragment", MTLPixelFormatRGBA16Float, false_v,
      VKR_METAL_PACKET_ABI_RECORD_COUNT, VKR_METAL_PACKET_ABI_SKYBOX_ROOT,
      @"VKR Metal packet skybox");
  renderer->opaque_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", fragment,
      "vkr_metal_packet_opaque_fragment", MTLPixelFormatRGBA16Float, false_v,
      VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT, VKR_METAL_PACKET_ABI_DRAW_ROOT,
      @"VKR Metal packet opaque world");
  renderer->blend_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", fragment,
      "vkr_metal_packet_opaque_fragment", MTLPixelFormatRGBA16Float, true_v,
      VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT, VKR_METAL_PACKET_ABI_DRAW_ROOT,
      @"VKR Metal packet alpha blend");
  renderer->overlay_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", fragment,
      "vkr_metal_packet_opaque_fragment",
      vkr_metal_packet_target_pixel_format(renderer), true_v,
      VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT, VKR_METAL_PACKET_ABI_DRAW_ROOT,
      @"VKR Metal packet UI/editor overlay");
  renderer->picking_pipeline = vkr_metal_packet_create_pipeline(
      renderer, slang, "vkr_metal_packet_vertex", slang,
      "vkr_metal_packet_picking_fragment", MTLPixelFormatR32Uint, false_v,
      VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT, VKR_METAL_PACKET_ABI_RECORD_COUNT,
      @"VKR Metal packet picking");
  renderer->tonemap_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_tonemap_vertex", fragment,
      "vkr_metal_packet_tonemap_fragment",
      vkr_metal_packet_target_pixel_format(renderer), false_v,
      VKR_METAL_PACKET_ABI_RECORD_COUNT, VKR_METAL_PACKET_ABI_TONEMAP_ROOT,
      @"VKR Metal packet tonemap");
  renderer->world_text_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_text_vertex", fragment,
      "vkr_metal_packet_text_fragment", MTLPixelFormatRGBA16Float, true_v,
      VKR_METAL_PACKET_ABI_TEXT_ROOT, VKR_METAL_PACKET_ABI_TEXT_ROOT,
      @"VKR Metal packet world text");
  renderer->ui_text_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_text_vertex", fragment,
      "vkr_metal_packet_text_fragment",
      vkr_metal_packet_target_pixel_format(renderer), true_v,
      VKR_METAL_PACKET_ABI_TEXT_ROOT, VKR_METAL_PACKET_ABI_TEXT_ROOT,
      @"VKR Metal packet UI text");
  renderer->picking_text_pipeline = vkr_metal_packet_create_pipeline(
      renderer, fragment, "vkr_metal_packet_text_vertex", fragment,
      "vkr_metal_packet_text_picking_fragment", MTLPixelFormatR32Uint, false_v,
      VKR_METAL_PACKET_ABI_TEXT_ROOT, VKR_METAL_PACKET_ABI_TEXT_ROOT,
      @"VKR Metal packet picking text");
  renderer->ibl_equirect_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_ibl_equirect",
      VKR_METAL_PACKET_ABI_EQUIRECT_ROOT,
      @"VKR Metal packet IBL equirect to cube");
  renderer->ibl_irradiance_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_ibl_irradiance",
      VKR_METAL_PACKET_ABI_IRRADIANCE_ROOT, @"VKR Metal packet IBL irradiance");
  renderer->ibl_prefilter_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_ibl_prefilter",
      VKR_METAL_PACKET_ABI_PREFILTER_ROOT, @"VKR Metal packet IBL prefilter");
  renderer->ibl_brdf_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, fragment, "vkr_metal_packet_ibl_brdf",
      VKR_METAL_PACKET_ABI_BRDF_ROOT, @"VKR Metal packet IBL BRDF LUT");
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
  table.maxBufferBindCount = 2;
  table.initializeBindings = YES;
  NSError *error = nil;
  renderer->argument_table =
      [renderer->device newArgumentTableWithDescriptor:table error:&error];
  [table release];
  if (!renderer->argument_table)
    vkr_metal_packet_print_error("argument_table", error);
  return renderer->shadow_pipeline && renderer->skybox_pipeline &&
                 renderer->opaque_pipeline && renderer->blend_pipeline &&
                 renderer->overlay_pipeline && renderer->picking_pipeline &&
                 renderer->tonemap_pipeline && renderer->world_text_pipeline &&
                 renderer->ui_text_pipeline &&
                 renderer->picking_text_pipeline &&
                 renderer->ibl_equirect_pipeline &&
                 renderer->ibl_irradiance_pipeline &&
                 renderer->ibl_prefilter_pipeline &&
                 renderer->ibl_brdf_pipeline && renderer->depth_write_state &&
                 renderer->depth_read_state && renderer->argument_table
             ? true_v
             : false_v;
}

static bool8_t
vkr_metal_packet_create_compiler(VkrMetalPacketRenderer *renderer,
                                 const char *archive_path)
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

static bool8_t
vkr_metal_packet_finalize_archive(VkrMetalPacketRenderer *renderer,
                                  const char *archive_path)
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

static bool8_t
vkr_metal_packet_create_ibl_targets(VkrMetalPacketRenderer *renderer)
    API_AVAILABLE(macos(26.0)) {
  MTLTextureDescriptor *cube = [MTLTextureDescriptor new];
  cube.textureType = MTLTextureTypeCube;
  cube.pixelFormat = MTLPixelFormatRGBA16Float;
  cube.width = VKR_IBL_IRRADIANCE_SIZE;
  cube.height = VKR_IBL_IRRADIANCE_SIZE;
  cube.mipmapLevelCount = 1;
  cube.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  MTLTextureDescriptor *brdf = [MTLTextureDescriptor new];
  brdf.textureType = MTLTextureType2D;
  brdf.pixelFormat = MTLPixelFormatRGBA16Float;
  brdf.width = VKR_IBL_BRDF_SIZE;
  brdf.height = VKR_IBL_BRDF_SIZE;
  brdf.mipmapLevelCount = 1;
  brdf.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  const bool8_t passed =
      vkr_metal_memory_device_create_texture(renderer->memory, cube,
                                             &renderer->ibl_irradiance) ==
      VKR_METAL_MEMORY_STATUS_OK;
  cube.width = VKR_IBL_PREFILTER_SIZE;
  cube.height = VKR_IBL_PREFILTER_SIZE;
  cube.mipmapLevelCount = VKR_IBL_PREFILTER_MIP_COUNT;
  const bool8_t prefilter_passed =
      passed && vkr_metal_memory_device_create_texture(
                    renderer->memory, cube, &renderer->ibl_prefilter) ==
                    VKR_METAL_MEMORY_STATUS_OK;
  const bool8_t targets_passed =
      prefilter_passed && vkr_metal_memory_device_create_texture(
                              renderer->memory, brdf, &renderer->ibl_brdf) ==
                              VKR_METAL_MEMORY_STATUS_OK;
  [brdf release];
  [cube release];
  if (!targets_passed) {
    if (renderer->ibl_irradiance.metal_texture)
      (void)vkr_metal_memory_device_retire(renderer->memory,
                                           renderer->ibl_irradiance.handle, 0);
    if (renderer->ibl_prefilter.metal_texture)
      (void)vkr_metal_memory_device_retire(renderer->memory,
                                           renderer->ibl_prefilter.handle, 0);
    if (renderer->ibl_brdf.metal_texture)
      (void)vkr_metal_memory_device_retire(renderer->memory,
                                           renderer->ibl_brdf.handle, 0);
    (void)vkr_metal_memory_device_collect(renderer->memory, 0, NULL);
    renderer->ibl_irradiance = (VkrMetalTextureResource){0};
    renderer->ibl_prefilter = (VkrMetalTextureResource){0};
    renderer->ibl_brdf = (VkrMetalTextureResource){0};
    return false_v;
  }
  renderer->ibl_live = true_v;
  return true_v;
}

static bool8_t vkr_metal_packet_upload_texture(
    VkrMetalPacketRenderer *renderer, const VkrTextureUploadPayload *payload,
    VkrTextureFormat format, id<MTLTexture> destination)
    API_AVAILABLE(macos(26.0)) {
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

static bool8_t vkr_metal_packet_upload(
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

static bool8_t vkr_metal_packet_upload_buffer_chunks(
    VkrMetalPacketRenderer *renderer, const void *source, uint64_t size,
    id<MTLBuffer> destination) API_AVAILABLE(macos(26.0)) {
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
          destinationOffset:offset
                       size:chunk_size];
    if (!vkr_metal_packet_submit_upload(renderer, encoder, slice))
      return false_v;
    offset += chunk_size;
  }
  return true_v;
}

static bool8_t vkr_metal_packet_upload_cubemap(VkrMetalPacketRenderer *renderer,
                                               const uint8_t *rgba8_faces,
                                               uint32_t extent,
                                               id<MTLTexture> destination)
    API_AVAILABLE(macos(26.0)) {
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

static uint32_t vkr_metal_packet_mip_count(uint32_t extent) {
  uint32_t count = 0;
  while (extent > 0) {
    count++;
    extent >>= 1u;
  }
  return count;
}

static bool8_t vkr_metal_packet_convert_equirect(
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
  const uint64_t memory_record_capacity =
      (uint64_t)config->max_images * 2u + (uint64_t)config->max_meshes * 2u +
      (uint64_t)config->max_material_rows * 4u + config->max_textures + 3u;
  if (memory_record_capacity >= UINT32_MAX)
    return false_v;
  *out_renderer = NULL;

  if (@available(macOS 26.0, *)) {
    VkrMetalPacketRenderer *renderer = calloc(1, sizeof(*renderer));
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
    renderer->capture_storage = calloc(1, capture_storage_size);
    renderer->images = calloc(config->max_images, sizeof(*renderer->images));
    renderer->meshes = calloc(config->max_meshes, sizeof(*renderer->meshes));
    renderer->submeshes =
        calloc((uint64_t)config->max_meshes * config->max_submeshes_per_mesh,
               sizeof(*renderer->submeshes));
    renderer->packet_materials =
        calloc(config->max_material_rows, sizeof(*renderer->packet_materials));
    renderer->textures =
        calloc(config->max_textures, sizeof(*renderer->textures));
    renderer->samplers =
        calloc(renderer->max_samplers, sizeof(*renderer->samplers));
    renderer->text_uploads =
        calloc(config->max_draws, sizeof(*renderer->text_uploads));
    renderer->render_passes =
        calloc(config->max_passes, sizeof(*renderer->render_passes));
    renderer->command_slots =
        calloc(config->frame_slot_count, sizeof(*renderer->command_slots));
    renderer->completed_timing_results = calloc(
        config->frame_slot_count, sizeof(*renderer->completed_timing_results));
    renderer->device = [MTLCreateSystemDefaultDevice() retain];
    renderer->queue = [renderer->device newMTL4CommandQueue];
    renderer->completion = [renderer->device newSharedEvent];
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
    renderer->timestamp_frequency = [renderer->device queryTimestampFrequency];
    [timestamp_descriptor release];
    if (config->target_kind == VKR_METAL_PACKET_TARGET_WINDOW)
      renderer->layer = [(CAMetalLayer *)config->metal_layer retain];
    bool8_t passed =
        renderer->graph_frame_arena &&
        vkr_allocator_arena(&renderer->graph_frame_allocator) &&
        renderer->images && renderer->meshes && renderer->submeshes &&
        renderer->packet_materials && renderer->textures &&
        renderer->samplers && renderer->text_uploads &&
        renderer->render_passes && renderer->capture_storage &&
        renderer->completed_timing_results && command_slots_created &&
        vkr_capture_ring_init(
            &renderer->capture_ring, config->capture_ring_capacity,
            config->capture_max_batch_bytes, renderer->capture_storage,
            capture_storage_size) &&
        renderer->device &&
        [renderer->device supportsFamily:MTLGPUFamilyMetal4] &&
        vkr_metal_packet_create_compiler(renderer,
                                         config->pipeline_archive_path) &&
        renderer->queue && renderer->completion &&
        renderer->timestamp_frequency > 0;
    if (config->target_kind == VKR_METAL_PACKET_TARGET_WINDOW) {
      passed = passed && renderer->layer;
      renderer->layer.device = renderer->device;
      renderer->layer.pixelFormat =
          vkr_metal_packet_target_pixel_format(renderer);
      renderer->layer.framebufferOnly = NO;
      renderer->layer.opaque = YES;
    }
    for (uint32_t i = 0; passed && i < config->max_passes; ++i) {
      renderer->render_passes[i] = [MTL4RenderPassDescriptor new];
      passed = renderer->render_passes[i] != nil;
    }
    passed = passed &&
             vkr_metal_memory_device_create(
                 &(VkrMetalMemoryDeviceConfig){
                     .metal_device = renderer->device,
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
                 renderer->device,
                 &renderer->materials) == VKR_METAL_MATERIAL_STATUS_OK &&
             vkr_metal_packet_create_ibl_targets(renderer) &&
             vkr_metal_packet_create_pipelines(renderer, config->slang_msl_path,
                                               config->fragment_msl_path) &&
             vkr_rg_executor_registry_init(&renderer->executors,
                                           renderer->allocator) &&
             vkr_metal_packet_register_executors(renderer) &&
             vkr_rg_json_load_file(renderer->allocator, config->graph_path,
                                   &renderer->json_graph) &&
             (renderer->graph = vkr_rg_create(renderer->allocator)) != NULL &&
             vkr_rg_set_frame_allocator(renderer->graph,
                                        &renderer->graph_frame_allocator) &&
             vkr_metal_packet_finalize_archive(renderer,
                                               config->pipeline_archive_path);
    if (!passed) {
      vkr_metal_packet_renderer_destroy(renderer);
      return false_v;
    }
    *out_renderer = renderer;
    return true_v;
  }
  return false_v;
}
