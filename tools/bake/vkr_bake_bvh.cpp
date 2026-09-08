#include "vkr_bake_bvh.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

struct VkrBakeBvhBuildContext {
  VkrBakeGeometry geometry;
  VkrBakeBvhNode *nodes;
  uint32_t node_capacity;
  uint32_t node_count;
};

struct VkrBakeBvhBin {
  VkrBakeAabb bounds;
  uint32_t count;
};

struct VkrBakeBvhSplit {
  uint32_t axis;
  uint32_t bin;
  float64_t cost;
  bool8_t found;
};

bool8_t vkr_bake_finite_vec2(Vec2 value) {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

bool8_t vkr_bake_finite_vec3(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool8_t vkr_bake_finite_vec4(Vec4 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z) && std::isfinite(value.w);
}

float32_t vkr_bake_component(Vec3 value, uint32_t axis) {
  return value.elements[axis];
}

VkrBakeAabb vkr_bake_aabb_empty(void) {
  const float32_t infinity = std::numeric_limits<float32_t>::infinity();
  return VkrBakeAabb{vec3_new(infinity, infinity, infinity),
                     vec3_new(-infinity, -infinity, -infinity)};
}

void vkr_bake_aabb_extend_point(VkrBakeAabb *bounds, Vec3 point) {
  bounds->min.x = Min(bounds->min.x, point.x);
  bounds->min.y = Min(bounds->min.y, point.y);
  bounds->min.z = Min(bounds->min.z, point.z);
  bounds->max.x = Max(bounds->max.x, point.x);
  bounds->max.y = Max(bounds->max.y, point.y);
  bounds->max.z = Max(bounds->max.z, point.z);
}

void vkr_bake_aabb_extend_bounds(VkrBakeAabb *bounds,
                                 const VkrBakeAabb *other) {
  vkr_bake_aabb_extend_point(bounds, other->min);
  vkr_bake_aabb_extend_point(bounds, other->max);
}

float64_t vkr_bake_aabb_surface_area(const VkrBakeAabb *bounds) {
  const float64_t x = Max((float64_t)bounds->max.x - bounds->min.x, 0.0);
  const float64_t y = Max((float64_t)bounds->max.y - bounds->min.y, 0.0);
  const float64_t z = Max((float64_t)bounds->max.z - bounds->min.z, 0.0);
  return 2.0 * (x * y + y * z + z * x);
}

Vec3 vkr_bake_triangle_centroid(const VkrBakeTriangle *triangle) {
  return vec3_new((float32_t)(((float64_t)triangle->vertex[0].position.x +
                               triangle->vertex[1].position.x +
                               triangle->vertex[2].position.x) /
                              3.0),
                  (float32_t)(((float64_t)triangle->vertex[0].position.y +
                               triangle->vertex[1].position.y +
                               triangle->vertex[2].position.y) /
                              3.0),
                  (float32_t)(((float64_t)triangle->vertex[0].position.z +
                               triangle->vertex[1].position.z +
                               triangle->vertex[2].position.z) /
                              3.0));
}

VkrBakeAabb vkr_bake_triangle_bounds(const VkrBakeTriangle *triangle) {
  VkrBakeAabb bounds = vkr_bake_aabb_empty();
  for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
    vkr_bake_aabb_extend_point(&bounds, triangle->vertex[vertex].position);
  return bounds;
}

bool8_t vkr_bake_triangle_normal(const VkrBakeTriangle *triangle,
                                 Vec3 *out_normal) {
  for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
    const VkrBakeVertex *source = &triangle->vertex[vertex];
    if (!vkr_bake_finite_vec3(source->position) ||
        !vkr_bake_finite_vec3(source->normal) ||
        !vkr_bake_finite_vec2(source->uv) ||
        !vkr_bake_finite_vec4(source->color))
      return false_v;
  }

  const Vec3 a = triangle->vertex[0].position;
  const Vec3 b = triangle->vertex[1].position;
  const Vec3 c = triangle->vertex[2].position;
  const float64_t ab_x = (float64_t)b.x - a.x;
  const float64_t ab_y = (float64_t)b.y - a.y;
  const float64_t ab_z = (float64_t)b.z - a.z;
  const float64_t ac_x = (float64_t)c.x - a.x;
  const float64_t ac_y = (float64_t)c.y - a.y;
  const float64_t ac_z = (float64_t)c.z - a.z;
  const float64_t normal_x = ab_y * ac_z - ab_z * ac_y;
  const float64_t normal_y = ab_z * ac_x - ab_x * ac_z;
  const float64_t normal_z = ab_x * ac_y - ab_y * ac_x;
  const float64_t length_squared =
      normal_x * normal_x + normal_y * normal_y + normal_z * normal_z;
  if (!(length_squared > 0.0) || !std::isfinite(length_squared))
    return false_v;

  const float64_t inverse_length = 1.0 / std::sqrt(length_squared);
  const Vec3 normal = vec3_new((float32_t)(normal_x * inverse_length),
                               (float32_t)(normal_y * inverse_length),
                               (float32_t)(normal_z * inverse_length));
  if (!vkr_bake_finite_vec3(normal))
    return false_v;
  *out_normal = normal;
  return true_v;
}

bool8_t vkr_bake_validate_geometry(VkrBakeGeometry geometry) {
  if (geometry.triangle_count > VKR_BAKE_BVH_MAX_TRIANGLES ||
      (geometry.triangle_count > 0u && !geometry.triangles))
    return false_v;
  for (uint32_t index = 0u; index < geometry.triangle_count; ++index) {
    Vec3 normal = vec3_zero();
    if (!vkr_bake_triangle_normal(&geometry.triangles[index], &normal))
      return false_v;
  }
  return true_v;
}

void vkr_bake_write_geometric_normals(VkrBakeGeometry geometry) {
  for (uint32_t index = 0u; index < geometry.triangle_count; ++index) {
    Vec3 normal = vec3_zero();
    const bool8_t valid =
        vkr_bake_triangle_normal(&geometry.triangles[index], &normal);
    (void)valid;
    geometry.triangles[index].geometric_normal = normal;
  }
}

VkrBakeAabb vkr_bake_range_bounds(const VkrBakeGeometry *geometry,
                                  uint32_t first, uint32_t count) {
  VkrBakeAabb bounds = vkr_bake_aabb_empty();
  for (uint32_t offset = 0u; offset < count; ++offset) {
    const VkrBakeAabb triangle_bounds =
        vkr_bake_triangle_bounds(&geometry->triangles[first + offset]);
    vkr_bake_aabb_extend_bounds(&bounds, &triangle_bounds);
  }
  return bounds;
}

VkrBakeAabb vkr_bake_range_centroid_bounds(const VkrBakeGeometry *geometry,
                                           uint32_t first, uint32_t count) {
  VkrBakeAabb bounds = vkr_bake_aabb_empty();
  for (uint32_t offset = 0u; offset < count; ++offset)
    vkr_bake_aabb_extend_point(
        &bounds,
        vkr_bake_triangle_centroid(&geometry->triangles[first + offset]));
  return bounds;
}

uint32_t vkr_bake_bin_index(float32_t centroid, float32_t min_value,
                            float32_t extent) {
  const float32_t normalized = (centroid - min_value) / extent;
  const float32_t scaled = normalized * (float32_t)VKR_BAKE_BVH_BIN_COUNT;
  const uint32_t bin = scaled > 0.0f ? (uint32_t)scaled : 0u;
  return Min(bin, VKR_BAKE_BVH_BIN_COUNT - 1u);
}

VkrBakeBvhSplit vkr_bake_choose_split(const VkrBakeGeometry *geometry,
                                      uint32_t first, uint32_t count,
                                      VkrBakeAabb centroid_bounds) {
  VkrBakeBvhSplit best = {0u, 0u, std::numeric_limits<float64_t>::infinity(),
                          false_v};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float32_t centroid_min =
        vkr_bake_component(centroid_bounds.min, axis);
    const float32_t extent =
        vkr_bake_component(centroid_bounds.max, axis) - centroid_min;
    if (!(extent > 0.0f) || !std::isfinite(extent))
      continue;

    VkrBakeBvhBin bins[VKR_BAKE_BVH_BIN_COUNT] = {};
    for (uint32_t bin = 0u; bin < VKR_BAKE_BVH_BIN_COUNT; ++bin)
      bins[bin].bounds = vkr_bake_aabb_empty();
    for (uint32_t offset = 0u; offset < count; ++offset) {
      const VkrBakeTriangle *triangle = &geometry->triangles[first + offset];
      const uint32_t bin = vkr_bake_bin_index(
          vkr_bake_component(vkr_bake_triangle_centroid(triangle), axis),
          centroid_min, extent);
      bins[bin].count++;
      const VkrBakeAabb bounds = vkr_bake_triangle_bounds(triangle);
      vkr_bake_aabb_extend_bounds(&bins[bin].bounds, &bounds);
    }

    VkrBakeAabb left_bounds[VKR_BAKE_BVH_BIN_COUNT - 1u] = {};
    VkrBakeAabb right_bounds[VKR_BAKE_BVH_BIN_COUNT - 1u] = {};
    uint32_t left_counts[VKR_BAKE_BVH_BIN_COUNT - 1u] = {};
    uint32_t right_counts[VKR_BAKE_BVH_BIN_COUNT - 1u] = {};
    VkrBakeAabb running_bounds = vkr_bake_aabb_empty();
    uint32_t running_count = 0u;
    for (uint32_t bin = 0u; bin + 1u < VKR_BAKE_BVH_BIN_COUNT; ++bin) {
      running_count += bins[bin].count;
      if (bins[bin].count)
        vkr_bake_aabb_extend_bounds(&running_bounds, &bins[bin].bounds);
      left_counts[bin] = running_count;
      left_bounds[bin] = running_bounds;
    }
    running_bounds = vkr_bake_aabb_empty();
    running_count = 0u;
    for (uint32_t bin = VKR_BAKE_BVH_BIN_COUNT - 1u; bin > 0u; --bin) {
      running_count += bins[bin].count;
      if (bins[bin].count)
        vkr_bake_aabb_extend_bounds(&running_bounds, &bins[bin].bounds);
      right_counts[bin - 1u] = running_count;
      right_bounds[bin - 1u] = running_bounds;
    }
    for (uint32_t bin = 0u; bin + 1u < VKR_BAKE_BVH_BIN_COUNT; ++bin) {
      if (!left_counts[bin] || !right_counts[bin])
        continue;
      const float64_t cost =
          vkr_bake_aabb_surface_area(&left_bounds[bin]) * left_counts[bin] +
          vkr_bake_aabb_surface_area(&right_bounds[bin]) * right_counts[bin];
      if (cost < best.cost) {
        best = VkrBakeBvhSplit{axis, bin, cost, true_v};
      }
    }
  }
  return best;
}

uint32_t vkr_bake_partition_for_split(VkrBakeGeometry *geometry, uint32_t first,
                                      uint32_t count,
                                      VkrBakeAabb centroid_bounds,
                                      VkrBakeBvhSplit split) {
  const float32_t centroid_min =
      vkr_bake_component(centroid_bounds.min, split.axis);
  const float32_t extent =
      vkr_bake_component(centroid_bounds.max, split.axis) - centroid_min;
  uint32_t left = first;
  uint32_t right = first + count;
  while (left < right) {
    const uint32_t bin = vkr_bake_bin_index(
        vkr_bake_component(
            vkr_bake_triangle_centroid(&geometry->triangles[left]), split.axis),
        centroid_min, extent);
    if (bin <= split.bin) {
      ++left;
      continue;
    }
    --right;
    const VkrBakeTriangle temporary = geometry->triangles[left];
    geometry->triangles[left] = geometry->triangles[right];
    geometry->triangles[right] = temporary;
  }
  return left - first;
}

bool8_t vkr_bake_build_range(VkrBakeBvhBuildContext *context, uint32_t first,
                             uint32_t count, uint32_t depth,
                             uint32_t *out_node_index) {
  if (context->node_count >= context->node_capacity ||
      depth > VKR_BAKE_BVH_MAX_DEPTH)
    return false_v;
  const uint32_t node_index = context->node_count++;
  VkrBakeBvhNode *node = &context->nodes[node_index];
  *node = VkrBakeBvhNode{
      vkr_bake_range_bounds(&context->geometry, first, count),
      VKR_BAKE_BVH_INVALID_NODE, VKR_BAKE_BVH_INVALID_NODE, first, count};
  if (count <= VKR_BAKE_BVH_LEAF_TRIANGLE_COUNT) {
    *out_node_index = node_index;
    return true_v;
  }

  const VkrBakeAabb centroid_bounds =
      vkr_bake_range_centroid_bounds(&context->geometry, first, count);
  const VkrBakeBvhSplit split =
      vkr_bake_choose_split(&context->geometry, first, count, centroid_bounds);
  uint32_t left_count =
      split.found ? vkr_bake_partition_for_split(&context->geometry, first,
                                                 count, centroid_bounds, split)
                  : 0u;
  /* Equal centroids have no SAH partition. Split their retained input order at
     the midpoint, which remains deterministic and bounds recursion depth. */
  if (left_count == 0u || left_count == count)
    left_count = count / 2u;
  const uint32_t right_count = count - left_count;
  if (!left_count || !right_count)
    return false_v;

  uint32_t left_child = VKR_BAKE_BVH_INVALID_NODE;
  uint32_t right_child = VKR_BAKE_BVH_INVALID_NODE;
  if (!vkr_bake_build_range(context, first, left_count, depth + 1u,
                            &left_child) ||
      !vkr_bake_build_range(context, first + left_count, right_count,
                            depth + 1u, &right_child))
    return false_v;
  node->left_child = left_child;
  node->right_child = right_child;
  node->first_triangle = 0u;
  node->triangle_count = 0u;
  *out_node_index = node_index;
  return true_v;
}

bool8_t vkr_bake_ray_valid(VkrBakeRay ray) {
  return vkr_bake_finite_vec3(ray.origin) &&
         vkr_bake_finite_vec3(ray.direction) && std::isfinite(ray.t_min) &&
         std::isfinite(ray.t_max) && ray.t_min >= 0.0f &&
         ray.t_max > ray.t_min &&
         (ray.direction.x != 0.0f || ray.direction.y != 0.0f ||
          ray.direction.z != 0.0f);
}

bool8_t vkr_bake_ray_intersects_aabb(VkrBakeRay ray, const VkrBakeAabb *bounds,
                                     float32_t limit, float32_t *out_t_enter) {
  float32_t t_enter = ray.t_min;
  float32_t t_exit = Min(ray.t_max, limit);
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float32_t origin = vkr_bake_component(ray.origin, axis);
    const float32_t direction = vkr_bake_component(ray.direction, axis);
    const float32_t min_value = vkr_bake_component(bounds->min, axis);
    const float32_t max_value = vkr_bake_component(bounds->max, axis);
    if (std::fabs(direction) <= std::numeric_limits<float32_t>::min()) {
      if (origin < min_value || origin > max_value)
        return false_v;
      continue;
    }
    const float32_t inverse_direction = 1.0f / direction;
    float32_t axis_enter = (min_value - origin) * inverse_direction;
    float32_t axis_exit = (max_value - origin) * inverse_direction;
    if (axis_enter > axis_exit) {
      const float32_t temporary = axis_enter;
      axis_enter = axis_exit;
      axis_exit = temporary;
    }
    t_enter = Max(t_enter, axis_enter);
    t_exit = Min(t_exit, axis_exit);
    if (t_enter > t_exit)
      return false_v;
  }
  *out_t_enter = t_enter;
  return true_v;
}

bool8_t vkr_bake_ray_intersects_triangle(VkrBakeRay ray,
                                         const VkrBakeTriangle *triangle,
                                         float32_t limit, VkrBakeHit *out_hit) {
  const Vec3 a = triangle->vertex[0].position;
  const Vec3 b = triangle->vertex[1].position;
  const Vec3 c = triangle->vertex[2].position;
  const float64_t edge1_x = (float64_t)b.x - a.x;
  const float64_t edge1_y = (float64_t)b.y - a.y;
  const float64_t edge1_z = (float64_t)b.z - a.z;
  const float64_t edge2_x = (float64_t)c.x - a.x;
  const float64_t edge2_y = (float64_t)c.y - a.y;
  const float64_t edge2_z = (float64_t)c.z - a.z;
  const float64_t direction_x = ray.direction.x;
  const float64_t direction_y = ray.direction.y;
  const float64_t direction_z = ray.direction.z;
  const float64_t p_x = direction_y * edge2_z - direction_z * edge2_y;
  const float64_t p_y = direction_z * edge2_x - direction_x * edge2_z;
  const float64_t p_z = direction_x * edge2_y - direction_y * edge2_x;
  const float64_t determinant = edge1_x * p_x + edge1_y * p_y + edge1_z * p_z;
  const float64_t edge1_length =
      std::sqrt(edge1_x * edge1_x + edge1_y * edge1_y + edge1_z * edge1_z);
  const float64_t edge2_length =
      std::sqrt(edge2_x * edge2_x + edge2_y * edge2_y + edge2_z * edge2_z);
  const float64_t direction_length =
      std::sqrt(direction_x * direction_x + direction_y * direction_y +
                direction_z * direction_z);
  const float64_t scale = edge1_length * edge2_length * direction_length;
  if (std::fabs(determinant) <= Max(1e-18, scale * 1e-12))
    return false_v;

  const float64_t inverse_determinant = 1.0 / determinant;
  const float64_t origin_to_a_x = (float64_t)ray.origin.x - a.x;
  const float64_t origin_to_a_y = (float64_t)ray.origin.y - a.y;
  const float64_t origin_to_a_z = (float64_t)ray.origin.z - a.z;
  float64_t bary_u =
      (origin_to_a_x * p_x + origin_to_a_y * p_y + origin_to_a_z * p_z) *
      inverse_determinant;
  const float64_t q_x = origin_to_a_y * edge1_z - origin_to_a_z * edge1_y;
  const float64_t q_y = origin_to_a_z * edge1_x - origin_to_a_x * edge1_z;
  const float64_t q_z = origin_to_a_x * edge1_y - origin_to_a_y * edge1_x;
  float64_t bary_v =
      (direction_x * q_x + direction_y * q_y + direction_z * q_z) *
      inverse_determinant;
  const float64_t t =
      (edge2_x * q_x + edge2_y * q_y + edge2_z * q_z) * inverse_determinant;
  constexpr float64_t barycentric_epsilon = 1e-9;
  if (bary_u < -barycentric_epsilon || bary_v < -barycentric_epsilon ||
      bary_u + bary_v > 1.0 + barycentric_epsilon || t < ray.t_min ||
      t > limit || !std::isfinite(t))
    return false_v;
  bary_u = Clamp(bary_u, 0.0, 1.0);
  bary_v = Clamp(bary_v, 0.0, 1.0 - bary_u);
  *out_hit = VkrBakeHit{(float32_t)t, (float32_t)bary_u, (float32_t)bary_v, 0u,
                        triangle->geometric_normal.x * ray.direction.x +
                                triangle->geometric_normal.y * ray.direction.y +
                                triangle->geometric_normal.z * ray.direction.z <
                            0.0f};
  return true_v;
}

} // namespace

bool8_t vkr_bake_bvh_build(VkrBakeGeometry geometry, Arena *arena,
                           VkrBakeBvh *out_bvh) {
  if (!out_bvh)
    return false_v;
  *out_bvh = VkrBakeBvh{};
  if (!arena || !vkr_bake_validate_geometry(geometry))
    return false_v;
  vkr_bake_write_geometric_normals(geometry);
  if (geometry.triangle_count == 0u) {
    out_bvh->geometry = geometry;
    return true_v;
  }

  const uint64_t node_capacity = (uint64_t)geometry.triangle_count * 2u - 1u;
  const uint64_t node_bytes = node_capacity * sizeof(VkrBakeBvhNode);
  if (node_capacity > UINT32_MAX ||
      node_bytes > std::numeric_limits<uint64_t>::max())
    return false_v;
  const Scratch allocation = scratch_create(arena);
  VkrBakeBvhNode *nodes =
      (VkrBakeBvhNode *)arena_alloc(arena, node_bytes, ARENA_MEMORY_TAG_ARRAY);
  if (!nodes) {
    scratch_destroy(allocation, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }

  VkrBakeBvhBuildContext context = {geometry, nodes, (uint32_t)node_capacity,
                                    0u};
  uint32_t root = VKR_BAKE_BVH_INVALID_NODE;
  if (!vkr_bake_build_range(&context, 0u, geometry.triangle_count, 0u, &root) ||
      root != 0u) {
    scratch_destroy(allocation, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }
  *out_bvh = VkrBakeBvh{geometry, nodes, context.node_count};
  return true_v;
}

bool8_t vkr_bake_bvh_intersect_closest(const VkrBakeBvh *bvh, VkrBakeRay ray,
                                       VkrBakeHit *out_hit) {
  if (!out_hit || !bvh || !vkr_bake_ray_valid(ray) ||
      (bvh->node_count > 0u && (!bvh->nodes || !bvh->geometry.triangles)))
    return false_v;
  if (bvh->node_count == 0u)
    return false_v;

  uint32_t stack[VKR_BAKE_BVH_MAX_DEPTH + 1u] = {0};
  float32_t stack_enter[VKR_BAKE_BVH_MAX_DEPTH + 1u] = {0.0f};
  uint32_t stack_count = 0u;
  float32_t root_enter = 0.0f;
  if (!vkr_bake_ray_intersects_aabb(ray, &bvh->nodes[0].bounds, ray.t_max,
                                    &root_enter))
    return false_v;
  stack[stack_count] = 0u;
  stack_enter[stack_count++] = root_enter;

  bool8_t found = false_v;
  float32_t closest_t = ray.t_max;
  VkrBakeHit closest = {};
  while (stack_count) {
    --stack_count;
    if (stack_enter[stack_count] > closest_t)
      continue;
    const uint32_t node_index = stack[stack_count];
    const VkrBakeBvhNode *node = &bvh->nodes[node_index];
    if (node->left_child == VKR_BAKE_BVH_INVALID_NODE) {
      for (uint32_t offset = 0u; offset < node->triangle_count; ++offset) {
        VkrBakeHit hit = {};
        const uint32_t triangle_index = node->first_triangle + offset;
        if (!vkr_bake_ray_intersects_triangle(
                ray, &bvh->geometry.triangles[triangle_index], closest_t, &hit))
          continue;
        found = true_v;
        closest_t = hit.t;
        hit.triangle_index = triangle_index;
        closest = hit;
      }
      continue;
    }

    const VkrBakeBvhNode *left = &bvh->nodes[node->left_child];
    const VkrBakeBvhNode *right = &bvh->nodes[node->right_child];
    float32_t left_enter = 0.0f;
    float32_t right_enter = 0.0f;
    const bool8_t left_hit = vkr_bake_ray_intersects_aabb(
        ray, &left->bounds, closest_t, &left_enter);
    const bool8_t right_hit = vkr_bake_ray_intersects_aabb(
        ray, &right->bounds, closest_t, &right_enter);
    if (left_hit && right_hit) {
      const bool8_t left_first =
          left_enter < right_enter ||
          (left_enter == right_enter && node->left_child < node->right_child);
      const uint32_t near_node =
          left_first ? node->left_child : node->right_child;
      const uint32_t far_node =
          left_first ? node->right_child : node->left_child;
      const float32_t near_enter = left_first ? left_enter : right_enter;
      const float32_t far_enter = left_first ? right_enter : left_enter;
      stack[stack_count] = far_node;
      stack_enter[stack_count++] = far_enter;
      stack[stack_count] = near_node;
      stack_enter[stack_count++] = near_enter;
    } else if (left_hit) {
      stack[stack_count] = node->left_child;
      stack_enter[stack_count++] = left_enter;
    } else if (right_hit) {
      stack[stack_count] = node->right_child;
      stack_enter[stack_count++] = right_enter;
    }
  }
  if (found)
    *out_hit = closest;
  return found;
}
