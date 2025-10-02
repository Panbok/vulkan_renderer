#pragma once

#include "memory/arena.h"
#include "renderer/vkr_renderer.h"
// todo: we need to either remove this or use it in the application
typedef struct WorldGraphicsPipeline {
  VkrGraphicsPipelineDescription pipeline;
  VkrPipelineOpaqueHandle pipeline_handle;

  uint32_t pipeline_attr_count;
  VkrVertexInputAttributeDescription *pipeline_attrs;
  uint32_t pipeline_binding_count;
  VkrVertexInputBindingDescription *pipeline_bindings;
} WorldGraphicsPipeline;

struct s_RendererFrontend {
  Arena *arena;
  VkrWindow *window;
  void *backend_state;
  VkrRendererBackendType backend_type;
  VkrRendererBackendInterface backend;

  WorldGraphicsPipeline world_graphics_pipeline;

  bool32_t frame_active;
};