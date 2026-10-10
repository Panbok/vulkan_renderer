// Sparse diffuse volume composition (DVOL v3): the active light layers'
// weighted SH, written in place into the volume's SH texture whenever the
// sun turns or a light group factor changes (ADR-090). Mirrors
// diffuse_volume_compose in vulkan/slang/world/diffuse_volume.slang.

struct alignas(16) VkrMetalPacketDiffuseVolumeComposeRoot {
  texture2d<float, access::read> layer_sh;
  texture2d<float, access::write> sh;
  uint probe_count;
  uint rows;
  uint active_layer_count;
  uint reserved;
  uint4 active_layers[2];
  float4 active_weights[2];
};

// One thread per probe.
kernel void vkr_metal_packet_diffuse_volume_compose(
    constant VkrMetalPacketDiffuseVolumeComposeRoot &root [[buffer(0)]],
    uint probe [[thread_position_in_grid]]) {
  if (probe >= root.probe_count)
    return;
  float4 sum[3] = {float4(0.0f), float4(0.0f), float4(0.0f)};
  for (uint a = 0u; a < root.active_layer_count; ++a) {
    uint layer = root.active_layers[a / 4u][a % 4u];
    float weight = root.active_weights[a / 4u][a % 4u];
    for (uint channel = 0u; channel < 3u; ++channel) {
      sum[channel] += weight *
          root.layer_sh.read(
              vkr_diffuse_volume_sh_texel(probe, channel, layer * root.rows));
    }
  }
  for (uint channel = 0u; channel < 3u; ++channel) {
    root.sh.write(sum[channel], vkr_diffuse_volume_sh_texel(probe, channel, 0u));
  }
}
