/**
 * @file vkr_scene_model.h
 * @brief Cooked models spawned into a live scene at runtime.
 *
 * Gameplay spawns a model, such as a character's mesh and animation bank,
 * under an existing entity the way the scene loader instantiates an entity
 * with `mesh` and `animation` blocks, but synchronously and outside any
 * document. The scene owns what a spawn created (the source-node entities,
 * their mesh instances and the resource requests) until
 * vkr_scene_despawn_model or scene shutdown. Spawned nodes carry no document
 * index, so edit overlays and saves never bind to them.
 */
#pragma once

#include "renderer/systems/vkr_scene_animation.h"

typedef struct VkrSceneModelDesc {
  /** Cooked .vkb; bare paths resolve against the content root. */
  String8 mesh_path;
  /** Optional .vka cooked from the same source; empty for a static model. */
  String8 animation_path;
  VkrSceneAnimationConfig animation;
} VkrSceneModelDesc;

/**
 * Loads the mesh and bank synchronously and instantiates the model under
 * `wrapper`, which must be alive, carry no mesh and no animation, and not
 * hold another spawned model. Structural edits must be allowed: call at a
 * paused boundary, never from a simulation hook. On failure nothing the
 * spawn created remains and `*error` names the stage.
 */
bool8_t vkr_scene_spawn_model(VkrScene *scene, struct VkrRenderAssets *assets,
                              VkrEntityId wrapper,
                              const VkrSceneModelDesc *desc,
                              const char **error);

/** Releases a spawned model: its animation binding and requests, source-node
 * entities and mesh instances. The wrapper itself stays. No-op when
 * `wrapper` holds no spawned model. */
void vkr_scene_despawn_model(VkrScene *scene, VkrEntityId wrapper);

/** Entity destruction hook: destroying a wrapper releases its model, so no
 * orphaned node keeps rendering. */
void vkr_scene_model_entity_destroying(VkrScene *scene, VkrEntityId entity);

/** Scene teardown: frees the spawn records. Animation shutdown and the
 * owned-instance release own the rest. */
void vkr_scene_models_shutdown(VkrScene *scene);
