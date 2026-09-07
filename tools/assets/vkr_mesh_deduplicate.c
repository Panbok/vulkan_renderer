#include "assets/vkr_mesh_deduplicate.h"

#include "math/vec.h"
#include "math/vkr_math.h"

static bool8_t vkr_mesh_cook_vertex_equal(const VkrVertex3d *lhs,
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

static uint32_t vkr_mesh_cook_vertex_hash(const VkrVertex3d *vertex) {
  const float32_t scale = 1000.0f;
  const int32_t px = (int32_t)(vertex->position.x * scale);
  const int32_t py = (int32_t)(vertex->position.y * scale);
  const int32_t pz = (int32_t)(vertex->position.z * scale);
  const int32_t nx = (int32_t)(vertex->normal.x * 100.0f);
  const int32_t ny = (int32_t)(vertex->normal.y * 100.0f);
  const int32_t nz = (int32_t)(vertex->normal.z * 100.0f);
  const int32_t tu = (int32_t)(vertex->texcoord.u * 10000.0f);
  const int32_t tv = (int32_t)(vertex->texcoord.v * 10000.0f);
  uint32_t hash = 2166136261u;
  hash = (hash ^ (uint32_t)px) * 16777619u;
  hash = (hash ^ (uint32_t)py) * 16777619u;
  hash = (hash ^ (uint32_t)pz) * 16777619u;
  hash = (hash ^ (uint32_t)nx) * 16777619u;
  hash = (hash ^ (uint32_t)ny) * 16777619u;
  hash = (hash ^ (uint32_t)nz) * 16777619u;
  hash = (hash ^ (uint32_t)tu) * 16777619u;
  return (hash ^ (uint32_t)tv) * 16777619u;
}

bool8_t vkr_mesh_cook_deduplicate_vertices(
    VkrAllocator *scratch_allocator, const VkrVertex3d *vertices,
    uint32_t vertex_count, uint32_t *indices, uint32_t index_count,
    VkrVertex3d **out_vertices, uint32_t *out_vertex_count) {
  if (!scratch_allocator || !vertices || !indices || !out_vertices ||
      !out_vertex_count || vertex_count == 0u ||
      vertex_count > UINT32_MAX / 2u) {
    return false_v;
  }
  const uint32_t table_size = Max(1024u, vertex_count * 2u);
  uint32_t *hash_table = vkr_allocator_alloc(
      scratch_allocator, (uint64_t)table_size * sizeof(*hash_table),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrVertex3d *unique = vkr_allocator_alloc(
      scratch_allocator, (uint64_t)vertex_count * sizeof(*unique),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *remap = vkr_allocator_alloc(
      scratch_allocator, (uint64_t)vertex_count * sizeof(*remap),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!hash_table || !unique || !remap) {
    return false_v;
  }
  for (uint32_t i = 0u; i < table_size; ++i) {
    hash_table[i] = UINT32_MAX;
  }
  uint32_t unique_count = 0u;
  for (uint32_t i = 0u; i < vertex_count; ++i) {
    const uint32_t bucket = vkr_mesh_cook_vertex_hash(&vertices[i]) % table_size;
    bool8_t found = false_v;
    for (uint32_t probe = 0u; probe < table_size; ++probe) {
      const uint32_t index = (bucket + probe) % table_size;
      if (hash_table[index] == UINT32_MAX) {
        hash_table[index] = unique_count;
        unique[unique_count] = vertices[i];
        remap[i] = unique_count++;
        found = true_v;
        break;
      }
      if (vkr_mesh_cook_vertex_equal(&vertices[i],
                                     &unique[hash_table[index]])) {
        remap[i] = hash_table[index];
        found = true_v;
        break;
      }
    }
    if (!found) {
      return false_v;
    }
  }
  for (uint32_t i = 0u; i < index_count; ++i) {
    if (indices[i] >= vertex_count) {
      return false_v;
    }
    indices[i] = remap[indices[i]];
  }
  *out_vertices = unique;
  *out_vertex_count = unique_count;
  return true_v;
}
