#include "temporal_test.h"

#include "renderer/vkr_temporal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static bool32_t temporal_near(float32_t a, float32_t b) {
  return fabsf(a - b) < 1e-6f;
}

static VkrTemporalFrameInput temporal_input(uint32_t frame_index) {
  return (VkrTemporalFrameInput){
      .view = mat4_identity(),
      .projection =
          mat4_perspective(vkr_to_radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f),
      .scene_generation = 1u,
      .frame_index = frame_index,
      .width = 1920u,
      .height = 1080u,
      .enabled = true_v,
  };
}

static void test_temporal_jitter_and_commit(void) {
  printf("  Running test_temporal_jitter_and_commit...\n");
  VkrTemporalState state = {0};
  VkrTemporalFrameInput input = temporal_input(0u);
  VkrTemporalFrame first = vkr_temporal_prepare(&state, &input);
  assert((first.reset_reasons & VKR_TEMPORAL_RESET_FIRST_FRAME) != 0u);
  assert(!first.history_valid);
  assert(first.jitter_pixels.x >= -0.5f && first.jitter_pixels.x < 0.5f &&
         first.jitter_pixels.y >= -0.5f && first.jitter_pixels.y < 0.5f);
  assert(temporal_near(first.jittered_projection.m02,
                       input.projection.m02 -
                           2.0f * first.jitter_pixels.x / 1920.0f));
  assert(temporal_near(first.jittered_projection.m12,
                       input.projection.m12 -
                           2.0f * first.jitter_pixels.y / 1080.0f));

  vkr_temporal_commit(&state, &input);
  input.frame_index = 1u;
  VkrTemporalFrame second = vkr_temporal_prepare(&state, &input);
  assert(second.reset_reasons == VKR_TEMPORAL_RESET_NONE);
  assert(second.history_valid);
  printf("  test_temporal_jitter_and_commit PASSED\n");
}

static void test_temporal_reset_reasons(void) {
  printf("  Running test_temporal_reset_reasons...\n");
  VkrTemporalState state = {0};
  VkrTemporalFrameInput input = temporal_input(10u);
  VkrTemporalFrame frame = vkr_temporal_prepare(&state, &input);
  vkr_temporal_commit(&state, &input);

  VkrTemporalFrameInput changed = input;
  changed.frame_index = 12u;
  changed.width = 1280u;
  changed.scene_generation = 2u;
  changed.render_mode = 1u;
  changed.projection.m00 *= 0.9f;
  changed.view_position = (Vec3){20.0f, 0.0f, 0.0f};
  changed.explicit_reset_reasons = VKR_TEMPORAL_RESET_EXPLICIT;
  frame = vkr_temporal_prepare(&state, &changed);
  const uint32_t expected =
      VKR_TEMPORAL_RESET_MODE_CHANGE | VKR_TEMPORAL_RESET_FRAME_GAP |
      VKR_TEMPORAL_RESET_EXTENT_CHANGE | VKR_TEMPORAL_RESET_SCENE_CHANGE |
      VKR_TEMPORAL_RESET_PROJECTION_CHANGE | VKR_TEMPORAL_RESET_CAMERA_CUT |
      VKR_TEMPORAL_RESET_EXPLICIT;
  assert((frame.reset_reasons & expected) == expected);
  assert(!frame.history_valid);

  changed = input;
  changed.frame_index = 11u;
  changed.enabled = false_v;
  frame = vkr_temporal_prepare(&state, &changed);
  assert((frame.reset_reasons & VKR_TEMPORAL_RESET_MODE_CHANGE) != 0u);
  assert(!frame.history_valid);
  assert(frame.jitter_pixels.x == 0.0f && frame.jitter_pixels.y == 0.0f);
  assert(temporal_near(frame.jittered_projection.m02, changed.projection.m02) &&
         temporal_near(frame.jittered_projection.m12, changed.projection.m12));
  printf("  test_temporal_reset_reasons PASSED\n");
}

static void test_temporal_sequence_repeats(void) {
  printf("  Running test_temporal_sequence_repeats...\n");
  VkrTemporalState state = {0};
  VkrTemporalFrameInput first_input = temporal_input(0u);
  VkrTemporalFrameInput repeated_input = temporal_input(8u);
  const VkrTemporalFrame first = vkr_temporal_prepare(&state, &first_input);
  const VkrTemporalFrame repeated =
      vkr_temporal_prepare(&state, &repeated_input);
  assert(temporal_near(first.jitter_pixels.x, repeated.jitter_pixels.x));
  assert(temporal_near(first.jitter_pixels.y, repeated.jitter_pixels.y));
  printf("  test_temporal_sequence_repeats PASSED\n");
}

static void test_temporal_rotation_cut(void) {
  printf("  Running test_temporal_rotation_cut...\n");
  VkrTemporalState state = {0};
  VkrTemporalFrameInput input = temporal_input(0u);
  vkr_temporal_commit(&state, &input);

  input.frame_index = 1u;
  input.view = mat4_look_at(vec3_zero(), (Vec3){0.0f, 0.0f, 1.0f}, vec3_up());
  const VkrTemporalFrame frame = vkr_temporal_prepare(&state, &input);
  assert((frame.reset_reasons & VKR_TEMPORAL_RESET_CAMERA_CUT) != 0u);
  assert(!frame.history_valid);
  printf("  test_temporal_rotation_cut PASSED\n");
}

bool32_t run_temporal_tests(void) {
  printf("Running temporal tests...\n");
  test_temporal_jitter_and_commit();
  test_temporal_reset_reasons();
  test_temporal_sequence_repeats();
  test_temporal_rotation_cut();
  printf("Temporal tests PASSED\n");
  return true_v;
}
