#pragma once

#include "defines.h"
#include "renderer/vkr_renderer.h"

#define VKR_DYNAMIC_RESOLUTION_SCALE_STEP 0.05f
#define VKR_DYNAMIC_RESOLUTION_DEFAULT_MIN_SCALE 0.5f
#define VKR_DYNAMIC_RESOLUTION_DEFAULT_MAX_SCALE 1.0f
#define VKR_DYNAMIC_RESOLUTION_DEFAULT_TARGET_FRAME_MS (1000.0f / 75.0f)

/** Deterministic, allocation-free state driven only by completed GPU samples.
 */
typedef struct VkrDynamicResolutionState {
  uint64_t target_frame_ns;
  uint64_t last_submit_value;
  float32_t current_scale;
  float32_t min_scale;
  float32_t max_scale;
  float64_t filtered_frame_ns;
  uint32_t over_budget_samples;
  uint32_t under_budget_samples;
  uint32_t cooldown_samples;
  uint32_t transition_count;
  bool8_t filtered_sample_valid;
  bool8_t enabled;
} VkrDynamicResolutionState;

/** Resolves zero defaults and snaps the initial scale to the bounded ladder. */
bool8_t vkr_dynamic_resolution_config_normalize(
    const VkrDynamicResolutionConfig *config, float32_t initial_scale,
    VkrDynamicResolutionConfig *out_config, float32_t *out_initial_scale);

/** Initializes a normalized controller. */
void vkr_dynamic_resolution_init(VkrDynamicResolutionState *state,
                                 const VkrDynamicResolutionConfig *config,
                                 float32_t initial_scale);

/**
 * Consumes one completion-owned submission interval. Samples from a prior
 * scale tier and duplicate submit values are ignored.
 *
 * @return true only when `out_scale` contains a newly selected tier.
 */
bool8_t vkr_dynamic_resolution_update(VkrDynamicResolutionState *state,
                                      uint64_t submit_value,
                                      uint64_t gpu_frame_ns,
                                      float32_t source_scale,
                                      float32_t *out_scale);
