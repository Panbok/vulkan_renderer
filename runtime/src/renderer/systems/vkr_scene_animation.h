#pragma once

#include "animation/vkr_animation_graph.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_system.h"
#include "vkr_frame_input.h"

typedef struct VkrSceneAnimationConfig {
  /* Optional versioned controller JSON, borrowed only through attach. */
  String8 controller_json;
  uint32_t clip;
  float64_t rate;
  bool8_t loop;
  bool8_t playing;
} VkrSceneAnimationConfig;

#define VKR_SCENE_ANIMATION_CONFIG_DEFAULT                                     \
  (VkrSceneAnimationConfig) { .rate = 1.0, .loop = true_v, .playing = true_v }

/* Attach only READY mesh and bank requests. On success the scene takes both
 * requests and clears the caller handles; failure leaves ownership unchanged.
 * Copies the original-node -> ECS mapping. The bank and mesh result remain
 * retained until detach/shutdown, independent of the mesh manager's GPU refs.
 * Pose evaluation does not replace authored ECS transforms; rendering consumes
 * the independently evaluated skin palettes.
 * Bind against unedited source-local transforms; edit the wrapper to place it.
 */
bool8_t vkr_scene_animation_attach(VkrScene *scene, VkrEntityId wrapper,
                                   VkrResourceHandleInfo *mesh_request,
                                   VkrResourceHandleInfo *animation_request,
                                   const VkrEntityId *source_nodes,
                                   uint32_t node_count,
                                   const VkrSceneAnimationConfig *config,
                                   VkrAllocator *scratch, const char **error);
void vkr_scene_animation_detach(VkrScene *scene, VkrEntityId wrapper);

/* Borrowed until detach, wrapper/target deletion or invalidating hierarchy/rest
 * edits, including edits detected by the next scene update. Do not destroy a
 * scene-owned player directly. */
VkrAnimationPlayer *vkr_scene_animation_get_player(const VkrScene *scene,
                                                   VkrEntityId wrapper);

/* Copies a validated controller and evaluates its initial pose transactionally.
 * NULL removes the controller and resumes the selected clip. Repeated apply
 * reuses scene-owned capacity; storage expires at detach. */
bool8_t vkr_scene_animation_apply_graph(const VkrScene *scene,
                                        VkrEntityId wrapper,
                                        const VkrAnimationGraph *graph,
                                        const char **error);
const VkrAnimationGraphInstance *
vkr_scene_animation_get_graph(const VkrScene *scene, VkrEntityId wrapper);
bool8_t vkr_scene_animation_seek(const VkrScene *scene, VkrEntityId wrapper,
                                 float64_t seconds);
bool8_t vkr_scene_animation_set_parameter(const VkrScene *scene,
                                          VkrEntityId wrapper,
                                          uint32_t parameter, float32_t value);

/* Scene lifecycle entry points. No evaluated transforms enter the edit journal.
 * Failed sampling pauses that player and retains the last good pose. */
void vkr_scene_animation_update(VkrScene *scene, float64_t dt);
void vkr_scene_animation_shutdown(VkrScene *scene);
void vkr_scene_animation_entity_destroying(VkrScene *scene, VkrEntityId entity);

void vkr_scene_animation_sync_render(VkrScene *scene,
                                     struct VkrRenderAssets *assets);

/** Append independent preview jobs to this frame's world arrays in scratch. */
bool8_t vkr_scene_animation_build_preview(
    const VkrScene *scene, VkrEntityId wrapper,
    const VkrAnimationPlayer *player, float32_t yaw, float32_t pitch,
    float32_t distance, VkrAllocator *scratch, VkrWorldPassPayload *world,
    VkrAnimationPreviewInput *preview);
