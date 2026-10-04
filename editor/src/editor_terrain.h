#pragma once

#include "editor_ui.h"

/* Terrain sculpting and painting in the Scene (ADR-084). While the tool is
 * on, the Scene takes the mouse: the brush follows the terrain under the
 * pointer, and holding the left button applies the Terrain window's mode,
 * radius, strength and layer every frame as one undoable stroke. */

typedef enum VkrEditorTerrainMode {
  VKR_EDITOR_TERRAIN_RAISE = 0,
  VKR_EDITOR_TERRAIN_LOWER,
  VKR_EDITOR_TERRAIN_SMOOTH,
  VKR_EDITOR_TERRAIN_FLATTEN,
  VKR_EDITOR_TERRAIN_PAINT,
  VKR_EDITOR_TERRAIN_MODE_COUNT,
} VkrEditorTerrainMode;

/* The terrain entity and world point a ray from `origin` along unit
   `direction` first meets, among every loaded scene's terrains. */
bool8_t vkr_editor_terrain_ray(const VkrSampleUiFrame *frame, Vec3 origin,
                               Vec3 direction, VkrEntityId *out_entity,
                               Vec3 *out_point);

/* Follows the pointer and applies strokes; call once per frame. */
void vkr_editor_terrain_update(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame);

/* The Terrain window body: mode, radius, strength, layer and the tool. */
void vkr_editor_terrain_window_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect bounds);

/* Points of the brush circle on the ground, for the overlay; zero when the
   brush is not over a terrain. */
uint32_t vkr_editor_terrain_brush_outline(const VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame,
                                          Vec3 *out, uint32_t capacity);
