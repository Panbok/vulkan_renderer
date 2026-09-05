#include "temporal_test.h"

#include "renderer/vkr_dynamic_resolution.h"
#include "renderer/vkr_render_packet.h"
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

static void test_temporal_projection_pixel_shift(void) {
  printf("  Running test_temporal_projection_pixel_shift...\n");
  Mat4 projections[] = {
      mat4_perspective(vkr_to_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f),
      mat4_perspective(vkr_to_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f),
      mat4_ortho_zo_yinv(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 100.0f),
      mat4_ortho_zo_yinv(-3.0f, 5.0f, -2.0f, 4.0f, 0.1f, 100.0f),
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
              ((float64_t)after.x / after.w -
               (float64_t)before.x / before.w) * 0.5 * input.width;
          const float64_t dy =
              ((float64_t)after.y / after.w -
               (float64_t)before.y / before.w) * viewport_y * input.height;
          assert(fabs(dx - frame.jitter_pixels.x) < 1e-4);
          assert(fabs(dy - frame.jitter_pixels.y) < 1e-4);
          assert(before.z == after.z && before.w == after.w);
        }
      }
    }
  }
  printf("  test_temporal_projection_pixel_shift PASSED\n");
}

static bool32_t temporal_signature_equal(VkrTemporalSceneSignature a,
                                         VkrTemporalSceneSignature b) {
  return a.eligible && b.eligible && a.hash[0] == b.hash[0] &&
         a.hash[1] == b.hash[1];
}

static void test_temporal_scene_signature(void) {
  printf("  Running test_temporal_scene_signature...\n");
  VkrWorldDrawCandidate candidate = {
      .geometry = {1u, 1u}, .material = {2u, 1u},
      .instance = {.model = mat4_identity()},
  };
  VkrWorldDrawCandidate transmission = candidate;
  VkrInstanceDataGPU instance = {.model = mat4_identity()};
  VkrDrawItem draw = {
      .geometry = {3u, 1u}, .material = {4u, 1u}, .instance_count = 1u,
  };
  VkrWorldPassPayload world = {
      .gpu_candidates = &candidate, .gpu_candidate_count = 1u,
      .transmission_gpu_candidates = &transmission,
      .transmission_gpu_candidate_count = 1u,
      .transparent_draws = &draw, .transparent_draw_count = 1u,
      .instances = &instance, .instance_count = 1u,
  };
  VkrPointLight light = {.color = {1.0f, 0.8f, 0.5f}, .intensity = 2.0f};
  VkrPointLightGrid grid = {.cell_size = 4.0f, .dimensions = {1u, 1u, 1u},
                             .cell_count = 1u, .masks = {{{1u, 0u, 0u, 0u}}}};
  VkrFrameIblProbe probe = {.prefilter = {5u, 1u}, .weight = 1.0f};
  VkrFrameLighting lighting = {
      .point_lights = &light, .point_light_count = 1u,
      .point_light_grid = &grid, .ibl_probes = &probe, .ibl_probe_count = 1u,
  };
  VkrShadowPassPayload shadow = {.cascade_count = 1u};
  shadow.cascades[0].light_view_projection = mat4_identity();
  VkrSkyboxPassPayload sky = {.cubemap = {6u, 1u}};
  VkrRenderPacket packet = {
      .globals = {.view = mat4_identity(), .projection = mat4_identity(),
                  .gtao = {.enabled = true_v, .radius = 0.5f, .power = 2.2f}},
      .world = &world, .lighting = &lighting, .shadow = &shadow, .skybox = &sky,
  };
  const VkrTemporalSceneSignature baseline = vkr_temporal_scene_signature(&packet);
  assert(baseline.eligible);

  // Each mutation changes scene radiance or its spatial/temporal identity.
#define EXPECT_SCENE_CHANGE(field, value)                                     \
  do {                                                                       \
    field = value;                                                           \
    assert(!temporal_signature_equal(baseline,                                \
                                      vkr_temporal_scene_signature(&packet))); \
  } while (0)
  EXPECT_SCENE_CHANGE(candidate.instance.model.m03, 1.0f);
  candidate.instance.model.m03 = 0.0f;
  EXPECT_SCENE_CHANGE(transmission.instance.model.m13, 1.0f);
  transmission.instance.model.m13 = 0.0f;
  EXPECT_SCENE_CHANGE(instance.model.m23, 1.0f);
  instance.model.m23 = 0.0f;
  EXPECT_SCENE_CHANGE(draw.material.generation, 2u);
  draw.material.generation = 1u;
  EXPECT_SCENE_CHANGE(light.color.x, 0.25f);
  light.color.x = 1.0f;
  EXPECT_SCENE_CHANGE(grid.masks[0].words[0], 0u);
  grid.masks[0].words[0] = 1u;
  EXPECT_SCENE_CHANGE(probe.weight, 0.5f);
  probe.weight = 1.0f;
  EXPECT_SCENE_CHANGE(shadow.receiver.pcf_radius_texels, 2.0f);
  shadow.receiver.pcf_radius_texels = 0.0f;
  EXPECT_SCENE_CHANGE(shadow.cascades[0].light_view_projection.m03, 2.0f);
  shadow.cascades[0].light_view_projection.m03 = 0.0f;
  EXPECT_SCENE_CHANGE(sky.cubemap.generation, 2u);
  sky.cubemap.generation = 1u;
  EXPECT_SCENE_CHANGE(packet.globals.view_position.x, 1.0f);
  packet.globals.view_position.x = 0.0f;
  EXPECT_SCENE_CHANGE(packet.globals.gtao.radius, 1.0f);
  packet.globals.gtao.radius = 0.5f;
#undef EXPECT_SCENE_CHANGE
  assert(temporal_signature_equal(baseline, vkr_temporal_scene_signature(&packet)));

  // Relocating borrowed arrays preserves content; addresses are not identity.
  const VkrWorldDrawCandidate relocated_candidate = candidate;
  const VkrPointLight relocated_light = light;
  world.gpu_candidates = &relocated_candidate;
  lighting.point_lights = &relocated_light;
  assert(temporal_signature_equal(baseline, vkr_temporal_scene_signature(&packet)));
  // Frame phase drives GTAO noise as well as jitter. Post-temporal controls
  // and normalized-away authoring values cannot reset the scene proof.
  packet.frame.frame_index = 37u;
  packet.frame.delta_time = 0.125;
  packet.globals.temporal.jitter_pixels = (Vec2){0.25f, -0.4f};
  packet.globals.temporal.jittered_projection.m02 = 0.4f;
  packet.globals.exposure.manual = 16.0f;
  packet.globals.manual_exposure = 16.0f;
  packet.globals.bloom_intensity = 2.0f;
  packet.globals.gtao_radius = 7.0f;
  VkrUiPassPayload ui = {0};
  packet.ui = &ui;
  assert(temporal_signature_equal(baseline, vkr_temporal_scene_signature(&packet)));
  world.publication_pending = true_v;
  assert(!vkr_temporal_scene_signature(&packet).eligible);
  world.publication_pending = false_v;
  world.text_draw_count = 1u;
  assert(!vkr_temporal_scene_signature(&packet).eligible);
  printf("  test_temporal_scene_signature PASSED\n");
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

static void test_temporal_sky_reprojection(void) {
  printf("  Running test_temporal_sky_reprojection...\n");
  const Vec3 current_eye = {25.0f, -9.0f, 8.0f};
  const Mat4 current_view = mat4_translate((Vec3){-25.0f, 9.0f, -8.0f});
  const Mat4 previous_translation =
      mat4_translate((Vec3){7.0f, -3.0f, -12.0f});
  const float32_t sine = 1.0f / sqrtf(5.0f);
  const float32_t cosine = 2.0f / sqrtf(5.0f);
  for (uint32_t native_clip = 0u; native_clip < 2u; ++native_clip) {
    Mat4 projection =
        mat4_perspective(vkr_to_radians(90.0f), 1.0f, 0.1f, 1000.0f);
    if (native_clip != 0u)
      projection.m11 = -projection.m11;
    const Mat4 current = mat4_mul(projection, current_view);
    Mat4 reprojection = vkr_temporal_sky_reprojection(
        current, mat4_mul(projection, previous_translation), current_eye);
    const Vec4 off_center = {0.25f, -0.375f, 1.0f, 1.0f};
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
        current,
        mat4_mul(projection, mat4_mul(yaw, previous_translation)), current_eye);
    previous_clip =
        mat4_mul_vec4(reprojection, (Vec4){0.0f, 0.0f, 1.0f, 1.0f});
    /* The center ray rotates to (-sin(a), 0, -cos(a)); tan(a) = 1/2. */
    assert(fabsf(previous_clip.x / previous_clip.w + 0.5f) < 1e-5f);
    assert(fabsf(previous_clip.y / previous_clip.w) < 1e-5f);

    Mat4 pitch = mat4_identity();
    pitch.m11 = cosine;
    pitch.m12 = -sine;
    pitch.m21 = sine;
    pitch.m22 = cosine;
    reprojection = vkr_temporal_sky_reprojection(
        current,
        mat4_mul(projection, mat4_mul(pitch, previous_translation)),
        current_eye);
    previous_clip =
        mat4_mul_vec4(reprojection, (Vec4){0.0f, 0.0f, 1.0f, 1.0f});
    assert(fabsf(previous_clip.x / previous_clip.w) < 1e-5f);
    assert(fabsf(previous_clip.y / previous_clip.w -
                 (native_clip != 0u ? 0.5f : -0.5f)) < 1e-5f);
  }
  printf("  test_temporal_sky_reprojection PASSED\n");
}

static void test_temporal_orthographic_sky_reprojection(void) {
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
  const Vec4 pixels[] = {{0.0f, 0.0f, 1.0f, 1.0f},
                         {0.25f, -0.375f, 1.0f, 1.0f}};
  for (uint32_t native_clip = 0u; native_clip < 2u; ++native_clip) {
    for (uint32_t shifted = 0u; shifted < 2u; ++shifted) {
      Mat4 projection = shifted != 0u
                            ? mat4_ortho_zo_yinv(-15.0f, 25.0f, -10.0f, 30.0f,
                                                 0.1f, 20.0f)
                            : mat4_ortho_zo_yinv(-20.0f, 20.0f, -20.0f, 20.0f,
                                                 0.1f, 20.0f);
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
          const Vec4 current_far = mat4_mul_vec4(inverse_current, pixels[pixel]);
          const Vec4 previous_far = mat4_mul_vec4(
              inverse_previous, vec4_new(previous_clip.x, previous_clip.y,
                                          1.0f, 1.0f));
          const Vec3 current_direction = vec3_normalize(vec3_sub(
              vec3_new(current_far.x / current_far.w,
                         current_far.y / current_far.w,
                         current_far.z / current_far.w),
              current_eye));
          const Vec3 previous_direction = vec3_normalize(vec3_sub(
              vec3_new(previous_far.x / previous_far.w,
                         previous_far.y / previous_far.w,
                         previous_far.z / previous_far.w),
              previous_eye));
          assert(vec3_dot(current_direction, previous_direction) > 1.0f - 1e-5f);
        }
      }
    }
  }
  printf("  test_temporal_orthographic_sky_reprojection PASSED\n");
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
  test_temporal_projection_pixel_shift();
  test_temporal_scene_signature();
  test_temporal_reset_reasons();
  test_temporal_sequence_repeats();
  test_temporal_rotation_cut();
  test_temporal_sky_reprojection();
  test_temporal_orthographic_sky_reprojection();
  test_dynamic_resolution_config();
  test_dynamic_resolution_hysteresis();
  printf("Temporal tests PASSED\n");
  return true_v;
}
