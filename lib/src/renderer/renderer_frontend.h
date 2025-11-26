#pragma once

#include "core/event.h"
#include "core/vkr_threads.h"
#include "memory/arena.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/systems/vkr_camera.h"
#include "renderer/systems/vkr_camera_controller.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/systems/vkr_view_system.h"
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
  VkrViewSystem view_system;

  // Camera moved into frontend
  VkrCameraSystem camera_system;
  VkrCameraHandle active_camera;
  VkrCameraController camera_controller;

  // Meshes
  VkrMeshManager mesh_manager;
  VkrMeshLoaderContext mesh_loader;

  VkrLayerHandle world_layer;
  VkrLayerHandle ui_layer;

  // Per-draw state
  VkrShaderStateObject draw_state;

  // Cached global material state for both world and UI
  VkrGlobalMaterialState globals;

  // Window size tracking and thread safety for resize events
  uint32_t last_window_width;
  uint32_t last_window_height;
  VkrMutex rf_mutex;

  bool32_t frame_active;
  uint64_t frame_number;
};

typedef struct s_RendererFrontend RendererFrontend;
