#include "renderer/systems/vkr_pipeline_registry.h"
#include "core/logger.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/vkr_resources.h"

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

  registry->temp_allocator.ctx = registry->temp_arena;
  if (!vkr_allocator_arena(&registry->temp_allocator)) {
    log_fatal("Failed to create pipeline registry temp allocator");
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

  // Optional alias for shader-qualified names
  if (name.str && name.length > 0 && string8_contains_cstr(&name, "shader.")) {
    // Duplicate key for alias (same as above; registry holds the memory)
    char *alias_key = (char *)arena_alloc(
        registry->pipeline_arena, name.length + 1, ARENA_MEMORY_TAG_STRING);
    if (alias_key) {
      MemCopy(alias_key, name.str, (size_t)name.length);
      alias_key[name.length] = '\0';
      VkrPipelineEntry alias_entry = {.id = (handle.id - 1),
                                      .ref_count = 1,
                                      .auto_release = false_v,
                                      .name = alias_key,
                                      .domain = pipeline->domain};
      vkr_hash_table_insert_VkrPipelineEntry(&registry->pipelines_by_name,
                                             alias_key, alias_entry);
    }
  }

  registry->stats.total_pipelines_created++;
  *out_handle = handle;
  return true_v;
}

bool8_t vkr_pipeline_registry_alias_pipeline_name(VkrPipelineRegistry *registry,
                                                  VkrPipelineHandle handle,
                                                  String8 alias,
                                                  VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;
  if (!alias.str || alias.length == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  if (handle.id == 0 || handle.id - 1 >= registry->pipelines.length) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  VkrPipeline *pipeline = &registry->pipelines.data[handle.id - 1];
  char *key = (char *)arena_alloc(registry->pipeline_arena, alias.length + 1,
                                  ARENA_MEMORY_TAG_STRING);
  if (!key) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  MemCopy(key, alias.str, (size_t)alias.length);
  key[alias.length] = '\0';
  VkrPipelineEntry entry = {.id = (handle.id - 1),
                            .ref_count = 1,
                            .auto_release = false_v,
                            .name = key,
                            .domain = pipeline->domain};
  vkr_hash_table_insert_VkrPipelineEntry(&registry->pipelines_by_name, key,
                                         entry);

  return true_v;
}

bool8_t vkr_pipeline_registry_create_from_shader_config(
    VkrPipelineRegistry *registry, const VkrShaderConfig *config,
    VkrPipelineDomain domain, VkrGeometryVertexLayoutType vertex_layout,
    String8 name, VkrPipelineHandle *out_handle, VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(config != NULL, "Config is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;
  *out_handle = VKR_PIPELINE_HANDLE_INVALID;

  Scratch scratch = scratch_create(registry->temp_arena);

  // Vertex input: derive solely from shader config
  uint32_t attr_count = 0, binding_count = 0, stride = 0;
  VkrVertexInputAttributeDescription *attrs = NULL;
  VkrVertexInputBindingDescription *bindings = NULL;

  binding_count = 1;
  stride = (uint32_t)config->attribute_stride;
  bindings = (VkrVertexInputBindingDescription *)arena_alloc(
      registry->temp_arena, sizeof(VkrVertexInputBindingDescription),
      ARENA_MEMORY_TAG_RENDERER);
  bindings[0] = (VkrVertexInputBindingDescription){
      .binding = 0,
      .stride = stride,
      .input_rate = VKR_VERTEX_INPUT_RATE_VERTEX};

  attr_count = config->attribute_count;
  if (attr_count > 0) {
    attrs = (VkrVertexInputAttributeDescription *)arena_alloc(
        registry->temp_arena,
        sizeof(VkrVertexInputAttributeDescription) * (uint64_t)attr_count,
        ARENA_MEMORY_TAG_RENDERER);
    for (uint32_t i = 0; i < attr_count; i++) {
      const VkrShaderAttributeDesc *ad =
          array_get_VkrShaderAttributeDesc(&config->attributes, i);
      VkrVertexFormat fmt = VKR_VERTEX_FORMAT_UNDEFINED;
      switch (ad->type) {
      case SHADER_ATTRIBUTE_TYPE_VEC2:
        fmt = VKR_VERTEX_FORMAT_R32G32_SFLOAT;
        break;
      case SHADER_ATTRIBUTE_TYPE_VEC3:
        fmt = VKR_VERTEX_FORMAT_R32G32B32_SFLOAT;
        break;
      case SHADER_ATTRIBUTE_TYPE_VEC4:
        fmt = VKR_VERTEX_FORMAT_R32G32B32A32_SFLOAT;
        break;
      case SHADER_ATTRIBUTE_TYPE_INT32:
        fmt = VKR_VERTEX_FORMAT_R32_SINT;
        break;
      case SHADER_ATTRIBUTE_TYPE_UINT32:
        fmt = VKR_VERTEX_FORMAT_R32_UINT;
        break;
      default:
        fmt = VKR_VERTEX_FORMAT_UNDEFINED;
        break;
      }
      attrs[i] = (VkrVertexInputAttributeDescription){
          .location = ad->location,
          .binding = 0,
          .format = fmt,
          .offset = ad->offset,
      };
    }
  }

  VkrShaderObjectDescription shader_desc = {
      .file_format = VKR_SHADER_FILE_FORMAT_SPIR_V,
      .file_type = VKR_SHADER_FILE_TYPE_MULTI,
  };

  // Initialize modules per stage present
  for (uint32_t i = 0; i < config->stage_count; i++) {
    VkrShaderStageFile sf = *array_get_VkrShaderStageFile(&config->stages, i);
    if (sf.stage == VKR_SHADER_STAGE_VERTEX) {
      shader_desc.modules[VKR_SHADER_STAGE_VERTEX] =
          (VkrShaderModuleDescription){
              .stages =
                  vkr_shader_stage_flags_from_bits(VKR_SHADER_STAGE_VERTEX_BIT),
              .path = sf.filename,
              .entry_point = sf.entry_point,
          };
    } else if (sf.stage == VKR_SHADER_STAGE_FRAGMENT) {
      shader_desc.modules[VKR_SHADER_STAGE_FRAGMENT] =
          (VkrShaderModuleDescription){
              .stages = vkr_shader_stage_flags_from_bits(
                  VKR_SHADER_STAGE_FRAGMENT_BIT),
              .path = sf.filename,
              .entry_point = sf.entry_point,
          };
    }
  }

  // If all stage paths are identical, treat as single-file multi-entry
  if (config->stage_count > 0) {
    String8 base = {0};
    bool8_t same = true_v;
    for (uint32_t i = 0; i < VKR_SHADER_STAGE_COUNT; i++) {
      if (shader_desc.modules[i].stages.set == 0)
        continue;
      if (base.str == NULL) {
        base = shader_desc.modules[i].path;
      } else {
        if (!string8_equals(&base, &shader_desc.modules[i].path)) {
          same = false_v;
          break;
        }
      }
    }

    if (same) {
      shader_desc.file_type = VKR_SHADER_FILE_TYPE_SINGLE;
      // Ensure both stages are present when using single-file multi-entry
      if (shader_desc.modules[VKR_SHADER_STAGE_VERTEX].stages.set == 0) {
        shader_desc.modules[VKR_SHADER_STAGE_VERTEX] =
            (VkrShaderModuleDescription){
                .stages = vkr_shader_stage_flags_from_bits(
                    VKR_SHADER_STAGE_VERTEX_BIT),
                .path = base,
                .entry_point = string8_lit("vertexMain"),
            };
      }

      if (shader_desc.modules[VKR_SHADER_STAGE_FRAGMENT].stages.set == 0) {
        shader_desc.modules[VKR_SHADER_STAGE_FRAGMENT] =
            (VkrShaderModuleDescription){
                .stages = vkr_shader_stage_flags_from_bits(
                    VKR_SHADER_STAGE_FRAGMENT_BIT),
                .path = base,
                .entry_point = string8_lit("fragmentMain"),
            };
      }
    }
  }

  // Validate that required stages are present (vertex + fragment)
  bool8_t has_vs = shader_desc.modules[VKR_SHADER_STAGE_VERTEX].stages.set != 0;
  bool8_t has_fs =
      shader_desc.modules[VKR_SHADER_STAGE_FRAGMENT].stages.set != 0;
  if (!has_vs || !has_fs) {
    *out_error = VKR_RENDERER_ERROR_SHADER_COMPILATION_FAILED;
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return false_v;
  }

  shader_desc.global_ubo_size = config->global_ubo_size;
  shader_desc.global_ubo_stride = config->global_ubo_stride;
  shader_desc.instance_ubo_size = config->instance_ubo_size;
  shader_desc.instance_ubo_stride = config->instance_ubo_stride;
  shader_desc.push_constant_size = config->push_constant_size;
  shader_desc.global_texture_count = config->global_texture_count;
  shader_desc.instance_texture_count = config->instance_texture_count;
  // NOTE: global/instance texture counts are used to build descriptor layouts
  // in the backend when present in config.

  VkrGraphicsPipelineDescription desc = {
      .shader_object_description = shader_desc,
      .attribute_count = attr_count,
      .attributes = attrs,
      .binding_count = binding_count,
      .bindings = bindings,
      .topology = VKR_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
      .polygon_mode = VKR_POLYGON_MODE_FILL,
      .domain = domain,
  };

  if (!vkr_pipeline_registry_create_graphics_pipeline(registry, &desc, name,
                                                      out_handle, out_error)) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return false_v;
  }

  // Alias the created pipeline with shader-qualified key for lookups by
  // material shader name.
  if (config->name.str && config->name.length > 0) {
    String8 alias = string8_create_formatted(scratch.arena, "%s_%u",
                                             (const char *)config->name.str,
                                             (uint32_t)vertex_layout);
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(registry, *out_handle, alias,
                                              &alias_err);
  }

  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
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
  VkrPipelineDomain prev_domain = registry->state.current_domain;
  bool8_t was_bound = registry->state.pipeline_bound;
  registry->state.current_pipeline = handle;
  registry->state.current_domain = pipeline->domain;
  registry->state.pipeline_bound = true_v;
  registry->state.global_state_dirty = true_v; // mark globals dirty on bind
  registry->state.frame_pipeline_changes++;
  registry->stats.total_pipeline_binds++;

  return true_v;
}

bool8_t vkr_pipeline_registry_update_global_state(VkrPipelineRegistry *registry,
                                                  const void *global_uniform,
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
  registry->stats.total_global_applies++;
  registry->state.global_state_dirty = false_v;
  return true_v;
}

void vkr_pipeline_registry_mark_global_state_dirty(
    VkrPipelineRegistry *registry) {
  assert_log(registry != NULL, "Registry is NULL");
  registry->state.global_state_dirty = true_v;
}

bool8_t vkr_pipeline_registry_acquire_instance_state(
    VkrPipelineRegistry *registry, VkrPipelineHandle handle,
    VkrRendererInstanceStateHandle *out_local_state,
    VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_local_state != NULL, "Out local state is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrPipeline *pipeline = NULL;
  if (!vkr_pipeline_registry_get_pipeline(registry, handle, &pipeline)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrRendererError err = vkr_renderer_acquire_instance_state(
      registry->renderer, pipeline->backend_handle, out_local_state);
  if (err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }
  registry->stats.total_instance_acquired++;

  return true_v;
}

bool8_t vkr_pipeline_registry_release_instance_state(
    VkrPipelineRegistry *registry, VkrPipelineHandle handle,
    VkrRendererInstanceStateHandle local_state, VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrPipeline *pipeline = NULL;
  if (!vkr_pipeline_registry_get_pipeline(registry, handle, &pipeline)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrRendererError err = vkr_renderer_release_instance_state(
      registry->renderer, pipeline->backend_handle, local_state);
  if (err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }
  registry->stats.total_instance_released++;

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

  VkrRendererError err = vkr_renderer_update_instance_state(
      registry->renderer, pipeline->backend_handle, data, material);
  if (err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }
  registry->stats.total_instance_applies++;

  return true_v;
}

void vkr_pipeline_registry_collect_backend_telemetry(
    VkrPipelineRegistry *registry) {
  assert_log(registry != NULL, "Registry is NULL");
  uint64_t avoided =
      vkr_renderer_get_and_reset_descriptor_writes_avoided(registry->renderer);
  registry->stats.total_descriptor_writes_avoided += avoided;
}

bool8_t vkr_pipeline_registry_render_renderable(VkrPipelineRegistry *registry,
                                                const VkrRenderable *renderable,
                                                const void *global_uniform,
                                                VkrRendererError *out_error) {
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
  // Instance update is handled by the app using
  // vkr_renderer_update_instance_state.
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

bool8_t
vkr_pipeline_registry_render_current_batch(VkrPipelineRegistry *registry,
                                           const void *global_uniform,
                                           VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  // NOTE: batching not yet implemented. Just mark globals clean if set.
  if (global_uniform) {
    registry->state.global_state_dirty = false_v;
    registry->stats.total_global_applies++;
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
bool8_t vkr_pipeline_registry_render_batch(VkrPipelineRegistry *registry,
                                           const VkrRenderable *renderables,
                                           uint32_t count,
                                           const void *global_uniform,
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
    VkrPipelineRegistry *registry, const char *shader_name,
    uint32_t material_pipeline_id, VkrGeometryVertexLayoutType vertex_layout,
    VkrPipelineHandle *out_handle, VkrRendererError *out_error) {
  assert_log(registry != NULL, "Registry is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  *out_error = VKR_RENDERER_ERROR_NONE;

  // For now, map material_pipeline_id to domain if in range; default to WORLD
  VkrPipelineDomain domain = VKR_PIPELINE_DOMAIN_WORLD;
  if (material_pipeline_id <= VKR_PIPELINE_DOMAIN_COMPUTE) {
    domain = (VkrPipelineDomain)material_pipeline_id;
  }

  // Build pipeline name key: prefer shader_name when provided
  Scratch scratch = scratch_create(registry->temp_arena);
  String8 name = {0};
  if (shader_name && shader_name[0] != '\0') {
    name = string8_create_formatted(scratch.arena, "%s_%u", shader_name,
                                    (uint32_t)vertex_layout);
  } else {
    name = string8_create_formatted(scratch.arena, "p_%u_%u", domain,
                                    (uint32_t)vertex_layout);
  }

  // Try to acquire existing
  VkrPipelineHandle found = VKR_PIPELINE_HANDLE_INVALID;
  if (vkr_pipeline_registry_acquire_by_name(registry, name, true_v, &found,
                                            out_error)) {
    *out_handle = found;
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return true_v;
  }

  // After creating, if shader_name is provided, alias this handle to the
  // shader-qualified key for quick lookup next time.
  if (shader_name && shader_name[0] != '\0') {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    String8 alias = string8_create_formatted(
        scratch.arena, "%s_%u", shader_name, (uint32_t)vertex_layout);
    vkr_pipeline_registry_alias_pipeline_name(registry, *out_handle, alias,
                                              &alias_err);
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
