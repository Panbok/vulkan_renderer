#pragma once

#include "defines.h"

/**
 * Resettable CPU-wait counters owned by the packet renderer's render thread.
 * Upload waits are a subset of command-slot waits, not an additional wait.
 */
typedef struct VkrMetalPacketWaitCounters {
  uint64_t command_slot;
  uint64_t upload;
} VkrMetalPacketWaitCounters;

static inline void
vkr_metal_packet_wait_counters_record(VkrMetalPacketWaitCounters *counters,
                                      bool8_t upload) {
  counters->command_slot++;
  if (upload) {
    counters->upload++;
  }
}

static inline uint64_t vkr_metal_packet_wait_counters_take_command_slot(
    VkrMetalPacketWaitCounters *counters) {
  const uint64_t count = counters->command_slot;
  counters->command_slot = 0;
  return count;
}

static inline uint64_t vkr_metal_packet_wait_counters_take_upload(
    VkrMetalPacketWaitCounters *counters) {
  const uint64_t count = counters->upload;
  counters->upload = 0;
  return count;
}
