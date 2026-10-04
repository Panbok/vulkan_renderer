#pragma once

#include "level/vkr_heightfield.h"
#include "renderer/systems/vkr_scene_system.h"

/* Scene terrains (ADR-084). An entity with a `terrain` component loads its
 * heightfield file and keeps the samples in memory: edits change them there,
 * through the edit journal, and saving the scene writes the file. The scene
 * builds one generated mesh with a submesh per 64-cell tile, so culling works
 * tile by tile, and one static height field body once edits rest. A terrain
 * ignores its entity's rotation and scale. The scene owns all of it and
 * releases it at shutdown. */

/* Terrains one scene holds. */
#define VKR_SCENE_TERRAIN_MAX 8u
/* Tiles per side of the largest terrain. */
#define VKR_SCENE_TERRAIN_TILES_MAX                                            \
  (VKR_HEIGHTFIELD_CELLS_MAX / VKR_HEIGHTFIELD_TILE_CELLS)
/* Updates a terrain waits after its last edit before it rebuilds its
   collision. */
#define VKR_SCENE_TERRAIN_COLLISION_SETTLE 8u

typedef struct s_VkrSceneTerrains VkrSceneTerrains;

/* A `terrain` component of `entity` appeared, changed or left. */
void vkr_scene_terrain_changed(VkrScene *scene, VkrEntityId entity);
void vkr_scene_terrain_entity_destroying(VkrScene *scene, VkrEntityId entity);
/* Loads new terrains, rebuilds edited tiles, and rebuilds collision once
   edits rest. */
void vkr_scene_terrain_update(VkrScene *scene);
void vkr_scene_terrain_shutdown(VkrScene *scene);

/* The loaded samples of `entity`'s terrain, or NULL. */
const VkrHeightfield *vkr_scene_terrain_field(const VkrScene *scene,
                                              VkrEntityId entity);
/* NULL when the terrain loaded, else why it did not. */
const char *vkr_scene_terrain_status(const VkrScene *scene, VkrEntityId entity);
/* `world` in the terrain's local space: metres from its centre, heights
   above its origin. */
bool8_t vkr_scene_terrain_to_local(const VkrScene *scene, VkrEntityId entity,
                                   Vec3 world, Vec3 *out_local);

/* Runs `op` (local space) on the samples and marks what it touched. */
bool8_t vkr_scene_terrain_apply(VkrScene *scene, VkrEntityId entity,
                                const VkrHeightfieldOp *op,
                                VkrAllocator *scratch,
                                VkrHeightfieldRect *out_touched);
/* Writes samples back, as undo and redo do. */
bool8_t vkr_scene_terrain_write(VkrScene *scene, VkrEntityId entity,
                                VkrHeightfieldRect rect,
                                const uint16_t *heights,
                                const uint32_t *weights);

/* Absolute path of a heightfield file the component names. */
bool8_t vkr_scene_terrain_resolve(const VkrScene *scene, const char *relative,
                                  char *out, uint32_t capacity);
/* Writes every terrain whose samples changed since it loaded or saved. */
bool8_t vkr_scene_terrain_save(const VkrScene *scene, char *error,
                               uint32_t capacity);
