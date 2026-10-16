// =============================================================================
// SDF sun shadows on the tiled pipeline (shadow quality proposal, T5)
// =============================================================================
//
// The Distance field shadows setting (owner decision 2026-10-16, after the
// proposal's gate). Each static opaque submesh gets a distance volume the
// first time a frame traces them: the GPU splats the submesh's resident
// triangles into an unsigned narrow-band distance (Bakery is to own the
// volumes). Each frame lists the casters whose volumes are ready in a
// light-space grid: a pixel's ray toward the sun is one point of the plane
// across the sun's direction, so the trace reads one cell's casters.
// `Tiled.SunShadow` traces at half resolution past the near cascades, and
// the atmosphere draw takes the darker of the far level and the trace
// through the opaque pass's unshadowed sun light: terrain, foliage and
// moving casters have no volumes and keep the far level's shadows.

/* Where the SDF trace starts to take over from the cascades, along the
   camera axis: where the far level starts, else where the cascades fade
   out; at once for the replace-all diagnostic. */
vkr_internal float32_t
vkr_metal_packet_sun_shadow_start(const VkrShadowPassPayload *shadow) {
  if (shadow->sdf_mode == VKR_SHADOW_SDF_MODE_REPLACE_ALL)
    return 0.0f;
  if (shadow->far_cascade < shadow->cascade_count)
    return shadow->cascades[shadow->far_cascade].split_near_far_texel_depth.x;
  return shadow->receiver.fade_end;
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

/* Creates `volume`'s texture and records its generation in `generation`, the
   frame's next generation slot: clear the slot's scratch, splat the
   submesh's triangles, convert to the volume. Its longest side takes
   VKR_METAL_SDF_MAX_DIM voxels with the border, and its voxels are no
   smaller than VKR_METAL_SDF_MIN_WORLD_VOXEL under `world_scale`, the
   generating instance's largest axis scale. A submesh whose texture or
   scratch cannot be created is skipped for good. */
vkr_internal bool8_t vkr_metal_packet_sdf_generate(
    VkrMetalPacketRenderer *renderer, const VkrMetalPacketMesh *mesh,
    uint32_t submesh_index, VkrMetalSdfVolume *volume, float32_t world_scale,
    VkrMetalPacketFrameUpload *upload, VkrMetalPacketPreparedPass *prepared,
    uint32_t generation) API_AVAILABLE(macos(26.0)) {
  const float32_t extent = Max(volume->box_size.x,
                               Max(volume->box_size.y, volume->box_size.z));
  const float32_t voxel =
      Max(Max(extent, 1e-6f) / (float32_t)(VKR_METAL_SDF_MAX_DIM -
                                           2u * VKR_METAL_SDF_PAD_VOXELS),
          VKR_METAL_SDF_MIN_WORLD_VOXEL / Max(world_scale, 1e-6f));
  const float32_t box_size[3] = {volume->box_size.x, volume->box_size.y,
                                 volume->box_size.z};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const uint32_t inner = (uint32_t)vkr_ceil_f32(box_size[axis] / voxel);
    volume->dims[axis] = Min(Max(inner, 1u) + 2u * VKR_METAL_SDF_PAD_VOXELS,
                             VKR_METAL_SDF_MAX_DIM);
  }
  volume->voxel = voxel;
  volume->box_min =
      vec3_sub(volume->box_min,
               vec3_new(VKR_METAL_SDF_PAD_VOXELS * voxel,
                        VKR_METAL_SDF_PAD_VOXELS * voxel,
                        VKR_METAL_SDF_PAD_VOXELS * voxel));

  VkrMetalPacketCommandSlot *slot = renderer->active_command_slot;
  VkrMetalBufferResource *scratch = &slot->sdf_scratch[generation];
  if (!scratch->metal_buffer &&
      vkr_metal_memory_device_create_buffer(
          renderer->memory,
          (uint64_t)VKR_METAL_SDF_MAX_DIM * VKR_METAL_SDF_MAX_DIM *
              VKR_METAL_SDF_MAX_DIM * sizeof(uint32_t),
          VKR_GPU_ALLOCATION_OWNER_SHADOW_FIELD,
          scratch) != VKR_METAL_MEMORY_STATUS_OK) {
    volume->state = VKR_METAL_SDF_VOLUME_SKIPPED;
    return false_v;
  }
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
  descriptor.textureType = MTLTextureType3D;
  descriptor.pixelFormat = MTLPixelFormatR8Unorm;
  descriptor.width = volume->dims[0];
  descriptor.height = volume->dims[1];
  descriptor.depth = volume->dims[2];
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
  renderer->sdf_volume_bytes +=
      (uint64_t)volume->dims[0] * volume->dims[1] * volume->dims[2];

  const VkrMetalPacketSubmeshCreateInfo *submesh =
      &mesh->submeshes[submesh_index];
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
  *root = (VkrMetalSdfGenerateRoot){
      .box_min_voxel = vec4_new(volume->box_min.x, volume->box_min.y,
                                volume->box_min.z, voxel),
      .dims = {volume->dims[0], volume->dims[1], volume->dims[2]},
      .triangle_count = submesh->index_count / 3u,
      .scratch = scratch->gpu_address,
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
  const uint32_t voxels = volume->dims[0] * volume->dims[1] * volume->dims[2];
  prepared->sun_shadow.clears[generation] = (VkrMetalPacketDispatch){
      .pipeline = renderer->sdf_clear_pipeline,
      .root = root_gpu,
      .threads = MTLSizeMake(voxels, 1u, 1u),
      .group_size = MTLSizeMake(256u, 1u, 1u),
  };
  prepared->sun_shadow.splats[generation] = (VkrMetalPacketDispatch){
      .pipeline = renderer->sdf_splat_pipeline,
      .root = root_gpu,
      .threads = MTLSizeMake(Max(root->triangle_count, 1u), 1u, 1u),
      .group_size = MTLSizeMake(64u, 1u, 1u),
  };
  prepared->sun_shadow.converts[generation] = (VkrMetalPacketDispatch){
      .pipeline = renderer->sdf_convert_pipeline,
      .root = root_gpu,
      .threads =
          MTLSizeMake(volume->dims[0], volume->dims[1], volume->dims[2]),
      .group_size = MTLSizeMake(4u, 4u, 4u),
  };
  volume->state = VKR_METAL_SDF_VOLUME_READY;
  return true_v;
}

/* One caster's object record: its volume's [0, 1] coordinates from a world
   point and its world distance scales. */
vkr_internal VkrMetalSdfObject
vkr_metal_packet_sdf_object(const VkrMetalSdfVolume *volume, Mat4 model) {
  const Mat4 inverse = mat4_inverse_affine(model);
  const float32_t box_min[3] = {volume->box_min.x, volume->box_min.y,
                                volume->box_min.z};
  VkrMetalSdfObject object = {.volume_texture_id = volume->texture.resource_id};
  for (uint32_t row = 0u; row < 3u; ++row) {
    const float32_t extent = (float32_t)volume->dims[row] * volume->voxel;
    object.world_to_volume[row] = vec4_new(
        inverse.elements[0 * 4 + row] / extent,
        inverse.elements[1 * 4 + row] / extent,
        inverse.elements[2 * 4 + row] / extent,
        (inverse.elements[3 * 4 + row] - box_min[row]) / extent);
  }
  /* The smallest axis scale keeps a stretched instance's distances
     conservative. */
  const float32_t world_voxel =
      volume->voxel * vkr_metal_packet_sdf_scale(model, false_v);
  object.scale = vec4_new(VKR_METAL_SDF_BAND_VOXELS * world_voxel,
                          world_voxel, 0.0f, 0.0f);
  return object;
}

/* The cells, as [u0, u1] x [v0, v1], that the light-space rectangle of a
   volume's box under `model` covers; false when it misses the grid. */
vkr_internal bool8_t vkr_metal_packet_sdf_cells(
    const VkrMetalSdfVolume *volume, Mat4 model, Vec3 light_u, Vec3 light_v,
    float32_t grid_u, float32_t grid_v, float32_t inverse_cell,
    uint16_t out_cells[4]) {
  float32_t u_min = VKR_FLOAT_MAX, u_max = -VKR_FLOAT_MAX;
  float32_t v_min = VKR_FLOAT_MAX, v_max = -VKR_FLOAT_MAX;
  for (uint32_t corner = 0u; corner < 8u; ++corner) {
    const Vec3 local = vec3_new(
        volume->box_min.x +
            ((corner & 1u) ? (float32_t)volume->dims[0] * volume->voxel : 0.0f),
        volume->box_min.y +
            ((corner & 2u) ? (float32_t)volume->dims[1] * volume->voxel : 0.0f),
        volume->box_min.z +
            ((corner & 4u) ? (float32_t)volume->dims[2] * volume->voxel
                           : 0.0f));
    const Vec3 world = mat4_mul_vec3(model, local);
    const float32_t u = vec3_dot(world, light_u);
    const float32_t v = vec3_dot(world, light_v);
    u_min = Min(u_min, u);
    u_max = Max(u_max, u);
    v_min = Min(v_min, v);
    v_max = Max(v_max, v);
  }
  const float32_t size = (float32_t)VKR_METAL_SDF_GRID_SIZE;
  const float32_t cu0 = (u_min - grid_u) * inverse_cell;
  const float32_t cu1 = (u_max - grid_u) * inverse_cell;
  const float32_t cv0 = (v_min - grid_v) * inverse_cell;
  const float32_t cv1 = (v_max - grid_v) * inverse_cell;
  if (cu1 < 0.0f || cv1 < 0.0f || cu0 >= size || cv0 >= size)
    return false_v;
  out_cells[0] = (uint16_t)Max(cu0, 0.0f);
  out_cells[1] = (uint16_t)Min(cu1, size - 1.0f);
  out_cells[2] = (uint16_t)Max(cv0, 0.0f);
  out_cells[3] = (uint16_t)Min(cv1, size - 1.0f);
  return true_v;
}

/* Grows the CPU grid scratch to `objects` cell rectangles. */
vkr_internal bool8_t
vkr_metal_packet_sdf_reserve(VkrMetalPacketRenderer *renderer,
                             uint32_t objects) {
  if (!renderer->sdf_cell_cursor) {
    renderer->sdf_cell_cursor = vkr_allocator_alloc(
        renderer->allocator,
        (uint64_t)VKR_METAL_SDF_GRID_SIZE * VKR_METAL_SDF_GRID_SIZE *
            sizeof(uint32_t),
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!renderer->sdf_cell_cursor)
      return false_v;
  }
  if (objects <= renderer->sdf_object_cell_capacity)
    return true_v;
  uint16_t *cells = vkr_allocator_alloc(
      renderer->allocator, (uint64_t)objects * 4u * sizeof(uint16_t),
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!cells)
    return false_v;
  if (renderer->sdf_object_cells)
    vkr_allocator_free(renderer->allocator, renderer->sdf_object_cells,
                       (uint64_t)renderer->sdf_object_cell_capacity * 4u *
                           sizeof(uint16_t),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->sdf_object_cells = cells;
  renderer->sdf_object_cell_capacity = objects;
  return true_v;
}

/* Prepares Tiled.SunShadow: generates at most
   VKR_METAL_SDF_GENERATIONS_PER_FRAME new distance volumes, lists the
   static and rigid moving opaque casters whose volumes are ready in the
   light-space grid around the camera, and traces them at half resolution
   into binding 0 from the resolved depth at binding 1. Cutout and skinned
   casters keep the cascades. */
vkr_internal bool8_t vkr_metal_packet_prepare_tiled_sun_shadow(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> output = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  id<MTLTexture> depth = vkr_metal_packet_pass_texture(renderer, pass, 1u);
  const VkrWorldPassPayload *world = packet->input.world;
  const VkrShadowPassPayload *shadow = packet->input.shadow;
  if (!output || !depth || !shadow || !packet->input.lighting)
    return false_v;
  prepared->sun_shadow.generation_count = 0u;

  const Vec3 light = vec3_normalize(
      vec3_negate(packet->input.lighting->directional_direction));
  const Vec3 up = vkr_abs_f32(light.y) < 0.99f ? vec3_new(0.0f, 1.0f, 0.0f)
                                               : vec3_new(1.0f, 0.0f, 0.0f);
  const Vec3 light_u = vec3_normalize(vec3_cross(up, light));
  const Vec3 light_v = vec3_cross(light, light_u);
  const Vec3 camera = packet->input.globals.view_position;
  const float32_t grid_u =
      vec3_dot(camera, light_u) - VKR_METAL_SDF_GRID_HALF_EXTENT;
  const float32_t grid_v =
      vec3_dot(camera, light_v) - VKR_METAL_SDF_GRID_HALF_EXTENT;
  const float32_t inverse_cell = (float32_t)VKR_METAL_SDF_GRID_SIZE /
                                 (2.0f * VKR_METAL_SDF_GRID_HALF_EXTENT);
  const uint32_t cell_count = VKR_METAL_SDF_GRID_SIZE * VKR_METAL_SDF_GRID_SIZE;

  /* Static casters, then moving ones: a rigid mover's volume holds in its
     own space, so it moves with its instance transform. */
  const uint32_t candidates =
      world ? Min(world->gpu_shadow_candidate_count, VKR_METAL_SDF_MAX_OBJECTS)
            : 0u;
  uint64_t objects_gpu = 0u;
  VkrMetalSdfObject *objects =
      candidates ? vkr_metal_packet_next_root_span(
                       upload, (uint64_t)candidates * sizeof(VkrMetalSdfObject),
                       &objects_gpu)
                 : NULL;
  if ((candidates && !objects) ||
      !vkr_metal_packet_sdf_reserve(renderer, Max(candidates, 1u)))
    return false_v;
  MemZero(renderer->sdf_cell_cursor, (uint64_t)cell_count * sizeof(uint32_t));

  uint32_t object_count = 0u;
  uint64_t entries = 0u;
  for (uint32_t i = 0u; i < candidates; ++i) {
    const VkrWorldDrawCandidate *candidate = &world->gpu_candidates[i];
    if ((candidate->state_bucket & 2u) != 0u ||
        candidate->instance.skinning_index != 0u)
      continue;
    /* Meshes are keyed by the candidate's geometry. */
    const VkrMeshHandle geometry = {
        .id = candidate->geometry.id,
        .generation = candidate->geometry.generation,
    };
    VkrMetalPacketMesh *mesh =
        vkr_metal_packet_resolve_mesh(renderer, geometry);
    if (!mesh || !mesh->sdf_volumes ||
        candidate->submesh_index >= mesh->submesh_count)
      continue;
    VkrMetalSdfVolume *volume = &mesh->sdf_volumes[candidate->submesh_index];
    /* Sizes are judged in world metres: a caster too small to cast a far
       shadow or too large for a volume, such as terrain, keeps the
       cascades. */
    const float32_t world_scale =
        vkr_metal_packet_sdf_scale(candidate->instance.model, true_v);
    const float32_t world_extent =
        Max(volume->box_size.x, Max(volume->box_size.y, volume->box_size.z)) *
        world_scale;
    if (world_extent < VKR_METAL_SDF_MIN_EXTENT ||
        world_extent > VKR_METAL_SDF_MAX_EXTENT)
      continue;
    if (volume->state == VKR_METAL_SDF_VOLUME_EMPTY &&
        prepared->sun_shadow.generation_count <
            VKR_METAL_SDF_GENERATIONS_PER_FRAME &&
        vkr_metal_packet_sdf_generate(renderer, mesh, candidate->submesh_index,
                                      volume, world_scale, upload, prepared,
                                      prepared->sun_shadow.generation_count))
      prepared->sun_shadow.generation_count++;
    if (volume->state != VKR_METAL_SDF_VOLUME_READY)
      continue;
    uint16_t *cells = &renderer->sdf_object_cells[object_count * 4u];
    if (!vkr_metal_packet_sdf_cells(volume, candidate->instance.model, light_u,
                                    light_v, grid_u, grid_v, inverse_cell,
                                    cells))
      continue;
    const uint64_t covered =
        (uint64_t)(cells[1] - cells[0] + 1u) * (cells[3] - cells[2] + 1u);
    if (entries + covered > VKR_METAL_SDF_MAX_GRID_ENTRIES)
      continue;
    entries += covered;
    objects[object_count] =
        vkr_metal_packet_sdf_object(volume, candidate->instance.model);
    for (uint32_t v = cells[2]; v <= cells[3]; ++v)
      for (uint32_t u = cells[0]; u <= cells[1]; ++u)
        renderer->sdf_cell_cursor[v * VKR_METAL_SDF_GRID_SIZE + u]++;
    object_count++;
  }

  /* The grid: cell_count + 1 offsets, then each cell's object indices. */
  uint64_t grid_gpu = 0u;
  uint32_t *grid = vkr_metal_packet_next_root_span(
      upload, ((uint64_t)cell_count + 1u + Max(entries, 1u)) * sizeof(uint32_t),
      &grid_gpu);
  if (!grid)
    return false_v;
  uint32_t offset = 0u;
  for (uint32_t cell = 0u; cell < cell_count; ++cell) {
    grid[cell] = offset;
    offset += renderer->sdf_cell_cursor[cell];
    renderer->sdf_cell_cursor[cell] = grid[cell];
  }
  grid[cell_count] = offset;
  uint32_t *indices = grid + cell_count + 1u;
  for (uint32_t object = 0u; object < object_count; ++object) {
    const uint16_t *cells = &renderer->sdf_object_cells[object * 4u];
    for (uint32_t v = cells[2]; v <= cells[3]; ++v)
      for (uint32_t u = cells[0]; u <= cells[1]; ++u)
        indices[renderer->sdf_cell_cursor[v * VKR_METAL_SDF_GRID_SIZE + u]++] =
            object;
  }

  const Mat4 view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      mat4_mul(packet->temporal.jittered_projection,
               packet->input.globals.view));
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
      .light_u = vec4_new(light_u.x, light_u.y, light_u.z, grid_u),
      .light_v = vec4_new(light_v.x, light_v.y, light_v.z, grid_v),
      .light_dir = vec4_new(light.x, light.y, light.z, inverse_cell),
      .depth_texture_id = depth.gpuResourceID._impl,
      .output_texture_id = output.gpuResourceID._impl,
      .objects = objects_gpu,
      .grid = grid_gpu,
      .extent = {(uint32_t)output.width, (uint32_t)output.height},
      .depth_extent = {renderer->prepared_frame.viewport_width,
                       renderer->prepared_frame.viewport_height},
      .grid_size = VKR_METAL_SDF_GRID_SIZE,
      .start_distance = start * (1.0f - VKR_METAL_SUN_SHADOW_BAND_SHARE),
      .max_distance = VKR_METAL_SDF_TRACE_DISTANCE,
      .softness = 1.0f / Max(sun_tan_half_angle, VKR_METAL_SDF_MIN_SUN_TAN),
      /* The reversed perspective maps distance d along the camera axis to
         depth m23 / d - m22 (mat4_perspective_reversed). */
      .depth_offset = packet->temporal.jittered_projection.m22,
      .depth_scale = packet->temporal.jittered_projection.m23,
  };
  prepared->sun_shadow.volumes_in_flight =
      renderer->sdf_generation_submit_value >
      vkr_metal_packet_completed(renderer);
  if (prepared->sun_shadow.generation_count)
    renderer->sdf_generation_submit_value = renderer->submit_value + 1u;
  prepared->sun_shadow.trace = (VkrMetalPacketDispatch){
      .pipeline = renderer->sun_shadow_trace_pipeline,
      .root = root_gpu,
      .threads = MTLSizeMake(output.width, output.height, 1u),
      .group_size = MTLSizeMake(8u, 8u, 1u),
  };
  prepared->active = true_v;
  return true_v;
}
