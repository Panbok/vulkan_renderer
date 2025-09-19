#pragma once

#include "defines.h"

/**
 * @brief Represents a clock that can be started, stopped, and updated.
 * The clock measures time in seconds.
 */
typedef struct VkrClock {
  float64_t
      start_time;    // Absolute time when clock was started, 0.0 when stopped
  float64_t elapsed; // Elapsed time since start_time
} VkrClock;

/**
 * @brief Creates a new VkrClock instance.
 * @return VkrClock instance.
 */
VkrClock vkr_clock_create();

/**
 * @brief Updates the VkrClock instance.
 * @param clock VkrClock instance.
 */
void vkr_clock_update(VkrClock *clock);

/**
 * @brief Starts the VkrClock instance.
 * @param clock VkrClock instance.
 */
void vkr_clock_start(VkrClock *clock);

/**
 * @brief Stops the VkrClock instance.
 * @param clock VkrClock instance.
 */
void vkr_clock_stop(VkrClock *clock);
