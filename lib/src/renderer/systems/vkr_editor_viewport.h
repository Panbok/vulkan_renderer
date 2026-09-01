/**
 * @file vkr_editor_viewport.h
 * @brief Editor viewport layout and packet helpers.
 */
#pragma once

#include "defines.h"
#include "renderer/vkr_render_packet.h"
#include "renderer/vkr_viewport.h"

/** Resolve an editor viewport from a dock-owned Y-down panel rectangle. */
bool8_t vkr_editor_viewport_mapping_from_panel_rect(
    Vec4 panel_rect_px, VkrViewportFitMode fit_mode, float32_t render_scale,
    VkrViewportMapping *out_mapping);

/**
 * @brief Compute the viewport mapping for the standard editor layout.
 *
 * Uses the current fixed panel proportions. `render_scale` is clamped to the
 * supported editor range before the offscreen target extent is resolved.
 */
bool8_t vkr_editor_viewport_compute_mapping(uint32_t window_width,
                                            uint32_t window_height,
                                            VkrViewportFitMode fit_mode,
                                            float32_t render_scale,
                                            VkrViewportMapping *out_mapping);

/** Fill the editor compositor payload from an already-resolved mapping. */
bool8_t vkr_editor_viewport_build_payload(const VkrViewportMapping *mapping,
                                          VkrEditorPassPayload *out_payload);
