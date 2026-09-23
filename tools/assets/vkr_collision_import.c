#include "assets/vkr_collision_import.h"
#include "assets/vkr_cgltf.h"
#include "assets/vkr_collision_hull.h"
#include "core/logger.h"
#include "math/mat.h"
#include <math.h>
#include <string.h>

static bool8_t collision_node_selected(const cgltf_data *data,
                                       const cgltf_node *node,
                                       uint32_t selected) {
  const cgltf_node *p = node;
  for (cgltf_size depth = 0; p && depth <= data->nodes_count;
       ++depth, p = p->parent) {
    if (selected != UINT32_MAX && p == data->nodes + selected) {
      return true_v;
    }
    if (selected == UINT32_MAX && data->scene) {
      for (cgltf_size r = 0; r < data->scene->nodes_count; ++r) {
        if (p == data->scene->nodes[r]) {
          return true_v;
        }
      }
    }
  }
  return selected == UINT32_MAX && !data->scene;
}

static const cgltf_accessor *
collision_positions(const cgltf_primitive *primitive) {
  for (cgltf_size a = 0; a < primitive->attributes_count; ++a) {
    if (primitive->attributes[a].type == cgltf_attribute_type_position) {
      return primitive->attributes[a].data;
    }
  }
  return NULL;
}

static bool8_t collision_accessor_supported(const cgltf_accessor *a) {
  return a && (!a->buffer_view || !a->buffer_view->has_meshopt_compression) &&
         (!a->is_sparse ||
          ((!a->sparse.indices_buffer_view ||
            !a->sparse.indices_buffer_view->has_meshopt_compression) &&
           (!a->sparse.values_buffer_view ||
            !a->sparse.values_buffer_view->has_meshopt_compression)));
}

bool8_t vkr_collision_import_gltf(VkrAllocator *result, VkrAllocator *scratch,
                                  String8 source_path, uint32_t selected,
                                  VkrCollisionKind kind,
                                  VkrCollisionGeometry *out,
                                  const char **error) {
  if (out) {
    *out = (VkrCollisionGeometry){0};
  }
  if (!result || !scratch || result == scratch || !out || !source_path.str ||
      !source_path.length || source_path.length >= 32768 ||
      memchr(source_path.str, 0, source_path.length) ||
      (kind != VKR_COLLISION_CONVEX_HULL &&
       kind != VKR_COLLISION_TRIANGLE_MESH)) {
    if (error) {
      *error = "Invalid collision import arguments";
    }
    return false_v;
  }
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    if (error) {
      *error = "Collision import requires scoped scratch";
    }
    return false_v;
  }
  bool8_t success = false_v;
  const char *message = "Unable to parse collision glTF";
  cgltf_data *data = NULL;
  cgltf_options options = {.file = vkr_cgltf_file_options()};
  String8 path = string8_duplicate(scratch, &source_path);
  if (!path.str || cgltf_parse_file(&options, string8_cstr(&path), &data) !=
                       cgltf_result_success) {
    goto cleanup;
  }
  message = "Unable to load or validate collision glTF";
  if (cgltf_load_buffers(&options, data, string8_cstr(&path)) !=
          cgltf_result_success ||
      cgltf_validate(data) != cgltf_result_success ||
      (selected != UINT32_MAX && selected >= data->nodes_count)) {
    goto cleanup;
  }
  for (cgltf_size n = 0; n < data->nodes_count; ++n) {
    const cgltf_node *p = data->nodes + n;
    cgltf_size depth = 0;
    for (; p && depth <= data->nodes_count; ++depth) {
      p = p->parent;
    }
    if (p) {
      message = "Collision source contains a hierarchy cycle";
      goto cleanup;
    }
  }
  uint64_t vertex_count = 0;
  uint64_t index_count = 0;
  // cgltf provides node storage for every counted node.
  assert_log(data->nodes_count == 0 || data->nodes,
             "Parsed glTF nodes need storage");
  for (cgltf_size n = 0; n < data->nodes_count; ++n) {
    const cgltf_node *node = data->nodes + n;
    if (!node->mesh || !collision_node_selected(data, node, selected)) {
      continue;
    }
    if (node->skin) {
      message =
          "Skinned collision requires bone colliders; source node has a skin";
      goto cleanup;
    }
    for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
      const cgltf_primitive *primitive = node->mesh->primitives + p;
      const cgltf_accessor *positions = collision_positions(primitive);
      if (primitive->type != cgltf_primitive_type_triangles ||
          primitive->has_draco_mesh_compression || primitive->targets_count ||
          !collision_accessor_supported(positions) ||
          positions->type != cgltf_type_vec3 ||
          (primitive->indices &&
           !collision_accessor_supported(primitive->indices))) {
        message =
            "Collision import requires uncompressed static triangle primitives";
        goto cleanup;
      }
      const uint64_t indices =
          primitive->indices ? primitive->indices->count : positions->count;
      vertex_count += positions->count;
      index_count += indices;
      if (indices % 3 || vertex_count > VKR_COLLISION_MAX_VERTICES ||
          index_count > VKR_COLLISION_MAX_INDICES) {
        message = "Collision source geometry exceeds capacity or has "
                  "incomplete triangles";
        goto cleanup;
      }
    }
  }
  if (vertex_count < 3 || index_count < 3) {
    message = "No collision triangles in selected source subtree";
    goto cleanup;
  }
  float32_t *positions = vkr_allocator_alloc(scratch, vertex_count * 12,
                                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices = vkr_allocator_alloc(scratch, index_count * 4,
                                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!positions || !indices) {
    message = "Collision source allocation failed";
    goto cleanup;
  }
  Mat4 inverse_root = mat4_identity();
  if (selected != UINT32_MAX) {
    Mat4 root;
    cgltf_node_transform_world(data->nodes + selected, root.elements);
    if (fabsf(mat4_determinant(root)) < 1.0e-12f) {
      message = "Selected collision node has a singular transform";
      goto cleanup;
    }
    inverse_root = mat4_inverse(root);
  }
  uint32_t vertex_cursor = 0, index_cursor = 0;
  for (cgltf_size n = 0; n < data->nodes_count; ++n) {
    const cgltf_node *node = data->nodes + n;
    if (!node->mesh || !collision_node_selected(data, node, selected)) {
      continue;
    }
    Mat4 transform;
    cgltf_node_transform_world(node, transform.elements);
    transform = mat4_mul(inverse_root, transform);
    const bool8_t reflected = mat4_determinant(transform) < 0;
    for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
      const cgltf_primitive *primitive = node->mesh->primitives + p;
      const cgltf_accessor *accessor = collision_positions(primitive);
      const uint32_t count = (uint32_t)accessor->count;
      const uint32_t triangle_indices =
          (uint32_t)(primitive->indices ? primitive->indices->count : count);
      if (cgltf_accessor_unpack_floats(accessor, positions + vertex_cursor * 3,
                                       (cgltf_size)count * 3) !=
          (cgltf_size)count * 3) {
        message = "Collision position accessor decode failed";
        goto cleanup;
      }
      for (uint32_t v = 0; v < count; ++v) {
        float32_t *point = positions + (vertex_cursor + v) * 3;
        const Vec4 world =
            mat4_mul_vec4(transform, (Vec4){point[0], point[1], point[2], 1});
        point[0] = world.x;
        point[1] = world.y;
        point[2] = world.z;
        if (!isfinite(world.x) || !isfinite(world.y) || !isfinite(world.z) ||
            fabsf(world.x) > 1e7f || fabsf(world.y) > 1e7f ||
            fabsf(world.z) > 1e7f) {
          message =
              "Collision transformed vertex exceeds supported coordinates";
          goto cleanup;
        }
      }
      for (uint32_t t = 0; t < triangle_indices; t += 3) {
        uint32_t tri[3];
        for (uint32_t k = 0; k < 3; ++k) {
          const cgltf_size index =
              primitive->indices
                  ? cgltf_accessor_read_index(primitive->indices, t + k)
                  : t + k;
          if (index >= count) {
            message = "Collision source triangle index out of bounds";
            goto cleanup;
          }
          tri[k] = vertex_cursor + (uint32_t)index;
        }
        const float32_t *a = positions + tri[0] * 3;
        const float32_t *b = positions + tri[1] * 3;
        const float32_t *c = positions + tri[2] * 3;
        const Vec3 ab = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 ac = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const Vec3 cross = vec3_cross(ab, ac);
        if (vec3_dot(cross, cross) <= 1e-20f) {
          continue;
        }
        indices[index_cursor++] = tri[0];
        indices[index_cursor++] = tri[reflected ? 2 : 1];
        indices[index_cursor++] = tri[reflected ? 1 : 2];
      }
      vertex_cursor += count;
    }
  }
  uint64_t fingerprint = UINT64_C(14695981039346656037);
  for (cgltf_size i = 0; i < data->json_size; ++i) {
    fingerprint =
        (fingerprint ^ (uint8_t)data->json[i]) * UINT64_C(1099511628211);
  }
  for (cgltf_size b = 0; b < data->buffers_count; ++b) {
    const uint8_t *buffer = data->buffers[b].data;
    for (cgltf_size i = 0; i < data->buffers[b].size; ++i) {
      fingerprint = (fingerprint ^ buffer[i]) * UINT64_C(1099511628211);
    }
  }
  VkrCollisionGeometry geometry = {.kind = kind,
                                   .positions = positions,
                                   .vertex_count = vertex_cursor,
                                   .indices = indices,
                                   .index_count = index_cursor,
                                   .source_fingerprint = fingerprint};
  if (kind == VKR_COLLISION_CONVEX_HULL) {
    float32_t *hull_positions =
        vkr_allocator_alloc(scratch, 256 * 12, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    uint32_t *hull_indices =
        vkr_allocator_alloc(scratch, 1536 * 4, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!hull_positions || !hull_indices ||
        !vkr_collision_build_hull(positions, vertex_cursor, hull_positions,
                                  hull_indices, &geometry.vertex_count,
                                  &geometry.index_count, &message)) {
      goto cleanup;
    }
    geometry.positions = hull_positions;
    geometry.indices = hull_indices;
  }
  if (!vkr_collision_geometry_validate(&geometry, &message)) {
    goto cleanup;
  }
  const uint64_t position_bytes = (uint64_t)geometry.vertex_count * 12;
  const uint64_t index_bytes = (uint64_t)geometry.index_count * 4;
  uint8_t *owned = vkr_allocator_alloc(result, position_bytes + index_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!owned) {
    message = "Collision result allocation failed";
    goto cleanup;
  }
  MemCopy(owned, geometry.positions, position_bytes);
  MemCopy(owned + position_bytes, geometry.indices, index_bytes);
  geometry.positions = (const float32_t *)owned;
  geometry.indices = (const uint32_t *)(owned + position_bytes);
  *out = geometry;
  success = true_v;
cleanup:
  if (data) {
    cgltf_free(data);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (error) {
    *error = success ? NULL : message;
  }
  return success;
}
