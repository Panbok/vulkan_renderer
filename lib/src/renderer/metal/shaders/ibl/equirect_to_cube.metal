struct alignas(16) VkrMetalPacketEquirectRoot {
  texture2d<float, access::sample> source;
  texturecube<float, access::write> target;
  uint target_size;
  uint reserved_0;
  uint reserved_1;
  uint reserved_2;
};

static_assert(sizeof(VkrMetalPacketEquirectRoot) == 32,
              "Metal packet equirect root ABI must remain 32 bytes");

kernel void
vkr_metal_packet_ibl_equirect(uint3 position [[thread_position_in_grid]],
                              constant VkrMetalPacketEquirectRoot *root
                              [[buffer(0)]]) {
  if (position.x >= root->target_size || position.y >= root->target_size ||
      position.z >= 6)
    return;
  constexpr sampler environment_sampler(coord::normalized, address::repeat,
                                        filter::linear);
  float2 face_uv = (float2(position.xy) + 0.5) / float(root->target_size);
  float3 direction =
      normalize(vkr_metal_packet_cube_direction(position.z, face_uv));
  float2 environment_uv = float2(
      atan2(direction.z, direction.x) * (0.5 / vkr_metal_packet_pi) + 0.5,
      acos(clamp(direction.y, -1.0, 1.0)) / vkr_metal_packet_pi);
  root->target.write(root->source.sample(environment_sampler, environment_uv),
                     position.xy, position.z);
}
