#pragma once

#include "math/vec.h"
#include "vkr_frame_input.h"

struct VkrLocalShadowCache;
struct VkrLocalShadowCamera;
struct VkrLocalShadowPendingHistory;

/** Inputs of one local-shadow cache resolve. */
typedef struct VkrLocalShadowCacheInput {
  const VkrPointLight *lights;
  uint32_t light_count;
  const struct VkrLocalShadowCamera *camera;
  /** When valid, fresh and newer than the last snap, ranks lights by measured
   * visible contribution instead of distance. May be NULL. */
  const VkrLocalLightContributionSample *feedback;
  /** Per light index: a dynamic caster may reach the light's faces. */
  const bool8_t *dynamic_overlap;
  VkrRetainedLocalShadowToken token;
  uint64_t static_generation;
  uint64_t publication_generation;
  /** A dynamic-caster scan was unavailable or an asset publication is in
   * flight, so drawn content may change without a generation change. */
  bool8_t contents_unstable;
  bool8_t refractive_casters;
  uint32_t face_budget;
  uint32_t map_size;
  /** Camera distance at which shadows have faded out; positive. */
  float32_t fade_distance;
} VkrLocalShadowCacheInput;

/* `scratch` holds the next cache state while the previous one is read. */
void vkr_local_shadow_cache_resolve(
    struct VkrLocalShadowCache *cache, struct VkrLocalShadowCache *scratch,
    const VkrLocalShadowCacheInput *input,
    struct VkrLocalShadowPendingHistory *pending,
    VkrLocalShadowPassPayload *out);

/** Commits a submitted frame's cleared layers and drawn faces. */
void vkr_local_shadow_cache_commit(
    struct VkrLocalShadowCache *cache,
    const struct VkrLocalShadowPendingHistory *pending, uint64_t submit_value);

/** Power-of-two face side for a light range, clamped to [128, map_size]. */
uint32_t vkr_local_shadow_face_size_for_range(float32_t range,
                                              uint32_t map_size);
