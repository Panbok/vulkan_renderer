/**
 * @file vkr_ui_types.h
 * @brief Renderer-independent UI geometry and resource identity.
 */
#pragma once
#include "vkr_ui_draw_types.h"

#include "defines.h"
#include "math/vec.h"

/** Y-down backing-pixel rectangle: x, y, width, height. */


/** Logical-point or resolved-pixel edge values in top/right/bottom/left order.
 */
typedef struct VkrUiEdges {
  float32_t top;
  float32_t right;
  float32_t bottom;
  float32_t left;
} VkrUiEdges;

/** Renderer-independent mirror of a generation-safe texture handle. */




bool8_t vkr_ui_rect_is_finite(VkrUiRect rect);
bool8_t vkr_ui_rect_has_area(VkrUiRect rect);
VkrUiRect vkr_ui_rect_intersect(VkrUiRect a, VkrUiRect b);
VkrUiRect vkr_ui_rect_inset(VkrUiRect rect, VkrUiEdges edges);
