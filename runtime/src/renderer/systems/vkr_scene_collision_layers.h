#pragma once

#include "renderer/systems/vkr_scene_system.h"

#define VKR_COLLISION_LAYER_COUNT 16u
#define VKR_COLLISION_PRESET_CAPACITY 16u
#define VKR_COLLISION_LAYER_NAME_CAPACITY 48u

typedef struct VkrSceneCollisionPreset {
  char name[VKR_COLLISION_LAYER_NAME_CAPACITY];
  uint16_t membership;
  uint16_t mask;
  bool8_t sensor;
} VkrSceneCollisionPreset;

typedef struct VkrSceneCollisionLayers {
  char names[VKR_COLLISION_LAYER_COUNT][VKR_COLLISION_LAYER_NAME_CAPACITY];
  uint16_t matrix[VKR_COLLISION_LAYER_COUNT];
  uint32_t preset_count;
  VkrSceneCollisionPreset presets[VKR_COLLISION_PRESET_CAPACITY];
} VkrSceneCollisionLayers;

typedef struct s_VkrSceneCollisionLayersPrepared
    VkrSceneCollisionLayersPrepared;

VkrSceneCollisionLayers vkr_scene_collision_layers_default(void);
void vkr_scene_collision_layers_read(const VkrScene *scene,
                                     VkrSceneCollisionLayers *settings);
bool8_t
vkr_scene_collision_layers_validate(const VkrSceneCollisionLayers *settings,
                                    const char **error);
uint16_t vkr_scene_collision_layers_effective_mask(const VkrScene *scene,
                                                   uint16_t membership,
                                                   uint16_t requested_mask);
/* Preparation exposes pending settings only to native-body construction. The
   caller prepares all affected body replacements before committing either part.
   Do not update/render between prepare and commit/discard. */
bool8_t vkr_scene_collision_layers_prepare(
    VkrScene *scene, const VkrSceneCollisionLayers *settings,
    VkrSceneCollisionLayersPrepared **prepared, const char **error);
void vkr_scene_collision_layers_commit(
    VkrSceneCollisionLayersPrepared *prepared);
void vkr_scene_collision_layers_discard(
    VkrSceneCollisionLayersPrepared *prepared);
/* Paused, transactional settings + all affected bodies; no partial publication.
 */
bool8_t
vkr_scene_collision_layers_apply(VkrScene *scene,
                                 const VkrSceneCollisionLayers *settings,
                                 const char **error);
void vkr_scene_collision_layers_shutdown(VkrScene *scene);
