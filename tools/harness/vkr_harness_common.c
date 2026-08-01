#include "vkr_harness.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void vkr_harness_error_clear(VkrHarnessError *error) {
  if (error) {
    memset(error, 0, sizeof(*error));
  }
}

void vkr_harness_error_set(VkrHarnessError *error, const char *code,
                           const char *field, const char *format, ...) {
  if (!error) {
    return;
  }
  snprintf(error->code, sizeof(error->code), "%s", code ? code : "error");
  snprintf(error->field, sizeof(error->field), "%s", field ? field : "$");
  va_list args;
  va_start(args, format);
  vsnprintf(error->message, sizeof(error->message), format, args);
  va_end(args);
}

const char *vkr_harness_tool_name(VkrHarnessTool tool) {
  switch (tool) {
  case VKR_HARNESS_TOOL_PROFILE:
    return "profile";
  case VKR_HARNESS_TOOL_SNAPSHOT:
    return "snapshot";
  case VKR_HARNESS_TOOL_AUTOTEST:
    return "autotest";
  default:
    return "unknown";
  }
}

const char *vkr_harness_target_name(VkrHarnessTarget target) {
  switch (target) {
  case VKR_HARNESS_TARGET_WINDOWED_VISIBLE:
    return "windowed_visible";
  case VKR_HARNESS_TARGET_WINDOWED_HIDDEN:
    return "windowed_hidden";
  case VKR_HARNESS_TARGET_OFFSCREEN:
    return "offscreen";
  default:
    return "unknown";
  }
}

const char *vkr_harness_present_name(VkrHarnessPresentMode present) {
  switch (present) {
  case VKR_HARNESS_PRESENT_IMMEDIATE:
    return "immediate";
  case VKR_HARNESS_PRESENT_FIFO:
    return "fifo";
  case VKR_HARNESS_PRESENT_NONE:
    return "none";
  case VKR_HARNESS_PRESENT_MAILBOX:
    return "mailbox";
  case VKR_HARNESS_PRESENT_UNKNOWN:
  default:
    return "unknown";
  }
}

const char *vkr_harness_cache_name(VkrHarnessCacheMode cache) {
  switch (cache) {
  case VKR_HARNESS_CACHE_ISOLATED_COLD:
    return "isolated_cold";
  case VKR_HARNESS_CACHE_ISOLATED_WARM:
    return "isolated_warm";
  case VKR_HARNESS_CACHE_SHARED:
    return "shared";
  default:
    return "unknown";
  }
}

const char *vkr_harness_boot_name(VkrHarnessBootProfile boot) {
  switch (boot) {
  case VKR_HARNESS_BOOT_FULL:
    return "full";
  case VKR_HARNESS_BOOT_AUTOMATION:
    return "automation";
  default:
    return "unknown";
  }
}

/** Index-aligned with VkrHarnessStatisticKind; also the report/CSV wire names.
 */
static const char *const vkr_harness_statistic_names[VKR_HARNESS_STAT_COUNT] = {
    "mean", "p50", "p95", "min", "max", "stddev", "total"};

const char *vkr_harness_statistic_name(VkrHarnessStatisticKind statistic) {
  return statistic < VKR_HARNESS_STAT_COUNT
             ? vkr_harness_statistic_names[statistic]
             : "unknown";
}

bool8_t
vkr_harness_statistic_from_name(const char *name,
                                VkrHarnessStatisticKind *out_statistic) {
  if (!name || !out_statistic) {
    return false_v;
  }
  for (uint32_t i = 0; i < VKR_HARNESS_STAT_COUNT; ++i) {
    if (strcmp(name, vkr_harness_statistic_names[i]) == 0) {
      *out_statistic = (VkrHarnessStatisticKind)i;
      return true_v;
    }
  }
  return false_v;
}

const char *vkr_harness_operator_name(VkrHarnessAssertionOperator operation) {
  switch (operation) {
  case VKR_HARNESS_ASSERT_MAX:
    return "max";
  case VKR_HARNESS_ASSERT_MIN:
    return "min";
  case VKR_HARNESS_ASSERT_EQUALS:
    return "equals";
  default:
    return "unknown";
  }
}

float64_t vkr_harness_statistic_value(const VkrHarnessStatistics *statistics,
                                      VkrHarnessStatisticKind statistic) {
  switch (statistic) {
  case VKR_HARNESS_STAT_P50:
    return statistics->p50;
  case VKR_HARNESS_STAT_P95:
    return statistics->p95;
  case VKR_HARNESS_STAT_MIN:
    return statistics->min;
  case VKR_HARNESS_STAT_MAX:
    return statistics->max;
  case VKR_HARNESS_STAT_STDDEV:
    return statistics->stddev;
  case VKR_HARNESS_STAT_TOTAL:
    return statistics->total;
  case VKR_HARNESS_STAT_MEAN:
  default:
    return statistics->mean;
  }
}

const VkrHarnessMetricResult *
vkr_harness_report_find_metric(const VkrHarnessReport *report,
                               const char *name) {
  for (uint32_t i = 0; i < report->metric_count; ++i) {
    if (strcmp(report->metrics[i].name, name) == 0) {
      return &report->metrics[i];
    }
  }
  return NULL;
}

VkrHarnessAssertionOutcome
vkr_harness_assertion_evaluate(const VkrHarnessReport *report,
                               const VkrHarnessAssertion *assertion,
                               float64_t *out_actual) {
  if (out_actual) {
    *out_actual = 0.0;
  }
  const VkrHarnessMetricResult *metric =
      vkr_harness_report_find_metric(report, assertion->metric);
  if (!metric || metric->statistics.sample_count == 0u ||
      metric->statistics.invalid_count > 0u) {
    return VKR_HARNESS_ASSERTION_INCOMPLETE;
  }
  const float64_t actual =
      vkr_harness_statistic_value(&metric->statistics, assertion->statistic);
  if (out_actual) {
    *out_actual = actual;
  }
  bool8_t passed = false_v;
  switch (assertion->operation) {
  case VKR_HARNESS_ASSERT_MAX:
    passed = actual <= assertion->limit;
    break;
  case VKR_HARNESS_ASSERT_MIN:
    passed = actual >= assertion->limit;
    break;
  case VKR_HARNESS_ASSERT_EQUALS:
  default:
    passed = fabs(actual - assertion->limit) <= assertion->tolerance;
    break;
  }
  return passed ? VKR_HARNESS_ASSERTION_PASS : VKR_HARNESS_ASSERTION_FAIL;
}

const char *
vkr_harness_assertion_outcome_name(VkrHarnessAssertionOutcome outcome) {
  switch (outcome) {
  case VKR_HARNESS_ASSERTION_PASS:
    return "pass";
  case VKR_HARNESS_ASSERTION_FAIL:
    return "fail";
  case VKR_HARNESS_ASSERTION_INCOMPLETE:
  default:
    return "incomplete";
  }
}
