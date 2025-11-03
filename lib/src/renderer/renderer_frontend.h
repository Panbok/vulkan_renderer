#pragma once

#include "core/event.h"
#include "core/vkr_threads.h"
#include "math/mat.h"
#include "memory/arena.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_camera.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend {
  Arena *arena;
  Arena *scratch_arena;
  VkrWindow *window;
  EventManager *event_manager;
  void *backend_state;
  VkrRendererBackendType backend_type;
  VkrRendererBackendInterface backend;

  // High-level renderer subsystems and state (now accessible)
  VkrPipelineRegistry pipeline_registry;
  VkrShaderSystem shader_system;
  VkrGeometrySystem geometry_system;
  VkrTextureSystem texture_system;
  VkrMaterialSystem material_system;

  // Camera moved into frontend
  VkrCamera camera;

  // Scene shader configs and pipelines
  VkrShaderConfig world_shader_config;
  VkrShaderConfig ui_shader_config;
  VkrPipelineHandle world_pipeline;
  VkrPipelineHandle ui_pipeline;

  // Scene models and geometry
  Mat4 world_model;
  Mat4 ui_model;
  VkrGeometryHandle cube_geometry;
  VkrGeometryHandle ui_geometry;

  // Materials
  VkrMaterialHandle world_material;
  VkrMaterialHandle ui_material;

  // Renderables
  Array_VkrRenderable renderables;
  uint32_t renderable_count;

  // Per-draw state
  VkrShaderStateObject draw_state;

  // Cached global material state for both world and UI
  VkrGlobalMaterialState globals;

  // Window size tracking and thread safety for resize events
  uint32_t last_window_width;
  uint32_t last_window_height;
  VkrMutex rf_mutex;

  bool32_t frame_active;
};

typedef struct s_RendererFrontend RendererFrontend;