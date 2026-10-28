#pragma once

#include "vkr_color_grading.h"
#include "vkr_frame_input.h"
#include "vkr_light_table.h"

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
  /** Ray-query sun shadows on a desktop frame whose device traces them:
      the shadow settings ask for them, or VKR_SUN_RAY_SHADOWS is "1" or
      "2". */
  bool8_t sun_ray_shadows;
  /** With `sun_ray_shadows`, one ray per pixel instead of one per 2x2
      block: the shadow settings ask for full rate, or VKR_SUN_RAY_SHADOWS
      is "2". */
  bool8_t sun_ray_full_rate;
  /** A contact march toward the sun on a desktop frame: the shadow
      settings ask for contact shadows (or VKR_SUN_CONTACT_SHADOWS is "1"),
      the sun shines and its cascades run. */
  bool8_t sun_contact_shadows;
  /** FSR 3.1 native antialiasing (VkrAntialiasingMode) replaces SMAA and T2x:
      the scene's HDR colour is reconstructed at its own resolution before
      post-processing. */
  bool8_t fsr31_enabled;
  /** The tiled pipeline's MetalFX temporal scaler reconstructs the jittered
      scene at the Scene output extent: the renderer upscales with MetalFX,
      the camera is perspective and the render mode is the default. */
  bool8_t tiled_temporal_enabled;
  /** MetalFX's history follows from the previous frame: temporal history
      is valid but for a dynamic-resolution step, whose new input content
      MetalFX takes without a reset (an output resize recreates the
      scaler). */
  bool8_t tiled_temporal_history_valid;
  /** The upscaled frame's camera has not moved since the previous frame:
      a running mean holds MetalFX's output steady against its jitter. */
  bool8_t tiled_temporal_still;
  /** Screen-space contact shadows for the sun on the tiled pipeline: the
      shadow payload asks for them, the sun shines and the backend's
      shading variants that write the sun's light are ready. */
  bool8_t tiled_sun_contact;
  /** Signed-distance-field sun shadows on the tiled pipeline, under the same
      conditions, past the near cascades (VkrShadowSdfMode). */
  bool8_t tiled_sun_sdf;
  /** A texture demand frame on the tiled pipeline (ADR-024): the world asks
      for demand, one frame in VKR_TILED_TEXTURE_DEMAND_INTERVAL, outside
      the overdraw view, and the backend's shading variants that write it
      are ready for the frame's sun variant. */
  bool8_t tiled_texture_demand;
  /** Screen-space sun shafts on the tiled pipeline (ADR-132): the scene's
      volumetric fog is on, a perspective frame in the default render mode
      has a shining sun above the horizon, and the sun stands in or near the
      view. `tiled_sun_shafts_fade`, in (0, 1], is their strength from the
      sun's place. */
  bool8_t tiled_sun_shafts;
  float32_t tiled_sun_shafts_fade;
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
  /** The internal Scene extent at dynamic resolution's largest scale; zero
      takes the viewport. */
  uint32_t viewport_capacity_width;
  uint32_t viewport_capacity_height;
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
  /** The scene frame's packed light rows and the ones its backend's table
      must update (vkr_light_table.h); zeroed on frames without a scene. */
  VkrLightTableFrame light_table;
} VkrPreparedFrame;
