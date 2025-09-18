#pragma once

#include "defines.h"
#include "pch.h"
#include "platform/vkr_platform.h"

typedef struct VkrClock {
  float64_t
      start_time;    // Absolute time when clock was started, 0.0 when stopped
  float64_t elapsed; // Elapsed time since start_time
} VkrClock;

VkrClock vkr_clock_create();

void vkr_clock_update(VkrClock *clock);

void vkr_clock_start(VkrClock *clock);

void vkr_clock_stop(VkrClock *clock);
