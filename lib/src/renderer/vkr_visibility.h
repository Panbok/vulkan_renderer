/**
 * @file vkr_visibility.h
 * @brief Feature-local ordinary-blend visibility and ordering helpers.
 */
#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "renderer/vkr_render_packet.h"

/** Independent world-blend and shadow-cutout routing from material alpha. */
typedef struct VkrDrawAlphaRouting {
  bool8_t world_transparent;
  bool8_t shadow_alpha_tested;
} VkrDrawAlphaRouting;

static INLINE VkrDrawAlphaRouting
vkr_draw_alpha_routing(VkrMaterialAlphaMode alpha_mode) {
  return (VkrDrawAlphaRouting){
      .world_transparent =
          alpha_mode == VKR_MATERIAL_ALPHA_BLEND ? true_v : false_v,
      .shadow_alpha_tested =
          alpha_mode == VKR_MATERIAL_ALPHA_CUTOUT ? true_v : false_v,
  };
}

/** Maps the two pipeline-affecting material properties to a stable bucket. */
static INLINE VkrWorldDrawStateBucket vkr_world_draw_state_bucket(
    VkrMaterialAlphaMode alpha_mode, bool8_t double_sided) {
  const bool8_t cutout =
      alpha_mode == VKR_MATERIAL_ALPHA_CUTOUT ? true_v : false_v;
  if (cutout) {
    return double_sided ? VKR_WORLD_DRAW_STATE_CUTOUT_DOUBLE_SIDED
                        : VKR_WORLD_DRAW_STATE_CUTOUT_BACK;
  }
  return double_sided ? VKR_WORLD_DRAW_STATE_OPAQUE_DOUBLE_SIDED
                      : VKR_WORLD_DRAW_STATE_OPAQUE_BACK;
}

/** Per-frame source-row and retained ordinary-blend visibility counters. */
typedef struct VkrVisibilityStats {
  uint32_t objects_tested;
  uint32_t objects_culled_camera;
  uint32_t objects_without_bounds;
} VkrVisibilityStats;

/** One camera-visible ordinary-blend draw before back-to-front ordering. */
typedef struct VkrTransparentDrawCandidate {
  VkrInstanceDataGPU instance;
  VkrMeshHandle mesh;
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  uint32_t submesh_index;
  uint64_t sort_key;
} VkrTransparentDrawCandidate;

/** Orders ordinary-blend candidates back to front. */
int vkr_transparent_draw_depth_compare(const void *lhs, const void *rhs);

/** Emits one retained draw and instance row per ordinary-blend candidate. */
uint32_t
vkr_transparent_draw_emit(const VkrTransparentDrawCandidate *candidates,
                          uint32_t count, VkrDrawItem *out_draws,
                          VkrInstanceDataGPU *out_instances);

/** Conservative world-space bounding sphere for a local-space AABB. */
void vkr_visibility_submesh_sphere(Mat4 model, Vec3 center, Vec3 min_extents,
                                   Vec3 max_extents, Vec3 *out_center,
                                   float32_t *out_radius);
