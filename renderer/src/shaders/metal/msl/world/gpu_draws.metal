struct VkrGpuDrawCompactionState {
  uint2 execution_ranges[8];
  atomic_uint bucket_counts[8];
  atomic_uint bucket_cursors[8];
  uint visible_count;
  atomic_uint overflow_count;
  atomic_uint resolve_invalid_count;
  atomic_uint occlusion_culled_count;
};

struct VkrMetalPacketGpuDrawRoot {
  device VkrGpuCandidateDrawRow *candidates;
  device VkrGpuGeometryRow *geometry_rows;
  device VkrMetalPacketInstance *instances;
  device uint *classifications;
  device VkrGpuDrawCompactionState *compaction_state;
  device VkrGpuVisibleDrawRow *visible_rows;
  constant VkrMetalPacketDrawRoot *draw_roots;
  constant struct VkrMetalPacketGpuDrawView *views;
  uint candidate_count;
  uint visible_capacity;
  uint view_count;
  uint encode_view_index;
  texture2d<float, access::read> hzb;
  constant VkrGpuLodView *lod_views;
  float4x4 history_view_projection;
  uint2 hzb_extent;
  uint hzb_mip_count;
  uint hzb_enabled;
  float hzb_depth_epsilon;
  uint icb_view_group_size;
  uint reserved_3[2];
};

struct VkrMetalPacketGpuDrawView {
  float4 frustum_planes[6];
  uint required_candidate_flags;
  uint hzb_enabled;
  uint excluded_candidate_flags;
  // Nonzero for a retained cascade, whose pass does not run this frame.
  uint encode_idle;
};

struct VkrMetalPacketIcbContainer {
  command_buffer command_buffer [[id(0)]];
};

struct VkrMetalPacketDepthClearOutput {
  float4 position [[position]];
};

// Clears one local shadow face's atlas square: a triangle covering the
// viewport at far depth, drawn with an always-passing depth write.
vertex VkrMetalPacketDepthClearOutput
vkr_metal_packet_depth_clear_vertex(uint vertex_id [[vertex_id]]) {
  float2 corner = float2((vertex_id << 1u) & 2u, vertex_id & 2u);
  return {float4(corner * 2.0f - 1.0f, 1.0f, 1.0f)};
}

fragment void vkr_metal_packet_gpu_shadow_fragment(
    VkrMetalPacketVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]]) {
  const device VkrGpuVisibleDrawRow &visible =
      root->visible_rows[input.visible_row_index];
  const device VkrMetalPacketMaterial &material =
      root->frame->materials[visible.material_index];
  if (material.alpha_mode == 1u) {
    float alpha = material.base_color_texture
                      .sample(material.base_color_sampler, input.texcoord)
                      .a *
                  material.tint.a * input.color.a;
    if (alpha < material.material_alpha.x)
      discard_fragment();
  }
}

static bool vkr_metal_packet_candidate_in_frustum(
    constant VkrMetalPacketGpuDrawRoot &root,
    constant VkrMetalPacketGpuDrawView &view,
    const device VkrGpuCandidateDrawRow &candidate) {
  if ((vkr_gpu_draw_candidate_flags(candidate.state_flags) & 1u) == 0u)
    return true;
  const device VkrMetalPacketInstance &instance =
      root.instances[candidate.instance_index];
  float3 center =
      (instance.model * float4(candidate.local_bounding_sphere.xyz, 1.0)).xyz;
  float radius = candidate.local_bounding_sphere.w * instance.normal_column1.w;
  for (uint plane = 0u; plane < 6u; ++plane) {
    float4 equation = view.frustum_planes[plane];
    if (dot(equation.xyz, center) + equation.w < -radius)
      return false;
  }
  return true;
}

static bool vkr_metal_packet_candidate_occluded(
    constant VkrMetalPacketGpuDrawRoot &root,
    const device VkrGpuCandidateDrawRow &candidate) {
  if (root.hzb_enabled == 0u || root.hzb_mip_count == 0u ||
      any(root.hzb_extent == 0u) ||
      (vkr_gpu_draw_candidate_flags(candidate.state_flags) & 1u) == 0u)
    return false;
  const device VkrMetalPacketInstance &instance =
      root.instances[candidate.instance_index];
  float3 center =
      (instance.model * float4(candidate.local_bounding_sphere.xyz, 1.0)).xyz;
  float radius = max(candidate.local_bounding_sphere.w *
                     instance.normal_column1.w, 0.0);
  float2 ndc_min = float2(1e30);
  float2 ndc_max = float2(-1e30);
  float nearest_depth = 1.0;
  for (uint corner = 0u; corner < 8u; ++corner) {
    float3 sign_vector =
        float3((corner & 1u) ? 1.0 : -1.0, (corner & 2u) ? 1.0 : -1.0,
               (corner & 4u) ? 1.0 : -1.0);
    float4 clip = root.history_view_projection *
                  float4(center + sign_vector * radius, 1.0);
    if (!all(isfinite(clip)) || clip.w <= 1e-6)
      return false;
    float3 ndc = clip.xyz / clip.w;
    ndc_min = min(ndc_min, ndc.xy);
    ndc_max = max(ndc_max, ndc.xy);
    nearest_depth = min(nearest_depth, ndc.z);
  }
  if (nearest_depth <= 0.0 || any(ndc_min > 1.0) || any(ndc_max < -1.0))
    return false;
  float2 uv_min = clamp(float2(ndc_min.x, -ndc_max.y) * 0.5 + 0.5, 0.0, 1.0);
  float2 uv_max = clamp(float2(ndc_max.x, -ndc_min.y) * 0.5 + 0.5, 0.0, 1.0);
  uint2 base_min =
      min(uint2(floor(uv_min * float2(root.hzb_extent))), root.hzb_extent - 1u);
  uint2 base_max =
      min(uint2(floor(uv_max * float2(root.hzb_extent))), root.hzb_extent - 1u);
  uint2 span = base_max - base_min + 1u;
  uint max_span = max(span.x, span.y);
  uint mip = min(max_span > 1u ? uint(floor(log2(float(max_span)))) : 0u,
                 root.hzb_mip_count - 1u);
  uint2 mip_extent = max(root.hzb_extent >> mip, uint2(1u));
  uint2 sample_min = min(base_min >> mip, mip_extent - 1u);
  uint2 sample_max = min(base_max >> mip, mip_extent - 1u);
  for (uint y = sample_min.y; y <= sample_max.y; ++y) {
    for (uint x = sample_min.x; x <= sample_max.x; ++x) {
      if (nearest_depth <=
          root.hzb.read(uint2(x, y), mip).x + root.hzb_depth_epsilon)
        return false;
    }
  }
  return true;
}

kernel void
vkr_metal_packet_gpu_draw_classify(constant VkrMetalPacketGpuDrawRoot &root
                                   [[buffer(0)]],
                                   uint2 position [[thread_position_in_grid]]) {
  uint index = position.x;
  uint view_index = position.y;
  if (index >= root.candidate_count || view_index >= root.view_count)
    return;
  const constant VkrMetalPacketGpuDrawView &view = root.views[view_index];
  const device VkrGpuCandidateDrawRow &candidate = root.candidates[index];
  uint classification_index = view_index * root.visible_capacity + index;
  uint candidate_flags = vkr_gpu_draw_candidate_flags(candidate.state_flags);
  uint bucket = vkr_gpu_draw_state_bucket(candidate.state_flags);
  if ((candidate_flags & view.required_candidate_flags) !=
          view.required_candidate_flags ||
      (candidate_flags & view.excluded_candidate_flags) != 0u ||
      !vkr_metal_packet_candidate_in_frustum(root, view, candidate)) {
    root.classifications[classification_index] = 0u;
    return;
  }
  device VkrGpuDrawCompactionState &state = root.compaction_state[view_index];
  if (view.hzb_enabled != 0u &&
      vkr_metal_packet_candidate_occluded(root, candidate)) {
    root.classifications[classification_index] = 0u;
    atomic_fetch_add_explicit(&state.occlusion_culled_count, 1u,
                              memory_order_relaxed);
    return;
  }
  /* The LOD state rides above the bucket (ADR-084). */
  const device VkrMetalPacketInstance &instance =
      root.instances[candidate.instance_index];
  /* Sub-texel casters skip the directional cascades. */
  if (view_index != 0u && (candidate_flags & 1u) != 0u &&
      vkr_gpu_cascade_caster_too_small(
          root.lod_views[view_index].position_scale.w,
          root.lod_views[view_index].flags,
          candidate.local_bounding_sphere.w * instance.normal_column1.w)) {
    root.classifications[classification_index] = 0u;
    return;
  }
  const device VkrGpuGeometryRow &geometry =
      root.geometry_rows[candidate.geometry_index];
  /* A candidate without valid bounds has no distance and keeps level 0. */
  uint lod_state = (candidate_flags & 1u) == 0u ? 0u : vkr_gpu_lod_select(
      vkr_gpu_geometry_lod_row(geometry.decode_address, candidate.decode_index),
      root.lod_views[view_index],
      (instance.model * float4(candidate.local_bounding_sphere.xyz, 1.0)).xyz,
      candidate.local_bounding_sphere.w * instance.normal_column1.w,
      instance.normal_column1.w);
  root.classifications[classification_index] = (bucket + 1u) | lod_state;
  atomic_fetch_add_explicit(&state.bucket_counts[bucket], 1u,
                            memory_order_relaxed);
}

kernel void
vkr_metal_packet_gpu_draw_prefix(constant VkrMetalPacketGpuDrawRoot &root
                                 [[buffer(0)]],
                                 uint view_index [[thread_position_in_grid]]) {
  if (view_index >= root.view_count)
    return;
  device VkrGpuDrawCompactionState &state = root.compaction_state[view_index];
  // Commands use the candidate count; visible rows use the prepared stride
  // shared by classification, raster, resolve and picking for this frame.
  uint command_base =
      (view_index % root.icb_view_group_size) * root.candidate_count;
  uint bucket_capacity = root.visible_capacity / 8u;
  uint visible_count = 0u;
  uint overflow_count = 0u;
  for (uint bucket = 0u; bucket < 8u; ++bucket) {
    uint bucket_count = atomic_load_explicit(&state.bucket_counts[bucket],
                                             memory_order_relaxed);
    uint written_count = min(bucket_count, bucket_capacity);
    state.execution_ranges[bucket] =
        uint2(command_base + visible_count, written_count);
    visible_count += written_count;
    overflow_count += bucket_count - written_count;
    atomic_store_explicit(&state.bucket_cursors[bucket], 0u,
                          memory_order_relaxed);
  }
  state.visible_count = visible_count;
  atomic_store_explicit(&state.overflow_count, overflow_count,
                        memory_order_relaxed);
}

kernel void vkr_metal_packet_gpu_draw_encode(
    constant VkrMetalPacketGpuDrawRoot &root [[buffer(0)]],
    constant VkrMetalPacketIcbContainer *icb [[buffer(1)]],
    uint2 position [[thread_position_in_grid]]) {
  uint index = position.x;
  uint view_index = root.encode_view_index + position.y;
  if (index >= root.candidate_count || view_index >= root.view_count ||
      icb == nullptr || root.views[view_index].encode_idle != 0u)
    return;
  uint view_base = view_index * root.visible_capacity;
  uint classification = root.classifications[view_base + index];
  if (classification == 0u)
    return;
  device VkrGpuDrawCompactionState &state = root.compaction_state[view_index];
  uint bucket = (classification & VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK) - 1u;
  uint lod_state = classification & ~VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK;
  uint local_index = atomic_fetch_add_explicit(&state.bucket_cursors[bucket],
                                               1u, memory_order_relaxed);
  uint bucket_capacity = root.visible_capacity / 8u;
  if (local_index >= bucket_capacity)
    return;
  uint command_base =
      (view_index % root.icb_view_group_size) * root.candidate_count;
  uint visible_index =
      state.execution_ranges[bucket].x - command_base + local_index;

  const device VkrGpuCandidateDrawRow &candidate = root.candidates[index];
  const device VkrGpuGeometryRow &geometry =
      root.geometry_rows[candidate.geometry_index];
  /* The selected level's indices replace the range's (ADR-084). */
  uint first_index = candidate.first_index;
  uint index_count = candidate.index_count;
  uint level = vkr_gpu_draw_lod_level(lod_state);
  if (level != 0u) {
    const device VkrGpuGeometryLodRow *row = vkr_gpu_geometry_lod_row(
        geometry.decode_address, candidate.decode_index);
    first_index += row->levels[level].first_index;
    index_count = row->levels[level].index_count;
  }
  root.visible_rows[view_base + visible_index] = {
      candidate.geometry_index, candidate.material_index,
      candidate.instance_index, first_index,
      index_count,              candidate.vertex_offset,
      candidate.decode_index,   candidate.state_flags | lod_state};
  device uint *indices = reinterpret_cast<device uint *>(
      geometry.index_address + ulong(first_index) * sizeof(uint));

  render_command command(icb->command_buffer, command_base + visible_index);
  command.set_vertex_buffer(&root.draw_roots[view_index], 0u);
  command.set_fragment_buffer(&root.draw_roots[view_index], 1u);
  command.draw_indexed_primitives(primitive_type::triangle, index_count,
                                  indices, 1u, candidate.vertex_offset,
                                  visible_index);
}

static float2 vkr_metal_packet_resolve_ndc(float2 pixel, uint2 extent) {
  float2 normalized = pixel / float2(extent);
  return float2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

/* One extra terrain layer's surface from its row; its slots always hold a
   map or the white and flat defaults. */
static VkrTerrainSurface vkr_metal_packet_terrain_layer(
    texture2d<float, access::sample> base_color,
    texture2d<float, access::sample> normal,
    texture2d<float, access::sample> orm, sampler layer_sampler, float4 tint,
    float4 surface, float2 texcoord, gradient2d gradients, bool normal_mapped) {
  float3 tangent_normal = float3(0.0f, 0.0f, 1.0f);
  if (normal_mapped)
    tangent_normal = vkr_normal_map_decode(
        normal.sample(layer_sampler, texcoord, gradients).xyz, surface.z);
  return vkr_terrain_layer(base_color.sample(layer_sampler, texcoord, gradients),
                           orm.sample(layer_sampler, texcoord, gradients).rgb,
                           tangent_normal, tint, surface);
}

/* The blended surface of a terrain material: `layer0` from the common row,
   then each extra layer the weights reach. */
static VkrTerrainSurface vkr_metal_packet_terrain_surface(
    VkrTerrainSurface layer0,
    const device VkrMetalPacketTerrainMaterial &terrain, float4 weights,
    float2 texcoord, gradient2d gradients, bool normal_mapped) {
  VkrTerrainSurface sum = layer0;
  sum.base *= weights.x;
  sum.tangent_normal *= weights.x;
  sum.metallic *= weights.x;
  sum.roughness *= weights.x;
  sum.occlusion *= weights.x;
  if (weights.y > 0.0f)
    sum = vkr_terrain_accumulate(
        sum,
        vkr_metal_packet_terrain_layer(
            terrain.layer1_base_color_texture, terrain.layer1_normal_texture,
            terrain.layer1_orm_texture, terrain.layer1_sampler,
            terrain.layer1_tint, terrain.layer1_surface, texcoord, gradients,
            normal_mapped),
        weights.y);
  if (weights.z > 0.0f)
    sum = vkr_terrain_accumulate(
        sum,
        vkr_metal_packet_terrain_layer(
            terrain.layer2_base_color_texture, terrain.layer2_normal_texture,
            terrain.layer2_orm_texture, terrain.layer2_sampler,
            terrain.layer2_tint, terrain.layer2_surface, texcoord, gradients,
            normal_mapped),
        weights.z);
  if (weights.w > 0.0f)
    sum = vkr_terrain_accumulate(
        sum,
        vkr_metal_packet_terrain_layer(
            terrain.layer3_base_color_texture, terrain.layer3_normal_texture,
            terrain.layer3_orm_texture, terrain.layer3_sampler,
            terrain.layer3_tint, terrain.layer3_surface, texcoord, gradients,
            normal_mapped),
        weights.w);
  return vkr_terrain_finish(sum);
}

// The view direction through a pixel's center toward the far plane.
static float3 vkr_metal_packet_sky_direction(
    constant VkrMetalPacketFrameRoot *frame, float4x4 inverse_view_projection,
    uint2 extent, float2 pixel_center) {
  float2 ndc = vkr_metal_packet_resolve_ndc(pixel_center, extent);
  float4 far_world = inverse_view_projection * float4(ndc, 1.0, 1.0);
  return -vkr_metal_packet_view_direction(frame,
      far_world.xyz / max(abs(far_world.w), 1e-7) * sign(far_world.w));
}

// The atmosphere's transmittance from the observer along `direction`.
static float3 vkr_metal_packet_sky_view_transmittance(
    constant VkrMetalPacketSky &sky, float3 direction) {
  VkrAtmosphereParams params = sky.params.atmosphere;
  return sky.transmittance
      .sample(vkr_metal_packet_sky_sampler,
              vkr_atmosphere_transmittance_uv(
                  params, params.planet.x + params.planet.z, direction.y))
      .rgb;
}

// The sun and moon discs along `direction` above the horizon.
static float3 vkr_metal_packet_sky_discs(constant VkrMetalPacketSky &sky,
                                         float3 direction,
                                         float3 view_transmittance) {
  VkrAtmosphereParams params = sky.params.atmosphere;
  return vkr_atmosphere_sun_disc(params, direction, view_transmittance) +
         vkr_atmosphere_moon_disc(params, direction, view_transmittance);
}

// A pixel's background without the cloud layer: the fallback colour, a
// uniform radiance or the atmosphere with its glows and stars. `disc` takes
// the sun and moon discs, which clouds hide (zero outside the atmosphere and
// below the horizon).
static float3 vkr_metal_packet_sky_clear(
    constant VkrMetalPacketFrameRoot *frame,
    texture2d<float, access::sample> sky_view, float4x4 inverse_view_projection,
    uint2 extent, uint sky_mode, float4 sky_radiance, uint2 pixel,
    thread float3 &disc) {
  disc = float3(0.0);
  if (sky_mode == VKR_SKY_MODE_NONE)
    return float3(0.02, 0.02, 0.03) * frame->pre_exposure;
  if (sky_mode == VKR_SKY_MODE_CONSTANT)
    return sky_radiance.rgb;
  float3 direction = vkr_metal_packet_sky_direction(
      frame, inverse_view_projection, extent, float2(pixel) + 0.5);
  // The sky-view lookup already integrates to the top of the atmosphere, so
  // sky pixels take no aerial perspective. Its sun and moon tables add; the
  // discs and their glows are analytic; clouds carry aerial perspective at
  // their own depth.
  constant VkrMetalPacketSky &sky = *frame->sky;
  VkrAtmosphereParams params = sky.params.atmosphere;
  float3 radiance =
      sky_view
          .sample(vkr_metal_packet_sky_sampler,
                  vkr_sky_view_uv(params, direction, params.sun.xyz,
                                  VKR_ATMOSPHERE_SKY_VIEW_SUN_TABLE))
          .rgb;
  if (!vkr_atmosphere_light_dark(params.lunar))
    radiance += sky_view
                    .sample(vkr_metal_packet_sky_sampler,
                            vkr_sky_view_uv(params, direction, params.moon.xyz,
                                            VKR_ATMOSPHERE_SKY_VIEW_MOON_TABLE))
                    .rgb;
  if (!vkr_sky_view_hits_ground(params, direction.y)) {
    float3 view_transmittance =
        vkr_metal_packet_sky_view_transmittance(sky, direction);
    disc = vkr_metal_packet_sky_discs(sky, direction, view_transmittance);
    radiance += vkr_atmosphere_light_glow(params, direction, params.sun,
                                          params.solar, view_transmittance) +
                vkr_atmosphere_light_glow(params, direction, params.moon,
                                          params.lunar, view_transmittance);
    // Stars sit behind the whole atmosphere; the neighbouring pixel's ray
    // sets each star's footprint.
    if (sky.params.star_axis.w > 0.0f) {
      float3 next = vkr_metal_packet_sky_direction(
          frame, inverse_view_projection, extent,
          float2(pixel) + float2(1.5, 0.5));
      radiance += vkr_atmosphere_stars(sky.params, direction,
                                       length(cross(direction, next))) *
                  view_transmittance;
    }
  }
  return radiance;
}

// A pixel's background: the clear sky, then the cloud layer over it and the
// discs as the clouds let them through. The tiled sky and atmosphere draws
// split the same composition between them (ADR-087).
static float3 vkr_metal_packet_sky_background(
    constant VkrMetalPacketFrameRoot *frame,
    texture2d<float, access::sample> sky_view, float4x4 inverse_view_projection,
    uint2 extent, uint sky_mode, float4 sky_radiance, uint2 pixel) {
  float3 disc;
  float3 radiance =
      vkr_metal_packet_sky_clear(frame, sky_view, inverse_view_projection,
                                 extent, sky_mode, sky_radiance, pixel, disc);
  if (sky_mode != VKR_SKY_MODE_ATMOSPHERE)
    return radiance;
  constant VkrMetalPacketSky &sky = *frame->sky;
  // The cloud trace shares screen coordinates with this pass. The disc
  // follows the cloud's apparent opacity; see vkr_cloud_disc_visibility.
  if (sky.params.clouds.noise.w > 0.0f) {
    float2 uv = (float2(pixel) + 0.5) / float2(extent);
    float4 cloud = sky.cloud_radiance.sample(vkr_metal_packet_sky_sampler, uv);
    radiance = vkr_cloud_composite(radiance, cloud);
    disc *= vkr_cloud_disc_visibility(cloud.a);
  }
  return radiance + disc;
}

struct VkrMetalPacketHzbBuildRoot {
  texture2d<float, access::read> source;
  texture2d<float, access::write> destination;
  uint2 source_extent;
  uint2 destination_extent;
  uint source_is_depth;
  uint reserved[3];
};

kernel void vkr_metal_packet_hzb_build(constant VkrMetalPacketHzbBuildRoot &root
                                       [[buffer(0)]],
                                       uint2 pixel
                                       [[thread_position_in_grid]]) {
  if (any(pixel >= root.destination_extent))
    return;
  float maximum_depth = 0.0;
  if (root.source_is_depth != 0u) {
    maximum_depth = root.source.read(min(pixel, root.source_extent - 1u)).x;
  } else {
    uint2 source_begin = pixel * 2u;
    uint2 source_end = min(source_begin + 2u, root.source_extent);
    // Metal mip extents floor odd dimensions. The last destination texel owns
    // the unpaired source edge so every depth sample contributes to the max.
    if (pixel.x + 1u == root.destination_extent.x)
      source_end.x = root.source_extent.x;
    if (pixel.y + 1u == root.destination_extent.y)
      source_end.y = root.source_extent.y;
    for (uint y = source_begin.y; y < source_end.y; ++y) {
      for (uint x = source_begin.x; x < source_end.x; ++x) {
        maximum_depth = max(maximum_depth, root.source.read(uint2(x, y)).x);
      }
    }
  }
  root.destination.write(float4(maximum_depth), pixel);
}

// One far cascade's EVSM moments layer, built from its depth layer.
struct VkrMetalPacketShadowMomentsRoot {
  depth2d_array<float, access::read> depth;
  texture2d_array<float, access::write> moments;
  uint depth_layer;
  uint moments_layer;
  uint depth_size;
  uint moments_size;
};

// Each moments texel takes the 4x4 depth texels around its 2x2 footprint with
// (1, 3, 3, 1) tent weights per axis, clamped at the map edge, and stores
// their weighted EVSM moments.
kernel void vkr_metal_packet_shadow_moments(
    constant VkrMetalPacketShadowMomentsRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= uint2(root.moments_size)))
    return;
  int2 origin = int2(pixel) * 2 - 1;
  int last = int(root.depth_size) - 1;
  float4 moments = 0.0;
  for (uint y = 0u; y < 4u; ++y) {
    for (uint x = 0u; x < 4u; ++x) {
      int2 texel = clamp(origin + int2(int(x), int(y)), 0, last);
      float depth = root.depth.read(uint2(texel), root.depth_layer);
      moments += vkr_shadow_evsm_moments(depth) *
                 (vkr_shadow_evsm_tent_weight(x) *
                  vkr_shadow_evsm_tent_weight(y));
    }
  }
  root.moments.write(moments, pixel, root.moments_layer);
}

static_assert(sizeof(VkrGpuDrawCompactionState) == 144,
              "GPU draw compaction state ABI must remain 144 bytes");
static_assert(sizeof(VkrMetalPacketGpuDrawRoot) == 192,
              "GPU draw root ABI must remain 192 bytes");
static_assert(sizeof(VkrMetalPacketGpuDrawView) == 112,
              "GPU draw view ABI must remain 112 bytes");
static_assert(sizeof(VkrMetalPacketHzbBuildRoot) == 48,
              "HZB build root ABI must remain 48 bytes");
static_assert(sizeof(VkrMetalPacketShadowMomentsRoot) == 32,
              "Shadow moments root ABI must remain 32 bytes");
