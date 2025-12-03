#include "renderer/systems/vkr_shader_system.h"
#include "containers/str.h"
#include "core/logger.h"
#include "renderer/resources/vkr_resources.h"

vkr_internal uint8_t *vkr_get_staging_buffer_for_scope(VkrShaderSystem *state,
                                                       VkrShaderScope scope) {
  assert_log(state != NULL, "State is NULL");
  switch (scope) {
  case VKR_SHADER_SCOPE_GLOBAL:
    return state->global_staging;
  case VKR_SHADER_SCOPE_INSTANCE:
    return state->instance_staging;
  case VKR_SHADER_SCOPE_LOCAL:
    return state->local_staging;
  default:
    return NULL;
  }
}

vkr_internal void vkr_ensure_staging_for_shader(VkrShaderSystem *state,
                                                VkrShader *shader) {
  assert_log(state != NULL, "State is NULL");
  assert_log(shader != NULL, "Shader is NULL");

  const uint64_t g_size = shader->config->global_ubo_size;
  const uint64_t i_size = shader->config->instance_ubo_size;
  const uint64_t l_size = shader->config->push_constant_size;

  if (g_size > 0 &&
      (state->global_staging == NULL || state->global_staging_size < g_size)) {
    state->global_staging = vkr_allocator_alloc(
        &state->allocator, g_size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    MemZero(state->global_staging, g_size);
    state->global_staging_size = g_size;
  }

  if (i_size > 0 && (state->instance_staging == NULL ||
                     state->instance_staging_size < i_size)) {
    state->instance_staging = vkr_allocator_alloc(
        &state->allocator, i_size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    MemZero(state->instance_staging, i_size);
    state->instance_staging_size = i_size;
  }

  if (l_size > 0 &&
      (state->local_staging == NULL || state->local_staging_size < l_size)) {
    state->local_staging = vkr_allocator_alloc(
        &state->allocator, l_size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    MemZero(state->local_staging, l_size);
    state->local_staging_size = l_size;
  }
}

vkr_internal bool8_t vkr_validate_shader_id(VkrShaderSystem *state,
                                            uint32_t shader_id) {
  return state && shader_id > 0 && shader_id < state->shaders.length &&
         state->active_shaders.data && // Add this
         (*array_get_bool8_t(&state->active_shaders, shader_id));
}

bool8_t vkr_shader_system_initialize(VkrShaderSystem *state,
                                     VkrShaderSystemConfig cfg) {
  assert_log(state != NULL, "State is NULL");

  ArenaFlags app_arena_flags = bitset8_create();
  bitset8_set(&app_arena_flags, ARENA_FLAG_LARGE_PAGES);
  state->arena = arena_create(MB(32), MB(8), app_arena_flags);
  if (!state->arena) {
    log_fatal("Failed to create shader system arena");
    return false_v;
  }

  state->allocator.ctx = state->arena;
  if (!vkr_allocator_arena(&state->allocator)) {
    log_fatal("Failed to create shader system allocator");
    return false_v;
  }

  state->config = cfg;
  state->shader_count = 0;
  state->current_shader_id = 0;
  state->current_shader = NULL;
  state->name_to_id = vkr_hash_table_create_uint32_t(&state->allocator, 128);
  state->shaders =
      array_create_VkrShader(&state->allocator, cfg.max_shader_count);
  state->active_shaders =
      array_create_bool8_t(&state->allocator, cfg.max_shader_count);
  MemZero(state->shaders.data,
          (uint64_t)cfg.max_shader_count * sizeof(VkrShader));

  // Initialize staging buffers
  state->instance_state = (VkrShaderStateObject){0};
  state->material_state = (VkrRendererMaterialState){0};
  state->global_staging = NULL;
  state->global_staging_size = 0;
  state->instance_staging = NULL;
  state->instance_staging_size = 0;
  state->local_staging = NULL;
  state->local_staging_size = 0;

  log_debug("Shader system initialized: max_shaders=%u",
            (uint32_t)cfg.max_shader_count);
  return true_v;
}

void vkr_shader_system_shutdown(VkrShaderSystem *state) {
  if (!state || !state->arena)
    return;

  // Release all instance resources before destroying arena
  if (state->registry) {
    for (uint32_t i = 1; i < state->shaders.length; i++) {
      if ((*array_get_bool8_t(&state->active_shaders, i))) {
        VkrShader *shader = array_get_VkrShader(&state->shaders, i);

        // Release all instances for this shader
        for (uint32_t j = 0; j < shader->instance_capacity; j++) {
          if (shader->instance_ids[j] != 0) {
            VkrRendererInstanceStateHandle handle = {
                .id = shader->instance_ids[j]};
            VkrPipelineHandle current =
                vkr_pipeline_registry_get_current_pipeline(state->registry);
            if (current.id != 0) {
              VkrRendererError err = VKR_RENDERER_ERROR_NONE;
              vkr_pipeline_registry_release_instance_state(
                  state->registry, current, handle, &err);
            }
          }
        }
      }
    }
  }

  arena_destroy(state->arena);
  state->registry = NULL;
  MemZero(state, sizeof(*state));
}

bool8_t vkr_shader_system_create(VkrShaderSystem *state,
                                 const VkrShaderConfig *cfg) {
  assert_log(state != NULL, "State is NULL");
  assert_log(cfg != NULL, "Config is NULL");

  if (state->shader_count >= state->config.max_shader_count) {
    log_error("Shader system: max shader count reached (%u)",
              (uint32_t)state->config.max_shader_count);
    return false_v;
  }

  if (cfg->name.length > VKR_SHADER_NAME_MAX_LENGTH) {
    log_error("Shader name too long: %u", cfg->name.length);
    return false_v;
  }

  uint32_t new_id = 0;
  for (uint32_t i = 1; i < state->shaders.length;
       i++) { // Start from 1, reserve 0 as invalid
    if (!(*array_get_bool8_t(&state->active_shaders, i))) {
      new_id = i;
      break;
    }
  }

  if (new_id == 0) {
    log_error("No free shader slots available");
    return false_v;
  }

  char *stable_name = vkr_allocator_alloc(&state->allocator,
                                          cfg->name.length + 1,
                                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_name) {
    log_error("Failed to allocate shader name");
    return false_v;
  }
  MemCopy(stable_name, cfg->name.str, cfg->name.length);
  stable_name[cfg->name.length] = '\0';
  vkr_hash_table_insert_uint32_t(&state->name_to_id, stable_name, new_id);

  log_debug("Shader created: %s -> id=%u", string8_cstr(&cfg->name), new_id);

  VkrShader *shader = array_get_VkrShader(&state->shaders, new_id);
  array_set_bool8_t(&state->active_shaders, new_id, true_v);

  shader->name =
      string8_create((uint8_t *)stable_name, cfg->name.length);
  shader->id = new_id;
  shader->config = cfg;

  // Initialize scope tracking
  shader->bound_scope = VKR_SHADER_SCOPE_GLOBAL;
  shader->bound_instance_id = VKR_INVALID_ID;

  // todo: select renderpass based on the shader config

  // Initialize instance resource tracking (simple fixed capacity for now)
  shader->instance_capacity = 1024; // TODO: make configurable
  shader->instance_used_count = 0;
  shader->instance_ids = (uint32_t *)vkr_allocator_alloc(
      &state->allocator, (uint64_t)shader->instance_capacity * sizeof(uint32_t),
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  MemZero(shader->instance_ids,
          (uint64_t)shader->instance_capacity * sizeof(uint32_t));

  shader->instance_free_list = (uint32_t *)vkr_allocator_alloc(
      &state->allocator, (uint64_t)shader->instance_capacity * sizeof(uint32_t),
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  MemZero(shader->instance_free_list,
          (uint64_t)shader->instance_capacity * sizeof(uint32_t));

  shader->instance_free_list_count = shader->instance_capacity;
  for (uint32_t i = 0; i < shader->instance_capacity; i++) {
    shader->instance_free_list[i] = i;
  }

  // Initialize warn-once table for missing uniforms/samplers
  shader->missing_uniform_warnings =
      vkr_hash_table_create_uint8_t(&state->allocator, 64);

  state->shader_count++;
  return true_v;
}

uint32_t vkr_shader_system_get_id(VkrShaderSystem *state,
                                  const char *shader_name) {
  if (!state || !shader_name)
    return 0;
  uint32_t *found =
      vkr_hash_table_get_uint32_t(&state->name_to_id, shader_name);
  return found ? *found : 0;
}

VkrShader *vkr_shader_system_get_by_id(VkrShaderSystem *state,
                                       uint32_t shader_id) {
  if (!state || shader_id == 0 || shader_id >= state->shaders.length ||
      !(*array_get_bool8_t(&state->active_shaders, shader_id)))
    return NULL;
  return array_get_VkrShader(&state->shaders, shader_id);
}

VkrShader *vkr_shader_system_get(VkrShaderSystem *state,
                                 const char *shader_name) {
  if (!state || !shader_name)
    return NULL;
  uint32_t id = vkr_shader_system_get_id(state, shader_name);
  return vkr_shader_system_get_by_id(state, id);
}

bool8_t vkr_shader_system_use(VkrShaderSystem *state, const char *shader_name) {
  assert_log(state != NULL, "State is NULL");
  assert_log(shader_name != NULL, "Shader name is NULL");

  uint32_t id = vkr_shader_system_get_id(state, shader_name);
  if (id == 0 &&
      !vkr_hash_table_contains_uint32_t(&state->name_to_id, shader_name)) {
    // Tolerant: leave current_shader unset; registry path still works
    state->current_shader_id = 0;
    state->current_shader = NULL;
    return false_v;
  }

  state->current_shader_id = id;
  state->current_shader = vkr_shader_system_get_by_id(state, id);
  return true_v;
}

bool8_t vkr_shader_system_use_by_id(VkrShaderSystem *state,
                                    uint32_t shader_id) {
  assert_log(state != NULL, "State is NULL");

  if (shader_id != 0 && !vkr_validate_shader_id(state, shader_id)) {
    log_warn("Invalid shader ID: %u", shader_id);
    state->current_shader_id = 0;
    state->current_shader = NULL;
    return false_v;
  }

  state->current_shader_id = shader_id;
  state->current_shader = vkr_shader_system_get_by_id(state, shader_id);
  return true_v;
}

uint32_t vkr_shader_system_uniform_index(VkrShaderSystem *state,
                                         VkrShader *shader,
                                         const char *uniform_name) {
  if (!state || !shader || !uniform_name)
    return VKR_SHADER_INVALID_UNIFORM_INDEX;
  uint32_t *idx = vkr_hash_table_get_uint32_t(
      &shader->config->uniform_name_to_index, uniform_name);
  return idx ? *idx : VKR_SHADER_INVALID_UNIFORM_INDEX;
}

bool8_t vkr_shader_system_uniform_set(VkrShaderSystem *state,
                                      const char *uniform_name,
                                      const void *value) {
  assert_log(state != NULL, "State is NULL");
  assert_log(uniform_name != NULL, "Uniform name is NULL");
  assert_log(value != NULL, "Value is NULL");

  if (!state->current_shader) {
    log_error("No shader currently bound");
    return false_v;
  }

  if (string_length(uniform_name) > VKR_SHADER_NAME_MAX_LENGTH) {
    log_error("Uniform name too long: %s", uniform_name);
    return false_v;
  }

  if (!(*array_get_bool8_t(&state->active_shaders, state->current_shader_id))) {
    log_error("Attempting to set uniform on inactive shader");
    return false_v;
  }

  VkrShader *shader = state->current_shader;

  uint32_t *idx = vkr_hash_table_get_uint32_t(
      &shader->config->uniform_name_to_index, uniform_name);
  if (!idx || *idx >= shader->config->uniform_count) {
    // Warn only once per shader+uniform
    if (!vkr_hash_table_contains_uint8_t(&shader->missing_uniform_warnings,
                                         uniform_name)) {
      vkr_hash_table_insert_uint8_t(&shader->missing_uniform_warnings,
                                    uniform_name, 1);
      log_warn("Shader '%s': uniform '%s' not found",
               string8_cstr(&shader->name), uniform_name);
    }
    return false_v;
  }

  VkrShaderUniformDesc *uniform =
      array_get_VkrShaderUniformDesc(&shader->config->uniforms, *idx);
  if (uniform->type == SHADER_UNIFORM_TYPE_SAMPLER) {
    log_error("Use vkr_shader_system_sampler_set for sampler uniforms");
    return false_v;
  }

  if (uniform->size == 0) {
    log_error("Uniform '%s' has zero size", uniform_name);
    return false_v;
  }

  shader->bound_scope = uniform->scope;

  vkr_ensure_staging_for_shader(state, shader);

  uint8_t *target_buffer =
      vkr_get_staging_buffer_for_scope(state, uniform->scope);
  if (!target_buffer) {
    log_error("Unknown uniform scope: %d", uniform->scope);
    return false_v;
  }

  MemCopy(target_buffer + uniform->offset, value, (uint64_t)uniform->size);

  return true_v;
}

bool8_t vkr_shader_system_sampler_set(VkrShaderSystem *state,
                                      const char *sampler_name,
                                      VkrTextureOpaqueHandle t) {
  assert_log(state != NULL, "State is NULL");
  assert_log(sampler_name != NULL, "Sampler name is NULL");

  if (!state->current_shader) {
    log_error("No shader currently bound");
    return false_v;
  }

  VkrShader *shader = state->current_shader;

  uint32_t *idx = vkr_hash_table_get_uint32_t(
      &shader->config->uniform_name_to_index, sampler_name);
  if (!idx || *idx >= shader->config->uniform_count) {
    if (!vkr_hash_table_contains_uint8_t(&shader->missing_uniform_warnings,
                                         sampler_name)) {
      vkr_hash_table_insert_uint8_t(&shader->missing_uniform_warnings,
                                    sampler_name, 1);
      log_warn("Shader '%s': sampler '%s' not found",
               string8_cstr(&shader->name), sampler_name);
    }
    return false_v;
  }

  VkrShaderUniformDesc *uniform =
      array_get_VkrShaderUniformDesc(&shader->config->uniforms, *idx);

  if (uniform->type != SHADER_UNIFORM_TYPE_SAMPLER) {
    log_error("Uniform '%s' is not a sampler", sampler_name);
    return false_v;
  }

  if (uniform->scope == VKR_SHADER_SCOPE_INSTANCE) {
    uint32_t slot = uniform->location;
    if (slot < VKR_MAX_INSTANCE_TEXTURES) {
      state->material_state.textures[slot] = t;
      state->material_state.textures_enabled[slot] = true_v;
      if (state->material_state.texture_count <= slot)
        state->material_state.texture_count = slot + 1;
    } else {
      log_warn("Instance sampler slot %u exceeds max %u", slot,
               VKR_MAX_INSTANCE_TEXTURES);
    }
  } else {
    log_warn("Global and local samplers not yet fully supported");
  }

  return true_v;
}

bool8_t vkr_shader_system_uniform_set_by_index(VkrShaderSystem *state,
                                               uint16_t index,
                                               const void *value) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->current_shader != NULL, "Current shader is NULL");
  assert_log(value != NULL, "Value is NULL");

  VkrShader *shader = state->current_shader;
  if (index >= shader->config->uniform_count)
    return false_v;

  const char *name = string8_cstr(
      &array_get_VkrShaderUniformDesc(&shader->config->uniforms, index)->name);
  return vkr_shader_system_uniform_set(state, name, value);
}

bool8_t vkr_shader_system_sampler_set_by_index(VkrShaderSystem *state,
                                               uint16_t index,
                                               VkrTextureOpaqueHandle t) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->current_shader != NULL, "Current shader is NULL");

  VkrShader *shader = state->current_shader;
  if (index >= shader->config->uniform_count)
    return false_v;

  const char *name = string8_cstr(
      &array_get_VkrShaderUniformDesc(&shader->config->uniforms, index)->name);
  return vkr_shader_system_sampler_set(state, name, t);
}

bool8_t vkr_shader_system_apply_global(VkrShaderSystem *state) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->registry != NULL, "Registry is NULL");

  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  if (state->current_shader) {
    vkr_ensure_staging_for_shader(state, state->current_shader);
  }

  const void *global_ptr = (const void *)state->global_staging;
  if (!global_ptr && state->current_shader &&
      state->current_shader->config->global_ubo_size > 0) {
    log_warn("Global staging is NULL while global_ubo_size > 0");
    return false_v;
  }

  if (!vkr_pipeline_registry_update_global_state(state->registry, global_ptr,
                                                 &err)) {
    log_error("shader_system: apply_global failed: %s",
              vkr_renderer_get_error_string(err));
    return false_v;
  }

  return true_v;
}

bool8_t vkr_shader_system_apply_instance(VkrShaderSystem *state) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->current_shader != NULL, "Current shader is NULL");
  assert_log(state->registry != NULL, "Registry is NULL");

  VkrShader *shader = state->current_shader;
  vkr_ensure_staging_for_shader(state, shader);
  VkrPipelineHandle current =
      vkr_pipeline_registry_get_current_pipeline(state->registry);
  if (current.id == 0)
    return false_v;

  state->instance_state.instance_state.id = shader->bound_instance_id;

  state->instance_state.instance_ubo_data = state->instance_staging;
  state->instance_state.instance_ubo_size = shader->config->instance_ubo_size;
  state->instance_state.push_constants_data = state->local_staging;
  state->instance_state.push_constants_size =
      shader->config->push_constant_size;

  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_update_instance_state(
          state->registry, current, &state->instance_state,
          &state->material_state, &err)) {
    log_error("shader_system: apply_instance failed: %s",
              vkr_renderer_get_error_string(err));
    return false_v;
  }

  return true_v;
}

bool8_t vkr_shader_system_bind_instance(VkrShaderSystem *state,
                                        uint32_t instance_id) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->current_shader != NULL, "Current shader is NULL");

  if (!vkr_validate_shader_id(state, state->current_shader_id)) {
    log_error("Current shader is invalid");
    return false_v;
  }

  state->current_shader->bound_instance_id = instance_id;
  return true_v;
}

void vkr_shader_system_set_registry(VkrShaderSystem *state,
                                    VkrPipelineRegistry *registry) {
  if (!state)
    return;
  state->registry = registry;
}

bool8_t vkr_shader_acquire_instance_resources(VkrShaderSystem *state,
                                              VkrShader *shader,
                                              uint32_t *out_instance_id) {
  assert_log(shader != NULL, "Shader is NULL");
  assert_log(out_instance_id != NULL, "Out instance ID is NULL");
  assert_log(state != NULL, "State is NULL");
  assert_log(state->registry != NULL, "Registry is NULL");

  if (shader->instance_free_list_count == 0) {
    log_error("Shader '%s': no free instance slots available",
              string8_cstr(&shader->name));
    return false_v;
  }

  VkrPipelineHandle pipeline_handle = VKR_PIPELINE_HANDLE_INVALID;

  // Try to find pipeline by shader name in registry
  // For now, use current pipeline if it matches shader
  VkrPipelineHandle current =
      vkr_pipeline_registry_get_current_pipeline(state->registry);
  if (current.id != 0) {
    pipeline_handle = current;
  } else {
    log_error("No pipeline bound for shader '%s'", string8_cstr(&shader->name));
    return false_v;
  }

  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  VkrRendererInstanceStateHandle backend_id;
  if (!vkr_pipeline_registry_acquire_instance_state(
          state->registry, pipeline_handle, &backend_id, &err)) {
    log_error("Failed to acquire instance resources for shader '%s': %s",
              string8_cstr(&shader->name), vkr_renderer_get_error_string(err));
    return false_v;
  }

  uint32_t slot =
      shader->instance_free_list[--shader->instance_free_list_count];
  shader->instance_ids[slot] = backend_id.id;
  shader->instance_used_count++;

  *out_instance_id = backend_id.id;

  return true_v;
}

bool8_t vkr_shader_release_instance_resources(VkrShaderSystem *state,
                                              VkrShader *shader,
                                              uint32_t instance_id) {
  assert_log(shader != NULL, "Shader is NULL");
  assert_log(state != NULL, "State is NULL");
  assert_log(state->registry != NULL, "Registry is NULL");
  assert_log(instance_id != VKR_INVALID_ID, "Instance ID is invalid");

  uint32_t slot = UINT32_MAX;
  for (uint32_t i = 0; i < shader->instance_capacity; i++) {
    if (shader->instance_ids[i] == instance_id) {
      slot = i;
      break;
    }
  }

  if (slot == UINT32_MAX) {
    log_warn("Instance ID %u not found in shader '%s'", instance_id,
             string8_cstr(&shader->name));
    return false_v;
  }

  VkrPipelineHandle current =
      vkr_pipeline_registry_get_current_pipeline(state->registry);
  if (current.id == 0) {
    log_warn("No pipeline bound when releasing instance %u", instance_id);
    return false_v;
  }

  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  VkrRendererInstanceStateHandle state_handle = {.id = instance_id};
  if (!vkr_pipeline_registry_release_instance_state(state->registry, current,
                                                    state_handle, &err)) {
    log_error("Failed to release instance resources: %s",
              vkr_renderer_get_error_string(err));
    return false_v;
  }

  shader->instance_free_list[shader->instance_free_list_count++] = slot;
  shader->instance_ids[slot] = 0;
  shader->instance_used_count--;

  return true_v;
}

bool8_t vkr_shader_system_delete_by_id(VkrShaderSystem *state,
                                       uint32_t shader_id) {
  if (!vkr_validate_shader_id(state, shader_id))
    return false_v;

  VkrShader *shader = array_get_VkrShader(&state->shaders, shader_id);

  if (state->registry) {
    for (uint32_t i = 0; i < shader->instance_capacity; i++) {
      if (shader->instance_ids[i] != 0) {
        vkr_shader_release_instance_resources(state, shader,
                                              shader->instance_ids[i]);
      }
    }
  }

  vkr_hash_table_remove_uint32_t(&state->name_to_id,
                                 string8_cstr(&shader->name));

  log_debug("Shader deleted: %s (hash table entries remain)",
            string8_cstr(&shader->name));

  array_set_bool8_t(&state->active_shaders, shader_id, false_v);

  if (state->current_shader_id == shader_id) {
    state->current_shader_id = 0;
    state->current_shader = NULL;
  }

  return true_v;
}

bool8_t vkr_shader_system_delete(VkrShaderSystem *state,
                                 const char *shader_name) {
  uint32_t id = vkr_shader_system_get_id(state, shader_name);
  return vkr_shader_system_delete_by_id(state, id);
}
