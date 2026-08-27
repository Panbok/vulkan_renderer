#include "exposure_test.h"

#include "renderer/renderer_frontend.h"
#include "renderer/vkr_exposure.h"
#include "renderer/vkr_render_packet.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static VkrExposureFrameInput exposure_input(uint32_t mode) {
  return (VkrExposureFrameInput){
      .mode = mode,
      .manual_exposure = VKR_DEFAULT_EXPOSURE,
      .compensation_ev = 1.5f,
      .delta_time = 1.0 / 60.0,
  };
}

static void test_exposure_manual_is_passthrough(void) {
  printf("  Running test_exposure_manual_is_passthrough...\n");
  VkrExposureState state = {0};
  VkrExposureFrameInput input = exposure_input(VKR_EXPOSURE_MODE_MANUAL);
  VkrExposureFrame frame = vkr_exposure_prepare(&state, &input);

  /* The byte-identity gate for phase E0: whatever the caller authored is what
     tonemapping multiplies by, with no substitution or renormalization. */
  assert(frame.manual == VKR_DEFAULT_EXPOSURE);
  assert(frame.mode == VKR_EXPOSURE_MODE_MANUAL);
  /* An EV bias is meaningless against a manual linear multiplier, so manual
     mode drops it rather than letting it silently change old output. */
  assert(frame.compensation_ev == 0.0f);
  assert(!frame.history_valid);

  vkr_exposure_commit(&state, &input);
  frame = vkr_exposure_prepare(&state, &input);
  assert(frame.manual == VKR_DEFAULT_EXPOSURE && !frame.history_valid &&
         frame.reset_reasons == 0u);
  printf("  test_exposure_manual_is_passthrough PASSED\n");
}

static void test_exposure_automatic_history(void) {
  printf("  Running test_exposure_automatic_history...\n");
  VkrExposureState state = {0};
  VkrExposureFrameInput input = exposure_input(VKR_EXPOSURE_MODE_AUTOMATIC);

  VkrExposureFrame first = vkr_exposure_prepare(&state, &input);
  assert((first.reset_reasons & VKR_TEMPORAL_RESET_FIRST_FRAME) != 0u);
  assert(!first.history_valid);
  assert(first.compensation_ev == 1.5f);

  vkr_exposure_commit(&state, &input);
  const VkrExposureFrame second = vkr_exposure_prepare(&state, &input);
  assert(second.reset_reasons == 0u && second.history_valid);

  /* Same state and same input must produce the same frame; a metering chain
     that drifts between two identical calls cannot be deterministic evidence.
   */
  const VkrExposureFrame repeated = vkr_exposure_prepare(&state, &input);
  assert(memcmp(&second, &repeated, sizeof(second)) == 0);
  printf("  test_exposure_automatic_history PASSED\n");
}

static void test_exposure_reset_reasons(void) {
  printf("  Running test_exposure_reset_reasons...\n");
  VkrExposureState state = {0};
  VkrExposureFrameInput input = exposure_input(VKR_EXPOSURE_MODE_AUTOMATIC);
  vkr_exposure_commit(&state, &input);

  const uint32_t shared[] = {
      VKR_TEMPORAL_RESET_FIRST_FRAME,   VKR_TEMPORAL_RESET_FRAME_GAP,
      VKR_TEMPORAL_RESET_EXTENT_CHANGE, VKR_TEMPORAL_RESET_SCENE_CHANGE,
      VKR_TEMPORAL_RESET_CAMERA_CUT,    VKR_TEMPORAL_RESET_EXPLICIT,
  };
  for (uint32_t i = 0u; i < ArrayCount(shared); ++i) {
    VkrExposureFrameInput reset = input;
    reset.temporal_reset_reasons = shared[i];
    const VkrExposureFrame frame = vkr_exposure_prepare(&state, &reset);
    assert((frame.reset_reasons & shared[i]) != 0u);
    assert(!frame.history_valid);
  }

  /* A lens change is not a brightness change, so exposure keeps accumulating
     across it even though temporal reconstruction restarts. */
  VkrExposureFrameInput projection = input;
  projection.temporal_reset_reasons = VKR_TEMPORAL_RESET_PROJECTION_CHANGE;
  const VkrExposureFrame kept = vkr_exposure_prepare(&state, &projection);
  assert(kept.reset_reasons == 0u && kept.history_valid);

  VkrExposureFrameInput explicit_reset = input;
  explicit_reset.explicit_reset_reasons = VKR_TEMPORAL_RESET_EXPLICIT;
  assert(!vkr_exposure_prepare(&state, &explicit_reset).history_valid);

  VkrExposureFrameInput mode_change = input;
  mode_change.mode = VKR_EXPOSURE_MODE_MANUAL;
  const VkrExposureFrame switched = vkr_exposure_prepare(&state, &mode_change);
  assert((switched.reset_reasons & VKR_EXPOSURE_RESET_MODE_CHANGE) != 0u);
  assert(!switched.history_valid);
  printf("  test_exposure_reset_reasons PASSED\n");
}

static void test_exposure_bounded_delta(void) {
  printf("  Running test_exposure_bounded_delta...\n");
  VkrExposureState state = {0};
  VkrExposureFrameInput input = exposure_input(VKR_EXPOSURE_MODE_AUTOMATIC);
  vkr_exposure_commit(&state, &input);

  input.delta_time = 1.0 / 60.0;
  assert(fabsf(vkr_exposure_prepare(&state, &input).delta_seconds -
               (float32_t)(1.0 / 60.0)) < 1e-7f);

  /* A hitch, a stalled clock, and a rewound clock must all be incapable of
     publishing an adapted exposure the scene never justified. */
  input.delta_time = 5.0;
  assert(vkr_exposure_prepare(&state, &input).delta_seconds ==
         VKR_EXPOSURE_MAX_DELTA_SECONDS);
  input.delta_time = 0.0;
  assert(vkr_exposure_prepare(&state, &input).delta_seconds == 0.0f);
  input.delta_time = -1.0;
  assert(vkr_exposure_prepare(&state, &input).delta_seconds == 0.0f);
  input.delta_time = (float64_t)NAN;
  assert(vkr_exposure_prepare(&state, &input).delta_seconds == 0.0f);
  printf("  test_exposure_bounded_delta PASSED\n");
}

static void test_exposure_metering_config_normalize(void) {
  printf("  Running test_exposure_metering_config_normalize...\n");
  const VkrExposureMeteringConfig defaults =
      vkr_exposure_metering_config_default();
  const VkrExposureMeteringConfig zeroed = {0};
  const VkrExposureMeteringConfig from_zeroed =
      vkr_exposure_metering_config_normalize(&zeroed);
  const VkrExposureMeteringConfig from_null =
      vkr_exposure_metering_config_normalize(NULL);
  assert(memcmp(&from_zeroed, &defaults, sizeof(defaults)) == 0);
  assert(memcmp(&from_null, &defaults, sizeof(defaults)) == 0);

  const VkrExposureMeteringConfig hostile = {
      .histogram_bin_count = 100000u,
      .min_log_luminance = -12.0f,
      .max_log_luminance = -1000.0f,
      .low_percentile = 0.9f,
      .high_percentile = 0.1f,
      .middle_gray = -1.0f,
      .min_ev = 6.0f,
      .max_ev = -6.0f,
      .brighten_rate_per_second = NAN,
      .darken_rate_per_second = -4.0f,
      .min_luminance = -1.0f,
  };
  const VkrExposureMeteringConfig fixed =
      vkr_exposure_metering_config_normalize(&hostile);
  assert(fixed.histogram_bin_count == VKR_EXPOSURE_HISTOGRAM_BIN_COUNT);
  assert(fixed.min_log_luminance == -12.0f);
  /* Inverted bounds collapse to the minimum span rather than to an empty or
     reversed window the resolve pass would divide by. */
  assert(fixed.max_log_luminance == -11.0f);
  assert(fixed.low_percentile < fixed.high_percentile);
  assert(fixed.low_percentile == 0.1f && fixed.high_percentile == 0.9f);
  assert(fixed.middle_gray == defaults.middle_gray);
  assert(fixed.min_ev == -6.0f && fixed.max_ev == 6.0f);
  assert(fixed.brighten_rate_per_second == defaults.brighten_rate_per_second);
  assert(fixed.darken_rate_per_second == 0.0f);
  assert(fixed.min_luminance == 0.0f);

  const VkrExposureMeteringConfig wide = {
      .histogram_bin_count = 64u,
      .min_log_luminance = 0.0f,
      .max_log_luminance = 1000.0f,
      .low_percentile = 0.25f,
      .high_percentile = 0.75f,
      .middle_gray = 0.5f,
      .min_ev = -2.0f,
      .max_ev = 2.0f,
      .brighten_rate_per_second = 1.0f,
      .darken_rate_per_second = 2.0f,
      .min_luminance = 1e-3f,
  };
  const VkrExposureMeteringConfig clamped =
      vkr_exposure_metering_config_normalize(&wide);
  assert(clamped.histogram_bin_count == VKR_EXPOSURE_HISTOGRAM_BIN_COUNT);
  assert(clamped.max_log_luminance == 40.0f);
  assert(clamped.middle_gray == 0.5f && clamped.min_luminance == 1e-3f);

  VkrExposureMeteringConfig extreme = defaults;
  extreme.min_log_luminance = FLT_MAX;
  extreme.max_log_luminance = FLT_MAX;
  extreme.min_ev = -FLT_MAX;
  extreme.max_ev = FLT_MAX;
  const VkrExposureMeteringConfig bounded =
      vkr_exposure_metering_config_normalize(&extreme);
  assert(bounded.min_log_luminance == 125.0f);
  assert(bounded.max_log_luminance == 126.0f);
  assert(bounded.min_ev == -126.0f && bounded.max_ev == 127.0f);
  const VkrExposureFrame extreme_frame = {
      .manual = FLT_MAX,
  };
  const VkrExposureGpuMetering extreme_metering =
      vkr_exposure_gpu_metering(&bounded, &extreme_frame);
  assert(isfinite(extreme_metering.inverse_log_luminance_range));
  assert(extreme_metering.manual_ev == 127.0f);
  const VkrExposureGpuState extreme_fallback =
      vkr_exposure_resolve(&extreme_metering, &(VkrExposureGpuHistogram){0},
                           &(VkrExposureGpuState){0});
  assert(isfinite(extreme_fallback.exposure_multiplier));
  printf("  test_exposure_metering_config_normalize PASSED\n");
}

static void test_exposure_histogram_reference(void) {
  printf("  Running test_exposure_histogram_reference...\n");
  VkrExposureMeteringConfig config = vkr_exposure_metering_config_default();
  config.min_log_luminance = -4.0f;
  config.max_log_luminance = 4.0f;
  config.low_percentile = 0.25f;
  config.high_percentile = 0.75f;
  config.middle_gray = 0.25f;
  config.min_ev = -16.0f;
  config.max_ev = 16.0f;
  const VkrExposureFrame frame = {
      .mode = VKR_EXPOSURE_MODE_AUTOMATIC,
      .manual = 0.5f,
      .compensation_ev = 0.5f,
      .delta_seconds = 0.1f,
  };
  VkrExposureGpuMetering metering = vkr_exposure_gpu_metering(&config, &frame);

  assert(!vkr_exposure_luminance_accepted(&metering, 0.0f));
  assert(!vkr_exposure_luminance_accepted(&metering, NAN));
  assert(!vkr_exposure_luminance_accepted(&metering, INFINITY));
  assert(vkr_exposure_luminance_accepted(&metering, 1.0f));
  assert(vkr_exposure_bin_index(&metering, exp2f(-8.0f)) == 0u);
  assert(vkr_exposure_bin_index(&metering, 1.0f) == 128u);
  assert(vkr_exposure_bin_index(&metering, exp2f(8.0f)) == 255u);

  VkrExposureGpuHistogram histogram = {0};
  histogram.bins[64] = 10u;
  histogram.bins[128] = 10u;
  const float32_t expected_average =
      (vkr_exposure_bin_log_luminance(&metering, 64u) +
       vkr_exposure_bin_log_luminance(&metering, 128u)) *
      0.5f;
  const float32_t expected_target =
      metering.log_middle_gray - expected_average + frame.compensation_ev;
  const VkrExposureGpuState resolved =
      vkr_exposure_resolve(&metering, &histogram, NULL);
  assert(resolved.accepted_texel_count == 20u);
  assert(resolved.retained_low_bin == 64.0f);
  assert(resolved.retained_high_bin == 128.0f);
  assert(fabsf(resolved.average_log_luminance - expected_average) < 1e-6f);
  assert(fabsf(resolved.target_ev - expected_target) < 1e-6f);
  assert(resolved.adapted_ev == resolved.target_ev);
  assert(fabsf(resolved.exposure_multiplier - exp2f(expected_target)) < 1e-6f);

  metering.history_valid = 1u;
  const VkrExposureGpuState previous = {.adapted_ev = -1.0f};
  const VkrExposureGpuState adapted =
      vkr_exposure_resolve(&metering, &histogram, &previous);
  const float32_t rate = expected_target > previous.adapted_ev
                             ? metering.brighten_rate_per_second
                             : metering.darken_rate_per_second;
  const float32_t expected_adapted =
      previous.adapted_ev + (expected_target - previous.adapted_ev) *
                                (1.0f - expf(-rate * metering.delta_seconds));
  assert(fabsf(adapted.adapted_ev - expected_adapted) < 1e-6f);

  const VkrExposureGpuHistogram empty = {0};
  const VkrExposureGpuState held =
      vkr_exposure_resolve(&metering, &empty, &previous);
  assert(held.accepted_texel_count == 0u);
  assert(held.target_ev == previous.adapted_ev);
  assert(held.adapted_ev == previous.adapted_ev);
  printf("  test_exposure_histogram_reference PASSED\n");
}

static void test_exposure_packet_validation(void) {
  printf("  Running test_exposure_packet_validation...\n");
  VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
  };
  VkrValidationError validation = {0};
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  packet.globals.exposure_mode = (uint32_t)VKR_EXPOSURE_MODE_COUNT;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.exposure_mode") == 0);

  packet.globals.exposure_mode = (uint32_t)VKR_EXPOSURE_MODE_AUTOMATIC;
  packet.globals.manual_exposure = 0.0f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.globals.manual_exposure") == 0);

  packet.globals.manual_exposure = NAN;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);

  packet.globals.manual_exposure = VKR_DEFAULT_EXPOSURE;
  packet.globals.exposure_compensation_ev = INFINITY;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.globals.exposure_compensation_ev") == 0);

  /* Version 20 introduced the exposure contract; packet 21 adds bloom.
     The validator still rejects every immediately preceding packet ABI. */
  packet.globals.exposure_compensation_ev = 0.0f;
  packet.packet_version = VKR_RENDER_PACKET_VERSION - 1u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_INCOMPATIBLE_SIGNATURE);
  printf("  test_exposure_packet_validation PASSED\n");
}

bool32_t run_exposure_tests(void) {
  printf("Running exposure tests...\n");
  test_exposure_manual_is_passthrough();
  test_exposure_automatic_history();
  test_exposure_reset_reasons();
  test_exposure_bounded_delta();
  test_exposure_metering_config_normalize();
  test_exposure_histogram_reference();
  test_exposure_packet_validation();
  printf("Exposure tests PASSED\n");
  return true_v;
}
