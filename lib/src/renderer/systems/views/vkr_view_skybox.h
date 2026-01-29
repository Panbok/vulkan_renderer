/**
 * @file vkr_view_skybox.h
 * @brief Skybox view layer public API.
 *
 * The Skybox layer renders an environment cube map as the scene background.
 * It renders first (order -10) and uses front-face culling to show the inside
 * of the skybox cube.
 *
 * The layer supports custom render targets for editor mode, where the skybox
 * renders to offscreen textures shared with World and UI layers.
 */
#pragma once

#include "containers/str.h"
#include "defines.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend;

/**
 * @brief Registers the skybox view layer with the renderer.
 *
 * Creates and registers the Skybox layer with:
 * - Order -10 (renders first, before World at 0)
 * - Depth testing enabled (writes at far plane)
 * - Front-face culling for inside-out cube rendering
 *
 * The skybox cube map is loaded from assets/textures/skybox/.
 *
 * @param rf The renderer frontend handle
 * @return true on success, false on failure
 */
bool32_t vkr_view_skybox_register(struct s_RendererFrontend *rf);

/**
 * @brief Unregisters the skybox view layer.
 *
 * Destroys all skybox resources and removes the layer from the view system.
 *
 * @param rf The renderer frontend handle
 */
void vkr_view_skybox_unregister(struct s_RendererFrontend *rf);

/**
 * @brief Assigns custom render targets for the skybox pass.
 *
 * Used by the World layer to route skybox rendering to offscreen targets
 * in editor mode. Also switches the skybox to use an offscreen-compatible
 * pipeline.
 *
 * @param rf                      The renderer frontend handle
 * @param renderpass_name         The renderpass name to use
 * @param renderpass              The renderpass handle
 * @param render_targets          Per-swapchain render targets array
 * @param render_target_count     Number of render targets
 * @param custom_color_attachments Color attachments for layout transitions
 * @param custom_color_attachment_count Number of custom color attachments
 * @param custom_color_layouts    Layout tracking array for transitions
 * @return true on success, false on failure
 */
bool32_t vkr_view_skybox_set_custom_targets(
    struct s_RendererFrontend *rf, String8 renderpass_name,
    VkrRenderPassHandle renderpass, VkrRenderTargetHandle *render_targets,
    uint32_t render_target_count,
    VkrTextureOpaqueHandle *custom_color_attachments,
    uint32_t custom_color_attachment_count,
    VkrTextureLayout *custom_color_layouts);

/**
 * @brief Restores swapchain-backed rendering for the skybox pass.
 *
 * Reverts the skybox layer to use the builtin swapchain renderpass.
 * Called during teardown or when switching from editor to fullscreen mode.
 *
 * @param rf The renderer frontend handle
 */
void vkr_view_skybox_use_swapchain_targets(struct s_RendererFrontend *rf);
