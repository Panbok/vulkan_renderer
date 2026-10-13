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

/** One frame's far proxies of the rendered scenes' scatters (large-level
    rendering, section 2), in caller storage: per mesh-manager instance
    slot, whether the camera leaves the copy out because its cell lies past
    the proxy distance, and per mesh slot, whether it leaves a cell's merged
    mesh out because the cell is near. */
typedef struct VkrScenePopulationFrame {
  uint8_t *instance_hidden;
  uint32_t instance_slot_count;
  uint8_t *mesh_hidden;
  uint32_t mesh_slot_count;
} VkrScenePopulationFrame;

/* Spline meshes and scatters one scene holds. */
#define VKR_SCENE_POPULATION_RULE_MAX 64u
/* Copies one rule places. */
#define VKR_SCENE_POPULATION_RULE_INSTANCE_MAX 2048u
/* Copies every rule of a scene places together: three quarters of the mesh
   manager's instances, leaving the rest to loaded models, and on the 16 GB
   floor (ADR-083) about 2 MB of instance rows a frame. */
#define VKR_SCENE_POPULATION_INSTANCE_MAX 12288u

/* A `spline`, `spline_point`, `spline_mesh` or `scatter` component of
   `entity` appeared, changed or left. */
void vkr_scene_population_changed(VkrScene *scene, VkrEntityId entity,
                                  const VkrTypeDesc *type);
void vkr_scene_population_entity_destroying(VkrScene *scene,
                                            VkrEntityId entity);
/* Rebuilds the rules whose inputs changed. */
void vkr_scene_population_update(VkrScene *scene);
/* Whether a scatter of `scene` has built proxy cells. */
bool8_t vkr_scene_population_has_proxies(const VkrScene *scene);
/* Swaps each proxy cell of `scene`'s scatters for the camera at `camera`
   into `frame`, whose tables the caller zeroes once for every rendered
   scene: past its rule's proxy distance the cell's merged mesh draws
   instead of its copies, and within 90% of it the copies return; with
   `copies_only` every copy draws and no merged mesh does. The copies keep
   casting shadows either way. */
void vkr_scene_population_mark_proxies(VkrScene *scene, Vec3 camera,
                                       bool8_t copies_only,
                                       VkrScenePopulationFrame *frame);
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

/* Advances whenever a rule places its copies again. */
uint64_t vkr_scene_population_revision(const VkrScene *scene);
/* Every rule's copies, up to `capacity`; the model stage copies those that
   land on its subject (vkr_model_stage.h). */
uint32_t vkr_scene_population_copies(const VkrScene *scene,
                                     VkrMeshInstanceHandle *out,
                                     uint32_t capacity);
