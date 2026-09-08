struct alignas(16) VkrMetalPacketFogRoot {
  VkrFogParams params;
  float4x4 inverse_view_projection;
  float4 camera_position;
  texture2d<float, access::read> depth;
  texture2d<float, access::read_write> target;
  uint2 extent;
  uint2 reserved;
};

static float3 vkr_metal_fog_world_position(
    constant VkrMetalPacketFogRoot &root, uint2 pixel, float device_depth) {
  float2 ndc = vkr_metal_packet_resolve_ndc(float2(pixel) + 0.5f, root.extent);
  float4 world = root.inverse_view_projection * float4(ndc, device_depth, 1.0f);
  return world.xyz / max(abs(world.w), 1e-7f) * sign(world.w);
}

kernel void vkr_metal_packet_fog_apply(
    constant VkrMetalPacketFogRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent))
    return;
  float4 current = root.target.read(pixel);
  float device_depth = root.depth.read(pixel).x;
  float3 fogged;
  if (device_depth >= 1.0f) {
    float3 far_world = vkr_metal_fog_world_position(root, pixel, 1.0f);
    fogged = vkr_fog_apply_sky(current.rgb, root.params,
                                root.camera_position.xyz,
                                far_world - root.camera_position.xyz);
  } else {
    fogged = vkr_fog_apply_surface(
        current.rgb, root.params, root.camera_position.xyz,
        vkr_metal_fog_world_position(root, pixel, device_depth));
  }
  root.target.write(float4(fogged, current.a), pixel);
}
