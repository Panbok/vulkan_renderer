#pragma once

#include "level/vkr_heightfield.h"
#include "renderer/systems/vkr_scene_system.h"
#include "vkr_gpu_abi.h"

/* Scene terrains (ADR-084). An entity with a `terrain` component loads its
 * heightfield file and keeps the samples in memory: edits change them there,
 * through the edit journal, and saving the scene writes the file. The scene
 * builds one generated mesh with a submesh per 64-cell tile, so culling works
 * tile by tile, and re-attaches it only once every rebuilt tile has uploaded,
 * so the terrain never leaves a frame; one static height field body follows
 * once edits rest. Hole samples open both the tile meshes and the body, on
 * the same triangles. A terrain ignores its entity's rotation and scale.
 * The scene owns all of it and releases it at shutdown.
 *
 * A terrain larger than VKR_HEIGHTFIELD_RESIDENT_CELLS streams (ADR-086): it
 * keeps the samples, tiles and a body near the scene's streaming
 * sources, loading the nearest missing tiles within a frame budget, and
 * draws the overview elsewhere with holes under the tiles that draw. Edits
 * load the samples they reach, which stay resident until saved. */

/* Terrains one scene holds. */
#define VKR_SCENE_TERRAIN_MAX 8u
/* Material of a terrain without a first layer: the untagged floor greybox
   look (vkr_surface.h). */
#define VKR_SCENE_TERRAIN_DEFAULT_MATERIAL                                     \
  "assets/materials/greybox/none_floor.mt"
/* Tiles per side of the largest terrain. */
#define VKR_SCENE_TERRAIN_TILES_MAX                                            \
  (VKR_HEIGHTFIELD_CELLS_MAX / VKR_HEIGHTFIELD_TILE_CELLS)
/* Updates a terrain waits after its last edit before it rebuilds its
   collision. */
#define VKR_SCENE_TERRAIN_COLLISION_SETTLE 8u
/* A streamed terrain's one body covers the tiles within this many tiles
   of the tile under each streaming source. */
#define VKR_SCENE_TERRAIN_BODY_TILES 2u
/* Milliseconds one update may spend streaming in tiles and bodies. */
#define VKR_SCENE_TERRAIN_STREAM_BUDGET_MS 2.0
/* Detail levels of a tile: 64 cells a side down to one. */
#define VKR_SCENE_TERRAIN_LOD_LEVELS 7u

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
/* What a terrain holds now: tiles drawing full detail and overview tiles
   drawing, tiles its body covers, and sample tiles in memory and unsaved.
   A resident terrain holds every tile and no overview. */
typedef struct VkrSceneTerrainStreaming {
  bool8_t streamed;
  uint32_t fine_tiles;
  uint32_t overview_tiles;
  uint32_t body_tiles;
  uint32_t resident_tiles;
  uint32_t unsaved_tiles;
} VkrSceneTerrainStreaming;
bool8_t vkr_scene_terrain_streaming(const VkrScene *scene, VkrEntityId entity,
                                    VkrSceneTerrainStreaming *out);
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
/* Loads the samples of `rect` so they can be read, as a journal entry
   does before an edit. */
bool8_t vkr_scene_terrain_require(VkrScene *scene, VkrEntityId entity,
                                  VkrHeightfieldRect rect);
/* The highest terrain surface under world point `top` and at or above
   height `bottom`, from the samples, which load as needed: its world
   position and normal. False where no loaded terrain lies there or its
   ground is open (vkr_heightfield_open). Unlike a physics ray, it finds a
   streamed terrain away from its body. */
bool8_t vkr_scene_terrain_ground(VkrScene *scene, Vec3 top, float32_t bottom,
                                 Vec3 *out_position, Vec3 *out_normal);
/* Writes samples back, as undo and redo do. */
bool8_t vkr_scene_terrain_write(VkrScene *scene, VkrEntityId entity,
                                VkrHeightfieldRect rect,
                                const uint16_t *heights,
                                const uint32_t *weights);

/* Indices every level of a tile needs. */
uint32_t vkr_scene_terrain_tile_index_count(void);
/* A tile's indices for every level (ADR-084) over `vertices`: its 65 x 65
   grid, then a skirt vertex below each edge vertex, edges -Z, +X, +Z, -X.
   Level L draws every 2^L-th grid line, cells split along their +X to +Z
   diagonal, and its skirt. A grid vertex whose four color weights are zero
   is a hole sample: a cell with a hole corner splits along its -X-Z to
   +X+Z diagonal, as the height field collision does, and drops each
   triangle with a hole corner and the skirt below its tile edge, and the
   tile keeps level 0 only. Fills the LOD row with each level's range and error
   (`spacing` sets the error floor of coarser levels) and returns the index
   count, or zero when `capacity` is too small. */
uint32_t vkr_scene_terrain_tile_indices(const VkrVertex3d *vertices,
                                        float32_t spacing, uint32_t *indices,
                                        uint32_t capacity,
                                        VkrGpuGeometryLodRow *out_lod);

/* Grows with every change to any terrain's samples, so things resting on
   terrain can tell they must settle again. */
uint64_t vkr_scene_terrain_revision(const VkrScene *scene);
/* Whether every terrain's mesh and collision match its samples. */
bool8_t vkr_scene_terrain_settled(const VkrScene *scene);

/* Absolute path of a heightfield file the component names. */
bool8_t vkr_scene_terrain_resolve(const VkrScene *scene, const char *relative,
                                  char *out, uint32_t capacity);
/* Writes every terrain whose samples changed since it loaded or saved,
   creating the directory of a staged terrain's file. */
bool8_t vkr_scene_terrain_save(const VkrScene *scene, char *error,
                               uint32_t capacity);
/* Stages a new flat terrain whose file `path` (absolute, as
   vkr_scene_terrain_resolve gives) does not exist yet: a terrain component
   naming it loads the field in memory, and the first save of its scene
   writes the file. A discarded scene leaves no file behind. Only resident
   sizes, up to VKR_HEIGHTFIELD_RESIDENT_CELLS cells a side, can be staged;
   a path staged already keeps its first field. Main thread; false when the
   staging table is full. */
bool8_t vkr_scene_terrain_stage(const char *path, uint32_t cells,
                                float32_t spacing, float32_t height_min,
                                float32_t height_max, float32_t height);
