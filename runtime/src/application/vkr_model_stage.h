#pragma once

#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_system.h"

/* The model stage (ADR-089): an empty in-memory scene with a ground, a sun
 * and a uniform sky that the editor's Model view renders in place of the
 * loaded containers, with copies of one object's meshes standing on its
 * ground. The copies share their assets and materials with the level's
 * instances, so the level stays loaded and unchanged behind it. */

/* Mesh instances one subject brings: the object and its descendants. */
#define VKR_MODEL_STAGE_COPY_MAX 1024u

/* World id of the stage's container, apart from the primary (0), added
   (1..VKR_SCENE_ADDITIVE_MAX) and World (VKR_SCENE_WORLD_ROOT_ID) ones. */
#define VKR_MODEL_STAGE_WORLD_ID 0x7FFEu

typedef struct VkrModelStage {
  VkrSceneHandle scene;
  /* The subject shown and the offset that moves its copies. */
  VkrEntityId subject;
  Vec3 offset;
  /* Each copy and the level instance it copies, index-aligned. */
  uint32_t copy_count;
  VkrMeshInstanceHandle sources[VKR_MODEL_STAGE_COPY_MAX];
  VkrMeshInstanceHandle copies[VKR_MODEL_STAGE_COPY_MAX];
  /* The subject had more mesh instances than the stage copies. */
  bool8_t truncated;
} VkrModelStage;

/* Loads the stage's scene once; true while it is open. */
bool8_t vkr_model_stage_open(VkrModelStage *stage, VkrRenderAssets *assets,
                             VkrAllocator *temp_alloc);

/* Shows `subject` of `source` on the stage, its copies moved by `offset`,
   copying again when the subject or offset changes or a copied instance's
   asset or materials no longer match its source. A NULL source or dead
   subject leaves the stage empty. */
void vkr_model_stage_show(VkrModelStage *stage, VkrRenderAssets *assets,
                          const VkrScene *source, VkrEntityId subject,
                          Vec3 offset);

/* Destroys the copies and the stage's scene. */
void vkr_model_stage_close(VkrModelStage *stage, VkrRenderAssets *assets);

/* The stage's scene while open, else NULL. */
VkrScene *vkr_model_stage_scene(const VkrModelStage *stage);
