constexpr sampler vkr_metal_froxel_linear_sampler(coord::normalized,
                                                   address::clamp_to_edge,
                                                   filter::linear,
                                                   mip_filter::none);

struct alignas(16) VkrMetalPacketFroxelInjectRoot {
  constant VkrMetalPacketFrameRoot *frame;
  constant VkrFroxelFogParams *params;
  texture3d<float, access::sample> history;
  texture3d<float, access::write> output;
  uint history_valid;
  packed_uint3 extent;
};

struct alignas(16) VkrMetalPacketFroxelIntegrateRoot {
  constant VkrMetalPacketFrameRoot *frame;
  constant VkrFroxelFogParams *params;
  texture3d<float, access::read> scattering;
  texture3d<float, access::write> integrated;
  packed_uint3 extent;
  uint reserved;
};

struct alignas(16) VkrMetalPacketFroxelApplyRoot {
  constant VkrMetalPacketFrameRoot *frame;
  constant VkrFroxelFogParams *params;
  texture2d<float, access::read> depth;
  texture3d<float, access::sample> integrated;
  texture2d<float, access::read_write> target;
  uint2 extent;
};

static float3 vkr_metal_froxel_local_radiance(
    constant VkrMetalPacketFrameRoot *frame, VkrGpuPointLightRow light,
    float3 world_position) {
  float4 p0 = light.p0;
  float4 p1 = light.p1;
  float4 p2 = light.p2;
  float4 p3 = light.p3;
  uint kind = uint(p2.w + 0.5f);
  float3 to_light = p0.xyz - world_position;
  float distance_squared = dot(to_light, to_light);
  if (kind != 0u && p2.z > 0.0f && distance_squared >= p2.z * p2.z)
    return float3(0.0f);
  float distance = sqrt(distance_squared);
  float3 light_direction = distance > 1e-6f ? to_light / distance : float3(0.0f);
  float attenuation;
  if (kind == 0u) {
    attenuation = 1.0f / max(max(p0.w, 1.0f) + p1.w * distance +
                                  p2.y * distance_squared,
                              1e-6f);
  } else {
    float range_attenuation = 1.0f;
    if (p2.z > 0.0f) {
      float ratio = distance / p2.z;
      range_attenuation = saturate(1.0f - ratio * ratio * ratio * ratio);
      range_attenuation *= range_attenuation;
    }
    attenuation = range_attenuation / max(distance_squared, 1e-4f);
    if (kind == 2u) {
      float cone = dot(-light_direction, normalize(p3.xyz));
      attenuation *= smoothstep(p1.w, p0.w, cone);
    }
  }
  attenuation *= vkr_metal_packet_local_shadow_one_tap(
      frame, uint(p3.w + 0.5f), kind, world_position);
  return max(p1.rgb * p2.x * attenuation, float3(0.0f));
}

static float3 vkr_metal_froxel_incident_radiance(
    constant VkrMetalPacketFrameRoot *frame, constant VkrFroxelFogParams &params,
    float3 world_position) {
  float3 incident = float3(0.0f);
  if (frame->directional_direction_enabled.w > 0.5f) {
    float visibility = vkr_metal_packet_directional_shadow_one_tap(
        frame, world_position);
    incident += frame->directional_color_intensity.rgb *
                frame->directional_color_intensity.w * visibility;
  }
  uint count = params.selected_local_indices_count.z;
  for (uint slot = 0u; slot < count; ++slot) {
    uint source = params.selected_local_indices_count[slot];
    incident += vkr_metal_froxel_local_radiance(
        frame, frame->point_light_data[source], world_position);
  }
  return incident;
}

static VkrFroxelSample vkr_metal_froxel_sample_integrated(
    texture3d<float, access::sample> integrated,
    constant VkrFroxelFogParams &params, float4x4 view,
    float3 world_position) {
  VkrFroxelSample identity = {float3(0.0f), 1.0f};
  VkrFroxelHistoryCoordinate coordinate =
      vkr_froxel_integrated_coordinate(params, world_position);
  if (coordinate.valid == 0u)
    return identity;
  float positive_depth = -((view * float4(world_position, 1.0f)).z);
  VkrFroxelIntegratedCoordinate z = vkr_froxel_integrated_sample_coordinate(
      params, positive_depth);
  float4 packed = integrated.sample(vkr_metal_froxel_linear_sampler,
                                    float3(coordinate.uvw.xy, z.z));
  VkrFroxelSample result = {packed.rgb, saturate(packed.w)};
  if (z.near_identity != 0u) {
    result.inscatter *= z.near_blend;
    result.transmittance = mix(1.0f, result.transmittance, z.near_blend);
  }
  return result;
}

static float3 vkr_metal_froxel_world_position(float4x4 inverse_view_projection,
                                               uint2 pixel, uint2 extent,
                                               float device_depth) {
  float2 ndc = vkr_metal_packet_resolve_ndc(float2(pixel) + 0.5f, extent);
  float4 world = inverse_view_projection * float4(ndc, device_depth, 1.0f);
  return world.xyz / max(abs(world.w), 1e-7f) * sign(world.w);
}

kernel void vkr_metal_packet_froxel_inject(
    constant VkrMetalPacketFroxelInjectRoot &root [[buffer(0)]],
    uint3 cell [[thread_position_in_grid]]) {
  uint3 extent = uint3(root.extent);
  if (any(cell >= extent) || !vkr_froxel_enabled(*root.params))
    return;
  float3 world = vkr_froxel_world_center(*root.params, root.frame->view, cell);
  float extinction = vkr_froxel_height_density(*root.params, world);
  float3 source = vkr_froxel_local_source(
      extinction, vkr_metal_froxel_incident_radiance(root.frame, *root.params, world),
      *root.params);
  float4 current = float4(source, extinction);
  VkrFroxelHistoryCoordinate history_coordinate =
      vkr_froxel_history_coordinate(*root.params, world);
  uint valid = root.history_valid != 0u && history_coordinate.valid != 0u ? 1u : 0u;
  float4 history = valid != 0u
                       ? root.history.sample(vkr_metal_froxel_linear_sampler,
                                             history_coordinate.uvw)
                       : float4(0.0f);
  root.output.write(vkr_froxel_temporal_filter(*root.params, current, history,
                                                valid),
                    cell);
}

kernel void vkr_metal_packet_froxel_integrate(
    constant VkrMetalPacketFroxelIntegrateRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  uint3 extent = uint3(root.extent);
  if (any(pixel >= extent.xy) || !vkr_froxel_enabled(*root.params))
    return;
  float2 uv = (float2(pixel) + 0.5f) / float2(extent.xy);
  float2 ndc = uv * 2.0f - 1.0f;
  float3 near_world = vkr_froxel_homogeneous(
      root.params->inverse_view_projection * float4(ndc, 0.0f, 1.0f));
  float3 far_world = vkr_froxel_homogeneous(
      root.params->inverse_view_projection * float4(ndc, 1.0f, 1.0f));
  float ray_scale = vkr_froxel_ray_length_scale(root.frame->view, near_world,
                                                 far_world);
  VkrFroxelSample accumulated = {float3(0.0f), 1.0f};
  for (uint z = 0u; z < extent.z; ++z) {
    VkrFroxelSliceInterval interval = vkr_froxel_slice_interval(*root.params, z);
    float4 local = root.scattering.read(uint3(pixel, z));
    accumulated = vkr_froxel_integrate_step(
        accumulated, local.rgb, local.w,
        (interval.far_distance - interval.near_distance) * ray_scale);
    root.integrated.write(float4(accumulated.inscatter, accumulated.transmittance),
                          uint3(pixel, z));
  }
}

kernel void vkr_metal_packet_froxel_apply(
    constant VkrMetalPacketFroxelApplyRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent) || !vkr_froxel_enabled(*root.params))
    return;
  float4 current = root.target.read(pixel);
  float device_depth = root.depth.read(pixel).x;
  float3 world;
  if (device_depth >= 1.0f) {
    float2 ndc = vkr_metal_packet_resolve_ndc(float2(pixel) + 0.5f,
                                               root.extent);
    world = vkr_froxel_world_at_view_depth(*root.params, root.frame->view, ndc,
                                            root.params->height_distance_phase.z);
  } else {
    world = vkr_metal_froxel_world_position(root.params->inverse_raster_view_projection,
                                             pixel, root.extent, device_depth);
  }
  VkrFroxelSample sample = vkr_metal_froxel_sample_integrated(
      root.integrated, *root.params, root.frame->view, world);
  root.target.write(float4(vkr_froxel_apply_integrated(current.rgb, sample).inscatter,
                           current.a),
                    pixel);
}
