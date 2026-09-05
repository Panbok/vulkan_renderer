#include "renderer/vkr_bloom.h"

#include <math.h>

/** Keeps the soft-knee divisor away from zero when `knee` is zero. */
#define VKR_BLOOM_KNEE_EPSILON 1e-4f
/**
 * Ceiling on the authored firefly clamp. Beyond this a clamped tap is no longer
 * representable in the R16G16B16A16_SFLOAT chain, so the clamp would stop being
 * the thing that bounds the value.
 */
#define VKR_BLOOM_FIREFLY_CLAMP_MAX 65504.0f

VkrBloomConfig vkr_bloom_config_default(void) {
  return (VkrBloomConfig){
      .max_mip_count = 6u,
      .min_mip_extent = 8u,
      .firefly_clamp = 32.0f,
      .filter = VKR_BLOOM_FILTER_TENT_13,
  };
}

VkrBloomConfig vkr_bloom_config_normalize(const VkrBloomConfig *config) {
  const VkrBloomConfig defaults = vkr_bloom_config_default();
  if (!config || config->max_mip_count == 0u)
    return defaults;

  VkrBloomConfig result = defaults;
  /* Two mips is the shortest chain that has an upsample step at all; one mip
     would make the accumulation chain a copy of the prefilter. */
  result.max_mip_count =
      Clamp(config->max_mip_count, 2u, VKR_BLOOM_MAX_MIP_COUNT);
  result.min_mip_extent = ClampBot(config->min_mip_extent, 1u);
  const float32_t firefly_clamp = isfinite(config->firefly_clamp)
                                      ? config->firefly_clamp
                                      : defaults.firefly_clamp;
  result.firefly_clamp =
      Clamp(firefly_clamp, 0.0f, VKR_BLOOM_FIREFLY_CLAMP_MAX);
  result.filter = config->filter < VKR_BLOOM_FILTER_COUNT ? config->filter
                                                          : defaults.filter;
  return result;
}

void vkr_bloom_mip_extent(uint32_t viewport_width, uint32_t viewport_height,
                          uint32_t mip, uint32_t *out_width,
                          uint32_t *out_height) {
  /* Mip 0 is the half-resolution prefilter target, so the chain shift is one
     more than the mip index. Every level clamps to one texel: an odd extent
     must still produce a dispatchable destination. */
  const uint32_t shift = mip + 1u;
  *out_width = ClampBot(viewport_width >> shift, 1u);
  *out_height = ClampBot(viewport_height >> shift, 1u);
}

uint32_t vkr_bloom_mip_count(const VkrBloomConfig *config,
                             uint32_t viewport_width,
                             uint32_t viewport_height) {
  uint32_t count = 0u;
  for (uint32_t mip = 0u; mip < config->max_mip_count; ++mip) {
    uint32_t width = 0u, height = 0u;
    vkr_bloom_mip_extent(viewport_width, viewport_height, mip, &width, &height);
    if (width < config->min_mip_extent || height < config->min_mip_extent)
      break;
    count = mip + 1u;
  }
  /* A single mip has no upsample step, so it is not a chain. Reporting zero
     makes the caller run the frame without bloom instead of dispatching a
     prefilter whose result nothing consumes. */
  return count >= 2u ? count : 0u;
}

VkrBloomGpuParams vkr_bloom_gpu_params(const VkrBloomConfig *config,
                                       const VkrBloomFrame *frame) {
  return (VkrBloomGpuParams){
      .threshold = frame->threshold,
      .knee = frame->knee,
      .knee_denominator = 4.0f * frame->knee + VKR_BLOOM_KNEE_EPSILON,
      .firefly_clamp = config->firefly_clamp,
      .intensity = frame->enabled ? frame->intensity : 0.0f,
  };
}

VkrBloomFrame vkr_bloom_prepare(bool8_t enabled, float32_t threshold,
                                float32_t knee, float32_t intensity) {
  if (!enabled)
    return (VkrBloomFrame){0};
  return (VkrBloomFrame){
      .enabled = true_v,
      .threshold = threshold,
      .knee = knee,
      .intensity = intensity,
  };
}
