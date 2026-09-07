#pragma once
#include "defines.h"
#include "math/mat.h"

#define VKR_SHADOW_CASCADE_COUNT_MAX 8
#define VKR_SHADOW_MAP_SIZE_DEFAULT 4096
#define VKR_SHADOW_TARGET_IMAGE_COUNT_MAX 8
#define VKR_SHADOW_DYNAMIC_SCAN_BUDGET_DEFAULT 4096

/**
 * @brief Tap counts the receiver kernel is built for.
 *
 * Packet validation rejects anything outside this set at the cold boundary, so
 * the receiver hot path indexes the shared Poisson table without a bounds or
 * fallback branch. One tap is retained deliberately: it is the low-granularity
 * comparand for the receiver-quality sweep, not a legacy path.
 */
vkr_internal INLINE bool8_t
vkr_shadow_pcf_sample_count_supported(uint32_t count) {
  switch (count) {
  case 1u:
  case 4u:
  case 9u:
  case 16u:
  case 32u:
    return true_v;
  default:
    return false_v;
  }
}

typedef struct VkrSdsmReduceValue {
  float32_t min_device_z;
  float32_t max_device_z;
  uint32_t occupied_count;
} VkrSdsmReduceValue;

typedef struct VkrShadowDepthRangeSample {
  float32_t min_device_z;
  float32_t max_device_z;
  uint32_t occupied_count;
  uint32_t projection_convention;
  Vec4 source_depth_linearize;
  float32_t source_near;
  float32_t source_far;
  uint64_t source_frame_index;
  uint64_t source_projection_generation;
  uint64_t source_scene_generation;
  uint64_t submit_value;
  bool8_t valid;
} VkrShadowDepthRangeSample;

typedef enum VkrShadowSdsmStatus {
  VKR_SHADOW_SDSM_FIXED_FALLBACK = 0,
  VKR_SHADOW_SDSM_WARMUP,
  VKR_SHADOW_SDSM_ACTIVE,
  VKR_SHADOW_SDSM_CACHED,
  VKR_SHADOW_SDSM_EMPTY,
  VKR_SHADOW_SDSM_STALE,
} VkrShadowSdsmStatus;

uint64_t vkr_shadow_projection_generation(const Mat4 *projection);
