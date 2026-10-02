/* The SDK table behind sdk.h (ADR-079): each entry adapts one engine call
 * to SDK value types, routes entities to their container, queues structural
 * edits made in fixed updates for right after the tick, and records
 * acquisitions in the calling scope's ledger. */
#include "script/vkr_script_internal.h"

#include "animation/vkr_animation_player.h"
#include "renderer/systems/vkr_scene_animation.h"
#include "renderer/systems/vkr_scene_model.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_simulation.h"
#include "renderer/systems/vkr_scene_types.h"
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(VkrEntity) == sizeof(VkrEntityId),
               "VkrEntity holds a VkrEntityId");
_Static_assert(
    sizeof(VkrAnimSample) == sizeof(VkrAnimationSample) &&
        offsetof(VkrAnimSample, clip) == offsetof(VkrAnimationSample, clip) &&
        offsetof(VkrAnimSample, time) == offsetof(VkrAnimationSample, time) &&
        offsetof(VkrAnimSample, weight) == offsetof(VkrAnimationSample, weight),
    "VkrAnimSample mirrors VkrAnimationSample");
_Static_assert((int)VKR_MOTION_DYNAMIC == (int)VKR_PHYSICS_DYNAMIC &&
                   (int)VKR_BODY_CAPSULE == (int)VKR_PHYSICS_CAPSULE &&
                   (int)VKR_GROUND_IN_AIR == (int)VKR_PHYSICS_CHARACTER_IN_AIR,
               "SDK physics enums match the engine's");
_Static_assert((int)VKR_MODEL_LOADING == (int)VKR_SCENE_MODEL_LOADING &&
                   (int)VKR_MODEL_FAILED == (int)VKR_SCENE_MODEL_FAILED,
               "SDK model states match the scene's");
_Static_assert(VKR_ANIM_BLEND_MAX == VKR_ANIMATION_BLEND_SAMPLE_CAPACITY,
               "The SDK blend cap matches the player's");
_Static_assert((int)VKR_LOG_ERROR == (int)LOG_LEVEL_ERROR &&
                   (int)VKR_LOG_DEBUG == (int)LOG_LEVEL_DEBUG,
               "SDK log levels match the engine's");

static ScriptCtx *sdk_ctx(VkrCtx *ctx) { return (ScriptCtx *)ctx; }

static VkrEntityId sdk_id(VkrEntity entity) {
  return (VkrEntityId){.u64 = entity.id};
}

static const char *sdk_module_name(const ScriptCtx *ctx) {
  const uint32_t module = ctx->instance->module;
  return module == SCRIPT_MODULE_NONE ? "script"
                                      : ctx->host->modules[module].name;
}

/* The scene holding `entity`, or NULL with the context error set. */
static VkrScene *sdk_scene_of(ScriptCtx *ctx, VkrEntity entity) {
  VkrScriptContainer *container = script_container_of(ctx->host, entity.id);
  if (!container) {
    (void)script_ctx_error(ctx, "The entity is in no attached container");
    return NULL;
  }
  if (!vkr_scene_entity_alive(container->scene, sdk_id(entity))) {
    (void)script_ctx_error(ctx, "The entity was destroyed");
    return NULL;
  }
  return container->scene;
}

static void sdk_mark_dirty(ScriptCtx *ctx, const VkrScene *scene) {
  for (uint32_t i = 0; i < ctx->host->container_count; ++i) {
    if (ctx->host->containers[i].scene == scene) {
      ctx->host->transforms_dirty[i] = true_v;
    }
  }
}

/* Applies pending transform edits before a read. */
static void sdk_refresh(ScriptCtx *ctx, VkrScene *scene) {
  script_refresh_scene(ctx->host, scene);
}

static VkrScene *sdk_active(ScriptCtx *ctx) {
  return ctx->host->session.active;
}

// =============================================================================
// Deferred structural edits
// =============================================================================

/* Capacity of queued edits added at a time. */
#define SDK_COMMAND_GROWTH 64u

bool8_t script_pending(const VkrScriptHost *host, uint64_t entity) {
  for (uint32_t i = 0; entity && i < host->pending_count; ++i) {
    if (host->pending[i] == entity) {
      return true_v;
    }
  }
  return false_v;
}

static void sdk_pending_remove(VkrScriptHost *host, uint64_t entity) {
  for (uint32_t i = 0; i < host->pending_count; ++i) {
    if (host->pending[i] == entity) {
      host->pending[i] = host->pending[--host->pending_count];
      return;
    }
  }
}

/* Grows a host-owned array of `size`-byte items to hold one more. */
static bool8_t sdk_grow(VkrScriptHost *host, void **items, uint32_t count,
                        uint32_t *capacity, uint64_t size) {
  if (count < *capacity) {
    return true_v;
  }
  const uint32_t grown = *capacity + SDK_COMMAND_GROWTH;
  void *next = vkr_allocator_alloc(&host->instance_allocator, size * grown,
                                   VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!next) {
    return false_v;
  }
  if (*items) {
    MemCopy(next, *items, size * count);
    vkr_allocator_free(&host->instance_allocator, *items, size * *capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  *items = next;
  *capacity = grown;
  return true_v;
}

/* In a fixed update, a structural call, or any call on a spawn still
 * pending, queues itself for right after the tick. */
static bool8_t sdk_deferred(ScriptCtx *ctx, VkrEntity entity,
                            bool8_t structural) {
  return ctx->host->phase == VKR_SCRIPT_PHASE_TICK &&
         (structural || script_pending(ctx->host, entity.id));
}

/* Copies call data into the queue's arena, which empties after replay. */
static void *sdk_command_copy(ScriptCtx *ctx, const void *data, uint64_t size,
                              uint64_t align) {
  void *copy =
      size ? arena_alloc_aligned(ctx->host->command_arena, size,
                                 align ? align : 1u, ARENA_MEMORY_TAG_UNKNOWN)
           : NULL;
  if (copy) {
    if (data) {
      MemCopy(copy, data, size);
    } else {
      MemZero(copy, size);
    }
  }
  return copy;
}

static const char *sdk_command_text(ScriptCtx *ctx, const char *text) {
  return text ? sdk_command_copy(ctx, text, strlen(text) + 1u, 1u) : NULL;
}

/* Appends a call of the running scope to the queue; NULL with the context
 * error set when memory runs out. */
static ScriptCommand *sdk_queue(ScriptCtx *ctx, ScriptCommandKind kind,
                                VkrEntity entity) {
  VkrScriptHost *host = ctx->host;
  if (!sdk_grow(host, (void **)&host->commands, host->command_count,
                &host->command_capacity, sizeof(*host->commands))) {
    (void)script_ctx_error(ctx, "Queued edit allocation failed");
    return NULL;
  }
  ScriptCommand *command = &host->commands[host->command_count++];
  *command = (ScriptCommand){.kind = kind,
                             .instance = ctx->instance,
                             .ledger = ctx->ledger ? ctx->ledger
                                                   : &ctx->instance->ledger,
                             .entity = entity.id};
  return command;
}

// =============================================================================
// Diagnostics and control
// =============================================================================

static void sdk_log(VkrCtx *ctx, VkrLogLevel level, const char *message) {
  _log_message((LogLevel)level, sdk_module_name(sdk_ctx(ctx)), 0u, "%s",
               message ? message : "");
}

static void sdk_fail(VkrCtx *ctx, const char *message) {
  ScriptCtx *script = sdk_ctx(ctx);
  char text[VKR_SCRIPT_ERROR_CAPACITY];
  snprintf(text, sizeof(text), "%s", message ? message : "script failed");
  snprintf(script->host->error, sizeof(script->host->error), "%s: %s",
           sdk_module_name(script), text);
  script->instance->failed = true_v;
}

static const char *sdk_last_error(VkrCtx *ctx) {
  return sdk_ctx(ctx)->last_error;
}

static void sdk_disable(VkrCtx *ctx) {
  sdk_ctx(ctx)->instance->disabled = true_v;
}

static void *sdk_temp_alloc(VkrCtx *ctx, uint64_t size, uint64_t align) {
  Arena *temp = sdk_ctx(ctx)->host->temp;
  void *memory = size ? arena_alloc_aligned(temp, size, align ? align : 1u,
                                            ARENA_MEMORY_TAG_UNKNOWN)
                      : NULL;
  if (memory) {
    MemZero(memory, size);
  }
  return memory;
}

static bool8_t sdk_option(VkrCtx *ctx, const char *name) {
  return name && !strcmp(name, "gameplay") &&
         sdk_ctx(ctx)->host->session.sample_content;
}

// =============================================================================
// Containers
// =============================================================================

static VkrContainer sdk_container_self(VkrCtx *ctx) {
  return (VkrContainer){.id = sdk_ctx(ctx)->instance->container + 1u};
}

static VkrContainer sdk_container_active(VkrCtx *ctx) {
  return (VkrContainer){.id = sdk_ctx(ctx)->host->active_container + 1u};
}

static VkrContainer sdk_container_world(VkrCtx *ctx) {
  return (VkrContainer){.id = sdk_ctx(ctx)->host->world_container + 1u};
}

static VkrContainer sdk_container_of(VkrCtx *ctx, VkrEntity entity) {
  VkrScriptHost *host = sdk_ctx(ctx)->host;
  VkrScriptContainer *container = script_container_of(host, entity.id);
  return (VkrContainer){
      .id = container ? (uint32_t)(container - host->containers) + 1u : 0u};
}

// =============================================================================
// Entities
// =============================================================================

/* A spawn in a fixed update: a reserved ID now, the entity after the
 * tick. */
static VkrEntity sdk_spawn_deferred(ScriptCtx *ctx, VkrScene *scene,
                                    uint32_t slot, const VkrSpawnDesc *spawn) {
  VkrScriptHost *host = ctx->host;
  const VkrEntityId reserved = vkr_scene_reserve_entity(scene);
  if (!reserved.u64) {
    (void)script_ctx_error(ctx, "No entity IDs are free in this tick");
    return VKR_ENTITY_NONE;
  }
  const VkrEntity entity = {.id = reserved.u64};
  ScriptCommand *command =
      sdk_grow(host, (void **)&host->pending, host->pending_count,
               &host->pending_capacity, sizeof(*host->pending))
          ? sdk_queue(ctx, SCRIPT_COMMAND_SPAWN, entity)
          : NULL;
  if (!command) {
    vkr_scene_cancel_reserved_entity(scene, reserved);
    return VKR_ENTITY_NONE;
  }
  host->pending[host->pending_count++] = reserved.u64;
  command->as.spawn = *spawn;
  command->as.spawn.name = sdk_command_text(ctx, spawn->name);
  command->as.spawn.container = (VkrContainer){.id = slot + 1u};
  return entity;
}

static VkrEntity sdk_spawn(VkrCtx *ctx, const VkrSpawnDesc *desc) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScriptHost *host = script->host;
  const VkrSpawnDesc spawn = desc ? *desc : (VkrSpawnDesc){0};
  const uint32_t slot = script_container_slot(script, spawn.container);
  if (slot == UINT32_MAX) {
    (void)script_ctx_error(script, "Unknown container");
    return VKR_ENTITY_NONE;
  }
  VkrScene *scene = host->containers[slot].scene;
  if (host->phase == VKR_SCRIPT_PHASE_TICK) {
    return sdk_spawn_deferred(script, scene, slot, &spawn);
  }
  /* A replayed spawn creates the entity its reservation named. */
  const VkrEntityId reserved = {.u64 = host->replay_reserved};
  const VkrEntityId entity =
      reserved.u64 ? (vkr_scene_create_reserved_entity(scene, reserved)
                          ? reserved
                          : VKR_ENTITY_ID_INVALID)
                   : vkr_scene_create_entity(scene, NULL);
  if (!entity.u64) {
    (void)script_ctx_error(script, "Entity creation failed");
    return VKR_ENTITY_NONE;
  }
  if (!script_ledger_record(script, SCRIPT_LEDGER_ENTITY, entity.u64, 0u)) {
    vkr_scene_destroy_entity(scene, entity);
    return VKR_ENTITY_NONE;
  }
  const VkrTRS *transform = &spawn.transform;
  const bool8_t zero_rotation =
      transform->rotation.x == 0.0f && transform->rotation.y == 0.0f &&
      transform->rotation.z == 0.0f && transform->rotation.w == 0.0f;
  const bool8_t zero_scale = transform->scale.x == 0.0f &&
                             transform->scale.y == 0.0f &&
                             transform->scale.z == 0.0f;
  bool8_t ok = vkr_scene_set_transform(
      scene, entity, transform->position,
      zero_rotation ? vkr_quat_identity() : transform->rotation,
      zero_scale ? vec3_one() : transform->scale);
  if (ok && spawn.name) {
    ok =
        vkr_scene_set_name(scene, entity,
                           string8_create_from_cstr((const uint8_t *)spawn.name,
                                                    strlen(spawn.name)));
  }
  if (ok && vkr_entity_valid(spawn.parent)) {
    if (script_container_of(script->host, spawn.parent.id) !=
        &script->host->containers[slot]) {
      ok = script_ctx_error(script, "A parent must be in the same container");
    } else {
      vkr_scene_set_parent(scene, entity, sdk_id(spawn.parent));
    }
  }
  /* Saving never writes what scripts spawn. */
  ok = ok && vkr_scene_set_transient(scene, entity);
  ok = ok &&
       script_lifetime_add(host, entity.u64, spawn.owner.id, spawn.lifetime);
  sdk_mark_dirty(script, scene);
  if (!ok) {
    script_ledger_forget(script, SCRIPT_LEDGER_ENTITY, entity.u64, 0u);
    vkr_scene_destroy_entity(scene, entity);
    (void)script_ctx_error(script, "The spawned entity could not be set up");
    return VKR_ENTITY_NONE;
  }
  return (VkrEntity){.id = entity.u64};
}

static void sdk_destroy(VkrCtx *ctx, VkrEntity entity) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (sdk_deferred(script, entity, true_v)) {
    (void)sdk_queue(script, SCRIPT_COMMAND_DESTROY, entity);
    return;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene) {
    return;
  }
  script_ledger_forget(script, SCRIPT_LEDGER_ENTITY, entity.id, 0u);
  script_destroy_tree(script->host, scene, sdk_id(entity));
  sdk_mark_dirty(script, scene);
}

static bool8_t sdk_alive(VkrCtx *ctx, VkrEntity entity) {
  VkrScriptHost *host = sdk_ctx(ctx)->host;
  if (script_pending(host, entity.id)) {
    return true_v;
  }
  VkrScriptContainer *container = script_container_of(host, entity.id);
  return container && vkr_scene_entity_alive(container->scene, sdk_id(entity));
}

static bool8_t sdk_set_name(VkrCtx *ctx, VkrEntity entity, const char *name) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (sdk_deferred(script, entity, true_v)) {
    ScriptCommand *command = sdk_queue(script, SCRIPT_COMMAND_SET_NAME, entity);
    if (command) {
      command->as.name = sdk_command_text(script, name);
    }
    return command && command->as.name;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  return scene && name &&
         vkr_scene_set_name(
             scene, sdk_id(entity),
             string8_create_from_cstr((const uint8_t *)name, strlen(name)));
}

static bool8_t sdk_set_parent(VkrCtx *ctx, VkrEntity entity, VkrEntity parent) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (sdk_deferred(script, entity, true_v)) {
    ScriptCommand *command =
        sdk_queue(script, SCRIPT_COMMAND_SET_PARENT, entity);
    if (command) {
      command->as.parent = parent.id;
    }
    return command != NULL;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene) {
    return false_v;
  }
  if (vkr_entity_valid(parent) &&
      script_container_of(script->host, parent.id) !=
          script_container_of(script->host, entity.id)) {
    return script_ctx_error(script, "A parent must be in the same container");
  }
  vkr_scene_set_parent(scene, sdk_id(entity), sdk_id(parent));
  sdk_mark_dirty(script, scene);
  return true_v;
}

static bool8_t sdk_set_visible(VkrCtx *ctx, VkrEntity entity, bool8_t visible) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (sdk_deferred(script, entity, true_v)) {
    ScriptCommand *command =
        sdk_queue(script, SCRIPT_COMMAND_SET_VISIBLE, entity);
    if (command) {
      command->as.visible = visible;
    }
    return command != NULL;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene) {
    return false_v;
  }
  vkr_scene_set_visibility(scene, sdk_id(entity), visible, true_v);
  return true_v;
}

static bool8_t sdk_visible(VkrCtx *ctx, VkrEntity entity) {
  VkrScene *scene = sdk_scene_of(sdk_ctx(ctx), entity);
  return scene && vkr_scene_entity_visible(scene, sdk_id(entity));
}

static bool8_t sdk_set_transform(VkrCtx *ctx, VkrEntity entity,
                                 const VkrTRS *transform) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (transform && sdk_deferred(script, entity, false_v)) {
    ScriptCommand *command =
        sdk_queue(script, SCRIPT_COMMAND_SET_TRANSFORM, entity);
    if (command) {
      command->as.transform = *transform;
    }
    return command != NULL;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene || !transform) {
    return false_v;
  }
  const bool8_t zero_rotation =
      transform->rotation.x == 0.0f && transform->rotation.y == 0.0f &&
      transform->rotation.z == 0.0f && transform->rotation.w == 0.0f;
  const bool8_t zero_scale = transform->scale.x == 0.0f &&
                             transform->scale.y == 0.0f &&
                             transform->scale.z == 0.0f;
  sdk_mark_dirty(script, scene);
  return vkr_scene_set_transform(scene, sdk_id(entity), transform->position,
                                 zero_rotation ? vkr_quat_identity()
                                               : transform->rotation,
                                 zero_scale ? vec3_one() : transform->scale);
}

static bool8_t sdk_world_matrix(VkrCtx *ctx, VkrEntity entity, Mat4 *out) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene || !out) {
    return false_v;
  }
  sdk_refresh(script, scene);
  const SceneTransform *transform =
      vkr_scene_get_transform(scene, sdk_id(entity));
  if (!transform) {
    return false_v;
  }
  *out = transform->world;
  return true_v;
}

static bool8_t sdk_set_render_pose(VkrCtx *ctx, VkrEntity entity,
                                   const Mat4 *world) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (sdk_deferred(script, entity, true_v)) {
    ScriptCommand *command =
        sdk_queue(script, SCRIPT_COMMAND_SET_RENDER_POSE, entity);
    if (command) {
      command->as.pose.clear = !world;
      command->as.pose.world = world ? *world : mat4_identity();
    }
    return command != NULL;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene) {
    return false_v;
  }
  if (!world) {
    script_ledger_forget(script, SCRIPT_LEDGER_RENDER_POSE, entity.id, 0u);
    return vkr_scene_set_evaluated_transform(scene, sdk_id(entity), NULL);
  }
  /* The first override in a scope records its release. */
  const bool8_t had = vkr_entity_has_component(scene->world, sdk_id(entity),
                                               scene->comp_evaluated_transform);
  if (!vkr_scene_set_evaluated_transform(scene, sdk_id(entity), world)) {
    return script_ctx_error(script, "The render pose was refused");
  }
  if (!had &&
      !script_ledger_record(script, SCRIPT_LEDGER_RENDER_POSE, entity.id, 0u)) {
    (void)vkr_scene_set_evaluated_transform(scene, sdk_id(entity), NULL);
    return false_v;
  }
  return true_v;
}

/* Deeper hierarchies answer from their first levels. */
#define SDK_VISUAL_DEPTH_MAX 64u

static bool8_t sdk_subtree_visual(const VkrScene *scene, VkrEntityId entity,
                                  uint32_t depth) {
  if (vkr_entity_has_component(scene->world, entity,
                               scene->comp_mesh_renderer) ||
      vkr_entity_has_component(scene->world, entity, scene->comp_shape)) {
    return true_v;
  }
  uint32_t count = 0;
  const VkrEntityId *children =
      depth < SDK_VISUAL_DEPTH_MAX
          ? vkr_scene_get_children(scene, entity, &count)
          : NULL;
  for (uint32_t i = 0; i < count; ++i) {
    if (sdk_subtree_visual(scene, children[i], depth + 1u)) {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t sdk_has_visual(VkrCtx *ctx, VkrEntity entity) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene) {
    return false_v;
  }
  sdk_refresh(script, scene);
  return sdk_subtree_visual(scene, sdk_id(entity), 0u);
}

static bool8_t sdk_set_shape(VkrCtx *ctx, VkrEntity entity,
                             const VkrShapeDesc *shape) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (shape && sdk_deferred(script, entity, true_v)) {
    ScriptCommand *command =
        sdk_queue(script, SCRIPT_COMMAND_SET_SHAPE, entity);
    if (command) {
      command->as.shape = *shape;
    }
    return command != NULL;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene || !shape) {
    return false_v;
  }
  VkrSceneShapeConfig config = VKR_SCENE_SHAPE_CONFIG_DEFAULT;
  config.dimensions = shape->size;
  config.color = shape->color;
  return vkr_scene_set_shape(scene, script->host->session.assets,
                             sdk_id(entity), &config, NULL) ||
         script_ctx_error(script, "The shape could not be created");
}

static bool8_t sdk_spawn_model(VkrCtx *ctx, VkrEntity entity, const char *mesh,
                               const char *animation) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (sdk_deferred(script, entity, true_v)) {
    ScriptCommand *command =
        sdk_queue(script, SCRIPT_COMMAND_SPAWN_MODEL, entity);
    if (command) {
      command->as.model.mesh = sdk_command_text(script, mesh);
      command->as.model.animation = sdk_command_text(script, animation);
    }
    return command != NULL;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene) {
    return false_v;
  }
  VkrSceneModelDesc desc = {.animation = VKR_SCENE_ANIMATION_CONFIG_DEFAULT};
  if (mesh) {
    desc.mesh_path =
        string8_create_from_cstr((const uint8_t *)mesh, strlen(mesh));
  }
  if (animation) {
    desc.animation_path =
        string8_create_from_cstr((const uint8_t *)animation, strlen(animation));
  }
  const char *error = NULL;
  if (!vkr_scene_request_model(scene, script->host->session.assets,
                               sdk_id(entity), &desc, &error)) {
    return script_ctx_error(script, error);
  }
  sdk_mark_dirty(script, scene);
  if (!script_ledger_record(script, SCRIPT_LEDGER_MODEL, entity.id, 0u)) {
    vkr_scene_despawn_model(scene, sdk_id(entity));
    return false_v;
  }
  return true_v;
}

static VkrModelState sdk_model_state(VkrCtx *ctx, VkrEntity entity) {
  VkrScene *scene = sdk_scene_of(sdk_ctx(ctx), entity);
  return scene ? (VkrModelState)vkr_scene_model_status(scene, sdk_id(entity),
                                                       NULL)
               : VKR_MODEL_NONE;
}

static void sdk_despawn_model(VkrCtx *ctx, VkrEntity entity) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (sdk_deferred(script, entity, true_v)) {
    (void)sdk_queue(script, SCRIPT_COMMAND_DESPAWN_MODEL, entity);
    return;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene) {
    return;
  }
  script_ledger_forget(script, SCRIPT_LEDGER_MODEL, entity.id, 0u);
  vkr_scene_despawn_model(scene, sdk_id(entity));
  sdk_mark_dirty(script, scene);
}

// =============================================================================
// Components and runtime state
// =============================================================================

static const VkrComponentDesc *sdk_component_named(VkrCtx *ctx,
                                                   const char *name) {
  VkrScriptHost *host = sdk_ctx(ctx)->host;
  if (!name) {
    return NULL;
  }
  for (uint32_t i = 0; i < host->named_type_count; ++i) {
    if (!strcmp(host->named_types[i].name, name)) {
      return &host->named_types[i];
    }
  }
  const VkrTypeDesc *type = vkr_scene_world_type_named(
      string8_create_from_cstr((const uint8_t *)name, strlen(name)));
  if (!type || host->named_type_count == ArrayCount(host->named_types)) {
    return NULL;
  }
  VkrComponentDesc *desc = &host->named_types[host->named_type_count++];
  /* Borrows the registered type's strings, which live for the process. */
  *desc = (VkrComponentDesc){.name = type->name,
                             .label = type->label,
                             .size = type->size,
                             .align = type->align,
                             .host_binding = type};
  return desc;
}

static const void *sdk_component_get(VkrCtx *ctx, VkrEntity entity,
                                     const VkrComponentDesc *type) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScene *scene = sdk_scene_of(script, entity);
  return scene ? vkr_scene_get_typed(scene, sdk_id(entity),
                                     script_resolve_type(script->host, type))
               : NULL;
}

static void *sdk_component_get_mut(VkrCtx *ctx, VkrEntity entity,
                                   const VkrComponentDesc *type) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene) {
    return NULL;
  }
  const VkrComponentTypeId id =
      vkr_scene_type_id(scene, script_resolve_type(script->host, type));
  return id == VKR_COMPONENT_TYPE_INVALID
             ? NULL
             : vkr_entity_get_component_if_alive(scene->world, sdk_id(entity),
                                                 id);
}

static bool8_t sdk_component_set(VkrCtx *ctx, VkrEntity entity,
                                 const VkrComponentDesc *type,
                                 const void *value) {
  ScriptCtx *script = sdk_ctx(ctx);
  const VkrTypeDesc *resolved = script_resolve_type(script->host, type);
  if (!resolved) {
    return script_ctx_error(script, "Unknown component type");
  }
  if (sdk_deferred(script, entity, true_v)) {
    ScriptCommand *command =
        sdk_queue(script, SCRIPT_COMMAND_COMPONENT_SET, entity);
    if (command) {
      command->as.component.type = type;
      command->as.component.value =
          value
              ? sdk_command_copy(script, value, resolved->size, resolved->align)
              : NULL;
    }
    return command && (!value || command->as.component.value);
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene) {
    return false_v;
  }
  void *defaults = NULL;
  if (!value) {
    defaults = sdk_temp_alloc(ctx, resolved->size, resolved->align);
    if (!defaults) {
      return script_ctx_error(script, "Out of temp memory");
    }
    if (resolved->defaults) {
      resolved->defaults(defaults);
    }
    value = defaults;
  }
  return vkr_scene_set_typed(scene, sdk_id(entity), resolved, value) ||
         script_ctx_error(script, "The component value was refused");
}

static uint32_t sdk_component_find(VkrCtx *ctx, VkrContainer container,
                                   const VkrComponentDesc *type, VkrEntity *out,
                                   uint32_t capacity) {
  ScriptCtx *script = sdk_ctx(ctx);
  const uint32_t slot = script_container_slot(script, container);
  const VkrTypeDesc *resolved = script_resolve_type(script->host, type);
  if (slot == UINT32_MAX || !resolved) {
    return 0u;
  }
  return vkr_scene_find_typed(script->host->containers[slot].scene, resolved,
                              (VkrEntityId *)out, out ? capacity : 0u);
}

static VkrStateType sdk_state_type(VkrCtx *ctx, const char *name, uint32_t size,
                                   uint32_t align) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScriptHost *host = script->host;
  if (!name || !size || !align || (align & (align - 1u))) {
    (void)script_ctx_error(script, "A state type needs a name, size and "
                                   "power-of-two alignment");
    return (VkrStateType){0};
  }
  for (uint32_t i = 0; i < host->state_type_count; ++i) {
    VkrScriptStateType *type = &host->state_types[i];
    if (!strcmp(type->name, name)) {
      if (type->size != size || type->align != align) {
        (void)script_ctx_error(script, "A state type changed its layout");
        return (VkrStateType){0};
      }
      return (VkrStateType){.id = i + 1u};
    }
  }
  if (host->state_type_count == VKR_SCRIPT_STATE_TYPE_MAX) {
    (void)script_ctx_error(script, "Too many state types");
    return (VkrStateType){0};
  }
  VkrScriptStateType *type = &host->state_types[host->state_type_count];
  *type = (VkrScriptStateType){.size = size, .align = align};
  snprintf(type->name, sizeof(type->name), "%s", name);
  for (uint32_t c = 0; c < VKR_SCRIPT_CONTAINER_MAX; ++c) {
    type->ids[c] = VKR_COMPONENT_TYPE_INVALID;
  }
  return (VkrStateType){.id = ++host->state_type_count};
}

/* The state type's ECS component in the entity's container, registered on
 * first use. */
static VkrComponentTypeId sdk_state_id(ScriptCtx *ctx, VkrEntity entity,
                                       VkrStateType type, VkrScene **scene) {
  VkrScriptHost *host = ctx->host;
  *scene = sdk_scene_of(ctx, entity);
  if (!*scene || !type.id || type.id > host->state_type_count) {
    return VKR_COMPONENT_TYPE_INVALID;
  }
  VkrScriptStateType *state = &host->state_types[type.id - 1u];
  const uint32_t slot =
      (uint32_t)(script_container_of(host, entity.id) - host->containers);
  if (state->ids[slot] == VKR_COMPONENT_TYPE_INVALID) {
    state->ids[slot] = vkr_entity_register_component_once(
        (*scene)->world, state->name, state->size, state->align);
  }
  return state->ids[slot];
}

static void *sdk_state_add(VkrCtx *ctx, VkrEntity entity, VkrStateType type,
                           const void *value) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (sdk_deferred(script, entity, true_v)) {
    /* The caller fills the queued value until the tick ends. */
    if (!type.id || type.id > script->host->state_type_count) {
      (void)script_ctx_error(script, "Unknown state type");
      return NULL;
    }
    const VkrScriptStateType *state = &script->host->state_types[type.id - 1u];
    ScriptCommand *command =
        sdk_queue(script, SCRIPT_COMMAND_STATE_ADD, entity);
    if (!command) {
      return NULL;
    }
    command->as.state.type = type;
    command->as.state.value =
        sdk_command_copy(script, value, state->size, state->align);
    return command->as.state.value;
  }
  VkrScene *scene = NULL;
  const VkrComponentTypeId id = sdk_state_id(script, entity, type, &scene);
  if (id == VKR_COMPONENT_TYPE_INVALID) {
    return NULL;
  }
  if (vkr_entity_has_component(scene->world, sdk_id(entity), id)) {
    (void)script_ctx_error(script, "The entity already has that state");
    return NULL;
  }
  const VkrScriptStateType *state = &script->host->state_types[type.id - 1u];
  const void *initial =
      value ? value : sdk_temp_alloc(ctx, state->size, state->align);
  if (!initial ||
      !vkr_entity_add_component(scene->world, sdk_id(entity), id, initial)) {
    (void)script_ctx_error(script, "State allocation failed");
    return NULL;
  }
  if (!script_ledger_record(script, SCRIPT_LEDGER_STATE, entity.id,
                            type.id - 1u)) {
    (void)vkr_entity_remove_component(scene->world, sdk_id(entity), id);
    return NULL;
  }
  return vkr_entity_get_component_if_alive(scene->world, sdk_id(entity), id);
}

static void *sdk_state_get(VkrCtx *ctx, VkrEntity entity, VkrStateType type) {
  VkrScene *scene = NULL;
  const VkrComponentTypeId id =
      sdk_state_id(sdk_ctx(ctx), entity, type, &scene);
  return id == VKR_COMPONENT_TYPE_INVALID
             ? NULL
             : vkr_entity_get_component_if_alive(scene->world, sdk_id(entity),
                                                 id);
}

static void sdk_state_remove(VkrCtx *ctx, VkrEntity entity, VkrStateType type) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (sdk_deferred(script, entity, true_v)) {
    ScriptCommand *command =
        sdk_queue(script, SCRIPT_COMMAND_STATE_REMOVE, entity);
    if (command) {
      command->as.state.type = type;
    }
    return;
  }
  VkrScene *scene = NULL;
  const VkrComponentTypeId id = sdk_state_id(script, entity, type, &scene);
  if (id == VKR_COMPONENT_TYPE_INVALID) {
    return;
  }
  script_ledger_forget(script, SCRIPT_LEDGER_STATE, entity.id, type.id - 1u);
  (void)vkr_entity_remove_component(scene->world, sdk_id(entity), id);
}

static bool8_t sdk_player_start(VkrCtx *ctx, Mat4 *out_world) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScene *scene = sdk_active(script);
  if (!scene || !out_world) {
    return false_v;
  }
  sdk_refresh(script, scene);
  return vkr_scene_player_start(scene, out_world);
}

// =============================================================================
// Clock and frame
// =============================================================================

static float64_t sdk_time(VkrCtx *ctx) {
  const VkrScriptHost *host = sdk_ctx(ctx)->host;
  return host->frame_time_valid ? host->frame.now
                                : vkr_platform_get_absolute_time();
}

static float64_t sdk_fixed_dt(VkrCtx *ctx) {
  (void)ctx;
  return VKR_SCENE_SIMULATION_FIXED_DT;
}

static uint64_t sdk_ticks(VkrCtx *ctx) {
  return vkr_scene_simulation_completed_ticks(sdk_active(sdk_ctx(ctx)));
}

static float64_t sdk_sim_time(VkrCtx *ctx) {
  return vkr_scene_physics_time(sdk_active(sdk_ctx(ctx)));
}

static float64_t sdk_sim_debt(VkrCtx *ctx) {
  return vkr_scene_physics_debt(sdk_active(sdk_ctx(ctx)));
}

static bool8_t sdk_simulating(VkrCtx *ctx) {
  const VkrScene *scene = sdk_active(sdk_ctx(ctx));
  return !scene->physics_paused && !scene->physics_disabled &&
         !scene->simulation.faulted;
}

static bool8_t sdk_paused(VkrCtx *ctx) {
  return vkr_scene_physics_is_paused(sdk_active(sdk_ctx(ctx)));
}

static bool8_t sdk_playing(VkrCtx *ctx) {
  return sdk_ctx(ctx)->host->frame.simulation_running;
}

static void sdk_set_time_step(VkrCtx *ctx, float64_t dt) {
  VkrScriptHost *host = sdk_ctx(ctx)->host;
  if (isfinite(dt) && dt >= 0.0) {
    host->time_step = dt;
    host->time_step_set = true_v;
  }
}

static Vec3 sdk_gravity(VkrCtx *ctx) {
  return vkr_scene_gravity(sdk_active(sdk_ctx(ctx)));
}

static bool8_t sdk_input_focused(VkrCtx *ctx) {
  return sdk_ctx(ctx)->host->frame.input_focused;
}

static bool8_t sdk_key_down(VkrCtx *ctx, VkrKey key) {
  InputState *input = sdk_ctx(ctx)->host->session.input;
  return input && input_is_key_down(input, (Keys)key);
}

static bool8_t sdk_camera_available(VkrCtx *ctx) {
  return sdk_ctx(ctx)->host->frame.camera_available;
}

static void sdk_set_camera(VkrCtx *ctx, Vec3 position, float32_t yaw_degrees,
                           float32_t pitch_degrees) {
  VkrScriptView *view = &sdk_ctx(ctx)->host->view;
  view->camera_valid = true_v;
  view->camera_position = position;
  view->camera_yaw_degrees = yaw_degrees;
  view->camera_pitch_degrees = pitch_degrees;
}

static void sdk_hud_text(VkrCtx *ctx, const char *text) {
  VkrScriptView *view = &sdk_ctx(ctx)->host->view;
  if (!text || !text[0]) {
    return;
  }
  const uint32_t length = (uint32_t)strlen(view->hud);
  snprintf(view->hud + length, sizeof(view->hud) - length, "%s%s",
           length ? "\n" : "", text);
}

// =============================================================================
// Physics
// =============================================================================

static VkrBodyDesc sdk_body_default(VkrCtx *ctx) {
  (void)ctx;
  const VkrScenePhysicsSnapshot snapshot = vkr_scene_physics_default();
  const VkrSceneColliderConfig *collider = &snapshot.colliders[0];
  return (VkrBodyDesc){
      .motion = (VkrMotion)snapshot.body.motion,
      .shape = VKR_BODY_BOX,
      .half_extent = collider->half_extent,
      .radius = collider->radius,
      .half_height = collider->half_height,
      .mass = snapshot.body.mass,
      .friction = snapshot.body.friction,
      .restitution = snapshot.body.restitution,
      .gravity_factor = snapshot.body.gravity_factor,
      .sensor = snapshot.body.sensor,
      .layer = snapshot.collision_layer,
      .mask = snapshot.collision_mask,
  };
}

static bool8_t sdk_set_body(VkrCtx *ctx, VkrEntity entity,
                            const VkrBodyDesc *body) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (body && sdk_deferred(script, entity, true_v)) {
    ScriptCommand *command = sdk_queue(script, SCRIPT_COMMAND_SET_BODY, entity);
    if (command) {
      command->as.body = *body;
    }
    return command != NULL;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene || !body) {
    return false_v;
  }
  VkrScenePhysicsSnapshot snapshot = vkr_scene_physics_default();
  snapshot.body.motion = (VkrPhysicsMotion)body->motion;
  snapshot.body.mass = body->mass;
  snapshot.body.friction = body->friction;
  snapshot.body.restitution = body->restitution;
  snapshot.body.gravity_factor = body->gravity_factor;
  snapshot.body.sensor = body->sensor;
  snapshot.collision_layer = body->layer;
  snapshot.collision_mask = body->mask;
  VkrSceneColliderConfig *collider = &snapshot.colliders[0];
  collider->shape = (VkrPhysicsShape)body->shape;
  collider->half_extent = body->half_extent;
  collider->radius = body->radius;
  collider->half_height = body->half_height;
  const char *error = NULL;
  return vkr_scene_physics_apply(scene, sdk_id(entity), &snapshot, &error) ||
         script_ctx_error(script, error);
}

static bool8_t sdk_impulse(VkrCtx *ctx, VkrEntity entity, Vec3 impulse,
                           const Vec3 *world_point) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScene *scene = sdk_scene_of(script, entity);
  const char *error = NULL;
  return scene && (vkr_scene_physics_impulse(scene, sdk_id(entity), impulse,
                                             world_point, &error) ||
                   script_ctx_error(script, error));
}

static VkrPhysicsQueryFilter sdk_filter(const VkrQueryFilter *filter) {
  return filter
             ? (VkrPhysicsQueryFilter){.mask = filter->mask,
                                       .include_sensors =
                                           filter->include_sensors,
                                       .ignored_entities =
                                           (const uint64_t *)filter->ignored,
                                       .ignored_count = filter->ignored_count}
             : (VkrPhysicsQueryFilter){.mask = UINT16_MAX};
}

static VkrRayHit sdk_hit(const VkrPhysicsRayHit *hit) {
  return (VkrRayHit){
      .entity = {.id = hit->entity_id},
      .collider = {.id = hit->collider_entity_id},
      .fraction = hit->fraction,
      .position =
          vec3_new(hit->position[0], hit->position[1], hit->position[2]),
      .normal = vec3_new(hit->normal[0], hit->normal[1], hit->normal[2]),
  };
}

static bool8_t sdk_raycast(VkrCtx *ctx, Vec3 origin, Vec3 displacement,
                           const VkrQueryFilter *filter, VkrRayHit *hit) {
  const VkrPhysicsQueryFilter query = sdk_filter(filter);
  VkrPhysicsRayHit result = {0};
  if (!vkr_scene_physics_raycast_query(sdk_active(sdk_ctx(ctx)), origin,
                                       displacement, &query, &result)) {
    return false_v;
  }
  if (hit) {
    *hit = sdk_hit(&result);
  }
  return true_v;
}

static bool8_t sdk_sweep_sphere(VkrCtx *ctx, Vec3 origin, Vec3 displacement,
                                float32_t radius, const VkrQueryFilter *filter,
                                VkrRayHit *hit) {
  ScriptCtx *script = sdk_ctx(ctx);
  const VkrPhysicsQueryFilter query = sdk_filter(filter);
  VkrPhysicsRayHit result = {0};
  bool8_t found = false_v;
  if (!vkr_scene_physics_sweep_sphere(sdk_active(script), origin, displacement,
                                      radius, &query, &result, &found)) {
    return script_ctx_error(script, "The sphere sweep failed");
  }
  if (found && hit) {
    *hit = sdk_hit(&result);
  }
  return found;
}

static VkrCharacterDesc sdk_character_default(VkrCtx *ctx) {
  (void)ctx;
  const VkrPhysicsCharacterDesc desc = vkr_physics_character_default();
  return (VkrCharacterDesc){.radius = desc.radius,
                            .half_height = desc.half_height,
                            .max_slope_radians = desc.max_slope_radians,
                            .step_up = desc.step_up,
                            .step_down = desc.step_down,
                            .mass = desc.mass,
                            .max_strength = desc.max_strength,
                            .layer = desc.collision_layer,
                            .mask = desc.collision_mask};
}

static void sdk_character_state_from(VkrCharacterState *out,
                                     const VkrPhysicsCharacterState *state) {
  *out = (VkrCharacterState){
      .foot = vec3_new(state->foot_position[0], state->foot_position[1],
                       state->foot_position[2]),
      .velocity =
          vec3_new(state->velocity[0], state->velocity[1], state->velocity[2]),
      .ground_velocity =
          vec3_new(state->ground_velocity[0], state->ground_velocity[1],
                   state->ground_velocity[2]),
      .ground_normal =
          vec3_new(state->ground_normal[0], state->ground_normal[1],
                   state->ground_normal[2]),
      .ground_entity = {.id = state->ground_entity_id},
      .ground = (VkrGround)state->ground,
      .crouched = state->crouched,
  };
}

static bool8_t sdk_character_create(VkrCtx *ctx, VkrEntity entity,
                                    const VkrCharacterDesc *desc,
                                    const Vec3 *spawn_foot) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (sdk_deferred(script, entity, true_v)) {
    ScriptCommand *command =
        sdk_queue(script, SCRIPT_COMMAND_CHARACTER_CREATE, entity);
    if (command) {
      command->as.character.desc = desc ? *desc : sdk_character_default(ctx);
      command->as.character.has_foot = spawn_foot != NULL;
      command->as.character.foot = spawn_foot ? *spawn_foot : vec3_zero();
    }
    return command != NULL;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene) {
    return false_v;
  }
  VkrPhysicsCharacterDesc settings = vkr_physics_character_default();
  if (desc) {
    settings.radius = desc->radius;
    settings.half_height = desc->half_height;
    settings.max_slope_radians = desc->max_slope_radians;
    settings.step_up = desc->step_up;
    settings.step_down = desc->step_down;
    settings.mass = desc->mass;
    settings.max_strength = desc->max_strength;
    settings.collision_layer = desc->layer;
    settings.collision_mask = desc->mask;
  }
  const char *error = NULL;
  if (!vkr_scene_character_create(scene, sdk_id(entity), &settings, spawn_foot,
                                  &error)) {
    return script_ctx_error(script, error);
  }
  if (!script_ledger_record(script, SCRIPT_LEDGER_CHARACTER, entity.id, 0u)) {
    (void)vkr_scene_character_destroy(scene, sdk_id(entity), NULL);
    return false_v;
  }
  return true_v;
}

static void sdk_character_destroy(VkrCtx *ctx, VkrEntity entity) {
  ScriptCtx *script = sdk_ctx(ctx);
  if (sdk_deferred(script, entity, true_v)) {
    (void)sdk_queue(script, SCRIPT_COMMAND_CHARACTER_DESTROY, entity);
    return;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene) {
    return;
  }
  script_ledger_forget(script, SCRIPT_LEDGER_CHARACTER, entity.id, 0u);
  (void)vkr_scene_character_destroy(scene, sdk_id(entity), NULL);
}

static bool8_t sdk_character_state(VkrCtx *ctx, VkrEntity entity,
                                   VkrCharacterState *out) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScene *scene = sdk_scene_of(script, entity);
  VkrPhysicsCharacterState state;
  const char *error = NULL;
  if (!scene ||
      !vkr_scene_character_get_state(scene, sdk_id(entity), &state, &error)) {
    return scene ? script_ctx_error(script, error) : false_v;
  }
  if (out) {
    sdk_character_state_from(out, &state);
  }
  return true_v;
}

static bool8_t sdk_character_move(VkrCtx *ctx, VkrEntity entity,
                                  const VkrCharacterMove *move,
                                  VkrCharacterState *out) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScene *scene = sdk_scene_of(script, entity);
  if (!scene || !move) {
    return false_v;
  }
  const VkrPhysicsCharacterInput input = {
      .velocity = {move->velocity.x, move->velocity.y, move->velocity.z},
      .gravity = {move->gravity.x, move->gravity.y, move->gravity.z},
      .dt = move->dt,
      .crouch = move->crouch};
  VkrPhysicsCharacterState state;
  const char *error = NULL;
  if (!vkr_scene_character_step(scene, sdk_id(entity), &input, &state,
                                &error)) {
    return script_ctx_error(script, error);
  }
  if (out) {
    sdk_character_state_from(out, &state);
  }
  return true_v;
}

// =============================================================================
// Animation
// =============================================================================

/* A tool's bound player, or NULL. */
static VkrAnimationPlayer *sdk_bound_player(const VkrScriptHost *host,
                                            VkrEntity entity) {
  for (uint32_t i = 0; i < host->bound_animation_count; ++i) {
    if (host->bound_animations[i].entity == entity.id) {
      return host->bound_animations[i].player;
    }
  }
  return NULL;
}

static VkrAnimationPlayer *sdk_player(VkrCtx *ctx, VkrEntity entity) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrAnimationPlayer *bound = sdk_bound_player(script->host, entity);
  if (bound) {
    return bound;
  }
  VkrScene *scene = sdk_scene_of(script, entity);
  VkrAnimationPlayer *player =
      scene ? vkr_scene_animation_get_player(scene, sdk_id(entity)) : NULL;
  if (scene && !player) {
    (void)script_ctx_error(script, "The entity has no animation");
  }
  return player;
}

static const VkrAnimationAsset *sdk_asset(VkrCtx *ctx, VkrEntity entity) {
  VkrAnimationPlayer *player = sdk_player(ctx, entity);
  return player ? vkr_animation_player_asset(player) : NULL;
}

static uint64_t sdk_anim_id(VkrCtx *ctx, VkrEntity entity) {
  VkrScriptHost *host = sdk_ctx(ctx)->host;
  VkrAnimationPlayer *player = sdk_bound_player(host, entity);
  VkrScriptContainer *container = script_container_of(host, entity.id);
  if (!player && container &&
      vkr_scene_entity_alive(container->scene, sdk_id(entity))) {
    player = vkr_scene_animation_get_player(container->scene, sdk_id(entity));
  }
  return (uint64_t)(uintptr_t)player;
}

static bool8_t sdk_anim_has_graph(VkrCtx *ctx, VkrEntity entity) {
  VkrScene *scene = sdk_scene_of(sdk_ctx(ctx), entity);
  return scene && vkr_scene_animation_get_graph(scene, sdk_id(entity));
}

static uint32_t sdk_anim_clip_count(VkrCtx *ctx, VkrEntity entity) {
  const VkrAnimationAsset *asset = sdk_asset(ctx, entity);
  return asset ? asset->clip_count : 0u;
}

static uint32_t sdk_anim_clip_find(VkrCtx *ctx, VkrEntity entity,
                                   const char *name) {
  const VkrAnimationAsset *asset = sdk_asset(ctx, entity);
  const uint64_t length = name ? strlen(name) : 0u;
  for (uint32_t i = 0; asset && name && i < asset->clip_count; ++i) {
    if (asset->clips[i].name.length == length &&
        MemCompare(asset->clips[i].name.str, name, length) == 0) {
      return i;
    }
  }
  return VKR_CLIP_NONE;
}

static float32_t sdk_anim_clip_duration(VkrCtx *ctx, VkrEntity entity,
                                        uint32_t clip) {
  const VkrAnimationAsset *asset = sdk_asset(ctx, entity);
  return asset && clip < asset->clip_count ? asset->clips[clip].duration : 0.0f;
}

static uint32_t sdk_anim_bone_count(VkrCtx *ctx, VkrEntity entity) {
  const VkrAnimationAsset *asset = sdk_asset(ctx, entity);
  return asset ? asset->node_count : 0u;
}

static bool8_t sdk_anim_bone_pose(VkrCtx *ctx, VkrEntity entity, uint32_t bone,
                                  Mat4 *out) {
  VkrAnimationPlayer *player = sdk_player(ctx, entity);
  const VkrAnimationAsset *asset =
      player ? vkr_animation_player_asset(player) : NULL;
  if (!asset || bone >= asset->node_count || !out) {
    return false_v;
  }
  *out = vkr_animation_player_global_pose(player)[bone];
  return true_v;
}

static bool8_t sdk_anim_play(VkrCtx *ctx, VkrEntity entity, uint32_t clip,
                             bool8_t loop) {
  VkrAnimationPlayer *player = sdk_player(ctx, entity);
  return player && vkr_animation_player_select_clip(player, clip, loop);
}

static bool8_t sdk_anim_crossfade(VkrCtx *ctx, VkrEntity entity, uint32_t clip,
                                  bool8_t loop, float64_t duration) {
  VkrAnimationPlayer *player = sdk_player(ctx, entity);
  return player && vkr_animation_player_crossfade(player, clip, loop, duration);
}

static void sdk_anim_set_playing(VkrCtx *ctx, VkrEntity entity,
                                 bool8_t playing) {
  VkrAnimationPlayer *player = sdk_player(ctx, entity);
  if (player) {
    vkr_animation_player_set_playing(player, playing);
  }
}

static bool8_t sdk_anim_set_rate(VkrCtx *ctx, VkrEntity entity,
                                 float64_t rate) {
  VkrAnimationPlayer *player = sdk_player(ctx, entity);
  return player && vkr_animation_player_set_rate(player, rate);
}

static float64_t sdk_anim_rate(VkrCtx *ctx, VkrEntity entity) {
  VkrAnimationPlayer *player = sdk_player(ctx, entity);
  return player ? vkr_animation_player_rate(player) : 0.0;
}

static float64_t sdk_anim_time(VkrCtx *ctx, VkrEntity entity) {
  VkrAnimationPlayer *player = sdk_player(ctx, entity);
  return player ? vkr_animation_player_time(player) : 0.0;
}

static float64_t sdk_anim_duration(VkrCtx *ctx, VkrEntity entity) {
  VkrAnimationPlayer *player = sdk_player(ctx, entity);
  return player ? vkr_animation_player_duration(player) : 0.0;
}

static bool8_t sdk_anim_blend(VkrCtx *ctx, VkrEntity entity,
                              const VkrAnimSample *samples, uint32_t count,
                              bool8_t discontinuity) {
  VkrAnimationPlayer *player = sdk_player(ctx, entity);
  return player &&
         vkr_animation_player_sample_blend(
             player, (const VkrAnimationSample *)samples, count, discontinuity);
}

// =============================================================================
// Tasks
// =============================================================================

/* Data follows the task, aligned for any SDK value. */
#define SCRIPT_TASK_HEADER AlignPow2(sizeof(ScriptTask), 16u)

static void *script_task_data(ScriptTask *task) {
  return (uint8_t *)task + SCRIPT_TASK_HEADER;
}

static uint64_t script_task_bytes(const ScriptTask *task) {
  return SCRIPT_TASK_HEADER + task->size;
}

/* Runs the task unless someone else claimed it. */
static void script_task_execute(ScriptTask *task) {
  if (!vkr_atomic_bool_exchange(&task->claimed, true_v,
                                VKR_MEMORY_ORDER_ACQ_REL)) {
    task->fn(script_task_data(task));
    vkr_atomic_bool_store(&task->done, true_v, VKR_MEMORY_ORDER_RELEASE);
  }
}

static bool8_t script_task_job(VkrJobContext *context, void *payload) {
  (void)context;
  ScriptTask *task = *(ScriptTask **)payload;
  script_task_execute(task);
  /* The task may be freed as soon as this is visible. */
  vkr_atomic_bool_store(&task->job_exited, true_v, VKR_MEMORY_ORDER_RELEASE);
  return true_v;
}

/* Runs an unclaimed task here, or waits for the worker running it. */
static void script_task_finish(ScriptTask *task) {
  script_task_execute(task);
  while (!vkr_atomic_bool_load(&task->done, VKR_MEMORY_ORDER_ACQUIRE)) {
    vkr_thread_sleep(0u);
  }
}

ScriptTask *script_task_find(VkrScriptHost *host, uint64_t id) {
  for (uint32_t i = 0; id && i < host->task_count; ++i) {
    if (host->tasks[i]->id == id && !host->tasks[i]->released) {
      return host->tasks[i];
    }
  }
  return NULL;
}

static bool8_t script_task_freeable(const ScriptTask *task) {
  return !task->queued ||
         vkr_atomic_bool_load(&task->job_exited, VKR_MEMORY_ORDER_ACQUIRE);
}

void script_tasks_collect(VkrScriptHost *host, bool8_t wait) {
  uint32_t kept = 0u;
  for (uint32_t i = 0; i < host->task_count; ++i) {
    ScriptTask *task = host->tasks[i];
    if (task->released && wait) {
      while (!script_task_freeable(task)) {
        vkr_thread_sleep(0u);
      }
    }
    if (task->released && script_task_freeable(task)) {
      vkr_allocator_free(&host->instance_allocator, task,
                         script_task_bytes(task),
                         VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
      continue;
    }
    host->tasks[kept++] = task;
  }
  host->task_count = kept;
}

void script_task_release(VkrScriptHost *host, uint64_t id) {
  ScriptTask *task = script_task_find(host, id);
  if (!task) {
    return;
  }
  script_task_finish(task);
  task->released = true_v;
  script_tasks_collect(host, false_v);
}

static VkrTask sdk_task_run(VkrCtx *ctx, VkrTaskFn fn, const void *data,
                            uint32_t size) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScriptHost *host = script->host;
  if (!fn || size > VKR_TASK_DATA_MAX || (size && !data)) {
    (void)script_ctx_error(script, "A task needs a function and at most "
                                   "VKR_TASK_DATA_MAX bytes of data");
    return (VkrTask){0};
  }
  if (host->task_count == host->task_capacity) {
    const uint32_t capacity = Max(16u, host->task_capacity * 2u);
    ScriptTask **tasks = vkr_allocator_alloc(&host->instance_allocator,
                                             sizeof(*tasks) * capacity,
                                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!tasks) {
      (void)script_ctx_error(script, "Task allocation failed");
      return (VkrTask){0};
    }
    if (host->tasks) {
      MemCopy(tasks, host->tasks, sizeof(*tasks) * host->task_count);
      vkr_allocator_free(&host->instance_allocator, host->tasks,
                         sizeof(*tasks) * host->task_capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    host->tasks = tasks;
    host->task_capacity = capacity;
  }
  ScriptTask *task =
      vkr_allocator_alloc(&host->instance_allocator, SCRIPT_TASK_HEADER + size,
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!task) {
    (void)script_ctx_error(script, "Task allocation failed");
    return (VkrTask){0};
  }
  *task = (ScriptTask){.id = ++host->task_serial, .fn = fn, .size = size};
  if (size) {
    MemCopy(script_task_data(task), data, size);
  }
  if (!script_ledger_record(script, SCRIPT_LEDGER_TASK, task->id, 0u)) {
    vkr_allocator_free(&host->instance_allocator, task, script_task_bytes(task),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    return (VkrTask){0};
  }
  host->tasks[host->task_count++] = task;

  VkrJobSystem *jobs = host->session.jobs;
  if (jobs) {
    Bitset8 type_mask = bitset8_create();
    bitset8_set(&type_mask, VKR_JOB_TYPE_GENERAL);
    const VkrJobDesc job = {.priority = VKR_JOB_PRIORITY_HIGH,
                            .type_mask = type_mask,
                            .run = script_task_job,
                            .payload = &task,
                            .payload_size = sizeof(task)};
    VkrJobHandle handle = {0};
    task->queued = vkr_job_try_submit(jobs, &job, &handle);
  }
  if (!task->queued) {
    /* No worker can take it: it runs now, before the call returns. */
    script_task_execute(task);
  }
  return (VkrTask){.id = task->id};
}

static bool8_t sdk_task_take(VkrCtx *ctx, VkrTask task_handle, void *out,
                             uint32_t size, bool8_t wait) {
  ScriptCtx *script = sdk_ctx(ctx);
  VkrScriptHost *host = script->host;
  ScriptTask *task = script_task_find(host, task_handle.id);
  if (!task) {
    return script_ctx_error(script, "Unknown or already taken task");
  }
  if (wait) {
    script_task_finish(task);
  } else if (!vkr_atomic_bool_load(&task->done, VKR_MEMORY_ORDER_ACQUIRE)) {
    return false_v;
  }
  if (out && size) {
    MemCopy(out, script_task_data(task), Min(size, task->size));
  }
  script_ledger_forget(script, SCRIPT_LEDGER_TASK, task->id, 0u);
  task->released = true_v;
  script_tasks_collect(host, false_v);
  return true_v;
}

// =============================================================================
// Replaying queued edits
// =============================================================================

/* Replays one queued call through its SDK entry, now outside the tick. */
static bool8_t sdk_replay(VkrScriptHost *host, const ScriptCommand *command) {
  VkrCtx *ctx = &command->instance->ctx.base;
  const VkrEntity entity = {.id = command->entity};
  switch ((ScriptCommandKind)command->kind) {
  case SCRIPT_COMMAND_SPAWN: {
    host->replay_reserved = command->entity;
    const VkrEntity spawned = sdk_spawn(ctx, &command->as.spawn);
    host->replay_reserved = 0u;
    return vkr_entity_valid(spawned);
  }
  case SCRIPT_COMMAND_DESTROY:
    sdk_destroy(ctx, entity);
    return true_v;
  case SCRIPT_COMMAND_SET_NAME:
    return sdk_set_name(ctx, entity, command->as.name);
  case SCRIPT_COMMAND_SET_PARENT:
    return sdk_set_parent(ctx, entity, (VkrEntity){.id = command->as.parent});
  case SCRIPT_COMMAND_SET_VISIBLE:
    return sdk_set_visible(ctx, entity, command->as.visible);
  case SCRIPT_COMMAND_SET_TRANSFORM:
    return sdk_set_transform(ctx, entity, &command->as.transform);
  case SCRIPT_COMMAND_SET_RENDER_POSE:
    return sdk_set_render_pose(
        ctx, entity, command->as.pose.clear ? NULL : &command->as.pose.world);
  case SCRIPT_COMMAND_SET_SHAPE:
    return sdk_set_shape(ctx, entity, &command->as.shape);
  case SCRIPT_COMMAND_SPAWN_MODEL:
    return sdk_spawn_model(ctx, entity, command->as.model.mesh,
                           command->as.model.animation);
  case SCRIPT_COMMAND_DESPAWN_MODEL:
    sdk_despawn_model(ctx, entity);
    return true_v;
  case SCRIPT_COMMAND_COMPONENT_SET:
    return sdk_component_set(ctx, entity, command->as.component.type,
                             command->as.component.value);
  case SCRIPT_COMMAND_STATE_ADD:
    return sdk_state_add(ctx, entity, command->as.state.type,
                         command->as.state.value) != NULL;
  case SCRIPT_COMMAND_STATE_REMOVE:
    sdk_state_remove(ctx, entity, command->as.state.type);
    return true_v;
  case SCRIPT_COMMAND_SET_BODY:
    return sdk_set_body(ctx, entity, &command->as.body);
  case SCRIPT_COMMAND_CHARACTER_CREATE:
    return sdk_character_create(
        ctx, entity, &command->as.character.desc,
        command->as.character.has_foot ? &command->as.character.foot : NULL);
  case SCRIPT_COMMAND_CHARACTER_DESTROY:
    sdk_character_destroy(ctx, entity);
    return true_v;
  }
  return false_v;
}

static void sdk_cancel_spawn(VkrScriptHost *host,
                             const ScriptCommand *command) {
  VkrScriptContainer *container = script_container_of(host, command->entity);
  if (container) {
    vkr_scene_cancel_reserved_entity(container->scene,
                                     (VkrEntityId){.u64 = command->entity});
  }
  sdk_pending_remove(host, command->entity);
}

void script_commands_flush(VkrScriptHost *host) {
  if (!host->command_count) {
    return;
  }
  const VkrScriptPhase phase = host->phase;
  host->phase = VKR_SCRIPT_PHASE_FRAME;
  /* Replay queues nothing, so the array stays put. */
  for (uint32_t i = 0; i < host->command_count; ++i) {
    const ScriptCommand *command = &host->commands[i];
    ScriptCtx *ctx = &command->instance->ctx;
    /* An ended scope keeps only destroys; its spawns never appear. */
    if (command->ledger->closed && command->kind != SCRIPT_COMMAND_DESTROY) {
      if (command->kind == SCRIPT_COMMAND_SPAWN) {
        sdk_cancel_spawn(host, command);
      }
      continue;
    }
    if (command->kind == SCRIPT_COMMAND_SPAWN) {
      sdk_pending_remove(host, command->entity);
    }
    ScriptLedger *saved = ctx->ledger;
    ctx->ledger = command->ledger;
    if (!sdk_replay(host, command)) {
      log_warn("%s: a call queued in a fixed update failed: %s",
               sdk_module_name(ctx), ctx->last_error);
    }
    ctx->ledger = saved;
  }
  host->command_count = 0u;
  arena_clear(host->command_arena, ARENA_MEMORY_TAG_UNKNOWN);
  host->phase = phase;
}

void script_commands_drop(VkrScriptHost *host,
                          const VkrScriptInstance *instance) {
  uint32_t kept = 0u;
  for (uint32_t i = 0; i < host->command_count; ++i) {
    const ScriptCommand command = host->commands[i];
    if (instance && command.instance != instance) {
      host->commands[kept++] = command;
      continue;
    }
    if (command.kind == SCRIPT_COMMAND_SPAWN) {
      sdk_cancel_spawn(host, &command);
    }
  }
  host->command_count = kept;
  if (!kept) {
    arena_clear(host->command_arena, ARENA_MEMORY_TAG_UNKNOWN);
  }
}

// =============================================================================
// Table
// =============================================================================

void script_sdk_table(VkrSdkTable *table) {
  *table = (VkrSdkTable){
      .log = sdk_log,
      .fail = sdk_fail,
      .last_error = sdk_last_error,
      .disable = sdk_disable,
      .temp_alloc = sdk_temp_alloc,
      .option = sdk_option,
      .container_self = sdk_container_self,
      .container_active = sdk_container_active,
      .container_world = sdk_container_world,
      .container_of = sdk_container_of,
      .spawn = sdk_spawn,
      .destroy = sdk_destroy,
      .alive = sdk_alive,
      .set_name = sdk_set_name,
      .set_parent = sdk_set_parent,
      .set_visible = sdk_set_visible,
      .visible = sdk_visible,
      .set_transform = sdk_set_transform,
      .world_matrix = sdk_world_matrix,
      .set_render_pose = sdk_set_render_pose,
      .has_visual = sdk_has_visual,
      .set_shape = sdk_set_shape,
      .spawn_model = sdk_spawn_model,
      .despawn_model = sdk_despawn_model,
      .model_state = sdk_model_state,
      .component_named = sdk_component_named,
      .component_get = sdk_component_get,
      .component_get_mut = sdk_component_get_mut,
      .component_set = sdk_component_set,
      .component_find = sdk_component_find,
      .state_type = sdk_state_type,
      .state_add = sdk_state_add,
      .state_get = sdk_state_get,
      .state_remove = sdk_state_remove,
      .player_start = sdk_player_start,
      .time = sdk_time,
      .fixed_dt = sdk_fixed_dt,
      .ticks = sdk_ticks,
      .sim_time = sdk_sim_time,
      .sim_debt = sdk_sim_debt,
      .simulating = sdk_simulating,
      .paused = sdk_paused,
      .playing = sdk_playing,
      .set_time_step = sdk_set_time_step,
      .gravity = sdk_gravity,
      .input_focused = sdk_input_focused,
      .key_down = sdk_key_down,
      .camera_available = sdk_camera_available,
      .set_camera = sdk_set_camera,
      .hud_text = sdk_hud_text,
      .body_default = sdk_body_default,
      .set_body = sdk_set_body,
      .impulse = sdk_impulse,
      .raycast = sdk_raycast,
      .sweep_sphere = sdk_sweep_sphere,
      .character_default = sdk_character_default,
      .character_create = sdk_character_create,
      .character_destroy = sdk_character_destroy,
      .character_state = sdk_character_state,
      .character_move = sdk_character_move,
      .anim_id = sdk_anim_id,
      .anim_has_graph = sdk_anim_has_graph,
      .anim_clip_count = sdk_anim_clip_count,
      .anim_clip_find = sdk_anim_clip_find,
      .anim_clip_duration = sdk_anim_clip_duration,
      .anim_bone_count = sdk_anim_bone_count,
      .anim_bone_pose = sdk_anim_bone_pose,
      .anim_play = sdk_anim_play,
      .anim_crossfade = sdk_anim_crossfade,
      .anim_set_playing = sdk_anim_set_playing,
      .anim_set_rate = sdk_anim_set_rate,
      .anim_rate = sdk_anim_rate,
      .task_run = sdk_task_run,
      .task_take = sdk_task_take,
      .anim_time = sdk_anim_time,
      .anim_duration = sdk_anim_duration,
      .anim_blend = sdk_anim_blend,
  };
}
