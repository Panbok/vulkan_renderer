constexpr sampler vkr_metal_gtao_point_sampler(coord::normalized,
                                               address::clamp_to_edge,
                                               filter::nearest,
                                               mip_filter::nearest);

struct alignas(16) VkrMetalPacketGtaoDepthRoot {
  VkrGtaoParams params;
  texture2d<float, access::read> source;
  texture2d<float, access::write> destination;
  uint2 source_extent;
  uint2 destination_extent;
};

struct alignas(16) VkrMetalPacketGtaoEvaluateRoot {
  VkrGtaoParams params;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::sample> view_depth;
  texture2d<float, access::read> normal;
  texture2d<float, access::write> destination;
  texture2d<float, access::write> edges;
  uint2 source_extent;
  uint2 destination_extent;
  uint2 reserved;
};

struct alignas(16) VkrMetalPacketGtaoDenoiseRoot {
  VkrGtaoParams params;
  texture2d<float, access::sample> source;
  texture2d<float, access::sample> edges;
  texture2d<float, access::write> destination;
  uint2 source_extent;
  uint2 destination_extent;
  uint2 reserved;
};

static float2 vkr_metal_gtao_uv(float2 pixel, uint2 extent) {
  return (pixel + 0.5) / float2(extent);
}

static void vkr_metal_gtao_store(texture2d<float, access::write> destination,
                                 uint2 pixel, float value) {
  destination.write(float4(value, 0.0, 0.0, 1.0), pixel);
}

kernel void
vkr_metal_packet_gtao_depth_prefilter(constant VkrMetalPacketGtaoDepthRoot &root
                                      [[buffer(0)]],
                                      uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.destination_extent))
    return;
  float device_depth = root.source.read(min(pixel, root.source_extent - 1u)).x;
  vkr_metal_gtao_store(root.destination, pixel,
                       vkr_gtao_linearize_depth(root.params, device_depth));
}

kernel void
vkr_metal_packet_gtao_depth_mip(constant VkrMetalPacketGtaoDepthRoot &root
                                [[buffer(0)]],
                                uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.destination_extent))
    return;
  uint2 source_pixel = pixel * 2u;
  uint2 source_limit = root.source_extent - 1u;
  float d0 = root.source.read(min(source_pixel, source_limit)).x;
  float d1 =
      root.source.read(min(source_pixel + uint2(1u, 0u), source_limit)).x;
  float d2 =
      root.source.read(min(source_pixel + uint2(0u, 1u), source_limit)).x;
  float d3 =
      root.source.read(min(source_pixel + uint2(1u, 1u), source_limit)).x;
  vkr_metal_gtao_store(root.destination, pixel,
                       vkr_gtao_depth_mip_filter(root.params, d0, d1, d2, d3));
}

static float
vkr_metal_gtao_view_depth(constant VkrMetalPacketGtaoEvaluateRoot &root,
                          float2 uv, float mip) {
  return root.view_depth.sample(vkr_metal_gtao_point_sampler, uv, level(mip)).x;
}

kernel void
vkr_metal_packet_gtao_evaluate(constant VkrMetalPacketGtaoEvaluateRoot &root
                               [[buffer(0)]],
                               uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.destination_extent))
    return;
  if (root.vbuffer.read(pixel).x == 0u) {
    vkr_metal_gtao_store(root.destination, pixel, 1.0);
    vkr_metal_gtao_store(root.edges, pixel, vkr_gtao_pack_edges(float4(1.0)));
    return;
  }

  float2 uv = (float2(pixel) + 0.5) * float2(root.params.viewport_pixel_size_x,
                                             root.params.viewport_pixel_size_y);
  float2 texel = float2(root.params.viewport_pixel_size_x,
                        root.params.viewport_pixel_size_y);
  float center_depth = vkr_metal_gtao_view_depth(root, uv, 0.0);
  float left_depth =
      vkr_metal_gtao_view_depth(root, uv - float2(texel.x, 0.0), 0.0);
  float right_depth =
      vkr_metal_gtao_view_depth(root, uv + float2(texel.x, 0.0), 0.0);
  float top_depth =
      vkr_metal_gtao_view_depth(root, uv - float2(0.0, texel.y), 0.0);
  float bottom_depth =
      vkr_metal_gtao_view_depth(root, uv + float2(0.0, texel.y), 0.0);
  float4 directional_edges = vkr_gtao_calculate_edges(
      center_depth, left_depth, right_depth, top_depth, bottom_depth);
  vkr_metal_gtao_store(root.edges, pixel,
                       vkr_gtao_pack_edges(directional_edges));

  float3 world_normal =
      vkr_metal_packet_octahedral_decode(root.normal.read(pixel).xy);
  float3 view_normal =
      normalize((root.params.view * float4(world_normal, 0.0)).xyz);
  float3 center_position =
      vkr_gtao_view_position(root.params, uv, center_depth);
  float3 view_direction = normalize(-center_position);
  float pixel_view_size =
      vkr_gtao_pixel_view_size(root.params, uv, center_depth);
  float projected_radius = root.params.effect_radius *
                           root.params.radius_multiplier / pixel_view_size;
  float2 noise = vkr_gtao_spatiotemporal_noise(pixel, root.params.noise_index);
  float visibility = 0.0;

  for (uint slice = 0u; slice < root.params.slice_count; ++slice) {
    float phi =
        (float(slice) + noise.x) * VKR_GTAO_PI / float(root.params.slice_count);
    float2 slice_direction = float2(cos(phi), sin(phi));
    float3 view_slice_direction = float3(slice_direction, 0.0);
    float3 slice_orthogonal =
        normalize(cross(view_slice_direction, view_direction));
    float3 projected_normal =
        view_normal - slice_orthogonal * dot(view_normal, slice_orthogonal);
    float projected_normal_length = max(length(projected_normal), 1e-6);
    float cos_normal =
        clamp(dot(projected_normal, view_direction) / projected_normal_length,
              0.0, 1.0);
    float normal_sign =
        dot(view_slice_direction, projected_normal) >= 0.0 ? 1.0 : -1.0;
    float normal_angle = normal_sign * acos(cos_normal);
    float low_horizon_cos0 = cos(normal_angle + VKR_GTAO_HALF_PI);
    float low_horizon_cos1 = cos(normal_angle - VKR_GTAO_HALF_PI);
    float horizon_cos0 = low_horizon_cos0;
    float horizon_cos1 = low_horizon_cos1;

    for (uint step = 0u; step < root.params.steps_per_slice; ++step) {
      float sample_distance =
          max(vkr_gtao_sample_distance(root.params, float(step), noise.y) *
                  projected_radius,
              1.0);
      float mip =
          clamp(log2(sample_distance) - root.params.depth_mip_sampling_offset,
                0.0, float(max(root.params.depth_mip_count, 1u) - 1u));
      for (uint side = 0u; side < 2u; ++side) {
        float side_sign = side == 0u ? -1.0 : 1.0;
        float2 sample_uv =
            clamp(uv + slice_direction * sample_distance * texel * side_sign,
                  texel * 0.5, 1.0 - texel * 0.5);
        float sample_depth = vkr_metal_gtao_view_depth(root, sample_uv, mip);
        float3 sample_position =
            vkr_gtao_view_position(root.params, sample_uv, sample_depth);
        float3 horizon_vector = sample_position - center_position;
        float horizon_distance = max(length(horizon_vector), 1e-6);
        float sample_horizon_cos =
            dot(horizon_vector / horizon_distance, view_direction);
        float falloff = vkr_gtao_falloff_weight(root.params, horizon_distance);
        if (side == 0u) {
          horizon_cos0 = max(
              horizon_cos0, mix(low_horizon_cos0, sample_horizon_cos, falloff));
        } else {
          horizon_cos1 = max(
              horizon_cos1, mix(low_horizon_cos1, sample_horizon_cos, falloff));
        }
      }
    }
    visibility +=
        vkr_gtao_integrate_slice(projected_normal_length, cos_normal,
                                 normal_angle, horizon_cos0, horizon_cos1);
  }

  vkr_metal_gtao_store(root.destination, pixel,
                       vkr_gtao_finalize_visibility(root.params, visibility));
}

static float
vkr_metal_gtao_denoise_sample(texture2d<float, access::sample> texture,
                              float2 pixel, uint2 extent) {
  return texture
      .sample(vkr_metal_gtao_point_sampler, vkr_metal_gtao_uv(pixel, extent),
              level(0.0))
      .x;
}

static float vkr_metal_gtao_edge_weight(float edge0, float edge1, float beta) {
  return pow(saturate(edge0 * edge1), beta);
}

kernel void
vkr_metal_packet_gtao_denoise(constant VkrMetalPacketGtaoDenoiseRoot &root
                              [[buffer(0)]],
                              uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.destination_extent))
    return;

  float2 p = float2(pixel);
  float beta = root.params.denoise_blur_beta;
  float4 center_edges = vkr_gtao_unpack_edges(
      vkr_metal_gtao_denoise_sample(root.edges, p, root.destination_extent));
  float4 left_edges = vkr_gtao_unpack_edges(vkr_metal_gtao_denoise_sample(
      root.edges, p + float2(-1.0, 0.0), root.destination_extent));
  float4 right_edges = vkr_gtao_unpack_edges(vkr_metal_gtao_denoise_sample(
      root.edges, p + float2(1.0, 0.0), root.destination_extent));
  float4 top_edges = vkr_gtao_unpack_edges(vkr_metal_gtao_denoise_sample(
      root.edges, p + float2(0.0, -1.0), root.destination_extent));
  float4 bottom_edges = vkr_gtao_unpack_edges(vkr_metal_gtao_denoise_sample(
      root.edges, p + float2(0.0, 1.0), root.destination_extent));
  float4 top_left_edges = vkr_gtao_unpack_edges(vkr_metal_gtao_denoise_sample(
      root.edges, p + float2(-1.0, -1.0), root.destination_extent));
  float4 top_right_edges = vkr_gtao_unpack_edges(vkr_metal_gtao_denoise_sample(
      root.edges, p + float2(1.0, -1.0), root.destination_extent));
  float4 bottom_left_edges =
      vkr_gtao_unpack_edges(vkr_metal_gtao_denoise_sample(
          root.edges, p + float2(-1.0, 1.0), root.destination_extent));
  float4 bottom_right_edges =
      vkr_gtao_unpack_edges(vkr_metal_gtao_denoise_sample(
          root.edges, p + float2(1.0, 1.0), root.destination_extent));

  float weights[9];
  weights[0] = pow(
      saturate(vkr_gtao_diagonal_edge_weight(center_edges.x, top_left_edges.y,
                                             center_edges.z, top_left_edges.w)),
      beta);
  weights[1] = vkr_metal_gtao_edge_weight(center_edges.z, top_edges.w, beta);
  weights[2] = pow(saturate(vkr_gtao_diagonal_edge_weight(
                       center_edges.y, top_right_edges.x, center_edges.z,
                       top_right_edges.w)),
                   beta);
  weights[3] = vkr_metal_gtao_edge_weight(center_edges.x, left_edges.y, beta);
  weights[4] = 1.0;
  weights[5] = vkr_metal_gtao_edge_weight(center_edges.y, right_edges.x, beta);
  weights[6] = pow(saturate(vkr_gtao_diagonal_edge_weight(
                       center_edges.x, bottom_left_edges.y, center_edges.w,
                       bottom_left_edges.z)),
                   beta);
  weights[7] = vkr_metal_gtao_edge_weight(center_edges.w, bottom_edges.z, beta);
  weights[8] = pow(saturate(vkr_gtao_diagonal_edge_weight(
                       center_edges.y, bottom_right_edges.x, center_edges.w,
                       bottom_right_edges.z)),
                   beta);

  float visibility = 0.0;
  float weight_sum = 0.0;
  uint index = 0u;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      visibility +=
          weights[index] * vkr_metal_gtao_denoise_sample(
                               root.source, p + float2(float(x), float(y)),
                               root.destination_extent);
      weight_sum += weights[index];
      ++index;
    }
  }
  vkr_metal_gtao_store(root.destination, pixel,
                       visibility / max(weight_sum, 1e-6));
}

static_assert(sizeof(VkrGtaoParams) == 192,
              "GTAO parameter ABI must remain 192 bytes");
static_assert(sizeof(VkrMetalPacketGtaoDepthRoot) == 224,
              "GTAO depth root ABI must remain 224 bytes");
static_assert(sizeof(VkrMetalPacketGtaoEvaluateRoot) == 256,
              "GTAO evaluate root ABI must remain 256 bytes");
static_assert(sizeof(VkrMetalPacketGtaoDenoiseRoot) == 240,
              "GTAO denoise root ABI must remain 240 bytes");
