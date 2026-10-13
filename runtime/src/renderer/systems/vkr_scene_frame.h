#pragma once

#include "math/mat.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "vkr_frame_input.h"
#include "vkr_visibility.h"

typedef struct VkrMeshManager VkrMeshManager;
typedef struct VkrMaterialSystem VkrMaterialSystem;

/** Current loaded caster bounds used before shadow cascade fitting. */
void vkr_scene_measure_caster_bounds(VkrMeshManager *meshes,
                                     VkrShadowCasterDepthBounds *out_bounds);

/**
 * Build world candidates and ordinary blend draws in frame scratch. Empty
 * scenes succeed with zero counts. The caller retains scratch until submission
 * returns. Candidate identity, static/dynamic partitions, and encounter order
 * match the mesh owner's published generations. With `transmission_blended`
 * (the tiled pipeline, ADR-087), transmissive sources join the camera-culled,
 * back-to-front blend draws instead of the transmission candidates. With
 * `static_only` (a reflection-probe capture, ADR-019), meshes and instances
 * of DYNAMIC mobility and every alpha-blended draw stay out; glass stays.
 * `draw_distance_scale` multiplies each source's draw distance for the camera
 * (VkrWorldPassPayload::draw_distance_scale); the GPU applies it to the
 * candidates and this function to the blend draws, by metres only.
 */
VkrRendererError vkr_scene_build_world_draws(
    VkrMeshManager *meshes, VkrMaterialSystem *materials,
    bool8_t publication_pending, uint64_t publication_generation, Mat4 view,
    Mat4 projection, Vec3 view_position, float32_t draw_distance_scale,
    bool8_t transmission_blended, bool8_t static_only, VkrAllocator *scratch,
    VkrWorldPassPayload *out_payload, VkrVisibilityStats *out_stats);
