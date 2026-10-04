#include "editor_ops.h"

#include "editor_agent.h"
#include "editor_internal.h"
#include "editor_level.h"

#include "core/logger.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"
#include "filesystem/filesystem.h"
#include "level/vkr_brush.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/systems/vkr_scene_brush.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_terrain.h"
#include "renderer/systems/vkr_scene_types.h"
#include "script/vkr_io_router.h"
#include "vkr_bakery_buffer.h"

#include "stb_image.h"
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
  /* Select the first operation's entity once applied, as the editor's own
     tools do. */
  bool8_t select;
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

static bool8_t ops_in_box(Vec3 lo, Vec3 hi, Vec3 box_min, Vec3 box_max) {
  return lo.x <= box_max.x && hi.x >= box_min.x && lo.y <= box_max.y &&
         hi.y >= box_min.y && lo.z <= box_max.z && hi.z >= box_min.z;
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
  /* A created entity's id is known while the batch builds, so a later
     operation of the batch can reference it, as io.connect does. */
  if (action == VKR_SCENE_EDIT_CREATE) {
    vkr_scene_entity_ref_generate(&item->request.values.ref);
  }
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
  if ((values.fields & VKR_SCENE_EDIT_TRANSFORM) && ref.item < 0 &&
      vkr_scene_get_typed(ops_scene(ctx->frame, ref.container), ref.entity,
                          &vkr_scene_brush_face_type)) {
    return ops_fail(ctx, OPS_INVALID,
                    "A brush face moves with its brush; set its plane with "
                    "component.set brush_face");
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
        !vkr_scene_entity_is_part(scene, child) &&
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
  /* A brush's delete takes its faces along (vkr_scene_edit_delete). */
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
// Brushes (docs/proposals/level-design-toolkit.md, phase 1)
// =============================================================================

/* Brush corners snap to 1/16 m unless `grid` says otherwise. */
#define OPS_BRUSH_GRID 0.0625f
#define OPS_DEV_FLOOR "assets/materials/dev/dev_floor.mt"
#define OPS_DEV_WALL "assets/materials/dev/dev_wall.mt"
#define OPS_DEV_GRID "assets/materials/dev/dev_grid.mt"
#define OPS_DEV_TRIGGER "assets/materials/dev/dev_trigger.mt"
#define OPS_DEV_ORANGE "assets/materials/dev/dev_orange.mt"
#define OPS_DEV_BLUE "assets/materials/dev/dev_blue.mt"
#define OPS_DEV_CLIP "assets/materials/dev/dev_clip.mt"

/* Arguments every brush and blockout operation shares. */
typedef struct OpsBrushArgs {
  OpsRef parent;
  uint16_t container;
  SceneBrushRole role;
  float32_t grid;
  char material[SCENE_BRUSH_MATERIAL_CAPACITY];
  char name[128];
} OpsBrushArgs;

static float32_t ops_snap(float32_t value, float32_t grid) {
  return grid > 0.0f ? roundf(value / grid) * grid : value;
}

static Vec3 ops_snap3(Vec3 value, float32_t grid) {
  return vec3_new(ops_snap(value.x, grid), ops_snap(value.y, grid),
                  ops_snap(value.z, grid));
}

static bool8_t ops_arg_string(OpsContext *ctx, const VkrBakeryJson *args,
                              const char *key, char *out, uint32_t capacity) {
  String8 text = {0};
  if (!vkr_bakery_json_get(args, key)) {
    return true_v;
  }
  if (!vkr_bakery_json_get_string(args, key, &text) ||
      text.length >= capacity) {
    return ops_fail(ctx, OPS_INVALID, "'%s' must be a string under %u bytes",
                    key, capacity);
  }
  MemCopy(out, text.str, text.length);
  out[text.length] = '\0';
  return true_v;
}

static bool8_t ops_brush_args(OpsContext *ctx, const VkrBakeryJson *args,
                              OpsBatch *batch, const char *default_name,
                              OpsBrushArgs *out) {
  *out = (OpsBrushArgs){.parent = {.item = -1},
                        .role = SCENE_BRUSH_ROLE_SOLID,
                        .grid = OPS_BRUSH_GRID};
  snprintf(out->name, sizeof(out->name), "%s", default_name);
  const VkrBakeryJson *parent = vkr_bakery_json_get(args, "parent");
  if (parent && parent->type != VKR_BAKERY_JSON_NULL) {
    if (!ops_ref(ctx, batch, parent, "parent", &out->parent)) {
      return false_v;
    }
    out->container = out->parent.container;
  } else if (!ops_arg_container(ctx, args, &out->container)) {
    return false_v;
  }
  String8 role = {0};
  if (vkr_bakery_json_get_string(args, "role", &role)) {
    const char *const roles[] = {"solid", "visual", "clip", "trigger"};
    out->role = SCENE_BRUSH_ROLE_COUNT;
    for (uint32_t i = 0; i < ArrayCount(roles); ++i) {
      if (ops_equals(role, roles[i])) {
        out->role = (SceneBrushRole)i;
      }
    }
    if (out->role == SCENE_BRUSH_ROLE_COUNT) {
      return ops_fail(ctx, OPS_INVALID,
                      "'role' is solid, visual, clip or trigger");
    }
  }
  float64_t grid = OPS_BRUSH_GRID;
  if (ops_arg_number(args, "grid", &grid)) {
    if (!(grid >= 0.0) || grid > 64.0) {
      return ops_fail(ctx, OPS_INVALID, "'grid' is 0 (off) to 64 meters");
    }
    out->grid = (float32_t)grid;
  }
  if (out->role == SCENE_BRUSH_ROLE_TRIGGER) {
    snprintf(out->material, sizeof(out->material), "%s", OPS_DEV_TRIGGER);
  } else if (out->role == SCENE_BRUSH_ROLE_CLIP) {
    snprintf(out->material, sizeof(out->material), "%s", OPS_DEV_CLIP);
  }
  return ops_arg_string(ctx, args, "material", out->material,
                        sizeof(out->material)) &&
         ops_arg_string(ctx, args, "name", out->name, sizeof(out->name));
}

static void ops_face_name(Vec3 normal, uint32_t index, char *out,
                          uint32_t capacity) {
  const float32_t ax = fabsf(normal.x);
  const float32_t ay = fabsf(normal.y);
  const float32_t az = fabsf(normal.z);
  const float32_t largest = Max(ax, Max(ay, az));
  if (largest < 0.999f) {
    snprintf(out, capacity, "Face %u", index + 1u);
  } else if (ay == largest) {
    snprintf(out, capacity, "%s", normal.y > 0.0f ? "Top" : "Bottom");
  } else if (ax == largest) {
    snprintf(out, capacity, "%s", normal.x > 0.0f ? "East +X" : "West -X");
  } else {
    snprintf(out, capacity, "%s", normal.z > 0.0f ? "South +Z" : "North -Z");
  }
}

/* Appends an empty group entity and returns its item. */
static bool8_t ops_group_add(OpsContext *ctx, OpsBatch *batch,
                             const OpsBrushArgs *brush, const char *name,
                             Vec3 position, VkrQuat rotation,
                             uint32_t *out_item) {
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, brush->container, VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  item->request.parent = brush->parent.entity;
  item->parent_ref = brush->parent.item;
  VkrSceneEditValues *values = &item->request.values;
  values->fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM;
  snprintf(values->name, sizeof(values->name), "%s", name);
  values->position = position;
  values->rotation = rotation;
  values->scale = vec3_one();
  *out_item = batch->count - 1u;
  return true_v;
}

/* Appends one brush and its faces. `planes` lie in the brush's own space,
   whose origin sits at `position` in its parent's space; `parent_item`
   (or -1 for `brush->parent`) names the parent. The solid is validated
   first, so a bad brush fails before anything is submitted. */
static bool8_t ops_brush_add(OpsContext *ctx, OpsBatch *batch,
                             const OpsBrushArgs *brush, int32_t parent_item,
                             const char *name, Vec3 position, VkrQuat rotation,
                             const VkrBrushPlane *planes, uint32_t count,
                             const char *const *materials, uint32_t *out_item) {
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  uint32_t failed = UINT32_MAX;
  if (!geometry) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  const VkrBrushError error = vkr_brush_build(planes, count, geometry, &failed);
  if (error != VKR_BRUSH_OK) {
    return ops_fail(ctx, OPS_INVALID, "Brush '%s': %s", name,
                    vkr_brush_error_text(error));
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, brush->container, VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  if (parent_item >= 0) {
    item->parent_ref = parent_item;
  } else {
    item->request.parent = brush->parent.entity;
    item->parent_ref = brush->parent.item;
  }
  VkrSceneEditValues *values = &item->request.values;
  values->fields =
      VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM | VKR_SCENE_EDIT_COMPONENT;
  snprintf(values->name, sizeof(values->name), "%s", name);
  values->position = position;
  values->rotation = rotation;
  values->scale = vec3_one();
  values->component_type = &vkr_scene_brush_type;
  *(SceneBrushSettings *)values->component =
      (SceneBrushSettings){.role = brush->role};
  const uint32_t brush_item = batch->count - 1u;
  for (uint32_t i = 0; i < count; ++i) {
    VkrSampleEditBatchItem *face =
        ops_batch_add(ctx, batch, brush->container, VKR_SCENE_EDIT_CREATE);
    if (!face) {
      return false_v;
    }
    face->parent_ref = (int32_t)brush_item;
    VkrSceneEditValues *face_values = &face->request.values;
    face_values->fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM |
                          VKR_SCENE_EDIT_COMPONENT;
    ops_face_name(planes[i].normal, i, face_values->name,
                  sizeof(face_values->name));
    face_values->rotation = vkr_quat_identity();
    face_values->scale = vec3_one();
    face_values->component_type = &vkr_scene_brush_face_type;
    SceneBrushFace *settings = (SceneBrushFace *)face_values->component;
    vkr_type_defaults(&vkr_scene_brush_face_type, settings);
    settings->normal = planes[i].normal;
    settings->distance = planes[i].distance;
    const char *material =
        materials && materials[i] ? materials[i] : brush->material;
    snprintf(settings->material, sizeof(settings->material), "%s", material);
  }
  *out_item = brush_item;
  return true_v;
}

/* min and max corners, snapped, with every side at least one grid step. */
static bool8_t ops_arg_box(OpsContext *ctx, const VkrBakeryJson *args,
                           float32_t grid, Vec3 *out_min, Vec3 *out_max) {
  bool8_t has_min = false_v;
  bool8_t has_max = false_v;
  if (!ops_arg_vec3(ctx, args, "min", out_min, &has_min) ||
      !ops_arg_vec3(ctx, args, "max", out_max, &has_max)) {
    return false_v;
  }
  if (!has_min || !has_max) {
    return ops_fail(ctx, OPS_INVALID, "'min' and 'max' corners are required");
  }
  Vec3 lo = ops_snap3(*out_min, grid);
  Vec3 hi = ops_snap3(*out_max, grid);
  *out_min = vec3_new(Min(lo.x, hi.x), Min(lo.y, hi.y), Min(lo.z, hi.z));
  *out_max = vec3_new(Max(lo.x, hi.x), Max(lo.y, hi.y), Max(lo.z, hi.z));
  const Vec3 size = vec3_sub(*out_max, *out_min);
  if (size.x < 1.0e-3f || size.y < 1.0e-3f || size.z < 1.0e-3f) {
    return ops_fail(ctx, OPS_INVALID, "Every side of the box must be positive");
  }
  return true_v;
}

/* A box brush between parent-space corners, its origin at the center. */
static bool8_t ops_box_add(OpsContext *ctx, OpsBatch *batch,
                           const OpsBrushArgs *brush, int32_t parent_item,
                           const char *name, Vec3 min, Vec3 max,
                           const char *material, uint32_t *out_item) {
  const Vec3 center = vec3_scale(vec3_add(min, max), 0.5f);
  VkrBrushPlane planes[6];
  const uint32_t count = vkr_brush_box_planes(vec3_sub(min, center),
                                              vec3_sub(max, center), planes);
  const char *materials[6] = {material, material, material,
                              material, material, material};
  return ops_brush_add(ctx, batch, brush, parent_item, name, center,
                       vkr_quat_identity(), planes, count,
                       material ? materials : NULL, out_item);
}

static bool8_t ops_build_brush_box(OpsContext *ctx, const VkrBakeryJson *args,
                                   OpsBatch *batch) {
  OpsBrushArgs brush;
  Vec3 min = {0};
  Vec3 max = {0};
  uint32_t item = 0u;
  if (!ops_brush_args(ctx, args, batch, "Brush", &brush) ||
      !ops_arg_box(ctx, args, brush.grid, &min, &max) ||
      !ops_box_add(ctx, batch, &brush, -1, brush.name, min, max, NULL, &item)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = item;
  return true_v;
}

static bool8_t ops_build_brush_wedge(OpsContext *ctx, const VkrBakeryJson *args,
                                     OpsBatch *batch) {
  OpsBrushArgs brush;
  Vec3 min = {0};
  Vec3 max = {0};
  if (!ops_brush_args(ctx, args, batch, "Wedge", &brush) ||
      !ops_arg_box(ctx, args, brush.grid, &min, &max)) {
    return false_v;
  }
  uint32_t slope = 0u;
  String8 direction = {0};
  if (vkr_bakery_json_get_string(args, "slope", &direction)) {
    const char *const directions[] = {"+x", "-x", "+z", "-z"};
    slope = UINT32_MAX;
    for (uint32_t i = 0; i < ArrayCount(directions); ++i) {
      if (ops_equals(direction, directions[i])) {
        slope = i;
      }
    }
    if (slope == UINT32_MAX) {
      return ops_fail(ctx, OPS_INVALID,
                      "'slope' is the low side: +x, -x, +z or -z");
    }
  }
  const Vec3 center = vec3_scale(vec3_add(min, max), 0.5f);
  VkrBrushPlane planes[8];
  const uint32_t count = vkr_brush_wedge_planes(
      vec3_sub(min, center), vec3_sub(max, center), slope, planes);
  uint32_t item = 0u;
  if (!ops_brush_add(ctx, batch, &brush, -1, brush.name, center,
                     vkr_quat_identity(), planes, count, NULL, &item)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = item;
  return true_v;
}

static bool8_t ops_build_brush_cylinder(OpsContext *ctx,
                                        const VkrBakeryJson *args,
                                        OpsBatch *batch) {
  OpsBrushArgs brush;
  Vec3 center = {0};
  bool8_t has_center = false_v;
  if (!ops_brush_args(ctx, args, batch, "Cylinder", &brush) ||
      !ops_arg_vec3(ctx, args, "center", &center, &has_center)) {
    return false_v;
  }
  float64_t radius = 0.0;
  float64_t height = 0.0;
  float64_t sides = 12.0;
  (void)ops_arg_number(args, "sides", &sides);
  if (!has_center || !ops_arg_number(args, "radius", &radius) ||
      !ops_arg_number(args, "height", &height) || !(radius > 0.0) ||
      !(height > 0.0) || sides < 3.0 || sides > 32.0) {
    return ops_fail(ctx, OPS_INVALID,
                    "brush.cylinder needs center (bottom), positive radius "
                    "and height, and sides 3 to 32");
  }
  center = ops_snap3(center, brush.grid);
  VkrBrushPlane planes[34];
  const uint32_t count = vkr_brush_cylinder_planes(
      vec3_zero(), (float32_t)radius, ops_snap((float32_t)height, brush.grid),
      (uint32_t)sides, planes);
  uint32_t item = 0u;
  if (!ops_brush_add(ctx, batch, &brush, -1, brush.name, center,
                     vkr_quat_identity(), planes, count, NULL, &item)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = item;
  return true_v;
}

/* A rotation about +Y that turns local +Z toward the horizontal part of
   `direction`. */
static VkrQuat ops_yaw_toward(Vec3 direction) {
  const float32_t yaw = atan2f(direction.x, direction.z);
  return vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), yaw);
}

static bool8_t ops_build_brush_stairs(OpsContext *ctx,
                                      const VkrBakeryJson *args,
                                      OpsBatch *batch) {
  OpsBrushArgs brush;
  Vec3 from = {0};
  Vec3 to = {0};
  bool8_t has_from = false_v;
  bool8_t has_to = false_v;
  if (!ops_brush_args(ctx, args, batch, "Stairs", &brush) ||
      !ops_arg_vec3(ctx, args, "from", &from, &has_from) ||
      !ops_arg_vec3(ctx, args, "to", &to, &has_to)) {
    return false_v;
  }
  float64_t width = 1.5;
  float64_t step = 0.1875;
  (void)ops_arg_number(args, "width", &width);
  (void)ops_arg_number(args, "step_height", &step);
  from = ops_snap3(from, brush.grid);
  to = ops_snap3(to, brush.grid);
  const Vec3 delta = vec3_sub(to, from);
  const float32_t run = sqrtf(delta.x * delta.x + delta.z * delta.z);
  if (!has_from || !has_to || delta.y <= 0.0f || run <= 0.0f ||
      !(width > 0.0) || !(step > 0.0)) {
    return ops_fail(ctx, OPS_INVALID,
                    "brush.stairs needs 'from' (bottom front center), a "
                    "higher 'to' (top back center) apart horizontally, and "
                    "positive width and step_height");
  }
  const uint32_t steps =
      (uint32_t)Min(32.0f, Max(1.0f, ceilf(delta.y / (float32_t)step)));
  const float32_t rise = delta.y / (float32_t)steps;
  const float32_t depth = run / (float32_t)steps;
  const float32_t half = (float32_t)width * 0.5f;
  uint32_t group = 0u;
  if (!ops_group_add(ctx, batch, &brush, brush.name, from,
                     ops_yaw_toward(delta), &group)) {
    return false_v;
  }
  /* Solid steps: each rises from the floor to its tread. */
  for (uint32_t i = 0; i < steps; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "Step %u", i + 1u);
    uint32_t item = 0u;
    if (!ops_box_add(ctx, batch, &brush, (int32_t)group, name,
                     vec3_new(-half, 0.0f, depth * (float32_t)i),
                     vec3_new(half, rise * (float32_t)(i + 1u),
                              depth * (float32_t)(i + 1u)),
                     NULL, &item)) {
      return false_v;
    }
  }
  batch->op_item[batch->op_count] = group;
  return true_v;
}

/* Box walls, floor and ceiling around an interior box in group space; the
   group sits at the floor's center. */
static bool8_t ops_room_shell(OpsContext *ctx, OpsBatch *batch,
                              const OpsBrushArgs *brush, uint32_t group,
                              Vec3 min, Vec3 max, float32_t wall,
                              bool8_t ceiling, bool8_t ends,
                              const char *wall_material,
                              const char *floor_material) {
  uint32_t item = 0u;
  const float32_t t = wall;
  if (!ops_box_add(ctx, batch, brush, (int32_t)group, "Floor",
                   vec3_new(min.x - t, min.y - t, min.z - (ends ? t : 0.0f)),
                   vec3_new(max.x + t, min.y, max.z + (ends ? t : 0.0f)),
                   floor_material, &item) ||
      (ceiling &&
       !ops_box_add(ctx, batch, brush, (int32_t)group, "Ceiling",
                    vec3_new(min.x - t, max.y, min.z - (ends ? t : 0.0f)),
                    vec3_new(max.x + t, max.y + t, max.z + (ends ? t : 0.0f)),
                    brush->material[0] ? brush->material : OPS_DEV_GRID,
                    &item)) ||
      !ops_box_add(ctx, batch, brush, (int32_t)group, "Wall West -X",
                   vec3_new(min.x - t, min.y, min.z),
                   vec3_new(min.x, max.y, max.z), wall_material, &item) ||
      !ops_box_add(ctx, batch, brush, (int32_t)group, "Wall East +X",
                   vec3_new(max.x, min.y, min.z),
                   vec3_new(max.x + t, max.y, max.z), wall_material, &item)) {
    return false_v;
  }
  if (!ends) {
    return true_v;
  }
  return ops_box_add(ctx, batch, brush, (int32_t)group, "Wall North -Z",
                     vec3_new(min.x - t, min.y, min.z - t),
                     vec3_new(max.x + t, max.y, min.z), wall_material, &item) &&
         ops_box_add(ctx, batch, brush, (int32_t)group, "Wall South +Z",
                     vec3_new(min.x - t, min.y, max.z),
                     vec3_new(max.x + t, max.y, max.z + t), wall_material,
                     &item);
}

static bool8_t ops_build_room(OpsContext *ctx, const VkrBakeryJson *args,
                              OpsBatch *batch) {
  OpsBrushArgs brush;
  Vec3 min = {0};
  Vec3 size = {0};
  bool8_t has_min = false_v;
  bool8_t has_size = false_v;
  char floor_material[SCENE_BRUSH_MATERIAL_CAPACITY] = OPS_DEV_FLOOR;
  if (!ops_brush_args(ctx, args, batch, "Room", &brush) ||
      !ops_arg_vec3(ctx, args, "min", &min, &has_min) ||
      !ops_arg_vec3(ctx, args, "size", &size, &has_size) ||
      !ops_arg_string(ctx, args, "floor_material", floor_material,
                      sizeof(floor_material))) {
    return false_v;
  }
  float64_t wall = 0.25;
  (void)ops_arg_number(args, "wall", &wall);
  min = ops_snap3(min, brush.grid);
  size = ops_snap3(size, brush.grid);
  if (!has_min || !has_size || size.x <= 0.0f || size.y <= 0.0f ||
      size.z <= 0.0f || !(wall > 0.0)) {
    return ops_fail(ctx, OPS_INVALID,
                    "blockout.room needs 'min' (interior corner), a positive "
                    "interior 'size' and a positive 'wall' thickness");
  }
  const Vec3 base =
      vec3_new(min.x + size.x * 0.5f, min.y, min.z + size.z * 0.5f);
  uint32_t group = 0u;
  if (!ops_group_add(ctx, batch, &brush, brush.name, base, vkr_quat_identity(),
                     &group)) {
    return false_v;
  }
  const char *wall_material = brush.material[0] ? brush.material : OPS_DEV_WALL;
  const Vec3 half = vec3_new(size.x * 0.5f, 0.0f, size.z * 0.5f);
  if (!ops_room_shell(ctx, batch, &brush, group,
                      vec3_new(-half.x, 0.0f, -half.z),
                      vec3_new(half.x, size.y, half.z),
                      ops_snap((float32_t)wall, brush.grid),
                      ops_arg_bool(args, "ceiling", true_v), true_v,
                      wall_material, floor_material)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = group;
  return true_v;
}

static bool8_t ops_build_corridor(OpsContext *ctx, const VkrBakeryJson *args,
                                  OpsBatch *batch) {
  OpsBrushArgs brush;
  Vec3 from = {0};
  Vec3 to = {0};
  bool8_t has_from = false_v;
  bool8_t has_to = false_v;
  char floor_material[SCENE_BRUSH_MATERIAL_CAPACITY] = OPS_DEV_FLOOR;
  if (!ops_brush_args(ctx, args, batch, "Corridor", &brush) ||
      !ops_arg_vec3(ctx, args, "from", &from, &has_from) ||
      !ops_arg_vec3(ctx, args, "to", &to, &has_to) ||
      !ops_arg_string(ctx, args, "floor_material", floor_material,
                      sizeof(floor_material))) {
    return false_v;
  }
  float64_t width = 2.0;
  float64_t height = 3.0;
  float64_t wall = 0.25;
  (void)ops_arg_number(args, "width", &width);
  (void)ops_arg_number(args, "height", &height);
  (void)ops_arg_number(args, "wall", &wall);
  from = ops_snap3(from, brush.grid);
  to = ops_snap3(to, brush.grid);
  const Vec3 delta = vec3_sub(to, from);
  const float32_t length = sqrtf(delta.x * delta.x + delta.z * delta.z);
  if (!has_from || !has_to || length <= 0.0f || !(width > 0.0) ||
      !(height > 0.0) || !(wall > 0.0)) {
    return ops_fail(ctx, OPS_INVALID,
                    "blockout.corridor needs 'from' and 'to' floor points "
                    "apart horizontally and positive width, height and "
                    "wall");
  }
  uint32_t group = 0u;
  if (!ops_group_add(ctx, batch, &brush, brush.name, from,
                     ops_yaw_toward(delta), &group)) {
    return false_v;
  }
  const float32_t half = (float32_t)width * 0.5f;
  if (!ops_room_shell(
          ctx, batch, &brush, group, vec3_new(-half, 0.0f, 0.0f),
          vec3_new(half, (float32_t)height, length), (float32_t)wall,
          ops_arg_bool(args, "ceiling", true_v), false_v,
          brush.material[0] ? brush.material : OPS_DEV_WALL, floor_material)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = group;
  return true_v;
}

/* The local box of an axis-aligned six-face brush. */
static bool8_t ops_brush_box_of(const VkrScene *scene, VkrEntityId brush,
                                Vec3 *out_min, Vec3 *out_max, char *material,
                                uint32_t capacity) {
  VkrEntityId faces[8];
  if (vkr_scene_brush_faces(scene, brush, faces, ArrayCount(faces)) != 6u) {
    return false_v;
  }
  Vec3 lo = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  Vec3 hi = vec3_new(INFINITY, INFINITY, INFINITY);
  material[0] = '\0';
  for (uint32_t i = 0; i < 6u; ++i) {
    const SceneBrushFace *face =
        vkr_scene_get_typed(scene, faces[i], &vkr_scene_brush_face_type);
    const Vec3 n = vec3_normalize(face->normal);
    const float32_t d = face->distance / vec3_length(face->normal);
    if (!material[0]) {
      snprintf(material, capacity, "%s", face->material);
    }
    if (n.x > 0.999f) {
      hi.x = d;
    } else if (n.x < -0.999f) {
      lo.x = -d;
    } else if (n.y > 0.999f) {
      hi.y = d;
    } else if (n.y < -0.999f) {
      lo.y = -d;
    } else if (n.z > 0.999f) {
      hi.z = d;
    } else if (n.z < -0.999f) {
      lo.z = -d;
    } else {
      return false_v;
    }
  }
  *out_min = lo;
  *out_max = hi;
  return isfinite(lo.x) && isfinite(lo.y) && isfinite(lo.z) && isfinite(hi.x) &&
         isfinite(hi.y) && isfinite(hi.z);
}

static bool8_t ops_build_doorway(OpsContext *ctx, const VkrBakeryJson *args,
                                 OpsBatch *batch) {
  OpsRef wall;
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "wall"), "wall", &wall)) {
    return false_v;
  }
  const VkrScene *scene = ops_scene(ctx->frame, wall.container);
  if (wall.item >= 0 ||
      !vkr_scene_get_typed(scene, wall.entity, &vkr_scene_brush_type)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'wall' must name an existing box brush; create it in an "
                    "earlier request");
  }
  Vec3 lo = {0};
  Vec3 hi = {0};
  char material[SCENE_BRUSH_MATERIAL_CAPACITY];
  const SceneTransform *transform = ops_transform(scene, wall.entity);
  if (!ops_brush_box_of(scene, wall.entity, &lo, &hi, material,
                        sizeof(material)) ||
      !transform ||
      vec3_length(vec3_sub(transform->scale, vec3_one())) > 1.0e-4f) {
    return ops_fail(ctx, OPS_INVALID,
                    "A doorway cuts an unscaled, axis-aligned box brush");
  }
  float64_t offset = 0.0;
  float64_t width = 1.0;
  float64_t height = 2.125;
  (void)ops_arg_number(args, "offset", &offset);
  (void)ops_arg_number(args, "width", &width);
  (void)ops_arg_number(args, "height", &height);
  /* The opening crosses the thinner horizontal axis. */
  const bool8_t along_x = (hi.x - lo.x) >= (hi.z - lo.z);
  const float32_t long_lo = along_x ? lo.x : lo.z;
  const float32_t long_hi = along_x ? hi.x : hi.z;
  const float32_t center = (long_lo + long_hi) * 0.5f + (float32_t)offset;
  const float32_t open_lo = center - (float32_t)width * 0.5f;
  const float32_t open_hi = center + (float32_t)width * 0.5f;
  const float32_t top = lo.y + (float32_t)height;
  if (!(width > 0.0) || !(height > 0.0) || open_lo <= long_lo + 1.0e-3f ||
      open_hi >= long_hi - 1.0e-3f || top > hi.y + 1.0e-3f) {
    return ops_fail(ctx, OPS_INVALID,
                    "The opening must fit inside the wall: %.3f m long and "
                    "%.3f m high",
                    long_hi - long_lo, hi.y - lo.y);
  }
  OpsBrushArgs brush = {.parent = {.item = -1},
                        .container = wall.container,
                        .role = SCENE_BRUSH_ROLE_SOLID};
  brush.parent.entity = transform->parent;
  brush.parent.container = wall.container;
  snprintf(brush.material, sizeof(brush.material), "%s", material);
  /* Pieces in the wall's space, placed in its parent's space. */
  Vec3 piece_lo[3];
  Vec3 piece_hi[3];
  uint32_t pieces = 0u;
  const char *names[3] = {"Wall left", "Wall right", "Lintel"};
  if (along_x) {
    piece_lo[pieces] = lo;
    piece_hi[pieces++] = vec3_new(open_lo, hi.y, hi.z);
    piece_lo[pieces] = vec3_new(open_hi, lo.y, lo.z);
    piece_hi[pieces++] = hi;
    piece_lo[pieces] = vec3_new(open_lo, top, lo.z);
    piece_hi[pieces++] = vec3_new(open_hi, hi.y, hi.z);
  } else {
    piece_lo[pieces] = lo;
    piece_hi[pieces++] = vec3_new(hi.x, hi.y, open_lo);
    piece_lo[pieces] = vec3_new(lo.x, lo.y, open_hi);
    piece_hi[pieces++] = hi;
    piece_lo[pieces] = vec3_new(lo.x, top, open_lo);
    piece_hi[pieces++] = vec3_new(hi.x, hi.y, open_hi);
  }
  uint32_t first = UINT32_MAX;
  for (uint32_t i = 0; i < pieces; ++i) {
    if (piece_hi[i].y - piece_lo[i].y < 1.0e-3f) {
      continue;
    }
    const Vec3 local_center =
        vec3_scale(vec3_add(piece_lo[i], piece_hi[i]), 0.5f);
    VkrBrushPlane planes[6];
    const uint32_t count =
        vkr_brush_box_planes(vec3_sub(piece_lo[i], local_center),
                             vec3_sub(piece_hi[i], local_center), planes);
    const Vec3 position =
        vec3_add(transform->position,
                 vkr_quat_rotate_vec3(transform->rotation, local_center));
    uint32_t item = 0u;
    if (!ops_brush_add(ctx, batch, &brush, -1, names[i], position,
                       transform->rotation, planes, count, NULL, &item)) {
      return false_v;
    }
    first = first == UINT32_MAX ? item : first;
  }
  if (!ops_delete_tree(ctx, batch, scene, wall.entity, true_v, 0u)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = first;
  return true_v;
}

/* Face selectors of brush.set_material: top, bottom, +x, -x, +z, -z. */
static bool8_t ops_face_matches(const SceneBrushFace *face,
                                const VkrBakeryJson *selectors) {
  if (!selectors) {
    return true_v;
  }
  const Vec3 n = vec3_normalize(face->normal);
  for (const VkrBakeryJson *s = selectors->first; s; s = s->next) {
    if ((vkr_bakery_json_is_string(s, "top") && n.y > 0.7f) ||
        (vkr_bakery_json_is_string(s, "bottom") && n.y < -0.7f) ||
        (vkr_bakery_json_is_string(s, "+x") && n.x > 0.7f) ||
        (vkr_bakery_json_is_string(s, "-x") && n.x < -0.7f) ||
        (vkr_bakery_json_is_string(s, "+z") && n.z > 0.7f) ||
        (vkr_bakery_json_is_string(s, "-z") && n.z < -0.7f) ||
        (vkr_bakery_json_is_string(s, "sides") && fabsf(n.y) < 0.7f)) {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t ops_build_set_material(OpsContext *ctx,
                                      const VkrBakeryJson *args,
                                      OpsBatch *batch) {
  OpsRef ref;
  char material[SCENE_BRUSH_MATERIAL_CAPACITY] = "";
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "brush"), "brush", &ref) ||
      !ops_arg_string(ctx, args, "material", material, sizeof(material))) {
    return false_v;
  }
  const VkrBakeryJson *selectors = vkr_bakery_json_get(args, "faces");
  if (selectors && selectors->type != VKR_BAKERY_JSON_ARRAY) {
    return ops_fail(ctx, OPS_INVALID,
                    "'faces' lists top, bottom, sides, +x, -x, +z or -z");
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  if (ref.item >= 0 ||
      !vkr_scene_get_typed(scene, ref.entity, &vkr_scene_brush_type)) {
    return ops_fail(ctx, OPS_INVALID, "'brush' must name an existing brush");
  }
  VkrEntityId faces[VKR_BRUSH_FACE_MAX];
  const uint32_t count =
      Min(vkr_scene_brush_faces(scene, ref.entity, faces, ArrayCount(faces)),
          (uint32_t)ArrayCount(faces));
  uint32_t changed = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    const SceneBrushFace *face =
        vkr_scene_get_typed(scene, faces[i], &vkr_scene_brush_face_type);
    if (!face || !ops_face_matches(face, selectors)) {
      continue;
    }
    VkrSampleEditBatchItem *item =
        ops_batch_add(ctx, batch, ref.container, VKR_SCENE_EDIT_APPLY);
    if (!item) {
      return false_v;
    }
    item->request.entity = faces[i];
    item->request.values.fields = VKR_SCENE_EDIT_COMPONENT;
    item->request.values.component_type = &vkr_scene_brush_face_type;
    SceneBrushFace *value = (SceneBrushFace *)item->request.values.component;
    *value = *face;
    snprintf(value->material, sizeof(value->material), "%s", material);
    changed++;
  }
  if (!changed) {
    return ops_fail(ctx, OPS_NOT_FOUND, "No face of the brush matched");
  }
  batch->op_target[batch->op_count] = ref.entity;
  return true_v;
}

// -----------------------------------------------------------------------------
// Brush editing (docs/proposals/level-design-toolkit.md, phase 2)
// -----------------------------------------------------------------------------

/* A brush as its faces read it: planes, face entities and their values. */
typedef struct OpsBrushRead {
  VkrEntityId entity;
  VkrEntityId parent;
  const SceneTransform *transform;
  SceneBrushRole role;
  uint32_t count;
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  VkrEntityId faces[VKR_BRUSH_FACE_MAX];
  SceneBrushFace values[VKR_BRUSH_FACE_MAX];
} OpsBrushRead;

static bool8_t ops_brush_read(OpsContext *ctx, const VkrScene *scene,
                              VkrEntityId entity, OpsBrushRead *out) {
  const SceneBrushSettings *settings =
      vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type);
  const SceneTransform *transform = ops_transform(scene, entity);
  if (!settings || !transform) {
    return ops_fail(ctx, OPS_INVALID, "That entity is not a brush");
  }
  out->entity = entity;
  out->parent = transform->parent;
  out->transform = transform;
  out->role = settings->role;
  const uint32_t total =
      vkr_scene_brush_faces(scene, entity, out->faces, VKR_BRUSH_FACE_MAX);
  if (total > VKR_BRUSH_FACE_MAX) {
    return ops_fail(ctx, OPS_INVALID, "The brush has more than %u faces",
                    VKR_BRUSH_FACE_MAX);
  }
  out->count = total;
  for (uint32_t i = 0; i < total; ++i) {
    out->values[i] = *(const SceneBrushFace *)vkr_scene_get_typed(
        scene, out->faces[i], &vkr_scene_brush_face_type);
    out->planes[i] = (VkrBrushPlane){.normal = out->values[i].normal,
                                     .distance = out->values[i].distance};
  }
  return true_v;
}

static bool8_t ops_brush_arg(OpsContext *ctx, OpsBatch *batch,
                             const VkrBakeryJson *args, const char *key,
                             OpsBrushRead *out) {
  OpsRef ref;
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, key), key, &ref)) {
    return false_v;
  }
  if (ref.item >= 0) {
    return ops_fail(ctx, OPS_INVALID,
                    "'%s' must exist before this request; edit it in a later "
                    "one",
                    key);
  }
  return ops_brush_read(ctx, ops_scene(ctx->frame, ref.container), ref.entity,
                        out);
}

/* A face entity named directly, or by `brush` and `side`. */
static bool8_t ops_face_arg(OpsContext *ctx, OpsBatch *batch,
                            const VkrBakeryJson *args, OpsBrushRead *brush,
                            uint32_t *out_face) {
  const VkrBakeryJson *face = vkr_bakery_json_get(args, "face");
  if (face) {
    OpsRef ref;
    if (!ops_ref(ctx, batch, face, "face", &ref) || ref.item >= 0) {
      return ref.item >= 0 ? ops_fail(ctx, OPS_INVALID,
                                      "'face' must exist before this request")
                           : false_v;
    }
    const VkrScene *scene = ops_scene(ctx->frame, ref.container);
    if (!vkr_scene_get_typed(scene, ref.entity, &vkr_scene_brush_face_type) ||
        !ops_brush_read(ctx, scene, ops_parent(scene, ref.entity), brush)) {
      return ops_fail(ctx, OPS_INVALID, "'face' must name a brush face");
    }
    for (uint32_t i = 0; i < brush->count; ++i) {
      if (brush->faces[i].u64 == ref.entity.u64) {
        *out_face = i;
        return true_v;
      }
    }
    return ops_fail(ctx, OPS_NOT_FOUND, "The face is not on its brush");
  }
  if (!ops_brush_arg(ctx, batch, args, "brush", brush)) {
    return false_v;
  }
  VkrBakeryJson *selectors = vkr_bakery_json_array(ops_arena(ctx));
  const VkrBakeryJson *side = vkr_bakery_json_get(args, "side");
  if (!side || side->type != VKR_BAKERY_JSON_STRING) {
    return ops_fail(ctx, OPS_INVALID,
                    "Name a 'face', or a 'brush' and a 'side' (top, bottom, "
                    "+x, -x, +z, -z)");
  }
  vkr_bakery_json_append(selectors,
                         vkr_bakery_json_clone(ops_arena(ctx), side));
  /* The face whose normal points most along the side. */
  float32_t best = 0.7f;
  *out_face = UINT32_MAX;
  for (uint32_t i = 0; i < brush->count; ++i) {
    if (!ops_face_matches(&brush->values[i], selectors)) {
      continue;
    }
    const Vec3 n = vec3_normalize(brush->values[i].normal);
    const float32_t score = Max(Max(fabsf(n.x), fabsf(n.y)), fabsf(n.z));
    if (score >= best) {
      best = score;
      *out_face = i;
    }
  }
  return *out_face != UINT32_MAX
             ? true_v
             : ops_fail(ctx, OPS_NOT_FOUND, "No face of the brush faces '%s'",
                        side->string.length ? (const char *)side->string.str
                                            : "");
}

/* Replaces `brush` with `pieces`, which lie in its space; each piece face
   copies the material and projection of the face its source names (target
   faces first, then `extra` faces), or `fallback` for a new plane. Returns
   the first new brush's item, or UINT32_MAX when there are no pieces. */
static bool8_t
ops_brush_replace(OpsContext *ctx, OpsBatch *batch, const OpsBrushRead *brush,
                  const VkrBrushPiece *pieces, uint32_t piece_count,
                  const SceneBrushFace *extra, uint32_t extra_count,
                  const SceneBrushFace *fallback, bool8_t delete_original,
                  const char *suffix, uint32_t *out_item) {
  const VkrScene *scene = ops_scene(ctx->frame, brush->entity.parts.world);
  OpsBrushArgs args = {.parent = {.entity = brush->parent, .item = -1},
                       .container = brush->entity.parts.world,
                       .role = brush->role};
  *out_item = UINT32_MAX;
  const String8 base = vkr_scene_get_name(scene, brush->entity);
  for (uint32_t p = 0; p < piece_count; ++p) {
    const char *materials[VKR_BRUSH_FACE_MAX];
    const SceneBrushFace *sources[VKR_BRUSH_FACE_MAX];
    for (uint32_t f = 0; f < pieces[p].count; ++f) {
      const uint32_t source = pieces[p].source[f];
      sources[f] = source < brush->count ? &brush->values[source]
                   : source != VKR_BRUSH_SOURCE_NEW &&
                           source - brush->count < extra_count
                       ? &extra[source - brush->count]
                       : fallback;
      materials[f] = sources[f]->material;
    }
    /* Pieces of one brush read as its parts unless the caller names them. */
    char name[96];
    snprintf(name, sizeof(name), "%.*s%s", (int)Min(base.length, 80u),
             (const char *)base.str,
             suffix             ? suffix
             : piece_count > 1u ? " part"
                                : "");
    uint32_t item = 0u;
    if (!ops_brush_add(ctx, batch, &args, -1, name, brush->transform->position,
                       brush->transform->rotation, pieces[p].planes,
                       pieces[p].count, materials, &item)) {
      return false_v;
    }
    /* Carry each face's texture projection along with its material. */
    for (uint32_t f = 0; f < pieces[p].count; ++f) {
      SceneBrushFace *face = (SceneBrushFace *)ctx->ops->items[item + 1u + f]
                                 .request.values.component;
      face->uv_offset = sources[f]->uv_offset;
      face->uv_scale = sources[f]->uv_scale;
      face->uv_rotation = sources[f]->uv_rotation;
      face->uv_world = sources[f]->uv_world;
    }
    *out_item = *out_item == UINT32_MAX ? item : *out_item;
  }
  if (!delete_original) {
    return true_v;
  }
  VkrSampleEditBatchItem *item = ops_batch_add(
      ctx, batch, brush->entity.parts.world, VKR_SCENE_EDIT_DELETE);
  if (!item) {
    return false_v;
  }
  item->request.entity = brush->entity;
  return true_v;
}

static bool8_t ops_brush_unscaled(OpsContext *ctx, const OpsBrushRead *brush) {
  return vec3_length(vec3_sub(brush->transform->scale, vec3_one())) < 1.0e-4f
             ? true_v
             : ops_fail(ctx, OPS_INVALID,
                        "Editing needs an unscaled brush; move its faces "
                        "instead of scaling it");
}

static bool8_t ops_build_move_face(OpsContext *ctx, const VkrBakeryJson *args,
                                   OpsBatch *batch) {
  OpsBrushRead *brush =
      arena_alloc(ops_arena(ctx), sizeof(*brush), ARENA_MEMORY_TAG_STRUCT);
  uint32_t face = 0u;
  float64_t distance = 0.0;
  if (!brush || !ops_face_arg(ctx, batch, args, brush, &face)) {
    return brush ? false_v : ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  if (!ops_arg_number(args, "distance", &distance) || !isfinite(distance)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'distance' in meters moves the face along its normal");
  }
  /* The plane's normal may not be unit length. */
  const float32_t length = vec3_length(brush->planes[face].normal);
  brush->planes[face].distance += (float32_t)distance * length;
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  uint32_t failed = UINT32_MAX;
  const VkrBrushError error =
      geometry ? vkr_brush_build(brush->planes, brush->count, geometry, &failed)
               : VKR_BRUSH_ERROR_FLAT;
  if (error != VKR_BRUSH_OK) {
    return ops_fail(ctx, OPS_INVALID,
                    "Moving the face that far breaks the brush: %s",
                    vkr_brush_error_text(error));
  }
  VkrSampleEditBatchItem *item = ops_batch_add(
      ctx, batch, brush->entity.parts.world, VKR_SCENE_EDIT_APPLY);
  if (!item) {
    return false_v;
  }
  item->request.entity = brush->faces[face];
  item->request.values.fields = VKR_SCENE_EDIT_COMPONENT;
  item->request.values.component_type = &vkr_scene_brush_face_type;
  SceneBrushFace *value = (SceneBrushFace *)item->request.values.component;
  *value = brush->values[face];
  value->distance = brush->planes[face].distance;
  batch->op_target[batch->op_count] = brush->faces[face];
  return true_v;
}

static bool8_t ops_build_extrude(OpsContext *ctx, const VkrBakeryJson *args,
                                 OpsBatch *batch) {
  OpsBrushRead *brush =
      arena_alloc(ops_arena(ctx), sizeof(*brush), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushPiece *piece =
      arena_alloc(ops_arena(ctx), sizeof(*piece), ARENA_MEMORY_TAG_STRUCT);
  uint32_t face = 0u;
  float64_t distance = 0.0;
  if (!brush || !geometry || !piece) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  if (!ops_face_arg(ctx, batch, args, brush, &face) ||
      !ops_brush_unscaled(ctx, brush)) {
    return false_v;
  }
  if (!ops_arg_number(args, "distance", &distance) || !(distance > 0.0)) {
    return ops_fail(ctx, OPS_INVALID, "'distance' must be positive meters");
  }
  if (vkr_brush_build(brush->planes, brush->count, geometry, NULL) !=
          VKR_BRUSH_OK ||
      !vkr_brush_extrude(geometry, brush->planes, face, (float32_t)distance,
                         piece)) {
    return ops_fail(ctx, OPS_INVALID, "The face cannot be extruded");
  }
  uint32_t item = 0u;
  if (!ops_brush_replace(ctx, batch, brush, piece, 1u, NULL, 0u,
                         &brush->values[face], false_v, " extrusion", &item)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = item;
  return true_v;
}

/* A world-space plane in a brush's space; the brush is unscaled. */
static VkrBrushPlane ops_plane_to_brush(const OpsBrushRead *brush,
                                        Vec3 world_normal, Vec3 world_point) {
  const Mat4 inverse = mat4_inverse_affine(brush->transform->world);
  const Vec3 point = mat4_mul_vec3(inverse, world_point);
  const Vec4 normal =
      mat4_mul_vec4(inverse, vec3_to_vec4(vec3_normalize(world_normal), 0.0f));
  const Vec3 n = vec3_normalize(vec3_new(normal.x, normal.y, normal.z));
  return (VkrBrushPlane){.normal = n, .distance = vec3_dot(n, point)};
}

static bool8_t ops_build_clip(OpsContext *ctx, const VkrBakeryJson *args,
                              OpsBatch *batch) {
  OpsBrushRead *brush =
      arena_alloc(ops_arena(ctx), sizeof(*brush), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushPiece *pieces = arena_alloc(ops_arena(ctx), 2u * sizeof(*pieces),
                                      ARENA_MEMORY_TAG_STRUCT);
  if (!brush || !geometry || !pieces) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  Vec3 normal = {0};
  Vec3 point = {0};
  bool8_t has_normal = false_v;
  bool8_t has_point = false_v;
  if (!ops_brush_arg(ctx, batch, args, "brush", brush) ||
      !ops_brush_unscaled(ctx, brush) ||
      !ops_arg_vec3(ctx, args, "normal", &normal, &has_normal) ||
      !ops_arg_vec3(ctx, args, "point", &point, &has_point)) {
    return false_v;
  }
  if (!has_normal || !has_point || vec3_length(normal) < 1.0e-4f) {
    return ops_fail(ctx, OPS_INVALID,
                    "brush.clip needs a world 'point' on the plane and its "
                    "'normal'");
  }
  String8 keep = string8_lit("both");
  (void)vkr_bakery_json_get_string(args, "keep", &keep);
  const bool8_t back = ops_equals(keep, "back") || ops_equals(keep, "both");
  const bool8_t front = ops_equals(keep, "front") || ops_equals(keep, "both");
  if (!back && !front) {
    return ops_fail(ctx, OPS_INVALID,
                    "'keep' is back (behind the normal), front or both");
  }
  const VkrBrushPlane plane = ops_plane_to_brush(brush, normal, point);
  /* Each side is the brush plus the plane, facing that side. */
  uint32_t count = 0u;
  for (uint32_t side = 0; side < 2u; ++side) {
    if ((side == 0u && !back) || (side == 1u && !front)) {
      continue;
    }
    VkrBrushPiece *piece = &pieces[count];
    *piece = (VkrBrushPiece){.count = brush->count};
    for (uint32_t i = 0; i < brush->count; ++i) {
      piece->planes[i] = brush->planes[i];
      piece->source[i] = i;
    }
    piece->planes[piece->count] =
        side == 0u
            ? plane
            : (VkrBrushPlane){vec3_scale(plane.normal, -1.0f), -plane.distance};
    piece->source[piece->count++] = VKR_BRUSH_SOURCE_NEW;
    if (piece->count <= VKR_BRUSH_FACE_MAX &&
        vkr_brush_prune(piece, geometry)) {
      count++;
    }
  }
  if (count < (uint32_t)(back + front)) {
    return ops_fail(ctx, OPS_INVALID, "The plane does not cut the brush");
  }
  uint32_t item = 0u;
  if (!ops_brush_replace(ctx, batch, brush, pieces, count, NULL, 0u,
                         &brush->values[0], true_v, NULL, &item)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = item;
  return true_v;
}

/* Carves `cutter`, in world space through its own transform, out of
   `target`; replaces the target with its pieces. */
static bool8_t ops_carve_one(OpsContext *ctx, OpsBatch *batch,
                             const OpsBrushRead *target,
                             const OpsBrushRead *cutter, uint32_t *out_item,
                             bool8_t *out_touched) {
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushPiece *pieces =
      arena_alloc(ops_arena(ctx), VKR_BRUSH_FACE_MAX * sizeof(*pieces),
                  ARENA_MEMORY_TAG_STRUCT);
  VkrBrushPlane local[VKR_BRUSH_FACE_MAX];
  if (!geometry || !pieces) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  /* The cutter's planes, through world space, in the target's space. */
  for (uint32_t i = 0; i < cutter->count; ++i) {
    const float32_t length = vec3_length(cutter->planes[i].normal);
    const Vec3 n = vec3_scale(cutter->planes[i].normal, 1.0f / length);
    const Vec3 on_plane = vec3_scale(n, cutter->planes[i].distance / length);
    const Vec4 world_normal = mat4_mul_vec4(
        mat4_transpose(mat4_inverse_affine(cutter->transform->world)),
        vec3_to_vec4(n, 0.0f));
    local[i] = ops_plane_to_brush(
        target, vec3_new(world_normal.x, world_normal.y, world_normal.z),
        mat4_mul_vec3(cutter->transform->world, on_plane));
  }
  const uint32_t count =
      vkr_brush_carve(target->planes, target->count, local, cutter->count,
                      pieces, VKR_BRUSH_FACE_MAX, geometry);
  *out_touched = count != UINT32_MAX;
  if (count == UINT32_MAX) {
    return true_v;
  }
  return ops_brush_replace(ctx, batch, target, pieces, count, cutter->values,
                           cutter->count, &target->values[0], true_v, NULL,
                           out_item);
}

static bool8_t ops_build_carve(OpsContext *ctx, const VkrBakeryJson *args,
                               OpsBatch *batch) {
  OpsBrushRead *cutter =
      arena_alloc(ops_arena(ctx), sizeof(*cutter), ARENA_MEMORY_TAG_STRUCT);
  OpsBrushRead *target =
      arena_alloc(ops_arena(ctx), sizeof(*target), ARENA_MEMORY_TAG_STRUCT);
  if (!cutter || !target) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  if (!ops_brush_arg(ctx, batch, args, "cutter", cutter) ||
      !ops_brush_unscaled(ctx, cutter)) {
    return false_v;
  }
  const VkrScene *scene = ops_scene(ctx->frame, cutter->entity.parts.world);
  uint32_t first = UINT32_MAX;
  uint32_t carved = 0u;
  if (vkr_bakery_json_get(args, "target")) {
    bool8_t touched = false_v;
    if (!ops_brush_arg(ctx, batch, args, "target", target) ||
        !ops_brush_unscaled(ctx, target) ||
        !ops_carve_one(ctx, batch, target, cutter, &first, &touched)) {
      return false_v;
    }
    carved = touched;
  } else {
    /* As in Hammer, the cutter carves every brush it touches. */
    Vec3 lo = {0};
    Vec3 hi = {0};
    if (!ops_world_bounds(scene, cutter->entity, &lo, &hi)) {
      return ops_fail(ctx, OPS_INVALID, "The cutter has not built yet");
    }
    for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
      const VkrEntityId other = vkr_entity_id_from_index(scene->world, i);
      Vec3 a = {0};
      Vec3 b = {0};
      bool8_t touched = false_v;
      uint32_t item = UINT32_MAX;
      if (other.u64 == cutter->entity.u64 ||
          !vkr_scene_entity_alive(scene, other) ||
          !vkr_scene_get_typed(scene, other, &vkr_scene_brush_type) ||
          !ops_world_bounds(scene, other, &a, &b) ||
          !ops_in_box(a, b, lo, hi)) {
        continue;
      }
      if (!ops_brush_read(ctx, scene, other, target) ||
          !ops_brush_unscaled(ctx, target) ||
          !ops_carve_one(ctx, batch, target, cutter, &item, &touched)) {
        return false_v;
      }
      carved += touched;
      first = first == UINT32_MAX ? item : first;
    }
  }
  if (!carved) {
    return ops_fail(ctx, OPS_NOT_FOUND, "The cutter touches no brush");
  }
  if (!ops_arg_bool(args, "keep_cutter", false_v)) {
    VkrSampleEditBatchItem *item = ops_batch_add(
        ctx, batch, cutter->entity.parts.world, VKR_SCENE_EDIT_DELETE);
    if (!item) {
      return false_v;
    }
    item->request.entity = cutter->entity;
  }
  if (first != UINT32_MAX) {
    batch->op_item[batch->op_count] = first;
  }
  return true_v;
}

static bool8_t ops_build_hollow(OpsContext *ctx, const VkrBakeryJson *args,
                                OpsBatch *batch) {
  OpsBrushRead *brush =
      arena_alloc(ops_arena(ctx), sizeof(*brush), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushPiece *pieces =
      arena_alloc(ops_arena(ctx), VKR_BRUSH_FACE_MAX * sizeof(*pieces),
                  ARENA_MEMORY_TAG_STRUCT);
  float64_t thickness = 0.25;
  if (!brush || !geometry || !pieces) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  if (!ops_brush_arg(ctx, batch, args, "brush", brush) ||
      !ops_brush_unscaled(ctx, brush)) {
    return false_v;
  }
  (void)ops_arg_number(args, "thickness", &thickness);
  /* The inside is the brush with every face moved in by the thickness. */
  VkrBrushPlane inner[VKR_BRUSH_FACE_MAX];
  for (uint32_t i = 0; i < brush->count; ++i) {
    const float32_t length = vec3_length(brush->planes[i].normal);
    inner[i] = (VkrBrushPlane){.normal = brush->planes[i].normal,
                               .distance = brush->planes[i].distance -
                                           (float32_t)thickness * length};
  }
  if (!(thickness > 0.0) ||
      vkr_brush_build(inner, brush->count, geometry, NULL) != VKR_BRUSH_OK) {
    return ops_fail(ctx, OPS_INVALID,
                    "'thickness' must be positive and leave an inside");
  }
  const uint32_t count =
      vkr_brush_carve(brush->planes, brush->count, inner, brush->count, pieces,
                      VKR_BRUSH_FACE_MAX, geometry);
  if (count == 0u || count == UINT32_MAX) {
    return ops_fail(ctx, OPS_INVALID, "The brush cannot be hollowed");
  }
  uint32_t item = 0u;
  if (!ops_brush_replace(ctx, batch, brush, pieces, count, brush->values,
                         brush->count, &brush->values[0], true_v, NULL,
                         &item)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = item;
  return true_v;
}

static bool8_t ops_build_merge(OpsContext *ctx, const VkrBakeryJson *args,
                               OpsBatch *batch) {
  const VkrBakeryJson *list = vkr_bakery_json_get(args, "brushes");
  if (!list || list->type != VKR_BAKERY_JSON_ARRAY || list->count < 2u ||
      list->count > 8u) {
    return ops_fail(ctx, OPS_INVALID, "'brushes' lists 2 to 8 brushes");
  }
  OpsBrushRead *brushes = arena_alloc(
      ops_arena(ctx), list->count * sizeof(*brushes), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushPiece *piece =
      arena_alloc(ops_arena(ctx), sizeof(*piece), ARENA_MEMORY_TAG_STRUCT);
  SceneBrushFace *values =
      arena_alloc(ops_arena(ctx), 8u * VKR_BRUSH_FACE_MAX * sizeof(*values),
                  ARENA_MEMORY_TAG_STRUCT);
  if (!brushes || !geometry || !piece || !values) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  /* Every brush's planes in the first brush's space. */
  VkrBrushPlane *planes =
      arena_alloc(ops_arena(ctx), 8u * VKR_BRUSH_FACE_MAX * sizeof(*planes),
                  ARENA_MEMORY_TAG_STRUCT);
  const VkrBrushPlane *lists[8];
  uint32_t counts[8];
  uint32_t total = 0u;
  uint32_t index = 0u;
  for (const VkrBakeryJson *entry = list->first; entry;
       entry = entry->next, ++index) {
    OpsRef ref;
    if (!planes || !ops_ref(ctx, batch, entry, "brushes", &ref) ||
        ref.item >= 0 ||
        !ops_brush_read(ctx, ops_scene(ctx->frame, ref.container), ref.entity,
                        &brushes[index]) ||
        !ops_brush_unscaled(ctx, &brushes[index]) ||
        ref.container != brushes[0].entity.parts.world) {
      return ctx->call->error_code
                 ? false_v
                 : ops_fail(ctx, OPS_INVALID,
                            "Merged brushes must exist in one scene");
    }
    for (uint32_t i = 0; i < brushes[index].count; ++i) {
      const VkrBrushPlane plane = brushes[index].planes[i];
      const float32_t length = vec3_length(plane.normal);
      const Vec3 n = vec3_scale(plane.normal, 1.0f / length);
      const Vec4 world_normal = mat4_mul_vec4(
          mat4_transpose(mat4_inverse_affine(brushes[index].transform->world)),
          vec3_to_vec4(n, 0.0f));
      planes[total + i] = ops_plane_to_brush(
          &brushes[0], vec3_new(world_normal.x, world_normal.y, world_normal.z),
          mat4_mul_vec3(brushes[index].transform->world,
                        vec3_scale(n, plane.distance / length)));
      values[total + i] = brushes[index].values[i];
    }
    lists[index] = planes + total;
    counts[index] = brushes[index].count;
    total += brushes[index].count;
  }
  if (!vkr_brush_merge(lists, counts, index, piece, geometry)) {
    return ops_fail(ctx, OPS_INVALID,
                    "The brushes overlap, leave a gap or form a shape that "
                    "is not convex");
  }
  /* Sources index the concatenated planes; give them all as extra faces. */
  OpsBrushRead merged = brushes[0];
  merged.count = 0u;
  uint32_t item = 0u;
  if (!ops_brush_replace(ctx, batch, &merged, piece, 1u, values, total,
                         &brushes[0].values[0], false_v, "", &item)) {
    return false_v;
  }
  for (uint32_t b = 0; b < index; ++b) {
    VkrSampleEditBatchItem *removal = ops_batch_add(
        ctx, batch, brushes[b].entity.parts.world, VKR_SCENE_EDIT_DELETE);
    if (!removal) {
      return false_v;
    }
    removal->request.entity = brushes[b].entity;
  }
  batch->op_item[batch->op_count] = item;
  return true_v;
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
  /* A change lists objects; faces and connections are parts of their
     owner. */
  for (uint32_t i = 0; i < batch->count; ++i) {
    if (scene && result->created[i].u64 &&
        !vkr_scene_entity_is_part(scene, result->created[i])) {
      ops_change_touch(&change, result->created[i]);
    }
  }
  ops_set(ctx, call->result, "results", results);
  if (batch->select && batch->op_count && ctx->frame->scene_edit) {
    VkrEntityId selected = batch->op_target[0];
    if (batch->op_item[0] != UINT32_MAX) {
      selected = result->created[batch->op_item[0]];
    }
    *ctx->frame->scene_edit = (VkrSceneEditRequest){
        .action = VKR_SCENE_EDIT_SELECT, .entity = selected};
  }
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
  batch->select = ops_arg_bool(call->args, "select", false_v);
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

/* Role, face count, build status and face materials of a brush. */
static VkrBakeryJson *ops_brush_summary(OpsContext *ctx, const VkrScene *scene,
                                        VkrEntityId entity) {
  Arena *arena = ops_arena(ctx);
  const SceneBrushSettings *settings =
      vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type);
  const char *const roles[] = {"solid", "visual", "clip", "trigger"};
  VkrBakeryJson *summary = vkr_bakery_json_object(arena);
  ops_set(ctx, summary, "role",
          vkr_bakery_json_cstr(arena, settings->role < ArrayCount(roles)
                                          ? roles[settings->role]
                                          : "solid"));
  VkrEntityId faces[VKR_BRUSH_FACE_MAX];
  const uint32_t count =
      vkr_scene_brush_faces(scene, entity, faces, ArrayCount(faces));
  ops_set(ctx, summary, "faces", vkr_bakery_json_int(arena, count));
  const char *status = vkr_scene_brush_status(scene, entity);
  ops_set(ctx, summary, "status",
          vkr_bakery_json_cstr(arena, status ? status : "built"));
  VkrBakeryJson *materials = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < Min(count, (uint32_t)ArrayCount(faces)); ++i) {
    const SceneBrushFace *face =
        vkr_scene_get_typed(scene, faces[i], &vkr_scene_brush_face_type);
    VkrBakeryJson *name = vkr_bakery_json_cstr(
        arena, face && face->material[0] ? face->material : "(dev grid)");
    bool8_t seen = false_v;
    for (const VkrBakeryJson *m = materials->first; m && !seen; m = m->next) {
      seen = vkr_bakery_json_equal(m, name);
    }
    if (!seen) {
      vkr_bakery_json_append(materials, name);
    }
  }
  ops_set(ctx, summary, "materials", materials);
  return summary;
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
  /* Brush faces are listed only on request; brushes summarise them. */
  const bool8_t faces = ops_arg_bool(args, "faces", false_v);
  VkrBakeryJson *entities = vkr_bakery_json_array(arena);
  uint32_t matched = 0u;
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    if (!vkr_scene_entity_alive(scene, entity) ||
        (root.entity.u64 && !ops_under(scene, entity, root.entity)) ||
        (!faces && vkr_scene_entity_is_part(scene, entity))) {
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
    if (vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type)) {
      ops_set(ctx, row, "brush", ops_brush_summary(ctx, scene, entity));
    }
    const VkrHeightfield *field = vkr_scene_terrain_field(scene, entity);
    if (vkr_scene_get_typed(scene, entity, &vkr_scene_terrain_type)) {
      VkrBakeryJson *summary = vkr_bakery_json_object(arena);
      const char *status = vkr_scene_terrain_status(scene, entity);
      ops_set(ctx, summary, "status",
              vkr_bakery_json_cstr(arena, status ? status : "loaded"));
      if (field) {
        ops_set(ctx, summary, "size",
                ops_number(ctx, field->spacing * (float32_t)field->cells));
        ops_set(ctx, summary, "spacing", ops_number(ctx, field->spacing));
        ops_set(ctx, summary, "height_range",
                ops_vec3(ctx,
                         vec3_new(field->height_min, field->height_max, 0.0f)));
      }
      ops_set(ctx, row, "terrain", summary);
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
  if (vkr_scene_get_typed(scene, ref.entity, &vkr_scene_brush_type)) {
    ops_set(ctx, result, "brush", ops_brush_summary(ctx, scene, ref.entity));
  }
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
  batch->select = ops_arg_bool(call->args, "select", false_v);
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

// -----------------------------------------------------------------------------
// Level checks (docs/proposals/level-design-toolkit.md, phase 2)
// -----------------------------------------------------------------------------

/* The default capsule with any overrides the arguments carry. */
static bool8_t ops_arg_capsule(OpsContext *ctx, const VkrBakeryJson *args,
                               VkrEditorLevelCapsule *out) {
  *out = vkr_editor_level_capsule_default();
  float64_t value = 0.0;
  if (ops_arg_number(args, "radius", &value)) {
    out->radius = (float32_t)value;
  }
  if (ops_arg_number(args, "height", &value)) {
    out->height = (float32_t)value;
  }
  if (ops_arg_number(args, "step_up", &value)) {
    out->step_up = (float32_t)value;
  }
  if (ops_arg_number(args, "max_slope", &value)) {
    out->max_slope_radians = (float32_t)value * 0.01745329252f;
  }
  if (!(out->radius > 0.05f) || !(out->height > 2.0f * out->radius) ||
      !(out->step_up >= 0.0f) || !(out->max_slope_radians > 0.0f) ||
      out->max_slope_radians >= 1.5707963f) {
    return ops_fail(ctx, OPS_INVALID,
                    "The capsule needs radius > 0.05, height > 2 radius, "
                    "step_up >= 0 and max_slope (degrees) between 0 and 90");
  }
  return true_v;
}

/* The first enabled Player Start of a scene, as the walk's start. */
static bool8_t ops_player_start(const VkrScene *scene, Vec3 *out) {
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    const ScenePlayerStart *start =
        vkr_scene_entity_alive(scene, entity)
            ? vkr_scene_get_typed(scene, entity, &vkr_scene_player_start_type)
            : NULL;
    const SceneTransform *transform =
        start && start->enabled ? ops_transform(scene, entity) : NULL;
    if (transform) {
      *out = mat4_position(transform->world);
      return true_v;
    }
  }
  return false_v;
}

static VkrEditorOpStatus ops_run_lint(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  const VkrBakeryJson *region = vkr_bakery_json_get(args, "region");
  Vec3 min = {0};
  Vec3 max = {0};
  bool8_t has_min = false_v;
  bool8_t has_max = false_v;
  uint16_t container = 0u;
  VkrEditorLevelCapsule capsule;
  if (!region || region->type != VKR_BAKERY_JSON_OBJECT ||
      !ops_arg_vec3(ctx, region, "min", &min, &has_min) ||
      !ops_arg_vec3(ctx, region, "max", &max, &has_max) || !has_min ||
      !has_max || max.x <= min.x || max.y <= min.y || max.z <= min.z) {
    if (!ctx->call->error_code) {
      ops_fail(ctx, OPS_INVALID,
               "'region' is {\"min\": [..], \"max\": [..]}, a box with "
               "volume");
    }
    return VKR_EDITOR_OP_DONE;
  }
  if (!ops_arg_container(ctx, args, &container) ||
      !ops_arg_capsule(ctx, args, &capsule)) {
    return VKR_EDITOR_OP_DONE;
  }
  const VkrScene *scene = ops_scene(ctx->frame, container);
  Vec3 start = {0};
  bool8_t has_start = false_v;
  if (!ops_arg_vec3(ctx, args, "start", &start, &has_start)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!has_start) {
    has_start = ops_player_start(scene, &start);
  }
  float64_t limit = 100.0;
  (void)ops_arg_number(args, "limit", &limit);
  const uint32_t capacity = (uint32_t)vkr_clamp_f64(limit, 1.0, 500.0);
  VkrEditorLevelIssue *issues = arena_alloc(
      ops_arena(ctx), capacity * sizeof(*issues), ARENA_MEMORY_TAG_STRUCT);
  if (!issues) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  VkrEditorLevelStats stats = {0};
  const uint32_t found = vkr_editor_level_lint(scene, min, max, &capsule,
                                               has_start ? &start : NULL,
                                               issues, capacity, &stats);
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *list = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < Min(found, capacity); ++i) {
    VkrBakeryJson *issue = vkr_bakery_json_object(arena);
    ops_set(ctx, issue, "kind",
            vkr_bakery_json_cstr(arena,
                                 vkr_editor_level_issue_name(issues[i].kind)));
    ops_set(ctx, issue, "position", ops_vec3(ctx, issues[i].position));
    ops_set(ctx, issue, "value", ops_number(ctx, issues[i].value));
    if (issues[i].entity.u64) {
      ops_set(ctx, issue, "entity", ops_entity(ctx, scene, issues[i].entity));
    }
    if (issues[i].other.u64) {
      ops_set(ctx, issue, "other", ops_entity(ctx, scene, issues[i].other));
    }
    char problem[160];
    if (issues[i].kind == VKR_EDITOR_LEVEL_BROKEN_CONNECTION &&
        vkr_io_connection_problem(scene, issues[i].other, problem,
                                  sizeof(problem))) {
      ops_set(ctx, issue, "problem", vkr_bakery_json_cstr(arena, problem));
    }
    vkr_bakery_json_append(list, issue);
  }
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  ops_set(ctx, result, "cell", ops_number(ctx, stats.cell));
  ops_set(ctx, result, "samples", vkr_bakery_json_int(arena, stats.samples));
  ops_set(ctx, result, "walkable", vkr_bakery_json_int(arena, stats.walkable));
  if (has_start) {
    ops_set(ctx, result, "start", ops_vec3(ctx, start));
    ops_set(ctx, result, "reachable",
            vkr_bakery_json_int(arena, stats.reachable));
  }
  ops_set(ctx, result, "found", vkr_bakery_json_int(arena, found));
  ops_set(ctx, result, "issues", list);
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_reachable(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  Vec3 from = {0};
  Vec3 to = {0};
  bool8_t has_from = false_v;
  bool8_t has_to = false_v;
  uint16_t container = 0u;
  VkrEditorLevelCapsule capsule;
  if (!ops_arg_vec3(ctx, args, "from", &from, &has_from) ||
      !ops_arg_vec3(ctx, args, "to", &to, &has_to) ||
      !ops_arg_container(ctx, args, &container) ||
      !ops_arg_capsule(ctx, args, &capsule)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!has_from || !has_to) {
    ops_fail(ctx, OPS_INVALID,
             "query.reachable needs floor points 'from' "
             "and 'to'");
    return VKR_EDITOR_OP_DONE;
  }
  Vec3 path[512];
  uint32_t count = 0u;
  float32_t length = 0.0f;
  const bool8_t reached = vkr_editor_level_reachable(
      ops_scene(ctx->frame, container), from, to, &capsule, path,
      ArrayCount(path), &count, &length);
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *points = vkr_bakery_json_array(arena);
  /* At most 32 points along the route. */
  const uint32_t stride = Max(1u, (count + 31u) / 32u);
  for (uint32_t i = 0; i < count; i += stride) {
    vkr_bakery_json_append(points, ops_vec3(ctx, path[i]));
  }
  if (count && (count - 1u) % stride) {
    vkr_bakery_json_append(points, ops_vec3(ctx, path[count - 1u]));
  }
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "reachable",
          vkr_bakery_json_bool(arena, reached));
  if (reached) {
    ops_set(ctx, ctx->call->result, "length", ops_number(ctx, length));
    ops_set(ctx, ctx->call->result, "path", points);
  }
  return VKR_EDITOR_OP_DONE;
}

/* view.camera: place the perspective camera at `eye` looking at `target`. */
static VkrEditorOpStatus ops_run_camera(OpsContext *ctx) {
  const VkrSampleUiFrame *frame = ctx->frame;
  Vec3 eye = {0};
  Vec3 target = {0};
  bool8_t has_eye = false_v;
  bool8_t has_target = false_v;
  if (!ops_arg_vec3(ctx, ctx->call->args, "eye", &eye, &has_eye) ||
      !ops_arg_vec3(ctx, ctx->call->args, "target", &target, &has_target)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!has_eye || !has_target || vec3_length(vec3_sub(target, eye)) < 1.0e-3f ||
      !frame->editor_state_request) {
    ops_fail(ctx, OPS_INVALID, "view.camera needs a different eye and target");
    return VKR_EDITOR_OP_DONE;
  }
  const Vec3 d = vec3_normalize(vec3_sub(target, eye));
  VkrSampleEditorStateRequest *request = frame->editor_state_request;
  request->apply_recall = true_v;
  request->recall = frame->scene_recall;
  request->recall.camera_valid = true_v;
  request->recall.position = eye;
  request->recall.pitch = asinf(vkr_clamp_f32(d.y, -1.0f, 1.0f)) * 57.29577951f;
  request->recall.yaw = atan2f(d.z, d.x) * 57.29577951f;
  request->recall.selection_valid = false_v;
  if (request->recall.field_of_view <= 0.0f) {
    request->recall.field_of_view = 60.0f;
    request->recall.near_plane = 0.1f;
    request->recall.far_plane = 1000.0f;
  }
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "eye", ops_vec3(ctx, eye));
  ops_set(ctx, ctx->call->result, "target", ops_vec3(ctx, target));
  return VKR_EDITOR_OP_DONE;
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
    /* An explicit eye and target place the perspective camera. */
    Vec3 eye = {0};
    Vec3 target = {0};
    bool8_t has_eye = false_v;
    bool8_t has_target = false_v;
    if (!ops_arg_vec3(ctx, call->args, "eye", &eye, &has_eye) ||
        !ops_arg_vec3(ctx, call->args, "target", &target, &has_target)) {
      return VKR_EDITOR_OP_DONE;
    }
    if (has_eye != has_target ||
        (has_eye && vec3_length(vec3_sub(target, eye)) < 1.0e-3f)) {
      ops_fail(ctx, OPS_INVALID, "'eye' and 'target' come together and differ");
      return VKR_EDITOR_OP_DONE;
    }
    if (has_eye && frame->editor_state_request) {
      const Vec3 d = vec3_normalize(vec3_sub(target, eye));
      VkrSampleEditorStateRequest *state_request = frame->editor_state_request;
      state_request->apply_recall = true_v;
      state_request->recall = frame->scene_recall;
      state_request->recall.camera_valid = true_v;
      state_request->recall.position = eye;
      state_request->recall.pitch =
          asinf(vkr_clamp_f32(d.y, -1.0f, 1.0f)) * 57.29577951f;
      state_request->recall.yaw = atan2f(d.z, d.x) * 57.29577951f;
      state_request->recall.selection_valid = false_v;
      if (state_request->recall.field_of_view <= 0.0f) {
        state_request->recall.field_of_view = 60.0f;
        state_request->recall.near_plane = 0.1f;
        state_request->recall.far_plane = 1000.0f;
      }
      next.camera_view = VKR_SAMPLE_CAMERA_PERSPECTIVE;
      request.value = next;
      request.frame_box = false_v;
    }
    pending->restore = has_eye || request.frame_box ||
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
// =============================================================================
// Terrain (vkr_scene_terrain.h)
// =============================================================================

/* An existing entity with a loaded terrain. */
static bool8_t ops_terrain_arg(OpsContext *ctx, const OpsBatch *batch,
                               const VkrBakeryJson *args, OpsRef *ref,
                               const VkrHeightfield **out_field) {
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "terrain"), "terrain",
               ref)) {
    return false_v;
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref->container);
  const VkrHeightfield *field =
      ref->item < 0 ? vkr_scene_terrain_field(scene, ref->entity) : NULL;
  if (!field) {
    const char *status =
        ref->item < 0 ? vkr_scene_terrain_status(scene, ref->entity) : NULL;
    return ops_fail(ctx, OPS_INVALID,
                    "'terrain' must name a loaded terrain%s%s",
                    status ? ": " : "", status ? status : "");
  }
  *out_field = field;
  return true_v;
}

static VkrSampleEditBatchItem *ops_terrain_item(OpsContext *ctx,
                                                OpsBatch *batch,
                                                const OpsRef *ref,
                                                const VkrHeightfieldOp *op) {
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, ref->container, VKR_SCENE_EDIT_TERRAIN);
  if (item) {
    ops_item_target(item, ref);
    item->request.terrain = *op;
    batch->op_target[batch->op_count] = ref->entity;
  }
  return item;
}

/* terrain.create: a new heightfield file and an entity that shows it. */
static bool8_t ops_build_terrain_create(OpsContext *ctx,
                                        const VkrBakeryJson *args,
                                        OpsBatch *batch) {
  uint16_t container = 0u;
  if (!ops_arg_container(ctx, args, &container)) {
    return false_v;
  }
  const VkrScene *scene = ops_scene(ctx->frame, container);
  float64_t size = 256.0;
  float64_t spacing = 1.0;
  float64_t height = 0.0;
  float64_t height_min = -100.0;
  float64_t height_max = 400.0;
  float64_t texture_size = 4.0;
  (void)ops_arg_number(args, "size", &size);
  (void)ops_arg_number(args, "spacing", &spacing);
  (void)ops_arg_number(args, "height", &height);
  (void)ops_arg_number(args, "height_min", &height_min);
  (void)ops_arg_number(args, "height_max", &height_max);
  (void)ops_arg_number(args, "texture_size", &texture_size);
  const float64_t cells = spacing > 0.0 ? round(size / spacing) : 0.0;
  if (!(cells >= VKR_HEIGHTFIELD_TILE_CELLS) ||
      cells > VKR_HEIGHTFIELD_CELLS_MAX ||
      fmod(cells, (float64_t)VKR_HEIGHTFIELD_TILE_CELLS) != 0.0 ||
      !(height_max > height_min) || height < height_min ||
      height > height_max || !(texture_size > 0.0)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'size' / 'spacing' must be a multiple of %u cells, at "
                    "most %u, with height_min <= height <= height_max",
                    VKR_HEIGHTFIELD_TILE_CELLS, VKR_HEIGHTFIELD_CELLS_MAX);
  }
  /* The file comes first, so the component finds it when it appears. */
  VkrEntityRef id;
  char uuid[37];
  vkr_scene_entity_ref_generate(&id);
  vkr_entity_ref_format(&id, uuid);
  char relative[SCENE_TERRAIN_PATH_CAPACITY];
  char absolute[1100];
  char directory[1100];
  snprintf(relative, sizeof(relative), "assets/terrain/%s.vkrhf", uuid);
  if (!vkr_scene_terrain_resolve(scene, relative, absolute, sizeof(absolute)) ||
      !vkr_scene_terrain_resolve(scene, "assets/terrain", directory,
                                 sizeof(directory))) {
    return ops_fail(ctx, OPS_INVALID, "The terrain path is too long");
  }
  VkrAllocator scratch = {.ctx = ops_arena(ctx)};
  vkr_allocator_arena(&scratch);
  const String8 directory_text =
      string8_create_from_cstr((const uint8_t *)directory, strlen(directory));
  VkrHeightfield field;
  char error[160] = {0};
  if (!file_ensure_directory(&scratch, &directory_text) ||
      !vkr_heightfield_create(&field, (uint32_t)cells, (float32_t)spacing,
                              (float32_t)height_min, (float32_t)height_max,
                              (float32_t)height, &scratch) ||
      !vkr_heightfield_save(&field, absolute, error, sizeof(error))) {
    return ops_fail(ctx, OPS_REJECTED, "The terrain file could not be made%s%s",
                    error[0] ? ": " : "", error);
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, container, VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  VkrSceneEditValues *values = &item->request.values;
  values->fields =
      VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM | VKR_SCENE_EDIT_COMPONENT;
  snprintf(values->name, sizeof(values->name), "Terrain");
  values->rotation = vkr_quat_identity();
  values->scale = vec3_one();
  bool8_t has_position = false_v;
  String8 name = {0};
  if (!ops_arg_vec3(ctx, args, "position", &values->position, &has_position)) {
    return false_v;
  }
  if (vkr_bakery_json_get_string(args, "name", &name) && name.length &&
      name.length < sizeof(values->name)) {
    snprintf(values->name, sizeof(values->name), "%.*s", (int)name.length,
             name.str);
  }
  SceneTerrain terrain;
  vkr_type_defaults(&vkr_scene_terrain_type, &terrain);
  snprintf(terrain.heightfield, sizeof(terrain.heightfield), "%s", relative);
  terrain.texture_size = (float32_t)texture_size;
  char *const layers[VKR_HEIGHTFIELD_LAYERS] = {terrain.layer0, terrain.layer1,
                                                terrain.layer2, terrain.layer3};
  /* Unnamed layers start as dev colors, so paint shows at once. */
  const char *const dev_layers[VKR_HEIGHTFIELD_LAYERS] = {
      OPS_DEV_GRID, OPS_DEV_FLOOR, OPS_DEV_ORANGE, OPS_DEV_BLUE};
  for (uint32_t i = 0; i < VKR_HEIGHTFIELD_LAYERS; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "layer%u", i);
    String8 layer = {0};
    if (vkr_bakery_json_get_string(args, key, &layer) &&
        layer.length < SCENE_TERRAIN_MATERIAL_CAPACITY) {
      MemCopy(layers[i], layer.str, layer.length);
      layers[i][layer.length] = '\0';
    } else {
      snprintf(layers[i], SCENE_TERRAIN_MATERIAL_CAPACITY, "%s", dev_layers[i]);
    }
  }
  values->component_type = &vkr_scene_terrain_type;
  MemCopy(values->component, &terrain, sizeof(terrain));
  batch->op_item[batch->op_count] = batch->count - 1u;
  return ops_validate_values(ctx, values);
}

/* terrain.brush: one brush step per point, as a sculpt stroke. */
static bool8_t ops_build_terrain_brush(OpsContext *ctx,
                                       const VkrBakeryJson *args,
                                       OpsBatch *batch) {
  static const char *const modes[VKR_HEIGHTFIELD_BRUSH_COUNT] = {
      "raise", "lower", "smooth", "flatten", "paint"};
  OpsRef ref;
  const VkrHeightfield *field = NULL;
  if (!ops_terrain_arg(ctx, batch, args, &ref, &field)) {
    return false_v;
  }
  String8 mode = string8_lit("raise");
  (void)vkr_bakery_json_get_string(args, "mode", &mode);
  VkrHeightfieldOp op = {.kind = VKR_HEIGHTFIELD_OP_BRUSH,
                         .brush = VKR_HEIGHTFIELD_BRUSH_COUNT,
                         .radius = 4.0f,
                         .strength = 0.5f};
  for (uint32_t i = 0; i < VKR_HEIGHTFIELD_BRUSH_COUNT; ++i) {
    if (ops_equals(mode, modes[i])) {
      op.brush = (VkrHeightfieldBrush)i;
    }
  }
  float64_t number = 0.0;
  if (ops_arg_number(args, "radius", &number)) {
    op.radius = (float32_t)number;
  }
  if (ops_arg_number(args, "strength", &number)) {
    op.strength = (float32_t)number;
  }
  const bool8_t has_height = ops_arg_number(args, "height", &number);
  op.height = (float32_t)number;
  if (ops_arg_number(args, "layer", &number)) {
    op.layer = (uint32_t)Max(0.0, number - 1.0);
  }
  if (op.brush == VKR_HEIGHTFIELD_BRUSH_COUNT || !(op.radius > 0.0f) ||
      op.layer >= VKR_HEIGHTFIELD_LAYERS || !isfinite(op.strength)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'mode' is raise, lower, smooth, flatten or paint, with a "
                    "positive 'radius' and a 'layer' of 1 to 4");
  }
  const VkrBakeryJson *points = vkr_bakery_json_get(args, "points");
  const VkrBakeryJson *point = vkr_bakery_json_get(args, "point");
  if ((!points || points->type != VKR_BAKERY_JSON_ARRAY || !points->count ||
       points->count > 256u) &&
      !point) {
    return ops_fail(ctx, OPS_INVALID,
                    "terrain.brush needs a 'point' or 1 to 256 'points'");
  }
  VkrBakeryJson single = {0};
  const VkrBakeryJson *first = points ? points->first : NULL;
  if (!points) {
    single = *point;
    single.next = NULL;
    first = &single;
  }
  for (const VkrBakeryJson *entry = first; entry; entry = entry->next) {
    Vec3 at = {0};
    if (entry->type != VKR_BAKERY_JSON_ARRAY || entry->count != 3u) {
      return ops_fail(ctx, OPS_INVALID, "Each point is [x, y, z]");
    }
    const VkrBakeryJson *c = entry->first;
    at.x = (float32_t)(c->type == VKR_BAKERY_JSON_INT ? (float64_t)c->integer
                                                      : c->number);
    c = c->next;
    at.y = (float32_t)(c->type == VKR_BAKERY_JSON_INT ? (float64_t)c->integer
                                                      : c->number);
    c = c->next;
    at.z = (float32_t)(c->type == VKR_BAKERY_JSON_INT ? (float64_t)c->integer
                                                      : c->number);
    VkrHeightfieldOp step = op;
    step.a = at;
    if (!has_height) {
      step.height = at.y;
    }
    if (!ops_terrain_item(ctx, batch, &ref, &step)) {
      return false_v;
    }
  }
  (void)field;
  return true_v;
}

/* terrain.flatten: a level footprint blending into the ground around it. */
static bool8_t ops_build_terrain_flatten(OpsContext *ctx,
                                         const VkrBakeryJson *args,
                                         OpsBatch *batch) {
  OpsRef ref;
  const VkrHeightfield *field = NULL;
  Vec3 min = {0};
  Vec3 max = {0};
  bool8_t has_min = false_v;
  bool8_t has_max = false_v;
  if (!ops_terrain_arg(ctx, batch, args, &ref, &field) ||
      !ops_arg_vec3(ctx, args, "min", &min, &has_min) ||
      !ops_arg_vec3(ctx, args, "max", &max, &has_max)) {
    return false_v;
  }
  if (!has_min || !has_max) {
    return ops_fail(ctx, OPS_INVALID,
                    "terrain.flatten needs the footprint's 'min' and 'max'");
  }
  float64_t height = Min(min.y, max.y);
  float64_t falloff = 2.0;
  (void)ops_arg_number(args, "height", &height);
  (void)ops_arg_number(args, "falloff", &falloff);
  const VkrHeightfieldOp op = {
      .kind = VKR_HEIGHTFIELD_OP_FLATTEN,
      .min = vec2_new(Min(min.x, max.x), Min(min.z, max.z)),
      .max = vec2_new(Max(min.x, max.x), Max(min.z, max.z)),
      .height = (float32_t)height,
      .falloff = (float32_t)falloff};
  (void)field;
  if (!ops_terrain_item(ctx, batch, &ref, &op)) {
    return false_v;
  }
  return true_v;
}

/* terrain.ramp: a straight slope from one point to another. */
static bool8_t ops_build_terrain_ramp(OpsContext *ctx,
                                      const VkrBakeryJson *args,
                                      OpsBatch *batch) {
  OpsRef ref;
  const VkrHeightfield *field = NULL;
  Vec3 from = {0};
  Vec3 to = {0};
  bool8_t has_from = false_v;
  bool8_t has_to = false_v;
  if (!ops_terrain_arg(ctx, batch, args, &ref, &field) ||
      !ops_arg_vec3(ctx, args, "from", &from, &has_from) ||
      !ops_arg_vec3(ctx, args, "to", &to, &has_to)) {
    return false_v;
  }
  float64_t width = 4.0;
  float64_t falloff = 2.0;
  (void)ops_arg_number(args, "width", &width);
  (void)ops_arg_number(args, "falloff", &falloff);
  if (!has_from || !has_to || !(width > 0.0)) {
    return ops_fail(ctx, OPS_INVALID,
                    "terrain.ramp needs 'from', 'to' and a positive 'width'");
  }
  const VkrHeightfieldOp op = {.kind = VKR_HEIGHTFIELD_OP_RAMP,
                               .a = from,
                               .b = to,
                               .width = (float32_t)width,
                               .falloff = (float32_t)falloff};
  (void)field;
  return ops_terrain_item(ctx, batch, &ref, &op) != NULL;
}

/* terrain.stamp: a grayscale image of heights over a rectangle. */
static bool8_t ops_build_terrain_stamp(OpsContext *ctx,
                                       const VkrBakeryJson *args,
                                       OpsBatch *batch) {
  OpsRef ref;
  const VkrHeightfield *field = NULL;
  Vec3 center = {0};
  bool8_t has_center = false_v;
  String8 image = {0};
  if (!ops_terrain_arg(ctx, batch, args, &ref, &field) ||
      !ops_arg_vec3(ctx, args, "center", &center, &has_center)) {
    return false_v;
  }
  float64_t size = 64.0;
  float64_t scale = 10.0;
  (void)ops_arg_number(args, "size", &size);
  (void)ops_arg_number(args, "height_scale", &scale);
  String8 mode = string8_lit("add");
  (void)vkr_bakery_json_get_string(args, "mode", &mode);
  if (!has_center || !vkr_bakery_json_get_string(args, "image", &image) ||
      !image.length || image.length >= 1024u || !(size > 0.0) ||
      (!ops_equals(mode, "add") && !ops_equals(mode, "set"))) {
    return ops_fail(ctx, OPS_INVALID,
                    "terrain.stamp needs an 'image' file, a 'center', a "
                    "positive 'size' and 'mode' add or set");
  }
  char path[1100];
  char relative[1024];
  snprintf(relative, sizeof(relative), "%.*s", (int)image.length, image.str);
  if (!vkr_scene_terrain_resolve(ops_scene(ctx->frame, ref.container), relative,
                                 path, sizeof(path))) {
    return ops_fail(ctx, OPS_INVALID, "The image path is too long");
  }
  /* The engine's stb_image reads memory only. */
  FILE *file = file_fopen(path, "rb");
  long bytes = 0;
  uint8_t *encoded = NULL;
  if (file && fseek(file, 0, SEEK_END) == 0 && (bytes = ftell(file)) > 0 &&
      bytes <= 256L * 1024L * 1024L && fseek(file, 0, SEEK_SET) == 0) {
    encoded =
        arena_alloc(ops_arena(ctx), (uint64_t)bytes, ARENA_MEMORY_TAG_ARRAY);
    if (encoded && fread(encoded, 1u, (size_t)bytes, file) != (size_t)bytes) {
      encoded = NULL;
    }
  }
  if (file) {
    fclose(file);
  }
  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_us *pixels = encoded
                        ? stbi_load_16_from_memory(encoded, (int)bytes, &width,
                                                   &height, &channels, 1)
                        : NULL;
  if (!pixels || width < 2 || height < 2 || width > 4096 || height > 4096) {
    if (pixels) {
      stbi_image_free(pixels);
    }
    return ops_fail(ctx, OPS_NOT_FOUND,
                    "The image did not load as 2 to 4096 pixels a side");
  }
  float32_t *values =
      arena_alloc(ops_arena(ctx), sizeof(float32_t) * (size_t)width * height,
                  ARENA_MEMORY_TAG_ARRAY);
  for (int i = 0; values && i < width * height; ++i) {
    values[i] = (float32_t)pixels[i] / 65535.0f;
  }
  stbi_image_free(pixels);
  if (!values) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  const float32_t half = (float32_t)size * 0.5f;
  const VkrHeightfieldOp op = {
      .kind = VKR_HEIGHTFIELD_OP_STAMP,
      .min = vec2_new(center.x - half, center.z - half),
      .max = vec2_new(center.x + half, center.z + half),
      .height = center.y,
      .strength = (float32_t)scale,
      .image = values,
      .image_width = (uint32_t)width,
      .image_height = (uint32_t)height,
      .add = ops_equals(mode, "add")};
  (void)field;
  return ops_terrain_item(ctx, batch, &ref, &op) != NULL;
}

/* terrain.sample: ground heights at points, for placing things. */
static VkrEditorOpStatus ops_run_terrain_sample(OpsContext *ctx) {
  OpsRef ref;
  const VkrHeightfield *field = NULL;
  const VkrBakeryJson *points = vkr_bakery_json_get(ctx->call->args, "points");
  if (!ops_terrain_arg(ctx, NULL, ctx->call->args, &ref, &field)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!points || points->type != VKR_BAKERY_JSON_ARRAY ||
      points->count > 1024u) {
    ops_fail(ctx, OPS_INVALID, "'points' is up to 1024 [x, z] pairs");
    return VKR_EDITOR_OP_DONE;
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  Vec3 origin = {0};
  (void)vkr_scene_terrain_to_local(scene, ref.entity, vec3_zero(), &origin);
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *heights = vkr_bakery_json_array(arena);
  for (const VkrBakeryJson *entry = points->first; entry; entry = entry->next) {
    float32_t h = 0.0f;
    if (entry->type != VKR_BAKERY_JSON_ARRAY || entry->count != 2u) {
      ops_fail(ctx, OPS_INVALID, "Each point is [x, z]");
      return VKR_EDITOR_OP_DONE;
    }
    const VkrBakeryJson *x = entry->first;
    const VkrBakeryJson *z = x->next;
    const float32_t px =
        (float32_t)(x->type == VKR_BAKERY_JSON_INT ? (float64_t)x->integer
                                                   : x->number);
    const float32_t pz =
        (float32_t)(z->type == VKR_BAKERY_JSON_INT ? (float64_t)z->integer
                                                   : z->number);
    vkr_bakery_json_append(
        heights, vkr_heightfield_sample(field, px + origin.x, pz + origin.z, &h)
                     ? ops_number(ctx, h - origin.y)
                     : vkr_bakery_json_null(arena));
  }
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "heights", heights);
  return VKR_EDITOR_OP_DONE;
}

// =============================================================================
// Entity IO (vkr_io_router.h)
// =============================================================================

static const char *ops_io_kind_name(uint32_t kind) {
  static const char *const names[] = {
      "bool", "i32",  "u32",   "f32",       "angle", "vec2",   "vec3",
      "vec4", "quat", "color", "direction", "enum",  "string", "entity"};
  return kind < ArrayCount(names) ? names[kind] : "none";
}

static VkrBakeryJson *ops_io_ports(OpsContext *ctx, const VkrScene *scene,
                                   VkrEntityId entity, bool8_t inputs) {
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *list = vkr_bakery_json_array(arena);
  for (uint32_t t = 0; t < scene->type_count; ++t) {
    const VkrTypeDesc *type = scene->types[t].type;
    const VkrIoPort *ports = inputs ? type->inputs : type->outputs;
    if (!ports || !vkr_scene_get_typed(scene, entity, type)) {
      continue;
    }
    for (uint32_t p = 0; ports[p].name; ++p) {
      VkrBakeryJson *port = vkr_bakery_json_object(arena);
      char name[160];
      snprintf(name, sizeof(name), "%s.%s", type->name, ports[p].name);
      ops_set(ctx, port, "name", vkr_bakery_json_cstr(arena, name));
      ops_set(ctx, port, "label", vkr_bakery_json_cstr(arena, ports[p].label));
      ops_set(ctx, port, "kind",
              vkr_bakery_json_cstr(arena, ops_io_kind_name(ports[p].kind)));
      vkr_bakery_json_append(list, port);
    }
  }
  for (uint32_t p = 0; inputs && vkr_io_builtin_inputs[p].name; ++p) {
    VkrBakeryJson *port = vkr_bakery_json_object(arena);
    ops_set(ctx, port, "name",
            vkr_bakery_json_cstr(arena, vkr_io_builtin_inputs[p].name));
    ops_set(ctx, port, "label",
            vkr_bakery_json_cstr(arena, vkr_io_builtin_inputs[p].label));
    ops_set(ctx, port, "kind", vkr_bakery_json_cstr(arena, "none"));
    vkr_bakery_json_append(list, port);
  }
  return list;
}

/* One connection as JSON: its ends, settings and why it does not route. */
static VkrBakeryJson *ops_io_connection(OpsContext *ctx, const VkrScene *scene,
                                        VkrEntityId connection) {
  Arena *arena = ops_arena(ctx);
  const SceneIoConnection *value =
      vkr_scene_get_typed(scene, connection, &vkr_scene_io_connection_type);
  VkrBakeryJson *object = vkr_bakery_json_object(arena);
  ops_set(ctx, object, "connection", ops_id(ctx, connection));
  ops_set(ctx, object, "source",
          ops_entity(ctx, scene, ops_parent(scene, connection)));
  ops_set(ctx, object, "output", vkr_bakery_json_cstr(arena, value->output));
  const VkrEntityId target = vkr_scene_find_entity_ref(scene, &value->target);
  ops_set(ctx, object, "target",
          target.u64 ? ops_entity(ctx, scene, target)
                     : vkr_bakery_json_null(arena));
  ops_set(ctx, object, "input", vkr_bakery_json_cstr(arena, value->input));
  ops_set(ctx, object, "value", vkr_bakery_json_cstr(arena, value->value));
  ops_set(ctx, object, "delay", vkr_bakery_json_float(arena, value->delay));
  ops_set(ctx, object, "limit", vkr_bakery_json_int(arena, value->limit));
  char problem[160];
  if (vkr_io_connection_problem(scene, connection, problem, sizeof(problem))) {
    ops_set(ctx, object, "problem", vkr_bakery_json_cstr(arena, problem));
  }
  return object;
}

/* io.connect: a connection under `source` from its `output` to `target`'s
   `input`. Names are checked against what the entities carry now; ends the
   batch creates are checked when the game publishes. */
static bool8_t ops_build_connect(OpsContext *ctx, const VkrBakeryJson *args,
                                 OpsBatch *batch) {
  OpsRef source;
  OpsRef target = {.item = -1};
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "source"), "source",
               &source)) {
    return false_v;
  }
  const VkrScene *scene = ops_scene(ctx->frame, source.container);
  const VkrBakeryJson *target_value = vkr_bakery_json_get(args, "target");
  if (target_value && target_value->type != VKR_BAKERY_JSON_NULL &&
      !ops_ref(ctx, batch, target_value, "target", &target)) {
    return false_v;
  }
  if ((target.entity.u64 || target.item >= 0) &&
      target.container != source.container) {
    return ops_fail(ctx, OPS_INVALID,
                    "A connection's target must be in its source's scene");
  }
  SceneIoConnection connection = {0};
  String8 output = {0};
  String8 input = {0};
  String8 value = {0};
  (void)vkr_bakery_json_get_string(args, "output", &output);
  (void)vkr_bakery_json_get_string(args, "input", &input);
  (void)vkr_bakery_json_get_string(args, "value", &value);
  if (!output.length || output.length >= sizeof(connection.output) ||
      input.length >= sizeof(connection.input) ||
      value.length >= sizeof(connection.value)) {
    return ops_fail(ctx, OPS_INVALID,
                    "io.connect needs the source's 'output'; names and "
                    "values hold at most %u bytes",
                    (unsigned)sizeof(connection.output) - 1u);
  }
  VkrIoEndpoint endpoint;
  if (source.item < 0 &&
      !vkr_io_find_output(scene, source.entity, output, &endpoint)) {
    return ops_fail(ctx, OPS_NOT_FOUND,
                    "The source has no output '%.*s'; io.list names them",
                    (int)output.length, output.str);
  }
  if (target.item < 0 && target.entity.u64 && input.length &&
      !vkr_io_find_input(scene, target.entity, input, &endpoint)) {
    return ops_fail(ctx, OPS_NOT_FOUND,
                    "The target has no input '%.*s'; io.list names them",
                    (int)input.length, input.str);
  }
  if (target.item >= 0) {
    connection.target = ctx->ops->items[target.item].request.values.ref;
  } else if (target.entity.u64 &&
             !vkr_scene_entity_ref(scene, target.entity, &connection.target)) {
    return ops_fail(ctx, OPS_INVALID,
                    "The target has no id to reference: a node inside a "
                    "model, or an object of a document without ids");
  }
  MemCopy(connection.output, output.str, output.length);
  MemCopy(connection.input, input.str, input.length);
  MemCopy(connection.value, value.str, value.length);
  float64_t number = 0.0;
  if (ops_arg_number(args, "delay", &number)) {
    if (!(number >= 0.0 && number <= 3600.0)) {
      return ops_fail(ctx, OPS_INVALID, "'delay' is 0 to 3600 seconds");
    }
    connection.delay = (float32_t)number;
  }
  if (ops_arg_number(args, "limit", &number)) {
    if (!(number >= 0.0 && number <= 1000000.0)) {
      return ops_fail(ctx, OPS_INVALID, "'limit' is 0 (unlimited) or more");
    }
    connection.limit = (uint32_t)number;
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, source.container, VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  item->request.parent = source.entity;
  item->parent_ref = source.item;
  VkrSceneEditValues *values = &item->request.values;
  values->fields =
      VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM | VKR_SCENE_EDIT_COMPONENT;
  snprintf(values->name, sizeof(values->name), "%.*s",
           (int)Min(output.length, (uint64_t)sizeof(values->name) - 1u),
           output.str);
  values->rotation = vkr_quat_identity();
  values->scale = vec3_one();
  values->component_type = &vkr_scene_io_connection_type;
  MemCopy(values->component, &connection, sizeof(connection));
  batch->op_item[batch->op_count] = batch->count - 1u;
  return ops_validate_values(ctx, values);
}

/* io.disconnect: one `connection`, or every connection of `source` (from
   `output` when given). */
static bool8_t ops_build_disconnect(OpsContext *ctx, const VkrBakeryJson *args,
                                    OpsBatch *batch) {
  OpsRef ref;
  if (vkr_bakery_json_get(args, "connection")) {
    if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "connection"),
                 "connection", &ref)) {
      return false_v;
    }
    const VkrScene *scene = ops_scene(ctx->frame, ref.container);
    if (ref.item >= 0 || !vkr_scene_get_typed(scene, ref.entity,
                                              &vkr_scene_io_connection_type)) {
      return ops_fail(ctx, OPS_INVALID, "'connection' is not a connection");
    }
    VkrSampleEditBatchItem *item =
        ops_batch_add(ctx, batch, ref.container, VKR_SCENE_EDIT_DELETE);
    if (!item) {
      return false_v;
    }
    ops_item_target(item, &ref);
    batch->op_target[batch->op_count] = ref.entity;
    return true_v;
  }
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "source"), "source",
               &ref) ||
      ref.item >= 0) {
    return ref.item >= 0 ? ops_fail(ctx, OPS_INVALID,
                                    "io.disconnect needs existing entities")
                         : false_v;
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  String8 output = {0};
  const bool8_t by_output =
      vkr_bakery_json_get_string(args, "output", &output) && output.length;
  uint32_t removed = 0u;
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId child = vkr_entity_id_from_index(scene->world, i);
    const SceneIoConnection *connection =
        vkr_scene_entity_alive(scene, child) &&
                ops_parent(scene, child).u64 == ref.entity.u64
            ? vkr_scene_get_typed(scene, child, &vkr_scene_io_connection_type)
            : NULL;
    if (!connection ||
        (by_output && strlen(connection->output) != output.length) ||
        (by_output &&
         MemCompare(connection->output, output.str, output.length) != 0)) {
      continue;
    }
    VkrSampleEditBatchItem *item =
        ops_batch_add(ctx, batch, ref.container, VKR_SCENE_EDIT_DELETE);
    if (!item) {
      return false_v;
    }
    item->request.entity = child;
    removed++;
  }
  if (!removed) {
    return ops_fail(ctx, OPS_NOT_FOUND, "No connection matches");
  }
  batch->op_target[batch->op_count] = ref.entity;
  return true_v;
}

/* io.list: an entity's outputs and inputs, its connections and the ones
   that reach it. */
static VkrEditorOpStatus ops_run_io_list(OpsContext *ctx) {
  OpsRef ref;
  if (!ops_ref(ctx, NULL, vkr_bakery_json_get(ctx->call->args, "entity"),
               "entity", &ref)) {
    return VKR_EDITOR_OP_DONE;
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  ops_set(ctx, result, "entity", ops_entity(ctx, scene, ref.entity));
  ops_set(ctx, result, "outputs",
          ops_io_ports(ctx, scene, ref.entity, false_v));
  ops_set(ctx, result, "inputs", ops_io_ports(ctx, scene, ref.entity, true_v));
  VkrBakeryJson *outgoing = vkr_bakery_json_array(arena);
  VkrBakeryJson *incoming = vkr_bakery_json_array(arena);
  VkrEntityRef self = {0};
  const bool8_t referable = vkr_scene_entity_ref(scene, ref.entity, &self);
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    const SceneIoConnection *connection =
        vkr_scene_entity_alive(scene, entity)
            ? vkr_scene_get_typed(scene, entity, &vkr_scene_io_connection_type)
            : NULL;
    if (!connection) {
      continue;
    }
    if (ops_parent(scene, entity).u64 == ref.entity.u64) {
      vkr_bakery_json_append(outgoing, ops_io_connection(ctx, scene, entity));
    }
    if (referable &&
        MemCompare(&connection->target, &self, sizeof(self)) == 0) {
      vkr_bakery_json_append(incoming, ops_io_connection(ctx, scene, entity));
    }
  }
  ops_set(ctx, result, "connections", outgoing);
  ops_set(ctx, result, "incoming", incoming);
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

/* io.fire: sends `input` to `entity` during Play, as a connection would. */
static VkrEditorOpStatus ops_run_io_fire(OpsContext *ctx) {
  VkrEditorOpCall *call = ctx->call;
  const VkrSampleUiFrame *frame = ctx->frame;
  if (call->stage == 0u) {
    OpsRef ref;
    String8 input = {0};
    String8 value = {0};
    if (!ops_ref(ctx, NULL, vkr_bakery_json_get(call->args, "entity"), "entity",
                 &ref)) {
      return VKR_EDITOR_OP_DONE;
    }
    (void)vkr_bakery_json_get_string(call->args, "value", &value);
    if (!vkr_bakery_json_get_string(call->args, "input", &input) ||
        !input.length || input.length >= 64u || value.length >= 64u) {
      ops_fail(ctx, OPS_INVALID, "io.fire needs the entity's 'input'");
      return VKR_EDITOR_OP_DONE;
    }
    if (!frame->io_request || !frame->scripts_running) {
      ops_fail(ctx, OPS_BUSY,
               "Inputs reach entities only while the game "
               "plays");
      return VKR_EDITOR_OP_DONE;
    }
    VkrSampleIoRequest *request = frame->io_request;
    *request = (VkrSampleIoRequest){
        .token = ++ctx->ops->next_token, .target = ref.entity, .send = true_v};
    MemCopy(request->input, input.str, input.length);
    MemCopy(request->value, value.str, value.length);
    call->token = request->token;
    call->stage = 1u;
    return VKR_EDITOR_OP_WAIT;
  }
  const VkrSampleIoResult *result = frame->io_result;
  if (!result || result->token != call->token) {
    if (++call->frames > OPS_WAIT_FRAMES) {
      ops_fail(ctx, OPS_BUSY, "The input was not delivered");
      return VKR_EDITOR_OP_DONE;
    }
    return VKR_EDITOR_OP_WAIT;
  }
  if (!result->ok) {
    ops_fail(ctx, OPS_REJECTED, "%s", result->message);
    return VKR_EDITOR_OP_DONE;
  }
  call->result = vkr_bakery_json_object(call->arena);
  ops_set(ctx, call->result, "delivered",
          vkr_bakery_json_bool(call->arena, true_v));
  return VKR_EDITOR_OP_DONE;
}

/* io.trace: switches the `[io]` lines the router logs for each delivery. */
static VkrEditorOpStatus ops_run_io_trace(OpsContext *ctx) {
  bool8_t on = true_v;
  if (!vkr_bakery_json_get_bool(ctx->call->args, "on", &on) ||
      !ctx->frame->io_request) {
    ops_fail(ctx, OPS_INVALID, "io.trace needs 'on'");
    return VKR_EDITOR_OP_DONE;
  }
  ctx->frame->io_request->set_trace = true_v;
  ctx->frame->io_request->trace = on;
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "trace",
          vkr_bakery_json_bool(ops_arena(ctx), on));
  return VKR_EDITOR_OP_DONE;
}

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
  "\"dry_run\":{\"type\":\"boolean\"},\"select\":{\"type\":\"boolean\","       \
  "\"description\":\"Select the first entity once applied\"}"
#define OPS_BRUSH_SCHEMA                                                       \
  "\"name\":{\"type\":\"string\"},\"parent\":" OPS_ENTITY_SCHEMA               \
  ",\"container\":" OPS_CONTAINER_SCHEMA ",\"role\":{\"type\":\"string\","     \
  "\"enum\":[\"solid\",\"visual\",\"clip\",\"trigger\"]},\"material\":"        \
  "{\"type\":\"string\",\"description\":\"Material file; empty uses the dev "  \
  "grid\"},\"grid\":{\"type\":\"number\",\"description\":\"Snap step in "      \
  "meters; 0 turns snapping off\"}"
#define OPS_CAPSULE_SCHEMA                                                     \
  "\"radius\":{\"type\":\"number\"},\"height\":{\"type\":\"number\"},"         \
  "\"step_up\":{\"type\":\"number\"},\"max_slope\":{\"type\":\"number\","      \
  "\"description\":\"Degrees\"}"
#define OPS_SIDE_SCHEMA                                                        \
  "\"side\":{\"type\":\"string\",\"enum\":[\"top\",\"bottom\",\"+x\","         \
  "\"-x\",\"+z\",\"-z\"]}"
#define OPS_VALUES_SCHEMA                                                      \
  "{\"type\":\"object\",\"description\":\"Property values by descriptor "      \
  "name, as scene documents store them\"}"

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
     "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500},"
     "\"faces\":{\"type\":\"boolean\",\"description\":\"Also list brush "
     "faces and IO connections, which their owners otherwise summarise\"}}}",
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
     NULL, ops_build_create},
    {"entity.set", "Change an entity's name, pose or visibility.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"name\":{\"type\":\"string\"},\"position\":" OPS_VEC3_SCHEMA
     ",\"rotation\":" OPS_VEC3_SCHEMA ",\"scale\":" OPS_VEC3_SCHEMA
     ",\"visible\":{\"type\":\"boolean\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"entity\"]}",
     NULL, ops_build_set},
    {"entity.delete",
     "Delete an entity; recursive also deletes its descendants first.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"recursive\":{\"type\":\"boolean\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"entity\"]}",
     NULL, ops_build_delete},
    {"entity.parent",
     "Move an entity under another in the same scene, or to the root with "
     "null, keeping its world pose.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"parent\":{\"oneOf\":[" OPS_ENTITY_SCHEMA
     ",{\"type\":\"null\"}]}," OPS_REVIEW_SCHEMA "},\"required\":[\"entity\"]}",
     NULL, ops_build_parent},
    {"component.add", "Add a world component type with optional values.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"type\":{\"type\":\"string\"},\"values\":" OPS_VALUES_SCHEMA
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"entity\",\"type\"]}",
     NULL, ops_build_component_add},
    {"component.set",
     "Set property values of a component the entity carries, lights "
     "included; unnamed properties keep their values.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"type\":{\"type\":\"string\"},\"values\":" OPS_VALUES_SCHEMA
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"entity\",\"type\",\"values\"]}",
     NULL, ops_build_component_set},
    {"component.remove", "Remove a world component type.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"type\":{\"type\":\"string\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"entity\",\"type\"]}",
     NULL, ops_build_component_remove},
    {"brush.box",
     "Create a box brush between two corners, snapped to 'grid' (default "
     "1/16 m). Role solid renders and collides, visual only renders, clip "
     "only collides, trigger is a sensor volume.",
     "{\"type\":\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA "," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA
     "},\"required\":[\"min\",\"max\"]}",
     NULL, ops_build_brush_box},
    {"brush.wedge",
     "Create a wedge brush in a box: its top slopes down to the floor at the "
     "side 'slope' names (+x, -x, +z or -z).",
     "{\"type\":\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA ",\"slope\":{\"type\":\"string\",\"enum\":"
     "[\"+x\",\"-x\",\"+z\",\"-z\"]}," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA
     "},\"required\":[\"min\",\"max\"]}",
     NULL, ops_build_brush_wedge},
    {"brush.cylinder",
     "Create a vertical prism brush standing on 'center' with corners on "
     "a circle of 'radius'.",
     "{\"type\":\"object\",\"properties\":{\"center\":" OPS_VEC3_SCHEMA
     ",\"radius\":{\"type\":\"number\",\"exclusiveMinimum\":0},"
     "\"height\":{\"type\":\"number\",\"exclusiveMinimum\":0},\"sides\":"
     "{\"type\":\"integer\",\"minimum\":3,\"maximum\":32}," OPS_BRUSH_SCHEMA
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"center\",\"radius\",\"height\"]}",
     NULL, ops_build_brush_cylinder},
    {"brush.stairs",
     "Create solid stairs under a group: 'from' is the bottom front center, "
     "'to' the top back center; steps are at most step_height (default "
     "0.1875 m) high.",
     "{\"type\":\"object\",\"properties\":{\"from\":" OPS_VEC3_SCHEMA
     ",\"to\":" OPS_VEC3_SCHEMA ",\"width\":{\"type\":\"number\"},"
     "\"step_height\":{\"type\":\"number\"}," OPS_BRUSH_SCHEMA
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"from\",\"to\"]}",
     NULL, ops_build_brush_stairs},
    {"brush.set_material",
     "Set the material file of a brush's faces, all or those 'faces' "
     "selects (top, bottom, sides, +x, -x, +z, -z).",
     "{\"type\":\"object\",\"properties\":{\"brush\":" OPS_ENTITY_SCHEMA
     ",\"material\":{\"type\":\"string\"},\"faces\":{\"type\":\"array\","
     "\"items\":{\"type\":\"string\"}}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"brush\",\"material\"]}",
     NULL, ops_build_set_material},
    {"brush.move_face",
     "Move one face along its normal by 'distance' meters (negative moves "
     "it in). Name the 'face', or a 'brush' and its 'side'.",
     "{\"type\":\"object\",\"properties\":{\"face\":" OPS_ENTITY_SCHEMA
     ",\"brush\":" OPS_ENTITY_SCHEMA "," OPS_SIDE_SCHEMA
     ",\"distance\":{\"type\":\"number\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"distance\"]}",
     NULL, ops_build_move_face},
    {"brush.extrude", "Grow a new brush out of a face by 'distance' meters.",
     "{\"type\":\"object\",\"properties\":{\"face\":" OPS_ENTITY_SCHEMA
     ",\"brush\":" OPS_ENTITY_SCHEMA "," OPS_SIDE_SCHEMA
     ",\"distance\":{\"type\":\"number\",\"exclusiveMinimum\":0}"
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"distance\"]}",
     NULL, ops_build_extrude},
    {"brush.clip",
     "Cut a brush with the world plane through 'point' facing 'normal'; "
     "'keep' the back (behind the normal), the front or both pieces.",
     "{\"type\":\"object\",\"properties\":{\"brush\":" OPS_ENTITY_SCHEMA
     ",\"point\":" OPS_VEC3_SCHEMA ",\"normal\":" OPS_VEC3_SCHEMA
     ",\"keep\":{\"type\":\"string\",\"enum\":[\"back\",\"front\","
     "\"both\"]}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"brush\",\"point\",\"normal\"]}",
     NULL, ops_build_clip},
    {"brush.hollow",
     "Turn a brush into walls of 'thickness' meters around its inside.",
     "{\"type\":\"object\",\"properties\":{\"brush\":" OPS_ENTITY_SCHEMA
     ",\"thickness\":{\"type\":\"number\",\"exclusiveMinimum\":0}"
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"brush\"]}",
     NULL, ops_build_hollow},
    {"brush.carve",
     "Subtract the 'cutter' brush from 'target', or from every brush it "
     "touches; each becomes non-overlapping convex pieces. The cutter is "
     "deleted unless keep_cutter.",
     "{\"type\":\"object\",\"properties\":{\"cutter\":" OPS_ENTITY_SCHEMA
     ",\"target\":" OPS_ENTITY_SCHEMA ",\"keep_cutter\":{\"type\":"
     "\"boolean\"}," OPS_REVIEW_SCHEMA "},\"required\":[\"cutter\"]}",
     NULL, ops_build_carve},
    {"brush.merge",
     "Join 2 to 8 touching brushes into one, when their union is convex.",
     "{\"type\":\"object\",\"properties\":{\"brushes\":{\"type\":"
     "\"array\",\"items\":" OPS_ENTITY_SCHEMA ",\"minItems\":2,"
     "\"maxItems\":8}," OPS_REVIEW_SCHEMA "},\"required\":[\"brushes\"]}",
     NULL, ops_build_merge},
    {"blockout.room",
     "Create a closed room under a group: floor, ceiling and four walls of "
     "'wall' thickness around an interior box from 'min' (interior corner) "
     "of 'size'.",
     "{\"type\":\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"size\":" OPS_VEC3_SCHEMA ",\"wall\":{\"type\":\"number\"},"
     "\"ceiling\":{\"type\":\"boolean\"},\"floor_material\":{\"type\":"
     "\"string\"}," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA
     "},\"required\":[\"min\",\"size\"]}",
     NULL, ops_build_room},
    {"blockout.corridor",
     "Create a corridor under a group from floor point 'from' to 'to', open "
     "at both ends.",
     "{\"type\":\"object\",\"properties\":{\"from\":" OPS_VEC3_SCHEMA
     ",\"to\":" OPS_VEC3_SCHEMA ",\"width\":{\"type\":\"number\"},"
     "\"height\":{\"type\":\"number\"},\"wall\":{\"type\":\"number\"},"
     "\"ceiling\":{\"type\":\"boolean\"},\"floor_material\":{\"type\":"
     "\"string\"}," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA
     "},\"required\":[\"from\",\"to\"]}",
     NULL, ops_build_corridor},
    {"blockout.doorway",
     "Cut a doorway through an existing box wall brush: it becomes up to "
     "three brushes around an opening 'width' wide and 'height' high, "
     "'offset' meters from the wall's center along its length.",
     "{\"type\":\"object\",\"properties\":{\"wall\":" OPS_ENTITY_SCHEMA
     ",\"offset\":{\"type\":\"number\"},\"width\":{\"type\":\"number\"},"
     "\"height\":{\"type\":\"number\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"wall\"]}",
     NULL, ops_build_doorway},
    {"terrain.create",
     "Create a heightfield terrain of 'size' metres a side (a multiple of 64 "
     "'spacing' cells, at most 1024 cells) centred on 'position', flat at "
     "local 'height', heights stored between 'height_min' and 'height_max'. "
     "'layer0' to 'layer3' name the materials paint blends (default dev grid, "
     "floor, orange and blue).",
     "{\"type\":\"object\",\"properties\":{\"position\":" OPS_VEC3_SCHEMA
     ",\"size\":{\"type\":\"number\"},\"spacing\":{\"type\":\"number\"},"
     "\"height\":{\"type\":\"number\"},\"height_min\":{\"type\":\"number\"},"
     "\"height_max\":{\"type\":\"number\"},\"texture_size\":{\"type\":"
     "\"number\"},\"name\":{\"type\":\"string\"},\"layer0\":{\"type\":"
     "\"string\"},\"layer1\":{\"type\":\"string\"},\"layer2\":{\"type\":"
     "\"string\"},\"layer3\":{\"type\":\"string\"},"
     "\"container\":" OPS_CONTAINER_SCHEMA "," OPS_REVIEW_SCHEMA "}}",
     NULL, ops_build_terrain_create},
    {"terrain.brush",
     "Sculpt or paint with a round brush at each of 'points' (or one "
     "'point'): 'mode' raise or lower by 'strength' metres, smooth or "
     "flatten (toward 'height', default the point's y) by a 0-1 'strength', "
     "or paint 'layer' 1-4. Falls off to zero at 'radius'.",
     "{\"type\":\"object\",\"properties\":{\"terrain\":" OPS_ENTITY_SCHEMA
     ",\"mode\":{\"type\":\"string\",\"enum\":[\"raise\",\"lower\","
     "\"smooth\",\"flatten\",\"paint\"]},\"point\":" OPS_VEC3_SCHEMA
     ",\"points\":{\"type\":\"array\",\"items\":" OPS_VEC3_SCHEMA
     ",\"maxItems\":256},\"radius\":{\"type\":\"number\"},\"strength\":"
     "{\"type\":\"number\"},\"height\":{\"type\":\"number\"},\"layer\":"
     "{\"type\":\"integer\",\"minimum\":1,\"maximum\":4}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"terrain\"]}",
     NULL, ops_build_terrain_brush},
    {"terrain.flatten",
     "Level the footprint from 'min' to 'max' (world x and z) at 'height' "
     "(default the lower y), blending into the ground over 'falloff' "
     "metres.",
     "{\"type\":\"object\",\"properties\":{\"terrain\":" OPS_ENTITY_SCHEMA
     ",\"min\":" OPS_VEC3_SCHEMA ",\"max\":" OPS_VEC3_SCHEMA
     ",\"height\":{\"type\":\"number\"},\"falloff\":{\"type\":"
     "\"number\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"terrain\",\"min\",\"max\"]}",
     NULL, ops_build_terrain_flatten},
    {"terrain.ramp",
     "Shape a straight slope 'width' metres wide from 'from' to 'to' (world "
     "points on the ramp's surface), blending over 'falloff' metres.",
     "{\"type\":\"object\",\"properties\":{\"terrain\":" OPS_ENTITY_SCHEMA
     ",\"from\":" OPS_VEC3_SCHEMA ",\"to\":" OPS_VEC3_SCHEMA
     ",\"width\":{\"type\":\"number\"},\"falloff\":{\"type\":"
     "\"number\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"terrain\",\"from\",\"to\"]}",
     NULL, ops_build_terrain_ramp},
    {"terrain.stamp",
     "Stamp a grayscale 'image' file over a square of 'size' metres at "
     "'center': 'mode' add raises by the image times 'height_scale'; set "
     "makes heights center.y plus that.",
     "{\"type\":\"object\",\"properties\":{\"terrain\":" OPS_ENTITY_SCHEMA
     ",\"image\":{\"type\":\"string\"},\"center\":" OPS_VEC3_SCHEMA
     ",\"size\":{\"type\":\"number\"},\"height_scale\":{\"type\":"
     "\"number\"},\"mode\":{\"type\":\"string\",\"enum\":[\"add\","
     "\"set\"]}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"terrain\",\"image\",\"center\"]}",
     NULL, ops_build_terrain_stamp},
    {"terrain.sample",
     "Ground heights (world y) of a terrain at [x, z] points; null outside.",
     "{\"type\":\"object\",\"properties\":{\"terrain\":" OPS_ENTITY_SCHEMA
     ",\"points\":{\"type\":\"array\",\"maxItems\":1024,\"items\":{"
     "\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":2,"
     "\"maxItems\":2}}},\"required\":[\"terrain\",\"points\"]}",
     ops_run_terrain_sample, NULL},
    {"io.connect",
     "Connect the source's 'output' to the 'input' of 'target' in the same "
     "scene: when the source fires it, the target receives the input after "
     "'delay' seconds, at most 'limit' times per session (0 unlimited). "
     "'value' replaces the output's value as text. io.list names outputs "
     "and inputs.",
     "{\"type\":\"object\",\"properties\":{\"source\":" OPS_ENTITY_SCHEMA
     ",\"output\":{\"type\":\"string\"},\"target\":" OPS_ENTITY_SCHEMA
     ",\"input\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"},"
     "\"delay\":{\"type\":\"number\",\"minimum\":0},\"limit\":{\"type\":"
     "\"integer\",\"minimum\":0}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"source\",\"output\"]}",
     NULL, ops_build_connect},
    {"io.disconnect",
     "Remove one 'connection', or the source's connections (from one "
     "'output' when given).",
     "{\"type\":\"object\",\"properties\":{\"connection\":" OPS_ENTITY_SCHEMA
     ",\"source\":" OPS_ENTITY_SCHEMA
     ",\"output\":{\"type\":\"string\"}," OPS_REVIEW_SCHEMA "}}",
     NULL, ops_build_disconnect},
    {"io.list",
     "An entity's outputs and inputs, its connections and the connections "
     "that reach it, each with the reason it does not route if it does not.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     "},\"required\":[\"entity\"]}",
     ops_run_io_list, NULL},
    {"io.fire",
     "While the game plays, send 'input' (with optional 'value' text) to an "
     "entity as a connection would.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"input\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},"
     "\"required\":[\"entity\",\"input\"]}",
     ops_run_io_fire, NULL},
    {"io.trace", "Turn the [io] log line of each delivery on or off.",
     "{\"type\":\"object\",\"properties\":{\"on\":{\"type\":\"boolean\"}},"
     "\"required\":[\"on\"]}",
     ops_run_io_trace, NULL},
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
    {"level.lint",
     "Check a region's walkable floor against the player capsule: steps too "
     "high, slopes too steep, low ceilings, gaps too narrow, edges into the "
     "void, areas the start (or the Player Start) cannot reach, overlapping "
     "solid brushes, brushes that did not build and IO connections that "
     "will not route.",
     "{\"type\":\"object\",\"properties\":{\"region\":{\"type\":"
     "\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA
     "},\"required\":[\"min\",\"max\"]},\"start\":" OPS_VEC3_SCHEMA
     ",\"container\":" OPS_CONTAINER_SCHEMA "," OPS_CAPSULE_SCHEMA
     ",\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}},"
     "\"required\":[\"region\"]}",
     ops_run_lint, NULL},
    {"query.reachable",
     "Whether the player capsule can walk from one floor point to another, "
     "with the route.",
     "{\"type\":\"object\",\"properties\":{\"from\":" OPS_VEC3_SCHEMA
     ",\"to\":" OPS_VEC3_SCHEMA ",\"container\":" OPS_CONTAINER_SCHEMA
     "," OPS_CAPSULE_SCHEMA "},\"required\":[\"from\",\"to\"]}",
     ops_run_reachable, NULL},
    {"query.bounds", "World bounds of an entity and its descendants.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     "},\"required\":[\"entity\"]}",
     ops_run_bounds, NULL},
    {"view.capture",
     "Capture the Scene as a PNG, optionally from another view (top is an "
     "orthographic map), framed on an entity or box, from a perspective "
     "'eye' looking at 'target', with grid labels.",
     "{\"type\":\"object\",\"properties\":{\"view\":{\"type\":\"string\","
     "\"enum\":[\"current\",\"perspective\",\"top\",\"left\",\"right\","
     "\"bottom\"]},\"focus\":{\"oneOf\":[" OPS_ENTITY_SCHEMA
     ",{\"type\":\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA "},\"required\":[\"min\",\"max\"]}]},"
     "\"grid_labels\":{\"type\":\"boolean\"},\"eye\":" OPS_VEC3_SCHEMA
     ",\"target\":" OPS_VEC3_SCHEMA ",\"area\":{\"type\":"
     "\"string\",\"enum\":[\"scene\",\"window\"],\"description\":\"The "
     "Scene image (default) or the whole editor window\"}}}",
     ops_run_capture, NULL},
    {"view.camera",
     "Place the perspective Scene camera at 'eye' looking at 'target'.",
     "{\"type\":\"object\",\"properties\":{\"eye\":" OPS_VEC3_SCHEMA
     ",\"target\":" OPS_VEC3_SCHEMA "},\"required\":[\"eye\",\"target\"]}",
     ops_run_camera, NULL},
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
  return def->run ? def->run(&ctx) : ops_run_write(&ctx, def);
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
