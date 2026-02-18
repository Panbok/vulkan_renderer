#pragma once

/**
 * @file vkr_world_resources.h
 * @brief Stateless world pipelines and 3D text resources.
 *
 * Owns the default world pipelines (opaque, transparent, overlay) and the
 * persistent 3D text slots used by the stateless renderer.
 */

#include "containers/array.h"
#include "containers/str.h"
#include "defines.h"
#include "math/vkr_transform.h"
#include "renderer/resources/world/vkr_text_3d.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend;

/**
 * @brief A single 3D text slot in the world resources.
 *
 * Holds the 3D text resource. Slots are indexed by text_id; inactive slots
 * may be reused for new text.
 */
typedef struct VkrWorldTextSlot {
  VkrText3D text; /**< 3D text resource and GPU state */
  bool8_t active; /**< Slot is in use and should be rendered */
} VkrWorldTextSlot;
Array(VkrWorldTextSlot);

/**
 * @brief World resources: pipelines and 3D text slots.
 *
 * Manages world pipelines (opaque, transparent, overlay) and a fixed array
 * of 3D text slots for stateless rendering. Uses the current global view
 * and projection from the render packet.
 */
typedef struct VkrWorldResources {
  VkrShaderConfig shader_config;          /**< Base world shader config */
  VkrPipelineHandle pipeline;             /**< Opaque geometry pipeline */
  VkrPipelineHandle transparent_pipeline; /**< Transparent geometry pipeline */
  VkrPipelineHandle overlay_pipeline;     /**< Overlay geometry pipeline */
  VkrShaderConfig pbr_shader_config;      /**< PBR world shader config */
  VkrShaderConfig
      pbr_world_shader_config; /**< PBR world shader config (opaque name) */
  VkrShaderConfig pbr_transparent_shader_config; /**< PBR shader config for
                                                    transparent domain */
  VkrShaderConfig
      pbr_overlay_shader_config;  /**< PBR shader config for overlay domain */
  VkrPipelineHandle pbr_pipeline; /**< PBR opaque pipeline */
  VkrPipelineHandle pbr_transparent_pipeline; /**< PBR transparent pipeline */
  VkrPipelineHandle pbr_overlay_pipeline;     /**< PBR overlay pipeline */

  VkrShaderConfig text_shader_config; /**< 3D text shader config */
  VkrPipelineHandle text_pipeline;    /**< 3D text glyph pipeline */
  Array_VkrWorldTextSlot text_slots;  /**< Allocated 3D text slots */

  bool8_t initialized; /**< Resources have been initialized */
} VkrWorldResources;

/**
 * @brief Initialize default world pipelines and text slots.
 * @param rf Renderer frontend
 * @param resources World resources to initialize
 * @return true on success, false on failure
 */
bool8_t vkr_world_resources_init(struct s_RendererFrontend *rf,
                                 VkrWorldResources *resources);

/**
 * @brief Release pipelines and text resources.
 * @param rf Renderer frontend
 * @param resources World resources to shutdown
 */
void vkr_world_resources_shutdown(struct s_RendererFrontend *rf,
                                  VkrWorldResources *resources);

/**
 * @brief Create or replace a 3D text slot.
 *
 * Uses payload->text_id when provided to target a specific slot; otherwise
 * allocates a free slot. Copies content and config from payload.
 * @param rf Renderer frontend
 * @param resources World resources
 * @param payload Create data (content, config, transform)
 * @return true on success, false on failure
 */
bool8_t vkr_world_resources_text_create(struct s_RendererFrontend *rf,
                                        VkrWorldResources *resources,
                                        const VkrWorldTextCreateData *payload);

/**
 * @brief Update text content for a 3D text slot.
 * @param rf Renderer frontend
 * @param resources World resources
 * @param text_id Slot id from vkr_world_resources_text_create
 * @param content New text content (copied)
 * @return true on success, false if slot not found
 */
bool8_t vkr_world_resources_text_update(struct s_RendererFrontend *rf,
                                        VkrWorldResources *resources,
                                        uint32_t text_id, String8 content);

/**
 * @brief Update the transform for a 3D text slot.
 * @param rf Renderer frontend
 * @param resources World resources
 * @param text_id Slot id
 * @param transform New world transform (position, rotation, scale)
 * @return true on success, false if slot not found
 */
bool8_t vkr_world_resources_text_set_transform(struct s_RendererFrontend *rf,
                                               VkrWorldResources *resources,
                                               uint32_t text_id,
                                               const VkrTransform *transform);

/**
 * @brief Destroy a 3D text slot.
 *
 * Releases the slot for reuse. Invalidates text_id.
 * @param rf Renderer frontend
 * @param resources World resources
 * @param text_id Slot id to destroy
 * @return true on success, false if slot not found
 */
bool8_t vkr_world_resources_text_destroy(struct s_RendererFrontend *rf,
                                         VkrWorldResources *resources,
                                         uint32_t text_id);

/**
 * @brief Render world text using the current global frame state.
 *
 * Uses view and projection from the render packet.
 * @param rf Renderer frontend
 * @param resources World resources
 */
void vkr_world_resources_render_text(struct s_RendererFrontend *rf,
                                     VkrWorldResources *resources);

/**
 * @brief Render world text into the picking pass.
 *
 * Same geometry as render_text but uses the given picking pipeline for ID
 * output.
 * @param rf Renderer frontend
 * @param resources World resources
 * @param pipeline Picking pipeline to bind
 */
void vkr_world_resources_render_picking_text(struct s_RendererFrontend *rf,
                                             VkrWorldResources *resources,
                                             VkrPipelineHandle pipeline);
