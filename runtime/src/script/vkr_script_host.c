#include "script/vkr_script_host.h"

#include "filesystem/filesystem.h"
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

/* A module passes its own descriptors; scenes know the registered copies,
 * which share the name. Engine types resolve to themselves. */
static const VkrTypeDesc *script_api_type(const VkrTypeDesc *type) {
  if (!type || !type->name || vkr_scene_world_type_registered(type)) {
    return type;
  }
  const VkrTypeDesc *named =
      vkr_scene_world_type_named(string8_create_from_cstr(
          (const uint8_t *)type->name, strlen(type->name)));
  return named ? named : type;
}

static const void *script_api_get_typed(const VkrScene *scene,
                                        VkrEntityId entity,
                                        const VkrTypeDesc *type) {
  return vkr_scene_get_typed(scene, entity, script_api_type(type));
}

static bool8_t script_api_set_typed(VkrScene *scene, VkrEntityId entity,
                                    const VkrTypeDesc *type,
                                    const void *value) {
  return vkr_scene_set_typed(scene, entity, script_api_type(type), value);
}

static uint32_t script_api_find_typed(const VkrScene *scene,
                                      const VkrTypeDesc *type,
                                      VkrEntityId *out_entities,
                                      uint32_t capacity) {
  return vkr_scene_find_typed(scene, script_api_type(type), out_entities,
                              capacity);
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
      .get_typed = script_api_get_typed,
      .set_typed = script_api_set_typed,
      .find_typed = script_api_find_typed,
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
      .input_key_down = script_api_input_key_down,
      .physics_world_matrix = vkr_scene_physics_world_matrix,
  };
}

// =============================================================================
// Component type copies
// =============================================================================

static char *script_copy_text(VkrScriptHost *host, const char *text) {
  if (!text) {
    return NULL;
  }
  const uint64_t length = strlen(text);
  char *copy = vkr_allocator_alloc(host->allocator, length + 1u,
                                   VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (copy) {
    MemCopy(copy, text, length + 1u);
  }
  return copy;
}

/* A NULL-terminated list of names, such as enum values or retired keys. */
static const char *const *script_copy_names(VkrScriptHost *host,
                                            const char *const *names) {
  if (!names) {
    return NULL;
  }
  uint32_t count = 0u;
  while (names[count]) {
    ++count;
  }
  const char **copy =
      vkr_allocator_alloc(host->allocator, sizeof(*copy) * (count + 1u),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!copy) {
    return NULL;
  }
  for (uint32_t i = 0; i < count; ++i) {
    copy[i] = script_copy_text(host, names[i]);
  }
  copy[count] = NULL;
  return copy;
}

/* Copies everything a descriptor borrows into host storage, so its strings
 * and property table outlive the library that declared them; the hooks stay
 * the library's until the next rebind. */
static bool8_t script_type_fill(VkrScriptHost *host, VkrTypeDesc *copy,
                                const VkrTypeDesc *source) {
  VkrPropertyDesc *properties = NULL;
  if (source->property_count) {
    properties = vkr_allocator_alloc(
        host->allocator, sizeof(*properties) * source->property_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!properties) {
      return false_v;
    }
  }
  for (uint32_t i = 0; i < source->property_count; ++i) {
    const VkrPropertyDesc *from = &source->properties[i];
    properties[i] = *from;
    properties[i].name = script_copy_text(host, from->name);
    properties[i].label = script_copy_text(host, from->label);
    properties[i].tooltip = script_copy_text(host, from->tooltip);
    properties[i].group = script_copy_text(host, from->group);
    properties[i].unit = script_copy_text(host, from->unit);
    properties[i].zero_label = script_copy_text(host, from->zero_label);
    properties[i].names = script_copy_names(host, from->names);
  }
  *copy = *source;
  copy->name = script_copy_text(host, source->name);
  copy->label = script_copy_text(host, source->label);
  copy->category = script_copy_text(host, source->category);
  copy->properties = properties;
  copy->retired = script_copy_names(host, source->retired);
  return copy->name != NULL;
}

/* Documents and live components keep their bytes across a reload, so a
 * reloaded type must store the same properties at the same places. */
static bool8_t script_type_same_layout(const VkrTypeDesc *left,
                                       const VkrTypeDesc *right) {
  if (strcmp(left->name, right->name) || left->size != right->size ||
      left->align != right->align || left->version != right->version ||
      left->flags != right->flags ||
      left->property_count != right->property_count) {
    return false_v;
  }
  for (uint32_t i = 0; i < left->property_count; ++i) {
    const VkrPropertyDesc *a = &left->properties[i];
    const VkrPropertyDesc *b = &right->properties[i];
    if (strcmp(a->name, b->name) || a->kind != b->kind ||
        a->offset != b->offset || a->capacity != b->capacity) {
      return false_v;
    }
  }
  return true_v;
}

static void script_type_unbind(VkrTypeDesc *copy) {
  copy->defaults = NULL;
  copy->validate = NULL;
  copy->normalize = NULL;
  copy->state = NULL;
}

// =============================================================================
// Simulation and input callbacks
// =============================================================================

static bool8_t script_host_callable(const VkrScriptHost *host, uint32_t i) {
  return host->active[i] && host->modules[i].desc && !host->modules[i].retired;
}

/* Copies a module failure beside its name; the coordinator copies it again
 * before publishing the fault. */
static const char *script_host_fail(VkrScriptHost *host, uint32_t module,
                                    const char *error) {
  snprintf(host->error, sizeof(host->error), "%s: %s",
           host->modules[module].name, error ? error : "script failed");
  return host->error;
}

static bool8_t script_host_before_physics(VkrScene *scene, uint64_t tick,
                                          void *context) {
  VkrScriptHost *host = context;
  for (uint32_t i = 0; i < host->module_count; ++i) {
    const VkrScriptModuleDesc *module = host->modules[i].desc;
    if (!script_host_callable(host, i) || !module->before_physics) {
      continue;
    }
    const char *error = NULL;
    if (!module->before_physics(&host->session, host->modules[i].state, tick,
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
    const VkrScriptModuleDesc *module = host->modules[i].desc;
    if (!script_host_callable(host, i) || !module->after_physics) {
      continue;
    }
    const char *error = NULL;
    if (!module->after_physics(&host->session, host->modules[i].state, tick,
                               &error)) {
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
    const VkrScriptModuleDesc *module = host->modules[i].desc;
    if (script_host_callable(host, i) && module->reset) {
      module->reset(&host->session, host->modules[i].state);
    }
  }
}

static void script_host_input(const VkrInputTransition *transition,
                              void *context) {
  VkrScriptHost *host = context;
  for (uint32_t i = 0; i < host->module_count; ++i) {
    const VkrScriptModuleDesc *module = host->modules[i].desc;
    if (script_host_callable(host, i) && module->input) {
      module->input(&host->session, host->modules[i].state, transition);
    }
  }
}

// =============================================================================
// Libraries
// =============================================================================

/* Loads a byte copy, so the build can be replaced while this one runs and a
 * reload never reuses a cached image of the same path. */
static bool8_t script_copy_file(const char *from, const char *to) {
  FILE *source = file_fopen(from, "rb");
  FILE *target = source ? file_fopen(to, "wb") : NULL;
  bool8_t ok = source && target;
  uint8_t buffer[64u * 1024u];
  while (ok) {
    const size_t read = fread(buffer, 1u, sizeof(buffer), source);
    if (read && fwrite(buffer, 1u, read, target) != read) {
      ok = false_v;
    }
    if (read < sizeof(buffer)) {
      ok = ok && !ferror(source);
      break;
    }
  }
  if (target && fclose(target) != 0) {
    ok = false_v;
  }
  if (source) {
    fclose(source);
  }
  return ok;
}

static void script_remove_file(VkrScriptHost *host, const char *path) {
  if (!path[0]) {
    return;
  }
  const FilePath file =
      file_path_create(path, host->allocator, FILE_PATH_TYPE_ABSOLUTE);
  if (file.path.str) {
    (void)file_remove(&file);
  }
}

static void script_close_library(VkrScriptHost *host,
                                 VkrPlatformLibrary *library, char *path) {
  vkr_platform_library_close(library);
  script_remove_file(host, path);
  path[0] = '\0';
}

/* A superseded library stays mapped while the session may still hold
 * pointers into its code or constants. */
static bool8_t script_retire_library(VkrScriptHost *host,
                                     VkrScriptModule *module) {
  if (!module->library.handle) {
    return true_v;
  }
  if (!host->started) {
    script_close_library(host, &module->library, module->loaded_path);
    return true_v;
  }
  if (host->retired_count == VKR_SCRIPT_RETIRED_LIBRARY_MAX) {
    return false_v;
  }
  VkrScriptRetiredLibrary *retired = &host->retired[host->retired_count++];
  retired->library = module->library;
  snprintf(retired->path, sizeof(retired->path), "%s", module->loaded_path);
  module->library = (VkrPlatformLibrary){0};
  module->loaded_path[0] = '\0';
  return true_v;
}

static void script_close_retired(VkrScriptHost *host) {
  while (host->retired_count) {
    VkrScriptRetiredLibrary *retired = &host->retired[--host->retired_count];
    script_close_library(host, &retired->library, retired->path);
  }
}

// =============================================================================
// Registration
// =============================================================================

static const char *script_validate(const VkrScriptModuleDesc *desc) {
  if (!desc || desc->abi_version != VKR_SCRIPT_ABI_VERSION ||
      desc->size < sizeof(VkrScriptModuleDesc) || !desc->name || !desc->start ||
      !desc->stop) {
    return "Script module has an incompatible description";
  }
  if (desc->type_count > VKR_SCRIPT_MODULE_TYPE_MAX ||
      (desc->type_count && !desc->types)) {
    return "Script module declares more than eight component types";
  }
  for (uint32_t i = 0; i < desc->type_count; ++i) {
    if (!desc->types[i] || !desc->types[i]->name) {
      return "Script component type is missing its descriptor or name";
    }
  }
  if (desc->state_size &&
      (!desc->state_align || (desc->state_align & (desc->state_align - 1u)))) {
    return "Script module state needs a power-of-two alignment";
  }
  return NULL;
}

/* State storage for the module's lifetime, grown only while no session can
 * hold the old block. */
static bool8_t script_state_reserve(VkrScriptHost *host,
                                    VkrScriptModule *module,
                                    const VkrScriptModuleDesc *desc) {
  if (desc->state_size <= module->state_capacity &&
      ((uintptr_t)module->state & (desc->state_align - 1u)) == 0u) {
    return true_v;
  }
  void *state = vkr_allocator_alloc_aligned(host->allocator, desc->state_size,
                                            desc->state_align,
                                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!state) {
    return false_v;
  }
  MemZero(state, desc->state_size);
  module->state = state;
  module->state_capacity = desc->state_size;
  return true_v;
}

/* Registers a new module's copies of its component types. A name another
 * module registered fails; nothing is registered then. */
static const char *script_register(VkrScriptHost *host, const char *name,
                                   const VkrScriptModuleDesc *desc,
                                   bool8_t dynamic, uint32_t *out_index) {
  if (host->module_count == VKR_SCRIPT_MODULE_MAX) {
    return "At most sixteen script modules register";
  }
  for (uint32_t i = 0; i < desc->type_count; ++i) {
    const VkrTypeDesc *type = desc->types[i];
    if (!type || !type->name ||
        vkr_scene_world_type_named(string8_create_from_cstr(
            (const uint8_t *)type->name, strlen(type->name)))) {
      return "Script component type is invalid or its name is taken";
    }
  }
  VkrScriptModule *module = &host->modules[host->module_count];
  *module = (VkrScriptModule){.desc = desc, .dynamic = dynamic};
  snprintf(module->name, sizeof(module->name), "%s", name);
  if (desc->state_size && !script_state_reserve(host, module, desc)) {
    return "Script module state allocation failed";
  }
  for (uint32_t i = 0; i < desc->type_count; ++i) {
    VkrTypeDesc *copy = vkr_allocator_alloc(host->allocator, sizeof(*copy),
                                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (!copy || !script_type_fill(host, copy, desc->types[i]) ||
        !vkr_scene_register_world_type(copy)) {
      /* Earlier copies stay registered: the scene type table has no
         removal. Their module never becomes callable. */
      return "Script component type registration failed";
    }
    module->types[module->type_count++] = copy;
  }
  module->generation = 1u;
  *out_index = host->module_count++;
  return NULL;
}

void vkr_script_host_init(VkrScriptHost *host, VkrAllocator *allocator) {
  *host = (VkrScriptHost){.api = script_api_table(), .allocator = allocator};
}

bool8_t vkr_script_host_add_module(VkrScriptHost *host,
                                   VkrScriptModuleEntry entry,
                                   const char **error) {
  const VkrScriptModuleDesc *desc = entry ? entry(&host->api) : NULL;
  const char *failure = host->started ? "Script modules register before a "
                                        "session"
                                      : script_validate(desc);
  uint32_t index = 0u;
  if (!failure) {
    failure = script_register(host, desc->name, desc, false_v, &index);
  }
  if (failure && error) {
    *error = failure;
  }
  return failure == NULL;
}

const VkrScriptModule *vkr_script_host_module(const VkrScriptHost *host,
                                              const char *name) {
  for (uint32_t i = 0; name && i < host->module_count; ++i) {
    if (!strcmp(host->modules[i].name, name)) {
      return &host->modules[i];
    }
  }
  return NULL;
}

/* Replaces a known module's code. The types keep their copies; their hooks
 * and display metadata follow the new code. */
static const char *script_swap(VkrScriptHost *host, VkrScriptModule *module,
                               const VkrScriptModuleDesc *desc,
                               VkrPlatformLibrary library,
                               const char *loaded_path) {
  if (!script_retire_library(host, module)) {
    return "Too many reloads in one session; stop the simulation first";
  }
  for (uint32_t i = 0; i < module->type_count; ++i) {
    VkrTypeDesc *copy = module->types[i];
    const VkrTypeDesc kept = *copy;
    if (script_type_fill(host, copy, desc->types[i])) {
      /* The registered name stays the pointer scenes compare. */
      copy->name = kept.name;
    } else {
      *copy = kept;
    }
  }
  module->desc = desc;
  module->library = library;
  snprintf(module->loaded_path, sizeof(module->loaded_path), "%s", loaded_path);
  module->retired = false_v;
  module->generation++;
  return NULL;
}

VkrScriptReload vkr_script_host_load_library(VkrScriptHost *host,
                                             const char *name,
                                             const char *library_path,
                                             const char **error) {
  const char *failure = NULL;
  VkrScriptModule *module =
      (VkrScriptModule *)vkr_script_host_module(host, name);
  if (!name || !name[0] || !library_path || !library_path[0]) {
    failure = "A script library needs a module name and a path";
  } else if (module && !module->dynamic) {
    failure = "That module is linked into the executable";
  }
  char loaded[VKR_SCRIPT_PATH_CAPACITY] = {0};
  char entry_name[VKR_SCRIPT_MODULE_NAME_CAPACITY + 32u];
  VkrPlatformLibrary library = {0};
  const VkrScriptModuleDesc *desc = NULL;
  if (!failure) {
    snprintf(loaded, sizeof(loaded), "%s.%u-%u.loaded", library_path,
             vkr_platform_get_process_id(), ++host->load_serial);
    snprintf(entry_name, sizeof(entry_name), "vkr_script_module_%s", name);
    if (!script_copy_file(library_path, loaded)) {
      failure = "The script library could not be copied for loading";
      loaded[0] = '\0';
    } else if (!vkr_platform_library_open(loaded, &library, host->error,
                                          sizeof(host->error))) {
      failure = host->error;
    } else {
      const VkrScriptModuleEntry entry =
          (VkrScriptModuleEntry)(uintptr_t)vkr_platform_library_symbol(
              &library, entry_name);
      desc = entry ? entry(&host->api) : NULL;
      failure = entry ? script_validate(desc)
                      : "The library has no vkr_script_module_<name> entry";
    }
  }

  VkrScriptReload result = VKR_SCRIPT_RELOAD_FAILED;
  if (!failure && !module) {
    uint32_t index = 0u;
    failure = script_register(host, name, desc, true_v, &index);
    if (!failure) {
      module = &host->modules[index];
      module->library = library;
      snprintf(module->loaded_path, sizeof(module->loaded_path), "%s", loaded);
      result = VKR_SCRIPT_RELOAD_LOADED;
    }
  } else if (!failure) {
    const uint32_t index = (uint32_t)(module - host->modules);
    const VkrScriptModuleDesc *old = module->desc;
    if (desc->type_count != module->type_count) {
      failure = "The module's component types changed; reopen the project";
    }
    for (uint32_t i = 0; !failure && i < module->type_count; ++i) {
      if (!script_type_same_layout(module->types[i], desc->types[i])) {
        snprintf(host->error, sizeof(host->error),
                 "Component %s changed its fields; reopen the project to "
                 "apply it",
                 module->types[i]->name);
        failure = host->error;
      }
    }
    const bool8_t running = host->started && host->active[index];
    const bool8_t same_state = old && old->state_size == desc->state_size &&
                               old->state_align == desc->state_align &&
                               old->state_version == desc->state_version;
    if (!failure && running && !same_state) {
      /* The state shape changed: stop with the old code, restart with the
         new one on the same scene. */
      const VkrScriptSession session = host->session;
      vkr_script_host_stop(host);
      failure = script_state_reserve(host, module, desc)
                    ? script_swap(host, module, desc, library, loaded)
                    : "Script module state allocation failed";
      const char *start_error = NULL;
      if (!failure &&
          !vkr_script_host_start(host, session.scene, session.input,
                                 session.assets, session.flags, &start_error)) {
        failure = start_error;
      }
      result = failure ? VKR_SCRIPT_RELOAD_FAILED : VKR_SCRIPT_RELOAD_RESTARTED;
      library = (VkrPlatformLibrary){0};
    } else if (!failure) {
      if (!running && !script_state_reserve(host, module, desc)) {
        failure = "Script module state allocation failed";
      } else {
        failure = script_swap(host, module, desc, library, loaded);
      }
      if (!failure) {
        library = (VkrPlatformLibrary){0};
        if (running && desc->reload) {
          desc->reload(&host->session, module->state);
        }
        result = VKR_SCRIPT_RELOAD_KEPT_STATE;
      }
    }
  }
  if (failure) {
    if (library.handle || loaded[0]) {
      script_close_library(host, &library, loaded);
    }
    if (failure != host->error) {
      snprintf(host->error, sizeof(host->error), "%s", failure);
    }
    if (error) {
      *error = host->error;
    }
  }
  return result;
}

void vkr_script_host_retire_libraries(VkrScriptHost *host) {
  vkr_script_host_stop(host);
  for (uint32_t i = 0; i < host->module_count; ++i) {
    VkrScriptModule *module = &host->modules[i];
    if (!module->dynamic || module->retired) {
      continue;
    }
    for (uint32_t t = 0; t < module->type_count; ++t) {
      script_type_unbind(module->types[t]);
    }
    module->desc = NULL;
    module->retired = true_v;
    script_close_library(host, &module->library, module->loaded_path);
  }
}

void vkr_script_host_shutdown(VkrScriptHost *host) {
  vkr_script_host_retire_libraries(host);
}

// =============================================================================
// Sessions
// =============================================================================

/* Stops the active modules among the first `count` in reverse order. */
static void script_host_stop_modules(VkrScriptHost *host, uint32_t count) {
  while (count) {
    const uint32_t i = --count;
    if (script_host_callable(host, i)) {
      host->modules[i].desc->stop(&host->session, host->modules[i].state);
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
    VkrScriptModule *module = &host->modules[i];
    if (!module->desc || module->retired) {
      continue;
    }
    if (module->state) {
      MemZero(module->state, module->desc->state_size);
    }
    const char *failure = NULL;
    const VkrScriptStart result =
        module->desc->start(&host->session, module->state, &failure);
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
    const char *failure = NULL;
    if (!input_observe(input, script_host_input, host)) {
      failure = "The input already has an observer";
    } else if (!vkr_scene_simulation_configure(scene, &callbacks, &failure)) {
      (void)input_unobserve(input, host);
    }
    if (failure) {
      script_host_stop_modules(host, host->module_count);
      if (error) {
        *error = failure;
      }
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
  (void)input_unobserve(host->session.input, host);
  script_host_stop_modules(host, host->module_count);
  host->session = (VkrScriptSession){0};
  host->started = false_v;
  script_close_retired(host);
}

bool8_t vkr_script_host_active(const VkrScriptHost *host) {
  for (uint32_t i = 0; host->started && i < host->module_count; ++i) {
    if (script_host_callable(host, i)) {
      return true_v;
    }
  }
  return false_v;
}

void vkr_script_host_frame(VkrScriptHost *host, VkrScriptFrame *frame) {
  for (uint32_t i = 0; host->started && i < host->module_count; ++i) {
    const VkrScriptModuleDesc *module = host->modules[i].desc;
    if (script_host_callable(host, i) && module->frame) {
      module->frame(&host->session, host->modules[i].state, frame);
    }
  }
}

void vkr_script_host_present(VkrScriptHost *host, const VkrScriptFrame *frame,
                             VkrScriptView *view) {
  *view = (VkrScriptView){0};
  for (uint32_t i = 0; host->started && i < host->module_count; ++i) {
    const VkrScriptModuleDesc *module = host->modules[i].desc;
    if (!script_host_callable(host, i) || !module->present) {
      continue;
    }
    VkrScriptView module_view = {0};
    module->present(&host->session, host->modules[i].state, frame,
                    &module_view);
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
