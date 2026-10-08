#pragma once

#include "renderer/systems/vkr_scene_system.h"

/* Scene material overrides. A `material_override` component names, per
 * submesh, the material file the entity's mesh instance draws instead of its
 * shared asset's (SceneMaterialOverride). The scene keeps one record per
 * component. After the component appears or changes, and whenever the
 * entity's mesh renderer holds a different instance than the one it applied
 * to, the record loads the named materials once the instance has loaded,
 * waits for their publication and hands them to the instance
 * (vkr_mesh_manager_instance_set_materials). The instance holds its own
 * references from then on; the record holds references only while it
 * waits. Removing the component, or emptying its paths, clears the
 * instance's overrides. */

/* Records that load their materials per update; the rest wait for the next
   one. */
#define VKR_SCENE_MATERIAL_OVERRIDE_LOAD_BUDGET 32u

/* A `material_override` component of `entity` appeared, changed or left. */
void vkr_scene_material_override_changed(VkrScene *scene, VkrEntityId entity);
void vkr_scene_material_override_entity_destroying(VkrScene *scene,
                                                   VkrEntityId entity);
/* Applies the overrides of changed components and of entities whose mesh
   instance changed. Call after models load. */
void vkr_scene_material_override_update(VkrScene *scene);
void vkr_scene_material_override_shutdown(VkrScene *scene);
