struct alignas(16) VkrMetalPacketIrradianceRoot {
  texturecube<float, access::sample> source;
  texturecube<float, access::write> target;
  uint sample_count;
  uint target_size;
  uint2 reserved;
};

static_assert(sizeof(VkrMetalPacketIrradianceRoot) == 32,
              "Metal packet irradiance root ABI must remain 32 bytes");

kernel void
vkr_metal_packet_ibl_irradiance(uint3 position [[thread_position_in_grid]],
                                constant VkrMetalPacketIrradianceRoot *root
                                [[buffer(0)]]) {
  if (position.x >= root->target_size || position.y >= root->target_size ||
      position.z >= 6)
    return;
  constexpr sampler cube_sampler(coord::normalized, address::clamp_to_edge,
                                 filter::linear);
  float2 uv = (float2(position.xy) + 0.5) / float(root->target_size);
  float3 normal = normalize(vkr_metal_packet_cube_direction(position.z, uv));
  float3 irradiance = 0.0;
  for (uint i = 0; i < root->sample_count; ++i) {
    float2 xi = vkr_metal_packet_hammersley(i, root->sample_count);
    float phi = 2.0 * vkr_metal_packet_pi * xi.x;
    float cos_theta = sqrt(1.0 - xi.y);
    float sin_theta = sqrt(xi.y);
    float3 direction = vkr_metal_packet_tangent_sample(
        normal, float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta));
    irradiance += root->source.sample(cube_sampler, direction).rgb;
  }
  irradiance /= float(max(root->sample_count, 1u));
  root->target.write(float4(irradiance, 1.0), position.xy, position.z);
}
