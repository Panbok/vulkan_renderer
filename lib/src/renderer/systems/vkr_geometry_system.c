#include "renderer/systems/vkr_geometry_system.h"

void vkr_geometry_system_render_instanced_range_with_index_buffer(
    VkrRendererFrontendHandle renderer, VkrGeometrySystem *system,
    VkrGeometryHandle handle, const VkrIndexBuffer *index_buffer,
    uint32_t index_count, uint32_t first_index, int32_t vertex_offset,
    uint32_t instance_count, uint32_t first_instance);

void vkr_geometry_system_render_indirect_with_index_buffer(
    VkrRendererFrontendHandle renderer, VkrGeometrySystem *system,
    VkrGeometryHandle handle, const VkrIndexBuffer *index_buffer,
    VkrBufferHandle indirect_buffer, uint64_t offset, uint32_t draw_count,
    uint32_t stride);

#include "containers/str.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "math/vkr_quat.h"
#include "memory/vkr_arena_allocator.h"

vkr_internal INLINE VkrGeometry *
vkr_geometry_from_handle(VkrGeometrySystem *system, VkrGeometryHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (!system || handle.id == 0)
    return NULL;

  uint32_t idx = handle.id - 1;
  if (idx >= system->geometries.length)
    return NULL;

  VkrGeometry *geometry = array_get_VkrGeometry(&system->geometries, idx);
  if (geometry->id == 0 || geometry->generation != handle.generation)
    return NULL;

  return geometry;
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
    MemZero(geometry_slot, sizeof(*geometry_slot));
    geometry_slot->id = slot + 1;
    geometry_slot->generation = system->generation_counter++;
    geometry_slot->pipeline_id = VKR_INVALID_ID;
    *out_handle = (VkrGeometryHandle){.id = geometry_slot->id,
                                      .generation = geometry_slot->generation};
    return geometry_slot;
  }

  for (uint32_t geometry = 0; geometry < system->max_geometries; geometry++) {
    VkrGeometry *geometry_slot =
        array_get_VkrGeometry(&system->geometries, geometry);
    if (geometry_slot->id == 0 && geometry_slot->generation == 0) {
      MemZero(geometry_slot, sizeof(*geometry_slot));
      geometry_slot->id = geometry + 1;
      geometry_slot->generation = system->generation_counter++;
      geometry_slot->pipeline_id = VKR_INVALID_ID;
      *out_handle = (VkrGeometryHandle){
          .id = geometry_slot->id, .generation = geometry_slot->generation};
      return geometry_slot;
    }
  }

  return NULL;
}

vkr_internal INLINE void vkr_write_vertex(VkrVertex3d *vertices, uint32_t index,
                                          Vec3 position, Vec3 normal,
                                          Vec2 texcoord, Vec4 color,
                                          Vec4 tangent) {
  VkrVertex3d *vertex = &vertices[index];
  vertex->position = vkr_vertex_pack_vec3(position);
  vertex->normal = vkr_vertex_pack_vec3(normal);
  vertex->texcoord = texcoord;
  vertex->colour = color;
  vertex->tangent = tangent;
}

vkr_internal Vec3 vkr_geometry_axis_or_default(Vec3 axis) {
  if (vec3_length_squared(axis) <= VKR_FLOAT_EPSILON * VKR_FLOAT_EPSILON) {
    return vec3_back();
  }
  return vec3_normalize(axis);
}

vkr_internal VkrQuat vkr_geometry_rotation_from_axis(Vec3 axis) {
  Vec3 from = vec3_back();
  Vec3 to = vkr_geometry_axis_or_default(axis);
  float32_t dot = vkr_clamp_f32(vec3_dot(from, to), -1.0f, 1.0f);
  if (dot > 0.9995f) {
    return vkr_quat_identity();
  }

  if (dot < -0.9995f) {
    return vkr_quat_from_axis_angle(vec3_right(), VKR_PI);
  }

  Vec3 rot_axis = vec3_cross(from, to);
  if (vec3_length_squared(rot_axis) <= VKR_FLOAT_EPSILON) {
    return vkr_quat_identity();
  }
  return vkr_quat_from_axis_angle(vec3_normalize(rot_axis), vkr_acos_f32(dot));
}

vkr_internal void vkr_geometry_apply_transform(VkrVertex3d *verts,
                                               uint32_t count, VkrQuat rotation,
                                               Vec3 translation) {
  for (uint32_t i = 0; i < count; ++i) {
    Vec3 position = vkr_vertex_unpack_vec3(verts[i].position);
    Vec3 normal = vkr_vertex_unpack_vec3(verts[i].normal);
    position = vec3_add(vkr_quat_rotate_vec3(rotation, position), translation);
    normal = vkr_quat_rotate_vec3(rotation, normal);
    verts[i].position = vkr_vertex_pack_vec3(position);
    verts[i].normal = vkr_vertex_pack_vec3(normal);
  }
}

vkr_internal void vkr_geometry_compute_bounds(const VkrVertex3d *verts,
                                              uint32_t count, Vec3 *out_min,
                                              Vec3 *out_max, Vec3 *out_center) {
  if (count == 0) {
    if (out_min)
      *out_min = vec3_zero();
    if (out_max)
      *out_max = vec3_zero();
    if (out_center)
      *out_center = vec3_zero();
    return;
  }

  Vec3 min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
  Vec3 max = vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);

  for (uint32_t i = 0; i < count; ++i) {
    Vec3 p = vkr_vertex_unpack_vec3(verts[i].position);
    min.x = vkr_min_f32(min.x, p.x);
    min.y = vkr_min_f32(min.y, p.y);
    min.z = vkr_min_f32(min.z, p.z);
    max.x = vkr_max_f32(max.x, p.x);
    max.y = vkr_max_f32(max.y, p.y);
    max.z = vkr_max_f32(max.z, p.z);
  }

  if (out_min) {
    *out_min = min;
  }
  if (out_max) {
    *out_max = max;
  }
  if (out_center) {
    *out_center = vec3_scale(vec3_add(min, max), 0.5f);
  }
}

/**
 * @brief Creates a unit cube (2x2x2 by default) using the POSITION_TEXCOORD
 * layout and set as default geometry. Dimensions are full extents.
 * @param system The geometry system to create the default cube in
 * @param out_error The error output
 */
vkr_internal VkrGeometryHandle vkr_geometry_system_create_default_cube(
    VkrGeometrySystem *system, VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  return vkr_geometry_system_create_cube(system, 10.0f, 10.0f, 10.0f,
                                         "Default Cube", out_error);
}

/**
 * @brief Creates a default plane (2x2 by default) using the POSITION_TEXCOORD
 * layout and set as default geometry. Dimensions are full extents.
 * @param system The geometry system to create the default plane in
 * @param width The width of the plane
 * @param height The height of the plane
 * @param out_error The error output
 */
vkr_internal VkrGeometryHandle vkr_geometry_system_create_default_plane(
    VkrGeometrySystem *system, float32_t width, float32_t height,
    VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  float32_t hw = width * 0.5f;
  float32_t hh = height * 0.5f;

  VkrVertex3d verts[4] = {0};
  Vec3 normal = vec3_new(0.0f, 0.0f, 1.0f);
  Vec4 zero_color = vec4_zero();
  Vec4 zero_tangent = vec4_zero();

  vkr_write_vertex(verts, 0, vec3_new(-hw, -hh, 0.0f), normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, 1, vec3_new(hw, -hh, 0.0f), normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, 2, vec3_new(hw, hh, 0.0f), normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, 3, vec3_new(-hw, hh, 0.0f), normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  uint32_t indices[6] = {0, 1, 2, 0, 2, 3};

  vkr_geometry_system_generate_tangents(&system->allocator, verts, 4, indices,
                                        6);

  VkrGeometryConfig config = {0};
  config.vertex_size = sizeof(VkrVertex3d);
  config.vertex_count = 4;
  config.vertices = verts;
  config.index_size = sizeof(uint32_t);
  config.index_count = 6;
  config.indices = indices;
  config.center = vec3_zero();
  config.min_extents = vec3_new(-hw, -hh, 0.0f);
  config.max_extents = vec3_new(hw, hh, 0.0f);
  string_format(config.name, sizeof(config.name), "Default Plane");

  return vkr_geometry_system_create(system, &config, false_v, out_error);
}

/**
 * @brief Creates a default 2D plane (2x2 by default) using the
 * POSITION2_TEXCOORD layout. Vertex format: [x, y, u, v].
 * @param system The geometry system to create the default 2D plane in
 * @param width The width of the plane
 * @param height The height of the plane
 * @param out_error The error output. May be null.
 * @return Returns a VkrGeometryHandle to the created geometry. Returns an
 * invalid handle on error (check out_error).
 */
vkr_internal VkrGeometryHandle vkr_geometry_system_create_default_plane2d(
    VkrGeometrySystem *system, float32_t width, float32_t height,
    VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrVertex2d verts[4] = {0};
  verts[0].position.x = 0.0f; // 0    3
  verts[0].position.y = 0.0f; //
  verts[0].texcoord.x = 0.0f; //
  verts[0].texcoord.y = 0.0f; // 2    1

  verts[1].position.y = height;
  verts[1].position.x = width;
  verts[1].texcoord.x = 1.0f;
  verts[1].texcoord.y = 1.0f;

  verts[2].position.x = 0.0f;
  verts[2].position.y = height;
  verts[2].texcoord.x = 0.0f;
  verts[2].texcoord.y = 1.0f;

  verts[3].position.x = width;
  verts[3].position.y = 0.0;
  verts[3].texcoord.x = 1.0f;
  verts[3].texcoord.y = 0.0f;

  // counter-clockwise winding order
  uint32_t indices[6] = {2, 1, 0, 3, 0, 1};

  VkrGeometryConfig config = {0};
  config.vertex_size = sizeof(VkrVertex2d);
  config.vertex_count = 4;
  config.vertices = verts;
  config.index_size = sizeof(uint32_t);
  config.index_count = 6;
  config.indices = indices;
  config.center = vec3_zero();
  config.min_extents = vec3_new(-width, -height, 0.0f);
  config.max_extents = vec3_new(width, height, 0.0f);
  string_format(config.name, sizeof(config.name), "Default Plane 2D");

  return vkr_geometry_system_create(system, &config, false_v, out_error);
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
  system->config = *config;
  system->max_geometries =
      (config->max_geometries > 0) ? config->max_geometries : 1024;

  system->geometries =
      array_create_VkrGeometry(&system->allocator, system->max_geometries);
  for (uint32_t geometry = 0; geometry < system->max_geometries; geometry++) {
    VkrGeometry init = {0};
    init.pipeline_id = VKR_INVALID_ID;
    array_set_VkrGeometry(&system->geometries, geometry, init);
  }
  system->free_ids =
      array_create_uint32_t(&system->allocator, system->max_geometries);
  system->free_count = 0;

  system->geometry_by_name = vkr_hash_table_create_VkrGeometryEntry(
      &system->allocator, (uint64_t)system->max_geometries * 2);

  system->generation_counter = 1;

  system->default_geometry =
      vkr_geometry_system_create_default_cube(system, out_error);
  if (system->default_geometry.id == 0) {
    log_error("Failed to create default cube");
    return false_v;
  }

  system->default_plane =
      vkr_geometry_system_create_default_plane(system, 10.0f, 10.0f, out_error);
  if (system->default_plane.id == 0) {
    log_error("Failed to create default plane");
    return false_v;
  }

  system->default_plane2d =
      vkr_geometry_system_create_default_plane2d(system, 2.0f, 2.0f, out_error);
  if (system->default_plane2d.id == 0) {
    log_error("Failed to create default plane 2D");
    return false_v;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

void vkr_geometry_system_shutdown(VkrGeometrySystem *system) {
  if (!system)
    return;

  for (uint32_t i = 0; i < system->geometries.length; ++i) {
    VkrGeometry *geometry = array_get_VkrGeometry(&system->geometries, i);
    if (geometry->vertex_buffer.handle) {
      vkr_vertex_buffer_destroy(system->renderer, &geometry->vertex_buffer);
      geometry->vertex_buffer.handle = NULL;
    }
    if (geometry->index_buffer.handle) {
      vkr_index_buffer_destroy(system->renderer, &geometry->index_buffer);
      geometry->index_buffer.handle = NULL;
    }
    if (geometry->opaque_index_buffer.handle) {
      vkr_index_buffer_destroy(system->renderer,
                               &geometry->opaque_index_buffer);
      geometry->opaque_index_buffer.handle = NULL;
    }
  }

  array_destroy_VkrGeometry(&system->geometries);
  array_destroy_uint32_t(&system->free_ids);

  if (system->arena) {
    arena_destroy(system->arena);
    system->arena = NULL;
  }

  MemZero(system, sizeof(VkrGeometrySystem));
}

vkr_internal VkrGeometryHandle geometry_creation_failure(
    VkrGeometrySystem *system, VkrGeometry *geom, VkrGeometryHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(geom != NULL, "Geometry is NULL");

  if (geom->vertex_buffer.handle) {
    vkr_vertex_buffer_destroy(system->renderer, &geom->vertex_buffer);
    geom->vertex_buffer.handle = NULL;
  }

  if (geom->index_buffer.handle) {
    vkr_index_buffer_destroy(system->renderer, &geom->index_buffer);
    geom->index_buffer.handle = NULL;
  }
  if (geom->opaque_index_buffer.handle) {
    vkr_index_buffer_destroy(system->renderer, &geom->opaque_index_buffer);
    geom->opaque_index_buffer.handle = NULL;
  }

  uint32_t slot = (handle.id > 0) ? (handle.id - 1) : 0;
  geom->id = 0;
  geom->generation = 0;
  assert_log(system->free_count < system->free_ids.length,
             "Geometry free list overflow");
  system->free_ids.data[system->free_count++] = slot;

  return (VkrGeometryHandle){0};
}

VkrGeometryHandle vkr_geometry_system_create(VkrGeometrySystem *system,
                                             const VkrGeometryConfig *config,
                                             bool8_t auto_release,
                                             VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrGeometryHandle handle = (VkrGeometryHandle){0};
  if (!config || config->vertex_size == 0 || config->vertex_count == 0 ||
      config->vertices == NULL) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return handle;
  }
  if (config->index_size == 0 || config->index_count == 0 ||
      config->indices == NULL) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return handle;
  }

  VkrGeometry *geom = geometry_acquire_slot(system, &handle);
  if (!geom) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return handle;
  }

  geom->vertex_size = config->vertex_size;
  geom->vertex_count = config->vertex_count;
  geom->index_size = config->index_size;
  geom->index_count = config->index_count;
  geom->center = config->center;
  geom->min_extents = config->min_extents;
  geom->max_extents = config->max_extents;

  if (config->name[0] != '\0') {
    string_copy(geom->name, config->name);
  } else {
    string_format(geom->name, sizeof(geom->name), "geometry_%u", handle.id);
  }

  if (config->material_name[0] != '\0') {
    string_copy(geom->material_name, config->material_name);
  } else {
    geom->material_name[0] = '\0';
  }

  String8 debug_name = {0};
  uint64_t geom_name_length = string_length(geom->name);
  if (geom_name_length > 0) {
    debug_name = string8_create((uint8_t *)geom->name, geom_name_length);
  } else {
    debug_name = string8_lit("geometry");
  }

  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  geom->vertex_buffer = vkr_vertex_buffer_create(
      system->renderer, config->vertices, geom->vertex_size, geom->vertex_count,
      VKR_VERTEX_INPUT_RATE_VERTEX, debug_name, &err);
  if (err != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to create vertex buffer for '%s'", geom->name);
    *out_error = err;
    return geometry_creation_failure(system, geom, handle);
  }

  VkrIndexType index_type = (config->index_size == sizeof(uint16_t))
                                ? VKR_INDEX_TYPE_UINT16
                                : VKR_INDEX_TYPE_UINT32;
  geom->index_buffer =
      vkr_index_buffer_create(system->renderer, config->indices, index_type,
                              geom->index_count, debug_name, &err);
  if (err != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to create index buffer for '%s'", geom->name);
    *out_error = err;
    return geometry_creation_failure(system, geom, handle);
  }

  const char *stable_name = geom->name;
  VkrGeometryEntry life_entry = {.id = (handle.id - 1),
                                 .ref_count = 1,
                                 .auto_release = auto_release,
                                 .name = stable_name};
  vkr_hash_table_insert_VkrGeometryEntry(&system->geometry_by_name, stable_name,
                                         life_entry);

  *out_error = VKR_RENDERER_ERROR_NONE;
  return handle;
}

uint32_t vkr_geometry_system_create_batch(
    VkrGeometrySystem *system, const VkrGeometryConfig *configs, uint32_t count,
    bool8_t auto_release, VkrGeometryHandle *out_handles,
    VkrRendererError *out_errors) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(configs != NULL, "Configs is NULL");
  assert_log(count > 0, "Count must be > 0");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  for (uint32_t i = 0; i < count; ++i) {
    out_handles[i] = VKR_GEOMETRY_HANDLE_INVALID;
    out_errors[i] = VKR_RENDERER_ERROR_UNKNOWN;
  }

  VkrGeometry **geometry_slots =
      vkr_allocator_alloc(&system->allocator, sizeof(VkrGeometry *) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  bool8_t *slot_reserved =
      vkr_allocator_alloc(&system->allocator, sizeof(bool8_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *vertex_request_indices =
      vkr_allocator_alloc(&system->allocator, sizeof(uint32_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *index_request_indices =
      vkr_allocator_alloc(&system->allocator, sizeof(uint32_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrBufferDescription *buffer_descriptions = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrBufferDescription) * count * 2,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrBufferUploadPayload *buffer_uploads = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrBufferUploadPayload) * count * 2,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrBufferBatchCreateRequest *buffer_requests = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrBufferBatchCreateRequest) * count * 2,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrBufferHandle *buffer_handles = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrBufferHandle) * count * 2,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrRendererError *buffer_errors = vkr_allocator_alloc(
      &system->allocator, sizeof(VkrRendererError) * count * 2,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!geometry_slots || !slot_reserved || !vertex_request_indices ||
      !index_request_indices || !buffer_descriptions || !buffer_uploads ||
      !buffer_requests || !buffer_handles || !buffer_errors) {
    for (uint32_t i = 0; i < count; ++i) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return 0;
  }

  MemZero(slot_reserved, sizeof(bool8_t) * count);
  uint32_t request_count = 0;
  for (uint32_t i = 0; i < count; ++i) {
    vertex_request_indices[i] = UINT32_MAX;
    index_request_indices[i] = UINT32_MAX;

    const VkrGeometryConfig *config = &configs[i];
    if (!config || config->vertex_size == 0 || config->vertex_count == 0 ||
        !config->vertices || config->index_size == 0 || config->index_count == 0 ||
        !config->indices) {
      out_errors[i] = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      continue;
    }

    VkrGeometryHandle handle = VKR_GEOMETRY_HANDLE_INVALID;
    VkrGeometry *geometry = geometry_acquire_slot(system, &handle);
    if (!geometry) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      continue;
    }

    geometry->vertex_size = config->vertex_size;
    geometry->vertex_count = config->vertex_count;
    geometry->index_size = config->index_size;
    geometry->index_count = config->index_count;
    geometry->center = config->center;
    geometry->min_extents = config->min_extents;
    geometry->max_extents = config->max_extents;

    if (config->name[0] != '\0') {
      string_copy(geometry->name, config->name);
    } else {
      string_format(geometry->name, sizeof(geometry->name), "geometry_%u",
                    handle.id);
    }
    if (config->material_name[0] != '\0') {
      string_copy(geometry->material_name, config->material_name);
    } else {
      geometry->material_name[0] = '\0';
    }

    VkrBufferTypeFlags buffer_type = bitset8_create();
    bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

    const uint64_t vertex_size_bytes =
        (uint64_t)config->vertex_size * (uint64_t)config->vertex_count;
    const uint64_t index_size_bytes =
        (uint64_t)config->index_size * (uint64_t)config->index_count;

    buffer_descriptions[request_count] = (VkrBufferDescription){
        .size = vertex_size_bytes,
        .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_VERTEX_BUFFER |
                                                  VKR_BUFFER_USAGE_TRANSFER_DST |
                                                  VKR_BUFFER_USAGE_TRANSFER_SRC),
        .memory_properties = vkr_memory_property_flags_from_bits(
            VKR_MEMORY_PROPERTY_DEVICE_LOCAL),
        .buffer_type = buffer_type,
        .bind_on_create = true_v,
    };
    buffer_uploads[request_count] = (VkrBufferUploadPayload){
        .data = config->vertices,
        .size = vertex_size_bytes,
        .offset = 0,
    };
    buffer_requests[request_count] = (VkrBufferBatchCreateRequest){
        .description = &buffer_descriptions[request_count],
        .upload = &buffer_uploads[request_count],
    };
    vertex_request_indices[i] = request_count;
    request_count++;

    buffer_descriptions[request_count] = (VkrBufferDescription){
        .size = index_size_bytes,
        .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_INDEX_BUFFER |
                                                  VKR_BUFFER_USAGE_TRANSFER_DST |
                                                  VKR_BUFFER_USAGE_TRANSFER_SRC),
        .memory_properties = vkr_memory_property_flags_from_bits(
            VKR_MEMORY_PROPERTY_DEVICE_LOCAL),
        .buffer_type = buffer_type,
        .bind_on_create = true_v,
    };
    buffer_uploads[request_count] = (VkrBufferUploadPayload){
        .data = config->indices,
        .size = index_size_bytes,
        .offset = 0,
    };
    buffer_requests[request_count] = (VkrBufferBatchCreateRequest){
        .description = &buffer_descriptions[request_count],
        .upload = &buffer_uploads[request_count],
    };
    index_request_indices[i] = request_count;
    request_count++;

    out_handles[i] = handle;
    geometry_slots[i] = geometry;
    slot_reserved[i] = true_v;
  }

  if (request_count > 0) {
    vkr_renderer_create_buffer_batch(system->renderer, buffer_requests,
                                     request_count, buffer_handles,
                                     buffer_errors);
  }

  uint32_t created = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (!slot_reserved[i]) {
      continue;
    }

    VkrGeometry *geometry = geometry_slots[i];
    const uint32_t vertex_request_index = vertex_request_indices[i];
    const uint32_t index_request_index = index_request_indices[i];
    const bool8_t vertex_ok =
        vertex_request_index != UINT32_MAX &&
        buffer_errors[vertex_request_index] == VKR_RENDERER_ERROR_NONE &&
        buffer_handles[vertex_request_index] != NULL;
    const bool8_t index_ok =
        index_request_index != UINT32_MAX &&
        buffer_errors[index_request_index] == VKR_RENDERER_ERROR_NONE &&
        buffer_handles[index_request_index] != NULL;

    if (!vertex_ok || !index_ok) {
      if (vertex_request_index != UINT32_MAX &&
          buffer_handles[vertex_request_index] != NULL) {
        vkr_renderer_destroy_buffer(system->renderer,
                                    buffer_handles[vertex_request_index]);
      }
      if (index_request_index != UINT32_MAX &&
          buffer_handles[index_request_index] != NULL) {
        vkr_renderer_destroy_buffer(system->renderer,
                                    buffer_handles[index_request_index]);
      }

      VkrRendererError first_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      if (vertex_request_index != UINT32_MAX &&
          buffer_errors[vertex_request_index] != VKR_RENDERER_ERROR_NONE) {
        first_error = buffer_errors[vertex_request_index];
      } else if (index_request_index != UINT32_MAX &&
                 buffer_errors[index_request_index] != VKR_RENDERER_ERROR_NONE) {
        first_error = buffer_errors[index_request_index];
      }
      out_errors[i] = first_error;
      out_handles[i] = geometry_creation_failure(system, geometry, out_handles[i]);
      continue;
    }

    String8 debug_name =
        string8_create((uint8_t *)geometry->name, string_length(geometry->name));
    geometry->vertex_buffer = (VkrVertexBuffer){
        .handle = buffer_handles[vertex_request_index],
        .stride = geometry->vertex_size,
        .vertex_count = geometry->vertex_count,
        .input_rate = VKR_VERTEX_INPUT_RATE_VERTEX,
        .is_dynamic = false_v,
        .debug_name = debug_name,
        .size_bytes = buffer_descriptions[vertex_request_index].size,
    };
    geometry->index_buffer = (VkrIndexBuffer){
        .handle = buffer_handles[index_request_index],
        .type = (geometry->index_size == sizeof(uint16_t))
                    ? VKR_INDEX_TYPE_UINT16
                    : VKR_INDEX_TYPE_UINT32,
        .index_count = geometry->index_count,
        .is_dynamic = false_v,
        .debug_name = debug_name,
        .size_bytes = buffer_descriptions[index_request_index].size,
    };

    const char *stable_name = geometry->name;
    VkrGeometryEntry life_entry = {
        .id = (out_handles[i].id - 1),
        .ref_count = 1,
        .auto_release = auto_release,
        .name = stable_name,
    };
    if (!vkr_hash_table_insert_VkrGeometryEntry(&system->geometry_by_name,
                                                stable_name, life_entry)) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      out_handles[i] = geometry_creation_failure(system, geometry, out_handles[i]);
      continue;
    }

    out_errors[i] = VKR_RENDERER_ERROR_NONE;
    created++;
  }

  return created;
}

VkrGeometryHandle
vkr_geometry_system_create_cube(VkrGeometrySystem *system, float32_t width,
                                float32_t height, float32_t depth,
                                const char *name, VkrRendererError *out_error) {
  return vkr_geometry_system_create_box(system, vec3_zero(), width, height,
                                        depth, false_v, name, out_error);
}

vkr_internal void vkr_geometry_write_box_vertices(VkrVertex3d *verts,
                                                  Vec3 center, float32_t hw,
                                                  float32_t hh, float32_t hd) {
  Vec4 zero_color = vec4_zero();
  Vec4 zero_tangent = vec4_zero();

  float32_t cx = center.x;
  float32_t cy = center.y;
  float32_t cz = center.z;

  Vec3 front_normal = vec3_new(0.0f, 0.0f, 1.0f);
  Vec3 back_normal = vec3_new(0.0f, 0.0f, -1.0f);
  Vec3 left_normal = vec3_new(-1.0f, 0.0f, 0.0f);
  Vec3 right_normal = vec3_new(1.0f, 0.0f, 0.0f);
  Vec3 top_normal = vec3_new(0.0f, 1.0f, 0.0f);
  Vec3 bottom_normal = vec3_new(0.0f, -1.0f, 0.0f);

  uint32_t w = 0;
  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy - hh, cz + hd),
                   front_normal, vec2_new(0.0f, 0.0f), zero_color,
                   zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy - hh, cz + hd),
                   front_normal, vec2_new(1.0f, 0.0f), zero_color,
                   zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy + hh, cz + hd),
                   front_normal, vec2_new(1.0f, 1.0f), zero_color,
                   zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy + hh, cz + hd),
                   front_normal, vec2_new(0.0f, 1.0f), zero_color,
                   zero_tangent);

  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy - hh, cz - hd), back_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy - hh, cz - hd), back_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy + hh, cz - hd), back_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy + hh, cz - hd), back_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);

  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy - hh, cz - hd), left_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy - hh, cz + hd), left_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy + hh, cz + hd), left_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy + hh, cz - hd), left_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy - hh, cz - hd),
                   right_normal, vec2_new(0.0f, 0.0f), zero_color,
                   zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy - hh, cz + hd),
                   right_normal, vec2_new(1.0f, 0.0f), zero_color,
                   zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy + hh, cz + hd),
                   right_normal, vec2_new(1.0f, 1.0f), zero_color,
                   zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy + hh, cz - hd),
                   right_normal, vec2_new(0.0f, 1.0f), zero_color,
                   zero_tangent);

  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy + hh, cz + hd), top_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy + hh, cz + hd), top_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy + hh, cz - hd), top_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy + hh, cz - hd), top_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy - hh, cz - hd),
                   bottom_normal, vec2_new(0.0f, 0.0f), zero_color,
                   zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy - hh, cz - hd),
                   bottom_normal, vec2_new(1.0f, 0.0f), zero_color,
                   zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx + hw, cy - hh, cz + hd),
                   bottom_normal, vec2_new(1.0f, 1.0f), zero_color,
                   zero_tangent);
  vkr_write_vertex(verts, w++, vec3_new(cx - hw, cy - hh, cz + hd),
                   bottom_normal, vec2_new(0.0f, 1.0f), zero_color,
                   zero_tangent);
}

VkrGeometryHandle
vkr_geometry_system_create_box(VkrGeometrySystem *system, Vec3 center,
                               float32_t width, float32_t height,
                               float32_t depth, bool8_t auto_release,
                               const char *name, VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(name != NULL, "Name is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  float32_t hw = width * 0.5f;
  float32_t hh = height * 0.5f;
  float32_t hd = depth * 0.5f;

  VkrVertex3d verts[24] = {0};
  vkr_geometry_write_box_vertices(verts, center, hw, hh, hd);

  uint32_t indices[36] = {
      0,  1,  2,  2,  3,  0,  4,  7,  6,  6,  5,  4,  8,  9,  10, 10, 11, 8,
      12, 15, 14, 14, 13, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20,
  };

  vkr_geometry_system_generate_tangents(&system->allocator, verts, 24, indices,
                                        36);

  VkrGeometryConfig config = {0};
  config.vertex_size = sizeof(VkrVertex3d);
  config.vertex_count = 24;
  config.vertices = verts;
  config.index_size = sizeof(uint32_t);
  config.index_count = 36;
  config.indices = indices;
  config.center = center;
  config.min_extents = vec3_new(center.x - hw, center.y - hh, center.z - hd);
  config.max_extents = vec3_new(center.x + hw, center.y + hh, center.z + hd);
  string_format(config.name, sizeof(config.name), "%s", name);

  return vkr_geometry_system_create(system, &config, auto_release, out_error);
}

vkr_internal uint32_t vkr_geometry_clamp_segments(uint32_t segments) {
  return (segments < 3) ? 3 : segments;
}

vkr_internal uint32_t vkr_geometry_clamp_lat_segments(uint32_t segments) {
  return (segments < 2) ? 2 : segments;
}

VkrGeometryHandle vkr_geometry_system_create_cylinder(
    VkrGeometrySystem *system, float32_t radius, float32_t height,
    uint32_t segments, Vec3 axis, Vec3 origin, bool8_t cap_bottom,
    bool8_t cap_top, const char *name, VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(name != NULL, "Name is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (radius <= 0.0f || height <= 0.0f) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return (VkrGeometryHandle){0};
  }

  segments = vkr_geometry_clamp_segments(segments);
  uint32_t ring = segments + 1;
  uint32_t side_vert_count = ring * 2;
  uint32_t cap_vert_count =
      (cap_bottom ? (ring + 1) : 0) + (cap_top ? (ring + 1) : 0);
  uint32_t vertex_count = side_vert_count + cap_vert_count;
  uint32_t index_count = segments * 6;
  if (cap_bottom) {
    index_count += segments * 3;
  }
  if (cap_top) {
    index_count += segments * 3;
  }

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&system->allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  VkrVertex3d *verts =
      vkr_allocator_alloc(&system->allocator, sizeof(*verts) * vertex_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices =
      vkr_allocator_alloc(&system->allocator, sizeof(*indices) * index_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!verts || !indices) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  Vec4 zero_color = vec4_zero();
  Vec4 zero_tangent = vec4_zero();

  uint32_t v = 0;
  uint32_t i = 0;
  for (uint32_t s = 0; s <= segments; ++s) {
    float32_t u = (float32_t)s / (float32_t)segments;
    float32_t angle = u * (VKR_PI * 2.0f);
    float32_t x = vkr_cos_f32(angle) * radius;
    float32_t y = vkr_sin_f32(angle) * radius;
    Vec3 normal = vec3_normalize(vec3_new(x, y, 0.0f));

    vkr_write_vertex(verts, v++, vec3_new(x, y, 0.0f), normal,
                     vec2_new(u, 0.0f), zero_color, zero_tangent);
    vkr_write_vertex(verts, v++, vec3_new(x, y, height), normal,
                     vec2_new(u, 1.0f), zero_color, zero_tangent);
  }

  for (uint32_t s = 0; s < segments; ++s) {
    uint32_t base = s * 2;
    uint32_t next = base + 2;
    indices[i++] = base;
    indices[i++] = base + 1;
    indices[i++] = next + 1;
    indices[i++] = base;
    indices[i++] = next + 1;
    indices[i++] = next;
  }

  if (cap_bottom) {
    uint32_t center_index = v;
    vkr_write_vertex(verts, v++, vec3_zero(), vec3_new(0.0f, 0.0f, -1.0f),
                     vec2_new(0.5f, 0.5f), zero_color, zero_tangent);
    for (uint32_t s = 0; s <= segments; ++s) {
      float32_t u = (float32_t)s / (float32_t)segments;
      float32_t angle = u * (VKR_PI * 2.0f);
      float32_t x = vkr_cos_f32(angle) * radius;
      float32_t y = vkr_sin_f32(angle) * radius;
      Vec2 uv =
          vec2_new((x / (radius * 2.0f)) + 0.5f, (y / (radius * 2.0f)) + 0.5f);
      vkr_write_vertex(verts, v++, vec3_new(x, y, 0.0f),
                       vec3_new(0.0f, 0.0f, -1.0f), uv, zero_color,
                       zero_tangent);
    }

    uint32_t ring_start = center_index + 1;
    for (uint32_t s = 0; s < segments; ++s) {
      indices[i++] = center_index;
      indices[i++] = ring_start + s + 1;
      indices[i++] = ring_start + s;
    }
  }

  if (cap_top) {
    uint32_t center_index = v;
    vkr_write_vertex(verts, v++, vec3_new(0.0f, 0.0f, height),
                     vec3_new(0.0f, 0.0f, 1.0f), vec2_new(0.5f, 0.5f),
                     zero_color, zero_tangent);
    for (uint32_t s = 0; s <= segments; ++s) {
      float32_t u = (float32_t)s / (float32_t)segments;
      float32_t angle = u * (VKR_PI * 2.0f);
      float32_t x = vkr_cos_f32(angle) * radius;
      float32_t y = vkr_sin_f32(angle) * radius;
      Vec2 uv =
          vec2_new((x / (radius * 2.0f)) + 0.5f, (y / (radius * 2.0f)) + 0.5f);
      vkr_write_vertex(verts, v++, vec3_new(x, y, height),
                       vec3_new(0.0f, 0.0f, 1.0f), uv, zero_color,
                       zero_tangent);
    }

    uint32_t ring_start = center_index + 1;
    for (uint32_t s = 0; s < segments; ++s) {
      indices[i++] = center_index;
      indices[i++] = ring_start + s;
      indices[i++] = ring_start + s + 1;
    }
  }

  VkrQuat rotation = vkr_geometry_rotation_from_axis(axis);
  vkr_geometry_apply_transform(verts, vertex_count, rotation, origin);
  vkr_geometry_system_generate_tangents(&system->allocator, verts, vertex_count,
                                        indices, index_count);

  Vec3 min = vec3_zero();
  Vec3 max = vec3_zero();
  Vec3 center = vec3_zero();
  vkr_geometry_compute_bounds(verts, vertex_count, &min, &max, &center);

  VkrGeometryConfig config = {0};
  config.vertex_size = sizeof(VkrVertex3d);
  config.vertex_count = vertex_count;
  config.vertices = verts;
  config.index_size = sizeof(uint32_t);
  config.index_count = index_count;
  config.indices = indices;
  config.center = center;
  config.min_extents = min;
  config.max_extents = max;
  string_format(config.name, sizeof(config.name), "%s", name);

  VkrGeometryHandle handle =
      vkr_geometry_system_create(system, &config, true_v, out_error);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return handle;
}

VkrGeometryHandle
vkr_geometry_system_create_cone(VkrGeometrySystem *system, float32_t radius,
                                float32_t height, uint32_t segments, Vec3 axis,
                                Vec3 origin, bool8_t cap_base, const char *name,
                                VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(name != NULL, "Name is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (radius <= 0.0f || height <= 0.0f) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return (VkrGeometryHandle){0};
  }

  segments = vkr_geometry_clamp_segments(segments);
  uint32_t ring = segments + 1;
  uint32_t side_vert_count = ring * 2;
  uint32_t cap_vert_count = cap_base ? (ring + 1) : 0;
  uint32_t vertex_count = side_vert_count + cap_vert_count;
  uint32_t index_count = segments * 6 + (cap_base ? segments * 3 : 0);

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&system->allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  VkrVertex3d *verts =
      vkr_allocator_alloc(&system->allocator, sizeof(*verts) * vertex_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices =
      vkr_allocator_alloc(&system->allocator, sizeof(*indices) * index_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!verts || !indices) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  Vec4 zero_color = vec4_zero();
  Vec4 zero_tangent = vec4_zero();
  float32_t slope = radius / height;

  uint32_t v = 0;
  uint32_t i = 0;
  for (uint32_t s = 0; s <= segments; ++s) {
    float32_t u = (float32_t)s / (float32_t)segments;
    float32_t angle = u * (VKR_PI * 2.0f);
    float32_t x = vkr_cos_f32(angle) * radius;
    float32_t y = vkr_sin_f32(angle) * radius;
    Vec3 normal =
        vec3_normalize(vec3_new(vkr_cos_f32(angle), vkr_sin_f32(angle), slope));

    vkr_write_vertex(verts, v++, vec3_new(x, y, 0.0f), normal,
                     vec2_new(u, 0.0f), zero_color, zero_tangent);
    vkr_write_vertex(verts, v++, vec3_new(0.0f, 0.0f, height), normal,
                     vec2_new(u, 1.0f), zero_color, zero_tangent);
  }

  for (uint32_t s = 0; s < segments; ++s) {
    uint32_t base = s * 2;
    uint32_t next = base + 2;
    indices[i++] = base;
    indices[i++] = base + 1;
    indices[i++] = next + 1;
    indices[i++] = base;
    indices[i++] = next + 1;
    indices[i++] = next;
  }

  if (cap_base) {
    uint32_t center_index = v;
    vkr_write_vertex(verts, v++, vec3_zero(), vec3_new(0.0f, 0.0f, -1.0f),
                     vec2_new(0.5f, 0.5f), zero_color, zero_tangent);
    for (uint32_t s = 0; s <= segments; ++s) {
      float32_t u = (float32_t)s / (float32_t)segments;
      float32_t angle = u * (VKR_PI * 2.0f);
      float32_t x = vkr_cos_f32(angle) * radius;
      float32_t y = vkr_sin_f32(angle) * radius;
      Vec2 uv =
          vec2_new((x / (radius * 2.0f)) + 0.5f, (y / (radius * 2.0f)) + 0.5f);
      vkr_write_vertex(verts, v++, vec3_new(x, y, 0.0f),
                       vec3_new(0.0f, 0.0f, -1.0f), uv, zero_color,
                       zero_tangent);
    }

    uint32_t ring_start = center_index + 1;
    for (uint32_t s = 0; s < segments; ++s) {
      indices[i++] = center_index;
      indices[i++] = ring_start + s + 1;
      indices[i++] = ring_start + s;
    }
  }

  VkrQuat rotation = vkr_geometry_rotation_from_axis(axis);
  vkr_geometry_apply_transform(verts, vertex_count, rotation, origin);
  vkr_geometry_system_generate_tangents(&system->allocator, verts, vertex_count,
                                        indices, index_count);

  Vec3 min = vec3_zero();
  Vec3 max = vec3_zero();
  Vec3 center = vec3_zero();
  vkr_geometry_compute_bounds(verts, vertex_count, &min, &max, &center);

  VkrGeometryConfig config = {0};
  config.vertex_size = sizeof(VkrVertex3d);
  config.vertex_count = vertex_count;
  config.vertices = verts;
  config.index_size = sizeof(uint32_t);
  config.index_count = index_count;
  config.indices = indices;
  config.center = center;
  config.min_extents = min;
  config.max_extents = max;
  string_format(config.name, sizeof(config.name), "%s", name);

  VkrGeometryHandle handle =
      vkr_geometry_system_create(system, &config, true_v, out_error);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return handle;
}

VkrGeometryHandle vkr_geometry_system_create_torus(
    VkrGeometrySystem *system, float32_t major_radius, float32_t minor_radius,
    uint32_t major_segments, uint32_t minor_segments, Vec3 axis, Vec3 origin,
    const char *name, VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(name != NULL, "Name is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (major_radius <= 0.0f || minor_radius <= 0.0f) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return (VkrGeometryHandle){0};
  }

  major_segments = vkr_geometry_clamp_segments(major_segments);
  minor_segments = vkr_geometry_clamp_segments(minor_segments);

  uint32_t ring = minor_segments + 1;
  uint32_t vertex_count = (major_segments + 1) * ring;
  uint32_t index_count = major_segments * minor_segments * 6;

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&system->allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  VkrVertex3d *verts =
      vkr_allocator_alloc(&system->allocator, sizeof(*verts) * vertex_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices =
      vkr_allocator_alloc(&system->allocator, sizeof(*indices) * index_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!verts || !indices) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  Vec4 zero_color = vec4_zero();
  Vec4 zero_tangent = vec4_zero();

  uint32_t v = 0;
  for (uint32_t s = 0; s <= major_segments; ++s) {
    float32_t u = (float32_t)s / (float32_t)major_segments;
    float32_t major_angle = u * (VKR_PI * 2.0f);
    float32_t cos_u = vkr_cos_f32(major_angle);
    float32_t sin_u = vkr_sin_f32(major_angle);
    for (uint32_t t = 0; t <= minor_segments; ++t) {
      float32_t v_coord = (float32_t)t / (float32_t)minor_segments;
      float32_t minor_angle = v_coord * (VKR_PI * 2.0f);
      float32_t cos_v = vkr_cos_f32(minor_angle);
      float32_t sin_v = vkr_sin_f32(minor_angle);

      float32_t ring_radius = major_radius + (minor_radius * cos_v);
      Vec3 position = vec3_new(ring_radius * cos_u, ring_radius * sin_u,
                               minor_radius * sin_v);
      Vec3 normal =
          vec3_normalize(vec3_new(cos_v * cos_u, cos_v * sin_u, sin_v));
      vkr_write_vertex(verts, v++, position, normal, vec2_new(u, v_coord),
                       zero_color, zero_tangent);
    }
  }

  uint32_t i = 0;
  for (uint32_t s = 0; s < major_segments; ++s) {
    for (uint32_t t = 0; t < minor_segments; ++t) {
      uint32_t base = s * ring + t;
      uint32_t next = (s + 1) * ring + t;
      indices[i++] = base;
      indices[i++] = next;
      indices[i++] = next + 1;
      indices[i++] = base;
      indices[i++] = next + 1;
      indices[i++] = base + 1;
    }
  }

  VkrQuat rotation = vkr_geometry_rotation_from_axis(axis);
  vkr_geometry_apply_transform(verts, vertex_count, rotation, origin);
  vkr_geometry_system_generate_tangents(&system->allocator, verts, vertex_count,
                                        indices, index_count);

  Vec3 min = vec3_zero();
  Vec3 max = vec3_zero();
  Vec3 center = vec3_zero();
  vkr_geometry_compute_bounds(verts, vertex_count, &min, &max, &center);

  VkrGeometryConfig config = {0};
  config.vertex_size = sizeof(VkrVertex3d);
  config.vertex_count = vertex_count;
  config.vertices = verts;
  config.index_size = sizeof(uint32_t);
  config.index_count = index_count;
  config.indices = indices;
  config.center = center;
  config.min_extents = min;
  config.max_extents = max;
  string_format(config.name, sizeof(config.name), "%s", name);

  VkrGeometryHandle handle =
      vkr_geometry_system_create(system, &config, true_v, out_error);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return handle;
}

VkrGeometryHandle vkr_geometry_system_create_sphere(
    VkrGeometrySystem *system, float32_t radius, uint32_t latitude_segments,
    uint32_t longitude_segments, Vec3 axis, Vec3 origin, const char *name,
    VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(name != NULL, "Name is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (radius <= 0.0f) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return (VkrGeometryHandle){0};
  }

  latitude_segments = vkr_geometry_clamp_lat_segments(latitude_segments);
  longitude_segments = vkr_geometry_clamp_segments(longitude_segments);

  uint32_t ring = longitude_segments + 1;
  uint32_t vertex_count = (latitude_segments + 1) * ring;
  uint32_t index_count = latitude_segments * longitude_segments * 6;

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&system->allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  VkrVertex3d *verts =
      vkr_allocator_alloc(&system->allocator, sizeof(*verts) * vertex_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices =
      vkr_allocator_alloc(&system->allocator, sizeof(*indices) * index_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!verts || !indices) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  Vec4 zero_color = vec4_zero();
  Vec4 zero_tangent = vec4_zero();

  uint32_t v = 0;
  for (uint32_t lat = 0; lat <= latitude_segments; ++lat) {
    float32_t v_coord = (float32_t)lat / (float32_t)latitude_segments;
    float32_t phi = v_coord * VKR_PI;
    float32_t sin_phi = vkr_sin_f32(phi);
    float32_t cos_phi = vkr_cos_f32(phi);

    for (uint32_t lon = 0; lon <= longitude_segments; ++lon) {
      float32_t u = (float32_t)lon / (float32_t)longitude_segments;
      float32_t theta = u * (VKR_PI * 2.0f);
      float32_t sin_theta = vkr_sin_f32(theta);
      float32_t cos_theta = vkr_cos_f32(theta);

      Vec3 normal = vec3_new(sin_phi * cos_theta, cos_phi, sin_phi * sin_theta);
      Vec3 position = vec3_scale(normal, radius);
      vkr_write_vertex(verts, v++, position, normal,
                       vec2_new(u, 1.0f - v_coord), zero_color, zero_tangent);
    }
  }

  uint32_t i = 0;
  for (uint32_t lat = 0; lat < latitude_segments; ++lat) {
    for (uint32_t lon = 0; lon < longitude_segments; ++lon) {
      uint32_t base = lat * ring + lon;
      uint32_t next = (lat + 1) * ring + lon;
      indices[i++] = base;
      indices[i++] = next;
      indices[i++] = next + 1;
      indices[i++] = base;
      indices[i++] = next + 1;
      indices[i++] = base + 1;
    }
  }

  VkrQuat rotation = vkr_geometry_rotation_from_axis(axis);
  vkr_geometry_apply_transform(verts, vertex_count, rotation, origin);
  vkr_geometry_system_generate_tangents(&system->allocator, verts, vertex_count,
                                        indices, index_count);

  Vec3 min = vec3_zero();
  Vec3 max = vec3_zero();
  Vec3 center = vec3_zero();
  vkr_geometry_compute_bounds(verts, vertex_count, &min, &max, &center);

  VkrGeometryConfig config = {0};
  config.vertex_size = sizeof(VkrVertex3d);
  config.vertex_count = vertex_count;
  config.vertices = verts;
  config.index_size = sizeof(uint32_t);
  config.index_count = index_count;
  config.indices = indices;
  config.center = center;
  config.min_extents = min;
  config.max_extents = max;
  string_format(config.name, sizeof(config.name), "%s", name);

  VkrGeometryHandle handle =
      vkr_geometry_system_create(system, &config, true_v, out_error);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return handle;
}

VkrGeometryHandle vkr_geometry_system_create_arrow(
    VkrGeometrySystem *system, float32_t shaft_length, float32_t shaft_radius,
    float32_t head_length, float32_t head_radius, uint32_t segments, Vec3 axis,
    Vec3 origin, const char *name, VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(name != NULL, "Name is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (shaft_length <= 0.0f || shaft_radius <= 0.0f || head_length <= 0.0f ||
      head_radius <= 0.0f) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return (VkrGeometryHandle){0};
  }

  segments = vkr_geometry_clamp_segments(segments);
  uint32_t ring = segments + 1;
  uint32_t vertex_count = ring * 4;
  uint32_t index_count = segments * 12;

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&system->allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  VkrVertex3d *verts =
      vkr_allocator_alloc(&system->allocator, sizeof(*verts) * vertex_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices =
      vkr_allocator_alloc(&system->allocator, sizeof(*indices) * index_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!verts || !indices) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrGeometryHandle){0};
  }

  Vec4 zero_color = vec4_zero();
  Vec4 zero_tangent = vec4_zero();
  float32_t cone_slope = head_radius / head_length;

  uint32_t v = 0;
  uint32_t i = 0;
  for (uint32_t s = 0; s <= segments; ++s) {
    float32_t u = (float32_t)s / (float32_t)segments;
    float32_t angle = u * (VKR_PI * 2.0f);
    float32_t cos_a = vkr_cos_f32(angle);
    float32_t sin_a = vkr_sin_f32(angle);
    float32_t x = cos_a * shaft_radius;
    float32_t y = sin_a * shaft_radius;
    Vec3 normal = vec3_normalize(vec3_new(cos_a, sin_a, 0.0f));

    vkr_write_vertex(verts, v++, vec3_new(x, y, 0.0f), normal,
                     vec2_new(u, 0.0f), zero_color, zero_tangent);
    vkr_write_vertex(verts, v++, vec3_new(x, y, shaft_length), normal,
                     vec2_new(u, 1.0f), zero_color, zero_tangent);
  }

  for (uint32_t s = 0; s < segments; ++s) {
    uint32_t base = s * 2;
    uint32_t next = base + 2;
    indices[i++] = base;
    indices[i++] = base + 1;
    indices[i++] = next + 1;
    indices[i++] = base;
    indices[i++] = next + 1;
    indices[i++] = next;
  }

  uint32_t cone_offset = v;
  for (uint32_t s = 0; s <= segments; ++s) {
    float32_t u = (float32_t)s / (float32_t)segments;
    float32_t angle = u * (VKR_PI * 2.0f);
    float32_t cos_a = vkr_cos_f32(angle);
    float32_t sin_a = vkr_sin_f32(angle);
    float32_t x = cos_a * head_radius;
    float32_t y = sin_a * head_radius;
    Vec3 normal = vec3_normalize(vec3_new(cos_a, sin_a, cone_slope));

    vkr_write_vertex(verts, v++, vec3_new(x, y, shaft_length), normal,
                     vec2_new(u, 0.0f), zero_color, zero_tangent);
    vkr_write_vertex(verts, v++,
                     vec3_new(0.0f, 0.0f, shaft_length + head_length), normal,
                     vec2_new(u, 1.0f), zero_color, zero_tangent);
  }

  for (uint32_t s = 0; s < segments; ++s) {
    uint32_t base = cone_offset + s * 2;
    uint32_t next = base + 2;
    indices[i++] = base;
    indices[i++] = base + 1;
    indices[i++] = next + 1;
    indices[i++] = base;
    indices[i++] = next + 1;
    indices[i++] = next;
  }

  VkrQuat rotation = vkr_geometry_rotation_from_axis(axis);
  vkr_geometry_apply_transform(verts, vertex_count, rotation, origin);
  vkr_geometry_system_generate_tangents(&system->allocator, verts, vertex_count,
                                        indices, index_count);

  Vec3 min = vec3_zero();
  Vec3 max = vec3_zero();
  Vec3 center = vec3_zero();
  vkr_geometry_compute_bounds(verts, vertex_count, &min, &max, &center);

  VkrGeometryConfig config = {0};
  config.vertex_size = sizeof(VkrVertex3d);
  config.vertex_count = vertex_count;
  config.vertices = verts;
  config.index_size = sizeof(uint32_t);
  config.index_count = index_count;
  config.indices = indices;
  config.center = center;
  config.min_extents = min;
  config.max_extents = max;
  string_format(config.name, sizeof(config.name), "%s", name);

  VkrGeometryHandle handle =
      vkr_geometry_system_create(system, &config, true_v, out_error);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return handle;
}

void vkr_geometry_system_acquire(VkrGeometrySystem *system,
                                 VkrGeometryHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  VkrGeometry *geometry = vkr_geometry_from_handle(system, handle);
  if (!geometry)
    return;

  if (geometry->name[0] != '\0') {
    VkrGeometryEntry *lifetime_entry = vkr_hash_table_get_VkrGeometryEntry(
        &system->geometry_by_name, geometry->name);
    if (lifetime_entry) {
      lifetime_entry->ref_count++;
    }
  }
}

VkrGeometryHandle
vkr_geometry_system_acquire_by_name(VkrGeometrySystem *system, String8 name,
                                    bool8_t auto_release,
                                    VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  if (!name.str || name.length == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return VKR_GEOMETRY_HANDLE_INVALID;
  }

  char lookup_name[GEOMETRY_NAME_MAX_LENGTH] = {0};
  uint64_t copy_len = (name.length < (GEOMETRY_NAME_MAX_LENGTH - 1))
                          ? name.length
                          : (GEOMETRY_NAME_MAX_LENGTH - 1);
  MemCopy(lookup_name, name.str, (size_t)copy_len);
  lookup_name[copy_len] = '\0';

  VkrGeometryEntry *entry = vkr_hash_table_get_VkrGeometryEntry(
      &system->geometry_by_name, lookup_name);
  if (!entry) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return VKR_GEOMETRY_HANDLE_INVALID;
  }

  if (entry->id >= system->geometries.length) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return VKR_GEOMETRY_HANDLE_INVALID;
  }

  VkrGeometry *geometry = array_get_VkrGeometry(&system->geometries, entry->id);
  if (!geometry || geometry->id == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return VKR_GEOMETRY_HANDLE_INVALID;
  }

  if (entry->ref_count == 0) {
    entry->auto_release = auto_release;
  }
  entry->ref_count++;

  return (VkrGeometryHandle){.id = geometry->id,
                             .generation = geometry->generation};
}

void vkr_geometry_system_release(VkrGeometrySystem *system,
                                 VkrGeometryHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  VkrGeometry *geometry = vkr_geometry_from_handle(system, handle);
  if (!geometry)
    return;

  VkrGeometryEntry *lifetime_entry = NULL;
  if (geometry->name[0] != '\0') {
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
    if (geometry->vertex_buffer.handle) {
      vkr_vertex_buffer_destroy(system->renderer, &geometry->vertex_buffer);
      geometry->vertex_buffer.handle = NULL;
    }
    if (geometry->index_buffer.handle) {
      vkr_index_buffer_destroy(system->renderer, &geometry->index_buffer);
      geometry->index_buffer.handle = NULL;
    }
    if (geometry->opaque_index_buffer.handle) {
      vkr_index_buffer_destroy(system->renderer,
                               &geometry->opaque_index_buffer);
      geometry->opaque_index_buffer.handle = NULL;
    }

    if (geometry->name[0] != '\0') {
      vkr_hash_table_remove_VkrGeometryEntry(&system->geometry_by_name,
                                             geometry->name);
      geometry->name[0] = '\0';
    }
    geometry->material_name[0] = '\0';
    geometry->vertex_count = 0;
    geometry->index_count = 0;
    geometry->opaque_index_count = 0;
    geometry->vertex_size = 0;
    geometry->index_size = 0;
    geometry->id = 0;
    geometry->generation = 0;

    uint32_t idx = handle.id - 1;
    assert_log(system->free_count < system->free_ids.length,
               "free_ids overflow");
    system->free_ids.data[system->free_count++] = idx;
  }
}

VkrGeometry *vkr_geometry_system_get_by_handle(VkrGeometrySystem *system,
                                               VkrGeometryHandle handle) {
  return vkr_geometry_from_handle(system, handle);
}

void vkr_geometry_system_render(VkrRendererFrontendHandle renderer,
                                VkrGeometrySystem *system,
                                VkrGeometryHandle handle,
                                uint32_t instance_count) {
  vkr_geometry_system_render_instanced_range(renderer, system, handle,
                                             UINT32_MAX, 0, 0, instance_count,
                                             0);
}

void vkr_geometry_system_render_instanced(VkrRendererFrontendHandle renderer,
                                          VkrGeometrySystem *system,
                                          VkrGeometryHandle handle,
                                          uint32_t instance_count,
                                          uint32_t first_instance) {
  vkr_geometry_system_render_instanced_range(
      renderer, system, handle, UINT32_MAX, 0, 0, instance_count,
      first_instance);
}

void vkr_geometry_system_render_instanced_range(VkrRendererFrontendHandle renderer,
                                                VkrGeometrySystem *system,
                                                VkrGeometryHandle handle,
                                                uint32_t index_count,
                                                uint32_t first_index,
                                                int32_t vertex_offset,
                                                uint32_t instance_count,
                                                uint32_t first_instance) {
  vkr_geometry_system_render_instanced_range_with_index_buffer(
      renderer, system, handle, NULL, index_count, first_index, vertex_offset,
      instance_count, first_instance);
}

void vkr_geometry_system_render_indirect(VkrRendererFrontendHandle renderer,
                                         VkrGeometrySystem *system,
                                         VkrGeometryHandle handle,
                                         VkrBufferHandle indirect_buffer,
                                         uint64_t offset, uint32_t draw_count,
                                         uint32_t stride) {
  vkr_geometry_system_render_indirect_with_index_buffer(
      renderer, system, handle, NULL, indirect_buffer, offset, draw_count,
      stride);
}

void vkr_geometry_system_render_instanced_range_with_index_buffer(
    VkrRendererFrontendHandle renderer, VkrGeometrySystem *system,
    VkrGeometryHandle handle, const VkrIndexBuffer *index_buffer,
    uint32_t index_count, uint32_t first_index, int32_t vertex_offset,
    uint32_t instance_count, uint32_t first_instance) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");
  assert_log(instance_count > 0, "Instance count must be > 0");

  VkrGeometry *geometry = vkr_geometry_from_handle(system, handle);
  if (!geometry)
    return;

  const VkrIndexBuffer *resolved_index =
      index_buffer ? index_buffer : &geometry->index_buffer;

  if (!geometry->vertex_buffer.handle || !resolved_index->handle)
    return;

  VkrVertexBufferBinding vbb = {
      .buffer = geometry->vertex_buffer.handle,
      .binding = 0,
      .offset = 0,
  };
  vkr_renderer_bind_vertex_buffer(renderer, &vbb);

  VkrIndexBufferBinding ibb = {
      .buffer = resolved_index->handle,
      .type = resolved_index->type,
      .offset = 0,
  };
  vkr_renderer_bind_index_buffer(renderer, &ibb);

  uint32_t draw_index_count =
      (index_count != UINT32_MAX) ? index_count : resolved_index->index_count;
  vkr_renderer_draw_indexed(renderer, draw_index_count, instance_count,
                            first_index, vertex_offset, first_instance);
}

void vkr_geometry_system_render_indirect_with_index_buffer(
    VkrRendererFrontendHandle renderer, VkrGeometrySystem *system,
    VkrGeometryHandle handle, const VkrIndexBuffer *index_buffer,
    VkrBufferHandle indirect_buffer, uint64_t offset, uint32_t draw_count,
    uint32_t stride) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");
  assert_log(indirect_buffer != NULL, "Indirect buffer is NULL");
  assert_log(draw_count > 0, "Draw count must be > 0");
  assert_log(stride > 0, "Stride must be > 0");

  VkrGeometry *geometry = vkr_geometry_from_handle(system, handle);
  if (!geometry)
    return;

  const VkrIndexBuffer *resolved_index =
      index_buffer ? index_buffer : &geometry->index_buffer;

  if (!geometry->vertex_buffer.handle || !resolved_index->handle)
    return;

  VkrVertexBufferBinding vbb = {
      .buffer = geometry->vertex_buffer.handle,
      .binding = 0,
      .offset = 0,
  };
  vkr_renderer_bind_vertex_buffer(renderer, &vbb);

  VkrIndexBufferBinding ibb = {
      .buffer = resolved_index->handle,
      .type = resolved_index->type,
      .offset = 0,
  };
  vkr_renderer_bind_index_buffer(renderer, &ibb);

  vkr_renderer_draw_indexed_indirect(renderer, indirect_buffer, offset,
                                     draw_count, stride);
}

void vkr_geometry_system_generate_tangents(VkrAllocator *allocator,
                                           VkrVertex3d *verts,
                                           uint32_t vertex_count,
                                           const uint32_t *indices,
                                           uint32_t index_count) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(verts != NULL, "Verts is NULL");
  assert_log(vertex_count > 0, "Vertex count must be > 0");
  assert_log(indices != NULL, "Indices is NULL");
  assert_log(index_count > 0, "Index count must be > 0");

  Vec3 *tangent_accumulators = (Vec3 *)vkr_allocator_alloc(
      allocator, vertex_count * sizeof(Vec3), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert_log(tangent_accumulators != NULL,
             "Failed to allocate tangent accumulators");

  float32_t *handedness_accumulators = (float32_t *)vkr_allocator_alloc(
      allocator, vertex_count * sizeof(float32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert_log(handedness_accumulators != NULL,
             "Failed to allocate handedness accumulators");

  for (uint32_t i = 0; i < vertex_count; ++i) {
    tangent_accumulators[i] = vec3_zero();
    handedness_accumulators[i] = 0.0f;
  }

  for (uint32_t i = 0; i + 2 < index_count; i += 3) {
    uint32_t i0 = indices[i];
    uint32_t i1 = indices[i + 1];
    uint32_t i2 = indices[i + 2];
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
      continue;

    VkrVertex3d *v0 = &verts[i0];
    VkrVertex3d *v1 = &verts[i1];
    VkrVertex3d *v2 = &verts[i2];

    Vec3 p0 = vkr_vertex_unpack_vec3(v0->position);
    Vec3 p1 = vkr_vertex_unpack_vec3(v1->position);
    Vec3 p2 = vkr_vertex_unpack_vec3(v2->position);
    Vec3 e1 = vec3_sub(p1, p0);
    Vec3 e2 = vec3_sub(p2, p0);

    float32_t deltaU1 = v1->texcoord.u - v0->texcoord.u;
    float32_t deltaV1 = v1->texcoord.v - v0->texcoord.v;
    float32_t deltaU2 = v2->texcoord.u - v0->texcoord.u;
    float32_t deltaV2 = v2->texcoord.v - v0->texcoord.v;

    float32_t dividend = (deltaU1 * deltaV2 - deltaU2 * deltaV1);
    if (vkr_abs_f32(dividend) < VKR_FLOAT_EPSILON) {
      continue;
    }
    float32_t fc = 1.0f / dividend;

    Vec3 tangent = vec3_new(fc * (deltaV2 * e1.x - deltaV1 * e2.x),
                            fc * (deltaV2 * e1.y - deltaV1 * e2.y),
                            fc * (deltaV2 * e1.z - deltaV1 * e2.z));
    tangent = vec3_normalize(tangent);

    float32_t handedness =
        ((deltaV1 * deltaU2 - deltaV2 * deltaU1) < 0.0f) ? -1.0f : 1.0f;

    tangent_accumulators[i0] = vec3_add(tangent_accumulators[i0], tangent);
    tangent_accumulators[i1] = vec3_add(tangent_accumulators[i1], tangent);
    tangent_accumulators[i2] = vec3_add(tangent_accumulators[i2], tangent);
    handedness_accumulators[i0] += handedness;
    handedness_accumulators[i1] += handedness;
    handedness_accumulators[i2] += handedness;
  }

  for (uint32_t i = 0; i < vertex_count; ++i) {
    Vec3 normal = vkr_vertex_unpack_vec3(verts[i].normal);
    Vec3 tangent = tangent_accumulators[i];

    float32_t tangent_len_sq = vec3_length_squared(tangent);
    if (tangent_len_sq < VKR_FLOAT_EPSILON * VKR_FLOAT_EPSILON) {
      if (vkr_abs_f32(normal.x) > 0.9f) {
        tangent = vec3_new(0.0f, 1.0f, 0.0f);
      } else {
        tangent = vec3_new(1.0f, 0.0f, 0.0f);
      }
    }

    float32_t dot_nt = vec3_dot(normal, tangent);
    tangent = vec3_sub(tangent, vec3_scale(normal, dot_nt));
    tangent = vec3_normalize(tangent);

    float32_t handedness = (handedness_accumulators[i] >= 0.0f) ? 1.0f : -1.0f;
    verts[i].tangent = vec3_to_vec4(tangent, handedness);
  }

  vkr_allocator_free(allocator, tangent_accumulators,
                     vertex_count * sizeof(Vec3),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_free(allocator, handedness_accumulators,
                     vertex_count * sizeof(float32_t),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
}

static INLINE bool8_t vkr_vertex3d_equal(const VkrVertex3d *lhs,
                                         const VkrVertex3d *rhs) {
  const float32_t epsilon = VKR_FLOAT_EPSILON;
  return vec3_equal(vkr_vertex_unpack_vec3(lhs->position),
                    vkr_vertex_unpack_vec3(rhs->position), epsilon) &&
         vec3_equal(vkr_vertex_unpack_vec3(lhs->normal),
                    vkr_vertex_unpack_vec3(rhs->normal), epsilon) &&
         vec2_equal(lhs->texcoord, rhs->texcoord, epsilon) &&
         vec4_equal(lhs->colour, rhs->colour, epsilon) &&
         vec4_equal(lhs->tangent, rhs->tangent, epsilon);
}

// Simple spatial hash for vertex deduplication - O(n) instead of O(n²)
vkr_internal uint32_t vkr_vertex_hash(const VkrVertex3d *v) {
  // Quantize position to grid cells for hashing
  const float32_t scale = 1000.0f; // 0.001 unit precision
  int32_t px = (int32_t)(v->position.x * scale);
  int32_t py = (int32_t)(v->position.y * scale);
  int32_t pz = (int32_t)(v->position.z * scale);
  int32_t nx = (int32_t)(v->normal.x * 100.0f);
  int32_t ny = (int32_t)(v->normal.y * 100.0f);
  int32_t nz = (int32_t)(v->normal.z * 100.0f);
  int32_t tu = (int32_t)(v->texcoord.u * 10000.0f);
  int32_t tv = (int32_t)(v->texcoord.v * 10000.0f);

  // FNV-1a hash
  uint32_t hash = 2166136261u;
  hash ^= (uint32_t)px;
  hash *= 16777619u;
  hash ^= (uint32_t)py;
  hash *= 16777619u;
  hash ^= (uint32_t)pz;
  hash *= 16777619u;
  hash ^= (uint32_t)nx;
  hash *= 16777619u;
  hash ^= (uint32_t)ny;
  hash *= 16777619u;
  hash ^= (uint32_t)nz;
  hash *= 16777619u;
  hash ^= (uint32_t)tu;
  hash *= 16777619u;
  hash ^= (uint32_t)tv;
  hash *= 16777619u;
  return hash;
}

bool8_t vkr_geometry_system_deduplicate_vertices(
    VkrGeometrySystem *system, VkrAllocator *scratch_alloc,
    const VkrVertex3d *vertices, uint32_t vertex_count, uint32_t *indices,
    uint32_t index_count, VkrVertex3d **out_vertices,
    uint32_t *out_vertex_count) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(scratch_alloc != NULL, "Scratch allocator is NULL");
  assert_log(vertices != NULL, "Vertices are NULL");
  assert_log(indices != NULL, "Indices are NULL");
  assert_log(out_vertices != NULL, "Out vertices pointer is NULL");
  assert_log(out_vertex_count != NULL, "Out vertex count pointer is NULL");

  if (vertex_count == 0) {
    *out_vertices = NULL;
    *out_vertex_count = 0;
    return true_v;
  }

  // Use a hash table for O(n) deduplication instead of O(n²)
  // Hash table size should be ~2x vertex count for good performance
  uint32_t table_size = vertex_count * 2;
  if (table_size < 1024)
    table_size = 1024;

  // Each bucket stores: vertex index in unique array, or UINT32_MAX if empty
  uint32_t *hash_table = vkr_allocator_alloc(
      scratch_alloc, (uint64_t)table_size * sizeof(uint32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrVertex3d *unique = vkr_allocator_alloc(
      scratch_alloc, (uint64_t)vertex_count * sizeof(VkrVertex3d),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *remap = vkr_allocator_alloc(
      scratch_alloc, (uint64_t)vertex_count * sizeof(uint32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!hash_table || !unique || !remap) {
    log_error("GeometrySystem: failed to allocate dedup buffers");
    return false_v;
  }

  // Initialize hash table to empty
  for (uint32_t i = 0; i < table_size; ++i) {
    hash_table[i] = UINT32_MAX;
  }

  uint32_t unique_count = 0;
  for (uint32_t i = 0; i < vertex_count; ++i) {
    uint32_t hash = vkr_vertex_hash(&vertices[i]);
    uint32_t bucket = hash % table_size;

    // Linear probing to find matching vertex or empty slot
    bool8_t found = false;
    for (uint32_t probe = 0; probe < table_size; ++probe) {
      uint32_t idx = (bucket + probe) % table_size;

      if (hash_table[idx] == UINT32_MAX) {
        // Empty slot - add new unique vertex
        hash_table[idx] = unique_count;
        unique[unique_count] = vertices[i];
        remap[i] = unique_count;
        unique_count++;
        found = true;
        break;
      }

      // Check if this is a matching vertex
      uint32_t existing_idx = hash_table[idx];
      if (vkr_vertex3d_equal(&vertices[i], &unique[existing_idx])) {
        remap[i] = existing_idx;
        found = true;
        break;
      }
    }

    if (!found) {
      // Hash table full (shouldn't happen with 2x size)
      log_error("GeometrySystem: hash table overflow during dedup");
      return false_v;
    }
  }

  for (uint32_t i = 0; i < index_count; ++i) {
    uint32_t idx = indices[i];
    assert_log(idx < vertex_count, "Index out of bounds during dedup");
    indices[i] = remap[idx];
  }

  *out_vertices = unique;
  *out_vertex_count = unique_count;

  return true_v;
}

VkrGeometryHandle
vkr_geometry_system_get_default_geometry(VkrGeometrySystem *system) {
  assert_log(system != NULL, "Geometry system is NULL");
  return system->default_geometry;
}

VkrGeometryHandle
vkr_geometry_system_get_default_plane(VkrGeometrySystem *system) {
  assert_log(system != NULL, "Geometry system is NULL");
  return system->default_plane;
}

VkrGeometryHandle vkr_geometry_system_get_default_plane2d(

    VkrGeometrySystem *system) {
  assert_log(system != NULL, "Geometry system is NULL");
  return system->default_plane2d;
}
