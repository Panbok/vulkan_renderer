/**
 * @file vkr_layer_messages.h
 * @brief Typed message protocol for inter-layer communication.
 *
 * This header defines a type-safe messaging protocol for layer communication.
 * Each message kind has an associated payload struct and optional response
 * struct. Messages are dispatched synchronously on the render thread.
 *
 * Usage:
 *   VkrLayerMsg_UiTextCreate msg = {
 *     .h = VKR_LAYER_MSG_HEADER_INIT(VKR_LAYER_MSG_UI_TEXT_CREATE,
 *                                    VkrViewUiTextCreateData),
 *     .payload = { .text_id = VKR_INVALID_ID, .content = str8_lit("Hello") }
 *   };
 *   VkrLayerRsp_UiTextCreate rsp;
 *   vkr_view_system_send_msg(renderer, ui_layer, &msg.h, &rsp, sizeof(rsp),
 * NULL);
 */
#pragma once

#include "defines.h"
#include "renderer/systems/views/vkr_view_editor.h"
#include "renderer/systems/views/vkr_view_ui.h"
#include "renderer/systems/views/vkr_view_world.h"

// ============================================================================
// Message Kind Enumeration
// ============================================================================

/**
 * @brief Unified message kind enumeration for all layer messages.
 *
 * Each message kind is associated with a specific payload struct.
 * The naming convention is VKR_LAYER_MSG_<LAYER>_<ACTION>.
 */
typedef enum VkrLayerMsgKind : uint32_t {
  VKR_LAYER_MSG_INVALID = 0,

  // UI Layer Messages (1xx)
  VKR_LAYER_MSG_UI_TEXT_CREATE = 100,
  VKR_LAYER_MSG_UI_TEXT_UPDATE = 101,
  VKR_LAYER_MSG_UI_TEXT_DESTROY = 102,

  // World Layer Messages (2xx)
  VKR_LAYER_MSG_WORLD_TEXT_CREATE = 200,
  VKR_LAYER_MSG_WORLD_TEXT_UPDATE = 201,
  VKR_LAYER_MSG_WORLD_TEXT_SET_TRANSFORM = 202,
  VKR_LAYER_MSG_WORLD_TEXT_DESTROY = 203,
  VKR_LAYER_MSG_WORLD_TOGGLE_OFFSCREEN = 204,
  VKR_LAYER_MSG_WORLD_SET_OFFSCREEN_SIZE = 205,

  // Editor Layer Messages (3xx)
  VKR_LAYER_MSG_EDITOR_GET_VIEWPORT_MAPPING = 300,
  VKR_LAYER_MSG_EDITOR_SET_VIEWPORT_FIT_MODE = 301,
  VKR_LAYER_MSG_EDITOR_SET_RENDER_SCALE = 302,

  VKR_LAYER_MSG_KIND_COUNT
} VkrLayerMsgKind;

// ============================================================================
// Message Header
// ============================================================================

/**
 * @brief Message header flags for optional behaviors.
 */
typedef enum VkrLayerMsgFlags : uint32_t {
  VKR_LAYER_MSG_FLAG_NONE = 0,
  VKR_LAYER_MSG_FLAG_EXPECTS_RESPONSE = 1 << 0,
  VKR_LAYER_MSG_FLAG_DEBUG_ONLY = 1 << 1,
} VkrLayerMsgFlags;

/**
 * @brief Common header for all layer messages.
 *
 * The header provides metadata for validation and debugging.
 * It must be the first field of every typed message struct.
 * 16-byte aligned to ensure payload alignment for SIMD types.
 */
typedef struct VKR_SIMD_ALIGN VkrLayerMsgHeader {
  VkrLayerMsgKind kind;
  uint16_t version;      /**< Protocol version, start at 1. */
  uint16_t payload_size; /**< sizeof(payload struct), validated at runtime. */
  uint32_t flags;        /**< VkrLayerMsgFlags. */
} VkrLayerMsgHeader;

/**
 * @brief Initialize a message header for a given kind and payload type.
 */
#define VKR_LAYER_MSG_HEADER_INIT(msg_kind, payload_type)                      \
  ((VkrLayerMsgHeader){                                                        \
      .kind = (msg_kind),                                                      \
      .version = 1,                                                            \
      .payload_size = sizeof(payload_type),                                    \
      .flags = VKR_LAYER_MSG_FLAG_NONE,                                        \
  })

/**
 * @brief Initialize a message header that expects a response.
 */
#define VKR_LAYER_MSG_HEADER_INIT_WITH_RSP(msg_kind, payload_type)             \
  ((VkrLayerMsgHeader){                                                        \
      .kind = (msg_kind),                                                      \
      .version = 1,                                                            \
      .payload_size = sizeof(payload_type),                                    \
      .flags = VKR_LAYER_MSG_FLAG_EXPECTS_RESPONSE,                            \
  })

/**
 * @brief Initialize a message header for messages with no payload.
 */
#define VKR_LAYER_MSG_HEADER_INIT_NO_PAYLOAD(msg_kind)                         \
  ((VkrLayerMsgHeader){                                                        \
      .kind = (msg_kind),                                                      \
      .version = 1,                                                            \
      .payload_size = 0,                                                       \
      .flags = VKR_LAYER_MSG_FLAG_NONE,                                        \
  })

// ============================================================================
// Response Header
// ============================================================================

/**
 * @brief Response kind enumeration for messages that return data.
 */
typedef enum VkrLayerRspKind : uint32_t {
  VKR_LAYER_RSP_NONE = 0,
  VKR_LAYER_RSP_UI_TEXT_CREATE = 1,
  VKR_LAYER_RSP_EDITOR_VIEWPORT_MAPPING = 2,
} VkrLayerRspKind;

/**
 * @brief Common header for all layer responses.
 */
typedef struct VkrLayerRspHeader {
  VkrLayerRspKind kind;
  uint16_t version;
  uint16_t data_size; /**< sizeof(response payload). */
  uint32_t error;     /**< VkrRendererError or 0 for success. */
} VkrLayerRspHeader;

// ============================================================================
// Typed Message Structures
// ============================================================================

// --- UI Layer Messages ---

/**
 * @brief Create UI text message.
 * Response: VkrLayerRsp_UiTextCreate (returns allocated text_id).
 */
typedef struct VkrLayerMsg_UiTextCreate {
  VkrLayerMsgHeader h;
  VkrViewUiTextCreateData payload;
} VkrLayerMsg_UiTextCreate;

/**
 * @brief Update UI text content message.
 * No response.
 */
typedef struct VkrLayerMsg_UiTextUpdate {
  VkrLayerMsgHeader h;
  VkrViewUiTextUpdateData payload;
} VkrLayerMsg_UiTextUpdate;

/**
 * @brief Destroy UI text message.
 * No response.
 */
typedef struct VkrLayerMsg_UiTextDestroy {
  VkrLayerMsgHeader h;
  VkrViewUiTextDestroyData payload;
} VkrLayerMsg_UiTextDestroy;

// --- World Layer Messages ---

/**
 * @brief Create 3D world text message.
 * No response (uses fixed ID provided in payload).
 */
typedef struct VkrLayerMsg_WorldTextCreate {
  VkrLayerMsgHeader h;
  VkrViewWorldTextCreateData payload;
} VkrLayerMsg_WorldTextCreate;

/**
 * @brief Update 3D world text content message.
 * No response.
 */
typedef struct VkrLayerMsg_WorldTextUpdate {
  VkrLayerMsgHeader h;
  VkrViewWorldTextUpdateData payload;
} VkrLayerMsg_WorldTextUpdate;

/**
 * @brief Set 3D world text transform message.
 * No response.
 */
typedef struct VkrLayerMsg_WorldTextSetTransform {
  VkrLayerMsgHeader h;
  VkrViewWorldTextTransformData payload;
} VkrLayerMsg_WorldTextSetTransform;

/**
 * @brief Destroy 3D world text message.
 * No response.
 */
typedef struct VkrLayerMsg_WorldTextDestroy {
  VkrLayerMsgHeader h;
  VkrViewWorldTextDestroyData payload;
} VkrLayerMsg_WorldTextDestroy;

/**
 * @brief Toggle offscreen rendering mode.
 * No payload, no response.
 */
typedef struct VkrLayerMsg_WorldToggleOffscreen {
  VkrLayerMsgHeader h;
  // No payload
} VkrLayerMsg_WorldToggleOffscreen;

/**
 * @brief Set offscreen render target size.
 * No response.
 */
typedef struct VkrLayerMsg_WorldSetOffscreenSize {
  VkrLayerMsgHeader h;
  VkrViewWorldOffscreenSizeData payload;
} VkrLayerMsg_WorldSetOffscreenSize;

// --- Editor Layer Messages ---

/**
 * @brief Query viewport mapping.
 * Response: VkrLayerRsp_EditorViewportMapping.
 */
typedef struct VkrLayerMsg_EditorGetViewportMapping {
  VkrLayerMsgHeader h;
  // No payload (query operation)
} VkrLayerMsg_EditorGetViewportMapping;

/**
 * @brief Set viewport fit mode.
 * No response.
 */
typedef struct VkrLayerMsg_EditorSetViewportFitMode {
  VkrLayerMsgHeader h;
  VkrViewportFitMode payload;
} VkrLayerMsg_EditorSetViewportFitMode;

/**
 * @brief Set render scale.
 * No response.
 */
typedef struct VkrLayerMsg_EditorSetRenderScale {
  VkrLayerMsgHeader h;
  float32_t payload;
} VkrLayerMsg_EditorSetRenderScale;

// ============================================================================
// Typed Response Structures
// ============================================================================

/**
 * @brief Response for UI text creation.
 */
typedef struct VkrLayerRsp_UiTextCreate {
  VkrLayerRspHeader h;
  uint32_t text_id;
} VkrLayerRsp_UiTextCreate;

/**
 * @brief Response for viewport mapping query.
 */
typedef struct VkrLayerRsp_EditorViewportMapping {
  VkrLayerRspHeader h;
  VkrViewportMapping mapping;
} VkrLayerRsp_EditorViewportMapping;

// ============================================================================
// Message Protocol Metadata
// ============================================================================

/**
 * @brief Metadata describing a message kind.
 */
typedef struct VkrLayerMsgMeta {
  VkrLayerMsgKind kind;
  const char *name;          /**< Human-readable name. */
  uint16_t expected_version; /**< Expected protocol version. */
  uint16_t payload_size;     /**< Expected payload size (0 if none). */
  VkrLayerRspKind rsp_kind;  /**< Response kind (RSP_NONE if no response). */
  uint16_t rsp_size;         /**< Expected response size (0 if none). */
} VkrLayerMsgMeta;

/**
 * @brief Get message metadata by kind.
 * @param kind The message kind.
 * @return Pointer to metadata, or NULL if invalid kind.
 */
const VkrLayerMsgMeta *vkr_layer_msg_get_meta(VkrLayerMsgKind kind);

// ============================================================================
// Compile-time Validation
// ============================================================================

_Static_assert(sizeof(VkrLayerMsgHeader) == 16,
               "VkrLayerMsgHeader must be 16 bytes for payload alignment");

_Static_assert(offsetof(VkrLayerMsg_UiTextCreate, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerMsg_UiTextUpdate, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerMsg_UiTextDestroy, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerMsg_WorldTextCreate, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerMsg_WorldTextUpdate, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerMsg_WorldTextSetTransform, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerMsg_WorldTextDestroy, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerMsg_WorldToggleOffscreen, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerMsg_WorldSetOffscreenSize, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerMsg_EditorGetViewportMapping, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerMsg_EditorSetViewportFitMode, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerMsg_EditorSetRenderScale, h) == 0,
               "Header must be first field");

// Ensure response structs have header as first field
_Static_assert(offsetof(VkrLayerRsp_UiTextCreate, h) == 0,
               "Header must be first field");
_Static_assert(offsetof(VkrLayerRsp_EditorViewportMapping, h) == 0,
               "Header must be first field");
