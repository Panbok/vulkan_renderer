#pragma once

#include "renderer/systems/vkr_scene_system.h"

#define VKR_SCENE_EDIT_NAME_CAPACITY 512u
#define VKR_SCENE_EDIT_UNDO_CAPACITY 128u

typedef enum VkrSceneEditAction {
  VKR_SCENE_EDIT_NONE,
  VKR_SCENE_EDIT_SELECT,
  VKR_SCENE_EDIT_APPLY,
  VKR_SCENE_EDIT_UNDO,
  VKR_SCENE_EDIT_REDO,
  VKR_SCENE_EDIT_SAVE,
  VKR_SCENE_EDIT_FRAME,
  VKR_SCENE_EDIT_LOAD,
  VKR_SCENE_EDIT_UNLOAD,
  VKR_SCENE_EDIT_RELOAD,
} VkrSceneEditAction;

typedef enum VkrSceneEditField {
  VKR_SCENE_EDIT_TRANSFORM = 1u << 0,
  VKR_SCENE_EDIT_NAME = 1u << 1,
  VKR_SCENE_EDIT_VISIBILITY = 1u << 2,
  VKR_SCENE_EDIT_POINT_LIGHT = 1u << 3,
  VKR_SCENE_EDIT_DIRECTIONAL_LIGHT = 1u << 4,
  VKR_SCENE_EDIT_RECTANGLE_LIGHT = 1u << 5,
} VkrSceneEditField;

typedef struct VkrSceneEditValues {
  uint32_t fields;
  char name[VKR_SCENE_EDIT_NAME_CAPACITY];
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;
  SceneVisibility visibility;
  ScenePointLight point_light;
  SceneDirectionalLight directional_light;
  SceneRectangleLight rectangle_light;
} VkrSceneEditValues;

typedef struct VkrSceneEditRequest {
  VkrSceneEditAction action;
  VkrEntityId entity;
  VkrSceneEditValues values;
} VkrSceneEditRequest;

typedef struct VkrSceneEditEntry {
  VkrEntityId entity;
  VkrSceneEditValues before;
  VkrSceneEditValues after;
} VkrSceneEditEntry;

/* Runtime owns the journal; UI borrows snapshots only for its current build.
   Journal storage is bounded and released when the scene is replaced. */
typedef struct VkrSceneEditState {
  VkrAllocator *allocator;
  VkrSceneEditEntry *undo;
  VkrEntityId *touched;
  uint32_t touched_count;
  uint32_t touched_capacity;
  uint32_t undo_count;
  uint32_t undo_cursor;
  uint64_t generation;
  uint64_t revision;
  uint64_t saved_revision;
  bool8_t sidecar_conflict;
  char status[192];
} VkrSceneEditState;

bool8_t vkr_scene_edit_read(const VkrScene *scene, VkrEntityId entity,
                            VkrSceneEditValues *out);
bool8_t vkr_scene_edit_validate(const VkrSceneEditValues *values);
void vkr_scene_edit_reset(VkrSceneEditState *state, VkrAllocator *allocator,
                          uint64_t generation);
bool8_t vkr_scene_edit_apply(VkrSceneEditState *state, VkrScene *scene,
                             VkrEntityId entity,
                             const VkrSceneEditValues *values);
/** Append an already-applied edit without changing the scene. If allocation
 * fails, the caller must restore its pre-drag values. */
bool8_t vkr_scene_edit_record_external(VkrSceneEditState *state,
                                       VkrScene *scene, VkrEntityId entity,
                                       const VkrSceneEditValues *before,
                                       const VkrSceneEditValues *after);
bool8_t vkr_scene_edit_undo(VkrSceneEditState *state, VkrScene *scene,
                            bool8_t redo);
bool8_t vkr_scene_edit_save(VkrSceneEditState *state, const VkrScene *scene,
                            String8 path);
bool8_t vkr_scene_edit_load(VkrSceneEditState *state, VkrScene *scene,
                            String8 path);
