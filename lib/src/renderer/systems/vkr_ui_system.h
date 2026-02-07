#pragma once

/**
 * @file vkr_ui_system.h
 * @brief Stateless UI text and pipeline resources.
 *
 * Owns the UI/text pipelines and persistent UI text slots. UI text is rendered
 * using the current global UI projection; callers may override via offscreen
 * sizing for editor viewports.
 */

#include "containers/array.h"
#include "defines.h"
#include "math/vec.h"
#include "renderer/resources/ui/vkr_ui_text.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend;

/**
 * @brief A single UI text slot in the system.
 *
 * Holds the text resource, layout anchor, and padding. Slots are indexed by
 * text_id; inactive slots may be reused for new text.
 */
typedef struct VkrUiTextSlot {
  VkrUiText text;          /**< Text resource and GPU state */
  bool8_t active;          /**< Slot is in use and should be rendered */
  VkrUiTextAnchor anchor;   /**< Corner anchor for positioning (e.g. top-left) */
  Vec2 padding;            /**< Offset from the anchor in pixels */
} VkrUiTextSlot;
Array(VkrUiTextSlot);

/**
 * @brief UI system state: pipelines, materials, and text slots.
 *
 * Manages UI and text pipelines plus a fixed array of text slots. Layout
 * uses either window dimensions or offscreen dimensions when enabled (e.g.
 * for editor viewport overlay). Call vkr_ui_system_resize on window resize.
 */
typedef struct VkrUiSystem {
  VkrShaderConfig shader_config;           /**< Base UI shader config */
  VkrPipelineHandle pipeline;               /**< UI quad pipeline */
  VkrMaterialHandle material;               /**< UI material */
  VkrRendererInstanceStateHandle instance_state; /**< Per-frame instance state */

  VkrShaderConfig text_shader_config;       /**< Text shader config */
  VkrPipelineHandle text_pipeline;           /**< Text glyph pipeline */

  uint32_t offscreen_width;                 /**< Override width when offscreen enabled */
  uint32_t offscreen_height;                 /**< Override height when offscreen enabled */
  bool8_t offscreen_enabled;                /**< Use offscreen dimensions for layout */
  uint32_t screen_width;                    /**< Last layout width used */
  uint32_t screen_height;                   /**< Last layout height used */

  Array_VkrUiTextSlot text_slots;           /**< Allocated text slots */
  bool8_t initialized;                     /**< System has been initialized */
} VkrUiSystem;

/**
 * @brief Initialize UI pipelines and text slots.
 * @param rf Renderer frontend
 * @param system UI system to initialize
 * @return true on success, false on failure
 */
bool8_t vkr_ui_system_init(struct s_RendererFrontend *rf,
                           VkrUiSystem *system);

/**
 * @brief Release UI pipelines and text slots.
 * @param rf Renderer frontend
 * @param system UI system to shutdown
 */
void vkr_ui_system_shutdown(struct s_RendererFrontend *rf,
                            VkrUiSystem *system);

/**
 * @brief Update UI layout sizing for the current window.
 *
 * If offscreen sizing is enabled, layout uses the offscreen dimensions.
 * Call on window resize or when switching between fullscreen and viewport.
 * @param rf Renderer frontend
 * @param system UI system
 * @param width New width
 * @param height New height
 */
void vkr_ui_system_resize(struct s_RendererFrontend *rf, VkrUiSystem *system,
                          uint32_t width, uint32_t height);

/**
 * @brief Toggle offscreen layout sizing (editor viewport).
 *
 * When enabled, layout uses (width, height) instead of window size. Use for
 * rendering UI into a smaller viewport region.
 * @param rf Renderer frontend
 * @param system UI system
 * @param enabled Whether to use offscreen dimensions
 * @param width Offscreen width when enabled
 * @param height Offscreen height when enabled
 */
void vkr_ui_system_set_offscreen_size(struct s_RendererFrontend *rf,
                                      VkrUiSystem *system, bool8_t enabled,
                                      uint32_t width, uint32_t height);

/**
 * @brief Create or replace a UI text slot.
 *
 * Uses payload->text_id when provided to target a specific slot; otherwise
 * allocates a free slot. Copies content and config from payload.
 * @param rf Renderer frontend
 * @param system UI system
 * @param payload Create data (content, config, anchor, padding)
 * @param out_text_id Output slot id for subsequent update/destroy
 * @return true on success, false on failure
 */
bool8_t vkr_ui_system_text_create(struct s_RendererFrontend *rf,
                                  VkrUiSystem *system,
                                  const VkrUiTextCreateData *payload,
                                  uint32_t *out_text_id);

/**
 * @brief Update UI text content for an existing slot.
 * @param rf Renderer frontend
 * @param system UI system
 * @param text_id Slot id from vkr_ui_system_text_create
 * @param content New text content (copied)
 * @return true on success, false if slot not found
 */
bool8_t vkr_ui_system_text_update(struct s_RendererFrontend *rf,
                                  VkrUiSystem *system, uint32_t text_id,
                                  String8 content);

/**
 * @brief Destroy a UI text slot.
 *
 * Releases the slot for reuse. Invalidates text_id.
 * @param rf Renderer frontend
 * @param system UI system
 * @param text_id Slot id to destroy
 * @return true on success, false if slot not found
 */
bool8_t vkr_ui_system_text_destroy(struct s_RendererFrontend *rf,
                                   VkrUiSystem *system, uint32_t text_id);

/**
 * @brief Render UI text with the current global UI projection.
 *
 * Uses screen or offscreen dimensions depending on vkr_ui_system_set_offscreen_size.
 * @param rf Renderer frontend
 * @param system UI system
 */
void vkr_ui_system_render_text(struct s_RendererFrontend *rf,
                               VkrUiSystem *system);

/**
 * @brief Render UI text into a picking pass.
 *
 * Same geometry as render_text but uses the given picking pipeline for ID output.
 * @param rf Renderer frontend
 * @param system UI system
 * @param pipeline Picking pipeline to bind
 */
void vkr_ui_system_render_picking_text(struct s_RendererFrontend *rf,
                                       VkrUiSystem *system,
                                       VkrPipelineHandle pipeline);
