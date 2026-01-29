/**
 * @file vkr_view_editor.h
 * @brief Editor viewport layer public API.
 *
 * The editor layer provides an editor-style UI layout with a central 3D
 * viewport surrounded by UI panels. When enabled, it displays the offscreen
 * scene texture rendered by the World/Skybox/UI layers.
 *
 * The layer is registered disabled by default and toggled via:
 * - VKR_VIEW_WORLD_DATA_TOGGLE_OFFSCREEN message to the World layer
 * - F6 key in the demo application
 */
#pragma once

#include "defines.h"
#include "math/vec.h"

struct s_RendererFrontend;

/**
 * @brief How the scene image should be fit inside the viewport panel.
 *
 * Coordinate conventions used by this module:
 * - All rectangles and coordinates are in **window pixel coordinates**
 * - Origin is **top-left** with **Y increasing downward**
 * - This matches the engine's window input coordinates on macOS (Retina-aware)
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
 * @brief Registers the editor viewport view layer with the renderer.
 *
 * Creates and registers the Editor layer with:
 * - Order 2 (renders after World and UI layers)
 * - Disabled by default (requires explicit enable via toggle)
 * - Uses Renderpass.Editor for final compositing
 *
 * @param rf The renderer frontend handle
 * @return true on success, false on failure
 */
bool32_t vkr_view_editor_register(struct s_RendererFrontend *rf);

/**
 * @brief Gets the current editor viewport mapping.
 *
 * @param rf Renderer frontend
 * @param out_mapping Filled with current mapping on success
 * @return true if mapping is available, false otherwise
 */
bool8_t vkr_view_editor_get_viewport_mapping(struct s_RendererFrontend *rf,
                                             VkrViewportMapping *out_mapping);

/**
 * @brief Sets how the scene image is fit inside the viewport panel.
 *
 * @return true on success, false on failure
 */
bool8_t vkr_view_editor_set_viewport_fit_mode(struct s_RendererFrontend *rf,
                                              VkrViewportFitMode mode);

/**
 * @brief Sets render scale for the scene render target.
 *
 * A value of 1.0 renders at native panel resolution. Values > 1.0 supersample,
 * values < 1.0 downscale. Clamped internally to a safe range.
 *
 * @return true on success, false on failure
 */
bool8_t vkr_view_editor_set_render_scale(struct s_RendererFrontend *rf,
                                         float32_t scale);

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
