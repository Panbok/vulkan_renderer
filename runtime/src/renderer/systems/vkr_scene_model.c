#include "renderer/systems/vkr_scene_model.h"

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

vkr_internal void scene_model_free(VkrScene *scene,
                                   struct VkrSceneModel *model) {
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
  for (uint32_t i = model->node_count; i-- > 0;) {
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

bool8_t vkr_scene_spawn_model(VkrScene *scene, struct VkrRenderAssets *assets,
                              VkrEntityId wrapper,
                              const VkrSceneModelDesc *desc,
                              const char **error) {
  *error = NULL;
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
  const bool8_t animated = desc->animation_path.length > 0;
  if (animated &&
      !vkr_resource_system_load_sync(
          VKR_RESOURCE_TYPE_ANIMATION, desc->animation_path,
          &assets->scratch_allocator, &animation_request, &load_error)) {
    *error = "Model animation bank failed to load";
    goto cleanup;
  }
  const VkrMeshSource *source = &mesh_request.as.mesh->source;
  if (!source->nodes.length) {
    *error = "Model mesh has no source nodes to instantiate";
    goto cleanup;
  }

  model = vkr_allocator_alloc(scene->alloc, sizeof(*model),
                              VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!model) {
    *error = "Model spawn allocation failed";
    goto cleanup;
  }
  *model = (struct VkrSceneModel){.wrapper = wrapper,
                                  .node_count = (uint32_t)source->nodes.length};
  model->nodes =
      vkr_allocator_alloc(scene->alloc, sizeof(VkrEntityId) * model->node_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  model->instances = vkr_allocator_alloc(
      scene->alloc, sizeof(VkrMeshInstanceHandle) * model->node_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  if (!model->nodes || !model->instances ||
      !vkr_scene_instantiate_source_nodes(scene, source, wrapper, UINT32_MAX,
                                          model->nodes, &scene_error)) {
    if (model->nodes) {
      /* A failed instantiation destroys its own entities. */
      for (uint32_t i = 0; i < model->node_count; ++i) {
        model->nodes[i] = VKR_ENTITY_ID_INVALID;
      }
    }
    *error = "Model source nodes could not be instantiated";
    goto cleanup;
  }

  for (uint32_t n = 0; n < model->node_count; ++n) {
    const VkrMeshSourceNode *node = &source->nodes.data[n];
    if (!node->in_scene || node->mesh_variant == UINT32_MAX ||
        !source->meshes.data[node->mesh_variant].range_count) {
      continue;
    }
    const VkrMeshLoadDesc mesh_desc = {
        .mesh_path = desc->mesh_path,
        .source_mesh_index_plus_one = node->mesh_variant + 1u,
        .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
        .transform = vkr_transform_identity(),
    };
    VkrMeshInstanceHandle instance =
        vkr_mesh_manager_create_instance_from_resource(
            &assets->mesh_manager, &mesh_desc, &mesh_request, 0u, true_v,
            &load_error);
    if (!instance.id) {
      *error = "Model mesh instance creation failed";
      goto cleanup;
    }
    if (!vkr_scene_track_instance(scene, instance, &scene_error)) {
      (void)vkr_mesh_manager_destroy_instance(&assets->mesh_manager, instance);
      *error = "Model mesh instance could not be tracked";
      goto cleanup;
    }
    model->instances[model->instance_count++] = instance;
    if (!vkr_scene_set_mesh_renderer(scene, model->nodes[n], instance)) {
      *error = "Model mesh renderer could not be attached";
      goto cleanup;
    }
    (void)vkr_mesh_manager_instance_set_shadow_mobility(
        &assets->mesh_manager, instance, VKR_SHADOW_CASTER_MOBILITY_DYNAMIC);
  }

  if (animated) {
    const char *animation_error = NULL;
    /* Attach takes both requests on success and clears the handles. */
    if (!vkr_scene_animation_attach(
            scene, wrapper, &mesh_request, &animation_request, model->nodes,
            model->node_count, &desc->animation, &assets->scratch_allocator,
            &animation_error)) {
      *error = animation_error ? animation_error
                               : "Model animation could not be attached";
      goto cleanup;
    }
    model->animated = true_v;
  }
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
