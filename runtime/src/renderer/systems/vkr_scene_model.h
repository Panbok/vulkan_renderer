/**
 * @file vkr_scene_model.h
 * @brief Cooked models spawned into a live scene at runtime.
 *
 * Gameplay spawns a model, such as a character's mesh and animation bank,
 * under an existing entity the way the scene loader instantiates an entity
 * with `mesh` and `animation` blocks, outside any document: synchronously,
 * or requested and instantiated by the scene's update once its loads are
 * ready. The scene owns what a spawn created (the source-node entities,
 * their mesh instances and the resource requests) until
 * vkr_scene_despawn_model or scene shutdown. Spawned nodes carry no document
 * index, so edit overlays and saves never bind to them.
 */
#pragma once

#include "renderer/systems/vkr_scene_animation.h"

/* Where a wrapper's spawned model stands. */
typedef enum VkrSceneModelStatus {
  VKR_SCENE_MODEL_NONE = 0,
  /** Requested; its loads are in flight. */
  VKR_SCENE_MODEL_LOADING,
  /** Its nodes and instances exist. */
  VKR_SCENE_MODEL_READY,
  /** A load or the instantiation failed; nothing it created remains. */
  VKR_SCENE_MODEL_FAILED,
} VkrSceneModelStatus;

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

/**
 * Starts loading the mesh and bank on the resource system's workers and
 * reserves `wrapper` for the model, under the same conditions as
 * vkr_scene_spawn_model. vkr_scene_models_update instantiates it once both
 * loads are ready; the scene's update calls it. False with `*error` when
 * the request cannot start.
 */
bool8_t vkr_scene_request_model(VkrScene *scene, struct VkrRenderAssets *assets,
                                VkrEntityId wrapper,
                                const VkrSceneModelDesc *desc,
                                const char **error);

/** Instantiates requested models whose loads finished and records failed
 * ones. Structural edits must be allowed. */
void vkr_scene_models_update(VkrScene *scene);

/** Where `wrapper`'s model stands; `*error`, when given, receives why a
 * requested model failed. */
VkrSceneModelStatus vkr_scene_model_status(const VkrScene *scene,
                                           VkrEntityId wrapper,
                                           const char **error);

/** Releases a spawned model: its animation binding and requests, source-node
 * entities and mesh instances, or a requested model's loads. The wrapper
 * itself stays. No-op when `wrapper` holds no spawned model. */
void vkr_scene_despawn_model(VkrScene *scene, VkrEntityId wrapper);

/** Entity destruction hook: destroying a wrapper releases its model, so no
 * orphaned node keeps rendering. */
void vkr_scene_model_entity_destroying(VkrScene *scene, VkrEntityId entity);

/** Scene teardown: frees the spawn records. Animation shutdown and the
 * owned-instance release own the rest. */
void vkr_scene_models_shutdown(VkrScene *scene);
