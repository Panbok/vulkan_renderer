#pragma once

#include "defines.h"

typedef struct VkrIblUv {
  float32_t u;
  float32_t v;
} VkrIblUv;

typedef struct VkrIblDirection {
  float32_t x;
  float32_t y;
  float32_t z;
} VkrIblDirection;

/** Vulkan cubemap array-layer order. */
typedef enum VkrIblCubeFace {
  VKR_IBL_CUBE_FACE_POSITIVE_X = 0,
  VKR_IBL_CUBE_FACE_NEGATIVE_X = 1,
  VKR_IBL_CUBE_FACE_POSITIVE_Y = 2,
  VKR_IBL_CUBE_FACE_NEGATIVE_Y = 3,
  VKR_IBL_CUBE_FACE_POSITIVE_Z = 4,
  VKR_IBL_CUBE_FACE_NEGATIVE_Z = 5,
  VKR_IBL_CUBE_FACE_COUNT = 6,
} VkrIblCubeFace;

/** Converts IEEE-754 binary32 to binary16 with round-to-nearest-even. */
uint16_t vkr_float32_to_float16(float32_t value);

/**
 * Validates a 2:1 equirect source and derives a power-of-two cube extent and
 * complete mip chain within the published device limits.
 */
bool8_t
vkr_ibl_derive_cubemap_size(uint32_t equirect_width, uint32_t equirect_height,
                            uint32_t max_cube_extent, uint32_t max_mip_levels,
                            uint32_t *out_face_size, uint32_t *out_mip_count);

/** Canonical equirect mapping used by the conversion shader. */
VkrIblUv vkr_ibl_direction_to_equirect_uv(VkrIblDirection direction);

/** Inverse of the canonical mapping away from the poles' arbitrary longitude.
 */
VkrIblDirection vkr_ibl_equirect_uv_to_direction(VkrIblUv uv);

/**
 * Maps normalized face coordinates to the unnormalized lookup direction used
 * by Vulkan cubemap sampling. Face coordinates use image convention: (0,0) is
 * the top-left texel and (1,1) is the bottom-right texel.
 */
VkrIblDirection vkr_ibl_cube_face_uv_to_direction(VkrIblCubeFace face,
                                                  VkrIblUv uv);

/**
 * Computes the source-cubemap mip for GGX prefiltered importance sampling.
 * The returned value is finite and clamped to the initialized source range.
 */
float32_t vkr_ibl_prefilter_source_lod(float32_t no_h, float32_t vo_h,
                                       float32_t roughness,
                                       uint32_t sample_count,
                                       uint32_t source_face_size,
                                       uint32_t source_mip_count);
