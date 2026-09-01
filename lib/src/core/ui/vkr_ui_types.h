/**
 * @file vkr_ui_types.h
 * @brief Renderer-independent UI geometry and resource identity.
 */
#pragma once

#include "defines.h"
#include "math/vec.h"

/** Y-down backing-pixel rectangle: x, y, width, height. */
typedef struct VkrUiRect {
  float32_t x;
  float32_t y;
  float32_t width;
  float32_t height;
} VkrUiRect;

/** Logical-point or resolved-pixel edge values in top/right/bottom/left order.
 */
typedef struct VkrUiEdges {
  float32_t top;
  float32_t right;
  float32_t bottom;
  float32_t left;
} VkrUiEdges;

/** Renderer-independent mirror of a generation-safe texture handle. */
typedef struct VkrUiTextureRef {
  uint32_t id;
  uint32_t generation;
} VkrUiTextureRef;

#define VKR_UI_TEXTURE_REF_NONE ((VkrUiTextureRef){0})

bool8_t vkr_ui_rect_is_finite(VkrUiRect rect);
bool8_t vkr_ui_rect_has_area(VkrUiRect rect);
VkrUiRect vkr_ui_rect_intersect(VkrUiRect a, VkrUiRect b);
VkrUiRect vkr_ui_rect_inset(VkrUiRect rect, VkrUiEdges edges);
