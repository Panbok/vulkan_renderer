constexpr sampler vkr_metal_ssr_linear_sampler(coord::normalized,
                                                address::clamp_to_edge,
                                                filter::linear,
                                                mip_filter::none);

struct alignas(16) VkrMetalPacketSsrDepthBaseRoot {
  VkrSsrParams params;
  texture2d<float, access::read> depth;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::write> pyramid;
  texture2d<uint, access::write> receiver;
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
  texture2d<uint, access::read> receiver;
  texture2d<float, access::sample> hdr;
  texture2d<float, access::write> raw;
  texture2d<float, access::read> clearcoat;
};

struct alignas(16) VkrMetalPacketSsrTemporalRoot {
  VkrSsrParams params;
  texture2d<float, access::read> raw;
  texture2d<uint, access::read> receiver;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::read> depth;
  texture2d<float, access::read> normal;
  texture2d<float, access::read> motion;
  texture2d<float, access::read> validity;
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
  constant VkrMetalPacketFrameRoot *frame;
  texture2d<float, access::read> albedo;
  texture2d<float, access::sample> gtao_visibility;
  texture2d<float, access::read> sheen;
  texture2d<float, access::read> anisotropy;
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

static uint2 vkr_metal_ssr_receiver_identity(
    uint encoded, device VkrGpuVisibleDrawRow *visible_rows,
    device VkrMetalPacketInstance *instances) {
  if (encoded == 0u)
    return uint2(0u);
  const device VkrGpuVisibleDrawRow &visible = visible_rows[encoded - 1u];
  const device VkrMetalPacketInstance &instance =
      instances[visible.instance_index];
  return uint2(instance.temporal_index + 1u, instance.temporal_generation);
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
  uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
  if (any(pixel >= trace_extent))
    return;
  uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
  uint begin_x = vkr_ssr_reduction_child_begin(pixel.x);
  uint begin_y = vkr_ssr_reduction_child_begin(pixel.y);
  uint end_x = vkr_ssr_reduction_child_end(pixel.x, trace_extent.x,
                                            source_extent.x);
  uint end_y = vkr_ssr_reduction_child_end(pixel.y, trace_extent.y,
                                            source_extent.y);
  float values[9] = {0.0f};
  uint count = 0u;
  float nearest = 3.402823466e+38f;
  uint2 selected = uint2(0xffffffffu);
  for (uint y = begin_y; y < end_y; ++y) {
    for (uint x = begin_x; x < end_x; ++x) {
      uint2 child = uint2(x, y);
      float value = root.vbuffer.read(child).x != 0u
                        ? vkr_metal_ssr_positive_depth(root.params, child,
                            root.depth.read(child).x, source_extent)
                        : 0.0f;
      values[count++] = value;
      if (vkr_ssr_valid_depth(value) && value < nearest) {
        nearest = value;
        selected = child;
      }
    }
  }
  root.pyramid.write(float4(vkr_ssr_depth_reduce(
                         values[0], values[1], values[2], values[3], values[4],
                         values[5], values[6], values[7], values[8], count),
                     0.0f, 0.0f, 1.0f),
                     pixel);
  root.receiver.write(uint4(selected, 0u, 0u), pixel);
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
  uint2 receiver_pixel = root.receiver.read(pixel).xy;
  if (any(receiver_pixel == uint2(0xffffffffu)) ||
      root.vbuffer.read(receiver_pixel).x == 0u) {
    root.raw.write(float4(0.0f), pixel);
    return;
  }
  uint2 source_extent =
      uint2(root.params.source_width, root.params.source_height);
  float roughness = vkr_metal_ssr_selected_roughness(
      root.specular, root.clearcoat, receiver_pixel);
  if (!vkr_ssr_eligible(roughness, true, root.params)) {
    root.raw.write(float4(0.0f), pixel);
    return;
  }
  float device_depth = root.depth.read(receiver_pixel).x;
  float3 origin = vkr_ssr_reconstruct_view_position(
      root.params, vkr_ssr_uv_from_pixel(receiver_pixel, source_extent),
      device_depth);
  if (!vkr_ssr_valid_depth(-origin.z)) {
    root.raw.write(float4(0.0f), pixel);
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
    if (!vkr_ssr_trace_hit_matches_pixel(trace, resolve.hit_t, source_pixel, root.params) ||
        !vkr_ssr_depth_hit_matches(ray_depth, surface_depth, root.params) ||
        root.vbuffer.read(source_pixel).x == 0u) {
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
      break;
    }
    trace = vkr_ssr_trace_advance(trace, cell_exit);
  }
  root.raw.write(float4(hit_radiance, confidence), pixel);
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

static float3 vkr_metal_ssr_shading_weight(
    constant VkrMetalPacketSsrTemporalRoot &root, uint2 receiver, uint visible,
    float device_depth, float3 normal, float4 specular, float4 coat,
    float material_roughness) {
  uint2 extent(root.params.source_width, root.params.source_height);
  float roughness = vkr_metal_ssr_filtered_roughness(
      root.normal, root.clearcoat, root.vbuffer, extent, receiver, visible,
      normal, material_roughness);
  float3 view_position = vkr_ssr_reconstruct_view_position(
      root.params, vkr_ssr_uv_from_pixel(receiver, extent), device_depth);
  float3 view = normalize((transpose(root.params.view) *
                          float4(-view_position, 0.0f)).xyz);
  float occlusion = saturate(root.albedo.read(receiver).a);
  constexpr sampler gtao_sampler(coord::normalized, address::clamp_to_edge,
                                 filter::nearest);
  float4 gtao = root.gtao_visibility.sample(
      gtao_sampler, (float2(receiver) + 0.5f) / float2(extent));
  if (vkr_clearcoat_active(coat.x)) {
    VkrClearcoatLayer layer = vkr_metal_packet_prepare_clearcoat(
        root.frame, coat.x, roughness, normal, view);
    float cone = vkr_gtao_cone_specular(
        vkr_gtao_decode_visibility(gtao),
        vkr_gtao_decode_bent_normal(gtao, layer.normal),
        reflect(-view, layer.normal), layer.roughness);
    return layer.factor * vkr_metal_environment_receiver_weight(
        reflect(-view, layer.normal), layer.normal, layer.roughness,
        occlusion, cone, layer.energy);
  }
  float cone = vkr_gtao_cone_specular(
      vkr_gtao_decode_visibility(gtao), vkr_gtao_decode_bent_normal(gtao, normal),
      reflect(-view, normal), roughness);
  VkrGgxMaterialEnergy energy = vkr_metal_prepare_gbuffer_brdf(
      root.frame, normal, view, roughness, saturate(specular.rgb),
      root.anisotropy.read(receiver));
  float3 reflection = reflect(-view,
      vkr_anisotropy_environment_normal(normal, view, energy));
  float3 weight = vkr_metal_environment_receiver_weight(
      reflection, normal, vkr_anisotropy_environment_roughness(roughness, energy),
      occlusion, cone, energy);
  float4 sheen = root.sheen.read(receiver);
  if (vkr_sheen_active(sheen.rgb))
    weight *= vkr_metal_packet_prepare_sheen(
        root.frame, sheen.rgb, sheen.a, normal, view).base_transmission;
  return weight;
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
  float device_depth = root.depth.read(pixel).x;
  float center_depth = vkr_metal_ssr_positive_depth(
      root.params, pixel, device_depth, source_extent);
  float4 coat = root.clearcoat.read(pixel);
  bool coat_active = vkr_clearcoat_active(coat.x);
  float4 specular = coat_active ? float4(0.0f) : root.specular.read(pixel);
  float roughness = clamp(coat_active ? coat.y : specular.w, 0.04f, 1.0f);
  if (!vkr_ssr_eligible(roughness, true, root.params) ||
      !vkr_ssr_valid_depth(center_depth)) {
    root.output_color.write(float4(0.0f), pixel);
    root.output_depth.write(float4(0.0f), pixel);
    root.output_identity.write(uint4(0u), pixel);
    return;
  }
  uint2 identity = vkr_metal_ssr_receiver_identity(
      visible, root.visible_rows, root.instances);
  float3 center_normal = coat_active
      ? vkr_metal_packet_octahedral_decode(coat.zw)
      : vkr_metal_packet_octahedral_decode(root.normal.read(pixel).xy);
  float2 current_uv = vkr_ssr_uv_from_pixel(pixel, source_extent);
  float2 trace_coordinate = current_uv * float2(trace_extent) - 0.5f;
  bool mirror = roughness <= VKR_SSR_MIRROR_ROUGHNESS;
  int2 gather_first = mirror ? int2(floor(trace_coordinate))
                            : int2(floor(trace_coordinate + 0.5f)) - 1;
  int gather_count = mirror ? 2 : 3;
  float4 filtered = 0.0f;
  float weight_sum = 0.0f;
  float3 neighborhood_min = float3(3.402823466e+38f);
  float3 neighborhood_max = float3(-3.402823466e+38f);
  uint neighborhood_hits = 0u;
  for (int y = 0; y < gather_count; ++y) {
    for (int x = 0; x < gather_count; ++x) {
      int2 signed_neighbor = gather_first + int2(x, y);
      if (any(signed_neighbor < 0) || any(signed_neighbor >= int2(trace_extent)))
        continue;
      uint2 neighbor = uint2(signed_neighbor);
      float2 offset = float2(neighbor) - trace_coordinate;
      float grid_weight = vkr_ssr_reconstruction_grid_weight(offset, roughness);
      if (grid_weight <= 0.0f)
        continue;
      uint2 receiver = root.receiver.read(neighbor).xy;
      if (any(receiver >= source_extent))
        continue;
      float neighbor_depth = vkr_metal_ssr_positive_depth(
          root.params, receiver, root.depth.read(receiver).x, source_extent);
      float3 neighbor_normal = vkr_metal_ssr_selected_normal(
          root.normal, root.clearcoat, receiver);
      float weight = grid_weight * vkr_ssr_receiver_bilateral_weight(
          offset, center_depth, neighbor_depth, center_normal, neighbor_normal,
          VKR_SSR_RECEIVER_DEPTH_ABSOLUTE);
      if (weight <= 0.0f)
        continue;
      float4 sample = root.raw.read(neighbor);
      filtered += vkr_ssr_spatial_sample(sample, weight);
      weight_sum += weight;
      if (sample.w > 0.0f) {
        ++neighborhood_hits;
        neighborhood_min = min(neighborhood_min, sample.rgb);
        neighborhood_max = max(neighborhood_max, sample.rgb);
      }
    }
  }
  float3 receiver_weight = vkr_metal_ssr_shading_weight(
      root, pixel, visible, device_depth, center_normal, specular, coat, roughness);
  float4 raw = vkr_ssr_shade_reflection(
      vkr_ssr_spatial_resolve(filtered, weight_sum), receiver_weight);
  if (neighborhood_hits > 0u) {
    neighborhood_min *= receiver_weight;
    neighborhood_max *= receiver_weight;
  }
  float4 history_sum = 0.0f;
  float history_support = 0.0f;
  float2 motion = root.motion.read(pixel).xy;
  if (root.params.history_valid != 0u) {
    float2 previous_uv = vkr_ssr_previous_uv(root.params, current_uv, motion);
    float2 half_texel = 0.5f / float2(source_extent);
    float2 coordinate = clamp(previous_uv, half_texel, 1.0f - half_texel) *
                            float2(source_extent) - 0.5f;
    int2 first = int2(floor(coordinate));
    float2 validity = root.validity.read(pixel).xy;
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) {
        int2 p = first + int2(x, y);
        if (any(p < 0) || any(p >= int2(source_extent)))
          continue;
        float weight = vkr_ssr_history_tap_weight(coordinate, p);
        if (weight <= 0.0f)
          continue;
        VkrSsrHistoryDecision tap = vkr_ssr_temporal_accept(
            root.params, identity, root.history_identity.read(uint2(p)).xy,
            root.history_depth.read(uint2(p)).x, validity.y, previous_uv,
            validity.x, source_extent);
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
  decision.weight = decision.accepted != 0u
      ? vkr_ssr_temporal_weight(root.params.temporal_weight, roughness,
                                 motion * float2(source_extent)) : 0.0f;
  decision.expected_previous_depth = 0.0f;
  decision.reserved_float_0 = 0.0f;
  root.output_color.write(vkr_ssr_temporal_filter(
      roughness, neighborhood_hits, raw, history, neighborhood_min,
      neighborhood_max, decision), pixel);
  root.output_depth.write(float4(center_depth, 0.0f, 0.0f, 1.0f), pixel);
  root.output_identity.write(uint4(identity, 0u, 0u), pixel);
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
    root.hdr.write(float4(vkr_ssr_replace_environment_specular(
                                opaque, old_specular, reflection.rgb,
                                reflection.w),
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
  root.hdr.write(float4(vkr_ssr_replace_environment_specular(
                              opaque, old_coat, reflection.rgb,
                              reflection.w),
                          1.0f),
                 pixel);
}

static_assert(sizeof(VkrMetalPacketSsrDepthBaseRoot) == 320,
              "Metal SSR depth-base root ABI drift");
static_assert(sizeof(VkrMetalPacketSsrDepthMipRoot) == 320,
              "Metal SSR depth-mip root ABI drift");
static_assert(sizeof(VkrMetalPacketSsrTraceRoot) == 368,
              "Metal SSR trace root ABI drift");
static_assert(sizeof(VkrMetalPacketSsrTemporalRoot) == 464,
              "Metal SSR temporal root ABI drift");
static_assert(sizeof(VkrMetalPacketSsrCompositeRoot) == 480,
              "Metal SSR composite root ABI drift");
