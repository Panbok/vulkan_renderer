// =============================================================================
// SDF sun shadows on the tiled pipeline (shadow quality proposal, T5)
// =============================================================================
//
// The Distance field shadows setting (owner decisions 2026-10-16 and
// 2026-10-17). Each opaque submesh a sun caster draws gets a distance volume
// once the generation scan reaches one of its candidates: the GPU splats the
// submesh's resident triangles into an unsigned narrow-band distance (Bakery
// is to own the volumes). The field table lists every live mesh's volumes,
// and each frame copies it for the GPU. There the casters whose volumes are
// ready, the field casters (vkr_metal_sdf_field_row), list themselves in a
// light-space grid over every receiver the shadows reach: a pixel's ray
// toward the sun is one point of the plane across the sun's direction, so
// the trace reads one cell's casters. `Tiled.SunShadow` traces at half
// resolution past the near cascades, and the atmosphere draw takes the
// darker of the far level and the trace through the opaque pass's
// unshadowed sun light.
//
// The far level leaves the field casters out (sdf_skip_view_mask), so the
// trace replaces their far shadows while terrain, foliage, cutout and
// skinned casters keep the far level's. The far level is rebuilt over many
// frames and kept long after, so its content may lack a caster only while
// the trace covers it: a field caster stays one while its mesh lives, and
// the grid spans every receiver the shadows reach. A rebuild drawn before a
// volume was ready still holds that caster, which only doubles its shadow.

/* Where the SDF trace has taken over from the cascades, along the camera
   axis: where the cascade before the far level starts to fade into it (the
   receiver's cross-fade), else where the cascades fade out; at once for the
   replace-all diagnostic. */
vkr_internal float32_t
vkr_metal_packet_sun_shadow_start(const VkrShadowPassPayload *shadow) {
  if (shadow->sdf_mode == VKR_SHADOW_SDF_MODE_REPLACE_ALL)
    return 0.0f;
  const uint32_t far = shadow->far_cascade;
  if (far >= shadow->cascade_count)
    return shadow->receiver.fade_end;
  if (far == 0u)
    return shadow->cascades[0].split_near_far_texel_depth.x;
  const Vec4 previous = shadow->cascades[far - 1u].split_near_far_texel_depth;
  return previous.y -
         (previous.y - previous.x) * shadow->receiver.cascade_blend_fraction;
}

/* The largest (`largest`) or smallest axis scale of `model`. */
vkr_internal float32_t vkr_metal_packet_sdf_scale(Mat4 model, bool8_t largest) {
  float32_t scale = largest ? 0.0f : VKR_FLOAT_MAX;
  for (uint32_t column = 0u; column < 3u; ++column) {
    const float32_t length = vec3_length(vec3_new(
        model.elements[column * 4 + 0], model.elements[column * 4 + 1],
        model.elements[column * 4 + 2]));
    scale = largest ? Max(scale, length) : Min(scale, length);
  }
  return scale;
}

/* `mesh`'s field table row of submesh `submesh`: its volume's placement
   once generated, else zero dims. */
vkr_internal VkrMetalSdfVolumeRow
vkr_metal_packet_sdf_row(const VkrMetalPacketMesh *mesh, uint32_t submesh) {
  const VkrMetalSdfVolume *volume = &mesh->sdf_volumes[submesh];
  VkrMetalSdfVolumeRow row = {
      .first_index = mesh->submeshes[submesh].first_index,
      .extent = Max(volume->box_size.x,
                    Max(volume->box_size.y, volume->box_size.z)),
  };
  if (volume->state != VKR_METAL_SDF_VOLUME_READY)
    return row;
  row.box_min_voxel = vec4_new(volume->box_min.x, volume->box_min.y,
                               volume->box_min.z, volume->voxel);
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    row.dims[axis] = volume->dims[axis];
  row.volume_texture_id = volume->texture.resource_id;
  return row;
}

/* Grows the field table to `rows` rows; its mesh ranges cover every mesh
   slot. The rows are written again after a growth. */
vkr_internal bool8_t vkr_metal_packet_sdf_reserve(
    VkrMetalPacketRenderer *renderer, uint32_t rows) {
  if (!renderer->sdf_mesh_volumes) {
    renderer->sdf_mesh_volumes = vkr_allocator_alloc(
        renderer->allocator,
        (uint64_t)renderer->max_meshes * 2u * sizeof(uint32_t),
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!renderer->sdf_mesh_volumes)
      return false_v;
  }
  if (rows <= renderer->sdf_row_capacity)
    return true_v;
  const uint32_t capacity = Max(rows, 2u * renderer->sdf_row_capacity);
  VkrMetalSdfVolumeRow *grown = vkr_allocator_alloc(
      renderer->allocator, (uint64_t)capacity * sizeof(VkrMetalSdfVolumeRow),
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!grown)
    return false_v;
  if (renderer->sdf_rows)
    vkr_allocator_free(renderer->allocator, renderer->sdf_rows,
                       (uint64_t)renderer->sdf_row_capacity *
                           sizeof(VkrMetalSdfVolumeRow),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->sdf_rows = grown;
  renderer->sdf_row_capacity = capacity;
  return true_v;
}

/* Lays the field table out again once meshes came or went: each live mesh's
   rows in submesh order, its slot's range, and each volume's row. */
vkr_internal bool8_t
vkr_metal_packet_sdf_layout(VkrMetalPacketRenderer *renderer) {
  if (!renderer->sdf_layout_dirty && renderer->sdf_mesh_volumes)
    return true_v;
  uint32_t row_count = 0u;
  uint32_t mesh_count = 0u;
  for (uint32_t i = 0u; i < renderer->max_meshes; ++i) {
    const VkrMetalPacketMesh *mesh = &renderer->meshes[i];
    if (mesh->live && mesh->sdf_volumes) {
      row_count += mesh->submesh_count;
      mesh_count = i + 1u;
    }
  }
  if (!vkr_metal_packet_sdf_reserve(renderer, Max(row_count, 1u)))
    return false_v;

  uint32_t row = 0u;
  for (uint32_t i = 0u; i < mesh_count; ++i) {
    VkrMetalPacketMesh *mesh = &renderer->meshes[i];
    uint32_t *range = &renderer->sdf_mesh_volumes[i * 2u];
    range[0] = row;
    range[1] = 0u;
    if (!mesh->live || !mesh->sdf_volumes)
      continue;
    for (uint32_t submesh = 0u; submesh < mesh->submesh_count; ++submesh) {
      mesh->sdf_volumes[submesh].row = row;
      renderer->sdf_rows[row++] = vkr_metal_packet_sdf_row(mesh, submesh);
    }
    range[1] = mesh->submesh_count;
  }
  renderer->sdf_row_count = row;
  renderer->sdf_mesh_count = mesh_count;
  renderer->sdf_layout_dirty = false_v;
  return true_v;
}

/* The draw-root cells the frame's SDF records take: the field table's copy,
   the generations, the grid build's root and the trace's. */
vkr_internal uint64_t
vkr_metal_packet_sdf_root_cells(const VkrMetalPacketRenderer *renderer) {
  const uint64_t table_bytes =
      (uint64_t)Max(renderer->sdf_row_count, 1u) *
          sizeof(VkrMetalSdfVolumeRow) +
      (uint64_t)Max(renderer->sdf_mesh_count, 1u) * 2u * sizeof(uint32_t);
  return table_bytes / VKR_METAL_PACKET_DRAW_ROOT_STRIDE + 2u +
         VKR_METAL_SDF_GENERATIONS_PER_FRAME + 2u;
}

/* Copies the field table for this frame's culling and grid kernels. */
vkr_internal bool8_t
vkr_metal_packet_write_upload_sdf_table(VkrMetalPacketRenderer *renderer,
                                        VkrMetalPacketFrameUpload *upload) {
  if (!vkr_metal_packet_sdf_layout(renderer))
    return false_v;
  VkrMetalSdfVolumeRow *rows = vkr_metal_packet_next_root_span(
      upload,
      (uint64_t)Max(renderer->sdf_row_count, 1u) * sizeof(VkrMetalSdfVolumeRow),
      &upload->sdf_volume_rows_gpu);
  uint32_t *meshes = vkr_metal_packet_next_root_span(
      upload,
      (uint64_t)Max(renderer->sdf_mesh_count, 1u) * 2u * sizeof(uint32_t),
      &upload->sdf_mesh_volumes_gpu);
  if (!rows || !meshes)
    return false_v;
  MemCopy(rows, renderer->sdf_rows,
          (uint64_t)renderer->sdf_row_count * sizeof(VkrMetalSdfVolumeRow));
  MemCopy(meshes, renderer->sdf_mesh_volumes,
          (uint64_t)renderer->sdf_mesh_count * 2u * sizeof(uint32_t));
  upload->sdf_mesh_count = renderer->sdf_mesh_count;
  return true_v;
}

/* The culling views that leave the field casters out: the far level's,
   and its dynamic casters' view while one draws over its static layer. */
vkr_internal uint32_t
vkr_metal_packet_sdf_skip_view_mask(const VkrRenderGraphFrameInfo *frame) {
  const uint32_t far = frame->shadow_far_cascade;
  if (!frame->tiled_sun_sdf || !frame->shadow_far_active ||
      far >= frame->shadow_cascade_count)
    return 0u;
  uint32_t mask = 0u;
  if (1u + far < 32u)
    mask |= 1u << (1u + far);
  if (far < frame->shadow_cascade_dynamic_view_count) {
    const uint32_t view = vkr_render_graph_cascade_dynamic_view(frame, far);
    if (view < 32u)
      mask |= 1u << view;
  }
  return mask;
}

/* Fits `volume` to its submesh and records its generation into the frame's
   next generation slot, `*scratch_voxels` voxels into the command slot's
   scratch: clear the scratch, splat the submesh's triangles, convert to the
   volume. Its longest side takes VKR_METAL_SDF_MAX_DIM voxels with the
   border, and its voxels are no smaller than VKR_METAL_SDF_MIN_WORLD_VOXEL
   under `world_scale`, the generating instance's largest axis scale. A
   volume the scratch has no room for this frame stays empty and sets
   `*out_full`; one whose texture or scratch cannot be created is skipped for
   good. */
vkr_internal bool8_t vkr_metal_packet_sdf_generate(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketMesh *mesh,
    uint32_t submesh_index, float32_t world_scale,
    VkrMetalPacketFrameUpload *upload, VkrMetalPacketPreparedPass *prepared,
    uint32_t *scratch_voxels, bool8_t *out_full) API_AVAILABLE(macos(26.0)) {
  *out_full = false_v;
  VkrMetalSdfVolume *volume = &mesh->sdf_volumes[submesh_index];
  const float32_t extent = Max(volume->box_size.x,
                               Max(volume->box_size.y, volume->box_size.z));
  const float32_t voxel =
      Max(Max(extent, 1e-6f) / (float32_t)(VKR_METAL_SDF_MAX_DIM -
                                           2u * VKR_METAL_SDF_PAD_VOXELS),
          VKR_METAL_SDF_MIN_WORLD_VOXEL / Max(world_scale, 1e-6f));
  const float32_t box_size[3] = {volume->box_size.x, volume->box_size.y,
                                 volume->box_size.z};
  uint32_t dims[3];
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const uint32_t inner = (uint32_t)vkr_ceil_f32(box_size[axis] / voxel);
    dims[axis] = Min(Max(inner, 1u) + 2u * VKR_METAL_SDF_PAD_VOXELS,
                     VKR_METAL_SDF_MAX_DIM);
  }
  const uint32_t voxels = dims[0] * dims[1] * dims[2];
  if (voxels > VKR_METAL_SDF_SCRATCH_VOXELS - *scratch_voxels) {
    *out_full = true_v;
    return false_v;
  }

  VkrMetalPacketCommandSlot *slot = renderer->active_command_slot;
  VkrMetalBufferResource *scratch = &slot->sdf_scratch;
  if (!scratch->metal_buffer &&
      vkr_metal_memory_device_create_buffer(
          renderer->memory,
          (uint64_t)VKR_METAL_SDF_SCRATCH_VOXELS * sizeof(uint32_t),
          VKR_GPU_ALLOCATION_OWNER_SHADOW_FIELD,
          scratch) != VKR_METAL_MEMORY_STATUS_OK) {
    volume->state = VKR_METAL_SDF_VOLUME_SKIPPED;
    return false_v;
  }
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
  descriptor.textureType = MTLTextureType3D;
  descriptor.pixelFormat = MTLPixelFormatR8Unorm;
  descriptor.width = dims[0];
  descriptor.height = dims[1];
  descriptor.depth = dims[2];
  descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  const VkrMetalMemoryStatus status = vkr_metal_memory_device_create_texture(
      renderer->memory, descriptor, VKR_GPU_ALLOCATION_OWNER_SHADOW_FIELD,
      &volume->texture);
  [descriptor release];
  if (status != VKR_METAL_MEMORY_STATUS_OK) {
    volume->texture = (VkrMetalTextureResource){0};
    volume->state = VKR_METAL_SDF_VOLUME_SKIPPED;
    return false_v;
  }
  uint64_t root_gpu = 0u;
  VkrMetalSdfGenerateRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root) {
    (void)vkr_metal_memory_device_retire(renderer->memory,
                                         volume->texture.handle,
                                         renderer->submit_value);
    volume->texture = (VkrMetalTextureResource){0};
    volume->state = VKR_METAL_SDF_VOLUME_SKIPPED;
    return false_v;
  }

  for (uint32_t axis = 0u; axis < 3u; ++axis)
    volume->dims[axis] = dims[axis];
  volume->voxel = voxel;
  volume->box_min =
      vec3_sub(volume->box_min,
               vec3_new(VKR_METAL_SDF_PAD_VOXELS * voxel,
                        VKR_METAL_SDF_PAD_VOXELS * voxel,
                        VKR_METAL_SDF_PAD_VOXELS * voxel));
  renderer->sdf_volume_bytes += voxels;
  const VkrMetalPacketSubmeshCreateInfo *submesh =
      &mesh->submeshes[submesh_index];
  *root = (VkrMetalSdfGenerateRoot){
      .box_min_voxel = vec4_new(volume->box_min.x, volume->box_min.y,
                                volume->box_min.z, voxel),
      .dims = {dims[0], dims[1], dims[2]},
      .triangle_count = submesh->index_count / 3u,
      .scratch = scratch->gpu_address +
                 (uint64_t)*scratch_voxels * sizeof(uint32_t),
      .volume_texture_id = volume->texture.resource_id,
      .vertices = mesh->gpu_row.vertex_address +
                  (uint64_t)mesh->gpu_row.first_vertex *
                      sizeof(VkrPackedStaticVertex),
      .indices = mesh->gpu_row.index_address +
                 ((uint64_t)mesh->gpu_row.first_index + submesh->first_index) *
                     sizeof(uint32_t),
      .decodes = mesh->gpu_row.decode_address,
      .vertex_offset = submesh->vertex_offset,
      .decode_index = submesh->decode_index,
      .band = VKR_METAL_SDF_BAND_VOXELS,
  };
  const uint32_t generation = prepared->sun_shadow.generation_count++;
  prepared->sun_shadow.generation_roots[generation] = root_gpu;
  prepared->sun_shadow.generation_voxels[generation] = voxels;
  prepared->sun_shadow.generation_triangles[generation] = root->triangle_count;
  *scratch_voxels += voxels;
  volume->state = VKR_METAL_SDF_VOLUME_READY;
  /* The next frame's table copy lists it; a stale layout is redone first. */
  if (!renderer->sdf_layout_dirty)
    renderer->sdf_rows[volume->row] =
        vkr_metal_packet_sdf_row(mesh, submesh_index);
  return true_v;
}

/* The mesh of `candidate` when it is a sun caster that needs its volume
   generated, with the instance's largest axis scale; the GPU's field
   casters (vkr_metal_sdf_field_row) are the same casters once ready. Sizes
   are judged in world metres: a caster too small to cast a far shadow or
   too large for a volume, such as terrain, keeps the cascades. */
vkr_internal VkrMetalPacketMesh *
vkr_metal_packet_sdf_pending_mesh(VkrMetalPacketRenderer *renderer,
                                  const VkrWorldDrawCandidate *candidate,
                                  float32_t *out_world_scale) {
  if ((candidate->flags & VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER) == 0u ||
      (candidate->flags & VKR_WORLD_DRAW_CANDIDATE_SHADOW_TRANSMISSION) !=
          0u ||
      (candidate->state_bucket & 2u) != 0u ||
      candidate->instance.skinning_index != 0u)
    return NULL;
  /* Meshes are keyed by the candidate's geometry. */
  const VkrMeshHandle geometry = {
      .id = candidate->geometry.id,
      .generation = candidate->geometry.generation,
  };
  VkrMetalPacketMesh *mesh = vkr_metal_packet_resolve_mesh(renderer, geometry);
  if (!mesh || !mesh->sdf_volumes ||
      candidate->submesh_index >= mesh->submesh_count ||
      mesh->sdf_volumes[candidate->submesh_index].state !=
          VKR_METAL_SDF_VOLUME_EMPTY)
    return NULL;
  const VkrMetalSdfVolume *volume = &mesh->sdf_volumes[candidate->submesh_index];
  *out_world_scale =
      vkr_metal_packet_sdf_scale(candidate->instance.model, true_v);
  const float32_t world_extent =
      Max(volume->box_size.x, Max(volume->box_size.y, volume->box_size.z)) *
      *out_world_scale;
  return world_extent >= VKR_METAL_SDF_MIN_EXTENT &&
                 world_extent <= VKR_METAL_SDF_MAX_EXTENT
             ? mesh
             : NULL;
}

/* Generates the volumes the frame's sun casters need: looks through at most
   VKR_METAL_SDF_SCAN_CANDIDATES candidates from where the last frame
   stopped, until the generations or the scratch run out. */
vkr_internal void vkr_metal_packet_sdf_generate_scan(
    VkrMetalPacketRenderer *renderer, const VkrWorldPassPayload *world,
    VkrMetalPacketFrameUpload *upload, VkrMetalPacketPreparedPass *prepared)
    API_AVAILABLE(macos(26.0)) {
  const uint32_t count = world ? world->gpu_candidate_count : 0u;
  if (count == 0u)
    return;
  uint32_t cursor = renderer->sdf_scan_cursor % count;
  uint32_t scratch_voxels = 0u;
  const uint32_t steps = Min(count, VKR_METAL_SDF_SCAN_CANDIDATES);
  for (uint32_t step = 0u;
       step < steps && prepared->sun_shadow.generation_count <
                           VKR_METAL_SDF_GENERATIONS_PER_FRAME;
       ++step) {
    const VkrWorldDrawCandidate *candidate = &world->gpu_candidates[cursor];
    float32_t world_scale = 1.0f;
    VkrMetalPacketMesh *mesh =
        vkr_metal_packet_sdf_pending_mesh(renderer, candidate, &world_scale);
    bool8_t full = false_v;
    if (mesh)
      (void)vkr_metal_packet_sdf_generate(
          renderer, mesh, candidate->submesh_index, world_scale, upload,
          prepared, &scratch_voxels, &full);
    /* A volume the scratch had no room for starts the next frame's scan. */
    if (full)
      break;
    cursor = (cursor + 1u) % count;
  }
  renderer->sdf_scan_cursor = cursor;
}

/* Prepares Tiled.SunShadow: generates the frame's new distance volumes,
   builds the light-space grid of the field casters around the camera from
   the candidates at binding 4 into the objects at binding 2 and the grid at
   binding 3, and traces them at half resolution into binding 0 from the
   resolved depth at binding 1. */
vkr_internal bool8_t vkr_metal_packet_prepare_tiled_sun_shadow(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> output = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  id<MTLTexture> depth = vkr_metal_packet_pass_texture(renderer, pass, 1u);
  VkrMetalPacketGraphBufferInstance *objects =
      vkr_metal_packet_pass_buffer(renderer, pass, 2u);
  VkrMetalPacketGraphBufferInstance *grid =
      vkr_metal_packet_pass_buffer(renderer, pass, 3u);
  VkrMetalPacketGraphBufferInstance *candidates =
      vkr_metal_packet_pass_buffer(renderer, pass, 4u);
  const VkrShadowPassPayload *shadow = packet->input.shadow;
  if (!output || !depth || !objects || !grid || !candidates || !shadow ||
      !packet->input.lighting)
    return false_v;
  prepared->sun_shadow.generation_count = 0u;
  vkr_metal_packet_sdf_generate_scan(renderer, packet->input.world, upload,
                                     prepared);

  /* The grid spans every receiver the shadows reach around the camera. */
  const Vec3 light = vec3_normalize(
      vec3_negate(packet->input.lighting->directional_direction));
  const Vec3 up = vkr_abs_f32(light.y) < 0.99f ? vec3_new(0.0f, 1.0f, 0.0f)
                                               : vec3_new(1.0f, 0.0f, 0.0f);
  const Vec3 light_u = vec3_normalize(vec3_cross(up, light));
  const Vec3 light_v = vec3_cross(light, light_u);
  const Vec3 camera = packet->input.globals.view_position;
  const float32_t half_extent =
      Max(shadow->receiver.fade_end, VKR_METAL_SDF_GRID_MIN_HALF_EXTENT);
  const float32_t grid_u = vec3_dot(camera, light_u) - half_extent;
  const float32_t grid_v = vec3_dot(camera, light_v) - half_extent;
  const float32_t inverse_cell =
      (float32_t)VKR_METAL_SDF_GRID_SIZE / (2.0f * half_extent);
  const uint32_t cell_count = VKR_METAL_SDF_GRID_CELLS;
  /* Without the frame's field table no candidate is a field caster. The
     graph sizes the grid (tiled.rendergraph.json) and the objects by the
     candidate capacity. */
  const uint32_t candidate_count =
      upload->sdf_volume_rows_gpu ? renderer->gpu_draw_count : 0u;
  if (grid->resource.length < VKR_METAL_SDF_GRID_BYTES ||
      objects->resource.length <
          (uint64_t)candidate_count * sizeof(VkrMetalSdfObject))
    return false_v;
  uint64_t grid_root_gpu = 0u;
  VkrMetalSdfGridRoot *grid_root =
      vkr_metal_packet_next_root(upload, sizeof(*grid_root), &grid_root_gpu);
  if (!grid_root)
    return false_v;
  *grid_root = (VkrMetalSdfGridRoot){
      .light_u = vec4_new(light_u.x, light_u.y, light_u.z, grid_u),
      .light_v = vec4_new(light_v.x, light_v.y, light_v.z, grid_v),
      .candidates = candidates->resource.gpu_address,
      .instances = upload->gpu_draw_instances_gpu,
      .geometry_rows = upload->gpu_draw_geometry_rows_gpu,
      .volume_rows = upload->sdf_volume_rows_gpu,
      .mesh_volumes = upload->sdf_mesh_volumes_gpu,
      .objects = objects->resource.gpu_address,
      .grid = grid->resource.gpu_address,
      .candidate_count = candidate_count,
      .mesh_count = upload->sdf_mesh_count,
      .grid_size = VKR_METAL_SDF_GRID_SIZE,
      .entry_capacity = VKR_METAL_SDF_GRID_ENTRIES,
      .inverse_cell = inverse_cell,
      .band = VKR_METAL_SDF_BAND_VOXELS,
  };
  /* The count and scatter take one SIMD group per candidate. */
  const MTLSize candidate_threads =
      MTLSizeMake(Max(candidate_count, 1u) * 32u, 1u, 1u);
  prepared->sun_shadow.grid[0] = (VkrMetalPacketDispatch){
      .pipeline = renderer->sdf_grid_clear_pipeline,
      .root = grid_root_gpu,
      .threads = MTLSizeMake(cell_count, 1u, 1u),
      .group_size = MTLSizeMake(256u, 1u, 1u),
  };
  prepared->sun_shadow.grid[1] = (VkrMetalPacketDispatch){
      .pipeline = renderer->sdf_grid_count_pipeline,
      .root = grid_root_gpu,
      .threads = candidate_threads,
      .group_size = MTLSizeMake(64u, 1u, 1u),
  };
  const MTLSize block_threads = MTLSizeMake(
      VKR_METAL_SDF_SCAN_BLOCKS * VKR_METAL_SDF_SCAN_THREADS, 1u, 1u);
  const MTLSize scan_group = MTLSizeMake(VKR_METAL_SDF_SCAN_THREADS, 1u, 1u);
  prepared->sun_shadow.grid[2] = (VkrMetalPacketDispatch){
      .pipeline = renderer->sdf_grid_reduce_pipeline,
      .root = grid_root_gpu,
      .threads = block_threads,
      .group_size = scan_group,
  };
  prepared->sun_shadow.grid[3] = (VkrMetalPacketDispatch){
      .pipeline = renderer->sdf_grid_scan_pipeline,
      .root = grid_root_gpu,
      .threads = scan_group,
      .group_size = scan_group,
  };
  prepared->sun_shadow.grid[4] = (VkrMetalPacketDispatch){
      .pipeline = renderer->sdf_grid_apply_pipeline,
      .root = grid_root_gpu,
      .threads = block_threads,
      .group_size = scan_group,
  };
  prepared->sun_shadow.grid[5] = (VkrMetalPacketDispatch){
      .pipeline = renderer->sdf_grid_scatter_pipeline,
      .root = grid_root_gpu,
      .threads = candidate_threads,
      .group_size = MTLSizeMake(64u, 1u, 1u),
  };

  const Mat4 view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      mat4_mul(packet->temporal.jittered_projection,
               packet->input.globals.view));
  /* The trace starts a band before the atmosphere's ramp toward it, which
     its half-resolution samples blend across. */
  const float32_t start = vkr_metal_packet_sun_shadow_start(shadow);
  const float32_t sun_tan_half_angle =
      shadow->cascade_count ? shadow->cascades[0].origin_inv_size_sun.w : 0.0f;
  uint64_t root_gpu = 0u;
  VkrMetalSunShadowRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalSunShadowRoot){
      .inverse_view_projection = mat4_inverse(view_projection),
      .light_u = grid_root->light_u,
      .light_v = grid_root->light_v,
      .light_dir = vec4_new(light.x, light.y, light.z, inverse_cell),
      .depth_texture_id = depth.gpuResourceID._impl,
      .output_texture_id = output.gpuResourceID._impl,
      .objects = objects->resource.gpu_address,
      .grid = grid->resource.gpu_address,
      .extent = {(uint32_t)output.width, (uint32_t)output.height},
      .depth_extent = {renderer->prepared_frame.viewport_width,
                       renderer->prepared_frame.viewport_height},
      .grid_size = VKR_METAL_SDF_GRID_SIZE,
      .start_distance = start * (1.0f - 2.0f * VKR_METAL_SUN_SHADOW_BAND_SHARE),
      .max_distance = VKR_METAL_SDF_TRACE_DISTANCE,
      .softness = 1.0f / Max(sun_tan_half_angle, VKR_METAL_SDF_MIN_SUN_TAN),
      /* The reversed perspective maps distance d along the camera axis to
         depth m23 / d - m22 (mat4_perspective_reversed). */
      .depth_offset = packet->temporal.jittered_projection.m22,
      .depth_scale = packet->temporal.jittered_projection.m23,
      .entry_capacity = VKR_METAL_SDF_GRID_ENTRIES,
  };
  prepared->sun_shadow.volumes_in_flight =
      renderer->sdf_generation_submit_value >
      vkr_metal_packet_completed(renderer);
  if (prepared->sun_shadow.generation_count)
    renderer->sdf_generation_submit_value = renderer->submit_value + 1u;
  /* Once the scan finds nothing more to generate, the far level rebuilds
     without the new field casters. */
  if (prepared->sun_shadow.generation_count) {
    renderer->sdf_field_grew = true_v;
  } else if (renderer->sdf_field_grew) {
    renderer->sdf_field_grew = false_v;
    vkr_atomic_uint64_store(
        &renderer->sdf_field_revision,
        vkr_atomic_uint64_load(&renderer->sdf_field_revision,
                               VKR_MEMORY_ORDER_RELAXED) +
            1u,
        VKR_MEMORY_ORDER_RELEASE);
  }
  prepared->sun_shadow.trace = (VkrMetalPacketDispatch){
      .pipeline = renderer->sun_shadow_trace_pipeline,
      .root = root_gpu,
      .threads = MTLSizeMake(output.width, output.height, 1u),
      .group_size = MTLSizeMake(8u, 8u, 1u),
  };
  prepared->active = true_v;
  return true_v;
}

uint64_t vkr_metal_packet_renderer_sun_field_revision(
    const VkrMetalPacketRenderer *renderer) {
  return renderer ? vkr_atomic_uint64_load(&renderer->sdf_field_revision,
                                           VKR_MEMORY_ORDER_ACQUIRE)
                  : 0u;
}
