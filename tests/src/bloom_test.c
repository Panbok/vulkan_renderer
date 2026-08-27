#include "bloom_test.h"

#include "renderer/renderer_frontend.h"
#include "renderer/vkr_bloom.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool8_t bloom_near(float32_t a, float32_t b) {
  return fabsf(a - b) <= 1e-5f;
}
static void bloom_assert_config_equal(const VkrBloomConfig *a,
                                      const VkrBloomConfig *b) {
  assert(a->max_mip_count == b->max_mip_count);
  assert(a->min_mip_extent == b->min_mip_extent);
  assert(a->firefly_clamp == b->firefly_clamp);
  assert(a->filter == b->filter);
}

static void test_bloom_config_and_mips(void) {
  printf("  Running test_bloom_config_and_mips...\n");
  const VkrBloomConfig defaults = vkr_bloom_config_default();
  const VkrBloomConfig zeroed = {0};
  const VkrBloomConfig from_null = vkr_bloom_config_normalize(NULL);
  const VkrBloomConfig from_zeroed = vkr_bloom_config_normalize(&zeroed);
  bloom_assert_config_equal(&defaults, &from_null);
  bloom_assert_config_equal(&defaults, &from_zeroed);

  const VkrBloomConfig hostile = {
      .max_mip_count = UINT32_MAX,
      .min_mip_extent = 0u,
      .firefly_clamp = INFINITY,
      .filter = VKR_BLOOM_FILTER_COUNT,
  };
  const VkrBloomConfig normalized = vkr_bloom_config_normalize(&hostile);
  assert(normalized.max_mip_count == VKR_BLOOM_MAX_MIP_COUNT);
  assert(normalized.min_mip_extent == 1u);
  assert(normalized.firefly_clamp == defaults.firefly_clamp);
  assert(normalized.filter == defaults.filter);

  assert(vkr_bloom_mip_count(&defaults, 801u, 601u) == 6u);
  uint32_t width = 0u, height = 0u;
  vkr_bloom_mip_extent(801u, 601u, 0u, &width, &height);
  assert(width == 400u && height == 300u);
  vkr_bloom_mip_extent(801u, 601u, 5u, &width, &height);
  assert(width == 12u && height == 9u);
  assert(vkr_bloom_mip_count(&defaults, 16u, 16u) == 0u);
  assert(vkr_bloom_mip_count(&defaults, 32u, 32u) == 2u);
  printf("  test_bloom_config_and_mips PASSED\n");
}

static void test_bloom_prefilter_reference(void) {
  printf("  Running test_bloom_prefilter_reference...\n");
  const VkrBloomConfig config = vkr_bloom_config_default();
  const VkrBloomFrame frame = vkr_bloom_prepare(true_v, 1.0f, 0.5f, 0.05f);
  const VkrBloomGpuParams params = vkr_bloom_gpu_params(&config, &frame);
  assert(params.threshold == 1.0f && params.knee == 0.5f);
  assert(params.knee_denominator > 2.0f);
  assert(params.firefly_clamp == 32.0f && params.intensity == 0.05f);

  const float32_t invalid[3] = {NAN, INFINITY, -1.0f};
  float32_t sanitized[3] = {0};
  vkr_bloom_sanitize(&params, invalid, sanitized);
  assert(sanitized[0] == 0.0f && sanitized[1] == 32.0f && sanitized[2] == 0.0f);

  const float32_t below_knee[3] = {0.5f, 0.25f, 0.0f};
  float32_t thresholded[3] = {1.0f, 1.0f, 1.0f};
  vkr_bloom_soft_threshold(&params, below_knee, thresholded);
  assert(thresholded[0] == 0.0f && thresholded[1] == 0.0f &&
         thresholded[2] == 0.0f);

  const float32_t above_knee[3] = {1.5f, 0.75f, 0.0f};
  vkr_bloom_soft_threshold(&params, above_knee, thresholded);
  assert(bloom_near(thresholded[0], 0.5f));
  assert(bloom_near(thresholded[1], 0.25f));
  assert(thresholded[2] == 0.0f);

  const float32_t white[3] = {1.0f, 1.0f, 1.0f};
  assert(bloom_near(vkr_bloom_karis_weight(white), 0.5f));

  const VkrBloomFrame disabled = vkr_bloom_prepare(false_v, NAN, NAN, NAN);
  assert(!disabled.enabled);
  assert(vkr_bloom_gpu_params(&config, &disabled).intensity == 0.0f);
  printf("  test_bloom_prefilter_reference PASSED\n");
}

static void test_bloom_packet_validation(void) {
  printf("  Running test_bloom_packet_validation...\n");
  VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
  };
  VkrValidationError validation = {0};
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  packet.globals.bloom_enabled = 2u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.bloom_enabled") == 0);

  packet.globals.bloom_enabled = true_v;
  packet.globals.bloom_threshold = VKR_BLOOM_DEFAULT_THRESHOLD;
  packet.globals.bloom_knee = VKR_BLOOM_DEFAULT_KNEE;
  packet.globals.bloom_intensity = VKR_BLOOM_DEFAULT_INTENSITY;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  packet.globals.bloom_threshold = NAN;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.bloom_threshold") == 0);
  packet.globals.bloom_threshold = VKR_BLOOM_DEFAULT_THRESHOLD;

  packet.globals.bloom_knee = -1.0f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.bloom_knee") == 0);
  packet.globals.bloom_knee = VKR_BLOOM_DEFAULT_KNEE;

  packet.globals.bloom_intensity = INFINITY;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.bloom_intensity") == 0);
  packet.globals.bloom_intensity = VKR_BLOOM_DEFAULT_INTENSITY;

  packet.packet_version = VKR_RENDER_PACKET_VERSION - 1u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_INCOMPATIBLE_SIGNATURE);
  printf("  test_bloom_packet_validation PASSED\n");
}

bool32_t run_bloom_tests(void) {
  printf("--- Running bloom tests... ---\n");
  test_bloom_config_and_mips();
  test_bloom_prefilter_reference();
  test_bloom_packet_validation();
  printf("Bloom tests PASSED\n");
  return true;
}
