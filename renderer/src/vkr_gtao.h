#pragma once

#include "defines.h"
#include "math/mat.h"

#define VKR_GTAO_DEFAULT_RADIUS 0.5f
#define VKR_GTAO_DEFAULT_POWER 2.2f
#define VKR_GTAO_MAX_DEPTH_MIP_COUNT 5u
#define VKR_GTAO_NOISE_SEQUENCE_LENGTH 64u
#define VKR_GTAO_VIEW_DEPTH_MAX 65504.0f
#define VKR_GTAO_RADIUS_MIN 0.0001f
#define VKR_GTAO_RADIUS_MAX 10000.0f

/** Cold GTAO quality configuration, normalized once at renderer creation. */
typedef struct VkrGtaoConfig {
  uint32_t max_depth_mip_count;
  uint32_t slice_count;
  uint32_t steps_per_slice;
  float32_t radius_multiplier;
  float32_t falloff_range;
  float32_t sample_distribution_power;
  float32_t depth_mip_sampling_offset;
  float32_t denoise_blur_beta;
} VkrGtaoConfig;

/** Packet-facing controls in view-space units. */
typedef struct VkrGtaoFrame {
  bool8_t enabled;
  float32_t radius;
  float32_t power;
} VkrGtaoFrame;

/**
 * Constants shared by every GTAO kernel.
 *
 * Field order mirrors `VkrGtaoParams` in
 * `shaders/shared/gtao_kernel.slangh`. The explicit reserved tail keeps the
 * host/shader ABI a 16-byte multiple on every backend.
 */
typedef struct VkrGtaoGpuParams {
  Mat4 view;

  uint32_t viewport_width;
  uint32_t viewport_height;
  uint32_t depth_mip_count;
  uint32_t reserved_u32_0;

  float32_t viewport_pixel_size_x;
  float32_t viewport_pixel_size_y;
  float32_t projection_m22;
  float32_t projection_m23;

  float32_t projection_m32;
  float32_t projection_m33;
  float32_t projection_m00;
  float32_t projection_m11;

  float32_t projection_m02;
  float32_t projection_m12;
  float32_t projection_m03;
  float32_t projection_m13;

  float32_t effect_radius;
  float32_t radius_multiplier;
  float32_t falloff_range;
  float32_t falloff_mul;

  float32_t falloff_add;
  float32_t depth_mip_falloff_mul;
  float32_t sample_distribution_power;
  float32_t final_value_power;

  float32_t depth_mip_sampling_offset;
  float32_t denoise_blur_beta;
  float32_t reserved_float0;
  float32_t reserved_float1;

  uint32_t slice_count;
  uint32_t steps_per_slice;
  uint32_t noise_index;
  uint32_t reserved0;
} VkrGtaoGpuParams;

_Static_assert(sizeof(VkrGtaoGpuParams) == 192,
               "GTAO parameter ABI must remain 192 bytes");
_Static_assert((sizeof(VkrGtaoGpuParams) % 16u) == 0u,
               "GTAO parameter ABI must be a 16-byte multiple");

VkrGtaoConfig vkr_gtao_config_default(void);
VkrGtaoConfig vkr_gtao_config_normalize(const VkrGtaoConfig *config);

/** Number of valid full-resolution-first depth levels for an image extent. */
uint32_t vkr_gtao_depth_mip_count(const VkrGtaoConfig *config, uint32_t width,
                                  uint32_t height);

/** Disabled frames discard packet controls without inspecting their values. */
VkrGtaoFrame vkr_gtao_prepare(bool8_t enabled, float32_t radius,
                              float32_t power);

VkrGtaoGpuParams vkr_gtao_gpu_params(const VkrGtaoConfig *config,
                                     const VkrGtaoFrame *frame, Mat4 view,
                                     Mat4 projection, uint32_t width,
                                     uint32_t height, uint32_t frame_index,
                                     bool8_t temporal_enabled);
