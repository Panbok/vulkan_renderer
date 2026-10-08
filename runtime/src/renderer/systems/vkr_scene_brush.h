#pragma once

#include "core/vkr_type_desc.h"
#include "renderer/systems/vkr_scene_system.h"

/* Scene brushes (ADR-084). A brush is
 * an entity with a `brush` component whose direct children carry
 * `brush_face`. The scene rebuilds a changed brush once per update, after
 * its transform has rested for two updates: one generated mesh with a
 * submesh per face material, held by the entity like a shape's mesh. Solid
 * and clip brushes add a convex hull to a static body of their world cell,
 * at most 32 hulls a body; trigger brushes own a sensor hull. Each face
 * shows its art-owned material, or the greybox look of its surface tag and
 * mark (vkr_surface.h); clip and trigger brushes always show their role's
 * look. A `blockout`
 * shape builds its mesh the same way from its pieces (vkr_blockout.h), and
 * owns one static body whose triangle mesh holds every piece. The scene owns
 * all of it and releases it at shutdown; documents store only the
 * components.
 *
 * Under a `mover` (the nearest one at or above the brush), solid and clip
 * hulls join one kinematic body per mover instead of a cell, at most 32;
 * the mover's motion in play never rebuilds its brushes, and their own
 * edits wait until its evaluated pose clears. A trigger brush under a mover
 * owns a kinematic sensor that moves with it; blockout shapes under a mover
 * keep their static bodies at rest.
 */

/* Edge of the world cells that group brush collision, in meters. */
#define VKR_SCENE_BRUSH_CELL_SIZE 32.0f
/* Brushes rebuilt per update; the rest wait for the next one. */
#define VKR_SCENE_BRUSH_REBUILD_BUDGET 256u

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
/* Moves the kinematic body of `mover`'s brushes from their rest from the
   next physics step, turned by `rotation` about the world point `pivot` and
   then moved by the world `offset`; nothing without one. The IO router
   calls it as the mover moves (vkr_io_router.h). */
void vkr_scene_brush_mover_move(VkrScene *scene, VkrEntityId mover, Vec3 offset,
                                VkrQuat rotation, Vec3 pivot);
void vkr_scene_brush_shutdown(VkrScene *scene);
/* Shows every brush face's greybox look, art-owned materials too, or
   returns them; changing it rebuilds the scene's brushes. */
void vkr_scene_brush_set_greybox_view(VkrScene *scene, bool8_t on);
bool8_t vkr_scene_brush_greybox_view(const VkrScene *scene);

/* NULL when brush or blockout shape `brush` built, else why it did not.
   Unbuilt ones report "pending". */
const char *vkr_scene_brush_status(const VkrScene *scene, VkrEntityId brush);
/* Brushes and blockout shapes whose mesh or collision does not match their
   components yet: waiting to rebuild, or drawing their previous mesh until
   the new one's uploads settle. */
uint32_t vkr_scene_brush_pending(const VkrScene *scene);
/* Faces (direct children with `brush_face`) of `brush` by entity index, at
   most `capacity`; returns the total. Reads the scene's child index while it
   is current, else scans the world. */
uint32_t vkr_scene_brush_faces(const VkrScene *scene, VkrEntityId brush,
                               VkrEntityId *out, uint32_t capacity);
