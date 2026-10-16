#include "temporal_test.h"

#include "vkr_dynamic_resolution.h"
#include "vkr_froxel_fog.h"
#include "vkr_prepared_frame.h"
#include "vkr_temporal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

vkr_internal bool32_t temporal_near(float32_t a, float32_t b) {
  return fabsf(a - b) < 1e-6f;
}

vkr_internal VkrTemporalFrameInput temporal_input(uint32_t frame_index) {
  return (VkrTemporalFrameInput){
      .view = mat4_identity(),
      .projection = mat4_perspective_reversed(vkr_to_radians(60.0f),
                                              16.0f / 9.0f, 0.1f, 1000.0f),
      .scene_generation = 1u,
      .frame_index = frame_index,
      .width = 1920u,
      .height = 1080u,
      .enabled = true_v,
  };
}

vkr_internal void test_temporal_jitter_and_commit(void) {
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

vkr_internal void test_temporal_projection_pixel_shift(void) {
  printf("  Running test_temporal_projection_pixel_shift...\n");
  Mat4 projections[] = {
      mat4_perspective_reversed(vkr_to_radians(60.0f), 16.0f / 9.0f, 0.1f,
                                100.0f),
      mat4_perspective_reversed(vkr_to_radians(60.0f), 16.0f / 9.0f, 0.1f,
                                100.0f),
      mat4_ortho_zo_yinv_reversed(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 100.0f),
      mat4_ortho_zo_yinv_reversed(-3.0f, 5.0f, -2.0f, 4.0f, 0.1f, 100.0f),
  };
  projections[1].m02 = 0.23f;
  projections[1].m12 = -0.17f;
  const Vec4 points[] = {{0.3f, -0.2f, -1.0f, 1.0f},
                         {0.7f, 0.3f, -7.0f, 1.0f},
                         {-2.0f, 1.0f, -70.0f, 1.0f}};
  const VkrTemporalState state = {0};
  for (uint32_t projection = 0u; projection < ArrayCount(projections);
       ++projection) {
    for (uint32_t phase = 0u; phase < VKR_TEMPORAL_SEQUENCE_LENGTH; ++phase) {
      VkrTemporalFrameInput input = temporal_input(phase);
      input.projection = projections[projection];
      input.width = 640u;
      input.height = 360u;
      const VkrTemporalFrame frame = vkr_temporal_prepare(&state, &input);
      for (uint32_t point = 0u; point < ArrayCount(points); ++point) {
        for (uint32_t metal = 0u; metal < 2u; ++metal) {
          Vec4 before = mat4_mul_vec4(input.projection, points[point]);
          Vec4 after = mat4_mul_vec4(frame.jittered_projection, points[point]);
          // Metal negates clip Y at lowering and its viewport inverts NDC Y.
          const float64_t viewport_y = metal ? -0.5 : 0.5;
          if (metal) {
            before.y = -before.y;
            after.y = -after.y;
          }
          const float64_t dx =
              ((float64_t)after.x / after.w - (float64_t)before.x / before.w) *
              0.5 * input.width;
          const float64_t dy =
              ((float64_t)after.y / after.w - (float64_t)before.y / before.w) *
              viewport_y * input.height;
          assert(fabs(dx - frame.jitter_pixels.x) < 1e-4);
          assert(fabs(dy - frame.jitter_pixels.y) < 1e-4);
          assert(before.z == after.z && before.w == after.w);
        }
      }
    }
  }
  printf("  test_temporal_projection_pixel_shift PASSED\n");
}

vkr_internal void test_temporal_reset_reasons(void) {
  printf("  Running test_temporal_reset_reasons...\n");
  VkrTemporalState state = {0};
  VkrTemporalFrameInput input = temporal_input(10u);
  vkr_temporal_commit(&state, &input);

  VkrTemporalFrameInput changed = input;
  changed.frame_index = 12u;
  changed.width = 1280u;
  changed.scene_generation = 2u;
  changed.render_mode = 1u;
  changed.view_position = (Vec3){20.0f, 0.0f, 0.0f};
  changed.explicit_reset_reasons = VKR_TEMPORAL_RESET_EXPLICIT;
  VkrTemporalFrame frame = vkr_temporal_prepare(&state, &changed);
  const uint32_t expected =
      VKR_TEMPORAL_RESET_MODE_CHANGE | VKR_TEMPORAL_RESET_FRAME_GAP |
      VKR_TEMPORAL_RESET_EXTENT_CHANGE | VKR_TEMPORAL_RESET_SCENE_CHANGE |
      VKR_TEMPORAL_RESET_CAMERA_CUT | VKR_TEMPORAL_RESET_EXPLICIT;
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

  // History reprojects a perspective zoom; an orthographic change resets.
  changed = input;
  changed.frame_index = 11u;
  changed.projection.m00 *= 0.9f;
  changed.projection.m11 *= 0.9f;
  frame = vkr_temporal_prepare(&state, &changed);
  assert(frame.reset_reasons == VKR_TEMPORAL_RESET_NONE && frame.history_valid);
  changed.projection =
      mat4_ortho_zo_yinv_reversed(-8.0f, 8.0f, -4.5f, 4.5f, 0.1f, 100.0f);
  frame = vkr_temporal_prepare(&state, &changed);
  assert((frame.reset_reasons & VKR_TEMPORAL_RESET_PROJECTION_CHANGE) != 0u);
  printf("  test_temporal_reset_reasons PASSED\n");
}

vkr_internal void test_temporal_sequence_repeats(void) {
  printf("  Running test_temporal_sequence_repeats...\n");
  VkrTemporalState state = {0};
  VkrTemporalFrameInput first_input = temporal_input(0u);
  VkrTemporalFrameInput second_input = temporal_input(1u);
  VkrTemporalFrameInput repeated_input =
      temporal_input(VKR_TEMPORAL_SEQUENCE_LENGTH);
  const VkrTemporalFrame first = vkr_temporal_prepare(&state, &first_input);
  const VkrTemporalFrame second = vkr_temporal_prepare(&state, &second_input);
  const VkrTemporalFrame repeated =
      vkr_temporal_prepare(&state, &repeated_input);
  assert(temporal_near(first.jitter_pixels.x, repeated.jitter_pixels.x));
  assert(temporal_near(first.jitter_pixels.y, repeated.jitter_pixels.y));
  // SMAA T2x's subsamples (@SUBSAMPLE_INDICES): (0.25, -0.25) y up with
  // area rows 1, then (-0.25, 0.25) with rows 2; VKR pixels run y down.
  assert(temporal_near(first.jitter_pixels.x, 0.25f) &&
         temporal_near(first.jitter_pixels.y, 0.25f));
  assert(temporal_near(second.jitter_pixels.x, -0.25f) &&
         temporal_near(second.jitter_pixels.y, -0.25f));
  assert(temporal_near(first.smaa_subsample_indices.x, 1.0f) &&
         temporal_near(first.smaa_subsample_indices.w, 0.0f));
  assert(temporal_near(second.smaa_subsample_indices.x, 2.0f) &&
         temporal_near(second.smaa_subsample_indices.z, 2.0f));
  printf("  test_temporal_sequence_repeats PASSED\n");
}

vkr_internal void test_temporal_halton_jitter(void) {
  printf("  Running test_temporal_halton_jitter...\n");
  /* Halton (2, 3) indices 1 to 8 minus one half: the published sequence
     1/2, 1/4, 3/4, ... and 1/3, 2/3, 1/9, ... */
  const float32_t expected[8][2] = {
      {0.5f, 1.0f / 3.0f},   {0.25f, 2.0f / 3.0f},   {0.75f, 1.0f / 9.0f},
      {0.125f, 4.0f / 9.0f}, {0.625f, 7.0f / 9.0f},  {0.375f, 2.0f / 9.0f},
      {0.875f, 5.0f / 9.0f}, {0.0625f, 8.0f / 9.0f},
  };
  VkrTemporalState state = {0};
  for (uint32_t frame = 0u; frame < 2u * VKR_TEMPORAL_METALFX_PHASE_COUNT;
       ++frame) {
    VkrTemporalFrameInput input = temporal_input(frame);
    input.halton_phase_count = VKR_TEMPORAL_METALFX_PHASE_COUNT;
    const VkrTemporalFrame result = vkr_temporal_prepare(&state, &input);
    const uint32_t phase = frame % VKR_TEMPORAL_METALFX_PHASE_COUNT;
    assert(fabsf(result.jitter_pixels.x - (expected[phase][0] - 0.5f)) < 1e-6f);
    assert(fabsf(result.jitter_pixels.y - (expected[phase][1] - 0.5f)) < 1e-6f);
    /* No SMAA rows apply to a Halton jitter. */
    assert(result.smaa_subsample_indices.x == 0.0f);
  }

  /* A runtime that renders every other submitted frame (a 120 Hz display
     at 60 frames a second) still walks every phase in order: the index
     follows committed frames, not frame numbers. Stepping by frame number
     would use only the four phases with x >= 0. */
  state = (VkrTemporalState){0};
  for (uint32_t committed = 0u;
       committed < 2u * VKR_TEMPORAL_METALFX_PHASE_COUNT; ++committed) {
    VkrTemporalFrameInput input = temporal_input(2u * committed);
    input.halton_phase_count = VKR_TEMPORAL_METALFX_PHASE_COUNT;
    const VkrTemporalFrame result = vkr_temporal_prepare(&state, &input);
    const uint32_t phase = committed % VKR_TEMPORAL_METALFX_PHASE_COUNT;
    assert(fabsf(result.jitter_pixels.x - (expected[phase][0] - 0.5f)) < 1e-6f);
    assert(fabsf(result.jitter_pixels.y - (expected[phase][1] - 0.5f)) < 1e-6f);
    vkr_temporal_commit(&state, &input);
  }
  printf("  test_temporal_halton_jitter PASSED\n");
}

/* The motion pass maps a rasterized depth through the jittered current
   camera into the previous camera. Far from the origin the eye-relative
   inversion must still place the point where projecting it with the previous
   camera does, within a tenth of a 1080p pixel. */
vkr_internal void test_temporal_reprojection(void) {
  printf("  Running test_temporal_reprojection...\n");
  const Vec3 eye = {4000.0f, 20.0f, -3000.0f};
  const Mat4 projection = mat4_perspective_reversed(
      vkr_to_radians(70.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
  const Mat4 view = mat4_look_at(eye, vec3_add(eye, (Vec3){0.3f, -0.1f, -1.0f}),
                                 (Vec3){0.0f, 1.0f, 0.0f});
  const Vec3 previous_eye = vec3_add(eye, (Vec3){-0.4f, 0.05f, 0.3f});
  const Mat4 previous_view = mat4_look_at(
      previous_eye, vec3_add(previous_eye, (Vec3){0.32f, -0.1f, -1.0f}),
      (Vec3){0.0f, 1.0f, 0.0f});
  VkrTemporalState state = {0};
  VkrTemporalFrameInput input = temporal_input(3u);
  input.view = view;
  input.projection = projection;
  input.view_position = eye;
  input.halton_phase_count = VKR_TEMPORAL_METALFX_PHASE_COUNT;
  const VkrTemporalFrame frame = vkr_temporal_prepare(&state, &input);
  const Mat4 current = mat4_mul(frame.jittered_projection, view);
  const Mat4 previous = mat4_mul(projection, previous_view);
  const Mat4 reprojection = vkr_temporal_reprojection(current, previous, eye);
  const Vec3 offsets[] = {
      {2.0f, -1.0f, -10.0f}, {-6.0f, 3.0f, -40.0f}, {0.5f, -1.5f, -2.5f}};
  for (uint32_t i = 0u; i < ArrayCount(offsets); ++i) {
    const Vec3 point = vec3_add(eye, offsets[i]);
    /* The oracle projects each point in double precision. */
    float64_t clip[4] = {0.0, 0.0, 0.0, 0.0};
    float64_t expected[4] = {0.0, 0.0, 0.0, 0.0};
    const float64_t world[4] = {point.x, point.y, point.z, 1.0};
    for (uint32_t row = 0u; row < 4u; ++row) {
      for (uint32_t column = 0u; column < 4u; ++column) {
        clip[row] +=
            (float64_t)current.elements[column * 4u + row] * world[column];
        expected[row] +=
            (float64_t)previous.elements[column * 4u + row] * world[column];
      }
    }
    const Vec4 raster = {(float32_t)(clip[0] / clip[3]),
                         (float32_t)(clip[1] / clip[3]),
                         (float32_t)(clip[2] / clip[3]), 1.0f};
    const Vec4 result = mat4_mul_vec4(reprojection, raster);
    assert(fabs(result.x / result.w - expected[0] / expected[3]) < 2e-4);
    assert(fabs(result.y / result.w - expected[1] / expected[3]) < 2e-4);
  }
  printf("  test_temporal_reprojection PASSED\n");
}

vkr_internal void test_temporal_rotation_cut(void) {
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

vkr_internal void test_temporal_sky_reprojection(void) {
  printf("  Running test_temporal_sky_reprojection...\n");
  const Vec3 current_eye = {25.0f, -9.0f, 8.0f};
  const Mat4 current_view = mat4_translate((Vec3){-25.0f, 9.0f, -8.0f});
  const Mat4 previous_translation = mat4_translate((Vec3){7.0f, -3.0f, -12.0f});
  const float32_t sine = 1.0f / sqrtf(5.0f);
  const float32_t cosine = 2.0f / sqrtf(5.0f);
  for (uint32_t native_clip = 0u; native_clip < 2u; ++native_clip) {
    Mat4 projection =
        mat4_perspective_reversed(vkr_to_radians(90.0f), 1.0f, 0.1f, 1000.0f);
    if (native_clip != 0u)
      projection.m11 = -projection.m11;
    const Mat4 current = mat4_mul(projection, current_view);
    Mat4 reprojection = vkr_temporal_sky_reprojection(
        current, mat4_mul(projection, previous_translation), current_eye);
    /* Sky pixels lie on the far plane, reversed depth 0. */
    const Vec4 off_center = {0.25f, -0.375f, 0.0f, 1.0f};
    Vec4 previous_clip = mat4_mul_vec4(reprojection, off_center);
    /* An infinite environment cannot move under camera translation. */
    assert(fabsf(previous_clip.x / previous_clip.w - off_center.x) < 1e-5f);
    assert(fabsf(previous_clip.y / previous_clip.w - off_center.y) < 1e-5f);

    Mat4 yaw = mat4_identity();
    yaw.m00 = cosine;
    yaw.m02 = sine;
    yaw.m20 = -sine;
    yaw.m22 = cosine;
    reprojection = vkr_temporal_sky_reprojection(
        current, mat4_mul(projection, mat4_mul(yaw, previous_translation)),
        current_eye);
    previous_clip = mat4_mul_vec4(reprojection, (Vec4){0.0f, 0.0f, 0.0f, 1.0f});
    /* The center ray rotates to (-sin(a), 0, -cos(a)); tan(a) = 1/2. */
    assert(fabsf(previous_clip.x / previous_clip.w + 0.5f) < 1e-5f);
    assert(fabsf(previous_clip.y / previous_clip.w) < 1e-5f);

    Mat4 pitch = mat4_identity();
    pitch.m11 = cosine;
    pitch.m12 = -sine;
    pitch.m21 = sine;
    pitch.m22 = cosine;
    reprojection = vkr_temporal_sky_reprojection(
        current, mat4_mul(projection, mat4_mul(pitch, previous_translation)),
        current_eye);
    previous_clip = mat4_mul_vec4(reprojection, (Vec4){0.0f, 0.0f, 0.0f, 1.0f});
    assert(fabsf(previous_clip.x / previous_clip.w) < 1e-5f);
    assert(fabsf(previous_clip.y / previous_clip.w -
                 (native_clip != 0u ? 0.5f : -0.5f)) < 1e-5f);
  }
  printf("  test_temporal_sky_reprojection PASSED\n");
}

vkr_internal void test_temporal_orthographic_sky_reprojection(void) {
  printf("  Running test_temporal_orthographic_sky_reprojection...\n");
  const Vec3 current_eye = {25.0f, -9.0f, 8.0f};
  const Vec3 previous_eye = {-7.0f, 3.0f, 12.0f};
  const Mat4 current_view = mat4_translate(vec3_scale(current_eye, -1.0f));
  const Mat4 previous_translation =
      mat4_translate(vec3_scale(previous_eye, -1.0f));
  const float32_t sine = 1.0f / sqrtf(5.0f);
  const float32_t cosine = 2.0f / sqrtf(5.0f);
  Mat4 rotations[3] = {mat4_identity(), mat4_identity(), mat4_identity()};
  rotations[1].m00 = cosine;
  rotations[1].m02 = sine;
  rotations[1].m20 = -sine;
  rotations[1].m22 = cosine;
  rotations[2].m11 = cosine;
  rotations[2].m12 = -sine;
  rotations[2].m21 = sine;
  rotations[2].m22 = cosine;
  const Vec4 pixels[] = {{0.0f, 0.0f, 0.0f, 1.0f},
                         {0.25f, -0.375f, 0.0f, 1.0f}};
  for (uint32_t native_clip = 0u; native_clip < 2u; ++native_clip) {
    for (uint32_t shifted = 0u; shifted < 2u; ++shifted) {
      Mat4 projection = shifted != 0u
                            ? mat4_ortho_zo_yinv_reversed(-15.0f, 25.0f, -10.0f,
                                                          30.0f, 0.1f, 20.0f)
                            : mat4_ortho_zo_yinv_reversed(-20.0f, 20.0f, -20.0f,
                                                          20.0f, 0.1f, 20.0f);
      if (native_clip != 0u) {
        projection.m11 = -projection.m11;
        projection.m13 = -projection.m13;
      }
      const Mat4 current = mat4_mul(projection, current_view);
      const Mat4 inverse_current = mat4_inverse(current);
      for (uint32_t rotation = 0u; rotation < 3u; ++rotation) {
        const Mat4 previous = mat4_mul(
            projection, mat4_mul(rotations[rotation], previous_translation));
        const Mat4 inverse_previous = mat4_inverse(previous);
        const Mat4 reprojection =
            vkr_temporal_sky_reprojection(current, previous, current_eye);
        for (uint32_t pixel = 0u; pixel < ArrayCount(pixels); ++pixel) {
          Vec4 previous_clip = mat4_mul_vec4(reprojection, pixels[pixel]);
          assert(previous_clip.w > 1e-6f);
          previous_clip = vec4_scale(previous_clip, 1.0f / previous_clip.w);
          if (rotation == 0u) {
            assert(fabsf(previous_clip.x - pixels[pixel].x) < 1e-5f);
            assert(fabsf(previous_clip.y - pixels[pixel].y) < 1e-5f);
          }
          /* Independent oracle: reconstruct both sky samples exactly as the
             shader does. They must address the same environment direction. */
          const Vec4 current_far =
              mat4_mul_vec4(inverse_current, pixels[pixel]);
          const Vec4 previous_far = mat4_mul_vec4(
              inverse_previous,
              vec4_new(previous_clip.x, previous_clip.y, 0.0f, 1.0f));
          const Vec3 current_direction =
              vec3_normalize(vec3_sub(vec3_new(current_far.x / current_far.w,
                                               current_far.y / current_far.w,
                                               current_far.z / current_far.w),
                                      current_eye));
          const Vec3 previous_direction =
              vec3_normalize(vec3_sub(vec3_new(previous_far.x / previous_far.w,
                                               previous_far.y / previous_far.w,
                                               previous_far.z / previous_far.w),
                                      previous_eye));
          assert(vec3_dot(current_direction, previous_direction) >
                 1.0f - 1e-5f);
        }
      }
    }
  }
  printf("  test_temporal_orthographic_sky_reprojection PASSED\n");
}

vkr_internal void test_dynamic_resolution_config(void) {
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

/* Samples inside the dead band between 80% and 100% of the frame target hold
   the tier; stale and repeated submits are ignored; a step clamps at the
   minimum scale. */
vkr_internal void test_dynamic_resolution_hysteresis(void) {
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
      !vkr_dynamic_resolution_update(&state, 1u, 14000000u, 1.0f, &next_scale));
  assert(
      vkr_dynamic_resolution_update(&state, 2u, 14000000u, 1.0f, &next_scale));
  assert(temporal_near(next_scale, 0.95f));
  assert(state.transition_count == 1u);

  // A sample of the old tier and a repeated submit leave the state alone.
  assert(
      !vkr_dynamic_resolution_update(&state, 3u, 5000000u, 1.0f, &next_scale));
  assert(state.last_submit_value == 2u);
  assert(
      !vkr_dynamic_resolution_update(&state, 3u, 5000000u, 0.95f, &next_scale));
  assert(state.last_submit_value == 3u && state.cooldown_samples == 0u);
  assert(
      !vkr_dynamic_resolution_update(&state, 3u, 5000000u, 0.95f, &next_scale));
  assert(state.under_budget_samples == 0u);

  for (uint64_t submit = 4u; submit < 33u; ++submit)
    assert(!vkr_dynamic_resolution_update(&state, submit, 5000000u, 0.95f,
                                          &next_scale));
  assert(
      vkr_dynamic_resolution_update(&state, 33u, 5000000u, 0.95f, &next_scale));
  assert(temporal_near(next_scale, 1.0f));
  assert(state.transition_count == 2u);

  VkrDynamicResolutionState steady = {0};
  vkr_dynamic_resolution_init(&steady, &config, 0.75f);
  for (uint64_t submit = 1u; submit <= 100u; ++submit)
    assert(!vkr_dynamic_resolution_update(&steady, submit, 12000000u, 0.75f,
                                          &next_scale));
  assert(steady.transition_count == 0u);

  const VkrDynamicResolutionConfig endpoint_config = {
      .min_scale = 0.334f,
      .max_scale = 1.0f,
      .target_frame_ms = 1000.0f / 75.0f,
      .enabled = true_v,
  };
  VkrDynamicResolutionState endpoint = {0};
  vkr_dynamic_resolution_init(&endpoint, &endpoint_config, 0.35f);
  assert(!vkr_dynamic_resolution_update(&endpoint, 1u, 20000000u, 0.35f,
                                        &next_scale));
  assert(vkr_dynamic_resolution_update(&endpoint, 2u, 20000000u, 0.35f,
                                       &next_scale));
  assert(temporal_near(next_scale, 0.334f));
  printf("  test_dynamic_resolution_hysteresis PASSED\n");
}

/* The controller steps down after two raw samples over the frame target, two
   steps when both exceed it by a quarter, and steps up after thirty samples
   of headroom. */
vkr_internal void test_dynamic_resolution_responsive(void) {
  printf("  Running test_dynamic_resolution_responsive...\n");
  const VkrDynamicResolutionConfig config = {
      .min_scale = 0.65f,
      .max_scale = 1.0f,
      .target_frame_ms = 16.0f,
      .enabled = true_v,
  };
  VkrDynamicResolutionState state = {0};
  vkr_dynamic_resolution_init(&state, &config, 1.0f);
  float32_t next_scale = 0.0f;
  uint64_t submit = 0u;
  // One spike alone does not step down; a second sample over budget does.
  assert(!vkr_dynamic_resolution_update(&state, ++submit, 17000000u, 1.0f,
                                        &next_scale));
  assert(!vkr_dynamic_resolution_update(&state, ++submit, 15000000u, 1.0f,
                                        &next_scale));
  assert(!vkr_dynamic_resolution_update(&state, ++submit, 17000000u, 1.0f,
                                        &next_scale));
  assert(vkr_dynamic_resolution_update(&state, ++submit, 17000000u, 1.0f,
                                       &next_scale));
  assert(temporal_near(next_scale, 0.95f));

  // In-flight samples of the old tier are ignored; one cooldown sample, then
  // two samples a quarter over budget step down twice.
  assert(!vkr_dynamic_resolution_update(&state, ++submit, 21000000u, 1.0f,
                                        &next_scale));
  assert(!vkr_dynamic_resolution_update(&state, ++submit, 21000000u, 0.95f,
                                        &next_scale));
  assert(!vkr_dynamic_resolution_update(&state, ++submit, 21000000u, 0.95f,
                                        &next_scale));
  assert(vkr_dynamic_resolution_update(&state, ++submit, 21000000u, 0.95f,
                                       &next_scale));
  assert(temporal_near(next_scale, 0.85f));
  assert(state.transition_count == 2u);

  // Thirty samples below 80% of the target step back up one tier.
  assert(!vkr_dynamic_resolution_update(&state, ++submit, 10000000u, 0.85f,
                                        &next_scale));
  for (uint32_t i = 0u; i < 29u; ++i)
    assert(!vkr_dynamic_resolution_update(&state, ++submit, 10000000u, 0.85f,
                                          &next_scale));
  assert(vkr_dynamic_resolution_update(&state, ++submit, 10000000u, 0.85f,
                                       &next_scale));
  assert(temporal_near(next_scale, 0.9f));
  assert(state.transition_count == 3u);
  printf("  test_dynamic_resolution_responsive PASSED\n");
}

vkr_internal void test_dynamic_resolution_failed_upshift_headroom(void) {
  printf("  Running test_dynamic_resolution_failed_upshift_headroom...\n");
  const VkrDynamicResolutionConfig config = {
      .min_scale = 0.85f,
      .max_scale = 0.9f,
      .target_frame_ms = 1000.0f / 75.0f,
      .enabled = true_v,
  };
  VkrDynamicResolutionState state = {0};
  vkr_dynamic_resolution_init(&state, &config, 0.85f);
  uint64_t submit = 0u;
  float32_t next_scale = 0.0f;

  // A cheap lower tier earns one probe; the upper tier misses the frame target.
  for (uint32_t i = 0u; i < 30u; ++i)
    vkr_dynamic_resolution_update(&state, ++submit, 9000000u,
                                  state.current_scale, &next_scale);
  assert(temporal_near(state.current_scale, 0.9f));
  assert(state.transition_count == 1u);
  for (uint32_t i = 0u; i < 3u; ++i)
    vkr_dynamic_resolution_update(&state, ++submit, 15000000u,
                                  state.current_scale, &next_scale);
  assert(temporal_near(state.current_scale, 0.85f));
  assert(state.transition_count == 2u);
  assert(state.failed_upshift_cost_ratio > 0.0);

  VkrDynamicResolutionState resized = state;
  const uint64_t resized_submit = submit;
  assert(resized.sample_frame_ns > 0.0 && resized.cooldown_samples > 0u);

  // Sustained identical work must not retry the known failing upper tier.
  for (uint32_t i = 0u; i < 600u; ++i) {
    const bool8_t changed = vkr_dynamic_resolution_update(
        &state, ++submit, 9000000u, state.current_scale, &next_scale);
    assert(!changed);
  }
  assert(temporal_near(state.current_scale, 0.85f));
  assert(state.transition_count == 2u);

  vkr_dynamic_resolution_reset_feedback(&resized);
  assert(resized.last_submit_value == resized_submit);
  assert(resized.transition_count == 2u);
  assert(resized.enabled == state.enabled);
  assert(resized.target_frame_ns == state.target_frame_ns);
  assert(resized.current_scale == state.current_scale);
  assert(resized.min_scale == state.min_scale);
  assert(resized.max_scale == state.max_scale);
  assert(resized.sample_frame_ns == 0.0);
  assert(resized.failed_upshift_cost_ratio == 0.0);
  assert(resized.cooldown_samples == 0u);
  assert(resized.over_budget_samples == 0u);
  assert(resized.under_budget_samples == 0u);
  // The submit watermark survives the reset.
  assert(!vkr_dynamic_resolution_update(&resized, resized_submit, 9000000u,
                                        resized.current_scale, &next_scale));
  assert(resized.sample_frame_ns == 0.0);
  for (uint64_t i = 1u; i <= 30u; ++i)
    vkr_dynamic_resolution_update(&resized, resized_submit + i, 9000000u,
                                  resized.current_scale, &next_scale);
  assert(temporal_near(resized.current_scale, 0.9f));
  assert(resized.transition_count == 3u);

  // A one-third cheaper lower tier predicts 10 ms upstairs, below headroom.
  for (uint32_t i = 0u; i < 100u; ++i) {
    const uint64_t cost =
        temporal_near(state.current_scale, 0.85f) ? 6000000u : 10000000u;
    vkr_dynamic_resolution_update(&state, ++submit, cost, state.current_scale,
                                  &next_scale);
  }
  assert(temporal_near(state.current_scale, 0.9f));
  assert(state.transition_count == 3u);
  for (uint32_t i = 0u; i < 100u; ++i) {
    const bool8_t changed = vkr_dynamic_resolution_update(
        &state, ++submit, 10000000u, state.current_scale, &next_scale);
    assert(!changed);
  }
  assert(state.failed_upshift_cost_ratio == 0.0);
  printf("  test_dynamic_resolution_failed_upshift_headroom PASSED\n");
}

/* Volumetric fog history follows only static changes inside its range and
   publications that can change drawn content: a streamed terrain tile's
   upload or a change 500 m away keeps it, one inside the range resets it. */
vkr_internal void test_froxel_fog_history_scope(void) {
  printf("  Running test_froxel_fog_history_scope...\n");
  VkrStaticChange change = {
      .generation = 5u,
      .min = {500.0f, 0.0f, 500.0f},
      .max = {564.0f, 50.0f, 564.0f},
      .bounded = true_v,
  };
  VkrWorldPassPayload world = {
      .static_generation = 5u,
      .static_changes = &change,
      .static_change_count = 1u,
      .static_change_floor = 4u,
      .publication_generation = 30u,
      .caster_publication_generation = 9u,
  };
  VkrFrameInput input = {.world = &world};
  input.globals.view_position = vec3_zero();
  input.globals.froxel_fog.max_distance = 200.0f;
  uint64_t seen = 4u;
  uint64_t followed = 4u;
  vkr_froxel_fog_static_generation(&input, &seen, &followed);
  assert(seen == 5u && followed == 4u);

  const VkrFroxelFogGpuParams params = {
      .grid_dimensions_cell_pixels = {160u, 90u, 64u, 8u}};
  const uint64_t signature =
      vkr_froxel_fog_content_signature(&input, &params, followed);
  world.publication_generation++;
  assert(vkr_froxel_fog_content_signature(&input, &params, followed) ==
         signature);
  world.caster_publication_generation++;
  assert(vkr_froxel_fog_content_signature(&input, &params, followed) !=
         signature);

  change = (VkrStaticChange){
      .generation = 6u,
      .min = {-10.0f, 0.0f, -10.0f},
      .max = {10.0f, 5.0f, 10.0f},
      .bounded = true_v,
  };
  world.static_generation = 6u;
  world.static_change_floor = 5u;
  vkr_froxel_fog_static_generation(&input, &seen, &followed);
  assert(seen == 6u && followed == 6u);
  change.generation = 7u;
  change.min = vec3_new(500.0f, 0.0f, 500.0f);
  change.max = vec3_new(564.0f, 50.0f, 564.0f);
  change.bounded = false_v;
  world.static_generation = 7u;
  world.static_change_floor = 6u;
  vkr_froxel_fog_static_generation(&input, &seen, &followed);
  assert(followed == 7u);
  world.static_generation = 8u;
  world.static_changes = NULL;
  vkr_froxel_fog_static_generation(&input, &seen, &followed);
  assert(followed == 8u);
  printf("  test_froxel_fog_history_scope PASSED\n");
}

/* Volumetric fog history survives what only resamples the same shadows: a
   cascade refit, a local shadow face moving in the atlas or drawing at
   another size, and the selected lights trading places in the frame's
   light array. The sun's size or a light moving resets it. */
vkr_internal void test_froxel_fog_history_survives_refits(void) {
  printf("  Running test_froxel_fog_history_survives_refits...\n");
  static VkrLocalShadowPassPayload local;
  MemZero(&local, sizeof(local));
  VkrPointLight lights[2] = {
      {.position = {0.0f, 1.0f, -10.0f},
       .color = {1.0f, 1.0f, 1.0f},
       .intensity = 10.0f,
       .range = 30.0f,
       .direction = {0.0f, -1.0f, 0.0f},
       .inner_cone_angle = 0.3f,
       .outer_cone_angle = 0.5f,
       .kind = VKR_POINT_LIGHT_KIND_GLTF_SPOT,
       .render_id = 7u},
      {.position = {3.0f, 1.0f, -20.0f},
       .color = {1.0f, 0.5f, 0.2f},
       .intensity = 20.0f,
       .range = 30.0f,
       .direction = {0.0f, -1.0f, 0.0f},
       .inner_cone_angle = 0.3f,
       .outer_cone_angle = 0.5f,
       .kind = VKR_POINT_LIGHT_KIND_GLTF_SPOT,
       .render_id = 3u},
  };
  local.view_count = 2u;
  for (uint32_t i = 0; i < 2u; ++i) {
    local.light_first_view[i] = i + 1u;
    local.views[i] = (VkrLocalShadowView){
        .light_view_projection = mat4_translate(lights[i].position),
        .light_position_near = {lights[i].position.x, lights[i].position.y,
                                lights[i].position.z, 0.05f},
        .light_direction_far = {0.0f, -1.0f, 0.0f, 30.0f},
        .projection_params = {0.5f, 1.0f / 512.0f, 1.0f, 1.0f},
        .shadow_params = {1.0f, 0.0f, 0.0f, 0.0f},
        .atlas_rect = {0.25f * (float32_t)i, 0.0f, 0.25f, 0.0f},
    };
  }
  VkrFrameLighting lighting = {.point_lights = lights, .point_light_count = 2u};
  VkrShadowPassPayload shadow = {.cascade_count = 2u};
  for (uint32_t i = 0; i < 2u; ++i) {
    shadow.cascades[i] = (VkrShadowCascadePacketData){
        .light_view_projection = mat4_identity(),
        .split_near_far_texel_depth = {0.1f, 20.0f, 0.05f, 100.0f},
        .origin_inv_size_sun = {1.0f, 2.0f, 1.0f / 2048.0f, 0.0046f},
    };
  }
  VkrFrameInput input = {
      .lighting = &lighting, .shadow = &shadow, .local_shadow = &local};
  input.globals.view = mat4_identity();
  input.globals.projection = mat4_perspective_reversed(
      vkr_to_radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
  input.globals.froxel_fog = vkr_froxel_fog_settings_defaults();
  input.globals.froxel_fog.enabled = true_v;
  input.globals.froxel_fog.max_distance = 100.0f;
  VkrFroxelFogGpuParams params = vkr_froxel_fog_prepare(&input, 1280u, 720u);
  assert(params.selected_local_indices_count[2] == 2u);
  const uint64_t signature =
      vkr_froxel_fog_content_signature(&input, &params, 0u);

  for (uint32_t i = 0; i < 2u; ++i) {
    shadow.cascades[i].light_view_projection =
        mat4_translate(vec3_new(0.5f, 0.0f, 0.25f));
    shadow.cascades[i].split_near_far_texel_depth.y = 25.0f;
    shadow.cascades[i].origin_inv_size_sun.x = 7.0f;
    local.views[i].atlas_rect.x += 0.5f;
    local.views[i].projection_params.y = 1.0f / 256.0f;
    local.views[i].shadow_params.z = 1.0f;
  }
  assert(vkr_froxel_fog_content_signature(&input, &params, 0u) == signature);

  const VkrPointLight first = lights[0];
  lights[0] = lights[1];
  lights[1] = first;
  const VkrLocalShadowView first_view = local.views[0];
  local.views[0] = local.views[1];
  local.views[1] = first_view;
  params = vkr_froxel_fog_prepare(&input, 1280u, 720u);
  assert(vkr_froxel_fog_content_signature(&input, &params, 0u) == signature);

  shadow.cascades[1].origin_inv_size_sun.w = 0.01f;
  assert(vkr_froxel_fog_content_signature(&input, &params, 0u) != signature);
  shadow.cascades[1].origin_inv_size_sun.w = 0.0046f;
  assert(vkr_froxel_fog_content_signature(&input, &params, 0u) == signature);
  lights[0].position.x += 1.0f;
  assert(vkr_froxel_fog_content_signature(&input, &params, 0u) != signature);
  printf("  test_froxel_fog_history_survives_refits PASSED\n");
}

bool32_t run_temporal_tests(void) {
  printf("Running temporal tests...\n");
  test_temporal_jitter_and_commit();
  test_temporal_projection_pixel_shift();
  test_froxel_fog_history_scope();
  test_froxel_fog_history_survives_refits();
  test_temporal_reset_reasons();
  test_temporal_sequence_repeats();
  test_temporal_halton_jitter();
  test_temporal_reprojection();
  test_temporal_rotation_cut();
  test_temporal_sky_reprojection();
  test_temporal_orthographic_sky_reprojection();
  test_dynamic_resolution_config();
  test_dynamic_resolution_hysteresis();
  test_dynamic_resolution_failed_upshift_headroom();
  test_dynamic_resolution_responsive();
  printf("Temporal tests PASSED\n");
  return true_v;
}
