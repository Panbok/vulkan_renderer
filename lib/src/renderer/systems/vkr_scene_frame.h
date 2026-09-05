#pragma once

#include "math/mat.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/vkr_frame_input.h"
#include "renderer/vkr_visibility.h"

typedef struct VkrMeshManager VkrMeshManager;
typedef struct VkrMaterialSystem VkrMaterialSystem;

/** Current loaded caster bounds used before shadow cascade fitting. */
void vkr_scene_measure_caster_bounds(VkrMeshManager *meshes,
                                     VkrShadowCasterDepthBounds *out_bounds);

/**
 * Build world candidates and ordinary blend draws in frame scratch. Empty
 * scenes succeed with zero counts. The caller retains scratch until submission
 * returns. Candidate identity, static/dynamic partitions, and encounter order
 * match the mesh owner's published generations.
 */
VkrRendererError vkr_scene_build_world_draws(
    VkrMeshManager *meshes, VkrMaterialSystem *materials,
    bool8_t publication_pending, uint64_t publication_generation, Mat4 view,
    Mat4 projection, VkrAllocator *scratch, VkrWorldPassPayload *out_payload,
    VkrVisibilityStats *out_stats);
