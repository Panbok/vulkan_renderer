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
 * index, so edit overlays and saves never bind to them. A mesh request
 * (vkr_scene_request_mesh) instead gives an existing entity, such as one
 * undo restored, the mesh it drew; it is no spawned model.
 */
#pragma once

#include "renderer/systems/vkr_mesh_manager.h"
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

/**
 * Starts loading the mesh file `desc` names so that `entity`, which must be
 * alive, carry a transform and hold no mesh, animation or spawned model,
 * draws it again the way the scene loader attached it: once the load is
 * ready, vkr_scene_models_update gives the entity a tracked instance of
 * `desc`'s source mesh with `mobility` (dynamic when the entity moves),
 * and the request ends. Undo uses it to restore a deleted mesh entity. The
 * entity's destruction drops the load; a failed load leaves the entity
 * without a mesh and logs a warning. `desc->transform` is ignored: the
 * scene's render sync places the instance at the entity's world matrix.
 * False with `*error` when the request cannot start.
 */
bool8_t vkr_scene_request_mesh(VkrScene *scene, VkrEntityId entity,
                               const VkrMeshLoadDesc *desc,
                               VkrShadowCasterMobility mobility,
                               const char **error);

/** Whether `entity` waits for a vkr_scene_request_mesh load; `out_desc`
 * and `out_mobility`, when given, receive the request, its strings
 * borrowed until the request ends. */
bool8_t vkr_scene_mesh_request(const VkrScene *scene, VkrEntityId entity,
                               VkrMeshLoadDesc *out_desc,
                               VkrShadowCasterMobility *out_mobility);

/** Instantiates requested models whose loads finished and the scene's
 * join hooks admit, attaches requested meshes whose loads finished, and
 * records failed models. Structural edits must be allowed. */
void vkr_scene_models_update(VkrScene *scene);

/** Writes up to `capacity` wrappers whose requested models still wait for
 * their loads; returns how many there are. */
uint32_t vkr_scene_models_loading(const VkrScene *scene, VkrEntityId *out,
                                  uint32_t capacity);

/** Where `wrapper`'s model stands; `*error`, when given, receives why a
 * requested model failed. */
VkrSceneModelStatus vkr_scene_model_status(const VkrScene *scene,
                                           VkrEntityId wrapper,
                                           const char **error);

/** The mesh and animation bank paths `wrapper`'s spawned model was made
 * with, the bank empty for a static model; false_v when it holds none.
 * Borrowed until the model is despawned. */
bool8_t vkr_scene_model_paths(const VkrScene *scene, VkrEntityId wrapper,
                              String8 *out_mesh, String8 *out_animation);

/** The entity of source node `node` of `wrapper`'s ready spawned model, or
 * invalid. */
VkrEntityId vkr_scene_model_node(const VkrScene *scene, VkrEntityId wrapper,
                                 uint32_t node);

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
