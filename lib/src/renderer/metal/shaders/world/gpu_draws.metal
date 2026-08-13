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
  device VkrMetalPacketDrawRoot *draw_root;
  float4 frustum_planes[6];
  uint candidate_count;
  uint visible_capacity;
  uint reserved_0;
  uint reserved_1;
  texture2d<float, access::read> hzb;
  ulong reserved_2;
  float4x4 history_view_projection;
  uint2 hzb_extent;
  uint hzb_mip_count;
  uint hzb_enabled;
  float hzb_depth_epsilon;
  uint reserved_3[3];
};

struct VkrMetalPacketIcbContainer {
  command_buffer command_buffer [[id(0)]];
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

static bool vkr_metal_packet_candidate_in_frustum(
    constant VkrMetalPacketGpuDrawRoot &root,
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
    float4 equation = root.frustum_planes[plane];
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
                                   uint index [[thread_position_in_grid]]) {
  if (index >= root.candidate_count)
    return;
  const device VkrGpuCandidateDrawRow &candidate = root.candidates[index];
  if (candidate.state_bucket >= 4u ||
      !vkr_metal_packet_candidate_in_frustum(root, candidate)) {
    root.classifications[index] = 0u;
    return;
  }
  if (vkr_metal_packet_candidate_occluded(root, candidate)) {
    root.classifications[index] = 0u;
    atomic_fetch_add_explicit(&root.compaction_state->occlusion_culled_count,
                              1u, memory_order_relaxed);
    return;
  }
  root.classifications[index] = candidate.state_bucket + 1u;
  atomic_fetch_add_explicit(
      &root.compaction_state->bucket_counts[candidate.state_bucket], 1u,
      memory_order_relaxed);
}

kernel void
vkr_metal_packet_gpu_draw_prefix(constant VkrMetalPacketGpuDrawRoot &root
                                 [[buffer(0)]],
                                 uint index [[thread_position_in_grid]]) {
  if (index != 0u)
    return;
  uint visible_count = 0u;
  for (uint bucket = 0u; bucket < 4u; ++bucket) {
    uint bucket_count = atomic_load_explicit(
        &root.compaction_state->bucket_counts[bucket], memory_order_relaxed);
    root.compaction_state->execution_ranges[bucket] =
        uint2(visible_count, bucket_count);
    visible_count += bucket_count;
    atomic_store_explicit(&root.compaction_state->bucket_cursors[bucket], 0u,
                          memory_order_relaxed);
  }
  root.compaction_state->visible_count = visible_count;
  const bool overflow = visible_count > root.visible_capacity;
  if (overflow) {
    for (uint bucket = 0u; bucket < 4u; ++bucket)
      root.compaction_state->execution_ranges[bucket].y = 0u;
  }
  atomic_store_explicit(&root.compaction_state->overflow_count,
                        overflow ? visible_count - root.visible_capacity : 0u,
                        memory_order_relaxed);
}

kernel void vkr_metal_packet_gpu_draw_encode(
    constant VkrMetalPacketGpuDrawRoot &root [[buffer(0)]],
    constant VkrMetalPacketIcbContainer *icb [[buffer(1)]],
    uint index [[thread_position_in_grid]]) {
  if (index >= root.candidate_count || icb == nullptr)
    return;
  uint classification = root.classifications[index];
  if (classification == 0u)
    return;
  uint bucket = classification - 1u;
  uint local_index = atomic_fetch_add_explicit(
      &root.compaction_state->bucket_cursors[bucket], 1u, memory_order_relaxed);
  uint visible_index =
      root.compaction_state->execution_ranges[bucket].x + local_index;
  if (visible_index >= root.visible_capacity) {
    atomic_fetch_add_explicit(&root.compaction_state->overflow_count, 1u,
                              memory_order_relaxed);
    return;
  }

  const device VkrGpuCandidateDrawRow &candidate = root.candidates[index];
  root.visible_rows[visible_index] = {
      candidate.geometry_index, candidate.material_index,
      candidate.instance_index, candidate.first_index,
      candidate.index_count,    candidate.vertex_offset,
      candidate.state_bucket,   candidate.flags};
  const device VkrGpuGeometryRow &geometry =
      root.geometry_rows[candidate.geometry_index];
  device uint *indices = reinterpret_cast<device uint *>(
      geometry.index_address + ulong(candidate.first_index) * sizeof(uint));

  render_command command(icb->command_buffer, visible_index);
  command.set_vertex_buffer(root.draw_root, 0u);
  command.set_fragment_buffer(root.draw_root, 1u);
  command.draw_indexed_primitives(primitive_type::triangle,
                                  candidate.index_count, indices, 1u,
                                  candidate.vertex_offset, visible_index);
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
  uint2 extent;
  uint visible_capacity;
  uint geometry_count;
  uint material_count;
  uint instance_count;
  uint render_mode;
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
}

static bool vkr_metal_packet_affine_barycentrics(float2 point, float2 p0,
                                                 float2 p1, float2 p2,
                                                 thread float3 &barycentrics) {
  float2 e0 = p1 - p0;
  float2 e1 = p2 - p0;
  float determinant = e0.x * e1.y - e0.y * e1.x;
  if (!isfinite(determinant) || abs(determinant) <= 1e-8)
    return false;
  float2 relative = point - p0;
  float b1 = (relative.x * e1.y - relative.y * e1.x) / determinant;
  float b2 = (e0.x * relative.y - e0.y * relative.x) / determinant;
  barycentrics = float3(1.0 - b1 - b2, b1, b2);
  return all(isfinite(barycentrics));
}

static bool vkr_metal_packet_perspective_barycentrics(
    float2 point, thread const float4 (&clip)[3], thread float3 &barycentrics) {
  float2 ndc[3];
  float3 inverse_w;
  for (uint i = 0u; i < 3u; ++i) {
    if (!all(isfinite(clip[i])) || abs(clip[i].w) <= 1e-7)
      return false;
    ndc[i] = clip[i].xy / clip[i].w;
    inverse_w[i] = 1.0 / clip[i].w;
  }
  float3 affine;
  if (!vkr_metal_packet_affine_barycentrics(point, ndc[0], ndc[1], ndc[2],
                                            affine))
    return false;
  float3 weighted = affine * inverse_w;
  float normalization = weighted.x + weighted.y + weighted.z;
  if (!isfinite(normalization) || abs(normalization) <= 1e-8)
    return false;
  barycentrics = weighted / normalization;
  return all(isfinite(barycentrics));
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
  if (geometry.vertex_address == 0u || geometry.index_address == 0u ||
      geometry.vertex_stride != sizeof(VkrMetalPacketVertex) ||
      geometry.vertex_layout != 0u) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
    return;
  }
  device const uint *indices = reinterpret_cast<device const uint *>(
      geometry.index_address + ulong(visible.first_index) * sizeof(uint));
  VkrMetalPacketVertex vertices[3];
  float4 clip[3];
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
    device const VkrMetalPacketVertex *vertex_rows =
        reinterpret_cast<device const VkrMetalPacketVertex *>(
            geometry.vertex_address);
    vertices[corner] = vertex_rows[geometry.first_vertex + uint(vertex_index)];
    float3 position =
        float3(vertices[corner].position_x, vertices[corner].position_y,
               vertices[corner].position_z);
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
  if (!all(isfinite(texcoord)) || !all(isfinite(gradient_x)) ||
      !all(isfinite(gradient_y))) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
    return;
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

  float3 object_normal =
      float3(vertices[0].normal_x, vertices[0].normal_y, vertices[0].normal_z) *
          barycentric.x +
      float3(vertices[1].normal_x, vertices[1].normal_y, vertices[1].normal_z) *
          barycentric.y +
      float3(vertices[2].normal_x, vertices[2].normal_y, vertices[2].normal_z) *
          barycentric.z;
  float4 object_tangent = vertices[0].tangent * barycentric.x +
                          vertices[1].tangent * barycentric.y +
                          vertices[2].tangent * barycentric.z;
  float2 screen[3];
  for (uint corner = 0u; corner < 3u; ++corner) {
    float2 ndc = clip[corner].xy / clip[corner].w;
    screen[corner] = float2((ndc.x * 0.5 + 0.5) * root.extent.x,
                            (0.5 - ndc.y * 0.5) * root.extent.y);
  }
  float signed_area =
      (screen[1].x - screen[0].x) * (screen[2].y - screen[0].y) -
      (screen[1].y - screen[0].y) * (screen[2].x - screen[0].x);
  float3 transformed_normal = (instance.model * float4(object_normal, 0.0)).xyz;
  if (!isfinite(signed_area) || abs(signed_area) <= 1e-8 ||
      !vkr_metal_packet_finite_nonzero(transformed_normal)) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
    return;
  }
  float face_sign = signed_area < 0.0 ? 1.0 : -1.0;
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
    float3 bitangent =
        normalize(cross(normal, tangent)) * sign(object_tangent.w);
    float3 mapped =
        tangent * sampled.x + bitangent * sampled.y + normal * sampled.z;
    if (!vkr_metal_packet_finite_nonzero(bitangent) ||
        !vkr_metal_packet_finite_nonzero(mapped)) {
      atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count,
                                1u, memory_order_relaxed);
      vkr_metal_packet_resolve_defaults(root, pixel, -1.0);
      return;
    }
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
  if (root.vbuffer.read(pixel).x == 0u) {
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
  if (frame->render_mode == 6u) {
    root.hdr.write(
        float4(hdr_seed.a, roughness, max(f0.x, max(f0.y, f0.z)), 1.0), pixel);
    return;
  }
  float3 world_position = vkr_metal_packet_deferred_world_position(
      root, pixel, root.depth.read(pixel).x);
  float3 view = normalize(frame->view_position.xyz - world_position);
  float no_v = max(dot(normal, view), 0.0);
  float3 analytic_diffuse = 0.0;
  float3 analytic_specular = 0.0;

  if (frame->directional_direction_enabled.w > 0.5) {
    float shadow = vkr_metal_packet_directional_shadow(frame, world_position);
    VkrMetalPacketDirectResult direct = vkr_metal_packet_direct_deferred(
        normal, view, normalize(-frame->directional_direction_enabled.xyz),
        frame->directional_color_intensity.rgb *
            frame->directional_color_intensity.w * shadow,
        diffuse_albedo, roughness, f0);
    analytic_diffuse = direct.diffuse;
    analytic_specular = direct.specular;
  }

  uint4 point_mask = vkr_metal_packet_point_light_mask(frame, world_position);
  uint point_count = min(frame->point_light_count, 128u);
  for (uint word = 0u; word < 4u; ++word) {
    uint remaining = point_mask[word];
    while (remaining != 0u) {
      uint light_index = word * 32u + ctz(remaining);
      remaining &= remaining - 1u;
      if (light_index >= point_count)
        continue;
      float4 p0 = frame->point_light_data[light_index * 4u + 0u];
      float4 p1 = frame->point_light_data[light_index * 4u + 1u];
      float4 p2 = frame->point_light_data[light_index * 4u + 2u];
      float4 p3 = frame->point_light_data[light_index * 4u + 3u];
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
        diffuse += (1.0 - fresnel) * probe_irradiance * diffuse_albedo * ao *
                   probe.intensity_box.x * probe.intensity_box.y * weight;
        specular += probe_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                    specular_visibility * probe.intensity_box.x *
                    probe.intensity_box.z * weight;
      }
    }
    float global_weight = max(1.0 - min(local_weight_sum, 1.0), 0.0);
    diffuse += (1.0 - fresnel) * global_irradiance * diffuse_albedo * ao *
               global_weight;
    specular += global_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                specular_visibility * global_weight;
    color +=
        (diffuse * frame->ibl_controls.y + specular * frame->ibl_controls.z) *
        frame->ibl_controls.x;
  } else {
    color += frame->ambient_color.rgb * diffuse_albedo * ao;
  }
  color += hdr_seed.rgb;
  root.hdr.write(float4(color, 1.0), pixel);
}

struct VkrMetalPacketTransmissionShadeRoot {
  constant VkrMetalPacketFrameRoot *frame;
  device VkrGpuVisibleDrawRow *visible_rows;
  device VkrGpuGeometryRow *geometry_rows;
  device VkrMetalPacketInstance *instances;
  device VkrMetalPacketMaterial *materials;
  device VkrGpuDrawCompactionState *compaction_state;
  texture2d<uint, access::read> vbuffer;
  texture2d<float, access::read> depth;
  texture2d<float, access::sample> source;
  texture2d<float, access::read_write> destination;
  float4x4 view_projection;
  float4x4 inverse_view_projection;
  uint2 extent;
  uint visible_capacity;
  uint geometry_count;
  uint material_count;
  uint instance_count;
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
  if (geometry.vertex_address == 0u || geometry.index_address == 0u ||
      geometry.vertex_stride != sizeof(VkrMetalPacketVertex) ||
      geometry.vertex_layout != 0u) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    return false;
  }
  device const uint *indices = reinterpret_cast<device const uint *>(
      geometry.index_address + ulong(visible.first_index) * sizeof(uint));
  device const VkrMetalPacketVertex *vertex_rows =
      reinterpret_cast<device const VkrMetalPacketVertex *>(
          geometry.vertex_address);
  VkrMetalPacketVertex vertices[3];
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
    vertices[corner] = vertex_rows[geometry.first_vertex + uint(vertex_index)];
    float3 position =
        float3(vertices[corner].position_x, vertices[corner].position_y,
               vertices[corner].position_z);
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

  float3 object_normal =
      float3(vertices[0].normal_x, vertices[0].normal_y, vertices[0].normal_z) *
          barycentric.x +
      float3(vertices[1].normal_x, vertices[1].normal_y, vertices[1].normal_z) *
          barycentric.y +
      float3(vertices[2].normal_x, vertices[2].normal_y, vertices[2].normal_z) *
          barycentric.z;
  float4 object_tangent = vertices[0].tangent * barycentric.x +
                          vertices[1].tangent * barycentric.y +
                          vertices[2].tangent * barycentric.z;
  float2 screen[3];
  for (uint corner = 0u; corner < 3u; ++corner) {
    float2 ndc = clip[corner].xy / clip[corner].w;
    screen[corner] = float2((ndc.x * 0.5 + 0.5) * root.extent.x,
                            (0.5 - ndc.y * 0.5) * root.extent.y);
  }
  float signed_area =
      (screen[1].x - screen[0].x) * (screen[2].y - screen[0].y) -
      (screen[1].y - screen[0].y) * (screen[2].x - screen[0].x);
  float3 transformed_normal = (instance.model * float4(object_normal, 0.0)).xyz;
  if (!isfinite(signed_area) || abs(signed_area) <= 1e-8 ||
      !vkr_metal_packet_finite_nonzero(transformed_normal)) {
    atomic_fetch_add_explicit(&root.compaction_state->resolve_invalid_count, 1u,
                              memory_order_relaxed);
    return false;
  }
  float3 normal =
      normalize(transformed_normal) * (signed_area < 0.0 ? 1.0 : -1.0);
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
  surface = {base,
             normal,
             metallic,
             roughness,
             occlusion,
             emissive,
             visible.material_index};
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
  float3 view = normalize(frame->view_position.xyz - world_position);
  float no_v = max(dot(surface.normal, view), 0.0);
  float3 analytic_diffuse = 0.0;
  float3 analytic_specular = 0.0;
  if (frame->directional_direction_enabled.w > 0.5) {
    float shadow = vkr_metal_packet_directional_shadow(frame, world_position);
    VkrMetalPacketDirectResult direct = vkr_metal_packet_direct(
        surface.normal, view,
        normalize(-frame->directional_direction_enabled.xyz),
        frame->directional_color_intensity.rgb *
            frame->directional_color_intensity.w * shadow,
        surface.base.rgb, surface.metallic, surface.roughness, f0);
    analytic_diffuse = direct.diffuse;
    analytic_specular = direct.specular;
  }
  uint4 point_mask = vkr_metal_packet_point_light_mask(frame, world_position);
  uint point_count = min(frame->point_light_count, 128u);
  for (uint word = 0u; word < 4u; ++word) {
    uint remaining = point_mask[word];
    while (remaining != 0u) {
      uint light_index = word * 32u + ctz(remaining);
      remaining &= remaining - 1u;
      if (light_index >= point_count)
        continue;
      float4 p0 = frame->point_light_data[light_index * 4u + 0u];
      float4 p1 = frame->point_light_data[light_index * 4u + 1u];
      float4 p2 = frame->point_light_data[light_index * 4u + 2u];
      float4 p3 = frame->point_light_data[light_index * 4u + 3u];
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

kernel void vkr_metal_packet_transmission_shade(
    constant VkrMetalPacketTransmissionShadeRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.extent) || root.vbuffer.read(pixel).x == 0u)
    return;
  VkrMetalPacketTransmissionSurface surface;
  if (!vkr_metal_packet_resolve_transmission_surface(root, pixel, surface))
    return;
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
  if (frame->render_mode == 0u && material.material_alpha.y > 0.0) {
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

static_assert(sizeof(VkrGpuDrawCompactionState) == 80,
              "GPU draw compaction state ABI must remain 80 bytes");
static_assert(sizeof(VkrMetalPacketGBufferResolveRoot) == 192,
              "G-buffer resolve root ABI must remain 192 bytes");
static_assert(sizeof(VkrMetalPacketDeferredLightingRoot) == 144,
              "Deferred-lighting root ABI must remain 144 bytes");
static_assert(sizeof(VkrMetalPacketTransmissionShadeRoot) == 240,
              "Transmission-shade root ABI must remain 240 bytes");
static_assert(sizeof(VkrMetalPacketGpuDrawRoot) == 288,
              "GPU draw root ABI must remain 288 bytes");
static_assert(sizeof(VkrMetalPacketHzbBuildRoot) == 48,
              "HZB build root ABI must remain 48 bytes");
