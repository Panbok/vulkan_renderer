#include "renderer/systems/vkr_pipeline_registry.h"
#include "core/logger.h"
#include "renderer/systems/vkr_geometry_system.h"

// TODO: we need to impl batch rendering, state caching, etc.

vkr_internal INLINE void
vkr__reset_registry_state(VkrPipelineRegistry *registry) {
  assert_log(registry != NULL, "Registry is NULL");

  registry->state.current_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  registry->state.current_domain = VKR_PIPELINE_DOMAIN_WORLD;
  registry->state.global_state_dirty = true_v;
  registry->state.pipeline_bound = false_v;
  registry->state.frame_pipeline_changes = 0;
  registry->state.frame_redundant_binds_avoided = 0;
}

bool8_t vkr_pipeline_registry_init(VkrPipelineRegistry *registry,
                                   VkrRendererFrontendHandle renderer,
                                   const VkrPipelineRegistryConfig *config) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");

  MemZero(registry, sizeof(*registry));
  registry->renderer = renderer;
  registry->config = config ? *config : VKR_PIPELINE_REGISTRY_CONFIG_DEFAULT;

  ArenaFlags flags = bitset8_create();
  bitset8_set(&flags, ARENA_FLAG_LARGE_PAGES);
  registry->pipeline_arena =
      arena_create(VKR_PIPELINE_REGISTRY_DEFAULT_ARENA_RSV,
                   VKR_PIPELINE_REGISTRY_DEFAULT_ARENA_CMT, flags);
  registry->temp_arena = arena_create(KB(64), KB(64));
  if (!registry->pipeline_arena || !registry->temp_arena) {
    log_fatal("Failed to create pipeline registry arenas");
    if (registry->pipeline_arena)
      arena_destroy(registry->pipeline_arena);
    if (registry->temp_arena)
      arena_destroy(registry->temp_arena);
    MemZero(registry, sizeof(*registry));
    return false_v;
  }

  uint32_t max_pipelines = registry->config.max_pipeline_count;
  registry->pipelines =
      array_create_VkrPipeline(registry->pipeline_arena, max_pipelines);
  for (uint32_t pipeline = 0; pipeline < registry->pipelines.length;
       pipeline++) {
    registry->pipelines.data[pipeline].handle = (VkrPipelineHandle){0};
    registry->pipelines.data[pipeline].backend_handle = NULL;
    registry->pipelines.data[pipeline].domain = VKR_PIPELINE_DOMAIN_WORLD;
  }

  registry->pipelines_by_name = vkr_hash_table_create_VkrPipelineEntry(
      registry->pipeline_arena, ((uint64_t)max_pipelines) * 2ULL);

  registry->free_ids =
      array_create_uint32_t(registry->pipeline_arena, max_pipelines);
  registry->free_count = 0;
  registry->next_free_index = 0;
  registry->generation_counter = 1;

  for (uint32_t domain = 0; domain < VKR_PIPELINE_DOMAIN_COUNT; domain++) {
    registry->pipelines_by_domain[domain] = array_create_VkrPipelineHandle(
        registry->pipeline_arena, registry->config.max_pipelines_per_domain);
  }

  MemZero(&registry->stats, sizeof(registry->stats));
  vkr__reset_registry_state(registry);
  return true_v;
}

bool8_t vkr_pipeline_registry_shutdown(VkrPipelineRegistry *registry) {
  if (!registry)
    return true_v;

  for (uint32_t pipeline_id = 0; pipeline_id < registry->pipelines.length;
       pipeline_id++) {
    VkrPipeline *pipeline = &registry->pipelines.data[pipeline_id];
    if (pipeline->handle.id != 0 && pipeline->backend_handle) {
      vkr_renderer_destroy_pipeline(registry->renderer,
                                    pipeline->backend_handle);
      pipeline->backend_handle = NULL;
      pipeline->handle.id = 0;
    }
  }

  if (registry->pipeline_arena)
    arena_destroy(registry->pipeline_arena);
  if (registry->temp_arena)
    arena_destroy(registry->temp_arena);
  MemZero(registry, sizeof(*registry));
  return true_v;
}

vkr_internal INLINE VkrPipeline *
vkr__acquire_pipeline_slot(VkrPipelineRegistry *registry,
                           VkrPipelineHandle *out_handle) {
  if (registry->free_count > 0) {
    uint32_t slot = registry->free_ids.data[registry->free_count - 1];
    registry->free_count--;
    VkrPipeline *pipeline = &registry->pipelines.data[slot];
    pipeline->handle.id = slot + 1;
    pipeline->handle.generation = registry->generation_counter++;
    *out_handle = pipeline->handle;
    return pipeline;
  }

  for (uint32_t pipeline_id = 0; pipeline_id < registry->pipelines.length;
       pipeline_id++) {
    VkrPipeline *pipeline = &registry->pipelines.data[pipeline_id];
    if (pipeline->handle.id == 0 && pipeline->handle.generation == 0) {
      pipeline->handle.id = pipeline_id + 1;
      pipeline->handle.generation = registry->generation_counter++;
      *out_handle = pipeline->handle;
      return pipeline;
    }
  }
  return NULL;
}

bool8_t vkr_pipeline_registry_create_graphics_pipeline(
    VkrPipelineRegistry *registry, const VkrGraphicsPipelineDescription *desc,
    String8 name, VkrPipelineHandle *out_handle, VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(desc != NULL, "Description is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;
  *out_handle = VKR_PIPELINE_HANDLE_INVALID;

  VkrPipelineHandle handle = {0};
  VkrPipeline *pipeline = vkr__acquire_pipeline_slot(registry, &handle);
  if (!pipeline) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  pipeline->description = *desc;
  pipeline->domain = desc->domain;

  VkrPipelineOpaqueHandle backend = vkr_renderer_create_graphics_pipeline(
      registry->renderer, &pipeline->description, out_error);
  if (*out_error != VKR_RENDERER_ERROR_NONE || backend == NULL) {
    // release slot
    pipeline->handle.id = 0;
    return false_v;
  }
  pipeline->backend_handle = backend;

  {
    Array_VkrPipelineHandle *domain_list =
        &registry->pipelines_by_domain[pipeline->domain];
    for (uint32_t i = 0; i < domain_list->length; i++) {
      if (domain_list->data[i].id == 0) {
        domain_list->data[i] = handle;
        break;
      }
    }
  }

  if (name.str && name.length > 0) {
    // Store lifetime entry by name
    char *key = (char *)arena_alloc(registry->pipeline_arena, name.length + 1,
                                    ARENA_MEMORY_TAG_STRING);
    MemCopy(key, name.str, (size_t)name.length);
    key[name.length] = '\0';
    VkrPipelineEntry entry = {.id = (handle.id - 1),
                              .ref_count = 1,
                              .auto_release = false_v,
                              .name = key,
                              .domain = pipeline->domain};
    vkr_hash_table_insert_VkrPipelineEntry(&registry->pipelines_by_name, key,
                                           entry);
  }

  registry->stats.total_pipelines_created++;
  *out_handle = handle;
  return true_v;
}

bool8_t vkr_pipeline_registry_create_from_material_layout(
    VkrPipelineRegistry *registry, VkrPipelineDomain domain,
    VkrGeometryVertexLayoutType vertex_layout, String8 shader_path,
    String8 name, VkrPipelineHandle *out_handle, VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;
  *out_handle = VKR_PIPELINE_HANDLE_INVALID;

  Scratch scratch = scratch_create(registry->temp_arena);
  uint32_t attr_count = 0, binding_count = 0, stride = 0;
  VkrVertexInputAttributeDescription *attrs = NULL;
  VkrVertexInputBindingDescription *bindings = NULL;
  vkr_geometry_fill_vertex_input_descriptions(
      vertex_layout, scratch.arena, &attr_count, &attrs, &binding_count,
      &bindings, &stride);

  VkrShaderObjectDescription shader_desc = {
      .file_format = VKR_SHADER_FILE_FORMAT_SPIR_V,
      .file_type = VKR_SHADER_FILE_TYPE_SINGLE,
      .modules = {[VKR_SHADER_STAGE_VERTEX] =
                      {.stages = vkr_shader_stage_flags_from_bits(
                           VKR_SHADER_STAGE_VERTEX_BIT),
                       .path = shader_path,
                       .entry_point = string8_lit("vertexMain")},
                  [VKR_SHADER_STAGE_FRAGMENT] =
                      {.stages = vkr_shader_stage_flags_from_bits(
                           VKR_SHADER_STAGE_FRAGMENT_BIT),
                       .path = shader_path,
                       .entry_point = string8_lit("fragmentMain")}},
  };

  VkrGraphicsPipelineDescription desc = {
      .shader_object_description = shader_desc,
      .attribute_count = attr_count,
      .attributes = attrs,
      .binding_count = binding_count,
      .bindings = bindings,
      .topology = VKR_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
      .polygon_mode = VKR_POLYGON_MODE_FILL,
      .domain = domain};

  bool8_t ok = vkr_pipeline_registry_create_graphics_pipeline(
      registry, &desc, name, out_handle, out_error);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
  if (!ok)
    return false_v;

  return true_v;
}

bool8_t vkr_pipeline_registry_acquire_by_name(VkrPipelineRegistry *registry,
                                              String8 name,
                                              bool8_t auto_release,
                                              VkrPipelineHandle *out_handle,
                                              VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;
  *out_handle = VKR_PIPELINE_HANDLE_INVALID;

  if (!name.str) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  const char *key = (const char *)name.str;
  VkrPipelineEntry *found =
      vkr_hash_table_get_VkrPipelineEntry(&registry->pipelines_by_name, key);
  if (!found) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }
  found->auto_release = auto_release;
  found->ref_count++;
  VkrPipeline *p = &registry->pipelines.data[found->id];
  *out_handle = p->handle;
  return true_v;
}

bool8_t vkr_pipeline_registry_destroy_pipeline(VkrPipelineRegistry *registry,
                                               VkrPipelineHandle handle) {
  assert_log(registry != NULL, "Registry is NULL");

  if (handle.id == 0)
    return false_v;
  uint32_t idx = handle.id - 1;
  if (idx >= registry->pipelines.length)
    return false_v;
  VkrPipeline *pipeline = &registry->pipelines.data[idx];
  if (pipeline->handle.generation != handle.generation ||
      pipeline->handle.id == 0)
    return false_v;

  if (pipeline->backend_handle) {
    vkr_renderer_destroy_pipeline(registry->renderer, pipeline->backend_handle);
    pipeline->backend_handle = NULL;
  }

  if (registry->free_count >= registry->free_ids.length) {
    log_error("Free list overflow in pipeline registry");
    return false_v;
  }
  registry->free_ids.data[registry->free_count++] = idx;

  pipeline->handle.id = 0;
  return true_v;
}

bool8_t vkr_pipeline_registry_acquire(VkrPipelineRegistry *registry,
                                      VkrPipelineHandle handle) {
  assert_log(registry != NULL, "Registry is NULL");
  if (handle.id == 0)
    return false_v;
  uint32_t idx = handle.id - 1;
  if (idx >= registry->pipelines.length)
    return false_v;
  // Lifetime tracked by name map when available; no-op here for now.
  return true_v;
}

bool8_t vkr_pipeline_registry_release(VkrPipelineRegistry *registry,
                                      VkrPipelineHandle handle) {
  assert_log(registry != NULL, "Registry is NULL");

  if (handle.id == 0)
    return false_v;
  uint32_t idx = handle.id - 1;
  if (idx >= registry->pipelines.length)
    return false_v;
  VkrPipeline *pipeline = &registry->pipelines.data[idx];
  if (pipeline->handle.generation != handle.generation ||
      pipeline->handle.id == 0)
    return false_v;
  pipeline->handle.id = 0;

  return true_v;
}

bool8_t vkr_pipeline_registry_get_pipeline(VkrPipelineRegistry *registry,
                                           VkrPipelineHandle handle,
                                           VkrPipeline **out_pipeline) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_pipeline != NULL, "Out pipeline is NULL");

  if (handle.id == 0)
    return false_v;
  uint32_t idx = handle.id - 1;
  if (idx >= registry->pipelines.length)
    return false_v;
  VkrPipeline *pipeline = &registry->pipelines.data[idx];
  if (pipeline->handle.generation != handle.generation ||
      pipeline->handle.id == 0)
    return false_v;
  *out_pipeline = pipeline;

  return true_v;
}

VkrPipelineHandle
vkr_pipeline_registry_get_current_pipeline(VkrPipelineRegistry *registry) {
  assert_log(registry != NULL, "Registry is NULL");

  return registry->state.current_pipeline;
}

bool8_t vkr_pipeline_registry_is_pipeline_bound(VkrPipelineRegistry *registry,
                                                VkrPipelineHandle handle) {
  assert_log(registry != NULL, "Registry is NULL");

  return (registry->state.pipeline_bound &&
          registry->state.current_pipeline.id == handle.id &&
          registry->state.current_pipeline.generation == handle.generation)
             ? true_v
             : false_v;
}

bool8_t vkr_pipeline_registry_bind_pipeline(VkrPipelineRegistry *registry,
                                            VkrPipelineHandle handle,
                                            VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  if (vkr_pipeline_registry_is_pipeline_bound(registry, handle)) {
    registry->state.frame_redundant_binds_avoided++;
    return true_v;
  }

  VkrPipeline *pipeline = NULL;
  if (!vkr_pipeline_registry_get_pipeline(registry, handle, &pipeline)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  // Vulkan backend binds inside update_state, but we track logical state here
  registry->state.current_pipeline = handle;
  registry->state.current_domain = pipeline->domain;
  registry->state.pipeline_bound = true_v;
  registry->state.global_state_dirty = true_v; // mark globals dirty on bind
  registry->state.frame_pipeline_changes++;
  registry->stats.total_pipeline_binds++;

  return true_v;
}

bool8_t vkr_pipeline_registry_update_global_state(
    VkrPipelineRegistry *registry, const VkrGlobalUniformObject *global_uniform,
    VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;
  if (!registry->state.pipeline_bound) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrPipeline *pipeline = NULL;
  if (!vkr_pipeline_registry_get_pipeline(
          registry, registry->state.current_pipeline, &pipeline)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrRendererError err = vkr_renderer_update_global_state(
      registry->renderer, pipeline->backend_handle, global_uniform);
  if (err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }
  registry->state.global_state_dirty = false_v;
  return true_v;
}

void vkr_pipeline_registry_mark_global_state_dirty(
    VkrPipelineRegistry *registry) {
  assert_log(registry != NULL, "Registry is NULL");
  registry->state.global_state_dirty = true_v;
}

bool8_t vkr_pipeline_registry_acquire_local_state(
    VkrPipelineRegistry *registry, VkrPipelineHandle handle,
    VkrRendererLocalStateHandle *out_local_state, VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_local_state != NULL, "Out local state is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrPipeline *pipeline = NULL;
  if (!vkr_pipeline_registry_get_pipeline(registry, handle, &pipeline)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrRendererError err = vkr_renderer_acquire_local_state(
      registry->renderer, pipeline->backend_handle, out_local_state);
  if (err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }

  return true_v;
}

bool8_t vkr_pipeline_registry_release_local_state(
    VkrPipelineRegistry *registry, VkrPipelineHandle handle,
    VkrRendererLocalStateHandle local_state, VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrPipeline *pipeline = NULL;
  if (!vkr_pipeline_registry_get_pipeline(registry, handle, &pipeline)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrRendererError err = vkr_renderer_release_local_state(
      registry->renderer, pipeline->backend_handle, local_state);
  if (err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }

  return true_v;
}

bool8_t vkr_pipeline_registry_update_local_state(
    VkrPipelineRegistry *registry, VkrPipelineHandle handle,
    const VkrShaderStateObject *data, const VkrRendererMaterialState *material,
    VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  *out_error = VKR_RENDERER_ERROR_NONE;

  VkrPipeline *pipeline = NULL;
  if (!vkr_pipeline_registry_get_pipeline(registry, handle, &pipeline)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrRendererError err = vkr_renderer_update_local_state(
      registry->renderer, pipeline->backend_handle, data, material);
  if (err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }

  return true_v;
}

bool8_t vkr_pipeline_registry_render_renderable(
    VkrPipelineRegistry *registry, const VkrRenderable *renderable,
    const VkrGlobalUniformObject *global_uniform, VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(renderable != NULL, "Renderable is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  // Minimal implementation: assume a single world pipeline already created
  // and bound by caller; update global if dirty, then local state and draw via
  // geometry system outside. This function is a placeholder to satisfy API.
  if (global_uniform && registry->state.global_state_dirty) {
    vkr_pipeline_registry_update_global_state(registry, global_uniform,
                                              out_error);
  }
  // Local update is handled by the app using vkr_renderer_update_local_state.
  return true_v;
}

bool8_t vkr_pipeline_registry_begin_batch(VkrPipelineRegistry *registry,
                                          VkrPipelineDomain domain,
                                          VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  registry->current_batch.active = true_v;
  registry->current_batch.domain = domain;
  registry->current_batch.renderables =
      array_create_VkrRenderable(registry->temp_arena, 1024);
  registry->current_batch.renderable_count = 0;
  *out_error = VKR_RENDERER_ERROR_NONE;

  return true_v;
}

bool8_t vkr_pipeline_registry_add_to_batch(VkrPipelineRegistry *registry,
                                           const VkrRenderable *renderable,
                                           VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(renderable != NULL, "Renderable is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (!registry->current_batch.active) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  if (registry->current_batch.renderables.length > 0 &&
      registry->current_batch.renderable_count <
          registry->current_batch.renderables.length) {
    array_set_VkrRenderable(&registry->current_batch.renderables,
                            registry->current_batch.renderable_count,
                            *renderable);
    registry->current_batch.renderable_count++;
    registry->stats.total_renderables_batched++;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;

  return true_v;
}

bool8_t vkr_pipeline_registry_render_current_batch(
    VkrPipelineRegistry *registry, const VkrGlobalUniformObject *global_uniform,
    VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  // NOTE: batching not yet implemented. Just mark globals clean if set.
  if (global_uniform) {
    registry->state.global_state_dirty = false_v;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;

  return true_v;
}

void vkr_pipeline_registry_end_batch(VkrPipelineRegistry *registry) {
  assert_log(registry != NULL, "Registry is NULL");
  registry->current_batch.active = false_v;
  arena_clear(registry->temp_arena, ARENA_MEMORY_TAG_RENDERER);
}

// todo: implement batch rendering
bool8_t vkr_pipeline_registry_render_batch(
    VkrPipelineRegistry *registry, const VkrRenderable *renderables,
    uint32_t count, const VkrGlobalUniformObject *global_uniform,
    VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  (void)renderables;
  (void)count;
  if (global_uniform) {
    registry->state.global_state_dirty = false_v;
  }
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

bool8_t vkr_pipeline_registry_get_pipelines_by_domain(
    VkrPipelineRegistry *registry, VkrPipelineDomain domain,
    VkrPipelineHandle *out_handles, uint32_t max_handles, uint32_t *out_count) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_count != NULL, "Out count is NULL");
  *out_count = 0;
  Array_VkrPipelineHandle *list = &registry->pipelines_by_domain[domain];
  uint32_t to_copy = (list->length < max_handles) ? list->length : max_handles;
  for (uint32_t i = 0; i < to_copy; i++) {
    out_handles[i] = list->data[i];
  }
  *out_count = to_copy;
  return true_v;
}

bool8_t vkr_pipeline_registry_get_pipeline_for_material(
    VkrPipelineRegistry *registry, uint32_t material_pipeline_id,
    VkrGeometryVertexLayoutType vertex_layout, VkrPipelineHandle *out_handle,
    VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  *out_error = VKR_RENDERER_ERROR_NONE;

  // For now, map material_pipeline_id to domain if in range; default to WORLD
  VkrPipelineDomain domain = VKR_PIPELINE_DOMAIN_WORLD;
  if (material_pipeline_id <= VKR_PIPELINE_DOMAIN_COMPUTE) {
    domain = (VkrPipelineDomain)material_pipeline_id;
  }

  // Make a synthetic name based on domain+layout
  Scratch scratch = scratch_create(registry->temp_arena);
  String8 name = string8_create_formatted(scratch.arena, "p_%u_%u", domain,
                                          (uint32_t)vertex_layout);

  // Try to acquire existing
  VkrPipelineHandle found = VKR_PIPELINE_HANDLE_INVALID;
  if (vkr_pipeline_registry_acquire_by_name(registry, name, true_v, &found,
                                            out_error)) {
    *out_handle = found;
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return true_v;
  }

  // NOTE: In a full impl, shader path would be carried by material/pipeline
  String8 shader_path = string8_lit("assets/cube.spv");
  if (!vkr_pipeline_registry_create_from_material_layout(
          registry, domain, vertex_layout, shader_path, name, out_handle,
          out_error)) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return false_v;
  }
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
  return true_v;
}

void vkr_pipeline_registry_reset_frame_stats(VkrPipelineRegistry *registry) {
  assert_log(registry != NULL, "Registry is NULL");
  registry->state.frame_pipeline_changes = 0;
  registry->state.frame_redundant_binds_avoided = 0;
}

void vkr_pipeline_registry_get_frame_stats(
    VkrPipelineRegistry *registry, uint32_t *out_pipeline_changes,
    uint32_t *out_redundant_binds_avoided) {
  assert_log(registry != NULL, "Registry is NULL");

  if (out_pipeline_changes)
    *out_pipeline_changes = registry->state.frame_pipeline_changes;
  if (out_redundant_binds_avoided)
    *out_redundant_binds_avoided =
        registry->state.frame_redundant_binds_avoided;
}

void vkr_pipeline_registry_get_stats(VkrPipelineRegistry *registry,
                                     uint32_t *out_total_pipelines,
                                     uint32_t *out_total_binds,
                                     uint32_t *out_total_redundant_avoided,
                                     uint32_t *out_total_batched) {
  assert_log(registry != NULL, "Registry is NULL");

  if (out_total_pipelines)
    *out_total_pipelines = registry->stats.total_pipelines_created;
  if (out_total_binds)
    *out_total_binds = registry->stats.total_pipeline_binds;
  if (out_total_redundant_avoided)
    *out_total_redundant_avoided = registry->stats.redundant_binds_avoided;
  if (out_total_batched)
    *out_total_batched = registry->stats.total_renderables_batched;
}
