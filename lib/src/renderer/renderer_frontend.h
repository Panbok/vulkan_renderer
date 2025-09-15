#pragma once

#include "memory/arena.h"
#include "renderer/renderer.h"

#include "vulkan/vulkan_backend.h"

typedef struct WorldGraphicsPipeline {
  GraphicsPipelineDescription pipeline;
  PipelineHandle pipeline_handle;

  uint32_t pipeline_attr_count;
  VertexInputAttributeDescription *pipeline_attrs;
  uint32_t pipeline_binding_count;
  VertexInputBindingDescription *pipeline_bindings;
} WorldGraphicsPipeline;

struct s_RendererFrontend {
  Arena *arena;
  VkrWindow *window;
  void *backend_state;
  RendererBackendType backend_type;
  RendererBackendInterface backend;

  WorldGraphicsPipeline world_graphics_pipeline;

  bool32_t frame_active;
};