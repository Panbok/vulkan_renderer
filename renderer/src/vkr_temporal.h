#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"

/** SMAA T2x's jitter period: two subsamples, alternating every frame. */
#define VKR_TEMPORAL_SEQUENCE_LENGTH 2u

/** FSR 3.1's Halton phases. At render scale 1 ffxFsr3GetJitterPhaseCount
 * gives 8 * (output / render)^2 = 8 phases; 16 lowered the Bistro street's
 * still edge error 9.58 -> 9.43 and its trail error in motion 13.13 -> 12.92
 * at no cost (ADR-052). */
#define VKR_TEMPORAL_FSR_PHASE_COUNT 16u

/** MetalFX's Halton phases on the tiled pipeline. Apple's 8 * (output /
 * render)^2 rule gives more at render scales below 1; the removed Metal
 * desktop pipeline measured 8 cleaner on the Bistro bar (ADR-040). */
#define VKR_TEMPORAL_METALFX_PHASE_COUNT 8u

typedef enum VkrTemporalResetReason {
  VKR_TEMPORAL_RESET_NONE = 0u,
  VKR_TEMPORAL_RESET_FIRST_FRAME = 1u << 0u,
  VKR_TEMPORAL_RESET_MODE_CHANGE = 1u << 1u,
  VKR_TEMPORAL_RESET_FRAME_GAP = 1u << 2u,
  VKR_TEMPORAL_RESET_EXTENT_CHANGE = 1u << 3u,
  VKR_TEMPORAL_RESET_SCENE_CHANGE = 1u << 4u,
  VKR_TEMPORAL_RESET_PROJECTION_CHANGE = 1u << 5u,
  VKR_TEMPORAL_RESET_CAMERA_CUT = 1u << 6u,
  VKR_TEMPORAL_RESET_EXPLICIT = 1u << 7u,
  /* Dynamic resolution stepped the render scale. Resolution-independent
     histories, such as the clouds' view-direction history, keep their
     content; every other consumer resets as for any reason. */
  VKR_TEMPORAL_RESET_RESOLUTION_STEP = 1u << 8u,
} VkrTemporalResetReason;

typedef struct VkrTemporalState {
  Mat4 view;
  Mat4 projection;
  Vec3 view_position;
  uint64_t scene_generation;
  uint32_t frame_index;
  /** The Halton index of the last committed frame. It advances by one per
      committed frame, so frame numbers with gaps (the windowed editor skips
      some) still walk every phase. */
  uint32_t halton_index;
  uint32_t width;
  uint32_t height;
  uint32_t render_mode;
  bool8_t enabled;
  bool8_t valid;
} VkrTemporalState;

typedef struct VkrTemporalFrameInput {
  Mat4 view;
  Mat4 projection;
  Vec3 view_position;
  uint64_t scene_generation;
  uint32_t frame_index;
  uint32_t width;
  uint32_t height;
  uint32_t render_mode;
  uint32_t explicit_reset_reasons;
  /** Nonzero: the jitter follows this many Halton (2, 3) phases and no SMAA
      rows apply; FSR 3.1 native AA takes 16 (desktop), MetalFX temporal
      upscaling 8 (tiled). Zero: SMAA T2x's or SMAA 4x's subsamples. */
  uint32_t halton_phase_count;
  bool8_t enabled;
  /** The visibility raster samples each pixel twice (SMAA 4x = S2x + T2x):
      the jitter and subsample rows follow the 4x table. */
  bool8_t msaa;
} VkrTemporalFrameInput;

typedef struct VkrTemporalFrame {
  Mat4 jittered_projection;
  Mat4 current_view_projection;
  /** The unjittered view-projection of the last committed frame, or the
      current one without history. */
  Mat4 previous_view_projection;
  /** The camera position of the last committed frame, or the current one
      without history. */
  Vec3 previous_view_position;
  Vec2 jitter_pixels;
  /** SMAA's area-texture rows for `jitter_pixels`; zero without jitter or
      MSAA. With MSAA they are sample zero's, and
      `smaa_subsample_indices_sample1` sample one's. */
  Vec4 smaa_subsample_indices;
  Vec4 smaa_subsample_indices_sample1;
  uint32_t reset_reasons;
  bool8_t history_valid;
  bool8_t enabled;
} VkrTemporalFrame;

/** Raster shift in pixels (y down) of an enabled temporal frame's SMAA T2x
 * subsample, or of SMAA 4x's when `msaa`. */
Vec2 vkr_temporal_jitter_for_frame(uint32_t frame_index, bool8_t msaa);

/** SMAA's area-texture subsample rows for the frame's jitter and, with
 * `msaa`, the 2x multisample `sample` (SMAA 4x). */
Vec4 vkr_temporal_smaa_subsample_indices(uint32_t frame_index, uint32_t sample,
                                         bool8_t msaa);

/** True when the camera moved or turned too far between two frames for history
 * to follow it. Other camera-driven transitions snap on the same frames. */
bool8_t vkr_temporal_is_camera_cut(Vec3 previous_position, Mat4 previous_view,
                                   Vec3 position, Mat4 view);

/** Builds frame-local temporal state without committing it. */
VkrTemporalFrame vkr_temporal_prepare(const VkrTemporalState *state,
                                      const VkrTemporalFrameInput *input);

/** Commits one successfully submitted frame as the next history source. */
void vkr_temporal_commit(VkrTemporalState *state,
                         const VkrTemporalFrameInput *input);

/** Maps a current clip position, depth included, into the previous frame's
 * clip space. Both matrices use the consumer's native clip convention; pass
 * the jittered current matrix to reproject rasterized depth. Both are taken
 * relative to the current eye before inversion to keep precision far from
 * the origin. */
Mat4 vkr_temporal_reprojection(Mat4 current_view_projection,
                               Mat4 previous_view_projection,
                               Vec3 current_view_position);

/** Maps current clip rays into history clip space without camera translation.
 * Both matrices use the consumer's native clip convention and exclude jitter.
 * History reuse must prove equal projections. Orthographic rays intersect the
 * history far plane to match the sky shader's far_world-eye direction. */
Mat4 vkr_temporal_sky_reprojection(Mat4 current_view_projection,
                                   Mat4 previous_view_projection,
                                   Vec3 current_view_position);
