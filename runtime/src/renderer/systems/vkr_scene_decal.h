#pragma once

#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_scene_system.h"
#include "vkr_decal.h"

/* Scene decals (ADR-092). A scene keeps one material reference per `decal`
 * component, loaded on the first collection after the component appears or
 * changes and released when it leaves, its entity is destroyed or the scene
 * shuts down. Each frame the runtime collects the enabled, visible decals of
 * the rendered and additive scenes, keeps the VKR_MAX_FRAME_DECALS nearest
 * the camera, orders them for compositing and builds their grid. */

#define VKR_SCENE_DECAL_DEFAULT_MATERIAL "assets/materials/dev/dev_decal.mt"

typedef struct s_VkrSceneDecals VkrSceneDecals;

/* A `decal` component of `entity` appeared, changed or left. */
void vkr_scene_decal_changed(VkrScene *scene, VkrEntityId entity);
void vkr_scene_decal_entity_destroying(VkrScene *scene, VkrEntityId entity);
void vkr_scene_decal_shutdown(VkrScene *scene);

/** One frame's decals while they are collected, then in compositing order
 * with their grid. The arrays past `count` hold no meaning. */
typedef struct VkrSceneDecalFrame {
  VkrDecal decals[VKR_MAX_FRAME_DECALS];
  /* Squared distance from the camera to each decal's bounds, nearest first
     while collecting. */
  float32_t distances[VKR_MAX_FRAME_DECALS];
  int32_t sort_orders[VKR_MAX_FRAME_DECALS];
  /* Collection order, which breaks sort-order ties. */
  uint32_t sequences[VKR_MAX_FRAME_DECALS];
  uint32_t count;
  /* Decals beyond the nearest VKR_MAX_FRAME_DECALS this frame. */
  uint32_t dropped_count;
  uint32_t next_sequence;
  VkrDecalGrid grid;
} VkrSceneDecalFrame;

void vkr_scene_decal_frame_begin(VkrSceneDecalFrame *frame);

/** Adds `scene`'s enabled, visible decals with a usable box, keeping the
 * nearest to `camera_position`. Loads the materials of changed decals. */
void vkr_scene_decal_frame_append(VkrSceneDecalFrame *frame, VkrScene *scene,
                                  Vec3 camera_position);

/** Orders the kept decals for compositing, builds their grid and keeps their
 * materials' textures resident. Call inside the frame's texture residency
 * pass (after vkr_scene_build_world_draws). */
void vkr_scene_decal_frame_finish(VkrSceneDecalFrame *frame,
                                  VkrMaterialSystem *materials);
