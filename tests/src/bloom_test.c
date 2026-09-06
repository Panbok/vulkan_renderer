#include "bloom_test.h"

#include "renderer/vkr_bloom.h"
#include "renderer/vkr_renderer_internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

vkr_internal void bloom_assert_config_equal(const VkrBloomConfig *a,
                                            const VkrBloomConfig *b) {
  assert(a->max_mip_count == b->max_mip_count);
  assert(a->min_mip_extent == b->min_mip_extent);
  assert(a->firefly_clamp == b->firefly_clamp);
  assert(a->filter == b->filter);
}

vkr_internal void test_bloom_config_and_mips(void) {
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

vkr_internal void test_bloom_chain_gain(void) {
  printf("  Running test_bloom_chain_gain...\n");
  const VkrBloomConfig config = vkr_bloom_config_default();
  VkrBloomFrame frame = vkr_bloom_prepare(true_v, 1.0f, 0.5f, 0.05f);
  const struct {
    uint32_t width;
    uint32_t height;
    uint32_t mip_count;
  } cases[] = {{1u, 1u, 0u},       {16u, 17u, 0u},
               {33u, 35u, 2u},   {65u, 67u, 3u},
               {129u, 131u, 4u}, {257u, 259u, 5u},
               {801u, 601u, 6u}};
  for (uint32_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const uint32_t count =
        vkr_bloom_mip_count(&config, cases[i].width, cases[i].height);
    assert(count == cases[i].mip_count);
    const VkrBloomGpuParams params =
        vkr_bloom_gpu_params(&config, &frame, count);
    /* A unit constant survives every normalized reduction and tent filter.
       Summing its levels must reproduce the authored six-level response. */
    const float32_t combined = (float32_t)count * params.intensity;
    assert(fabsf(combined - (count ? 0.3f : 0.0f)) < 1e-6f);
    if (count == 6u)
      assert(params.intensity == frame.intensity);
  }
  VkrBloomConfig shorter = config;
  shorter.max_mip_count = 4u;
  const VkrBloomGpuParams short_params =
      vkr_bloom_gpu_params(&shorter, &frame, 2u);
  assert(fabsf(2.0f * short_params.intensity - 0.2f) < 1e-6f);
  frame.enabled = false_v;
  assert(vkr_bloom_gpu_params(&config, &frame, 6u).intensity == 0.0f);
  assert(vkr_bloom_gpu_params(&config, &frame, 0u).intensity == 0.0f);
  printf("  test_bloom_chain_gain PASSED\n");
}

vkr_internal void test_bloom_packet_validation(void) {
  printf("  Running test_bloom_packet_validation...\n");
  VkrFrameInput packet = {
      .version = VKR_FRAME_INPUT_VERSION,
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
  };
  VkrValidationError validation = {0};
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  packet.globals.bloom_enabled = 2u;
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.bloom_enabled") == 0);

  packet.globals.bloom_enabled = true_v;
  packet.globals.bloom_threshold = VKR_BLOOM_DEFAULT_THRESHOLD;
  packet.globals.bloom_knee = VKR_BLOOM_DEFAULT_KNEE;
  packet.globals.bloom_intensity = VKR_BLOOM_DEFAULT_INTENSITY;
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  packet.globals.bloom_threshold = NAN;
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.bloom_threshold") == 0);
  packet.globals.bloom_threshold = VKR_BLOOM_DEFAULT_THRESHOLD;

  packet.globals.bloom_knee = -1.0f;
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.bloom_knee") == 0);
  packet.globals.bloom_knee = VKR_BLOOM_DEFAULT_KNEE;

  packet.globals.bloom_intensity = INFINITY;
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.bloom_intensity") == 0);
  packet.globals.bloom_intensity = VKR_BLOOM_DEFAULT_INTENSITY;

  packet.version = VKR_FRAME_INPUT_VERSION - 1u;
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_INCOMPATIBLE_SIGNATURE);
  printf("  test_bloom_packet_validation PASSED\n");
}

bool32_t run_bloom_tests(void) {
  printf("--- Running bloom tests... ---\n");
  test_bloom_config_and_mips();
  test_bloom_chain_gain();
  test_bloom_packet_validation();
  printf("Bloom tests PASSED\n");
  return true;
}
