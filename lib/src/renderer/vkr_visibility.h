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
#include "renderer/vkr_render_packet.h"

/** Object is inside the camera volume and belongs in the world draw list. */
#define VKR_VISIBLE_CAMERA 0x1u
/** Object is inside the light volume and belongs in the shadow caster list. */
#define VKR_VISIBLE_SHADOW 0x2u

/**
 * Independent world and shadow routing derived from one material alpha mode.
 */
typedef struct VkrDrawAlphaRouting {
  bool8_t world_transparent;
  bool8_t shadow_alpha_tested;
} VkrDrawAlphaRouting;

/**
 * @brief Maps material alpha semantics to the two draw-list decisions.
 *
 * Blending controls the transparent world list. Cutout controls the
 * alpha-tested shadow list. All non-cutout shadow casters use the opaque
 * shadow path, independently of world blending.
 */
static INLINE VkrDrawAlphaRouting
vkr_draw_alpha_routing(VkrMaterialAlphaMode alpha_mode) {
  return (VkrDrawAlphaRouting){
      .world_transparent =
          alpha_mode == VKR_MATERIAL_ALPHA_BLEND ? true_v : false_v,
      .shadow_alpha_tested =
          alpha_mode == VKR_MATERIAL_ALPHA_CUTOUT ? true_v : false_v,
  };
}

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
  /** Objects kept because their bounds were not ready; conservative, not free.
   */
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
  /** Opaque draws actually emitted after merging, and the count before it. */
  uint32_t opaque_draws_emitted;
  uint32_t opaque_draws_before_merge;
  /**
   * Distinct geometry buffers and materials across the opaque list. These say
   * *why* batching does or does not pay: indirect submission needs draws that
   * share both, so a list with as many materials as draws can never batch
   * however it is submitted.
   */
  uint32_t distinct_geometries;
  /** Distinct geometry/material pairs after key sorting (not unique handles).
   */
  uint32_t distinct_geometry_material_pairs;
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
  /**
   * Per-draw binding state not represented by geometry/material handles.
   * Zero means freely compatible. A unique value prevents merging when state
   * is position-dependent (currently local reflection-probe descriptors).
   */
  uint64_t binding_context;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t domain;
} VkrDrawMergeKey;

/** @brief qsort comparator establishing a total order over merge keys. */
int vkr_draw_merge_key_compare(const void *lhs, const void *rhs);

/**
 * @brief One prospective draw, before merging decides how many draws to emit.
 *
 * Collected during scene traversal so the emission order can be chosen after
 * the fact: merging requires draws sharing a key to be adjacent *and* their
 * instance records contiguous, which is only possible once every candidate is
 * known.
 */
typedef struct VkrDrawCandidate {
  VkrDrawMergeKey key;
  Mat4 model;
  VkrMeshHandle mesh;
  uint32_t submesh_index;
  uint32_t object_id;
  /** Back-to-front ordering for transparent draws; unused when opaque. */
  uint64_t sort_key;
} VkrDrawCandidate;

/** @brief Orders candidates by merge key so equal keys become adjacent runs. */
int vkr_draw_candidate_key_compare(const void *lhs, const void *rhs);

/** @brief Orders transparent candidates back to front. */
int vkr_draw_candidate_depth_compare(const void *lhs, const void *rhs);

/**
 * @brief Merges key-adjacent candidates into instanced draws.
 *
 * Sorts @p candidates by merge key, then emits one VkrDrawItem per run with
 * `instance_count` set to the run length. Instance records are written in the
 * same order, so each run's records are contiguous starting at its
 * `first_instance` -- the property that makes a single instanced draw legal.
 *
 * @param instance_base Index in @p out_instances where this list starts, so
 *        several lists can share one instance array.
 * @param out_draw_count Receives the number of draws emitted (<= @p count).
 * @param stats Optional merge measurements derived from the same sort/run scan.
 * @return Number of instance records written, always @p count.
 */
uint32_t vkr_draw_merge_candidates(VkrDrawCandidate *candidates, uint32_t count,
                                   uint32_t instance_base,
                                   VkrDrawItem *out_draws,
                                   uint32_t *out_draw_count,
                                   VkrInstanceDataGPU *out_instances,
                                   VkrVisibilityStats *stats);

/**
 * @brief Emits candidates one draw each, preserving their current order.
 *
 * Used for transparent and alpha-tested lists, where submission order carries
 * meaning (depth sorting) or per-draw descriptor state changes, so merging
 * would be incorrect rather than merely unhelpful.
 */
uint32_t vkr_draw_emit_unmerged(const VkrDrawCandidate *candidates,
                                uint32_t count, uint32_t instance_base,
                                VkrDrawItem *out_draws,
                                VkrInstanceDataGPU *out_instances);

/**
 * @brief Measures opaque merge opportunity for diagnostics and tests.
 *
 * Sorts @p keys in place and records run lengths into @p stats. Production
 * packet construction gathers the same counters while merging candidates so
 * the hot path does not allocate and sort a second key array.
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
 * Shadow visibility is the union of every cascade volume; cascades have
 * different centers and are not generally nested. A NULL camera, or a NULL/
 * empty shadow array, means "no culling for that view". An object whose bounds
 * are not yet valid is treated as visible to both — being conservative costs a
 * draw, being wrong drops geometry.
 *
 * @return Bitwise OR of VKR_VISIBLE_CAMERA and VKR_VISIBLE_SHADOW.
 */
uint8_t vkr_visibility_classify(const VkrFrustum *camera_frustum,
                                const VkrFrustum *shadow_frustums,
                                uint32_t shadow_frustum_count,
                                bool8_t bounds_valid, Vec3 center,
                                float32_t radius, VkrVisibilityStats *stats);
