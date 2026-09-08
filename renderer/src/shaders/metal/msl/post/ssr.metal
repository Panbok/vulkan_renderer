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
  texture2d<float, access::read> history_depth;
  texture2d<uint, access::read> receiver;
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
    texture2d<uint, access::read> vbuffer, uint2 receiver_pixel,
    device VkrGpuVisibleDrawRow *visible_rows,
    device VkrMetalPacketInstance *instances) {
  uint encoded = vbuffer.read(receiver_pixel).x;
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
  float radius = vkr_ssr_hit_filter_radius_pixels(roughness, params);
  float2 texel = float2(1.0f / float(params.source_width),
                        1.0f / float(params.source_height));
  float3 result = 0.0f;
  for (uint tap = 0u; tap < tap_count; ++tap)
    result += hdr.sample(vkr_metal_ssr_linear_sampler,
                         hit_uv + vkr_ssr_hit_filter_offset(tap) * radius *
                                      texel)
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
      max(root.params.depth_mip_count, 1u) - 1u);
  float3 hit_radiance = 0.0f;
  float confidence = 0.0f;
  for (uint step = 0u; step < root.params.max_steps && trace.active != 0u;
       ++step) {
    uint2 mip_extent = uint2(root.pyramid.get_width(trace.mip),
                             root.pyramid.get_height(trace.mip));
    float cell_exit =
        vkr_ssr_trace_cell_exit(trace, mip_extent, root.params);
    float2 trace_uv = vkr_ssr_trace_uv(trace);
    uint2 source_pixel = min(uint2(floor(trace_uv * float2(source_extent))),
                             source_extent - 1u);
    uint2 cell = min(source_pixel >> (trace.mip + 1u), mip_extent - 1u);
    VkrSsrTraceResolve resolve = vkr_ssr_trace_resolve(
        trace, root.pyramid.read(cell, trace.mip).x, cell_exit, root.params);
    if (resolve.hit == 0u) {
      trace = resolve.descend != 0u
                  ? vkr_ssr_trace_descend(trace)
                  : vkr_ssr_trace_advance(trace, cell_exit);
      continue;
    }

    float2 hit_uv = mix(trace.start_uv, trace.end_uv, resolve.hit_t);
    uint2 hit_pixel = min(uint2(floor(hit_uv * float2(source_extent))),
                          source_extent - 1u);
    float hit_depth = vkr_metal_ssr_positive_depth(
        root.params, hit_pixel, root.depth.read(hit_pixel).x, source_extent);
    float ray_depth = vkr_ssr_trace_depth_at(trace, resolve.hit_t);
    float3 hit_world_normal = vkr_metal_ssr_selected_normal(
        root.normal, root.clearcoat, hit_pixel);
    float3 hit_view_normal =
        normalize((root.params.view * float4(hit_world_normal, 0.0f)).xyz);
    const bool covered = root.vbuffer.read(hit_pixel).x != 0u;
    const bool depth_matches = vkr_ssr_valid_depth(hit_depth) &&
                               vkr_ssr_valid_depth(ray_depth) &&
                               abs(hit_depth - ray_depth) <=
                                   root.params.thickness;
    const bool front_facing = dot(hit_view_normal, -direction) > 1e-4f;
    if (covered && depth_matches && front_facing) {
      hit_radiance =
          vkr_metal_ssr_hdr_cone(root.hdr, hit_uv, roughness, root.params);
      confidence = vkr_ssr_trace_confidence(roughness, hit_uv,
                                             resolve.depth_confidence,
                                             root.params);
      break;
    }
    /* A hierarchy candidate only proposes a hit. Advancing after an invalid
     * leaf prevents an uncovered/back-facing/self surface from sampling HDR. */
    trace = vkr_ssr_trace_advance(trace, cell_exit);
  }
  root.raw.write(float4(hit_radiance, confidence), pixel);
}

kernel void vkr_metal_packet_ssr_temporal(
    constant VkrMetalPacketSsrTemporalRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
  if (any(pixel >= trace_extent))
    return;
  uint2 receiver_pixel = root.receiver.read(pixel).xy;
  if (any(receiver_pixel == uint2(0xffffffffu))) {
    root.output_color.write(float4(0.0f), pixel);
    root.output_depth.write(float4(0.0f), pixel);
    root.output_identity.write(uint4(0u), pixel);
    return;
  }
  uint2 source_extent =
      uint2(root.params.source_width, root.params.source_height);
  uint2 identity = vkr_metal_ssr_receiver_identity(
      root.vbuffer, receiver_pixel, root.visible_rows, root.instances);
  float center_depth = vkr_metal_ssr_positive_depth(
      root.params, receiver_pixel, root.depth.read(receiver_pixel).x,
      source_extent);
  float4 raw = root.raw.read(pixel);
  float roughness = vkr_metal_ssr_selected_roughness(
      root.specular, root.clearcoat, receiver_pixel);
  if (roughness > VKR_SSR_MIRROR_ROUGHNESS) {
    float3 center_normal = vkr_metal_ssr_selected_normal(
        root.normal, root.clearcoat, receiver_pixel);
    float3 filtered = 0.0f;
    float weight_sum = 0.0f;
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        int2 signed_pixel = int2(pixel) + int2(x, y);
        if (any(signed_pixel < 0))
          continue;
        uint2 neighbor_pixel = uint2(signed_pixel);
        if (any(neighbor_pixel >= trace_extent))
          continue;
        uint2 neighbor_receiver = root.receiver.read(neighbor_pixel).xy;
        if (any(neighbor_receiver == uint2(0xffffffffu)))
          continue;
        float neighbor_depth = vkr_metal_ssr_positive_depth(
            root.params, neighbor_receiver,
            root.depth.read(neighbor_receiver).x, source_extent);
        float3 neighbor_normal = vkr_metal_ssr_selected_normal(
            root.normal, root.clearcoat, neighbor_receiver);
        float weight = vkr_ssr_receiver_bilateral_weight(
            float2(x, y), center_depth, neighbor_depth, center_normal,
            neighbor_normal, root.params);
        filtered += root.raw.read(neighbor_pixel).rgb * weight;
        weight_sum += weight;
      }
    }
    if (weight_sum > 0.0f)
      raw.rgb = filtered / weight_sum;
  }
  if (root.params.history_valid == 0u) {
    root.output_color.write(float4(raw.rgb, saturate(raw.w)), pixel);
    root.output_depth.write(float4(center_depth, 0.0f, 0.0f, 1.0f), pixel);
    root.output_identity.write(uint4(identity, 0u, 0u), pixel);
    return;
  }
  float2 current_uv = vkr_ssr_uv_from_pixel(receiver_pixel, source_extent);
  float2 previous_uv = current_uv + root.motion.read(receiver_pixel).xy;
  float2 half_texel = float2(root.params.trace_texel_size_x,
                             root.params.trace_texel_size_y) *
                      0.5f;
  float2 history_uv = clamp(previous_uv, half_texel, 1.0f - half_texel);
  uint2 previous_pixel =
      min(uint2(floor(history_uv * float2(trace_extent))), trace_extent - 1u);
  VkrSsrHistoryDecision decision = vkr_ssr_temporal_accept(
      root.params, identity, root.history_identity.read(previous_pixel).xy,
      root.history_depth.read(previous_pixel).x,
      root.validity.read(receiver_pixel).y, previous_uv,
      root.validity.read(receiver_pixel).x);
  float3 neighborhood_min = raw.rgb;
  float3 neighborhood_max = raw.rgb;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      int2 p = int2(pixel) + int2(x, y);
      if (all(p >= 0) && all(uint2(p) < trace_extent)) {
        float3 sample = root.raw.read(uint2(p)).rgb;
        neighborhood_min = min(neighborhood_min, sample);
        neighborhood_max = max(neighborhood_max, sample);
      }
    }
  /* Identity/depth select one discrete source. Linear filtering here would
   * blend radiance from a different object after the metadata proof. */
  float4 history = root.history_color.read(previous_pixel);
  root.output_color.write(vkr_ssr_temporal_filter(
                              root.params, raw, history, neighborhood_min,
                              neighborhood_max, decision),
                          pixel);
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

static float vkr_metal_ssr_filtered_roughness(
    constant VkrMetalPacketSsrCompositeRoot &root, uint2 pixel,
    uint visible_index, float3 normal, float roughness) {
  uint2 limit = root.extent - 1u;
  uint2 pixel_x = min(pixel + uint2(1u, 0u), limit);
  uint2 pixel_y = min(pixel + uint2(0u, 1u), limit);
  float3 normal_x = vkr_metal_ssr_selected_normal(
      root.normal, root.clearcoat, pixel_x);
  float3 normal_y = vkr_metal_ssr_selected_normal(
      root.normal, root.clearcoat, pixel_y);
  float same_x = root.vbuffer.read(pixel_x).x == visible_index ? 1.0f : 0.0f;
  float same_y = root.vbuffer.read(pixel_y).x == visible_index ? 1.0f : 0.0f;
  float3 dx = (normal_x - normal) * same_x;
  float3 dy = (normal_y - normal) * same_y;
  return vkr_ggx_filter_roughness(
      roughness, 0.25f * (dot(dx, dx) + dot(dy, dy)));
}

struct VkrMetalSsrReflectionSample {
  float3 radiance;
  float confidence;
};

static VkrMetalSsrReflectionSample vkr_metal_ssr_history_reflection(
    constant VkrMetalPacketSsrCompositeRoot &root, uint2 pixel,
    float current_depth, float3 current_normal) {
  VkrMetalSsrReflectionSample result;
  result.radiance = float3(0.0f);
  result.confidence = 0.0f;
  if (!vkr_ssr_valid_depth(current_depth))
    return result;
  uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
  float2 trace_coordinate =
      (float2(pixel) + 0.5f) / float2(root.extent) * float2(trace_extent) -
      0.5f;
  int2 first = int2(floor(trace_coordinate));
  float accepted_weight = 0.0f;
  float support_weight = 0.0f;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      int2 signed_trace_pixel = first + int2(x, y);
      if (any(signed_trace_pixel < 0))
        continue;
      uint2 trace_pixel = uint2(signed_trace_pixel);
      if (any(trace_pixel >= trace_extent))
        continue;
      float2 offset = float2(trace_pixel) - trace_coordinate;
      float bilinear = max(1.0f - abs(offset.x), 0.0f) *
                       max(1.0f - abs(offset.y), 0.0f);
      uint2 receiver_pixel = root.receiver.read(trace_pixel).xy;
      if (any(receiver_pixel == uint2(0xffffffffu)))
        continue;
      float sample_depth = root.history_depth.read(trace_pixel).x;
      float3 sample_normal = vkr_metal_ssr_selected_normal(
          root.normal, root.clearcoat, receiver_pixel);
      float receiver_weight = bilinear * vkr_ssr_receiver_bilateral_weight(
          offset, current_depth, sample_depth, current_normal, sample_normal,
          root.params);
      if (receiver_weight <= 0.0f)
        continue;
      /* Support excludes out-of-bounds/incompatible receivers but retains
       * valid zero-confidence taps, preventing miss black from diluting a hit. */
      support_weight += receiver_weight;
      float4 sample = root.history_color.read(trace_pixel);
      float weighted_confidence = receiver_weight * saturate(sample.w);
      result.radiance += sample.rgb * weighted_confidence;
      accepted_weight += weighted_confidence;
    }
  }
  if (accepted_weight <= 0.0f || support_weight <= 0.0f)
    return result;
  result.radiance /= accepted_weight;
  result.confidence = saturate(accepted_weight / support_weight);
  return result;
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
  float roughness = vkr_metal_ssr_filtered_roughness(
      root, pixel, visible, normal,
      clearcoat_active ? clamp(clearcoat_packed.y, 0.04f, 1.0f)
                       : clamp(specular.w, 0.04f, 1.0f));
  if (!vkr_ssr_eligible(roughness, true, root.params))
    return;
  float current_depth = vkr_metal_ssr_positive_depth(
      root.params, pixel, root.depth.read(pixel).x, root.extent);
  VkrMetalSsrReflectionSample reflection =
      vkr_metal_ssr_history_reflection(root, pixel, current_depth, normal);
  /* Preserve an untouched HDR texel on a trace/upscale miss. */
  if (reflection.confidence <= 0.0f)
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
  float3 gtao_bent_normal = vkr_gtao_decode_bent_normal(gtao, normal);
  float gtao_specular_cone = vkr_gtao_cone_specular(
      gtao_visibility, gtao_bent_normal, reflect(-view, normal), roughness);
  float3 opaque = root.hdr.read(pixel).rgb;
  if (!clearcoat_active) {
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
                                opaque, old_specular, reflection.radiance,
                                environment.ssr_receiver_weight *
                                    sheen_base_transmission,
                                reflection.confidence),
                            1.0f),
                   pixel);
    return;
  }
  VkrClearcoatLayer clearcoat = vkr_metal_packet_prepare_clearcoat(
      root.frame, clearcoat_packed.x, roughness, normal, view);
  VkrMetalPacketEnvironmentLighting coat_environment =
      vkr_metal_packet_environment_lighting(
          root.frame, world_position, clearcoat.normal, clearcoat.normal, view,
          clearcoat.roughness, occlusion, gtao_specular_cone, clearcoat.energy,
          false);
  float3 old_coat = clearcoat.factor * coat_environment.incoming_specular *
                    coat_environment.specular_receiver_weight;
  root.hdr.write(float4(vkr_ssr_replace_environment_specular(
                              opaque, old_coat, reflection.radiance,
                              clearcoat.factor * coat_environment.ssr_receiver_weight,
                              reflection.confidence),
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
static_assert(sizeof(VkrMetalPacketSsrCompositeRoot) == 496,
              "Metal SSR composite root ABI drift");
