#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include "memory/arena.h"
#ifdef __cplusplus
}
#endif
#include "vkr_bake_geometry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VKR_BAKE_BVH_MAX_TRIANGLES 8000000u
#define VKR_BAKE_BVH_LEAF_TRIANGLE_COUNT 4u
#define VKR_BAKE_BVH_MAX_DEPTH 128u
#define VKR_BAKE_BVH_BIN_COUNT 16u
#define VKR_BAKE_BVH_INVALID_NODE UINT32_MAX

/**
 * A leaf has `left_child == VKR_BAKE_BVH_INVALID_NODE` and owns the contiguous
 * triangle range `[first_triangle, first_triangle + triangle_count)`. Internal
 * nodes own two child indices and have a zero triangle count.
 */
typedef struct VkrBakeBvhNode {
  VkrBakeAabb bounds;
  uint32_t left_child;
  uint32_t right_child;
  uint32_t first_triangle;
  uint32_t triangle_count;
} VkrBakeBvhNode;

/**
 * `geometry.triangles` remains owned by the caller. `nodes` is borrowed from
 * the caller's bake-lifetime Arena and remains valid until that Arena resets
 * or is destroyed.
 */
typedef struct VkrBakeBvh {
  VkrBakeGeometry geometry;
  VkrBakeBvhNode *nodes;
  uint32_t node_count;
} VkrBakeBvh;

/**
 * Validates finite, non-degenerate triangles, writes geometric normals, then
 * partitions `geometry.triangles` in place into deterministic SAH leaves.
 * The caller owns triangle bytes; `arena` owns one pre-sized node allocation.
 */
bool8_t vkr_bake_bvh_build(VkrBakeGeometry geometry, Arena *arena,
                           VkrBakeBvh *out_bvh);

/**
 * Closest two-sided triangle query. `bvh` must be the successful output of
 * `vkr_bake_bvh_build`; the query allocates no memory.
 */
bool8_t vkr_bake_bvh_intersect_closest(const VkrBakeBvh *bvh, VkrBakeRay ray,
                                       VkrBakeHit *out_hit);

#ifdef __cplusplus
}
#endif
