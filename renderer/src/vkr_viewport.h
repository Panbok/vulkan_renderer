/**
 * @file vkr_viewport.h
 * @brief Viewport mapping utilities shared by editor layout and picking.
 *
 * Coordinate conventions:
 * - All rectangles/coordinates are in window pixel space.
 * - Origin is top-left with +Y downward.
 */
#pragma once

#include "defines.h"
#include "math/vec.h"

/**
 * @brief How the scene image should be fit inside a viewport panel.
 */
typedef enum VkrViewportFitMode {
  /** Stretch image to fill the panel rect (no letterboxing). */
  VKR_VIEWPORT_FIT_STRETCH = 0,
  /** Preserve aspect ratio and letterbox/pillarbox (contain). */
  VKR_VIEWPORT_FIT_CONTAIN = 1,
} VkrViewportFitMode;

/**
 * @brief Mapping between the viewport panel and the rendered scene image.
 *
 * - `panel_rect_px`: the full panel rectangle where the viewport lives.
 * - `image_rect_px`: the actual on-screen rectangle where the scene texture is
 *   drawn (may be smaller than the panel when using CONTAIN/letterboxing).
 * - `target_width/target_height`: the render-target resolution in pixels.
 */
typedef struct VkrViewportMapping {
  Vec4 panel_rect_px; /**< (x, y, w, h) in window pixels. */
  Vec4 image_rect_px; /**< (x, y, w, h) in window pixels. */
  uint32_t target_width;
  uint32_t target_height;
  VkrViewportFitMode fit_mode;
} VkrViewportMapping;

/**
 * @brief Converts a window pixel coordinate into a render-target pixel.
 *
 * @param mapping Viewport mapping
 * @param window_x Window X in pixels (origin top-left)
 * @param window_y Window Y in pixels (origin top-left)
 * @param out_x Render-target X in pixels
 * @param out_y Render-target Y in pixels
 * @return true if the point lies within `image_rect_px`, false otherwise
 */
bool8_t vkr_viewport_mapping_window_to_target_pixel(
    const VkrViewportMapping *mapping, int32_t window_x, int32_t window_y,
    uint32_t *out_x, uint32_t *out_y);
