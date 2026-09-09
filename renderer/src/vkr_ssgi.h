#pragma once

#include "defines.h"
#include "shaders/shared/ssgi_contract.slangh"
#include "math/mat.h"

/** SSGI never emits more than this many hierarchy decisions for one ray. */
#define VKR_SSGI_MAX_STEPS 24u

/** The graph owns at most this many half-resolution depth levels. */
#define VKR_SSGI_MAX_DEPTH_MIP_COUNT 16u

#define VKR_SSGI_DEFAULT_THICKNESS 0.10f
#define VKR_SSGI_DEFAULT_MAX_DISTANCE 20.0f
#define VKR_SSGI_DEFAULT_NORMAL_BIAS 0.02f
#define VKR_SSGI_DEFAULT_EDGE_FADE_PIXELS 8.0f
#define VKR_SSGI_DEFAULT_TEMPORAL_WEIGHT 0.90f
#define VKR_SSGI_DEFAULT_TEMPORAL_DEPTH_RELATIVE 0.05f
#define VKR_SSGI_DEFAULT_TEMPORAL_DEPTH_ABSOLUTE 0.02f

/**
 * Cold SSGI quality controls. The renderer normalizes this record once before
 * it creates the graph, so native traversal loops consume only proven values.
 */
typedef struct VkrSsgiConfig {
  uint32_t max_steps;
  float32_t thickness;
  float32_t max_distance;
  float32_t normal_bias;
  float32_t edge_fade_pixels;
  float32_t temporal_weight;
  float32_t temporal_depth_relative;
  float32_t temporal_depth_absolute;
} VkrSsgiConfig;

/**
 * Constants shared by the SSGI base, mip, trace, temporal and composite
 * kernels. This order mirrors `VkrSsgiParams` in `ssgi_kernel.slangh`.
 *
 * UV is canonical top-left. View space is right handed with forward -Z; all
 * hierarchy and history depths are positive view depth (`-view.z`), where zero
 * represents an uncovered pixel. `view` transforms the world-space G-buffer
 * normal before cosine-weighted diffuse sampling. `previous_projection_*` linearize the
 * previous device depth supplied by the existing motion-validity buffer.
 */
typedef struct VkrSsgiGpuParams {
  Mat4 projection;
  Mat4 inverse_projection;
  Mat4 view;

  uint32_t source_width;
  uint32_t source_height;
  uint32_t trace_width;
  uint32_t trace_height;

  uint32_t depth_mip_count;
  uint32_t max_steps;
  uint32_t history_valid;
  uint32_t sample_phase;

  float32_t thickness;
  float32_t max_distance;
  float32_t normal_bias;
  float32_t edge_fade_pixels;

  float32_t temporal_weight;
  float32_t temporal_depth_relative;
  float32_t temporal_depth_absolute;
  float32_t reserved_float_0;

  float32_t previous_projection_m22;
  float32_t previous_projection_m23;
  float32_t previous_projection_m32;
  float32_t previous_projection_m33;

  float32_t trace_texel_size_x;
  float32_t trace_texel_size_y;
  float32_t history_jitter_uv_x;
  float32_t history_jitter_uv_y;
} VkrSsgiGpuParams;

_Static_assert(sizeof(VkrSsgiGpuParams) == 288u,
               "SSGI parameter ABI must remain 288 bytes");
_Static_assert((sizeof(VkrSsgiGpuParams) % 16u) == 0u,
               "SSGI parameter ABI must be a 16-byte multiple");

VkrSsgiConfig vkr_ssgi_config_default(void);
VkrSsgiConfig vkr_ssgi_config_normalize(const VkrSsgiConfig *config);

/**
 * The graph uses floor-half extents, clamped to one. Its final output texel
 * owns every remaining odd source row or column, so reduction never drops
 * coverage at an odd extent.
 */
uint32_t vkr_ssgi_reduced_extent(uint32_t extent);

/** Includes the half-resolution base level and ends at 1x1. */
uint32_t vkr_ssgi_depth_mip_count(uint32_t source_width, uint32_t source_height);

/**
 * Builds one GPU record from an already normalized configuration. `projection`
 * is the current jittered projection used by the opaque raster;
 * `previous_projection` belongs to the previous submitted raster and is used
 * only to linearize motion validity.y.
 * A zero source extent returns a zero record so the caller can reject it at
 * its frame boundary without dividing by zero in a shader.
 */
VkrSsgiGpuParams vkr_ssgi_gpu_params(const VkrSsgiConfig *config, Mat4 projection,
                                   Mat4 inverse_projection, Mat4 view,
                                   Mat4 previous_projection,
                                   uint32_t source_width,
                                   uint32_t source_height,
                                   bool8_t history_valid, uint32_t sample_phase);
