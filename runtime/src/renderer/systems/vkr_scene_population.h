#pragma once

#include "level/vkr_spline.h"
#include "renderer/systems/vkr_scene_system.h"

/* Spline meshes and scatters (ADR-084,
 * phase 5). Each rule owns runtime mesh instances of one cooked mesh that
 * the scene rebuilds whenever the rule, its entity's transform, a spline's
 * points or, for a scatter, a terrain changes; nothing of them is saved. A
 * scatter drops its copies onto the first physics surface below its box, so
 * it waits for terrain collision to rest. Picking an instance selects the
 * rule's entity. The scene owns the instances and releases them with the
 * rule, its entity or the scene. */

typedef struct s_VkrScenePopulation VkrScenePopulation;

/* Spline meshes and scatters one scene holds. */
#define VKR_SCENE_POPULATION_RULE_MAX 64u
/* Copies one rule places. */
#define VKR_SCENE_POPULATION_RULE_INSTANCE_MAX 2048u
/* Copies every rule of a scene places together: the residency bound that
   keeps population within the mesh manager's instances on the 16 GB floor
   (ADR-083). */
#define VKR_SCENE_POPULATION_INSTANCE_MAX 4096u

/* A `spline`, `spline_point`, `spline_mesh` or `scatter` component of
   `entity` appeared, changed or left. */
void vkr_scene_population_changed(VkrScene *scene, VkrEntityId entity,
                                  const VkrTypeDesc *type);
void vkr_scene_population_entity_destroying(VkrScene *scene,
                                            VkrEntityId entity);
/* Rebuilds the rules whose inputs changed. */
void vkr_scene_population_update(VkrScene *scene);
/* Rules whose copies do not follow their inputs yet, as a scatter waiting
   for terrain collision. */
uint32_t vkr_scene_population_pending(const VkrScene *scene);
void vkr_scene_population_shutdown(VkrScene *scene);

/* Calls `visit` with each rule entity that has instances, so the render
   bridge can map its render id back to it. */
void vkr_scene_population_each_owner(const VkrScene *scene,
                                     void (*visit)(void *context,
                                                   VkrEntityId entity),
                                     void *context);

/* A spline's points in the spline entity's space, in order; up to
   `capacity`. */
uint32_t vkr_scene_spline_points(const VkrScene *scene, VkrEntityId spline,
                                 Vec3 *out, uint32_t capacity,
                                 bool8_t *out_closed);
/* The same points in world space. */
uint32_t vkr_scene_spline_world_points(const VkrScene *scene,
                                       VkrEntityId spline, Vec3 *out,
                                       uint32_t capacity, bool8_t *out_closed);

/* Copies a rule placed, and NULL or why it placed fewer than it asked. */
uint32_t vkr_scene_population_instances(const VkrScene *scene,
                                        VkrEntityId entity);
const char *vkr_scene_population_status(const VkrScene *scene,
                                        VkrEntityId entity);
