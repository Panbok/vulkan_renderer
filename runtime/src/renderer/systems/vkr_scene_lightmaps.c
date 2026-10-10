/* Scene lightmap sets (ADR-088): binding baked lightmap instances to mesh
 * instances and weighing the set's layers for the frame. */
#include "renderer/systems/vkr_scene_system.h"

#include "core/logger.h"
#include "renderer/systems/vkr_render_assets.h"

#include <stdlib.h>

/* A drawn entity: its mesh instance or generated mesh and, when it has a
 * source identity, its document entity index and the source node it draws
 * (zero for a model without nodes). */
typedef struct SceneLightmapTarget {
  uint32_t entity_index;
  uint32_t instance_index;
  VkrEntityId entity;
  VkrMeshLightmapSlot draw;
} SceneLightmapTarget;

/* An entity's document-stable id. */
typedef struct SceneLightmapRef {
  VkrEntityRef ref;
  VkrEntityId entity;
} SceneLightmapRef;

typedef struct SceneLightmapGather {
  VkrScene *scene;
  SceneLightmapTarget *targets;
  uint32_t target_count;
  uint32_t target_capacity;
  SceneLightmapRef *refs;
  uint32_t ref_count;
  uint32_t ref_capacity;
  bool8_t failed;
} SceneLightmapGather;

vkr_internal bool8_t scene_lightmap_grow(void **items, uint32_t *capacity,
                                         uint32_t count, uint64_t item_size) {
  if (count < *capacity) {
    return true_v;
  }
  const uint32_t grown = *capacity ? *capacity * 2u : 256u;
  void *resized = realloc(*items, (size_t)grown * item_size);
  if (!resized) {
    return false_v;
  }
  *items = resized;
  *capacity = grown;
  return true_v;
}

/* Records a drawn entity of a mesh-renderer or shape chunk. */
vkr_internal void scene_lightmap_gather_target(SceneLightmapGather *gather,
                                               VkrEntityId entity,
                                               VkrMeshLightmapSlot draw) {
  if (!scene_lightmap_grow((void **)&gather->targets, &gather->target_capacity,
                           gather->target_count, sizeof(SceneLightmapTarget))) {
    gather->failed = true_v;
    return;
  }
  const SceneSourceIdentity *identity = vkr_entity_get_component(
      gather->scene->world, entity, gather->scene->comp_source_identity);
  gather->targets[gather->target_count++] = (SceneLightmapTarget){
      .entity_index = identity ? identity->scene_entity_index : UINT32_MAX,
      .instance_index = identity && identity->gltf_node_index != UINT32_MAX
                            ? identity->gltf_node_index
                            : 0u,
      .entity = entity,
      .draw = draw,
  };
}

vkr_internal void scene_lightmap_gather_renderers(const VkrArchetype *arch,
                                                  VkrChunk *chunk, void *user) {
  (void)arch;
  SceneLightmapGather *gather = (SceneLightmapGather *)user;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  const VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  const SceneMeshRenderer *renderers =
      (const SceneMeshRenderer *)vkr_entity_chunk_column(
          chunk, gather->scene->comp_mesh_renderer);
  for (uint32_t i = 0u; i < count && !gather->failed; ++i) {
    if (renderers[i].instance.id != 0u) {
      scene_lightmap_gather_target(
          gather, entities[i],
          (VkrMeshLightmapSlot){.instance = renderers[i].instance,
                                .mesh_index = VKR_INVALID_ID});
    }
  }
}

vkr_internal void scene_lightmap_gather_shapes(const VkrArchetype *arch,
                                               VkrChunk *chunk, void *user) {
  (void)arch;
  SceneLightmapGather *gather = (SceneLightmapGather *)user;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  const VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  const SceneShape *shapes = (const SceneShape *)vkr_entity_chunk_column(
      chunk, gather->scene->comp_shape);
  for (uint32_t i = 0u; i < count && !gather->failed; ++i) {
    if (shapes[i].mesh_index != VKR_INVALID_ID) {
      scene_lightmap_gather_target(
          gather, entities[i],
          (VkrMeshLightmapSlot){.mesh_index = shapes[i].mesh_index});
    }
  }
}

/* Records the document-stable ids of a source-identity or entity-ref chunk:
   model wrappers, node-less entities and entities the editor created. */
vkr_internal void scene_lightmap_gather_refs(const VkrArchetype *arch,
                                             VkrChunk *chunk, void *user) {
  (void)arch;
  SceneLightmapGather *gather = (SceneLightmapGather *)user;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  const VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  for (uint32_t i = 0u; i < count && !gather->failed; ++i) {
    VkrEntityRef ref = {0};
    if (!vkr_scene_entity_ref(gather->scene, entities[i], &ref)) {
      continue;
    }
    if (!scene_lightmap_grow((void **)&gather->refs, &gather->ref_capacity,
                             gather->ref_count, sizeof(SceneLightmapRef))) {
      gather->failed = true_v;
      return;
    }
    gather->refs[gather->ref_count++] = (SceneLightmapRef){
        .ref = ref,
        .entity = entities[i],
    };
  }
}

vkr_internal void scene_lightmap_gather_query(SceneLightmapGather *gather,
                                              VkrComponentTypeId type,
                                              VkrChunkFn fn) {
  VkrQuery query;
  vkr_entity_query_build(gather->scene->world, &type, 1u, NULL, 0u, &query);
  vkr_entity_query_each_chunk(gather->scene->world, &query, fn, gather);
}

/* Orders targets by baked key; targets without a source identity sort
   last. */
vkr_internal int scene_lightmap_target_key_compare(const void *a,
                                                   const void *b) {
  const SceneLightmapTarget *left = (const SceneLightmapTarget *)a;
  const SceneLightmapTarget *right = (const SceneLightmapTarget *)b;
  if (left->entity_index != right->entity_index) {
    return left->entity_index < right->entity_index ? -1 : 1;
  }
  if (left->instance_index != right->instance_index) {
    return left->instance_index < right->instance_index ? -1 : 1;
  }
  return 0;
}

vkr_internal int scene_lightmap_target_entity_compare(const void *a,
                                                      const void *b) {
  const uint64_t left = ((const SceneLightmapTarget *)a)->entity.u64;
  const uint64_t right = ((const SceneLightmapTarget *)b)->entity.u64;
  return left < right ? -1 : left > right ? 1 : 0;
}

vkr_internal int scene_lightmap_ref_compare(const void *a, const void *b) {
  return MemCompare(&((const SceneLightmapRef *)a)->ref,
                    &((const SceneLightmapRef *)b)->ref, sizeof(VkrEntityRef));
}

vkr_internal bool8_t scene_lightmap_ref_is_zero(const uint8_t id[16]) {
  for (uint32_t i = 0u; i < 16u; ++i) {
    if (id[i] != 0u) {
      return false_v;
    }
  }
  return true_v;
}

/* The draw a lightmap instance names: through its document id, the entity
   itself for instance zero, else that entity's source node; without an id,
   the source node of the document entity at its index. */
vkr_internal const SceneLightmapTarget *scene_lightmap_find_target(
    const VkrScene *scene, const SceneLightmapGather *gather,
    const SceneLightmapTarget *by_entity, const VkrLightmapInstance *instance) {
  SceneLightmapTarget key = {
      .entity_index = instance->entity_index,
      .instance_index = instance->instance_index,
  };
  if (!scene_lightmap_ref_is_zero(instance->document_id)) {
    SceneLightmapRef ref_key = {0};
    MemCopy(ref_key.ref.bytes, instance->document_id,
            sizeof(ref_key.ref.bytes));
    const SceneLightmapRef *ref = (const SceneLightmapRef *)bsearch(
        &ref_key, gather->refs, gather->ref_count, sizeof(SceneLightmapRef),
        scene_lightmap_ref_compare);
    if (!ref) {
      return NULL;
    }
    if (instance->instance_index == 0u) {
      const SceneLightmapTarget entity_key = {.entity = ref->entity};
      const SceneLightmapTarget *target = (const SceneLightmapTarget *)bsearch(
          &entity_key, by_entity, gather->target_count,
          sizeof(SceneLightmapTarget), scene_lightmap_target_entity_compare);
      if (target) {
        return target;
      }
    }
    const SceneSourceIdentity *identity = vkr_entity_get_component(
        scene->world, ref->entity, scene->comp_source_identity);
    if (!identity) {
      return NULL;
    }
    key.entity_index = identity->scene_entity_index;
  }
  return (const SceneLightmapTarget *)bsearch(
      &key, gather->targets, gather->target_count, sizeof(SceneLightmapTarget),
      scene_lightmap_target_key_compare);
}

void vkr_scene_reset_lightmaps(VkrScene *scene,
                               struct VkrRenderAssets *assets) {
  if (!scene || !scene->lightmaps) {
    return;
  }
  VkrSceneLightmaps *lightmaps = scene->lightmaps;
  if (assets && lightmaps->bound_count > 0u) {
    for (uint32_t i = 0u; i < lightmaps->bound_count; ++i) {
      lightmaps->bound[i].slot = 0u;
    }
    (void)vkr_mesh_manager_set_lightmap_slots(
        &assets->mesh_manager, lightmaps->bound, lightmaps->bound_count);
  }
  if (assets && lightmaps->texture.id != 0u) {
    vkr_texture_system_release_by_handle(&assets->texture_system,
                                         lightmaps->texture);
  }
  if (assets && lightmaps->direction.id != 0u) {
    vkr_texture_system_release_by_handle(&assets->texture_system,
                                         lightmaps->direction);
  }
  free(lightmaps->instances);
  free(lightmaps->rects);
  free(lightmaps->bound);
  free(lightmaps);
  scene->lightmaps = NULL;
}

uint32_t vkr_scene_bind_lightmaps(VkrScene *scene) {
  VkrSceneLightmaps *lightmaps = scene ? scene->lightmaps : NULL;
  if (!lightmaps || !scene->assets || !scene->world) {
    return 0u;
  }
  if (lightmaps->binding_current &&
      lightmaps->bound_structure_revision == scene->structure_revision &&
      lightmaps->bound_mesh_revision == scene->mesh_revision) {
    return lightmaps->bound_count;
  }

  SceneLightmapGather gather = {.scene = scene};
  scene_lightmap_gather_query(&gather, scene->comp_mesh_renderer,
                              scene_lightmap_gather_renderers);
  scene_lightmap_gather_query(&gather, scene->comp_shape,
                              scene_lightmap_gather_shapes);
  scene_lightmap_gather_query(&gather, scene->comp_source_identity,
                              scene_lightmap_gather_refs);
  scene_lightmap_gather_query(&gather, scene->comp_entity_ref,
                              scene_lightmap_gather_refs);
  /* Slots cleared from the last binding come first, so a draw bound again
     keeps its new slot. */
  const uint64_t capacity =
      (uint64_t)lightmaps->bound_count + lightmaps->instance_count;
  SceneLightmapTarget *by_entity =
      gather.failed
          ? NULL
          : (SceneLightmapTarget *)malloc((size_t)Max(gather.target_count, 1u) *
                                          sizeof(SceneLightmapTarget));
  VkrMeshLightmapSlot *writes =
      by_entity ? (VkrMeshLightmapSlot *)malloc((size_t)Max(capacity, 1u) *
                                                sizeof(VkrMeshLightmapSlot))
                : NULL;
  VkrMeshLightmapSlot *bound =
      writes ? (VkrMeshLightmapSlot *)malloc(
                   (size_t)Max(lightmaps->instance_count, 1u) *
                   sizeof(VkrMeshLightmapSlot))
             : NULL;
  if (!bound) {
    free(by_entity);
    free(writes);
    free(gather.targets);
    free(gather.refs);
    log_error("Scene lightmaps: binding allocation failed");
    return lightmaps->bound_count;
  }
  if (gather.target_count > 0u) {
    MemCopy(by_entity, gather.targets,
            gather.target_count * sizeof(SceneLightmapTarget));
  }
  qsort(gather.targets, gather.target_count, sizeof(SceneLightmapTarget),
        scene_lightmap_target_key_compare);
  qsort(by_entity, gather.target_count, sizeof(SceneLightmapTarget),
        scene_lightmap_target_entity_compare);
  qsort(gather.refs, gather.ref_count, sizeof(SceneLightmapRef),
        scene_lightmap_ref_compare);

  uint32_t write_count = 0u;
  for (uint32_t i = 0u; i < lightmaps->bound_count; ++i) {
    writes[write_count] = lightmaps->bound[i];
    writes[write_count++].slot = 0u;
  }
  uint32_t bound_count = 0u;
  for (uint32_t i = 0u; i < lightmaps->instance_count; ++i) {
    const SceneLightmapTarget *target = scene_lightmap_find_target(
        scene, &gather, by_entity, &lightmaps->instances[i]);
    if (!target) {
      continue;
    }
    VkrMeshLightmapSlot draw = target->draw;
    draw.slot = i + 1u;
    writes[write_count++] = draw;
    bound[bound_count++] = draw;
  }
  (void)vkr_mesh_manager_set_lightmap_slots(&scene->assets->mesh_manager,
                                            writes, write_count);
  free(by_entity);
  free(writes);
  free(gather.targets);
  free(gather.refs);
  if (!lightmaps->binding_current || bound_count != lightmaps->bound_count) {
    log_info("Scene lightmaps: %u of %u instances bound", bound_count,
             lightmaps->instance_count);
  }
  free(lightmaps->bound);
  lightmaps->bound = bound;
  lightmaps->bound_count = bound_count;
  lightmaps->bound_structure_revision = scene->structure_revision;
  lightmaps->bound_mesh_revision = scene->mesh_revision;
  lightmaps->binding_current = true_v;
  return bound_count;
}

void vkr_scene_lightmap_binding(const VkrScene *scene,
                                VkrLightmapBinding *out_binding) {
  MemZero(out_binding, sizeof(*out_binding));
  out_binding->texture = VKR_TEXTURE_HANDLE_INVALID;
  const VkrSceneLightmaps *lightmaps = scene ? scene->lightmaps : NULL;
  if (!lightmaps || lightmaps->texture.id == 0u) {
    return;
  }
  float32_t weights[VKR_LIGHTMAP_SET_MAX_LAYERS];
  vkr_scene_light_layer_weights(scene, lightmaps->layers,
                                lightmaps->layer_count, weights);
  out_binding->texture = lightmaps->texture;
  out_binding->direction = lightmaps->direction;
  out_binding->page_size = lightmaps->page_size;
  out_binding->layer_count = lightmaps->slice_layer_count;
  out_binding->rects = lightmaps->rects;
  out_binding->rect_count = lightmaps->instance_count;
  /* At most two sun keys weigh anything, and a set holds at most
     VKR_LIGHT_LAYER_MAX_LAMP_GROUPS lamp groups. Active layers name texture
     slices within a page. */
  for (uint32_t slice = 0u; slice < lightmaps->slice_layer_count; ++slice) {
    const float32_t weight = weights[lightmaps->slice_layers[slice]];
    if (weight > 0.0f &&
        out_binding->active_layer_count < VKR_LIGHTMAP_MAX_ACTIVE_LAYERS) {
      out_binding->active_layers[out_binding->active_layer_count] = slice;
      out_binding->active_weights[out_binding->active_layer_count++] = weight;
    }
  }
}
