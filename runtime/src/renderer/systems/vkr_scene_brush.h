#pragma once

#include "core/vkr_type_desc.h"
#include "renderer/systems/vkr_scene_system.h"

/* Scene brushes (ADR-084). A brush is
 * an entity with a `brush` component whose direct children carry
 * `brush_face`. The scene rebuilds a changed brush once per update, after
 * its transform has rested for two updates: one generated mesh with a
 * submesh per face material, held by the entity like a shape's mesh. Solid
 * and clip brushes add a convex hull to a static body of their world cell,
 * at most 32 hulls a body; trigger brushes own a sensor hull. A `blockout`
 * shape builds its mesh the same way from its pieces (vkr_blockout.h), and
 * owns one static body whose triangle mesh holds every piece. The scene owns
 * all of it and releases it at shutdown; documents store only the
 * components.
 *
 * Under a `mover` (the nearest one at or above the brush), solid and clip
 * hulls join one kinematic body per mover instead of a cell, at most 32;
 * the mover's motion in play never rebuilds its brushes, and their own
 * edits wait until its evaluated pose clears. Trigger brushes and blockout
 * shapes under a mover keep their static bodies at rest.
 */

/* Edge of the world cells that group brush collision, in meters. */
#define VKR_SCENE_BRUSH_CELL_SIZE 32.0f
/* Brushes rebuilt per update; the rest wait for the next one. */
#define VKR_SCENE_BRUSH_REBUILD_BUDGET 256u
/* Material a face without one uses. */
#define VKR_SCENE_BRUSH_DEFAULT_MATERIAL "assets/materials/dev/dev_grid.mt"

/* A `brush`, `brush_face` or `blockout` component of `entity` changed,
   appeared or left. */
void vkr_scene_brush_changed(VkrScene *scene, VkrEntityId entity,
                             const VkrTypeDesc *type);
/* `entity` moved from `old_parent` to `new_parent`. */
void vkr_scene_brush_parent_changed(VkrScene *scene, VkrEntityId entity,
                                    VkrEntityId old_parent,
                                    VkrEntityId new_parent);
void vkr_scene_brush_entity_destroying(VkrScene *scene, VkrEntityId entity);
/* Rebuilds settled dirty brushes and the collision cells and mover bodies
   they touch. */
void vkr_scene_brush_update(VkrScene *scene);
/* Moves the kinematic body of `mover`'s brushes by `offset` metres in world
   space from their rest from the next physics step; nothing without one.
   The IO router calls it as the mover moves (vkr_io_router.h). */
void vkr_scene_brush_mover_move(VkrScene *scene, VkrEntityId mover,
                                Vec3 offset);
void vkr_scene_brush_shutdown(VkrScene *scene);

/* NULL when brush or blockout shape `brush` built, else why it did not.
   Unbuilt ones report "pending". */
const char *vkr_scene_brush_status(const VkrScene *scene, VkrEntityId brush);
/* Faces (direct children with `brush_face`) of `brush`, at most `capacity`;
   returns the total. */
uint32_t vkr_scene_brush_faces(const VkrScene *scene, VkrEntityId brush,
                               VkrEntityId *out, uint32_t capacity);
