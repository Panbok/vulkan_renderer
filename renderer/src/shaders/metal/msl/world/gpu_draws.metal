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
  device struct VkrMetalCustomCompaction *custom_compaction;
  // Candidates below it are static; a local face view whose LOD flags
  // select one mobility skips the others.
  uint static_candidate_count;
  // With SDF sun shadows, the views (bit per view) that skip the casters
  // the trace covers, from the frame's field table.
  uint sdf_skip_view_mask;
  const device struct VkrMetalSdfVolumeRow *sdf_volume_rows;
  const device uint2 *sdf_mesh_volumes;
  uint sdf_mesh_count;
  // Commands each ICB group's pool holds: the prefix places the views'
  // ranges one after another within it.
  uint command_capacity;
  // The late occlusion view, or zero while the camera culls in one phase
  // (ADR-032). The late phase's root points `custom_compaction` at the late
  // view's own table.
  uint late_view;
  // The first view a classify dispatch's rows cover: zero, or the late view.
  uint classify_view_base;
  uint reserved[2];
};

// Whether `view_index` draws the camera: the camera view or the late view.
static bool vkr_metal_packet_camera_view(
    constant VkrMetalPacketGpuDrawRoot &root, uint view_index) {
  return view_index == 0u ||
         (root.late_view != 0u && view_index == root.late_view);
}

// SDF sun shadows (vkr_metal_packet_sdf.inc): one submesh's distance volume
// in the frame's field table; mirrors VkrMetalSdfVolumeRow. A mesh's rows
// follow in submesh order; `dims` stay zero until the volume is generated.
// The grid build (world/sun_shadow.metal, later in the library) reads it
// too.
struct VkrMetalSdfVolumeRow {
  // xyz the volume's mesh-local origin, w its voxel size.
  float4 box_min_voxel;
  uint dims[3];
  // The submesh's first index from its mesh's first.
  uint first_index;
  texture3d<float, access::sample> volume;
  // The submesh's longest mesh-local box side.
  float extent;
  uint reserved;
};

static_assert(sizeof(VkrMetalSdfVolumeRow) == 48u,
              "VkrMetalSdfVolumeRow ABI drift");

// Mirror VKR_METAL_SDF_MIN_EXTENT and VKR_METAL_SDF_MAX_EXTENT: the world
// sizes of the casters the field holds.
#define VKR_METAL_SDF_MIN_EXTENT 0.25f
#define VKR_METAL_SDF_MAX_EXTENT 120.0f
#define VKR_METAL_SDF_NO_ROW 0xFFFFFFFFu

// The largest axis scale of `model`.
static float vkr_metal_sdf_largest_scale(float4x4 model) {
  return max(length(model[0].xyz),
             max(length(model[1].xyz), length(model[2].xyz)));
}

// The field table row of `candidate`'s distance volume, or
// VKR_METAL_SDF_NO_ROW unless the trace covers it: an opaque rigid sun
// caster whose volume is generated and whose world size lies in the field's
// range. Cutout, glass and skinned casters keep the cascades.
static uint vkr_metal_sdf_field_row(
    const device VkrMetalSdfVolumeRow *rows, const device uint2 *meshes,
    uint mesh_count, const device VkrGpuCandidateDrawRow &candidate,
    const device VkrGpuGeometryRow &geometry,
    const device VkrMetalPacketInstance &instance) {
  // Candidate flags: 4 casts sun shadows, 8 casts them through glass.
  const uint flags = vkr_gpu_draw_candidate_flags(candidate.state_flags);
  if ((flags & 4u) == 0u || (flags & 8u) != 0u ||
      (vkr_gpu_draw_state_bucket(candidate.state_flags) & 2u) != 0u ||
      instance.deformation_address != 0ul ||
      candidate.geometry_index >= mesh_count)
    return VKR_METAL_SDF_NO_ROW;
  const uint2 range = meshes[candidate.geometry_index];
  const uint first_index = candidate.first_index - geometry.first_index;
  for (uint row = range.x; row < range.x + range.y; ++row) {
    if (rows[row].first_index != first_index)
      continue;
    const float extent =
        rows[row].extent * vkr_metal_sdf_largest_scale(instance.model);
    return rows[row].dims[0] != 0u && extent >= VKR_METAL_SDF_MIN_EXTENT &&
                   extent <= VKR_METAL_SDF_MAX_EXTENT
               ? row
               : VKR_METAL_SDF_NO_ROW;
  }
  return VKR_METAL_SDF_NO_ROW;
}

// The camera view's draw buckets per Custom graph slot (ADR-096), in two
// planes by BRDF variant (ADR-113): plane 0 holds the Custom graphs' draws;
// plane 1 the draws whose material uses a BRDF extension, Standard ones in
// slot 0 and Custom ones in their graph's slot. Plane 0's slot 0 stays empty.
// The Standard draw kinds (ADR-129) follow the Custom slots in both planes.
#define VKR_METAL_CUSTOM_GRAPH_SLOTS 33u
#define VKR_METAL_STANDARD_KIND_COUNT 2u
#define VKR_METAL_COMPACTION_SLOTS                                              \
  (VKR_METAL_CUSTOM_GRAPH_SLOTS + VKR_METAL_STANDARD_KIND_COUNT)
#define VKR_METAL_CUSTOM_COMPACTION_PLANES 2u
#define VKR_METAL_MATERIAL_CUSTOM_SHIFT 24u
#define VKR_METAL_MATERIAL_CUSTOM_MASK 63u
// A camera classification of a Custom draw: this bit, and its slot - 1 in the
// bits the probe takes later.
#define VKR_METAL_CUSTOM_CLASS_BIT 16u
// A camera classification of a draw in plane 1: an extended Standard draw
// alone, an extended Custom draw with the Custom bit and slot.
#define VKR_METAL_BRDF_CLASS_BIT 32u
// A camera classification of a Standard draw of a kind: this bit, and the
// kind in the slot bits, which a Standard draw leaves free.
#define VKR_METAL_KIND_CLASS_BIT 64u
#define VKR_METAL_CUSTOM_CLASS_SLOT_SHIFT 27u
// The Standard draw kinds: draws that cannot be lightmapped on non-terrain
// materials, and on terrain materials. A draw that can be lightmapped keeps
// the general pipelines.
#define VKR_METAL_STANDARD_KIND_UNLIT 0u
#define VKR_METAL_STANDARD_KIND_TERRAIN 1u
#define VKR_METAL_STANDARD_KIND_NONE 2u
// Mirrors the terrain bit of VkrMetalPacketMaterial.flags.
#define VKR_METAL_MATERIAL_TERRAIN 2048u
struct VkrMetalCustomCompaction {
  uint2 execution_ranges[VKR_METAL_CUSTOM_COMPACTION_PLANES]
                        [VKR_METAL_COMPACTION_SLOTS][8];
  atomic_uint bucket_counts[VKR_METAL_CUSTOM_COMPACTION_PLANES]
                           [VKR_METAL_COMPACTION_SLOTS][8];
  atomic_uint bucket_cursors[VKR_METAL_CUSTOM_COMPACTION_PLANES]
                            [VKR_METAL_COMPACTION_SLOTS][8];
  atomic_uint late_count;
  // Slots whose pipelines exist this frame, and those still being created,
  // bit slot - 1.
  uint ready_mask;
  uint pending_mask;
  // Nonzero when the tiled BRDF extension pipelines exist this frame; plane
  // 1's slot 0 then takes the extended Standard draws.
  uint brdf_ready;
  // The Custom slots whose BRDF extension pipelines exist this frame, bit
  // slot - 1; their extended draws take plane 1.
  uint brdf_ready_mask;
  // The Standard kinds whose pipelines exist this frame, bit
  // plane * VKR_METAL_STANDARD_KIND_COUNT + kind.
  uint kind_ready_mask;
  // The frame's material table; row flags carry each material's slot.
  const device VkrMetalPacketMaterial *materials;
};

// The Custom graph slot a camera-view candidate shades with, or 0. A Custom
// material whose graph's pipelines are not ready this frame draws its
// Standard fallback, which counts as a late draw while they are being
// created.
static uint vkr_metal_packet_custom_slot(constant VkrMetalPacketGpuDrawRoot &root,
                                         const device VkrGpuCandidateDrawRow &candidate) {
  if (root.custom_compaction == nullptr)
    return 0u;
  const uint slot =
      (root.custom_compaction->materials[candidate.material_index].flags >>
       VKR_METAL_MATERIAL_CUSTOM_SHIFT) &
      VKR_METAL_MATERIAL_CUSTOM_MASK;
  if (slot == 0u || slot >= VKR_METAL_CUSTOM_GRAPH_SLOTS)
    return 0u;
  if (((root.custom_compaction->ready_mask >> (slot - 1u)) & 1u) == 0u) {
    if (((root.custom_compaction->pending_mask >> (slot - 1u)) & 1u) != 0u)
      atomic_fetch_add_explicit(&root.custom_compaction->late_count, 1u,
                                memory_order_relaxed);
    return 0u;
  }
  return slot;
}

// Whether a camera-view candidate of Custom slot `slot` (0 for Standard)
// takes plane 1: its material uses a BRDF extension and that slot's
// extension pipelines exist this frame (ADR-113).
static bool vkr_metal_packet_brdf_extended(
    constant VkrMetalPacketGpuDrawRoot &root,
    const device VkrGpuCandidateDrawRow &candidate, uint slot) {
  if (root.custom_compaction == nullptr)
    return false;
  const bool ready =
      slot == 0u ? root.custom_compaction->brdf_ready != 0u
                 : ((root.custom_compaction->brdf_ready_mask >> (slot - 1u)) &
                    1u) != 0u;
  if (!ready)
    return false;
  const device VkrMetalPacketMaterial &material =
      root.custom_compaction->materials[candidate.material_index];
  return vkr_brdf_extension_active(vkr_brdf_extension_unpack(
      material.material_brdf, material.material_specular_tint));
}

// The Standard kind of a camera-view candidate of plane `plane` whose
// pipelines exist this frame, or VKR_METAL_STANDARD_KIND_NONE. A draw can be
// lightmapped when its instance has a lightmap slot and its geometry
// lightmap UVs, as the vertex tests; such a draw keeps the general
// pipelines.
static uint vkr_metal_packet_standard_kind(
    constant VkrMetalPacketGpuDrawRoot &root,
    const device VkrGpuCandidateDrawRow &candidate,
    const device VkrMetalPacketInstance &instance,
    const device VkrGpuGeometryRow &geometry, uint plane) {
  if (root.custom_compaction == nullptr ||
      root.custom_compaction->kind_ready_mask == 0u)
    return VKR_METAL_STANDARD_KIND_NONE;
  const bool terrain =
      (root.custom_compaction->materials[candidate.material_index].flags &
       VKR_METAL_MATERIAL_TERRAIN) != 0u;
  const device VkrGpuGeometryDecodeRecord *records =
      (const device VkrGpuGeometryDecodeRecord *)geometry.decode_address;
  const bool lightmapped =
      uint(instance.normal_column2.w) != 0u &&
      (records[candidate.decode_index].flags &
       VKR_GPU_GEOMETRY_DECODE_LIGHTMAP_UV) != 0u;
  const uint kind = lightmapped ? VKR_METAL_STANDARD_KIND_NONE
                    : terrain   ? VKR_METAL_STANDARD_KIND_TERRAIN
                                : VKR_METAL_STANDARD_KIND_UNLIT;
  if (kind == VKR_METAL_STANDARD_KIND_NONE ||
      ((root.custom_compaction->kind_ready_mask >>
        (plane * VKR_METAL_STANDARD_KIND_COUNT + kind)) &
       1u) == 0u)
    return VKR_METAL_STANDARD_KIND_NONE;
  return kind;
}

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
// viewport at the shadow map's far depth 1, drawn with an always-passing depth
// write. Shadow maps keep near at 0; only camera depth is reversed (ADR-109).
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
  // Reversed camera depth (ADR-109): near is 1, far 0, so the nearest corner
  // has the largest depth and the HZB keeps each cell's smallest.
  float2 ndc_min = float2(1e30);
  float2 ndc_max = float2(-1e30);
  float nearest_depth = 0.0;
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
    nearest_depth = max(nearest_depth, ndc.z);
  }
  if (nearest_depth >= 1.0 || any(ndc_min > 1.0) || any(ndc_max < -1.0))
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
      if (nearest_depth >=
          root.hzb.read(uint2(x, y), mip).x - root.hzb_depth_epsilon)
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
  uint view_index = root.classify_view_base + position.y;
  if (index >= root.candidate_count || view_index >= root.view_count)
    return;
  uint classification_index = view_index * root.visible_capacity + index;
  /* The late view classifies only the candidates the camera deferred. They
     passed every camera test but the earlier frame's pyramid, so they pass
     them again here; this frame's pyramid then decides (ADR-032). A
     rejected word names the first test that failed (lod_kernel.slangh). */
  if (root.late_view != 0u && view_index == root.late_view &&
      root.classifications[index] != VKR_GPU_DRAW_DEFERRED) {
    root.classifications[classification_index] =
        vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_FILTERED);
    return;
  }
  const bool camera = vkr_metal_packet_camera_view(root, view_index);
  const constant VkrMetalPacketGpuDrawView &view = root.views[view_index];
  const device VkrGpuCandidateDrawRow &candidate = root.candidates[index];
  uint candidate_flags = vkr_gpu_draw_candidate_flags(candidate.state_flags);
  uint bucket = vkr_gpu_draw_state_bucket(candidate.state_flags);
  /* A static square or cascade layer holds the static casters; a dynamic
     square or composed cascade adds the dynamic ones over a copy of it. */
  uint mobility = root.lod_views[view_index].flags;
  bool static_candidate = index < root.static_candidate_count;
  bool mobility_excluded =
      ((mobility & VKR_GPU_LOD_VIEW_STATIC_CASTERS_ONLY) != 0u &&
       !static_candidate) ||
      ((mobility & VKR_GPU_LOD_VIEW_DYNAMIC_CASTERS_ONLY) != 0u &&
       static_candidate);
  if ((candidate_flags & view.required_candidate_flags) !=
          view.required_candidate_flags ||
      (candidate_flags & view.excluded_candidate_flags) != 0u ||
      mobility_excluded) {
    root.classifications[classification_index] =
        vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_FILTERED);
    return;
  }
  if (!vkr_metal_packet_candidate_in_frustum(root, view, candidate)) {
    root.classifications[classification_index] =
        vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_FRUSTUM);
    return;
  }
  const device VkrMetalPacketInstance &instance =
      root.instances[candidate.instance_index];
  /* With SDF sun shadows the far level leaves the casters the trace covers
     (vkr_metal_packet_sdf.inc). */
  if (view_index < 32u &&
      ((root.sdf_skip_view_mask >> view_index) & 1u) != 0u &&
      vkr_metal_sdf_field_row(root.sdf_volume_rows, root.sdf_mesh_volumes,
                              root.sdf_mesh_count, candidate,
                              root.geometry_rows[candidate.geometry_index],
                              instance) != VKR_METAL_SDF_NO_ROW) {
    root.classifications[classification_index] =
        vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_FILTERED);
    return;
  }
  constant VkrGpuLodView &lod_view = root.lod_views[view_index];
  const float radius =
      candidate.local_bounding_sphere.w * instance.normal_column1.w;
  /* The camera skips candidates in hidden rooms, past their draw distance or
     below their smallest drawn size; a cascade skips sealed casters and
     those whose shadow distance ends before its slice. Other views have no
     slice start and keep them. */
  const uint cull = vkr_gpu_draw_cull_word(candidate.state_flags);
  if (cull != 0u && (candidate_flags & 1u) != 0u &&
      (camera
           ? vkr_gpu_draw_distance_culled(
                 cull, lod_view.position_scale.w, lod_view.flags,
                 lod_view.distance_scale,
                 vkr_lod_distance(
                     lod_view.position_scale.xyz,
                     (instance.model *
                      float4(candidate.local_bounding_sphere.xyz, 1.0)).xyz,
                     radius),
                 radius)
           : vkr_gpu_draw_shadow_culled(cull, lod_view.shadow_near))) {
    root.classifications[classification_index] =
        vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_DISTANCE);
    return;
  }
  device VkrGpuDrawCompactionState &state = root.compaction_state[view_index];
  /* A deformed draw's bounds need not hold its pose, so occlusion never
     rejects it. In two phases an earlier frame's pyramid only defers a
     camera candidate; the late view's count holds the final rejections. */
  if (view.hzb_enabled != 0u && instance.deformation_address == 0ul &&
      vkr_metal_packet_candidate_occluded(root, candidate)) {
    root.classifications[classification_index] =
        view_index == 0u && root.late_view != 0u
            ? VKR_GPU_DRAW_DEFERRED
            : vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_OCCLUDED);
    atomic_fetch_add_explicit(&state.occlusion_culled_count, 1u,
                              memory_order_relaxed);
    return;
  }
  /* Sub-texel casters, and copies below their larger limit, skip the
     directional cascades. */
  if (!camera && (candidate_flags & 1u) != 0u &&
      vkr_gpu_cascade_caster_too_small(cull, lod_view.position_scale.w,
                                       lod_view.flags, radius)) {
    root.classifications[classification_index] =
        vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_CASTER_SMALL);
    return;
  }
  const device VkrGpuGeometryRow &geometry =
      root.geometry_rows[candidate.geometry_index];
  /* The LOD state rides above the bucket (ADR-084). A candidate without
     valid bounds has no distance and keeps level 0. */
  uint lod_state = (candidate_flags & 1u) == 0u ? 0u : vkr_gpu_lod_select(
      vkr_gpu_geometry_lod_row(geometry.decode_address, candidate.decode_index),
      lod_view,
      (instance.model * float4(candidate.local_bounding_sphere.xyz, 1.0)).xyz,
      radius, instance.normal_column1.w);
  /* The camera sorts Custom draws into their graph's buckets, and draws
     whose material uses a BRDF extension into plane 1, which the tiled
     pipeline shades with the extension variant. */
  const uint slot =
      camera ? vkr_metal_packet_custom_slot(root, candidate) : 0u;
  const uint plane =
      camera && vkr_metal_packet_brdf_extended(root, candidate, slot) ? 1u
                                                                     : 0u;
  /* A Standard camera draw of a kind whose pipelines exist takes its
     kind's range in its plane (ADR-129). */
  const uint kind = camera && slot == 0u
                        ? vkr_metal_packet_standard_kind(root, candidate,
                                                         instance, geometry,
                                                         plane)
                        : VKR_METAL_STANDARD_KIND_NONE;
  if (kind != VKR_METAL_STANDARD_KIND_NONE) {
    root.classifications[classification_index] =
        (bucket + 1u) | lod_state |
        (plane != 0u ? VKR_METAL_BRDF_CLASS_BIT : 0u) |
        VKR_METAL_KIND_CLASS_BIT | (kind << VKR_METAL_CUSTOM_CLASS_SLOT_SHIFT);
    atomic_fetch_add_explicit(
        &root.custom_compaction
             ->bucket_counts[plane][VKR_METAL_CUSTOM_GRAPH_SLOTS + kind]
                            [bucket],
        1u, memory_order_relaxed);
    return;
  }
  if (plane != 0u || (slot != 0u && slot < VKR_METAL_CUSTOM_GRAPH_SLOTS)) {
    root.classifications[classification_index] =
        (bucket + 1u) | lod_state |
        (plane != 0u ? VKR_METAL_BRDF_CLASS_BIT : 0u) |
        (slot != 0u ? VKR_METAL_CUSTOM_CLASS_BIT |
                          ((slot - 1u) << VKR_METAL_CUSTOM_CLASS_SLOT_SHIFT)
                    : 0u);
    atomic_fetch_add_explicit(
        &root.custom_compaction->bucket_counts[plane][slot][bucket], 1u,
        memory_order_relaxed);
    return;
  }
  root.classifications[classification_index] = (bucket + 1u) | lod_state;
  atomic_fetch_add_explicit(&state.bucket_counts[bucket], 1u,
                            memory_order_relaxed);
}

/* Whether compaction plane `plane`'s slot `slot` draws this frame: the
   extended Standard slot, a Custom slot or a Standard kind whose pipelines
   exist. Mirrors vkr_metal_packet_custom_range_ready on the host, which
   executes only these ranges; classification fills no other. */
static bool vkr_metal_packet_compaction_range_ready(
    device const VkrMetalCustomCompaction &custom, uint plane, uint slot) {
  if (slot == 0u)
    return plane != 0u && custom.brdf_ready != 0u;
  if (slot >= VKR_METAL_CUSTOM_GRAPH_SLOTS)
    return ((custom.kind_ready_mask >>
             (plane * VKR_METAL_STANDARD_KIND_COUNT + slot -
              VKR_METAL_CUSTOM_GRAPH_SLOTS)) &
            1u) != 0u;
  const uint mask = plane == 0u ? custom.ready_mask : custom.brdf_ready_mask;
  return ((mask >> (slot - 1u)) & 1u) != 0u;
}

/* One view's buckets from `pool_base` in its ICB group's pool: each takes
   its whole count while the view's rows and the pool hold it, and the rest
   counts as overflow. Returns the commands the view placed. */
static uint vkr_metal_packet_gpu_draw_place_view(
    constant VkrMetalPacketGpuDrawRoot &root, uint view_index, uint pool_base,
    uint pool_left) {
  device VkrGpuDrawCompactionState &state = root.compaction_state[view_index];
  /* A retained cascade's pass does not run this frame: it takes no
     commands, and its counts are no overflow. */
  const bool idle = root.views[view_index].encode_idle != 0u;
  uint capacity = idle ? 0u : min(root.visible_capacity, pool_left);
  uint visible_count = 0u;
  uint overflow_count = 0u;
  for (uint bucket = 0u; bucket < 8u; ++bucket) {
    uint bucket_count = atomic_load_explicit(&state.bucket_counts[bucket],
                                             memory_order_relaxed);
    uint written_count = min(bucket_count, capacity - visible_count);
    state.execution_ranges[bucket] =
        uint2(pool_base + visible_count, written_count);
    visible_count += written_count;
    overflow_count += idle ? 0u : bucket_count - written_count;
    atomic_store_explicit(&state.bucket_cursors[bucket], 0u,
                          memory_order_relaxed);
  }
  /* The camera's Custom buckets follow its Standard ones. */
  if (vkr_metal_packet_camera_view(root, view_index) &&
      root.custom_compaction != nullptr) {
    device VkrMetalCustomCompaction &custom = *root.custom_compaction;
    for (uint plane = 0u; plane < VKR_METAL_CUSTOM_COMPACTION_PLANES;
         ++plane) {
      for (uint slot = 0u; slot < VKR_METAL_COMPACTION_SLOTS; ++slot) {
        /* A range no pass draws stays empty, as the table starts zeroed. */
        if (!vkr_metal_packet_compaction_range_ready(custom, plane, slot))
          continue;
        for (uint bucket = 0u; bucket < 8u; ++bucket) {
          uint bucket_count = atomic_load_explicit(
              &custom.bucket_counts[plane][slot][bucket],
              memory_order_relaxed);
          uint written_count = min(bucket_count, capacity - visible_count);
          custom.execution_ranges[plane][slot][bucket] =
              uint2(pool_base + visible_count, written_count);
          visible_count += written_count;
          overflow_count += bucket_count - written_count;
          atomic_store_explicit(&custom.bucket_cursors[plane][slot][bucket],
                                0u, memory_order_relaxed);
        }
      }
    }
  }
  state.visible_count = visible_count;
  atomic_store_explicit(&state.overflow_count, overflow_count,
                        memory_order_relaxed);
  return visible_count;
}

/* One thread per ICB group places its views' ranges one after another in
   the group's pool, the camera first, so the pool holds the frame's visible
   draws rather than every candidate per view. Each view's first range
   starts its commands; visible rows keep each view's own stride, shared by
   classification, raster, resolve and picking. A view the pool cannot hold
   whole reports the rest as overflow, which the host repairs (ADR-024). */
kernel void
vkr_metal_packet_gpu_draw_prefix(constant VkrMetalPacketGpuDrawRoot &root
                                 [[buffer(0)]],
                                 uint group [[thread_position_in_grid]]) {
  const uint first_view = group * root.icb_view_group_size;
  if (first_view >= root.view_count)
    return;
  const uint end_view =
      min(first_view + root.icb_view_group_size, root.view_count);
  uint pool_used = 0u;
  for (uint view_index = first_view; view_index < end_view; ++view_index)
    pool_used += vkr_metal_packet_gpu_draw_place_view(
        root, view_index, pool_used, root.command_capacity - pool_used);
}

/* The late phase's prefix: one thread places the late view's ranges in its
   ICB group's pool after the commands the early prefix placed there, which
   the early opaque pass has drawn (ADR-032). The late view comes last, so
   the views before it in its group are exactly the early ones. */
kernel void vkr_metal_packet_gpu_draw_prefix_late(
    constant VkrMetalPacketGpuDrawRoot &root [[buffer(0)]],
    uint position [[thread_position_in_grid]]) {
  const uint late_view = root.late_view;
  if (position != 0u || late_view == 0u || late_view >= root.view_count)
    return;
  const uint first_view = late_view - late_view % root.icb_view_group_size;
  uint pool_used = 0u;
  for (uint view_index = first_view; view_index < late_view; ++view_index)
    pool_used += root.compaction_state[view_index].visible_count;
  vkr_metal_packet_gpu_draw_place_view(root, late_view, pool_used,
                                       root.command_capacity - pool_used);
}

// Where a probe's influence reaches one half against a world sphere: 0 nowhere
// in it, 1 everywhere in it, 2 in part of it. Influence reaches one half
// inside the box shrunk by half the blend band at the side and top faces
// (the bottom face takes no band; vkr_reflection_probe_influence), so the
// per-pixel first-probe rule changes inside a sphere only where that box's
// surface crosses it.
static uint vkr_metal_packet_probe_sphere_reach(
    const device VkrMetalPacketIblProbe &probe, float3 center, float radius) {
  if (!(probe.extents_weight.w > 0.0f))
    return 0u;
  const float3 extents = max(probe.extents_weight.xyz, 0.0f);
  const float half_band = 0.5f * max(probe.center_blend.w, 0.0f);
  const float3 low = probe.center_blend.xyz - extents +
                     float3(half_band, 0.0f, half_band);
  const float3 high = probe.center_blend.xyz + extents - half_band;
  const float3 outside = max(max(low - center, center - high), 0.0f);
  if (any(low > high) || dot(outside, outside) > radius * radius)
    return 0u;
  return all(center - radius >= low) && all(center + radius <= high) ? 1u
                                                                      : 2u;
}

// The reflection probe a camera-view draw shades with on the tiled pipeline
// (ADR-087), as frame probe index + 1: the probe bound to the draw's object
// (a planar reflector's baked plane probe), else the first unbound probe, in
// the host's smaller-box-first order, whose influence reaches one half over
// the whole bounding sphere. A draw that an earlier unbound probe's half-
// influence surface cuts takes VKR_GPU_DRAW_PROBE_PER_PIXEL, and its pixels
// choose by the same rule (vkr_metal_tiled_pixel_probe). A mover's draw
// tests its lighting origin as a point instead (ADR-120), so it never
// chooses per pixel. A bound probe serves its owner only. Zero takes the
// global environment.
static uint vkr_metal_packet_draw_probe(
    constant VkrMetalPacketFrameRoot *frame,
    const device VkrMetalPacketInstance &instance, float4 local_sphere) {
  const uint count = min(frame->ibl_probe_count, VKR_GPU_DRAW_PROBE_MASK - 1u);
  if (count == 0u || frame->ibl_probes == nullptr)
    return 0u;
  for (uint i = 0u; i < count; ++i) {
    if (frame->ibl_probes[i].owner_object_id != 0u &&
        frame->ibl_probes[i].owner_object_id == instance.object_id)
      return i + 1u;
  }
  const bool lit_at_origin = instance.lighting_origin.w != 0.0f;
  const float3 center =
      lit_at_origin
          ? instance.lighting_origin.xyz
          : (instance.model * float4(local_sphere.xyz, 1.0f)).xyz;
  const float radius =
      lit_at_origin ? 0.0f
                    : max(local_sphere.w * instance.normal_column1.w, 0.0f);
  for (uint i = 0u; i < count; ++i) {
    if (frame->ibl_probes[i].owner_object_id != 0u)
      continue;
    const uint reach =
        vkr_metal_packet_probe_sphere_reach(frame->ibl_probes[i], center, radius);
    if (reach == 1u)
      return i + 1u;
    if (reach == 2u)
      return VKR_GPU_DRAW_PROBE_PER_PIXEL;
  }
  return 0u;
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
  /* Rejected and deferred words have all four bucket bits set. */
  if (!vkr_gpu_draw_word_drawn(classification))
    return;
  device VkrGpuDrawCompactionState &state = root.compaction_state[view_index];
  uint bucket = (classification & VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK) - 1u;
  /* The prefix starts the view's commands at its first range. */
  uint command_base = state.execution_ranges[0].x;
  uint visible_index = 0u;
  if ((classification & (VKR_METAL_CUSTOM_CLASS_BIT | VKR_METAL_BRDF_CLASS_BIT |
                         VKR_METAL_KIND_CLASS_BIT)) != 0u) {
    /* A Custom, extended or kind draw fills its plane and slot's bucket
       after the Standard ones. */
    uint plane = (classification & VKR_METAL_BRDF_CLASS_BIT) != 0u ? 1u : 0u;
    uint slot =
        (classification & VKR_METAL_CUSTOM_CLASS_BIT) != 0u
            ? (classification >> VKR_METAL_CUSTOM_CLASS_SLOT_SHIFT) + 1u
        : (classification & VKR_METAL_KIND_CLASS_BIT) != 0u
            ? VKR_METAL_CUSTOM_GRAPH_SLOTS +
                  (classification >> VKR_METAL_CUSTOM_CLASS_SLOT_SHIFT)
            : 0u;
    device VkrMetalCustomCompaction &custom = *root.custom_compaction;
    uint local_index = atomic_fetch_add_explicit(
        &custom.bucket_cursors[plane][slot][bucket], 1u, memory_order_relaxed);
    if (local_index >= custom.execution_ranges[plane][slot][bucket].y)
      return;
    visible_index = custom.execution_ranges[plane][slot][bucket].x -
                    command_base + local_index;
  } else {
    uint local_index = atomic_fetch_add_explicit(&state.bucket_cursors[bucket],
                                                 1u, memory_order_relaxed);
    if (local_index >= state.execution_ranges[bucket].y)
      return;
    visible_index =
        state.execution_ranges[bucket].x - command_base + local_index;
  }
  /* The LOD state, without the bucket and the Custom slot. */
  uint lod_state = classification & ~(VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK |
                                      VKR_METAL_CUSTOM_CLASS_BIT |
                                      VKR_METAL_BRDF_CLASS_BIT |
                                      VKR_METAL_KIND_CLASS_BIT |
                                      (31u << VKR_METAL_CUSTOM_CLASS_SLOT_SHIFT));

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
  constant VkrMetalPacketDrawRoot &draw_root = root.draw_roots[view_index];
  uint probe_state = 0u;
  if (vkr_metal_packet_camera_view(root, view_index) &&
      draw_root.frame->render_mode == 19u) {
    /* The Draw reach view lights nothing, so the probe bits carry the
       draw's reach code to its inspection instead (ADR-114). */
    const device VkrMetalPacketInstance &instance =
        root.instances[candidate.instance_index];
    constant VkrGpuLodView &lod_view = root.lod_views[0];
    const float radius =
        candidate.local_bounding_sphere.w * instance.normal_column1.w;
    probe_state =
        vkr_gpu_draw_reach_code(
            vkr_gpu_draw_cull_word(candidate.state_flags),
            lod_view.position_scale.w, lod_view.flags, lod_view.distance_scale,
            vkr_lod_distance(
                lod_view.position_scale.xyz,
                (instance.model *
                 float4(candidate.local_bounding_sphere.xyz, 1.0)).xyz,
                radius),
            radius)
        << VKR_GPU_DRAW_PROBE_SHIFT;
  } else if ((draw_root.flags & VKR_METAL_PACKET_DRAW_ROOT_PROBES) != 0u)
    probe_state = vkr_metal_packet_draw_probe(
                      draw_root.frame, root.instances[candidate.instance_index],
                      candidate.local_bounding_sphere)
                  << VKR_GPU_DRAW_PROBE_SHIFT;
  root.visible_rows[view_base + visible_index] = {
      candidate.geometry_index,
      candidate.material_index,
      candidate.instance_index,
      first_index,
      index_count,
      candidate.vertex_offset,
      candidate.decode_index,
      (candidate.state_flags & VKR_GPU_DRAW_CANDIDATE_STATE_MASK) | lod_state |
          probe_state};
  device uint *indices = reinterpret_cast<device uint *>(
      geometry.index_address + ulong(first_index) * sizeof(uint));

  render_command command(icb->command_buffer, command_base + visible_index);
  command.set_vertex_buffer(&root.draw_roots[view_index], 0u);
  command.set_fragment_buffer(&root.draw_roots[view_index], 1u);
  command.draw_indexed_primitives(primitive_type::triangle, index_count,
                                  indices, 1u, candidate.vertex_offset,
                                  visible_index);
}

/* The draw census (VkrGpuDebugPayload.draw_census): mirrors
   VkrMetalPacketGpuDrawCensusRoot in metal/vkr_metal_packet_abi.h. */
struct VkrMetalPacketGpuDrawCensusRoot {
  const device uint *classifications;
  const device VkrGpuCandidateDrawRow *candidates;
  const device VkrMetalPacketInstance *instances;
  device uint *states;
  device uint *object_ids;
  uint candidate_count;
  // The late view's first classification row, or zero while the camera
  // culls in one phase (ADR-032).
  uint late_base;
  uint reserved[4];
};

/* One thread per eight candidates: each candidate's camera state from the
   early camera word and, for a deferred one, the late view's word, packed
   eight to a word, and its object id. A tail word's unused lanes stay
   zero. */
kernel void vkr_metal_packet_gpu_draw_census(
    constant VkrMetalPacketGpuDrawCensusRoot &root [[buffer(0)]],
    uint word_index [[thread_position_in_grid]]) {
  const uint first = word_index * VKR_GPU_DRAW_CENSUS_STATES_PER_WORD;
  if (first >= root.candidate_count)
    return;
  const uint end =
      min(first + VKR_GPU_DRAW_CENSUS_STATES_PER_WORD, root.candidate_count);
  uint word = 0u;
  for (uint index = first; index < end; ++index) {
    const uint early = root.classifications[index];
    const uint late = root.late_base != 0u && early == VKR_GPU_DRAW_DEFERRED
                          ? root.classifications[root.late_base + index]
                          : early;
    word = vkr_gpu_draw_census_put(word, index - first,
                                   vkr_gpu_draw_camera_state(early, late));
    root.object_ids[index] =
        root.instances[root.candidates[index].instance_index].object_id;
  }
  root.states[word_index] = word;
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

// The world-space view direction through a pixel's center. Depth 1 is the
// near plane under reversed camera depth. `view_rays` leaves out the camera
// translation (vkr_atmosphere_view_rays), so the direction keeps its
// precision however far the eye is from the origin; an orthographic camera's
// matrix returns its forward axis for every pixel.
static float3 vkr_metal_packet_sky_direction(float4x4 view_rays, uint2 extent,
                                             float2 pixel_center) {
  float2 ndc = vkr_metal_packet_resolve_ndc(pixel_center, extent);
  return normalize((view_rays * float4(ndc, 1.0, 1.0)).xyz);
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
    texture2d<float, access::sample> sky_view, float4x4 view_rays,
    uint2 extent, uint sky_mode, float4 sky_radiance, uint2 pixel,
    thread float3 &disc) {
  disc = float3(0.0);
  if (sky_mode == VKR_SKY_MODE_NONE)
    return float3(0.02, 0.02, 0.03) * frame->pre_exposure;
  if (sky_mode == VKR_SKY_MODE_CONSTANT)
    return sky_radiance.rgb;
  float3 direction =
      vkr_metal_packet_sky_direction(view_rays, extent, float2(pixel) + 0.5);
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
          view_rays, extent, float2(pixel) + float2(1.5, 0.5));
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
    texture2d<float, access::sample> sky_view, float4x4 view_rays,
    uint2 extent, uint sky_mode, float4 sky_radiance, uint2 pixel) {
  float3 disc;
  float3 radiance =
      vkr_metal_packet_sky_clear(frame, sky_view, view_rays, extent, sky_mode,
                                 sky_radiance, pixel, disc);
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
  // Each cell keeps its farthest depth, the smallest under reversed camera
  // depth (ADR-109).
  float farthest_depth = 1.0;
  if (root.source_is_depth != 0u) {
    farthest_depth = root.source.read(min(pixel, root.source_extent - 1u)).x;
  } else {
    uint2 source_begin = pixel * 2u;
    uint2 source_end = min(source_begin + 2u, root.source_extent);
    // Metal mip extents floor odd dimensions. The last destination texel owns
    // the unpaired source edge so every depth sample contributes to the min.
    if (pixel.x + 1u == root.destination_extent.x)
      source_end.x = root.source_extent.x;
    if (pixel.y + 1u == root.destination_extent.y)
      source_end.y = root.source_extent.y;
    for (uint y = source_begin.y; y < source_end.y; ++y) {
      for (uint x = source_begin.x; x < source_end.x; ++x) {
        farthest_depth = min(farthest_depth, root.source.read(uint2(x, y)).x);
      }
    }
  }
  root.destination.write(float4(farthest_depth), pixel);
}

static_assert(sizeof(VkrMetalCustomCompaction) ==
                  VKR_METAL_CUSTOM_COMPACTION_PLANES *
                          VKR_METAL_COMPACTION_SLOTS * 8u * 16u +
                      32u,
              "Custom compaction ABI drift");
static_assert(sizeof(VkrGpuDrawCompactionState) == 144,
              "GPU draw compaction state ABI must remain 144 bytes");
static_assert(sizeof(VkrMetalPacketGpuDrawRoot) == 240,
              "GPU draw root ABI must remain 240 bytes");
static_assert(sizeof(VkrMetalPacketGpuDrawView) == 112,
              "GPU draw view ABI must remain 112 bytes");
static_assert(sizeof(VkrMetalPacketGpuDrawCensusRoot) == 64u,
              "GPU draw census root ABI must remain 64 bytes");
static_assert(sizeof(VkrMetalPacketHzbBuildRoot) == 48,
              "HZB build root ABI must remain 48 bytes");
