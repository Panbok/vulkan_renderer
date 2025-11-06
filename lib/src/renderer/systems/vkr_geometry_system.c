#include "renderer/systems/vkr_geometry_system.h"
#include "math/vec.h"
#include "memory/vkr_arena_allocator.h"

// Convenience for index type bytes
vkr_internal INLINE uint32_t vkr_index_type_size(VkrIndexType type) {
  switch (type) {
  case VKR_INDEX_TYPE_UINT16:
    return 2;
  case VKR_INDEX_TYPE_UINT32:
  default:
    return 4;
  }
}

// Lazily initialize a pool for a given layout using defaults
vkr_internal bool32_t vkr_geometry_pool_init(VkrGeometrySystem *system,
                                             VkrGeometryVertexLayoutType layout,
                                             uint32_t vertex_stride_bytes,
                                             VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(layout < GEOMETRY_VERTEX_LAYOUT_COUNT, "Invalid layout");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(vertex_stride_bytes > 0, "Vertex stride bytes must be > 0");
  assert_log(system->config.default_max_vertices > 0,
             "Default capacity vertices must be > 0");
  assert_log(system->config.default_max_indices > 0,
             "Default capacity indices must be > 0");

  VkrGeometryPool *pool = &system->pools[layout];
  if (pool->initialized) {
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  pool->layout = layout;
  pool->vertex_stride_bytes = vertex_stride_bytes;
  pool->capacity_vertices = system->config.default_max_vertices;
  pool->capacity_indices = system->config.default_max_indices;

  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  pool->vertex_buffer = vkr_vertex_buffer_create(
      system->renderer, NULL, pool->vertex_stride_bytes,
      (uint32_t)pool->capacity_vertices, VKR_VERTEX_INPUT_RATE_VERTEX,
      string8_lit("GeometrySystem.VertexBuffer"), &err);
  if (err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }

  pool->index_buffer =
      vkr_index_buffer_create(system->renderer, NULL, VKR_INDEX_TYPE_UINT32,
                              (uint32_t)pool->capacity_indices,
                              string8_lit("GeometrySystem.IndexBuffer"), &err);
  if (err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }

  // Create freelists in BYTES, aligned by stride/element size via allocation
  // sizes
  uint64_t vb_total_bytes =
      pool->capacity_vertices * (uint64_t)pool->vertex_stride_bytes;
  uint64_t ib_total_bytes =
      pool->capacity_indices *
      (uint64_t)vkr_index_type_size(VKR_INDEX_TYPE_UINT32);

  if (vb_total_bytes > UINT32_MAX) {
    log_error("Vertex buffer size exceeds maximum: %llu bytes", vb_total_bytes);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // Allocate memory for vertex freelist nodes
  uint64_t vb_freelist_mem_size =
      vkr_freelist_calculate_memory_requirement(vb_total_bytes);
  pool->vertex_freelist_memory =
      vkr_allocator_alloc(&system->allocator, vb_freelist_mem_size,
                          VKR_ALLOCATOR_MEMORY_TAG_FREELIST);
  if (pool->vertex_freelist_memory == NULL) {
    log_error("Failed to allocate memory for vertex freelist");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  if (!vkr_freelist_create(pool->vertex_freelist_memory, vb_freelist_mem_size,
                           vb_total_bytes, &pool->vertex_freelist)) {
    log_error("Failed to create geometry vertex freelist");
    vkr_allocator_free(&system->allocator, pool->vertex_freelist_memory,
                       vb_freelist_mem_size, VKR_ALLOCATOR_MEMORY_TAG_FREELIST);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // Allocate memory for index freelist nodes
  uint64_t ib_freelist_mem_size =
      vkr_freelist_calculate_memory_requirement(ib_total_bytes);
  pool->index_freelist_memory =
      vkr_allocator_alloc(&system->allocator, ib_freelist_mem_size,
                          VKR_ALLOCATOR_MEMORY_TAG_FREELIST);
  if (pool->index_freelist_memory == NULL) {
    log_error("Failed to allocate memory for index freelist");
    vkr_freelist_destroy(&pool->vertex_freelist);
    vkr_allocator_free(&system->allocator, pool->vertex_freelist_memory,
                       vb_freelist_mem_size, VKR_ALLOCATOR_MEMORY_TAG_FREELIST);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  if (!vkr_freelist_create(pool->index_freelist_memory, ib_freelist_mem_size,
                           ib_total_bytes, &pool->index_freelist)) {
    log_error("Failed to create geometry index freelist");
    vkr_freelist_destroy(&pool->vertex_freelist);
    vkr_allocator_free(&system->allocator, pool->vertex_freelist_memory,
                       vb_freelist_mem_size, VKR_ALLOCATOR_MEMORY_TAG_FREELIST);
    vkr_allocator_free(&system->allocator, pool->index_freelist_memory,
                       ib_freelist_mem_size, VKR_ALLOCATOR_MEMORY_TAG_FREELIST);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  pool->initialized = true_v;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal INLINE VkrGeometryPool *
vkr_geometry_get_pool(VkrGeometrySystem *system,
                      VkrGeometryVertexLayoutType layout,
                      VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(layout < GEOMETRY_VERTEX_LAYOUT_COUNT, "Invalid layout");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrGeometryPool *pool = &system->pools[layout];
  if (pool->initialized) {
    *out_error = VKR_RENDERER_ERROR_NONE;
    return pool;
  }
  // Pool not yet configured; caller must require a stride first
  *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
  return NULL;
}

vkr_internal VkrGeometry *geometry_acquire_slot(VkrGeometrySystem *system,
                                                VkrGeometryHandle *out_handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");

  if (system->free_count > 0) {
    uint32_t slot = system->free_ids.data[system->free_count - 1];
    system->free_count--;
    VkrGeometry *geometry_slot =
        array_get_VkrGeometry(&system->geometries, slot);
    geometry_slot->id = slot + 1;
    geometry_slot->generation = (geometry_slot->generation == 0)
                                    ? system->generation_counter++
                                    : (system->generation_counter++);
    *out_handle = (VkrGeometryHandle){.id = geometry_slot->id,
                                      .generation = geometry_slot->generation};
    return geometry_slot;
  }

  for (uint32_t geometry = 0; geometry < system->max_geometries; geometry++) {
    VkrGeometry *geometry_slot =
        array_get_VkrGeometry(&system->geometries, geometry);
    if (geometry_slot->id == 0 && geometry_slot->generation == 0) {
      geometry_slot->id = geometry + 1;
      geometry_slot->generation = system->generation_counter++;
      *out_handle = (VkrGeometryHandle){
          .id = geometry_slot->id, .generation = geometry_slot->generation};
      return geometry_slot;
    }
  }

  return NULL;
}

bool32_t vkr_geometry_system_init(VkrGeometrySystem *system,
                                  VkrRendererFrontendHandle renderer,
                                  const VkrGeometrySystemConfig *config,
                                  VkrRendererError *out_error) {
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
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  system->allocator.ctx = system->arena;
  if (!vkr_allocator_arena(&system->allocator)) {
    log_fatal("Failed to create geometry system allocator");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  system->renderer = renderer;
  system->max_geometries = config->default_max_geometries;
  system->config = *config;

  system->geometries =
      array_create_VkrGeometry(system->arena, system->max_geometries);
  for (uint32_t geometry = 0; geometry < system->max_geometries; geometry++) {
    VkrGeometry init = {0};
    init.pipeline_id =
        VKR_INVALID_ID; // todo: get pipeline id from config and use it
    // back-end of the renderer instead of object_id on ShaderStateObject
    array_set_VkrGeometry(&system->geometries, geometry, init);
  }
  system->free_ids =
      array_create_uint32_t(system->arena, system->max_geometries);
  system->free_count = 0;

  system->geometry_by_name = vkr_hash_table_create_VkrGeometryEntry(
      system->arena, (uint64_t)system->max_geometries * 2);

  system->generation_counter = 1;

  for (VkrGeometryVertexLayoutType layout_type = 0;
       layout_type < GEOMETRY_VERTEX_LAYOUT_COUNT; layout_type++) {
    system->pools[layout_type].initialized = false_v;
    system->pools[layout_type].layout = layout_type;
    system->pools[layout_type].vertex_stride_bytes = 0;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
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
    vkr_vertex_buffer_destroy(system->renderer, &pool->vertex_buffer);
    vkr_index_buffer_destroy(system->renderer, &pool->index_buffer);

    // Destroy freelists and free their memory
    uint64_t vb_total = pool->vertex_freelist.total_size;
    vkr_freelist_destroy(&pool->vertex_freelist);
    if (pool->vertex_freelist_memory) {
      uint64_t vb_freelist_mem_size =
          vkr_freelist_calculate_memory_requirement(vb_total);
      vkr_allocator_free(&system->allocator, pool->vertex_freelist_memory,
                         vb_freelist_mem_size,
                         VKR_ALLOCATOR_MEMORY_TAG_FREELIST);
      pool->vertex_freelist_memory = NULL;
    }

    uint64_t ib_total = pool->index_freelist.total_size;
    vkr_freelist_destroy(&pool->index_freelist);
    if (pool->index_freelist_memory) {
      uint64_t ib_freelist_mem_size =
          vkr_freelist_calculate_memory_requirement(ib_total);
      vkr_allocator_free(&system->allocator, pool->index_freelist_memory,
                         ib_freelist_mem_size,
                         VKR_ALLOCATOR_MEMORY_TAG_FREELIST);
      pool->index_freelist_memory = NULL;
    }

    pool->initialized = false_v;
  }

  array_destroy_VkrGeometry(&system->geometries);
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
    uint32_t index_count, bool8_t auto_release, String8 debug_name,
    VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(vertices != NULL, "Vertices is NULL");
  assert_log(indices != NULL, "Indices is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrGeometryHandle handle = (VkrGeometryHandle){0};

  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  VkrGeometryPool *pool = vkr_geometry_get_pool(system, layout, &err);
  if (!pool || err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return handle;
  }

  // Allocate ranges in BYTES from freelists; enforce size multiples of stride
  uint64_t vb_bytes = (uint64_t)vertex_count * pool->vertex_stride_bytes;
  uint64_t ib_bytes =
      (uint64_t)index_count * vkr_index_type_size(pool->index_buffer.type);
  // Align allocations to stride/element for safety and round up to next power
  // of 2
  uint64_t vb_align =
      1u << (64 - VkrCountLeadingZeros64(pool->vertex_stride_bytes - 1));
  uint64_t ib_align =
      1u << (64 - VkrCountLeadingZeros64(
                      vkr_index_type_size(pool->index_buffer.type) - 1));
  vb_bytes = AlignPow2(vb_bytes, vb_align);
  ib_bytes = AlignPow2(ib_bytes, ib_align);

  uint64_t vb_offset_bytes = 0, ib_offset_bytes = 0;
  if (!vkr_freelist_allocate(&pool->vertex_freelist, vb_bytes,
                             &vb_offset_bytes)) {
    log_error("Geometry vertex pool out of space for '%s'",
              string8_cstr(&debug_name));
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return handle;
  }

  if (!vkr_freelist_allocate(&pool->index_freelist, ib_bytes,
                             &ib_offset_bytes)) {
    // Rollback vertex allocation
    vkr_freelist_free(&pool->vertex_freelist, vb_bytes, vb_offset_bytes);
    log_error("Geometry index pool out of space for '%s'",
              string8_cstr(&debug_name));
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return handle;
  }

  uint32_t first_vertex = vb_offset_bytes / pool->vertex_stride_bytes;
  uint32_t first_index =
      ib_offset_bytes / vkr_index_type_size(pool->index_buffer.type);

  VkrGeometry *geom = geometry_acquire_slot(system, &handle);
  if (!geom) {
    // Rollback allocations
    vkr_freelist_free(&pool->index_freelist, ib_bytes, ib_offset_bytes);
    vkr_freelist_free(&pool->vertex_freelist, vb_bytes, vb_offset_bytes);
    log_error("No free geometry entries for '%s'", string8_cstr(&debug_name));
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  geom->layout = layout;
  geom->first_vertex = first_vertex;
  geom->vertex_count = vertex_count;
  geom->first_index = first_index;
  geom->index_count = index_count;

  // Upload data to GPU buffers at the allocated offsets
  err = vkr_renderer_upload_buffer(system->renderer, pool->vertex_buffer.handle,
                                   vb_offset_bytes, vb_bytes, vertices);
  if (err != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to upload vertices for '%s'", string8_cstr(&debug_name));
    // Rollback allocations and entry
    vkr_freelist_free(&pool->vertex_freelist, vb_bytes, vb_offset_bytes);
    vkr_freelist_free(&pool->index_freelist, ib_bytes, ib_offset_bytes);
    // Return slot to unused state
    geom->id = 0;
    *out_error = err;
    return (VkrGeometryHandle){0};
  }

  err = vkr_renderer_upload_buffer(system->renderer, pool->index_buffer.handle,
                                   ib_offset_bytes, ib_bytes, indices);
  if (err != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to upload indices for '%s'", string8_cstr(&debug_name));
    // Rollback allocations and entry
    vkr_freelist_free(&pool->vertex_freelist, vb_bytes, vb_offset_bytes);
    vkr_freelist_free(&pool->index_freelist, ib_bytes, ib_offset_bytes);
    // Return slot to unused state
    geom->id = 0;
    *out_error = err;
    return (VkrGeometryHandle){0};
  }

  // Register lifetime entry by name (create stable key; synthesize if missing)
  const char *stable_name = NULL;
  if (debug_name.str && debug_name.length > 0) {
    char *name_copy =
        (char *)vkr_allocator_alloc(&system->allocator, debug_name.length + 1,
                                    VKR_ALLOCATOR_MEMORY_TAG_STRING);
    assert_log(name_copy != NULL, "Failed to allocate geometry name");
    MemCopy(name_copy, debug_name.str, (size_t)debug_name.length);
    name_copy[debug_name.length] = '\0';
    stable_name = (const char *)name_copy;
  } else {
    char *name_copy = (char *)vkr_allocator_alloc(
        &system->allocator, 32, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    assert_log(name_copy != NULL, "Failed to allocate geometry name");
    int written = snprintf(name_copy, 32, "geom_%u", handle.id);
    if (written < 0) {
      MemCopy(name_copy, "geom", 5);
    }
    stable_name = (const char *)name_copy;
  }

  geom->name = stable_name;
  VkrGeometryEntry life_entry = {.id = (handle.id - 1),
                                 .ref_count = 1,
                                 .auto_release = auto_release,
                                 .name = stable_name};
  vkr_hash_table_insert_VkrGeometryEntry(&system->geometry_by_name, stable_name,
                                         life_entry);

  *out_error = VKR_RENDERER_ERROR_NONE;
  return handle;
}

void vkr_geometry_system_acquire(VkrGeometrySystem *system,
                                 VkrGeometryHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  uint32_t idx = handle.id - 1;
  if (idx >= system->geometries.length)
    return;

  VkrGeometry *geometry = array_get_VkrGeometry(&system->geometries, idx);
  if (geometry->generation != handle.generation)
    return;
  if (geometry->id == 0)
    return;

  if (geometry->name) {
    VkrGeometryEntry *lifetime_entry = vkr_hash_table_get_VkrGeometryEntry(
        &system->geometry_by_name, geometry->name);
    if (lifetime_entry) {
      lifetime_entry->ref_count++;
    }
  }
}

void vkr_geometry_system_release(VkrGeometrySystem *system,
                                 VkrGeometryHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  uint32_t idx = handle.id - 1;
  if (idx >= system->geometries.length)
    return;

  VkrGeometry *geometry = array_get_VkrGeometry(&system->geometries, idx);
  if (geometry->generation != handle.generation)
    return;

  VkrGeometryEntry *lifetime_entry = NULL;
  if (geometry->name) {
    lifetime_entry = vkr_hash_table_get_VkrGeometryEntry(
        &system->geometry_by_name, geometry->name);
  }

  if (lifetime_entry && lifetime_entry->ref_count > 0)
    lifetime_entry->ref_count--;

  bool32_t should_release = false_v;
  if (lifetime_entry) {
    should_release =
        (lifetime_entry->ref_count == 0) && lifetime_entry->auto_release;
  }

  if (should_release) {
    // Free ranges back to freelists
    VkrGeometryPool *pool = &system->pools[geometry->layout];
    uint32_t vb_bytes = geometry->vertex_count * pool->vertex_stride_bytes;
    uint32_t ib_bytes =
        geometry->index_count * vkr_index_type_size(pool->index_buffer.type);
    uint32_t vb_align =
        1u << (32 - VkrCountLeadingZeros32(pool->vertex_stride_bytes - 1));
    uint32_t ib_align =
        1u << (32 - VkrCountLeadingZeros32(
                        vkr_index_type_size(pool->index_buffer.type) - 1));
    vb_bytes = (uint32_t)AlignPow2(vb_bytes, vb_align);
    ib_bytes = (uint32_t)AlignPow2(ib_bytes, ib_align);
    uint32_t vb_offset_bytes =
        geometry->first_vertex * pool->vertex_stride_bytes;
    uint32_t ib_offset_bytes =
        geometry->first_index * vkr_index_type_size(pool->index_buffer.type);
    vkr_freelist_free(&pool->vertex_freelist, vb_bytes, vb_offset_bytes);
    vkr_freelist_free(&pool->index_freelist, ib_bytes, ib_offset_bytes);

    // push to free id stack
    assert_log(system->free_count < system->free_ids.length,
               "free_ids overflow");
    system->free_ids.data[system->free_count++] = idx;

    // mark slot as empty
    geometry->id = 0;
    geometry->first_vertex = 0;
    geometry->vertex_count = 0;
    geometry->first_index = 0;
    geometry->index_count = 0;
    if (geometry->name) {
      vkr_hash_table_remove_VkrGeometryEntry(&system->geometry_by_name,
                                             geometry->name);
      geometry->name = NULL;
    }
  }
}

void vkr_geometry_system_render(VkrRendererFrontendHandle renderer,
                                VkrGeometrySystem *system,
                                VkrGeometryHandle handle,
                                uint32_t instance_count) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");
  assert_log(instance_count > 0, "Instance count must be > 0");

  VkrGeometry *entry = NULL;
  uint32_t idx = handle.id - 1;
  if (idx < system->max_geometries) {
    VkrGeometry *geometry_candidate =
        array_get_VkrGeometry(&system->geometries, idx);
    if (geometry_candidate->generation == handle.generation) {
      entry = geometry_candidate;
    }
  }
  if (!entry)
    return;

  VkrGeometryPool *pool = &system->pools[entry->layout];

  VkrVertexBufferBinding vbb = {
      .buffer = pool->vertex_buffer.handle,
      .binding = 0,
      .offset =
          (uint64_t)entry->first_vertex * (uint64_t)pool->vertex_stride_bytes,
  };
  vkr_renderer_bind_vertex_buffer(renderer, &vbb);

  VkrIndexBufferBinding ibb = {
      .buffer = pool->index_buffer.handle,
      .type = VKR_INDEX_TYPE_UINT32,
      .offset = (uint64_t)entry->first_index *
                vkr_index_type_size(VKR_INDEX_TYPE_UINT32),
  };
  vkr_renderer_bind_index_buffer(renderer, &ibb);

  vkr_renderer_draw_indexed(renderer, entry->index_count, instance_count, 0, 0,
                            0);
}

// Helper function to write a vertex to the interleaved vertex buffer
// Vertex layout: Position (Vec3 + pad), Normal (Vec3 + pad), Texcoord (Vec2),
// Color (Vec4), Tangent (Vec4), trailing pad (2 floats)
// Total: 20 floats per vertex
vkr_internal void vkr_write_vertex(float32_t *verts, uint32_t *offset,
                                   Vec3 position, Vec3 normal, Vec2 texcoord,
                                   Vec4 color, Vec4 tangent) {
  uint32_t w = *offset;
  // Position (Vec3 + 1 float pad)
  verts[w++] = position.x;
  verts[w++] = position.y;
  verts[w++] = position.z;
  verts[w++] = 0.0f; // padding

  // Normal (Vec3 + 1 float pad)
  verts[w++] = normal.x;
  verts[w++] = normal.y;
  verts[w++] = normal.z;
  verts[w++] = 0.0f; // padding

  // Texcoord (Vec2)
  verts[w++] = texcoord.u;
  verts[w++] = texcoord.v;

  // Color (Vec4) - zeroed, will be computed later
  verts[w++] = color.r;
  verts[w++] = color.g;
  verts[w++] = color.b;
  verts[w++] = color.a;

  // Tangent (Vec4) - zeroed, will be computed later
  verts[w++] = tangent.x;
  verts[w++] = tangent.y;
  verts[w++] = tangent.z;
  verts[w++] = tangent.w;

  // Trailing pad (2 floats)
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;

  *offset = w;
}

VkrGeometryHandle vkr_geometry_system_create_default_cube(
    VkrGeometrySystem *system, float32_t width, float32_t height,
    float32_t depth, VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  // Calculate half-dimensions for centered cube
  float32_t hw = width * 0.5f;
  float32_t hh = height * 0.5f;
  float32_t hd = depth * 0.5f;

  // Zero vectors for color and tangent (will be computed later)
  Vec4 zero_color = vec4_zero();
  Vec4 zero_tangent = vec4_zero();

  float32_t verts[24 * 20] = {0};
  uint32_t w = 0;

  // ============================================================================
  // Front Face (+Z direction, facing toward viewer in right-handed system)
  // ============================================================================
  // Normal: (0, 0, 1) - points forward along positive Z-axis
  // Vertices ordered counter-clockwise when viewed from outside
  Vec3 front_normal = vec3_new(0.0f, 0.0f, 1.0f);

  // v0: Bottom-left corner
  // Position: (-hw, -hh, hd)
  // Texcoord: (0, 0) - bottom-left of texture
  vkr_write_vertex(verts, &w, vec3_new(-hw, -hh, hd), front_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v1: Bottom-right corner
  // Position: (hw, -hh, hd)
  // Texcoord: (1, 0) - bottom-right of texture
  vkr_write_vertex(verts, &w, vec3_new(hw, -hh, hd), front_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v2: Top-right corner
  // Position: (hw, hh, hd)
  // Texcoord: (1, 1) - top-right of texture
  vkr_write_vertex(verts, &w, vec3_new(hw, hh, hd), front_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);

  // v3: Top-left corner
  // Position: (-hw, hh, hd)
  // Texcoord: (0, 1) - top-left of texture
  vkr_write_vertex(verts, &w, vec3_new(-hw, hh, hd), front_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  // ============================================================================
  // Back Face (-Z direction, facing away from viewer)
  // ============================================================================
  // Normal: (0, 0, -1) - points backward along negative Z-axis
  // Vertices ordered counter-clockwise when viewed from outside (from back)
  Vec3 back_normal = vec3_new(0.0f, 0.0f, -1.0f);

  // v4: Bottom-left corner
  // Position: (-hw, -hh, -hd)
  // Texcoord: (1, 0) - bottom-right of texture (flipped horizontally)
  vkr_write_vertex(verts, &w, vec3_new(-hw, -hh, -hd), back_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v5: Bottom-right corner
  // Position: (hw, -hh, -hd)
  // Texcoord: (0, 0) - bottom-left of texture (flipped horizontally)
  vkr_write_vertex(verts, &w, vec3_new(hw, -hh, -hd), back_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v6: Top-right corner
  // Position: (hw, hh, -hd)
  // Texcoord: (0, 1) - top-left of texture (flipped horizontally)
  vkr_write_vertex(verts, &w, vec3_new(hw, hh, -hd), back_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  // v7: Top-left corner
  // Position: (-hw, hh, -hd)
  // Texcoord: (1, 1) - top-right of texture (flipped horizontally)
  vkr_write_vertex(verts, &w, vec3_new(-hw, hh, -hd), back_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);

  // ============================================================================
  // Left Face (-X direction, facing left)
  // ============================================================================
  // Normal: (-1, 0, 0) - points left along negative X-axis
  // Vertices ordered counter-clockwise when viewed from outside (from left)
  Vec3 left_normal = vec3_new(-1.0f, 0.0f, 0.0f);

  // v8: Bottom-back corner
  // Position: (-hw, -hh, -hd)
  // Texcoord: (0, 0) - bottom-left of texture
  vkr_write_vertex(verts, &w, vec3_new(-hw, -hh, -hd), left_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v9: Bottom-front corner
  // Position: (-hw, -hh, hd)
  // Texcoord: (1, 0) - bottom-right of texture
  vkr_write_vertex(verts, &w, vec3_new(-hw, -hh, hd), left_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v10: Top-front corner
  // Position: (-hw, hh, hd)
  // Texcoord: (1, 1) - top-right of texture
  vkr_write_vertex(verts, &w, vec3_new(-hw, hh, hd), left_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);

  // v11: Top-back corner
  // Position: (-hw, hh, -hd)
  // Texcoord: (0, 1) - top-left of texture
  vkr_write_vertex(verts, &w, vec3_new(-hw, hh, -hd), left_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  // ============================================================================
  // Right Face (+X direction, facing right)
  // ============================================================================
  // Normal: (1, 0, 0) - points right along positive X-axis
  // Vertices ordered counter-clockwise when viewed from outside (from right)
  Vec3 right_normal = vec3_new(1.0f, 0.0f, 0.0f);

  // v12: Bottom-back corner
  // Position: (hw, -hh, -hd)
  // Texcoord: (1, 0) - bottom-right of texture (flipped horizontally)
  vkr_write_vertex(verts, &w, vec3_new(hw, -hh, -hd), right_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v13: Bottom-front corner
  // Position: (hw, -hh, hd)
  // Texcoord: (0, 0) - bottom-left of texture (flipped horizontally)
  vkr_write_vertex(verts, &w, vec3_new(hw, -hh, hd), right_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v14: Top-front corner
  // Position: (hw, hh, hd)
  // Texcoord: (0, 1) - top-left of texture (flipped horizontally)
  vkr_write_vertex(verts, &w, vec3_new(hw, hh, hd), right_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  // v15: Top-back corner
  // Position: (hw, hh, -hd)
  // Texcoord: (1, 1) - top-right of texture (flipped horizontally)
  vkr_write_vertex(verts, &w, vec3_new(hw, hh, -hd), right_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);

  // ============================================================================
  // Top Face (+Y direction, facing up)
  // ============================================================================
  // Normal: (0, 1, 0) - points up along positive Y-axis
  // Vertices ordered counter-clockwise when viewed from outside (from above)
  Vec3 top_normal = vec3_new(0.0f, 1.0f, 0.0f);

  // v16: Front-left corner
  // Position: (-hw, hh, hd)
  // Texcoord: (0, 0) - bottom-left of texture
  vkr_write_vertex(verts, &w, vec3_new(-hw, hh, hd), top_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v17: Front-right corner
  // Position: (hw, hh, hd)
  // Texcoord: (1, 0) - bottom-right of texture
  vkr_write_vertex(verts, &w, vec3_new(hw, hh, hd), top_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v18: Back-right corner
  // Position: (hw, hh, -hd)
  // Texcoord: (1, 1) - top-right of texture
  vkr_write_vertex(verts, &w, vec3_new(hw, hh, -hd), top_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);

  // v19: Back-left corner
  // Position: (-hw, hh, -hd)
  // Texcoord: (0, 1) - top-left of texture
  vkr_write_vertex(verts, &w, vec3_new(-hw, hh, -hd), top_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  // ============================================================================
  // Bottom Face (-Y direction, facing down)
  // ============================================================================
  // Normal: (0, -1, 0) - points down along negative Y-axis
  // Vertices ordered counter-clockwise when viewed from outside (from below)
  Vec3 bottom_normal = vec3_new(0.0f, -1.0f, 0.0f);

  // v20: Back-left corner
  // Position: (-hw, -hh, -hd)
  // Texcoord: (0, 0) - bottom-left of texture
  vkr_write_vertex(verts, &w, vec3_new(-hw, -hh, -hd), bottom_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v21: Back-right corner
  // Position: (hw, -hh, -hd)
  // Texcoord: (1, 0) - bottom-right of texture
  vkr_write_vertex(verts, &w, vec3_new(hw, -hh, -hd), bottom_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v22: Front-right corner
  // Position: (hw, -hh, hd)
  // Texcoord: (1, 1) - top-right of texture
  vkr_write_vertex(verts, &w, vec3_new(hw, -hh, hd), bottom_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);

  // v23: Front-left corner
  // Position: (-hw, -hh, hd)
  // Texcoord: (0, 1) - top-left of texture
  vkr_write_vertex(verts, &w, vec3_new(-hw, -hh, hd), bottom_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  uint32_t indices[36] = {
      0,  1,  2,  2,  3,  0,  // Front
      4,  7,  6,  6,  5,  4,  // Back
      8,  9,  10, 10, 11, 8,  // Left
      12, 15, 14, 14, 13, 12, // Right
      16, 17, 18, 18, 19, 16, // Top
      20, 21, 22, 22, 23, 20  // Bottom
  };

  VkrGeometryHandle h = vkr_geometry_system_create_from_interleaved(
      system, GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_TEXCOORD2_COLOR_TANGENT,
      verts, 24, indices, 36, false_v, string8_lit("Default Cube"), out_error);
  if (out_error && *out_error == VKR_RENDERER_ERROR_NONE) {
    system->default_geometry = h;
  }

  return h;
}

VkrGeometryHandle
vkr_geometry_system_create_default_plane(VkrGeometrySystem *system,
                                         float32_t width, float32_t height,
                                         VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  float32_t hw = width * 0.5f;
  float32_t hh = height * 0.5f;

  float32_t verts[4 * 8] = {0};
  uint32_t w = 0;
  verts[w++] = -hw;
  verts[w++] = -hh;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = hw;
  verts[w++] = -hh;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = 1.0f;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = hw;
  verts[w++] = hh;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = 1.0f;
  verts[w++] = 1.0f;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = -hw;
  verts[w++] = hh;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;
  verts[w++] = 1.0f;
  verts[w++] = 0.0f;
  verts[w++] = 0.0f;

  // counter-clockwise winding
  uint32_t indices[6] = {0, 2, 1, 0, 3, 2};

  VkrGeometryHandle h = vkr_geometry_system_create_from_interleaved(
      system, GEOMETRY_VERTEX_LAYOUT_POSITION_TEXCOORD, verts, 4, indices, 6,
      false_v, string8_lit("Default Plane"), out_error);
  if (out_error && *out_error == VKR_RENDERER_ERROR_NONE) {
    system->default_geometry = h;
  }

  return h;
}

VkrGeometryHandle
vkr_geometry_system_create_default_plane2d(VkrGeometrySystem *system,
                                           float32_t width, float32_t height,
                                           VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  float32_t hw = width * 0.5f;
  float32_t hh = height * 0.5f;

  // [x, y, u, v]
  float32_t verts[4 * 4] = {0};
  uint32_t w = 0;
  verts[w++] = -hw;  // x
  verts[w++] = -hh;  // y
  verts[w++] = 0.0f; // u
  verts[w++] = 0.0f; // v

  verts[w++] = hw;
  verts[w++] = -hh;
  verts[w++] = 1.0f;
  verts[w++] = 0.0f;

  verts[w++] = hw;
  verts[w++] = hh;
  verts[w++] = 1.0f;
  verts[w++] = 1.0f;

  verts[w++] = -hw;
  verts[w++] = hh;
  verts[w++] = 0.0f;
  verts[w++] = 1.0f;

  uint32_t indices[6] = {0, 2, 1, 0, 3, 2};

  VkrGeometryHandle h = vkr_geometry_system_create_from_interleaved(
      system, GEOMETRY_VERTEX_LAYOUT_POSITION2_TEXCOORD, verts, 4, indices, 6,
      false_v, string8_lit("Default Plane 2D"), out_error);
  return h;
}

bool32_t
vkr_geometry_system_get_layout(VkrGeometrySystem *system,
                               VkrGeometryHandle handle,
                               VkrGeometryVertexLayoutType *out_layout) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(handle.id != 0, "Invalid geometry handle");
  assert_log(out_layout != NULL, "Out layout is NULL");

  if (handle.id == 0)
    return false_v;
  uint32_t idx = handle.id - 1;
  if (idx >= system->geometries.length)
    return false_v;
  VkrGeometry *geom = array_get_VkrGeometry(&system->geometries, idx);
  if (geom->generation != handle.generation || geom->id == 0)
    return false_v;
  *out_layout = geom->layout;
  return true_v;
}

void vkr_geometry_system_require_layout_stride(
    VkrGeometrySystem *system, VkrGeometryVertexLayoutType layout,
    uint32_t stride_bytes, VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(layout < GEOMETRY_VERTEX_LAYOUT_COUNT, "Invalid layout");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(stride_bytes > 0, "Stride bytes must be > 0");

  VkrGeometryPool *pool = &system->pools[layout];
  if (pool->initialized) {
    if (pool->vertex_stride_bytes != stride_bytes) {
      log_error(
          "Geometry pool for layout %u already initialized with stride %u; "
          "requested stride %u",
          (uint32_t)layout, pool->vertex_stride_bytes, stride_bytes);
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      return;
    }
    *out_error = VKR_RENDERER_ERROR_NONE;
    return;
  }

  // Initialize the pool with the requested stride
  if (!vkr_geometry_pool_init(system, layout, stride_bytes, out_error)) {
    return;
  }
}
