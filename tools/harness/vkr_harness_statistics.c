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

/**
 * How specifically `pattern` matches `name`, or zero when it does not: twice
 * the literal length, plus two for an exact pattern and one for a wildcard. A
 * longer literal always ranks higher, and an exact name outranks a wildcard
 * over the same text.
 */
vkr_internal uint64_t vkr_harness_pass_pattern_rank(const char *pattern,
                                                    const char *name) {
  const uint64_t length = string_length(pattern);
  if (length > 0u && pattern[length - 1u] == '*') {
    const uint64_t literal = length - 1u;
    if (!string_n_equals(name, pattern, literal)) {
      return 0u;
    }
    return literal * 2u + 1u;
  }
  if (!string_equals(name, pattern)) {
    return 0u;
  }
  return length * 2u + 2u;
}

/**
 * The group holding the most specific pattern that matches `name`, or
 * `groups->group_count` when none does. The parser rejects a pattern listed
 * twice, so two matching patterns never share a rank.
 */
vkr_internal uint32_t vkr_harness_pass_group_find(
    const VkrHarnessPassGroups *groups, const char *name) {
  uint32_t best_group = groups->group_count;
  uint64_t best_rank = 0u;
  for (uint32_t group = 0; group < groups->group_count; ++group) {
    const VkrHarnessPassGroup *candidate = &groups->groups[group];
    for (uint32_t i = 0; i < candidate->pattern_count; ++i) {
      const uint64_t rank =
          vkr_harness_pass_pattern_rank(candidate->patterns[i], name);
      if (rank > best_rank) {
        best_rank = rank;
        best_group = group;
      }
    }
  }
  return best_group;
}

bool8_t vkr_harness_compute_pass_group_results(
    const VkrHarnessArenas *arenas, const VkrHarnessPassGroups *groups,
    uint32_t frame_count, uint32_t pass_count,
    const VkrHarnessPassResult *passes, const float64_t *gpu_ms,
    const uint8_t *flags, VkrHarnessPassGroupResult **out_groups,
    uint32_t *out_group_count, VkrHarnessError *out_error) {
  /* The implicit "unassigned" group follows the file's groups. */
  const uint32_t unassigned = groups->group_count;
  const uint64_t result_bytes =
      (uint64_t)(unassigned + 1u) * sizeof(VkrHarnessPassGroupResult);
  const uint64_t member_bytes = (uint64_t)pass_count * sizeof(uint32_t);
  const uint64_t scratch_bytes = (uint64_t)frame_count * sizeof(float64_t);
  VkrHarnessPassGroupResult *results =
      arena_alloc(arenas->persistent, result_bytes, ARENA_MEMORY_TAG_STRUCT);
  uint32_t *members = pass_count > 0u
                          ? arena_alloc(arenas->persistent, member_bytes,
                                        ARENA_MEMORY_TAG_ARRAY)
                          : NULL;
  Scratch scratch = scratch_create(arenas->transient);
  uint32_t *membership =
      pass_count > 0u
          ? arena_alloc(arenas->transient, member_bytes, ARENA_MEMORY_TAG_ARRAY)
          : NULL;
  float64_t *valid =
      arena_alloc(arenas->transient, scratch_bytes, ARENA_MEMORY_TAG_ARRAY);
  float64_t *sorted =
      arena_alloc(arenas->transient, scratch_bytes, ARENA_MEMORY_TAG_ARRAY);
  if (!results || (pass_count > 0u && (!members || !membership)) || !valid ||
      !sorted) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    vkr_harness_error_set(out_error, "statistics.allocate", "$",
                          "Unable to allocate pass group statistics");
    return false_v;
  }
  MemZero(results, result_bytes);
  for (uint32_t pass = 0; pass < pass_count; ++pass) {
    membership[pass] = vkr_harness_pass_group_find(groups, passes[pass].name);
  }

  const uint8_t skipped = VKR_HARNESS_PASS_FLAG_CULLED |
                          VKR_HARNESS_PASS_FLAG_DISABLED |
                          VKR_HARNESS_PASS_FLAG_OMITTED;
  uint32_t result_count = 0u;
  uint32_t member_count = 0u;
  for (uint32_t group = 0; group <= unassigned; ++group) {
    const uint32_t first_member = member_count;
    for (uint32_t pass = 0; pass < pass_count; ++pass) {
      if (membership[pass] == group) {
        members[member_count++] = pass;
      }
    }
    /* "unassigned" only shows what the file left out. A declared group that
       matched no pass still reports every frame valid at zero: a sum over no
       passes costs nothing, and its empty pass list says why. */
    if (group == unassigned && member_count == first_member) {
      break;
    }
    VkrHarnessPassGroupResult *result = &results[result_count];
    string_format(result->name, sizeof(result->name), "%s",
                  group < unassigned ? groups->groups[group].name
                                     : "unassigned");
    if (group < unassigned) {
      result->p95_target_ms = groups->groups[group].p95_target_ms;
      result->has_p95_target = groups->groups[group].has_p95_target;
    }
    result->passes = members ? members + first_member : NULL;
    result->pass_count = member_count - first_member;

    uint64_t valid_count = 0u;
    uint64_t invalid_count = 0u;
    for (uint32_t frame = 0; frame < frame_count; ++frame) {
      const uint64_t row = (uint64_t)frame * pass_count;
      float64_t sum = 0.0;
      bool8_t frame_valid = true_v;
      for (uint32_t member = 0; frame_valid && member < result->pass_count;
           ++member) {
        const uint64_t offset = row + result->passes[member];
        if ((flags[offset] & VKR_HARNESS_PASS_FLAG_GPU_VALID) != 0u) {
          sum += gpu_ms[offset];
        } else if ((flags[offset] & skipped) == 0u) {
          /* An unsupported timestamp scope or a missing sample: the frame's
             line is unknown, not cheaper. */
          frame_valid = false_v;
        }
      }
      if (frame_valid) {
        valid[valid_count++] = sum;
      } else {
        invalid_count++;
      }
    }
    if (!vkr_harness_statistics_compute(valid, valid_count, invalid_count,
                                        sorted, &result->gpu_ms)) {
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      vkr_harness_error_set(out_error, "statistics.compute", "$",
                            "Unable to compute pass group '%s'", result->name);
      return false_v;
    }
    result_count++;
  }
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  *out_groups = results;
  *out_group_count = result_count;
  return true_v;
}
