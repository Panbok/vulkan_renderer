#include "script/vkr_script_internal.h"

#include "filesystem/filesystem.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_model.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_simulation.h"
#include "renderer/systems/vkr_scene_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Instance data, ledgers and bindings live in the host's own freeable
   memory; temp memory rewinds after every hook. */
#define SCRIPT_INSTANCE_MEMORY_RESERVE MB(64)
#define SCRIPT_INSTANCE_MEMORY_SIZE MB(4)
#define SCRIPT_TEMP_RESERVE MB(16)
#define SCRIPT_TEMP_COMMIT KB(64)
#define SCRIPT_LEDGER_MIN_CAPACITY 16u
/* Entity IDs every attached world keeps free for spawns in one tick. */
#define SCRIPT_TICK_SPAWN_RESERVE 256u
#define SCRIPT_COMMAND_RESERVE MB(16)
#define SCRIPT_COMMAND_COMMIT KB(64)

// =============================================================================
// Component type copies
// =============================================================================

/* Indexed by VkrFieldKind. */
static const VkrPropertyKind s_field_kinds[] = {
    VKR_PROPERTY_BOOL,  VKR_PROPERTY_I32,       VKR_PROPERTY_U32,
    VKR_PROPERTY_F32,   VKR_PROPERTY_ANGLE,     VKR_PROPERTY_VEC2,
    VKR_PROPERTY_VEC3,  VKR_PROPERTY_VEC4,      VKR_PROPERTY_QUAT,
    VKR_PROPERTY_COLOR, VKR_PROPERTY_DIRECTION, VKR_PROPERTY_ENUM,
};

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

/* A NULL-terminated list of names, such as enum values. */
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

/* Copies everything an SDK descriptor borrows into host storage, so its
 * strings and fields outlive the library that declared them; `defaults`
 * stays the library's until the next rebind. */
static bool8_t script_type_fill(VkrScriptHost *host, VkrTypeDesc *copy,
                                const VkrComponentDesc *source) {
  VkrPropertyDesc *properties = NULL;
  if (source->field_count) {
    properties = vkr_allocator_alloc(host->allocator,
                                     sizeof(*properties) * source->field_count,
                                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!properties) {
      return false_v;
    }
  }
  for (uint32_t i = 0; i < source->field_count; ++i) {
    const VkrFieldDesc *from = &source->fields[i];
    properties[i] = (VkrPropertyDesc){
        .name = script_copy_text(host, from->name),
        .label = script_copy_text(host, from->label),
        .tooltip = script_copy_text(host, from->tooltip),
        .group = script_copy_text(host, from->group),
        .unit = script_copy_text(host, from->unit),
        .zero_label = script_copy_text(host, from->zero_label),
        .names = script_copy_names(host, from->names),
        .offset = from->offset,
        .kind = s_field_kinds[from->kind],
        .flags = from->flags,
        .min = from->min,
        .max = from->max,
        .step = from->step,
    };
  }
  *copy = (VkrTypeDesc){
      .name = script_copy_text(host, source->name),
      .label = script_copy_text(host, source->label),
      .category = "Scripts",
      .properties = properties,
      .property_count = source->field_count,
      .size = source->size,
      .align = source->align,
      .defaults = source->defaults,
  };
  return copy->name != NULL;
}

/* Documents and live components keep their bytes across a reload, so a
 * reloaded type must store the same fields at the same places. */
static bool8_t script_type_same_layout(const VkrTypeDesc *left,
                                       const VkrComponentDesc *right) {
  if (strcmp(left->name, right->name) || left->size != right->size ||
      left->align != right->align ||
      left->property_count != right->field_count) {
    return false_v;
  }
  for (uint32_t i = 0; i < left->property_count; ++i) {
    const VkrPropertyDesc *a = &left->properties[i];
    const VkrFieldDesc *b = &right->fields[i];
    if (strcmp(a->name, b->name) || a->kind != s_field_kinds[b->kind] ||
        a->offset != b->offset) {
      return false_v;
    }
  }
  return true_v;
}

/* The module's component types in registration order: its exported
 * components, then its behaviors' components not exported on their own. */
static uint32_t script_module_components(const VkrModuleDesc *desc,
                                         VkrComponentDesc **out) {
  uint32_t count = 0u;
  const uint32_t components = Min(desc->component_count, VKR_SDK_EXPORT_MAX);
  const uint32_t behaviors = Min(desc->behavior_count, VKR_SDK_EXPORT_MAX);
  for (uint32_t i = 0; i < components + behaviors; ++i) {
    VkrComponentDesc *type = i < components
                                 ? desc->components[i]
                                 : desc->behaviors[i - components]->component;
    bool8_t seen = false_v;
    for (uint32_t j = 0; j < count; ++j) {
      seen = seen || out[j] == type || !strcmp(out[j]->name, type->name);
    }
    if (!seen) {
      if (count < VKR_SDK_EXPORT_MAX) {
        out[count] = type;
      }
      ++count;
    }
  }
  return count;
}

const VkrTypeDesc *script_resolve_type(VkrScriptHost *host,
                                       const VkrComponentDesc *type) {
  (void)host;
  if (!type) {
    return NULL;
  }
  if (type->host_binding) {
    return type->host_binding;
  }
  return type->name ? vkr_scene_world_type_named(string8_create_from_cstr(
                          (const uint8_t *)type->name, strlen(type->name)))
                    : NULL;
}

// =============================================================================
// Containers and ledgers
// =============================================================================

VkrScriptContainer *script_container_of(VkrScriptHost *host, uint64_t entity) {
  const VkrEntityId id = {.u64 = entity};
  for (uint32_t i = 0; entity && i < host->container_count; ++i) {
    if (host->containers[i].world_id == id.parts.world) {
      return &host->containers[i];
    }
  }
  return NULL;
}

uint32_t script_container_slot(ScriptCtx *ctx, VkrContainer container) {
  VkrScriptHost *host = ctx->host;
  if (container.id == 0u) {
    return ctx->instance->container;
  }
  return container.id - 1u < host->container_count ? container.id - 1u
                                                   : UINT32_MAX;
}

bool8_t script_ctx_error(ScriptCtx *ctx, const char *message) {
  snprintf(ctx->last_error, sizeof(ctx->last_error), "%s",
           message ? message : "Script call failed");
  return false_v;
}

static void script_ledger_free(VkrScriptHost *host, ScriptLedger *ledger) {
  if (ledger->records) {
    vkr_allocator_free(&host->instance_allocator, ledger->records,
                       sizeof(*ledger->records) * ledger->capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  *ledger = (ScriptLedger){0};
}

/* Drops records whose entity is already gone, as when spawns expired, so a
 * long session's ledger holds what lives. */
static void script_ledger_compact(VkrScriptHost *host, ScriptLedger *ledger) {
  uint32_t kept = 0u;
  for (uint32_t i = 0; i < ledger->count; ++i) {
    const ScriptLedgerRecord record = ledger->records[i];
    if (record.kind == SCRIPT_LEDGER_TASK) {
      ledger->records[kept++] = record;
      continue;
    }
    VkrScriptContainer *container = script_container_of(host, record.entity);
    if (record.kind != SCRIPT_LEDGER_NONE && container &&
        vkr_scene_entity_alive(container->scene,
                               (VkrEntityId){.u64 = record.entity})) {
      ledger->records[kept++] = record;
    }
  }
  ledger->count = kept;
}

bool8_t script_ledger_record(ScriptCtx *ctx, ScriptLedgerKind kind,
                             uint64_t entity, uint32_t aux) {
  VkrScriptHost *host = ctx->host;
  ScriptLedger *ledger = ctx->ledger ? ctx->ledger : &ctx->instance->ledger;
  if (ledger->count == ledger->capacity) {
    script_ledger_compact(host, ledger);
  }
  if (ledger->count == ledger->capacity) {
    const uint32_t capacity =
        Max(SCRIPT_LEDGER_MIN_CAPACITY, ledger->capacity * 2u);
    ScriptLedgerRecord *records = vkr_allocator_alloc(
        &host->instance_allocator, sizeof(*records) * capacity,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!records) {
      return script_ctx_error(ctx, "Script ledger allocation failed");
    }
    if (ledger->count) {
      MemCopy(records, ledger->records, sizeof(*records) * ledger->count);
    }
    if (ledger->records) {
      vkr_allocator_free(&host->instance_allocator, ledger->records,
                         sizeof(*records) * ledger->capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    ledger->records = records;
    ledger->capacity = capacity;
  }
  ledger->records[ledger->count++] =
      (ScriptLedgerRecord){.entity = entity, .aux = aux, .kind = kind};
  return true_v;
}

/* Drops the newest matching record; trailing released records shrink the
 * count. */
static bool8_t script_ledger_drop(ScriptLedger *ledger, ScriptLedgerKind kind,
                                  uint64_t entity, uint32_t aux) {
  for (uint32_t i = ledger->count; i-- > 0u;) {
    ScriptLedgerRecord *record = &ledger->records[i];
    if (record->kind == (uint32_t)kind && record->entity == entity &&
        record->aux == aux) {
      record->kind = SCRIPT_LEDGER_NONE;
      while (ledger->count &&
             ledger->records[ledger->count - 1u].kind == SCRIPT_LEDGER_NONE) {
        --ledger->count;
      }
      return true_v;
    }
  }
  return false_v;
}

void script_ledger_forget(ScriptCtx *ctx, ScriptLedgerKind kind,
                          uint64_t entity, uint32_t aux) {
  VkrScriptInstance *instance = ctx->instance;
  if ((ctx->ledger && script_ledger_drop(ctx->ledger, kind, entity, aux)) ||
      script_ledger_drop(&instance->ledger, kind, entity, aux)) {
    return;
  }
  for (uint32_t i = 0; i < instance->binding_count; ++i) {
    if (script_ledger_drop(&instance->bindings[i].ledger, kind, entity, aux)) {
      return;
    }
  }
}

void script_ledger_release_record(VkrScriptHost *host,
                                  const ScriptLedgerRecord *record) {
  if (record->kind == SCRIPT_LEDGER_TASK) {
    script_task_release(host, record->entity);
    return;
  }
  VkrScriptContainer *container = script_container_of(host, record->entity);
  if (!container || record->kind == SCRIPT_LEDGER_NONE) {
    return;
  }
  VkrScene *scene = container->scene;
  const VkrEntityId entity = {.u64 = record->entity};
  if (!vkr_scene_entity_alive(scene, entity)) {
    return;
  }
  host->transforms_dirty[container - host->containers] = true_v;
  switch ((ScriptLedgerKind)record->kind) {
  case SCRIPT_LEDGER_ENTITY:
    script_destroy_tree(host, scene, entity);
    break;
  case SCRIPT_LEDGER_CHARACTER:
    (void)vkr_scene_character_destroy(scene, entity, NULL);
    break;
  case SCRIPT_LEDGER_MODEL:
    vkr_scene_despawn_model(scene, entity);
    break;
  case SCRIPT_LEDGER_STATE: {
    const VkrComponentTypeId type =
        record->aux < host->state_type_count
            ? host->state_types[record->aux].ids[container - host->containers]
            : VKR_COMPONENT_TYPE_INVALID;
    if (type != VKR_COMPONENT_TYPE_INVALID) {
      (void)vkr_entity_remove_component(scene->world, entity, type);
    }
    break;
  }
  case SCRIPT_LEDGER_RENDER_POSE:
    (void)vkr_scene_set_evaluated_transform(scene, entity, NULL);
    break;
  case SCRIPT_LEDGER_TASK:
  case SCRIPT_LEDGER_NONE:
    break;
  }
}

/* Releases a scope's acquisitions newest first. */
static void script_ledger_release(VkrScriptHost *host, ScriptLedger *ledger) {
  while (ledger->count) {
    const ScriptLedgerRecord record = ledger->records[--ledger->count];
    script_ledger_release_record(host, &record);
  }
}

void script_refresh_scene(VkrScriptHost *host, VkrScene *scene) {
  bool8_t dirty = !scene->child_index_valid;
  for (uint32_t i = 0; i < host->container_count; ++i) {
    if (host->containers[i].scene == scene) {
      dirty = dirty || host->transforms_dirty[i];
      host->transforms_dirty[i] = false_v;
    }
  }
  if (dirty) {
    vkr_scene_update_transforms(scene);
  }
}

void script_destroy_tree(VkrScriptHost *host, VkrScene *scene,
                         VkrEntityId entity) {
  script_refresh_scene(host, scene);
  /* Destroying a child edits the parent's child list, so walk a copy. */
  uint32_t count = 0u;
  const VkrEntityId *children = vkr_scene_get_children(scene, entity, &count);
  const uint64_t temp_position = arena_pos(host->temp);
  VkrEntityId *copy = count ? arena_alloc(host->temp, sizeof(*copy) * count,
                                          ARENA_MEMORY_TAG_ARRAY)
                            : NULL;
  if (copy) {
    MemCopy(copy, children, sizeof(*copy) * count);
    for (uint32_t i = count; i-- > 0u;) {
      if (vkr_scene_entity_alive(scene, copy[i])) {
        script_destroy_tree(host, scene, copy[i]);
      }
    }
  }
  arena_reset_to(host->temp, temp_position, ARENA_MEMORY_TAG_UNKNOWN);
  vkr_scene_destroy_entity(scene, entity);
}

static void script_update_transforms(VkrScriptHost *host) {
  for (uint32_t i = 0; i < host->container_count; ++i) {
    if (host->transforms_dirty[i]) {
      host->transforms_dirty[i] = false_v;
      vkr_scene_update_transforms(host->containers[i].scene);
    }
  }
}

// =============================================================================
// Hook calls
// =============================================================================

typedef struct ScriptCall {
  uint64_t temp_position;
  VkrScriptPhase phase;
  ScriptLedger *ledger;
} ScriptCall;

static void script_call_begin(VkrScriptHost *host, VkrScriptInstance *instance,
                              ScriptLedger *ledger, VkrScriptPhase phase,
                              ScriptCall *saved) {
  *saved = (ScriptCall){.temp_position = arena_pos(host->temp),
                        .phase = host->phase,
                        .ledger = instance->ctx.ledger};
  host->phase = phase;
  instance->ctx.ledger = ledger;
}

/* Rewinds temp memory and refreshes changed transforms outside ticks.
 * False when the hook failed. */
static bool8_t script_call_end(VkrScriptHost *host, VkrScriptInstance *instance,
                               const ScriptCall *saved) {
  arena_reset_to(host->temp, saved->temp_position, ARENA_MEMORY_TAG_UNKNOWN);
  if (host->phase != VKR_SCRIPT_PHASE_TICK) {
    script_update_transforms(host);
  }
  host->phase = saved->phase;
  instance->ctx.ledger = saved->ledger;
  return !instance->failed;
}

static const VkrModuleDesc *script_instance_desc(const VkrScriptHost *host,
                                                 const VkrScriptInstance *i) {
  return i->module == SCRIPT_MODULE_NONE ? NULL : host->modules[i->module].desc;
}

static bool8_t script_instance_callable(const VkrScriptHost *host,
                                        const VkrScriptInstance *instance) {
  return instance->started && !instance->disabled &&
         script_instance_desc(host, instance) &&
         !host->modules[instance->module].retired;
}

/* The behavior's component on its entity, valid until the next structural
 * change; NULL when the entity or component is gone. */
static void *script_binding_component(VkrScriptHost *host,
                                      const VkrScriptInstance *instance,
                                      const ScriptBinding *binding) {
  VkrScriptContainer *container = script_container_of(host, binding->entity);
  const VkrModuleDesc *desc = script_instance_desc(host, instance);
  if (!container || !desc) {
    return NULL;
  }
  const VkrTypeDesc *type =
      script_resolve_type(host, desc->behaviors[binding->behavior]->component);
  const VkrComponentTypeId id = vkr_scene_type_id(container->scene, type);
  return id == VKR_COMPONENT_TYPE_INVALID
             ? NULL
             : vkr_entity_get_component_if_alive(
                   container->scene->world,
                   (VkrEntityId){.u64 = binding->entity}, id);
}

typedef enum ScriptHook {
  SCRIPT_HOOK_START,
  SCRIPT_HOOK_STOP,
  SCRIPT_HOOK_UPDATE,
  SCRIPT_HOOK_LATE_UPDATE,
  SCRIPT_HOOK_FIXED_UPDATE,
  SCRIPT_HOOK_LATE_FIXED_UPDATE,
} ScriptHook;

/* Runs one module hook and then its behavior hooks. False when one failed. */
static bool8_t script_instance_run(VkrScriptHost *host,
                                   VkrScriptInstance *instance, ScriptHook hook,
                                   float32_t dt) {
  const VkrModuleDesc *desc = script_instance_desc(host, instance);
  VkrCtx *ctx = &instance->ctx.base;
  const VkrScriptPhase phase =
      hook == SCRIPT_HOOK_FIXED_UPDATE || hook == SCRIPT_HOOK_LATE_FIXED_UPDATE
          ? VKR_SCRIPT_PHASE_TICK
          : VKR_SCRIPT_PHASE_FRAME;
  ScriptCall call;
  script_call_begin(host, instance, &instance->ledger, phase, &call);
  switch (hook) {
  case SCRIPT_HOOK_UPDATE:
    if (desc->update) {
      desc->update(ctx, instance->data, dt);
    }
    break;
  case SCRIPT_HOOK_LATE_UPDATE:
    if (desc->late_update) {
      desc->late_update(ctx, instance->data, dt);
    }
    break;
  case SCRIPT_HOOK_FIXED_UPDATE:
    if (desc->fixed_update) {
      desc->fixed_update(ctx, instance->data);
    }
    break;
  case SCRIPT_HOOK_LATE_FIXED_UPDATE:
    if (desc->late_fixed_update) {
      desc->late_fixed_update(ctx, instance->data);
    }
    break;
  default:
    break;
  }
  if (!script_call_end(host, instance, &call)) {
    return false_v;
  }
  for (uint32_t i = 0; i < instance->binding_count && !instance->disabled;
       ++i) {
    ScriptBinding *binding = &instance->bindings[i];
    const VkrBehaviorDesc *behavior = desc->behaviors[binding->behavior];
    if (!binding->started) {
      continue;
    }
    void (*frame_hook)(VkrCtx *, VkrEntity, void *, float32_t) =
        hook == SCRIPT_HOOK_UPDATE        ? behavior->update
        : hook == SCRIPT_HOOK_LATE_UPDATE ? behavior->late_update
                                          : NULL;
    void (*tick_hook)(VkrCtx *, VkrEntity, void *) =
        hook == SCRIPT_HOOK_FIXED_UPDATE        ? behavior->fixed_update
        : hook == SCRIPT_HOOK_LATE_FIXED_UPDATE ? behavior->late_fixed_update
                                                : NULL;
    if (!frame_hook && !tick_hook) {
      continue;
    }
    void *component = script_binding_component(host, instance, binding);
    if (!component) {
      continue;
    }
    const VkrEntity self = {.id = binding->entity};
    script_call_begin(host, instance, &binding->ledger, phase, &call);
    if (frame_hook) {
      frame_hook(ctx, self, component, dt);
    } else {
      tick_hook(ctx, self, component);
    }
    if (!script_call_end(host, instance, &call)) {
      return false_v;
    }
  }
  return true_v;
}

/* Copies a module failure beside its name into the host error. */
static const char *script_host_fail(VkrScriptHost *host, const char *module,
                                    const char *error) {
  if (error != host->error) {
    char message[VKR_SCRIPT_ERROR_CAPACITY];
    snprintf(message, sizeof(message), "%s", error ? error : "script failed");
    snprintf(host->error, sizeof(host->error), "%s: %s",
             module ? module : "script", message);
  }
  return host->error;
}

// =============================================================================
// Instances and behavior bindings
// =============================================================================

static VkrScriptInstance *script_instance_create(VkrScriptHost *host,
                                                 uint32_t module,
                                                 uint32_t container) {
  VkrScriptInstance *instance =
      vkr_allocator_alloc(&host->instance_allocator, sizeof(*instance),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!instance) {
    return NULL;
  }
  MemZero(instance, sizeof(*instance));
  instance->ctx = (ScriptCtx){
      .base = {.sdk = &host->table}, .host = host, .instance = instance};
  instance->ctx.ledger = &instance->ledger;
  instance->module = module;
  instance->container = container;
  const VkrModuleDesc *desc =
      module == SCRIPT_MODULE_NONE ? NULL : host->modules[module].desc;
  if (desc && desc->data_size) {
    instance->data_size = desc->data_size;
    instance->data_align = desc->data_align;
    instance->data = vkr_allocator_alloc_aligned(
        &host->instance_allocator, desc->data_size, desc->data_align,
        VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (!instance->data) {
      vkr_allocator_free(&host->instance_allocator, instance, sizeof(*instance),
                         VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
      return NULL;
    }
    MemZero(instance->data, desc->data_size);
  }
  return instance;
}

static void script_instance_free(VkrScriptHost *host,
                                 VkrScriptInstance *instance) {
  for (uint32_t i = 0; i < instance->binding_count; ++i) {
    script_ledger_free(host, &instance->bindings[i].ledger);
  }
  if (instance->bindings) {
    vkr_allocator_free(&host->instance_allocator, instance->bindings,
                       sizeof(*instance->bindings) * instance->binding_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  script_ledger_free(host, &instance->ledger);
  if (instance->data) {
    vkr_allocator_free_aligned(&host->instance_allocator, instance->data,
                               instance->data_size, instance->data_align,
                               VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }
  vkr_allocator_free(&host->instance_allocator, instance, sizeof(*instance),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

/* Runs a binding's stop hook and releases what its entity scope acquired. */
void script_binding_end(VkrScriptHost *host, VkrScriptInstance *instance,
                        ScriptBinding *binding) {
  const VkrModuleDesc *desc = script_instance_desc(host, instance);
  const VkrBehaviorDesc *behavior =
      desc ? desc->behaviors[binding->behavior] : NULL;
  if (binding->started && behavior && behavior->stop &&
      !host->modules[instance->module].retired) {
    ScriptCall call;
    script_call_begin(host, instance, &binding->ledger, VKR_SCRIPT_PHASE_FRAME,
                      &call);
    behavior->stop(&instance->ctx.base, (VkrEntity){.id = binding->entity},
                   script_binding_component(host, instance, binding));
    (void)script_call_end(host, instance, &call);
  }
  binding->started = false_v;
  binding->ledger.closed = true_v;
  script_ledger_release(host, &binding->ledger);
  script_update_transforms(host);
}

/* Ends an instance: behaviors in reverse, the module's stop hook, then
 * everything it acquired, newest first. */
static void script_instance_end(VkrScriptHost *host,
                                VkrScriptInstance *instance) {
  /* Edits it queued in a tick never apply. */
  script_commands_drop(host, instance);
  for (uint32_t i = instance->binding_count; i-- > 0u;) {
    script_binding_end(host, instance, &instance->bindings[i]);
  }
  const VkrModuleDesc *desc = script_instance_desc(host, instance);
  if (instance->started && desc && desc->stop &&
      !host->modules[instance->module].retired) {
    ScriptCall call;
    script_call_begin(host, instance, &instance->ledger, VKR_SCRIPT_PHASE_FRAME,
                      &call);
    desc->stop(&instance->ctx.base, instance->data);
    (void)script_call_end(host, instance, &call);
  }
  instance->started = false_v;
  instance->ledger.closed = true_v;
  script_ledger_release(host, &instance->ledger);
  script_update_transforms(host);
}

typedef struct ScriptFound {
  uint64_t entity;
  uint32_t behavior;
} ScriptFound;

static int script_found_compare(const void *left, const void *right) {
  const ScriptFound *a = left;
  const ScriptFound *b = right;
  if (a->behavior != b->behavior) {
    return a->behavior < b->behavior ? -1 : 1;
  }
  return a->entity < b->entity ? -1 : a->entity > b->entity ? 1 : 0;
}

static int script_binding_compare(const ScriptBinding *binding,
                                  const ScriptFound *found) {
  const ScriptFound key = {.entity = binding->entity,
                           .behavior = binding->behavior};
  return script_found_compare(&key, found);
}

// =============================================================================
// Simulation, destroy and input callbacks
// =============================================================================

/* Matches bindings with the entities carrying each behavior's component:
 * gone entities stop and release, new ones start. False when a hook
 * failed. */
static bool8_t script_instance_sync(VkrScriptHost *host,
                                    VkrScriptInstance *instance) {
  const VkrModuleDesc *desc = script_instance_desc(host, instance);
  if (!desc || !desc->behavior_count || instance->disabled) {
    return true_v;
  }
  const bool8_t world_scope = desc->scope == VKR_SCOPE_WORLD;
  /* Adding, removing or destroying a typed component bumps its container's
     world revision, so unchanged revisions mean unchanged bindings. */
  bool8_t changed = !instance->synced;
  for (uint32_t c = 0; c < host->container_count; ++c) {
    const uint64_t revision = host->containers[c].scene->world_revision;
    changed = changed || instance->synced_revisions[c] != revision;
    instance->synced_revisions[c] = revision;
  }
  instance->synced = true_v;
  if (!changed) {
    return true_v;
  }
  const uint64_t temp_position = arena_pos(host->temp);
  uint32_t found_count = 0u;
  ScriptFound *found = NULL;
  for (uint32_t pass = 0; pass < 2u; ++pass) {
    uint32_t written = 0u;
    for (uint32_t b = 0; b < desc->behavior_count; ++b) {
      const VkrTypeDesc *type =
          script_resolve_type(host, desc->behaviors[b]->component);
      for (uint32_t c = 0; c < host->container_count; ++c) {
        if (!world_scope && c != instance->container) {
          continue;
        }
        VkrScene *scene = host->containers[c].scene;
        if (pass == 0u) {
          found_count += vkr_scene_find_typed(scene, type, NULL, 0u);
          continue;
        }
        VkrEntityId *ids =
            arena_alloc(host->temp, sizeof(*ids) * (found_count + 1u),
                        ARENA_MEMORY_TAG_ARRAY);
        const uint32_t count =
            ids ? Min(vkr_scene_find_typed(scene, type, ids,
                                           found_count - written),
                      found_count - written)
                : 0u;
        for (uint32_t i = 0; i < count; ++i) {
          found[written++] = (ScriptFound){.entity = ids[i].u64, .behavior = b};
        }
      }
    }
    if (pass == 0u) {
      found = found_count
                  ? arena_alloc(host->temp, sizeof(*found) * found_count,
                                ARENA_MEMORY_TAG_ARRAY)
                  : NULL;
      if (found_count && !found) {
        arena_reset_to(host->temp, temp_position, ARENA_MEMORY_TAG_UNKNOWN);
        return true_v;
      }
    } else {
      found_count = written;
    }
  }
  if (found_count > 1u) {
    qsort(found, found_count, sizeof(*found), script_found_compare);
  }

  /* Stop bindings whose entity no longer carries the component. */
  uint32_t f = 0u;
  for (uint32_t i = 0; i < instance->binding_count; ++i) {
    ScriptBinding *binding = &instance->bindings[i];
    while (f < found_count && script_binding_compare(binding, &found[f]) > 0) {
      ++f;
    }
    const bool8_t kept =
        f < found_count && script_binding_compare(binding, &found[f]) == 0;
    if (!kept) {
      script_binding_end(host, instance, binding);
      script_ledger_free(host, &binding->ledger);
      binding->entity = 0u;
    }
  }

  /* Merge kept bindings with new entities, both in key order. */
  ScriptBinding *bindings = NULL;
  if (found_count) {
    bindings = vkr_allocator_alloc(&host->instance_allocator,
                                   sizeof(*bindings) * found_count,
                                   VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!bindings) {
      arena_reset_to(host->temp, temp_position, ARENA_MEMORY_TAG_UNKNOWN);
      instance->failed = true_v;
      script_host_fail(host, desc->name, "Behavior allocation failed");
      return false_v;
    }
  }
  uint32_t kept = 0u;
  uint32_t b = 0u;
  for (f = 0u; f < found_count; ++f) {
    while (b < instance->binding_count &&
           (!instance->bindings[b].entity ||
            script_binding_compare(&instance->bindings[b], &found[f]) < 0)) {
      ++b;
    }
    if (b < instance->binding_count &&
        script_binding_compare(&instance->bindings[b], &found[f]) == 0) {
      bindings[kept++] = instance->bindings[b++];
    } else {
      bindings[kept++] = (ScriptBinding){.entity = found[f].entity,
                                         .behavior = found[f].behavior};
    }
  }
  if (instance->bindings) {
    vkr_allocator_free(&host->instance_allocator, instance->bindings,
                       sizeof(*instance->bindings) * instance->binding_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  instance->bindings = bindings;
  instance->binding_count = kept;
  instance->binding_capacity = found_count;
  arena_reset_to(host->temp, temp_position, ARENA_MEMORY_TAG_UNKNOWN);

  /* Start the new ones in key order. */
  for (uint32_t i = 0; i < instance->binding_count && !instance->disabled;
       ++i) {
    ScriptBinding *binding = &instance->bindings[i];
    if (binding->started) {
      continue;
    }
    binding->started = true_v;
    const VkrBehaviorDesc *behavior = desc->behaviors[binding->behavior];
    if (!behavior->start) {
      continue;
    }
    ScriptCall call;
    script_call_begin(host, instance, &binding->ledger, VKR_SCRIPT_PHASE_FRAME,
                      &call);
    behavior->start(&instance->ctx.base, (VkrEntity){.id = binding->entity},
                    script_binding_component(host, instance, binding));
    if (!script_call_end(host, instance, &call)) {
      return false_v;
    }
  }
  return true_v;
}

/* The binding of `behavior` on `entity`, or NULL; bindings are sorted by
 * behavior, then entity. */
static ScriptBinding *script_binding_find(VkrScriptInstance *instance,
                                          uint32_t behavior, uint64_t entity) {
  const ScriptFound key = {.entity = entity, .behavior = behavior};
  uint32_t low = 0u;
  uint32_t high = instance->binding_count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2u;
    const int order = script_binding_compare(&instance->bindings[middle], &key);
    if (order == 0) {
      return &instance->bindings[middle];
    }
    if (order < 0) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return NULL;
}

static void script_lifetimes_owner_destroyed(VkrScriptHost *host,
                                             uint64_t entity);

/* An entity of an attached container is about to be destroyed: each
 * running behavior on it runs `destroy` and then `stop` while it still
 * exists, and its scope is released. */
static void script_host_entity_destroying(VkrScene *scene, VkrEntityId entity,
                                          void *context) {
  VkrScriptHost *host = context;
  for (uint32_t i = 0; i < host->instance_count; ++i) {
    VkrScriptInstance *instance = host->instances[i];
    const VkrModuleDesc *desc = script_instance_desc(host, instance);
    if (!script_instance_callable(host, instance) || !desc->behavior_count) {
      continue;
    }
    for (uint32_t b = 0; b < desc->behavior_count; ++b) {
      const VkrTypeDesc *type =
          script_resolve_type(host, desc->behaviors[b]->component);
      const VkrComponentTypeId id = vkr_scene_type_id(scene, type);
      if (id == VKR_COMPONENT_TYPE_INVALID ||
          !vkr_entity_has_component(scene->world, entity, id)) {
        continue;
      }
      ScriptBinding *binding = script_binding_find(instance, b, entity.u64);
      if (!binding || !binding->started) {
        continue;
      }
      if (desc->behaviors[b]->destroy) {
        ScriptCall call;
        script_call_begin(host, instance, &binding->ledger,
                          host->phase == VKR_SCRIPT_PHASE_TICK
                              ? VKR_SCRIPT_PHASE_TICK
                              : VKR_SCRIPT_PHASE_FRAME,
                          &call);
        desc->behaviors[b]->destroy(
            &instance->ctx.base, (VkrEntity){.id = entity.u64},
            script_binding_component(host, instance, binding));
        (void)script_call_end(host, instance, &call);
      }
      /* The hook may have ended it, through a nested destroy. */
      binding = script_binding_find(instance, b, entity.u64);
      if (binding && binding->started) {
        script_binding_end(host, instance, binding);
      }
    }
    if (instance->failed) {
      host->faulted = true_v;
      vkr_scene_physics_set_paused(host->session.active, true_v);
    }
  }
  script_lifetimes_owner_destroyed(host, entity.u64);
}

static bool8_t script_host_tick(VkrScriptHost *host, VkrScene *scene,
                                ScriptHook hook) {
  for (uint32_t i = 0; i < host->instance_count; ++i) {
    VkrScriptInstance *instance = host->instances[i];
    if (!script_instance_callable(host, instance)) {
      continue;
    }
    if (!script_instance_run(host, instance, hook, 0.0f)) {
      scene->simulation.error = host->error;
      return false_v;
    }
  }
  return true_v;
}

static bool8_t script_host_before_physics(VkrScene *scene, uint64_t tick,
                                          void *context) {
  (void)tick;
  return script_host_tick(context, scene, SCRIPT_HOOK_FIXED_UPDATE);
}

static bool8_t script_host_after_physics(VkrScene *scene, uint64_t tick,
                                         void *context) {
  (void)tick;
  return script_host_tick(context, scene, SCRIPT_HOOK_LATE_FIXED_UPDATE);
}

// =============================================================================
// Lifetimes
// =============================================================================

/* The simulated seconds deadlines compare with. */
static float64_t script_sim_time(const VkrScriptHost *host) {
  return host->session.active ? vkr_scene_physics_time(host->session.active)
                              : 0.0;
}

bool8_t script_lifetime_add(VkrScriptHost *host, uint64_t entity,
                            uint64_t owner, float32_t seconds) {
  if (seconds > 0.0f) {
    if (host->timed_count == host->timed_capacity) {
      const uint32_t capacity = Max(64u, host->timed_capacity * 2u);
      ScriptTimed *timed = vkr_allocator_alloc(&host->instance_allocator,
                                               sizeof(*timed) * capacity,
                                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!timed) {
        return false_v;
      }
      if (host->timed) {
        MemCopy(timed, host->timed, sizeof(*timed) * host->timed_count);
        vkr_allocator_free(&host->instance_allocator, host->timed,
                           sizeof(*timed) * host->timed_capacity,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      host->timed = timed;
      host->timed_capacity = capacity;
    }
    host->timed[host->timed_count++] = (ScriptTimed){
        .entity = entity, .deadline = script_sim_time(host) + seconds};
  }
  if (owner) {
    if (host->owned_count == host->owned_capacity) {
      const uint32_t capacity = Max(64u, host->owned_capacity * 2u);
      ScriptOwned *owned = vkr_allocator_alloc(&host->instance_allocator,
                                               sizeof(*owned) * capacity,
                                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!owned) {
        return false_v;
      }
      if (host->owned) {
        MemCopy(owned, host->owned, sizeof(*owned) * host->owned_count);
        vkr_allocator_free(&host->instance_allocator, host->owned,
                           sizeof(*owned) * host->owned_capacity,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      host->owned = owned;
      host->owned_capacity = capacity;
    }
    host->owned[host->owned_count++] =
        (ScriptOwned){.entity = entity, .owner = owner};
  }
  return true_v;
}

static bool8_t script_entity_alive(VkrScriptHost *host, uint64_t entity) {
  VkrScriptContainer *container = script_container_of(host, entity);
  return container &&
         vkr_scene_entity_alive(container->scene, (VkrEntityId){.u64 = entity});
}

/* Destroys an entity of an attached container, running its destroy hooks. */
static void script_entity_destroy(VkrScriptHost *host, uint64_t entity) {
  VkrScriptContainer *container = script_container_of(host, entity);
  if (container) {
    script_destroy_tree(host, container->scene, (VkrEntityId){.u64 = entity});
    host->transforms_dirty[container - host->containers] = true_v;
  }
}

void script_lifetimes_update(VkrScriptHost *host) {
  const float64_t now = script_sim_time(host);
  /* Destroying can run destroy hooks that spawn timed entities, so the
     array is read by index and its count again each step. */
  for (uint32_t i = 0; i < host->timed_count;) {
    const ScriptTimed timed = host->timed[i];
    const bool8_t alive = script_entity_alive(host, timed.entity);
    if (alive && now < timed.deadline) {
      ++i;
      continue;
    }
    host->timed[i] = host->timed[--host->timed_count];
    if (alive) {
      script_entity_destroy(host, timed.entity);
    }
  }
  uint32_t kept = 0u;
  for (uint32_t i = 0; i < host->owned_count; ++i) {
    const ScriptOwned owned = host->owned[i];
    if (owned.owner && script_entity_alive(host, owned.entity)) {
      host->owned[kept++] = owned;
    }
  }
  host->owned_count = kept;
}

/* Spawns owned by a destroyed entity go with it; entries of either end. */
static void script_lifetimes_owner_destroyed(VkrScriptHost *host,
                                             uint64_t entity) {
  for (uint32_t i = 0; i < host->owned_count; ++i) {
    if (host->owned[i].entity == entity) {
      host->owned[i].owner = 0u;
    }
    if (host->owned[i].owner != entity) {
      continue;
    }
    const uint64_t owned = host->owned[i].entity;
    host->owned[i].owner = 0u;
    if (script_entity_alive(host, owned)) {
      script_entity_destroy(host, owned);
    }
  }
}

/* After each completed tick, outside its read scope: queued edits apply in
 * order, then expired spawns go. */
static void script_host_after_tick(VkrScene *scene, uint64_t tick,
                                   void *context) {
  (void)scene;
  (void)tick;
  VkrScriptHost *host = context;
  script_commands_flush(host);
  script_lifetimes_update(host);
}

/* Keeps entity IDs free so spawns in a tick never grow a directory. */
static void script_reserve_spawns(VkrScriptHost *host) {
  for (uint32_t c = 0; c < host->container_count; ++c) {
    (void)vkr_entity_reserve_capacity(host->containers[c].scene->world,
                                      SCRIPT_TICK_SPAWN_RESERVE);
  }
}

/* A native reset restores bodies; the instances restart at the next frame
 * boundary, where structural edits are allowed. */
static void script_host_reset(VkrScene *scene, void *context) {
  (void)scene;
  VkrScriptHost *host = context;
  host->restart_pending = true_v;
}

_Static_assert(
    sizeof(VkrInputEvent) == sizeof(VkrInputTransition) &&
        offsetof(VkrInputEvent, time) ==
            offsetof(VkrInputTransition, time_seconds) &&
        offsetof(VkrInputEvent, kind) == offsetof(VkrInputTransition, kind) &&
        offsetof(VkrInputEvent, code) == offsetof(VkrInputTransition, code) &&
        offsetof(VkrInputEvent, pressed) ==
            offsetof(VkrInputTransition, pressed) &&
        offsetof(VkrInputEvent, dx) == offsetof(VkrInputTransition, delta_x) &&
        offsetof(VkrInputEvent, dy) == offsetof(VkrInputTransition, delta_y),
    "VkrInputEvent mirrors VkrInputTransition");
_Static_assert((int)VKR_INPUT_LOOK == (int)VKR_INPUT_TRANSITION_LOOK &&
                   (int)VKR_KEY_W == (int)KEY_W &&
                   (int)VKR_KEY_LCONTROL == (int)KEY_LCONTROL &&
                   (int)VKR_KEY_F1 == (int)KEY_F1 &&
                   (int)VKR_MOUSE_LEFT == (int)BUTTON_LEFT,
               "SDK input codes match the engine's");

static void script_host_input(const VkrInputTransition *transition,
                              void *context) {
  VkrScriptHost *host = context;
  const VkrInputEvent *event = (const VkrInputEvent *)transition;
  for (uint32_t i = 0; !host->faulted && i < host->instance_count; ++i) {
    VkrScriptInstance *instance = host->instances[i];
    const VkrModuleDesc *desc = script_instance_desc(host, instance);
    if (!script_instance_callable(host, instance) || !desc->input) {
      continue;
    }
    ScriptCall call;
    script_call_begin(host, instance, &instance->ledger, VKR_SCRIPT_PHASE_FRAME,
                      &call);
    desc->input(&instance->ctx.base, instance->data, event);
    if (!script_call_end(host, instance, &call)) {
      host->faulted = true_v;
      vkr_scene_physics_set_paused(host->session.active, true_v);
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

/* A superseded library stays mapped while a session may still hold
 * pointers into its code or constants. */
static bool8_t script_retire_handle(VkrScriptHost *host,
                                    VkrScriptLibrary *library) {
  if (!library->handle.handle) {
    return true_v;
  }
  if (!host->started) {
    script_close_library(host, &library->handle, library->loaded_path);
    return true_v;
  }
  if (host->retired_count == VKR_SCRIPT_RETIRED_LIBRARY_MAX) {
    return false_v;
  }
  VkrScriptRetiredLibrary *retired = &host->retired[host->retired_count++];
  retired->library = library->handle;
  snprintf(retired->path, sizeof(retired->path), "%s", library->loaded_path);
  library->handle = (VkrPlatformLibrary){0};
  library->loaded_path[0] = '\0';
  return true_v;
}

static void script_close_retired(VkrScriptHost *host) {
  while (host->retired_count) {
    VkrScriptRetiredLibrary *retired = &host->retired[--host->retired_count];
    script_close_library(host, &retired->library, retired->path);
  }
}

/* The library slot named `name`, kept after retirement so a later load of
 * the same name adopts its modules; UINT32_MAX when none is free. */
static uint32_t script_library_slot(VkrScriptHost *host, const char *name) {
  for (uint32_t i = 0; i < host->library_count; ++i) {
    if (!strcmp(host->libraries[i].name, name)) {
      return i;
    }
  }
  if (host->library_count == VKR_SCRIPT_LIBRARY_MAX) {
    return UINT32_MAX;
  }
  VkrScriptLibrary *library = &host->libraries[host->library_count];
  *library = (VkrScriptLibrary){0};
  snprintf(library->name, sizeof(library->name), "%s", name);
  return host->library_count++;
}

// =============================================================================
// Registration
// =============================================================================

static const char *script_validate(const VkrModuleDesc *desc) {
  if (!desc || desc->sdk_version != VKR_SDK_VERSION || !desc->name ||
      (desc->scope != VKR_SCOPE_CONTAINER && desc->scope != VKR_SCOPE_WORLD)) {
    return "Script module has an incompatible description";
  }
  if (desc->component_count > VKR_SDK_EXPORT_MAX ||
      desc->behavior_count > VKR_SDK_EXPORT_MAX) {
    return "Script module exports more than 64 components or behaviors";
  }
  for (uint32_t i = 0; i < desc->component_count + desc->behavior_count; ++i) {
    const VkrComponentDesc *type =
        i < desc->component_count
            ? desc->components[i]
            : (desc->behaviors[i - desc->component_count]
                   ? desc->behaviors[i - desc->component_count]->component
                   : NULL);
    if (!type || !type->name || !type->size || !type->align) {
      return "Script component type is missing its descriptor or name";
    }
    for (uint32_t f = 0; f < type->field_count; ++f) {
      if (!type->fields[f].name ||
          (uint32_t)type->fields[f].kind >= ArrayCount(s_field_kinds) ||
          type->fields[f].offset >= type->size) {
        return "Script component field is invalid";
      }
    }
  }
  VkrComponentDesc *types[VKR_SDK_EXPORT_MAX];
  if (script_module_components(desc, types) > VKR_SDK_EXPORT_MAX) {
    return "Script module declares more than 64 component types";
  }
  if (desc->data_size &&
      (!desc->data_align || (desc->data_align & (desc->data_align - 1u)))) {
    return "Script module data needs a power-of-two alignment";
  }
  return NULL;
}

static bool8_t script_modules_reserve(VkrScriptHost *host, uint32_t count) {
  if (count <= host->module_capacity) {
    return true_v;
  }
  const uint32_t capacity = Max(count, Max(16u, host->module_capacity * 2u));
  VkrScriptModule *modules =
      vkr_allocator_alloc(host->allocator, sizeof(*modules) * capacity,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!modules) {
    return false_v;
  }
  if (host->modules) {
    MemCopy(modules, host->modules, sizeof(*modules) * host->module_count);
    vkr_allocator_free(host->allocator, host->modules,
                       sizeof(*modules) * host->module_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  host->modules = modules;
  host->module_capacity = capacity;
  return true_v;
}

/* Index of a registered type among the module's copies, or UINT32_MAX. */
static uint32_t script_type_index(const VkrScriptModule *module,
                                  const VkrTypeDesc *type) {
  for (uint32_t i = 0; i < module->type_count; ++i) {
    if (module->types[i] == type) {
      return i;
    }
  }
  return UINT32_MAX;
}

/* A type name some module other than `self` registered, or NULL. */
static const char *script_type_taken(const VkrScriptHost *host,
                                     const VkrModuleDesc *desc, uint32_t self) {
  VkrComponentDesc *types[VKR_SDK_EXPORT_MAX];
  const uint32_t type_count = script_module_components(desc, types);
  for (uint32_t i = 0; i < type_count; ++i) {
    const VkrTypeDesc *registered =
        vkr_scene_world_type_named(string8_create_from_cstr(
            (const uint8_t *)types[i]->name, strlen(types[i]->name)));
    if (!registered) {
      continue;
    }
    const bool8_t own =
        self != UINT32_MAX &&
        script_type_index(&host->modules[self], registered) != UINT32_MAX;
    if (!own) {
      return types[i]->name;
    }
  }
  return NULL;
}

/* Registers a new module's copies of its component types. A name another
 * module registered fails; nothing is registered then. */
static const char *script_register(VkrScriptHost *host, const char *name,
                                   const VkrModuleDesc *desc, uint32_t library,
                                   uint32_t *out_index) {
  if (script_type_taken(host, desc, UINT32_MAX)) {
    return "Script component type is invalid or its name is taken";
  }
  if (!script_modules_reserve(host, host->module_count + 1u)) {
    return "Script module allocation failed";
  }
  VkrComponentDesc *types[VKR_SDK_EXPORT_MAX];
  const uint32_t type_count = script_module_components(desc, types);
  VkrScriptModule *module = &host->modules[host->module_count];
  *module = (VkrScriptModule){.desc = desc,
                              .library = library,
                              .dynamic = library != VKR_SCRIPT_LIBRARY_NONE};
  snprintf(module->name, sizeof(module->name), "%s", name);
  for (uint32_t i = 0; i < type_count; ++i) {
    VkrTypeDesc *copy = vkr_allocator_alloc(host->allocator, sizeof(*copy),
                                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (!copy || !script_type_fill(host, copy, types[i]) ||
        !vkr_scene_register_world_type(copy)) {
      /* Earlier copies stay registered: the scene type table has no
         removal. Their module never becomes callable. */
      return "Script component type registration failed";
    }
    module->types[module->type_count++] = copy;
    types[i]->host_binding = copy;
  }
  module->generation = 1u;
  *out_index = host->module_count++;
  return NULL;
}

bool8_t vkr_script_host_init(VkrScriptHost *host, VkrAllocator *allocator) {
  MemZero(host, sizeof(*host));
  host->allocator = allocator;
  script_sdk_table(&host->table);
  if (!vkr_dmemory_create(SCRIPT_INSTANCE_MEMORY_SIZE,
                          SCRIPT_INSTANCE_MEMORY_RESERVE, &host->memory)) {
    return false_v;
  }
  host->instance_allocator = (VkrAllocator){.ctx = &host->memory};
  vkr_dmemory_allocator_create(&host->instance_allocator);
  host->temp = arena_create(SCRIPT_TEMP_RESERVE, SCRIPT_TEMP_COMMIT);
  host->command_arena =
      arena_create(SCRIPT_COMMAND_RESERVE, SCRIPT_COMMAND_COMMIT);
  if (!host->temp || !host->command_arena) {
    if (host->temp) {
      arena_destroy(host->temp);
    }
    if (host->command_arena) {
      arena_destroy(host->command_arena);
    }
    vkr_dmemory_allocator_destroy(&host->instance_allocator);
    return false_v;
  }
  return true_v;
}

bool8_t vkr_script_host_add_module(VkrScriptHost *host, VkrModuleEntry entry,
                                   const char **error) {
  const VkrModuleDesc *desc = entry ? entry(VKR_SDK_VERSION) : NULL;
  const char *failure = host->started ? "Script modules register before a "
                                        "session"
                                      : script_validate(desc);
  uint32_t index = 0u;
  if (!failure) {
    failure = script_register(host, desc->name, desc, VKR_SCRIPT_LIBRARY_NONE,
                              &index);
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

/* Replaces a known module's code. The types keep their copies; display
 * metadata and defaults follow the new code. */
static void script_swap(VkrScriptHost *host, VkrScriptModule *module,
                        const VkrModuleDesc *desc, uint32_t library) {
  VkrComponentDesc *types[VKR_SDK_EXPORT_MAX];
  (void)script_module_components(desc, types);
  for (uint32_t i = 0; i < module->type_count; ++i) {
    VkrTypeDesc *copy = module->types[i];
    const VkrTypeDesc kept = *copy;
    if (script_type_fill(host, copy, types[i])) {
      /* The registered name stays the pointer scenes compare. */
      copy->name = kept.name;
    } else {
      *copy = kept;
    }
    types[i]->host_binding = copy;
  }
  module->desc = desc;
  module->library = library;
  module->dynamic = true_v;
  module->retired = false_v;
  module->generation++;
}

/* A module that keeps its types but runs no code until a load adopts it. */
static void script_retire_module(VkrScriptModule *module) {
  for (uint32_t t = 0; t < module->type_count; ++t) {
    module->types[t]->defaults = NULL;
  }
  module->desc = NULL;
  module->retired = true_v;
}

/* Running instances keep their data and bindings only when the new code
 * describes the same data and behaviors. */
static bool8_t script_same_shape(const VkrModuleDesc *old,
                                 const VkrModuleDesc *desc) {
  if (!old || old->data_size != desc->data_size ||
      old->data_align != desc->data_align ||
      old->data_version != desc->data_version || old->scope != desc->scope ||
      old->behavior_count != desc->behavior_count) {
    return false_v;
  }
  for (uint32_t i = 0; i < desc->behavior_count; ++i) {
    if (strcmp(old->behaviors[i]->component->name,
               desc->behaviors[i]->component->name)) {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t script_module_running(const VkrScriptHost *host,
                                     uint32_t module) {
  for (uint32_t i = 0; host->started && i < host->instance_count; ++i) {
    if (host->instances[i]->module == module) {
      return true_v;
    }
  }
  return false_v;
}

/* One module a library lists: its description and the registered module it
 * reloads, or UINT32_MAX for a new one. */
typedef struct ScriptListed {
  const VkrModuleDesc *desc;
  const char *name;
  uint32_t module;
} ScriptListed;

/* Checks every module a library lists before anything changes: a refused
 * one keeps all of them on their previous code. NULL when the load may
 * apply; `restart` reports whether a running session must restart. */
static const char *script_load_check(VkrScriptHost *host, uint32_t slot,
                                     ScriptListed *listed, uint32_t count,
                                     bool8_t *restart) {
  *restart = false_v;
  for (uint32_t i = 0; i < count; ++i) {
    ScriptListed *entry = &listed[i];
    const char *invalid = script_validate(entry->desc);
    if (invalid) {
      return invalid;
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (!strcmp(listed[j].name, entry->name)) {
        return "The library lists one module name twice";
      }
    }
    const VkrScriptModule *known = vkr_script_host_module(host, entry->name);
    entry->module = known ? (uint32_t)(known - host->modules) : UINT32_MAX;
    if (known && !known->dynamic) {
      return "A module of that name is linked into the executable";
    }
    if (known && !known->retired && known->library != slot) {
      return "A module of that name belongs to another library";
    }
    const char *taken = script_type_taken(host, entry->desc, entry->module);
    if (taken) {
      snprintf(host->error, sizeof(host->error),
               "Component %s belongs to another module", taken);
      return host->error;
    }
    if (!known) {
      *restart = *restart || host->started;
      continue;
    }
    VkrComponentDesc *types[VKR_SDK_EXPORT_MAX];
    if (script_module_components(entry->desc, types) != known->type_count) {
      snprintf(host->error, sizeof(host->error),
               "%s changed its component types; reopen the project",
               known->name);
      return host->error;
    }
    for (uint32_t t = 0; t < known->type_count; ++t) {
      if (!script_type_same_layout(known->types[t], types[t])) {
        snprintf(host->error, sizeof(host->error),
                 "Component %s changed its fields; reopen the project to "
                 "apply it",
                 known->types[t]->name);
        return host->error;
      }
    }
    if (script_module_running(host, entry->module) &&
        !script_same_shape(known->desc, entry->desc)) {
      *restart = true_v;
    }
  }
  /* A module the library no longer lists retires; a running one restarts
     the session. */
  for (uint32_t m = 0; m < host->module_count; ++m) {
    const VkrScriptModule *module = &host->modules[m];
    if (module->library != slot || module->retired) {
      continue;
    }
    bool8_t listed_again = false_v;
    for (uint32_t i = 0; i < count && !listed_again; ++i) {
      listed_again = listed[i].module == m;
    }
    if (!listed_again && script_module_running(host, m)) {
      *restart = true_v;
    }
  }
  return NULL;
}

static bool8_t script_prepare_fail(VkrScriptPrepared *prepared,
                                   const char *message) {
  snprintf(prepared->error, sizeof(prepared->error), "%s", message);
  return false_v;
}

bool8_t vkr_script_host_prepare(VkrScriptHost *host,
                                VkrScriptPrepared *prepared, const char *name,
                                const char *library_path, bool8_t project) {
  *prepared = (VkrScriptPrepared){.project = project};
  if (!name || !name[0] || !library_path || !library_path[0]) {
    return script_prepare_fail(prepared,
                               "A script library needs a name and a path");
  }
  if (strlen(name) >= sizeof(prepared->name) ||
      strlen(library_path) + 32u >= sizeof(prepared->path)) {
    return script_prepare_fail(prepared,
                               "The script library's name or path is too "
                               "long");
  }
  snprintf(prepared->name, sizeof(prepared->name), "%s", name);
  snprintf(prepared->path, sizeof(prepared->path), "%s", library_path);
  /* A unique copy per load: the build can be replaced, and a reload never
     reuses a cached image. */
  snprintf(prepared->loaded_path, sizeof(prepared->loaded_path),
           "%s.%u-%u.loaded", library_path, vkr_platform_get_process_id(),
           ++host->load_serial);
  return true_v;
}

void vkr_script_prepare_run(VkrScriptPrepared *prepared) {
  prepared->ready = false_v;
  prepared->count = 0u;
  if (!prepared->loaded_path[0]) {
    return;
  }
  if (!script_copy_file(prepared->path, prepared->loaded_path)) {
    (void)script_prepare_fail(
        prepared, "The script library could not be copied for loading");
    return;
  }
  if (!vkr_platform_library_open(prepared->loaded_path, &prepared->handle,
                                 prepared->error, sizeof(prepared->error))) {
    return;
  }
  const VkrPlatformLibrary *library = &prepared->handle;
  if (prepared->project) {
    const VkrProjectEntry list =
        (VkrProjectEntry)(uintptr_t)vkr_platform_library_symbol(
            library, "vkr_project_modules");
    if (!list) {
      (void)script_prepare_fail(prepared,
                                "The library has no vkr_project_modules entry");
      return;
    }
    VkrModuleEntry entries[VKR_SCRIPT_PREPARED_MODULE_MAX];
    const uint32_t count = list(entries, ArrayCount(entries));
    if (count > ArrayCount(entries)) {
      (void)script_prepare_fail(prepared,
                                "The library lists more than 256 modules");
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      prepared->modules[i] = entries[i](VKR_SDK_VERSION);
    }
    prepared->count = count;
  } else {
    char entry_name[VKR_SCRIPT_MODULE_NAME_CAPACITY + 32u];
    snprintf(entry_name, sizeof(entry_name), "vkr_module_%s", prepared->name);
    const VkrModuleEntry single =
        (VkrModuleEntry)(uintptr_t)vkr_platform_library_symbol(library,
                                                               entry_name);
    if (!single) {
      (void)script_prepare_fail(prepared,
                                "The library has no vkr_module_<name> entry");
      return;
    }
    prepared->modules[0] = single(VKR_SDK_VERSION);
    prepared->count = 1u;
  }
  for (uint32_t i = 0; i < prepared->count; ++i) {
    if (!prepared->modules[i]) {
      (void)script_prepare_fail(
          prepared, "A module of the library was built for another SDK "
                    "version");
      return;
    }
  }
  prepared->ready = true_v;
}

void vkr_script_host_discard(VkrScriptHost *host, VkrScriptPrepared *prepared) {
  if (prepared->handle.handle || prepared->loaded_path[0]) {
    script_close_library(host, &prepared->handle, prepared->loaded_path);
  }
  prepared->ready = false_v;
}

VkrScriptReload vkr_script_host_commit(VkrScriptHost *host,
                                       VkrScriptPrepared *prepared,
                                       const char **error) {
  const uint64_t temp_position = arena_pos(host->temp);
  const char *failure = NULL;
  uint32_t slot = UINT32_MAX;
  const uint32_t count = prepared->count;
  ScriptListed *listed = NULL;
  if (!prepared->ready) {
    failure = prepared->error[0] ? prepared->error
                                 : "The script library was not prepared";
  } else if (host->tool) {
    failure = "A tool context is open";
  } else if ((slot = script_library_slot(host, prepared->name)) == UINT32_MAX) {
    failure = "Too many script libraries are loaded";
  } else if (count &&
             !(listed = arena_alloc(host->temp, sizeof(*listed) * count,
                                    ARENA_MEMORY_TAG_ARRAY))) {
    failure = "The library's module list could not be read";
  }
  for (uint32_t i = 0; !failure && i < count; ++i) {
    const VkrModuleDesc *desc = prepared->modules[i];
    listed[i] = (ScriptListed){.desc = desc,
                               .name = prepared->project
                                           ? (desc->name ? desc->name : "")
                                           : prepared->name,
                               .module = UINT32_MAX};
  }
  bool8_t restart = false_v;
  if (!failure) {
    failure = script_load_check(host, slot, listed, count, &restart);
  }
  VkrScriptLibrary *library =
      slot != UINT32_MAX ? &host->libraries[slot] : NULL;
  if (!failure && !restart && host->started &&
      host->retired_count == VKR_SCRIPT_RETIRED_LIBRARY_MAX &&
      library->handle.handle) {
    failure = "Too many reloads in one session; stop the simulation first";
  }

  VkrScriptReload result = VKR_SCRIPT_RELOAD_FAILED;
  if (!failure) {
    const VkrScriptSessionDesc session = host->session;
    const bool8_t restarting = restart && host->started;
    if (restarting) {
      /* Stop with the old code, restart with the new on the same
         containers. */
      vkr_script_host_stop(host);
    }
    (void)script_retire_handle(host, library);
    const bool8_t fresh = library->generation == 0u;
    library->handle = prepared->handle;
    library->project = prepared->project;
    library->generation++;
    snprintf(library->loaded_path, sizeof(library->loaded_path), "%s",
             prepared->loaded_path);
    prepared->handle = (VkrPlatformLibrary){0};
    prepared->loaded_path[0] = '\0';
    bool8_t all_new = true_v;
    for (uint32_t i = 0; i < count && !failure; ++i) {
      if (listed[i].module != UINT32_MAX) {
        all_new = false_v;
        script_swap(host, &host->modules[listed[i].module], listed[i].desc,
                    slot);
      } else {
        uint32_t index = 0u;
        failure =
            script_register(host, listed[i].name, listed[i].desc, slot, &index);
        listed[i].module = index;
      }
    }
    for (uint32_t m = 0; m < host->module_count; ++m) {
      VkrScriptModule *module = &host->modules[m];
      bool8_t listed_again = false_v;
      for (uint32_t i = 0; i < count && !listed_again; ++i) {
        listed_again = listed[i].module == m;
      }
      if (module->library == slot && !module->retired && !listed_again) {
        script_retire_module(module);
      }
    }
    const char *start_error = NULL;
    if (!failure && restarting &&
        !vkr_script_host_start(host, &session, &start_error)) {
      failure = start_error;
    }
    result = failure            ? VKR_SCRIPT_RELOAD_FAILED
             : restarting       ? VKR_SCRIPT_RELOAD_RESTARTED
             : fresh && all_new ? VKR_SCRIPT_RELOAD_LOADED
                                : VKR_SCRIPT_RELOAD_KEPT_STATE;
  }
  arena_reset_to(host->temp, temp_position, ARENA_MEMORY_TAG_UNKNOWN);
  if (failure) {
    if (failure != host->error) {
      snprintf(host->error, sizeof(host->error), "%s", failure);
    }
    vkr_script_host_discard(host, prepared);
    if (error) {
      *error = host->error;
    }
  }
  prepared->ready = false_v;
  return result;
}

static VkrScriptReload script_load(VkrScriptHost *host, const char *name,
                                   const char *library_path, bool8_t project,
                                   const char **error) {
  VkrScriptPrepared prepared;
  if (!vkr_script_host_prepare(host, &prepared, name, library_path, project)) {
    snprintf(host->error, sizeof(host->error), "%s", prepared.error);
    if (error) {
      *error = host->error;
    }
    return VKR_SCRIPT_RELOAD_FAILED;
  }
  vkr_script_prepare_run(&prepared);
  return vkr_script_host_commit(host, &prepared, error);
}

VkrScriptReload vkr_script_host_load_library(VkrScriptHost *host,
                                             const char *name,
                                             const char *library_path,
                                             const char **error) {
  return script_load(host, name, library_path, false_v, error);
}

VkrScriptReload vkr_script_host_load_project(VkrScriptHost *host,
                                             const char *name,
                                             const char *library_path,
                                             const char **error) {
  return script_load(host, name, library_path, true_v, error);
}

void vkr_script_host_retire_libraries(VkrScriptHost *host) {
  vkr_script_host_stop(host);
  for (uint32_t i = 0; i < host->module_count; ++i) {
    VkrScriptModule *module = &host->modules[i];
    if (module->dynamic && !module->retired) {
      script_retire_module(module);
    }
  }
  for (uint32_t i = 0; i < host->library_count; ++i) {
    VkrScriptLibrary *library = &host->libraries[i];
    if (library->handle.handle) {
      script_close_library(host, &library->handle, library->loaded_path);
    }
  }
}

void vkr_script_host_shutdown(VkrScriptHost *host) {
  if (!host->temp) {
    return;
  }
  vkr_script_host_close_context(host);
  vkr_script_host_retire_libraries(host);
  arena_destroy(host->temp);
  host->temp = NULL;
  arena_destroy(host->command_arena);
  host->command_arena = NULL;
  vkr_dmemory_allocator_destroy(&host->instance_allocator);
}

// =============================================================================
// Sessions
// =============================================================================

static uint32_t script_container_attach(VkrScriptHost *host, VkrScene *scene) {
  for (uint32_t i = 0; i < host->container_count; ++i) {
    if (host->containers[i].scene == scene) {
      return i;
    }
  }
  const uint32_t slot = host->container_count++;
  host->containers[slot] = (VkrScriptContainer){
      .scene = scene, .world_id = scene->world ? scene->world->world_id : 0u};
  host->transforms_dirty[slot] = false_v;
  for (uint32_t t = 0; t < host->state_type_count; ++t) {
    host->state_types[t].ids[slot] = VKR_COMPONENT_TYPE_INVALID;
  }
  return slot;
}

/* Ends every instance in reverse order and forgets the containers. */
static void script_session_end(VkrScriptHost *host) {
  for (uint32_t i = host->instance_count; i-- > 0u;) {
    script_instance_end(host, host->instances[i]);
  }
  for (uint32_t i = host->instance_count; i-- > 0u;) {
    script_instance_free(host, host->instances[i]);
    host->instances[i] = NULL;
  }
  host->instance_count = 0u;
}

static bool8_t script_instance_needs_ticks(const VkrScriptHost *host,
                                           const VkrScriptInstance *instance) {
  const VkrModuleDesc *desc = script_instance_desc(host, instance);
  if (!script_instance_callable(host, instance)) {
    return false_v;
  }
  if (desc->fixed_update || desc->late_fixed_update) {
    return true_v;
  }
  for (uint32_t b = 0; b < desc->behavior_count; ++b) {
    if (desc->behaviors[b]->fixed_update ||
        desc->behaviors[b]->late_fixed_update) {
      return true_v;
    }
  }
  return false_v;
}

/* Appends a started instance, growing the list in instance memory. */
static bool8_t script_instances_push(VkrScriptHost *host,
                                     VkrScriptInstance *instance) {
  if (host->instance_count == host->instance_capacity) {
    const uint32_t capacity = Max(32u, host->instance_capacity * 2u);
    VkrScriptInstance **instances = vkr_allocator_alloc(
        &host->instance_allocator, sizeof(*instances) * capacity,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!instances) {
      return false_v;
    }
    if (host->instances) {
      MemCopy(instances, host->instances,
              sizeof(*instances) * host->instance_count);
      vkr_allocator_free(&host->instance_allocator, host->instances,
                         sizeof(*instances) * host->instance_capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    host->instances = instances;
    host->instance_capacity = capacity;
  }
  host->instances[host->instance_count++] = instance;
  return true_v;
}

/* Starts module hooks, then behaviors, then installs the callbacks the
 * enabled instances need. */
static const char *script_session_begin(VkrScriptHost *host) {
  script_reserve_spawns(host);
  for (uint32_t m = 0; m < host->module_count; ++m) {
    const VkrScriptModule *module = &host->modules[m];
    if (!module->desc || module->retired) {
      continue;
    }
    const bool8_t world_scope = module->desc->scope == VKR_SCOPE_WORLD;
    for (uint32_t pass = 0; pass < 2u; ++pass) {
      const uint32_t container =
          pass == 0u ? host->world_container : host->active_container;
      if ((pass == 1u && (world_scope || container == host->world_container))) {
        continue;
      }
      VkrScriptInstance *instance = script_instance_create(host, m, container);
      if (!instance || !script_instances_push(host, instance)) {
        if (instance) {
          script_instance_free(host, instance);
        }
        return script_host_fail(host, module->name,
                                "Script instance allocation failed");
      }
    }
  }
  for (uint32_t i = 0; i < host->instance_count; ++i) {
    VkrScriptInstance *instance = host->instances[i];
    const VkrModuleDesc *desc = script_instance_desc(host, instance);
    instance->started = true_v;
    if (!desc->start) {
      continue;
    }
    ScriptCall call;
    script_call_begin(host, instance, &instance->ledger, VKR_SCRIPT_PHASE_FRAME,
                      &call);
    desc->start(&instance->ctx.base, instance->data);
    if (!script_call_end(host, instance, &call)) {
      return host->error;
    }
  }
  for (uint32_t i = 0; i < host->instance_count; ++i) {
    if (!script_instance_sync(host, host->instances[i])) {
      return host->error;
    }
  }
  bool8_t ticks = false_v;
  bool8_t input = false_v;
  for (uint32_t i = 0; i < host->instance_count; ++i) {
    const VkrScriptInstance *instance = host->instances[i];
    ticks = ticks || script_instance_needs_ticks(host, instance);
    input = input || (script_instance_callable(host, instance) &&
                      script_instance_desc(host, instance)->input);
  }
  VkrScene *active = host->session.active;
  for (uint32_t c = 0; c < host->container_count; ++c) {
    if (!vkr_scene_observe_destroy(host->containers[c].scene,
                                   script_host_entity_destroying, host)) {
      return "The scene already has a destroy observer";
    }
  }
  if (input) {
    if (!input_observe(host->session.input, script_host_input, host)) {
      return "The input already has an observer";
    }
    host->observing_input = true_v;
  }
  if (ticks) {
    const VkrSceneSimulationCallbacks callbacks = {
        .before_physics = script_host_before_physics,
        .after_physics = script_host_after_physics,
        .after_tick = script_host_after_tick,
        .reset = script_host_reset,
        .context = host};
    const char *failure = NULL;
    if (!vkr_scene_simulation_configure(active, &callbacks, &failure)) {
      return failure ? failure : "Script simulation callbacks were refused";
    }
    host->callbacks_installed = true_v;
  }
  return NULL;
}

/* Detaches callbacks and the input observer and ends every instance. */
static void script_session_teardown(VkrScriptHost *host) {
  VkrScene *active = host->session.active;
  vkr_scene_physics_set_paused(active, true_v);
  script_commands_drop(host, NULL);
  host->timed_count = 0u;
  host->owned_count = 0u;
  /* Instances end by stopping, not destroying: what their ledgers release
     runs no destroy hook. */
  for (uint32_t c = 0; c < host->container_count; ++c) {
    (void)vkr_scene_unobserve_destroy(host->containers[c].scene, host);
  }
  if (host->callbacks_installed) {
    (void)vkr_scene_simulation_detach(active, host);
    host->callbacks_installed = false_v;
  }
  if (host->observing_input) {
    (void)input_unobserve(host->session.input, host);
    host->observing_input = false_v;
  }
  script_session_end(host);
}

bool8_t vkr_script_host_start(VkrScriptHost *host,
                              const VkrScriptSessionDesc *desc,
                              const char **error) {
  if (host->started || host->tool || !desc || !desc->active || !desc->input) {
    if (error) {
      *error = "Script session requires a scene and input and no session";
    }
    return false_v;
  }
  VkrScene *active = desc->active;
  vkr_scene_physics_set_paused(active, true_v);
  if ((vkr_scene_simulation_completed_ticks(active) ||
       vkr_scene_physics_debt(active)) &&
      !vkr_scene_physics_reset(active, error)) {
    return false_v;
  }
  host->session = *desc;
  host->container_count = 0u;
  host->active_container = script_container_attach(host, active);
  host->world_container = desc->world
                              ? script_container_attach(host, desc->world)
                              : host->active_container;
  host->started = true_v;
  host->faulted = false_v;
  host->restart_pending = false_v;
  host->view = (VkrScriptView){0};
  host->error[0] = '\0';
  host->frame_serial++;
  const char *failure = script_session_begin(host);
  if (failure) {
    if (failure != host->error) {
      snprintf(host->error, sizeof(host->error), "%s", failure);
    }
    script_session_teardown(host);
    host->started = false_v;
    host->container_count = 0u;
    if (error) {
      *error = host->error;
    }
    return false_v;
  }
  return true_v;
}

void vkr_script_host_stop(VkrScriptHost *host) {
  if (!host->started) {
    return;
  }
  script_session_teardown(host);
  /* Every scope released its tasks; no worker may still hold one when the
     superseded libraries close. */
  script_tasks_collect(host, true_v);
  host->session = (VkrScriptSessionDesc){0};
  host->container_count = 0u;
  host->started = false_v;
  host->faulted = false_v;
  host->restart_pending = false_v;
  host->view = (VkrScriptView){0};
  script_close_retired(host);
}

void vkr_script_host_detach(VkrScriptHost *host, const VkrScene *scene) {
  if (!host->started || !scene) {
    return;
  }
  if (scene == host->session.active) {
    vkr_script_host_stop(host);
    return;
  }
  uint32_t slot = UINT32_MAX;
  for (uint32_t i = 0; i < host->container_count; ++i) {
    if (host->containers[i].scene == scene) {
      slot = i;
    }
  }
  if (slot == UINT32_MAX) {
    return;
  }
  /* The container's instances end, and World-scoped behaviors on its
     entities stop, before its entities go; unloading runs no destroy hook. */
  (void)vkr_scene_unobserve_destroy(host->containers[slot].scene, host);
  for (uint32_t i = host->instance_count; i-- > 0u;) {
    VkrScriptInstance *instance = host->instances[i];
    if (instance->container == slot) {
      script_instance_end(host, instance);
      script_instance_free(host, instance);
      for (uint32_t j = i; j + 1u < host->instance_count; ++j) {
        host->instances[j] = host->instances[j + 1u];
      }
      host->instances[--host->instance_count] = NULL;
      continue;
    }
    for (uint32_t b = instance->binding_count; b-- > 0u;) {
      ScriptBinding *binding = &instance->bindings[b];
      if (script_container_of(host, binding->entity) ==
          &host->containers[slot]) {
        script_binding_end(host, instance, binding);
      }
    }
  }
  /* The World is the last slot; the active container stays at zero. */
  host->container_count = slot;
  host->world_container = host->active_container;
  host->session.world = NULL;
}

bool8_t vkr_script_host_active(const VkrScriptHost *host) {
  for (uint32_t i = 0; host->started && i < host->instance_count; ++i) {
    const VkrScriptInstance *instance = host->instances[i];
    const VkrModuleDesc *desc = script_instance_desc(host, instance);
    if (!script_instance_callable(host, instance)) {
      continue;
    }
    if (instance->binding_count || desc->update || desc->late_update ||
        desc->fixed_update || desc->late_fixed_update || desc->input) {
      return true_v;
    }
  }
  return false_v;
}

/* Restarts after a native reset, keeping the scene's run state. */
static void script_host_restart(VkrScriptHost *host) {
  host->restart_pending = false_v;
  const VkrScriptSessionDesc session = host->session;
  const bool8_t paused = vkr_scene_physics_is_paused(session.active);
  vkr_script_host_stop(host);
  const char *error = NULL;
  if (vkr_script_host_start(host, &session, &error)) {
    vkr_scene_physics_set_paused(session.active, paused);
  }
}

/* A hook failed outside a tick: pause and stop calling hooks. */
static void script_host_fault(VkrScriptHost *host) {
  host->faulted = true_v;
  vkr_scene_physics_set_paused(host->session.active, true_v);
}

void vkr_script_host_frame(VkrScriptHost *host, VkrScriptFrame *frame) {
  host->frame = *frame;
  host->time_step_set = false_v;
  if (host->started && host->restart_pending) {
    script_host_restart(host);
  }
  if (!host->started || host->faulted) {
    return;
  }
  host->frame_serial++;
  /* Released tasks whose workers have let go of them. */
  script_tasks_collect(host, false_v);
  script_commands_flush(host);
  script_lifetimes_update(host);
  script_reserve_spawns(host);
  host->frame_time_valid = true_v;
  for (uint32_t i = 0; i < host->instance_count; ++i) {
    VkrScriptInstance *instance = host->instances[i];
    if (!script_instance_callable(host, instance)) {
      continue;
    }
    if (!script_instance_sync(host, instance) ||
        !script_instance_run(host, instance, SCRIPT_HOOK_UPDATE,
                             (float32_t)frame->scene_delta)) {
      script_host_fault(host);
      break;
    }
  }
  host->frame_time_valid = false_v;
  if (host->time_step_set) {
    frame->scene_delta = host->time_step;
  }
}

void vkr_script_host_present(VkrScriptHost *host, const VkrScriptFrame *frame,
                             VkrScriptView *view) {
  host->frame = *frame;
  host->view = (VkrScriptView){0};
  host->frame_time_valid = true_v;
  for (uint32_t i = 0;
       host->started && !host->faulted && i < host->instance_count; ++i) {
    VkrScriptInstance *instance = host->instances[i];
    if (!script_instance_callable(host, instance)) {
      continue;
    }
    if (!script_instance_run(host, instance, SCRIPT_HOOK_LATE_UPDATE,
                             (float32_t)frame->scene_delta)) {
      script_host_fault(host);
      break;
    }
  }
  host->frame_time_valid = false_v;
  *view = host->view;
}

const char *vkr_script_host_error(const VkrScriptHost *host) {
  return host->error[0] ? host->error : NULL;
}

void *vkr_script_host_instance_data(VkrScriptHost *host, const char *module,
                                    const VkrScene *scene) {
  for (uint32_t i = 0; host->started && i < host->instance_count; ++i) {
    VkrScriptInstance *instance = host->instances[i];
    if (instance->module != SCRIPT_MODULE_NONE &&
        !strcmp(host->modules[instance->module].name, module) &&
        host->containers[instance->container].scene == scene) {
      return instance->data;
    }
  }
  return NULL;
}

VkrCtx *vkr_script_host_open_context(VkrScriptHost *host, VkrScene *scene,
                                     InputState *input,
                                     struct VkrRenderAssets *assets) {
  if (host->started || host->tool || !scene) {
    return NULL;
  }
  host->session =
      (VkrScriptSessionDesc){.active = scene, .input = input, .assets = assets};
  host->container_count = 0u;
  host->active_container = script_container_attach(host, scene);
  host->world_container = host->active_container;
  host->tool =
      script_instance_create(host, SCRIPT_MODULE_NONE, host->active_container);
  if (!host->tool) {
    host->container_count = 0u;
    return NULL;
  }
  host->tool->started = true_v;
  return &host->tool->ctx.base;
}

void vkr_script_host_close_context(VkrScriptHost *host) {
  if (!host->tool) {
    return;
  }
  script_ledger_release(host, &host->tool->ledger);
  script_update_transforms(host);
  script_instance_free(host, host->tool);
  host->tool = NULL;
  host->container_count = 0u;
  host->session = (VkrScriptSessionDesc){0};
  host->bound_animation_count = 0u;
}

bool8_t vkr_script_host_bind_animation(VkrScriptHost *host, VkrEntityId entity,
                                       VkrAnimationPlayer *player) {
  if (!host->tool || !player) {
    return false_v;
  }
  for (uint32_t i = 0; i < host->bound_animation_count; ++i) {
    if (host->bound_animations[i].entity == entity.u64) {
      host->bound_animations[i].player = player;
      return true_v;
    }
  }
  if (host->bound_animation_count == VKR_SCRIPT_BOUND_ANIMATION_MAX) {
    return false_v;
  }
  host->bound_animations[host->bound_animation_count++] =
      (VkrScriptBoundAnimation){.entity = entity.u64, .player = player};
  return true_v;
}
