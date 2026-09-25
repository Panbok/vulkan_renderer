struct alignas(16) VkrMetalPacketFogRoot {
  VkrFogParams params;
  float4x4 inverse_view_projection;
  float4 camera_position;
  texture2d<float, access::read> depth;
  texture2d<float, access::read_write> target;
  uint2 extent;
  texture3d<float, access::sample> aerial_perspective;
  constant VkrMetalPacketSky *sky;
  constant VkrMetalPacketFrameRoot *frame;
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
  VkrFogLighting lighting = vkr_metal_packet_fog_lighting(root.frame);
  float3 fogged;
  if (device_depth >= 1.0f) {
    // The sky-view lookup already carries the atmosphere to its top.
    float3 far_world = vkr_metal_fog_world_position(root, pixel, 1.0f);
    fogged = vkr_fog_apply_sky(current.rgb, root.params, lighting,
                                root.camera_position.xyz,
                                far_world - root.camera_position.xyz);
  } else {
    float3 world = vkr_metal_fog_world_position(root, pixel, device_depth);
    fogged = current.rgb;
    // Aerial perspective is the farther medium, so it applies before fog.
    if (root.sky->params.aerial.w > 0.0f) {
      VkrMetalPacketAerialSample aerial = vkr_metal_packet_aerial_sample(
          root.sky, root.aerial_perspective, world);
      fogged = vkr_sky_apply_aerial(fogged, aerial.packed, aerial.weight);
    }
    fogged = vkr_fog_apply_surface(fogged, root.params, lighting,
                                   root.camera_position.xyz, world);
  }
  root.target.write(float4(fogged, current.a), pixel);
}
