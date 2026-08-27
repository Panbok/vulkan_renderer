struct VkrGpuDrawCompactionState {
  uint2 execution_ranges[4];
  atomic_uint bucket_counts[4];
  atomic_uint bucket_cursors[4];
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
  ulong reserved_2;
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
  uint reserved[2];
};

struct VkrMetalPacketIcbContainer {
  command_buffer command_buffer [[id(0)]];
};

struct VkrMetalPacketTransmissionPeelRoot {
  texture2d<float, access::read> previous_depth;
  float depth_epsilon;
  uint previous_depth_enabled;
};

fragment uint2 vkr_metal_packet_vbuffer_fragment(
    VkrMetalPacketVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    uint primitive_id [[primitive_id]]) {
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
  return uint2(input.visible_row_index + 1u, primitive_id);
}

/* Opaque visibility draws never alpha-test, so this variant drops the material
   lookup and the discard. Without a possible discard the tiler can run depth
   testing before the fragment stage instead of after it. Mirrors
   vk_visibility_opaque_fragment. */
[[early_fragment_tests]] fragment uint2
vkr_metal_packet_vbuffer_opaque_fragment(VkrMetalPacketVertexOutput input
                                         [[stage_in]],
                                         uint primitive_id [[primitive_id]]) {
  return uint2(input.visible_row_index + 1u, primitive_id);
}

fragment uint2 vkr_metal_packet_transmission_vbuffer_fragment(
    VkrMetalPacketVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    constant VkrMetalPacketTransmissionPeelRoot &peel [[buffer(2)]],
    uint primitive_id [[primitive_id]]) {
  if (peel.previous_depth_enabled != 0u &&
      input.position.z <= peel.previous_depth.read(uint2(input.position.xy)).x +
                              peel.depth_epsilon)
    discard_fragment();
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
  return uint2(input.visible_row_index + 1u, primitive_id);
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
  if ((candidate.flags & 1u) == 0u)
    return true;
  const device VkrMetalPacketInstance &instance =
      root.instances[candidate.instance_index];
  float3 center =
      (instance.model * float4(candidate.local_bounding_sphere.xyz, 1.0)).xyz;
  float scale =
      max(length(instance.model[0].xyz),
          max(length(instance.model[1].xyz), length(instance.model[2].xyz)));
  float radius = candidate.local_bounding_sphere.w * scale;
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
      any(root.hzb_extent == 0u) || (candidate.flags & 1u) == 0u)
    return false;
  const device VkrMetalPacketInstance &instance =
      root.instances[candidate.instance_index];
  float3 center =
      (instance.model * float4(candidate.local_bounding_sphere.xyz, 1.0)).xyz;
  float scale =
      max(length(instance.model[0].xyz),
          max(length(instance.model[1].xyz), length(instance.model[2].xyz)));
  float radius = max(candidate.local_bounding_sphere.w * scale, 0.0);
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
  if (candidate.state_bucket >= 4u ||
      (candidate.flags & view.required_candidate_flags) !=
          view.required_candidate_flags ||
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
  root.classifications[classification_index] = candidate.state_bucket + 1u;
  atomic_fetch_add_explicit(&state.bucket_counts[candidate.state_bucket], 1u,
                            memory_order_relaxed);
}

kernel void
vkr_metal_packet_gpu_draw_prefix(constant VkrMetalPacketGpuDrawRoot &root
                                 [[buffer(0)]],
                                 uint view_index [[thread_position_in_grid]]) {
  if (view_index >= root.view_count)
    return;
  device VkrGpuDrawCompactionState &state = root.compaction_state[view_index];
  uint command_base =
      (view_index % root.icb_view_group_size) * root.visible_capacity;
  uint visible_count = 0u;
  for (uint bucket = 0u; bucket < 4u; ++bucket) {
    uint bucket_count = atomic_load_explicit(&state.bucket_counts[bucket],
                                             memory_order_relaxed);
    state.execution_ranges[bucket] =
        uint2(command_base + visible_count, bucket_count);
    visible_count += bucket_count;
    atomic_store_explicit(&state.bucket_cursors[bucket], 0u,
                          memory_order_relaxed);
  }
  state.visible_count = visible_count;
  const bool overflow = visible_count > root.visible_capacity;
  if (overflow) {
    for (uint bucket = 0u; bucket < 4u; ++bucket)
      state.execution_ranges[bucket].y = 0u;
  }
  atomic_store_explicit(&state.overflow_count,
                        overflow ? visible_count - root.visible_capacity : 0u,
                        memory_order_relaxed);
}

kernel void vkr_metal_packet_gpu_draw_encode(
    constant VkrMetalPacketGpuDrawRoot &root [[buffer(0)]],
    constant VkrMetalPacketIcbContainer *icb [[buffer(1)]],
    uint2 position [[thread_position_in_grid]]) {
  uint index = position.x;
  uint view_index = root.encode_view_index + position.y;
  if (index >= root.candidate_count || view_index >= root.view_count ||
      icb == nullptr)
    return;
  uint view_base = view_index * root.visible_capacity;
  uint classification = root.classifications[view_base + index];
  if (classification == 0u)
    return;
  device VkrGpuDrawCompactionState &state = root.compaction_state[view_index];
  uint bucket = classification - 1u;
  uint local_index = atomic_fetch_add_explicit(&state.bucket_cursors[bucket],
                                               1u, memory_order_relaxed);
  uint command_base =
      (view_index % root.icb_view_group_size) * root.visible_capacity;
  uint visible_index =
      state.execution_ranges[bucket].x - command_base + local_index;
  if (visible_index >= root.visible_capacity) {
    atomic_fetch_add_explicit(&state.overflow_count, 1u, memory_order_relaxed);
    return;
  }

  const device VkrGpuCandidateDrawRow &candidate = root.candidates[index];
  root.visible_rows[view_base + visible_index] = {
      candidate.geometry_index, candidate.material_index,
      candidate.instance_index, candidate.first_index,
      candidate.index_count,    candidate.vertex_offset,
      candidate.state_bucket,   candidate.flags};
  const device VkrGpuGeometryRow &geometry =
      root.geometry_rows[candidate.geometry_index];
  device uint *indices = reinterpret_cast<device uint *>(
      geometry.index_address + ulong(candidate.first_index) * sizeof(uint));

  render_command command(icb->command_buffer, command_base + visible_index);
  command.set_vertex_buffer(&root.draw_roots[view_index], 0u);
  command.set_fragment_buffer(&root.draw_roots[view_index], 1u);
  command.draw_indexed_primitives(primitive_type::triangle,
                                  candidate.index_count, indices, 1u,
                                  candidate.vertex_offset, visible_index);
}

struct VkrMetalPacketPickingResolveRoot {
  device VkrGpuVisibleDrawRow *opaque_visible_rows;
  device VkrMetalPacketInstance *opaque_instances;
  device VkrGpuDrawCompactionState *opaque_compaction_state;
  device VkrGpuVisibleDrawRow *transmission_visible_rows;
  device VkrMetalPacketInstance *transmission_instances;
  device VkrGpuDrawCompactionState *transmission_compaction_state;
  texture2d<uint, access::read> opaque_vbuffer;
  texture2d<float, access::read> opaque_depth;
  texture2d<uint, access::read> transmission_vbuffer;
  texture2d<float, access::read> transmission_depth;
  texture2d<uint, access::write> destination;
  uint2 pixel;
  uint2 extent;
  uint opaque_visible_capacity;
  uint opaque_instance_count;
  uint transmission_visible_capacity;
  uint transmission_instance_count;
  uint transmission_enabled;
  uint reserved[1];
};

static uint vkr_metal_packet_picking_object_id(
    texture2d<uint, access::read> vbuffer,
    const device VkrGpuVisibleDrawRow *visible_rows,
    const device VkrMetalPacketInstance *instances,
    const device VkrGpuDrawCompactionState *state, uint visible_capacity,
    uint instance_count, uint2 pixel) {
  uint encoded = vbuffer.read(pixel).x;
  if (encoded == 0u || visible_rows == nullptr || instances == nullptr ||
      state == nullptr)
    return 0u;
  uint visible_index = encoded - 1u;
  if (visible_index >= visible_capacity ||
      visible_index >= state->visible_count)
    return 0u;
  uint instance_index = visible_rows[visible_index].instance_index;
  return instance_index < instance_count ? instances[instance_index].object_id
                                         : 0u;
}

kernel void
vkr_metal_packet_picking_resolve(constant VkrMetalPacketPickingResolveRoot &root
                                 [[buffer(0)]],
                                 uint index [[thread_position_in_grid]]) {
  if (index != 0u || any(root.pixel >= root.extent))
    return;
  uint object_id = vkr_metal_packet_picking_object_id(
      root.opaque_vbuffer, root.opaque_visible_rows, root.opaque_instances,
      root.opaque_compaction_state, root.opaque_visible_capacity,
      root.opaque_instance_count, root.pixel);
  uint transmission_encoded = root.transmission_enabled != 0u
                                  ? root.transmission_vbuffer.read(root.pixel).x
                                  : 0u;
  if (transmission_encoded != 0u &&
      (root.opaque_vbuffer.read(root.pixel).x == 0u ||
       root.transmission_depth.read(root.pixel).x <=
           root.opaque_depth.read(root.pixel).x)) {
    object_id = vkr_metal_packet_picking_object_id(
        root.transmission_vbuffer, root.transmission_visible_rows,
        root.transmission_instances, root.transmission_compaction_state,
        root.transmission_visible_capacity, root.transmission_instance_count,
        root.pixel);
  }
  root.destination.write(uint4(object_id, 0u, 0u, 0u), root.pixel);
}

struct VkrMetalPacketTemporalTransformRoot {
  device VkrMetalPacketInstance *instances;
  device VkrTemporalTransform *transforms;
  uint instance_count;
  uint transform_capacity;
  uint frame_index;
  uint reserved;
};

kernel void vkr_metal_packet_temporal_transform(
    constant VkrMetalPacketTemporalTransformRoot &root [[buffer(0)]],
    uint index [[thread_position_in_grid]]) {
  if (index >= root.instance_count)
    return;
  const device VkrMetalPacketInstance &instance = root.instances[index];
  if ((instance.temporal_flags & VKR_INSTANCE_TEMPORAL_OWNER) == 0u ||
      instance.temporal_index >= root.transform_capacity)
    return;
  VkrTemporalTransform transform;
  transform.model = instance.model;
  transform.generation = instance.temporal_generation;
  transform.frame_index = root.frame_index;
  transform.valid = 1u;
  transform.reserved = 0u;
  root.transforms[instance.temporal_index] = transform;
}

struct VkrMetalPacketGBufferResolveRoot {
  device VkrGpuVisibleDrawRow *visible_rows;
  device VkrGpuGeometryRow *geometry_rows;
  device VkrMetalPacketInstance *instances;
  device VkrMetalPacketMaterial *materials;
  device VkrGpuDrawCompactionState *compaction_state;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::write> albedo;
  texture2d<float, access::write> specular;
  texture2d<float, access::write> normal;
  texture2d<float, access::write> emissive;
  texture2d<float, access::write> debug;
  texture2d<float, access::write> hdr_seed;
  float4x4 view_projection;
  float4x4 current_view_projection;
  float4x4 previous_view_projection;
  device VkrTemporalTransform *previous_transforms;
  texture2d<float, access::write> motion;
  texture2d<float, access::write> validity;
  uint2 extent;
  uint visible_capacity;
  uint geometry_count;
  uint material_count;
  uint instance_count;
  uint render_mode;
  uint history_valid;
  uint previous_frame_index;
  uint reserved;
};

static void vkr_metal_packet_resolve_defaults(
    constant VkrMetalPacketGBufferResolveRoot &root, uint2 pixel,
    float debug_marker) {
  root.albedo.write(float4(0.0, 0.0, 0.0, 1.0), pixel);
  root.specular.write(float4(0.0, 0.0, 0.0, 1.0), pixel);
  root.normal.write(float4(0.0), pixel);
  root.emissive.write(float4(0.0), pixel);
  root.hdr_seed.write(float4(0.0), pixel);
  root.debug.write(float4(0.0, 0.0, 0.0, debug_marker), pixel);
  root.motion.write(float4(0.0), pixel);
  root.validity.write(float4(0.0), pixel);
}

static bool vkr_metal_packet_perspective_barycentrics(
    float2 point, thread const float4 (&clip)[3], thread float3 &barycentrics) {
  /* Solve the two homogeneous screen-point constraints plus sum(lambda)=1.
     Unlike divide-by-w affine reconstruction, this remains defined when an
     original triangle crosses the eye plane and Metal rasterizes its clipped
     polygon. */
  float3 horizontal =
      float3(clip[0].x - point.x * clip[0].w, clip[1].x - point.x * clip[1].w,
             clip[2].x - point.x * clip[2].w);
  float3 vertical =
      float3(clip[0].y - point.y * clip[0].w, clip[1].y - point.y * clip[1].w,
             clip[2].y - point.y * clip[2].w);
  float3 weights = cross(horizontal, vertical);
  /* A zero-area or edge-on triangle is legitimate mesh content, so these two
     magnitude guards stay. Finiteness follows from finite vertex positions and
     a finite view-projection, so the tests they were paired with are implied.
     Mirrors vkr_vk_perspective_barycentric. */
  float scale = max(abs(weights.x), max(abs(weights.y), abs(weights.z)));
  if (scale <= 1e-12)
    return false;
  weights /= scale;
  float normalization = weights.x + weights.y + weights.z;
  if (abs(normalization) <= 1e-8)
    return false;
  barycentrics = weights / normalization;
  return true;
}

static bool vkr_metal_packet_projected_face_sign(thread const float4 (&clip)[3],
                                                 thread float &face_sign) {
  float3 q0 = float3(clip[0].x, clip[0].y, clip[0].w);
  float3 q1 = float3(clip[1].x, clip[1].y, clip[1].w);
  float3 q2 = float3(clip[2].x, clip[2].y, clip[2].w);
  float orientation = dot(cross(q0, q1), q2);
  if (abs(orientation) <= 1e-12)
    return false;
  /* Metal screen Y is inverted relative to NDC, matching the former signed
     screen-area test without dividing eye-plane-crossing vertices by w. */
  face_sign = orientation > 0.0 ? 1.0 : -1.0;
  return true;
}

static float2 vkr_metal_packet_resolve_ndc(float2 pixel, uint2 extent) {
  float2 normalized = pixel / float2(extent);
  return float2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

static float2 vkr_metal_packet_octahedral_encode(float3 normal) {
  normal /= max(abs(normal.x) + abs(normal.y) + abs(normal.z), 1e-7);
  float2 encoded = normal.xy;
  if (normal.z < 0.0)
    encoded = (1.0 - abs(encoded.yx)) *
              select(float2(-1.0), float2(1.0), encoded >= 0.0);
  return clamp(encoded, -1.0, 1.0);
}

static bool vkr_metal_packet_finite_nonzero(float3 value) {
  return all(isfinite(value)) && dot(value, value) > 1e-12;
}

kernel void
vkr_metal_packet_gbuffer_resolve(constant VkrMetalPacketGBufferResolveRoot &root
                                 [[buffer(0)]],
                                 uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent))
    return;
  uint2 visibility = root.vbuffer.read(pixel).xy;
  if (visibility.x == 0u) {
    vkr_metal_packet_resolve_defaults(root, pixel, 0.0);
    return;
  }
  uint visible_index = visibility.x - 1u;
  uint visible_count = root.compaction_state->visible_count;
  if (visible_index >= root.visible_capacity ||
      visible_index >= visible_count) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
    return;
  }

  const device VkrGpuVisibleDrawRow &visible = root.visible_rows[visible_index];
  if (visible.geometry_index >= root.geometry_count ||
      visible.material_index >= root.material_count ||
      visible.instance_index >= root.instance_count) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
    return;
  }
  uint primitive_id = visibility.y;
  if (visible.index_count < 3u ||
      primitive_id > (visible.index_count - 3u) / 3u) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
    return;
  }
  const device VkrGpuGeometryRow &geometry =
      root.geometry_rows[visible.geometry_index];
  device const uint *indices = reinterpret_cast<device const uint *>(
      geometry.index_address + ulong(visible.first_index) * sizeof(uint));
  VkrGpuDecodedVertex vertices[3];
  float4 clip[3];
  device const VkrPackedStaticVertex *vertex_rows =
      reinterpret_cast<device const VkrPackedStaticVertex *>(
          geometry.vertex_address);
  device const VkrGpuGeometryDecodeRecord &decode =
      *reinterpret_cast<device const VkrGpuGeometryDecodeRecord *>(
          geometry.decode_address);
  const device VkrMetalPacketInstance &instance =
      root.instances[visible.instance_index];
  for (uint corner = 0u; corner < 3u; ++corner) {
    int vertex_index =
        int(indices[primitive_id * 3u + corner]) + visible.vertex_offset;
    if (vertex_index < 0) {
      atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count,
                                1u, memory_order_relaxed);
      vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
      return;
    }
    vertices[corner] = vkr_decode_packed_vertex(
        vertex_rows[geometry.first_vertex + uint(vertex_index)], decode);
    float3 position = vertices[corner].position;
    clip[corner] =
        root.view_projection * (instance.model * float4(position, 1.0));
  }

  float2 center = float2(pixel) + 0.5;
  float3 barycentric;
  float3 barycentric_dx;
  float3 barycentric_dy;
  if (!vkr_metal_packet_perspective_barycentrics(
          vkr_metal_packet_resolve_ndc(center, root.extent), clip,
          barycentric) ||
      !vkr_metal_packet_perspective_barycentrics(
          vkr_metal_packet_resolve_ndc(center + float2(1.0, 0.0), root.extent),
          clip, barycentric_dx) ||
      !vkr_metal_packet_perspective_barycentrics(
          vkr_metal_packet_resolve_ndc(center + float2(0.0, 1.0), root.extent),
          clip, barycentric_dy)) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
    return;
  }

  float2 uv[3] = {vertices[0].texcoord, vertices[1].texcoord,
                  vertices[2].texcoord};
  float2 texcoord =
      uv[0] * barycentric.x + uv[1] * barycentric.y + uv[2] * barycentric.z;
  float2 texcoord_dx = uv[0] * barycentric_dx.x + uv[1] * barycentric_dx.y +
                       uv[2] * barycentric_dx.z;
  float2 texcoord_dy = uv[0] * barycentric_dy.x + uv[1] * barycentric_dy.y +
                       uv[2] * barycentric_dy.z;
  float2 gradient_x = texcoord_dx - texcoord;
  float2 gradient_y = texcoord_dy - texcoord;
  /* Finite barycentrics and finite mesh texcoords give finite results here;
     the degenerate-triangle guard in the barycentric solve is what actually
     rejects this pixel. */

  const device VkrMetalPacketMaterial &material =
      root.materials[visible.material_index];
  gradient2d gradients(gradient_x, gradient_y);
  float4 vertex_color = vertices[0].color * barycentric.x +
                        vertices[1].color * barycentric.y +
                        vertices[2].color * barycentric.z;
  float4 base = material.base_color_texture.sample(material.base_color_sampler,
                                                   texcoord, gradients) *
                material.tint * vertex_color;
  float metallic = saturate(material.material_surface.x);
  float roughness = clamp(material.material_surface.y, 0.04, 1.0);
  float occlusion = saturate(material.material_surface.w);
  if ((material.flags & 2u) != 0u) {
    float3 orm =
        material.orm_texture.sample(material.orm_sampler, texcoord, gradients)
            .rgb;
    occlusion *= orm.r;
    roughness = clamp(roughness * orm.g, 0.04, 1.0);
    metallic = saturate(metallic * orm.b);
  }

  float3 object_normal = vertices[0].normal * barycentric.x +
                         vertices[1].normal * barycentric.y +
                         vertices[2].normal * barycentric.z;
  float3 object_position = vertices[0].position * barycentric.x +
                           vertices[1].position * barycentric.y +
                           vertices[2].position * barycentric.z;
  float4 object_tangent = vertices[0].tangent * barycentric.x +
                          vertices[1].tangent * barycentric.y +
                          vertices[2].tangent * barycentric.z;
  float3 transformed_normal = (instance.model * float4(object_normal, 0.0)).xyz;
  float face_sign;
  if (!vkr_metal_packet_projected_face_sign(clip, face_sign) ||
      !vkr_metal_packet_finite_nonzero(transformed_normal)) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
    return;
  }
  float3 normal = normalize(transformed_normal) * face_sign;
  if ((material.flags & 1u) != 0u) {
    float3 sampled =
        material.normal_texture
                .sample(material.normal_sampler, texcoord, gradients)
                .xyz *
            2.0 -
        1.0;
    sampled.xy *= material.material_surface.z;
    sampled.y = -sampled.y;
    float3 tangent = (instance.model * float4(object_tangent.xyz, 0.0)).xyz;
    if (!vkr_metal_packet_finite_nonzero(sampled) ||
        !vkr_metal_packet_finite_nonzero(tangent)) {
      atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count,
                                1u, memory_order_relaxed);
      vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
      return;
    }
    tangent = normalize(tangent);
    tangent -= dot(tangent, normal) * normal;
    if (!vkr_metal_packet_finite_nonzero(tangent)) {
      atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count,
                                1u, memory_order_relaxed);
      vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
      return;
    }
    tangent = normalize(tangent);
    /* `normal` and `tangent` are unit and orthogonal by the Gram-Schmidt step
       above, so the cross product is unit and `mapped` has exactly the
       magnitude of `sampled`, which the guard above already established as
       nonzero. Both former checks were implied. */
    float3 bitangent =
        normalize(cross(normal, tangent)) * sign(object_tangent.w);
    float3 mapped =
        tangent * sampled.x + bitangent * sampled.y + normal * sampled.z;
    normal = normalize(mapped);
  }

  float3 f0 = mix(saturate(material.material_dielectric_specular.rgb), base.rgb,
                  metallic);
  float3 emissive = material.material_emissive.rgb;
  if ((material.flags & 4u) != 0u)
    emissive *= material.emissive_texture
                    .sample(material.emissive_sampler, texcoord, gradients)
                    .rgb;
  float2 texture_extent = float2(material.base_color_texture.get_width(),
                                 material.base_color_texture.get_height());
  float footprint = max(length(gradient_x * texture_extent),
                        length(gradient_y * texture_extent));
  float selected_lod = max(log2(max(footprint, 1e-8)), 0.0);
  float2 motion = 0.0;
  float2 validity = 0.0;
  if (root.history_valid != 0u &&
      instance.temporal_index < VKR_TEMPORAL_TRANSFORM_CAPACITY) {
    const device VkrTemporalTransform &previous =
        root.previous_transforms[instance.temporal_index];
    if (previous.valid != 0u &&
        previous.generation == instance.temporal_generation &&
        previous.frame_index == root.previous_frame_index) {
      float4 current_clip = root.current_view_projection *
                            (instance.model * float4(object_position, 1.0));
      float4 previous_clip = root.previous_view_projection *
                             (previous.model * float4(object_position, 1.0));
      if (current_clip.w > 1e-6 && previous_clip.w > 1e-6) {
        float2 current_ndc = current_clip.xy / current_clip.w;
        float2 previous_ndc = previous_clip.xy / previous_clip.w;
        float2 current_uv =
            float2(current_ndc.x * 0.5 + 0.5, 0.5 - current_ndc.y * 0.5);
        float2 previous_uv =
            float2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5);
        motion = previous_uv - current_uv;
        validity = float2(1.0, previous_clip.z / previous_clip.w);
      }
    }
  }
  root.motion.write(float4(motion, 0.0, 0.0), pixel);
  root.validity.write(float4(validity, 0.0, 0.0), pixel);

  root.albedo.write(float4(base.rgb * (1.0 - metallic), occlusion), pixel);
  root.specular.write(float4(f0, roughness), pixel);
  root.normal.write(
      float4(vkr_metal_packet_octahedral_encode(normal), 0.0, 0.0), pixel);
  root.emissive.write(float4(emissive, 1.0), pixel);
  float3 hdr_seed = root.render_mode == 3u ? base.rgb + emissive : emissive;
  root.hdr_seed.write(float4(hdr_seed, metallic), pixel);
  root.debug.write(float4(barycentric, selected_lod + 1.0), pixel);
}

struct VkrMetalPacketDeferredLightingRoot {
  constant VkrMetalPacketFrameRoot *frame;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::read> depth;
  texture2d<float, access::read> albedo;
  texture2d<float, access::read> specular;
  texture2d<float, access::read> normal;
  texture2d<float, access::read_write> hdr;
  texturecube<float, access::sample> sky;
  texture2d<float, access::sample> gtao_visibility;
  float4x4 inverse_view_projection;
  uint2 extent;
  uint sky_enabled;
  uint reserved;
};

static float3 vkr_metal_packet_octahedral_decode(float2 encoded) {
  float3 normal = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
  if (normal.z < 0.0) {
    normal.xy = (1.0 - abs(normal.yx)) *
                select(float2(-1.0), float2(1.0), normal.xy >= 0.0);
  }
  return normalize(normal);
}

static float3 vkr_metal_packet_deferred_world_position(
    constant VkrMetalPacketDeferredLightingRoot &root, uint2 pixel,
    float depth) {
  float2 ndc = vkr_metal_packet_resolve_ndc(float2(pixel) + 0.5, root.extent);
  float4 world = root.inverse_view_projection * float4(ndc, depth, 1.0);
  return world.xyz / max(abs(world.w), 1e-7) * sign(world.w);
}

static float vkr_metal_packet_deferred_filter_roughness(
    constant VkrMetalPacketDeferredLightingRoot &root, uint2 pixel,
    uint visible_index, float3 normal, float roughness) {
  uint2 limit = root.extent - 1u;
  uint2 pixel_x = min(pixel + uint2(1u, 0u), limit);
  uint2 pixel_y = min(pixel + uint2(0u, 1u), limit);
  uint visible_x = root.vbuffer.read(pixel_x).x;
  uint visible_y = root.vbuffer.read(pixel_y).x;
  float3 normal_x =
      vkr_metal_packet_octahedral_decode(root.normal.read(pixel_x).xy);
  float3 normal_y =
      vkr_metal_packet_octahedral_decode(root.normal.read(pixel_y).xy);
  float same_x = select(0.0, 1.0, visible_x == visible_index);
  float same_y = select(0.0, 1.0, visible_y == visible_index);
  float3 normal_dx = (normal_x - normal) * same_x;
  float3 normal_dy = (normal_y - normal) * same_y;
  float normal_variance =
      0.25 * (dot(normal_dx, normal_dx) + dot(normal_dy, normal_dy));
  return sqrt(saturate(roughness * roughness + min(normal_variance, 0.25)));
}

static float3
vkr_metal_packet_deferred_sky(constant VkrMetalPacketDeferredLightingRoot &root,
                              uint2 pixel) {
  if (root.sky_enabled == 0u)
    return float3(0.02, 0.02, 0.03);
  float2 ndc = vkr_metal_packet_resolve_ndc(float2(pixel) + 0.5, root.extent);
  float4 far_world = root.inverse_view_projection * float4(ndc, 1.0, 1.0);
  float3 direction = normalize(far_world.xyz / max(abs(far_world.w), 1e-7) *
                                   sign(far_world.w) -
                               root.frame->view_position.xyz);
  constexpr sampler sky_sampler(coord::normalized, address::clamp_to_edge,
                                filter::linear);
  return root.sky.sample(sky_sampler, direction).rgb;
}

kernel void vkr_metal_packet_deferred_lighting(
    constant VkrMetalPacketDeferredLightingRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent))
    return;
  uint visible_index = root.vbuffer.read(pixel).x;
  if (visible_index == 0u) {
    root.hdr.write(float4(vkr_metal_packet_deferred_sky(root, pixel), 1.0),
                   pixel);
    return;
  }

  constant VkrMetalPacketFrameRoot *frame = root.frame;
  float4 hdr_seed = root.hdr.read(pixel);
  if (frame->render_mode == 3u) {
    root.hdr.write(float4(hdr_seed.rgb, 1.0), pixel);
    return;
  }
  float4 albedo_ao = root.albedo.read(pixel);
  float4 f0_roughness = root.specular.read(pixel);
  float3 normal =
      vkr_metal_packet_octahedral_decode(root.normal.read(pixel).xy);
  float3 diffuse_albedo = albedo_ao.rgb;
  float ao = saturate(albedo_ao.a);
  float3 f0 = saturate(f0_roughness.rgb);
  float roughness = clamp(f0_roughness.a, 0.04, 1.0);
  if (frame->render_mode == 2u) {
    root.hdr.write(float4(normal * 0.5 + 0.5, 1.0), pixel);
    return;
  }
  float3 world_position = vkr_metal_packet_deferred_world_position(
      root, pixel, root.depth.read(pixel).x);
  roughness = vkr_metal_packet_deferred_filter_roughness(
      root, pixel, visible_index, normal, roughness);
  if (frame->render_mode == 6u) {
    root.hdr.write(
        float4(hdr_seed.a, roughness, max(f0.x, max(f0.y, f0.z)), 1.0), pixel);
    return;
  }
  if (frame->shadow_debug_mode != 0u) {
    VkrMetalPacketShadowSample shadow_sample =
        vkr_metal_packet_directional_shadow_sample(frame, world_position,
                                                   normal);
    root.hdr.write(float4(vkr_metal_packet_shadow_debug_color(
                              frame->shadow_debug_mode, shadow_sample),
                          1.0),
                   pixel);
    return;
  }
  float3 view = normalize(frame->view_position.xyz - world_position);
  float no_v = max(dot(normal, view), 0.0);
  float3 analytic_diffuse = 0.0;
  float3 analytic_specular = 0.0;

  if (frame->directional_direction_enabled.w > 0.5) {
    VkrMetalPacketShadowSample shadow_sample =
        vkr_metal_packet_directional_shadow_sample(frame, world_position,
                                                   normal);
    VkrMetalPacketDirectResult direct = vkr_metal_packet_direct_deferred(
        normal, view, normalize(-frame->directional_direction_enabled.xyz),
        frame->directional_color_intensity.rgb *
            frame->directional_color_intensity.w * shadow_sample.factor,
        diffuse_albedo, roughness, f0);
    analytic_diffuse = direct.diffuse;
    analytic_specular = direct.specular;
  }

  uint4 point_mask = vkr_metal_packet_point_light_mask(frame, world_position);
  uint point_count = min(frame->point_light_count, 128u);
  for (uint word = 0u; word < 4u; ++word) {
    /* The mask comes from a per-pixel cluster cell, so lanes straddling a cell
       boundary carry different masks and the SIMD-group already pays for the
       union of their set bits. Iterating that union explicitly makes
       `light_index` SIMD-uniform, which lets the light rows load as scalars
       instead of per-lane vectors. Each lane still accumulates only its own
       bits, so the summation order and result are unchanged. */
    uint remaining = simd_or(point_mask[word]);
    while (remaining != 0u) {
      uint bit = ctz(remaining);
      remaining &= remaining - 1u;
      uint light_index = word * 32u + bit;
      if (light_index >= point_count)
        continue;
      float4 p0 = frame->point_light_data[light_index * 4u + 0u];
      float4 p1 = frame->point_light_data[light_index * 4u + 1u];
      float4 p2 = frame->point_light_data[light_index * 4u + 2u];
      float4 p3 = frame->point_light_data[light_index * 4u + 3u];
      if ((point_mask[word] & (1u << bit)) == 0u)
        continue;
      uint kind = uint(p2.w + 0.5);
      float3 to_light = p0.xyz - world_position;
      float distance_squared = dot(to_light, to_light);
      if (kind != 0u && p2.z > 0.0 && distance_squared >= p2.z * p2.z)
        continue;
      float distance = sqrt(distance_squared);
      float3 light_direction =
          distance > 1e-6 ? to_light / distance : float3(0.0);
      float attenuation = 0.0;
      if (kind == 0u) {
        attenuation = 1.0 / max(max(p0.w, 1.0) + p1.w * distance +
                                    p2.y * distance_squared,
                                1e-6);
      } else {
        float range_attenuation = 1.0;
        if (p2.z > 0.0) {
          float ratio = distance / p2.z;
          range_attenuation = saturate(1.0 - ratio * ratio * ratio * ratio);
          range_attenuation *= range_attenuation;
        }
        attenuation = range_attenuation / max(distance_squared, 1e-4);
        if (kind == 2u) {
          float cone = dot(-light_direction, normalize(p3.xyz));
          float cone_attenuation = smoothstep(p1.w, p0.w, cone);
          if (cone_attenuation <= 0.0)
            continue;
          attenuation *= cone_attenuation;
        }
      }
      VkrMetalPacketDirectResult direct = vkr_metal_packet_direct_deferred(
          normal, view, light_direction, p1.rgb * p2.x * attenuation,
          diffuse_albedo, roughness, f0);
      analytic_diffuse += direct.diffuse;
      analytic_specular += direct.specular;
    }
  }
  if (frame->render_mode == 1u) {
    root.hdr.write(float4(analytic_diffuse + analytic_specular, 1.0), pixel);
    return;
  }
  if (frame->render_mode == 4u) {
    root.hdr.write(float4(analytic_diffuse, 1.0), pixel);
    return;
  }
  if (frame->render_mode == 5u) {
    root.hdr.write(float4(analytic_specular, 1.0), pixel);
    return;
  }

  constexpr sampler gtao_sampler(coord::normalized, address::clamp_to_edge,
                                 filter::nearest);
  float2 gtao_uv = (float2(pixel) + 0.5) / float2(root.extent);
  float gtao_visibility =
      root.gtao_visibility.sample(gtao_sampler, gtao_uv).x;
  float diffuse_ao = ao * gtao_visibility;
  float3 color = analytic_diffuse + analytic_specular;
  if ((frame->flags & 2u) != 0u) {
    constexpr sampler environment_sampler(coord::normalized,
                                          address::clamp_to_edge,
                                          filter::linear, mip_filter::linear);
    float3 reflection = reflect(-view, normal);
    float3 fresnel = vkr_metal_packet_fresnel_roughness(no_v, f0, roughness);
    float3 global_irradiance =
        frame->irradiance.sample(environment_sampler, normal).rgb;
    float3 global_prefiltered =
        frame->prefilter
            .sample(environment_sampler, reflection,
                    level(roughness *
                          float(max(frame->prefilter_mip_count, 1u) - 1u)))
            .rgb;
    float2 brdf = vkr_metal_packet_brdf_approximation(no_v, roughness);
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0);
    float horizon = saturate(1.0 + dot(reflection, normal));
    float specular_visibility =
        horizon * horizon * vkr_metal_packet_specular_ao(ao, no_v, roughness);
    float3 diffuse = 0.0;
    float3 specular = 0.0;
    float local_weight_sum = 0.0;
    uint probe_count = min(frame->ibl_probe_count, 16u);
    if (frame->ibl_probes != nullptr) {
      for (uint i = 0u; i < probe_count; ++i)
        local_weight_sum += vkr_metal_packet_probe_influence(
            frame->ibl_probes[i], world_position);
      float weight_scale =
          local_weight_sum > 1.0 ? 1.0 / local_weight_sum : 1.0;
      for (uint i = 0u; i < probe_count; ++i) {
        const device VkrMetalPacketIblProbe &probe = frame->ibl_probes[i];
        float weight = vkr_metal_packet_probe_influence(probe, world_position) *
                       weight_scale;
        if (weight <= 1e-6)
          continue;
        float3 probe_reflection =
            probe.intensity_box.w > 0.5
                ? vkr_metal_packet_box_project(
                      reflection, world_position, probe.center_blend.xyz,
                      max(probe.extents_weight.xyz, 0.0))
                : reflection;
        float3 probe_irradiance =
            probe.irradiance.sample(environment_sampler, normal).rgb;
        float3 probe_prefiltered =
            probe.prefilter
                .sample(environment_sampler, probe_reflection,
                        level(roughness *
                              float(max(frame->prefilter_mip_count, 1u) - 1u)))
                .rgb;
        diffuse += (1.0 - fresnel) * probe_irradiance * diffuse_albedo *
                   diffuse_ao * probe.intensity_box.x * probe.intensity_box.y *
                   weight;
        specular += probe_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                    specular_visibility * probe.intensity_box.x *
                    probe.intensity_box.z * weight;
      }
    }
    float global_weight = max(1.0 - min(local_weight_sum, 1.0), 0.0);
    diffuse += (1.0 - fresnel) * global_irradiance * diffuse_albedo *
               diffuse_ao * global_weight;
    specular += global_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                specular_visibility * global_weight;
    color +=
        (diffuse * frame->ibl_controls.y + specular * frame->ibl_controls.z) *
        frame->ibl_controls.x;
  } else {
    color += frame->ambient_color.rgb * diffuse_albedo * diffuse_ao;
  }
  color += hdr_seed.rgb;
  root.hdr.write(float4(color, 1.0), pixel);
}

struct alignas(16) VkrMetalPacketTemporalResolveRoot {
  device VkrGpuVisibleDrawRow *visible_rows;
  device VkrMetalPacketInstance *instances;
  texture2d<float, access::read> scene;
  texture2d<float, access::read> pre_transmission;
  texture2d<float, access::read> motion;
  texture2d<float, access::read> validity;
  texture2d<float, access::read> depth;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::sample> history_color;
  texture2d<float, access::sample> history_depth;
  texture2d<uint, access::sample> history_identity;
  texture2d<uint, access::sample> history_primitive;
  texture2d<float, access::write> output_color;
  texture2d<float, access::write> output_depth;
  texture2d<uint, access::write> output_identity;
  texture2d<uint, access::write> output_primitive;
  uint2 extent;
  uint history_valid;
  uint render_mode;
  uint camera_stationary;
  device VkrGpuVisibleDrawRow *transmission_visible_rows;
  device VkrMetalPacketInstance *transmission_instances;
  texture2d<uint, access::read> transmission_vbuffer;
  texture2d<float, access::read> transmission_depth;
  uint transmission_enabled;
  uint transmission_alignment_padding;
  uint transmission_reserved[2];
};

static float vkr_metal_packet_temporal_luminance(float3 color) {
  return dot(color, float3(0.2126, 0.7152, 0.0722));
}

struct VkrMetalPacketTemporalHistorySample {
  float4 color;
  bool accepted;
};

static VkrMetalPacketTemporalHistorySample
vkr_metal_packet_temporal_history_sample(
    constant VkrMetalPacketTemporalResolveRoot &root, float2 history_uv,
    uint2 identity, uint primitive, float expected_depth, bool check_depth) {
  float2 history_texel = history_uv * float2(root.extent) - 0.5;
  int2 base = int2(floor(history_texel));
  constexpr sampler metadata_sampler(coord::normalized, address::clamp_to_edge,
                                     filter::nearest);
  uint4 identity_x = root.history_identity.gather(metadata_sampler, history_uv,
                                                  int2(0), component::x);
  uint4 identity_y = root.history_identity.gather(metadata_sampler, history_uv,
                                                  int2(0), component::y);
  uint4 primitives = root.history_primitive.gather(metadata_sampler, history_uv,
                                                   int2(0), component::x);
  float4 depths = root.history_depth.gather(metadata_sampler, history_uv,
                                            int2(0), component::x);
  bool4 matches =
      (identity_x == identity.x) & (identity_y == identity.y) &
      (primitives == primitive) &
      select(bool4(true), abs(depths - expected_depth) <= 0.005, check_depth);
  constexpr sampler linear_sampler(coord::normalized, address::clamp_to_edge,
                                   filter::linear);
  if (all(matches))
    return {root.history_color.sample(linear_sampler, history_uv), true};
  if (!any(matches))
    return {float4(0.0), false};

  constexpr sampler nearest_sampler(coord::normalized, address::clamp_to_edge,
                                    filter::nearest);
  float2 history_fraction = fract(history_texel);
  float4 color = 0.0;
  float weight_sum = 0.0;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      uint2 sample_pixel =
          uint2(clamp(base + int2(x, y), int2(0), int2(root.extent) - 1));
      float2 sample_uv = (float2(sample_pixel) + 0.5) / float2(root.extent);
      uint2 previous_identity =
          root.history_identity.sample(metadata_sampler, sample_uv).xy;
      uint previous_primitive =
          root.history_primitive.sample(metadata_sampler, sample_uv).x;
      float previous_depth =
          root.history_depth.sample(metadata_sampler, sample_uv).x;
      bool matches =
          all(previous_identity == identity) &&
          previous_primitive == primitive &&
          (!check_depth || abs(previous_depth - expected_depth) <= 0.005);
      if (!matches)
        continue;
      float weight_x = x == 0 ? 1.0 - history_fraction.x : history_fraction.x;
      float weight_y = y == 0 ? 1.0 - history_fraction.y : history_fraction.y;
      float weight = weight_x * weight_y;
      color += root.history_color.sample(nearest_sampler, sample_uv) * weight;
      weight_sum += weight;
    }
  }
  return weight_sum > 1e-5
             ? VkrMetalPacketTemporalHistorySample{color / weight_sum, true}
             : VkrMetalPacketTemporalHistorySample{float4(0.0), false};
}

kernel void vkr_metal_packet_temporal_resolve(
    constant VkrMetalPacketTemporalResolveRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent))
    return;
  float4 current = root.scene.read(pixel);
  float3 pre_transmission = root.pre_transmission.read(pixel).rgb;
  float2 motion = root.motion.read(pixel).xy;
  float2 validity = root.validity.read(pixel).xy;
  float surface_depth = root.depth.read(pixel).x;
  uint2 encoded = root.vbuffer.read(pixel).xy;
  constexpr uint overlay_bit = 0x80000000u;
  constexpr uint generation_mask = 0x7fffffffu;
  constexpr uint primitive_mask = 0x1ffffu;
  bool blend_overlay = (encoded.x & overlay_bit) != 0u;
  bool check_history_depth = !blend_overlay;
  uint2 identity = 0u;
  uint primitive = 0u;
  if (blend_overlay) {
    uint generation = encoded.x & generation_mask;
    if (encoded.y != 0u && generation != 0u) {
      identity = uint2((encoded.y >> 17u) + 1u, generation);
      primitive = encoded.y & primitive_mask;
    }
  } else {
    uint2 transmission_encoded = root.transmission_enabled != 0u
                                     ? root.transmission_vbuffer.read(pixel).xy
                                     : uint2(0u);
    if (transmission_encoded.x != 0u) {
      const device VkrGpuVisibleDrawRow &visible =
          root.transmission_visible_rows[transmission_encoded.x - 1u];
      const device VkrMetalPacketInstance &instance =
          root.transmission_instances[visible.instance_index];
      identity =
          uint2(instance.temporal_index + 1u, instance.temporal_generation);
      primitive = transmission_encoded.y + 1u;
      surface_depth = root.transmission_depth.read(pixel).x;
    } else if (encoded.x != 0u) {
      const device VkrGpuVisibleDrawRow &visible =
          root.visible_rows[encoded.x - 1u];
      const device VkrMetalPacketInstance &instance =
          root.instances[visible.instance_index];
      identity =
          uint2(instance.temporal_index + 1u, instance.temporal_generation);
      primitive = encoded.y + 1u;
    }
  }

  float current_luminance = vkr_metal_packet_temporal_luminance(current.rgb);
  float pre_luminance = vkr_metal_packet_temporal_luminance(pre_transmission);
  float derived_reactive =
      root.camera_stationary != 0u
          ? 0.0
          : min(0.75,
                saturate(abs(current_luminance - pre_luminance) /
                         max(max(current_luminance, pre_luminance), 0.05)));
  float authored_reactive =
      validity.x >= 2.0 ? saturate(validity.x - 2.0) : 0.0;
  float reactive = max(derived_reactive, authored_reactive);
  float2 uv = (float2(pixel) + 0.5) / float2(root.extent);
  float2 history_uv = uv + motion;
  bool accepted = root.history_valid != 0u &&
                  !(blend_overlay && identity.x == 0u) &&
                  (validity.x > 0.5 || root.camera_stationary != 0u) &&
                  all(history_uv >= 0.0) && all(history_uv <= 1.0);
  float4 history = current;
  if (accepted) {
    if (root.camera_stationary != 0u) {
      constexpr sampler history_sampler(coord::normalized,
                                        address::clamp_to_edge, filter::linear);
      history = root.history_color.sample(history_sampler, history_uv);
    } else {
      VkrMetalPacketTemporalHistorySample sample =
          vkr_metal_packet_temporal_history_sample(root, history_uv, identity,
                                                   primitive, validity.y,
                                                   check_history_depth);
      accepted = sample.accepted;
      history = accepted ? sample.color : current;
    }
  }

  if (accepted) {
    float3 neighborhood_min = current.rgb;
    float3 neighborhood_max = current.rgb;
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        int2 sample_pixel =
            clamp(int2(pixel) + int2(x, y), int2(0), int2(root.extent) - 1);
        float3 sample_color = root.scene.read(uint2(sample_pixel)).rgb;
        neighborhood_min = min(neighborhood_min, sample_color);
        neighborhood_max = max(neighborhood_max, sample_color);
      }
    }
    history.rgb = clamp(history.rgb, neighborhood_min, neighborhood_max);
  }
  float motion_pixels = length(motion * float2(root.extent));
  bool stationary_surface =
      root.camera_stationary != 0u && motion_pixels < 0.01;
  /* A fixed 0.9 EMA preserves a visible periodic residual from the eight-phase
     jitter sequence. Once both the camera and surface are stationary, retain
     enough history to make that residual sub-perceptual. Moving surfaces keep
     the responsive path even when the camera itself is still. */
  float history_retention = stationary_surface ? 0.99 : 0.9;
  float history_weight = accepted ? history_retention * (1.0 - reactive) *
                                        saturate(1.0 - motion_pixels / 128.0)
                                  : 0.0;
  float4 resolved =
      float4(mix(current.rgb, history.rgb, history_weight), current.a);
  if (root.render_mode == 7u)
    resolved = float4(saturate(abs(motion) * float2(root.extent) / 16.0),
                      validity.x, 1.0);
  else if (root.render_mode == 8u)
    resolved =
        accepted ? float4(0.0, 1.0, 0.0, 1.0) : float4(1.0, 0.0, 0.0, 1.0);

  root.output_color.write(resolved, pixel);
  root.output_depth.write(float4(surface_depth, 0.0, 0.0, 0.0), pixel);
  root.output_identity.write(uint4(identity, 0u, 0u), pixel);
  root.output_primitive.write(uint4(primitive, 0u, 0u, 0u), pixel);
}

struct VkrMetalPacketTransmissionShadeRoot {
  constant VkrMetalPacketFrameRoot *frame;
  device VkrGpuVisibleDrawRow *visible_rows;
  device VkrGpuGeometryRow *geometry_rows;
  device VkrMetalPacketInstance *instances;
  device VkrMetalPacketMaterial *materials;
  device VkrGpuDrawCompactionState *compaction_state;
  device uint *pixel_list;
  device atomic_uint *compact_counts;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::read> depth;
  texture2d<float, access::sample> source;
  texture2d<float, access::write> destination;
  float4x4 view_projection;
  float4x4 inverse_view_projection;
  uint2 extent;
  uint visible_capacity;
  uint geometry_count;
  uint material_count;
  uint instance_count;
  uint pixel_capacity;
  uint compact_layer;
  uint compact_enabled;
  uint reserved[3];
  texture2d<float, access::write> motion;
  texture2d<float, access::write> validity;
  device VkrTemporalTransform *previous_transforms;
  uint2 previous_transform_address_padding;
  float4x4 current_view_projection;
  float4x4 previous_view_projection;
  uint history_valid;
  uint previous_frame_index;
  uint temporal_outputs_enabled;
  uint temporal_reserved;
};

struct VkrMetalPacketTransmissionCoverageRoot {
  texture2d<uint, access::read> vbuffer;
  device atomic_uint *covered_pixels;
  uint2 extent;
  uint layer;
  uint reserved;
};

struct VkrMetalPacketTransmissionCompactRoot {
  texture2d<uint, access::read> vbuffer;
  device uint *pixel_list;
  device atomic_uint *covered_pixels;
  device atomic_uint *overflow_counts;
  device uint *indirect_arguments;
  texture2d<float, access::read> source;
  texture2d<float, access::write> destination;
  uint2 extent;
  uint layer;
  uint capacity;
  uint2 reserved;
};

struct VkrMetalPacketTransmissionSurface {
  float4 base;
  float3 normal;
  float metallic;
  float roughness;
  float occlusion;
  float3 emissive;
  uint material_index;
  float3 object_position;
  uint instance_index;
  uint primitive_id;
};

static bool vkr_metal_packet_resolve_transmission_surface(
    constant VkrMetalPacketTransmissionShadeRoot &root, uint2 pixel,
    thread VkrMetalPacketTransmissionSurface &surface) {
  uint2 visibility = root.vbuffer.read(pixel).xy;
  if (visibility.x == 0u)
    return false;
  uint visible_index = visibility.x - 1u;
  uint visible_count = root.compaction_state->visible_count;
  if (visible_index >= root.visible_capacity ||
      visible_index >= visible_count) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    return false;
  }
  const device VkrGpuVisibleDrawRow &visible = root.visible_rows[visible_index];
  if (visible.geometry_index >= root.geometry_count ||
      visible.material_index >= root.material_count ||
      visible.instance_index >= root.instance_count ||
      visible.index_count < 3u ||
      visibility.y > (visible.index_count - 3u) / 3u) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    return false;
  }
  const device VkrGpuGeometryRow &geometry =
      root.geometry_rows[visible.geometry_index];
  device const uint *indices = reinterpret_cast<device const uint *>(
      geometry.index_address + ulong(visible.first_index) * sizeof(uint));
  device const VkrPackedStaticVertex *vertex_rows =
      reinterpret_cast<device const VkrPackedStaticVertex *>(
          geometry.vertex_address);
  device const VkrGpuGeometryDecodeRecord &decode =
      *reinterpret_cast<device const VkrGpuGeometryDecodeRecord *>(
          geometry.decode_address);
  VkrGpuDecodedVertex vertices[3];
  float4 clip[3];
  const device VkrMetalPacketInstance &instance =
      root.instances[visible.instance_index];
  for (uint corner = 0u; corner < 3u; ++corner) {
    int vertex_index =
        int(indices[visibility.y * 3u + corner]) + visible.vertex_offset;
    if (vertex_index < 0) {
      atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count,
                                1u, memory_order_relaxed);
      return false;
    }
    vertices[corner] = vkr_decode_packed_vertex(
        vertex_rows[geometry.first_vertex + uint(vertex_index)], decode);
    float3 position = vertices[corner].position;
    clip[corner] =
        root.view_projection * (instance.model * float4(position, 1.0));
  }

  float2 center = float2(pixel) + 0.5;
  float3 barycentric;
  float3 barycentric_dx;
  float3 barycentric_dy;
  if (!vkr_metal_packet_perspective_barycentrics(
          vkr_metal_packet_resolve_ndc(center, root.extent), clip,
          barycentric) ||
      !vkr_metal_packet_perspective_barycentrics(
          vkr_metal_packet_resolve_ndc(center + float2(1.0, 0.0), root.extent),
          clip, barycentric_dx) ||
      !vkr_metal_packet_perspective_barycentrics(
          vkr_metal_packet_resolve_ndc(center + float2(0.0, 1.0), root.extent),
          clip, barycentric_dy)) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    return false;
  }

  float2 uv[3] = {vertices[0].texcoord, vertices[1].texcoord,
                  vertices[2].texcoord};
  float2 texcoord =
      uv[0] * barycentric.x + uv[1] * barycentric.y + uv[2] * barycentric.z;
  float2 texcoord_dx = uv[0] * barycentric_dx.x + uv[1] * barycentric_dx.y +
                       uv[2] * barycentric_dx.z;
  float2 texcoord_dy = uv[0] * barycentric_dy.x + uv[1] * barycentric_dy.y +
                       uv[2] * barycentric_dy.z;
  float2 gradient_x = texcoord_dx - texcoord;
  float2 gradient_y = texcoord_dy - texcoord;
  if (!all(isfinite(texcoord)) || !all(isfinite(gradient_x)) ||
      !all(isfinite(gradient_y))) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    return false;
  }

  const device VkrMetalPacketMaterial &material =
      root.materials[visible.material_index];
  gradient2d gradients(gradient_x, gradient_y);
  float4 vertex_color = vertices[0].color * barycentric.x +
                        vertices[1].color * barycentric.y +
                        vertices[2].color * barycentric.z;
  float4 base = material.base_color_texture.sample(material.base_color_sampler,
                                                   texcoord, gradients) *
                material.tint * vertex_color;
  float metallic = saturate(material.material_surface.x);
  float roughness = clamp(material.material_surface.y, 0.04, 1.0);
  float occlusion = saturate(material.material_surface.w);
  if ((material.flags & 2u) != 0u) {
    float3 orm =
        material.orm_texture.sample(material.orm_sampler, texcoord, gradients)
            .rgb;
    occlusion *= orm.r;
    roughness = clamp(roughness * orm.g, 0.04, 1.0);
    metallic = saturate(metallic * orm.b);
  }

  float3 object_normal = vertices[0].normal * barycentric.x +
                         vertices[1].normal * barycentric.y +
                         vertices[2].normal * barycentric.z;
  float4 object_tangent = vertices[0].tangent * barycentric.x +
                          vertices[1].tangent * barycentric.y +
                          vertices[2].tangent * barycentric.z;
  float3 transformed_normal = (instance.model * float4(object_normal, 0.0)).xyz;
  float face_sign;
  if (!vkr_metal_packet_projected_face_sign(clip, face_sign) ||
      !vkr_metal_packet_finite_nonzero(transformed_normal)) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    return false;
  }
  float3 normal = normalize(transformed_normal) * face_sign;
  if ((material.flags & 1u) != 0u) {
    float3 sampled =
        material.normal_texture
                .sample(material.normal_sampler, texcoord, gradients)
                .xyz *
            2.0 -
        1.0;
    sampled.xy *= material.material_surface.z;
    sampled.y = -sampled.y;
    float3 tangent = (instance.model * float4(object_tangent.xyz, 0.0)).xyz;
    if (!vkr_metal_packet_finite_nonzero(sampled) ||
        !vkr_metal_packet_finite_nonzero(tangent)) {
      atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count,
                                1u, memory_order_relaxed);
      return false;
    }
    tangent = normalize(tangent);
    tangent -= dot(tangent, normal) * normal;
    if (!vkr_metal_packet_finite_nonzero(tangent)) {
      atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count,
                                1u, memory_order_relaxed);
      return false;
    }
    tangent = normalize(tangent);
    float3 bitangent =
        normalize(cross(normal, tangent)) * sign(object_tangent.w);
    float3 mapped =
        tangent * sampled.x + bitangent * sampled.y + normal * sampled.z;
    if (!vkr_metal_packet_finite_nonzero(bitangent) ||
        !vkr_metal_packet_finite_nonzero(mapped)) {
      atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count,
                                1u, memory_order_relaxed);
      return false;
    }
    normal = normalize(mapped);
  }
  float3 emissive = material.material_emissive.rgb;
  if ((material.flags & 4u) != 0u)
    emissive *= material.emissive_texture
                    .sample(material.emissive_sampler, texcoord, gradients)
                    .rgb;
  float3 object_position = vertices[0].position * barycentric.x +
                           vertices[1].position * barycentric.y +
                           vertices[2].position * barycentric.z;
  surface = {base,
             normal,
             metallic,
             roughness,
             occlusion,
             emissive,
             visible.material_index,
             object_position,
             visible.instance_index,
             visibility.y};
  return true;
}

static float3 vkr_metal_packet_transmission_lighting(
    constant VkrMetalPacketFrameRoot *frame,
    const device VkrMetalPacketMaterial &material,
    thread const VkrMetalPacketTransmissionSurface &surface,
    float3 world_position) {
  float3 f0 = mix(saturate(material.material_dielectric_specular.rgb),
                  surface.base.rgb, surface.metallic);
  if (frame->render_mode == 2u)
    return surface.normal * 0.5 + 0.5;
  if (frame->render_mode == 3u)
    return surface.base.rgb + surface.emissive;
  if (frame->render_mode == 6u)
    return float3(surface.metallic, surface.roughness,
                  max(f0.x, max(f0.y, f0.z)));
  if (frame->shadow_debug_mode != 0u) {
    VkrMetalPacketShadowSample shadow_sample =
        vkr_metal_packet_directional_shadow_sample(frame, world_position,
                                                   surface.normal);
    return vkr_metal_packet_shadow_debug_color(frame->shadow_debug_mode,
                                               shadow_sample);
  }
  float3 view = normalize(frame->view_position.xyz - world_position);
  float no_v = max(dot(surface.normal, view), 0.0);
  float3 analytic_diffuse = 0.0;
  float3 analytic_specular = 0.0;
  if (frame->directional_direction_enabled.w > 0.5) {
    VkrMetalPacketShadowSample shadow_sample =
        vkr_metal_packet_directional_shadow_sample(frame, world_position,
                                                   surface.normal);
    VkrMetalPacketDirectResult direct = vkr_metal_packet_direct(
        surface.normal, view,
        normalize(-frame->directional_direction_enabled.xyz),
        frame->directional_color_intensity.rgb *
            frame->directional_color_intensity.w * shadow_sample.factor,
        surface.base.rgb, surface.metallic, surface.roughness, f0);
    analytic_diffuse = direct.diffuse;
    analytic_specular = direct.specular;
  }
  uint4 point_mask = vkr_metal_packet_point_light_mask(frame, world_position);
  uint point_count = min(frame->point_light_count, 128u);
  for (uint word = 0u; word < 4u; ++word) {
    /* SIMD-uniform light index; see vkr_metal_packet_deferred_lighting. */
    uint remaining = simd_or(point_mask[word]);
    while (remaining != 0u) {
      uint bit = ctz(remaining);
      remaining &= remaining - 1u;
      uint light_index = word * 32u + bit;
      if (light_index >= point_count)
        continue;
      float4 p0 = frame->point_light_data[light_index * 4u + 0u];
      float4 p1 = frame->point_light_data[light_index * 4u + 1u];
      float4 p2 = frame->point_light_data[light_index * 4u + 2u];
      float4 p3 = frame->point_light_data[light_index * 4u + 3u];
      if ((point_mask[word] & (1u << bit)) == 0u)
        continue;
      uint kind = uint(p2.w + 0.5);
      float3 to_light = p0.xyz - world_position;
      float distance_squared = dot(to_light, to_light);
      if (kind != 0u && p2.z > 0.0 && distance_squared >= p2.z * p2.z)
        continue;
      float distance = sqrt(distance_squared);
      float3 light_direction =
          distance > 1e-6 ? to_light / distance : float3(0.0);
      float attenuation = 0.0;
      if (kind == 0u) {
        attenuation = 1.0 / max(max(p0.w, 1.0) + p1.w * distance +
                                    p2.y * distance_squared,
                                1e-6);
      } else {
        float ratio = p2.z > 0.0 ? distance / p2.z : 0.0;
        float range_attenuation =
            p2.z > 0.0 ? saturate(1.0 - ratio * ratio * ratio * ratio) : 1.0;
        attenuation =
            range_attenuation * range_attenuation / max(distance_squared, 1e-4);
        if (kind == 2u) {
          float cone = dot(-light_direction, normalize(p3.xyz));
          float cone_attenuation = smoothstep(p1.w, p0.w, cone);
          if (cone_attenuation <= 0.0)
            continue;
          attenuation *= cone_attenuation;
        }
      }
      VkrMetalPacketDirectResult direct = vkr_metal_packet_direct(
          surface.normal, view, light_direction, p1.rgb * p2.x * attenuation,
          surface.base.rgb, surface.metallic, surface.roughness, f0);
      analytic_diffuse += direct.diffuse;
      analytic_specular += direct.specular;
    }
  }
  if (frame->render_mode == 1u)
    return analytic_diffuse + analytic_specular;
  if (frame->render_mode == 4u)
    return analytic_diffuse;
  if (frame->render_mode == 5u)
    return analytic_specular;
  float3 color = analytic_diffuse + analytic_specular;
  if ((frame->flags & 2u) != 0u) {
    constexpr sampler environment_sampler(coord::normalized,
                                          address::clamp_to_edge,
                                          filter::linear, mip_filter::linear);
    float3 reflection = reflect(-view, surface.normal);
    float3 fresnel =
        vkr_metal_packet_fresnel_roughness(no_v, f0, surface.roughness);
    float3 kd = (1.0 - fresnel) * (1.0 - surface.metallic);
    float3 global_irradiance =
        frame->irradiance.sample(environment_sampler, surface.normal).rgb;
    float3 global_prefiltered =
        frame->prefilter
            .sample(environment_sampler, reflection,
                    level(surface.roughness *
                          float(max(frame->prefilter_mip_count, 1u) - 1u)))
            .rgb;
    float2 brdf = vkr_metal_packet_brdf_approximation(no_v, surface.roughness);
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0);
    float horizon = saturate(1.0 + dot(reflection, surface.normal));
    float specular_visibility = horizon * horizon *
                                vkr_metal_packet_specular_ao(
                                    surface.occlusion, no_v, surface.roughness);
    float3 diffuse = 0.0;
    float3 specular = 0.0;
    float local_weight_sum = 0.0;
    uint probe_count = min(frame->ibl_probe_count, 16u);
    if (frame->ibl_probes != nullptr) {
      for (uint i = 0u; i < probe_count; ++i)
        local_weight_sum += vkr_metal_packet_probe_influence(
            frame->ibl_probes[i], world_position);
      float weight_scale =
          local_weight_sum > 1.0 ? 1.0 / local_weight_sum : 1.0;
      for (uint i = 0u; i < probe_count; ++i) {
        const device VkrMetalPacketIblProbe &probe = frame->ibl_probes[i];
        float weight = vkr_metal_packet_probe_influence(probe, world_position) *
                       weight_scale;
        if (weight <= 1e-6)
          continue;
        float3 probe_reflection =
            probe.intensity_box.w > 0.5
                ? vkr_metal_packet_box_project(
                      reflection, world_position, probe.center_blend.xyz,
                      max(probe.extents_weight.xyz, 0.0))
                : reflection;
        float3 probe_irradiance =
            probe.irradiance.sample(environment_sampler, surface.normal).rgb;
        float3 probe_prefiltered =
            probe.prefilter
                .sample(environment_sampler, probe_reflection,
                        level(surface.roughness *
                              float(max(frame->prefilter_mip_count, 1u) - 1u)))
                .rgb;
        diffuse += kd * probe_irradiance * surface.base.rgb *
                   surface.occlusion * probe.intensity_box.x *
                   probe.intensity_box.y * weight;
        specular += probe_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                    specular_visibility * probe.intensity_box.x *
                    probe.intensity_box.z * weight;
      }
    }
    float global_weight = max(1.0 - min(local_weight_sum, 1.0), 0.0);
    diffuse += kd * global_irradiance * surface.base.rgb * surface.occlusion *
               global_weight;
    specular += global_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                specular_visibility * global_weight;
    color +=
        (diffuse * frame->ibl_controls.y + specular * frame->ibl_controls.z) *
        frame->ibl_controls.x;
  } else {
    color += frame->ambient_color.rgb * surface.base.rgb * surface.occlusion;
  }
  return color + surface.emissive;
}

static void vkr_metal_packet_write_transmission_temporal(
    constant VkrMetalPacketTransmissionShadeRoot &root,
    const thread VkrMetalPacketTransmissionSurface &surface, uint2 pixel) {
  if (root.temporal_outputs_enabled == 0u)
    return;
  float2 motion = 0.0;
  float2 validity = 0.0;
  const device VkrMetalPacketInstance &instance =
      root.instances[surface.instance_index];
  if (root.history_valid != 0u &&
      (instance.temporal_flags & VKR_INSTANCE_TEMPORAL_OWNER) != 0u &&
      instance.temporal_index < VKR_TEMPORAL_TRANSFORM_CAPACITY) {
    const device VkrTemporalTransform &previous =
        root.previous_transforms[instance.temporal_index];
    if (previous.valid != 0u &&
        previous.generation == instance.temporal_generation &&
        previous.frame_index == root.previous_frame_index) {
      float4 current_clip =
          root.current_view_projection *
          (instance.model * float4(surface.object_position, 1.0));
      float4 previous_clip =
          root.previous_view_projection *
          (previous.model * float4(surface.object_position, 1.0));
      if (current_clip.w > 1e-6 && previous_clip.w > 1e-6) {
        float2 current_ndc = current_clip.xy / current_clip.w;
        float2 previous_ndc = previous_clip.xy / previous_clip.w;
        float2 current_uv =
            float2(current_ndc.x * 0.5 + 0.5, 0.5 - current_ndc.y * 0.5);
        float2 previous_uv =
            float2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5);
        motion = previous_uv - current_uv;
        validity = float2(2.0 + saturate(root.materials[surface.material_index]
                                             .temporal_reactivity),
                          previous_clip.z / previous_clip.w);
      }
    }
  }
  root.motion.write(float4(motion, 0.0, 0.0), pixel);
  root.validity.write(float4(validity, 0.0, 0.0), pixel);
}

kernel void vkr_metal_packet_transmission_shade(
    constant VkrMetalPacketTransmissionShadeRoot &root [[buffer(0)]],
    uint3 grid_position [[thread_position_in_grid]]) {
  uint2 pixel = grid_position.xy;
  if (root.compact_enabled != 0u) {
    if (root.compact_layer >= 4u || root.pixel_list == nullptr ||
        root.compact_counts == nullptr)
      return;
    uint compact_count =
        min(atomic_load_explicit(&root.compact_counts[root.compact_layer],
                                 memory_order_relaxed),
            root.pixel_capacity);
    if (grid_position.x >= compact_count)
      return;
    uint packed = root.pixel_list[grid_position.x];
    pixel = uint2(packed & 0xffffu, packed >> 16u);
  }
  if (any(pixel >= root.extent))
    return;
  float4 background = root.source.read(pixel);
  if (root.vbuffer.read(pixel).x == 0u) {
    root.destination.write(background, pixel);
    return;
  }
  if (root.temporal_outputs_enabled != 0u) {
    root.motion.write(float4(0.0), pixel);
    root.validity.write(float4(0.0), pixel);
  }
  VkrMetalPacketTransmissionSurface surface;
  if (!vkr_metal_packet_resolve_transmission_surface(root, pixel, surface)) {
    root.destination.write(background, pixel);
    return;
  }
  vkr_metal_packet_write_transmission_temporal(root, surface, pixel);
  constant VkrMetalPacketFrameRoot *frame = root.frame;
  const device VkrMetalPacketMaterial &material =
      root.materials[surface.material_index];
  float depth = root.depth.read(pixel).x;
  float2 ndc = vkr_metal_packet_resolve_ndc(float2(pixel) + 0.5, root.extent);
  float4 world_h = root.inverse_view_projection * float4(ndc, depth, 1.0);
  float safe_world_w = copysign(max(abs(world_h.w), 1e-7), world_h.w);
  float3 world_position = world_h.xyz / safe_world_w;
  float3 color = vkr_metal_packet_transmission_lighting(
      frame, material, surface, world_position);
  if (frame->render_mode == 0u && frame->shadow_debug_mode == 0u &&
      material.material_alpha.y > 0.0) {
    float3 view = normalize(frame->view_position.xyz - world_position);
    float no_v = max(dot(surface.normal, view), 0.0);
    float3 f0 = mix(saturate(material.material_dielectric_specular.rgb),
                    surface.base.rgb, surface.metallic);
    float thickness = max(material.material_alpha.w, 0.0);
    float3 view_normal =
        normalize((frame->view * float4(surface.normal, 0.0)).xyz);
    float3 refracted = refract(float3(0.0, 0.0, -1.0), view_normal,
                               1.0 / max(material.material_alpha.z, 1.0));
    float2 screen_uv = (float2(pixel) + 0.5) / float2(root.extent);
    float2 offset =
        refracted.xy / max(abs(refracted.z), 0.25) * thickness * 0.02;
    constexpr sampler transmission_sampler(
        coord::normalized, address::clamp_to_edge, filter::linear);
    float3 transmitted =
        root.source
            .sample(transmission_sampler, clamp(screen_uv + offset, 0.0, 1.0))
            .rgb;
    if (material.material_attenuation_color.w > 1e-4 && thickness > 0.0) {
      transmitted *=
          pow(clamp(material.material_attenuation_color.rgb, 1e-4, 1.0),
              thickness / material.material_attenuation_color.w);
    }
    float3 fresnel = vkr_metal_packet_fresnel(no_v, f0);
    float weight = saturate(material.material_alpha.y) *
                   (1.0 - surface.metallic) *
                   (1.0 - max(fresnel.x, max(fresnel.y, fresnel.z)));
    color = mix(color, transmitted, saturate(weight));
  }
  root.destination.write(float4(color, 1.0), pixel);
}

/* The dispatch fixes this kernel's threadgroup at 8x8, so it never exceeds
   eight SIMD-groups for any SIMD width down to eight lanes. */
constant uint vkr_metal_compact_simd_max = 8u;

kernel void vkr_metal_packet_transmission_compact(
    constant VkrMetalPacketTransmissionCompactRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]],
    ushort simd_group [[simdgroup_index_in_threadgroup]],
    ushort simd_group_count [[simdgroups_per_threadgroup]]) {
  if (root.layer >= 4u)
    return;
  if (all(pixel < root.extent))
    root.destination.write(root.source.read(pixel), pixel);
  threadgroup uint simd_totals[vkr_metal_compact_simd_max];
  threadgroup uint group_base;
  bool covered = all(pixel < root.extent) && root.vbuffer.read(pixel).x != 0u;

  /* Two-level scan: a SIMD prefix sum for the lane offset plus one threadgroup
     slot per SIMD-group, replacing a 64-iteration serial scan run by lane 0
     with the rest of the threadgroup idle. Ordering stays SIMD-group then lane,
     which is the order the serial scan produced. */
  uint lane_offset = simd_prefix_exclusive_sum(covered ? 1u : 0u);
  uint simd_total = simd_sum(covered ? 1u : 0u);
  if (simd_is_first())
    simd_totals[simd_group] = simd_total;
  threadgroup_barrier(mem_flags::mem_threadgroup);

  uint simd_base = 0u;
  for (uint i = 0u; i < (uint)simd_group; ++i)
    simd_base += simd_totals[i];

  if (simd_group == 0u && simd_is_first()) {
    uint total = 0u;
    for (uint i = 0u; i < (uint)simd_group_count; ++i)
      total += simd_totals[i];
    group_base = atomic_fetch_add_explicit(&root.covered_pixels[root.layer],
                                           total, memory_order_relaxed);
    uint writable = group_base < root.capacity
                        ? min(total, root.capacity - group_base)
                        : 0u;
    if (writable < total)
      atomic_fetch_add_explicit(&root.overflow_counts[root.layer],
                                total - writable, memory_order_relaxed);
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (covered) {
    uint destination = group_base + simd_base + lane_offset;
    if (destination < root.capacity)
      root.pixel_list[destination] = (pixel.y << 16u) | pixel.x;
  }
}

kernel void vkr_metal_packet_transmission_compact_finalize(
    constant VkrMetalPacketTransmissionCompactRoot &root [[buffer(0)]],
    uint thread_index [[thread_position_in_grid]]) {
  if (thread_index != 0u)
    return;
  uint count = min(atomic_load_explicit(&root.covered_pixels[root.layer],
                                        memory_order_relaxed),
                   root.capacity);
  root.indirect_arguments[0] = (count + 63u) / 64u;
  root.indirect_arguments[1] = 1u;
  root.indirect_arguments[2] = 1u;
}

kernel void vkr_metal_packet_transmission_coverage(
    constant VkrMetalPacketTransmissionCoverageRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]],
    ushort thread_index [[thread_index_in_threadgroup]]) {
  threadgroup atomic_uint group_coverage;
  if (thread_index == 0u)
    atomic_store_explicit(&group_coverage, 0u, memory_order_relaxed);
  threadgroup_barrier(mem_flags::mem_threadgroup);
  /* One threadgroup atomic per SIMD-group rather than one per covered lane; a
     full-screen dispatch otherwise serializes every lane onto one address. */
  bool covered = all(pixel < root.extent) && root.vbuffer.read(pixel).x != 0u;
  uint simd_covered = simd_sum(covered ? 1u : 0u);
  if (simd_is_first() && simd_covered != 0u)
    atomic_fetch_add_explicit(&group_coverage, simd_covered,
                              memory_order_relaxed);
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (thread_index == 0u && root.layer < 4u)
    atomic_fetch_add_explicit(
        &root.covered_pixels[root.layer],
        atomic_load_explicit(&group_coverage, memory_order_relaxed),
        memory_order_relaxed);
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

struct VkrMetalPacketSdsmState {
  atomic_uint min_device_z_bits;
  atomic_uint max_device_z_bits;
  atomic_uint occupied_count;
  uint reserved;
};

struct VkrMetalPacketSdsmRoot {
  texture2d<float, access::read> depth;
  texture2d<uint, access::read> vbuffer;
  device VkrMetalPacketSdsmState *reduce_state;
  uint2 extent;
};

kernel void vkr_metal_packet_sdsm_reduce(constant VkrMetalPacketSdsmRoot &root
                                         [[buffer(0)]],
                                         uint2 pixel
                                         [[thread_position_in_grid]]) {
  bool occupied = all(pixel < root.extent) && root.vbuffer.read(pixel).x != 0u;
  float depth = occupied ? root.depth.read(pixel).x : 0.0f;
  uint occupied_in_simd = simd_sum(occupied ? 1u : 0u);
  float minimum_in_simd = simd_min(occupied ? depth : 1.0f);
  float maximum_in_simd = simd_max(occupied ? depth : 0.0f);
  if (simd_is_first() && occupied_in_simd != 0u) {
    atomic_fetch_min_explicit(&root.reduce_state->min_device_z_bits,
                              as_type<uint>(minimum_in_simd),
                              memory_order_relaxed);
    atomic_fetch_max_explicit(&root.reduce_state->max_device_z_bits,
                              as_type<uint>(maximum_in_simd),
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&root.reduce_state->occupied_count,
                              occupied_in_simd, memory_order_relaxed);
  }
}

static_assert(sizeof(VkrGpuDrawCompactionState) == 80,
              "GPU draw compaction state ABI must remain 80 bytes");
static_assert(sizeof(VkrMetalPacketTemporalTransformRoot) == 32,
              "Temporal-transform root ABI must remain 32 bytes");
static_assert(sizeof(VkrMetalPacketGBufferResolveRoot) == 352,
              "G-buffer resolve root ABI must remain 352 bytes");
static_assert(sizeof(VkrMetalPacketTemporalResolveRoot) == 208,
              "Temporal-resolve root ABI must remain 208 bytes");
static_assert(sizeof(VkrMetalPacketDeferredLightingRoot) == 160,
              "Deferred-lighting root ABI must remain 160 bytes");
static_assert(sizeof(VkrMetalPacketTransmissionShadeRoot) == 448,
              "Transmission-shade root ABI must remain 448 bytes");
static_assert(sizeof(VkrMetalPacketTransmissionCoverageRoot) == 32,
              "Transmission-coverage root ABI must remain 32 bytes");
static_assert(sizeof(VkrMetalPacketTransmissionCompactRoot) == 80,
              "Transmission-compact root ABI must remain 80 bytes");
static_assert(sizeof(VkrMetalPacketGpuDrawRoot) == 192,
              "GPU draw root ABI must remain 192 bytes");
static_assert(sizeof(VkrMetalPacketTransmissionPeelRoot) == 16,
              "Transmission-peel root ABI must remain 16 bytes");
static_assert(sizeof(VkrMetalPacketGpuDrawView) == 112,
              "GPU draw view ABI must remain 112 bytes");
static_assert(sizeof(VkrMetalPacketHzbBuildRoot) == 48,
              "HZB build root ABI must remain 48 bytes");
static_assert(sizeof(VkrMetalPacketSdsmRoot) == 32,
              "SDSM root ABI must remain 32 bytes");
static_assert(sizeof(VkrMetalPacketSdsmState) == 16,
              "SDSM state ABI must remain 16 bytes");
