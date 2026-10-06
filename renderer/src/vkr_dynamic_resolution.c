#include "vkr_dynamic_resolution.h"

#include <math.h>

#define VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON 1e-5f

/* One policy's thresholds. Samples from another tier are ignored, so a short
   cooldown only covers the first sample the new tier completes. */
typedef struct VkrDynamicResolutionTuning {
  uint32_t downshift_samples;
  uint32_t upshift_samples;
  uint32_t cooldown_samples;
  float64_t filter_alpha;
  float64_t over_budget_ratio;
  float64_t under_budget_ratio;
  /* Steps down twice when the filtered time exceeds this; zero never does. */
  float64_t far_over_budget_ratio;
} VkrDynamicResolutionTuning;

static const VkrDynamicResolutionTuning vkr_dynamic_resolution_tunings[] = {
    [VKR_DYNAMIC_RESOLUTION_POLICY_STABLE] =
        {
            .downshift_samples = 3u,
            .upshift_samples = 45u,
            .cooldown_samples = 30u,
            .filter_alpha = 0.2,
            .over_budget_ratio = 1.02,
            .under_budget_ratio = 0.82,
            .far_over_budget_ratio = 0.0,
        },
    [VKR_DYNAMIC_RESOLUTION_POLICY_RESPONSIVE] =
        {
            .downshift_samples = 2u,
            .upshift_samples = 30u,
            .cooldown_samples = 1u,
            .filter_alpha = 1.0,
            .over_budget_ratio = 1.0,
            .under_budget_ratio = 0.8,
            .far_over_budget_ratio = 1.25,
        },
};

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
                                 float32_t initial_scale,
                                 VkrDynamicResolutionPolicy policy) {
  if (!state)
    return;
  *state = (VkrDynamicResolutionState){0};
  state->policy = policy;
  if (!config || !config->enabled)
    return;
  state->enabled = true_v;
  state->current_scale = initial_scale;
  state->min_scale = config->min_scale;
  state->max_scale = config->max_scale;
  state->target_frame_ns =
      (uint64_t)((float64_t)config->target_frame_ms * 1000000.0 + 0.5);
}

void vkr_dynamic_resolution_reset_feedback(VkrDynamicResolutionState *state) {
  state->filtered_frame_ns = 0.0;
  state->filtered_sample_valid = false_v;
  state->upshift_source_frame_ns = 0.0;
  state->upshift_source_scale = 0.0f;
  state->failed_upshift_cost_ratio = 0.0;
  state->failed_upshift_lower_scale = 0.0f;
  state->failed_upshift_upper_scale = 0.0f;
  state->over_budget_samples = 0u;
  state->under_budget_samples = 0u;
  state->far_over_budget = false_v;
  state->cooldown_samples = 0u;
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
  const VkrDynamicResolutionTuning *tuning =
      &vkr_dynamic_resolution_tunings[state->policy];
  state->last_submit_value = submit_value;
  if (!state->filtered_sample_valid) {
    state->filtered_frame_ns = (float64_t)gpu_frame_ns;
    state->filtered_sample_valid = true_v;
  } else {
    state->filtered_frame_ns +=
        ((float64_t)gpu_frame_ns - state->filtered_frame_ns) *
        tuning->filter_alpha;
  }

  if (state->cooldown_samples > 0u) {
    state->cooldown_samples--;
    state->over_budget_samples = 0u;
    state->under_budget_samples = 0u;
    return false_v;
  }

  const float64_t over_threshold =
      (float64_t)state->target_frame_ns * tuning->over_budget_ratio;
  const float64_t under_threshold =
      (float64_t)state->target_frame_ns * tuning->under_budget_ratio;
  const float64_t far_over_threshold =
      (float64_t)state->target_frame_ns * tuning->far_over_budget_ratio;
  // A failed probe calibrates this boundary from measured costs. Do not expire
  // it on a timer: unchanged work would start probing the same failure again.
  const bool8_t failed_upshift_has_headroom =
      state->failed_upshift_cost_ratio == 0.0 ||
      fabsf(state->current_scale - state->failed_upshift_lower_scale) >
          VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON ||
      state->filtered_frame_ns * state->failed_upshift_cost_ratio <
          under_threshold;
  if (state->filtered_frame_ns > over_threshold) {
    // Far over budget only while every counted sample is.
    if (state->over_budget_samples == 0u)
      state->far_over_budget = far_over_threshold > 0.0;
    state->far_over_budget =
        state->far_over_budget && state->filtered_frame_ns > far_over_threshold;
    state->over_budget_samples =
        Min(state->over_budget_samples + 1u, tuning->downshift_samples);
    state->under_budget_samples = 0u;
  } else if (state->filtered_frame_ns < under_threshold &&
             failed_upshift_has_headroom) {
    state->under_budget_samples =
        Min(state->under_budget_samples + 1u, tuning->upshift_samples);
    state->over_budget_samples = 0u;
  } else {
    state->over_budget_samples = 0u;
    state->under_budget_samples = 0u;
  }

  if (state->under_budget_samples >= tuning->upshift_samples) {
    // Sustained headroom at the upper tier completes its probe successfully.
    state->upshift_source_frame_ns = 0.0;
    if (fabsf(state->current_scale - state->failed_upshift_upper_scale) <=
        VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON)
      state->failed_upshift_cost_ratio = 0.0;
  }

  bool8_t changed = false_v;
  if (state->over_budget_samples >= tuning->downshift_samples &&
      state->current_scale >
          state->min_scale + VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON) {
    float32_t lower_scale = vkr_dynamic_resolution_next_scale(state, false_v);
    if (state->far_over_budget) {
      const float32_t current_scale = state->current_scale;
      state->current_scale = lower_scale;
      lower_scale = vkr_dynamic_resolution_next_scale(state, false_v);
      state->current_scale = current_scale;
    }
    if (state->upshift_source_frame_ns > 0.0 &&
        fabsf(lower_scale - state->upshift_source_scale) <=
            VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON) {
      state->failed_upshift_lower_scale = lower_scale;
      state->failed_upshift_upper_scale = state->current_scale;
      state->failed_upshift_cost_ratio =
          state->filtered_frame_ns / state->upshift_source_frame_ns;
    }
    state->upshift_source_frame_ns = 0.0;
    state->current_scale = lower_scale;
    changed = true_v;
  } else if (state->under_budget_samples >= tuning->upshift_samples &&
             state->current_scale <
                 state->max_scale - VKR_DYNAMIC_RESOLUTION_SCALE_EPSILON) {
    state->upshift_source_frame_ns = state->filtered_frame_ns;
    state->upshift_source_scale = state->current_scale;
    state->current_scale = vkr_dynamic_resolution_next_scale(state, true_v);
    changed = true_v;
  }
  if (!changed)
    return false_v;

  state->over_budget_samples = 0u;
  state->under_budget_samples = 0u;
  state->far_over_budget = false_v;
  state->cooldown_samples = tuning->cooldown_samples;
  state->filtered_sample_valid = false_v;
  state->transition_count++;
  *out_scale = state->current_scale;
  return true_v;
}
