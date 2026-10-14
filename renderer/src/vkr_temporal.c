#include "vkr_temporal.h"

#define VKR_TEMPORAL_CAMERA_CUT_DISTANCE 10.0f
#define VKR_TEMPORAL_CAMERA_CUT_FORWARD_DOT 0.5f

Mat4 vkr_temporal_sky_reprojection(Mat4 current_view_projection,
                                   Mat4 previous_view_projection,
                                   Vec3 current_view_position) {
  /* Convert a reconstructed homogeneous world point into a direction from
     the current eye. A zero w removes the previous camera's translation. */
  Mat4 world_direction = mat4_identity();
  world_direction.m03 = -current_view_position.x;
  world_direction.m13 = -current_view_position.y;
  world_direction.m23 = -current_view_position.z;
  world_direction.m33 = 0.0f;
  Mat4 reprojection = mat4_mul(
      previous_view_projection,
      mat4_mul(world_direction, mat4_inverse(current_view_projection)));
  if (current_view_projection.m30 == 0.0f &&
      current_view_projection.m31 == 0.0f &&
      current_view_projection.m32 == 0.0f) {
    /* Orthographic projection leaves a direction's clip w at zero. Intersect
       the previous eye ray with its far plane, q.z == 0 under reversed camera
       depth (ADR-109), where the resolve shader places sky pixels. History
       reuse proves equal projections, so the eye has the same clip
       coordinates in both frames. For projected direction d and eye e,
       q = d - e*d.z/e.z lies on q.z == 0. */
    const Vec4 eye_clip =
        mat4_mul_vec4(current_view_projection,
                      vec4_new(current_view_position.x, current_view_position.y,
                               current_view_position.z, 1.0f));
    const Vec4 far_eye = vec4_scale(eye_clip, 1.0f / eye_clip.z);
    Mat4 far_plane = mat4_identity();
    far_plane.cols[2] = vec4_sub(far_plane.cols[2], far_eye);
    reprojection = mat4_mul(far_plane, reprojection);
  }
  return reprojection;
}

Vec2 vkr_temporal_jitter_for_frame(uint32_t frame_index) {
  /* SMAA T2x's two subsamples (Jimenez et al. 2012, @SUBSAMPLE_INDICES):
     (0.25, -0.25) and (-0.25, 0.25) with y up are right-down and left-up
     shifts, which VKR's raster pixels, y down, write as below. */
  return frame_index % VKR_TEMPORAL_SEQUENCE_LENGTH == 0u
             ? (Vec2){0.25f, 0.25f}
             : (Vec2){-0.25f, -0.25f};
}

Vec4 vkr_temporal_smaa_subsample_indices(uint32_t frame_index) {
  return frame_index % VKR_TEMPORAL_SEQUENCE_LENGTH == 0u
             ? (Vec4){1.0f, 1.0f, 1.0f, 0.0f}
             : (Vec4){2.0f, 2.0f, 2.0f, 0.0f};
}

vkr_internal Vec3 vkr_temporal_view_forward(Mat4 view) {
  const Mat4 camera = mat4_inverse(view);
  return vec3_normalize((Vec3){-camera.m02, -camera.m12, -camera.m22});
}

vkr_internal bool8_t vkr_temporal_matrix_equal(Mat4 a, Mat4 b) {
  return MemCompare(&a, &b, sizeof(a)) == 0 ? true_v : false_v;
}

bool8_t vkr_temporal_is_camera_cut(Vec3 previous_position, Mat4 previous_view,
                                   Vec3 position, Mat4 view) {
  const Vec3 delta = vec3_sub(position, previous_position);
  if (vec3_length_squared(delta) >
      VKR_TEMPORAL_CAMERA_CUT_DISTANCE * VKR_TEMPORAL_CAMERA_CUT_DISTANCE)
    return true_v;
  return vec3_dot(vkr_temporal_view_forward(previous_view),
                  vkr_temporal_view_forward(view)) <
                 VKR_TEMPORAL_CAMERA_CUT_FORWARD_DOT
             ? true_v
             : false_v;
}

vkr_internal bool8_t vkr_temporal_camera_cut(
    const VkrTemporalState *state, const VkrTemporalFrameInput *input) {
  return vkr_temporal_is_camera_cut(state->view_position, state->view,
                                    input->view_position, input->view);
}

vkr_internal Mat4 vkr_temporal_jitter_projection(Mat4 projection,
                                                 Vec2 jitter_pixels,
                                                 uint32_t width,
                                                 uint32_t height) {
  // Translate clip xy by clip w so the raster shift is independent of depth,
  // including orthographic projections where clip w is constant.
  const float32_t x = 2.0f * jitter_pixels.x / (float32_t)width;
  const float32_t y = 2.0f * jitter_pixels.y / (float32_t)height;
  projection.m00 += x * projection.m30;
  projection.m01 += x * projection.m31;
  projection.m02 += x * projection.m32;
  projection.m03 += x * projection.m33;
  projection.m10 += y * projection.m30;
  projection.m11 += y * projection.m31;
  projection.m12 += y * projection.m32;
  projection.m13 += y * projection.m33;
  return projection;
}

VkrTemporalFrame vkr_temporal_prepare(const VkrTemporalState *state,
                                      const VkrTemporalFrameInput *input) {
  VkrTemporalFrame frame = {
      .jittered_projection = input->projection,
      .current_view_projection = mat4_mul(input->projection, input->view),
      .previous_view_position =
          state->valid ? state->view_position : input->view_position,
      .reset_reasons = input->explicit_reset_reasons,
      .enabled = input->enabled,
  };
  if (!state->valid) {
    frame.reset_reasons |= VKR_TEMPORAL_RESET_FIRST_FRAME;
  } else {
    if (state->enabled != input->enabled)
      frame.reset_reasons |= VKR_TEMPORAL_RESET_MODE_CHANGE;
    if (state->frame_index + 1u != input->frame_index)
      frame.reset_reasons |= VKR_TEMPORAL_RESET_FRAME_GAP;
    if (state->width != input->width || state->height != input->height)
      frame.reset_reasons |= VKR_TEMPORAL_RESET_EXTENT_CHANGE;
    if (state->scene_generation != input->scene_generation)
      frame.reset_reasons |= VKR_TEMPORAL_RESET_SCENE_CHANGE;
    if (state->render_mode != input->render_mode)
      frame.reset_reasons |= VKR_TEMPORAL_RESET_MODE_CHANGE;
    /* History reprojects perspective changes such as a zoom. An orthographic
       projection keeps a fixed eye ray, which the sky reprojection assumes
       (vkr_temporal_sky_reprojection), so any change involving one resets. */
    const bool8_t orthographic =
        state->projection.m32 == 0.0f || input->projection.m32 == 0.0f;
    if (orthographic &&
        !vkr_temporal_matrix_equal(state->projection, input->projection))
      frame.reset_reasons |= VKR_TEMPORAL_RESET_PROJECTION_CHANGE;
    if (vkr_temporal_camera_cut(state, input))
      frame.reset_reasons |= VKR_TEMPORAL_RESET_CAMERA_CUT;
  }
  if (input->enabled && input->width > 0u && input->height > 0u) {
    frame.jitter_pixels = vkr_temporal_jitter_for_frame(input->frame_index);
    frame.smaa_subsample_indices =
        vkr_temporal_smaa_subsample_indices(input->frame_index);
    frame.jittered_projection = vkr_temporal_jitter_projection(
        input->projection, frame.jitter_pixels, input->width, input->height);
  }
  frame.history_valid = input->enabled && frame.reset_reasons == 0u;
  return frame;
}

void vkr_temporal_commit(VkrTemporalState *state,
                         const VkrTemporalFrameInput *input) {
  *state = (VkrTemporalState){
      .view = input->view,
      .projection = input->projection,
      .view_position = input->view_position,
      .scene_generation = input->scene_generation,
      .frame_index = input->frame_index,
      .width = input->width,
      .height = input->height,
      .render_mode = input->render_mode,
      .enabled = input->enabled,
      .valid = true_v,
  };
}
