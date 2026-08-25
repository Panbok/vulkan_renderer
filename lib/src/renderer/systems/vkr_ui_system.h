#pragma once

/**
 * @file vkr_ui_system.h
 * @brief Stateless UI text and pipeline resources.
 *
 * Owns persistent CPU-side UI text slots used to assemble render packets.
 */

#include "containers/array.h"
#include "defines.h"
#include "math/vec.h"
#include "renderer/resources/ui/vkr_ui_text.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend;
typedef struct VkrPreparedTextDraw VkrPreparedTextDraw;

/**
 * @brief A single UI text slot in the system.
 *
 * Holds the text resource, layout anchor, and padding. Slots are indexed by
 * text_id; inactive slots may be reused for new text.
 */
typedef struct VkrUiTextSlot {
  VkrUiText text;         /**< Text resource and GPU state */
  bool8_t active;         /**< Slot is in use and should be rendered */
  VkrUiTextAnchor anchor; /**< Corner anchor for positioning (e.g. top-left) */
  Vec2 padding;           /**< Offset from the anchor in pixels */
} VkrUiTextSlot;
Array(VkrUiTextSlot);

/**
 * @brief UI text layout and packet-preparation state.
 *
 * Manages a fixed array of text slots. Layout
 * uses either window dimensions or offscreen dimensions when enabled (e.g.
 * for editor viewport overlay). Call vkr_ui_system_resize on window resize.
 */
typedef struct VkrUiSystem {
  uint32_t offscreen_width;     /**< Override width when offscreen enabled */
  uint32_t offscreen_height;    /**< Override height when offscreen enabled */
  bool8_t offscreen_enabled;    /**< Use offscreen dimensions for layout */
  uint32_t screen_width;        /**< Last layout width used */
  uint32_t screen_height;       /**< Last layout height used */
  float32_t text_content_scale; /**< Windows 800x600 design-extent scale */

  Array_VkrUiTextSlot text_slots; /**< Allocated text slots */
  bool8_t initialized;            /**< System has been initialized */
} VkrUiSystem;

/**
 * @brief Initialize UI pipelines and text slots.
 * @param rf Renderer frontend
 * @param system UI system to initialize
 * @return true on success, false on failure
 */
bool8_t vkr_ui_system_init(struct s_RendererFrontend *rf, VkrUiSystem *system);

/**
 * @brief Release UI pipelines and text slots.
 * @param rf Renderer frontend
 * @param system UI system to shutdown
 */
void vkr_ui_system_shutdown(struct s_RendererFrontend *rf, VkrUiSystem *system);

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

/** Builds packet-ready UI text descriptors without issuing GPU commands. */
uint32_t vkr_ui_system_prepare_text_draws(struct s_RendererFrontend *rf,
                                          VkrUiSystem *system,
                                          VkrPreparedTextDraw *out_draws,
                                          uint32_t capacity);
