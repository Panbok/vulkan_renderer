#include "script/vkr_script_host.h"

#include "renderer/systems/vkr_scene_animation.h"
#include "renderer/systems/vkr_scene_types.h"
#include <stdio.h>
#include <string.h>

// =============================================================================
// API table: engine entry points whose signatures differ from the ABI
// =============================================================================

static void script_api_log(LogLevel level, const char *message) {
  _log_message(level, "script", 0u, "%s", message ? message : "");
}

static bool8_t script_api_world_matrix(VkrScene *scene, VkrEntityId entity,
                                       Mat4 *out) {
  const SceneTransform *transform =
      scene && vkr_scene_entity_alive(scene, entity)
          ? vkr_scene_get_transform(scene, entity)
          : NULL;
  if (!transform || !out) {
    return false_v;
  }
  *out = transform->world;
  return true_v;
}

static VkrComponentTypeId script_api_register_state(VkrScene *scene,
                                                    const char *name,
                                                    uint32_t size,
                                                    uint32_t align) {
  return vkr_entity_register_component_once(scene->world, name, size, align);
}

static bool8_t script_api_add_state(VkrScene *scene, VkrEntityId entity,
                                    VkrComponentTypeId type,
                                    const void *value) {
  return vkr_entity_add_component(scene->world, entity, type, value);
}

static bool8_t script_api_remove_state(VkrScene *scene, VkrEntityId entity,
                                       VkrComponentTypeId type) {
  return vkr_entity_remove_component(scene->world, entity, type);
}

static bool8_t script_api_has_state(const VkrScene *scene, VkrEntityId entity,
                                    VkrComponentTypeId type) {
  return type != VKR_COMPONENT_TYPE_INVALID &&
         vkr_entity_has_component(scene->world, entity, type);
}

static void *script_api_get_state(VkrScene *scene, VkrEntityId entity,
                                  VkrComponentTypeId type) {
  return vkr_entity_get_component_if_alive(scene->world, entity, type);
}

static bool8_t script_api_simulation_running(const VkrScene *scene) {
  return !scene->physics_paused && !scene->physics_disabled &&
         !scene->simulation.faulted;
}

static bool8_t script_api_input_key_down(InputState *input, Keys key) {
  return input_is_key_down(input, key);
}

static VkrScriptApi script_api_table(void) {
  return (VkrScriptApi){
      .version = VKR_SCRIPT_ABI_VERSION,
      .size = sizeof(VkrScriptApi),
      .log = script_api_log,
      .entity_alive = vkr_scene_entity_alive,
      .create_entity = vkr_scene_create_entity,
      .destroy_entity = vkr_scene_destroy_entity,
      .set_name = vkr_scene_set_name,
      .set_transform = vkr_scene_set_transform,
      .set_parent = vkr_scene_set_parent,
      .set_visibility = vkr_scene_set_visibility,
      .entity_visible = vkr_scene_entity_visible,
      .set_shape = vkr_scene_set_shape,
      .world_matrix = script_api_world_matrix,
      .set_evaluated_transform = vkr_scene_set_evaluated_transform,
      .update_transforms = vkr_scene_update_transforms,
      .get_typed = vkr_scene_get_typed,
      .set_typed = vkr_scene_set_typed,
      .find_typed = vkr_scene_find_typed,
      .register_state = script_api_register_state,
      .add_state = script_api_add_state,
      .remove_state = script_api_remove_state,
      .has_state = script_api_has_state,
      .get_state = script_api_get_state,
      .player_start_type = &vkr_scene_player_start_type,
      .player_start = vkr_scene_player_start,
      .simulation_running = script_api_simulation_running,
      .simulation_completed_ticks = vkr_scene_simulation_completed_ticks,
      .physics_time = vkr_scene_physics_time,
      .physics_debt = vkr_scene_physics_debt,
      .physics_paused = vkr_scene_physics_is_paused,
      .gravity = vkr_scene_gravity,
      .physics_default = vkr_scene_physics_default,
      .physics_apply = vkr_scene_physics_apply,
      .physics_impulse = vkr_scene_physics_impulse,
      .raycast = vkr_scene_physics_raycast_query,
      .sweep_sphere = vkr_scene_physics_sweep_sphere,
      .character_default = vkr_physics_character_default,
      .character_create = vkr_scene_character_create,
      .character_destroy = vkr_scene_character_destroy,
      .character_get_state = vkr_scene_character_get_state,
      .character_step = vkr_scene_character_step,
      .animation_player = vkr_scene_animation_get_player,
      .animation_graph = vkr_scene_animation_get_graph,
      .animation_asset = vkr_animation_player_asset,
      .animation_global_pose = vkr_animation_player_global_pose,
      .animation_select_clip = vkr_animation_player_select_clip,
      .animation_crossfade = vkr_animation_player_crossfade,
      .animation_set_playing = vkr_animation_player_set_playing,
      .animation_set_rate = vkr_animation_player_set_rate,
      .animation_rate = vkr_animation_player_rate,
      .animation_duration = vkr_animation_player_duration,
      .animation_time = vkr_animation_player_time,
      .input_observe = input_observe,
      .input_unobserve = input_unobserve,
      .input_key_down = script_api_input_key_down,
  };
}

// =============================================================================
// Simulation callbacks
// =============================================================================

/* Copies a module failure beside its name; the coordinator copies it again
 * before publishing the fault. */
static const char *script_host_fail(VkrScriptHost *host, uint32_t module,
                                    const char *error) {
  snprintf(host->error, sizeof(host->error), "%s: %s",
           host->modules[module]->name, error ? error : "script failed");
  return host->error;
}

static bool8_t script_host_before_physics(VkrScene *scene, uint64_t tick,
                                          void *context) {
  VkrScriptHost *host = context;
  for (uint32_t i = 0; i < host->module_count; ++i) {
    const VkrScriptModuleDesc *module = host->modules[i];
    if (!host->active[i] || !module->before_physics) {
      continue;
    }
    const char *error = NULL;
    if (!module->before_physics(&host->session, host->states[i], tick,
                                &error)) {
      scene->simulation.error = script_host_fail(host, i, error);
      return false_v;
    }
  }
  return true_v;
}

static bool8_t script_host_after_physics(VkrScene *scene, uint64_t tick,
                                         void *context) {
  VkrScriptHost *host = context;
  for (uint32_t i = 0; i < host->module_count; ++i) {
    const VkrScriptModuleDesc *module = host->modules[i];
    if (!host->active[i] || !module->after_physics) {
      continue;
    }
    const char *error = NULL;
    if (!module->after_physics(&host->session, host->states[i], tick, &error)) {
      scene->simulation.error = script_host_fail(host, i, error);
      return false_v;
    }
  }
  return true_v;
}

static void script_host_reset(VkrScene *scene, void *context) {
  (void)scene;
  VkrScriptHost *host = context;
  for (uint32_t i = 0; i < host->module_count; ++i) {
    const VkrScriptModuleDesc *module = host->modules[i];
    if (host->active[i] && module->reset) {
      module->reset(&host->session, host->states[i]);
    }
  }
}

// =============================================================================
// Lifetime
// =============================================================================

void vkr_script_host_init(VkrScriptHost *host, VkrAllocator *allocator) {
  *host = (VkrScriptHost){.api = script_api_table(), .allocator = allocator};
}

bool8_t vkr_script_host_add_module(VkrScriptHost *host,
                                   VkrScriptModuleEntry entry,
                                   const char **error) {
  const char *failure = NULL;
  const VkrScriptModuleDesc *module = entry ? entry(&host->api) : NULL;
  if (host->started || host->module_count == VKR_SCRIPT_MODULE_MAX) {
    failure = "Script modules register before a session, at most eight";
  } else if (!module || module->abi_version != VKR_SCRIPT_ABI_VERSION ||
             module->size < sizeof(VkrScriptModuleDesc) || !module->name ||
             !module->start || !module->stop) {
    failure = "Script module has an incompatible description";
  } else if (module->state_size &&
             (!module->state_align ||
              (module->state_align & (module->state_align - 1u)))) {
    failure = "Script module state needs a power-of-two alignment";
  }
  for (uint32_t i = 0; !failure && i < module->type_count; ++i) {
    if (!vkr_scene_register_world_type(module->types[i])) {
      failure = "Script component type is invalid or its name is taken";
    }
  }
  /* One state block per module for the process; each session zeroes it. */
  void *state = NULL;
  if (!failure && module->state_size) {
    state = vkr_allocator_alloc_aligned(host->allocator, module->state_size,
                                        module->state_align,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (!state) {
      failure = "Script module state allocation failed";
    }
  }
  if (failure) {
    if (error) {
      *error = failure;
    }
    return false_v;
  }
  host->states[host->module_count] = state;
  host->modules[host->module_count++] = module;
  return true_v;
}

/* Stops the active modules among the first `count` in reverse order. */
static void script_host_stop_modules(VkrScriptHost *host, uint32_t count) {
  while (count) {
    const uint32_t i = --count;
    if (host->active[i]) {
      host->modules[i]->stop(&host->session, host->states[i]);
    }
    host->active[i] = false_v;
  }
}

bool8_t vkr_script_host_start(VkrScriptHost *host, VkrScene *scene,
                              InputState *input, struct VkrRenderAssets *assets,
                              uint32_t flags, const char **error) {
  if (host->started || !scene || !input) {
    if (error) {
      *error = "Script session requires a scene and input and no session";
    }
    return false_v;
  }
  vkr_scene_physics_set_paused(scene, true_v);
  if ((vkr_scene_simulation_completed_ticks(scene) ||
       vkr_scene_physics_debt(scene)) &&
      !vkr_scene_physics_reset(scene, error)) {
    return false_v;
  }
  host->session = (VkrScriptSession){.api = &host->api,
                                     .scene = scene,
                                     .input = input,
                                     .assets = assets,
                                     .instance_id = ++host->last_instance_id,
                                     .flags = flags};
  bool8_t any_active = false_v;
  for (uint32_t i = 0; i < host->module_count; ++i) {
    const VkrScriptModuleDesc *module = host->modules[i];
    if (host->states[i]) {
      MemZero(host->states[i], module->state_size);
    }
    const char *failure = NULL;
    const VkrScriptStart result =
        module->start(&host->session, host->states[i], &failure);
    if (result == VKR_SCRIPT_START_FAILED) {
      script_host_stop_modules(host, i);
      if (error) {
        *error = script_host_fail(host, i, failure);
      }
      return false_v;
    }
    host->active[i] = result == VKR_SCRIPT_START_ACTIVE;
    any_active = any_active || host->active[i];
  }
  if (any_active) {
    const VkrSceneSimulationCallbacks callbacks = {
        .before_physics = script_host_before_physics,
        .after_physics = script_host_after_physics,
        .reset = script_host_reset,
        .context = host};
    if (!vkr_scene_simulation_configure(scene, &callbacks, error)) {
      script_host_stop_modules(host, host->module_count);
      return false_v;
    }
  }
  host->started = true_v;
  return true_v;
}

void vkr_script_host_stop(VkrScriptHost *host) {
  if (!host->started) {
    return;
  }
  VkrScene *scene = host->session.scene;
  vkr_scene_physics_set_paused(scene, true_v);
  // Detach only the host's borrowed context; other clock and fault
  // diagnostics stay with the scene.
  (void)vkr_scene_simulation_detach(scene, host);
  script_host_stop_modules(host, host->module_count);
  host->session = (VkrScriptSession){0};
  host->started = false_v;
}

bool8_t vkr_script_host_active(const VkrScriptHost *host) {
  for (uint32_t i = 0; host->started && i < host->module_count; ++i) {
    if (host->active[i]) {
      return true_v;
    }
  }
  return false_v;
}

void vkr_script_host_frame(VkrScriptHost *host, VkrScriptFrame *frame) {
  for (uint32_t i = 0; host->started && i < host->module_count; ++i) {
    const VkrScriptModuleDesc *module = host->modules[i];
    if (host->active[i] && module->frame) {
      module->frame(&host->session, host->states[i], frame);
    }
  }
}

void vkr_script_host_present(VkrScriptHost *host, const VkrScriptFrame *frame,
                             VkrScriptView *view) {
  *view = (VkrScriptView){0};
  for (uint32_t i = 0; host->started && i < host->module_count; ++i) {
    const VkrScriptModuleDesc *module = host->modules[i];
    if (!host->active[i] || !module->present) {
      continue;
    }
    VkrScriptView module_view = {0};
    module->present(&host->session, host->states[i], frame, &module_view);
    if (module_view.camera_valid) {
      view->camera_valid = true_v;
      view->camera_position = module_view.camera_position;
      view->camera_yaw_degrees = module_view.camera_yaw_degrees;
      view->camera_pitch_degrees = module_view.camera_pitch_degrees;
    }
    if (module_view.hud[0]) {
      const uint32_t length = (uint32_t)strlen(view->hud);
      snprintf(view->hud + length, sizeof(view->hud) - length, "%s%s",
               length ? "\n" : "", module_view.hud);
    }
  }
}
