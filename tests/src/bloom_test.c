#include "bloom_test.h"

#include "renderer/renderer_frontend.h"
#include "renderer/vkr_bloom.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
  test_bloom_packet_validation();
  printf("Bloom tests PASSED\n");
  return true;
}
