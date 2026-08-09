#pragma once

#include "defines.h"

typedef struct VkrBindlessVkMappedRange {
  uint64_t offset;
  uint64_t size;
} VkrBindlessVkMappedRange;

/**
 * Expands a mapped write/read to Vulkan's non-coherent atom boundary. The end
 * may equal allocation_size without being atom-aligned, as required by
 * VkMappedMemoryRange.
 */
bool8_t vkr_bindless_vulkan_noncoherent_range(
    uint64_t offset, uint64_t size, uint64_t allocation_size,
    uint64_t noncoherent_atom_size, VkrBindlessVkMappedRange *out_range);
