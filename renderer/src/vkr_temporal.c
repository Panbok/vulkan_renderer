#include "vkr_temporal.h"

#define VKR_TEMPORAL_CAMERA_CUT_DISTANCE 10.0f
#define VKR_TEMPORAL_CAMERA_CUT_FORWARD_DOT 0.5f

/* `view_projection` with `eye` moved to the origin: the clip position of the
   eye-relative point. The eye's clip position is accumulated in float64 so
   its cancellation leaves only the matrix's own rounding. */
vkr_internal Mat4 vkr_temporal_eye_relative(Mat4 view_projection, Vec3 eye) {
  Mat4 relative = view_projection;
  const float64_t position[3] = {eye.x, eye.y, eye.z};
  for (uint32_t row = 0u; row < 4u; ++row) {
    float64_t clip = view_projection.elements[12u + row];
    for (uint32_t column = 0u; column < 3u; ++column) {
      clip += (float64_t)view_projection.elements[column * 4u + row] *
              position[column];
    }
    relative.elements[12u + row] = (float32_t)clip;
  }
  return relative;
}

Mat4 vkr_temporal_reprojection(Mat4 current_view_projection,
                               Mat4 previous_view_projection,
                               Vec3 current_view_position) {
  return mat4_mul(vkr_temporal_eye_relative(previous_view_projection,
                                            current_view_position),
                  mat4_inverse(vkr_temporal_eye_relative(
                      current_view_projection, current_view_position)));
}

Mat4 vkr_temporal_sky_reprojection(Mat4 current_view_projection,
                                   Mat4 previous_view_projection,
                                   Vec3 current_view_position) {
  /* Move the current eye to the origin before inverting, so a reconstructed
     homogeneous point is already relative to the eye. Inverting the world
     view-projection and subtracting the eye afterwards loses the direction's
     precision hundreds of metres from the origin. */
  const Mat4 current_relative =
      vkr_temporal_eye_relative(current_view_projection, current_view_position);

  /* A zero w turns the eye-relative point into a direction, which removes
     the previous camera's translation. */
  Mat4 direction = mat4_identity();
  direction.m33 = 0.0f;
  Mat4 reprojection =
      mat4_mul(previous_view_projection,
               mat4_mul(direction, mat4_inverse(current_relative)));
  if (current_view_projection.m30 == 0.0f &&
      current_view_projection.m31 == 0.0f &&
      current_view_projection.m32 == 0.0f) {
    /* Orthographic projection leaves a direction's clip w at zero. Intersect
       the previous eye ray with its far plane, q.z == 0 under reversed camera
       depth (ADR-109), where the resolve shader places sky pixels. History
       reuse proves equal projections, so the eye has the same clip
       coordinates in both frames. For projected direction d and eye e,
       q = d - e*d.z/e.z lies on q.z == 0. The eye-relative matrix places
       the eye at the origin, so its clip position is the last column. */
    const Vec4 eye_clip = current_relative.cols[3];
    const Vec4 far_eye = vec4_scale(eye_clip, 1.0f / eye_clip.z);
    Mat4 far_plane = mat4_identity();
    far_plane.cols[2] = vec4_sub(far_plane.cols[2], far_eye);
    reprojection = mat4_mul(far_plane, reprojection);
  }
  return reprojection;
}

Vec2 vkr_temporal_jitter_for_frame(uint32_t frame_index, bool8_t msaa) {
  /* SMAA T2x's two subsamples (Jimenez et al. 2012, @SUBSAMPLE_INDICES):
     (0.25, -0.25) and (-0.25, 0.25) with y up are right-down and left-up
     shifts, which VKR's raster pixels, y down, write as below. SMAA 4x
     jitters the 2x multisample pattern by (0.125, 0.125) and
     (-0.125, -0.125) with y up. */
  const bool8_t first = frame_index % VKR_TEMPORAL_SEQUENCE_LENGTH == 0u;
  if (msaa) {
    return first ? (Vec2){0.125f, -0.125f} : (Vec2){-0.125f, 0.125f};
  }
  return first ? (Vec2){0.25f, 0.25f} : (Vec2){-0.25f, -0.25f};
}

/* The Halton (2, 3) jitter of FSR 3.1 and MetalFX, offset to [-0.5, 0.5)
   pixels. Its phase count comes with the frame input
   (VKR_TEMPORAL_FSR_PHASE_COUNT, VKR_TEMPORAL_METALFX_PHASE_COUNT). */
vkr_internal float32_t vkr_temporal_halton(uint32_t index, uint32_t base) {
  float32_t result = 0.0f;
  float32_t fraction = 1.0f;
  while (index > 0u) {
    fraction /= (float32_t)base;
    result += fraction * (float32_t)(index % base);
    index /= base;
  }
  return result;
}

vkr_internal Vec2 vkr_temporal_halton_jitter(uint32_t frame_index,
                                             uint32_t phase_count) {
  /* Phases start at index 1; index 0's (0, 0) would sample a pixel corner. */
  const uint32_t index = frame_index % phase_count + 1u;
  return (Vec2){vkr_temporal_halton(index, 2u) - 0.5f,
                vkr_temporal_halton(index, 3u) - 0.5f};
}

Vec4 vkr_temporal_smaa_subsample_indices(uint32_t frame_index, uint32_t sample,
                                         bool8_t msaa) {
  const bool8_t first = frame_index % VKR_TEMPORAL_SEQUENCE_LENGTH == 0u;
  if (msaa) {
    /* SMAA 4x's table: frame zero's samples zero and one, then frame
       one's. */
    if (first) {
      return sample == 0u ? (Vec4){5.0f, 3.0f, 1.0f, 3.0f}
                          : (Vec4){4.0f, 6.0f, 2.0f, 3.0f};
    }
    return sample == 0u ? (Vec4){3.0f, 5.0f, 1.0f, 4.0f}
                        : (Vec4){6.0f, 4.0f, 2.0f, 4.0f};
  }
  return first ? (Vec4){1.0f, 1.0f, 1.0f, 0.0f}
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
      .previous_view_projection =
          state->valid ? mat4_mul(state->projection, state->view)
                       : mat4_mul(input->projection, input->view),
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
  if (input->enabled && input->width > 0u && input->height > 0u &&
      input->halton_phase_count > 0u) {
    frame.jitter_pixels = vkr_temporal_halton_jitter(input->frame_index,
                                                     input->halton_phase_count);
    frame.jittered_projection = vkr_temporal_jitter_projection(
        input->projection, frame.jitter_pixels, input->width, input->height);
  } else if (input->enabled && input->width > 0u && input->height > 0u) {
    frame.jitter_pixels =
        vkr_temporal_jitter_for_frame(input->frame_index, input->msaa);
    frame.smaa_subsample_indices = vkr_temporal_smaa_subsample_indices(
        input->frame_index, 0u, input->msaa);
    frame.smaa_subsample_indices_sample1 = vkr_temporal_smaa_subsample_indices(
        input->frame_index, 1u, input->msaa);
    frame.jittered_projection = vkr_temporal_jitter_projection(
        input->projection, frame.jitter_pixels, input->width, input->height);
  } else if (input->msaa) {
    /* SMAA S2x without T2x: the reference's rows for the two samples of the
       unjittered 2x multisample pattern. */
    frame.smaa_subsample_indices = (Vec4){1.0f, 1.0f, 1.0f, 0.0f};
    frame.smaa_subsample_indices_sample1 = (Vec4){2.0f, 2.0f, 2.0f, 0.0f};
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
