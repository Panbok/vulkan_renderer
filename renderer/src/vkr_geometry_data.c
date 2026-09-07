#include "vkr_geometry_data.h"

#include "math/vec.h"
#include "math/vkr_math.h"

void vkr_geometry_generate_tangents(VkrAllocator *allocator,
                                    VkrVertex3d *vertices,
                                    uint32_t vertex_count,
                                    const uint32_t *indices,
                                    uint32_t index_count) {
  if (!allocator || !vertices || vertex_count == 0u || !indices ||
      index_count == 0u) {
    return;
  }

  Vec3 *tangent_accumulators = vkr_allocator_alloc(
      allocator, (uint64_t)vertex_count * sizeof(*tangent_accumulators),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  float32_t *handedness_accumulators = vkr_allocator_alloc(
      allocator, (uint64_t)vertex_count * sizeof(*handedness_accumulators),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!tangent_accumulators || !handedness_accumulators) {
    if (tangent_accumulators) {
      vkr_allocator_free(allocator, tangent_accumulators,
                         (uint64_t)vertex_count * sizeof(*tangent_accumulators),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    if (handedness_accumulators) {
      vkr_allocator_free(allocator, handedness_accumulators,
                         (uint64_t)vertex_count *
                             sizeof(*handedness_accumulators),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    return;
  }

  for (uint32_t i = 0u; i < vertex_count; ++i) {
    tangent_accumulators[i] = vec3_zero();
    handedness_accumulators[i] = 0.0f;
  }

  for (uint32_t i = 0u; i + 2u < index_count; i += 3u) {
    const uint32_t i0 = indices[i];
    const uint32_t i1 = indices[i + 1u];
    const uint32_t i2 = indices[i + 2u];
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) {
      continue;
    }

    const VkrVertex3d *v0 = &vertices[i0];
    const VkrVertex3d *v1 = &vertices[i1];
    const VkrVertex3d *v2 = &vertices[i2];
    const Vec3 e1 = vec3_sub(vkr_vertex_unpack_vec3(v1->position),
                             vkr_vertex_unpack_vec3(v0->position));
    const Vec3 e2 = vec3_sub(vkr_vertex_unpack_vec3(v2->position),
                             vkr_vertex_unpack_vec3(v0->position));
    const float32_t delta_u1 = v1->texcoord.u - v0->texcoord.u;
    const float32_t delta_v1 = v1->texcoord.v - v0->texcoord.v;
    const float32_t delta_u2 = v2->texcoord.u - v0->texcoord.u;
    const float32_t delta_v2 = v2->texcoord.v - v0->texcoord.v;
    const float32_t dividend = delta_u1 * delta_v2 - delta_u2 * delta_v1;
    if (vkr_abs_f32(dividend) < VKR_FLOAT_EPSILON) {
      continue;
    }
    const float32_t factor = 1.0f / dividend;
    const Vec3 tangent =
        vec3_normalize(vec3_new(factor * (delta_v2 * e1.x - delta_v1 * e2.x),
                                factor * (delta_v2 * e1.y - delta_v1 * e2.y),
                                factor * (delta_v2 * e1.z - delta_v1 * e2.z)));
    const float32_t handedness =
        (delta_v1 * delta_u2 - delta_v2 * delta_u1 < 0.0f) ? -1.0f : 1.0f;
    tangent_accumulators[i0] = vec3_add(tangent_accumulators[i0], tangent);
    tangent_accumulators[i1] = vec3_add(tangent_accumulators[i1], tangent);
    tangent_accumulators[i2] = vec3_add(tangent_accumulators[i2], tangent);
    handedness_accumulators[i0] += handedness;
    handedness_accumulators[i1] += handedness;
    handedness_accumulators[i2] += handedness;
  }

  for (uint32_t i = 0u; i < vertex_count; ++i) {
    const Vec3 normal = vkr_vertex_unpack_vec3(vertices[i].normal);
    Vec3 tangent = tangent_accumulators[i];
    tangent = vec3_sub(tangent, vec3_scale(normal, vec3_dot(normal, tangent)));
    if (vec3_length_squared(tangent) < VKR_FLOAT_EPSILON * VKR_FLOAT_EPSILON) {
      tangent = vkr_abs_f32(normal.x) > 0.9f ? vec3_new(0.0f, 1.0f, 0.0f)
                                             : vec3_new(1.0f, 0.0f, 0.0f);
      tangent =
          vec3_sub(tangent, vec3_scale(normal, vec3_dot(normal, tangent)));
    }
    tangent = vec3_normalize(tangent);
    vertices[i].tangent = vec3_to_vec4(
        tangent, handedness_accumulators[i] >= 0.0f ? 1.0f : -1.0f);
  }

  vkr_allocator_free(allocator, tangent_accumulators,
                     (uint64_t)vertex_count * sizeof(*tangent_accumulators),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_free(allocator, handedness_accumulators,
                     (uint64_t)vertex_count * sizeof(*handedness_accumulators),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
}
