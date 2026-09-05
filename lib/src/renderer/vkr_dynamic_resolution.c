#include "renderer/vkr_dynamic_resolution.h"

#include <math.h>

#define VKR_DYNAMIC_RESOLUTION_DOWNSHIFT_SAMPLES 3u
#define VKR_DYNAMIC_RESOLUTION_UPSHIFT_SAMPLES 45u
#define VKR_DYNAMIC_RESOLUTION_COOLDOWN_SAMPLES 30u
#define VKR_DYNAMIC_RESOLUTION_FILTER_ALPHA 0.2
#define VKR_DYNAMIC_RESOLUTION_OVER_BUDGET_RATIO 1.02
#define VKR_DYNAMIC_RESOLUTION_UNDER_BUDGET_RATIO 0.82
#define VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON 1e-5f

static float32_t vkr_dynamic_resolution_canonical_scale(float32_t scale) {
  return roundf(scale * 1000.0f) / 1000.0f;
}

static float32_t vkr_dynamic_resolution_nearest_scale(float32_t scale,
                                                      float32_t min_scale,
                                                      float32_t max_scale) {
  const float32_t clamped = Max(min_scale, Min(scale, max_scale));
  const float32_t steps =
      floorf((max_scale - clamped) / VKR_DYNAMIC_RESOLUTION_SCALE_STEP + 0.5f);
  const float32_t lattice = Max(
      min_scale, vkr_dynamic_resolution_canonical_scale(
                     max_scale - steps * VKR_DYNAMIC_RESOLUTION_SCALE_STEP));
  return fabsf(clamped - min_scale) < fabsf(clamped - lattice) ? min_scale
                                                               : lattice;
}

static float32_t
vkr_dynamic_resolution_next_scale(const VkrDynamicResolutionState *state,
                                  bool8_t increase) {
  if (!increase)
    return Max(state->min_scale,
               vkr_dynamic_resolution_canonical_scale(
                   state->current_scale - VKR_DYNAMIC_RESOLUTION_SCALE_STEP));
  if (state->current_scale <=
      state->min_scale + VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON) {
    const float32_t steps = floorf((state->max_scale - state->min_scale) /
                                       VKR_DYNAMIC_RESOLUTION_SCALE_STEP +
                                   VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON);
    float32_t next = vkr_dynamic_resolution_canonical_scale(
        state->max_scale - steps * VKR_DYNAMIC_RESOLUTION_SCALE_STEP);
    if (next <= state->min_scale + VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON)
      next += VKR_DYNAMIC_RESOLUTION_SCALE_STEP;
    return Min(state->max_scale, vkr_dynamic_resolution_canonical_scale(next));
  }
  return Min(state->max_scale,
             vkr_dynamic_resolution_canonical_scale(
                 state->current_scale + VKR_DYNAMIC_RESOLUTION_SCALE_STEP));
}

bool8_t vkr_dynamic_resolution_config_normalize(
    const VkrDynamicResolutionConfig *config, float32_t initial_scale,
    VkrDynamicResolutionConfig *out_config, float32_t *out_initial_scale) {
  if (!config || !out_config || !out_initial_scale ||
      !isfinite(initial_scale) || initial_scale <= 0.0f || initial_scale > 1.0f)
    return false_v;

  VkrDynamicResolutionConfig normalized = *config;
  if (!normalized.enabled) {
    *out_config = (VkrDynamicResolutionConfig){0};
    *out_initial_scale = initial_scale;
    return true_v;
  }
  if (normalized.min_scale == 0.0f)
    normalized.min_scale = VKR_DYNAMIC_RESOLUTION_DEFAULT_MIN_SCALE;
  if (normalized.max_scale == 0.0f)
    normalized.max_scale = VKR_DYNAMIC_RESOLUTION_DEFAULT_MAX_SCALE;
  if (normalized.target_frame_ms == 0.0f)
    normalized.target_frame_ms = VKR_DYNAMIC_RESOLUTION_DEFAULT_TARGET_FRAME_MS;
  if (!isfinite(normalized.min_scale) || !isfinite(normalized.max_scale) ||
      !isfinite(normalized.target_frame_ms) || normalized.min_scale <= 0.0f ||
      normalized.max_scale > 1.0f ||
      normalized.min_scale > normalized.max_scale ||
      normalized.target_frame_ms <= 0.0f ||
      (float64_t)normalized.target_frame_ms > (float64_t)UINT64_MAX / 1000000.0)
    return false_v;

  if (normalized.max_scale - normalized.min_scale <=
      VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON)
    return false_v;
  *out_config = normalized;
  *out_initial_scale = vkr_dynamic_resolution_nearest_scale(
      initial_scale, normalized.min_scale, normalized.max_scale);
  return true_v;
}

void vkr_dynamic_resolution_init(VkrDynamicResolutionState *state,
                                 const VkrDynamicResolutionConfig *config,
                                 float32_t initial_scale) {
  if (!state)
    return;
  *state = (VkrDynamicResolutionState){0};
  if (!config || !config->enabled)
    return;
  state->enabled = true_v;
  state->current_scale = initial_scale;
  state->min_scale = config->min_scale;
  state->max_scale = config->max_scale;
  state->target_frame_ns =
      (uint64_t)((float64_t)config->target_frame_ms * 1000000.0 + 0.5);
}

bool8_t vkr_dynamic_resolution_update(VkrDynamicResolutionState *state,
                                      uint64_t submit_value,
                                      uint64_t gpu_frame_ns,
                                      float32_t source_scale,
                                      float32_t *out_scale) {
  if (!state || !out_scale || !state->enabled || submit_value == 0u ||
      submit_value <= state->last_submit_value || gpu_frame_ns == 0u ||
      fabsf(source_scale - state->current_scale) > 0.001f)
    return false_v;
  state->last_submit_value = submit_value;
  if (!state->filtered_sample_valid) {
    state->filtered_frame_ns = (float64_t)gpu_frame_ns;
    state->filtered_sample_valid = true_v;
  } else {
    state->filtered_frame_ns +=
        ((float64_t)gpu_frame_ns - state->filtered_frame_ns) *
        VKR_DYNAMIC_RESOLUTION_FILTER_ALPHA;
  }

  if (state->cooldown_samples > 0u) {
    state->cooldown_samples--;
    state->over_budget_samples = 0u;
    state->under_budget_samples = 0u;
    return false_v;
  }

  const float64_t over_threshold = (float64_t)state->target_frame_ns *
                                   VKR_DYNAMIC_RESOLUTION_OVER_BUDGET_RATIO;
  const float64_t under_threshold = (float64_t)state->target_frame_ns *
                                    VKR_DYNAMIC_RESOLUTION_UNDER_BUDGET_RATIO;
  if (state->filtered_frame_ns > over_threshold) {
    state->over_budget_samples++;
    state->under_budget_samples = 0u;
  } else if (state->filtered_frame_ns < under_threshold) {
    state->under_budget_samples++;
    state->over_budget_samples = 0u;
  } else {
    state->over_budget_samples = 0u;
    state->under_budget_samples = 0u;
  }

  bool8_t changed = false_v;
  if (state->over_budget_samples >= VKR_DYNAMIC_RESOLUTION_DOWNSHIFT_SAMPLES &&
      state->current_scale >
          state->min_scale + VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON) {
    state->current_scale = vkr_dynamic_resolution_next_scale(state, false_v);
    changed = true_v;
  } else if (state->under_budget_samples >=
                 VKR_DYNAMIC_RESOLUTION_UPSHIFT_SAMPLES &&
             state->current_scale <
                 state->max_scale - VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON) {
    state->current_scale = vkr_dynamic_resolution_next_scale(state, true_v);
    changed = true_v;
  }
  if (!changed)
    return false_v;

  state->over_budget_samples = 0u;
  state->under_budget_samples = 0u;
  state->cooldown_samples = VKR_DYNAMIC_RESOLUTION_COOLDOWN_SAMPLES;
  state->filtered_sample_valid = false_v;
  state->transition_count++;
  *out_scale = state->current_scale;
  return true_v;
}
