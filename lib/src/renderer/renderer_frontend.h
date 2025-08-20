#pragma once

#include "memory/arena.h"
#include "renderer/renderer.h"

#include "vulkan/vulkan_backend.h"

struct s_RendererFrontend {
  Arena *arena;
  RendererBackendType backend_type;
  VkrWindow *window;
  void *backend_state;
  RendererBackendInterface backend;
  bool32_t frame_active;
};