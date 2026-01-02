/**
 * @file vkr_layer_messages.c
 * @brief Implementation of typed message protocol metadata and helpers.
 */

#include "vkr_layer_messages.h"

// ============================================================================
// Message Metadata Table
// ============================================================================

static const VkrLayerMsgMeta s_msg_meta_table[] = {
    // UI Layer Messages
    {
        .kind = VKR_LAYER_MSG_UI_TEXT_CREATE,
        .name = "UI_TEXT_CREATE",
        .expected_version = 1,
        .payload_size = sizeof(VkrViewUiTextCreateData),
        .rsp_kind = VKR_LAYER_RSP_UI_TEXT_CREATE,
        .rsp_size = sizeof(VkrLayerRsp_UiTextCreate),
    },
    {
        .kind = VKR_LAYER_MSG_UI_TEXT_UPDATE,
        .name = "UI_TEXT_UPDATE",
        .expected_version = 1,
        .payload_size = sizeof(VkrViewUiTextUpdateData),
        .rsp_kind = VKR_LAYER_RSP_NONE,
        .rsp_size = 0,
    },
    {
        .kind = VKR_LAYER_MSG_UI_TEXT_DESTROY,
        .name = "UI_TEXT_DESTROY",
        .expected_version = 1,
        .payload_size = sizeof(VkrViewUiTextDestroyData),
        .rsp_kind = VKR_LAYER_RSP_NONE,
        .rsp_size = 0,
    },

    // World Layer Messages
    {
        .kind = VKR_LAYER_MSG_WORLD_TEXT_CREATE,
        .name = "WORLD_TEXT_CREATE",
        .expected_version = 1,
        .payload_size = sizeof(VkrViewWorldTextCreateData),
        .rsp_kind = VKR_LAYER_RSP_NONE,
        .rsp_size = 0,
    },
    {
        .kind = VKR_LAYER_MSG_WORLD_TEXT_UPDATE,
        .name = "WORLD_TEXT_UPDATE",
        .expected_version = 1,
        .payload_size = sizeof(VkrViewWorldTextUpdateData),
        .rsp_kind = VKR_LAYER_RSP_NONE,
        .rsp_size = 0,
    },
    {
        .kind = VKR_LAYER_MSG_WORLD_TEXT_SET_TRANSFORM,
        .name = "WORLD_TEXT_SET_TRANSFORM",
        .expected_version = 1,
        .payload_size = sizeof(VkrViewWorldTextTransformData),
        .rsp_kind = VKR_LAYER_RSP_NONE,
        .rsp_size = 0,
    },
    {
        .kind = VKR_LAYER_MSG_WORLD_TEXT_DESTROY,
        .name = "WORLD_TEXT_DESTROY",
        .expected_version = 1,
        .payload_size = sizeof(VkrViewWorldTextDestroyData),
        .rsp_kind = VKR_LAYER_RSP_NONE,
        .rsp_size = 0,
    },
    {
        .kind = VKR_LAYER_MSG_WORLD_TOGGLE_OFFSCREEN,
        .name = "WORLD_TOGGLE_OFFSCREEN",
        .expected_version = 1,
        .payload_size = 0,
        .rsp_kind = VKR_LAYER_RSP_NONE,
        .rsp_size = 0,
    },
    {
        .kind = VKR_LAYER_MSG_WORLD_SET_OFFSCREEN_SIZE,
        .name = "WORLD_SET_OFFSCREEN_SIZE",
        .expected_version = 1,
        .payload_size = sizeof(VkrViewWorldOffscreenSizeData),
        .rsp_kind = VKR_LAYER_RSP_NONE,
        .rsp_size = 0,
    },

    // Editor Layer Messages
    {
        .kind = VKR_LAYER_MSG_EDITOR_GET_VIEWPORT_MAPPING,
        .name = "EDITOR_GET_VIEWPORT_MAPPING",
        .expected_version = 1,
        .payload_size = 0,
        .rsp_kind = VKR_LAYER_RSP_EDITOR_VIEWPORT_MAPPING,
        .rsp_size = sizeof(VkrLayerRsp_EditorViewportMapping),
    },
    {
        .kind = VKR_LAYER_MSG_EDITOR_SET_VIEWPORT_FIT_MODE,
        .name = "EDITOR_SET_VIEWPORT_FIT_MODE",
        .expected_version = 1,
        .payload_size = sizeof(VkrViewportFitMode),
        .rsp_kind = VKR_LAYER_RSP_NONE,
        .rsp_size = 0,
    },
    {
        .kind = VKR_LAYER_MSG_EDITOR_SET_RENDER_SCALE,
        .name = "EDITOR_SET_RENDER_SCALE",
        .expected_version = 1,
        .payload_size = sizeof(float32_t),
        .rsp_kind = VKR_LAYER_RSP_NONE,
        .rsp_size = 0,
    },
};

#define MSG_META_TABLE_COUNT                                                   \
  (sizeof(s_msg_meta_table) / sizeof(s_msg_meta_table[0]))

// ============================================================================
// Public API Implementation
// ============================================================================

const VkrLayerMsgMeta *vkr_layer_msg_get_meta(VkrLayerMsgKind kind) {
  for (uint32_t i = 0; i < MSG_META_TABLE_COUNT; ++i) {
    if (s_msg_meta_table[i].kind == kind) {
      return &s_msg_meta_table[i];
    }
  }
  return NULL;
}
