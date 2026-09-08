struct VkrMetalPacketMotionBlurRoot {
  VkrMotionBlurGpuParams params;
  texture2d<float, access::sample> source0;
  texture2d<float, access::sample> source1;
  texture2d<float, access::sample> source2;
  texture2d<float, access::sample> source3;
  texture2d<float, access::sample> source4;
  texture2d<float, access::write> destination0;
};
constexpr sampler vkr_motion_blur_linear(coord::normalized,
                                         address::clamp_to_edge,
                                         filter::linear);
constexpr sampler vkr_motion_blur_nearest(coord::normalized,
                                          address::clamp_to_edge,
                                          filter::nearest);

bool vkr_metal_motion_blur_eligible(constant VkrMetalPacketMotionBlurRoot &root,
                                    float2 output_uv) {
  float2 raw_uv = output_uv * root.params.depth_uv.xy + root.params.depth_uv.zw;
  uint2 extent = uint2(root.source3.get_width(), root.source3.get_height());
  int2 base = int2(floor(raw_uv * float2(extent) - 0.5f));
  // Conservative bilinear footprint: reconstructed glass color must not be
  // gathered with the opaque depth behind its separately written velocity.
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x) {
      uint2 q = uint2(clamp(base + int2(x, y), int2(0), int2(extent) - 1));
      if (root.source3.read(q).x != 1.0f)
        return false;
    }
  return true;
}
float3
vkr_metal_motion_blur_metadata(constant VkrMetalPacketMotionBlurRoot &root,
                               float2 output_uv) {
  float2 raw_uv = output_uv * root.params.depth_uv.xy + root.params.depth_uv.zw;
  float2 velocity = vkr_motion_blur_velocity(
      root.params, root.source1.sample(vkr_motion_blur_nearest, raw_uv).xy);
  float depth = vkr_motion_blur_depth(
      root.params, root.source2.sample(vkr_motion_blur_nearest, raw_uv).x);
  return float3(velocity, depth);
}
kernel void vkr_metal_packet_motion_blur_tile_max(
    constant VkrMetalPacketMotionBlurRoot &root [[buffer(0)]],
    uint2 local_pixel [[thread_position_in_threadgroup]],
    uint2 tile [[threadgroup_position_in_grid]]) {
  threadgroup float2 tile_velocities[256];
  uint lane = local_pixel.y * 16u + local_pixel.x;
  uint2 pixel = tile * 16u + local_pixel;
  float2 velocity = float2(0.0f);
  if (all(pixel < root.params.dimensions.xy)) {
    float2 uv = (float2(pixel) + 0.5f) / float2(root.params.dimensions.xy);
    float2 raw_uv = uv * root.params.depth_uv.xy + root.params.depth_uv.zw;
    if (root.source2.sample(vkr_motion_blur_nearest, raw_uv).x == 1.0f)
      velocity = vkr_motion_blur_velocity(
          root.params, root.source0.sample(vkr_motion_blur_nearest, raw_uv).xy);
  }
  tile_velocities[lane] = velocity;
  threadgroup_barrier(mem_flags::mem_threadgroup);
  for (uint stride = 128u; stride > 0u; stride >>= 1u) {
    if (lane < stride) {
      float2 candidate = tile_velocities[lane + stride];
      float2 current = tile_velocities[lane];
      if (dot(candidate, candidate) > dot(current, current))
        tile_velocities[lane] = candidate;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if (lane == 0u)
    root.destination0.write(float4(tile_velocities[0], 0.0f, 0.0f), tile);
}
kernel void vkr_metal_packet_motion_blur_neighbor_max(
    constant VkrMetalPacketMotionBlurRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (!all(pixel < root.params.dimensions.zw))
    return;
  float2 velocity = float2(0.0f);
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      uint2 q = uint2(clamp(int2(pixel) + int2(x, y), int2(0),
                            int2(root.params.dimensions.zw) - 1));
      float2 candidate = root.source0.read(q).xy;
      if (dot(candidate, candidate) > dot(velocity, velocity))
        velocity = candidate;
    }
  root.destination0.write(float4(velocity, 0.0f, 0.0f), pixel);
}
kernel void vkr_metal_packet_motion_blur_reconstruct(
    constant VkrMetalPacketMotionBlurRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (!all(pixel < root.params.dimensions.xy))
    return;
  float2 uv = (float2(pixel) + 0.5f) / float2(root.params.dimensions.xy);
  float4 original = root.source0.sample(vkr_motion_blur_linear, uv);
  float2 neighbor_velocity = root.source4.read(pixel / 16u).xy;
  if (dot(neighbor_velocity, neighbor_velocity) <= 0.25f ||
      !vkr_metal_motion_blur_eligible(root, uv)) {
    root.destination0.write(original, pixel);
    return;
  }
  float3 center = vkr_metal_motion_blur_metadata(root, uv);
  float2 center_line =
      dot(center.xy, center.xy) > 0.25f ? center.xy : neighbor_velocity;
  float3 sum = original.rgb;
  float weight_sum = 1.0f;
  // The original pixel plus 31 candidates is at most 32 color samples. The
  // center and neighborhood lines alternate to retain crossing motion.
  for (uint i = 1u; i < 32u; ++i) {
    float t = (float(i) - 16.0f) / 15.0f;
    float2 velocity = (i & 1u) != 0u ? neighbor_velocity : center_line;
    float2 offset = velocity * t;
    float2 tap_uv = uv + offset / float2(root.params.dimensions.xy);
    if (any(tap_uv < float2(0.0f)) || any(tap_uv > float2(1.0f)) ||
        !vkr_metal_motion_blur_eligible(root, tap_uv))
      continue;
    float3 metadata = vkr_metal_motion_blur_metadata(root, tap_uv);
    float weight = vkr_motion_blur_sample_weight(offset, center.xy, metadata.xy,
                                                 center.z, metadata.z);
    if (weight <= 0.0f)
      continue;
    sum += root.source0.sample(vkr_motion_blur_linear, tap_uv).rgb * weight;
    weight_sum += weight;
  }
  root.destination0.write(float4(sum / weight_sum, original.a), pixel);
}

static_assert(sizeof(VkrMotionBlurGpuParams) == 48, "Motion blur params ABI");
static_assert(sizeof(VkrMetalPacketMotionBlurRoot) == 96,
              "Motion blur root ABI");
