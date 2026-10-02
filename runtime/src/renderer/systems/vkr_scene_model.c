#include "renderer/systems/vkr_scene_model.h"

#include "core/logger.h"
#include "math/vkr_transform.h"
#include "renderer/resources/loaders/scene_loader.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_physics.h"

/* What one spawn created. Node entities and instances are released by
 * despawn; at scene shutdown the world, the animation shutdown and the
 * owned-instance list release them instead. */
struct VkrSceneModel {
  struct VkrSceneModel *next;
  VkrEntityId wrapper;
  VkrEntityId *nodes;
  uint32_t node_count;
  VkrMeshInstanceHandle *instances;
  uint32_t instance_count;
  bool8_t animated;
  VkrSceneModelStatus status;
  /* A requested model: its loads in flight, the paths they were made with
     (owned copies) and what it binds once they are ready. */
  struct VkrRenderAssets *assets;
  VkrResourceHandleInfo mesh_request;
  VkrResourceHandleInfo animation_request;
  String8 mesh_path;
  String8 animation_path;
  VkrSceneAnimationConfig animation;
  /* Why a requested model failed; static text. */
  const char *error;
};

vkr_internal struct VkrSceneModel *scene_model_find(const VkrScene *scene,
                                                    VkrEntityId wrapper) {
  for (struct VkrSceneModel *model = scene->models; model;
       model = model->next) {
    if (model->wrapper.u64 == wrapper.u64) {
      return model;
    }
  }
  return NULL;
}

/* Drops a requested model's loads: a pending request is cancelled, a ready
 * one released. */
vkr_internal void scene_model_drop_requests(struct VkrSceneModel *model) {
  if (model->mesh_request.request_id || model->mesh_request.as.mesh) {
    vkr_resource_system_unload(&model->mesh_request, model->mesh_path);
  }
  if (model->animation_request.request_id ||
      model->animation_request.as.animation) {
    vkr_resource_system_unload(&model->animation_request,
                               model->animation_path);
  }
  model->mesh_request = (VkrResourceHandleInfo){0};
  model->animation_request = (VkrResourceHandleInfo){0};
}

vkr_internal void scene_model_free(VkrScene *scene,
                                   struct VkrSceneModel *model) {
  if (model->status == VKR_SCENE_MODEL_LOADING) {
    scene_model_drop_requests(model);
  }
  if (model->mesh_path.str) {
    vkr_allocator_free(scene->alloc, model->mesh_path.str,
                       model->mesh_path.length + 1u,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  if (model->animation_path.str) {
    vkr_allocator_free(scene->alloc, model->animation_path.str,
                       model->animation_path.length + 1u,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  if (model->nodes) {
    vkr_allocator_free(scene->alloc, model->nodes,
                       sizeof(VkrEntityId) * model->node_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (model->instances) {
    vkr_allocator_free(scene->alloc, model->instances,
                       sizeof(VkrMeshInstanceHandle) * model->node_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  vkr_allocator_free(scene->alloc, model, sizeof(*model),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

/* Destroys what the spawn created, children before parents; source nodes
 * list parents first. */
vkr_internal void scene_model_release(VkrScene *scene,
                                      struct VkrSceneModel *model) {
  if (model->animated) {
    vkr_scene_animation_detach(scene, model->wrapper);
  }
  for (uint32_t i = model->nodes ? model->node_count : 0u; i-- > 0;) {
    if (model->nodes[i].u64 != VKR_ENTITY_ID_INVALID.u64 &&
        vkr_scene_entity_alive(scene, model->nodes[i])) {
      vkr_scene_destroy_entity(scene, model->nodes[i]);
    }
  }
  struct VkrRenderAssets *assets = scene->assets;
  for (uint32_t i = 0; i < model->instance_count; ++i) {
    if (vkr_scene_untrack_instance(scene, model->instances[i]) && assets) {
      (void)vkr_mesh_manager_destroy_instance(&assets->mesh_manager,
                                              model->instances[i]);
    }
  }
}

/* Checks shared by both spawns: a live wrapper with a transform for the
 * nodes to parent to, no mesh or animation of its own and no model yet. */
vkr_internal bool8_t scene_model_check(VkrScene *scene,
                                       struct VkrRenderAssets *assets,
                                       VkrEntityId wrapper,
                                       const VkrSceneModelDesc *desc,
                                       const char **error) {
  if (!scene || !scene->world || !assets || !desc || !desc->mesh_path.length ||
      !vkr_scene_entity_alive(scene, wrapper)) {
    *error = "Model spawn needs a live wrapper entity and a mesh path";
    return false_v;
  }
  if (!vkr_scene_physics_mutations_allowed(scene)) {
    *error = "Models spawn only between simulation ticks";
    return false_v;
  }
  if (!vkr_scene_get_transform(scene, wrapper)) {
    *error = "The model wrapper needs a transform to parent its nodes";
    return false_v;
  }
  if (scene_model_find(scene, wrapper) ||
      vkr_entity_has_component(scene->world, wrapper,
                               scene->comp_mesh_renderer) ||
      vkr_scene_animation_get_player(scene, wrapper)) {
    *error = "The model wrapper already carries a mesh or animation";
    return false_v;
  }
  return true_v;
}

/* Creates the model's node entities and mesh instances from loaded
 * requests, and binds the animation when `animation_request` has one. On
 * failure the caller releases what `model` holds. */
vkr_internal bool8_t scene_model_instantiate(
    VkrScene *scene, struct VkrRenderAssets *assets,
    struct VkrSceneModel *model, String8 mesh_path,
    VkrResourceHandleInfo *mesh_request,
    VkrResourceHandleInfo *animation_request,
    const VkrSceneAnimationConfig *animation, const char **error) {
  VkrResourceHandleInfo mesh_resolved = {0};
  if (!vkr_resource_system_try_get_resolved(mesh_request, &mesh_resolved) ||
      !mesh_resolved.as.mesh) {
    *error = "Model mesh failed to load";
    return false_v;
  }
  const VkrMeshSource *source = &mesh_resolved.as.mesh->source;
  if (!source->nodes.length) {
    *error = "Model mesh has no source nodes to instantiate";
    return false_v;
  }

  model->node_count = (uint32_t)source->nodes.length;
  model->nodes =
      vkr_allocator_alloc(scene->alloc, sizeof(VkrEntityId) * model->node_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  model->instances = vkr_allocator_alloc(
      scene->alloc, sizeof(VkrMeshInstanceHandle) * model->node_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  if (!model->nodes || !model->instances ||
      !vkr_scene_instantiate_source_nodes(scene, source, model->wrapper,
                                          UINT32_MAX, model->nodes,
                                          &scene_error)) {
    if (model->nodes) {
      /* A failed instantiation destroys its own entities. */
      for (uint32_t i = 0; i < model->node_count; ++i) {
        model->nodes[i] = VKR_ENTITY_ID_INVALID;
      }
    }
    *error = "Model source nodes could not be instantiated";
    return false_v;
  }

  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
  for (uint32_t n = 0; n < model->node_count; ++n) {
    const VkrMeshSourceNode *node = &source->nodes.data[n];
    if (!node->in_scene || node->mesh_variant == UINT32_MAX ||
        !source->meshes.data[node->mesh_variant].range_count) {
      continue;
    }
    const VkrMeshLoadDesc mesh_desc = {
        .mesh_path = mesh_path,
        .source_mesh_index_plus_one = node->mesh_variant + 1u,
        .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
        .transform = vkr_transform_identity(),
    };
    VkrMeshInstanceHandle instance =
        vkr_mesh_manager_create_instance_from_resource(
            &assets->mesh_manager, &mesh_desc, &mesh_resolved, 0u, true_v,
            &load_error);
    if (!instance.id) {
      *error = "Model mesh instance creation failed";
      return false_v;
    }
    if (!vkr_scene_track_instance(scene, instance, &scene_error)) {
      (void)vkr_mesh_manager_destroy_instance(&assets->mesh_manager, instance);
      *error = "Model mesh instance could not be tracked";
      return false_v;
    }
    model->instances[model->instance_count++] = instance;
    if (!vkr_scene_set_mesh_renderer(scene, model->nodes[n], instance)) {
      *error = "Model mesh renderer could not be attached";
      return false_v;
    }
    (void)vkr_mesh_manager_instance_set_shadow_mobility(
        &assets->mesh_manager, instance, VKR_SHADOW_CASTER_MOBILITY_DYNAMIC);
  }

  if (animation_request->request_id || animation_request->as.animation) {
    const char *animation_error = NULL;
    /* Attach takes both requests on success and clears the handles. */
    if (!vkr_scene_animation_attach(
            scene, model->wrapper, mesh_request, animation_request,
            model->nodes, model->node_count, animation,
            &assets->scratch_allocator, &animation_error)) {
      *error = animation_error ? animation_error
                               : "Model animation could not be attached";
      return false_v;
    }
    model->animated = true_v;
  }
  return true_v;
}

vkr_internal struct VkrSceneModel *scene_model_new(VkrScene *scene,
                                                   VkrEntityId wrapper) {
  struct VkrSceneModel *model = vkr_allocator_alloc(
      scene->alloc, sizeof(*model), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (model) {
    *model = (struct VkrSceneModel){.wrapper = wrapper};
  }
  return model;
}

bool8_t vkr_scene_spawn_model(VkrScene *scene, struct VkrRenderAssets *assets,
                              VkrEntityId wrapper,
                              const VkrSceneModelDesc *desc,
                              const char **error) {
  *error = NULL;
  if (!scene_model_check(scene, assets, wrapper, desc, error)) {
    return false_v;
  }

  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&assets->scratch_allocator);
  VkrResourceHandleInfo mesh_request = {0};
  VkrResourceHandleInfo animation_request = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
  struct VkrSceneModel *model = NULL;
  bool8_t ok = false_v;

  if (!vkr_resource_system_load_sync(VKR_RESOURCE_TYPE_MESH, desc->mesh_path,
                                     &assets->scratch_allocator, &mesh_request,
                                     &load_error) ||
      !mesh_request.as.mesh) {
    *error = "Model mesh failed to load";
    goto cleanup;
  }
  if (desc->animation_path.length &&
      !vkr_resource_system_load_sync(
          VKR_RESOURCE_TYPE_ANIMATION, desc->animation_path,
          &assets->scratch_allocator, &animation_request, &load_error)) {
    *error = "Model animation bank failed to load";
    goto cleanup;
  }
  model = scene_model_new(scene, wrapper);
  if (!model) {
    *error = "Model spawn allocation failed";
    goto cleanup;
  }
  if (!scene_model_instantiate(scene, assets, model, desc->mesh_path,
                               &mesh_request, &animation_request,
                               &desc->animation, error)) {
    goto cleanup;
  }
  model->status = VKR_SCENE_MODEL_READY;
  model->next = scene->models;
  scene->models = model;
  model = NULL;
  ok = true_v;

cleanup:
  if (model) {
    scene_model_release(scene, model);
    scene_model_free(scene, model);
  }
  /* Instances hold their own references; a static model keeps no request. */
  if (mesh_request.as.mesh || mesh_request.request_id) {
    vkr_resource_system_unload(&mesh_request, desc->mesh_path);
  }
  if (animation_request.as.animation || animation_request.request_id) {
    vkr_resource_system_unload(&animation_request, desc->animation_path);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return ok;
}

vkr_internal bool8_t scene_model_copy_path(VkrScene *scene, String8 path,
                                           String8 *out) {
  *out = (String8){0};
  if (!path.length) {
    return true_v;
  }
  uint8_t *copy = vkr_allocator_alloc(scene->alloc, path.length + 1u,
                                      VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!copy) {
    return false_v;
  }
  MemCopy(copy, path.str, path.length);
  copy[path.length] = '\0';
  *out = (String8){.str = copy, .length = path.length};
  return true_v;
}

bool8_t vkr_scene_request_model(VkrScene *scene, struct VkrRenderAssets *assets,
                                VkrEntityId wrapper,
                                const VkrSceneModelDesc *desc,
                                const char **error) {
  *error = NULL;
  if (!scene_model_check(scene, assets, wrapper, desc, error)) {
    return false_v;
  }
  struct VkrSceneModel *model = scene_model_new(scene, wrapper);
  if (!model ||
      !scene_model_copy_path(scene, desc->mesh_path, &model->mesh_path) ||
      !scene_model_copy_path(scene, desc->animation_path,
                             &model->animation_path)) {
    if (model) {
      scene_model_free(scene, model);
    }
    *error = "Model spawn allocation failed";
    return false_v;
  }
  model->assets = assets;
  model->animation = desc->animation;
  model->status = VKR_SCENE_MODEL_LOADING;
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_MESH, model->mesh_path,
                                &assets->scratch_allocator,
                                &model->mesh_request, &load_error) ||
      (model->animation_path.length &&
       !vkr_resource_system_load(VKR_RESOURCE_TYPE_ANIMATION,
                                 model->animation_path,
                                 &assets->scratch_allocator,
                                 &model->animation_request, &load_error))) {
    *error = model->mesh_request.load_state == VKR_RESOURCE_LOAD_STATE_FAILED ||
                     !model->animation_path.length
                 ? "Model mesh failed to load"
                 : "Model animation bank failed to load";
    scene_model_free(scene, model);
    return false_v;
  }
  model->next = scene->models;
  scene->models = model;
  return true_v;
}

/* Where one request stands: ready, still loading or failed. */
vkr_internal VkrSceneModelStatus
scene_model_request_status(const VkrResourceHandleInfo *request) {
  if (!request->request_id && !request->as.custom) {
    /* No request, or a load that completed inline. */
    return request->load_state == VKR_RESOURCE_LOAD_STATE_FAILED
               ? VKR_SCENE_MODEL_FAILED
               : VKR_SCENE_MODEL_READY;
  }
  switch (vkr_resource_system_get_state(request, NULL)) {
  case VKR_RESOURCE_LOAD_STATE_READY:
    return VKR_SCENE_MODEL_READY;
  case VKR_RESOURCE_LOAD_STATE_FAILED:
  case VKR_RESOURCE_LOAD_STATE_CANCELED:
  case VKR_RESOURCE_LOAD_STATE_INVALID:
    return VKR_SCENE_MODEL_FAILED;
  default:
    return VKR_SCENE_MODEL_LOADING;
  }
}

void vkr_scene_models_update(VkrScene *scene) {
  if (!scene || !vkr_scene_physics_mutations_allowed(scene)) {
    return;
  }
  for (struct VkrSceneModel *model = scene->models; model;
       model = model->next) {
    if (model->status != VKR_SCENE_MODEL_LOADING) {
      continue;
    }
    const VkrSceneModelStatus mesh =
        scene_model_request_status(&model->mesh_request);
    const VkrSceneModelStatus animation =
        model->animation_path.length
            ? scene_model_request_status(&model->animation_request)
            : VKR_SCENE_MODEL_READY;
    if (mesh == VKR_SCENE_MODEL_LOADING ||
        animation == VKR_SCENE_MODEL_LOADING) {
      continue;
    }
    const char *error = NULL;
    if (mesh == VKR_SCENE_MODEL_FAILED) {
      error = "Model mesh failed to load";
    } else if (animation == VKR_SCENE_MODEL_FAILED) {
      error = "Model animation bank failed to load";
    } else if (!vkr_scene_entity_alive(scene, model->wrapper) ||
               !vkr_scene_get_transform(scene, model->wrapper)) {
      error = "The model wrapper lost its transform while loading";
    } else if (!scene_model_instantiate(scene, model->assets, model,
                                        model->mesh_path, &model->mesh_request,
                                        &model->animation_request,
                                        &model->animation, &error)) {
      /* Undo the partial instantiation; the record stays to report it. */
      scene_model_release(scene, model);
      if (model->nodes) {
        vkr_allocator_free(scene->alloc, model->nodes,
                           sizeof(VkrEntityId) * model->node_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      if (model->instances) {
        vkr_allocator_free(scene->alloc, model->instances,
                           sizeof(VkrMeshInstanceHandle) * model->node_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      model->nodes = NULL;
      model->instances = NULL;
      model->node_count = 0u;
      model->instance_count = 0u;
      model->animated = false_v;
    }
    /* Instances and the animation binding hold their own references. */
    scene_model_drop_requests(model);
    model->status = error ? VKR_SCENE_MODEL_FAILED : VKR_SCENE_MODEL_READY;
    model->error = error;
    if (error) {
      log_warn("Spawned model %.*s failed: %s", (int)model->mesh_path.length,
               model->mesh_path.str, error);
    }
  }
}

VkrSceneModelStatus vkr_scene_model_status(const VkrScene *scene,
                                           VkrEntityId wrapper,
                                           const char **error) {
  const struct VkrSceneModel *model = scene_model_find(scene, wrapper);
  if (error) {
    *error = model ? model->error : NULL;
  }
  return model ? model->status : VKR_SCENE_MODEL_NONE;
}

void vkr_scene_despawn_model(VkrScene *scene, VkrEntityId wrapper) {
  if (!scene || !vkr_scene_physics_mutations_allowed(scene)) {
    return;
  }
  for (struct VkrSceneModel **cursor = &scene->models; *cursor;
       cursor = &(*cursor)->next) {
    if ((*cursor)->wrapper.u64 == wrapper.u64) {
      /* Unlink first: releasing destroys node entities, whose destroy hook
         must not find this record again. */
      struct VkrSceneModel *model = *cursor;
      *cursor = model->next;
      scene_model_release(scene, model);
      scene_model_free(scene, model);
      return;
    }
  }
}

void vkr_scene_model_entity_destroying(VkrScene *scene, VkrEntityId entity) {
  if (scene && scene->models) {
    vkr_scene_despawn_model(scene, entity);
  }
}

void vkr_scene_models_shutdown(VkrScene *scene) {
  while (scene->models) {
    struct VkrSceneModel *model = scene->models;
    scene->models = model->next;
    scene_model_free(scene, model);
  }
}
