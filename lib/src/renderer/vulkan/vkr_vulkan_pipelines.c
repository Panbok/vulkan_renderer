#include "renderer/vulkan/vkr_vulkan_internal.h"

vkr_internal bool8_t
vkr_vk_create_packet_pipelines(VkrVulkanRenderer *renderer);
vkr_internal bool8_t vkr_vk_create_ibl_pipelines(VkrVulkanRenderer *renderer);
vkr_internal bool8_t
vkr_vk_create_deferred_pipelines(VkrVulkanRenderer *renderer);

bool8_t vkr_vk_pipeline_cache_initialize(VkrVulkanRenderer *renderer) {
  const char *path = getenv("VKR_PIPELINE_CACHE_PATH");
  const size_t path_length = path ? strlen(path) : 0u;
  if (path_length >= sizeof(renderer->pipeline_cache_path)) {
    log_error("Vulkan pipeline cache path exceeds %zu bytes",
              sizeof(renderer->pipeline_cache_path) - 1u);
    return false_v;
  }
  if (path_length) {
    MemCopy(renderer->pipeline_cache_path, path, path_length + 1u);
    log_info("Pipeline cache path: %s", renderer->pipeline_cache_path);
  }
  void *initial_data = NULL;
  size_t initial_size = 0u;
  if (path_length) {
    FILE *file = fopen(renderer->pipeline_cache_path, "rb");
    if (file) {
      if (fseek(file, 0, SEEK_END) == 0) {
        const long end = ftell(file);
        if (end > 0 && (uint64_t)end <= MB(64) &&
            fseek(file, 0, SEEK_SET) == 0) {
          initial_size = (size_t)end;
          initial_data = vkr_allocator_alloc(renderer->allocator, initial_size,
                                             VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
          if (!initial_data ||
              fread(initial_data, 1u, initial_size, file) != initial_size) {
            if (initial_data) {
              vkr_allocator_free(renderer->allocator, initial_data,
                                 initial_size,
                                 VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
            }
            initial_data = NULL;
            initial_size = 0u;
          }
        }
      }
      fclose(file);
    }
  }
  VkPipelineCacheCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
      .initialDataSize = initial_size,
      .pInitialData = initial_data,
  };
  VkResult result =
      vkCreatePipelineCache(vkr_vulkan_device_handle(renderer->device), &info,
                            NULL, &renderer->pipeline_cache);
  if (initial_data) {
    vkr_allocator_free(renderer->allocator, initial_data, initial_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (result != VK_SUCCESS && initial_size) {
    info.initialDataSize = 0u;
    info.pInitialData = NULL;
    result = vkCreatePipelineCache(vkr_vulkan_device_handle(renderer->device),
                                   &info, NULL, &renderer->pipeline_cache);
  }
  if (result != VK_SUCCESS)
    return false_v;
  if (initial_size)
    log_info("Loaded pipeline cache data: %zu bytes", initial_size);
  log_info("Initialized Vulkan pipeline cache with %s data",
           initial_size ? "persisted" : "empty");
  return true_v;
}

void vkr_vk_pipeline_cache_shutdown(VkrVulkanRenderer *renderer) {
  if (!renderer->pipeline_cache)
    return;
  VkDevice device = vkr_vulkan_device_handle(renderer->device);
  if (renderer->pipeline_cache_path[0]) {
    size_t size = 0u;
    if (vkGetPipelineCacheData(device, renderer->pipeline_cache, &size, NULL) ==
            VK_SUCCESS &&
        size > 0u) {
      const size_t allocation_size = size;
      void *data = vkr_allocator_alloc(renderer->allocator, allocation_size,
                                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      if (data && vkGetPipelineCacheData(device, renderer->pipeline_cache,
                                         &size, data) == VK_SUCCESS) {
        char temporary[sizeof(renderer->pipeline_cache_path) + 5u];
        const int written = snprintf(temporary, sizeof(temporary), "%s.tmp",
                                     renderer->pipeline_cache_path);
        FILE *file = written > 0 && (size_t)written < sizeof(temporary)
                         ? fopen(temporary, "wb")
                         : NULL;
        const bool8_t wrote = file && fwrite(data, 1u, size, file) == size;
        const bool8_t closed = file && fclose(file) == 0;
        if (wrote && closed) {
          const FilePath temporary_path = {
              .path = string8_create_from_cstr((const uint8_t *)temporary,
                                               string_length(temporary)),
              .type = FILE_PATH_TYPE_ABSOLUTE,
          };
          const FilePath cache_path = {
              .path = string8_create_from_cstr(
                  (const uint8_t *)renderer->pipeline_cache_path,
                  string_length(renderer->pipeline_cache_path)),
              .type = FILE_PATH_TYPE_ABSOLUTE,
          };
          if (file_rename(&temporary_path, &cache_path, true_v) ==
              FILE_ERROR_NONE) {
            log_info("Saved pipeline cache data: %zu bytes -> %s", size,
                     renderer->pipeline_cache_path);
          } else {
            (void)file_remove(&temporary_path);
          }
        }
      }
      if (data) {
        vkr_allocator_free(renderer->allocator, data, allocation_size,
                           VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      }
    }
  }
  vkDestroyPipelineCache(device, renderer->pipeline_cache, NULL);
  renderer->pipeline_cache = VK_NULL_HANDLE;
}

vkr_internal SpvReflectBlockVariable *
vkr_vk_reflect_member(SpvReflectBlockVariable *parent, const char *name) {
  if (!parent || !name)
    return NULL;
  for (uint32_t i = 0; i < parent->member_count; ++i) {
    SpvReflectBlockVariable *member = &parent->members[i];
    if (member->name && string_equals(member->name, name))
      return member;
  }
  return NULL;
}

vkr_internal bool8_t vkr_vk_reflect_member_offset(
    SpvReflectBlockVariable *parent, const char *name, uint32_t offset,
    SpvReflectBlockVariable **out_member) {
  SpvReflectBlockVariable *member = vkr_vk_reflect_member(parent, name);
  if (out_member)
    *out_member = member;
  if (!member || member->offset != offset) {
    log_error("Vulkan shader ABI member %s is %s (offset %u, "
              "expected %u)",
              name, member ? "misaligned" : "missing",
              member ? member->offset : UINT32_MAX, offset);
    return false_v;
  }
  return true_v;
}

vkr_internal uint32_t
vkr_vk_reflected_struct_size(const SpvReflectBlockVariable *value) {
  uint32_t size = 0u;
  if (!value)
    return size;
  for (uint32_t i = 0; i < value->member_count; ++i) {
    const SpvReflectBlockVariable *member = &value->members[i];
    const uint32_t member_size = Max(member->size, member->padded_size);
    if (member->offset <= UINT32_MAX - member_size)
      size = Max(size, member->offset + member_size);
  }
  return size;
}

vkr_internal bool8_t vkr_vk_validate_reflected_gpu_abi(
    SpvReflectBlockVariable *value, VkrGpuAbiRecordId id) {
  const VkrGpuAbiRecord *record = vkr_gpu_abi_record(id);
  if (!value || !record ||
      vkr_vk_reflected_struct_size(value) != record->expected_size)
    return false_v;
  bool8_t valid = true_v;
  for (uint32_t i = 0u; i < record->field_count; ++i) {
    const VkrGpuAbiField *field = &record->fields[i];
    valid &= vkr_vk_reflect_member_offset(value, field->shader_name,
                                          field->expected_offset, NULL);
  }
  return valid;
}

vkr_internal bool8_t
vkr_vk_validate_packet_root_abi(VkrVulkanRenderer *renderer) {
  FilePath shader_path =
      file_path_create(VKR_VULKAN_PACKET_WORLD_FRAG_SPV, renderer->allocator,
                       FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u)
    return false_v;
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult created =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (created != SPV_REFLECT_RESULT_SUCCESS)
    return false_v;
  uint32_t count = 0u;
  SpvReflectBlockVariable *blocks[1] = {0};
  bool8_t valid = spvReflectEnumerateEntryPointPushConstantBlocks(
                      &module, "world_fragment", &count, NULL) ==
                      SPV_REFLECT_RESULT_SUCCESS &&
                  count == 1u &&
                  spvReflectEnumerateEntryPointPushConstantBlocks(
                      &module, "world_fragment", &count, blocks) ==
                      SPV_REFLECT_RESULT_SUCCESS;
  valid &= blocks[0] && blocks[0]->size == sizeof(VkrVulkanPushConstants);
  SpvReflectBlockVariable *root =
      valid ? vkr_vk_reflect_member(blocks[0], "root") : NULL;
  if (!root || root->member_count == 0u) {
    valid = false_v;
  } else {
    SpvReflectBlockVariable *geometry_rows = NULL;
    SpvReflectBlockVariable *visible_rows = NULL;
    SpvReflectBlockVariable *vertices = NULL;
    SpvReflectBlockVariable *frame = NULL;
    SpvReflectBlockVariable *materials = NULL;
    valid &= vkr_vk_reflect_member_offset(
        root, "geometry_rows", offsetof(VkrVulkanPacketDrawRoot, geometry_rows),
        &geometry_rows);
    valid &= vkr_vk_reflect_member_offset(
        root, "visible_rows", offsetof(VkrVulkanPacketDrawRoot, visible_rows),
        &visible_rows);
    valid &= vkr_vk_reflect_member_offset(
        root, "vertices", offsetof(VkrVulkanPacketDrawRoot, vertices),
        &vertices);
    valid &= vkr_vk_reflect_member_offset(
        root, "frame", offsetof(VkrVulkanPacketDrawRoot, frame), &frame);
    valid &= vkr_vk_reflect_member_offset(
        root, "visible_row_index",
        offsetof(VkrVulkanPacketDrawRoot, visible_row_index), NULL);
    valid &= vkr_vk_reflect_member_offset(
        root, "flags", offsetof(VkrVulkanPacketDrawRoot, flags), NULL);
    valid &= vkr_vk_reflect_member_offset(
        root, "reserved", offsetof(VkrVulkanPacketDrawRoot, reserved), NULL);
    valid &=
        vkr_vk_reflected_struct_size(root) == sizeof(VkrVulkanPacketDrawRoot);

    valid &= vkr_vk_reflect_member_offset(
        frame, "materials", offsetof(VkrVulkanPacketFrameRoot, materials),
        &materials);
    valid &= vkr_vk_reflect_member_offset(
        frame, "instance_address_padding",
        offsetof(VkrVulkanPacketFrameRoot, instance_address_padding), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "view_projection",
        offsetof(VkrVulkanPacketFrameRoot, view_projection), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "transmission_texture",
        offsetof(VkrVulkanPacketFrameRoot, transmission_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "transmission_sampler",
        offsetof(VkrVulkanPacketFrameRoot, transmission_sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "flags", offsetof(VkrVulkanPacketFrameRoot, flags), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "shadow_debug_mode",
        offsetof(VkrVulkanPacketFrameRoot, shadow_debug_mode), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "point_light_grid_origin_cell_size",
        offsetof(VkrVulkanPacketFrameRoot, point_light_grid_origin_cell_size),
        NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "view", offsetof(VkrVulkanPacketFrameRoot, view), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "shadow_pcf_uniform_early_out",
        offsetof(VkrVulkanPacketFrameRoot, shadow_pcf_uniform_early_out), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "ibl_probes", offsetof(VkrVulkanPacketFrameRoot, ibl_probes),
        NULL);
    valid &= frame && vkr_vk_reflected_struct_size(frame) ==
                          sizeof(VkrVulkanPacketFrameRoot);

    const VkrGpuAbiRecord *vertex_abi =
        vkr_gpu_abi_record(VKR_GPU_ABI_PACKED_STATIC_VERTEX);
    valid &=
        vertices && materials && vertex_abi &&
        vkr_vk_reflected_struct_size(vertices) == vertex_abi->expected_size &&
        vkr_vk_reflected_struct_size(materials) ==
            sizeof(VkrVulkanMaterialGpuRow);
    valid &= vkr_vk_validate_reflected_gpu_abi(
        vertices, VKR_GPU_ABI_PACKED_STATIC_VERTEX);
    valid &= vkr_vk_validate_reflected_gpu_abi(geometry_rows,
                                               VKR_GPU_ABI_GEOMETRY_ROW);
    valid &= vkr_vk_validate_reflected_gpu_abi(visible_rows,
                                               VKR_GPU_ABI_VISIBLE_DRAW_ROW);
    valid &= vkr_vk_reflect_member_offset(
        materials, "base_color_texture",
        offsetof(VkrVulkanMaterialGpuRow, base_color_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "base_color_sampler",
        offsetof(VkrVulkanMaterialGpuRow, base_color_sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_id",
        offsetof(VkrVulkanMaterialGpuRow, material_id), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "alpha_mode", offsetof(VkrVulkanMaterialGpuRow, alpha_mode),
        NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_emissive",
        offsetof(VkrVulkanMaterialGpuRow, material_emissive), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_surface",
        offsetof(VkrVulkanMaterialGpuRow, material_surface), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_attenuation_color",
        offsetof(VkrVulkanMaterialGpuRow, material_attenuation_color), NULL);
  }
  spvReflectDestroyShaderModule(&module);
  return valid;
}

vkr_internal bool8_t vkr_vk_create_shader_module(VkrVulkanRenderer *renderer,
                                                 const char *path,
                                                 VkShaderModule *out_module) {
  FilePath shader_path =
      file_path_create(path, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0 || (size % sizeof(uint32_t)) != 0) {
    return false_v;
  }
  VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = (size_t)size,
      .pCode = (const uint32_t *)bytes,
  };
  const VkResult result = vkCreateShaderModule(vkr_vk_renderer_device(renderer),
                                               &module_info, NULL, out_module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  return result == VK_SUCCESS;
}

bool8_t vkr_vk_create_pipelines(VkrVulkanRenderer *renderer) {
  if (!vkr_vk_validate_packet_root_abi(renderer)) {
    return false_v;
  }
  const VkrVulkanDescriptorLayout *resource_layout =
      vkr_vulkan_device_resource_layout(renderer->device);
  const VkrVulkanDescriptorLayout *sampler_layout =
      vkr_vulkan_device_sampler_layout(renderer->device);
  VkDescriptorSetLayout layouts[] = {
      resource_layout->handle,
      sampler_layout->handle,
  };
  VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                    VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0u,
      .size = sizeof(VkrVulkanPushConstants),
  };
  VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = ArrayCount(layouts),
      .pSetLayouts = layouts,
      .pushConstantRangeCount = 1u,
      .pPushConstantRanges = &push_range,
  };
  VkDevice device = vkr_vk_renderer_device(renderer);
  if (vkCreatePipelineLayout(device, &layout_info, NULL,
                             &renderer->pipeline_layout) != VK_SUCCESS) {
    return false_v;
  }
  return vkr_vk_create_packet_pipelines(renderer) &&
         vkr_vk_create_ibl_pipelines(renderer) &&
         vkr_vk_create_deferred_pipelines(renderer);
}

vkr_internal bool8_t vkr_vk_create_packet_pipeline(
    VkrVulkanRenderer *renderer, VkrVulkanPacketPipeline pipeline,
    VkrVulkanPacketShader vertex_shader, VkrVulkanPacketShader fragment_shader,
    VkFormat color_format, VkFormat depth_format, bool8_t depth_test,
    bool8_t depth_write, bool8_t blend_enabled, bool8_t depth_bias) {
  // A fragment shader of VKR_VULKAN_PACKET_SHADER_COUNT builds a
  // depth-only pipeline with no fragment stage, which is what a shadow cascade
  // rendering opaque geometry should use.
  const bool8_t depth_only = fragment_shader == VKR_VULKAN_PACKET_SHADER_COUNT;
  const VkPipelineShaderStageCreateInfo stages[] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = renderer->packet_shaders[vertex_shader],
          .pName = vertex_shader == VKR_VULKAN_PACKET_SHADER_WORLD_VERTEX
                       ? "world_vertex"
                   : vertex_shader == VKR_VULKAN_PACKET_SHADER_TEXT_VERTEX
                       ? "text_vertex"
                   : vertex_shader == VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX
                       ? "vk_visibility_vertex"
                       : "fullscreen_vertex",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = depth_only ? VK_NULL_HANDLE
                               : renderer->packet_shaders[fragment_shader],
          .pName =
              depth_only ? ""
              : fragment_shader == VKR_VULKAN_PACKET_SHADER_WORLD_FRAGMENT
                  ? "world_fragment"
              : fragment_shader == VKR_VULKAN_PACKET_SHADER_PICKING_FRAGMENT
                  ? "picking_fragment"
              : fragment_shader == VKR_VULKAN_PACKET_SHADER_TEXT_FRAGMENT
                  ? "text_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_TEXT_PICKING_FRAGMENT
                  ? "text_picking_fragment"
              : fragment_shader == VKR_VULKAN_PACKET_SHADER_VISIBILITY_FRAGMENT
                  ? "vk_visibility_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_VISIBILITY_OPAQUE_FRAGMENT
                  ? "vk_visibility_opaque_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_VISIBILITY_SHADOW_FRAGMENT
                  ? "vk_visibility_shadow_fragment"
                  : "fullscreen_fragment",
      },
  };
  const VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkPipelineViewportStateCreateInfo viewport = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1u,
      .scissorCount = 1u,
  };
  const VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .depthBiasEnable = depth_bias,
      .lineWidth = 1.0f,
  };
  const VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = depth_test,
      .depthWriteEnable = depth_write,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
  };
  const VkPipelineColorBlendAttachmentState color_attachment = {
      .blendEnable = blend_enabled,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  const VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = color_format != VK_FORMAT_UNDEFINED ? 1u : 0u,
      .pAttachments =
          color_format != VK_FORMAT_UNDEFINED ? &color_attachment : NULL,
  };
  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_CULL_MODE,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
  };
  const VkPipelineDynamicStateCreateInfo dynamic = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = depth_bias ? ArrayCount(dynamic_states) : 3u,
      .pDynamicStates = dynamic_states,
  };
  const VkPipelineRenderingCreateInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = color_format != VK_FORMAT_UNDEFINED ? 1u : 0u,
      .pColorAttachmentFormats =
          color_format != VK_FORMAT_UNDEFINED ? &color_format : NULL,
      .depthAttachmentFormat = depth_format,
  };
  const VkGraphicsPipelineCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering,
      .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
      .stageCount = depth_only ? 1u : (uint32_t)ArrayCount(stages),
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pDepthStencilState =
          depth_format != VK_FORMAT_UNDEFINED ? &depth_stencil : NULL,
      .pColorBlendState = &blend,
      .pDynamicState = &dynamic,
      .layout = renderer->pipeline_layout,
  };
  return vkCreateGraphicsPipelines(
             vkr_vk_renderer_device(renderer), renderer->pipeline_cache, 1u,
             &create_info, NULL,
             &renderer->packet_pipelines[pipeline]) == VK_SUCCESS;
}

vkr_internal bool8_t
vkr_vk_create_packet_pipelines(VkrVulkanRenderer *renderer) {
  vkr_local_persist const char *const paths[VKR_VULKAN_PACKET_SHADER_COUNT] = {
      VKR_VULKAN_PACKET_WORLD_VERT_SPV,
      VKR_VULKAN_PACKET_WORLD_FRAG_SPV,
      VKR_VULKAN_PACKET_PICKING_FRAG_SPV,
      VKR_VULKAN_PACKET_FULLSCREEN_VERT_SPV,
      VKR_VULKAN_PACKET_FULLSCREEN_FRAG_SPV,
      VKR_VULKAN_PACKET_TEXT_VERT_SPV,
      VKR_VULKAN_PACKET_TEXT_FRAG_SPV,
      VKR_VULKAN_PACKET_TEXT_PICKING_FRAG_SPV,
      VKR_VULKAN_PACKET_VISIBILITY_VERT_SPV,
      VKR_VULKAN_PACKET_VISIBILITY_FRAG_SPV,
      VKR_VULKAN_PACKET_VISIBILITY_OPAQUE_FRAG_SPV,
      VKR_VULKAN_PACKET_VISIBILITY_SHADOW_FRAG_SPV,
  };
  for (uint32_t i = 0u; i < VKR_VULKAN_PACKET_SHADER_COUNT; ++i) {
    if (!vkr_vk_create_shader_module(renderer, paths[i],
                                     &renderer->packet_shaders[i]))
      return false_v;
  }
  return vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_PICKING,
             VKR_VULKAN_PACKET_SHADER_WORLD_VERTEX,
             VKR_VULKAN_PACKET_SHADER_PICKING_FRAGMENT, VK_FORMAT_R32_UINT,
             VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_WORLD_BLEND,
             VKR_VULKAN_PACKET_SHADER_WORLD_VERTEX,
             VKR_VULKAN_PACKET_SHADER_WORLD_FRAGMENT,
             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, true_v,
             false_v, true_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_FINAL,
             VKR_VULKAN_PACKET_SHADER_FULLSCREEN_VERTEX,
             VKR_VULKAN_PACKET_SHADER_FULLSCREEN_FRAGMENT,
             VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, false_v, false_v,
             false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_UI,
             VKR_VULKAN_PACKET_SHADER_WORLD_VERTEX,
             VKR_VULKAN_PACKET_SHADER_WORLD_FRAGMENT, VK_FORMAT_R8G8B8A8_UNORM,
             VK_FORMAT_UNDEFINED, false_v, false_v, true_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_WORLD_TEXT,
             VKR_VULKAN_PACKET_SHADER_TEXT_VERTEX,
             VKR_VULKAN_PACKET_SHADER_TEXT_FRAGMENT,
             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, true_v,
             false_v, true_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_PICKING_TEXT,
             VKR_VULKAN_PACKET_SHADER_TEXT_VERTEX,
             VKR_VULKAN_PACKET_SHADER_TEXT_PICKING_FRAGMENT, VK_FORMAT_R32_UINT,
             VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_UI_TEXT,
             VKR_VULKAN_PACKET_SHADER_TEXT_VERTEX,
             VKR_VULKAN_PACKET_SHADER_TEXT_FRAGMENT, VK_FORMAT_R8G8B8A8_UNORM,
             VK_FORMAT_UNDEFINED, false_v, false_v, true_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_VISIBILITY,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_FRAGMENT,
             VK_FORMAT_R32G32_UINT, VK_FORMAT_D32_SFLOAT, true_v, true_v,
             false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_OPAQUE,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_OPAQUE_FRAGMENT,
             VK_FORMAT_R32G32_UINT, VK_FORMAT_D32_SFLOAT, true_v, true_v,
             false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_SHADOW_FRAGMENT,
             VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v,
             true_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW_OPAQUE,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX,
             VKR_VULKAN_PACKET_SHADER_COUNT, VK_FORMAT_UNDEFINED,
             VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v, true_v);
}

vkr_internal bool8_t vkr_vk_create_ibl_pipelines(VkrVulkanRenderer *renderer) {
  vkr_local_persist const char *const paths[VKR_VULKAN_IBL_PIPELINE_COUNT] = {
      VKR_VULKAN_PACKET_IBL_EQUIRECT_COMP_SPV,
      VKR_VULKAN_PACKET_IBL_IRRADIANCE_COMP_SPV,
      VKR_VULKAN_PACKET_IBL_PREFILTER_COMP_SPV,
  };
  vkr_local_persist const char *const entries[VKR_VULKAN_IBL_PIPELINE_COUNT] = {
      "ibl_equirect",
      "ibl_irradiance",
      "ibl_prefilter",
  };
  for (uint32_t i = 0u; i < VKR_VULKAN_IBL_PIPELINE_COUNT; ++i) {
    if (!vkr_vk_create_shader_module(renderer, paths[i],
                                     &renderer->ibl_shaders[i]))
      return false_v;
    const VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = renderer->ibl_shaders[i],
        .pName = entries[i],
    };
    const VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stage = stage,
        .layout = renderer->pipeline_layout,
    };
    if (vkCreateComputePipelines(vkr_vk_renderer_device(renderer),
                                 renderer->pipeline_cache, 1u, &info, NULL,
                                 &renderer->ibl_pipelines[i]) != VK_SUCCESS)
      return false_v;
  }
  return true_v;
}

vkr_internal bool8_t
vkr_vk_create_deferred_pipelines(VkrVulkanRenderer *renderer) {
  vkr_local_persist const char
      *const paths[VKR_VULKAN_DEFERRED_PIPELINE_COUNT] = {
          VKR_VULKAN_PACKET_GPU_DRAW_CLASSIFY_COMP_SPV,
          VKR_VULKAN_PACKET_GPU_DRAW_PREFIX_COMP_SPV,
          VKR_VULKAN_PACKET_GPU_DRAW_ENCODE_COMP_SPV,
          VKR_VULKAN_PACKET_GBUFFER_RESOLVE_COMP_SPV,
          VKR_VULKAN_PACKET_DEFERRED_LIGHTING_COMP_SPV,
          VKR_VULKAN_PACKET_HZB_BUILD_COMP_SPV,
          VKR_VULKAN_PACKET_SDSM_REDUCE_COMP_SPV,
          VKR_VULKAN_PACKET_PICKING_RESOLVE_COMP_SPV,
          VKR_VULKAN_PACKET_TRANSMISSION_SHADE_COMP_SPV,
          VKR_VULKAN_PACKET_TRANSMISSION_COVERAGE_COMP_SPV,
      };
  vkr_local_persist const char
      *const entries[VKR_VULKAN_DEFERRED_PIPELINE_COUNT] = {
          "vk_gpu_draw_classify",  "vk_gpu_draw_prefix",
          "vk_gpu_draw_encode",    "vk_gbuffer_resolve",
          "vk_deferred_lighting",  "vk_hzb_build",
          "vk_sdsm_reduce",        "vk_picking_resolve",
          "vk_transmission_shade", "vk_transmission_coverage",
      };
  for (uint32_t i = 0u; i < VKR_VULKAN_DEFERRED_PIPELINE_COUNT; ++i) {
    if (!vkr_vk_create_shader_module(renderer, paths[i],
                                     &renderer->deferred_shaders[i]))
      return false_v;
    const VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = renderer->deferred_shaders[i],
        .pName = entries[i],
    };
    const VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stage = stage,
        .layout = renderer->pipeline_layout,
    };
    if (vkCreateComputePipelines(
            vkr_vk_renderer_device(renderer), renderer->pipeline_cache, 1u,
            &info, NULL, &renderer->deferred_pipelines[i]) != VK_SUCCESS)
      return false_v;
  }
  return true_v;
}
