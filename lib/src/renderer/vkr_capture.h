/**
 * @file vkr_capture.h
 * @brief Frontend side of the direct-capture batch: the channel catalog, the
 *        per-frame reservation, and the request-specific graph overlay.
 *
 * A capture request arrives on the packet's debug payload and is reserved
 * before any retained state is mutated, so a rejected or busy batch cancels the
 * frame without half-applying it. The overlay then declares the copy as
 * ordinary graph work — one imported staging buffer write and one exact image
 * slice read per item — rather than discovering resources while recording.
 */
#pragma once

#include "renderer_frontend.h"

/**
 * Validates the packet's capture request, lays its items out in the batch
 * buffer, and reserves a backend ring slot.
 *
 * @return `VKR_RENDERER_ERROR_CAPTURE_BUSY` when every slot is still owned by
 *         earlier work, which the caller may retry without advancing the run.
 */
VkrRendererError vkr_capture_frame_reserve(RendererFrontend *rf,
                                           const VkrRenderPacket *packet,
                                           uint32_t shadow_map_size,
                                           uint32_t shadow_cascade_count,
                                           VkrValidationError *validation);

/** Appends the `Capture.Readback` pass; a no-op when no batch is reserved. */
bool8_t vkr_capture_graph_overlay_build(RendererFrontend *rf);

void vkr_capture_frame_clear(RendererFrontend *rf);
