/**
 * @file vkr_view_world.h
 * @brief World view layer public API.
 *
 * The World layer is the primary 3D scene rendering layer, responsible for:
 * - Rendering all registered meshes (opaque then transparent)
 * - 3D text rendering
 * - Camera updates from keyboard/mouse/gamepad input
 * - Managing offscreen rendering for editor mode
 *
 * The layer supports runtime switching between swapchain and offscreen
 * rendering via the VKR_VIEW_WORLD_DATA_TOGGLE_OFFSCREEN message.
 */
#pragma once

#include "containers/str.h"
#include "defines.h"
#include "math/vkr_transform.h"
#include "renderer/resources/world/vkr_text_3d.h"

struct s_RendererFrontend;

/**
 * @brief Registers the world view layer with the renderer.
 *
 * Creates and registers the World layer with:
 * - Order 0 (renders after Skybox at -10, before UI at 1)
 * - Depth testing and writing enabled
 * - Camera control from input system
 *
 * @param rf The renderer frontend handle
 * @return true on success, false on failure
 */
bool32_t vkr_view_world_register(struct s_RendererFrontend *rf);

/**
 * @brief Render 3D text into the picking pass.
 * @param rf The renderer frontend handle
 * @param pipeline Picking text pipeline handle
 */
void vkr_view_world_render_picking_text(struct s_RendererFrontend *rf,
                                        VkrPipelineHandle pipeline);

/** @brief Payload for VKR_LAYER_MSG_WORLD_TEXT_CREATE. */
typedef struct VkrViewWorldTextCreateData {
  uint32_t text_id;
  String8 content;
  VkrText3DConfig config;
  bool8_t has_config;
  VkrTransform transform;
} VkrViewWorldTextCreateData;

/** @brief Payload for VKR_LAYER_MSG_WORLD_TEXT_UPDATE. */
typedef struct VkrViewWorldTextUpdateData {
  uint32_t text_id;
  String8 content;
} VkrViewWorldTextUpdateData;

/** @brief Payload for VKR_LAYER_MSG_WORLD_TEXT_SET_TRANSFORM. */
typedef struct VkrViewWorldTextTransformData {
  uint32_t text_id;
  VkrTransform transform;
} VkrViewWorldTextTransformData;

/** @brief Payload for VKR_LAYER_MSG_WORLD_TEXT_DESTROY. */
typedef struct VkrViewWorldTextDestroyData {
  uint32_t text_id;
} VkrViewWorldTextDestroyData;

/** @brief Payload for VKR_LAYER_MSG_WORLD_SET_OFFSCREEN_SIZE. */
typedef struct VkrViewWorldOffscreenSizeData {
  uint32_t width;
  uint32_t height;
} VkrViewWorldOffscreenSizeData;
