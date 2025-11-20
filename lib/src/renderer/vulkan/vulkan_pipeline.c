#include "vulkan_pipeline.h"
#include "vulkan_shaders.h"

bool8_t vulkan_graphics_graphics_pipeline_create(
    VulkanBackendState *state, const VkrGraphicsPipelineDescription *desc,
    struct s_GraphicsPipeline *out_pipeline) {
  assert_log(state != NULL, "State is NULL");
  assert_log(desc != NULL, "Description is NULL");
  assert_log(out_pipeline != NULL, "Out pipeline is NULL");

  if (!vulkan_shader_object_create(state, &desc->shader_object_description,
                                   &out_pipeline->shader_object)) {
    log_fatal("Failed to create shader object");
    return false_v;
  }

  // Bind the description so subsequent dereferences are valid
  out_pipeline->desc = *desc;

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                     VK_DYNAMIC_STATE_SCISSOR,
                                     VK_DYNAMIC_STATE_LINE_WIDTH};

  VkPipelineDynamicStateCreateInfo dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = ArrayCount(dynamic_states),
      .pDynamicStates = dynamic_states,
  };

  Scratch scratch = scratch_create(state->temp_arena);

  Array_VkVertexInputBindingDescription bindings = {0};
  if (desc->binding_count > 0) {
    bindings = array_create_VkVertexInputBindingDescription(
        scratch.arena, desc->binding_count);
    for (uint32_t i = 0; i < desc->binding_count; i++) {
      VkVertexInputBindingDescription binding = {
          .binding = desc->bindings[i].binding,
          .stride = desc->bindings[i].stride,
          .inputRate =
              desc->bindings[i].input_rate == VKR_VERTEX_INPUT_RATE_VERTEX
                  ? VK_VERTEX_INPUT_RATE_VERTEX
                  : VK_VERTEX_INPUT_RATE_INSTANCE,
      };
      array_set_VkVertexInputBindingDescription(&bindings, i, binding);
    }
  }

  Array_VkVertexInputAttributeDescription attributes = {0};
  if (desc->attribute_count > 0) {
    attributes = array_create_VkVertexInputAttributeDescription(
        scratch.arena, desc->attribute_count);
    for (uint32_t i = 0; i < desc->attribute_count; i++) {
      VkVertexInputAttributeDescription attribute = {
          .location = desc->attributes[i].location,
          .binding = desc->attributes[i].binding,
          .format = vulkan_vertex_format_to_vk(desc->attributes[i].format),
          .offset = desc->attributes[i].offset,
      };
      array_set_VkVertexInputAttributeDescription(&attributes, i, attribute);
    }
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

  VkViewport viewport = {
      .x = 0.0f,
      .y = 0.0f,
      .width = (float32_t)state->swapchain.extent.width,
      .height = (float32_t)state->swapchain.extent.height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };

  VkRect2D scissor = {
      .offset = {0, 0},
      .extent = {state->swapchain.extent.width, state->swapchain.extent.height},
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
      .cullMode = VK_CULL_MODE_BACK_BIT,
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

  VkPushConstantRange push_constant = {0};
  if (desc->shader_object_description.push_constant_size > 0) {
    push_constant = (VkPushConstantRange){
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = (uint32_t)desc->shader_object_description.push_constant_size,
    };
  }

  VkDescriptorSetLayout set_layouts[2] = {
      out_pipeline->shader_object.global_descriptor_set_layout,
      out_pipeline->shader_object.instance_descriptor_set_layout,
  };

  VkPipelineLayoutCreateInfo pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 2,
      .pSetLayouts = set_layouts,
      .pushConstantRangeCount =
          (desc->shader_object_description.push_constant_size > 0) ? 1u : 0u,
      .pPushConstantRanges =
          (desc->shader_object_description.push_constant_size > 0)
              ? &push_constant
              : NULL,
  };

  VkPipelineLayout pipeline_layout;
  if (vkCreatePipelineLayout(state->device.logical_device,
                             &pipeline_layout_info, state->allocator,
                             &pipeline_layout) != VK_SUCCESS) {
    log_fatal("Failed to create pipeline layout");
    return false_v;
  }

  out_pipeline->pipeline_layout = pipeline_layout;

  VkPipelineShaderStageCreateInfo shader_stages[] = {
      out_pipeline->shader_object.stages[VKR_SHADER_STAGE_VERTEX],
      out_pipeline->shader_object.stages[VKR_SHADER_STAGE_FRAGMENT],
  };

  VulkanRenderPass *render_pass = state->domain_render_passes[desc->domain];

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

  VkPipeline pipeline;
  if (vkCreateGraphicsPipelines(state->device.logical_device, NULL, 1,
                                &pipeline_info, state->allocator,
                                &pipeline) != VK_SUCCESS) {
    log_fatal("Failed to create graphics pipeline");
    vkDestroyPipelineLayout(state->device.logical_device, pipeline_layout,
                            state->allocator);
    out_pipeline->pipeline_layout = VK_NULL_HANDLE;
    out_pipeline->pipeline = VK_NULL_HANDLE;
    return false_v;
  }

  // Note: Local state is now acquired via frontend API per renderable.

  if (desc->binding_count > 0) {
    array_destroy_VkVertexInputBindingDescription(&bindings);
  }

  if (desc->attribute_count > 0) {
    array_destroy_VkVertexInputAttributeDescription(&attributes);
  }

  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  out_pipeline->pipeline = pipeline;

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

  if (uniform != NULL) {
    if (!vulkan_shader_update_global_state(state, &pipeline->shader_object,
                                           pipeline->pipeline_layout,
                                           uniform)) {
      log_error("Failed to update global state");
      return VKR_RENDERER_ERROR_PIPELINE_STATE_UPDATE_FAILED;
    }
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
}