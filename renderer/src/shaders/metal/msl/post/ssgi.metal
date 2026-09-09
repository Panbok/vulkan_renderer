// Metal SSGI compute passes. Portable hierarchy and estimator math is supplied
// by ssgi_kernel.slangh, concatenated after ssr_kernel.slangh by the library.
constexpr sampler vkr_metal_ssgi_linear_sampler(coord::normalized,
                                                 address::clamp_to_edge,
                                                 filter::linear,
                                                 mip_filter::none);

struct alignas(16) VkrMetalPacketSsgiDepthBaseRoot {
  VkrSsgiParams params;
  texture2d<float, access::read> depth;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::write> pyramid;
};
struct alignas(16) VkrMetalPacketSsgiDepthMipRoot {
  VkrSsgiParams params;
  texture2d<float, access::read> source;
  texture2d<float, access::write> destination;
  uint2 source_extent;
  uint2 destination_extent;
};
struct alignas(16) VkrMetalPacketSsgiTraceRoot {
  VkrSsgiParams params;
  texture2d<float, access::read> depth;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::read> normal;
  texture2d<float, access::read> albedo;
  texture2d<float, access::read> pyramid;
  texture2d<float, access::read> direct_source;
  texture2d<float, access::write> raw;
};
struct alignas(16) VkrMetalPacketSsgiTemporalRoot {
  VkrSsgiParams params;
  texture2d<float, access::read> raw;
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
};
struct alignas(16) VkrMetalPacketSsgiCompositeRoot {
  constant VkrMetalPacketFrameRoot *frame;
  VkrSsgiParams params;
  texture2d<float, access::read_write> hdr;
  texture2d<float, access::read> history_color;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::read> depth;
  texture2d<float, access::read> albedo;
  texture2d<float, access::read> normal;
  texture2d<float, access::read> history_depth;
  texture2d<float, access::read> specular;
  float4x4 inverse_view_projection;
  uint2 extent;
  uint subsurface_profile_count;
  uint reserved;
  texture2d<float, access::read> clearcoat;
  texture2d<float, access::read> sheen;
  texture2d<float, access::read> anisotropy;
  uint2 visible_rows_reserved;
  device VkrGpuVisibleDrawRow *visible_rows;
  texture2d<float, access::read_write> subsurface_source;
};

static float vkr_metal_ssgi_positive_depth(VkrSsrParams screen, uint2 pixel,
                                           float device_depth, uint2 extent) {
  float3 view = vkr_ssr_reconstruct_view_position(
      screen, vkr_ssr_uv_from_pixel(pixel, extent), device_depth);
  return vkr_ssr_valid_depth(-view.z) ? -view.z : 0.0f;
}

static VkrSsgiReceiver vkr_metal_ssgi_receiver(
    VkrSsrParams screen, uint2 trace_pixel, uint2 trace_extent,
    uint2 source_extent, texture2d<uint, access::read> vbuffer,
    texture2d<float, access::read> depth) {
  VkrSsgiReceiver result = vkr_ssgi_empty_receiver();
  uint begin_x = vkr_ssr_reduction_child_begin(trace_pixel.x);
  uint begin_y = vkr_ssr_reduction_child_begin(trace_pixel.y);
  uint end_x = vkr_ssr_reduction_child_end(trace_pixel.x, trace_extent.x,
                                            source_extent.x);
  uint end_y = vkr_ssr_reduction_child_end(trace_pixel.y, trace_extent.y,
                                            source_extent.y);
  for (uint y = begin_y; y < end_y; ++y)
    for (uint x = begin_x; x < end_x; ++x) {
      uint2 child(x, y);
      uint covered = vbuffer.read(child).x != 0u ? 1u : 0u;
      float value = covered != 0u
          ? vkr_metal_ssgi_positive_depth(screen, child, depth.read(child).x,
                                           source_extent)
          : 0.0f;
      result = vkr_ssgi_consider_receiver(result, child, value, covered);
    }
  return result;
}

static uint2 vkr_metal_ssgi_identity(
    constant VkrMetalPacketSsgiTemporalRoot &root, uint2 receiver_pixel) {
  uint encoded = root.vbuffer.read(receiver_pixel).x;
  if (encoded == 0u)
    return uint2(0u);
  const device VkrGpuVisibleDrawRow &visible = root.visible_rows[encoded - 1u];
  const device VkrMetalPacketInstance &instance =
      root.instances[visible.instance_index];
  return uint2(instance.temporal_index + 1u, instance.temporal_generation);
}

static float3 vkr_metal_ssgi_view_normal(VkrSsgiParams params,
                                         texture2d<float, access::read> normal,
                                         uint2 pixel, float3 origin) {
  float3 world_normal = vkr_metal_packet_octahedral_decode(normal.read(pixel).xy);
  float3 view_normal = normalize((params.view * float4(world_normal, 0.0f)).xyz);
  return vkr_ssgi_faceforward_view_normal(view_normal, origin);
}

kernel void vkr_metal_packet_ssgi_depth_base(
    constant VkrMetalPacketSsgiDepthBaseRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  uint2 trace_extent(root.params.trace_width, root.params.trace_height);
  if (any(pixel >= trace_extent))
    return;
  uint2 source_extent(root.params.source_width, root.params.source_height);
  VkrSsrParams screen = vkr_ssgi_screen_trace_params(root.params);
  uint begin_x = vkr_ssr_reduction_child_begin(pixel.x);
  uint begin_y = vkr_ssr_reduction_child_begin(pixel.y);
  uint end_x = vkr_ssr_reduction_child_end(pixel.x, trace_extent.x, source_extent.x);
  uint end_y = vkr_ssr_reduction_child_end(pixel.y, trace_extent.y, source_extent.y);
  float values[9] = {0.0f};
  uint count = 0u;
  for (uint y = begin_y; y < end_y; ++y)
    for (uint x = begin_x; x < end_x; ++x) {
      uint2 child(x, y);
      if (root.vbuffer.read(child).x != 0u)
        values[count] = vkr_metal_ssgi_positive_depth(screen, child,
                                                       root.depth.read(child).x,
                                                       source_extent);
      ++count;
    }
  root.pyramid.write(float4(vkr_ssr_depth_reduce(
                         values[0], values[1], values[2], values[3], values[4],
                         values[5], values[6], values[7], values[8], count),
                     0.0f, 0.0f, 1.0f), pixel);
}

kernel void vkr_metal_packet_ssgi_depth_mip(
    constant VkrMetalPacketSsgiDepthMipRoot &root [[buffer(0)]],
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
                             values[0], values[1], values[2], values[3], values[4],
                             values[5], values[6], values[7], values[8], count),
                         0.0f, 0.0f, 1.0f), pixel);
}

kernel void vkr_metal_packet_ssgi_trace(
    constant VkrMetalPacketSsgiTraceRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  uint2 trace_extent(root.params.trace_width, root.params.trace_height);
  if (any(pixel >= trace_extent))
    return;
  uint2 source_extent(root.params.source_width, root.params.source_height);
  VkrSsrParams screen = vkr_ssgi_screen_trace_params(root.params);
  VkrSsgiReceiver receiver = vkr_metal_ssgi_receiver(
      screen, pixel, trace_extent, source_extent, root.vbuffer, root.depth);
  if (receiver.valid == 0u || saturate(root.albedo.read(receiver.pixel).a) <= 0.0f) {
    root.raw.write(float4(0.0f), pixel);
    return;
  }
  float device_depth = root.depth.read(receiver.pixel).x;
  float3 origin = vkr_ssr_reconstruct_view_position(
      screen, vkr_ssr_uv_from_pixel(receiver.pixel, source_extent), device_depth);
  float3 normal = vkr_metal_ssgi_view_normal(root.params, root.normal,
                                              receiver.pixel, origin);
  float3 direction = vkr_ssgi_cosine_direction(
      vkr_ssgi_sample_2d(receiver.pixel, root.params.sample_phase), normal);
  VkrSsrTrace trace = vkr_ssr_trace_begin(
      screen, origin + normal * root.params.normal_bias, direction,
      max(root.params.depth_mip_count, 1u) - 1u);
  float3 estimate(0.0f);
  for (uint step = 0u; step < root.params.max_steps && trace.active != 0u; ++step) {
    uint2 mip_extent(root.pyramid.get_width(trace.mip), root.pyramid.get_height(trace.mip));
    float cell_exit = vkr_ssgi_trace_cell_exit(trace, mip_extent, screen);
    float2 trace_uv = vkr_ssr_trace_uv(trace);
    uint2 source_pixel = min(uint2(floor(trace_uv * float2(source_extent))), source_extent - 1u);
    uint2 cell = min(source_pixel >> (trace.mip + 1u), mip_extent - 1u);
    VkrSsrTraceResolve resolve = vkr_ssgi_trace_resolve(
        trace, root.pyramid.read(cell, trace.mip).x, cell_exit, screen);
    if (resolve.hit == 0u) {
      trace = resolve.descend != 0u ? vkr_ssr_trace_descend(trace)
                                     : vkr_ssr_trace_advance(trace, cell_exit);
      continue;
    }
    float2 hit_uv = vkr_ssr_lerp(trace.start_uv, trace.end_uv, resolve.hit_t);
    uint2 hit_pixel = min(uint2(floor(hit_uv * float2(source_extent))), source_extent - 1u);
    float hit_device_depth = root.depth.read(hit_pixel).x;
    float hit_depth = vkr_metal_ssgi_positive_depth(screen, hit_pixel,
                                                     hit_device_depth, source_extent);
    float ray_depth = vkr_ssr_trace_depth_at(trace, resolve.hit_t);
    float3 hit_view = vkr_ssr_reconstruct_view_position(
        screen, vkr_ssr_uv_from_pixel(hit_pixel, source_extent), hit_device_depth);
    float3 hit_normal = vkr_metal_ssgi_view_normal(root.params, root.normal,
                                                    hit_pixel, hit_view);
    uint covered = root.vbuffer.read(hit_pixel).x != 0u ? 1u : 0u;
    if (vkr_ssgi_hit_valid(covered, hit_depth, ray_depth, hit_normal,
                           direction, root.params)) {
      estimate = vkr_ssgi_bare_diffuse_estimate(root.direct_source.read(hit_pixel).rgb) *
                 vkr_ssgi_trace_confidence(hit_uv, screen);
      break;
    }
    trace = vkr_ssr_trace_advance(trace, cell_exit);
  }
  // A trace miss is a zero-valued but valid one-ray sample for this receiver.
  root.raw.write(float4(estimate, 1.0f), pixel);
}

kernel void vkr_metal_packet_ssgi_temporal(
    constant VkrMetalPacketSsgiTemporalRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  uint2 trace_extent(root.params.trace_width, root.params.trace_height);
  if (any(pixel >= trace_extent))
    return;
  uint2 source_extent(root.params.source_width, root.params.source_height);
  VkrSsrParams screen = vkr_ssgi_screen_trace_params(root.params);
  VkrSsgiReceiver receiver = vkr_metal_ssgi_receiver(
      screen, pixel, trace_extent, source_extent, root.vbuffer, root.depth);
  if (receiver.valid == 0u) {
    root.output_color.write(float4(0.0f), pixel);
    root.output_depth.write(float4(0.0f), pixel);
    root.output_identity.write(uint4(0u), pixel);
    return;
  }
  float4 raw = root.raw.read(pixel);
  uint2 identity = vkr_metal_ssgi_identity(root, receiver.pixel);
  float center_device_depth = root.depth.read(receiver.pixel).x;
  float3 center_origin = vkr_ssr_reconstruct_view_position(
      screen, vkr_ssr_uv_from_pixel(receiver.pixel, source_extent),
      center_device_depth);
  float3 center_normal = vkr_metal_ssgi_view_normal(
      root.params, root.normal, receiver.pixel, center_origin);
  float3 neighborhood_sum(0.0f);
  float3 neighborhood_square_sum(0.0f);
  float neighborhood_weight = 0.0f;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      int2 neighbor = int2(pixel) + int2(x, y);
      if (all(neighbor >= 0) && all(uint2(neighbor) < trace_extent)) {
        uint2 neighbor_pixel = uint2(neighbor);
        VkrSsgiReceiver neighbor_receiver = vkr_metal_ssgi_receiver(
            screen, neighbor_pixel, trace_extent, source_extent, root.vbuffer,
            root.depth);
        if (neighbor_receiver.valid == 0u)
          continue;
        float4 sample = root.raw.read(neighbor_pixel);
        if (sample.w <= 0.0f)
          continue;
        float neighbor_device_depth = root.depth.read(neighbor_receiver.pixel).x;
        float3 neighbor_origin = vkr_ssr_reconstruct_view_position(
            screen, vkr_ssr_uv_from_pixel(neighbor_receiver.pixel, source_extent),
            neighbor_device_depth);
        float3 neighbor_normal = vkr_metal_ssgi_view_normal(
            root.params, root.normal, neighbor_receiver.pixel, neighbor_origin);
        float weight = vkr_ssgi_spatial_weight(
            float2(x, y), receiver.positive_depth,
            neighbor_receiver.positive_depth, center_normal, neighbor_normal,
            screen);
        if (weight <= 0.0f)
          continue;
        // A trace miss is sample.rgb == 0 with sample.w == 1 and must
        // participate in this unbiased local Monte-Carlo average.
        neighborhood_sum += sample.rgb * weight;
        neighborhood_square_sum += sample.rgb * sample.rgb * weight;
        neighborhood_weight += weight;
      }
    }
  if (neighborhood_weight > 0.0f)
    raw.rgb = neighborhood_sum / neighborhood_weight;
  if (root.params.history_valid != 0u) {
    float3 neighborhood_mean = neighborhood_sum / neighborhood_weight;
    float3 neighborhood_variance = max(
        neighborhood_square_sum / neighborhood_weight -
            neighborhood_mean * neighborhood_mean, float3(0.0f));
    float2 current_uv = vkr_ssr_uv_from_pixel(receiver.pixel, source_extent);
    float2 previous_uv = current_uv + root.motion.read(receiver.pixel).xy;
    float2 half_texel(root.params.trace_texel_size_x * 0.5f,
                      root.params.trace_texel_size_y * 0.5f);
    float2 history_uv = clamp(previous_uv, half_texel, 1.0f - half_texel);
    uint2 history_pixel = min(uint2(floor(history_uv * float2(trace_extent))),
                              trace_extent - 1u);
    VkrSsrHistoryDecision decision = vkr_ssr_temporal_accept(
        screen, identity, root.history_identity.read(history_pixel).xy,
        root.history_depth.read(history_pixel).x,
        root.validity.read(receiver.pixel).y, previous_uv,
        root.validity.read(receiver.pixel).x);
    raw = vkr_ssgi_temporal_filter(raw,
                                   root.history_color.read(history_pixel),
                                   neighborhood_mean, neighborhood_variance, decision);
  }
  root.output_color.write(raw, pixel);
  root.output_depth.write(float4(receiver.positive_depth, 0.0f, 0.0f, 1.0f), pixel);
  root.output_identity.write(uint4(identity, 0u, 0u), pixel);
}

static bool vkr_metal_ssgi_volume_valid(constant VkrMetalPacketFrameRoot *frame,
                                        float3 world) {
  if (frame->diffuse_volume_params == nullptr)
    return false;
  uint3 dimensions = frame->diffuse_volume_params->dimensions.xyz;
  VkrDiffuseVolumeCell cell = vkr_diffuse_volume_cell(
      world, frame->diffuse_volume_params->origin.xyz,
      frame->diffuse_volume_params->inverse_spacing.xyz, dimensions);
  return cell.valid && frame->diffuse_volume.read(uint2(7u, cell.row)).y > 0.0f;
}

static float vkr_metal_ssgi_filtered_roughness(
    constant VkrMetalPacketSsgiCompositeRoot &root, uint2 pixel, uint visible,
    float3 normal, float roughness) {
  uint2 limit = root.extent - 1u;
  uint2 pixel_x = min(pixel + uint2(1u, 0u), limit);
  uint2 pixel_y = min(pixel + uint2(0u, 1u), limit);
  float3 normal_x = vkr_metal_packet_octahedral_decode(root.normal.read(pixel_x).xy);
  float3 normal_y = vkr_metal_packet_octahedral_decode(root.normal.read(pixel_y).xy);
  float same_x = root.vbuffer.read(pixel_x).x == visible ? 1.0f : 0.0f;
  float same_y = root.vbuffer.read(pixel_y).x == visible ? 1.0f : 0.0f;
  float3 dx = (normal_x - normal) * same_x;
  float3 dy = (normal_y - normal) * same_y;
  return vkr_ggx_filter_roughness(roughness, 0.25f * (dot(dx, dx) + dot(dy, dy)));
}

kernel void vkr_metal_packet_ssgi_composite(
    constant VkrMetalPacketSsgiCompositeRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent) || root.vbuffer.read(pixel).x == 0u)
    return;
  float device_depth = root.depth.read(pixel).x;
  float2 ndc = vkr_metal_packet_resolve_ndc(float2(pixel) + 0.5f, root.extent);
  float4 world_h = root.inverse_view_projection * float4(ndc, device_depth, 1.0f);
  float3 world = world_h.xyz / max(abs(world_h.w), 1e-7f) * sign(world_h.w);
  if (vkr_metal_ssgi_volume_valid(root.frame, world))
    return;
  VkrSsrParams screen = vkr_ssgi_screen_trace_params(root.params);
  float current_depth = vkr_metal_ssgi_positive_depth(screen, pixel, device_depth,
                                                       root.extent);
  if (!vkr_ssr_valid_depth(current_depth))
    return;
  float3 normal = vkr_metal_packet_octahedral_decode(root.normal.read(pixel).xy);
  float2 coordinate = (float2(pixel) + 0.5f) / float2(root.extent) *
                      float2(root.params.trace_width, root.params.trace_height) - 0.5f;
  int2 first = int2(floor(coordinate));
  uint2 trace_extent(root.params.trace_width, root.params.trace_height);
  float3 radiance(0.0f);
  float accepted_weight = 0.0f;
  float support_weight = 0.0f;
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x) {
      int2 signed_trace = first + int2(x, y);
      if (any(signed_trace < 0) || any(uint2(signed_trace) >= trace_extent))
        continue;
      uint2 trace_pixel = uint2(signed_trace);
      VkrSsgiReceiver receiver = vkr_metal_ssgi_receiver(
          screen, trace_pixel, trace_extent, root.extent, root.vbuffer, root.depth);
      if (receiver.valid == 0u)
        continue;
      float3 receiver_normal = vkr_metal_packet_octahedral_decode(
          root.normal.read(receiver.pixel).xy);
      float2 offset = float2(trace_pixel) - coordinate;
      float bilinear = max(1.0f - abs(offset.x), 0.0f) *
                       max(1.0f - abs(offset.y), 0.0f);
      float weight = bilinear * vkr_ssr_receiver_bilateral_weight(
          offset, current_depth, root.history_depth.read(trace_pixel).x,
          normal, receiver_normal, screen.thickness);
      if (weight <= 0.0f)
        continue;
      support_weight += weight;
      float4 sample = root.history_color.read(trace_pixel);
      radiance += sample.rgb * weight;
      accepted_weight += weight * saturate(sample.w);
    }
  if (accepted_weight <= 0.0f || support_weight <= 0.0f)
    return;
  radiance /= accepted_weight;
  float confidence = saturate(accepted_weight / support_weight);
  uint visible = root.vbuffer.read(pixel).x;
  float4 albedo = root.albedo.read(pixel);
  float4 specular = root.specular.read(pixel);
  float roughness = vkr_metal_ssgi_filtered_roughness(
      root, pixel, visible, normal, clamp(specular.w, 0.04f, 1.0f));
  float3 view = normalize(root.frame->view_position.xyz - world);
  VkrGgxMaterialEnergy energy = vkr_metal_prepare_gbuffer_brdf(
      root.frame, normal, view, roughness, saturate(specular.rgb), root.anisotropy.read(pixel));
    energy = vkr_ggx_diffuse_transmission(energy,
        root.frame->materials[root.visible_rows[visible - 1u].material_index].material_diffuse_transmission);
  float3 diffuse_weight = energy.diffuse_weight;
  float4 sheen_packed = root.sheen.read(pixel);
  if (vkr_sheen_active(sheen_packed.rgb)) {
    VkrSheenLayer sheen = vkr_metal_packet_prepare_sheen(
        root.frame, sheen_packed.rgb, sheen_packed.a, normal, view);
    diffuse_weight *= sheen.base_transmission;
  }
  float4 clearcoat_packed = root.clearcoat.read(pixel);
  if (vkr_clearcoat_active(clearcoat_packed.x)) {
    VkrClearcoatLayer clearcoat = vkr_metal_packet_prepare_clearcoat(
        root.frame, clearcoat_packed.x, clearcoat_packed.y,
        vkr_metal_packet_octahedral_decode(clearcoat_packed.zw), view);
    diffuse_weight *= clearcoat.base_transmission;
  }
  float4 scene = root.hdr.read(pixel);
  scene.rgb = vkr_ssgi_add_diffuse(scene.rgb, radiance, diffuse_weight,
                                   albedo.rgb, albedo.a, confidence);
  if (root.subsurface_profile_count != 0u) {
    float4 subsurface = root.frame->materials[root.visible_rows[visible-1u].material_index].material_subsurface;
    if (subsurface.x > 0.0f && uint(subsurface.y) < root.subsurface_profile_count) {
      float4 source = root.subsurface_source.read(pixel);
      source.rgb += vkr_subsurface_root_amplitude(albedo.rgb) * radiance * saturate(albedo.a) * saturate(confidence);
      source.rgb = vkr_subsurface_source_storage(source.rgb);
      root.subsurface_source.write(source,pixel);
    }
  }
  root.hdr.write(scene, pixel);
}
