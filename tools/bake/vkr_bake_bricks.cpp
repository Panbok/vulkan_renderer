#include "vkr_bake_bricks.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

/* Lattice coordinates pack into 20 bits per axis with the level above. */
constexpr uint32_t lattice_bits = 20u;
constexpr uint32_t lattice_limit = 1u << lattice_bits;

uint32_t level_span(uint32_t level) {
  uint32_t span = 1u;
  for (uint32_t i = 0u; i < level; ++i) {
    span *= 3u;
  }
  return span;
}

bool8_t aabb_overlaps(const VkrBakeAabb &a, const VkrBakeAabb &b) {
  return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y &&
         a.max.y >= b.min.y && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

bool8_t axis_overlaps(Vec3 axis, Vec3 vertex0, Vec3 vertex1, Vec3 vertex2,
                      Vec3 half_extent) {
  const float32_t axis_length_squared = vec3_dot(axis, axis);
  if (!(axis_length_squared > 0.0f)) {
    return true_v;
  }
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

/* Separating-axis test of a triangle against an axis-aligned box. */
bool8_t triangle_overlaps_box(const VkrBakeTriangle &triangle,
                              const VkrBakeAabb &box) {
  VkrBakeAabb triangle_bounds = {triangle.vertex[0].position,
                                 triangle.vertex[0].position};
  for (uint32_t vertex = 1u; vertex < 3u; ++vertex) {
    const Vec3 point = triangle.vertex[vertex].position;
    triangle_bounds.min.x = Min(triangle_bounds.min.x, point.x);
    triangle_bounds.min.y = Min(triangle_bounds.min.y, point.y);
    triangle_bounds.min.z = Min(triangle_bounds.min.z, point.z);
    triangle_bounds.max.x = Max(triangle_bounds.max.x, point.x);
    triangle_bounds.max.y = Max(triangle_bounds.max.y, point.y);
    triangle_bounds.max.z = Max(triangle_bounds.max.z, point.z);
  }
  if (!aabb_overlaps(triangle_bounds, box)) {
    return false_v;
  }

  const Vec3 center = vec3_scale(vec3_add(box.min, box.max), 0.5f);
  const Vec3 half_extent = vec3_scale(vec3_sub(box.max, box.min), 0.5f);
  const Vec3 vertex0 = vec3_sub(triangle.vertex[0].position, center);
  const Vec3 vertex1 = vec3_sub(triangle.vertex[1].position, center);
  const Vec3 vertex2 = vec3_sub(triangle.vertex[2].position, center);
  const Vec3 edges[] = {vec3_sub(vertex1, vertex0), vec3_sub(vertex2, vertex1),
                        vec3_sub(vertex0, vertex2)};
  const Vec3 box_axes[] = {vec3_right(), vec3_up(), vec3_back()};
  for (uint32_t e = 0u; e < ArrayCount(edges); ++e) {
    for (uint32_t a = 0u; a < ArrayCount(box_axes); ++a) {
      if (!axis_overlaps(vec3_cross(edges[e], box_axes[a]), vertex0, vertex1,
                         vertex2, half_extent)) {
        return false_v;
      }
    }
  }

  const Vec3 normal = vec3_cross(edges[0], edges[1]);
  return axis_overlaps(normal, vertex0, vertex1, vertex2, half_extent);
}

/* Whether a blocking triangle overlaps `box`. */
bool8_t blocking_geometry_in(const VkrBakeBvh &bvh,
                             const VkrBakeBrickMaterial *materials,
                             const VkrBakeAabb &box) {
  if (!bvh.node_count) {
    return false_v;
  }
  uint32_t stack[VKR_BAKE_BVH_MAX_DEPTH + 1u] = {0};
  uint32_t stack_count = 0u;
  stack[stack_count++] = 0u;
  while (stack_count) {
    const VkrBakeBvhNode &node = bvh.nodes[stack[--stack_count]];
    if (!aabb_overlaps(node.bounds, box)) {
      continue;
    }
    if (node.left_child != VKR_BAKE_BVH_INVALID_NODE) {
      stack[stack_count++] = node.right_child;
      stack[stack_count++] = node.left_child;
      continue;
    }
    for (uint32_t offset = 0u; offset < node.triangle_count; ++offset) {
      const VkrBakeTriangle &triangle =
          bvh.geometry.triangles[node.first_triangle + offset];
      if (materials[triangle.material_index].blocks &&
          triangle_overlaps_box(triangle, box)) {
        return true_v;
      }
    }
  }
  return false_v;
}

/* The closest blocking hit along a ray; surfaces light passes are skipped. */
bool8_t closest_blocking_hit(const VkrBakeBvh &bvh,
                             const VkrBakeBrickMaterial *materials, Vec3 origin,
                             Vec3 direction, float32_t minimum_distance,
                             float32_t maximum_distance, VkrBakeHit *out_hit) {
  VkrBakeRay ray = {origin, direction, minimum_distance, maximum_distance};
  while (vkr_bake_bvh_intersect_closest(&bvh, ray, out_hit)) {
    const VkrBakeTriangle &triangle =
        bvh.geometry.triangles[out_hit->triangle_index];
    if (materials[triangle.material_index].blocks) {
      return true_v;
    }
    const float32_t next_minimum =
        out_hit->t + Max(minimum_distance, out_hit->t * 1.0e-5f);
    if (!(next_minimum < maximum_distance)) {
      return false_v;
    }
    ray.t_min = next_minimum;
  }
  return false_v;
}

struct Placement {
  const VkrBakeBvh *bvh;
  const VkrBakeBrickMaterial *materials;
  Vec3 origin;
  float32_t entry_size;
  float32_t margin_spans;
  uint32_t dimensions[3];
  std::vector<uint32_t> entries;
  std::vector<VkrDiffuseVolumeBrick> bricks;
  bool overflow = false;
};

uint32_t entry_index(const Placement &placement, uint32_t x, uint32_t y,
                     uint32_t z) {
  return x + placement.dimensions[0] * (y + placement.dimensions[1] * z);
}

VkrBakeAabb block_box(const Placement &placement, const uint32_t entry[3],
                      uint32_t span) {
  VkrBakeAabb box = {};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    box.min.elements[axis] = placement.origin.elements[axis] +
                             placement.entry_size * (float32_t)entry[axis];
    box.max.elements[axis] =
        box.min.elements[axis] + placement.entry_size * (float32_t)span;
  }
  return box;
}

/* Whether a level-`level` block at `entry` holds geometry within its margin,
   so its entries need finer bricks. */
bool near_surface(const Placement &placement, const uint32_t entry[3],
                  uint32_t level) {
  const uint32_t span = level_span(level);
  VkrBakeAabb box = block_box(placement, entry, span);
  const float32_t margin =
      placement.margin_spans * placement.entry_size * (float32_t)span;
  box.min = vec3_sub(box.min, vec3_new(margin, margin, margin));
  box.max = vec3_add(box.max, vec3_new(margin, margin, margin));
  return blocking_geometry_in(*placement.bvh, placement.materials, box);
}

void assign_entries(Placement *placement, const uint32_t entry[3],
                    uint32_t span, uint32_t value) {
  for (uint32_t z = 0u; z < span; ++z) {
    for (uint32_t y = 0u; y < span; ++y) {
      for (uint32_t x = 0u; x < span; ++x) {
        placement->entries[entry_index(*placement, entry[0] + x, entry[1] + y,
                                       entry[2] + z)] = value;
      }
    }
  }
}

uint32_t add_brick(Placement *placement, const uint32_t entry[3],
                   uint32_t level) {
  if (placement->bricks.size() >= VKR_DIFFUSE_VOLUME_MAX_BRICKS) {
    placement->overflow = true;
    return VKR_DIFFUSE_VOLUME_ENTRY_EMPTY;
  }
  const uint32_t brick = (uint32_t)placement->bricks.size();
  placement->bricks.push_back({{entry[0], entry[1], entry[2]}, level});
  return (level << VKR_DIFFUSE_VOLUME_ENTRY_LEVEL_SHIFT) | brick;
}

/* A block refines its children near geometry; the entries of the others
   share one brick of the block's own level. */
void place_block(Placement *placement, const uint32_t entry[3],
                 uint32_t level) {
  if (placement->overflow) {
    return;
  }
  if (level == 0u) {
    assign_entries(placement, entry, 1u, add_brick(placement, entry, 0u));
    return;
  }
  const uint32_t child_span = level_span(level - 1u);
  bool refine[27] = {};
  bool any_coarse = false;
  for (uint32_t c = 0u; c < 27u; ++c) {
    const uint32_t child[3] = {entry[0] + child_span * (c % 3u),
                               entry[1] + child_span * ((c / 3u) % 3u),
                               entry[2] + child_span * (c / 9u)};
    refine[c] = near_surface(*placement, child, level - 1u);
    any_coarse = any_coarse || !refine[c];
  }
  if (any_coarse) {
    const uint32_t value = add_brick(placement, entry, level);
    for (uint32_t c = 0u; c < 27u; ++c) {
      if (refine[c]) {
        continue;
      }
      const uint32_t child[3] = {entry[0] + child_span * (c % 3u),
                                 entry[1] + child_span * ((c / 3u) % 3u),
                                 entry[2] + child_span * (c / 9u)};
      assign_entries(placement, child, child_span, value);
    }
  }
  for (uint32_t c = 0u; c < 27u; ++c) {
    if (!refine[c]) {
      continue;
    }
    const uint32_t child[3] = {entry[0] + child_span * (c % 3u),
                               entry[1] + child_span * ((c / 3u) % 3u),
                               entry[2] + child_span * (c / 9u)};
    place_block(placement, child, level - 1u);
  }
}

uint64_t probe_key(const uint32_t lattice[3], uint32_t level) {
  return ((uint64_t)level << (3u * lattice_bits)) |
         ((uint64_t)lattice[2] << (2u * lattice_bits)) |
         ((uint64_t)lattice[1] << lattice_bits) | (uint64_t)lattice[0];
}

uint32_t mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  return x ^ (x >> 16);
}

float32_t unit(uint32_t seed) {
  return (float32_t)(mix(seed) >> 8) * (1.0f / 16777216.0f);
}

/* Octahedral direction of a point in [-1, 1]^2; the shader's encoding is
   its inverse. */
Vec3 octahedral_direction(float32_t u, float32_t v) {
  Vec3 direction = vec3_new(u, v, 1.0f - std::fabs(u) - std::fabs(v));
  if (direction.z < 0.0f) {
    const float32_t x = direction.x;
    direction.x = (1.0f - std::fabs(direction.y)) * (x >= 0.0f ? 1.0f : -1.0f);
    direction.y =
        (1.0f - std::fabs(x)) * (direction.y >= 0.0f ? 1.0f : -1.0f);
  }
  return vec3_normalize(direction);
}

struct Trace {
  uint32_t backfaces = 0u;
  float32_t closest_backface = INFINITY;
  Vec3 closest_backface_direction = {};
  float32_t closest_frontface = INFINITY;
  Vec3 closest_frontface_direction = {};
  float32_t moments[VKR_DIFFUSE_VOLUME_MOMENT_TEXELS * 2u] = {};
};

/* Traces VKR_BAKE_BRICK_RAYS_PER_TEXEL rays through each interior moment
   texel, stratified over the texel; distances past `moment_range` and
   escapes count as `moment_range`. */
void trace_probe(const VkrBakeBvh &bvh, const VkrBakeBrickMaterial *materials,
                 Vec3 position, uint32_t seed, float32_t moment_range,
                 float32_t ray_range, Trace *out) {
  constexpr uint32_t interior = VKR_DIFFUSE_VOLUME_MOMENT_INTERIOR;
  constexpr uint32_t size = VKR_DIFFUSE_VOLUME_MOMENT_SIZE;
  constexpr uint32_t strata = 4u;
  static_assert(strata * strata == VKR_BAKE_BRICK_RAYS_PER_TEXEL,
                "Visibility rays fill a square of strata");
  const float32_t minimum_distance = 1.0e-4f;
  *out = Trace{};
  for (uint32_t ty = 0u; ty < interior; ++ty) {
    for (uint32_t tx = 0u; tx < interior; ++tx) {
      float64_t sum = 0.0;
      float64_t sum_squares = 0.0;
      for (uint32_t r = 0u; r < VKR_BAKE_BRICK_RAYS_PER_TEXEL; ++r) {
        const uint32_t ray_seed = seed ^ mix((ty * interior + tx) * 64u + r);
        const float32_t sx = ((float32_t)(r % strata) + unit(ray_seed)) /
                             (float32_t)strata;
        const float32_t sy = ((float32_t)(r / strata) +
                              unit(ray_seed ^ 0x9e3779b9u)) /
                             (float32_t)strata;
        const float32_t u =
            -1.0f + 2.0f * ((float32_t)tx + sx) / (float32_t)interior;
        const float32_t v =
            -1.0f + 2.0f * ((float32_t)ty + sy) / (float32_t)interior;
        const Vec3 direction = octahedral_direction(u, v);
        VkrBakeHit hit = {};
        float32_t distance = moment_range;
        if (closest_blocking_hit(bvh, materials, position, direction,
                                 minimum_distance, ray_range, &hit)) {
          const VkrBakeTriangle &triangle =
              bvh.geometry.triangles[hit.triangle_index];
          const bool back = !materials[triangle.material_index].double_sided &&
                            vec3_dot(triangle.geometric_normal, direction) >
                                0.0f;
          if (back) {
            ++out->backfaces;
            if (hit.t < out->closest_backface) {
              out->closest_backface = hit.t;
              out->closest_backface_direction = direction;
            }
          } else if (hit.t < out->closest_frontface) {
            out->closest_frontface = hit.t;
            out->closest_frontface_direction = direction;
          }
          distance = Min(hit.t, moment_range);
        }
        sum += distance;
        sum_squares += (float64_t)distance * distance;
      }
      const uint32_t texel = (ty + 1u) * size + tx + 1u;
      out->moments[texel * 2u] =
          (float32_t)(sum / VKR_BAKE_BRICK_RAYS_PER_TEXEL);
      out->moments[texel * 2u + 1u] =
          (float32_t)(sum_squares / VKR_BAKE_BRICK_RAYS_PER_TEXEL);
    }
  }

  /* The border repeats the interior across each octahedral seam, so a
     bilinear tap at the tile's edge reads its true neighbor. */
  auto copy = [out](uint32_t dst_x, uint32_t dst_y, uint32_t src_x,
                    uint32_t src_y) {
    const uint32_t dst = dst_y * size + dst_x;
    const uint32_t src = src_y * size + src_x;
    out->moments[dst * 2u] = out->moments[src * 2u];
    out->moments[dst * 2u + 1u] = out->moments[src * 2u + 1u];
  };
  for (uint32_t i = 1u; i <= interior; ++i) {
    copy(i, 0u, interior + 1u - i, 1u);
    copy(i, interior + 1u, interior + 1u - i, interior);
    copy(0u, i, 1u, interior + 1u - i);
    copy(interior + 1u, i, interior, interior + 1u - i);
  }
  copy(0u, 0u, interior, interior);
  copy(interior + 1u, 0u, 1u, interior);
  copy(0u, interior + 1u, interior, 1u);
  copy(interior + 1u, interior + 1u, 1u, 1u);
}

Vec3 clamp_offset(Vec3 offset, float32_t limit) {
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    offset.elements[axis] = Max(-limit, Min(limit, offset.elements[axis]));
  }
  return offset;
}

/* Moves a probe out of geometry: through its closest back face when it
   sees too many, else away from a front face closer than a tenth of its
   spacing. Moments come from the final position. */
void relocate_probe(const VkrBakeBvh &bvh,
                    const VkrBakeBrickMaterial *materials, Vec3 lattice,
                    float32_t spacing, uint32_t seed, float32_t ray_range,
                    VkrBakeBrickVisibility *out) {
  constexpr uint32_t ray_count = VKR_DIFFUSE_VOLUME_MOMENT_INTERIOR *
                                 VKR_DIFFUSE_VOLUME_MOMENT_INTERIOR *
                                 VKR_BAKE_BRICK_RAYS_PER_TEXEL;
  const float32_t moment_range = 2.0f * spacing;
  const float32_t clearance = 0.1f * spacing;
  const float32_t limit = 0.45f * spacing;
  Vec3 offset = vec3_zero();
  Trace trace;
  for (uint32_t step = 0u;; ++step) {
    trace_probe(bvh, materials, vec3_add(lattice, offset), seed, moment_range,
                ray_range, &trace);
    const float32_t fraction = (float32_t)trace.backfaces / (float32_t)ray_count;
    if (step == VKR_BAKE_BRICK_RELOCATION_STEPS) {
      break;
    }
    Vec3 next = offset;
    if (fraction > VKR_BAKE_BRICK_INSIDE_BACKFACE_FRACTION) {
      next = vec3_add(offset,
                      vec3_scale(trace.closest_backface_direction,
                                 trace.closest_backface + clearance));
    } else if (trace.closest_frontface < clearance) {
      next = vec3_sub(offset,
                      vec3_scale(trace.closest_frontface_direction,
                                 clearance - trace.closest_frontface));
    } else {
      break;
    }
    next = clamp_offset(next, limit);
    if (vec3_dot(vec3_sub(next, offset), vec3_sub(next, offset)) <
        1.0e-10f) {
      break;
    }
    offset = next;
  }
  out->offset = offset;
  out->backface_fraction = (float32_t)trace.backfaces / (float32_t)ray_count;
  out->valid =
      out->backface_fraction <= VKR_BAKE_BRICK_INSIDE_BACKFACE_FRACTION;
  for (uint32_t i = 0u; i < ArrayCount(trace.moments); ++i) {
    out->moments[i] = trace.moments[i];
  }
}

} // namespace

bool8_t vkr_bake_bricks_place(const VkrBakeBvh *bvh,
                              const VkrBakeBrickMaterial *materials,
                              uint32_t material_count, VkrBakeBrickDesc desc,
                              Arena *arena, VkrBakeBrickLayout *out_layout) {
  if (!out_layout) {
    return false_v;
  }
  *out_layout = VkrBakeBrickLayout{};
  if (!bvh || !arena || !std::isfinite(desc.spacing) ||
      !(desc.spacing > 0.0f) || desc.level_count == 0u ||
      desc.level_count > VKR_DIFFUSE_VOLUME_MAX_LEVELS ||
      !std::isfinite(desc.margin_spans) || desc.margin_spans < 0.0f) {
    return false_v;
  }
  for (uint32_t i = 0u; i < bvh->geometry.triangle_count; ++i) {
    if (bvh->geometry.triangles[i].material_index >= material_count) {
      return false_v;
    }
  }

  Placement placement;
  placement.bvh = bvh;
  placement.materials = materials;
  placement.entry_size =
      (float32_t)(VKR_DIFFUSE_VOLUME_BRICK_SIZE - 1u) * desc.spacing;
  placement.margin_spans = desc.margin_spans;
  /* Probes sit half a spacing off the bounds, so a floor or wall on a
     bounds face lies between probe layers rather than through one. */
  const float32_t inset = 0.5f * desc.spacing;
  placement.origin = vec3_sub(desc.bounds.min, vec3_new(inset, inset, inset));
  const uint32_t block = level_span(desc.level_count - 1u);
  uint64_t entry_count = 1u;
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float64_t extent = (float64_t)desc.bounds.max.elements[axis] -
                             (float64_t)desc.bounds.min.elements[axis];
    if (!std::isfinite(extent) || !(extent > 0.0)) {
      return false_v;
    }
    const float64_t blocks =
        std::ceil((extent + inset) / ((float64_t)placement.entry_size * block));
    if (!(blocks >= 1.0) ||
        blocks * block * 3.0 + 4.0 >= (float64_t)lattice_limit) {
      return false_v;
    }
    placement.dimensions[axis] = (uint32_t)blocks * block;
    entry_count *= placement.dimensions[axis];
    if (entry_count > VKR_DIFFUSE_VOLUME_MAX_ENTRIES) {
      return false_v;
    }
  }
  placement.entries.assign((size_t)entry_count, VKR_DIFFUSE_VOLUME_ENTRY_EMPTY);

  for (uint32_t z = 0u; z < placement.dimensions[2]; z += block) {
    for (uint32_t y = 0u; y < placement.dimensions[1]; y += block) {
      for (uint32_t x = 0u; x < placement.dimensions[0]; x += block) {
        const uint32_t entry[3] = {x, y, z};
        place_block(&placement, entry, desc.level_count - 1u);
      }
    }
  }
  if (placement.overflow) {
    return false_v;
  }

  /* Each distinct (level, lattice) probe is baked once. */
  const uint32_t brick_count = (uint32_t)placement.bricks.size();
  std::vector<VkrBakeBrickProbe> probes;
  std::vector<uint32_t> brick_probes((size_t)brick_count *
                                     VKR_DIFFUSE_VOLUME_BRICK_PROBES);
  std::unordered_map<uint64_t, uint32_t> probe_indices;
  probe_indices.reserve(brick_probes.size());
  for (uint32_t b = 0u; b < brick_count; ++b) {
    const VkrDiffuseVolumeBrick &brick = placement.bricks[b];
    const uint32_t step = level_span(brick.level);
    for (uint32_t i = 0u; i < VKR_DIFFUSE_VOLUME_BRICK_PROBES; ++i) {
      const uint32_t local[3] = {i % 4u, (i / 4u) % 4u, i / 16u};
      VkrBakeBrickProbe probe = {};
      probe.level = brick.level;
      for (uint32_t axis = 0u; axis < 3u; ++axis) {
        probe.lattice[axis] = 3u * brick.entry[axis] + step * local[axis];
      }
      const auto inserted = probe_indices.emplace(
          probe_key(probe.lattice, probe.level), (uint32_t)probes.size());
      if (inserted.second) {
        probes.push_back(probe);
      }
      brick_probes[(size_t)b * VKR_DIFFUSE_VOLUME_BRICK_PROBES + i] =
          inserted.first->second;
    }
  }

  const Scratch scope = scratch_create(arena);
  uint32_t *entries = (uint32_t *)arena_alloc(
      arena, entry_count * sizeof(uint32_t), ARENA_MEMORY_TAG_ARRAY);
  VkrDiffuseVolumeBrick *bricks = (VkrDiffuseVolumeBrick *)arena_alloc(
      arena, (uint64_t)brick_count * sizeof(VkrDiffuseVolumeBrick),
      ARENA_MEMORY_TAG_ARRAY);
  uint32_t *brick_probe_indices = (uint32_t *)arena_alloc(
      arena, brick_probes.size() * sizeof(uint32_t), ARENA_MEMORY_TAG_ARRAY);
  VkrBakeBrickProbe *probe_records = (VkrBakeBrickProbe *)arena_alloc(
      arena, probes.size() * sizeof(VkrBakeBrickProbe), ARENA_MEMORY_TAG_ARRAY);
  if (!entries || !bricks || !brick_probe_indices || !probe_records) {
    scratch_destroy(scope, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }
  MemCopy(entries, placement.entries.data(), entry_count * sizeof(uint32_t));
  MemCopy(bricks, placement.bricks.data(),
          (uint64_t)brick_count * sizeof(VkrDiffuseVolumeBrick));
  MemCopy(brick_probe_indices, brick_probes.data(),
          brick_probes.size() * sizeof(uint32_t));
  MemCopy(probe_records, probes.data(),
          probes.size() * sizeof(VkrBakeBrickProbe));

  *out_layout = VkrBakeBrickLayout{
      placement.origin,
      desc.spacing,
      desc.level_count,
      {placement.dimensions[0], placement.dimensions[1],
       placement.dimensions[2]},
      entries,
      (uint32_t)entry_count,
      bricks,
      brick_count,
      brick_probe_indices,
      probe_records,
      (uint32_t)probes.size(),
  };
  return true_v;
}

Vec3 vkr_bake_bricks_probe_position(const VkrBakeBrickLayout *layout,
                                    uint32_t probe) {
  const VkrBakeBrickProbe &record = layout->probes[probe];
  return vec3_new(
      layout->origin.x + layout->spacing * (float32_t)record.lattice[0],
      layout->origin.y + layout->spacing * (float32_t)record.lattice[1],
      layout->origin.z + layout->spacing * (float32_t)record.lattice[2]);
}

float32_t vkr_bake_bricks_probe_spacing(const VkrBakeBrickLayout *layout,
                                        uint32_t probe) {
  return layout->spacing * (float32_t)level_span(layout->probes[probe].level);
}

bool8_t vkr_bake_bricks_visibility(const VkrBakeBvh *bvh,
                                   const VkrBakeBrickMaterial *materials,
                                   uint32_t material_count,
                                   const VkrBakeBrickLayout *layout,
                                   uint32_t thread_count,
                                   VkrBakeBrickVisibility *out_visibility) {
  if (!bvh || !layout || !out_visibility ||
      (bvh->geometry.triangle_count && (!materials || !material_count))) {
    return false_v;
  }
  /* Rays reach across the scene, so a probe deep inside a closed mass still
     sees its back faces. */
  float32_t ray_range = 1.0f;
  if (bvh->node_count) {
    const Vec3 extent = vec3_sub(bvh->nodes[0].bounds.max,
                                 bvh->nodes[0].bounds.min);
    ray_range = Max(ray_range, std::sqrt(vec3_dot(extent, extent)));
  }
  std::atomic<uint32_t> next_probe{0u};
  auto work = [&]() {
    for (;;) {
      const uint32_t probe = next_probe.fetch_add(1u);
      if (probe >= layout->probe_count) {
        return;
      }
      const VkrBakeBrickProbe &record = layout->probes[probe];
      const uint32_t seed =
          mix(record.lattice[0] ^ mix(record.lattice[1] ^
                                      mix(record.lattice[2] ^
                                          mix(record.level + 0x51ed27u))));
      if (!bvh->node_count) {
        VkrBakeBrickVisibility &visibility = out_visibility[probe];
        visibility = VkrBakeBrickVisibility{};
        visibility.valid = true_v;
        const float32_t range =
            2.0f * vkr_bake_bricks_probe_spacing(layout, probe);
        for (uint32_t t = 0u; t < VKR_DIFFUSE_VOLUME_MOMENT_TEXELS; ++t) {
          visibility.moments[t * 2u] = range;
          visibility.moments[t * 2u + 1u] = range * range;
        }
        continue;
      }
      relocate_probe(*bvh, materials,
                     vkr_bake_bricks_probe_position(layout, probe),
                     vkr_bake_bricks_probe_spacing(layout, probe), seed,
                     ray_range, &out_visibility[probe]);
    }
  };
  uint32_t workers = thread_count;
  if (!workers) {
    workers = Max(1u, std::thread::hardware_concurrency());
  }
  workers = Min(workers, Max(1u, layout->probe_count));
  std::vector<std::thread> threads;
  threads.reserve(workers - 1u);
  for (uint32_t i = 1u; i < workers; ++i) {
    threads.emplace_back(work);
  }
  work();
  for (std::thread &thread : threads) {
    thread.join();
  }
  return true_v;
}

void vkr_bake_bricks_drop_invalid(VkrBakeBrickLayout *layout,
                                  const VkrBakeBrickVisibility *visibility) {
  std::vector<uint32_t> remap(layout->brick_count,
                              VKR_DIFFUSE_VOLUME_ENTRY_EMPTY);
  uint32_t kept = 0u;
  for (uint32_t b = 0u; b < layout->brick_count; ++b) {
    const uint32_t *probes =
        layout->brick_probes + (size_t)b * VKR_DIFFUSE_VOLUME_BRICK_PROBES;
    bool any_valid = false;
    for (uint32_t i = 0u; i < VKR_DIFFUSE_VOLUME_BRICK_PROBES && !any_valid;
         ++i) {
      any_valid = visibility[probes[i]].valid;
    }
    if (!any_valid) {
      continue;
    }
    remap[b] = kept;
    layout->bricks[kept] = layout->bricks[b];
    MemCopy(layout->brick_probes + (size_t)kept * VKR_DIFFUSE_VOLUME_BRICK_PROBES,
            probes, VKR_DIFFUSE_VOLUME_BRICK_PROBES * sizeof(uint32_t));
    ++kept;
  }
  layout->brick_count = kept;
  for (uint32_t i = 0u; i < layout->entry_count; ++i) {
    const uint32_t entry = layout->entries[i];
    if (entry == VKR_DIFFUSE_VOLUME_ENTRY_EMPTY) {
      continue;
    }
    const uint32_t brick = remap[entry & VKR_DIFFUSE_VOLUME_ENTRY_BRICK_MASK];
    layout->entries[i] =
        brick == VKR_DIFFUSE_VOLUME_ENTRY_EMPTY
            ? VKR_DIFFUSE_VOLUME_ENTRY_EMPTY
            : (entry & ~VKR_DIFFUSE_VOLUME_ENTRY_BRICK_MASK) | brick;
  }
}
