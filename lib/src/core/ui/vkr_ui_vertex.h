/**
 * @file vkr_ui_vertex.h
 * @brief Renderer-independent UI vertex shared with the GPU ABI.
 */
#pragma once

#include "math/vec.h"

typedef struct VkrUiVertex {
  /** Y-up attachment-pixel position. */
  Vec2 position;
  Vec2 texcoord;
  /** Linear RGB and linear alpha for attachment blending. */
  Vec4 color;
} VkrUiVertex;

_Static_assert(sizeof(VkrUiVertex) == 32u,
               "VkrUiVertex must remain a 32-byte GPU record");
