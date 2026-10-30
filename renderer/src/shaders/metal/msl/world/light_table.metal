// The retained light table's scatter (Lighting.Scatter, vkr_light_table.h):
// each thread writes one of the frame's changed rows into its row of the
// table, which every pass that shades lights then reads.

// Mirrors VkrLightRowUpdate in vkr_light_table.h.
struct VkrLightRowUpdate {
  uint row;
  uint reserved[3];
  VkrGpuPointLightRow value;
};

// Mirrors VkrMetalLightScatterRoot in vkr_metal_packet_abi.h.
struct VkrMetalLightScatterRoot {
  device const VkrLightRowUpdate *updates;
  device VkrGpuPointLightRow *table;
  uint update_count;
  uint reserved[3];
};

kernel void vkr_metal_light_scatter(constant VkrMetalLightScatterRoot &root
                                    [[buffer(0)]],
                                    uint index [[thread_position_in_grid]]) {
  if (index >= root.update_count)
    return;
  const device VkrLightRowUpdate &update = root.updates[index];
  root.table[update.row] = update.value;
}
