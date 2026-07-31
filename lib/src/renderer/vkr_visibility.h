/**
 * @file vkr_visibility.h
 * @brief Per-frame visibility classification and draw-merge measurement.
 *
 * Draw-list construction decides two things every frame: which objects are
 * visible, and which of the resulting draws could be collapsed into one
 * instanced draw. Both are pure functions of the scene's bounds and material
 * assignment, so they live here rather than in the application header — they
 * are testable without a window, a device, or an event loop.
 */
#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_frustum.h"

/** Object is inside the camera volume and belongs in the world draw list. */
#define VKR_VISIBLE_CAMERA 0x1u
/** Object is inside the light volume and belongs in the shadow caster list. */
#define VKR_VISIBLE_SHADOW 0x2u

/**
 * @brief Per-frame visibility and merge-opportunity counters.
 *
 * These exist so throughput claims are measurements rather than assumptions:
 * culling that rejects nothing and instancing that merges nothing both look
 * identical to a frame-time graph full of noise.
 */
typedef struct VkrVisibilityStats {
  uint32_t objects_tested;
  uint32_t objects_culled_camera;
  uint32_t objects_culled_shadow;
  /** Objects kept because their bounds were not ready; conservative, not free. */
  uint32_t objects_without_bounds;

  /**
   * Opaque draws sharing a merge key with an earlier draw — the number that
   * instancing could collapse. Two draws are mergeable only when they are the
   * same submesh of the same geometry with the same material, i.e. one asset
   * drawn more than once.
   */
  uint32_t mergeable_opaque_draws;
  uint32_t distinct_opaque_keys;
  /** Longest run of identical keys; 1 means nothing can be merged at all. */
  uint32_t largest_mergeable_run;
} VkrVisibilityStats;

/**
 * @brief Identity a draw must share to be merged into one instanced draw.
 *
 * Pipeline domain is included because two draws with the same geometry and
 * material still cannot merge across a pipeline change.
 */
typedef struct VkrDrawMergeKey {
  uint64_t geometry;
  uint64_t material;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t domain;
} VkrDrawMergeKey;

/** @brief qsort comparator establishing a total order over merge keys. */
int vkr_draw_merge_key_compare(const void *lhs, const void *rhs);

/**
 * @brief Measures how many opaque draws instancing could collapse.
 *
 * Sorts @p keys in place and records run lengths into @p stats. This is the
 * input to the instancing and multi-draw-indirect decisions: a scene whose runs
 * are all length one has nothing for either to merge, however draws are
 * submitted.
 */
void vkr_draw_measure_merge_opportunity(VkrDrawMergeKey *keys, uint32_t count,
                                        VkrVisibilityStats *stats);

/**
 * @brief Conservative world-space bounding sphere for a local-space AABB.
 *
 * The radius uses the largest column length of the model matrix, matching how
 * mesh world bounds are derived: under non-uniform scale it over-estimates
 * rather than clipping geometry that should be drawn.
 */
void vkr_visibility_submesh_sphere(Mat4 model, Vec3 center, Vec3 min_extents,
                                   Vec3 max_extents, Vec3 *out_center,
                                   float32_t *out_radius);

/**
 * @brief Classifies one bounding sphere against the camera and light volumes.
 *
 * A NULL frustum means "no culling for that view". An object whose bounds are
 * not yet valid is treated as visible to both — being conservative costs a
 * draw, being wrong drops geometry.
 *
 * @return Bitwise OR of VKR_VISIBLE_CAMERA and VKR_VISIBLE_SHADOW.
 */
uint8_t vkr_visibility_classify(const VkrFrustum *camera_frustum,
                                const VkrFrustum *shadow_frustum,
                                bool8_t bounds_valid, Vec3 center,
                                float32_t radius, VkrVisibilityStats *stats);
