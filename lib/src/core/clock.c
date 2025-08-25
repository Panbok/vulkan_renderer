#include "clock.h"

Clock clock_create() { return (Clock){0.0, 0.0}; }

void clock_update(Clock *clock) {
  if (clock->start_time == 0.0) {
    return;
  }

  clock->elapsed = vkr_platform_get_absolute_time() - clock->start_time;
}

void clock_start(Clock *clock) {
  clock->start_time = vkr_platform_get_absolute_time();
  clock->elapsed = 0.0;
}

void clock_stop(Clock *clock) { clock->start_time = 0.0; }
