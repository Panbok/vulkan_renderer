#include "temporal_test.h"

#include "renderer/vkr_dynamic_resolution.h"
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

static void test_dynamic_resolution_config(void) {
  printf("  Running test_dynamic_resolution_config...\n");
  VkrDynamicResolutionConfig normalized = {0};
  float32_t initial_scale = 0.0f;
  assert(vkr_dynamic_resolution_config_normalize(
      &(VkrDynamicResolutionConfig){.enabled = true_v}, 0.73f, &normalized,
      &initial_scale));
  assert(temporal_near(normalized.min_scale, 0.5f));
  assert(temporal_near(normalized.max_scale, 1.0f));
  assert(temporal_near(normalized.target_frame_ms, 1000.0f / 75.0f));
  assert(temporal_near(initial_scale, 0.75f));

  assert(vkr_dynamic_resolution_config_normalize(
      &(VkrDynamicResolutionConfig){.min_scale = 0.334f,
                                    .max_scale = 1.0f,
                                    .target_frame_ms = 13.0f,
                                    .enabled = true_v},
      0.34f, &normalized, &initial_scale));
  assert(temporal_near(normalized.min_scale, 0.334f));
  assert(temporal_near(initial_scale, 0.334f));

  assert(!vkr_dynamic_resolution_config_normalize(
      &(VkrDynamicResolutionConfig){.min_scale = 0.99f,
                                    .max_scale = 0.99f,
                                    .target_frame_ms = 13.0f,
                                    .enabled = true_v},
      1.0f, &normalized, &initial_scale));
  assert(!vkr_dynamic_resolution_config_normalize(
      &(VkrDynamicResolutionConfig){.min_scale = 0.5f,
                                    .max_scale = 1.0f,
                                    .target_frame_ms = NAN,
                                    .enabled = true_v},
      1.0f, &normalized, &initial_scale));
  assert(!vkr_dynamic_resolution_config_normalize(
      &(VkrDynamicResolutionConfig){.min_scale = 0.5f,
                                    .max_scale = 1.0f,
                                    .target_frame_ms = 2.0e13f,
                                    .enabled = true_v},
      1.0f, &normalized, &initial_scale));

  assert(vkr_dynamic_resolution_config_normalize(
      &(VkrDynamicResolutionConfig){0}, 0.4f, &normalized, &initial_scale));
  assert(!normalized.enabled && temporal_near(initial_scale, 0.4f));
  printf("  test_dynamic_resolution_config PASSED\n");
}

static void test_dynamic_resolution_hysteresis(void) {
  printf("  Running test_dynamic_resolution_hysteresis...\n");
  const VkrDynamicResolutionConfig config = {
      .min_scale = 0.5f,
      .max_scale = 1.0f,
      .target_frame_ms = 1000.0f / 75.0f,
      .enabled = true_v,
  };
  VkrDynamicResolutionState state = {0};
  vkr_dynamic_resolution_init(&state, &config, 1.0f);
  float32_t next_scale = 0.0f;

  assert(
      !vkr_dynamic_resolution_update(&state, 1u, 20000000u, 1.0f, &next_scale));
  assert(
      !vkr_dynamic_resolution_update(&state, 2u, 20000000u, 1.0f, &next_scale));
  assert(
      vkr_dynamic_resolution_update(&state, 3u, 20000000u, 1.0f, &next_scale));
  assert(temporal_near(next_scale, 0.95f));
  assert(state.transition_count == 1u);

  assert(
      !vkr_dynamic_resolution_update(&state, 4u, 5000000u, 1.0f, &next_scale));
  assert(state.last_submit_value == 3u);
  assert(
      !vkr_dynamic_resolution_update(&state, 4u, 5000000u, 0.95f, &next_scale));
  assert(
      !vkr_dynamic_resolution_update(&state, 4u, 5000000u, 0.95f, &next_scale));

  for (uint64_t submit = 5u; submit <= 33u; ++submit)
    assert(!vkr_dynamic_resolution_update(&state, submit, 5000000u, 0.95f,
                                          &next_scale));
  for (uint64_t submit = 34u; submit < 78u; ++submit)
    assert(!vkr_dynamic_resolution_update(&state, submit, 5000000u, 0.95f,
                                          &next_scale));
  assert(
      vkr_dynamic_resolution_update(&state, 78u, 5000000u, 0.95f, &next_scale));
  assert(temporal_near(next_scale, 1.0f));
  assert(state.transition_count == 2u);

  VkrDynamicResolutionState stable = {0};
  vkr_dynamic_resolution_init(&stable, &config, 0.75f);
  for (uint64_t submit = 1u; submit <= 100u; ++submit)
    assert(!vkr_dynamic_resolution_update(&stable, submit, 12000000u, 0.75f,
                                          &next_scale));
  assert(stable.transition_count == 0u);

  const VkrDynamicResolutionConfig endpoint_config = {
      .min_scale = 0.334f,
      .max_scale = 1.0f,
      .target_frame_ms = 1000.0f / 75.0f,
      .enabled = true_v,
  };
  VkrDynamicResolutionState endpoint = {0};
  vkr_dynamic_resolution_init(&endpoint, &endpoint_config, 0.35f);
  for (uint64_t submit = 1u; submit < 3u; ++submit)
    assert(!vkr_dynamic_resolution_update(&endpoint, submit, 20000000u, 0.35f,
                                          &next_scale));
  assert(vkr_dynamic_resolution_update(&endpoint, 3u, 20000000u, 0.35f,
                                       &next_scale));
  assert(temporal_near(next_scale, 0.334f));
  printf("  test_dynamic_resolution_hysteresis PASSED\n");
}

bool32_t run_temporal_tests(void) {
  printf("Running temporal tests...\n");
  test_temporal_jitter_and_commit();
  test_temporal_reset_reasons();
  test_temporal_sequence_repeats();
  test_temporal_rotation_cut();
  test_dynamic_resolution_config();
  test_dynamic_resolution_hysteresis();
  printf("Temporal tests PASSED\n");
  return true_v;
}
