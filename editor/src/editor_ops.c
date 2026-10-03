#include "editor_ops.h"

#include "editor_agent.h"
#include "editor_internal.h"

#include "core/logger.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_types.h"
#include "vkr_bakery_buffer.h"

#include "stb_image_write.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#define OPS_MALFORMED "VKR-AGENT-0001"
#define OPS_UNKNOWN "VKR-AGENT-0002"
#define OPS_INVALID "VKR-AGENT-0003"
#define OPS_NOT_FOUND "VKR-AGENT-0004"
#define OPS_REJECTED "VKR-AGENT-0005"
#define OPS_BUSY "VKR-AGENT-0006"
#define OPS_CAPTURE "VKR-AGENT-0007"
#define OPS_LIMIT "VKR-AGENT-0008"

/* Entities scene.describe lists per request, and its default page. */
#define OPS_DESCRIBE_MAX 500u
#define OPS_DESCRIBE_DEFAULT 200u
/* Builds a batch or capture may wait before it reports a timeout. */
#define OPS_WAIT_FRAMES 600u
/* Capture PNGs kept in the private directory. */
#define OPS_CAPTURE_KEEP 32u
/* Builds a capture waits after changing the view, so the frame shows it. */
#define OPS_CAPTURE_SETTLE_FRAMES 4u

struct VkrEditorOps {
  VkrAllocator *allocator;
  /* Reused batch storage; the runtime borrows it until it dispatches. */
  VkrSampleEditBatchItem *items;
  uint64_t next_token;
  VkrEditorChange changes[VKR_EDITOR_CHANGE_MAX];
  uint32_t change_count;
  uint32_t next_change_id;
  uint32_t capture_serial;
};

typedef struct OpsContext {
  VkrEditorOps *ops;
  VkrEditorUi *editor;
  const VkrSampleUiFrame *frame;
  VkrEditorOpCall *call;
} OpsContext;

/* A batch under construction. `op_item` names each operation's primary item
   (the entity `$k` refers to) or UINT32_MAX; `op_target` names an existing
   entity an operation edited. */
typedef struct OpsBatch {
  uint32_t count;
  int32_t container;
  uint32_t op_count;
  uint32_t op_item[VKR_SAMPLE_EDIT_BATCH_MAX];
  VkrEntityId op_target[VKR_SAMPLE_EDIT_BATCH_MAX];
  String8 label;
  bool8_t review;
} OpsBatch;

/* An entity argument: an existing entity, or `item` of the batch. */
typedef struct OpsRef {
  VkrEntityId entity;
  int32_t item;
  uint16_t container;
} OpsRef;

typedef VkrEditorOpStatus (*OpsRun)(OpsContext *ctx);
typedef bool8_t (*OpsBuild)(OpsContext *ctx, const VkrBakeryJson *args,
                            OpsBatch *batch);

typedef struct OpsDef {
  const char *name;
  const char *description;
  /* JSON Schema of `args`. */
  const char *schema;
  /* Read operations and batches run; write operations build batch items. */
  OpsRun run;
  OpsBuild build;
} OpsDef;

// =============================================================================
// Failures and JSON values
// =============================================================================

static bool8_t ops_fail(OpsContext *ctx, const char *code, const char *format,
                        ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(ctx->call->error, sizeof(ctx->call->error), format, arguments);
  va_end(arguments);
  ctx->call->error_code = code;
  ctx->call->result = NULL;
  return false_v;
}

static VkrEditorOpStatus ops_done(OpsContext *ctx) {
  (void)ctx;
  return VKR_EDITOR_OP_DONE;
}

static Arena *ops_arena(OpsContext *ctx) { return ctx->call->arena; }

static VkrBakeryJson *ops_number(OpsContext *ctx, float64_t value) {
  return vkr_bakery_json_float(ops_arena(ctx), isfinite(value) ? value : 0.0);
}

static VkrBakeryJson *ops_vec3(OpsContext *ctx, Vec3 value) {
  VkrBakeryJson *array = vkr_bakery_json_array(ops_arena(ctx));
  vkr_bakery_json_append(array, ops_number(ctx, value.x));
  vkr_bakery_json_append(array, ops_number(ctx, value.y));
  vkr_bakery_json_append(array, ops_number(ctx, value.z));
  return array;
}

static VkrBakeryJson *ops_string(OpsContext *ctx, String8 text) {
  return vkr_bakery_json_string(ops_arena(ctx), text);
}

static void ops_set(OpsContext *ctx, VkrBakeryJson *object, const char *key,
                    VkrBakeryJson *value) {
  vkr_bakery_json_set(ops_arena(ctx), object, key, value);
}

static bool8_t ops_equals(String8 text, const char *word) {
  const uint64_t length = strlen(word);
  return text.length == length && MemCompare(text.str, word, length) == 0;
}

/* An optional three-number array; false with an error when malformed. */
static bool8_t ops_arg_vec3(OpsContext *ctx, const VkrBakeryJson *args,
                            const char *key, Vec3 *out, bool8_t *present) {
  const VkrBakeryJson *value = vkr_bakery_json_get(args, key);
  if (present) {
    *present = value != NULL;
  }
  if (!value) {
    return true_v;
  }
  float32_t parts[3] = {0};
  if (value->type != VKR_BAKERY_JSON_ARRAY || value->count != 3u) {
    return ops_fail(ctx, OPS_INVALID, "'%s' must be an array of 3 numbers",
                    key);
  }
  for (uint32_t i = 0; i < 3u; ++i) {
    const VkrBakeryJson *part = vkr_bakery_json_at(value, i);
    if (part->type == VKR_BAKERY_JSON_INT) {
      parts[i] = (float32_t)part->integer;
    } else if (part->type == VKR_BAKERY_JSON_FLOAT) {
      parts[i] = (float32_t)part->number;
    } else {
      return ops_fail(ctx, OPS_INVALID, "'%s' must be an array of 3 numbers",
                      key);
    }
    if (!isfinite(parts[i])) {
      return ops_fail(ctx, OPS_INVALID, "'%s' must be finite", key);
    }
  }
  *out = vec3_new(parts[0], parts[1], parts[2]);
  return true_v;
}

static bool8_t ops_arg_number(const VkrBakeryJson *args, const char *key,
                              float64_t *out) {
  const VkrBakeryJson *value = vkr_bakery_json_get(args, key);
  if (value && value->type == VKR_BAKERY_JSON_INT) {
    *out = (float64_t)value->integer;
    return true_v;
  }
  if (value && value->type == VKR_BAKERY_JSON_FLOAT) {
    *out = value->number;
    return true_v;
  }
  return false_v;
}

static bool8_t ops_arg_bool(const VkrBakeryJson *args, const char *key,
                            bool8_t fallback) {
  bool8_t value = fallback;
  return vkr_bakery_json_get_bool(args, key, &value) ? value : fallback;
}

// =============================================================================
// Containers and entities
// =============================================================================

static const VkrScene *ops_scene(const VkrSampleUiFrame *frame,
                                 uint32_t world) {
  if (world == VKR_SCENE_WORLD_ROOT_ID) {
    return frame->world;
  }
  if (world == 0u) {
    return frame->scene;
  }
  return world <= VKR_SCENE_ADDITIVE_MAX ? frame->additive[world - 1u] : NULL;
}

static const VkrSceneEditState *ops_journal(const VkrSampleUiFrame *frame,
                                            uint32_t world) {
  if (world == VKR_SCENE_WORLD_ROOT_ID) {
    return frame->world_edits;
  }
  if (world == 0u) {
    return frame->edits;
  }
  return world <= VKR_SCENE_ADDITIVE_MAX ? frame->additive_edits[world - 1u]
                                         : NULL;
}

static const char *ops_container_name(uint32_t world, char *buffer,
                                      uint32_t capacity) {
  if (world == VKR_SCENE_WORLD_ROOT_ID) {
    return "world";
  }
  if (world == 0u) {
    return "primary";
  }
  snprintf(buffer, capacity, "%u", world);
  return buffer;
}

/* `primary`, `world` or an added scene's slot (1 to 6); absent picks the
   primary scene, else the World. */
static bool8_t ops_arg_container(OpsContext *ctx, const VkrBakeryJson *args,
                                 uint16_t *out) {
  const VkrBakeryJson *value = vkr_bakery_json_get(args, "container");
  if (!value) {
    *out = ctx->frame->scene ? 0u : VKR_SCENE_WORLD_ROOT_ID;
  } else if (vkr_bakery_json_is_string(value, "primary")) {
    *out = 0u;
  } else if (vkr_bakery_json_is_string(value, "world")) {
    *out = VKR_SCENE_WORLD_ROOT_ID;
  } else if (value->type == VKR_BAKERY_JSON_INT && value->integer >= 1 &&
             value->integer <= VKR_SCENE_ADDITIVE_MAX) {
    *out = (uint16_t)value->integer;
  } else {
    return ops_fail(ctx, OPS_INVALID,
                    "'container' is \"primary\", \"world\" or a slot 1-%u",
                    VKR_SCENE_ADDITIVE_MAX);
  }
  if (!ops_scene(ctx->frame, *out)) {
    return ops_fail(ctx, OPS_BUSY, "That scene is not loaded");
  }
  return true_v;
}

static void ops_id_text(VkrEntityId entity, char *out, uint32_t capacity) {
  snprintf(out, capacity, "%u:%u:%u", (unsigned)entity.parts.world,
           (unsigned)entity.parts.index, (unsigned)entity.parts.generation);
}

static VkrBakeryJson *ops_id(OpsContext *ctx, VkrEntityId entity) {
  if (!entity.u64) {
    return vkr_bakery_json_null(ops_arena(ctx));
  }
  char text[40];
  ops_id_text(entity, text, sizeof(text));
  return vkr_bakery_json_cstr(ops_arena(ctx), text);
}

static bool8_t ops_parse_id(String8 text, VkrEntityId *out) {
  uint32_t parts[3] = {0};
  uint32_t part = 0u;
  bool8_t digit = false_v;
  for (uint64_t i = 0; i < text.length; ++i) {
    const uint8_t c = text.str[i];
    if (c >= '0' && c <= '9') {
      parts[part] = parts[part] * 10u + (uint32_t)(c - '0');
      digit = true_v;
      if (parts[part] > UINT32_MAX / 10u) {
        return false_v;
      }
    } else if (c == ':' && digit && part < 2u) {
      part++;
      digit = false_v;
    } else {
      return false_v;
    }
  }
  if (part != 2u || !digit || parts[0] > UINT16_MAX || parts[2] > UINT16_MAX) {
    return false_v;
  }
  out->parts.world = (uint16_t)parts[0];
  out->parts.index = parts[1];
  out->parts.generation = (uint16_t)parts[2];
  return true_v;
}

static const SceneTransform *ops_transform(const VkrScene *scene,
                                           VkrEntityId entity) {
  return vkr_entity_get_component(scene->world, entity, scene->comp_transform);
}

static VkrEntityId ops_parent(const VkrScene *scene, VkrEntityId entity) {
  const SceneTransform *transform = ops_transform(scene, entity);
  return transform ? transform->parent : VKR_ENTITY_ID_INVALID;
}

/* Searches the loaded containers for an alive entity named exactly `name`. */
static uint32_t ops_find_named(const VkrSampleUiFrame *frame, String8 name,
                               VkrEntityId *out) {
  uint32_t found = 0u;
  const uint32_t worlds[2u + VKR_SCENE_ADDITIVE_MAX] = {
      0u, VKR_SCENE_WORLD_ROOT_ID, 1u, 2u, 3u, 4u, 5u, 6u};
  for (uint32_t w = 0; w < ArrayCount(worlds); ++w) {
    const VkrScene *scene = ops_scene(frame, worlds[w]);
    if (!scene) {
      continue;
    }
    for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
      const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
      if (!vkr_scene_entity_alive(scene, entity)) {
        continue;
      }
      const String8 current = vkr_scene_get_name(scene, entity);
      if (current.length == name.length &&
          MemCompare(current.str, name.str, name.length) == 0) {
        if (!found) {
          *out = entity;
        }
        found++;
      }
    }
  }
  return found;
}

/* Resolves an entity argument: an id, `$k` within `batch`, or a unique
   exact name. */
static bool8_t ops_ref(OpsContext *ctx, const OpsBatch *batch,
                       const VkrBakeryJson *value, const char *key,
                       OpsRef *out) {
  *out = (OpsRef){.item = -1};
  String8 text = {0};
  if (!value || value->type != VKR_BAKERY_JSON_STRING ||
      !value->string.length) {
    return ops_fail(ctx, OPS_INVALID, "'%s' must name an entity", key);
  }
  text = value->string;
  if (text.str[0] == '$') {
    uint32_t index = 0u;
    for (uint64_t i = 1; i < text.length; ++i) {
      if (text.str[i] < '0' || text.str[i] > '9' || index > 100000u) {
        return ops_fail(ctx, OPS_INVALID, "'%s' has a malformed reference",
                        key);
      }
      index = index * 10u + (uint32_t)(text.str[i] - '0');
    }
    if (!batch || text.length < 2u || index >= batch->op_count ||
        batch->op_item[index] == UINT32_MAX) {
      return ops_fail(ctx, OPS_INVALID,
                      "'%s' refers to $%u, which created no entity before "
                      "this operation",
                      key, index);
    }
    out->item = (int32_t)batch->op_item[index];
    out->container = (uint16_t)batch->container;
    return true_v;
  }
  VkrEntityId entity = VKR_ENTITY_ID_INVALID;
  if (ops_parse_id(text, &entity)) {
    const VkrScene *scene = ops_scene(ctx->frame, entity.parts.world);
    if (!scene || !vkr_scene_entity_alive(scene, entity)) {
      return ops_fail(ctx, OPS_NOT_FOUND, "No entity %.*s is alive",
                      (int)text.length, text.str);
    }
  } else {
    const uint32_t found = ops_find_named(ctx->frame, text, &entity);
    if (found > 1u) {
      return ops_fail(ctx, OPS_NOT_FOUND,
                      "%u entities are named '%.*s'; use an id", found,
                      (int)text.length, text.str);
    }
    if (!found) {
      return ops_fail(ctx, OPS_NOT_FOUND, "No entity is named '%.*s'",
                      (int)text.length, text.str);
    }
  }
  out->entity = entity;
  out->container = entity.parts.world;
  return true_v;
}

static VkrBakeryJson *ops_entity(OpsContext *ctx, const VkrScene *scene,
                                 VkrEntityId entity) {
  VkrBakeryJson *object = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, object, "id", ops_id(ctx, entity));
  if (scene && entity.u64 && vkr_scene_entity_alive(scene, entity)) {
    ops_set(ctx, object, "name",
            ops_string(ctx, vkr_scene_get_name(scene, entity)));
  }
  return object;
}

/* World box of an entity's loaded meshes and shapes and its descendants'. */
static bool8_t ops_world_bounds(const VkrScene *scene, VkrEntityId entity,
                                Vec3 *out_min, Vec3 *out_max) {
  Vec3 lower = {0};
  Vec3 upper = {0};
  const SceneTransform *transform = ops_transform(scene, entity);
  if (!transform ||
      !vkr_scene_entity_local_bounds(scene, entity, &lower, &upper)) {
    return false_v;
  }
  Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  for (uint32_t corner = 0; corner < 8u; ++corner) {
    const Vec3 local = vec3_new((corner & 1u) ? upper.x : lower.x,
                                (corner & 2u) ? upper.y : lower.y,
                                (corner & 4u) ? upper.z : lower.z);
    const Vec3 world = mat4_mul_vec3(transform->world, local);
    lo = vec3_new(Min(lo.x, world.x), Min(lo.y, world.y), Min(lo.z, world.z));
    hi = vec3_new(Max(hi.x, world.x), Max(hi.y, world.y), Max(hi.z, world.z));
  }
  *out_min = lo;
  *out_max = hi;
  return true_v;
}

/* Component type named `name`: an edit component (transform, visibility,
   the lights, physics_body) with its journal field, else a world type. */
static const VkrTypeDesc *ops_type_named(String8 name, uint32_t *out_field) {
  const VkrTypeDesc *type = NULL;
  *out_field = 0u;
  for (uint32_t i = 0; (type = vkr_scene_edit_component_type(i)); ++i) {
    if (ops_equals(name, type->name)) {
      *out_field = vkr_scene_edit_component_field(type);
      return type;
    }
  }
  return vkr_scene_world_type_named(name);
}

// =============================================================================
// Component values
// =============================================================================

static bool8_t ops_buffer_sink(void *context, const uint8_t *data,
                               uint64_t length) {
  VkrBakeryBuffer *buffer = context;
  vkr_bakery_buffer_append(buffer, data, length);
  return !buffer->failed;
}

/* A component value as JSON, through its descriptor as documents write it. */
static VkrBakeryJson *ops_component_json(OpsContext *ctx,
                                         const VkrTypeDesc *type,
                                         const void *value) {
  VkrBakeryBuffer buffer = {0};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, ops_buffer_sink, &buffer);
  VkrBakeryJson *json = NULL;
  if (vkr_type_write_json(&writer, type, value) &&
      vkr_json_writer_complete(&writer) && !buffer.failed) {
    json = vkr_bakery_json_parse(ops_arena(ctx), buffer.data, buffer.length,
                                 32u, NULL);
  }
  vkr_bakery_buffer_free(&buffer);
  return json ? json : vkr_bakery_json_null(ops_arena(ctx));
}

/* Applies a JSON object of property values over `value` through the
   descriptor; members it lacks keep their current values. */
static bool8_t ops_component_read(OpsContext *ctx, const VkrTypeDesc *type,
                                  const VkrBakeryJson *values, void *value) {
  if (!values) {
    return true_v;
  }
  if (values->type != VKR_BAKERY_JSON_OBJECT) {
    return ops_fail(ctx, OPS_INVALID, "'values' must be an object");
  }
  String8 text = {0};
  if (!vkr_bakery_json_write(ops_arena(ctx), values, VKR_BAKERY_JSON_COMPACT,
                             &text)) {
    return ops_fail(ctx, OPS_INVALID, "'values' cannot be serialized");
  }
  VkrAllocator scratch = {.ctx = ops_arena(ctx)};
  vkr_allocator_arena(&scratch);
  char error[192] = {0};
  if (!vkr_type_read_json_document(text, type, value, &scratch, error,
                                   sizeof(error))) {
    return ops_fail(ctx, OPS_INVALID, "%s: %s", type->name,
                    error[0] ? error : "invalid values");
  }
  return true_v;
}

/* Current bytes of `type` on an existing entity; false when it lacks one. */
static bool8_t ops_component_get(const VkrScene *scene, VkrEntityId entity,
                                 const VkrTypeDesc *type, uint32_t field,
                                 void *out) {
  if (field) {
    VkrSceneEditValues values;
    return vkr_scene_edit_read(scene, entity, &values) &&
           vkr_scene_edit_component_get(&values, type, out);
  }
  const void *current = vkr_scene_get_typed(scene, entity, type);
  if (!current) {
    return false_v;
  }
  MemCopy(out, current, type->size);
  return true_v;
}

// =============================================================================
// Batches
// =============================================================================

static VkrSampleEditBatchItem *ops_batch_add(OpsContext *ctx, OpsBatch *batch,
                                             uint16_t container,
                                             VkrSceneEditAction action) {
  if (batch->count >= VKR_SAMPLE_EDIT_BATCH_MAX) {
    ops_fail(ctx, OPS_LIMIT, "A batch holds at most %u edits",
             VKR_SAMPLE_EDIT_BATCH_MAX);
    return NULL;
  }
  if (batch->container >= 0 && (uint16_t)batch->container != container) {
    ops_fail(ctx, OPS_INVALID,
             "A batch edits one scene; this operation "
             "targets another one");
    return NULL;
  }
  batch->container = container;
  VkrSampleEditBatchItem *item = &ctx->ops->items[batch->count++];
  MemZero(item, sizeof(*item));
  item->request.action = action;
  item->request.container = container;
  item->entity_ref = -1;
  item->parent_ref = -1;
  return item;
}

static void ops_item_target(VkrSampleEditBatchItem *item, const OpsRef *ref) {
  item->request.entity = ref->entity;
  item->entity_ref = ref->item;
}

/* The values an entity has now, or the values its batch creation sets. */
static bool8_t ops_current_values(OpsContext *ctx, const OpsRef *ref,
                                  VkrSceneEditValues *out) {
  if (ref->item >= 0) {
    *out = ctx->ops->items[ref->item].request.values;
    return true_v;
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref->container);
  if (!scene || !vkr_scene_edit_read(scene, ref->entity, out)) {
    return ops_fail(ctx, OPS_NOT_FOUND, "That entity cannot be read");
  }
  return true_v;
}

static bool8_t ops_validate_values(OpsContext *ctx,
                                   const VkrSceneEditValues *values) {
  if ((values->fields & VKR_SCENE_EDIT_COMPONENT) && values->component_type) {
    char error[160] = {0};
    if (!vkr_type_validate(values->component_type, values->component, error,
                           sizeof(error))) {
      return ops_fail(ctx, OPS_INVALID, "%s: %s", values->component_type->name,
                      error);
    }
  }
  if (!vkr_scene_edit_validate(values)) {
    return ops_fail(ctx, OPS_INVALID, "Invalid transform or values");
  }
  return true_v;
}

/* Reads name, position, rotation (degrees) and scale over `values`. */
static bool8_t ops_read_pose(OpsContext *ctx, const VkrBakeryJson *args,
                             VkrSceneEditValues *values) {
  String8 name = {0};
  if (vkr_bakery_json_get_string(args, "name", &name)) {
    if (!name.length || name.length >= sizeof(values->name)) {
      return ops_fail(ctx, OPS_INVALID, "'name' must be 1-%u bytes",
                      (unsigned)sizeof(values->name) - 1u);
    }
    MemCopy(values->name, name.str, name.length);
    values->name[name.length] = '\0';
    values->fields |= VKR_SCENE_EDIT_NAME;
  }
  bool8_t has_position = false_v;
  bool8_t has_rotation = false_v;
  bool8_t has_scale = false_v;
  Vec3 degrees = {0};
  if (!ops_arg_vec3(ctx, args, "position", &values->position, &has_position) ||
      !ops_arg_vec3(ctx, args, "rotation", &degrees, &has_rotation) ||
      !ops_arg_vec3(ctx, args, "scale", &values->scale, &has_scale)) {
    return false_v;
  }
  if (has_rotation) {
    const float32_t euler[3] = {degrees.x, degrees.y, degrees.z};
    values->rotation = vkr_property_quat_from_euler(euler);
  }
  if (has_position || has_rotation || has_scale) {
    values->fields |= VKR_SCENE_EDIT_TRANSFORM;
  }
  return true_v;
}

/* entity.create: a new entity with a pose and optionally one component. */
static bool8_t ops_build_create(OpsContext *ctx, const VkrBakeryJson *args,
                                OpsBatch *batch) {
  OpsRef parent = {.item = -1};
  uint16_t container = 0u;
  const VkrBakeryJson *parent_value = vkr_bakery_json_get(args, "parent");
  if (parent_value && parent_value->type != VKR_BAKERY_JSON_NULL) {
    if (!ops_ref(ctx, batch, parent_value, "parent", &parent)) {
      return false_v;
    }
    container = parent.container;
  } else if (!ops_arg_container(ctx, args, &container)) {
    return false_v;
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, container, VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  item->request.parent = parent.entity;
  item->parent_ref = parent.item;
  VkrSceneEditValues *values = &item->request.values;
  values->fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM;
  snprintf(values->name, sizeof(values->name), "Object");
  values->rotation = vkr_quat_identity();
  values->scale = vec3_one();
  if (!ops_read_pose(ctx, args, values)) {
    return false_v;
  }
  const VkrBakeryJson *component = vkr_bakery_json_get(args, "component");
  if (component) {
    String8 type_name = {0};
    if (component->type != VKR_BAKERY_JSON_OBJECT ||
        !vkr_bakery_json_get_string(component, "type", &type_name)) {
      return ops_fail(ctx, OPS_INVALID,
                      "'component' is {\"type\": <name>, \"values\": {...}}");
    }
    uint32_t field = 0u;
    const VkrTypeDesc *type = ops_type_named(type_name, &field);
    if (!type || (!field && !vkr_scene_world_type_live(type)) ||
        field == VKR_SCENE_EDIT_TRANSFORM || field == VKR_SCENE_EDIT_PHYSICS) {
      return ops_fail(ctx, OPS_INVALID,
                      "'%.*s' cannot be created this way; use a world "
                      "component type or a light",
                      (int)type_name.length, type_name.str);
    }
    _Alignas(16) uint8_t bytes[VKR_TYPE_VALUE_MAX];
    vkr_type_defaults(type, bytes);
    if (!ops_component_read(ctx, type, vkr_bakery_json_get(component, "values"),
                            bytes)) {
      return false_v;
    }
    if (field) {
      (void)vkr_scene_edit_component_set(values, type, bytes);
      values->fields |= field;
    } else {
      values->component_type = type;
      MemCopy(values->component, bytes, type->size);
      values->fields |= VKR_SCENE_EDIT_COMPONENT;
    }
  }
  batch->op_item[batch->op_count] = batch->count - 1u;
  return ops_validate_values(ctx, values);
}

/* entity.set: name, pose or visibility of one entity. A batch creation takes
   the change into its own values. */
static bool8_t ops_build_set(OpsContext *ctx, const VkrBakeryJson *args,
                             OpsBatch *batch) {
  OpsRef ref;
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "entity"), "entity",
               &ref)) {
    return false_v;
  }
  VkrSceneEditValues values;
  if (!ops_current_values(ctx, &ref, &values)) {
    return false_v;
  }
  const uint32_t created_fields = values.fields;
  values.fields = 0u;
  if (!ops_read_pose(ctx, args, &values)) {
    return false_v;
  }
  bool8_t visible = false_v;
  if (vkr_bakery_json_get_bool(args, "visible", &visible)) {
    values.visibility.visible = visible;
    values.visibility.inherit_parent = true_v;
    values.fields |= VKR_SCENE_EDIT_VISIBILITY;
  }
  if (!values.fields) {
    return ops_fail(ctx, OPS_INVALID,
                    "entity.set needs name, position, rotation, scale or "
                    "visible");
  }
  if (!ops_validate_values(ctx, &values)) {
    return false_v;
  }
  if (ref.item >= 0) {
    values.fields |= created_fields;
    ctx->ops->items[ref.item].request.values = values;
    batch->op_item[batch->op_count] = (uint32_t)ref.item;
    return true_v;
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, ref.container, VKR_SCENE_EDIT_APPLY);
  if (!item) {
    return false_v;
  }
  ops_item_target(item, &ref);
  item->request.values = values;
  batch->op_target[batch->op_count] = ref.entity;
  return true_v;
}

/* Appends deletes for `entity` and, with `recursive`, its descendants first. */
static bool8_t ops_delete_tree(OpsContext *ctx, OpsBatch *batch,
                               const VkrScene *scene, VkrEntityId entity,
                               bool8_t recursive, uint32_t depth) {
  if (depth > 64u) {
    return ops_fail(ctx, OPS_LIMIT, "The hierarchy is too deep to delete");
  }
  for (uint32_t i = 0; recursive && i < scene->world->dir.living; ++i) {
    const VkrEntityId child = vkr_entity_id_from_index(scene->world, i);
    if (vkr_scene_entity_alive(scene, child) &&
        ops_parent(scene, child).u64 == entity.u64 &&
        !ops_delete_tree(ctx, batch, scene, child, true_v, depth + 1u)) {
      return false_v;
    }
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, entity.parts.world, VKR_SCENE_EDIT_DELETE);
  if (!item) {
    return false_v;
  }
  item->request.entity = entity;
  return true_v;
}

static bool8_t ops_build_delete(OpsContext *ctx, const VkrBakeryJson *args,
                                OpsBatch *batch) {
  OpsRef ref;
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "entity"), "entity",
               &ref)) {
    return false_v;
  }
  if (ref.item >= 0) {
    return ops_fail(ctx, OPS_INVALID,
                    "Delete an entity in a later batch than its creation");
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  const bool8_t recursive = ops_arg_bool(args, "recursive", false_v);
  const char *reason = NULL;
  if (!recursive && !vkr_scene_edit_can_delete(scene, ref.entity, &reason)) {
    return ops_fail(ctx, OPS_REJECTED, "%s%s",
                    reason ? reason : "It cannot be deleted",
                    "; pass \"recursive\": true to delete its children too");
  }
  batch->op_target[batch->op_count] = ref.entity;
  return ops_delete_tree(ctx, batch, scene, ref.entity, recursive, 0u);
}

static bool8_t ops_build_parent(OpsContext *ctx, const VkrBakeryJson *args,
                                OpsBatch *batch) {
  OpsRef ref;
  OpsRef parent = {.item = -1};
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "entity"), "entity",
               &ref)) {
    return false_v;
  }
  const VkrBakeryJson *parent_value = vkr_bakery_json_get(args, "parent");
  if (parent_value && parent_value->type != VKR_BAKERY_JSON_NULL) {
    if (!ops_ref(ctx, batch, parent_value, "parent", &parent)) {
      return false_v;
    }
    if (parent.container != ref.container) {
      return ops_fail(ctx, OPS_INVALID, "A parent must be in the same scene");
    }
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, ref.container, VKR_SCENE_EDIT_REPARENT);
  if (!item) {
    return false_v;
  }
  ops_item_target(item, &ref);
  item->request.parent = parent.entity;
  item->parent_ref = parent.item;
  batch->op_target[batch->op_count] = ref.entity;
  if (ref.item >= 0) {
    batch->op_item[batch->op_count] = (uint32_t)ref.item;
  }
  return true_v;
}

/* component.add, component.set and component.remove. */
static bool8_t ops_build_component(OpsContext *ctx, const VkrBakeryJson *args,
                                   OpsBatch *batch, VkrSceneEditAction action) {
  OpsRef ref;
  String8 type_name = {0};
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "entity"), "entity",
               &ref)) {
    return false_v;
  }
  if (!vkr_bakery_json_get_string(args, "type", &type_name)) {
    return ops_fail(ctx, OPS_INVALID, "'type' must name a component type");
  }
  uint32_t field = 0u;
  const VkrTypeDesc *type = ops_type_named(type_name, &field);
  if (!type) {
    return ops_fail(ctx, OPS_NOT_FOUND, "No component type '%.*s'",
                    (int)type_name.length, type_name.str);
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  if (action != VKR_SCENE_EDIT_APPLY &&
      (field || !vkr_scene_world_type_live(type) ||
       (scene && !vkr_scene_type_allowed(scene, type)))) {
    return ops_fail(ctx, OPS_INVALID,
                    "'%s' cannot be added or removed here; lights and bodies "
                    "use entity.create or the cmd operation",
                    type->name);
  }
  _Alignas(16) uint8_t bytes[VKR_TYPE_VALUE_MAX];
  if (action == VKR_SCENE_EDIT_APPLY) {
    VkrSceneEditValues values;
    if (!ops_current_values(ctx, &ref, &values)) {
      return false_v;
    }
    bool8_t present = false_v;
    if (ref.item >= 0) {
      present = field ? vkr_scene_edit_component_get(&values, type, bytes)
                      : values.component_type == type;
      if (present && !field) {
        MemCopy(bytes, values.component, type->size);
      }
    } else {
      present = ops_component_get(scene, ref.entity, type, field, bytes);
    }
    if (!present) {
      return ops_fail(ctx, OPS_NOT_FOUND, "The entity has no %s", type->name);
    }
    if (!ops_component_read(ctx, type, vkr_bakery_json_get(args, "values"),
                            bytes)) {
      return false_v;
    }
    const uint32_t created_fields = values.fields;
    if (field) {
      (void)vkr_scene_edit_component_set(&values, type, bytes);
      values.fields = field;
    } else {
      values.component_type = type;
      MemCopy(values.component, bytes, type->size);
      values.fields = VKR_SCENE_EDIT_COMPONENT;
    }
    if (!ops_validate_values(ctx, &values)) {
      return false_v;
    }
    if (ref.item >= 0) {
      values.fields |= created_fields;
      ctx->ops->items[ref.item].request.values = values;
      batch->op_item[batch->op_count] = (uint32_t)ref.item;
      return true_v;
    }
    VkrSampleEditBatchItem *item =
        ops_batch_add(ctx, batch, ref.container, VKR_SCENE_EDIT_APPLY);
    if (!item) {
      return false_v;
    }
    ops_item_target(item, &ref);
    item->request.values = values;
    batch->op_target[batch->op_count] = ref.entity;
    return true_v;
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, ref.container, action);
  if (!item) {
    return false_v;
  }
  ops_item_target(item, &ref);
  item->request.values.component_type = type;
  if (action == VKR_SCENE_EDIT_ADD_COMPONENT) {
    vkr_type_defaults(type, item->request.values.component);
    if (!ops_component_read(ctx, type, vkr_bakery_json_get(args, "values"),
                            item->request.values.component)) {
      return false_v;
    }
    char error[160] = {0};
    if (!vkr_type_validate(type, item->request.values.component, error,
                           sizeof(error))) {
      return ops_fail(ctx, OPS_INVALID, "%s: %s", type->name, error);
    }
  }
  batch->op_target[batch->op_count] = ref.entity;
  if (ref.item >= 0) {
    batch->op_item[batch->op_count] = (uint32_t)ref.item;
  }
  return true_v;
}

static bool8_t ops_build_component_add(OpsContext *ctx,
                                       const VkrBakeryJson *args,
                                       OpsBatch *batch) {
  return ops_build_component(ctx, args, batch, VKR_SCENE_EDIT_ADD_COMPONENT);
}

static bool8_t ops_build_component_set(OpsContext *ctx,
                                       const VkrBakeryJson *args,
                                       OpsBatch *batch) {
  return ops_build_component(ctx, args, batch, VKR_SCENE_EDIT_APPLY);
}

static bool8_t ops_build_component_remove(OpsContext *ctx,
                                          const VkrBakeryJson *args,
                                          OpsBatch *batch) {
  return ops_build_component(ctx, args, batch, VKR_SCENE_EDIT_REMOVE_COMPONENT);
}

// =============================================================================
// Changes
// =============================================================================

static void ops_change_touch(VkrEditorChange *change, VkrEntityId entity) {
  if (!entity.u64) {
    return;
  }
  for (uint32_t i = 0; i < change->entity_count; ++i) {
    if (change->entities[i].u64 == entity.u64) {
      return;
    }
  }
  if (change->entity_count < VKR_EDITOR_CHANGE_ENTITY_MAX) {
    change->entities[change->entity_count++] = entity;
  }
}

static void ops_change_remove(VkrEditorOps *ops, uint32_t index) {
  MemCopy(&ops->changes[index], &ops->changes[index + 1u],
          (ops->change_count - index - 1u) * sizeof(ops->changes[0]));
  ops->change_count--;
}

static int32_t ops_change_find(const VkrEditorOps *ops, uint32_t id) {
  for (uint32_t i = 0; i < ops->change_count; ++i) {
    if (ops->changes[i].id == id) {
      return (int32_t)i;
    }
  }
  return -1;
}

static VkrBakeryJson *ops_change_json(OpsContext *ctx,
                                      const VkrEditorChange *change) {
  char slot[16];
  VkrBakeryJson *object = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, object, "change",
          vkr_bakery_json_int(ops_arena(ctx), change->id));
  ops_set(ctx, object, "label",
          vkr_bakery_json_cstr(ops_arena(ctx), change->label));
  ops_set(ctx, object, "container",
          vkr_bakery_json_cstr(
              ops_arena(ctx),
              ops_container_name(change->container, slot, sizeof(slot))));
  VkrBakeryJson *entities = vkr_bakery_json_array(ops_arena(ctx));
  const VkrScene *scene = ops_scene(ctx->frame, change->container);
  for (uint32_t i = 0; i < change->entity_count; ++i) {
    vkr_bakery_json_append(entities,
                           ops_entity(ctx, scene, change->entities[i]));
  }
  ops_set(ctx, object, "entities", entities);
  return object;
}

// =============================================================================
// Batch submission
// =============================================================================

typedef struct OpsPendingBatch {
  OpsBatch batch;
  uint64_t token;
} OpsPendingBatch;

/* Builds one write operation into `batch`. */
static bool8_t ops_build_one(OpsContext *ctx, const OpsDef *def,
                             const VkrBakeryJson *args, OpsBatch *batch) {
  if (batch->op_count >= VKR_SAMPLE_EDIT_BATCH_MAX) {
    return ops_fail(ctx, OPS_LIMIT, "A batch holds at most %u operations",
                    VKR_SAMPLE_EDIT_BATCH_MAX);
  }
  batch->op_item[batch->op_count] = UINT32_MAX;
  batch->op_target[batch->op_count] = VKR_ENTITY_ID_INVALID;
  if (!def->build(ctx, args, batch)) {
    return false_v;
  }
  batch->op_count++;
  return true_v;
}

/* Hands the built batch to the runtime and waits for its result. */
static VkrEditorOpStatus ops_submit(OpsContext *ctx, OpsBatch *batch,
                                    bool8_t dry_run) {
  VkrEditorOpCall *call = ctx->call;
  if (!batch->count) {
    ops_fail(ctx, OPS_INVALID, "The batch holds no edits");
    return VKR_EDITOR_OP_DONE;
  }
  if (dry_run) {
    call->result = vkr_bakery_json_object(call->arena);
    ops_set(ctx, call->result, "dry_run", vkr_bakery_json_bool(call->arena, 1));
    ops_set(ctx, call->result, "edits",
            vkr_bakery_json_int(call->arena, batch->count));
    return VKR_EDITOR_OP_DONE;
  }
  if (ctx->frame->simulation_running) {
    ops_fail(ctx, OPS_BUSY, "Stop the simulation before editing the scene");
    return VKR_EDITOR_OP_DONE;
  }
  if (!ctx->frame->edit_batch || ctx->frame->edit_batch->token) {
    ops_fail(ctx, OPS_BUSY, "Another batch is in flight");
    return VKR_EDITOR_OP_DONE;
  }
  OpsPendingBatch *pending =
      arena_alloc(call->arena, sizeof(*pending), ARENA_MEMORY_TAG_STRUCT);
  if (!pending) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  pending->batch = *batch;
  pending->token = ++ctx->ops->next_token;
  *ctx->frame->edit_batch = (VkrSampleEditBatchRequest){
      .token = pending->token,
      .items = ctx->ops->items,
      .count = batch->count,
      .container = (uint16_t)batch->container,
  };
  call->state = pending;
  call->stage = 1u;
  call->frames = 0u;
  return VKR_EDITOR_OP_WAIT;
}

/* Reads the runtime's batch result and answers with each operation's entity;
   a reviewed batch becomes a pending change. */
static VkrEditorOpStatus ops_batch_wait(OpsContext *ctx) {
  VkrEditorOpCall *call = ctx->call;
  const OpsPendingBatch *pending = call->state;
  const VkrSampleEditBatchResult *result = ctx->frame->edit_batch_result;
  if (!result || result->token != pending->token) {
    if (++call->frames > OPS_WAIT_FRAMES) {
      ops_fail(ctx, OPS_BUSY, "The batch was not applied");
      return VKR_EDITOR_OP_DONE;
    }
    return VKR_EDITOR_OP_WAIT;
  }
  const OpsBatch *batch = &pending->batch;
  if (!result->ok) {
    if (result->failed_index != UINT32_MAX) {
      ops_fail(ctx, OPS_REJECTED,
               "Edit %u failed, so the batch rolled back: %s",
               result->failed_index, result->message);
    } else {
      ops_fail(ctx, OPS_REJECTED, "%s", result->message);
    }
    return VKR_EDITOR_OP_DONE;
  }
  const VkrScene *scene = ops_scene(ctx->frame, (uint32_t)batch->container);
  call->result = vkr_bakery_json_object(call->arena);
  VkrBakeryJson *results = vkr_bakery_json_array(call->arena);
  VkrEditorChange change = {.group = result->group,
                            .container = (uint16_t)batch->container};
  for (uint32_t op = 0; op < batch->op_count; ++op) {
    VkrEntityId entity = batch->op_target[op];
    if (batch->op_item[op] != UINT32_MAX) {
      entity = result->created[batch->op_item[op]];
    }
    VkrBakeryJson *entry = vkr_bakery_json_object(call->arena);
    ops_set(ctx, entry, "entity", ops_entity(ctx, scene, entity));
    vkr_bakery_json_append(results, entry);
    ops_change_touch(&change, entity);
  }
  for (uint32_t i = 0; i < batch->count; ++i) {
    ops_change_touch(&change, result->created[i]);
  }
  ops_set(ctx, call->result, "results", results);
  VkrEditorOps *ops = ctx->ops;
  if (batch->review && ops->change_count < VKR_EDITOR_CHANGE_MAX) {
    change.id = ++ops->next_change_id;
    snprintf(change.label, sizeof(change.label), "%.*s",
             (int)Min(batch->label.length, (uint64_t)95u),
             batch->label.length ? (const char *)batch->label.str : "");
    if (!change.label[0]) {
      snprintf(change.label, sizeof(change.label), "Agent edit %u", change.id);
    }
    ops->changes[ops->change_count++] = change;
    ops_set(ctx, call->result, "change",
            vkr_bakery_json_int(call->arena, change.id));
    char toast[160];
    snprintf(toast, sizeof(toast), "Agent change to review: %s", change.label);
    vkr_editor_toast(ctx->editor, VKR_UI_ICON_TERMINAL,
                     vkr_ui_theme()->accent_hover, toast);
  } else if (batch->review) {
    ops_set(ctx, call->result, "warning",
            vkr_bakery_json_cstr(call->arena,
                                 "Too many pending changes; this one applied "
                                 "without review"));
  }
  return VKR_EDITOR_OP_DONE;
}

// =============================================================================
// Operations
// =============================================================================

static const OpsDef *ops_find(String8 name);

static VkrEditorOpStatus ops_run_batch(OpsContext *ctx) {
  VkrEditorOpCall *call = ctx->call;
  if (call->stage) {
    return ops_batch_wait(ctx);
  }
  const VkrBakeryJson *list = vkr_bakery_json_get(call->args, "ops");
  if (!list || list->type != VKR_BAKERY_JSON_ARRAY || !list->count) {
    ops_fail(ctx, OPS_INVALID, "'ops' must be a non-empty array");
    return VKR_EDITOR_OP_DONE;
  }
  OpsBatch *batch =
      arena_alloc(call->arena, sizeof(*batch), ARENA_MEMORY_TAG_STRUCT);
  if (!batch) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  MemZero(batch, sizeof(*batch));
  batch->container = -1;
  batch->review = ops_arg_bool(call->args, "review", true_v);
  (void)vkr_bakery_json_get_string(call->args, "label", &batch->label);
  uint32_t index = 0u;
  for (const VkrBakeryJson *entry = list->first; entry;
       entry = entry->next, ++index) {
    String8 name = {0};
    const VkrBakeryJson *args = entry->type == VKR_BAKERY_JSON_OBJECT
                                    ? vkr_bakery_json_get(entry, "args")
                                    : NULL;
    if (entry->type != VKR_BAKERY_JSON_OBJECT ||
        !vkr_bakery_json_get_string(entry, "op", &name) ||
        (args && args->type != VKR_BAKERY_JSON_OBJECT)) {
      ops_fail(ctx, OPS_INVALID,
               "ops[%u] must be {\"op\": <name>, \"args\": {...}}", index);
      return VKR_EDITOR_OP_DONE;
    }
    const OpsDef *def = ops_find(name);
    if (!def || !def->build) {
      ops_fail(ctx, def ? OPS_INVALID : OPS_UNKNOWN,
               def ? "ops[%u]: '%.*s' cannot join a batch"
                   : "ops[%u]: unknown operation '%.*s'",
               index, (int)name.length, name.str);
      return VKR_EDITOR_OP_DONE;
    }
    if (!ops_build_one(ctx, def,
                       args ? args : vkr_bakery_json_object(call->arena),
                       batch)) {
      char message[sizeof(call->error)];
      snprintf(message, sizeof(message), "ops[%u] %.*s: %s", index,
               (int)name.length, name.str, call->error);
      snprintf(call->error, sizeof(call->error), "%s", message);
      return VKR_EDITOR_OP_DONE;
    }
  }
  return ops_submit(ctx, batch, ops_arg_bool(call->args, "dry_run", false_v));
}

static VkrEditorOpStatus ops_run_list(OpsContext *ctx);

static VkrEditorOpStatus ops_run_status(OpsContext *ctx) {
  const VkrSampleUiFrame *frame = ctx->frame;
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  const uint32_t worlds[2] = {0u, VKR_SCENE_WORLD_ROOT_ID};
  const char *keys[2] = {"scene", "world"};
  for (uint32_t i = 0; i < 2u; ++i) {
    const VkrScene *scene = ops_scene(frame, worlds[i]);
    const VkrSceneEditState *edits = ops_journal(frame, worlds[i]);
    VkrBakeryJson *container = vkr_bakery_json_object(arena);
    ops_set(ctx, container, "loaded",
            vkr_bakery_json_bool(arena, scene != NULL));
    if (scene) {
      uint32_t alive = 0u;
      for (uint32_t e = 0; e < scene->world->dir.living; ++e) {
        alive += vkr_scene_entity_alive(
            scene, vkr_entity_id_from_index(scene->world, e));
      }
      ops_set(ctx, container, "entities", vkr_bakery_json_int(arena, alive));
      ops_set(ctx, container, "unsaved",
              vkr_bakery_json_bool(arena, edits && edits->revision !=
                                                       edits->saved_revision));
    }
    ops_set(ctx, result, keys[i], container);
  }
  ops_set(ctx, result, "scene_path", ops_string(ctx, frame->scene_path));
  ops_set(ctx, result, "loading",
          vkr_bakery_json_bool(arena, frame->scene_loading ||
                                          frame->additive_loading ||
                                          frame->world_loading));
  VkrBakeryJson *additive = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (frame->additive[i]) {
      VkrBakeryJson *entry = vkr_bakery_json_object(arena);
      ops_set(ctx, entry, "slot", vkr_bakery_json_int(arena, i + 1u));
      ops_set(ctx, entry, "path", ops_string(ctx, frame->additive_names[i]));
      vkr_bakery_json_append(additive, entry);
    }
  }
  ops_set(ctx, result, "additive", additive);
  ops_set(ctx, result, "selection",
          frame->selected_entity.u64
              ? ops_entity(
                    ctx, vkr_editor_entity_scene(frame, frame->selected_entity),
                    frame->selected_entity)
              : vkr_bakery_json_null(arena));
  VkrBakeryJson *simulation = vkr_bakery_json_object(arena);
  ops_set(ctx, simulation, "running",
          vkr_bakery_json_bool(arena, frame->simulation_running));
  ops_set(ctx, simulation, "time", ops_number(ctx, frame->simulation_time));
  ops_set(ctx, result, "simulation", simulation);
  ops_set(ctx, result, "pending_changes",
          vkr_bakery_json_int(arena, ctx->ops->change_count));
  VkrBakeryJson *view = vkr_bakery_json_object(arena);
  ops_set(
      ctx, view, "camera",
      vkr_bakery_json_cstr(
          arena, vkr_editor_cmd_camera_views[frame->view_state.camera_view]));
  ops_set(ctx, view, "grid_spacing",
          ops_number(ctx, frame->view_state.grid_spacing));
  ops_set(ctx, view, "grid_height",
          ops_number(ctx, frame->view_state.grid_height));
  ops_set(ctx, result, "view", view);
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

/* The component type names an entity carries. */
static VkrBakeryJson *ops_component_names(OpsContext *ctx,
                                          const VkrScene *scene,
                                          VkrEntityId entity,
                                          const VkrSceneEditValues *values) {
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *names = vkr_bakery_json_array(arena);
  const VkrTypeDesc *type = NULL;
  _Alignas(16) uint8_t bytes[VKR_TYPE_VALUE_MAX];
  for (uint32_t i = 0; (type = vkr_scene_edit_component_type(i)); ++i) {
    if (type != &vkr_scene_transform_type &&
        type != &vkr_scene_visibility_type &&
        vkr_scene_edit_component_get(values, type, bytes)) {
      vkr_bakery_json_append(names, vkr_bakery_json_cstr(arena, type->name));
    }
  }
  for (uint32_t i = 0; (type = vkr_scene_world_type(i)); ++i) {
    if (vkr_scene_get_typed(scene, entity, type)) {
      vkr_bakery_json_append(names, vkr_bakery_json_cstr(arena, type->name));
    }
  }
  return names;
}

static bool8_t ops_in_box(Vec3 lo, Vec3 hi, Vec3 box_min, Vec3 box_max) {
  return lo.x <= box_max.x && hi.x >= box_min.x && lo.y <= box_max.y &&
         hi.y >= box_min.y && lo.z <= box_max.z && hi.z >= box_min.z;
}

static bool8_t ops_under(const VkrScene *scene, VkrEntityId entity,
                         VkrEntityId root) {
  for (uint32_t depth = 0; entity.u64 && depth < 256u; ++depth) {
    if (entity.u64 == root.u64) {
      return true_v;
    }
    entity = ops_parent(scene, entity);
  }
  return false_v;
}

static VkrEditorOpStatus ops_run_describe(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  Arena *arena = ops_arena(ctx);
  uint16_t container = 0u;
  OpsRef root = {.item = -1};
  const VkrBakeryJson *root_value = vkr_bakery_json_get(args, "root");
  if (root_value) {
    if (!ops_ref(ctx, NULL, root_value, "root", &root)) {
      return VKR_EDITOR_OP_DONE;
    }
    container = root.container;
  } else if (!ops_arg_container(ctx, args, &container)) {
    return VKR_EDITOR_OP_DONE;
  }
  Vec3 box_min = {0};
  Vec3 box_max = {0};
  const VkrBakeryJson *region = vkr_bakery_json_get(args, "region");
  if (region && (region->type != VKR_BAKERY_JSON_OBJECT ||
                 !ops_arg_vec3(ctx, region, "min", &box_min, NULL) ||
                 !ops_arg_vec3(ctx, region, "max", &box_max, NULL))) {
    if (!ctx->call->error_code) {
      ops_fail(ctx, OPS_INVALID, "'region' is {\"min\": [..], \"max\": [..]}");
    }
    return VKR_EDITOR_OP_DONE;
  }
  float64_t offset_value = 0.0;
  float64_t limit_value = OPS_DESCRIBE_DEFAULT;
  (void)ops_arg_number(args, "offset", &offset_value);
  (void)ops_arg_number(args, "limit", &limit_value);
  const uint32_t offset = (uint32_t)Max(0.0, offset_value);
  const uint32_t limit =
      (uint32_t)vkr_clamp_f64(limit_value, 1.0, OPS_DESCRIBE_MAX);
  const VkrScene *scene = ops_scene(ctx->frame, container);
  VkrBakeryJson *entities = vkr_bakery_json_array(arena);
  uint32_t matched = 0u;
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    if (!vkr_scene_entity_alive(scene, entity) ||
        (root.entity.u64 && !ops_under(scene, entity, root.entity))) {
      continue;
    }
    Vec3 lo = {0};
    Vec3 hi = {0};
    const bool8_t bounded = ops_world_bounds(scene, entity, &lo, &hi);
    const SceneTransform *transform = ops_transform(scene, entity);
    if (region) {
      const Vec3 at = transform ? mat4_position(transform->world) : vec3_zero();
      if (!(bounded ? ops_in_box(lo, hi, box_min, box_max)
                    : ops_in_box(at, at, box_min, box_max))) {
        continue;
      }
    }
    if (matched++ < offset || matched > offset + limit) {
      continue;
    }
    VkrSceneEditValues values;
    if (!vkr_scene_edit_read(scene, entity, &values)) {
      continue;
    }
    VkrBakeryJson *row = ops_entity(ctx, scene, entity);
    ops_set(ctx, row, "parent", ops_id(ctx, ops_parent(scene, entity)));
    ops_set(ctx, row, "components",
            ops_component_names(ctx, scene, entity, &values));
    if (values.fields & VKR_SCENE_EDIT_TRANSFORM) {
      float32_t euler[3];
      vkr_property_quat_euler(values.rotation, euler);
      ops_set(ctx, row, "position", ops_vec3(ctx, values.position));
      ops_set(ctx, row, "rotation",
              ops_vec3(ctx, vec3_new(euler[0], euler[1], euler[2])));
      ops_set(ctx, row, "scale", ops_vec3(ctx, values.scale));
    }
    if (values.fields & VKR_SCENE_EDIT_VISIBILITY) {
      ops_set(ctx, row, "visible",
              vkr_bakery_json_bool(arena, values.visibility.visible));
    }
    if (bounded) {
      VkrBakeryJson *bounds = vkr_bakery_json_object(arena);
      ops_set(ctx, bounds, "min", ops_vec3(ctx, lo));
      ops_set(ctx, bounds, "max", ops_vec3(ctx, hi));
      ops_set(ctx, row, "bounds", bounds);
    }
    vkr_bakery_json_append(entities, row);
  }
  char slot[16];
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  ops_set(ctx, result, "container",
          vkr_bakery_json_cstr(
              arena, ops_container_name(container, slot, sizeof(slot))));
  ops_set(ctx, result, "matched", vkr_bakery_json_int(arena, matched));
  ops_set(ctx, result, "offset", vkr_bakery_json_int(arena, offset));
  ops_set(ctx, result, "entities", entities);
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_get(OpsContext *ctx) {
  OpsRef ref;
  if (!ops_ref(ctx, NULL, vkr_bakery_json_get(ctx->call->args, "entity"),
               "entity", &ref)) {
    return VKR_EDITOR_OP_DONE;
  }
  Arena *arena = ops_arena(ctx);
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  VkrSceneEditValues values;
  if (!vkr_scene_edit_read(scene, ref.entity, &values)) {
    ops_fail(ctx, OPS_NOT_FOUND, "That entity cannot be read");
    return VKR_EDITOR_OP_DONE;
  }
  VkrBakeryJson *result = ops_entity(ctx, scene, ref.entity);
  ops_set(ctx, result, "parent", ops_id(ctx, ops_parent(scene, ref.entity)));
  VkrBakeryJson *children = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId child = vkr_entity_id_from_index(scene->world, i);
    if (vkr_scene_entity_alive(scene, child) &&
        ops_parent(scene, child).u64 == ref.entity.u64) {
      vkr_bakery_json_append(children, ops_entity(ctx, scene, child));
    }
  }
  ops_set(ctx, result, "children", children);
  const SceneTransform *transform = ops_transform(scene, ref.entity);
  if (transform) {
    float32_t euler[3];
    vkr_property_quat_euler(transform->rotation, euler);
    ops_set(ctx, result, "position", ops_vec3(ctx, transform->position));
    ops_set(ctx, result, "rotation",
            ops_vec3(ctx, vec3_new(euler[0], euler[1], euler[2])));
    ops_set(ctx, result, "scale", ops_vec3(ctx, transform->scale));
    ops_set(ctx, result, "world_position",
            ops_vec3(ctx, mat4_position(transform->world)));
  }
  if (values.fields & VKR_SCENE_EDIT_VISIBILITY) {
    ops_set(ctx, result, "visible",
            vkr_bakery_json_bool(arena, values.visibility.visible));
  }
  Vec3 lo = {0};
  Vec3 hi = {0};
  if (ops_world_bounds(scene, ref.entity, &lo, &hi)) {
    VkrBakeryJson *bounds = vkr_bakery_json_object(arena);
    ops_set(ctx, bounds, "min", ops_vec3(ctx, lo));
    ops_set(ctx, bounds, "max", ops_vec3(ctx, hi));
    ops_set(ctx, result, "bounds", bounds);
  }
  VkrBakeryJson *components = vkr_bakery_json_object(arena);
  const VkrTypeDesc *type = NULL;
  _Alignas(16) uint8_t bytes[VKR_TYPE_VALUE_MAX];
  for (uint32_t i = 0; (type = vkr_scene_edit_component_type(i)); ++i) {
    if (type != &vkr_scene_transform_type &&
        type != &vkr_scene_visibility_type &&
        vkr_scene_edit_component_get(&values, type, bytes)) {
      ops_set(ctx, components, type->name,
              ops_component_json(ctx, type, bytes));
    }
  }
  for (uint32_t i = 0; (type = vkr_scene_world_type(i)); ++i) {
    const void *value = vkr_scene_get_typed(scene, ref.entity, type);
    if (value) {
      ops_set(ctx, components, type->name,
              ops_component_json(ctx, type, value));
    }
  }
  ops_set(ctx, result, "components", components);
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_bounds(OpsContext *ctx) {
  OpsRef ref;
  if (!ops_ref(ctx, NULL, vkr_bakery_json_get(ctx->call->args, "entity"),
               "entity", &ref)) {
    return VKR_EDITOR_OP_DONE;
  }
  Vec3 lo = {0};
  Vec3 hi = {0};
  if (!ops_world_bounds(ops_scene(ctx->frame, ref.container), ref.entity, &lo,
                        &hi)) {
    ops_fail(ctx, OPS_NOT_FOUND,
             "The entity and its descendants have no loaded geometry");
    return VKR_EDITOR_OP_DONE;
  }
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "min", ops_vec3(ctx, lo));
  ops_set(ctx, ctx->call->result, "max", ops_vec3(ctx, hi));
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_raycast(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  Vec3 origin = {0};
  Vec3 direction = {0};
  bool8_t has_origin = false_v;
  bool8_t has_direction = false_v;
  if (!ops_arg_vec3(ctx, args, "origin", &origin, &has_origin) ||
      !ops_arg_vec3(ctx, args, "direction", &direction, &has_direction)) {
    return VKR_EDITOR_OP_DONE;
  }
  float64_t distance = 1000.0;
  (void)ops_arg_number(args, "max_distance", &distance);
  if (!has_origin || !has_direction || vec3_length(direction) < 1e-6f ||
      !(distance > 0.0)) {
    ops_fail(ctx, OPS_INVALID,
             "query.raycast needs origin, a nonzero direction and a "
             "positive max_distance");
    return VKR_EDITOR_OP_DONE;
  }
  uint16_t container = 0u;
  if (!ops_arg_container(ctx, args, &container)) {
    return VKR_EDITOR_OP_DONE;
  }
  const Vec3 displacement =
      vec3_scale(vec3_normalize(direction), (float32_t)distance);
  VkrPhysicsQueryFilter filter = {.mask = UINT16_MAX};
  VkrPhysicsRayHit hit = {0};
  Arena *arena = ops_arena(ctx);
  ctx->call->result = vkr_bakery_json_object(arena);
  /* Physics queries take a mutable scene but change none of its state. */
  VkrScene *scene = (VkrScene *)ops_scene(ctx->frame, container);
  if (!vkr_scene_physics_raycast_query(scene, origin, displacement, &filter,
                                       &hit)) {
    ops_set(ctx, ctx->call->result, "hit", vkr_bakery_json_bool(arena, 0));
    return VKR_EDITOR_OP_DONE;
  }
  ops_set(ctx, ctx->call->result, "hit", vkr_bakery_json_bool(arena, 1));
  ops_set(ctx, ctx->call->result, "entity",
          ops_entity(ctx, scene, (VkrEntityId){.u64 = hit.entity_id}));
  ops_set(ctx, ctx->call->result, "collider",
          ops_id(ctx, (VkrEntityId){.u64 = hit.collider_entity_id}));
  ops_set(ctx, ctx->call->result, "position",
          ops_vec3(ctx, vec3_new(hit.position[0], hit.position[1],
                                 hit.position[2])));
  ops_set(ctx, ctx->call->result, "normal",
          ops_vec3(ctx, vec3_new(hit.normal[0], hit.normal[1], hit.normal[2])));
  ops_set(ctx, ctx->call->result, "distance",
          ops_number(ctx, hit.fraction * distance));
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_write(OpsContext *ctx, const OpsDef *def) {
  VkrEditorOpCall *call = ctx->call;
  if (call->stage) {
    return ops_batch_wait(ctx);
  }
  OpsBatch *batch =
      arena_alloc(call->arena, sizeof(*batch), ARENA_MEMORY_TAG_STRUCT);
  if (!batch) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  MemZero(batch, sizeof(*batch));
  batch->container = -1;
  batch->review = ops_arg_bool(call->args, "review", true_v);
  batch->label = call->op;
  if (!ops_build_one(ctx, def, call->args, batch)) {
    return VKR_EDITOR_OP_DONE;
  }
  return ops_submit(ctx, batch, ops_arg_bool(call->args, "dry_run", false_v));
}

static VkrEditorOpStatus ops_run_changes_list(OpsContext *ctx) {
  VkrBakeryJson *changes = vkr_bakery_json_array(ops_arena(ctx));
  for (uint32_t i = 0; i < ctx->ops->change_count; ++i) {
    vkr_bakery_json_append(changes,
                           ops_change_json(ctx, &ctx->ops->changes[i]));
  }
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "changes", changes);
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_changes_accept(OpsContext *ctx) {
  int64_t id = 0;
  VkrEditorOps *ops = ctx->ops;
  uint32_t accepted = 0u;
  if (vkr_bakery_json_get_int(ctx->call->args, "change", &id)) {
    if (id <= 0 || !vkr_editor_ops_accept(ops, (uint32_t)id)) {
      ops_fail(ctx, OPS_NOT_FOUND, "No pending change %lld", (long long)id);
      return VKR_EDITOR_OP_DONE;
    }
    accepted = 1u;
  } else {
    accepted = ops->change_count;
    (void)vkr_editor_ops_accept(ops, 0u);
  }
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "accepted",
          vkr_bakery_json_int(ops_arena(ctx), accepted));
  return VKR_EDITOR_OP_DONE;
}

typedef struct OpsPendingReject {
  uint32_t change;
  uint64_t token;
} OpsPendingReject;

static VkrEditorOpStatus ops_run_changes_reject(OpsContext *ctx) {
  VkrEditorOpCall *call = ctx->call;
  VkrEditorOps *ops = ctx->ops;
  if (!call->stage) {
    int64_t id = 0;
    if (!vkr_bakery_json_get_int(call->args, "change", &id)) {
      ops_fail(ctx, OPS_INVALID, "'change' must name a pending change");
      return VKR_EDITOR_OP_DONE;
    }
    const int32_t index = ops_change_find(ops, (uint32_t)id);
    if (index < 0) {
      ops_fail(ctx, OPS_NOT_FOUND, "No pending change %lld", (long long)id);
      return VKR_EDITOR_OP_DONE;
    }
    if (ctx->frame->simulation_running || !ctx->frame->edit_batch ||
        ctx->frame->edit_batch->token) {
      ops_fail(ctx, OPS_BUSY, "The scene cannot change now");
      return VKR_EDITOR_OP_DONE;
    }
    OpsPendingReject *pending =
        arena_alloc(call->arena, sizeof(*pending), ARENA_MEMORY_TAG_STRUCT);
    if (!pending) {
      ops_fail(ctx, OPS_LIMIT, "Out of request memory");
      return VKR_EDITOR_OP_DONE;
    }
    const VkrEditorChange *change = &ops->changes[index];
    *pending =
        (OpsPendingReject){.change = change->id, .token = ++ops->next_token};
    *ctx->frame->edit_batch = (VkrSampleEditBatchRequest){
        .token = pending->token,
        .container = change->container,
        .revert_group = change->group,
    };
    call->state = pending;
    call->stage = 1u;
    return VKR_EDITOR_OP_WAIT;
  }
  const OpsPendingReject *pending = call->state;
  const VkrSampleEditBatchResult *result = ctx->frame->edit_batch_result;
  if (!result || result->token != pending->token) {
    if (++call->frames > OPS_WAIT_FRAMES) {
      ops_fail(ctx, OPS_BUSY, "The revert was not applied");
      return VKR_EDITOR_OP_DONE;
    }
    return VKR_EDITOR_OP_WAIT;
  }
  const int32_t index = ops_change_find(ops, pending->change);
  if (!result->ok) {
    char conflict[40] = "";
    if (result->conflict.u64) {
      ops_id_text(result->conflict, conflict, sizeof(conflict));
    }
    ops_fail(ctx, OPS_REJECTED, "%s%s%s", result->message,
             conflict[0] ? " Conflicting entity: " : "", conflict);
    return VKR_EDITOR_OP_DONE;
  }
  if (index >= 0) {
    ops_change_remove(ops, (uint32_t)index);
  }
  call->result = vkr_bakery_json_object(call->arena);
  ops_set(ctx, call->result, "rejected",
          vkr_bakery_json_int(call->arena, pending->change));
  return VKR_EDITOR_OP_DONE;
}

/* cmd: runs one Cmd line through the shared queue and returns the lines it
   printed once the queue is idle again. */
typedef struct OpsPendingCmd {
  char output[8192];
} OpsPendingCmd;

static VkrEditorOpStatus ops_run_cmd(OpsContext *ctx) {
  VkrEditorOpCall *call = ctx->call;
  VkrEditorUi *editor = ctx->editor;
  if (!call->stage) {
    String8 line = {0};
    if (!vkr_bakery_json_get_string(call->args, "line", &line) ||
        !line.length || line.length >= 1024u) {
      ops_fail(ctx, OPS_INVALID, "'line' must be a Cmd statement");
      return VKR_EDITOR_OP_DONE;
    }
    if (editor->cmd_capture) {
      ops_fail(ctx, OPS_BUSY, "Another Cmd capture is running");
      return VKR_EDITOR_OP_DONE;
    }
    OpsPendingCmd *pending =
        arena_alloc(call->arena, sizeof(*pending), ARENA_MEMORY_TAG_STRUCT);
    char *text =
        arena_alloc(call->arena, line.length + 1u, ARENA_MEMORY_TAG_STRING);
    if (!pending || !text) {
      ops_fail(ctx, OPS_LIMIT, "Out of request memory");
      return VKR_EDITOR_OP_DONE;
    }
    MemCopy(text, line.str, line.length);
    text[line.length] = '\0';
    vkr_editor_cmd_capture_begin(editor, pending->output,
                                 sizeof(pending->output));
    if (!vkr_editor_cmd_enqueue(editor, text)) {
      (void)vkr_editor_cmd_capture_end(editor);
      ops_fail(ctx, OPS_BUSY, "The Cmd queue is full");
      return VKR_EDITOR_OP_DONE;
    }
    call->state = pending;
    call->stage = 1u;
    return VKR_EDITOR_OP_WAIT;
  }
  /* The queue runs the line in a later build; it is done once idle. */
  if (!vkr_editor_cmd_idle(editor) || call->frames++ < 1u) {
    return VKR_EDITOR_OP_WAIT;
  }
  OpsPendingCmd *pending = call->state;
  const uint32_t length = vkr_editor_cmd_capture_end(editor);
  VkrBakeryJson *lines = vkr_bakery_json_array(call->arena);
  bool8_t failed = false_v;
  uint32_t start = 0u;
  for (uint32_t i = 0; i < length; ++i) {
    if (pending->output[i] != '\n') {
      continue;
    }
    const String8 text = {.str = (uint8_t *)pending->output + start,
                          .length = i - start};
    failed |= text.length >= 6u && MemCompare(text.str, "error:", 6u) == 0;
    vkr_bakery_json_append(lines, vkr_bakery_json_string(call->arena, text));
    start = i + 1u;
  }
  call->result = vkr_bakery_json_object(call->arena);
  ops_set(ctx, call->result, "ok", vkr_bakery_json_bool(call->arena, !failed));
  ops_set(ctx, call->result, "lines", lines);
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_undo(OpsContext *ctx) {
  /* Undo and redo run as Cmd lines, which own the scene edit slot. */
  if (!ctx->call->stage) {
    const bool8_t redo = ops_equals(ctx->call->op, "redo");
    VkrBakeryJson *args = vkr_bakery_json_object(ops_arena(ctx));
    ops_set(ctx, args, "line",
            vkr_bakery_json_cstr(ops_arena(ctx), redo ? "redo" : "undo"));
    ctx->call->args = args;
  }
  return ops_run_cmd(ctx);
}

// -----------------------------------------------------------------------------
// view.capture
// -----------------------------------------------------------------------------

typedef struct OpsPendingCapture {
  /* Capture the whole window instead of the Scene's image. */
  bool8_t window;
  VkrSampleViewState saved_view;
  VkrSampleSceneRecall saved_recall;
  bool8_t restore;
  char path[512];
  uint32_t width;
  uint32_t height;
} OpsPendingCapture;

static float32_t ops_half_to_float(uint16_t half) {
  const uint32_t sign = (uint32_t)(half >> 15) << 31;
  const uint32_t exponent = (half >> 10) & 0x1Fu;
  const uint32_t mantissa = half & 0x3FFu;
  uint32_t bits = 0u;
  if (exponent == 0u) {
    if (mantissa == 0u) {
      bits = sign;
    } else {
      float32_t value = (float32_t)mantissa / 1024.0f / 16384.0f;
      return sign ? -value : value;
    }
  } else if (exponent == 31u) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
  }
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

static uint8_t ops_encode_srgb(float32_t linear) {
  if (!(linear > 0.0f)) {
    return 0u;
  }
  if (linear >= 1.0f) {
    return 255u;
  }
  const float32_t encoded = linear <= 0.0031308f
                                ? linear * 12.92f
                                : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
  return (uint8_t)(encoded * 255.0f + 0.5f);
}

static void ops_png_sink(void *context, void *data, int size) {
  (void)fwrite(data, 1, (size_t)size, (FILE *)context);
}

/* Writes the Scene image area of the captured window as an RGBA8 PNG. */
static bool8_t ops_capture_write(OpsContext *ctx, OpsPendingCapture *pending,
                                 const VkrCaptureItemResult *item) {
  const bool8_t rgba8 = item->format == VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM ||
                        item->format == VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB;
  const bool8_t bgra8 = item->format == VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM ||
                        item->format == VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB;
  const bool8_t half = item->format == VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT;
  if (!rgba8 && !bgra8 && !half) {
    return ops_fail(ctx, OPS_CAPTURE, "Unsupported capture format %u",
                    (unsigned)item->format);
  }
  /* The Scene's image rectangle, clamped to the captured window. */
  const Vec4 rect = ctx->frame->mapping_valid && !pending->window
                        ? ctx->frame->mapping.image_rect_px
                        : vec4_new(0.0f, 0.0f, (float32_t)item->width,
                                   (float32_t)item->height);
  const uint32_t x0 = (uint32_t)vkr_clamp_f32(rect.x, 0.0f, item->width);
  const uint32_t y0 = (uint32_t)vkr_clamp_f32(rect.y, 0.0f, item->height);
  const uint32_t x1 =
      (uint32_t)vkr_clamp_f32(rect.x + rect.z, (float32_t)x0, item->width);
  const uint32_t y1 =
      (uint32_t)vkr_clamp_f32(rect.y + rect.w, (float32_t)y0, item->height);
  const uint32_t width = x1 - x0;
  const uint32_t height = y1 - y0;
  if (!width || !height) {
    return ops_fail(ctx, OPS_CAPTURE, "The Scene has no visible area");
  }
  uint8_t *pixels = malloc((size_t)width * height * 4u);
  if (!pixels) {
    return ops_fail(ctx, OPS_LIMIT, "Out of memory for the capture");
  }
  const uint32_t texel = half ? 8u : 4u;
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t source_y = item->origin == VKR_CAPTURE_ORIGIN_BOTTOM_LEFT
                                  ? item->height - 1u - (y0 + y)
                                  : y0 + y;
    const uint8_t *row = (const uint8_t *)item->data +
                         (uint64_t)source_y * item->row_pitch +
                         (uint64_t)x0 * texel;
    uint8_t *out = pixels + (size_t)y * width * 4u;
    for (uint32_t x = 0; x < width; ++x, out += 4) {
      const uint8_t *in = row + (uint64_t)x * texel;
      if (half) {
        uint16_t channels[4];
        MemCopy(channels, in, sizeof(channels));
        for (uint32_t c = 0; c < 3u; ++c) {
          out[c] = ops_encode_srgb(ops_half_to_float(channels[c]));
        }
        out[3] = 255u;
      } else {
        out[0] = bgra8 ? in[2] : in[0];
        out[1] = in[1];
        out[2] = bgra8 ? in[0] : in[2];
        out[3] = 255u;
      }
    }
  }
  char directory[256];
  bool8_t written = false_v;
  if (vkr_editor_agent_directory(directory, sizeof(directory))) {
    char captures[300];
    snprintf(captures, sizeof(captures), "%s/captures", directory);
#if !defined(_WIN32)
    (void)mkdir(captures, 0700);
    const int pid = (int)getpid();
#else
    const int pid = 0;
#endif
    const uint32_t serial = ++ctx->ops->capture_serial;
    snprintf(pending->path, sizeof(pending->path), "%s/capture-%d-%u.png",
             captures, pid, serial);
    FILE *file = file_fopen(pending->path, "wb");
    if (file) {
      written =
          stbi_write_png_to_func(ops_png_sink, file, (int)width, (int)height, 4,
                                 pixels, (int)width * 4) != 0;
      written = fclose(file) == 0 && written;
    }
    if (serial > OPS_CAPTURE_KEEP) {
      char old[512];
      snprintf(old, sizeof(old), "%s/capture-%d-%u.png", captures, pid,
               serial - OPS_CAPTURE_KEEP);
      const FilePath old_path = {
          .path = string8_create_from_cstr((const uint8_t *)old, strlen(old)),
          .type = FILE_PATH_TYPE_ABSOLUTE};
      (void)file_remove(&old_path);
    }
  }
  free(pixels);
  if (!written) {
    return ops_fail(ctx, OPS_CAPTURE, "The capture PNG could not be written");
  }
  pending->width = width;
  pending->height = height;
  return true_v;
}

static bool8_t ops_capture_view(OpsContext *ctx, String8 word,
                                VkrSampleCameraView *out) {
  for (uint32_t i = 0; vkr_editor_cmd_camera_views[i]; ++i) {
    if (ops_equals(word, vkr_editor_cmd_camera_views[i])) {
      *out = (VkrSampleCameraView)i;
      return true_v;
    }
  }
  return ops_fail(ctx, OPS_INVALID,
                  "'view' is current, perspective, top, left, right or bottom");
}

static VkrEditorOpStatus ops_run_capture(OpsContext *ctx) {
  VkrEditorOpCall *call = ctx->call;
  const VkrSampleUiFrame *frame = ctx->frame;
  OpsPendingCapture *pending = call->state;
  switch (call->stage) {
  case 0u: {
    if (!frame->capture_request || !frame->view_request) {
      ops_fail(ctx, OPS_CAPTURE, "Captures are unavailable");
      return VKR_EDITOR_OP_DONE;
    }
    pending =
        arena_alloc(call->arena, sizeof(*pending), ARENA_MEMORY_TAG_STRUCT);
    if (!pending) {
      ops_fail(ctx, OPS_LIMIT, "Out of request memory");
      return VKR_EDITOR_OP_DONE;
    }
    MemZero(pending, sizeof(*pending));
    pending->saved_view = frame->view_state;
    pending->saved_recall = frame->scene_recall;
    String8 area = {0};
    if (vkr_bakery_json_get_string(call->args, "area", &area)) {
      if (!ops_equals(area, "scene") && !ops_equals(area, "window")) {
        ops_fail(ctx, OPS_INVALID, "'area' is scene or window");
        return VKR_EDITOR_OP_DONE;
      }
      pending->window = ops_equals(area, "window");
    }
    VkrSampleViewState next = frame->view_state;
    String8 view = {0};
    if (vkr_bakery_json_get_string(call->args, "view", &view) &&
        !ops_equals(view, "current") &&
        !ops_capture_view(ctx, view, &next.camera_view)) {
      return VKR_EDITOR_OP_DONE;
    }
    bool8_t labels = false_v;
    if (vkr_bakery_json_get_bool(call->args, "grid_labels", &labels)) {
      next.grid_labels = labels;
      next.grid_enabled = next.grid_enabled || labels;
    }
    VkrSampleViewRequest request = {.value = next, .apply = true_v};
    const VkrBakeryJson *focus = vkr_bakery_json_get(call->args, "focus");
    if (focus && focus->type == VKR_BAKERY_JSON_OBJECT) {
      if (!ops_arg_vec3(ctx, focus, "min", &request.frame_min, NULL) ||
          !ops_arg_vec3(ctx, focus, "max", &request.frame_max, NULL)) {
        return VKR_EDITOR_OP_DONE;
      }
      request.frame_box = true_v;
    } else if (focus) {
      OpsRef ref;
      if (!ops_ref(ctx, NULL, focus, "focus", &ref)) {
        return VKR_EDITOR_OP_DONE;
      }
      if (!ops_world_bounds(ops_scene(frame, ref.container), ref.entity,
                            &request.frame_min, &request.frame_max)) {
        const SceneTransform *transform =
            ops_transform(ops_scene(frame, ref.container), ref.entity);
        const Vec3 at =
            transform ? mat4_position(transform->world) : vec3_zero();
        request.frame_min = vec3_sub(at, vec3_one());
        request.frame_max = vec3_add(at, vec3_one());
      }
      request.frame_box = true_v;
    }
    pending->restore = request.frame_box ||
                       next.camera_view != frame->view_state.camera_view ||
                       next.grid_labels != frame->view_state.grid_labels ||
                       next.grid_enabled != frame->view_state.grid_enabled;
    *frame->view_request = request;
    call->state = pending;
    call->stage = 1u;
    call->frames = 0u;
    return VKR_EDITOR_OP_WAIT;
  }
  case 1u:
    if (++call->frames < OPS_CAPTURE_SETTLE_FRAMES) {
      return VKR_EDITOR_OP_WAIT;
    }
    call->token = ++ctx->ops->next_token;
    *frame->capture_request =
        (VkrSampleCaptureRequest){.request = true_v, .token = call->token};
    call->stage = 2u;
    call->frames = 0u;
    return VKR_EDITOR_OP_WAIT;
  case 2u: {
    const VkrSampleCaptureReady *ready = frame->capture_ready;
    if (!ready || ready->token != call->token) {
      if (++call->frames > OPS_WAIT_FRAMES) {
        ops_fail(ctx, OPS_CAPTURE, "The capture did not finish");
        call->stage = 3u;
      }
      return call->stage == 3u ? ops_run_capture(ctx) : VKR_EDITOR_OP_WAIT;
    }
    if (ready->failed || !ready->item) {
      ops_fail(ctx, OPS_CAPTURE, "The renderer could not capture the window");
    } else if (ops_capture_write(ctx, pending, ready->item)) {
      call->result = vkr_bakery_json_object(call->arena);
      ops_set(ctx, call->result, "path",
              vkr_bakery_json_cstr(call->arena, pending->path));
      ops_set(ctx, call->result, "width",
              vkr_bakery_json_int(call->arena, pending->width));
      ops_set(ctx, call->result, "height",
              vkr_bakery_json_int(call->arena, pending->height));
    }
    call->stage = 3u;
  }
    /* fall through */
  default:
    if (pending && pending->restore) {
      *frame->view_request =
          (VkrSampleViewRequest){.value = pending->saved_view, .apply = true_v};
      if (frame->editor_state_request) {
        frame->editor_state_request->apply_recall = true_v;
        frame->editor_state_request->recall = pending->saved_recall;
        frame->editor_state_request->recall.selection_valid = false_v;
      }
    }
    return VKR_EDITOR_OP_DONE;
  }
}

// =============================================================================
// Table
// =============================================================================

#define OPS_VEC3_SCHEMA                                                        \
  "{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,"        \
  "\"maxItems\":3}"
#define OPS_ENTITY_SCHEMA                                                      \
  "{\"type\":\"string\",\"description\":\"An entity: world:index:generation, " \
  "an exact unique name, or $k for the entity operation k of the same batch "  \
  "created\"}"
#define OPS_CONTAINER_SCHEMA                                                   \
  "{\"description\":\"primary, world, or an added scene slot 1-6\","           \
  "\"oneOf\":[{\"type\":\"string\",\"enum\":[\"primary\",\"world\"]},"         \
  "{\"type\":\"integer\",\"minimum\":1,\"maximum\":6}]}"
#define OPS_REVIEW_SCHEMA                                                      \
  "\"review\":{\"type\":\"boolean\",\"description\":\"Keep the edit as a "     \
  "pending change the designer accepts or rejects (default true)\"},"          \
  "\"dry_run\":{\"type\":\"boolean\"}"
#define OPS_VALUES_SCHEMA                                                      \
  "{\"type\":\"object\",\"description\":\"Property values by descriptor "      \
  "name, as scene documents store them\"}"

static VkrEditorOpStatus ops_run_create(OpsContext *ctx);
static VkrEditorOpStatus ops_run_set(OpsContext *ctx);
static VkrEditorOpStatus ops_run_delete(OpsContext *ctx);
static VkrEditorOpStatus ops_run_parent(OpsContext *ctx);
static VkrEditorOpStatus ops_run_component_add(OpsContext *ctx);
static VkrEditorOpStatus ops_run_component_set(OpsContext *ctx);
static VkrEditorOpStatus ops_run_component_remove(OpsContext *ctx);

static const OpsDef s_ops[] = {
    {"ops.list", "Every operation with its description and argument schema.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_list, NULL},
    {"editor.status",
     "Loaded scenes, selection, simulation, view and pending changes.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_status, NULL},
    {"scene.describe",
     "Entities of one scene with id, name, parent, components, local pose and "
     "world bounds. Page with offset and limit (at most 500).",
     "{\"type\":\"object\",\"properties\":{\"container\":" OPS_CONTAINER_SCHEMA
     ",\"root\":" OPS_ENTITY_SCHEMA
     ",\"region\":{\"type\":\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA "},\"required\":[\"min\",\"max\"]},"
     "\"offset\":{\"type\":\"integer\",\"minimum\":0},"
     "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}}}",
     ops_run_describe, NULL},
    {"entity.get",
     "One entity: pose, children, bounds and every component's values.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     "},\"required\":[\"entity\"]}",
     ops_run_get, NULL},
    {"entity.create",
     "Create an entity with a name, pose (rotation in degrees XYZ) and "
     "optionally one component.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
     "\"parent\":" OPS_ENTITY_SCHEMA ",\"container\":" OPS_CONTAINER_SCHEMA
     ",\"position\":" OPS_VEC3_SCHEMA ",\"rotation\":" OPS_VEC3_SCHEMA
     ",\"scale\":" OPS_VEC3_SCHEMA
     ",\"component\":{\"type\":\"object\",\"properties\":{\"type\":{\"type\":"
     "\"string\"},\"values\":" OPS_VALUES_SCHEMA
     "},\"required\":[\"type\"]}," OPS_REVIEW_SCHEMA "}}",
     ops_run_create, ops_build_create},
    {"entity.set", "Change an entity's name, pose or visibility.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"name\":{\"type\":\"string\"},\"position\":" OPS_VEC3_SCHEMA
     ",\"rotation\":" OPS_VEC3_SCHEMA ",\"scale\":" OPS_VEC3_SCHEMA
     ",\"visible\":{\"type\":\"boolean\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"entity\"]}",
     ops_run_set, ops_build_set},
    {"entity.delete",
     "Delete an entity; recursive also deletes its descendants first.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"recursive\":{\"type\":\"boolean\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"entity\"]}",
     ops_run_delete, ops_build_delete},
    {"entity.parent",
     "Move an entity under another in the same scene, or to the root with "
     "null, keeping its world pose.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"parent\":{\"oneOf\":[" OPS_ENTITY_SCHEMA
     ",{\"type\":\"null\"}]}," OPS_REVIEW_SCHEMA "},\"required\":[\"entity\"]}",
     ops_run_parent, ops_build_parent},
    {"component.add", "Add a world component type with optional values.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"type\":{\"type\":\"string\"},\"values\":" OPS_VALUES_SCHEMA
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"entity\",\"type\"]}",
     ops_run_component_add, ops_build_component_add},
    {"component.set",
     "Set property values of a component the entity carries, lights "
     "included; unnamed properties keep their values.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"type\":{\"type\":\"string\"},\"values\":" OPS_VALUES_SCHEMA
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"entity\",\"type\",\"values\"]}",
     ops_run_component_set, ops_build_component_set},
    {"component.remove", "Remove a world component type.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"type\":{\"type\":\"string\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"entity\",\"type\"]}",
     ops_run_component_remove, ops_build_component_remove},
    {"batch",
     "Apply write operations in order as one undo step; $k names the entity "
     "operation k created. A failure rolls the whole batch back.",
     "{\"type\":\"object\",\"properties\":{\"ops\":{\"type\":\"array\","
     "\"maxItems\":256,\"items\":{\"type\":\"object\",\"properties\":{\"op\":"
     "{\"type\":\"string\"},\"args\":{\"type\":\"object\"}},\"required\":["
     "\"op\"]}},\"label\":{\"type\":\"string\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"ops\"]}",
     ops_run_batch, NULL},
    {"changes.list", "Pending changes awaiting the designer's review.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_changes_list, NULL},
    {"changes.accept",
     "Accept one pending change, or all of them without 'change'.",
     "{\"type\":\"object\",\"properties\":{\"change\":{\"type\":\"integer\"}}}",
     ops_run_changes_accept, NULL},
    {"changes.reject",
     "Reject a pending change: its edits revert unless later edits depend on "
     "them.",
     "{\"type\":\"object\",\"properties\":{\"change\":{\"type\":\"integer\"}},"
     "\"required\":[\"change\"]}",
     ops_run_changes_reject, NULL},
    {"undo", "Undo the newest edit step.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_undo, NULL},
    {"redo", "Redo the next edit step.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_undo, NULL},
    {"query.raycast",
     "First collision surface along a ray: entity, position, normal and "
     "distance.",
     "{\"type\":\"object\",\"properties\":{\"origin\":" OPS_VEC3_SCHEMA
     ",\"direction\":" OPS_VEC3_SCHEMA
     ",\"max_distance\":{\"type\":\"number\",\"exclusiveMinimum\":0},"
     "\"container\":" OPS_CONTAINER_SCHEMA "},\"required\":[\"origin\","
     "\"direction\"]}",
     ops_run_raycast, NULL},
    {"query.bounds", "World bounds of an entity and its descendants.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     "},\"required\":[\"entity\"]}",
     ops_run_bounds, NULL},
    {"view.capture",
     "Capture the Scene as a PNG, optionally from another view (top is an "
     "orthographic map), framed on an entity or box, with grid labels.",
     "{\"type\":\"object\",\"properties\":{\"view\":{\"type\":\"string\","
     "\"enum\":[\"current\",\"perspective\",\"top\",\"left\",\"right\","
     "\"bottom\"]},\"focus\":{\"oneOf\":[" OPS_ENTITY_SCHEMA
     ",{\"type\":\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA "},\"required\":[\"min\",\"max\"]}]},"
     "\"grid_labels\":{\"type\":\"boolean\"},\"area\":{\"type\":"
     "\"string\",\"enum\":[\"scene\",\"window\"],\"description\":\"The "
     "Scene image (default) or the whole editor window\"}}}",
     ops_run_capture, NULL},
    {"cmd",
     "Run one editor Cmd statement (see 'cmd' with line 'help') and return "
     "the lines it printed.",
     "{\"type\":\"object\",\"properties\":{\"line\":{\"type\":\"string\"}},"
     "\"required\":[\"line\"]}",
     ops_run_cmd, NULL},
};

static const OpsDef *ops_find(String8 name) {
  for (uint32_t i = 0; i < ArrayCount(s_ops); ++i) {
    if (ops_equals(name, s_ops[i].name)) {
      return &s_ops[i];
    }
  }
  return NULL;
}

static VkrEditorOpStatus ops_run_def(OpsContext *ctx, const char *name) {
  return ops_run_write(ctx, ops_find(string8_create_from_cstr(
                                (const uint8_t *)name, strlen(name))));
}

static VkrEditorOpStatus ops_run_create(OpsContext *ctx) {
  return ops_run_def(ctx, "entity.create");
}

static VkrEditorOpStatus ops_run_set(OpsContext *ctx) {
  return ops_run_def(ctx, "entity.set");
}

static VkrEditorOpStatus ops_run_delete(OpsContext *ctx) {
  return ops_run_def(ctx, "entity.delete");
}

static VkrEditorOpStatus ops_run_parent(OpsContext *ctx) {
  return ops_run_def(ctx, "entity.parent");
}

static VkrEditorOpStatus ops_run_component_add(OpsContext *ctx) {
  return ops_run_def(ctx, "component.add");
}

static VkrEditorOpStatus ops_run_component_set(OpsContext *ctx) {
  return ops_run_def(ctx, "component.set");
}

static VkrEditorOpStatus ops_run_component_remove(OpsContext *ctx) {
  return ops_run_def(ctx, "component.remove");
}

static VkrEditorOpStatus ops_run_list(OpsContext *ctx) {
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *list = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < ArrayCount(s_ops); ++i) {
    VkrBakeryJsonError error = {0};
    VkrBakeryJson *schema =
        vkr_bakery_json_parse(arena, (const uint8_t *)s_ops[i].schema,
                              strlen(s_ops[i].schema), 32u, &error);
    if (!schema) {
      ops_fail(ctx, OPS_LIMIT, "Schema of %s is malformed: %s", s_ops[i].name,
               error.message);
      return VKR_EDITOR_OP_DONE;
    }
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    ops_set(ctx, entry, "name", vkr_bakery_json_cstr(arena, s_ops[i].name));
    ops_set(ctx, entry, "description",
            vkr_bakery_json_cstr(arena, s_ops[i].description));
    ops_set(ctx, entry, "writes",
            vkr_bakery_json_bool(arena, s_ops[i].build != NULL ||
                                            s_ops[i].run == ops_run_batch));
    ops_set(ctx, entry, "schema", schema);
    vkr_bakery_json_append(list, entry);
  }
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "ops", list);
  return VKR_EDITOR_OP_DONE;
}

// =============================================================================
// Public
// =============================================================================

VkrEditorOps *vkr_editor_ops_create(VkrAllocator *allocator) {
  VkrEditorOps *ops = calloc(1u, sizeof(*ops));
  if (!ops) {
    return NULL;
  }
  ops->allocator = allocator;
  ops->items = calloc(VKR_SAMPLE_EDIT_BATCH_MAX, sizeof(*ops->items));
  if (!ops->items) {
    free(ops);
    return NULL;
  }
  return ops;
}

void vkr_editor_ops_destroy(VkrEditorOps *ops) {
  if (!ops) {
    return;
  }
  free(ops->items);
  free(ops);
}

VkrEditorOpStatus vkr_editor_ops_run(VkrEditorOps *ops, VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrEditorOpCall *call) {
  OpsContext ctx = {.ops = ops, .editor = editor, .frame = frame, .call = call};
  const OpsDef *def = ops_find(call->op);
  if (!def) {
    ops_fail(&ctx, OPS_UNKNOWN, "Unknown operation '%.*s'; see ops.list",
             (int)call->op.length, call->op.str);
    return ops_done(&ctx);
  }
  if (!call->stage && !frame->scene && !frame->world &&
      def->run != ops_run_list && def->run != ops_run_cmd &&
      def->run != ops_run_status) {
    ops_fail(&ctx, OPS_BUSY, "No scene is loaded");
    return ops_done(&ctx);
  }
  return def->run(&ctx);
}

void vkr_editor_ops_update(VkrEditorOps *ops, const VkrSampleUiFrame *frame) {
  for (uint32_t i = 0; ops && i < ops->change_count;) {
    const VkrSceneEditState *journal =
        ops_journal(frame, ops->changes[i].container);
    if (!journal ||
        !vkr_scene_edit_group_present(journal, ops->changes[i].group)) {
      ops_change_remove(ops, i);
      continue;
    }
    ++i;
  }
}

uint32_t vkr_editor_ops_change_count(const VkrEditorOps *ops) {
  return ops ? ops->change_count : 0u;
}

const VkrEditorChange *vkr_editor_ops_change(const VkrEditorOps *ops,
                                             uint32_t index) {
  return ops && index < ops->change_count ? &ops->changes[index] : NULL;
}

bool8_t vkr_editor_ops_entity_pending(const VkrEditorOps *ops,
                                      VkrEntityId entity) {
  for (uint32_t i = 0; ops && i < ops->change_count; ++i) {
    for (uint32_t e = 0; e < ops->changes[i].entity_count; ++e) {
      if (ops->changes[i].entities[e].u64 == entity.u64) {
        return true_v;
      }
    }
  }
  return false_v;
}

bool8_t vkr_editor_ops_accept(VkrEditorOps *ops, uint32_t id) {
  if (!ops) {
    return false_v;
  }
  if (!id) {
    ops->change_count = 0u;
    return true_v;
  }
  const int32_t index = ops_change_find(ops, id);
  if (index < 0) {
    return false_v;
  }
  ops_change_remove(ops, (uint32_t)index);
  return true_v;
}

// =============================================================================
// Changes window
// =============================================================================

/* Frames the Scene on every alive entity of a change. */
static void ops_change_focus(const VkrSampleUiFrame *frame,
                             const VkrEditorChange *change) {
  const VkrScene *scene = ops_scene(frame, change->container);
  Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  bool8_t any = false_v;
  for (uint32_t i = 0; scene && i < change->entity_count; ++i) {
    const VkrEntityId entity = change->entities[i];
    if (!vkr_scene_entity_alive(scene, entity)) {
      continue;
    }
    Vec3 a = {0};
    Vec3 b = {0};
    if (!ops_world_bounds(scene, entity, &a, &b)) {
      const SceneTransform *transform = ops_transform(scene, entity);
      if (!transform) {
        continue;
      }
      a = b = mat4_position(transform->world);
    }
    lo = vec3_new(Min(lo.x, a.x), Min(lo.y, a.y), Min(lo.z, a.z));
    hi = vec3_new(Max(hi.x, b.x), Max(hi.y, b.y), Max(hi.z, b.z));
    any = true_v;
  }
  if (any && frame->view_request) {
    frame->view_request->frame_box = true_v;
    frame->view_request->frame_min = lo;
    frame->view_request->frame_max = hi;
  }
}

static bool8_t ops_row_button(VkrUiSystem *ui, VkrEditorUi *editor, String8 id,
                              String8 text, uint32_t column, bool8_t primary) {
  VkrUiWidgetConfig button = vkr_ui_widget_config_default();
  if (primary) {
    vkr_editor_primary_style(&button, editor->heading_font);
  } else {
    vkr_editor_action_style(&button, editor->heading_font);
  }
  button.placement = (VkrUiPlacement){
      .column = column,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_CENTER,
      .margin_pt = {0.0f, 3.0f, 0.0f, 3.0f},
  };
  return vkr_ui_button(ui, id, text, &button);
}

void vkr_editor_changes_build(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, VkrUiRect bounds) {
  (void)bounds;
  VkrUiSystem *ui = frame->ui;
  VkrEditorOps *ops = vkr_editor_agent_ops(editor->agent);
  const VkrUiTheme *theme = vkr_ui_theme();
  const uint32_t count = vkr_editor_ops_change_count(ops);
  VkrUiTrack rows[VKR_EDITOR_CHANGE_MAX + 2u];
  rows[0] = (VkrUiTrack){.value = 36.0f, .unit = VKR_UI_TRACK_PX};
  for (uint32_t i = 0; i < count; ++i) {
    rows[i + 1u] = (VkrUiTrack){.value = 40.0f, .unit = VKR_UI_TRACK_PX};
  }
  rows[count + 1u] = (VkrUiTrack){.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig list = vkr_ui_panel_config_default();
  list.placement = (VkrUiPlacement){.column = 0u,
                                    .row = 0u,
                                    .column_span = 1u,
                                    .row_span = 1u,
                                    .justify = VKR_UI_ALIGN_STRETCH,
                                    .align = VKR_UI_ALIGN_STRETCH};
  list.columns = &column;
  list.column_count = 1u;
  list.rows = rows;
  list.row_count = count + 2u;
  list.style.padding_pt = (VkrUiEdges){8.0f, 10.0f, 8.0f, 10.0f};
  list.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("changes.list"), &list)) {
    return;
  }
  const VkrUiTrack header_columns[] = {
      column, {.value = 96.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig header = vkr_ui_panel_config_default();
  header.placement = list.placement;
  header.columns = header_columns;
  header.column_count = ArrayCount(header_columns);
  header.rows = &column;
  header.row_count = 1u;
  header.style.padding_pt = (VkrUiEdges){0};
  if (vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    char text[200];
    if (count) {
      snprintf(text, sizeof(text), "%u agent change%s to review", count,
               count == 1u ? "" : "s");
    } else {
      snprintf(text, sizeof(text), "No agent changes. %s",
               vkr_editor_agent_status(editor->agent));
    }
    VkrUiWidgetConfig label =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    label.placement = (VkrUiPlacement){.column = 0u,
                                       .row = 0u,
                                       .column_span = 1u,
                                       .row_span = 1u,
                                       .justify = VKR_UI_ALIGN_START,
                                       .align = VKR_UI_ALIGN_CENTER};
    vkr_ui_label(ui, string8_lit("summary"),
                 string8_create_from_cstr((const uint8_t *)text, strlen(text)),
                 &label);
    if (count && ops_row_button(ui, editor, string8_lit("accept_all"),
                                string8_lit("Accept all"), 1u, true_v)) {
      (void)vkr_editor_ops_accept(ops, 0u);
    }
    (void)vkr_ui_panel_end(ui);
  }
  const VkrUiTrack row_columns[] = {
      column,
      {.value = 72.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 72.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 72.0f, .unit = VKR_UI_TRACK_PX},
  };
  /* Rows act on a copy, because Accept removes the change it shows. */
  VkrEditorChange shown[VKR_EDITOR_CHANGE_MAX];
  for (uint32_t i = 0; i < count; ++i) {
    shown[i] = *vkr_editor_ops_change(ops, i);
  }
  for (uint32_t i = 0; i < count; ++i) {
    const VkrEditorChange *change = &shown[i];
    (void)vkr_ui_push_id_u64(ui, change->id);
    VkrUiPanelConfig row = vkr_ui_panel_config_default();
    row.placement = list.placement;
    row.placement.row = i + 1u;
    row.columns = row_columns;
    row.column_count = ArrayCount(row_columns);
    row.rows = &column;
    row.row_count = 1u;
    row.style.padding_pt = (VkrUiEdges){2.0f, 6.0f, 2.0f, 8.0f};
    row.style.border_pt = (VkrUiEdges){0.0f, 0.0f, 1.0f, 0.0f};
    row.style.border_color = theme->separator;
    if (vkr_ui_panel_begin(ui, string8_lit("row"), &row)) {
      char text[160];
      snprintf(text, sizeof(text), "%u  %s  (%u object%s)", change->id,
               change->label, change->entity_count,
               change->entity_count == 1u ? "" : "s");
      VkrUiWidgetConfig label =
          vkr_editor_text_config(theme->font_body, theme->text);
      label.placement = (VkrUiPlacement){.column = 0u,
                                         .row = 0u,
                                         .column_span = 1u,
                                         .row_span = 1u,
                                         .justify = VKR_UI_ALIGN_START,
                                         .align = VKR_UI_ALIGN_CENTER};
      vkr_ui_label(
          ui, string8_lit("label"),
          string8_create_from_cstr((const uint8_t *)text, strlen(text)),
          &label);
      if (ops_row_button(ui, editor, string8_lit("focus"), string8_lit("Focus"),
                         1u, false_v)) {
        ops_change_focus(frame, change);
      }
      if (ops_row_button(ui, editor, string8_lit("reject"),
                         string8_lit("Reject"), 2u, false_v)) {
        char request[160];
        snprintf(request, sizeof(request),
                 "{\"v\":1,\"id\":\"changes\",\"op\":\"changes.reject\","
                 "\"args\":{\"change\":%u}}",
                 change->id);
        (void)vkr_editor_agent_submit(editor->agent, request);
      }
      if (ops_row_button(ui, editor, string8_lit("accept"),
                         string8_lit("Accept"), 3u, true_v)) {
        (void)vkr_editor_ops_accept(ops, change->id);
      }
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}
