#pragma once

/**
 * @file vkr_ibl_bake_types.h
 * @brief Internal prepared state shared by scene and world IBL ownership.
 */

#include "renderer/vkr_renderer.h"

#define VKR_IBL_MAX_CUBE_MIPS 16u
#define VKR_IBL_MAX_CUBE_TARGETS (6u * VKR_IBL_MAX_CUBE_MIPS)

/** Face/mip attachments created before recording and retired after the bake. */
typedef struct VkrIblPreparedTargetSet {
  VkrTextureOpaqueHandle texture;
  VkrRenderTargetHandle targets[VKR_IBL_MAX_CUBE_TARGETS];
  uint32_t base_size;
  uint32_t mip_count;
  uint32_t target_count;
  bool8_t ready;
} VkrIblPreparedTargetSet;
