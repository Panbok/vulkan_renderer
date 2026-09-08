struct VkrMetalPacketDofRoot {
  VkrDofGpuParams params;
  texture2d<float, access::sample> source0;
  texture2d<float, access::sample> source1;
  texture2d<float, access::sample> source2;
  texture2d<float, access::sample> source3;
  texture2d<float, access::sample> source4;
  texture2d<float, access::write> destination0;
  texture2d<float, access::write> destination1;
  uint2 reserved;
};
constexpr sampler vkr_dof_linear(coord::normalized, address::clamp_to_edge,
                                 filter::linear);
constexpr sampler vkr_dof_nearest(coord::normalized, address::clamp_to_edge,
                                  filter::nearest);
static float4 vkr_metal_dof_load(constant VkrMetalPacketDofRoot &root,
                                 uint index, uint2 pixel) {
  switch (index) {
  case 0u:
    return root.source0.read(pixel);
  case 1u:
    return root.source1.read(pixel);
  case 2u:
    return root.source2.read(pixel);
  case 3u:
    return root.source3.read(pixel);
  default:
    return root.source4.read(pixel);
  }
}

// A half-resolution footprint has separate color coverage for each hemisphere
// of the lens. Keeping the layers apart prevents silhouette color
// contamination.
float3 vkr_metal_dof_metadata(constant VkrMetalPacketDofRoot &root,
                              uint source_index, float2 uv) {
  int2 base = int2(floor(uv * float2(root.params.dimensions.xy) - 0.5f));
  float near_radius = 0.0f;
  float far_radius = 0.0f;
  float near_depth = 65504.0f;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      uint2 q = uint2(clamp(base + int2(x, y), int2(0),
                            int2(root.params.dimensions.xy) - 1));
      float2 metadata = vkr_metal_dof_load(root, source_index, q).xy;
      near_radius = max(near_radius, -metadata.x);
      far_radius = max(far_radius, metadata.x);
      if (metadata.x < 0.0f)
        near_depth = min(near_depth, metadata.y);
    }
  }
  return float3(near_radius, far_radius, near_depth);
}

kernel void vkr_metal_packet_dof_coc(constant VkrMetalPacketDofRoot &root
                                     [[buffer(0)]],
                                     uint2 pixel [[thread_position_in_grid]]) {
  if (!all(pixel < root.params.dimensions.xy))
    return;
  float2 uv = (float2(pixel) + 0.5f) / float2(root.params.dimensions.xy);
  float2 depth_uv = uv * root.params.depth_uv.xy + root.params.depth_uv.zw;
  float device_depth = root.source0.sample(vkr_dof_nearest, depth_uv).x;
  float depth = root.params.lens.z * root.params.lens.w /
                (root.params.lens.w -
                 device_depth * (root.params.lens.w - root.params.lens.z));
  float radius = vkr_dof_radius(root.params, depth, device_depth >= 1.0f);
  root.destination0.write(float4(radius, min(depth, 65504.0f), 0.0f, 0.0f),
                          pixel);
}
kernel void vkr_metal_packet_dof_dilate_horizontal(
    constant VkrMetalPacketDofRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (!all(pixel < root.params.dimensions.zw))
    return;
  float2 half_extent = float2(root.params.dimensions.zw);
  float2 uv = (float2(pixel) + 0.5f) / half_extent;
  float near_radius = 0.0f;
  float near_depth = 65504.0f;
  // Sixteen output pixels span at most eight half-resolution texels.
  for (int x = -8; x <= 8; ++x) {
    float2 tap_uv = uv + float2(float(x), 0.0f) / half_extent;
    float3 metadata = vkr_metal_dof_metadata(root, 0, tap_uv);
    if (metadata.x + 0.5f >= abs(float(x)) * 2.0f) {
      near_radius = max(near_radius, metadata.x);
      near_depth = min(near_depth, metadata.z);
    }
  }
  root.destination0.write(float4(near_radius, near_depth, 0.0f, 0.0f), pixel);
}
kernel void
vkr_metal_packet_dof_dilate_vertical(constant VkrMetalPacketDofRoot &root
                                     [[buffer(0)]],
                                     uint2 pixel [[thread_position_in_grid]]) {
  if (!all(pixel < root.params.dimensions.zw))
    return;
  float near_radius = 0.0f;
  float near_depth = 65504.0f;
  for (int y = -8; y <= 8; ++y) {
    uint2 q = uint2(clamp(int2(pixel) + int2(0, y), int2(0),
                          int2(root.params.dimensions.zw) - 1));
    float2 metadata = root.source0.read(q).xy;
    if (metadata.x + 0.5f >= abs(float(y)) * 2.0f) {
      near_radius = max(near_radius, metadata.x);
      near_depth = min(near_depth, metadata.y);
    }
  }
  root.destination0.write(float4(near_radius, near_depth, 0.0f, 0.0f), pixel);
}
kernel void vkr_metal_packet_dof_prefilter(constant VkrMetalPacketDofRoot &root
                                           [[buffer(0)]],
                                           uint2 pixel
                                           [[thread_position_in_grid]]) {
  if (!all(pixel < root.params.dimensions.zw))
    return;
  float2 uv = (float2(pixel) + 0.5f) / float2(root.params.dimensions.zw);
  int2 base = int2(floor(uv * float2(root.params.dimensions.xy) - 0.5f));
  float4 near_color = float4(0.0f);
  float4 far_color = float4(0.0f);
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      uint2 q = uint2(clamp(base + int2(x, y), int2(0),
                            int2(root.params.dimensions.xy) - 1));
      float coc = root.source1.read(q).x;
      // The spatial reconstruction source may retain the internal extent.
      float2 source_uv = (float2(q) + 0.5f) / float2(root.params.dimensions.xy);
      float3 color = root.source0.sample(vkr_dof_linear, source_uv).rgb;
      float near_weight = coc < 0.0f ? vkr_dof_coverage(coc) : 0.0f;
      near_color += float4(color * near_weight, near_weight) * 0.25f;
      // Focused surfaces remain sharp in the original image and do not
      // spread color into defocused receivers across their silhouettes.
      float far_weight = coc > 0.0f ? vkr_dof_coverage(coc) : 0.0f;
      far_color += float4(color * far_weight, far_weight) * 0.25f;
    }
  }
  root.destination0.write(near_color, pixel);
  root.destination1.write(far_color, pixel);
}
kernel void vkr_metal_packet_dof_gather(constant VkrMetalPacketDofRoot &root
                                        [[buffer(0)]],
                                        uint2 pixel
                                        [[thread_position_in_grid]]) {
  if (!all(pixel < root.params.dimensions.zw))
    return;
  float2 uv = (float2(pixel) + 0.5f) / float2(root.params.dimensions.zw);
  float3 center_metadata = vkr_metal_dof_metadata(root, 2, uv);
  float2 dilation = root.source3.read(pixel).xy;
  float near_search = max(1.0f, dilation.x);
  float far_search = max(1.0f, center_metadata.y);
  float4 near_sum = float4(0.0f);
  float4 far_sum = float4(0.0f);
  for (uint i = 0; i < 32u; ++i) {
    float2 disk = vkr_dof_disk(i);
    float2 near_offset = disk * near_search;
    float2 near_uv = uv + near_offset / float2(root.params.dimensions.xy);
    float3 near_metadata = vkr_metal_dof_metadata(root, 2, near_uv);
    float4 near_color = root.source0.sample(vkr_dof_linear, near_uv);
    float near_weight =
        vkr_dof_disk_weight(near_metadata.x, length(near_offset));
    // Gather area compensates for smaller source discs inside a larger
    // dilated search footprint. Alpha remains premultiplied coverage.
    near_weight *= near_search * near_search /
                   max(1.0f, near_metadata.x * near_metadata.x);
    near_sum += near_color * near_weight * (1.0f / 32.0f);
    float2 far_offset = disk * far_search;
    float2 far_uv = uv + far_offset / float2(root.params.dimensions.xy);
    float3 far_metadata = vkr_metal_dof_metadata(root, 2, far_uv);
    float4 far_color = root.source1.sample(vkr_dof_linear, far_uv);
    float far_weight =
        vkr_dof_disk_weight(max(1.0f, far_metadata.y), length(far_offset));
    // Different positive-CoC depths must mix where their lens discs overlap.
    // Symmetric depth rejection would preserve a hard far-layer silhouette.
    far_sum += far_color * far_weight;
  }
  near_sum /= max(1.0f, near_sum.a);
  far_sum.rgb /= max(far_sum.a, 1e-6f);
  far_sum.a = far_sum.a > 0.0f ? 1.0f : 0.0f;
  root.destination0.write(near_sum, pixel);
  root.destination1.write(far_sum, pixel);
}
kernel void vkr_metal_packet_dof_composite(constant VkrMetalPacketDofRoot &root
                                           [[buffer(0)]],
                                           uint2 pixel
                                           [[thread_position_in_grid]]) {
  if (!all(pixel < root.params.dimensions.xy))
    return;
  float2 source_uv = (float2(pixel) + 0.5f) / float2(root.params.dimensions.xy);
  float4 original = root.source0.sample(vkr_dof_linear, source_uv);
  float2 metadata = root.source1.read(pixel).xy;
  float2 half_position = (float2(pixel) + 0.5f) *
                             float2(root.params.dimensions.zw) /
                             float2(root.params.dimensions.xy) -
                         0.5f;
  int2 base = int2(floor(half_position));
  float2 fraction = half_position - float2(base);
  float4 near_sum = float4(0.0f);
  float4 far_sum = float4(0.0f);
  float near_weight_sum = 0.0f;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      uint2 q = uint2(clamp(base + int2(x, y), int2(0),
                            int2(root.params.dimensions.zw) - 1));
      float weight = (x == 0 ? 1.0f - fraction.x : fraction.x) *
                     (y == 0 ? 1.0f - fraction.y : fraction.y);
      float4 far_color = root.source3.read(q);
      far_sum += float4(far_color.rgb * far_color.a, far_color.a) * weight;
      float2 dilation = root.source4.read(q).xy;
      float near_weight =
          dilation.y <= metadata.y * 1.03f + 0.05f ? weight : 0.0f;
      near_sum += root.source2.read(q) * near_weight;
      near_weight_sum += weight;
    }
  }
  float far_mix = metadata.x > 0.0f ? vkr_dof_coverage(metadata.x) : 0.0f;
  float3 far_color = far_sum.a > 1e-6f ? far_sum.rgb / far_sum.a : original.rgb;
  float3 color = original.rgb * (1.0f - far_mix) + far_color * far_mix;
  near_sum /= max(near_weight_sum, 1e-6f);
  color = color * (1.0f - near_sum.a) + near_sum.rgb;
  root.destination0.write(float4(color, original.a), pixel);
}

static_assert(sizeof(VkrDofGpuParams) == 48, "DoF params ABI");
static_assert(sizeof(VkrMetalPacketDofRoot) == 112, "DoF root ABI");
