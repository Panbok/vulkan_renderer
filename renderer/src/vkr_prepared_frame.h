#pragma once

#include "vkr_color_grading.h"
#include "vkr_frame_input.h"

/* Private recording input. The renderer owns derived frame values; input
 * arrays remain borrowed through the synchronous render call. GPU resources
 * retain their independent last-submission lifetime. */
typedef struct VkrPreparedFrame {
  VkrFrameInput input;
  bool8_t scene_rendering;
  bool8_t post_transform_cache_enabled;
  bool8_t editor_image_available;
  uint32_t editor_image_width;
  uint32_t editor_image_height;
  Vec4 editor_image_rect_px;
  /** Authoritative reconstructed Scene extent and internal render scale. */
  uint32_t scene_output_width;
  uint32_t scene_output_height;
  float32_t render_scale;
  /** Vulkan FSR 3.1 consumes temporal inputs and owns its private history. */
  bool8_t fsr31_enabled;
  VkrTemporalFrame temporal;
  VkrExposureFrame exposure;
  VkrColorGradingGpu color_grading;
  VkrBloomFrame bloom;
  bool8_t dof_enabled;
  VkrDofGpuParams dof;
  bool8_t motion_blur_enabled;
  VkrMotionBlurGpuParams motion_blur;
  float64_t motion_blur_delta_seconds;
  bool8_t subsurface_enabled;
  VkrSubsurfaceGpuParams subsurface;
  VkrGtaoFrame gtao;
  bool8_t ssr_enabled;
  bool8_t ssgi_enabled;
  VkrFogGpuParams fog;
  VkrFroxelFogGpuParams froxel_fog;
  uint64_t froxel_fog_signature;
} VkrPreparedFrame;
