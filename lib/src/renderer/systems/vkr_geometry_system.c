#include "renderer/systems/vkr_geometry_system.h"

#include "containers/str.h"
#include "math/vec.h"
#include "math/vkr_math.h"
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
  vertex->position = position;
  vertex->normal = normal;
  vertex->texcoord = texcoord;
  vertex->colour = color;
  vertex->tangent = tangent;
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

VkrGeometryHandle
vkr_geometry_system_create_cube(VkrGeometrySystem *system, float32_t width,
                                float32_t height, float32_t depth,
                                const char *name, VkrRendererError *out_error) {
  assert_log(system != NULL, "Geometry system is NULL");
  assert_log(name != NULL, "Name is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  // Calculate half-dimensions for centered cube
  float32_t hw = width * 0.5f;
  float32_t hh = height * 0.5f;
  float32_t hd = depth * 0.5f;

  // Zero vectors for color and tangent (will be computed later)
  Vec4 zero_color = vec4_zero();
  Vec4 zero_tangent = vec4_zero();

  VkrVertex3d verts[24] = {0};
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
  vkr_write_vertex(verts, w++, vec3_new(-hw, -hh, hd), front_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v1: Bottom-right corner
  // Position: (hw, -hh, hd)
  // Texcoord: (1, 0) - bottom-right of texture
  vkr_write_vertex(verts, w++, vec3_new(hw, -hh, hd), front_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v2: Top-right corner
  // Position: (hw, hh, hd)
  // Texcoord: (1, 1) - top-right of texture
  vkr_write_vertex(verts, w++, vec3_new(hw, hh, hd), front_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);

  // v3: Top-left corner
  // Position: (-hw, hh, hd)
  // Texcoord: (0, 1) - top-left of texture
  vkr_write_vertex(verts, w++, vec3_new(-hw, hh, hd), front_normal,
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
  vkr_write_vertex(verts, w++, vec3_new(-hw, -hh, -hd), back_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v5: Bottom-right corner
  // Position: (hw, -hh, -hd)
  // Texcoord: (0, 0) - bottom-left of texture (flipped horizontally)
  vkr_write_vertex(verts, w++, vec3_new(hw, -hh, -hd), back_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v6: Top-right corner
  // Position: (hw, hh, -hd)
  // Texcoord: (0, 1) - top-left of texture (flipped horizontally)
  vkr_write_vertex(verts, w++, vec3_new(hw, hh, -hd), back_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  // v7: Top-left corner
  // Position: (-hw, hh, -hd)
  // Texcoord: (1, 1) - top-right of texture (flipped horizontally)
  vkr_write_vertex(verts, w++, vec3_new(-hw, hh, -hd), back_normal,
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
  vkr_write_vertex(verts, w++, vec3_new(-hw, -hh, -hd), left_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v9: Bottom-front corner
  // Position: (-hw, -hh, hd)
  // Texcoord: (1, 0) - bottom-right of texture
  vkr_write_vertex(verts, w++, vec3_new(-hw, -hh, hd), left_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v10: Top-front corner
  // Position: (-hw, hh, hd)
  // Texcoord: (1, 1) - top-right of texture
  vkr_write_vertex(verts, w++, vec3_new(-hw, hh, hd), left_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);

  // v11: Top-back corner
  // Position: (-hw, hh, -hd)
  // Texcoord: (0, 1) - top-left of texture
  vkr_write_vertex(verts, w++, vec3_new(-hw, hh, -hd), left_normal,
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
  vkr_write_vertex(verts, w++, vec3_new(hw, -hh, -hd), right_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v13: Bottom-front corner
  // Position: (hw, -hh, hd)
  // Texcoord: (0, 0) - bottom-left of texture (flipped horizontally)
  vkr_write_vertex(verts, w++, vec3_new(hw, -hh, hd), right_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v14: Top-front corner
  // Position: (hw, hh, hd)
  // Texcoord: (0, 1) - top-left of texture (flipped horizontally)
  vkr_write_vertex(verts, w++, vec3_new(hw, hh, hd), right_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  // v15: Top-back corner
  // Position: (hw, hh, -hd)
  // Texcoord: (1, 1) - top-right of texture (flipped horizontally)
  vkr_write_vertex(verts, w++, vec3_new(hw, hh, -hd), right_normal,
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
  vkr_write_vertex(verts, w++, vec3_new(-hw, hh, hd), top_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v17: Front-right corner
  // Position: (hw, hh, hd)
  // Texcoord: (1, 0) - bottom-right of texture
  vkr_write_vertex(verts, w++, vec3_new(hw, hh, hd), top_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v18: Back-right corner
  // Position: (hw, hh, -hd)
  // Texcoord: (1, 1) - top-right of texture
  vkr_write_vertex(verts, w++, vec3_new(hw, hh, -hd), top_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);

  // v19: Back-left corner
  // Position: (-hw, hh, -hd)
  // Texcoord: (0, 1) - top-left of texture
  vkr_write_vertex(verts, w++, vec3_new(-hw, hh, -hd), top_normal,
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
  vkr_write_vertex(verts, w++, vec3_new(-hw, -hh, -hd), bottom_normal,
                   vec2_new(0.0f, 0.0f), zero_color, zero_tangent);

  // v21: Back-right corner
  // Position: (hw, -hh, -hd)
  // Texcoord: (1, 0) - bottom-right of texture
  vkr_write_vertex(verts, w++, vec3_new(hw, -hh, -hd), bottom_normal,
                   vec2_new(1.0f, 0.0f), zero_color, zero_tangent);

  // v22: Front-right corner
  // Position: (hw, -hh, hd)
  // Texcoord: (1, 1) - top-right of texture
  vkr_write_vertex(verts, w++, vec3_new(hw, -hh, hd), bottom_normal,
                   vec2_new(1.0f, 1.0f), zero_color, zero_tangent);

  // v23: Front-left corner
  // Position: (-hw, -hh, hd)
  // Texcoord: (0, 1) - top-left of texture
  vkr_write_vertex(verts, w++, vec3_new(-hw, -hh, hd), bottom_normal,
                   vec2_new(0.0f, 1.0f), zero_color, zero_tangent);

  // counter-clockwise winding order
  uint32_t indices[36] = {
      0,  1,  2,  2,  3,  0,  // Front
      4,  7,  6,  6,  5,  4,  // Back
      8,  9,  10, 10, 11, 8,  // Left
      12, 15, 14, 14, 13, 12, // Right
      16, 17, 18, 18, 19, 16, // Top
      20, 21, 22, 22, 23, 20  // Bottom
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
  config.center = vec3_zero();
  config.min_extents = vec3_new(-hw, -hh, -hd);
  config.max_extents = vec3_new(hw, hh, hd);
  string_format(config.name, sizeof(config.name), "%s", name);

  return vkr_geometry_system_create(system, &config, false_v, out_error);
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

    if (geometry->name[0] != '\0') {
      vkr_hash_table_remove_VkrGeometryEntry(&system->geometry_by_name,
                                             geometry->name);
      geometry->name[0] = '\0';
    }
    geometry->material_name[0] = '\0';
    geometry->vertex_count = 0;
    geometry->index_count = 0;
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
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");
  assert_log(instance_count > 0, "Instance count must be > 0");

  VkrGeometry *geometry = vkr_geometry_from_handle(system, handle);
  if (!geometry)
    return;

  if (!geometry->vertex_buffer.handle || !geometry->index_buffer.handle)
    return;

  VkrVertexBufferBinding vbb = {
      .buffer = geometry->vertex_buffer.handle,
      .binding = 0,
      .offset = 0,
  };
  vkr_renderer_bind_vertex_buffer(renderer, &vbb);

  VkrIndexBufferBinding ibb = {
      .buffer = geometry->index_buffer.handle,
      .type = geometry->index_buffer.type,
      .offset = 0,
  };
  vkr_renderer_bind_index_buffer(renderer, &ibb);

  vkr_renderer_draw_indexed(renderer, geometry->index_count, instance_count, 0,
                            0, 0);
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

    Vec3 e1 = vec3_sub(v1->position, v0->position);
    Vec3 e2 = vec3_sub(v2->position, v0->position);

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
    Vec3 normal = verts[i].normal;
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
  return vec3_equal(lhs->position, rhs->position, epsilon) &&
         vec3_equal(lhs->normal, rhs->normal, epsilon) &&
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
  hash ^= (uint32_t)px; hash *= 16777619u;
  hash ^= (uint32_t)py; hash *= 16777619u;
  hash ^= (uint32_t)pz; hash *= 16777619u;
  hash ^= (uint32_t)nx; hash *= 16777619u;
  hash ^= (uint32_t)ny; hash *= 16777619u;
  hash ^= (uint32_t)nz; hash *= 16777619u;
  hash ^= (uint32_t)tu; hash *= 16777619u;
  hash ^= (uint32_t)tv; hash *= 16777619u;
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
  if (table_size < 1024) table_size = 1024;

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
