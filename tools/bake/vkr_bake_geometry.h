#pragma once

#include <stdint.h>

#include "math/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One source vertex after bake-scene flattening. */
typedef struct VkrBakeVertex {
  Vec3 position;
  Vec3 normal;
  Vec2 uv;
  Vec4 color;
  /* Cooked world tangent and handedness, zero only for procedural inputs. */
  Vec4 tangent;
} VkrBakeVertex;

/**
 * Caller-owned triangle storage. BVH construction partitions this array once
 * and writes `geometric_normal` after validating the three positions.
 */
typedef struct VkrBakeTriangle {
  VkrBakeVertex vertex[3];
  Vec3 geometric_normal;
  uint32_t material_index;
  uint32_t source_instance_index;
  uint32_t flags;
} VkrBakeTriangle;

typedef struct VkrBakeAabb {
  Vec3 min;
  Vec3 max;
} VkrBakeAabb;

typedef struct VkrBakeRay {
  Vec3 origin;
  Vec3 direction;
  float32_t t_min;
  float32_t t_max;
} VkrBakeRay;

typedef struct VkrBakeHit {
  float32_t t;
  float32_t bary_u;
  float32_t bary_v;
  uint32_t triangle_index;
  bool8_t front_face;
} VkrBakeHit;

/** Mutable caller-owned triangle span consumed by the BVH builder. */
typedef struct VkrBakeGeometry {
  VkrBakeTriangle *triangles;
  uint32_t triangle_count;
} VkrBakeGeometry;

#ifdef __cplusplus
}
#endif
