#include "vkr_clock.h"

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
