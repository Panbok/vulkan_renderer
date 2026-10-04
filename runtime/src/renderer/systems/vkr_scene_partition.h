#pragma once

#include "renderer/systems/vkr_scene_system.h"

/* World partition (ADR-086). A scene carrying a `world_partition` component
 * divides the ground plane into square cells. Each root entity the editor
 * created streams with the cell that holds its world position, children
 * with their root. Document entities, terrains, singletons and entities
 * marked `always_loaded` form the scene's persistent layer and never
 * stream.
 *
 * The scene keeps a table of the cells it knows: those with a cell document
 * on disk and those loaded now. Planning picks the cells to load (the
 * nearest within the load radius of a streaming source) and to unload (past
 * the radius, then the farthest over the cell budget); the owner of the
 * scene's edit state moves their entities in and out
 * (vkr_scene_edit_cell_load). Pinned and held cells never unload, and
 * unreadable ones never load. */

/* Loads and unloads one plan proposes. */
#define VKR_SCENE_PARTITION_PLAN_MAX 64u

typedef struct VkrScenePartitionCell {
  int32_t x;
  int32_t z;
} VkrScenePartitionCell;

typedef enum VkrScenePartitionCellFlag {
  /* A cell document exists. */
  VKR_SCENE_PARTITION_CELL_ON_DISK = 1u << 0,
  VKR_SCENE_PARTITION_CELL_LOADED = 1u << 1,
  /* Pinned for editing (partition.load, the World Partition window); stays
     loaded until released. */
  VKR_SCENE_PARTITION_CELL_PINNED = 1u << 2,
  /* A document of the cell size used before; the next save replaces or
     removes it. */
  VKR_SCENE_PARTITION_CELL_STALE = 1u << 3,
  /* Loaded when Play began; Reset loads it again if it left. */
  VKR_SCENE_PARTITION_CELL_BEFORE_PLAY = 1u << 4,
  /* The bakery built no proxy for the cell. */
  VKR_SCENE_PARTITION_CELL_NO_PROXY = 1u << 5,
  /* Held by a Play session that began with unsaved edits, so Reset finds
     it as it was; never unloads until Reset. */
  VKR_SCENE_PARTITION_CELL_HELD = 1u << 6,
  /* Its document could not be read: streaming leaves the cell unloaded and
     saving keeps the document listed, so its objects are never dropped. */
  VKR_SCENE_PARTITION_CELL_UNREADABLE = 1u << 7,
} VkrScenePartitionCellFlag;

typedef struct VkrScenePartitionCellRecord {
  VkrScenePartitionCell cell;
  uint32_t flags;
  /* The runtime-only entity drawing the cell's proxy, or invalid. */
  VkrEntityId proxy;
  /* The edit revision plus one at which an unsaved scene last compared the
     loaded cell with its document, and whether they matched; zero when it
     never did. */
  uint64_t checked_revision;
  bool8_t checked_clean;
} VkrScenePartitionCellRecord;

typedef struct VkrScenePartitionPlan {
  VkrScenePartitionCell load[VKR_SCENE_PARTITION_PLAN_MAX];
  uint32_t load_count;
  VkrScenePartitionCell unload[VKR_SCENE_PARTITION_PLAN_MAX];
  uint32_t unload_count;
} VkrScenePartitionPlan;

/* The scene's partition settings, validated; false when it has none. */
bool8_t vkr_scene_partition_settings(const VkrScene *scene,
                                     SceneWorldPartition *out);
VkrScenePartitionCell
vkr_scene_partition_cell_at(const SceneWorldPartition *settings, Vec3 world);
/* The cell `entity` streams with: its root's when that root is an
   editor-created entity outside the persistent layer. False for the
   persistent layer. */
bool8_t vkr_scene_partition_entity_cell(const VkrScene *scene,
                                        const SceneWorldPartition *settings,
                                        VkrEntityId entity,
                                        VkrScenePartitionCell *out);

/* The known cells, valid until the table changes. */
const VkrScenePartitionCellRecord *
vkr_scene_partition_cells(const VkrScene *scene, uint32_t *out_count);
/* The record of `cell`, made with no flags when `create` is set; NULL when
   unknown or out of memory. */
VkrScenePartitionCellRecord *
vkr_scene_partition_cell(VkrScene *scene, VkrScenePartitionCell cell,
                         bool8_t create);
/* Forgets every cell, as when the scene's files are read anew. */
void vkr_scene_partition_reset(VkrScene *scene);

/* Cells to load and unload around the scene's streaming sources. Without
   sources nothing changes. */
void vkr_scene_partition_plan(const VkrScene *scene,
                              const SceneWorldPartition *settings,
                              VkrScenePartitionPlan *out);

/* Whether `record`'s cell should draw its proxy: it has a document, is not
   loaded, and lies within the proxy radius of a streaming source. */
bool8_t
vkr_scene_partition_proxy_wanted(const VkrScene *scene,
                                 const SceneWorldPartition *settings,
                                 const VkrScenePartitionCellRecord *record);

void vkr_scene_partition_shutdown(VkrScene *scene);
