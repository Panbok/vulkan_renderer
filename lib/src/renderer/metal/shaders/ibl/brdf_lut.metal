struct VkrMetalPacketBrdfRoot {
  texture2d<float, access::write> target;
  uint sample_count;
  uint target_size;
  ulong2 reserved;
};

static_assert(sizeof(VkrMetalPacketBrdfRoot) == 32,
              "Metal packet BRDF root ABI must remain 32 bytes");

float vkr_metal_packet_geometry_schlick(float no_v, float roughness) {
  float k = roughness * roughness * 0.5;
  return no_v / max(no_v * (1.0 - k) + k, 1e-5);
}

kernel void vkr_metal_packet_ibl_brdf(uint2 position
                                      [[thread_position_in_grid]],
                                      constant VkrMetalPacketBrdfRoot *root
                                      [[buffer(0)]]) {
  if (position.x >= root->target_size || position.y >= root->target_size)
    return;
  float2 uv = (float2(position) + 0.5) / float(root->target_size);
  float no_v = clamp(uv.x, 1e-4, 1.0);
  float roughness = clamp(uv.y, 0.0, 1.0);
  float3 view = float3(sqrt(max(1.0 - no_v * no_v, 0.0)), 0.0, no_v);
  float2 result = 0.0;
  for (uint i = 0; i < root->sample_count; ++i) {
    float3 half_vector = vkr_metal_packet_importance_ggx(
        vkr_metal_packet_hammersley(i, root->sample_count),
        float3(0.0, 0.0, 1.0), roughness);
    float3 light = normalize(2.0 * dot(view, half_vector) * half_vector - view);
    float no_l = max(light.z, 0.0);
    float no_h = max(half_vector.z, 0.0);
    float vo_h = max(dot(view, half_vector), 0.0);
    if (no_l <= 0.0)
      continue;
    float visibility = vkr_metal_packet_geometry_schlick(no_v, roughness) *
                       vkr_metal_packet_geometry_schlick(no_l, roughness);
    float g_vis = visibility * vo_h / max(no_h * no_v, 1e-5);
    float fresnel = pow(1.0 - vo_h, 5.0);
    result += float2((1.0 - fresnel) * g_vis, fresnel * g_vis);
  }
  result /= float(max(root->sample_count, 1u));
  root->target.write(float4(result, 0.0, 1.0), position);
}
