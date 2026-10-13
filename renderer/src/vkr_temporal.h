#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"

/** SMAA T2x's jitter period: two subsamples, alternating every frame. */
#define VKR_TEMPORAL_SEQUENCE_LENGTH 2u

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
} VkrTemporalResetReason;

typedef struct VkrTemporalState {
  Mat4 view;
  Mat4 projection;
  Vec3 view_position;
  uint64_t scene_generation;
  uint32_t frame_index;
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
  bool8_t enabled;
} VkrTemporalFrameInput;

typedef struct VkrTemporalFrame {
  Mat4 jittered_projection;
  Mat4 current_view_projection;
  /** The camera position of the last committed frame, or the current one
      without history. */
  Vec3 previous_view_position;
  Vec2 jitter_pixels;
  /** SMAA's area-texture rows for `jitter_pixels`; zero without jitter. */
  Vec4 smaa_subsample_indices;
  uint32_t reset_reasons;
  bool8_t history_valid;
  bool8_t enabled;
} VkrTemporalFrame;

/** Raster shift in pixels (y down) of an enabled temporal frame's SMAA T2x
 * subsample. */
Vec2 vkr_temporal_jitter_for_frame(uint32_t frame_index);

/** SMAA's area-texture subsample rows for the frame's jitter. */
Vec4 vkr_temporal_smaa_subsample_indices(uint32_t frame_index);

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

/** Maps current clip rays into history clip space without camera translation.
 * Both matrices use the consumer's native clip convention and exclude jitter.
 * History reuse must prove equal projections. Orthographic rays intersect the
 * history far plane to match the sky shader's far_world-eye direction. */
Mat4 vkr_temporal_sky_reprojection(Mat4 current_view_projection,
                                   Mat4 previous_view_projection,
                                   Vec3 current_view_position);
