struct VkrVkMotionBlurRoot {
  VkrMotionBlurGpuParams params;
  uint source0;
  uint source1;
  uint source2;
  uint source3;
  uint source4;
  uint destination0;
  uint source_sampler;
  uint reserved;
};
struct VkrVkMotionBlurPush {
  VkrVkMotionBlurRoot *root;
  uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVkMotionBlurPush> g_motion_blur_push;
groupshared float2 vkr_motion_blur_tile_velocities[256];
uint2 vkr_vk_motion_blur_extent(uint index) {
  uint width, height;
  g_textures[NonUniformResourceIndex(index)].GetDimensions(width, height);
  return uint2(width, height);
}
float4 vkr_vk_motion_blur_nearest(uint index, float2 uv) {
  uint2 extent = vkr_vk_motion_blur_extent(index);
  uint2 pixel =
      uint2(clamp(uv * float2(extent), float2(0.0f), float2(extent) - 1.0f));
  return g_textures[NonUniformResourceIndex(index)].Load(int3(pixel, 0));
}

bool vkr_vk_motion_blur_eligible(VkrVkMotionBlurRoot *root, float2 output_uv) {
  float2 raw_uv = output_uv * root.params.depth_uv.xy + root.params.depth_uv.zw;
  uint2 extent = vkr_vk_motion_blur_extent(root.source3);
  int2 base = int2(floor(raw_uv * float2(extent) - 0.5f));
  // Conservative bilinear footprint: reconstructed glass color must not be
  // gathered with the opaque depth behind its separately written velocity.
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x) {
      uint2 q = uint2(clamp(base + int2(x, y), int2(0), int2(extent) - 1));
      if (g_textures[NonUniformResourceIndex(root.source3)]
              .Load(int3(q, 0))
              .x != 1.0f)
        return false;
    }
  return true;
}
float3 vkr_vk_motion_blur_metadata(VkrVkMotionBlurRoot *root,
                                   float2 output_uv) {
  float2 raw_uv = output_uv * root.params.depth_uv.xy + root.params.depth_uv.zw;
  float2 velocity = vkr_motion_blur_velocity(
      root.params, vkr_vk_motion_blur_nearest(root.source1, raw_uv).xy);
  float depth = vkr_motion_blur_depth(
      root.params, vkr_vk_motion_blur_nearest(root.source2, raw_uv).x);
  return float3(velocity, depth);
}
[shader("compute")][numthreads(16, 16, 1)] void
vk_motion_blur_tile_max(uint3 local_id : SV_GroupThreadID,
                        uint3 group_id : SV_GroupID) {
  VkrVkMotionBlurRoot *root = g_motion_blur_push.root;
  uint2 local_pixel = local_id.xy;
  uint2 tile = group_id.xy;
  uint lane = local_pixel.y * 16u + local_pixel.x;
  uint2 pixel = tile * 16u + local_pixel;
  float2 velocity = float2(0.0f);
  if (all(pixel < root.params.dimensions.xy)) {
    float2 uv = (float2(pixel) + 0.5f) / float2(root.params.dimensions.xy);
    float2 raw_uv = uv * root.params.depth_uv.xy + root.params.depth_uv.zw;
    if (vkr_vk_motion_blur_nearest(root.source2, raw_uv).x == 1.0f)
      velocity = vkr_motion_blur_velocity(
          root.params, vkr_vk_motion_blur_nearest(root.source0, raw_uv).xy);
  }
  vkr_motion_blur_tile_velocities[lane] = velocity;
  GroupMemoryBarrierWithGroupSync();
  for (uint stride = 128u; stride > 0u; stride >>= 1u) {
    if (lane < stride) {
      float2 candidate = vkr_motion_blur_tile_velocities[lane + stride];
      float2 current = vkr_motion_blur_tile_velocities[lane];
      if (dot(candidate, candidate) > dot(current, current))
        vkr_motion_blur_tile_velocities[lane] = candidate;
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (lane == 0u)
    g_storage_2d[NonUniformResourceIndex(root.destination0)][tile] =
        float4(vkr_motion_blur_tile_velocities[0], 0.0f, 0.0f);
}[shader("compute")]
 [numthreads(8, 8, 1)] void vk_motion_blur_neighbor_max(
     uint3 thread_id : SV_DispatchThreadID) {
  VkrVkMotionBlurRoot *root = g_motion_blur_push.root;
  uint2 pixel = thread_id.xy;
  if (!all(pixel < root.params.dimensions.zw))
    return;
  float2 velocity = float2(0.0f);
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      uint2 q = uint2(clamp(int2(pixel) + int2(x, y), int2(0),
                            int2(root.params.dimensions.zw) - 1));
      float2 candidate =
          g_textures[NonUniformResourceIndex(root.source0)].Load(int3(q, 0)).xy;
      if (dot(candidate, candidate) > dot(velocity, velocity))
        velocity = candidate;
    }
  g_storage_2d[NonUniformResourceIndex(root.destination0)][pixel] =
      float4(velocity, 0.0f, 0.0f);
}
[shader("compute")][numthreads(8, 8, 1)] void
vk_motion_blur_reconstruct(uint3 thread_id : SV_DispatchThreadID) {
  VkrVkMotionBlurRoot *root = g_motion_blur_push.root;
  uint2 pixel = thread_id.xy;
  if (!all(pixel < root.params.dimensions.xy))
    return;
  float2 uv = (float2(pixel) + 0.5f) / float2(root.params.dimensions.xy);
  float4 original =
      g_textures[NonUniformResourceIndex(root.source0)].SampleLevel(
          g_samplers[NonUniformResourceIndex(root.source_sampler)], uv, 0.0f);
  float2 neighbor_velocity = g_textures[NonUniformResourceIndex(root.source4)]
                                 .Load(int3(pixel / 16u, 0))
                                 .xy;
  if (dot(neighbor_velocity, neighbor_velocity) <= 0.25f ||
      !vkr_vk_motion_blur_eligible(root, uv)) {
    g_storage_2d[NonUniformResourceIndex(root.destination0)][pixel] = original;
    return;
  }
  float3 center = vkr_vk_motion_blur_metadata(root, uv);
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
        !vkr_vk_motion_blur_eligible(root, tap_uv))
      continue;
    float3 metadata = vkr_vk_motion_blur_metadata(root, tap_uv);
    float weight = vkr_motion_blur_sample_weight(offset, center.xy, metadata.xy,
                                                 center.z, metadata.z);
    if (weight <= 0.0f)
      continue;
    sum += g_textures[NonUniformResourceIndex(root.source0)]
               .SampleLevel(
                   g_samplers[NonUniformResourceIndex(root.source_sampler)],
                   tap_uv, 0.0f)
               .rgb *
           weight;
    weight_sum += weight;
  }
  g_storage_2d[NonUniformResourceIndex(root.destination0)][pixel] =
      float4(sum / weight_sum, original.a);
}
