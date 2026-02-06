#include "vulkan_pipeline.h"
#include "vulkan_shaders.h"
#include "platform/vkr_platform.h"

vkr_internal const VkrDescriptorSetDesc *
vulkan_pipeline_find_reflected_set(const VkrShaderReflection *reflection,
                                   uint32_t set_index) {
  if (!reflection || !reflection->sets) {
    return NULL;
  }

  for (uint32_t i = 0; i < reflection->set_count; ++i) {
    if (reflection->sets[i].set == set_index) {
      return &reflection->sets[i];
    }
  }
  return NULL;
}

vkr_internal void vulkan_pipeline_destroy_set_layouts(
    VulkanBackendState *state, VkDescriptorSetLayout *layouts,
    uint32_t layout_count) {
  if (!state || !layouts) {
    return;
  }
  for (uint32_t i = 0; i < layout_count; ++i) {
    if (layouts[i] != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(state->device.logical_device, layouts[i],
                                   state->allocator);
    }
  }
}

vkr_internal bool8_t vulkan_pipeline_build_vertex_input_from_reflection(
    VulkanBackendState *state, const VkrShaderReflection *reflection,
    Array_VkVertexInputBindingDescription *out_bindings,
    Array_VkVertexInputAttributeDescription *out_attributes) {
  if (!state || !reflection || !out_bindings || !out_attributes) {
    return false_v;
  }

  if (reflection->vertex_binding_count > 0) {
    *out_bindings = array_create_VkVertexInputBindingDescription(
        &state->temp_scope, reflection->vertex_binding_count);
    if (!out_bindings->data) {
      log_error("Failed to allocate reflected vertex binding descriptions");
      return false_v;
    }

    for (uint32_t i = 0; i < reflection->vertex_binding_count; ++i) {
      array_set_VkVertexInputBindingDescription(
          out_bindings, i,
          (VkVertexInputBindingDescription){
              .binding = reflection->vertex_bindings[i].binding,
              .stride = reflection->vertex_bindings[i].stride,
              .inputRate = reflection->vertex_bindings[i].rate,
          });
    }
  }

  if (reflection->vertex_attribute_count > 0) {
    *out_attributes = array_create_VkVertexInputAttributeDescription(
        &state->temp_scope, reflection->vertex_attribute_count);
    if (!out_attributes->data) {
      log_error("Failed to allocate reflected vertex attribute descriptions");
      return false_v;
    }

    for (uint32_t i = 0; i < reflection->vertex_attribute_count; ++i) {
      array_set_VkVertexInputAttributeDescription(
          out_attributes, i,
          (VkVertexInputAttributeDescription){
              .location = reflection->vertex_attributes[i].location,
              .binding = reflection->vertex_attributes[i].binding,
              .format = reflection->vertex_attributes[i].format,
              .offset = reflection->vertex_attributes[i].offset,
          });
    }
  }

  return true_v;
}

bool8_t vulkan_graphics_graphics_pipeline_create(
    VulkanBackendState *state, const VkrGraphicsPipelineDescription *desc,
    struct s_GraphicsPipeline *out_pipeline) {
  assert_log(state != NULL, "State is NULL");
  assert_log(desc != NULL, "Description is NULL");
  assert_log(out_pipeline != NULL, "Out pipeline is NULL");

  bool8_t success = false_v;
  bool8_t shader_object_created = false_v;
  bool8_t scope_valid = false_v;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorSetLayout *reflected_set_layouts = NULL;
  uint32_t reflected_set_layout_count = 0;
  Array_VkVertexInputBindingDescription bindings = {0};
  Array_VkVertexInputAttributeDescription attributes = {0};

  if (!vulkan_shader_object_create(state, &desc->shader_object_description,
                                   &out_pipeline->shader_object)) {
    log_fatal("Failed to create shader object");
    goto cleanup;
  }
  shader_object_created = true_v;

  // Bind the description so subsequent dereferences are valid
  out_pipeline->desc = *desc;

  VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_LINE_WIDTH,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
  };

  VkPipelineDynamicStateCreateInfo dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = ArrayCount(dynamic_states),
      .pDynamicStates = dynamic_states,
  };

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&state->temp_scope);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    log_error("Failed to create valid allocator scope");
    goto cleanup;
  }
  scope_valid = true_v;

  if (!out_pipeline->shader_object.has_reflection) {
    log_error("Shader object is missing reflection data");
    goto cleanup;
  }
  const VkrShaderReflection *reflection = &out_pipeline->shader_object.reflection;

  if (!vulkan_pipeline_build_vertex_input_from_reflection(
          state, reflection, &bindings, &attributes)) {
    goto cleanup;
  }

  VkPipelineVertexInputStateCreateInfo vertex_input_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = bindings.length,
      .pVertexBindingDescriptions = bindings.data,
      .vertexAttributeDescriptionCount = attributes.length,
      .pVertexAttributeDescriptions = attributes.data,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = vulkan_primitive_topology_to_vk(desc->topology),
      .primitiveRestartEnable = VK_FALSE,
  };

  float32_t viewport_width = (float32_t)state->swapchain.extent.width;
  float32_t viewport_height = (float32_t)state->swapchain.extent.height;

  VkViewport viewport = {
      .x = 0.0f,
      .y = 0.0f,
      .width = viewport_width,
      .height = viewport_height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };

  VkRect2D scissor = {
      .offset = {0, 0},
      .extent = {(uint32_t)viewport_width, (uint32_t)viewport_height},
  };

  VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &scissor,
  };

  VkPipelineRasterizationStateCreateInfo rasterization_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .depthClampEnable = VK_FALSE,
      .rasterizerDiscardEnable = VK_FALSE,
      .polygonMode = vulkan_polygon_mode_to_vk(desc->polygon_mode),
      .cullMode = vulkan_cull_mode_to_vk(desc->cull_mode),
      .frontFace = VK_FRONT_FACE_CLOCKWISE,
      .depthBiasEnable = VK_FALSE,
      .depthBiasConstantFactor = 0.0f,
      .depthBiasClamp = 0.0f,
      .depthBiasSlopeFactor = 0.0f,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
      .sampleShadingEnable = VK_FALSE,
      .minSampleShading = 1.0f,
      .pSampleMask = NULL,
      .alphaToCoverageEnable = VK_FALSE,
      .alphaToOneEnable = VK_FALSE,
  };

  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_LESS,
      .depthBoundsTestEnable = VK_FALSE,
      .stencilTestEnable = VK_FALSE,
  };

  VkPipelineColorBlendAttachmentState color_blend_attachment_state = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
      .blendEnable = VK_FALSE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
  };

  switch (desc->domain) {
  case VKR_PIPELINE_DOMAIN_WORLD:
    // Depth on, blending off (default)
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_TRUE;
    color_blend_attachment_state.blendEnable = VK_FALSE;
    break;
  case VKR_PIPELINE_DOMAIN_UI:
    // Depth off, alpha blending on
    depth_stencil_state.depthTestEnable = VK_FALSE;
    depth_stencil_state.depthWriteEnable = VK_FALSE;
    color_blend_attachment_state.blendEnable = VK_TRUE;
    break;
  case VKR_PIPELINE_DOMAIN_POST:
    // Screen-space: no depth; blending off by default
    depth_stencil_state.depthTestEnable = VK_FALSE;
    depth_stencil_state.depthWriteEnable = VK_FALSE;
    color_blend_attachment_state.blendEnable = VK_FALSE;
    break;
  case VKR_PIPELINE_DOMAIN_SHADOW:
    // Depth-only pipeline; no color attachments. Keep depth on; blending n/a
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_TRUE;
    // Shadow pass uses dynamic depth bias (vkCmdSetDepthBias) so presets can
    // tune bias without recreating pipelines.
    rasterization_state.depthBiasEnable = VK_TRUE;
    break;
  case VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT:
    // Transparent world objects: depth test on (respects opaque occlusion),
    // depth write off (transparent objects don't occlude each other),
    // alpha blending on
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_FALSE;
    color_blend_attachment_state.blendEnable = VK_TRUE;
    break;
  case VKR_PIPELINE_DOMAIN_WORLD_OVERLAY:
    // Overlay: no depth, alpha blending on.
    depth_stencil_state.depthTestEnable = VK_FALSE;
    depth_stencil_state.depthWriteEnable = VK_FALSE;
    color_blend_attachment_state.blendEnable = VK_TRUE;
    break;
  case VKR_PIPELINE_DOMAIN_SKYBOX:
    // Skybox renders first: depth test off (nothing to test against),
    // depth write off (world geometry should write to depth),
    // blending off
    depth_stencil_state.depthTestEnable = VK_FALSE;
    depth_stencil_state.depthWriteEnable = VK_FALSE;
    color_blend_attachment_state.blendEnable = VK_FALSE;
    break;
  case VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT:
    // Depth-tested picking that does not write depth.
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_FALSE;
    color_blend_attachment_state.blendEnable = VK_FALSE;
    break;
  case VKR_PIPELINE_DOMAIN_PICKING_OVERLAY:
    // Picking overlay: no depth, no blending (integer render target).
    depth_stencil_state.depthTestEnable = VK_FALSE;
    depth_stencil_state.depthWriteEnable = VK_FALSE;
    color_blend_attachment_state.blendEnable = VK_FALSE;
    break;
  default:
    break;
  }

  VkPipelineColorBlendStateCreateInfo color_blend_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .logicOpEnable = VK_FALSE,
      .logicOp = VK_LOGIC_OP_COPY,
      .attachmentCount = (desc->domain == VKR_PIPELINE_DOMAIN_SHADOW) ? 0 : 1,
      .pAttachments = (desc->domain == VKR_PIPELINE_DOMAIN_SHADOW)
                          ? NULL
                          : &color_blend_attachment_state,
  };

  VkPushConstantRange *push_constant_ranges = NULL;
  if (reflection->push_constant_range_count > 0) {
    push_constant_ranges = vkr_allocator_alloc(
        &state->temp_scope,
        sizeof(VkPushConstantRange) *
            (uint64_t)reflection->push_constant_range_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!push_constant_ranges) {
      log_error("Failed to allocate reflected push constant ranges");
      goto cleanup;
    }

    for (uint32_t i = 0; i < reflection->push_constant_range_count; ++i) {
      push_constant_ranges[i] = (VkPushConstantRange){
          .stageFlags = reflection->push_constant_ranges[i].stages,
          .offset = reflection->push_constant_ranges[i].offset,
          .size = reflection->push_constant_ranges[i].size,
      };
    }
  }

  reflected_set_layout_count = reflection->layout_set_count;
  if (reflected_set_layout_count > 0) {
    reflected_set_layouts = vkr_allocator_alloc(
        &state->temp_scope,
        sizeof(VkDescriptorSetLayout) * (uint64_t)reflected_set_layout_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!reflected_set_layouts) {
      log_error("Failed to allocate reflected descriptor set layouts");
      goto cleanup;
    }
    MemZero(reflected_set_layouts,
            sizeof(VkDescriptorSetLayout) * (uint64_t)reflected_set_layout_count);

    for (uint32_t set_index = 0; set_index < reflected_set_layout_count;
         ++set_index) {
      const VkrDescriptorSetDesc *set_desc =
          vulkan_pipeline_find_reflected_set(reflection, set_index);
      uint32_t binding_count = 0;
      VkDescriptorSetLayoutBinding *layout_bindings = NULL;
      if (set_desc) {
        binding_count = set_desc->binding_count;
        if (binding_count > 0) {
          layout_bindings = vkr_allocator_alloc(
              &state->temp_scope,
              sizeof(VkDescriptorSetLayoutBinding) * (uint64_t)binding_count,
              VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
          if (!layout_bindings) {
            log_error("Failed to allocate reflected set bindings for set %u",
                      set_index);
            goto cleanup;
          }
          for (uint32_t binding_index = 0; binding_index < binding_count;
               ++binding_index) {
            const VkrDescriptorBindingDesc *binding_desc =
                &set_desc->bindings[binding_index];
            layout_bindings[binding_index] = (VkDescriptorSetLayoutBinding){
                .binding = binding_desc->binding,
                .descriptorType = binding_desc->type,
                .descriptorCount = binding_desc->count,
                .stageFlags = binding_desc->stages,
                .pImmutableSamplers = NULL,
            };
          }
        }
      }

      const VkDescriptorSetLayoutCreateInfo set_layout_info = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = binding_count,
          .pBindings = layout_bindings,
      };
      if (vkCreateDescriptorSetLayout(state->device.logical_device,
                                      &set_layout_info, state->allocator,
                                      &reflected_set_layouts[set_index]) !=
          VK_SUCCESS) {
        log_error("Failed to create reflected descriptor set layout for set %u",
                  set_index);
        goto cleanup;
      }
    }
  }

  VkPipelineLayoutCreateInfo pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = reflected_set_layout_count,
      .pSetLayouts = reflected_set_layouts,
      .pushConstantRangeCount = reflection->push_constant_range_count,
      .pPushConstantRanges = push_constant_ranges,
  };

  if (vkCreatePipelineLayout(state->device.logical_device,
                             &pipeline_layout_info, state->allocator,
                             &pipeline_layout) != VK_SUCCESS) {
    log_fatal("Failed to create pipeline layout");
    goto cleanup;
  }
  out_pipeline->pipeline_layout = VK_NULL_HANDLE;

  VkPipelineShaderStageCreateInfo shader_stages[] = {
      out_pipeline->shader_object.stages[VKR_SHADER_STAGE_VERTEX],
      out_pipeline->shader_object.stages[VKR_SHADER_STAGE_FRAGMENT],
  };

  VulkanRenderPass *render_pass = state->domain_render_passes[desc->domain];
  if (desc->renderpass) {
    struct s_RenderPass *named_pass = (struct s_RenderPass *)desc->renderpass;
    if (named_pass->vk) {
      render_pass = named_pass->vk;
    } else {
      log_error("Render pass is not initialized");
      goto cleanup;
    }
  }

  // Derive multisample state from render pass signature
  if (render_pass && render_pass->signature.color_attachment_count > 0) {
    multisample_state.rasterizationSamples =
        (VkSampleCountFlagBits)render_pass->signature.color_samples[0];
  } else if (render_pass && render_pass->signature.has_depth_stencil) {
    // Depth-only pass: use depth/stencil sample count
    multisample_state.rasterizationSamples =
        (VkSampleCountFlagBits)render_pass->signature.depth_stencil_samples;
  }

  VkGraphicsPipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = ArrayCount(shader_stages),
      .pStages = shader_stages,
      .pVertexInputState = &vertex_input_state,
      .pInputAssemblyState = &input_assembly_state,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization_state,
      .pMultisampleState = &multisample_state,
      .pDepthStencilState = &depth_stencil_state,
      .pColorBlendState = &color_blend_state,
      .pDynamicState = &dynamic_state,
      .layout = pipeline_layout,
      .renderPass = render_pass->handle,
      .subpass = 0,
      .basePipelineHandle = VK_NULL_HANDLE,
      .basePipelineIndex = -1,
  };

  const float64_t pipeline_create_start_time = vkr_platform_get_absolute_time();
  const VkResult pipeline_create_result = vkCreateGraphicsPipelines(
      state->device.logical_device, state->pipeline_cache, 1, &pipeline_info,
      state->allocator, &pipeline);
  const float64_t pipeline_create_ms =
      (vkr_platform_get_absolute_time() - pipeline_create_start_time) * 1000.0;
  if (pipeline_create_result != VK_SUCCESS) {
    log_fatal("Failed to create graphics pipeline (VkResult=%d, %.3f ms)",
              pipeline_create_result, pipeline_create_ms);
    goto cleanup;
  }
  log_info("Pipeline create time: %.3f ms (domain=%u cache=%s sets=%u attrs=%u)",
           pipeline_create_ms, desc->domain,
           state->pipeline_cache != VK_NULL_HANDLE ? "enabled" : "disabled",
           reflection->layout_set_count, reflection->vertex_attribute_count);

  // Note: Local state is now acquired via frontend API per renderable.

  if (bindings.length > 0) {
    array_destroy_VkVertexInputBindingDescription(&bindings);
    bindings = (Array_VkVertexInputBindingDescription){0};
  }

  if (attributes.length > 0) {
    array_destroy_VkVertexInputAttributeDescription(&attributes);
    attributes = (Array_VkVertexInputAttributeDescription){0};
  }

  vulkan_pipeline_destroy_set_layouts(state, reflected_set_layouts,
                                      reflected_set_layout_count);
  reflected_set_layouts = NULL;
  reflected_set_layout_count = 0;

  out_pipeline->pipeline_layout = pipeline_layout;
  out_pipeline->pipeline = pipeline;
  success = true_v;

cleanup:
  if (bindings.length > 0) {
    array_destroy_VkVertexInputBindingDescription(&bindings);
  }
  if (attributes.length > 0) {
    array_destroy_VkVertexInputAttributeDescription(&attributes);
  }
  if (reflected_set_layouts) {
    vulkan_pipeline_destroy_set_layouts(state, reflected_set_layouts,
                                        reflected_set_layout_count);
  }
  if (scope_valid) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }

  if (!success) {
    if (pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(state->device.logical_device, pipeline, state->allocator);
    }
    if (pipeline_layout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(state->device.logical_device, pipeline_layout,
                              state->allocator);
    }
    out_pipeline->pipeline = VK_NULL_HANDLE;
    out_pipeline->pipeline_layout = VK_NULL_HANDLE;
    if (shader_object_created) {
      vulkan_shader_object_destroy(state, &out_pipeline->shader_object);
    }
    return false_v;
  }

  log_debug("Created Vulkan pipeline: %p", pipeline);

  return true_v;
}

void vulkan_graphics_pipeline_bind(VulkanCommandBuffer *command_buffer,
                                   VkPipelineBindPoint bind_point,
                                   struct s_GraphicsPipeline *pipeline) {
  assert_log(command_buffer != NULL, "Command buffer is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");

  vkCmdBindPipeline(command_buffer->handle, bind_point, pipeline->pipeline);
}

/**
 * @brief Update pipeline state
 * @param state Vulkan backend state
 * @param pipeline Pipeline containing domain information
 * @param uniform Global uniform data (view/projection matrices)
 * @param data Per-object shader state (model matrix, etc.)
 * @param material Material state (textures, properties)
 * @return VKR_RENDERER_ERROR_NONE on success
 */
VkrRendererError vulkan_graphics_pipeline_update_state(
    VulkanBackendState *state, struct s_GraphicsPipeline *pipeline,
    const void *uniform, const VkrShaderStateObject *data,
    const VkrRendererMaterialState *material) {
  assert_log(state != NULL, "State is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");

  uint32_t image_index = state->image_index;
  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, image_index);

  if (!vulkan_shader_update_global_state(state, &pipeline->shader_object,
                                         pipeline->pipeline_layout, uniform)) {
    log_error("Failed to update global state");
    return VKR_RENDERER_ERROR_PIPELINE_STATE_UPDATE_FAILED;
  }

  if (data != NULL) {
    if (!vulkan_shader_update_instance(state, &pipeline->shader_object,
                                       pipeline->pipeline_layout, data,
                                       material)) {
      log_error("Failed to update state");
      return VKR_RENDERER_ERROR_PIPELINE_STATE_UPDATE_FAILED;
    }
  }

  // Some GPUs don't support updating the pipeline state after the pipeline was
  // bound, so we bind it here
  vulkan_graphics_pipeline_bind(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline);

  return VKR_RENDERER_ERROR_NONE;
}

void vulkan_graphics_pipeline_destroy(VulkanBackendState *state,
                                      struct s_GraphicsPipeline *pipeline) {
  assert_log(state != NULL, "State is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");

  // Local state resources are released via frontend per-object. Nothing to do
  // here.

  vulkan_shader_object_destroy(state, &pipeline->shader_object);

  if (pipeline && pipeline->pipeline != VK_NULL_HANDLE) {
    log_debug("Destroying Vulkan pipeline");
    vkDestroyPipeline(state->device.logical_device, pipeline->pipeline,
                      state->allocator);
    pipeline->pipeline = VK_NULL_HANDLE;
  }

  if (pipeline && pipeline->pipeline_layout != VK_NULL_HANDLE) {
    log_debug("Destroying Vulkan pipeline layout");
    vkDestroyPipelineLayout(state->device.logical_device,
                            pipeline->pipeline_layout, state->allocator);
    pipeline->pipeline_layout = VK_NULL_HANDLE;
  }

  // Note: The pipeline struct itself is owned by the caller and should not be
  // freed here. The caller is responsible for freeing the struct if it
  // allocated it.
}
