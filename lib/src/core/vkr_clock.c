#include "vkr_clock.h"
#include "core/logger.h"
#include "platform/vkr_platform.h"

VkrClock vkr_clock_create() { return (VkrClock){0.0, 0.0}; }

void vkr_clock_update(VkrClock *clock) {
  assert_log(clock != NULL, "Clock is NULL");

  if (clock->start_time <= 0.0) {
    return;
  }

  clock->elapsed = vkr_platform_get_absolute_time() - clock->start_time;
}

void vkr_clock_start(VkrClock *clock) {
  assert_log(clock != NULL, "Clock is NULL");

  clock->start_time = vkr_platform_get_absolute_time();
  clock->elapsed = 0.0;
}

void vkr_clock_stop(VkrClock *clock) {
  assert_log(clock != NULL, "Clock is NULL");

  if (clock->start_time > 0.0) {
    clock->elapsed = vkr_platform_get_absolute_time() - clock->start_time;
  }

  clock->start_time = 0.0;
}

bool8_t vkr_clock_interval_elapsed(VkrClock *clock,
                                   float64_t interval_seconds) {
  assert_log(clock != NULL, "Clock is NULL");
  assert_log(interval_seconds > 0.0, "Interval must be > 0");

  if (clock->start_time <= 0.0) {
    return false_v;
  }

  vkr_clock_update(clock);
  if (clock->elapsed >= interval_seconds) {
    clock->start_time += clock->elapsed;
    clock->elapsed = 0.0;
    return true_v;
  }

  return false_v;
}
