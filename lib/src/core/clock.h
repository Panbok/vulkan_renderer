#pragma once

#include "defines.h"
#include "pch.h"
#include "platform/vkr_platform.h"

typedef struct Clock {
  float64_t start_time;
  float64_t elapsed;
} Clock;

Clock clock_create();

void clock_update(Clock *clock);

void clock_start(Clock *clock);

void clock_stop(Clock *clock);
