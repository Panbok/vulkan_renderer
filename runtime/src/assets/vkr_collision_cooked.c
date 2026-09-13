#include "assets/vkr_collision_cooked.h"

#include <math.h>

static bool8_t collision_fail(const char **error, const char *message) {
  if (error) {
    *error = message;
  }
  return false_v;
}

static uint32_t collision_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t collision_u64(const uint8_t *p) {
  return collision_u32(p) | ((uint64_t)collision_u32(p + 4) << 32);
}

static void collision_put_u32(uint8_t *p, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) {
    p[i] = (uint8_t)(value >> (i * 8));
  }
}

static void collision_put_u64(uint8_t *p, uint64_t value) {
  collision_put_u32(p, (uint32_t)value);
  collision_put_u32(p + 4, (uint32_t)(value >> 32));
}

static float32_t collision_float(const uint8_t *p) {
  const uint32_t bits = collision_u32(p);
  float32_t result;
  MemCopy(&result, &bits, sizeof(result));
  return result;
}

static uint64_t collision_checksum(const uint8_t *bytes, uint64_t size) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (uint64_t i = 0; i < size; ++i) {
    hash =
        (hash ^ (i >= 24 && i < 32 ? 0 : bytes[i])) * UINT64_C(1099511628211);
  }
  return hash;
}

bool8_t vkr_collision_geometry_validate(const VkrCollisionGeometry *g,
                                        const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!g || !g->positions || !g->indices ||
      (g->kind != VKR_COLLISION_CONVEX_HULL &&
       g->kind != VKR_COLLISION_TRIANGLE_MESH) ||
      g->vertex_count < 3 || g->vertex_count > VKR_COLLISION_MAX_VERTICES ||
      g->index_count < 3 || g->index_count > VKR_COLLISION_MAX_INDICES ||
      g->index_count % 3 ||
      (g->kind == VKR_COLLISION_CONVEX_HULL &&
       (g->vertex_count < 4 || g->vertex_count > 256))) {
    return collision_fail(error, "Invalid collision geometry kind or capacity");
  }
  for (uint32_t i = 0; i < g->vertex_count * 3; ++i) {
    if (!isfinite(g->positions[i]) || fabsf(g->positions[i]) > 1.0e7f) {
      return collision_fail(error, "Invalid collision vertex coordinate");
    }
  }
  for (uint32_t i = 0; i < g->index_count; i += 3) {
    const uint32_t a = g->indices[i];
    const uint32_t b = g->indices[i + 1];
    const uint32_t c = g->indices[i + 2];
    if (a >= g->vertex_count || b >= g->vertex_count || c >= g->vertex_count ||
        a == b || a == c || b == c) {
      return collision_fail(error, "Invalid collision triangle indices");
    }
    float64_t ab[3], ac[3];
    for (uint32_t j = 0; j < 3; ++j) {
      ab[j] = (float64_t)g->positions[b * 3 + j] - g->positions[a * 3 + j];
      ac[j] = (float64_t)g->positions[c * 3 + j] - g->positions[a * 3 + j];
    }
    const float64_t x = ab[1] * ac[2] - ab[2] * ac[1];
    const float64_t y = ab[2] * ac[0] - ab[0] * ac[2];
    const float64_t z = ab[0] * ac[1] - ab[1] * ac[0];
    if (x * x + y * y + z * z <= 1.0e-20) {
      return collision_fail(error, "Degenerate collision triangle");
    }
  }
  if (g->kind == VKR_COLLISION_CONVEX_HULL) {
    if (g->index_count != (g->vertex_count - 2u) * 6u) {
      return collision_fail(
          error, "Convex surface must be a closed triangulated hull");
    }
    float64_t volume6 = 0;
    for (uint32_t i = 0; i < g->index_count; i += 3) {
      const float32_t *a = g->positions + g->indices[i] * 3;
      const float32_t *b = g->positions + g->indices[i + 1] * 3;
      const float32_t *c = g->positions + g->indices[i + 2] * 3;
      const float64_t ab[3] = {(float64_t)b[0] - a[0], (float64_t)b[1] - a[1],
                               (float64_t)b[2] - a[2]};
      const float64_t ac[3] = {(float64_t)c[0] - a[0], (float64_t)c[1] - a[1],
                               (float64_t)c[2] - a[2]};
      const float64_t normal[3] = {ab[1] * ac[2] - ab[2] * ac[1],
                                   ab[2] * ac[0] - ab[0] * ac[2],
                                   ab[0] * ac[1] - ab[1] * ac[0]};
      const float64_t length =
          sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
               normal[2] * normal[2]);
      for (uint32_t v = 0; v < g->vertex_count; ++v) {
        float64_t distance = 0;
        for (uint32_t j = 0; j < 3; ++j) {
          distance += normal[j] * ((float64_t)g->positions[v * 3 + j] - a[j]);
        }
        if (distance > length * 1e-4) {
          return collision_fail(
              error, "Convex surface is concave or has reversed winding");
        }
      }
      float64_t ar[3], br[3], cr[3];
      for (uint32_t j = 0; j < 3; ++j) {
        ar[j] = (float64_t)a[j] - g->positions[j];
        br[j] = (float64_t)b[j] - g->positions[j];
        cr[j] = (float64_t)c[j] - g->positions[j];
      }
      volume6 += ar[0] * (br[1] * cr[2] - br[2] * cr[1]) +
                 ar[1] * (br[2] * cr[0] - br[0] * cr[2]) +
                 ar[2] * (br[0] * cr[1] - br[1] * cr[0]);
      for (uint32_t e = 0; e < 3; ++e) {
        const uint32_t start = g->indices[i + e];
        const uint32_t end = g->indices[i + (e + 1) % 3];
        uint32_t reverse_edges = 0;
        for (uint32_t t = 0; t < g->index_count; t += 3) {
          for (uint32_t k = 0; k < 3; ++k) {
            reverse_edges += g->indices[t + k] == end &&
                             g->indices[t + (k + 1) % 3] == start;
          }
        }
        if (reverse_edges != 1) {
          return collision_fail(error, "Convex surface is not watertight");
        }
      }
    }
    if (volume6 <= 6e-9) {
      return collision_fail(error, "Convex surface has no positive volume");
    }
  }
  return true_v;
}

bool8_t vkr_collision_cooked_decode(VkrAllocator *result, const uint8_t *bytes,
                                    uint64_t size, VkrCollisionGeometry *out,
                                    const char **error) {
  if (out) {
    *out = (VkrCollisionGeometry){0};
  }
  if (!result || !bytes || !out || size < 48 ||
      size > VKR_COLLISION_COOKED_MAX_BYTES || MemCompare(bytes, "VKC1", 4) ||
      collision_u32(bytes + 4) != 1 || collision_u64(bytes + 8) != size ||
      collision_u32(bytes + 44) ||
      collision_checksum(bytes, size) != collision_u64(bytes + 24)) {
    return collision_fail(error, "Invalid or corrupt collision asset header");
  }
  const uint32_t vertices = collision_u32(bytes + 36);
  const uint32_t indices = collision_u32(bytes + 40);
  const uint32_t kind = collision_u32(bytes + 32);
  if (vertices < 3 || vertices > VKR_COLLISION_MAX_VERTICES || indices < 3 ||
      indices > VKR_COLLISION_MAX_INDICES || indices % 3 ||
      (kind != VKR_COLLISION_CONVEX_HULL &&
       kind != VKR_COLLISION_TRIANGLE_MESH) ||
      (kind == VKR_COLLISION_CONVEX_HULL && (vertices < 4 || vertices > 256)) ||
      48 + (uint64_t)vertices * 12 + (uint64_t)indices * 4 != size) {
    return collision_fail(error, "Invalid collision asset dimensions");
  }
  /* Validate serialized numeric values and indices before allocating. */
  for (uint32_t i = 0; i < vertices * 3; ++i) {
    const float32_t value = collision_float(bytes + 48 + i * 4);
    if (!isfinite(value) || fabsf(value) > 1.0e7f) {
      return collision_fail(error, "Invalid collision asset vertex");
    }
  }
  const uint8_t *triangles = bytes + 48 + (uint64_t)vertices * 12;
  for (uint32_t i = 0; i < indices; ++i) {
    if (collision_u32(triangles + i * 4) >= vertices) {
      return collision_fail(error, "Collision asset index out of bounds");
    }
  }
  const uint64_t allocation = (uint64_t)vertices * 12 + (uint64_t)indices * 4;
  float32_t *positions =
      vkr_allocator_alloc(result, allocation, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!positions) {
    return collision_fail(error, "Collision decode allocation failed");
  }
  uint32_t *decoded_indices = (uint32_t *)(positions + vertices * 3);
  for (uint32_t i = 0; i < vertices * 3; ++i) {
    positions[i] = collision_float(bytes + 48 + i * 4);
  }
  for (uint32_t i = 0; i < indices; ++i) {
    decoded_indices[i] = collision_u32(triangles + i * 4);
  }
  VkrCollisionGeometry geometry = {.kind = (VkrCollisionKind)kind,
                                   .positions = positions,
                                   .vertex_count = vertices,
                                   .indices = decoded_indices,
                                   .index_count = indices,
                                   .source_fingerprint =
                                       collision_u64(bytes + 16)};
  if (!vkr_collision_geometry_validate(&geometry, error)) {
    vkr_allocator_free(result, positions, allocation,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }
  *out = geometry;
  return true_v;
}

bool8_t vkr_collision_cooked_encode(VkrAllocator *result,
                                    const VkrCollisionGeometry *g,
                                    uint8_t **bytes, uint64_t *size,
                                    const char **error) {
  if (bytes) {
    *bytes = NULL;
  }
  if (size) {
    *size = 0;
  }
  if (!result || !bytes || !size ||
      !vkr_collision_geometry_validate(g, error)) {
    return false_v;
  }
  const uint64_t length =
      48 + (uint64_t)g->vertex_count * 12 + (uint64_t)g->index_count * 4;
  uint8_t *output =
      vkr_allocator_alloc(result, length, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!output) {
    return collision_fail(error, "Collision encode allocation failed");
  }
  MemZero(output, 48);
  MemCopy(output, "VKC1", 4);
  collision_put_u32(output + 4, VKR_COLLISION_COOKED_VERSION);
  collision_put_u64(output + 8, length);
  collision_put_u64(output + 16, g->source_fingerprint);
  collision_put_u32(output + 32, g->kind);
  collision_put_u32(output + 36, g->vertex_count);
  collision_put_u32(output + 40, g->index_count);
  for (uint32_t i = 0; i < g->vertex_count * 3; ++i) {
    uint32_t bits;
    MemCopy(&bits, g->positions + i, sizeof(bits));
    collision_put_u32(output + 48 + i * 4, bits);
  }
  for (uint32_t i = 0; i < g->index_count; ++i) {
    collision_put_u32(output + 48 + (uint64_t)g->vertex_count * 12 + i * 4,
                      g->indices[i]);
  }
  collision_put_u64(output + 24, collision_checksum(output, length));
  *bytes = output;
  *size = length;
  return true_v;
}
