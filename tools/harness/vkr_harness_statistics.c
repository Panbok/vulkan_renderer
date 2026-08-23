#include "vkr_harness.h"

static int32_t vkr_harness_compare_f64(const void *a, const void *b) {
  const float64_t lhs = *(const float64_t *)a;
  const float64_t rhs = *(const float64_t *)b;
  return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
}

bool8_t vkr_harness_statistics_compute(const float64_t *samples,
                                       uint64_t sample_count,
                                       uint64_t invalid_count,
                                       float64_t *sort_scratch,
                                       VkrHarnessStatistics *out_statistics) {
  if (!out_statistics || (sample_count > 0 && (!samples || !sort_scratch)) ||
      sample_count > SIZE_MAX / sizeof(float64_t)) {
    return false_v;
  }
  MemZero(out_statistics, sizeof(*out_statistics));
  out_statistics->invalid_count = invalid_count;
  if (sample_count == 0) {
    return true_v;
  }
  float64_t *sorted = sort_scratch;
  MemCopy(sorted, samples, (size_t)sample_count * sizeof(*sorted));
  for (uint64_t i = 0; i < sample_count; ++i) {
    if (!vkr_is_finite_f64(sorted[i])) {
      return false_v;
    }
  }
  vkr_sort(sorted, sample_count, sizeof(*sorted), vkr_harness_compare_f64);
  float64_t total = 0.0;
  for (uint64_t i = 0; i < sample_count; ++i) {
    total += samples[i];
  }
  const float64_t mean = total / (float64_t)sample_count;
  float64_t variance = 0.0;
  for (uint64_t i = 0; i < sample_count; ++i) {
    const float64_t delta = samples[i] - mean;
    variance += delta * delta;
  }
  const uint64_t p50_rank = (sample_count * 50u + 99u) / 100u;
  const uint64_t p95_rank = (sample_count * 95u + 99u) / 100u;
  *out_statistics = (VkrHarnessStatistics){
      .sample_count = sample_count,
      .invalid_count = invalid_count,
      .mean = mean,
      .p50 = sorted[p50_rank > 0 ? p50_rank - 1u : 0u],
      .p95 = sorted[p95_rank > 0 ? p95_rank - 1u : 0u],
      .min = sorted[0],
      .max = sorted[sample_count - 1u],
      .stddev = vkr_sqrt_f64(variance / (float64_t)sample_count),
      .total = total,
  };
  return true_v;
}

bool8_t vkr_harness_gpu_pass_samples_complete(const uint8_t *flags,
                                              uint64_t sample_count) {
  if (sample_count == 0u || !flags) {
    return false_v;
  }
  static const uint8_t kTimed =
      VKR_HARNESS_PASS_FLAG_CPU_VALID | VKR_HARNESS_PASS_FLAG_GPU_VALID;
  static const uint8_t kSkipped = VKR_HARNESS_PASS_FLAG_CULLED |
                                  VKR_HARNESS_PASS_FLAG_DISABLED |
                                  VKR_HARNESS_PASS_FLAG_OMITTED;
  static const uint8_t kUnsupported =
      VKR_HARNESS_PASS_FLAG_CPU_VALID |
      VKR_HARNESS_PASS_FLAG_GPU_UNSUPPORTED_SCOPE;
  for (uint64_t i = 0; i < sample_count; ++i) {
    /* Explicitly unsupported scope is a complete availability result, not a
       missing sample. It remains invalid for statistics and cannot support a
       timing claim. */
    const uint8_t evidence =
        flags[i] & (kTimed | VKR_HARNESS_PASS_FLAG_GPU_UNSUPPORTED_SCOPE);
    const bool8_t complete =
        (flags[i] & kSkipped) != 0u
            ? evidence == 0u
            : evidence == kTimed || evidence == kUnsupported;
    if (!complete)
      return false_v;
  }
  return true_v;
}
