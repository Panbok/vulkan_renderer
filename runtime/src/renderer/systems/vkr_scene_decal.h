#pragma once

#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_scene_system.h"
#include "vkr_decal.h"

/* Scene decals (ADR-092). A scene keeps one material reference per `decal`
 * component, loaded on the first collection after the component appears or
 * changes and released when it leaves, its entity is destroyed or the scene
 * shuts down. Each frame the runtime collects the enabled, visible decals of
 * the rendered and additive scenes, keeps the nearest the camera up to the
 * frame's capacity and orders them for compositing; the tiled pipeline
 * builds their grid. */

#define VKR_SCENE_DECAL_DEFAULT_MATERIAL "assets/materials/dev/dev_decal.mt"

typedef struct s_VkrSceneDecals VkrSceneDecals;

/* A `decal` component of `entity` appeared, changed or left. */
void vkr_scene_decal_changed(VkrScene *scene, VkrEntityId entity);
void vkr_scene_decal_entity_destroying(VkrScene *scene, VkrEntityId entity);
void vkr_scene_decal_shutdown(VkrScene *scene);

/** One kept decal's compositing key, sorted with its row. */
typedef struct VkrSceneDecalOrder {
  int32_t sort_order;
  /* Collection order, which breaks sort-order ties. */
  uint32_t sequence;
  float32_t distance;
  uint32_t row;
} VkrSceneDecalOrder;

/** One frame's decals while they are collected, in no order, then in
 * compositing order. The arrays past `count` hold no meaning. */
typedef struct VkrSceneDecalFrame {
  VkrDecal decals[VKR_MAX_FRAME_DECALS];
  /* Squared distance from the camera to each decal's bounds. */
  float32_t distances[VKR_MAX_FRAME_DECALS];
  int32_t sort_orders[VKR_MAX_FRAME_DECALS];
  /* Collection order, which breaks sort-order ties. */
  uint32_t sequences[VKR_MAX_FRAME_DECALS];
  uint32_t count;
  /* Decals the frame keeps at most, up to VKR_MAX_FRAME_DECALS. */
  uint32_t capacity;
  /* Row of the farthest kept decal once the frame is full. */
  uint32_t farthest;
  /* Decals beyond the nearest `capacity` this frame. */
  uint32_t dropped_count;
  uint32_t next_sequence;
  /* Scratch of the compositing sort. */
  VkrSceneDecalOrder order[VKR_MAX_FRAME_DECALS];
  VkrDecal sorted[VKR_MAX_FRAME_DECALS];
  /* Built by the tiled pipeline's caller with vkr_decal_grid_build. */
  VkrDecalGrid grid;
} VkrSceneDecalFrame;

/** Begins a frame that keeps the `capacity` decals nearest the camera, at
 * most VKR_MAX_FRAME_DECALS. */
void vkr_scene_decal_frame_begin(VkrSceneDecalFrame *frame, uint32_t capacity);

/** Adds `scene`'s enabled, visible decals with a usable box, keeping the
 * nearest to `camera_position`. Loads the materials of changed decals. */
void vkr_scene_decal_frame_append(VkrSceneDecalFrame *frame, VkrScene *scene,
                                  Vec3 camera_position);

/** As vkr_scene_decal_frame_append, for the decals of `root` and the
 * entities below it only, each moved by `offset`: the model stage shows its
 * subject's decals where it stands the subject (vkr_model_stage.h). */
void vkr_scene_decal_frame_append_moved(VkrSceneDecalFrame *frame,
                                        VkrScene *scene, Vec3 camera_position,
                                        VkrEntityId root, Vec3 offset);

/** Orders the kept decals for compositing, ascending sort order then
 * collection order, and keeps their materials' textures resident. Call
 * inside the frame's texture residency pass (after
 * vkr_scene_build_world_draws). */
void vkr_scene_decal_frame_finish(VkrSceneDecalFrame *frame,
                                  VkrMaterialSystem *materials);
