constexpr sampler vkr_metal_ssr_linear_sampler(coord::normalized,
                                                address::clamp_to_edge,
                                                filter::linear,
                                                mip_filter::none);

struct alignas(16) VkrMetalPacketSsrDepthBaseRoot {
  VkrSsrParams params;
  texture2d<float, access::read> depth;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::write> pyramid;
};

struct alignas(16) VkrMetalPacketSsrDepthMipRoot {
  VkrSsrParams params;
  texture2d<float, access::read> source;
  texture2d<float, access::write> destination;
  uint2 source_extent;
  uint2 destination_extent;
};

struct alignas(16) VkrMetalPacketSsrTraceRoot {
  VkrSsrParams params;
  texture2d<float, access::read> depth;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::read> normal;
  texture2d<float, access::read> specular;
  texture2d<float, access::read> pyramid;
  texture2d<float, access::sample> hdr;
  texture2d<float, access::write> raw;
  texture2d<float, access::read> clearcoat;
  texture2d<uint, access::write> hit;
};

struct alignas(16) VkrMetalPacketSsrTemporalRoot {
  VkrSsrParams params;
  texture2d<float, access::read> raw;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::read> depth;
  texture2d<float, access::read> normal;
  texture2d<float, access::read> history_color;
  texture2d<float, access::read> history_depth;
  texture2d<uint, access::read> history_identity;
  texture2d<float, access::write> output_color;
  texture2d<float, access::write> output_depth;
  texture2d<uint, access::write> output_identity;
  device VkrGpuVisibleDrawRow *visible_rows;
  device VkrMetalPacketInstance *instances;
  texture2d<float, access::read> specular;
  texture2d<float, access::read> clearcoat;
  texture2d<uint, access::read> hit;
  device VkrTemporalTransform *previous_transforms;
  constant VkrSsrReprojectionParams *reprojection;
  uint previous_frame_index;
  uint reserved;
};

struct alignas(16) VkrMetalPacketSsrCompositeRoot {
  constant VkrMetalPacketFrameRoot *frame;
  VkrSsrParams params;
  texture2d<float, access::read_write> hdr;
  texture2d<float, access::read> history_color;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::read> depth;
  texture2d<float, access::read> albedo;
  texture2d<float, access::read> specular;
  texture2d<float, access::read> normal;
  texture2d<float, access::sample> gtao_visibility;
  float4x4 inverse_view_projection;
  uint2 extent;
  uint2 reserved;
  texture2d<float, access::read> clearcoat;
  texture2d<float, access::read> sheen;
  texture2d<float, access::read> anisotropy;
};

static float vkr_metal_ssr_positive_depth(VkrSsrParams params, uint2 pixel,
                                          float device_depth,
                                          uint2 extent) {
  float3 view = vkr_ssr_reconstruct_view_position(
      params, vkr_ssr_uv_from_pixel(pixel, extent), device_depth);
  return vkr_ssr_valid_depth(-view.z) ? -view.z : 0.0f;
}

static float3 vkr_metal_ssr_selected_normal(
    texture2d<float, access::read> normal,
    texture2d<float, access::read> clearcoat, uint2 pixel) {
  float4 packed = clearcoat.read(pixel);
  return vkr_clearcoat_active(packed.x)
             ? vkr_metal_packet_octahedral_decode(packed.zw)
             : vkr_metal_packet_octahedral_decode(normal.read(pixel).xy);
}

static float vkr_metal_ssr_selected_roughness(
    texture2d<float, access::read> specular,
    texture2d<float, access::read> clearcoat, uint2 pixel) {
  float4 packed = clearcoat.read(pixel);
  return vkr_clearcoat_active(packed.x)
             ? clamp(packed.y, 0.04f, 1.0f)
             : clamp(specular.read(pixel).w, 0.04f, 1.0f);
}

static float3 vkr_metal_ssr_hdr_cone(
    texture2d<float, access::sample> hdr, float2 hit_uv, float roughness,
    VkrSsrParams params) {
  uint tap_count = vkr_ssr_hit_filter_tap_count(roughness);
  float3 result = 0.0f;
  for (uint tap = 0u; tap < tap_count; ++tap)
    result += hdr.sample(vkr_metal_ssr_linear_sampler,
                         vkr_ssr_hit_filter_uv(hit_uv, roughness, tap, params))
                  .rgb;
  return result / float(tap_count);
}

kernel void vkr_metal_packet_ssr_depth_base(
    constant VkrMetalPacketSsrDepthBaseRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  uint2 depth_extent = uint2(root.pyramid.get_width(), root.pyramid.get_height());
  if (any(pixel >= depth_extent))
    return;
  uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
  uint begin_x = vkr_ssr_reduction_child_begin(pixel.x);
  uint begin_y = vkr_ssr_reduction_child_begin(pixel.y);
  uint end_x = vkr_ssr_reduction_child_end(pixel.x, depth_extent.x,
                                            source_extent.x);
  uint end_y = vkr_ssr_reduction_child_end(pixel.y, depth_extent.y,
                                            source_extent.y);
  float values[9] = {0.0f};
  uint count = 0u;
  for (uint y = begin_y; y < end_y; ++y) {
    for (uint x = begin_x; x < end_x; ++x) {
      uint2 child = uint2(x, y);
      float value = root.vbuffer.read(child).x != 0u
                        ? vkr_metal_ssr_positive_depth(root.params, child,
                            root.depth.read(child).x, source_extent)
                        : 0.0f;
      values[count++] = value;
    }
  }
  root.pyramid.write(float4(vkr_ssr_depth_reduce(
                         values[0], values[1], values[2], values[3], values[4],
                         values[5], values[6], values[7], values[8], count),
                     0.0f, 0.0f, 1.0f),
                     pixel);
}

kernel void vkr_metal_packet_ssr_depth_mip(
    constant VkrMetalPacketSsrDepthMipRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.destination_extent))
    return;
  uint begin_x = vkr_ssr_reduction_child_begin(pixel.x);
  uint begin_y = vkr_ssr_reduction_child_begin(pixel.y);
  uint end_x = vkr_ssr_reduction_child_end(pixel.x, root.destination_extent.x,
                                            root.source_extent.x);
  uint end_y = vkr_ssr_reduction_child_end(pixel.y, root.destination_extent.y,
                                            root.source_extent.y);
  float values[9] = {0.0f};
  uint count = 0u;
  for (uint y = begin_y; y < end_y; ++y)
    for (uint x = begin_x; x < end_x; ++x)
      values[count++] = root.source.read(uint2(x, y)).x;
  root.destination.write(float4(vkr_ssr_depth_reduce(
                             values[0], values[1], values[2], values[3],
                             values[4], values[5], values[6], values[7],
                             values[8], count),
                         0.0f, 0.0f, 1.0f),
                         pixel);
}

kernel void vkr_metal_packet_ssr_trace(
    constant VkrMetalPacketSsrTraceRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
  if (any(pixel >= trace_extent))
    return;
  uint2 receiver_pixel = pixel;
  if (root.vbuffer.read(receiver_pixel).x == 0u) {
    root.raw.write(float4(0.0f), pixel);
    root.hit.write(uint4(0u), pixel);
    return;
  }
  uint2 source_extent =
      uint2(root.params.source_width, root.params.source_height);
  float roughness = vkr_metal_ssr_selected_roughness(
      root.specular, root.clearcoat, receiver_pixel);
  if (!vkr_ssr_eligible(roughness, true, root.params)) {
    root.raw.write(float4(0.0f), pixel);
    root.hit.write(uint4(0u), pixel);
    return;
  }
  float device_depth = root.depth.read(receiver_pixel).x;
  float3 origin = vkr_ssr_reconstruct_view_position(
      root.params, vkr_ssr_uv_from_pixel(receiver_pixel, source_extent),
      device_depth);
  if (!vkr_ssr_valid_depth(-origin.z)) {
    root.raw.write(float4(0.0f), pixel);
    root.hit.write(uint4(0u), pixel);
    return;
  }
  float3 world_normal = vkr_metal_ssr_selected_normal(
      root.normal, root.clearcoat, receiver_pixel);
  float3 view_normal =
      normalize((root.params.view * float4(world_normal, 0.0f)).xyz);
  if (dot(view_normal, -origin) < 0.0f)
    view_normal = -view_normal;
  float3 direction = reflect(normalize(origin), view_normal);
  VkrSsrTrace trace = vkr_ssr_trace_begin(
      root.params, origin + view_normal * VKR_SSR_RAY_ORIGIN_BIAS, direction,
      max(root.params.depth_mip_count, 1u));
  // Start at the receiver's full-resolution cell; ascend only as cells exit.
  // Logical level one is the existing half-resolution pyramid base.
  trace.mip = 0u;
  float3 hit_radiance = 0.0f;
  float confidence = 0.0f;
  uint4 hit = uint4(0u);
  for (uint step = 0u; step < root.params.max_steps && trace.active != 0u;
       ++step) {
    uint2 mip_extent = trace.mip == 0u
        ? source_extent
        : uint2(root.pyramid.get_width(trace.mip - 1u),
                 root.pyramid.get_height(trace.mip - 1u));
    float cell_exit = vkr_ssr_trace_cell_exit(trace, mip_extent, root.params);
    float2 trace_uv = vkr_ssr_trace_uv(trace);
    uint2 source_pixel = min(uint2(floor(trace_uv * float2(source_extent))),
                             source_extent - 1u);
    float surface_depth;
    if (trace.mip == 0u) {
      surface_depth = vkr_metal_ssr_positive_depth(
          root.params, source_pixel, root.depth.read(source_pixel).x, source_extent);
    } else {
      uint2 cell = min(source_pixel >> trace.mip, mip_extent - 1u);
      surface_depth = root.pyramid.read(cell, trace.mip - 1u).x;
    }
    VkrSsrTraceResolve resolve = vkr_ssr_trace_resolve(
        trace, surface_depth, cell_exit, root.params);
    if (resolve.hit == 0u) {
      trace = resolve.descend != 0u
                  ? vkr_ssr_trace_descend(trace)
                  : vkr_ssr_trace_advance(trace, cell_exit);
      continue;
    }

    // A leaf represents exactly the pixel whose depth was loaded. A crossing
    // on its excluded exit boundary belongs to the next cell.
    float ray_depth = vkr_ssr_trace_depth_at(trace, resolve.hit_t);
    if (!vkr_ssr_trace_hit_matches_pixel(trace, resolve.hit_t, source_pixel,
                                         root.params) ||
        !vkr_ssr_depth_hit_matches(ray_depth, surface_depth, root.params)) {
      trace = vkr_ssr_trace_advance(trace, cell_exit);
      continue;
    }
    uint hit_visible = root.vbuffer.read(source_pixel).x;
    if (hit_visible == 0u) {
      trace = vkr_ssr_trace_advance(trace, cell_exit);
      continue;
    }
    float3 hit_world_normal = vkr_metal_ssr_selected_normal(
        root.normal, root.clearcoat, source_pixel);
    float3 hit_view_normal =
        normalize((root.params.view * float4(hit_world_normal, 0.0f)).xyz);
    if (dot(hit_view_normal, -direction) > 1e-4f) {
      float2 hit_uv = mix(trace.start_uv, trace.end_uv, resolve.hit_t);
      hit_radiance =
          vkr_metal_ssr_hdr_cone(root.hdr, hit_uv, roughness, root.params);
      confidence = vkr_ssr_trace_confidence(roughness, hit_uv,
                                             resolve.depth_confidence,
                                             root.params);
      hit =
          uint4(as_type<uint2>(hit_uv), as_type<uint>(ray_depth), hit_visible);
      break;
    }
    trace = vkr_ssr_trace_advance(trace, cell_exit);
  }
  root.raw.write(float4(hit_radiance, confidence), pixel);
  root.hit.write(hit, pixel);
}

static float vkr_metal_ssr_filtered_roughness(
    texture2d<float, access::read> normals,
    texture2d<float, access::read> coats,
    texture2d<uint, access::read> vbuffer, uint2 extent, uint2 pixel,
    uint visible_index, float3 normal, float roughness) {
  uint2 limit = extent - 1u;
  uint2 pixel_x = min(pixel + uint2(1u, 0u), limit);
  uint2 pixel_y = min(pixel + uint2(0u, 1u), limit);
  float3 normal_x = vkr_metal_ssr_selected_normal(
      normals, coats, pixel_x);
  float3 normal_y = vkr_metal_ssr_selected_normal(
      normals, coats, pixel_y);
  float same_x = vbuffer.read(pixel_x).x == visible_index ? 1.0f : 0.0f;
  float same_y = vbuffer.read(pixel_y).x == visible_index ? 1.0f : 0.0f;
  float3 dx = (normal_x - normal) * same_x;
  float3 dy = (normal_y - normal) * same_y;
  return vkr_ggx_filter_roughness(
      roughness, 0.25f * (dot(dx, dx) + dot(dy, dy)));
}

kernel void vkr_metal_packet_ssr_temporal(
    constant VkrMetalPacketSsrTemporalRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
  uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
  if (any(pixel >= source_extent))
    return;
  uint visible = root.vbuffer.read(pixel).x;
  if (visible == 0u) {
    root.output_color.write(float4(0.0f), pixel);
    root.output_depth.write(float4(0.0f), pixel);
    root.output_identity.write(uint4(0u), pixel);
    return;
  }
  float2 current_uv = vkr_ssr_uv_from_pixel(pixel, source_extent);
  float3 receiver_view = vkr_ssr_reconstruct_view_position(
      root.params, current_uv, root.depth.read(pixel).x);
  float center_depth = -receiver_view.z;
  float4 coat = root.clearcoat.read(pixel);
  bool coat_active = vkr_clearcoat_active(coat.x);
  float roughness =
      clamp(coat_active ? coat.y : root.specular.read(pixel).w, 0.04f, 1.0f);
  if (!vkr_ssr_eligible(roughness, true, root.params) ||
      !vkr_ssr_valid_depth(center_depth)) {
    root.output_color.write(float4(0.0f), pixel);
    root.output_depth.write(float4(0.0f), pixel);
    root.output_identity.write(uint4(0u), pixel);
    return;
  }
  const device VkrMetalPacketInstance &receiver_instance =
      root.instances[root.visible_rows[visible - 1u].instance_index];
  uint4 identity(receiver_instance.temporal_index + 1u,
                 receiver_instance.temporal_generation, 0u, 0u);
  float3 center_normal = coat_active
      ? vkr_metal_packet_octahedral_decode(coat.zw)
      : vkr_metal_packet_octahedral_decode(root.normal.read(pixel).xy);
  float3 receiver_normal_view = normalize(
      vkr_ssr_matrix_vector(root.params.view, float4(center_normal, 0.0f)).xyz);
  float2 trace_coordinate = float2(pixel);
  bool mirror = roughness <= VKR_SSR_MIRROR_ROUGHNESS;
  // Retain the half-resolution gather's physical width with full-source rays.
  int gather_stride = mirror ? 1 : 2;
  int2 gather_first = mirror ? int2(floor(trace_coordinate))
                            : int2(floor(trace_coordinate + 0.5f)) - gather_stride;
  int gather_count = mirror ? 2 : 3;
  float4 filtered = 0.0f;
  float weight_sum = 0.0f;
  float3 neighborhood_min = float3(3.402823466e+38f);
  float3 neighborhood_max = float3(-3.402823466e+38f);
  uint neighborhood_hits = 0u;
  float4 dominant_contribution = 0.0f;
  uint2 dominant_pixel = uint2(0u);
  uint traced_visible = 0u;
  float3 traced_receiver_view = 0.0f;
  float3 traced_normal_world = 0.0f;
  for (int y = 0; y < gather_count; ++y) {
    for (int x = 0; x < gather_count; ++x) {
      int2 signed_neighbor = gather_first + int2(x, y) * gather_stride;
      if (any(signed_neighbor < 0) || any(signed_neighbor >= int2(trace_extent)))
        continue;
      uint2 neighbor = uint2(signed_neighbor);
      float2 offset = (float2(neighbor) - trace_coordinate) / float(gather_stride);
      float grid_weight = vkr_ssr_reconstruction_grid_weight(offset, roughness);
      if (grid_weight <= 0.0f)
        continue;
      uint2 receiver = neighbor;
      uint neighbor_visible = root.vbuffer.read(receiver).x;
      if (neighbor_visible == 0u)
        continue;
      float3 neighbor_view = vkr_ssr_reconstruct_view_position(
          root.params, vkr_ssr_uv_from_pixel(receiver, source_extent),
          root.depth.read(receiver).x);
      float neighbor_depth =
          vkr_ssr_valid_depth(-neighbor_view.z) ? -neighbor_view.z : 0.0f;
      float3 neighbor_normal = vkr_metal_ssr_selected_normal(
          root.normal, root.clearcoat, receiver);
      float weight = grid_weight * vkr_ssr_receiver_bilateral_weight(
          offset, center_depth, neighbor_depth, center_normal, neighbor_normal,
          VKR_SSR_RECEIVER_DEPTH_ABSOLUTE);
      if (weight <= 0.0f)
        continue;
      float4 sample = root.raw.read(neighbor);
      float4 contribution = vkr_ssr_spatial_sample(sample, weight);
      filtered += contribution;
      weight_sum += weight;
      if (sample.w > 0.0f) {
        ++neighborhood_hits;
        neighborhood_min = min(neighborhood_min, sample.rgb);
        neighborhood_max = max(neighborhood_max, sample.rgb);
        if (vkr_ssr_reprojection_sample_dominates(
                contribution, dominant_contribution)) {
          dominant_contribution = contribution;
          dominant_pixel = neighbor;
          traced_visible = neighbor_visible;
          traced_receiver_view = neighbor_view;
          traced_normal_world = neighbor_normal;
        }
      }
    }
  }
  float4 raw = vkr_ssr_spatial_resolve(filtered, weight_sum);
  VkrSsrReflectedHit hit = {};
  float current_virtual_depth = 0.0f;
  float3 previous_receiver_normal_view = 0.0f;
  bool current_correspondence = false;
  uint4 dominant_hit =
      dominant_contribution.w > 0.0f ? root.hit.read(dominant_pixel) : uint4(0u);
  float hit_depth = as_type<float>(dominant_hit.z);
  if (dominant_hit.w != 0u && vkr_ssr_valid_depth(hit_depth)) {
    const device VkrMetalPacketInstance &hit_instance =
        root.instances[root.visible_rows[dominant_hit.w - 1u].instance_index];
    identity.zw = uint2(hit_instance.temporal_index + 1u,
                        hit_instance.temporal_generation);
    float3 hit_view = vkr_ssr_view_from_positive_depth(
        root.params, as_type<float2>(dominant_hit.xy), hit_depth);
    float3 traced_normal_view =
        normalize(vkr_ssr_matrix_vector(root.params.view,
                                        float4(traced_normal_world, 0.0f))
                      .xyz);
    current_virtual_depth = vkr_ssr_virtual_hit_depth(
        traced_receiver_view, traced_normal_view, hit_view);
    const device VkrMetalPacketInstance &traced_instance =
        root.instances[root.visible_rows[traced_visible - 1u].instance_index];
    current_correspondence =
        traced_instance.temporal_index == receiver_instance.temporal_index &&
        traced_instance.temporal_generation ==
            receiver_instance.temporal_generation &&
        receiver_instance.temporal_index < VKR_TEMPORAL_TRANSFORM_CAPACITY &&
        hit_instance.temporal_index < VKR_TEMPORAL_TRANSFORM_CAPACITY &&
        receiver_instance.temporal_generation != 0u &&
        hit_instance.temporal_generation != 0u &&
        vkr_ssr_valid_depth(current_virtual_depth);
    if (current_correspondence && root.params.history_valid != 0u) {
      const device VkrTemporalTransform &previous_receiver =
          root.previous_transforms[receiver_instance.temporal_index];
      const device VkrTemporalTransform &previous_hit =
          root.previous_transforms[hit_instance.temporal_index];
      if (previous_receiver.valid != 0u && previous_hit.valid != 0u &&
          previous_receiver.generation ==
              receiver_instance.temporal_generation &&
          previous_hit.generation == hit_instance.temporal_generation &&
          previous_receiver.frame_index == root.previous_frame_index &&
          previous_hit.frame_index == root.previous_frame_index) {
        float3 traced_world =
            vkr_ssr_matrix_vector(root.reprojection->inverse_view,
                                  float4(traced_receiver_view, 1.0f))
                .xyz;
        float3 receiver_world =
            vkr_ssr_matrix_vector(root.reprojection->inverse_view,
                                  float4(receiver_view, 1.0f))
                .xyz;
        float3 hit_world =
            vkr_ssr_matrix_vector(root.reprojection->inverse_view,
                                  float4(hit_view, 1.0f))
                .xyz;
        VkrSsrSurface prior_trace = vkr_ssr_transport_surface(
            traced_world, traced_normal_world, receiver_instance.model,
            previous_receiver.model);
        VkrSsrSurface prior_receiver = vkr_ssr_transport_surface(
            receiver_world, center_normal, receiver_instance.model,
            previous_receiver.model);
        float4 prior_hit = vkr_ssr_transport_point(
            hit_world, hit_instance.model, previous_hit.model);
        if (prior_trace.valid != 0u && prior_receiver.valid != 0u &&
            prior_hit.w != 0.0f) {
          float3 previous_trace_view =
              vkr_ssr_matrix_vector(root.reprojection->previous_view,
                                    float4(prior_trace.position, 1.0f))
                  .xyz;
          float3 previous_trace_normal_view =
              normalize(vkr_ssr_matrix_vector(root.reprojection->previous_view,
                                              float4(prior_trace.normal, 0.0f))
                            .xyz);
          float3 previous_hit_view =
              vkr_ssr_matrix_vector(root.reprojection->previous_view,
                                    float4(prior_hit.xyz, 1.0f))
                  .xyz;
          float3 previous_receiver_view =
              vkr_ssr_matrix_vector(root.reprojection->previous_view,
                                    float4(prior_receiver.position, 1.0f))
                  .xyz;
          previous_receiver_normal_view = normalize(
              vkr_ssr_matrix_vector(root.reprojection->previous_view,
                                    float4(prior_receiver.normal, 0.0f))
                  .xyz);
          hit = vkr_ssr_reproject_hit(
              root.params, current_uv, traced_receiver_view, traced_normal_view,
              hit_view, previous_trace_view, previous_trace_normal_view,
              previous_hit_view, previous_receiver_view,
              previous_receiver_normal_view);
        }
      }
    }
  }
  float4 history_sum = 0.0f;
  float history_support = 0.0f;
  if (hit.valid != 0u) {
    float2 half_texel = 0.5f / float2(source_extent);
    float2 coordinate = clamp(hit.previous_uv, half_texel, 1.0f - half_texel) *
                            float2(source_extent) -
                        0.5f;
    int2 first = int2(floor(coordinate));
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) {
        int2 p = first + int2(x, y);
        if (any(p < 0) || any(p >= int2(source_extent)))
          continue;
        float weight = vkr_ssr_history_tap_weight(coordinate, p);
        if (weight <= 0.0f)
          continue;
        VkrSsrHistoryDecision tap = vkr_ssr_reflected_history_accept(
            root.params, hit, identity, root.history_identity.read(uint2(p)),
            root.history_depth.read(uint2(p)), previous_receiver_normal_view,
            source_extent);
        if (tap.accepted == 0u)
          continue;
        history_sum += vkr_ssr_spatial_sample(
            root.history_color.read(uint2(p)), weight);
        history_support += weight;
      }
    }
  }
  float4 history = vkr_ssr_spatial_resolve(history_sum, history_support);
  VkrSsrHistoryDecision decision;
  decision.accepted = history_support > 0.0f ? 1u : 0u;
  float2 reflected_motion =
      hit.previous_uv - current_uv -
      float2(root.params.history_jitter_uv_x, root.params.history_jitter_uv_y);
  decision.weight =
      decision.accepted != 0u
          ? vkr_ssr_temporal_weight(root.params.temporal_weight, roughness,
                                    reflected_motion * float2(source_extent))
          : 0.0f;
  decision.expected_previous_depth = 0.0f;
  decision.reserved_float_0 = 0.0f;
  root.output_color.write(vkr_ssr_temporal_filter(
      roughness, neighborhood_hits, raw, history, neighborhood_min,
      neighborhood_max, decision), pixel);
  root.output_depth.write(
      current_correspondence
          ? float4(center_depth, current_virtual_depth,
                   vkr_ssr_encode_normal(receiver_normal_view))
          : float4(0.0f),
      pixel);
  root.output_identity.write(current_correspondence ? identity : uint4(0u),
                             pixel);
}

static float3 vkr_metal_ssr_world_position(
    constant VkrMetalPacketSsrCompositeRoot &root, uint2 pixel,
    float device_depth) {
  float2 ndc = vkr_metal_packet_resolve_ndc(float2(pixel) + 0.5f, root.extent);
  float4 world = root.inverse_view_projection * float4(ndc, device_depth, 1.0f);
  return world.xyz / max(abs(world.w), 1e-7f) * sign(world.w);
}


kernel void vkr_metal_packet_ssr_composite(
    constant VkrMetalPacketSsrCompositeRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent))
    return;
  uint visible = root.vbuffer.read(pixel).x;
  if (visible == 0u)
    return;
  float3 base_normal =
      vkr_metal_packet_octahedral_decode(root.normal.read(pixel).xy);
  float4 specular = root.specular.read(pixel);
  float4 clearcoat_packed = root.clearcoat.read(pixel);
  bool clearcoat_active = vkr_clearcoat_active(clearcoat_packed.x);
  float3 normal = clearcoat_active
                      ? vkr_metal_packet_octahedral_decode(clearcoat_packed.zw)
                      : base_normal;
  float material_roughness = clearcoat_active
                                 ? clamp(clearcoat_packed.y, 0.04f, 1.0f)
                                 : clamp(specular.w, 0.04f, 1.0f);
  // Match trace eligibility; raster-dependent normal variance only broadens
  // BRDF weights.
  if (!vkr_ssr_eligible(material_roughness, true, root.params))
    return;
  float4 reflection = root.history_color.read(pixel);
  /* Preserve an untouched HDR texel on a trace/upscale miss. */
  if (reflection.w <= 0.0f)
    return;
  float3 world_position =
      vkr_metal_ssr_world_position(root, pixel, root.depth.read(pixel).x);
  float3 view = normalize(root.frame->view_position.xyz - world_position);
  float occlusion = saturate(root.albedo.read(pixel).a);
  constexpr sampler gtao_sampler(coord::normalized, address::clamp_to_edge,
                                 filter::nearest);
  float4 gtao = root.gtao_visibility.sample(
      gtao_sampler, (float2(pixel) + 0.5f) / float2(root.extent));
  float gtao_visibility = vkr_gtao_decode_visibility(gtao);
  float3 opaque = root.hdr.read(pixel).rgb;
  if (!clearcoat_active) {
    uint2 limit = root.extent - 1u;
    uint2 px = min(pixel + uint2(1u, 0u), limit);
    uint2 py = min(pixel + uint2(0u, 1u), limit);
    float3 dx = (vkr_metal_packet_octahedral_decode(root.normal.read(px).xy) - base_normal) *
        (root.vbuffer.read(px).x == visible ? 1.0f : 0.0f);
    float3 dy = (vkr_metal_packet_octahedral_decode(root.normal.read(py).xy) - base_normal) *
        (root.vbuffer.read(py).x == visible ? 1.0f : 0.0f);
    float roughness = vkr_ggx_filter_roughness(clamp(specular.w, 0.04f, 1.0f),
        0.25f * (dot(dx, dx) + dot(dy, dy)));
    float3 gtao_bent_normal = vkr_gtao_decode_bent_normal(gtao, base_normal);
    float gtao_specular_cone = vkr_gtao_cone_specular(
        gtao_visibility, gtao_bent_normal, reflect(-view, base_normal), roughness);
    VkrGgxMaterialEnergy energy = vkr_metal_prepare_gbuffer_brdf(
        root.frame, normal, view, roughness,
        saturate(specular.rgb), root.anisotropy.read(pixel));
    VkrMetalPacketEnvironmentLighting environment =
        vkr_metal_packet_environment_lighting(
            root.frame, world_position, normal, normal, view, roughness,
            occlusion, gtao_specular_cone, energy, false);
    float sheen_base_transmission = 1.0f;
    float4 sheen_packed = root.sheen.read(pixel);
    if (vkr_sheen_active(sheen_packed.rgb))
      sheen_base_transmission = vkr_metal_packet_prepare_sheen(
          root.frame, sheen_packed.rgb, sheen_packed.a, normal, view)
                                    .base_transmission;
    float3 old_specular = environment.incoming_specular *
                          environment.specular_receiver_weight *
                          sheen_base_transmission;
    float3 shaded_reflection = vkr_ssr_shade_reflection(
        reflection, environment.ssr_receiver_weight).rgb * sheen_base_transmission;
    root.hdr.write(
        float4(vkr_ssr_replace_environment_specular(
                   opaque, old_specular, shaded_reflection, reflection.w),
               1.0f),
        pixel);
    return;
  }
  VkrClearcoatLayer clearcoat = vkr_metal_packet_prepare_clearcoat(
      root.frame, clearcoat_packed.x, clearcoat_packed.y, normal, view);
  float gtao_specular_cone = vkr_gtao_cone_specular(
      gtao_visibility, vkr_gtao_decode_bent_normal(gtao, clearcoat.normal),
      reflect(-view, clearcoat.normal), clearcoat.roughness);
  VkrMetalPacketEnvironmentLighting coat_environment =
      vkr_metal_packet_environment_lighting(
          root.frame, world_position, clearcoat.normal, clearcoat.normal, view,
          clearcoat.roughness, occlusion, gtao_specular_cone, clearcoat.energy,
          false);
  float3 old_coat = clearcoat.factor * coat_environment.incoming_specular *
                    coat_environment.specular_receiver_weight;
  float filtered_roughness = vkr_metal_ssr_filtered_roughness(
      root.normal, root.clearcoat, root.vbuffer, root.extent, pixel, visible,
      normal, material_roughness);
  VkrClearcoatLayer filtered_coat = vkr_metal_packet_prepare_clearcoat(
      root.frame, clearcoat_packed.x, filtered_roughness, normal, view);
  float filtered_cone = vkr_gtao_cone_specular(
      gtao_visibility, vkr_gtao_decode_bent_normal(gtao, filtered_coat.normal),
      reflect(-view, filtered_coat.normal), filtered_coat.roughness);
  reflection.rgb *= filtered_coat.factor;
  float3 shaded_reflection = vkr_ssr_shade_reflection(
      reflection, vkr_metal_environment_receiver_weight(
          reflect(-view, filtered_coat.normal), filtered_coat.normal,
          filtered_coat.roughness, occlusion, filtered_cone,
          filtered_coat.energy)).rgb;
  root.hdr.write(float4(vkr_ssr_replace_environment_specular(
                            opaque, old_coat, shaded_reflection, reflection.w),
                        1.0f),
                 pixel);
}

static_assert(sizeof(VkrMetalPacketSsrDepthBaseRoot) == 320,
              "Metal SSR depth-base root ABI drift");
static_assert(sizeof(VkrMetalPacketSsrDepthMipRoot) == 320,
              "Metal SSR depth-mip root ABI drift");
static_assert(sizeof(VkrMetalPacketSsrTraceRoot) == 368,
              "Metal SSR trace root ABI drift");
static_assert(sizeof(VkrMetalPacketSsrTemporalRoot) == 432,
              "Metal SSR temporal root ABI drift");
static_assert(sizeof(VkrMetalPacketSsrCompositeRoot) == 480,
              "Metal SSR composite root ABI drift");
