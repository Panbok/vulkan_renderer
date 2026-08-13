struct VkrGpuDrawCompactionState {
  uint2 execution_ranges[4];
  atomic_uint bucket_counts[4];
  atomic_uint bucket_cursors[4];
  uint visible_count;
  atomic_uint overflow_count;
  atomic_uint resolve_invalid_count;
  uint reserved;
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

static bool vkr_metal_packet_candidate_visible(
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

kernel void
vkr_metal_packet_gpu_draw_classify(constant VkrMetalPacketGpuDrawRoot &root
                                   [[buffer(0)]],
                                   uint index [[thread_position_in_grid]]) {
  if (index >= root.candidate_count)
    return;
  const device VkrGpuCandidateDrawRow &candidate = root.candidates[index];
  if (candidate.state_bucket >= 4u ||
      !vkr_metal_packet_candidate_visible(root, candidate)) {
    root.classifications[index] = 0u;
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
  float4x4 view_projection;
  uint2 extent;
  uint visible_capacity;
  uint geometry_count;
  uint material_count;
  uint instance_count;
};

static void vkr_metal_packet_resolve_defaults(
    constant VkrMetalPacketGBufferResolveRoot &root, uint2 pixel,
    float debug_marker) {
  root.albedo.write(float4(0.0, 0.0, 0.0, 1.0), pixel);
  root.specular.write(float4(0.0, 0.0, 0.0, 1.0), pixel);
  root.normal.write(float4(0.0), pixel);
  root.emissive.write(float4(0.0), pixel);
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
  root.debug.write(float4(barycentric, selected_lod + 1.0), pixel);
}

static_assert(sizeof(VkrGpuDrawCompactionState) == 80,
              "GPU draw compaction state ABI must remain 80 bytes");
static_assert(sizeof(VkrMetalPacketGBufferResolveRoot) == 192,
              "G-buffer resolve root ABI must remain 192 bytes");
