#include "vkr_bake_voxels.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

struct VkrBakeVoxelLayout {
  uint32_t dimensions[3];
  uint32_t count;
  Vec3 spacing;
};

bool8_t vkr_bake_voxel_finite_vec3(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

float32_t vkr_bake_voxel_component(Vec3 value, uint32_t axis) {
  return value.elements[axis];
}

uint32_t vkr_bake_voxel_index(const VkrBakeVoxelLayout *layout, uint32_t x,
                              uint32_t y, uint32_t z) {
  return x + layout->dimensions[0] * (y + layout->dimensions[1] * z);
}

bool8_t vkr_bake_voxel_count_checked(uint32_t x, uint32_t y, uint32_t z,
                                     uint32_t maximum_count,
                                     uint32_t *out_count) {
  const uint64_t count = (uint64_t)x * y * z;
  if (!x || !y || !z || count > maximum_count || count > UINT32_MAX)
    return false_v;
  *out_count = (uint32_t)count;
  return true_v;
}

bool8_t vkr_bake_voxel_bounds_valid(VkrBakeAabb bounds) {
  return vkr_bake_voxel_finite_vec3(bounds.min) &&
         vkr_bake_voxel_finite_vec3(bounds.max) &&
         bounds.max.x > bounds.min.x && bounds.max.y > bounds.min.y &&
         bounds.max.z > bounds.min.z;
}

bool8_t vkr_bake_voxel_probe_layout(VkrBakeVoxelGridDesc desc,
                                    VkrBakeVoxelLayout *out_layout) {
  if (!vkr_bake_voxel_bounds_valid(desc.bounds) ||
      !std::isfinite(desc.voxel_size) || !(desc.voxel_size > 0.0f))
    return false_v;
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    if (desc.probe_dimensions[axis] < 2u || desc.probe_dimensions[axis] > 256u)
      return false_v;
  }
  if (!vkr_bake_voxel_count_checked(
          desc.probe_dimensions[0], desc.probe_dimensions[1],
          desc.probe_dimensions[2], VKR_BAKE_VOXEL_MAX_PROBES,
          &out_layout->count))
    return false_v;
  out_layout->dimensions[0] = desc.probe_dimensions[0];
  out_layout->dimensions[1] = desc.probe_dimensions[1];
  out_layout->dimensions[2] = desc.probe_dimensions[2];
  out_layout->spacing = vec3_new((desc.bounds.max.x - desc.bounds.min.x) /
                                     (float32_t)desc.probe_dimensions[0],
                                 (desc.bounds.max.y - desc.bounds.min.y) /
                                     (float32_t)desc.probe_dimensions[1],
                                 (desc.bounds.max.z - desc.bounds.min.z) /
                                     (float32_t)desc.probe_dimensions[2]);
  const float32_t minimum_spacing = Min(
      out_layout->spacing.x, Min(out_layout->spacing.y, out_layout->spacing.z));
  return std::isfinite(minimum_spacing) &&
         desc.voxel_size <= minimum_spacing * 0.5f;
}

bool8_t vkr_bake_voxel_occupancy_layout(VkrBakeAabb bounds,
                                        float32_t requested_size,
                                        VkrBakeVoxelLayout *out_layout) {
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float32_t extent = vkr_bake_voxel_component(bounds.max, axis) -
                             vkr_bake_voxel_component(bounds.min, axis);
    const float64_t needed = std::ceil((float64_t)extent / requested_size);
    if (!(needed >= 1.0) || needed > UINT32_MAX)
      return false_v;
    out_layout->dimensions[axis] = (uint32_t)needed;
  }
  if (!vkr_bake_voxel_count_checked(
          out_layout->dimensions[0], out_layout->dimensions[1],
          out_layout->dimensions[2], VKR_BAKE_VOXEL_MAX_OCCUPANCY_COUNT,
          &out_layout->count))
    return false_v;
  out_layout->spacing = vec3_new(
      (bounds.max.x - bounds.min.x) / (float32_t)out_layout->dimensions[0],
      (bounds.max.y - bounds.min.y) / (float32_t)out_layout->dimensions[1],
      (bounds.max.z - bounds.min.z) / (float32_t)out_layout->dimensions[2]);
  return vkr_bake_voxel_finite_vec3(out_layout->spacing) &&
         out_layout->spacing.x > 0.0f && out_layout->spacing.y > 0.0f &&
         out_layout->spacing.z > 0.0f;
}

VkrBakeAabb vkr_bake_voxel_aabb(VkrBakeAabb bounds,
                                const VkrBakeVoxelLayout *layout, uint32_t x,
                                uint32_t y, uint32_t z) {
  const Vec3 min = vec3_new(bounds.min.x + layout->spacing.x * x,
                            bounds.min.y + layout->spacing.y * y,
                            bounds.min.z + layout->spacing.z * z);
  return VkrBakeAabb{min, vec3_new(min.x + layout->spacing.x,
                                   min.y + layout->spacing.y,
                                   min.z + layout->spacing.z)};
}

bool8_t vkr_bake_voxel_aabb_overlaps(const VkrBakeAabb *a,
                                     const VkrBakeAabb *b) {
  return a->min.x <= b->max.x && a->max.x >= b->min.x && a->min.y <= b->max.y &&
         a->max.y >= b->min.y && a->min.z <= b->max.z && a->max.z >= b->min.z;
}

bool8_t vkr_bake_voxel_axis_overlaps(Vec3 axis, Vec3 vertex0, Vec3 vertex1,
                                     Vec3 vertex2, Vec3 half_extent) {
  const float32_t axis_length_squared = vec3_dot(axis, axis);
  if (!(axis_length_squared > 0.0f))
    return true_v;
  const float32_t projection0 = vec3_dot(vertex0, axis);
  const float32_t projection1 = vec3_dot(vertex1, axis);
  const float32_t projection2 = vec3_dot(vertex2, axis);
  const float32_t minimum = Min(projection0, Min(projection1, projection2));
  const float32_t maximum = Max(projection0, Max(projection1, projection2));
  const float32_t radius = half_extent.x * std::fabs(axis.x) +
                           half_extent.y * std::fabs(axis.y) +
                           half_extent.z * std::fabs(axis.z);
  return minimum <= radius && maximum >= -radius;
}

bool8_t vkr_bake_voxel_triangle_overlaps_box(const VkrBakeTriangle *triangle,
                                             const VkrBakeAabb *box) {
  VkrBakeAabb triangle_bounds = {triangle->vertex[0].position,
                                 triangle->vertex[0].position};
  for (uint32_t vertex = 1u; vertex < 3u; ++vertex) {
    const Vec3 point = triangle->vertex[vertex].position;
    triangle_bounds.min.x = Min(triangle_bounds.min.x, point.x);
    triangle_bounds.min.y = Min(triangle_bounds.min.y, point.y);
    triangle_bounds.min.z = Min(triangle_bounds.min.z, point.z);
    triangle_bounds.max.x = Max(triangle_bounds.max.x, point.x);
    triangle_bounds.max.y = Max(triangle_bounds.max.y, point.y);
    triangle_bounds.max.z = Max(triangle_bounds.max.z, point.z);
  }
  if (!vkr_bake_voxel_aabb_overlaps(&triangle_bounds, box))
    return false_v;

  const Vec3 center = vec3_new((box->min.x + box->max.x) * 0.5f,
                               (box->min.y + box->max.y) * 0.5f,
                               (box->min.z + box->max.z) * 0.5f);
  const Vec3 half_extent = vec3_new((box->max.x - box->min.x) * 0.5f,
                                    (box->max.y - box->min.y) * 0.5f,
                                    (box->max.z - box->min.z) * 0.5f);
  const Vec3 vertex0 = vec3_sub(triangle->vertex[0].position, center);
  const Vec3 vertex1 = vec3_sub(triangle->vertex[1].position, center);
  const Vec3 vertex2 = vec3_sub(triangle->vertex[2].position, center);
  const Vec3 edge0 = vec3_sub(vertex1, vertex0);
  const Vec3 edge1 = vec3_sub(vertex2, vertex1);
  const Vec3 edge2 = vec3_sub(vertex0, vertex2);
  const Vec3 box_axis[] = {vec3_right(), vec3_up(), vec3_back()};
  const Vec3 edges[] = {edge0, edge1, edge2};
  for (uint32_t edge_index = 0u; edge_index < ArrayCount(edges); ++edge_index)
    for (uint32_t axis_index = 0u; axis_index < ArrayCount(box_axis);
         ++axis_index)
      if (!vkr_bake_voxel_axis_overlaps(
              vec3_cross(edges[edge_index], box_axis[axis_index]), vertex0,
              vertex1, vertex2, half_extent))
        return false_v;

  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float32_t minimum = Min(vkr_bake_voxel_component(vertex0, axis),
                                  Min(vkr_bake_voxel_component(vertex1, axis),
                                      vkr_bake_voxel_component(vertex2, axis)));
    const float32_t maximum = Max(vkr_bake_voxel_component(vertex0, axis),
                                  Max(vkr_bake_voxel_component(vertex1, axis),
                                      vkr_bake_voxel_component(vertex2, axis)));
    if (minimum > vkr_bake_voxel_component(half_extent, axis) ||
        maximum < -vkr_bake_voxel_component(half_extent, axis))
      return false_v;
  }

  const Vec3 normal = vec3_cross(edge0, edge1);
  return vkr_bake_voxel_axis_overlaps(normal, vertex0, vertex1, vertex2,
                                      half_extent);
}

bool8_t vkr_bake_voxel_materials_valid(const VkrBakeBvh *bvh,
                                       const bool8_t *material_blocks_rooms,
                                       uint32_t material_count) {
  if (!bvh || (bvh->geometry.triangle_count > 0u &&
               (!bvh->geometry.triangles || !material_blocks_rooms)))
    return false_v;
  for (uint32_t index = 0u; index < bvh->geometry.triangle_count; ++index) {
    if (bvh->geometry.triangles[index].material_index >= material_count)
      return false_v;
  }
  return true_v;
}

bool8_t vkr_bake_voxel_is_solid(const VkrBakeBvh *bvh,
                                const bool8_t *material_blocks_rooms,
                                const VkrBakeAabb *voxel) {
  if (!bvh->node_count)
    return false_v;
  uint32_t stack[VKR_BAKE_BVH_MAX_DEPTH + 1u] = {0};
  uint32_t stack_count = 0u;
  stack[stack_count++] = 0u;
  while (stack_count) {
    const VkrBakeBvhNode *node = &bvh->nodes[stack[--stack_count]];
    if (!vkr_bake_voxel_aabb_overlaps(&node->bounds, voxel))
      continue;
    if (node->left_child != VKR_BAKE_BVH_INVALID_NODE) {
      stack[stack_count++] = node->right_child;
      stack[stack_count++] = node->left_child;
      continue;
    }
    for (uint32_t offset = 0u; offset < node->triangle_count; ++offset) {
      const VkrBakeTriangle *triangle =
          &bvh->geometry.triangles[node->first_triangle + offset];
      if (material_blocks_rooms[triangle->material_index] &&
          vkr_bake_voxel_triangle_overlaps_box(triangle, voxel))
        return true_v;
    }
  }
  return false_v;
}

void vkr_bake_voxel_dilate(const VkrBakeVoxelLayout *layout,
                           const uint8_t *occupied, uint8_t *out_dilated) {
  for (uint32_t z = 0u; z < layout->dimensions[2]; ++z) {
    for (uint32_t y = 0u; y < layout->dimensions[1]; ++y) {
      for (uint32_t x = 0u; x < layout->dimensions[0]; ++x) {
        bool8_t solid = false_v;
        for (int32_t dz = -1; dz <= 1 && !solid; ++dz) {
          const int32_t neighbor_z = (int32_t)z + dz;
          if (neighbor_z < 0 || neighbor_z >= (int32_t)layout->dimensions[2])
            continue;
          for (int32_t dy = -1; dy <= 1 && !solid; ++dy) {
            const int32_t neighbor_y = (int32_t)y + dy;
            if (neighbor_y < 0 || neighbor_y >= (int32_t)layout->dimensions[1])
              continue;
            for (int32_t dx = -1; dx <= 1; ++dx) {
              const int32_t neighbor_x = (int32_t)x + dx;
              if (neighbor_x < 0 ||
                  neighbor_x >= (int32_t)layout->dimensions[0])
                continue;
              if (occupied[vkr_bake_voxel_index(layout, (uint32_t)neighbor_x,
                                                (uint32_t)neighbor_y,
                                                (uint32_t)neighbor_z)]) {
                solid = true_v;
                break;
              }
            }
          }
        }
        out_dilated[vkr_bake_voxel_index(layout, x, y, z)] = solid;
      }
    }
  }
}

void vkr_bake_voxel_enqueue_boundary_empty(const VkrBakeVoxelLayout *layout,
                                           const uint8_t *occupied,
                                           uint32_t *regions, uint32_t *queue,
                                           uint32_t *queue_count) {
  for (uint32_t z = 0u; z < layout->dimensions[2]; ++z)
    for (uint32_t y = 0u; y < layout->dimensions[1]; ++y)
      for (uint32_t x = 0u; x < layout->dimensions[0]; ++x) {
        if (x != 0u && y != 0u && z != 0u && x + 1u != layout->dimensions[0] &&
            y + 1u != layout->dimensions[1] && z + 1u != layout->dimensions[2])
          continue;
        const uint32_t index = vkr_bake_voxel_index(layout, x, y, z);
        if (!occupied[index] && regions[index] == 0u) {
          regions[index] = UINT32_MAX;
          queue[(*queue_count)++] = index;
        }
      }
}

void vkr_bake_voxel_flood(const VkrBakeVoxelLayout *layout,
                          const uint8_t *occupied, uint32_t *regions,
                          uint32_t *queue, uint32_t *queue_count,
                          uint32_t region_id) {
  uint32_t read = 0u;
  while (read < *queue_count) {
    const uint32_t index = queue[read++];
    const uint32_t z = index / (layout->dimensions[0] * layout->dimensions[1]);
    const uint32_t remainder =
        index - z * layout->dimensions[0] * layout->dimensions[1];
    const uint32_t y = remainder / layout->dimensions[0];
    const uint32_t x = remainder - y * layout->dimensions[0];
    const int32_t offsets[][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                  {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};
    for (uint32_t direction = 0u; direction < ArrayCount(offsets);
         ++direction) {
      const int32_t neighbor_x = (int32_t)x + offsets[direction][0];
      const int32_t neighbor_y = (int32_t)y + offsets[direction][1];
      const int32_t neighbor_z = (int32_t)z + offsets[direction][2];
      if (neighbor_x < 0 || neighbor_y < 0 || neighbor_z < 0 ||
          neighbor_x >= (int32_t)layout->dimensions[0] ||
          neighbor_y >= (int32_t)layout->dimensions[1] ||
          neighbor_z >= (int32_t)layout->dimensions[2])
        continue;
      const uint32_t neighbor =
          vkr_bake_voxel_index(layout, (uint32_t)neighbor_x,
                               (uint32_t)neighbor_y, (uint32_t)neighbor_z);
      if (occupied[neighbor] || regions[neighbor] != 0u)
        continue;
      regions[neighbor] = region_id;
      queue[(*queue_count)++] = neighbor;
    }
  }
}

uint32_t vkr_bake_voxel_region_at(const VkrBakeVoxelLayout *layout,
                                  VkrBakeAabb bounds, const uint32_t *regions,
                                  Vec3 position) {
  uint32_t coordinates[3] = {0};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float32_t offset = (vkr_bake_voxel_component(position, axis) -
                              vkr_bake_voxel_component(bounds.min, axis)) /
                             vkr_bake_voxel_component(layout->spacing, axis);
    if (!std::isfinite(offset) || !(offset >= 0.0f) ||
        !(offset < (float32_t)layout->dimensions[axis]))
      return 0u;
    coordinates[axis] = Min((uint32_t)offset, layout->dimensions[axis] - 1u);
  }
  const uint32_t region = regions[vkr_bake_voxel_index(
      layout, coordinates[0], coordinates[1], coordinates[2])];
  return region == UINT32_MAX ? 0u : region;
}

uint32_t vkr_bake_voxel_cell_index(uint32_t cell_x, uint32_t cell_y,
                                   uint32_t cell_z, uint32_t cell_width,
                                   uint32_t cell_height) {
  return cell_x + cell_width * (cell_y + cell_height * cell_z);
}

bool8_t vkr_bake_voxel_nearest_blocking_hit(
    const VkrBakeBvh *bvh, const bool8_t *material_blocks_rooms, Vec3 origin,
    Vec3 direction, float32_t minimum_distance, float32_t maximum_distance,
    VkrBakeHit *out_hit) {
  VkrBakeRay ray = {origin, direction, minimum_distance, maximum_distance};
  while (vkr_bake_bvh_intersect_closest(bvh, ray, out_hit)) {
    const VkrBakeTriangle *triangle =
        &bvh->geometry.triangles[out_hit->triangle_index];
    if (material_blocks_rooms[triangle->material_index])
      return true_v;
    const float32_t next_minimum =
        out_hit->t + Max(minimum_distance, out_hit->t * 1.0e-5f);
    if (!(next_minimum < maximum_distance))
      return false_v;
    ray.t_min = next_minimum;
  }
  return false_v;
}

bool8_t vkr_bake_voxel_region_is_room_air(const VkrBakeBvh *bvh,
                                          const bool8_t *material_blocks_rooms,
                                          Vec3 origin,
                                          float32_t minimum_spacing,
                                          float32_t maximum_distance) {
  const float32_t minimum_distance = Max(1.0e-5f, minimum_spacing * 1.0e-4f);
  const Vec3 directions[] = {
      vec3_new(-1.0f, 0.0f, 0.0f), vec3_new(1.0f, 0.0f, 0.0f),
      vec3_new(0.0f, -1.0f, 0.0f), vec3_new(0.0f, 1.0f, 0.0f),
      vec3_new(0.0f, 0.0f, -1.0f), vec3_new(0.0f, 0.0f, 1.0f),
  };
  for (uint32_t direction_index = 0u; direction_index < ArrayCount(directions);
       ++direction_index) {
    VkrBakeHit hit = {};
    if (!vkr_bake_voxel_nearest_blocking_hit(
            bvh, material_blocks_rooms, origin, directions[direction_index],
            minimum_distance, maximum_distance, &hit))
      return false_v;
    const Vec3 normal =
        bvh->geometry.triangles[hit.triangle_index].geometric_normal;
    if (!(vec3_dot(normal, directions[direction_index]) < -1.0e-4f))
      return false_v;
  }
  return true_v;
}

bool8_t vkr_bake_voxel_cell_is_clear(const VkrBakeVoxelLayout *occupancy_layout,
                                     VkrBakeAabb bounds,
                                     const uint32_t *regions,
                                     VkrBakeAabb cell_bounds,
                                     uint32_t region_id) {
  uint32_t first[3] = {0u};
  uint32_t last[3] = {0u};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float64_t minimum =
        ((float64_t)vkr_bake_voxel_component(cell_bounds.min, axis) -
         vkr_bake_voxel_component(bounds.min, axis)) /
        vkr_bake_voxel_component(occupancy_layout->spacing, axis);
    const float64_t maximum =
        ((float64_t)vkr_bake_voxel_component(cell_bounds.max, axis) -
         vkr_bake_voxel_component(bounds.min, axis)) /
        vkr_bake_voxel_component(occupancy_layout->spacing, axis);
    if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
        !(minimum >= 0.0) || !(maximum >= minimum) ||
        !(maximum <= (float64_t)occupancy_layout->dimensions[axis]))
      return false_v;
    const int64_t first_index = (int64_t)std::ceil(minimum) - 1;
    const int64_t last_index = (int64_t)std::floor(maximum);
    first[axis] = (uint32_t)Max(first_index, 0);
    last[axis] = (uint32_t)Min(last_index,
                               (int64_t)occupancy_layout->dimensions[axis] - 1);
    if (first[axis] > last[axis])
      return false_v;
  }

  for (uint32_t z = first[2]; z <= last[2]; ++z)
    for (uint32_t y = first[1]; y <= last[1]; ++y)
      for (uint32_t x = first[0]; x <= last[0]; ++x)
        if (regions[vkr_bake_voxel_index(occupancy_layout, x, y, z)] !=
            region_id)
          return false_v;
  return true_v;
}

} // namespace

bool8_t vkr_bake_voxels_build(const VkrBakeBvh *bvh,
                              const bool8_t *material_blocks_rooms,
                              uint32_t material_count,
                              VkrBakeVoxelGridDesc desc, Arena *arena,
                              VkrBakeVoxelResult *out_result) {
  if (!out_result)
    return false_v;
  *out_result = VkrBakeVoxelResult{};
  if (!arena || !bvh ||
      !vkr_bake_voxel_materials_valid(bvh, material_blocks_rooms,
                                      material_count))
    return false_v;

  VkrBakeVoxelLayout probe_layout = {};
  VkrBakeVoxelLayout occupancy_layout = {};
  if (!vkr_bake_voxel_probe_layout(desc, &probe_layout) ||
      !vkr_bake_voxel_occupancy_layout(desc.bounds, desc.voxel_size,
                                       &occupancy_layout))
    return false_v;
  uint32_t cell_count = 0u;
  if (!vkr_bake_voxel_count_checked(probe_layout.dimensions[0] - 1u,
                                    probe_layout.dimensions[1] - 1u,
                                    probe_layout.dimensions[2] - 1u,
                                    VKR_BAKE_VOXEL_MAX_PROBES, &cell_count))
    return false_v;

  const Scratch result_scope = scratch_create(arena);
  VkrBakeVoxelProbe *probes = (VkrBakeVoxelProbe *)arena_alloc(
      arena, (uint64_t)probe_layout.count * sizeof(*probes),
      ARENA_MEMORY_TAG_ARRAY);
  uint32_t *cell_region_ids = (uint32_t *)arena_alloc(
      arena, (uint64_t)cell_count * sizeof(*cell_region_ids),
      ARENA_MEMORY_TAG_ARRAY);
  if (!probes || !cell_region_ids) {
    scratch_destroy(result_scope, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }
  const Scratch temporary_scope = scratch_create(arena);
  uint8_t *occupied = (uint8_t *)arena_alloc(arena, occupancy_layout.count,
                                             ARENA_MEMORY_TAG_ARRAY);
  uint8_t *dilated = (uint8_t *)arena_alloc(arena, occupancy_layout.count,
                                            ARENA_MEMORY_TAG_ARRAY);
  uint32_t *regions = (uint32_t *)arena_alloc(
      arena, (uint64_t)occupancy_layout.count * sizeof(*regions),
      ARENA_MEMORY_TAG_ARRAY);
  uint32_t *queue = (uint32_t *)arena_alloc(
      arena, (uint64_t)occupancy_layout.count * sizeof(*queue),
      ARENA_MEMORY_TAG_ARRAY);
  if (!occupied || !dilated || !regions || !queue) {
    scratch_destroy(result_scope, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }
  MemZero(regions, (uint64_t)occupancy_layout.count * sizeof(*regions));

  for (uint32_t z = 0u; z < occupancy_layout.dimensions[2]; ++z)
    for (uint32_t y = 0u; y < occupancy_layout.dimensions[1]; ++y)
      for (uint32_t x = 0u; x < occupancy_layout.dimensions[0]; ++x) {
        const VkrBakeAabb voxel =
            vkr_bake_voxel_aabb(desc.bounds, &occupancy_layout, x, y, z);
        occupied[vkr_bake_voxel_index(&occupancy_layout, x, y, z)] =
            vkr_bake_voxel_is_solid(bvh, material_blocks_rooms, &voxel);
      }
  vkr_bake_voxel_dilate(&occupancy_layout, occupied, dilated);

  uint32_t queue_count = 0u;
  vkr_bake_voxel_enqueue_boundary_empty(&occupancy_layout, dilated, regions,
                                        queue, &queue_count);
  vkr_bake_voxel_flood(&occupancy_layout, dilated, regions, queue, &queue_count,
                       UINT32_MAX);
  uint32_t next_region = 1u;
  for (uint32_t index = 0u; index < occupancy_layout.count; ++index) {
    if (dilated[index] || regions[index] != 0u)
      continue;
    queue[0] = index;
    regions[index] = next_region;
    queue_count = 1u;
    vkr_bake_voxel_flood(&occupancy_layout, dilated, regions, queue,
                         &queue_count, next_region);
    if (next_region == UINT32_MAX) {
      scratch_destroy(result_scope, ARENA_MEMORY_TAG_ARRAY);
      return false_v;
    }
    ++next_region;
  }

  const uint32_t region_count = next_region - 1u;
  uint8_t *region_valid = nullptr;
  if (region_count) {
    region_valid =
        (uint8_t *)arena_alloc(arena, region_count, ARENA_MEMORY_TAG_ARRAY);
    if (!region_valid) {
      scratch_destroy(result_scope, ARENA_MEMORY_TAG_ARRAY);
      return false_v;
    }
    MemZero(region_valid, region_count);
  }
  for (uint32_t index = 0u; index < occupancy_layout.count; ++index) {
    const uint32_t region = regions[index];
    if (region != 0u && region != UINT32_MAX)
      queue[region - 1u] = index;
  }
  const Vec3 extent = vec3_sub(desc.bounds.max, desc.bounds.min);
  const float32_t maximum_distance =
      sqrtf(vec3_dot(extent, extent)) + desc.voxel_size;
  const float32_t minimum_spacing =
      Min(occupancy_layout.spacing.x,
          Min(occupancy_layout.spacing.y, occupancy_layout.spacing.z));
  for (uint32_t region = 1u; region <= region_count; ++region) {
    const uint32_t index = queue[region - 1u];
    const uint32_t z = index / (occupancy_layout.dimensions[0] *
                                occupancy_layout.dimensions[1]);
    const uint32_t remainder = index - z * occupancy_layout.dimensions[0] *
                                           occupancy_layout.dimensions[1];
    const uint32_t y = remainder / occupancy_layout.dimensions[0];
    const uint32_t x = remainder - y * occupancy_layout.dimensions[0];
    const VkrBakeAabb voxel =
        vkr_bake_voxel_aabb(desc.bounds, &occupancy_layout, x, y, z);
    const Vec3 representative =
        vec3_scale(vec3_add(voxel.min, voxel.max), 0.5f);
    region_valid[region - 1u] = vkr_bake_voxel_region_is_room_air(
        bvh, material_blocks_rooms, representative, minimum_spacing,
        maximum_distance);
  }
  for (uint32_t index = 0u; index < occupancy_layout.count; ++index) {
    const uint32_t region = regions[index];
    if (region != 0u && region != UINT32_MAX && !region_valid[region - 1u])
      regions[index] = 0u;
  }

  const Vec3 origin =
      vec3_new(desc.bounds.min.x + probe_layout.spacing.x * 0.5f,
               desc.bounds.min.y + probe_layout.spacing.y * 0.5f,
               desc.bounds.min.z + probe_layout.spacing.z * 0.5f);
  for (uint32_t z = 0u; z < probe_layout.dimensions[2]; ++z)
    for (uint32_t y = 0u; y < probe_layout.dimensions[1]; ++y)
      for (uint32_t x = 0u; x < probe_layout.dimensions[0]; ++x) {
        const Vec3 position = vec3_new(origin.x + probe_layout.spacing.x * x,
                                       origin.y + probe_layout.spacing.y * y,
                                       origin.z + probe_layout.spacing.z * z);
        probes[vkr_bake_voxel_index(&probe_layout, x, y, z)] =
            VkrBakeVoxelProbe{position, vkr_bake_voxel_region_at(
                                            &occupancy_layout, desc.bounds,
                                            regions, position)};
      }

  const uint32_t cell_width = probe_layout.dimensions[0] - 1u;
  const uint32_t cell_height = probe_layout.dimensions[1] - 1u;
  for (uint32_t z = 0u; z + 1u < probe_layout.dimensions[2]; ++z)
    for (uint32_t y = 0u; y + 1u < probe_layout.dimensions[1]; ++y)
      for (uint32_t x = 0u; x + 1u < probe_layout.dimensions[0]; ++x) {
        const uint32_t cell_index =
            vkr_bake_voxel_cell_index(x, y, z, cell_width, cell_height);
        const uint32_t corner_indices[] = {
            vkr_bake_voxel_index(&probe_layout, x, y, z),
            vkr_bake_voxel_index(&probe_layout, x + 1u, y, z),
            vkr_bake_voxel_index(&probe_layout, x, y + 1u, z),
            vkr_bake_voxel_index(&probe_layout, x + 1u, y + 1u, z),
            vkr_bake_voxel_index(&probe_layout, x, y, z + 1u),
            vkr_bake_voxel_index(&probe_layout, x + 1u, y, z + 1u),
            vkr_bake_voxel_index(&probe_layout, x, y + 1u, z + 1u),
            vkr_bake_voxel_index(&probe_layout, x + 1u, y + 1u, z + 1u),
        };
        const uint32_t region = probes[corner_indices[0]].region_id;
        bool8_t valid = region != 0u;
        Vec3 center = vec3_zero();
        for (uint32_t corner = 0u; corner < ArrayCount(corner_indices);
             ++corner) {
          valid = valid && probes[corner_indices[corner]].region_id == region;
          center = vec3_add(center, probes[corner_indices[corner]].position);
        }
        center = vec3_scale(center, 0.125f);
        valid =
            valid && vkr_bake_voxel_region_at(&occupancy_layout, desc.bounds,
                                              regions, center) == region;
        const VkrBakeAabb cell_bounds = {
            probes[corner_indices[0]].position,
            probes[corner_indices[7]].position,
        };
        valid = valid &&
                vkr_bake_voxel_cell_is_clear(&occupancy_layout, desc.bounds,
                                             regions, cell_bounds, region);
        cell_region_ids[cell_index] = valid ? region : 0u;
      }

  scratch_destroy(temporary_scope, ARENA_MEMORY_TAG_ARRAY);
  *out_result = VkrBakeVoxelResult{
      origin,
      probe_layout.spacing,
      {probe_layout.dimensions[0], probe_layout.dimensions[1],
       probe_layout.dimensions[2]},
      probes,
      probe_layout.count,
      cell_region_ids,
      cell_count,
  };
  return true_v;
}
