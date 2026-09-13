#pragma once

#include "containers/str.h"
#include "memory/vkr_allocator.h"

#define VKR_COLLISION_COOKED_VERSION 1u
#define VKR_COLLISION_COOKED_MAX_BYTES MB(64)
#define VKR_COLLISION_MAX_VERTICES 1048576u
#define VKR_COLLISION_MAX_INDICES 3145728u

typedef enum VkrCollisionKind {
  VKR_COLLISION_CONVEX_HULL = 1,
  VKR_COLLISION_TRIANGLE_MESH = 2
} VkrCollisionKind;

typedef struct VkrCollisionGeometry {
  VkrCollisionKind kind;
  const float32_t *positions; /* Packed xyz, immutable. */
  uint32_t vertex_count;
  const uint32_t
      *indices; /* Outward-wound triangles, including hull surface. */
  uint32_t index_count;
  uint64_t source_fingerprint;
} VkrCollisionGeometry;

/* VKC1: explicit little-endian fields, 48-byte header: magic,version,size(u64),
 * source fingerprint(u64),FNV1a checksum(u64, checksum bytes treated as zero),
 * kind,vertex_count,index_count,reserved0. Then xyz float32 and u32 indices.
 * Header, coordinate and index bounds precede allocation; triangle validation
 * also rejects degeneracy. Decoded arrays belong to result allocator; failure
 * leaves output zero and releases any decoded allocation. */
bool8_t vkr_collision_geometry_validate(const VkrCollisionGeometry *geometry,
                                        const char **error);
bool8_t vkr_collision_cooked_decode(VkrAllocator *result, const uint8_t *bytes,
                                    uint64_t size,
                                    VkrCollisionGeometry *geometry,
                                    const char **error);
bool8_t vkr_collision_cooked_encode(VkrAllocator *result,
                                    const VkrCollisionGeometry *geometry,
                                    uint8_t **bytes, uint64_t *size,
                                    const char **error);
