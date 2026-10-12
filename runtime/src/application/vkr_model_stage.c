#include "application/vkr_model_stage.h"

#include "core/logger.h"
#include "renderer/resources/loaders/scene_loader.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_population.h"

/* A neutral studio: a uniform grey sky that lights from every side, a warm
   key sun from above and in front, and a mid-grey ground whose top is
   y = 0, far wider than any one object. It authors no atmosphere, fog,
   probe or lightmap, and does not inherit the World's. */
static const char s_stage_json[] =
    "{\"version\": 2, "
    "\"environment\": {\"enabled\": true, \"constant\": [0.42, 0.43, 0.46], "
    "\"intensity\": 1, \"diffuse_intensity\": 1, \"specular_intensity\": 1}, "
    "\"reflection_probes\": [], "
    "\"entities\": ["
    "{\"name\": \"Stage sun\", \"parent\": null, \"transform\": {\"pos\": "
    "[0, 0, 0], \"rot\": [0, 0, 0, 1], \"scale\": [1, 1, 1]}, "
    "\"directional_light\": {\"enabled\": true, \"color\": [1.0, 0.96, 0.9], "
    "\"intensity\": 3.0, \"direction_local\": [-0.42, -0.78, -0.46]}}, "
    "{\"name\": \"Stage ground\", \"parent\": null, \"transform\": {\"pos\": "
    "[0, -0.05, 0], \"rot\": [0, 0, 0, 1], \"scale\": [1, 1, 1]}, "
    "\"shape\": {\"type\": \"cube\", \"dimensions\": [400, 0.1, 400], "
    "\"color\": [0.36, 0.36, 0.37, 1]}}"
    "]}";

bool8_t vkr_model_stage_open(VkrModelStage *stage, VkrRenderAssets *assets,
                             VkrAllocator *temp_alloc) {
  if (stage->scene) {
    return true_v;
  }
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  VkrSceneHandle handle = vkr_scene_handle_create(
      &assets->allocator, VKR_MODEL_STAGE_WORLD_ID, 16u, 16u, &error);
  if (!handle) {
    log_error("Model stage: scene could not be created (%d)", (int)error);
    return false_v;
  }
  VkrScene *scene = vkr_scene_handle_get_scene(handle);
  scene->settings.inherit_world = false_v;
  if (!vkr_scene_load_from_json(scene, assets, string8_lit(s_stage_json),
                                temp_alloc, NULL, &error)) {
    log_error("Model stage: studio did not load (%d)", (int)error);
    vkr_scene_handle_destroy(handle, assets);
    return false_v;
  }
  stage->scene = handle;
  stage->subject = VKR_ENTITY_ID_INVALID;
  stage->copy_count = 0u;
  stage->truncated = false_v;
  return true_v;
}

static void model_stage_clear(VkrModelStage *stage, VkrRenderAssets *assets) {
  for (uint32_t i = 0; i < stage->copy_count; ++i) {
    (void)vkr_mesh_manager_destroy_instance(&assets->mesh_manager,
                                            stage->copies[i]);
  }
  stage->copy_count = 0u;
  stage->truncated = false_v;
}

/* Whether every copy still draws its source's asset and materials. */
static bool8_t model_stage_current(const VkrModelStage *stage,
                                   VkrRenderAssets *assets) {
  VkrMeshManager *manager = &assets->mesh_manager;
  for (uint32_t i = 0; i < stage->copy_count; ++i) {
    const VkrMeshInstance *source =
        vkr_mesh_manager_get_instance(manager, stage->sources[i]);
    const VkrMeshInstance *copy =
        vkr_mesh_manager_get_instance(manager, stage->copies[i]);
    if (!source || !copy || source->asset.id != copy->asset.id ||
        source->asset.generation != copy->asset.generation ||
        source->material_override_count != copy->material_override_count ||
        MemCompare(source->material_overrides, copy->material_overrides,
                   source->material_override_count *
                       sizeof(source->material_overrides[0])) != 0) {
      return false_v;
    }
  }
  return true_v;
}

/* Moves each copy with its source, as a gizmo drag moves the level's
   object: the copy stays its source moved by the stage's offset. */
static void model_stage_follow(VkrModelStage *stage, VkrRenderAssets *assets) {
  VkrMeshManager *manager = &assets->mesh_manager;
  const Mat4 move = mat4_translate(stage->offset);
  for (uint32_t i = 0; i < stage->copy_count; ++i) {
    const VkrMeshInstance *source =
        vkr_mesh_manager_get_instance(manager, stage->sources[i]);
    const VkrMeshInstance *copy =
        vkr_mesh_manager_get_instance(manager, stage->copies[i]);
    if (!source || !copy) {
      continue;
    }
    const Mat4 placed = mat4_mul(move, source->model);
    if (MemCompare(&placed, &copy->model, sizeof(placed)) != 0) {
      vkr_mesh_manager_instance_sync_render_state(manager, stage->copies[i],
                                                  placed, 0u, true_v);
    }
  }
}

/* Copies the mesh instances of `subject` and of every entity below it. */
static void model_stage_copy(VkrModelStage *stage, VkrRenderAssets *assets,
                             const VkrScene *source, VkrEntityId subject,
                             Vec3 offset) {
  const Mat4 move = mat4_translate(offset);
  for (uint32_t i = 0; i < source->topo_count; ++i) {
    const VkrEntityId candidate = source->topo_order[i];
    VkrEntityId ancestor = candidate;
    /* The directory's capacity bounds the walk, so a cycle cannot loop. */
    for (uint32_t depth = 0; ancestor.u64 && ancestor.u64 != subject.u64 &&
                             depth < source->world->dir.capacity;
         ++depth) {
      const SceneTransform *parent = vkr_entity_get_component(
          source->world, ancestor, source->comp_transform);
      ancestor = parent ? parent->parent : VKR_ENTITY_ID_INVALID;
    }
    if (ancestor.u64 != subject.u64) {
      continue;
    }
    const SceneMeshRenderer *renderer = vkr_entity_get_component(
        source->world, candidate, source->comp_mesh_renderer);
    const VkrMeshInstance *instance =
        renderer ? vkr_mesh_manager_get_instance(&assets->mesh_manager,
                                                 renderer->instance)
                 : NULL;
    if (!instance) {
      continue;
    }
    if (stage->copy_count == VKR_MODEL_STAGE_COPY_MAX) {
      stage->truncated = true_v;
      return;
    }
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    const VkrMeshInstanceHandle copy = vkr_mesh_manager_instance_clone(
        &assets->mesh_manager, renderer->instance,
        mat4_mul(move, instance->model), &error);
    if (copy.id == 0u) {
      continue;
    }
    stage->sources[stage->copy_count] = renderer->instance;
    stage->copies[stage->copy_count++] = copy;
  }
}

/* Copies the scatter and spline copies whose bounds' centres lie within the
   subject's copied bounds, half a metre wider, so what was scattered onto
   the subject stands on it on the stage. */
static void model_stage_copy_scatter(VkrModelStage *stage,
                                     VkrRenderAssets *assets,
                                     const VkrScene *source, Vec3 offset) {
  VkrMeshManager *manager = &assets->mesh_manager;
  Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  for (uint32_t i = 0; i < stage->copy_count; ++i) {
    const VkrMeshInstance *instance =
        vkr_mesh_manager_get_instance(manager, stage->sources[i]);
    if (!instance || !instance->bounds_valid) {
      continue;
    }
    const Vec3 c = instance->bounds_world_center;
    const float32_t r = instance->bounds_world_radius + 0.5f;
    lo = vec3_new(Min(lo.x, c.x - r), Min(lo.y, c.y - r), Min(lo.z, c.z - r));
    hi = vec3_new(Max(hi.x, c.x + r), Max(hi.y, c.y + r), Max(hi.z, c.z + r));
  }
  if (!(lo.x <= hi.x)) {
    return;
  }
  VkrMeshInstanceHandle copies[VKR_MODEL_STAGE_COPY_MAX];
  const uint32_t count =
      vkr_scene_population_copies(source, copies, ArrayCount(copies));
  const Mat4 move = mat4_translate(offset);
  for (uint32_t i = 0; i < count; ++i) {
    const VkrMeshInstance *instance =
        vkr_mesh_manager_get_instance(manager, copies[i]);
    if (!instance || !instance->bounds_valid) {
      continue;
    }
    const Vec3 c = instance->bounds_world_center;
    if (c.x < lo.x || c.x > hi.x || c.y < lo.y || c.y > hi.y || c.z < lo.z ||
        c.z > hi.z) {
      continue;
    }
    if (stage->copy_count == VKR_MODEL_STAGE_COPY_MAX) {
      stage->truncated = true_v;
      return;
    }
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    const VkrMeshInstanceHandle copy = vkr_mesh_manager_instance_clone(
        manager, copies[i], mat4_mul(move, instance->model), &error);
    if (copy.id != 0u) {
      stage->sources[stage->copy_count] = copies[i];
      stage->copies[stage->copy_count++] = copy;
    }
  }
}

void vkr_model_stage_show(VkrModelStage *stage, VkrRenderAssets *assets,
                          const VkrScene *source, VkrEntityId subject,
                          Vec3 offset) {
  if (!stage->scene) {
    return;
  }
  const bool8_t alive = source && vkr_scene_entity_alive(source, subject);
  if (!alive) {
    subject = VKR_ENTITY_ID_INVALID;
  }
  const uint64_t population = vkr_scene_population_revision(source);
  if (subject.u64 == stage->subject.u64 &&
      vec3_length(vec3_sub(offset, stage->offset)) < 1.0e-5f &&
      population == stage->population_revision &&
      model_stage_current(stage, assets)) {
    model_stage_follow(stage, assets);
    return;
  }
  model_stage_clear(stage, assets);
  stage->subject = subject;
  stage->offset = offset;
  stage->population_revision = population;
  if (alive) {
    model_stage_copy(stage, assets, source, subject, offset);
    model_stage_copy_scatter(stage, assets, source, offset);
  }
}

void vkr_model_stage_close(VkrModelStage *stage, VkrRenderAssets *assets) {
  model_stage_clear(stage, assets);
  if (stage->scene) {
    vkr_scene_handle_destroy(stage->scene, assets);
  }
  stage->scene = NULL;
  stage->subject = VKR_ENTITY_ID_INVALID;
}

VkrScene *vkr_model_stage_scene(const VkrModelStage *stage) {
  return stage->scene ? vkr_scene_handle_get_scene(stage->scene) : NULL;
}
