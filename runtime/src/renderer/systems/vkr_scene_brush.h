#pragma once

#include "core/vkr_type_desc.h"
#include "renderer/systems/vkr_scene_system.h"

/* Scene brushes (ADR-084). A brush is
 * an entity with a `brush` component whose direct children carry
 * `brush_face`. The scene rebuilds a changed brush once per update, after
 * its transform has rested for two updates: one generated mesh with a
 * submesh per face material, held by the entity like a shape's mesh. Solid
 * and clip brushes add a convex hull to a static body of their world cell,
 * at most 32 hulls a body; trigger brushes own a sensor hull. The scene owns
 * all of it and releases it at shutdown; documents store only the components.
 */

/* Edge of the world cells that group brush collision, in meters. */
#define VKR_SCENE_BRUSH_CELL_SIZE 32.0f
/* Brushes rebuilt per update; the rest wait for the next one. */
#define VKR_SCENE_BRUSH_REBUILD_BUDGET 256u
/* Material a face without one uses. */
#define VKR_SCENE_BRUSH_DEFAULT_MATERIAL "assets/materials/dev/dev_grid.mt"

/* A `brush` or `brush_face` component of `entity` changed, appeared or
   left. */
void vkr_scene_brush_changed(VkrScene *scene, VkrEntityId entity,
                             const VkrTypeDesc *type);
/* `entity` moved from `old_parent` to `new_parent`. */
void vkr_scene_brush_parent_changed(VkrScene *scene, VkrEntityId entity,
                                    VkrEntityId old_parent,
                                    VkrEntityId new_parent);
void vkr_scene_brush_entity_destroying(VkrScene *scene, VkrEntityId entity);
/* Rebuilds settled dirty brushes and the collision cells they touch. */
void vkr_scene_brush_update(VkrScene *scene);
void vkr_scene_brush_shutdown(VkrScene *scene);

/* NULL when `brush` built, else why it did not. Unbuilt brushes report
   "pending". */
const char *vkr_scene_brush_status(const VkrScene *scene, VkrEntityId brush);
/* Faces (direct children with `brush_face`) of `brush`, at most `capacity`;
   returns the total. */
uint32_t vkr_scene_brush_faces(const VkrScene *scene, VkrEntityId brush,
                               VkrEntityId *out, uint32_t capacity);
