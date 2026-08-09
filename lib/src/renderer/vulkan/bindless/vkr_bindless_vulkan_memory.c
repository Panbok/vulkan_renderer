#include "renderer/vulkan/bindless/vkr_bindless_vulkan_memory.h"

bool8_t vkr_bindless_vulkan_noncoherent_range(
    uint64_t offset, uint64_t size, uint64_t allocation_size,
    uint64_t noncoherent_atom_size, VkrBindlessVkMappedRange *out_range) {
  if (!out_range || !size || !allocation_size || !noncoherent_atom_size ||
      (noncoherent_atom_size & (noncoherent_atom_size - 1u)) != 0u ||
      offset >= allocation_size || size > allocation_size - offset)
    return false_v;
  const uint64_t start = offset & ~(noncoherent_atom_size - 1u);
  const uint64_t requested_end = offset + size;
  uint64_t end = requested_end;
  if (requested_end <= UINT64_MAX - (noncoherent_atom_size - 1u))
    end = AlignPow2(requested_end, noncoherent_atom_size);
  if (end > allocation_size)
    end = allocation_size;
  *out_range = (VkrBindlessVkMappedRange){.offset = start, .size = end - start};
  return true_v;
}
