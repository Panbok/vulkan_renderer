#include "renderer/vkr_exposure.h"

#include <math.h>

/** Log2-luminance window the histogram may span, in stops. */
#define VKR_EXPOSURE_LOG_LUMINANCE_SPAN_MIN 1.0f
#define VKR_EXPOSURE_LOG_LUMINANCE_SPAN_MAX 40.0f
/** Finite binary32 log/EV domain. Keeps all derived shader values finite. */
#define VKR_EXPOSURE_LOG_MIN -126.0f
#define VKR_EXPOSURE_LOG_MAX 126.0f
#define VKR_EXPOSURE_EV_MIN -126.0f
#define VKR_EXPOSURE_EV_MAX 127.0f

static float32_t vkr_exposure_finite_or(float32_t value, float32_t fallback) {
  return isfinite(value) ? value : fallback;
}

VkrExposureMeteringConfig vkr_exposure_metering_config_default(void) {
  return (VkrExposureMeteringConfig){
      .histogram_bin_count = VKR_EXPOSURE_HISTOGRAM_BIN_COUNT,
      .min_log_luminance = -10.0f,
      .max_log_luminance = 10.0f,
      .low_percentile = 0.5f,
      .high_percentile = 0.95f,
      .middle_gray = 0.18f,
      .min_ev = -8.0f,
      .max_ev = 4.0f,
      .brighten_rate_per_second = 1.0f,
      .darken_rate_per_second = 8.0f,
      .min_luminance = 1e-4f,
  };
}

VkrExposureMeteringConfig vkr_exposure_metering_config_normalize(
    const VkrExposureMeteringConfig *config) {
  const VkrExposureMeteringConfig defaults =
      vkr_exposure_metering_config_default();
  if (!config || config->histogram_bin_count == 0u)
    return defaults;

  VkrExposureMeteringConfig result = defaults;
  const float32_t min_log =
      Clamp(vkr_exposure_finite_or(config->min_log_luminance,
                                   defaults.min_log_luminance),
            VKR_EXPOSURE_LOG_MIN,
            VKR_EXPOSURE_LOG_MAX - VKR_EXPOSURE_LOG_LUMINANCE_SPAN_MIN);
  const float32_t max_log =
      Clamp(vkr_exposure_finite_or(config->max_log_luminance,
                                   defaults.max_log_luminance),
            VKR_EXPOSURE_LOG_MIN, VKR_EXPOSURE_LOG_MAX);
  result.min_log_luminance = min_log;
  result.max_log_luminance =
      min_log + Clamp(max_log - min_log, VKR_EXPOSURE_LOG_LUMINANCE_SPAN_MIN,
                      Min(VKR_EXPOSURE_LOG_LUMINANCE_SPAN_MAX,
                          VKR_EXPOSURE_LOG_MAX - min_log));

  /* The resolve pass divides by the retained bin weight, so an empty or
     inverted percentile window must be impossible before it ever runs. */
  const float32_t low = Clamp(
      vkr_exposure_finite_or(config->low_percentile, defaults.low_percentile),
      0.0f, 1.0f);
  const float32_t high = Clamp(
      vkr_exposure_finite_or(config->high_percentile, defaults.high_percentile),
      0.0f, 1.0f);
  result.low_percentile = Min(low, high);
  result.high_percentile = Max(low, high);
  if (result.low_percentile == result.high_percentile) {
    result.low_percentile = defaults.low_percentile;
    result.high_percentile = defaults.high_percentile;
  }

  const float32_t middle_gray =
      vkr_exposure_finite_or(config->middle_gray, defaults.middle_gray);
  result.middle_gray = middle_gray > 0.0f ? middle_gray : defaults.middle_gray;

  const float32_t min_ev =
      Clamp(vkr_exposure_finite_or(config->min_ev, defaults.min_ev),
            VKR_EXPOSURE_EV_MIN, VKR_EXPOSURE_EV_MAX);
  const float32_t max_ev =
      Clamp(vkr_exposure_finite_or(config->max_ev, defaults.max_ev),
            VKR_EXPOSURE_EV_MIN, VKR_EXPOSURE_EV_MAX);
  result.min_ev = Min(min_ev, max_ev);
  result.max_ev = Max(min_ev, max_ev);

  result.brighten_rate_per_second =
      ClampBot(vkr_exposure_finite_or(config->brighten_rate_per_second,
                                      defaults.brighten_rate_per_second),
               0.0f);
  result.darken_rate_per_second =
      ClampBot(vkr_exposure_finite_or(config->darken_rate_per_second,
                                      defaults.darken_rate_per_second),
               0.0f);
  result.min_luminance = ClampBot(
      vkr_exposure_finite_or(config->min_luminance, defaults.min_luminance),
      0.0f);
  return result;
}

VkrExposureGpuMetering
vkr_exposure_gpu_metering(const VkrExposureMeteringConfig *config,
                          const VkrExposureFrame *frame) {
  const float32_t range = config->max_log_luminance - config->min_log_luminance;
  return (VkrExposureGpuMetering){
      .min_log_luminance = config->min_log_luminance,
      .log_luminance_range = range,
      .inverse_log_luminance_range = 1.0f / range,
      .min_luminance = config->min_luminance,
      .low_percentile = config->low_percentile,
      .high_percentile = config->high_percentile,
      .log_middle_gray = log2f(config->middle_gray),
      .min_ev = config->min_ev,
      .max_ev = config->max_ev,
      .brighten_rate_per_second = config->brighten_rate_per_second,
      .darken_rate_per_second = config->darken_rate_per_second,
      .compensation_ev = frame->compensation_ev,
      .delta_seconds = frame->delta_seconds,
      /* A finite binary32 multiplier can round to EV 128 at FLT_MAX, whose
         exp2 fallback is infinite. Keep the logarithmic fallback inside the
         finite output domain even though manual tonemapping retains the exact
         caller-provided multiplier. */
      .manual_ev =
          Clamp(log2f(frame->manual), VKR_EXPOSURE_EV_MIN, VKR_EXPOSURE_EV_MAX),
      .bin_count = VKR_EXPOSURE_HISTOGRAM_BIN_COUNT,
      .history_valid = frame->history_valid ? 1u : 0u,
  };
}

float32_t vkr_exposure_history_delta(float64_t current_seconds,
                                      float64_t history_seconds) {
  return (float32_t)Min(current_seconds - history_seconds,
                       (float64_t)VKR_EXPOSURE_MAX_DELTA_SECONDS);
}

VkrExposureFrame vkr_exposure_prepare(const VkrExposureState *state,
                                      const VkrExposureFrameInput *input) {
  VkrExposureFrame frame = {
      .mode = input->mode,
      .manual = input->manual_exposure,
      .compensation_ev = input->mode == VKR_EXPOSURE_MODE_AUTOMATIC
                             ? input->compensation_ev
                             : 0.0f,
      .reset_reasons =
          (input->temporal_reset_reasons & VKR_EXPOSURE_RESET_TEMPORAL_MASK) |
          input->explicit_reset_reasons,
  };
  /* A stalled or rewound clock must not move adaptation, and a long hitch must
     not move it by the whole hitch: both publish an exposure the scene never
     justified. */
  const float32_t delta = (float32_t)input->delta_time;
  frame.delta_seconds = isfinite(delta) && delta > 0.0f
                            ? ClampTop(delta, VKR_EXPOSURE_MAX_DELTA_SECONDS)
                            : 0.0f;

  if (!state->valid)
    frame.reset_reasons |= VKR_TEMPORAL_RESET_FIRST_FRAME;
  else if (state->mode != input->mode)
    frame.reset_reasons |= VKR_EXPOSURE_RESET_MODE_CHANGE;

  frame.history_valid =
      input->mode == VKR_EXPOSURE_MODE_AUTOMATIC && frame.reset_reasons == 0u;
  return frame;
}

void vkr_exposure_commit(VkrExposureState *state,
                         const VkrExposureFrameInput *input) {
  *state = (VkrExposureState){
      .mode = input->mode,
      .valid = true_v,
  };
}
