/**
 * @file vkr_view_ui.h
 * @brief UI view layer public API.
 *
 * The UI layer renders 2D user interface elements using an orthographic
 * projection. It supports:
 * - Anchor-based text positioning (top-left, top-right, etc.)
 * - Runtime switching between swapchain and offscreen rendering
 * - Automatic layout updates on resize
 */
#pragma once

#include "containers/str.h"
#include "defines.h"
#include "math/vec.h"
#include "renderer/resources/ui/vkr_ui_text.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend;

/**
 * @brief Registers the UI view layer with the renderer.
 *
 * Creates and registers the UI layer with:
 * - Order 1 (renders after World at 0, before Editor at 2)
 * - No depth testing (2D overlay)
 * - Orthographic projection matching screen dimensions
 *
 * @param rf The renderer frontend handle
 * @return true on success, false on failure
 */
bool32_t vkr_view_ui_register(struct s_RendererFrontend *rf);

/**
 * @brief Switches the UI layer between offscreen and swapchain rendering.
 *
 * When enabled, the UI layer renders to offscreen targets shared with
 * the World and Skybox layers. When disabled, renders directly to swapchain.
 *
 * This function properly destroys old framebuffers before switching modes
 * to prevent resource leaks.
 *
 * @param rf               The renderer frontend handle
 * @param enabled          true to use offscreen pass, false for swapchain
 * @param color_attachments Offscreen color attachments (required when enabled)
 * @param color_layouts    Layout tracking for color attachments
 * @param attachment_count Number of attachments (typically swapchain count)
 * @param width            Offscreen target width (required when enabled)
 * @param height           Offscreen target height (required when enabled)
 * @return true on success, false on failure
 */
bool32_t vkr_view_ui_set_offscreen_enabled(
    struct s_RendererFrontend *rf, bool8_t enabled,
    VkrTextureOpaqueHandle *color_attachments, VkrTextureLayout *color_layouts,
    uint32_t attachment_count, uint32_t width, uint32_t height);

/**
 * @brief Render UI text into the picking pass.
 * @param rf The renderer frontend handle
 * @param pipeline Picking text pipeline handle
 */
void vkr_view_ui_render_picking_text(struct s_RendererFrontend *rf,
                                     VkrPipelineHandle pipeline);

typedef enum VkrViewUiTextAnchor {
  VKR_VIEW_UI_TEXT_ANCHOR_TOP_LEFT = 0,
  VKR_VIEW_UI_TEXT_ANCHOR_TOP_RIGHT,
  VKR_VIEW_UI_TEXT_ANCHOR_BOTTOM_LEFT,
  VKR_VIEW_UI_TEXT_ANCHOR_BOTTOM_RIGHT,
} VkrViewUiTextAnchor;

/** @brief Payload for VKR_LAYER_MSG_UI_TEXT_CREATE. */
typedef struct VkrViewUiTextCreateData {
  uint32_t text_id;
  String8 content;
  VkrUiTextConfig config;
  bool8_t has_config;
  VkrViewUiTextAnchor anchor;
  Vec2 padding;
} VkrViewUiTextCreateData;

/** @brief Payload for VKR_VIEW_UI_DATA_TEXT_UPDATE. */
typedef struct VkrViewUiTextUpdateData {
  uint32_t text_id;
  String8 content;
} VkrViewUiTextUpdateData;

/** @brief Payload for VKR_VIEW_UI_DATA_TEXT_DESTROY. */
typedef struct VkrViewUiTextDestroyData {
  uint32_t text_id;
} VkrViewUiTextDestroyData;
