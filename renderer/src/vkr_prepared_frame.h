#pragma once

#include "vkr_color_grading.h"
#include "vkr_frame_input.h"

/* Private recording input. The renderer owns derived frame values; input
 * arrays remain borrowed through the synchronous render call. GPU resources
 * retain their independent last-submission lifetime. */
typedef struct VkrPreparedFrame {
  VkrFrameInput input;
  bool8_t scene_rendering;
  /** The tiled graphics pipeline (ADR-087): no temporal reconstruction,
   * screen-space effects, local shadows or transmission. */
  bool8_t tiled_pipeline;
  /** SMAA antialiases the display-linear image: enabled, desktop pipeline,
      default render mode and no temporal reconstruction. */
  bool8_t smaa_enabled;
  /** The visibility raster takes two samples a pixel: with SMAA when
      VKR_MSAA_ENABLED is set (SMAA S2x). */
  bool8_t msaa_enabled;
  /** FSR 3.1 native antialiasing (VkrAntialiasingMode) replaces SMAA and T2x:
      the scene's HDR colour is reconstructed at its own resolution before
      post-processing. */
  bool8_t fsr31_enabled;
  bool8_t post_transform_cache_enabled;
  bool8_t editor_image_available;
  /** The presenter composites this frame's Scene image with the UI on its
      own queue (ADR-111); the graph stops at the Scene image. */
  bool8_t editor_presenter;
  uint32_t editor_image_width;
  uint32_t editor_image_height;
  Vec4 editor_image_rect_px;
  /** Authoritative reconstructed Scene extent and internal render scale. */
  uint32_t scene_output_width;
  uint32_t scene_output_height;
  float32_t render_scale;
  VkrTemporalFrame temporal;
  VkrExposureFrame exposure;
  VkrColorGradingGpu color_grading;
  VkrBloomFrame bloom;
  bool8_t dof_enabled;
  VkrDofGpuParams dof;
  bool8_t motion_blur_enabled;
  VkrMotionBlurGpuParams motion_blur;
  float64_t motion_blur_delta_seconds;
  /** The renderer's clock, the sum of frame deltas wrapped hourly, which
      Custom graphs read as time (ADR-096). */
  float32_t clock_seconds;
  bool8_t subsurface_enabled;
  VkrSubsurfaceGpuParams subsurface;
  VkrGtaoFrame gtao;
  VkrFogGpuParams fog;
  VkrFroxelFogGpuParams froxel_fog;
  uint64_t froxel_fog_signature;
  /** Zeroed unless the packet's sky has an enabled atmosphere. */
  VkrSkyGpuParams sky;
} VkrPreparedFrame;
