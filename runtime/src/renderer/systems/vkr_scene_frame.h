#pragma once

#include "math/mat.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "vkr_frame_input.h"
#include "vkr_visibility.h"

typedef struct VkrMeshManager VkrMeshManager;
typedef struct VkrMaterialSystem VkrMaterialSystem;
struct VkrSceneRoomFrame;
struct VkrScenePopulationFrame;

/** What hides draws from the camera this frame besides their distance:
    rooms the portals do not show (vkr_scene_rooms.h), and scatter copies
    past their proxy distance or the merged cells standing in for near ones
    (vkr_scene_population_mark_proxies). Either may be NULL. */
typedef struct VkrSceneDrawGate {
  const struct VkrSceneRoomFrame *rooms;
  const struct VkrScenePopulationFrame *proxies;
} VkrSceneDrawGate;

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
 * candidates and this function to the blend draws, by metres only. Under
 * `gate`'s rooms (vkr_scene_rooms.h), draws in rooms the camera does not see
 * this frame carry the hidden bit and draws in sealed rooms the sealed bit; so
 * do far scatter copies and near proxy cells under `gate`'s proxies.
 */
VkrRendererError vkr_scene_build_world_draws(
    VkrMeshManager *meshes, VkrMaterialSystem *materials,
    bool8_t publication_pending, uint64_t publication_generation, Mat4 view,
    Mat4 projection, Vec3 view_position, float32_t draw_distance_scale,
    const VkrSceneDrawGate *gate, bool8_t transmission_blended,
    bool8_t static_only, VkrAllocator *scratch,
    VkrWorldPassPayload *out_payload, VkrVisibilityStats *out_stats);

/** How the camera's tests sort one payload's camera candidates
    (view.cull_report): outside the frustum, hidden (rooms the portals do
    not show, or scatter copies a proxy cell draws in place of), past their
    draw distance, below their smallest drawn size, or drawn. Drawn rows
    still face HZB occlusion. */
typedef struct VkrSceneCullReport {
  uint32_t candidates;
  uint32_t outside_view;
  uint32_t hidden;
  uint32_t distance;
  uint32_t size;
  uint32_t drawn;
  float32_t distance_scale;
} VkrSceneCullReport;

/** Sorts `world`'s camera candidates as the classify kernels do, through
    the host mirror of their camera test (vkr_gpu_draw_distance_reason), for
    a camera of `view_projection` at `view_position` whose image is
    `image_rows` pixels high. */
void vkr_scene_cull_report(const VkrWorldPassPayload *world,
                           Mat4 view_projection, float32_t image_rows,
                           Vec3 view_position, VkrSceneCullReport *out);
