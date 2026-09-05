#include "renderer/vkr_temporal.h"

#include <math.h>

#define VKR_TEMPORAL_CAMERA_CUT_DISTANCE 10.0f
#define VKR_TEMPORAL_CAMERA_CUT_FORWARD_DOT 0.5f

static float32_t vkr_temporal_halton(uint32_t index, uint32_t base) {
  float32_t result = 0.0f;
  float32_t fraction = 1.0f;
  while (index > 0u) {
    fraction /= (float32_t)base;
    result += fraction * (float32_t)(index % base);
    index /= base;
  }
  return result;
}

static Vec2 vkr_temporal_jitter(uint32_t frame_index) {
  const uint32_t sequence_index =
      frame_index % VKR_TEMPORAL_SEQUENCE_LENGTH + 1u;
  return (Vec2){vkr_temporal_halton(sequence_index, 2u) - 0.5f,
                vkr_temporal_halton(sequence_index, 3u) - 0.5f};
}

static Vec3 vkr_temporal_view_forward(Mat4 view) {
  const Mat4 camera = mat4_inverse(view);
  return vec3_normalize((Vec3){-camera.m02, -camera.m12, -camera.m22});
}

static bool8_t vkr_temporal_matrix_equal(Mat4 a, Mat4 b) {
  return MemCompare(&a, &b, sizeof(a)) == 0 ? true_v : false_v;
}

static bool8_t vkr_temporal_camera_cut(const VkrTemporalState *state,
                                       const VkrTemporalFrameInput *input) {
  const Vec3 delta = vec3_sub(input->view_position, state->view_position);
  if (vec3_length_squared(delta) >
      VKR_TEMPORAL_CAMERA_CUT_DISTANCE * VKR_TEMPORAL_CAMERA_CUT_DISTANCE)
    return true_v;
  return vec3_dot(vkr_temporal_view_forward(state->view),
                  vkr_temporal_view_forward(input->view)) <
                 VKR_TEMPORAL_CAMERA_CUT_FORWARD_DOT
             ? true_v
             : false_v;
}

static Mat4 vkr_temporal_jitter_projection(Mat4 projection, Vec2 jitter_pixels,
                                           uint32_t width, uint32_t height) {
  projection.m02 -= 2.0f * jitter_pixels.x / (float32_t)width;
  projection.m12 -= 2.0f * jitter_pixels.y / (float32_t)height;
  return projection;
}

VkrTemporalFrame vkr_temporal_prepare(const VkrTemporalState *state,
                                      const VkrTemporalFrameInput *input) {
  VkrTemporalFrame frame = {
      .jittered_projection = input->projection,
      .current_view_projection = mat4_mul(input->projection, input->view),
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
    if (!vkr_temporal_matrix_equal(state->projection, input->projection))
      frame.reset_reasons |= VKR_TEMPORAL_RESET_PROJECTION_CHANGE;
    if (vkr_temporal_camera_cut(state, input))
      frame.reset_reasons |= VKR_TEMPORAL_RESET_CAMERA_CUT;
  }
  if (input->enabled && input->width > 0u && input->height > 0u) {
    frame.jitter_pixels = vkr_temporal_jitter(input->frame_index);
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
