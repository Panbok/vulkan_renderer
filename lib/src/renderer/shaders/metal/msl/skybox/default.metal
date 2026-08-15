struct VkrMetalPacketSkyboxRoot {
  float4x4 inverse_view_projection;
  texturecube<float, access::sample> cubemap;
  float2 target_size;
};

static_assert(sizeof(VkrMetalPacketSkyboxRoot) == 80,
              "Metal packet skybox root ABI must remain 80 bytes");

vertex VkrMetalPacketTonemapOutput
vkr_metal_packet_skybox_vertex(uint vertex_id [[vertex_id]]) {
  const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0),
                               float2(-1.0, 3.0)};
  VkrMetalPacketTonemapOutput output;
  output.position = float4(positions[vertex_id], 1.0, 1.0);
  return output;
}

fragment float4 vkr_metal_packet_skybox_fragment(
    VkrMetalPacketTonemapOutput input [[stage_in]],
    constant VkrMetalPacketSkyboxRoot *root [[buffer(1)]]) {
  constexpr sampler cube_sampler(coord::normalized, address::clamp_to_edge,
                                 filter::linear);
  float2 ndc = float2(input.position.x / root->target_size.x * 2.0 - 1.0,
                      1.0 - input.position.y / root->target_size.y * 2.0);
  float4 world = root->inverse_view_projection * float4(ndc, 1.0, 1.0);
  return root->cubemap.sample(cube_sampler, normalize(world.xyz / world.w));
}
