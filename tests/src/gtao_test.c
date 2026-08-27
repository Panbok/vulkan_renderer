#include "gtao_test.h"

#include "renderer/renderer_frontend.h"
#include "renderer/vkr_gtao.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool8_t gtao_near(float32_t a, float32_t b) {
  const float32_t scale = fmaxf(1.0f, fabsf(b));
  return fabsf(a - b) <= 1e-4f * scale;
}

static void gtao_assert_projection_roundtrip(Mat4 projection,
                                             Vec3 view_position) {
  const VkrGtaoConfig config = vkr_gtao_config_default();
  const VkrGtaoFrame frame =
      vkr_gtao_prepare(true_v, VKR_GTAO_DEFAULT_RADIUS, VKR_GTAO_DEFAULT_POWER);
  const VkrGtaoGpuParams params = vkr_gtao_gpu_params(
      &config, &frame, mat4_identity(), projection, 1920u, 1080u, 0u, false_v);
  const Vec4 clip =
      mat4_mul_vec4(projection, vec4_new(view_position.x, view_position.y,
                                         view_position.z, 1.0f));
  const float32_t inverse_w = 1.0f / clip.w;
  const float32_t screen_x = (clip.x * inverse_w + 1.0f) * 0.5f;
  const float32_t screen_y = (clip.y * inverse_w + 1.0f) * 0.5f;
  const float32_t device_depth = clip.z * inverse_w;
  const float32_t view_depth = vkr_gtao_linearize_depth(&params, device_depth);
  const Vec3 reconstructed =
      vkr_gtao_view_position(&params, screen_x, screen_y, view_depth);
  assert(gtao_near(reconstructed.x, view_position.x));
  assert(gtao_near(reconstructed.y, view_position.y));
  assert(gtao_near(reconstructed.z, view_position.z));
}

static void gtao_assert_config_equal(const VkrGtaoConfig *a,
                                     const VkrGtaoConfig *b) {
  assert(a->max_depth_mip_count == b->max_depth_mip_count);
  assert(a->slice_count == b->slice_count);
  assert(a->steps_per_slice == b->steps_per_slice);
  assert(a->radius_multiplier == b->radius_multiplier);
  assert(a->falloff_range == b->falloff_range);
  assert(a->sample_distribution_power == b->sample_distribution_power);
  assert(a->depth_mip_sampling_offset == b->depth_mip_sampling_offset);
  assert(a->denoise_blur_beta == b->denoise_blur_beta);
}

static void test_gtao_config_and_mips(void) {
  printf("  Running test_gtao_config_and_mips...\n");
  const VkrGtaoConfig defaults = vkr_gtao_config_default();
  assert(defaults.max_depth_mip_count == 5u);
  assert(defaults.slice_count == 3u);
  assert(defaults.steps_per_slice == 3u);
  assert(defaults.radius_multiplier == 1.457f);
  assert(defaults.falloff_range == 0.615f);
  assert(defaults.sample_distribution_power == 2.0f);
  assert(defaults.depth_mip_sampling_offset == 3.30f);
  assert(defaults.denoise_blur_beta == 1.2f);

  const VkrGtaoConfig zeroed = {0};
  const VkrGtaoConfig from_null = vkr_gtao_config_normalize(NULL);
  const VkrGtaoConfig from_zeroed = vkr_gtao_config_normalize(&zeroed);
  gtao_assert_config_equal(&defaults, &from_null);
  gtao_assert_config_equal(&defaults, &from_zeroed);

  const VkrGtaoConfig hostile = {
      .max_depth_mip_count = UINT32_MAX,
      .slice_count = 0u,
      .steps_per_slice = UINT32_MAX,
      .radius_multiplier = INFINITY,
      .falloff_range = -INFINITY,
      .sample_distribution_power = -1.0f,
      .depth_mip_sampling_offset = -2.0f,
      .denoise_blur_beta = INFINITY,
  };
  const VkrGtaoConfig normalized = vkr_gtao_config_normalize(&hostile);
  assert(normalized.max_depth_mip_count == VKR_GTAO_MAX_DEPTH_MIP_COUNT);
  assert(normalized.slice_count == 1u);
  assert(normalized.steps_per_slice == 3u);
  assert(normalized.radius_multiplier == defaults.radius_multiplier);
  assert(normalized.falloff_range == defaults.falloff_range);
  assert(normalized.sample_distribution_power == 1.0f);
  assert(normalized.depth_mip_sampling_offset == 0.0f);
  assert(normalized.denoise_blur_beta == defaults.denoise_blur_beta);

  assert(vkr_gtao_depth_mip_count(&defaults, 801u, 601u) == 5u);
  assert(vkr_gtao_depth_mip_count(&defaults, 3u, 5u) == 3u);
  assert(vkr_gtao_depth_mip_count(&defaults, 1u, 1u) == 1u);
  assert(vkr_gtao_depth_mip_count(&defaults, 0u, 1u) == 0u);
  printf("  test_gtao_config_and_mips PASSED\n");
}

static void test_gtao_frame_controls(void) {
  printf("  Running test_gtao_frame_controls...\n");
  const VkrGtaoFrame disabled = vkr_gtao_prepare(false_v, NAN, NAN);
  assert(!disabled.enabled);
  assert(disabled.radius == 0.0f);
  assert(disabled.power == 0.0f);

  const VkrGtaoFrame enabled = vkr_gtao_prepare(true_v, 0.75f, 1.8f);
  assert(enabled.enabled == true_v);
  assert(enabled.radius == 0.75f);
  assert(enabled.power == 1.8f);
  printf("  test_gtao_frame_controls PASSED\n");
}

static void test_gtao_depth_reference(void) {
  printf("  Running test_gtao_depth_reference...\n");
  const float32_t near_clip = 0.1f;
  const float32_t far_clip = 100.0f;
  const Mat4 projection = mat4_perspective(vkr_to_radians(60.0f), 16.0f / 9.0f,
                                           near_clip, far_clip);
  const VkrGtaoConfig config = vkr_gtao_config_default();
  const VkrGtaoFrame frame =
      vkr_gtao_prepare(true_v, VKR_GTAO_DEFAULT_RADIUS, VKR_GTAO_DEFAULT_POWER);
  const VkrGtaoGpuParams params = vkr_gtao_gpu_params(
      &config, &frame, mat4_identity(), projection, 1920u, 1080u, 0u, false_v);

  assert(gtao_near(vkr_gtao_linearize_depth(&params, 0.0f), near_clip));
  assert(gtao_near(vkr_gtao_linearize_depth(&params, 1.0f), far_clip));
  const Mat4 extreme_projection = mat4_perspective(
      vkr_to_radians(60.0f), 1.0f, near_clip, VKR_GTAO_VIEW_DEPTH_MAX * 2.0f);
  const VkrGtaoGpuParams extreme_params =
      vkr_gtao_gpu_params(&config, &frame, mat4_identity(), extreme_projection,
                          1u, 1u, 0u, false_v);
  assert(gtao_near(vkr_gtao_linearize_depth(&extreme_params, 1.0f),
                   VKR_GTAO_VIEW_DEPTH_MAX));

  const float32_t plane =
      vkr_gtao_depth_mip_filter(&params, 4.0f, 4.0f, 4.0f, 4.0f);
  assert(gtao_near(plane, 4.0f));

  const float32_t plain_mean = (1.0f + 10.0f + 10.0f + 10.0f) * 0.25f;
  const float32_t filtered =
      vkr_gtao_depth_mip_filter(&params, 1.0f, 10.0f, 10.0f, 10.0f);
  assert(filtered > plain_mean);
  assert(filtered <= 10.0f);
  printf("  test_gtao_depth_reference PASSED\n");
}

static void test_gtao_projection_roundtrip(void) {
  printf("  Running test_gtao_projection_roundtrip...\n");
  Mat4 jittered =
      mat4_perspective(vkr_to_radians(67.0f), 16.0f / 9.0f, 0.1f, 500.0f);
  jittered.m02 -= 0.037f;
  jittered.m12 += 0.021f;
  gtao_assert_projection_roundtrip(jittered, vec3_new(0.7f, -0.4f, -5.0f));

  const Mat4 orthographic =
      mat4_ortho_zo_yinv(-4.0f, 6.0f, -3.0f, 5.0f, 0.2f, 80.0f);
  gtao_assert_projection_roundtrip(orthographic,
                                   vec3_new(1.25f, -0.75f, -12.0f));
  printf("  test_gtao_projection_roundtrip PASSED\n");
}

static void test_gtao_gpu_params(void) {
  printf("  Running test_gtao_gpu_params...\n");
  assert(sizeof(VkrGtaoGpuParams) == 192u);
  assert((sizeof(VkrGtaoGpuParams) % 16u) == 0u);

  const VkrGtaoConfig config = vkr_gtao_config_default();
  const VkrGtaoFrame frame =
      vkr_gtao_prepare(true_v, VKR_GTAO_DEFAULT_RADIUS, VKR_GTAO_DEFAULT_POWER);
  const Mat4 projection =
      mat4_perspective(vkr_to_radians(60.0f), 1.0f, 0.1f, 100.0f);
  const VkrGtaoGpuParams temporal = vkr_gtao_gpu_params(
      &config, &frame, mat4_identity(), projection, 1279u, 719u, 130u, true_v);
  const VkrGtaoGpuParams static_noise = vkr_gtao_gpu_params(
      &config, &frame, mat4_identity(), projection, 1279u, 719u, 130u, false_v);

  assert(temporal.reserved_u32_0 == 0u);
  assert(temporal.depth_mip_count == 5u);
  assert(temporal.slice_count == 3u && temporal.steps_per_slice == 3u);
  assert(temporal.noise_index == 2u);
  assert(static_noise.noise_index == 0u);
  assert(temporal.effect_radius == VKR_GTAO_DEFAULT_RADIUS);
  assert(temporal.final_value_power == VKR_GTAO_DEFAULT_POWER);
  assert(temporal.falloff_mul < 0.0f);
  assert(temporal.falloff_add > 1.0f);
  const VkrGtaoFrame minimum_radius =
      vkr_gtao_prepare(true_v, VKR_GTAO_RADIUS_MIN, VKR_GTAO_DEFAULT_POWER);
  const VkrGtaoGpuParams minimum_params =
      vkr_gtao_gpu_params(&config, &minimum_radius, mat4_identity(), projection,
                          1279u, 719u, 0u, false_v);
  const VkrGtaoFrame maximum_radius =
      vkr_gtao_prepare(true_v, VKR_GTAO_RADIUS_MAX, VKR_GTAO_DEFAULT_POWER);
  const VkrGtaoGpuParams maximum_params =
      vkr_gtao_gpu_params(&config, &maximum_radius, mat4_identity(), projection,
                          1279u, 719u, 0u, false_v);
  assert(isfinite(minimum_params.falloff_mul));
  assert(isfinite(minimum_params.depth_mip_falloff_mul));
  assert(isfinite(maximum_params.falloff_mul));
  assert(isfinite(maximum_params.depth_mip_falloff_mul));
  printf("  test_gtao_gpu_params PASSED\n");
}
static void test_gtao_packet_validation(void) {
  printf("  Running test_gtao_packet_validation...\n");
  VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
  };
  VkrValidationError validation = {0};
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  packet.globals.gtao_enabled = 2u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.gtao_enabled") == 0);

  packet.globals.gtao_enabled = true_v;
  packet.globals.gtao_radius = VKR_GTAO_DEFAULT_RADIUS;
  packet.globals.gtao_power = VKR_GTAO_DEFAULT_POWER;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  packet.globals.gtao_radius = 0.0f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.gtao_radius") == 0);
  packet.globals.gtao_radius = nextafterf(0.0f, 1.0f);
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.gtao_radius") == 0);
  packet.globals.gtao_radius = VKR_GTAO_RADIUS_MAX + 1.0f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.gtao_radius") == 0);
  packet.globals.gtao_radius = VKR_GTAO_RADIUS_MIN;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);
  packet.globals.gtao_radius = VKR_GTAO_RADIUS_MAX;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);
  packet.globals.gtao_radius = VKR_GTAO_DEFAULT_RADIUS;

  packet.globals.gtao_power = INFINITY;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.gtao_power") == 0);
  printf("  test_gtao_packet_validation PASSED\n");
}

bool32_t run_gtao_tests(void) {
  printf("--- Running GTAO tests... ---\n");
  test_gtao_config_and_mips();
  test_gtao_frame_controls();
  test_gtao_depth_reference();
  test_gtao_projection_roundtrip();
  test_gtao_gpu_params();
  test_gtao_packet_validation();
  printf("GTAO tests PASSED\n");
  return true;
}
