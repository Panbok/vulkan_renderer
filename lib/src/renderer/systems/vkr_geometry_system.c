#include "renderer/systems/vkr_geometry_system.h"

// Convenience for index type bytes
vkr_internal INLINE uint32_t vkr_index_type_size(IndexType type) {
  switch (type) {
  case INDEX_TYPE_UINT16:
    return 2;
  case INDEX_TYPE_UINT32:
  default:
    return 4;
  }
}

// Lazily initialize a pool for a given layout using defaults
vkr_internal bool32_t vkr_geometry_pool_init(VkrGeometrySystem *system,
                                             VkrGeometryVertexLayoutType layout,
                                             uint32_t vertex_stride_bytes,
                                             RendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(layout < GEOMETRY_VERTEX_LAYOUT_COUNT, "Invalid layout");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(vertex_stride_bytes > 0, "Vertex stride bytes must be > 0");
  assert_log(system->default_capacity_vertices > 0,
             "Default capacity vertices must be > 0");
  assert_log(system->default_capacity_indices > 0,
             "Default capacity indices must be > 0");

  VkrGeometryPool *pool = &system->pools[layout];
  if (pool->initialized) {
    *out_error = RENDERER_ERROR_NONE;
    return true_v;
  }

  pool->layout = layout;
  pool->vertex_stride_bytes = vertex_stride_bytes;
  pool->capacity_vertices = system->default_capacity_vertices;
  pool->capacity_indices = system->default_capacity_indices;

  RendererError err = RENDERER_ERROR_NONE;
  pool->vertex_buffer = vertex_buffer_create(
      system->renderer, system->arena, NULL, pool->vertex_stride_bytes,
      (uint32_t)pool->capacity_vertices, VERTEX_INPUT_RATE_VERTEX,
      string8_lit("GeometrySystem.VertexBuffer"), &err);
  if (err != RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }

  pool->index_buffer =
      index_buffer_create(system->renderer, system->arena, NULL,
                          INDEX_TYPE_UINT32, (uint32_t)pool->capacity_indices,
                          string8_lit("GeometrySystem.IndexBuffer"), &err);
  if (err != RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }

  // Create freelists in BYTES, aligned by stride/element size via allocation
  // sizes
  uint64_t vb_total_bytes =
      pool->capacity_vertices * (uint64_t)pool->vertex_stride_bytes;
  uint64_t ib_total_bytes =
      pool->capacity_indices * (uint64_t)vkr_index_type_size(INDEX_TYPE_UINT32);

  if (vb_total_bytes > UINT32_MAX) {
    log_error("Vertex buffer size exceeds maximum: %llu bytes", vb_total_bytes);
    *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  if (!vkr_freelist_create(system->arena, (uint32_t)vb_total_bytes,
                           &pool->vertex_freelist)) {
    log_error("Failed to create geometry vertex freelist");
    *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  if (!vkr_freelist_create(system->arena, (uint32_t)ib_total_bytes,
                           &pool->index_freelist)) {
    log_error("Failed to create geometry index freelist");
    *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  pool->initialized = true_v;
  *out_error = RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal INLINE VkrGeometryPool *
vkr_geometry_get_pool(VkrGeometrySystem *system,
                      VkrGeometryVertexLayoutType layout,
                      RendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(layout < GEOMETRY_VERTEX_LAYOUT_COUNT, "Invalid layout");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrGeometryPool *pool = &system->pools[layout];
  if (!pool->initialized) {
    // Determine stride for this layout if not primary
    uint32_t attr_count = 0, binding_count = 0, stride = 0;
    VertexInputAttributeDescription *attrs = NULL;
    VertexInputBindingDescription *bindings = NULL;
    uint64_t mark = arena_pos(system->arena);
    vkr_geometry_fill_vertex_input_descriptions(
        layout, system->arena, &attr_count, &attrs, &binding_count, &bindings,
        &stride);
    arena_reset_to(system->arena, mark, ARENA_MEMORY_TAG_RENDERER);

    if (!vkr_geometry_pool_init(system, layout, stride, out_error)) {
      return NULL;
    }
  }
  *out_error = RENDERER_ERROR_NONE;
  return pool;
}

vkr_internal VkrGeometryEntry *
geometry_acquire_entry(VkrGeometrySystem *system,
                       VkrGeometryHandle *out_handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");

  if (system->free_count > 0) {
    uint32_t slot = system->free_ids.data[system->free_count - 1];
    system->free_count--;
    VkrGeometryEntry *e = array_get_VkrGeometryEntry(&system->entries, slot);
    e->id = slot + 1;
    e->generation = (e->generation == 0) ? 1 : (e->generation + 1);
    e->ref_count = 1;
    *out_handle = (VkrGeometryHandle){.id = e->id, .generation = e->generation};
    return e;
  }

  for (uint32_t geometry = 0; geometry < system->max_geometries; geometry++) {
    VkrGeometryEntry *e =
        array_get_VkrGeometryEntry(&system->entries, geometry);
    if (e->id == 0 && e->generation == 0) {
      e->id = geometry + 1;
      e->generation = 1;
      e->ref_count = 1;
      *out_handle =
          (VkrGeometryHandle){.id = e->id, .generation = e->generation};
      return e;
    }
  }
  return NULL;
}

bool32_t vkr_geometry_system_init(VkrGeometrySystem *system,
                                  RendererFrontendHandle renderer,
                                  const VkrGeometrySystemConfig *config,
                                  RendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(config != NULL, "Config is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  MemZero(system, sizeof(VkrGeometrySystem));

  // Create internal arena for geometry system
  // Reserve/commit sizes tuned for geometry metadata and freelist node storage.
  // This owns CPU-side allocations only.

  ArenaFlags app_arena_flags = bitset8_create();
  bitset8_set(&app_arena_flags, ARENA_FLAG_LARGE_PAGES);
  system->arena = arena_create(MB(32), MB(8), app_arena_flags);
  if (!system->arena) {
    log_fatal("Failed to create geometry system arena");
    *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  system->renderer = renderer;
  system->max_geometries = config->max_geometries;
  system->default_capacity_vertices = config->max_vertices;
  system->default_capacity_indices = config->max_indices;

  system->entries =
      array_create_VkrGeometryEntry(system->arena, system->max_geometries);
  for (uint32_t i = 0; i < system->max_geometries; i++) {
    VkrGeometryEntry init = {0};
    array_set_VkrGeometryEntry(&system->entries, i, init);
  }
  system->free_ids =
      array_create_uint32_t(system->arena, system->max_geometries);
  system->free_count = 0;

  system->geometry_by_name = vkr_hash_table_create_VkrGeometryHandle(
      system->arena, (uint64_t)system->max_geometries * 2);

  for (VkrGeometryVertexLayoutType layout_type = 0;
       layout_type < GEOMETRY_VERTEX_LAYOUT_COUNT; layout_type++) {
    system->pools[layout_type].initialized = false_v;
    system->pools[layout_type].layout = layout_type;
    system->pools[layout_type].vertex_stride_bytes = 0;
  }

  // Eagerly initialize primary layout pool using provided stride
  if (config->vertex_stride_bytes > 0) {
    if (!vkr_geometry_pool_init(system, config->primary_layout,
                                config->vertex_stride_bytes, out_error)) {
      return false_v;
    }
  }

  *out_error = RENDERER_ERROR_NONE;
  return true_v;
}

void vkr_geometry_system_shutdown(VkrGeometrySystem *system) {
  if (!system)
    return;

  // Destroy GPU buffers and freelists per pool
  for (uint32_t i = 0; i < GEOMETRY_VERTEX_LAYOUT_COUNT; i++) {
    VkrGeometryPool *pool = &system->pools[i];
    if (!pool->initialized)
      continue;
    vertex_buffer_destroy(system->renderer, &pool->vertex_buffer);
    index_buffer_destroy(system->renderer, &pool->index_buffer);
    vkr_freelist_destroy(&pool->vertex_freelist);
    vkr_freelist_destroy(&pool->index_freelist);
    pool->initialized = false_v;
  }

  array_destroy_VkrGeometryEntry(&system->entries);
  array_destroy_uint32_t(&system->free_ids);

  if (system->arena) {
    arena_destroy(system->arena);
    system->arena = NULL;
  }

  MemZero(system, sizeof(VkrGeometrySystem));
}

VkrGeometryHandle vkr_geometry_system_create_from_interleaved(
    VkrGeometrySystem *system, VkrGeometryVertexLayoutType layout,
    const void *vertices, uint32_t vertex_count, const uint32_t *indices,
    uint32_t index_count, String8 debug_name, RendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(vertices != NULL, "Vertices is NULL");
  assert_log(indices != NULL, "Indices is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrGeometryHandle handle = (VkrGeometryHandle){0};

  RendererError err = RENDERER_ERROR_NONE;
  VkrGeometryPool *pool = vkr_geometry_get_pool(system, layout, &err);
  if (!pool || err != RENDERER_ERROR_NONE) {
    *out_error = err;
    return handle;
  }

  // Allocate ranges in BYTES from freelists; enforce size multiples of stride
  uint32_t vb_bytes = vertex_count * pool->vertex_stride_bytes;
  uint32_t ib_bytes =
      index_count * vkr_index_type_size(pool->index_buffer.type);
  // Align allocations to stride/element for safety and round up to next power
  // of 2
  uint32_t vb_align =
      1u << (32 - VkrCountLeadingZeros32(pool->vertex_stride_bytes - 1));
  uint32_t ib_align =
      1u << (32 - VkrCountLeadingZeros32(
                      vkr_index_type_size(pool->index_buffer.type) - 1));
  vb_bytes = (uint32_t)AlignPow2(vb_bytes, vb_align);
  ib_bytes = (uint32_t)AlignPow2(ib_bytes, ib_align);

  uint32_t vb_offset_bytes = 0, ib_offset_bytes = 0;
  if (!vkr_freelist_allocate(&pool->vertex_freelist, vb_bytes,
                             &vb_offset_bytes)) {
    log_error("Geometry vertex pool out of space for '%s'",
              string8_cstr(&debug_name));
    *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
    return handle;
  }

  if (!vkr_freelist_allocate(&pool->index_freelist, ib_bytes,
                             &ib_offset_bytes)) {
    // Rollback vertex allocation
    vkr_freelist_free(&pool->vertex_freelist, vb_bytes, vb_offset_bytes);
    log_error("Geometry index pool out of space for '%s'",
              string8_cstr(&debug_name));
    *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
    return handle;
  }

  uint32_t first_vertex = vb_offset_bytes / pool->vertex_stride_bytes;
  uint32_t first_index =
      ib_offset_bytes / vkr_index_type_size(pool->index_buffer.type);

  VkrGeometryEntry *entry = geometry_acquire_entry(system, &handle);
  if (!entry) {
    // Rollback allocations
    vkr_freelist_free(&pool->index_freelist, ib_bytes, ib_offset_bytes);
    vkr_freelist_free(&pool->vertex_freelist, vb_bytes, vb_offset_bytes);
    log_error("No free geometry entries for '%s'", string8_cstr(&debug_name));
    *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  entry->layout = layout;
  entry->first_vertex = first_vertex;
  entry->vertex_count = vertex_count;
  entry->first_index = first_index;
  entry->index_count = index_count;

  // Upload data to GPU buffers at the allocated offsets
  err = renderer_upload_buffer(system->renderer, pool->vertex_buffer.handle,
                               vb_offset_bytes, vb_bytes, vertices);
  if (err != RENDERER_ERROR_NONE) {
    log_error("Failed to upload vertices for '%s'", string8_cstr(&debug_name));
    // Rollback allocations and entry
    vkr_freelist_free(&pool->vertex_freelist, vb_bytes, vb_offset_bytes);
    vkr_freelist_free(&pool->index_freelist, ib_bytes, ib_offset_bytes);
    // Return entry to pool
    entry->id = 0;
    entry->ref_count = 0;
    *out_error = err;
    return (VkrGeometryHandle){0};
  }

  err = renderer_upload_buffer(system->renderer, pool->index_buffer.handle,
                               ib_offset_bytes, ib_bytes, indices);
  if (err != RENDERER_ERROR_NONE) {
    log_error("Failed to upload indices for '%s'", string8_cstr(&debug_name));
    // Rollback allocations and entry
    vkr_freelist_free(&pool->vertex_freelist, vb_bytes, vb_offset_bytes);
    vkr_freelist_free(&pool->index_freelist, ib_bytes, ib_offset_bytes);
    // Return entry to pool
    entry->id = 0;
    entry->ref_count = 0;
    *out_error = err;
    return (VkrGeometryHandle){0};
  }

  // Optional: register by name if provided
  if (debug_name.str && debug_name.length > 0) {
    entry->name = (const char *)debug_name.str;
    vkr_hash_table_insert_VkrGeometryHandle(&system->geometry_by_name,
                                            entry->name, handle);
  } else {
    entry->name = NULL;
  }

  *out_error = RENDERER_ERROR_NONE;
  return handle;
}

void vkr_geometry_system_acquire(VkrGeometrySystem *system,
                                 VkrGeometryHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  uint32_t idx = handle.id - 1;
  if (idx >= system->entries.length)
    return;

  VkrGeometryEntry *entry = array_get_VkrGeometryEntry(&system->entries, idx);
  if (entry->generation != handle.generation)
    return;
  if (entry->id == 0)
    return;

  entry->ref_count++;
}

void vkr_geometry_system_release(VkrGeometrySystem *system,
                                 VkrGeometryHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  uint32_t idx = handle.id - 1;
  if (idx >= system->entries.length)
    return;

  VkrGeometryEntry *entry = array_get_VkrGeometryEntry(&system->entries, idx);
  if (entry->generation != handle.generation)
    return;

  if (entry->ref_count > 0)
    entry->ref_count--;
  if (entry->ref_count == 0) {
    // Free ranges back to freelists
    VkrGeometryPool *pool = &system->pools[entry->layout];
    uint32_t vb_bytes = entry->vertex_count * pool->vertex_stride_bytes;
    uint32_t ib_bytes =
        entry->index_count * vkr_index_type_size(pool->index_buffer.type);
    uint32_t vb_align =
        1u << (32 - VkrCountLeadingZeros32(pool->vertex_stride_bytes - 1));
    uint32_t ib_align =
        1u << (32 - VkrCountLeadingZeros32(
                        vkr_index_type_size(pool->index_buffer.type) - 1));
    vb_bytes = (uint32_t)AlignPow2(vb_bytes, vb_align);
    ib_bytes = (uint32_t)AlignPow2(ib_bytes, ib_align);
    uint32_t vb_offset_bytes = entry->first_vertex * pool->vertex_stride_bytes;
    uint32_t ib_offset_bytes =
        entry->first_index * vkr_index_type_size(pool->index_buffer.type);
    vkr_freelist_free(&pool->vertex_freelist, vb_bytes, vb_offset_bytes);
    vkr_freelist_free(&pool->index_freelist, ib_bytes, ib_offset_bytes);

    // push to free id stack
    assert_log(system->free_count < system->free_ids.length,
               "free_ids overflow");
    system->free_ids.data[system->free_count++] = idx;

    // mark slot as empty
    entry->id = 0;
    entry->first_vertex = 0;
    entry->vertex_count = 0;
    entry->first_index = 0;
    entry->index_count = 0;
    if (entry->name) {
      vkr_hash_table_remove_VkrGeometryHandle(&system->geometry_by_name,
                                              entry->name);
      entry->name = NULL;
    }
  }
}

void vkr_geometry_system_render(RendererFrontendHandle renderer,
                                VkrGeometrySystem *system,
                                VkrGeometryHandle handle,
                                uint32_t instance_count) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");
  assert_log(instance_count > 0, "Instance count must be > 0");

  VkrGeometryEntry *entry = NULL;
  uint32_t idx = handle.id - 1;
  if (idx < system->max_geometries) {
    VkrGeometryEntry *e = array_get_VkrGeometryEntry(&system->entries, idx);
    if (e->generation == handle.generation) {
      entry = e;
    }
  }
  if (!entry)
    return;

  VkrGeometryPool *pool = &system->pools[entry->layout];

  VertexBufferBinding vbb = {
      .buffer = pool->vertex_buffer.handle,
      .binding = 0,
      .offset =
          (uint64_t)entry->first_vertex * (uint64_t)pool->vertex_stride_bytes,
  };
  renderer_bind_vertex_buffer(renderer, &vbb);

  IndexBufferBinding ibb = {
      .buffer = pool->index_buffer.handle,
      .type = INDEX_TYPE_UINT32,
      .offset =
          (uint64_t)entry->first_index * vkr_index_type_size(INDEX_TYPE_UINT32),
  };
  renderer_bind_index_buffer(renderer, &ibb);

  renderer_draw_indexed(renderer, entry->index_count, instance_count, 0, 0, 0);
}

typedef InterleavedVertex_PositionTexcoord GeoVertexPT;

VkrGeometryHandle vkr_geometry_system_create_default_cube(
    VkrGeometrySystem *system, float32_t width, float32_t height,
    float32_t depth, RendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  float32_t hw = width * 0.5f;
  float32_t hh = height * 0.5f;
  float32_t hd = depth * 0.5f;

  GeoVertexPT verts[24] = {0};
  uint32_t idx = 0;
  // Front
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, -hh, hd),
                               .texcoord = vec2_new(0, 0)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, -hh, hd),
                               .texcoord = vec2_new(1, 0)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, hh, hd),
                               .texcoord = vec2_new(1, 1)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, hh, hd),
                               .texcoord = vec2_new(0, 1)};
  // Back
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, -hh, -hd),
                               .texcoord = vec2_new(1, 0)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, -hh, -hd),
                               .texcoord = vec2_new(0, 0)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, hh, -hd),
                               .texcoord = vec2_new(0, 1)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, hh, -hd),
                               .texcoord = vec2_new(1, 1)};
  // Left
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, -hh, -hd),
                               .texcoord = vec2_new(0, 0)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, -hh, hd),
                               .texcoord = vec2_new(1, 0)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, hh, hd),
                               .texcoord = vec2_new(1, 1)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, hh, -hd),
                               .texcoord = vec2_new(0, 1)};
  // Right
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, -hh, -hd),
                               .texcoord = vec2_new(1, 0)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, -hh, hd),
                               .texcoord = vec2_new(0, 0)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, hh, hd),
                               .texcoord = vec2_new(0, 1)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, hh, -hd),
                               .texcoord = vec2_new(1, 1)};
  // Top
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, hh, hd),
                               .texcoord = vec2_new(0, 1)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, hh, hd),
                               .texcoord = vec2_new(1, 1)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, hh, -hd),
                               .texcoord = vec2_new(1, 0)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, hh, -hd),
                               .texcoord = vec2_new(0, 0)};
  // Bottom
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, -hh, -hd),
                               .texcoord = vec2_new(0, 0)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, -hh, -hd),
                               .texcoord = vec2_new(1, 0)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(hw, -hh, hd),
                               .texcoord = vec2_new(1, 1)};
  verts[idx++] = (GeoVertexPT){.position = vec3_new(-hw, -hh, hd),
                               .texcoord = vec2_new(0, 1)};

  uint32_t indices[36] = {
      0,  1,  2,  2,  3,  0,  // Front
      4,  7,  6,  6,  5,  4,  // Back
      8,  9,  10, 10, 11, 8,  // Left
      12, 15, 14, 14, 13, 12, // Right
      16, 17, 18, 18, 19, 16, // Top
      20, 21, 22, 22, 23, 20  // Bottom
  };

  VkrGeometryHandle h = vkr_geometry_system_create_from_interleaved(
      system, GEOMETRY_VERTEX_LAYOUT_POSITION_TEXCOORD, verts, 24, indices, 36,
      string8_lit("Default Cube"), out_error);
  if (out_error && *out_error == RENDERER_ERROR_NONE) {
    system->default_geometry = h;
  }

  return h;
}

void vkr_geometry_fill_vertex_input_descriptions(
    VkrGeometryVertexLayoutType layout, Arena *arena, uint32_t *out_attr_count,
    VertexInputAttributeDescription **out_attrs, uint32_t *out_binding_count,
    VertexInputBindingDescription **out_bindings, uint32_t *out_stride) {
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(out_attr_count != NULL && out_attrs != NULL, "Attr outputs NULL");
  assert_log(out_binding_count != NULL && out_bindings != NULL,
             "Binding outputs NULL");
  assert_log(out_stride != NULL, "Stride output NULL");

  switch (layout) {
  case GEOMETRY_VERTEX_LAYOUT_POSITION_TEXCOORD: {
    *out_attr_count = 2;
    *out_binding_count = 1;
    *out_stride = sizeof(InterleavedVertex_PositionTexcoord);
    VertexInputAttributeDescription *attrs = arena_alloc(
        arena, sizeof(VertexInputAttributeDescription) * (*out_attr_count),
        ARENA_MEMORY_TAG_RENDERER);
    VertexInputBindingDescription *bindings = arena_alloc(
        arena, sizeof(VertexInputBindingDescription) * (*out_binding_count),
        ARENA_MEMORY_TAG_RENDERER);
    attrs[0] = (VertexInputAttributeDescription){
        .location = 0,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionTexcoord, position)};
    attrs[1] = (VertexInputAttributeDescription){
        .location = 1,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionTexcoord, texcoord)};
    bindings[0] =
        (VertexInputBindingDescription){.binding = 0,
                                        .stride = *out_stride,
                                        .input_rate = VERTEX_INPUT_RATE_VERTEX};
    *out_attrs = attrs;
    *out_bindings = bindings;
    break;
  }
  case GEOMETRY_VERTEX_LAYOUT_POSITION_COLOR: {
    *out_attr_count = 2;
    *out_binding_count = 1;
    *out_stride = sizeof(InterleavedVertex_PositionColor);
    VertexInputAttributeDescription *attrs = arena_alloc(
        arena, sizeof(VertexInputAttributeDescription) * (*out_attr_count),
        ARENA_MEMORY_TAG_RENDERER);
    VertexInputBindingDescription *bindings = arena_alloc(
        arena, sizeof(VertexInputBindingDescription) * (*out_binding_count),
        ARENA_MEMORY_TAG_RENDERER);
    attrs[0] = (VertexInputAttributeDescription){
        .location = 0,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionColor, position)};
    attrs[1] = (VertexInputAttributeDescription){
        .location = 1,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionColor, color)};
    bindings[0] =
        (VertexInputBindingDescription){.binding = 0,
                                        .stride = *out_stride,
                                        .input_rate = VERTEX_INPUT_RATE_VERTEX};
    *out_attrs = attrs;
    *out_bindings = bindings;
    break;
  }
  case GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_COLOR: {
    *out_attr_count = 3;
    *out_binding_count = 1;
    *out_stride = sizeof(InterleavedVertex_PositionNormalColor);
    VertexInputAttributeDescription *attrs = arena_alloc(
        arena, sizeof(VertexInputAttributeDescription) * (*out_attr_count),
        ARENA_MEMORY_TAG_RENDERER);
    VertexInputBindingDescription *bindings = arena_alloc(
        arena, sizeof(VertexInputBindingDescription) * (*out_binding_count),
        ARENA_MEMORY_TAG_RENDERER);
    attrs[0] = (VertexInputAttributeDescription){
        .location = 0,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionNormalColor, position)};
    attrs[1] = (VertexInputAttributeDescription){
        .location = 1,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionNormalColor, normal)};
    attrs[2] = (VertexInputAttributeDescription){
        .location = 2,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionNormalColor, color)};
    bindings[0] =
        (VertexInputBindingDescription){.binding = 0,
                                        .stride = *out_stride,
                                        .input_rate = VERTEX_INPUT_RATE_VERTEX};
    *out_attrs = attrs;
    *out_bindings = bindings;
    break;
  }
  case GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_TEXCOORD: {
    *out_attr_count = 3;
    *out_binding_count = 1;
    *out_stride = sizeof(InterleavedVertex_PositionNormalTexcoord);
    VertexInputAttributeDescription *attrs = arena_alloc(
        arena, sizeof(VertexInputAttributeDescription) * (*out_attr_count),
        ARENA_MEMORY_TAG_RENDERER);
    VertexInputBindingDescription *bindings = arena_alloc(
        arena, sizeof(VertexInputBindingDescription) * (*out_binding_count),
        ARENA_MEMORY_TAG_RENDERER);
    attrs[0] = (VertexInputAttributeDescription){
        .location = 0,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionNormalTexcoord, position)};
    attrs[1] = (VertexInputAttributeDescription){
        .location = 1,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionNormalTexcoord, normal)};
    attrs[2] = (VertexInputAttributeDescription){
        .location = 2,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionNormalTexcoord, texcoord)};
    bindings[0] =
        (VertexInputBindingDescription){.binding = 0,
                                        .stride = *out_stride,
                                        .input_rate = VERTEX_INPUT_RATE_VERTEX};
    *out_attrs = attrs;
    *out_bindings = bindings;
    break;
  }
  case GEOMETRY_VERTEX_LAYOUT_FULL: {
    *out_attr_count = 4;
    *out_binding_count = 1;
    *out_stride = sizeof(InterleavedVertex_Full);
    VertexInputAttributeDescription *attrs = arena_alloc(
        arena, sizeof(VertexInputAttributeDescription) * (*out_attr_count),
        ARENA_MEMORY_TAG_RENDERER);
    VertexInputBindingDescription *bindings = arena_alloc(
        arena, sizeof(VertexInputBindingDescription) * (*out_binding_count),
        ARENA_MEMORY_TAG_RENDERER);
    attrs[0] = (VertexInputAttributeDescription){
        .location = 0,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_Full, position)};
    attrs[1] = (VertexInputAttributeDescription){
        .location = 1,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_Full, normal)};
    attrs[2] = (VertexInputAttributeDescription){
        .location = 2,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32_SFLOAT,
        .offset = offsetof(InterleavedVertex_Full, texcoord)};
    attrs[3] = (VertexInputAttributeDescription){
        .location = 3,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_Full, color)};
    bindings[0] =
        (VertexInputBindingDescription){.binding = 0,
                                        .stride = *out_stride,
                                        .input_rate = VERTEX_INPUT_RATE_VERTEX};
    *out_attrs = attrs;
    *out_bindings = bindings;
    break;
  }
  default: {
    *out_attr_count = 0;
    *out_binding_count = 0;
    *out_stride = 0;
    *out_attrs = NULL;
    *out_bindings = NULL;
    break;
  }
  }
}