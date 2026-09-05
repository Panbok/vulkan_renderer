#pragma once

#include "defines.h"

/**
 * @brief Bloom filter kernel selection.
 *
 * The design deliberately does not freeze a tap count: a 13-tap tent and a
 * 4-tap box are both production paths, selected once at initialization, so the
 * comparison on a target GPU is a configuration change rather than a rewrite.
 * The prefilter always uses the 13-tap form because its Karis weighting is what
 * suppresses fireflies at the point they enter the chain.
 */
typedef enum VkrBloomFilter {
  VKR_BLOOM_FILTER_TENT_13 = 0u,
  VKR_BLOOM_FILTER_BOX_4 = 1u,
  VKR_BLOOM_FILTER_COUNT,
} VkrBloomFilter;

/** Production defaults for the per-frame, art-directable controls. */
#define VKR_BLOOM_DEFAULT_THRESHOLD 1.0f
#define VKR_BLOOM_DEFAULT_KNEE 0.5f
#define VKR_BLOOM_DEFAULT_INTENSITY 0.05f

/**
 * @brief Longest chain the graph may instantiate.
 *
 * Compile-time rather than authored: the render graph reserves repeat slots for
 * the downsample and upsample passes, and an unbounded chain would make the
 * per-frame pass count unbounded. Eight halvings reach a single texel from any
 * extent this renderer targets.
 */
#define VKR_BLOOM_MAX_MIP_COUNT 8u

/**
 * @brief Cold bloom configuration.
 *
 * Normalized once at renderer initialization and never per frame, so the chain
 * kernels contain no range or finiteness guard. The per-frame threshold, knee,
 * and intensity arrive through the packet instead; they are art direction, not
 * a resource contract.
 */
typedef struct VkrBloomConfig {
  /** Chain length bound. Normalizes into `[2, VKR_BLOOM_MAX_MIP_COUNT]`. */
  uint32_t max_mip_count;
  /**
   * Smallest extent either dimension of the deepest mip may reach. A chain that
   * runs to a single texel spends passes on a value the upsample tent cannot
   * distinguish from a constant.
   */
  uint32_t min_mip_extent;
  /**
   * Scene-linear ceiling applied to every prefilter tap. One hot texel would
   * otherwise survive every reduction and become a whole-screen halo.
   */
  float32_t firefly_clamp;
  VkrBloomFilter filter;
} VkrBloomConfig;

/**
 * @brief Per-frame bloom controls lowered with the packet.
 *
 * Validated at the frontend boundary. `enabled` false means no bloom resource
 * and no bloom pass is instantiated at all, so a manual frame pays nothing.
 */
typedef struct VkrBloomFrame {
  bool8_t enabled;
  /** Scene-linear luminance at which a texel starts contributing. */
  float32_t threshold;
  /** Soft-knee width in scene-linear luminance, centered on the threshold. */
  float32_t knee;
  /** Weight applied to the resolved bloom when combined with the HDR source. */
  float32_t intensity;
} VkrBloomFrame;

/**
 * @brief Constants lowered to every bloom kernel.
 *
 * Mirrors `VkrBloomParams` in `shaders/shared/bloom_kernel.slangh`. The knee
 * denominator is precomputed here, at the cold boundary, because it is
 * frame-uniform and the kernels would otherwise recompute it per invocation.
 */
typedef struct VkrBloomGpuParams {
  float32_t threshold;
  float32_t knee;
  /** `4 * knee + epsilon`; the soft-knee divisor, never zero. */
  float32_t knee_denominator;
  float32_t firefly_clamp;
  float32_t intensity;
  float32_t reserved[3];
} VkrBloomGpuParams;

_Static_assert(sizeof(VkrBloomGpuParams) == 32,
               "Bloom parameter ABI must remain 32 bytes");

/** Production configuration, already normalized. */
VkrBloomConfig vkr_bloom_config_default(void);

/**
 * @brief Clamps an authored cold record into the supported ranges.
 *
 * A zeroed record normalizes to the production defaults, so a caller may leave
 * the block unset.
 */
VkrBloomConfig vkr_bloom_config_normalize(const VkrBloomConfig *config);

/**
 * @brief Chain length for one viewport extent.
 *
 * Mip 0 is half the viewport, rounded down but never below one texel, so an odd
 * extent still produces a usable chain. Returns 0 when fewer than two mips meet
 * `min_mip_extent`; the caller must then run the frame without bloom
 * rather than dispatch a degenerate chain.
 */
uint32_t vkr_bloom_mip_count(const VkrBloomConfig *config,
                             uint32_t viewport_width, uint32_t viewport_height);

/** Extent of one chain mip, given the viewport. Never returns zero. */
void vkr_bloom_mip_extent(uint32_t viewport_width, uint32_t viewport_height,
                          uint32_t mip, uint32_t *out_width,
                          uint32_t *out_height);

/** Lowers the cold record plus this frame's controls for the kernels. */
VkrBloomGpuParams vkr_bloom_gpu_params(const VkrBloomConfig *config,
                                       const VkrBloomFrame *frame);

/** Builds frame-local bloom controls from validated packet fields. */
VkrBloomFrame vkr_bloom_prepare(bool8_t enabled, float32_t threshold,
                                float32_t knee, float32_t intensity);
