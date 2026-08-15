struct alignas(16) VkrMetalPacketPrefilterRoot {
  texturecube<float, access::sample> source;
  texturecube<float, access::write> target;
  float roughness;
  float source_face_size;
  float source_mip_count;
  uint target_mip;
};

static_assert(sizeof(VkrMetalPacketPrefilterRoot) == 32,
              "Metal packet prefilter root ABI must remain 32 bytes");

kernel void
vkr_metal_packet_ibl_prefilter(uint3 position [[thread_position_in_grid]],
                               constant VkrMetalPacketPrefilterRoot *root
                               [[buffer(0)]]) {
  uint target_size = root->target.get_width(root->target_mip);
  if (position.x >= target_size || position.y >= target_size || position.z >= 6)
    return;
  constexpr sampler cube_sampler(coord::normalized, address::clamp_to_edge,
                                 filter::linear, mip_filter::linear);
  float2 uv = (float2(position.xy) + 0.5) / float(target_size);
  float3 normal = normalize(vkr_metal_packet_cube_direction(position.z, uv));
  float3 view = normal;
  float3 color = 0.0;
  float total_weight = 0.0;
  constexpr uint sample_count = 256u;
  for (uint i = 0; i < sample_count; ++i) {
    float3 half_vector = vkr_metal_packet_importance_ggx(
        vkr_metal_packet_hammersley(i, sample_count), normal, root->roughness);
    float3 light = normalize(2.0 * dot(view, half_vector) * half_vector - view);
    float no_l = max(dot(normal, light), 0.0);
    if (no_l <= 0.0)
      continue;
    float no_h = max(dot(normal, half_vector), 1e-6);
    float vo_h = max(dot(view, half_vector), 1e-6);
    float distribution =
        vkr_metal_packet_distribution_ggx(no_h, root->roughness);
    float pdf = distribution * no_h / max(4.0 * vo_h, 1e-6);
    float sample_solid_angle = 1.0 / (float(sample_count) * max(pdf, 1e-6));
    float texel_solid_angle =
        4.0 * vkr_metal_packet_pi /
        max(6.0 * root->source_face_size * root->source_face_size, 1.0);
    float lod = root->roughness <= 0.001
                    ? 0.0
                    : 0.5 * log2(4.0 * sample_solid_angle / texel_solid_angle);
    lod = clamp(lod, 0.0, max(root->source_mip_count - 1.0, 0.0));
    color += root->source.sample(cube_sampler, light, level(lod)).rgb * no_l;
    total_weight += no_l;
  }
  color /= max(total_weight, 1e-4);
  root->target.write(float4(color, 1.0), position.xy, position.z,
                     root->target_mip);
}
