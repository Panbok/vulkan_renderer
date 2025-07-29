#include "mesh.h"

// =============================================================================
// Mesh Creation Functions
// =============================================================================

Mesh mesh_create(Arena *arena, uint32_t vertex_count, uint32_t index_count) {
  assert_log(arena != NULL, "Arena cannot be NULL");
  assert_log(vertex_count > 0, "Vertex count must be > 0");
  assert_log(index_count > 0, "Index count must be > 0");

  Mesh mesh = {0};
  mesh.arena = arena;
  mesh.vertex_count = vertex_count;
  mesh.index_count = index_count;

  mesh.positions = array_create_Vec3(arena, vertex_count);
  mesh.normals = array_create_Vec3(arena, vertex_count);
  mesh.texcoords = array_create_Vec2(arena, vertex_count);
  mesh.colors = array_create_Vec3(arena, vertex_count);
  mesh.indices = array_create_uint32_t(arena, index_count);

  return mesh;
}

// =============================================================================
// SIMD-Optimized Mesh Operations
// =============================================================================

void mesh_transform_positions(Mesh *mesh, const Mat4 *transform_matrix) {
  assert_log(mesh != NULL, "Mesh cannot be NULL");
  assert_log(transform_matrix != NULL, "Transform matrix cannot be NULL");

  uint32_t simd_count = mesh->vertex_count / 4;
  uint32_t remainder = mesh->vertex_count % 4;

  SIMD_F32X4 row0 = simd_load_f32x4(&transform_matrix->elements[0]);
  SIMD_F32X4 row1 = simd_load_f32x4(&transform_matrix->elements[4]);
  SIMD_F32X4 row2 = simd_load_f32x4(&transform_matrix->elements[8]);
  SIMD_F32X4 row3 = simd_load_f32x4(&transform_matrix->elements[12]);

  for (uint32_t i = 0; i < simd_count; i++) {
    uint32_t base_idx = i * 4;

    for (uint32_t j = 0; j < 4; j++) {
      Vec3 *pos = array_get_Vec3(&mesh->positions, base_idx + j);
      SIMD_F32X4 position = *pos;

      SIMD_F32X4 result = simd_fma_f32x4(
          row3, simd_set1_f32x4(position.w),
          simd_fma_f32x4(
              row2, simd_set1_f32x4(position.z),
              simd_fma_f32x4(
                  row1, simd_set1_f32x4(position.y),
                  simd_mul_f32x4(row0, simd_set1_f32x4(position.x)))));

      *pos = result;
    }
  }

  for (uint32_t i = simd_count * 4; i < mesh->vertex_count; i++) {
    Vec3 *pos = array_get_Vec3(&mesh->positions, i);
    SIMD_F32X4 position = *pos;

    SIMD_F32X4 result = simd_fma_f32x4(
        row3, simd_set1_f32x4(position.w),
        simd_fma_f32x4(
            row2, simd_set1_f32x4(position.z),
            simd_fma_f32x4(row1, simd_set1_f32x4(position.y),
                           simd_mul_f32x4(row0, simd_set1_f32x4(position.x)))));

    *pos = result;
  }
}

void mesh_transform_normals(Mesh *mesh, const Mat4 *normal_matrix) {
  assert_log(mesh != NULL, "Mesh cannot be NULL");
  assert_log(normal_matrix != NULL, "Normal matrix cannot be NULL");

  SIMD_F32X4 row0 =
      simd_set_f32x4(normal_matrix->elements[0], normal_matrix->elements[4],
                     normal_matrix->elements[8], 0.0f);
  SIMD_F32X4 row1 =
      simd_set_f32x4(normal_matrix->elements[1], normal_matrix->elements[5],
                     normal_matrix->elements[9], 0.0f);
  SIMD_F32X4 row2 =
      simd_set_f32x4(normal_matrix->elements[2], normal_matrix->elements[6],
                     normal_matrix->elements[10], 0.0f);

  for (uint32_t i = 0; i < mesh->vertex_count; i++) {
    Vec3 *normal = array_get_Vec3(&mesh->normals, i);
    SIMD_F32X4 n = *normal;

    SIMD_F32X4 result = simd_add_f32x4(
        simd_add_f32x4(simd_mul_f32x4(row0, simd_set1_f32x4(n.x)),
                       simd_mul_f32x4(row1, simd_set1_f32x4(n.y))),
        simd_mul_f32x4(row2, simd_set1_f32x4(n.z)));

    *normal = vec3_normalize(result);
  }
}

void mesh_calculate_normals(Mesh *mesh) {
  assert_log(mesh != NULL, "Mesh cannot be NULL");
  assert_log(mesh->index_count % 3 == 0, "Index count must be divisible by 3");

  for (uint32_t i = 0; i < mesh->vertex_count; i++) {
    *array_get_Vec3(&mesh->normals, i) = vec3_zero();
  }

  for (uint32_t i = 0; i < mesh->index_count; i += 3) {
    uint32_t i0 = *array_get_uint32_t(&mesh->indices, i);
    uint32_t i1 = *array_get_uint32_t(&mesh->indices, i + 1);
    uint32_t i2 = *array_get_uint32_t(&mesh->indices, i + 2);

    Vec3 *v0 = array_get_Vec3(&mesh->positions, i0);
    Vec3 *v1 = array_get_Vec3(&mesh->positions, i1);
    Vec3 *v2 = array_get_Vec3(&mesh->positions, i2);

    Vec3 edge1 = vec3_sub(*v1, *v0);
    Vec3 edge2 = vec3_sub(*v2, *v0);
    Vec3 face_normal = vec3_cross(edge1, edge2);

    Vec3 *n0 = array_get_Vec3(&mesh->normals, i0);
    Vec3 *n1 = array_get_Vec3(&mesh->normals, i1);
    Vec3 *n2 = array_get_Vec3(&mesh->normals, i2);

    *n0 = vec3_add(*n0, face_normal);
    *n1 = vec3_add(*n1, face_normal);
    *n2 = vec3_add(*n2, face_normal);
  }

  for (uint32_t i = 0; i < mesh->vertex_count; i++) {
    Vec3 *normal = array_get_Vec3(&mesh->normals, i);
    *normal = vec3_normalize(*normal);
  }
}

void mesh_calculate_tangents(Mesh *mesh) {
  assert_log(mesh != NULL, "Mesh cannot be NULL");
  assert_log(mesh->index_count % 3 == 0, "Index count must be divisible by 3");

  // This is a simplified tangent calculation
  // For full implementation, you'd need tangent and bitangent arrays
  // For now, we'll generate tangents perpendicular to normals
  for (uint32_t i = 0; i < mesh->vertex_count; i++) {
    Vec3 *normal = array_get_Vec3(&mesh->normals, i);

    // Create a tangent perpendicular to the normal
    Vec3 up = vec3_new(0.0f, 1.0f, 0.0f);
    Vec3 tangent = vec3_cross(*normal, up);

    // If normal is parallel to up, use a different reference
    if (vec3_length_squared(tangent) < 0.01f) {
      up = vec3_new(1.0f, 0.0f, 0.0f);
      tangent = vec3_cross(*normal, up);
    }

    // Normalize tangent (could store in separate array if needed)
    tangent = vec3_normalize(tangent);
  }
}

void mesh_compute_aabb(const Mesh *mesh, Vec3 *aabb_min, Vec3 *aabb_max) {
  assert_log(mesh != NULL, "Mesh cannot be NULL");
  assert_log(aabb_min != NULL, "AABB min cannot be NULL");
  assert_log(aabb_max != NULL, "AABB max cannot be NULL");
  assert_log(mesh->vertex_count > 0, "Mesh must have vertices");

  *aabb_min = *array_get_Vec3(&mesh->positions, 0);
  *aabb_max = *array_get_Vec3(&mesh->positions, 0);

  SIMD_F32X4 min_vec = *aabb_min;
  SIMD_F32X4 max_vec = *aabb_max;

  for (uint32_t i = 1; i < mesh->vertex_count; i++) {
    SIMD_F32X4 pos = *array_get_Vec3(&mesh->positions, i);

    min_vec = simd_min_f32x4(min_vec, pos);
    max_vec = simd_max_f32x4(max_vec, pos);
  }

  *aabb_min = min_vec;
  *aabb_max = max_vec;
}

void mesh_compute_bounding_sphere(const Mesh *mesh, Vec3 *center,
                                  float32_t *radius) {
  assert_log(mesh != NULL, "Mesh cannot be NULL");
  assert_log(center != NULL, "Center cannot be NULL");
  assert_log(radius != NULL, "Radius cannot be NULL");
  assert_log(mesh->vertex_count > 0, "Mesh must have vertices");

  SIMD_F32X4 sum = vec3_zero();

  for (uint32_t i = 0; i < mesh->vertex_count; i++) {
    sum = simd_add_f32x4(sum, *array_get_Vec3(&mesh->positions, i));
  }

  *center = vec3_scale(sum, 1.0f / (float32_t)mesh->vertex_count);

  float32_t max_distance_sq = 0.0f;
  SIMD_F32X4 center_vec = *center;

  for (uint32_t i = 0; i < mesh->vertex_count; i++) {
    SIMD_F32X4 pos = *array_get_Vec3(&mesh->positions, i);
    SIMD_F32X4 diff = simd_sub_f32x4(pos, center_vec);

    float32_t distance_sq = simd_dot3_f32x4(diff, diff);
    if (distance_sq > max_distance_sq) {
      max_distance_sq = distance_sq;
    }
  }

  *radius = sqrt_f32(max_distance_sq);
}

// =============================================================================
// Mesh Utility Functions
// =============================================================================

bool32_t mesh_validate(const Mesh *mesh) {
  if (mesh == NULL)
    return false;
  if (mesh->vertex_count == 0)
    return false;
  if (mesh->index_count == 0)
    return false;
  if (array_is_null_Vec3(&mesh->positions))
    return false;
  if (array_is_null_Vec3(&mesh->normals))
    return false;
  if (array_is_null_Vec2(&mesh->texcoords))
    return false;
  if (array_is_null_Vec3(&mesh->colors))
    return false;
  if (array_is_null_uint32_t(&mesh->indices))
    return false;

  return true;
}

// =============================================================================
// Mesh Primitive Generation
// =============================================================================

Mesh mesh_generate_cube(Arena *arena, float32_t width, float32_t height,
                        float32_t depth) {
  assert_log(arena != NULL, "Arena cannot be NULL");

  Mesh mesh = mesh_create(arena, 24, 36); // 24 vertices, 36 indices (6 faces *
                                          // 4 vertices, 6 faces * 6 indices)

  float32_t half_width = width * 0.5f;
  float32_t half_height = height * 0.5f;
  float32_t half_depth = depth * 0.5f;

  Vec3 positions[24] = {
      // Front face (z = +half_depth)
      vec3_new(-half_width, -half_height, half_depth),
      vec3_new(half_width, -half_height, half_depth),
      vec3_new(half_width, half_height, half_depth),
      vec3_new(-half_width, half_height, half_depth),
      // Back face (z = -half_depth)
      vec3_new(-half_width, -half_height, -half_depth),
      vec3_new(half_width, -half_height, -half_depth),
      vec3_new(half_width, half_height, -half_depth),
      vec3_new(-half_width, half_height, -half_depth),
      // Left face (x = -half_width)
      vec3_new(-half_width, -half_height, -half_depth),
      vec3_new(-half_width, -half_height, half_depth),
      vec3_new(-half_width, half_height, half_depth),
      vec3_new(-half_width, half_height, -half_depth),
      // Right face (x = +half_width)
      vec3_new(half_width, -half_height, -half_depth),
      vec3_new(half_width, -half_height, half_depth),
      vec3_new(half_width, half_height, half_depth),
      vec3_new(half_width, half_height, -half_depth),
      // Top face (y = +half_height)
      vec3_new(-half_width, half_height, half_depth),
      vec3_new(half_width, half_height, half_depth),
      vec3_new(half_width, half_height, -half_depth),
      vec3_new(-half_width, half_height, -half_depth),
      // Bottom face (y = -half_height)
      vec3_new(-half_width, -half_height, -half_depth),
      vec3_new(half_width, -half_height, -half_depth),
      vec3_new(half_width, -half_height, half_depth),
      vec3_new(-half_width, -half_height, half_depth),
  };

  Vec3 normals[24] = {
      // Front face
      vec3_new(0.0f, 0.0f, 1.0f),
      vec3_new(0.0f, 0.0f, 1.0f),
      vec3_new(0.0f, 0.0f, 1.0f),
      vec3_new(0.0f, 0.0f, 1.0f),
      // Back face
      vec3_new(0.0f, 0.0f, -1.0f),
      vec3_new(0.0f, 0.0f, -1.0f),
      vec3_new(0.0f, 0.0f, -1.0f),
      vec3_new(0.0f, 0.0f, -1.0f),
      // Left face
      vec3_new(-1.0f, 0.0f, 0.0f),
      vec3_new(-1.0f, 0.0f, 0.0f),
      vec3_new(-1.0f, 0.0f, 0.0f),
      vec3_new(-1.0f, 0.0f, 0.0f),
      // Right face
      vec3_new(1.0f, 0.0f, 0.0f),
      vec3_new(1.0f, 0.0f, 0.0f),
      vec3_new(1.0f, 0.0f, 0.0f),
      vec3_new(1.0f, 0.0f, 0.0f),
      // Top face
      vec3_new(0.0f, 1.0f, 0.0f),
      vec3_new(0.0f, 1.0f, 0.0f),
      vec3_new(0.0f, 1.0f, 0.0f),
      vec3_new(0.0f, 1.0f, 0.0f),
      // Bottom face
      vec3_new(0.0f, -1.0f, 0.0f),
      vec3_new(0.0f, -1.0f, 0.0f),
      vec3_new(0.0f, -1.0f, 0.0f),
      vec3_new(0.0f, -1.0f, 0.0f),
  };

  Vec2 texcoords[24] = {
      // Front face
      vec2_new(0.0f, 0.0f),
      vec2_new(1.0f, 0.0f),
      vec2_new(1.0f, 1.0f),
      vec2_new(0.0f, 1.0f),
      // Back face
      vec2_new(1.0f, 0.0f),
      vec2_new(0.0f, 0.0f),
      vec2_new(0.0f, 1.0f),
      vec2_new(1.0f, 1.0f),
      // Left face
      vec2_new(0.0f, 0.0f),
      vec2_new(1.0f, 0.0f),
      vec2_new(1.0f, 1.0f),
      vec2_new(0.0f, 1.0f),
      // Right face
      vec2_new(1.0f, 0.0f),
      vec2_new(0.0f, 0.0f),
      vec2_new(0.0f, 1.0f),
      vec2_new(1.0f, 1.0f),
      // Top face
      vec2_new(0.0f, 1.0f),
      vec2_new(1.0f, 1.0f),
      vec2_new(1.0f, 0.0f),
      vec2_new(0.0f, 0.0f),
      // Bottom face
      vec2_new(0.0f, 0.0f),
      vec2_new(1.0f, 0.0f),
      vec2_new(1.0f, 1.0f),
      vec2_new(0.0f, 1.0f),
  };

  // Indices are in CCW order
  uint32_t indices[36] = {
      0,  1,  2,  2,  3,  0,  // Front
      4,  7,  6,  6,  5,  4,  // Back
      8,  9,  10, 10, 11, 8,  // Left
      12, 15, 14, 14, 13, 12, // Right
      16, 17, 18, 18, 19, 16, // Top
      20, 21, 22, 22, 23, 20  // Bottom
  };

  for (uint32_t i = 0; i < 24; i++) {
    *array_get_Vec3(&mesh.positions, i) = positions[i];
    *array_get_Vec3(&mesh.normals, i) = normals[i];
    *array_get_Vec2(&mesh.texcoords, i) = texcoords[i];
    *array_get_Vec3(&mesh.colors, i) =
        vec3_new(1.0f, 1.0f, 1.0f); // White color
  }

  for (uint32_t i = 0; i < 36; i++) {
    *array_get_uint32_t(&mesh.indices, i) = indices[i];
  }

  return mesh;
}

Mesh mesh_generate_sphere(Arena *arena, float32_t radius,
                          uint32_t longitude_segments,
                          uint32_t latitude_segments) {
  assert_log(arena != NULL, "Arena cannot be NULL");
  assert_log(longitude_segments >= 3, "Longitude segments must be >= 3");
  assert_log(latitude_segments >= 2, "Latitude segments must be >= 2");

  uint32_t vertex_count = (longitude_segments + 1) * (latitude_segments + 1);
  uint32_t index_count = longitude_segments * latitude_segments * 6;

  Mesh mesh = mesh_create(arena, vertex_count, index_count);

  uint32_t vertex_index = 0;
  for (uint32_t lat = 0; lat <= latitude_segments; lat++) {
    float32_t theta = (float32_t)lat * PI / (float32_t)latitude_segments;
    float32_t sin_theta = sin_f32(theta);
    float32_t cos_theta = cos_f32(theta);

    for (uint32_t lon = 0; lon <= longitude_segments; lon++) {
      float32_t phi =
          (float32_t)lon * 2.0f * PI / (float32_t)longitude_segments;
      float32_t sin_phi = sin_f32(phi);
      float32_t cos_phi = cos_f32(phi);

      Vec3 position = vec3_new(radius * sin_theta * cos_phi, radius * cos_theta,
                               radius * sin_theta * sin_phi);

      Vec3 normal = vec3_normalize(position);
      Vec2 texcoord = vec2_new((float32_t)lon / (float32_t)longitude_segments,
                               (float32_t)lat / (float32_t)latitude_segments);

      *array_get_Vec3(&mesh.positions, vertex_index) = position;
      *array_get_Vec3(&mesh.normals, vertex_index) = normal;
      *array_get_Vec2(&mesh.texcoords, vertex_index) = texcoord;
      *array_get_Vec3(&mesh.colors, vertex_index) =
          vec3_new(1.0f, 1.0f, 1.0f); // White color

      vertex_index++;
    }
  }

  uint32_t index_index = 0;
  for (uint32_t lat = 0; lat < latitude_segments; lat++) {
    for (uint32_t lon = 0; lon < longitude_segments; lon++) {
      uint32_t first = lat * (longitude_segments + 1) + lon;
      uint32_t second = first + longitude_segments + 1;

      *array_get_uint32_t(&mesh.indices, index_index++) = first;
      *array_get_uint32_t(&mesh.indices, index_index++) = second;
      *array_get_uint32_t(&mesh.indices, index_index++) = first + 1;

      *array_get_uint32_t(&mesh.indices, index_index++) = second;
      *array_get_uint32_t(&mesh.indices, index_index++) = second + 1;
      *array_get_uint32_t(&mesh.indices, index_index++) = first + 1;
    }
  }

  return mesh;
}

Mesh mesh_generate_plane(Arena *arena, float32_t width, float32_t height,
                         uint32_t width_segments, uint32_t height_segments) {
  assert_log(arena != NULL, "Arena cannot be NULL");
  assert_log(width_segments >= 1, "Width segments must be >= 1");
  assert_log(height_segments >= 1, "Height segments must be >= 1");

  uint32_t vertex_count = (width_segments + 1) * (height_segments + 1);
  uint32_t index_count = width_segments * height_segments * 6;

  Mesh mesh = mesh_create(arena, vertex_count, index_count);

  float32_t half_width = width * 0.5f;
  float32_t half_height = height * 0.5f;

  uint32_t vertex_index = 0;
  for (uint32_t y = 0; y <= height_segments; y++) {
    float32_t v = (float32_t)y / (float32_t)height_segments;
    float32_t pos_y = (v - 0.5f) * height;

    for (uint32_t x = 0; x <= width_segments; x++) {
      float32_t u = (float32_t)x / (float32_t)width_segments;
      float32_t pos_x = (u - 0.5f) * width;

      Vec3 position = vec3_new(pos_x, 0.0f, pos_y);
      Vec3 normal = vec3_new(0.0f, 1.0f, 0.0f);
      Vec2 texcoord = vec2_new(u, v);

      *array_get_Vec3(&mesh.positions, vertex_index) = position;
      *array_get_Vec3(&mesh.normals, vertex_index) = normal;
      *array_get_Vec2(&mesh.texcoords, vertex_index) = texcoord;
      *array_get_Vec3(&mesh.colors, vertex_index) =
          vec3_new(1.0f, 1.0f, 1.0f); // White color

      vertex_index++;
    }
  }

  uint32_t index_index = 0;
  for (uint32_t y = 0; y < height_segments; y++) {
    for (uint32_t x = 0; x < width_segments; x++) {
      uint32_t first = y * (width_segments + 1) + x;
      uint32_t second = first + width_segments + 1;

      *array_get_uint32_t(&mesh.indices, index_index++) = first;
      *array_get_uint32_t(&mesh.indices, index_index++) = second;
      *array_get_uint32_t(&mesh.indices, index_index++) = first + 1;

      *array_get_uint32_t(&mesh.indices, index_index++) = second;
      *array_get_uint32_t(&mesh.indices, index_index++) = second + 1;
      *array_get_uint32_t(&mesh.indices, index_index++) = first + 1;
    }
  }

  return mesh;
}
