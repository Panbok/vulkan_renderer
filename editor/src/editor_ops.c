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
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_brush.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_scene_partition.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_population.h"
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
#endif

#define OPS_MALFORMED "VKR-AGENT-0001"
#define OPS_UNKNOWN "VKR-AGENT-0002"
#define OPS_INVALID "VKR-AGENT-0003"
#define OPS_NOT_FOUND "VKR-AGENT-0004"
#define OPS_REJECTED "VKR-AGENT-0005"
#define OPS_BUSY "VKR-AGENT-0006"
#define OPS_CAPTURE "VKR-AGENT-0007"
#define OPS_LIMIT "VKR-AGENT-0008"
#define OPS_NOT_OWNER "VKR-AGENT-0009"
#define OPS_CLAIMED "VKR-AGENT-0010"

/* Entities scene.describe lists per request, and its default page. */
#define OPS_DESCRIBE_MAX 500u
#define OPS_DESCRIBE_DEFAULT 200u
/* Cells partition.describe lists. */
#define OPS_PARTITION_CELLS_MAX 500u
/* Builds a batch or capture may wait before it reports a timeout. */
#define OPS_WAIT_FRAMES 600u
/* Capture PNGs kept in the private directory. */
#define OPS_CAPTURE_KEEP 32u
/* Builds a capture waits after changing the view, so the frame shows it. */
#define OPS_CAPTURE_SETTLE_FRAMES 4u
/* A windowed editor captures after this long without the designer's input,
   and gives up after the limit. */
#define OPS_CAPTURE_IDLE_SECONDS 1.5
#define OPS_CAPTURE_IDLE_LIMIT_SECONDS 20.0
/* Builds a request waits for brush, shape, terrain and scatter rebuilds
   before it answers with `settled` false. */
#define OPS_SCENE_SETTLE_FRAMES 240u
/* Newest agent batches whose author an agent's undo can check. */
#define OPS_AUTHORED_MAX 64u
/* World boxes one batch records for the regions its terrain edits change. */
#define OPS_TOUCHED_MAX 64u
/* Events the change feed keeps, and the entities one event names. */
#define OPS_FEED_MAX 128u
#define OPS_FEED_ENTITIES 16u

typedef enum OpsFeedKind {
  OPS_FEED_APPLIED = 0,
  OPS_FEED_ACCEPTED,
  OPS_FEED_REJECTED,
  OPS_FEED_CLAIMED,
  OPS_FEED_RELEASED,
  OPS_FEED_KIND_COUNT,
} OpsFeedKind;

/* One event of the change feed (changes.feed): a batch applied, a change
   accepted or rejected, or a claim set or released, with the box it
   covers. */
typedef struct OpsFeedEvent {
  uint64_t sequence;
  OpsFeedKind kind;
  uint32_t change;
  uint32_t claim;
  uint16_t container;
  bool8_t bounded;
  char author[VKR_EDITOR_AUTHOR_CAPACITY];
  char label[96];
  Vec3 min;
  Vec3 max;
  uint32_t entity_count;
  VkrEntityId entities[OPS_FEED_ENTITIES];
} OpsFeedEvent;

/* The journal group of an agent batch, reviewed or not, and its author. */
typedef struct OpsAuthored {
  uint64_t group;
  uint16_t container;
  char author[VKR_EDITOR_AUTHOR_CAPACITY];
} OpsAuthored;

struct VkrEditorOps {
  VkrAllocator *allocator;
  /* Reused batch storage, grown up to VKR_SAMPLE_EDIT_BATCH_MAX as a batch
     needs; the runtime borrows it until it dispatches, and one batch is in
     flight at a time. A build addresses earlier items by index, since a
     growth moves them. */
  VkrSampleEditBatchItem *items;
  uint32_t capacity;
  uint64_t next_token;
  VkrEditorChange changes[VKR_EDITOR_CHANGE_MAX];
  uint32_t change_count;
  uint32_t next_change_id;
  /* Batches have applied unreviewed since the list last had room; the
     designer hears of it once per run. */
  bool8_t review_overflow;
  uint32_t capture_serial;
  /* A ring of the newest agent batches; `authored_next` counts every one
     recorded. */
  OpsAuthored authored[OPS_AUTHORED_MAX];
  uint32_t authored_next;
  /* The Changes window's author filter, empty for every author, and the
     time its two-click Reject stops waiting for the confirming click. */
  char changes_filter[VKR_EDITOR_AUTHOR_CAPACITY];
  float64_t reject_all_armed_until;
  /* Claimed boxes of the scene generation `claims_generation`, kept in the
     scene's claims file (ops_claims_save). */
  VkrEditorClaim claims[VKR_EDITOR_CLAIM_MAX];
  uint32_t claim_count;
  uint32_t next_claim_id;
  uint64_t claims_generation;
  /* A ring of the newest feed events, each in slot `sequence %
     OPS_FEED_MAX`; `feed_last` is the newest sequence, zero for none. */
  OpsFeedEvent feed[OPS_FEED_MAX];
  uint64_t feed_last;
  /* The level check the running request samples over builds; one request
     runs at a time, so one slot. */
  VkrEditorLevelJob *level_job;
  /* Platform time of the last key, button, wheel or pointer motion, so a
     windowed editor's captures wait for the designer to stop. */
  float64_t input_last;
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
  /* World boxes the batch's terrain edits may change, for claims. */
  uint32_t touched_count;
  Vec3 touched_min[OPS_TOUCHED_MAX];
  Vec3 touched_max[OPS_TOUCHED_MAX];
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

/* A cheap read that changes nothing: it may run in a build after other
   cheap reads, and takes `settle`. */
#define OPS_QUICK 1u
/* A read of collision or built geometry: it waits for the scene to settle
   unless `settle` is false. */
#define OPS_SETTLES 2u

typedef struct OpsDef {
  const char *name;
  const char *description;
  /* JSON Schema of `args`. */
  const char *schema;
  /* Read operations and batches run; write operations build batch items. */
  OpsRun run;
  OpsBuild build;
  /* OPS_QUICK and OPS_SETTLES. */
  uint32_t flags;
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
/* An optional array of `count` finite numbers named `key`. */
static bool8_t ops_arg_floats(OpsContext *ctx, const VkrBakeryJson *args,
                              const char *key, float32_t *out, uint32_t count,
                              bool8_t *present) {
  const VkrBakeryJson *value = vkr_bakery_json_get(args, key);
  if (present) {
    *present = value != NULL;
  }
  if (!value) {
    return true_v;
  }
  if (value->type != VKR_BAKERY_JSON_ARRAY || value->count != count) {
    return ops_fail(ctx, OPS_INVALID, "'%s' must be an array of %u numbers",
                    key, count);
  }
  for (uint32_t i = 0; i < count; ++i) {
    const VkrBakeryJson *part = vkr_bakery_json_at(value, i);
    if (part->type == VKR_BAKERY_JSON_INT) {
      out[i] = (float32_t)part->integer;
    } else if (part->type == VKR_BAKERY_JSON_FLOAT) {
      out[i] = (float32_t)part->number;
    } else {
      return ops_fail(ctx, OPS_INVALID, "'%s' must be an array of %u numbers",
                      key, count);
    }
    if (!isfinite(out[i])) {
      return ops_fail(ctx, OPS_INVALID, "'%s' must be finite", key);
    }
  }
  return true_v;
}

static bool8_t ops_arg_vec3(OpsContext *ctx, const VkrBakeryJson *args,
                            const char *key, Vec3 *out, bool8_t *present) {
  float32_t parts[3] = {0};
  bool8_t found = false_v;
  if (!ops_arg_floats(ctx, args, key, parts, 3u, &found)) {
    return false_v;
  }
  if (present) {
    *present = found;
  }
  if (found) {
    *out = vec3_new(parts[0], parts[1], parts[2]);
  }
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

/* What the loaded scenes still rebuild after their edits: brushes and
   shapes, terrain meshes and collision, and scatter or spline copies. */
typedef struct OpsSettle {
  uint32_t brushes;
  uint32_t population;
  bool8_t terrain;
  bool8_t loading;
} OpsSettle;

/* Whether nothing rebuilds and no scene loads. The scene does not rebuild
   while the simulation runs, so Play counts as settled. */
static bool8_t ops_settled(const VkrSampleUiFrame *frame, OpsSettle *out) {
  OpsSettle settle = {.loading = frame->scene_loading ||
                                 frame->additive_loading ||
                                 frame->world_loading};
  if (!frame->simulation_running) {
    const VkrScene *scenes[VKR_SCENE_ADDITIVE_MAX + 2u] = {frame->scene,
                                                           frame->world};
    for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
      scenes[i + 2u] = frame->additive[i];
    }
    for (uint32_t i = 0; i < ArrayCount(scenes); ++i) {
      if (!scenes[i]) {
        continue;
      }
      settle.brushes += vkr_scene_brush_pending(scenes[i]);
      settle.population += vkr_scene_population_pending(scenes[i]);
      settle.terrain = settle.terrain || !vkr_scene_terrain_settled(scenes[i]);
    }
  }
  if (out) {
    *out = settle;
  }
  return !settle.loading && !settle.brushes && !settle.population &&
         !settle.terrain;
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

/* Adds what a write made of `entity` to its result `entry`: its world
   bounds, and the build status of a brush, shape, terrain or population
   rule. */
static void ops_entity_built(OpsContext *ctx, const VkrScene *scene,
                             VkrEntityId entity, VkrBakeryJson *entry) {
  if (!scene || !entity.u64 || !vkr_scene_entity_alive(scene, entity)) {
    return;
  }
  Vec3 lo = {0};
  Vec3 hi = {0};
  if (ops_world_bounds(scene, entity, &lo, &hi)) {
    VkrBakeryJson *bounds = vkr_bakery_json_object(ops_arena(ctx));
    ops_set(ctx, bounds, "min", ops_vec3(ctx, lo));
    ops_set(ctx, bounds, "max", ops_vec3(ctx, hi));
    ops_set(ctx, entry, "bounds", bounds);
  }
  const char *status = NULL;
  if (vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type) ||
      vkr_scene_get_typed(scene, entity, &vkr_scene_blockout_type)) {
    status = vkr_scene_brush_status(scene, entity);
    status = status ? status : "built";
  } else if (vkr_scene_get_typed(scene, entity, &vkr_scene_terrain_type)) {
    status = vkr_scene_terrain_status(scene, entity);
    status = status ? status : "loaded";
  } else if (vkr_scene_get_typed(scene, entity, &vkr_scene_spline_mesh_type) ||
             vkr_scene_get_typed(scene, entity, &vkr_scene_scatter_type)) {
    status = vkr_scene_population_status(scene, entity);
    status = status ? status : "placed";
    ops_set(ctx, entry, "copies",
            vkr_bakery_json_int(ops_arena(ctx),
                                vkr_scene_population_instances(scene, entity)));
  }
  if (status) {
    ops_set(ctx, entry, "status", vkr_bakery_json_cstr(ops_arena(ctx), status));
  }
}

// -----------------------------------------------------------------------------
// Claims and the change feed (ADR-084)
// -----------------------------------------------------------------------------

/* Whether two boxes overlap with volume; boxes that only touch, as a wall
   flush against a neighbour's claim, do not. */
static bool8_t ops_boxes_overlap(Vec3 a_lo, Vec3 a_hi, Vec3 b_lo, Vec3 b_hi) {
  return a_lo.x < b_hi.x && a_hi.x > b_lo.x && a_lo.y < b_hi.y &&
         a_hi.y > b_lo.y && a_lo.z < b_hi.z && a_hi.z > b_lo.z;
}

/* A claim of another author than `author` in `container` that the box
   overlaps, or NULL. The editor's own requests have no author and ignore
   claims. */
static const VkrEditorClaim *ops_claim_hit(const VkrEditorOps *ops,
                                           const char *author,
                                           uint16_t container, Vec3 lo,
                                           Vec3 hi) {
  for (uint32_t i = 0; author[0] && i < ops->claim_count; ++i) {
    const VkrEditorClaim *claim = &ops->claims[i];
    if (claim->container == container && strcmp(claim->author, author) &&
        ops_boxes_overlap(lo, hi, claim->min, claim->max)) {
      return claim;
    }
  }
  return NULL;
}

/* The world box claims and the feed judge `entity` by: its solids (brushes
   and blockout pieces, from their faces), else its meshes, else its
   position. */
static void ops_object_box(const VkrScene *scene, VkrEntityId entity,
                           VkrBrushGeometry *scratch, Vec3 *lo, Vec3 *hi) {
  if (scratch && vkr_editor_entity_world_box(scene, entity, scratch, lo, hi)) {
    return;
  }
  if (ops_world_bounds(scene, entity, lo, hi)) {
    return;
  }
  const SceneTransform *transform = ops_transform(scene, entity);
  *lo = *hi = transform ? mat4_position(transform->world) : vec3_zero();
}

/* `name` under the user's private agent directory (editor_agent.c), made
   when missing: captures and claims live there. */
static bool8_t ops_private_directory(const char *name, char *out,
                                     uint64_t capacity) {
  char directory[256];
  if (!vkr_editor_agent_directory(directory, sizeof(directory))) {
    return false_v;
  }
  snprintf(out, capacity, "%s/%s", directory, name);
#if !defined(_WIN32)
  (void)mkdir(out, 0700);
#else
  const FilePath path = {
      .path = string8_create_from_cstr((const uint8_t *)out, strlen(out)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  (void)file_create_directory(&path);
#endif
  return true_v;
}

/* Claims outlive an editor restart: each scene's claims live in a file of
   the private agent directory named by a hash of the scene's path, which
   the file repeats so a collision never loads another scene's claims. Two
   editors on one scene share the file; the last writer wins. */
static bool8_t ops_claims_path(const VkrSampleUiFrame *frame, char *out,
                               uint64_t capacity) {
  char directory[300];
  if (!frame->scene_path.length ||
      !ops_private_directory("claims", directory, sizeof(directory))) {
    return false_v;
  }
  uint64_t hash = 1469598103934665603ull;
  for (uint64_t i = 0; i < frame->scene_path.length; ++i) {
    hash = (hash ^ frame->scene_path.str[i]) * 1099511628211ull;
  }
  snprintf(out, capacity, "%s/%016llx.json", directory,
           (unsigned long long)hash);
  return true_v;
}

/* Writes the scene's claims, replacing the file at once; no claims removes
   it. */
static void ops_claims_save(const VkrEditorOps *ops,
                            const VkrSampleUiFrame *frame) {
  char path[512];
  if (!ops_claims_path(frame, path, sizeof(path))) {
    return;
  }
  const FilePath target = {
      .path = string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  if (!ops->claim_count) {
    (void)file_remove(&target);
    return;
  }
  Arena *arena = arena_create(KB(64), KB(64));
  if (!arena) {
    return;
  }
  VkrBakeryJson *root = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, root, "version", vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, root, "scene",
                      vkr_bakery_json_string(arena, frame->scene_path));
  vkr_bakery_json_set(arena, root, "next",
                      vkr_bakery_json_int(arena, ops->next_claim_id));
  VkrBakeryJson *claims = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < ops->claim_count; ++i) {
    const VkrEditorClaim *claim = &ops->claims[i];
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    VkrBakeryJson *lo = vkr_bakery_json_array(arena);
    VkrBakeryJson *hi = vkr_bakery_json_array(arena);
    const float32_t *lo_values = &claim->min.x;
    const float32_t *hi_values = &claim->max.x;
    for (uint32_t axis = 0; axis < 3u; ++axis) {
      vkr_bakery_json_append(lo, vkr_bakery_json_float(arena, lo_values[axis]));
      vkr_bakery_json_append(hi, vkr_bakery_json_float(arena, hi_values[axis]));
    }
    vkr_bakery_json_set(arena, entry, "id",
                        vkr_bakery_json_int(arena, claim->id));
    vkr_bakery_json_set(arena, entry, "author",
                        vkr_bakery_json_cstr(arena, claim->author));
    vkr_bakery_json_set(arena, entry, "name",
                        vkr_bakery_json_cstr(arena, claim->name));
    vkr_bakery_json_set(arena, entry, "container",
                        vkr_bakery_json_int(arena, claim->container));
    vkr_bakery_json_set(arena, entry, "min", lo);
    vkr_bakery_json_set(arena, entry, "max", hi);
    vkr_bakery_json_append(claims, entry);
  }
  vkr_bakery_json_set(arena, root, "claims", claims);
  String8 text = {0};
  char temporary[520];
  snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  bool8_t written = false_v;
  FILE *file =
      vkr_bakery_json_write(arena, root, VKR_BAKERY_JSON_COMPACT, &text)
          ? file_fopen(temporary, "wb")
          : NULL;
  if (file) {
    written = fwrite(text.str, 1u, text.length, file) == text.length;
    written = fclose(file) == 0 && written;
  }
  const FilePath staged = {.path = string8_create_from_cstr(
                               (const uint8_t *)temporary, strlen(temporary)),
                           .type = FILE_PATH_TYPE_ABSOLUTE};
  if (!written || file_rename(&staged, &target, true_v) != FILE_ERROR_NONE) {
    log_warn("Agent claims: could not write %s", path);
    (void)file_remove(&staged);
  }
  arena_destroy(arena);
}

/* Reads the claims of the scene that just loaded, replacing any others. */
static void ops_claims_load(VkrEditorOps *ops, const VkrSampleUiFrame *frame) {
  ops->claim_count = 0u;
  char path[512];
  FILE *file = ops_claims_path(frame, path, sizeof(path))
                   ? file_fopen(path, "rb")
                   : NULL;
  if (!file) {
    return;
  }
  Arena *arena = arena_create(KB(256), KB(64));
  uint8_t *text =
      arena ? arena_alloc(arena, KB(64), ARENA_MEMORY_TAG_STRING) : NULL;
  const size_t length = text ? fread(text, 1u, KB(64), file) : 0u;
  fclose(file);
  const VkrBakeryJson *root =
      length && length < KB(64)
          ? vkr_bakery_json_parse(arena, text, length, 16u, NULL)
          : NULL;
  String8 scene = {0};
  int64_t version = 0;
  int64_t next = 0;
  const VkrBakeryJson *claims =
      root ? vkr_bakery_json_get(root, "claims") : NULL;
  if (!root || !vkr_bakery_json_get_int(root, "version", &version) ||
      version != 1 || !vkr_bakery_json_get_string(root, "scene", &scene) ||
      !string8_equals(&scene, &frame->scene_path) || !claims ||
      claims->type != VKR_BAKERY_JSON_ARRAY) {
    if (arena) {
      arena_destroy(arena);
    }
    return;
  }
  (void)vkr_bakery_json_get_int(root, "next", &next);
  for (const VkrBakeryJson *entry = claims->first;
       entry && ops->claim_count < VKR_EDITOR_CLAIM_MAX; entry = entry->next) {
    int64_t id = 0;
    int64_t container = 0;
    String8 author = {0};
    String8 name = {0};
    const VkrBakeryJson *lo = vkr_bakery_json_get(entry, "min");
    const VkrBakeryJson *hi = vkr_bakery_json_get(entry, "max");
    if (!vkr_bakery_json_get_int(entry, "id", &id) || id <= 0 ||
        !vkr_bakery_json_get_int(entry, "container", &container) ||
        !vkr_bakery_json_get_string(entry, "author", &author) ||
        !author.length || !lo || !hi || lo->type != VKR_BAKERY_JSON_ARRAY ||
        hi->type != VKR_BAKERY_JSON_ARRAY || lo->count != 3u ||
        hi->count != 3u) {
      continue;
    }
    (void)vkr_bakery_json_get_string(entry, "name", &name);
    VkrEditorClaim claim = {.id = (uint32_t)id,
                            .container = (uint16_t)container};
    snprintf(claim.author, sizeof(claim.author), "%.*s",
             (int)Min(author.length, (uint64_t)sizeof(claim.author) - 1u),
             (const char *)author.str);
    snprintf(claim.name, sizeof(claim.name), "%.*s",
             (int)Min(name.length, (uint64_t)sizeof(claim.name) - 1u),
             (const char *)name.str);
    float32_t *lo_values = &claim.min.x;
    float32_t *hi_values = &claim.max.x;
    const VkrBakeryJson *a = lo->first;
    const VkrBakeryJson *b = hi->first;
    for (uint32_t axis = 0; axis < 3u; ++axis, a = a->next, b = b->next) {
      lo_values[axis] =
          (float32_t)(a->type == VKR_BAKERY_JSON_INT ? (float64_t)a->integer
                                                     : a->number);
      hi_values[axis] =
          (float32_t)(b->type == VKR_BAKERY_JSON_INT ? (float64_t)b->integer
                                                     : b->number);
    }
    ops->claims[ops->claim_count++] = claim;
    ops->next_claim_id = Max(ops->next_claim_id, claim.id);
  }
  ops->next_claim_id = Max(ops->next_claim_id, (uint32_t)Max(next, 0));
  arena_destroy(arena);
}

/* Appends a feed event and returns it for the caller to fill. */
static OpsFeedEvent *ops_feed_add(VkrEditorOps *ops, OpsFeedKind kind,
                                  uint16_t container, const char *author,
                                  const char *label) {
  const uint64_t sequence = ++ops->feed_last;
  OpsFeedEvent *event = &ops->feed[sequence % OPS_FEED_MAX];
  *event = (OpsFeedEvent){
      .sequence = sequence, .kind = kind, .container = container};
  snprintf(event->author, sizeof(event->author), "%s", author);
  snprintf(event->label, sizeof(event->label), "%s", label);
  return event;
}

/* Names `entity` in `event` and grows its box by the entity's. */
static void ops_feed_entity(OpsFeedEvent *event, const VkrScene *scene,
                            VkrEntityId entity, VkrBrushGeometry *scratch) {
  if (!scene || !entity.u64 || !vkr_scene_entity_alive(scene, entity)) {
    return;
  }
  for (uint32_t i = 0; i < event->entity_count; ++i) {
    if (event->entities[i].u64 == entity.u64) {
      return;
    }
  }
  if (event->entity_count < OPS_FEED_ENTITIES) {
    event->entities[event->entity_count++] = entity;
  }
  if (vkr_scene_get_typed(scene, entity, &vkr_scene_terrain_type)) {
    return;
  }
  Vec3 lo = {0};
  Vec3 hi = {0};
  ops_object_box(scene, entity, scratch, &lo, &hi);
  event->min = event->bounded
                   ? vec3_new(Min(event->min.x, lo.x), Min(event->min.y, lo.y),
                              Min(event->min.z, lo.z))
                   : lo;
  event->max = event->bounded
                   ? vec3_new(Max(event->max.x, hi.x), Max(event->max.y, hi.y),
                              Max(event->max.z, hi.z))
                   : hi;
  event->bounded = true_v;
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

bool8_t vkr_editor_ops_component_text(const VkrTypeDesc *type,
                                      const void *value, char *out,
                                      uint64_t capacity) {
  VkrBakeryBuffer buffer = {0};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, ops_buffer_sink, &buffer);
  const bool8_t ok = vkr_type_write_json(&writer, type, value) &&
                     vkr_json_writer_complete(&writer) && !buffer.failed &&
                     buffer.length < capacity;
  if (ok) {
    MemCopy(out, buffer.data, buffer.length);
    out[buffer.length] = '\0';
  }
  vkr_bakery_buffer_free(&buffer);
  return ok;
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
  VkrEditorOps *ops = ctx->ops;
  if (batch->count >= ops->capacity) {
    const uint32_t capacity =
        Min(ops->capacity * 2u, (uint32_t)VKR_SAMPLE_EDIT_BATCH_MAX);
    VkrSampleEditBatchItem *grown =
        realloc(ops->items, (size_t)capacity * sizeof(*grown));
    if (!grown) {
      ops_fail(ctx, OPS_LIMIT, "Out of memory for %u edits", capacity);
      return NULL;
    }
    ops->items = grown;
    ops->capacity = capacity;
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

static Vec3 ops_snap_best(Vec3 lo, Vec3 hi, float32_t floor, const Vec3 *lows,
                          const Vec3 *highs, const float32_t *floors,
                          uint32_t count, float32_t gap);
static bool8_t ops_move_world(OpsContext *ctx, OpsBatch *batch,
                              const OpsRef *ref, const VkrScene *scene,
                              Vec3 shift);

/* entity.parent: the entity under `parent`, or a root, keeping its world
   pose; `snap` first sets it flush against the parent unless it is free. */
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
  /* With `snap`, the child first moves flush against its new parent, keeping
     that world pose through the reparent. */
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  if (ops_arg_bool(args, "snap", false_v) && parent.entity.u64 &&
      parent.item < 0 && ref.item < 0 && scene &&
      !vkr_editor_entity_free(scene, ref.entity)) {
    VkrBrushGeometry *geometry =
        arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
    Vec3 lo = vec3_zero();
    Vec3 hi = vec3_zero();
    Vec3 plo = vec3_zero();
    Vec3 phi = vec3_zero();
    VkrEntityId faces[VKR_BRUSH_FACE_MAX];
    const bool8_t parent_brush =
        geometry &&
        vkr_scene_get_typed(scene, parent.entity, &vkr_scene_brush_type) &&
        vkr_editor_brush_build(scene, parent.entity, geometry, faces);
    if (parent_brush) {
      plo = geometry->min;
      phi = geometry->max;
    }
    if (geometry &&
        vkr_editor_entity_world_box(scene, ref.entity, geometry, &lo, &hi) &&
        (parent_brush || vkr_editor_entity_world_box(scene, parent.entity,
                                                     geometry, &plo, &phi))) {
      const float32_t floor =
          vkr_editor_entity_floor(scene, ref.entity, geometry, lo, hi);
      const float32_t pfloor =
          parent_brush ? (vkr_editor_box_slab(plo, phi) ? phi.y : plo.y)
                       : vkr_editor_entity_floor(scene, parent.entity, geometry,
                                                 plo, phi);
      const Vec3 shift =
          ops_snap_best(lo, hi, floor, &plo, &phi, &pfloor, 1u, 0.0f);
      if (vec3_length(shift) > 1.0e-6f &&
          !ops_move_world(ctx, batch, &ref, scene, shift)) {
        return false_v;
      }
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

static bool8_t ops_build_blockout_build(OpsContext *ctx,
                                        const VkrBakeryJson *args,
                                        OpsBatch *batch);

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
  /* A blockout shape's settings build its brushes again with them. */
  if (action == VKR_SCENE_EDIT_APPLY && type == &vkr_scene_blockout_type &&
      ref.item < 0) {
    return ops_build_blockout_build(ctx, args, batch);
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
// Brushes (ADR-084)
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

static uint32_t ops_arg_points(OpsContext *ctx, const VkrBakeryJson *args,
                               const char *key, Vec3 *out, uint32_t min_count,
                               uint32_t max_count);

/* Adds layout `pieces` as brushes under batch item `group`, or under
   `brush->parent` when it is negative; each brush sits at its piece's
   center. Floors take `floor_material`, the other pieces `material`, an
   empty name meaning the dev grid. */
static bool8_t ops_pieces_add(OpsContext *ctx, OpsBatch *batch,
                              const OpsBrushArgs *brush, int32_t group,
                              const VkrBlockoutPiece *pieces, uint32_t count,
                              const char *material,
                              const char *floor_material) {
  static const char *const names[] = {"Step",  "Landing", "Pole",
                                      "Floor", "Wall",    "Ceiling"};
  uint32_t numbers[ArrayCount(names)] = {0};
  for (uint32_t i = 0; i < count; ++i) {
    const VkrBlockoutPiece *piece = &pieces[i];
    Vec3 center = vec3_zero();
    for (uint32_t p = 0; p < piece->point_count; ++p) {
      center = vec3_add(center, piece->points[p]);
    }
    center = vec3_scale(center, 1.0f / (float32_t)piece->point_count);
    Vec3 local[VKR_BLOCKOUT_PIECE_POINT_MAX];
    for (uint32_t p = 0; p < piece->point_count; ++p) {
      local[p] = vec3_sub(piece->points[p], center);
    }
    VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
    const uint32_t plane_count =
        vkr_brush_hull(local, piece->point_count, planes, VKR_BRUSH_FACE_MAX);
    if (!plane_count) {
      return ops_fail(ctx, OPS_INVALID, "A %s of the shape spans no volume",
                      names[piece->kind]);
    }
    const char *face_material =
        piece->kind == VKR_BLOCKOUT_PIECE_FLOOR ? floor_material : material;
    const char *materials[VKR_BRUSH_FACE_MAX];
    for (uint32_t f = 0; f < plane_count; ++f) {
      materials[f] = face_material;
    }
    char name[64];
    snprintf(name, sizeof(name), "%s %u", names[piece->kind],
             ++numbers[piece->kind]);
    uint32_t item = 0u;
    if (!ops_brush_add(ctx, batch, brush, group, name, center,
                       vkr_quat_identity(), planes, plane_count, materials,
                       &item)) {
      return false_v;
    }
  }
  return true_v;
}

/* The pieces of `shape`, or a failure naming why it cannot be built. */
static uint32_t ops_blockout_pieces(OpsContext *ctx, const SceneBlockout *shape,
                                    VkrBlockoutPiece **out) {
  const uint32_t capacity = vkr_blockout_piece_capacity(shape);
  *out = arena_alloc(ops_arena(ctx), capacity * sizeof(**out),
                     ARENA_MEMORY_TAG_STRUCT);
  if (!*out) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return 0u;
  }
  char error[160] = {0};
  const uint32_t count =
      vkr_blockout_layout(shape, *out, capacity, error, sizeof(error));
  if (!count) {
    ops_fail(ctx, OPS_INVALID, "%s", error);
  }
  return count;
}

/* Creates an editable blockout shape at `position`, turned `yaw` about +Y,
   which the scene builds from its settings; returns its item. */
static bool8_t ops_blockout_add(OpsContext *ctx, OpsBatch *batch,
                                OpsBrushArgs *brush, const SceneBlockout *shape,
                                Vec3 position, VkrQuat yaw,
                                uint32_t *out_item) {
  VkrBlockoutPiece *pieces = NULL;
  if (!ops_blockout_pieces(ctx, shape, &pieces)) {
    return false_v;
  }
  uint32_t group = 0u;
  if (!ops_group_add(ctx, batch, brush, brush->name, position, yaw, &group)) {
    return false_v;
  }
  VkrSceneEditValues *values = &ctx->ops->items[group].request.values;
  values->fields |= VKR_SCENE_EDIT_COMPONENT;
  values->component_type = &vkr_scene_blockout_type;
  MemCopy(values->component, shape, sizeof(*shape));
  *out_item = group;
  return true_v;
}

/* Default settings of `kind` of shape: stairs 3 m high, or a corridor. */
static SceneBlockout ops_blockout_defaults(SceneBlockoutShape kind) {
  SceneBlockout shape;
  vkr_type_defaults(&vkr_scene_blockout_type, &shape);
  shape.shape = kind;
  if (kind == SCENE_BLOCKOUT_CORRIDOR) {
    shape.width = 2.0f;
    shape.height = 3.0f;
    shape.thickness = 0.25f;
    shape.radius = 0.0f;
    snprintf(shape.floor_material, sizeof(shape.floor_material), "%s",
             OPS_DEV_FLOOR);
    snprintf(shape.material, sizeof(shape.material), "%s", OPS_DEV_WALL);
  }
  return shape;
}

/* `shape`'s settings with `values` (a blockout component object) over
   them; the shape itself stays. */
static bool8_t ops_blockout_values(OpsContext *ctx, const VkrBakeryJson *values,
                                   SceneBlockout *shape) {
  if (!values) {
    return true_v;
  }
  const SceneBlockoutShape kind = shape->shape;
  if (!ops_component_read(ctx, &vkr_scene_blockout_type, values, shape)) {
    return false_v;
  }
  shape->shape = kind;
  return true_v;
}

/* blockout.create: an editable stairs or corridor group (ADR-084). */
static bool8_t ops_build_blockout_create(OpsContext *ctx,
                                         const VkrBakeryJson *args,
                                         OpsBatch *batch) {
  String8 kind = {0};
  if (!vkr_bakery_json_get_string(args, "shape", &kind) ||
      (!ops_equals(kind, "stairs") && !ops_equals(kind, "corridor"))) {
    return ops_fail(ctx, OPS_INVALID, "'shape' is stairs or corridor");
  }
  const bool8_t corridor = ops_equals(kind, "corridor");
  OpsBrushArgs brush;
  if (!ops_brush_args(ctx, args, batch, corridor ? "Corridor" : "Stairs",
                      &brush)) {
    return false_v;
  }
  SceneBlockout shape = ops_blockout_defaults(corridor ? SCENE_BLOCKOUT_CORRIDOR
                                                       : SCENE_BLOCKOUT_STAIRS);
  if (brush.material[0]) {
    snprintf(shape.material, sizeof(shape.material), "%s", brush.material);
  }
  Vec3 position = vec3_zero();
  bool8_t has_position = false_v;
  float64_t yaw = 0.0;
  (void)ops_arg_number(args, "yaw", &yaw);
  if (!ops_arg_vec3(ctx, args, "position", &position, &has_position) ||
      !ops_blockout_values(ctx, vkr_bakery_json_get(args, "values"), &shape)) {
    return false_v;
  }
  /* A corridor may name its floor points in the container's space; the
     group sits at the first. */
  if (vkr_bakery_json_get(args, "points")) {
    Vec3 points[SCENE_BLOCKOUT_POINT_MAX];
    const uint32_t count = ops_arg_points(ctx, args, "points", points, 2u,
                                          SCENE_BLOCKOUT_POINT_MAX);
    if (!count) {
      return false_v;
    }
    position = points[0];
    has_position = true_v;
    shape.point_count = count;
    for (uint32_t i = 0; i < count; ++i) {
      shape.points[i] = vec3_sub(points[i], position);
    }
  }
  if (!has_position) {
    return ops_fail(ctx, OPS_INVALID, "blockout.create needs a 'position'");
  }
  uint32_t group = 0u;
  if (!ops_blockout_add(
          ctx, batch, &brush, &shape, position,
          vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f),
                                   (float32_t)yaw * 0.0174532925f),
          &group)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = group;
  return true_v;
}

/* The blockout shape `ref` names, or a failure. */
static const SceneBlockout *ops_blockout_ref(OpsContext *ctx,
                                             const OpsRef *ref) {
  const VkrScene *scene = ops_scene(ctx->frame, ref->container);
  const SceneBlockout *shape =
      ref->item < 0 && scene
          ? vkr_scene_get_typed(scene, ref->entity, &vkr_scene_blockout_type)
          : NULL;
  if (!shape) {
    ops_fail(ctx, OPS_INVALID, "'entity' must name an existing blockout shape");
  }
  return shape;
}

/* blockout.build: sets a shape's settings to `values` over its current
   ones, refused when it would not build, as one undo step. */
static bool8_t ops_build_blockout_build(OpsContext *ctx,
                                        const VkrBakeryJson *args,
                                        OpsBatch *batch) {
  OpsRef ref;
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "entity"), "entity",
               &ref)) {
    return false_v;
  }
  const SceneBlockout *current = ops_blockout_ref(ctx, &ref);
  if (!current) {
    return false_v;
  }
  SceneBlockout shape = *current;
  VkrBlockoutPiece *pieces = NULL;
  if (!ops_blockout_values(ctx, vkr_bakery_json_get(args, "values"), &shape) ||
      !ops_blockout_pieces(ctx, &shape, &pieces)) {
    return false_v;
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, ref.container, VKR_SCENE_EDIT_APPLY);
  if (!item) {
    return false_v;
  }
  ops_item_target(item, &ref);
  item->request.values.fields = VKR_SCENE_EDIT_COMPONENT;
  item->request.values.component_type = &vkr_scene_blockout_type;
  MemCopy(item->request.values.component, &shape, sizeof(shape));
  batch->op_target[batch->op_count] = ref.entity;
  return true_v;
}

/* blockout.bake: turns a shape into plain brushes under its entity, which
   then edit one by one, as one undo step. A shape of more pieces than one
   batch holds is refused. */
static bool8_t ops_build_blockout_bake(OpsContext *ctx,
                                       const VkrBakeryJson *args,
                                       OpsBatch *batch) {
  OpsRef ref;
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "entity"), "entity",
               &ref)) {
    return false_v;
  }
  const SceneBlockout *shape = ops_blockout_ref(ctx, &ref);
  VkrBlockoutPiece *pieces = NULL;
  const uint32_t count = shape ? ops_blockout_pieces(ctx, shape, &pieces) : 0u;
  if (!count) {
    return false_v;
  }
  /* Each brush takes an item and one per face. */
  uint32_t items = batch->count + 1u;
  for (uint32_t i = 0; i < count; ++i) {
    VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
    items += 1u + vkr_brush_hull(pieces[i].points, pieces[i].point_count,
                                 planes, VKR_BRUSH_FACE_MAX);
  }
  if (items > VKR_SAMPLE_EDIT_BATCH_MAX) {
    return ops_fail(ctx, OPS_LIMIT,
                    "The shape has %u pieces, more brushes than one bake "
                    "holds; make it smaller to bake it",
                    count);
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, ref.container, VKR_SCENE_EDIT_REMOVE_COMPONENT);
  if (!item) {
    return false_v;
  }
  ops_item_target(item, &ref);
  item->request.values.component_type = &vkr_scene_blockout_type;
  OpsBrushArgs brush = {.parent = {.entity = ref.entity, .item = -1},
                        .container = ref.container,
                        .role = SCENE_BRUSH_ROLE_SOLID};
  brush.parent.container = ref.container;
  if (!ops_pieces_add(ctx, batch, &brush, -1, pieces, count, shape->material,
                      shape->floor_material)) {
    return false_v;
  }
  batch->op_target[batch->op_count] = ref.entity;
  return true_v;
}

/* brush.stairs: editable stairs from `from`, the bottom front center (a
   spiral's pole), toward `to`, whose height above `from` is the stairs'
   height unless `height` says otherwise and whose horizontal distance is
   the length (a spiral's outer radius). */
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
  if (!has_from || !has_to) {
    return ops_fail(ctx, OPS_INVALID,
                    "brush.stairs needs 'from' (bottom front center) and 'to' "
                    "(top back center, or a spiral's rim)");
  }
  SceneBlockout shape = ops_blockout_defaults(SCENE_BLOCKOUT_STAIRS);
  if (brush.material[0]) {
    snprintf(shape.material, sizeof(shape.material), "%s", brush.material);
  }
  String8 kind = {0};
  if (vkr_bakery_json_get_string(args, "kind", &kind)) {
    shape.stairs = SCENE_STAIRS_KIND_COUNT;
    for (uint32_t i = 0; vkr_blockout_stairs_kinds[i]; ++i) {
      if (ops_equals(kind, vkr_blockout_stairs_kinds[i])) {
        shape.stairs = (SceneStairsKind)i;
      }
    }
    if (shape.stairs == SCENE_STAIRS_KIND_COUNT) {
      return ops_fail(ctx, OPS_INVALID,
                      "'kind' is straight, l, u, curved or spiral");
    }
  }
  String8 turn = {0};
  if (vkr_bakery_json_get_string(args, "turn", &turn)) {
    if (!ops_equals(turn, "left") && !ops_equals(turn, "right")) {
      return ops_fail(ctx, OPS_INVALID, "'turn' is left or right");
    }
    shape.left = ops_equals(turn, "left");
  }
  from = ops_snap3(from, brush.grid);
  to = ops_snap3(to, brush.grid);
  const Vec3 delta = vec3_sub(to, from);
  const float32_t run = sqrtf(delta.x * delta.x + delta.z * delta.z);
  float64_t width = shape.width;
  float64_t step = shape.step_height;
  float64_t sweep = shape.turn;
  float64_t height = delta.y;
  float64_t thickness = shape.thickness;
  (void)ops_arg_number(args, "width", &width);
  (void)ops_arg_number(args, "step_height", &step);
  (void)ops_arg_number(args, "height", &height);
  /* A spiral turns at least once, 22.5 degrees a step, so a tall one keeps
     sixteen steps a turn. */
  if (shape.stairs == SCENE_STAIRS_SPIRAL && step > 0.0) {
    sweep = Max(360.0, ceil(height / step - 1.0e-4) * 22.5);
    sweep = Min(sweep, (float64_t)VKR_BLOCKOUT_TURN_MAX);
  }
  (void)ops_arg_number(args, "sweep", &sweep);
  (void)ops_arg_number(args, "thickness", &thickness);
  shape.width = (float32_t)width;
  shape.step_height = (float32_t)step;
  shape.turn = (float32_t)sweep;
  shape.height = (float32_t)height;
  shape.thickness = (float32_t)thickness;
  shape.length = run;
  if (shape.stairs == SCENE_STAIRS_SPIRAL) {
    shape.radius = vkr_clamp_f32(run * 0.15f, 0.1f, 0.5f);
    shape.width = Max(run - shape.radius, 0.25f);
  }
  uint32_t group = 0u;
  if (!ops_blockout_add(ctx, batch, &brush, &shape, from,
                        ops_yaw_toward(run > 0.0f ? delta : vec3_new(0, 0, 1)),
                        &group)) {
    return false_v;
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

/* blockout.corridor: an editable corridor along `points`, or from `from`
   to `to`; `curved` rounds its corners with 2 m arcs unless `radius` says
   otherwise. */
static bool8_t ops_build_corridor(OpsContext *ctx, const VkrBakeryJson *args,
                                  OpsBatch *batch) {
  OpsBrushArgs brush;
  SceneBlockout shape = ops_blockout_defaults(SCENE_BLOCKOUT_CORRIDOR);
  if (!ops_brush_args(ctx, args, batch, "Corridor", &brush) ||
      !ops_arg_string(ctx, args, "floor_material", shape.floor_material,
                      sizeof(shape.floor_material))) {
    return false_v;
  }
  if (brush.material[0]) {
    snprintf(shape.material, sizeof(shape.material), "%s", brush.material);
  }
  Vec3 points[SCENE_BLOCKOUT_POINT_MAX];
  uint32_t point_count = 0u;
  if (vkr_bakery_json_get(args, "points")) {
    point_count = ops_arg_points(ctx, args, "points", points, 2u,
                                 SCENE_BLOCKOUT_POINT_MAX);
    if (!point_count) {
      return false_v;
    }
  } else {
    bool8_t has_from = false_v;
    bool8_t has_to = false_v;
    if (!ops_arg_vec3(ctx, args, "from", &points[0], &has_from) ||
        !ops_arg_vec3(ctx, args, "to", &points[1], &has_to)) {
      return false_v;
    }
    if (!has_from || !has_to) {
      return ops_fail(ctx, OPS_INVALID,
                      "blockout.corridor needs 'points', or 'from' and 'to'");
    }
    point_count = 2u;
  }
  float64_t width = shape.width;
  float64_t height = shape.height;
  float64_t wall = shape.thickness;
  float64_t radius = ops_arg_bool(args, "curved", false_v) ? 2.0 : 0.0;
  (void)ops_arg_number(args, "width", &width);
  (void)ops_arg_number(args, "height", &height);
  (void)ops_arg_number(args, "wall", &wall);
  (void)ops_arg_number(args, "radius", &radius);
  shape.width = (float32_t)width;
  shape.height = (float32_t)height;
  shape.thickness = (float32_t)wall;
  shape.radius = (float32_t)Max(radius, 0.0);
  shape.ceiling = ops_arg_bool(args, "ceiling", true_v);
  const Vec3 origin = ops_snap3(points[0], brush.grid);
  shape.point_count = point_count;
  for (uint32_t i = 0; i < point_count; ++i) {
    shape.points[i] = vec3_sub(ops_snap3(points[i], brush.grid), origin);
  }
  uint32_t group = 0u;
  if (!ops_blockout_add(ctx, batch, &brush, &shape, origin, vkr_quat_identity(),
                        &group)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = group;
  return true_v;
}

/* Objects one mover.create gathers. */
#define OPS_MOVER_OBJECTS_MAX 64u

/* mover.create: a group carrying a mover (ADR-084) with `values` over its
   defaults, holding `objects`, as one batch. The group sits at the center
   of their world box under the parent they share, and they keep their world
   poses. */
static bool8_t ops_build_mover_create(OpsContext *ctx,
                                      const VkrBakeryJson *args,
                                      OpsBatch *batch) {
  const VkrBakeryJson *objects = vkr_bakery_json_get(args, "objects");
  if (!objects || objects->type != VKR_BAKERY_JSON_ARRAY || !objects->count ||
      objects->count > OPS_MOVER_OBJECTS_MAX) {
    return ops_fail(ctx, OPS_INVALID,
                    "mover.create needs 'objects', 1 to %u entities",
                    OPS_MOVER_OBJECTS_MAX);
  }
  OpsRef refs[OPS_MOVER_OBJECTS_MAX];
  const uint32_t count = (uint32_t)objects->count;
  const VkrScene *scene = NULL;
  VkrEntityId parent = VKR_ENTITY_ID_INVALID;
  Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  if (!geometry) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!ops_ref(ctx, batch, vkr_bakery_json_at(objects, i), "objects",
                 &refs[i])) {
      return false_v;
    }
    if (refs[i].item >= 0) {
      return ops_fail(ctx, OPS_INVALID,
                      "mover.create moves existing entities; create them in "
                      "an earlier batch");
    }
    if (refs[i].container != refs[0].container) {
      return ops_fail(ctx, OPS_INVALID, "A mover's objects share one scene");
    }
    scene = ops_scene(ctx->frame, refs[i].container);
    const VkrEntityId above = ops_parent(scene, refs[i].entity);
    if (i == 0u) {
      parent = above;
    } else if (above.u64 != parent.u64) {
      return ops_fail(ctx, OPS_INVALID,
                      "A mover's objects share one parent; move them under "
                      "one first");
    }
    /* Its world box, or its origin when it has no geometry. */
    Vec3 a = vec3_zero();
    Vec3 b = vec3_zero();
    if (!vkr_editor_entity_world_box(scene, refs[i].entity, geometry, &a, &b)) {
      const SceneTransform *transform = ops_transform(scene, refs[i].entity);
      a = transform ? mat4_position(transform->world) : vec3_zero();
      b = a;
    }
    lo = vec3_new(Min(lo.x, a.x), Min(lo.y, a.y), Min(lo.z, a.z));
    hi = vec3_new(Max(hi.x, b.x), Max(hi.y, b.y), Max(hi.z, b.z));
  }
  char name[128] = "Mover";
  if (!ops_arg_string(ctx, args, "name", name, sizeof(name))) {
    return false_v;
  }
  _Alignas(16) uint8_t mover[VKR_TYPE_VALUE_MAX];
  vkr_type_defaults(&vkr_scene_mover_type, mover);
  if (!ops_component_read(ctx, &vkr_scene_mover_type,
                          vkr_bakery_json_get(args, "values"), mover)) {
    return false_v;
  }
  /* The group's position in its parent's space. */
  Vec3 center = vec3_scale(vec3_add(lo, hi), 0.5f);
  const SceneTransform *parent_transform =
      parent.u64 ? ops_transform(scene, parent) : NULL;
  if (parent_transform) {
    center =
        mat4_mul_vec3(mat4_inverse_affine(parent_transform->world), center);
  }

  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, refs[0].container, VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  item->request.parent = parent;
  VkrSceneEditValues *values = &item->request.values;
  values->fields =
      VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM | VKR_SCENE_EDIT_COMPONENT;
  snprintf(values->name, sizeof(values->name), "%s", name);
  values->position = center;
  values->rotation = vkr_quat_identity();
  values->scale = vec3_one();
  values->component_type = &vkr_scene_mover_type;
  MemCopy(values->component, mover, vkr_scene_mover_type.size);
  /* Validated before more items grow the batch and move this one. */
  if (!ops_validate_values(ctx, values)) {
    return false_v;
  }
  const uint32_t group = batch->count - 1u;
  for (uint32_t i = 0; i < count; ++i) {
    VkrSceneEditValues pose;
    if (!ops_current_values(ctx, &refs[i], &pose)) {
      return false_v;
    }
    VkrSampleEditBatchItem *move =
        ops_batch_add(ctx, batch, refs[i].container, VKR_SCENE_EDIT_REPARENT);
    if (!move) {
      return false_v;
    }
    ops_item_target(move, &refs[i]);
    move->parent_ref = (int32_t)group;
    /* The reparent reads the new group's world matrix before the scene
       computes it, so its local pose is set here: the group adds only
       `center` to the parent's space. */
    VkrSampleEditBatchItem *place =
        ops_batch_add(ctx, batch, refs[i].container, VKR_SCENE_EDIT_APPLY);
    if (!place) {
      return false_v;
    }
    ops_item_target(place, &refs[i]);
    pose.fields = VKR_SCENE_EDIT_TRANSFORM;
    pose.position = vec3_sub(pose.position, center);
    place->request.values = pose;
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
// Brush editing (ADR-084)
// -----------------------------------------------------------------------------

static uint32_t ops_arg_points(OpsContext *ctx, const VkrBakeryJson *args,
                               const char *key, Vec3 *out, uint32_t min_count,
                               uint32_t max_count);

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

/* The settings face `f` of `piece` copies: the face its source names
   (`brush` faces first, then `extra` faces), or `fallback` for a new
   plane. */
static const SceneBrushFace *
ops_piece_source(const OpsBrushRead *brush, const VkrBrushPiece *piece,
                 uint32_t f, const SceneBrushFace *extra, uint32_t extra_count,
                 const SceneBrushFace *fallback) {
  const uint32_t source = piece->source[f];
  return source < brush->count ? &brush->values[source]
         : source != VKR_BRUSH_SOURCE_NEW && source - brush->count < extra_count
             ? &extra[source - brush->count]
             : fallback;
}

/* Makes `brush` the solid of `piece` in place, so it keeps its id, its
   components and the references to it: each face the piece keeps takes its
   new plane, a plane of its own becomes a new face, and the faces it drops
   go, in the batch's one journal group. */
static bool8_t ops_brush_edit(OpsContext *ctx, OpsBatch *batch,
                              const OpsBrushRead *brush,
                              const VkrBrushPiece *piece,
                              const SceneBrushFace *extra, uint32_t extra_count,
                              const SceneBrushFace *fallback) {
  const uint16_t container = brush->entity.parts.world;
  bool8_t kept[VKR_BRUSH_FACE_MAX] = {0};
  for (uint32_t f = 0; f < piece->count; ++f) {
    const uint32_t source = piece->source[f];
    SceneBrushFace value =
        *ops_piece_source(brush, piece, f, extra, extra_count, fallback);
    value.normal = piece->planes[f].normal;
    value.distance = piece->planes[f].distance;
    const bool8_t reuse = source < brush->count && !kept[source];
    VkrSampleEditBatchItem *item =
        ops_batch_add(ctx, batch, container,
                      reuse ? VKR_SCENE_EDIT_APPLY : VKR_SCENE_EDIT_CREATE);
    if (!item) {
      return false_v;
    }
    VkrSceneEditValues *values = &item->request.values;
    if (reuse) {
      kept[source] = true_v;
      item->request.entity = brush->faces[source];
      values->fields = VKR_SCENE_EDIT_COMPONENT;
    } else {
      item->request.parent = brush->entity;
      values->fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM |
                       VKR_SCENE_EDIT_COMPONENT;
      ops_face_name(value.normal, f, values->name, sizeof(values->name));
      values->rotation = vkr_quat_identity();
      values->scale = vec3_one();
    }
    values->component_type = &vkr_scene_brush_face_type;
    MemCopy(values->component, &value, sizeof(value));
  }
  for (uint32_t i = 0; i < brush->count; ++i) {
    if (kept[i]) {
      continue;
    }
    VkrSampleEditBatchItem *item =
        ops_batch_add(ctx, batch, container, VKR_SCENE_EDIT_DELETE);
    if (!item) {
      return false_v;
    }
    item->request.entity = brush->faces[i];
  }
  return true_v;
}

/* Replaces `brush` with `pieces`, which lie in its space; each piece face
   copies the material and projection of the face its source names (target
   faces first, then `extra` faces), or `fallback` for a new plane. With
   `delete_original` the brush itself becomes the first piece and keeps its
   id; else every piece is a new brush. Returns the first new brush's item,
   or UINT32_MAX when there is none; a kept brush is the operation's
   target. */
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
  const bool8_t keep = delete_original && piece_count > 0u;
  if (keep) {
    if (!ops_brush_edit(ctx, batch, brush, &pieces[0], extra, extra_count,
                        fallback)) {
      return false_v;
    }
    if (!batch->op_target[batch->op_count].u64) {
      batch->op_target[batch->op_count] = brush->entity;
    }
  }
  const String8 base = vkr_scene_get_name(scene, brush->entity);
  for (uint32_t p = keep ? 1u : 0u; p < piece_count; ++p) {
    const char *materials[VKR_BRUSH_FACE_MAX];
    const SceneBrushFace *sources[VKR_BRUSH_FACE_MAX];
    for (uint32_t f = 0; f < pieces[p].count; ++f) {
      sources[f] =
          ops_piece_source(brush, &pieces[p], f, extra, extra_count, fallback);
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
  if (!delete_original || keep) {
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
  if (count == VKR_BRUSH_CARVE_FAILED) {
    return ops_fail(ctx, OPS_LIMIT,
                    "Carving brush %u needs a piece of more than %u faces",
                    target->entity.parts.index, VKR_BRUSH_FACE_MAX);
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
  if (count == VKR_BRUSH_CARVE_FAILED) {
    return ops_fail(ctx, OPS_LIMIT,
                    "Hollowing needs a wall piece of more than %u faces",
                    VKR_BRUSH_FACE_MAX);
  }
  if (count == 0u || count == UINT32_MAX) {
    return ops_fail(ctx, OPS_INVALID,
                    "'thickness' leaves no wall around the inside");
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

/* The shift on `axis` that lines `lo`-`hi` up with `plo`-`phi`'s nearest
   side or center, when that is a nudge of at most half the box's size there,
   else zero. */
static float32_t ops_snap_align(Vec3 lo, Vec3 hi, Vec3 plo, Vec3 phi,
                                uint32_t axis) {
  const float32_t candidates[3] = {
      plo.elements[axis] - lo.elements[axis],
      phi.elements[axis] - hi.elements[axis],
      (plo.elements[axis] + phi.elements[axis] - lo.elements[axis] -
       hi.elements[axis]) *
          0.5f,
  };
  float32_t best = candidates[0];
  for (uint32_t c = 1u; c < 3u; ++c) {
    if (fabsf(candidates[c]) < fabsf(best)) {
      best = candidates[c];
    }
  }
  return fabsf(best) <= (hi.elements[axis] - lo.elements[axis]) * 0.5f ? best
                                                                       : 0.0f;
}

/* The shift that places box `lo`-`hi`, its floor at `floor`, against box
   `plo`-`phi` with floor `pfloor`, `gap` apart. Over the other box's
   footprint it rests on top (or hangs below, when it lies wholly below the
   other's middle), lined up by small nudges; beside it, it meets the nearest
   side and levels its floor with the other's. */
static Vec3 ops_snap_place(Vec3 lo, Vec3 hi, float32_t floor, Vec3 plo,
                           Vec3 phi, float32_t pfloor, float32_t gap) {
  const Vec3 center = vec3_scale(vec3_add(lo, hi), 0.5f);
  Vec3 shift = vec3_zero();
  if (center.x > plo.x && center.x < phi.x && center.z > plo.z &&
      center.z < phi.z) {
    /* It rests on top unless it lies wholly below the other's middle. */
    shift.y =
        hi.y > (plo.y + phi.y) * 0.5f ? phi.y + gap - lo.y : plo.y - gap - hi.y;
    shift.x = ops_snap_align(lo, hi, plo, phi, 0u);
    shift.z = ops_snap_align(lo, hi, plo, phi, 2u);
    return shift;
  }
  /* The side whose contact moves the box least. */
  float32_t best = INFINITY;
  for (uint32_t axis = 0; axis < 3u; axis += 2u) {
    for (uint32_t side = 0; side < 2u; ++side) {
      const float32_t move = side
                                 ? phi.elements[axis] + gap - lo.elements[axis]
                                 : plo.elements[axis] - gap - hi.elements[axis];
      if (fabsf(move) < best) {
        best = fabsf(move);
        shift = vec3_zero();
        shift.elements[axis] = move;
        shift.elements[2u - axis] = ops_snap_align(lo, hi, plo, phi, 2u - axis);
      }
    }
  }
  shift.y = pfloor - floor;
  return shift;
}

/* The least shift that places box `lo`-`hi` (floor `floor`) against one of
   `count` boxes (ops_snap_place). */
static Vec3 ops_snap_best(Vec3 lo, Vec3 hi, float32_t floor, const Vec3 *lows,
                          const Vec3 *highs, const float32_t *floors,
                          uint32_t count, float32_t gap) {
  Vec3 best = vec3_zero();
  float32_t best_length = INFINITY;
  for (uint32_t p = 0; p < count; ++p) {
    const Vec3 shift =
        ops_snap_place(lo, hi, floor, lows[p], highs[p], floors[p], gap);
    if (vec3_length(shift) < best_length) {
      best_length = vec3_length(shift);
      best = shift;
    }
  }
  return best;
}

/* Appends an edit that moves `ref` by the world `shift`, in its parent's
   space. */
static bool8_t ops_move_world(OpsContext *ctx, OpsBatch *batch,
                              const OpsRef *ref, const VkrScene *scene,
                              Vec3 shift) {
  VkrSceneEditValues values;
  if (!ops_current_values(ctx, ref, &values)) {
    return false_v;
  }
  const SceneTransform *transform = ops_transform(scene, ref->entity);
  const SceneTransform *parent =
      transform ? ops_transform(scene, transform->parent) : NULL;
  Vec3 local = shift;
  if (parent) {
    const Vec4 moved = mat4_mul_vec4(mat4_inverse_affine(parent->world),
                                     vec3_to_vec4(shift, 0.0f));
    local = vec3_new(moved.x, moved.y, moved.z);
  }
  values.position = vec3_add(values.position, local);
  values.fields = VKR_SCENE_EDIT_TRANSFORM;
  if (!ops_validate_values(ctx, &values)) {
    return false_v;
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, ref->container, VKR_SCENE_EDIT_APPLY);
  if (!item) {
    return false_v;
  }
  ops_item_target(item, ref);
  item->request.values = values;
  return true_v;
}

/* brush.snap: the first entity stays; each other one in turn, unless free,
   moves the least distance that sets it flush against a side of one placed
   before it, `gap` apart, lined up with that one's sides or center on the
   other two axes. Boxes are world bounds: of an entity's brushes, else of
   its meshes and shapes. */
static bool8_t ops_build_snap(OpsContext *ctx, const VkrBakeryJson *args,
                              OpsBatch *batch) {
  const VkrBakeryJson *list = vkr_bakery_json_get(args, "brushes");
  if (!list || list->type != VKR_BAKERY_JSON_ARRAY || list->count < 2u ||
      list->count > VKR_EDITOR_SELECTION_MAX) {
    return ops_fail(ctx, OPS_INVALID, "'brushes' lists 2 to %u objects",
                    VKR_EDITOR_SELECTION_MAX);
  }
  float64_t gap = 0.0;
  if (ops_arg_number(args, "gap", &gap) && !(gap >= 0.0 && gap <= 64.0)) {
    return ops_fail(ctx, OPS_INVALID, "'gap' is 0 to 64 meters");
  }
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  if (!geometry) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  Vec3 lows[VKR_EDITOR_SELECTION_MAX];
  Vec3 highs[VKR_EDITOR_SELECTION_MAX];
  float32_t floors[VKR_EDITOR_SELECTION_MAX];
  uint32_t placed = 0u;
  uint32_t moved = 0u;
  for (const VkrBakeryJson *entry = list->first; entry; entry = entry->next) {
    OpsRef ref;
    if (!ops_ref(ctx, batch, entry, "brushes", &ref)) {
      return false_v;
    }
    const VkrScene *scene = ops_scene(ctx->frame, ref.container);
    Vec3 lo = vec3_zero();
    Vec3 hi = vec3_zero();
    if (ref.item >= 0 || !scene ||
        !vkr_editor_entity_world_box(scene, ref.entity, geometry, &lo, &hi)) {
      return ops_fail(ctx, OPS_INVALID,
                      "'brushes' must name existing objects with brushes, "
                      "meshes or shapes");
    }
    float32_t floor =
        vkr_editor_entity_floor(scene, ref.entity, geometry, lo, hi);
    if (placed > 0u && !vkr_editor_entity_free(scene, ref.entity)) {
      const Vec3 best = ops_snap_best(lo, hi, floor, lows, highs, floors,
                                      placed, (float32_t)gap);
      lo = vec3_add(lo, best);
      hi = vec3_add(hi, best);
      floor += best.y;
      if (vec3_length(best) > 1.0e-6f) {
        if (!ops_move_world(ctx, batch, &ref, scene, best)) {
          return false_v;
        }
        if (!batch->op_target[batch->op_count].u64) {
          batch->op_target[batch->op_count] = ref.entity;
        }
        moved++;
      }
    }
    lows[placed] = lo;
    highs[placed] = hi;
    floors[placed] = floor;
    placed++;
  }
  if (!moved) {
    return ops_fail(ctx, OPS_INVALID, "The objects are already snapped");
  }
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

/* A face's world-space unit normal. */
static Vec3 ops_face_world_normal(const OpsBrushRead *brush, uint32_t face) {
  const Vec3 n = vec3_normalize(brush->planes[face].normal);
  const Vec4 world = mat4_mul_vec4(
      mat4_transpose(mat4_inverse_affine(brush->transform->world)),
      vec3_to_vec4(n, 0.0f));
  return vec3_normalize(vec3_new(world.x, world.y, world.z));
}

/* Pulls a grid rectangle of a face out, or pushes it in (ADR-084): pulled,
   it joins the brush when the union stays convex and is a new brush
   otherwise; pushed, it carves a recess, or a hole when it goes through. */
static bool8_t ops_build_patch(OpsContext *ctx, const VkrBakeryJson *args,
                               OpsBatch *batch) {
  OpsBrushRead *brush =
      arena_alloc(ops_arena(ctx), sizeof(*brush), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushPiece *pieces =
      arena_alloc(ops_arena(ctx), VKR_BRUSH_FACE_MAX * sizeof(*pieces),
                  ARENA_MEMORY_TAG_STRUCT);
  SceneBrushFace *extra =
      arena_alloc(ops_arena(ctx), VKR_BRUSH_FACE_MAX * sizeof(*extra),
                  ARENA_MEMORY_TAG_STRUCT);
  if (!brush || !geometry || !pieces || !extra) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  uint32_t face = 0u;
  float32_t min[2] = {0};
  float32_t max[2] = {0};
  bool8_t has_min = false_v;
  bool8_t has_max = false_v;
  float64_t distance = 0.0;
  if (!ops_face_arg(ctx, batch, args, brush, &face) ||
      !ops_brush_unscaled(ctx, brush) ||
      !ops_arg_floats(ctx, args, "min", min, 2u, &has_min) ||
      !ops_arg_floats(ctx, args, "max", max, 2u, &has_max)) {
    return false_v;
  }
  if (!has_min || !has_max || !(max[0] > min[0]) || !(max[1] > min[1])) {
    return ops_fail(ctx, OPS_INVALID,
                    "'min' and 'max' are the rectangle's [u, v] corners on "
                    "the face's grid axes, max above min");
  }
  if (!ops_arg_number(args, "distance", &distance) || !isfinite(distance) ||
      fabs(distance) < 1.0e-4) {
    return ops_fail(ctx, OPS_INVALID,
                    "'distance' in meters pulls the patch out, or pushes it "
                    "in when negative");
  }

  /* The rectangle's sides, from the face's world grid axes. */
  const Vec3 normal = ops_face_world_normal(brush, face);
  Vec3 u = {0};
  Vec3 v = {0};
  vkr_brush_grid_axes(normal, &u, &v);
  const VkrBrushPlane rect[4] = {
      ops_plane_to_brush(brush, u, vec3_scale(u, max[0])),
      ops_plane_to_brush(brush, vec3_scale(u, -1.0f), vec3_scale(u, min[0])),
      ops_plane_to_brush(brush, v, vec3_scale(v, max[1])),
      ops_plane_to_brush(brush, vec3_scale(v, -1.0f), vec3_scale(v, min[1])),
  };
  for (uint32_t i = 0; i < VKR_BRUSH_FACE_MAX; ++i) {
    extra[i] = brush->values[face];
  }
  VkrBrushPiece prism = {0};
  const bool8_t pull = distance > 0.0;
  /* A pushed patch's cutter starts just outside the face. */
  if (!vkr_brush_patch_prism(brush->planes, brush->count, face, rect,
                             pull ? 0.0f : (float32_t)distance,
                             pull ? (float32_t)distance : 0.01f, &prism,
                             geometry)) {
    return ops_fail(ctx, OPS_INVALID, "The rectangle does not lie on the face");
  }

  uint32_t item = UINT32_MAX;
  if (pull) {
    const VkrBrushPlane *lists[2] = {brush->planes, prism.planes};
    const uint32_t counts[2] = {brush->count, prism.count};
    if (vkr_brush_merge(lists, counts, 2u, &pieces[0], geometry)) {
      if (!ops_brush_replace(ctx, batch, brush, pieces, 1u, extra, prism.count,
                             &brush->values[face], true_v, "", &item)) {
        return false_v;
      }
    } else if (!ops_brush_replace(ctx, batch, brush, &prism, 1u, NULL, 0u,
                                  &brush->values[face], false_v, " patch",
                                  &item)) {
      return false_v;
    }
  } else {
    const uint32_t count =
        vkr_brush_carve(brush->planes, brush->count, prism.planes, prism.count,
                        pieces, VKR_BRUSH_FACE_MAX, geometry);
    if (count == UINT32_MAX) {
      return ops_fail(ctx, OPS_INVALID, "The patch does not reach the brush");
    }
    if (count == VKR_BRUSH_CARVE_FAILED) {
      return ops_fail(ctx, OPS_LIMIT,
                      "The patch needs a piece of more than %u faces",
                      VKR_BRUSH_FACE_MAX);
    }
    if (!ops_brush_replace(ctx, batch, brush, pieces, count, extra, prism.count,
                           &brush->values[face], true_v, NULL, &item)) {
      return false_v;
    }
  }
  if (item != UINT32_MAX) {
    batch->op_item[batch->op_count] = item;
  }
  return true_v;
}

/* Moves corners of a brush (ADR-084): a vertex, the two ends of an edge, or
   the ends of a grid line, which first splits the brush along `split`. Each
   piece becomes the hull of its moved corners, or, when a moved corner
   dents it, convex pieces of the dented solid; the brush keeps the first
   piece and the rest become new brushes. */
static bool8_t ops_build_reshape(OpsContext *ctx, const VkrBakeryJson *args,
                                 OpsBatch *batch) {
  OpsBrushRead *brush =
      arena_alloc(ops_arena(ctx), sizeof(*brush), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  VkrBrushPiece *pieces =
      arena_alloc(ops_arena(ctx), VKR_BRUSH_RESHAPE_PIECE_MAX * sizeof(*pieces),
                  ARENA_MEMORY_TAG_STRUCT);
  if (!brush || !geometry || !pieces) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  Vec3 points[8];
  Vec3 delta = {0};
  bool8_t has_delta = false_v;
  if (!ops_brush_arg(ctx, batch, args, "brush", brush) ||
      !ops_brush_unscaled(ctx, brush) ||
      !ops_arg_vec3(ctx, args, "delta", &delta, &has_delta)) {
    return false_v;
  }
  const uint32_t point_count =
      ops_arg_points(ctx, args, "points", points, 1u, ArrayCount(points));
  if (!point_count) {
    return false_v;
  }
  if (!has_delta || vec3_length(delta) < 1.0e-4f) {
    return ops_fail(ctx, OPS_INVALID,
                    "'delta' is the world move of the corners in meters");
  }

  /* Corners and the move in the brush's space. */
  const Mat4 inverse = mat4_inverse_affine(brush->transform->world);
  for (uint32_t i = 0; i < point_count; ++i) {
    points[i] = mat4_mul_vec3(inverse, points[i]);
  }
  const Vec4 local_delta = mat4_mul_vec4(inverse, vec3_to_vec4(delta, 0.0f));
  const Vec3 move = vec3_new(local_delta.x, local_delta.y, local_delta.z);
  VkrBrushPlane split = {0};
  const VkrBakeryJson *split_args = vkr_bakery_json_get(args, "split");
  if (split_args) {
    Vec3 normal = {0};
    Vec3 point = {0};
    bool8_t has_normal = false_v;
    bool8_t has_point = false_v;
    if (split_args->type != VKR_BAKERY_JSON_OBJECT ||
        !ops_arg_vec3(ctx, split_args, "normal", &normal, &has_normal) ||
        !ops_arg_vec3(ctx, split_args, "point", &point, &has_point) ||
        !has_normal || !has_point || vec3_length(normal) < 1.0e-4f) {
      return ctx->call->error_code
                 ? false_v
                 : ops_fail(ctx, OPS_INVALID,
                            "'split' is {point, normal}, a world plane");
    }
    split = ops_plane_to_brush(brush, normal, point);
  }

  uint32_t piece_count = 0u;
  const VkrBrushError error =
      vkr_brush_reshape(brush->planes, brush->count, split_args ? &split : NULL,
                        points, point_count, move, pieces,
                        VKR_BRUSH_RESHAPE_PIECE_MAX, &piece_count, geometry);
  if (error != VKR_BRUSH_OK) {
    return ops_fail(ctx, OPS_INVALID, "The brush cannot take that shape: %s",
                    vkr_brush_error_text(error));
  }
  uint32_t item = UINT32_MAX;
  if (!ops_brush_replace(ctx, batch, brush, pieces, piece_count, NULL, 0u,
                         &brush->values[0], true_v, NULL, &item)) {
    return false_v;
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
  if (change->author[0]) {
    ops_set(ctx, object, "author",
            vkr_bakery_json_cstr(ops_arena(ctx), change->author));
  }
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
  /* With `settle`, an applied batch answers once the scene rebuilt what it
     changed; `results` holds its answer's entries and `entities` their
     entities, one per operation. */
  bool8_t settle;
  bool8_t applied;
  VkrBakeryJson *results;
  VkrEntityId *entities;
  /* A batch that put something in another author's claim reverts its group
     and then fails with `claim_error`. */
  bool8_t reverting;
  uint64_t revert_token;
  char claim_error[256];
} OpsPendingBatch;

/* Remembers that `author` made `group`, for the undo of agents. */
static void ops_authored_add(VkrEditorOps *ops, uint64_t group,
                             uint16_t container, const char *author) {
  OpsAuthored *slot = &ops->authored[ops->authored_next % OPS_AUTHORED_MAX];
  *slot = (OpsAuthored){.group = group, .container = container};
  snprintf(slot->author, sizeof(slot->author), "%s", author);
  ops->authored_next++;
}

/* The author of `group`, or NULL when no recent agent batch made it. */
static const char *ops_authored_find(const VkrEditorOps *ops, uint64_t group,
                                     uint16_t container) {
  const uint32_t count = Min(ops->authored_next, OPS_AUTHORED_MAX);
  for (uint32_t i = 0; group && i < count; ++i) {
    const OpsAuthored *entry = &ops->authored[i];
    if (entry->group == group && entry->container == container) {
      return entry->author;
    }
  }
  return NULL;
}

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

/* Fails with the claim `hit` that `what` would touch. */
static bool8_t ops_claim_fail(OpsContext *ctx, const VkrEditorClaim *hit,
                              const char *what) {
  return ops_fail(ctx, OPS_CLAIMED,
                  "%s lies in %s's claim '%s' (claim %u); build outside it, "
                  "or ask %s to release it",
                  what, hit->author, hit->name[0] ? hit->name : "unnamed",
                  hit->id, hit->author);
}

/* Refuses a batch before it applies when it changes another author's
   claim: the existing entities it edits or deletes, by their current box,
   and the regions its terrain edits change. */
static bool8_t ops_claims_before(OpsContext *ctx, const OpsBatch *batch) {
  VkrEditorOps *ops = ctx->ops;
  if (!ctx->call->author[0] || !ops->claim_count) {
    return true_v;
  }
  const uint16_t container = (uint16_t)batch->container;
  const VkrScene *scene = ops_scene(ctx->frame, container);
  VkrBrushGeometry *scratch = arena_alloc(
      ops_arena(ctx), sizeof(VkrBrushGeometry), ARENA_MEMORY_TAG_STRUCT);
  for (uint32_t op = 0; scene && op < batch->op_count; ++op) {
    const VkrEntityId entity = batch->op_target[op];
    if (!entity.u64 || !vkr_scene_entity_alive(scene, entity) ||
        vkr_scene_get_typed(scene, entity, &vkr_scene_terrain_type)) {
      continue;
    }
    Vec3 lo = {0};
    Vec3 hi = {0};
    ops_object_box(scene, entity, scratch, &lo, &hi);
    const VkrEditorClaim *hit =
        ops_claim_hit(ops, ctx->call->author, container, lo, hi);
    if (hit) {
      const String8 name = vkr_scene_get_name(scene, entity);
      char what[96];
      snprintf(what, sizeof(what), "ops[%u] changes '%.*s', which", op,
               (int)Min(name.length, (uint64_t)48u), (const char *)name.str);
      return ops_claim_fail(ctx, hit, what);
    }
  }
  for (uint32_t i = 0; i < batch->touched_count; ++i) {
    const VkrEditorClaim *hit =
        ops_claim_hit(ops, ctx->call->author, container, batch->touched_min[i],
                      batch->touched_max[i]);
    if (hit) {
      return ops_claim_fail(ctx, hit, "A terrain edit");
    }
  }
  return true_v;
}

/* The claim of another author an applied batch put something in: each
   operation's entity and every object it created, by its new box. */
static const VkrEditorClaim *
ops_claims_after(OpsContext *ctx, const OpsBatch *batch,
                 const VkrSampleEditBatchResult *result,
                 VkrEntityId *out_entity) {
  VkrEditorOps *ops = ctx->ops;
  const uint16_t container = (uint16_t)batch->container;
  const VkrScene *scene = ops_scene(ctx->frame, container);
  if (!scene) {
    return NULL;
  }
  VkrBrushGeometry *scratch = arena_alloc(
      ops_arena(ctx), sizeof(VkrBrushGeometry), ARENA_MEMORY_TAG_STRUCT);
  for (uint32_t i = 0; i < batch->op_count + batch->count; ++i) {
    VkrEntityId entity = VKR_ENTITY_ID_INVALID;
    if (i < batch->op_count) {
      entity = batch->op_item[i] != UINT32_MAX
                   ? result->created[batch->op_item[i]]
                   : batch->op_target[i];
    } else {
      entity = result->created[i - batch->op_count];
    }
    if (!entity.u64 || !vkr_scene_entity_alive(scene, entity) ||
        vkr_scene_entity_is_part(scene, entity) ||
        vkr_scene_get_typed(scene, entity, &vkr_scene_terrain_type)) {
      continue;
    }
    Vec3 lo = {0};
    Vec3 hi = {0};
    ops_object_box(scene, entity, scratch, &lo, &hi);
    const VkrEditorClaim *hit =
        ops_claim_hit(ops, ctx->call->author, container, lo, hi);
    if (hit) {
      *out_entity = entity;
      return hit;
    }
  }
  return NULL;
}

/* Hands the built batch to the runtime and waits for its result. */
static VkrEditorOpStatus ops_submit(OpsContext *ctx, OpsBatch *batch,
                                    bool8_t dry_run) {
  VkrEditorOpCall *call = ctx->call;
  if (!batch->count) {
    ops_fail(ctx, OPS_INVALID, "The batch holds no edits");
    return VKR_EDITOR_OP_DONE;
  }
  if (!ops_claims_before(ctx, batch)) {
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
  MemZero(pending, sizeof(*pending));
  pending->batch = *batch;
  pending->token = ++ctx->ops->next_token;
  pending->settle = ops_arg_bool(call->args, "settle", false_v);
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

/* Answers an applied batch with `settle` once nothing rebuilds, adding each
   operation entity's bounds and build status. */
static VkrEditorOpStatus ops_batch_settle(OpsContext *ctx,
                                          OpsPendingBatch *pending) {
  VkrEditorOpCall *call = ctx->call;
  const bool8_t settled = ops_settled(ctx->frame, NULL);
  if (!settled && call->frames < OPS_SCENE_SETTLE_FRAMES) {
    call->frames++;
    return VKR_EDITOR_OP_WAIT;
  }
  call->settle_timeout = !settled;
  const VkrScene *scene =
      ops_scene(ctx->frame, (uint32_t)pending->batch.container);
  uint32_t op = 0u;
  for (VkrBakeryJson *entry = pending->results->first;
       entry && op < pending->batch.op_count; entry = entry->next, ++op) {
    ops_entity_built(ctx, scene, pending->entities[op], entry);
  }
  return VKR_EDITOR_OP_DONE;
}

/* Reads the runtime's batch result and answers with each operation's entity;
   a reviewed batch becomes a pending change. */
static VkrEditorOpStatus ops_batch_wait(OpsContext *ctx) {
  VkrEditorOpCall *call = ctx->call;
  OpsPendingBatch *pending = call->state;
  if (pending->applied) {
    return ops_batch_settle(ctx, pending);
  }
  if (pending->reverting) {
    const VkrSampleEditBatchResult *reverted = ctx->frame->edit_batch_result;
    if (!reverted || reverted->token != pending->revert_token) {
      if (++call->frames > OPS_WAIT_FRAMES) {
        ops_fail(ctx, OPS_CLAIMED, "%s; the revert did not finish",
                 pending->claim_error);
        return VKR_EDITOR_OP_DONE;
      }
      return VKR_EDITOR_OP_WAIT;
    }
    ops_fail(ctx, OPS_CLAIMED, "%s%s%s", pending->claim_error,
             reverted->ok ? "; the batch was reverted"
                          : "; the batch could not be reverted: ",
             reverted->ok ? "" : reverted->message);
    return VKR_EDITOR_OP_DONE;
  }
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
  /* Something now inside another author's claim: revert the whole batch,
     the newest group, in this build's edit slot. */
  VkrEntityId offender = VKR_ENTITY_ID_INVALID;
  const VkrEditorClaim *claimed =
      call->author[0] && ctx->ops->claim_count
          ? ops_claims_after(ctx, batch, result, &offender)
          : NULL;
  if (claimed && ctx->frame->edit_batch && !ctx->frame->edit_batch->token) {
    const VkrScene *where = ops_scene(ctx->frame, (uint32_t)batch->container);
    const String8 name = vkr_scene_get_name(where, offender);
    char what[96];
    snprintf(what, sizeof(what), "'%.*s'", (int)Min(name.length, (uint64_t)48u),
             (const char *)name.str);
    (void)ops_claim_fail(ctx, claimed, what);
    snprintf(pending->claim_error, sizeof(pending->claim_error), "%s",
             call->error);
    call->error_code = NULL;
    call->error[0] = '\0';
    pending->reverting = true_v;
    pending->revert_token = ++ctx->ops->next_token;
    *ctx->frame->edit_batch = (VkrSampleEditBatchRequest){
        .token = pending->revert_token,
        .container = (uint16_t)batch->container,
        .revert_group = result->group,
    };
    call->frames = 0u;
    return VKR_EDITOR_OP_WAIT;
  }
  const VkrScene *scene = ops_scene(ctx->frame, (uint32_t)batch->container);
  call->result = vkr_bakery_json_object(call->arena);
  VkrBakeryJson *results = vkr_bakery_json_array(call->arena);
  VkrEditorChange change = {.group = result->group,
                            .container = (uint16_t)batch->container};
  snprintf(change.author, sizeof(change.author), "%s", call->author);
  change.created = vkr_platform_get_absolute_time();
  if (pending->settle) {
    pending->entities =
        arena_alloc(call->arena, sizeof(VkrEntityId) * Max(batch->op_count, 1u),
                    ARENA_MEMORY_TAG_ARRAY);
    pending->settle = pending->entities != NULL;
  }
  for (uint32_t op = 0; op < batch->op_count; ++op) {
    VkrEntityId entity = batch->op_target[op];
    if (batch->op_item[op] != UINT32_MAX) {
      entity = result->created[batch->op_item[op]];
    }
    VkrBakeryJson *entry = vkr_bakery_json_object(call->arena);
    ops_set(ctx, entry, "entity", ops_entity(ctx, scene, entity));
    vkr_bakery_json_append(results, entry);
    ops_change_touch(&change, entity);
    if (pending->settle) {
      pending->entities[op] = entity;
    }
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
  if (call->author[0]) {
    ops_authored_add(ops, result->group, change.container, call->author);
  }
  /* The feed names the batch's objects and the box around them. */
  char label[96];
  snprintf(label, sizeof(label), "%.*s",
           (int)Min(batch->label.length, (uint64_t)95u),
           batch->label.length ? (const char *)batch->label.str : "");
  OpsFeedEvent *event = ops_feed_add(ops, OPS_FEED_APPLIED, change.container,
                                     call->author, label);
  VkrBrushGeometry *scratch = arena_alloc(call->arena, sizeof(VkrBrushGeometry),
                                          ARENA_MEMORY_TAG_STRUCT);
  for (uint32_t i = 0; i < change.entity_count; ++i) {
    ops_feed_entity(event, scene, change.entities[i], scratch);
  }
  if (batch->review && ops->change_count < VKR_EDITOR_CHANGE_MAX) {
    ops->review_overflow = false_v;
    change.id = ++ops->next_change_id;
    snprintf(change.label, sizeof(change.label), "%.*s",
             (int)Min(batch->label.length, (uint64_t)95u),
             batch->label.length ? (const char *)batch->label.str : "");
    if (!change.label[0]) {
      snprintf(change.label, sizeof(change.label), "Agent edit %u", change.id);
    }
    ops->changes[ops->change_count++] = change;
    event->change = change.id;
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
    if (!ops->review_overflow) {
      ops->review_overflow = true_v;
      char toast[160];
      snprintf(toast, sizeof(toast),
               "%u agent changes wait for review; new ones apply unreviewed "
               "until some are accepted or rejected",
               VKR_EDITOR_CHANGE_MAX);
      vkr_editor_toast(ctx->editor, VKR_UI_ICON_TERMINAL,
                       vkr_ui_theme()->warning, toast);
    }
  }
  if (!pending->settle) {
    return VKR_EDITOR_OP_DONE;
  }
  pending->applied = true_v;
  pending->results = results;
  call->frames = 0u;
  return ops_batch_settle(ctx, pending);
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
      /* Grows with every edit, undo and redo, so a client can tell that
         someone changed the scene. */
      ops_set(ctx, container, "revision",
              vkr_bakery_json_int(arena, edits ? (int64_t)edits->revision : 0));
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
  OpsSettle pending = {0};
  VkrBakeryJson *settle = vkr_bakery_json_object(arena);
  ops_set(ctx, settle, "settled",
          vkr_bakery_json_bool(arena, ops_settled(frame, &pending)));
  ops_set(ctx, settle, "brushes", vkr_bakery_json_int(arena, pending.brushes));
  ops_set(ctx, settle, "terrain", vkr_bakery_json_bool(arena, pending.terrain));
  ops_set(ctx, settle, "population",
          vkr_bakery_json_int(arena, pending.population));
  ops_set(ctx, result, "rebuilding", settle);
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
  const uint32_t active = ctx->editor->workbenches.active;
  ops_set(ctx, result, "workbench",
          vkr_bakery_json_cstr(arena, vkr_editor_workbench_id(
                                          &ctx->editor->workbenches, active)));
  ops_set(ctx, result, "scene_tool",
          vkr_bakery_json_cstr(
              arena, vkr_editor_scene_tool_name(
                         vkr_editor_workbench_mode(ctx->editor, frame, active)
                             .scene_tool)));
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

// -----------------------------------------------------------------------------
// Workbenches (ADR-089)
// -----------------------------------------------------------------------------

static VkrEditorOpStatus ops_run_workbench_list(OpsContext *ctx) {
  const VkrSampleUiFrame *frame = ctx->frame;
  const VkrEditorUi *editor = ctx->editor;
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  VkrBakeryJson *items = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < editor->workbenches.count; ++i) {
    VkrBakeryJson *item = vkr_bakery_json_object(arena);
    ops_set(ctx, item, "id",
            vkr_bakery_json_cstr(
                arena, vkr_editor_workbench_id(&editor->workbenches, i)));
    ops_set(ctx, item, "name",
            vkr_bakery_json_cstr(
                arena, vkr_editor_workbench_name(&editor->workbenches, i)));
    ops_set(ctx, item, "custom",
            vkr_bakery_json_bool(arena, editor->workbenches.items[i].custom));
    ops_set(ctx, item, "position", vkr_bakery_json_int(arena, i + 1u));
    char shortcut[48] = "";
    (void)vkr_editor_command_shortcut((EditorCommand)(CMD_WORKBENCH_1 + i),
                                      shortcut, sizeof(shortcut));
    ops_set(ctx, item, "shortcut", vkr_bakery_json_cstr(arena, shortcut));
    /* Panel tabs, in tree order. */
    VkrBakeryJson *panels = vkr_bakery_json_array(arena);
    const VkrUiDockTree *tree = vkr_editor_workbench_layout(editor, frame, i);
    for (uint32_t n = 0; n < tree->node_high_water; ++n) {
      const VkrUiDockNode *node = &tree->nodes[n];
      if (!node->used || node->kind != VKR_UI_DOCK_NODE_TABS) {
        continue;
      }
      for (uint32_t t = 0; t < node->as.leaf.tab_count; ++t) {
        const VkrUiDockPanelKind kind = node->as.leaf.tabs[t].panel_kind;
        if (kind != VKR_UI_DOCK_PANEL_TOOLBAR) {
          vkr_bakery_json_append(
              panels, ops_string(ctx, vkr_ui_dock_panel_label(kind)));
        }
      }
    }
    ops_set(ctx, item, "panels", panels);
    VkrBakeryJson *windows = vkr_bakery_json_array(arena);
    const uint32_t open = vkr_editor_workbench_windows(editor, i);
    for (uint32_t w = 0; w < VKR_EDITOR_WINDOW_COUNT; ++w) {
      if ((open >> w) & 1u) {
        vkr_bakery_json_append(
            windows, vkr_bakery_json_cstr(arena, vkr_editor_cmd_window_name(
                                                     (VkrEditorWindowKind)w)));
      }
    }
    ops_set(ctx, item, "windows", windows);
    const VkrEditorWorkbenchMode mode =
        vkr_editor_workbench_mode(editor, frame, i);
    VkrBakeryJson *mode_json = vkr_bakery_json_object(arena);
    uint32_t tool = 0u;
    while (tool < 3u && vkr_editor_cmd_tool_modes[tool] != mode.gizmo_tool) {
      ++tool;
    }
    ops_set(ctx, mode_json, "tool",
            vkr_bakery_json_cstr(arena, vkr_editor_cmd_tools[tool]));
    ops_set(ctx, mode_json, "scene_tool",
            vkr_bakery_json_cstr(arena,
                                 vkr_editor_scene_tool_name(mode.scene_tool)));
    ops_set(ctx, mode_json, "snap",
            vkr_bakery_json_cstr(arena, vkr_editor_snap_name(mode.snap)));
    ops_set(ctx, mode_json, "grid", vkr_bakery_json_bool(arena, mode.grid));
    ops_set(ctx, item, "mode", mode_json);
    vkr_bakery_json_append(items, item);
  }
  ops_set(ctx, result, "active",
          vkr_bakery_json_cstr(
              arena, vkr_editor_workbench_id(&editor->workbenches,
                                             editor->workbenches.active)));
  ops_set(ctx, result, "workbenches", items);
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

/* Answers two builds after the request: the switch applies at the start of
   the next build, and the Scene rectangle follows the build after. */
static VkrEditorOpStatus ops_run_workbench_switch(OpsContext *ctx) {
  VkrEditorOpCall *call = ctx->call;
  if (call->stage == 0u) {
    String8 word = {0};
    const uint32_t index =
        vkr_bakery_json_get_string(call->args, "workbench", &word)
            ? vkr_editor_workbench_find(&ctx->editor->workbenches, word)
            : UINT32_MAX;
    if (index == UINT32_MAX) {
      ops_fail(ctx, OPS_INVALID,
               "'workbench' is general, level_design, terrain, lighting or "
               "scripting");
      return VKR_EDITOR_OP_DONE;
    }
    char message[96];
    if (!vkr_editor_workbench_request(ctx->editor, ctx->frame, index, message,
                                      sizeof(message))) {
      ops_fail(ctx, OPS_BUSY, "%s", message);
      return VKR_EDITOR_OP_DONE;
    }
    call->token = index;
    call->stage = 1u;
    return VKR_EDITOR_OP_WAIT;
  }
  if (call->stage == 1u) {
    call->stage = 2u;
    return VKR_EDITOR_OP_WAIT;
  }
  const VkrSampleUiFrame *frame = ctx->frame;
  if (ctx->editor->workbenches.active != (uint32_t)call->token) {
    ops_fail(ctx, OPS_BUSY, "A drag held the mouse; the switch did not apply");
    return VKR_EDITOR_OP_DONE;
  }
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  ops_set(ctx, result, "workbench",
          vkr_bakery_json_cstr(
              arena, vkr_editor_workbench_id(&ctx->editor->workbenches,
                                             (uint32_t)call->token)));
  const float32_t scale = frame->ui->content_scale;
  const Vec4 rect = frame->mapping.panel_rect_px;
  VkrBakeryJson *scene = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(scene, ops_number(ctx, rect.x / scale));
  vkr_bakery_json_append(scene, ops_number(ctx, rect.y / scale));
  vkr_bakery_json_append(scene, ops_number(ctx, rect.z / scale));
  vkr_bakery_json_append(scene, ops_number(ctx, rect.w / scale));
  ops_set(ctx, result, "scene_rect", scene);
  call->result = result;
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
      VkrSceneTerrainStreaming streaming;
      if (vkr_scene_terrain_streaming(scene, entity, &streaming)) {
        VkrBakeryJson *held = vkr_bakery_json_object(arena);
        ops_set(ctx, held, "streamed",
                vkr_bakery_json_bool(arena, streaming.streamed));
        ops_set(ctx, held, "detail_tiles",
                ops_number(ctx, (float32_t)streaming.fine_tiles));
        ops_set(ctx, held, "overview_tiles",
                ops_number(ctx, (float32_t)streaming.overview_tiles));
        ops_set(ctx, held, "body_tiles",
                ops_number(ctx, (float32_t)streaming.body_tiles));
        ops_set(ctx, held, "sample_tiles",
                ops_number(ctx, (float32_t)streaming.resident_tiles));
        ops_set(ctx, held, "unsaved_tiles",
                ops_number(ctx, (float32_t)streaming.unsaved_tiles));
        ops_set(ctx, summary, "held", held);
      }
      ops_set(ctx, row, "terrain", summary);
    }
    if (vkr_scene_get_typed(scene, entity, &vkr_scene_spline_mesh_type) ||
        vkr_scene_get_typed(scene, entity, &vkr_scene_scatter_type)) {
      VkrBakeryJson *summary = vkr_bakery_json_object(arena);
      const char *status = vkr_scene_population_status(scene, entity);
      ops_set(ctx, summary, "copies",
              ops_number(ctx, (float32_t)vkr_scene_population_instances(
                                  scene, entity)));
      if (status) {
        ops_set(ctx, summary, "status", vkr_bakery_json_cstr(arena, status));
      }
      ops_set(ctx, row, "population", summary);
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
  if (!vkr_bakery_json_get_string(call->args, "label", &batch->label) ||
      !batch->label.length) {
    batch->label = call->op;
  }
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

/* Tells the designer why `change` could not revert: the later pending
   change that edited `conflict`, which must be rejected first, else the
   editor's message. */
static void ops_reject_reason(VkrEditorOps *ops, VkrEditorChange *change,
                              VkrEntityId conflict, const char *name,
                              const char *message) {
  change->rejecting = false_v;
  for (uint32_t i = 0; conflict.u64 && i < ops->change_count; ++i) {
    const VkrEditorChange *later = &ops->changes[i];
    if (later->id <= change->id) {
      continue;
    }
    for (uint32_t e = 0; e < later->entity_count; ++e) {
      if (later->entities[e].u64 != conflict.u64) {
        continue;
      }
      snprintf(change->problem, sizeof(change->problem),
               "Blocked by %s's later change '%s'%s%s; reject that one "
               "first.",
               later->author[0] ? later->author : "the editor", later->label,
               name[0] ? " to " : "", name);
      return;
    }
  }
  if (name[0]) {
    snprintf(change->problem, sizeof(change->problem),
             "Blocked by a later edit to %s; undo or reject that edit "
             "first.",
             name);
    return;
  }
  snprintf(change->problem, sizeof(change->problem), "%s", message);
}

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
    VkrEditorChange *target = &ops->changes[index];
    if (ctx->frame->simulation_running || !ctx->frame->edit_batch ||
        ctx->frame->edit_batch->token) {
      ops_fail(ctx, OPS_BUSY, "The scene cannot change now");
      snprintf(target->problem, sizeof(target->problem),
               "The scene cannot change now: the game plays or another "
               "edit applies. Try again.");
      target->rejecting = false_v;
      return VKR_EDITOR_OP_DONE;
    }
    OpsPendingReject *pending =
        arena_alloc(call->arena, sizeof(*pending), ARENA_MEMORY_TAG_STRUCT);
    if (!pending) {
      ops_fail(ctx, OPS_LIMIT, "Out of request memory");
      target->rejecting = false_v;
      return VKR_EDITOR_OP_DONE;
    }
    target->rejecting = true_v;
    target->problem[0] = '\0';
    const VkrEditorChange *change = target;
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
      const int32_t waiting = ops_change_find(ops, pending->change);
      if (waiting >= 0) {
        snprintf(ops->changes[waiting].problem,
                 sizeof(ops->changes[waiting].problem),
                 "The revert was not applied; try again.");
        ops->changes[waiting].rejecting = false_v;
      }
      return VKR_EDITOR_OP_DONE;
    }
    return VKR_EDITOR_OP_WAIT;
  }
  const int32_t index = ops_change_find(ops, pending->change);
  if (!result->ok) {
    char conflict[40] = "";
    char name[64] = "";
    if (result->conflict.u64) {
      ops_id_text(result->conflict, conflict, sizeof(conflict));
      const VkrScene *scene =
          index >= 0 ? ops_scene(ctx->frame, ops->changes[index].container)
                     : NULL;
      if (scene && vkr_scene_entity_alive(scene, result->conflict)) {
        const String8 text = vkr_scene_get_name(scene, result->conflict);
        snprintf(name, sizeof(name), "%.*s",
                 (int)Min(text.length, (uint64_t)48u), (const char *)text.str);
      }
    }
    ops_fail(ctx, OPS_REJECTED, "%s%s%s%s%s%s", result->message,
             conflict[0] ? " Conflicting entity: " : "", name,
             name[0] ? " (" : "", conflict, name[0] ? ")" : "");
    if (index >= 0) {
      ops_reject_reason(ops, &ops->changes[index], result->conflict, name,
                        ctx->call->error);
    }
    return VKR_EDITOR_OP_DONE;
  }
  if (index >= 0) {
    const VkrEditorChange *gone = &ops->changes[index];
    OpsFeedEvent *event = ops_feed_add(ops, OPS_FEED_REJECTED, gone->container,
                                       gone->author, gone->label);
    event->change = gone->id;
    for (uint32_t i = 0; i < Min(gone->entity_count, OPS_FEED_ENTITIES); ++i) {
      event->entities[event->entity_count++] = gone->entities[i];
    }
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

/* Whether the step undo (or redo) takes next is one of the caller's own
   batches. Undo follows the most recent entry across every journal, as the
   runtime does, so an agent would otherwise undo whatever another agent or
   the designer did last. */
static bool8_t ops_undo_own(OpsContext *ctx, bool8_t redo) {
  uint32_t containers[VKR_SCENE_ADDITIVE_MAX + 2u] = {0u,
                                                      VKR_SCENE_WORLD_ROOT_ID};
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    containers[i + 2u] = i + 1u;
  }
  const VkrSceneEditState *best = NULL;
  uint32_t best_container = 0u;
  uint64_t best_sequence = 0u;
  for (uint32_t i = 0; i < ArrayCount(containers); ++i) {
    const VkrSceneEditState *journal = ops_journal(ctx->frame, containers[i]);
    const uint64_t sequence =
        journal ? vkr_scene_edit_next_sequence(journal, redo) : 0u;
    if (sequence && (!best_sequence || (redo ? sequence < best_sequence
                                             : sequence > best_sequence))) {
      best = journal;
      best_container = containers[i];
      best_sequence = sequence;
    }
  }
  if (!best) {
    return true_v;
  }
  const VkrSceneEditEntry *entry =
      &best->undo[redo ? best->undo_cursor : best->undo_cursor - 1u];
  const char *owner =
      ops_authored_find(ctx->ops, entry->group, (uint16_t)best_container);
  if (owner && strcmp(owner, ctx->call->author) == 0) {
    return true_v;
  }
  return ops_fail(ctx, OPS_NOT_OWNER,
                  "The next %s step belongs to %s%s; an agent undoes only its "
                  "own batches, or rejects its change with changes.reject",
                  redo ? "redo" : "undo", owner ? owner : "the designer",
                  owner ? "" : " or an older edit");
}

static VkrEditorOpStatus ops_run_undo(OpsContext *ctx) {
  /* Undo and redo run as Cmd lines, which own the scene edit slot. */
  if (!ctx->call->stage) {
    const bool8_t redo = ops_equals(ctx->call->op, "redo");
    if (ctx->call->author[0] && !ops_undo_own(ctx, redo)) {
      return VKR_EDITOR_OP_DONE;
    }
    VkrBakeryJson *args = vkr_bakery_json_object(ops_arena(ctx));
    ops_set(ctx, args, "line",
            vkr_bakery_json_cstr(ops_arena(ctx), redo ? "redo" : "undo"));
    ctx->call->args = args;
  }
  return ops_run_cmd(ctx);
}

// -----------------------------------------------------------------------------
// partition.describe, partition.load, partition.unload (ADR-086)
// -----------------------------------------------------------------------------

/* The cells a `region` in metres covers, x0, z0, x1, z1; false without a
   valid region. */
static bool8_t ops_partition_region(OpsContext *ctx,
                                    const SceneWorldPartition *settings,
                                    int32_t out[4]) {
  const VkrBakeryJson *region = vkr_bakery_json_get(ctx->call->args, "region");
  Vec3 lo = {0};
  Vec3 hi = {0};
  bool8_t has_lo = false_v;
  bool8_t has_hi = false_v;
  if (!region || !ops_arg_vec3(ctx, region, "min", &lo, &has_lo) ||
      !ops_arg_vec3(ctx, region, "max", &hi, &has_hi) || !has_lo || !has_hi) {
    return false_v;
  }
  const VkrScenePartitionCell a = vkr_scene_partition_cell_at(settings, lo);
  const VkrScenePartitionCell b = vkr_scene_partition_cell_at(settings, hi);
  out[0] = Min(a.x, b.x);
  out[1] = Min(a.z, b.z);
  out[2] = Max(a.x, b.x);
  out[3] = Max(a.z, b.z);
  return true_v;
}

static VkrEditorOpStatus ops_run_partition_describe(OpsContext *ctx) {
  const VkrScene *scene = ctx->frame->scene;
  SceneWorldPartition settings;
  if (!scene || !vkr_scene_partition_settings(scene, &settings)) {
    ops_fail(ctx, OPS_INVALID, "The open scene has no world partition");
    return VKR_EDITOR_OP_DONE;
  }
  int32_t range[4] = {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX};
  if (vkr_bakery_json_get(ctx->call->args, "region") &&
      !ops_partition_region(ctx, &settings, range)) {
    ops_fail(ctx, OPS_INVALID, "'region' needs 'min' and 'max' points");
    return VKR_EDITOR_OP_DONE;
  }
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  ops_set(ctx, result, "cell_size", ops_number(ctx, settings.cell_size));
  ops_set(ctx, result, "load_radius", ops_number(ctx, settings.load_radius));
  ops_set(ctx, result, "proxy_radius", ops_number(ctx, settings.proxy_radius));
  ops_set(ctx, result, "cell_budget",
          ops_number(ctx, (float64_t)settings.cell_budget));
  VkrBakeryJson *cells = vkr_bakery_json_array(arena);
  uint32_t count = 0u;
  const VkrScenePartitionCellRecord *records =
      vkr_scene_partition_cells(scene, &count);
  uint32_t matched = 0u;
  uint32_t loaded = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    const VkrScenePartitionCellRecord *record = &records[i];
    const uint32_t flags = record->flags;
    loaded += (flags & VKR_SCENE_PARTITION_CELL_LOADED) != 0u;
    if (record->cell.x < range[0] || record->cell.x > range[2] ||
        record->cell.z < range[1] || record->cell.z > range[3]) {
      continue;
    }
    if (++matched > OPS_PARTITION_CELLS_MAX) {
      continue;
    }
    VkrBakeryJson *row = vkr_bakery_json_object(arena);
    VkrBakeryJson *at = vkr_bakery_json_array(arena);
    vkr_bakery_json_append(at, vkr_bakery_json_int(arena, record->cell.x));
    vkr_bakery_json_append(at, vkr_bakery_json_int(arena, record->cell.z));
    ops_set(ctx, row, "cell", at);
    ops_set(
        ctx, row, "loaded",
        vkr_bakery_json_bool(arena, flags & VKR_SCENE_PARTITION_CELL_LOADED));
    ops_set(
        ctx, row, "pinned",
        vkr_bakery_json_bool(arena, flags & VKR_SCENE_PARTITION_CELL_PINNED));
    ops_set(
        ctx, row, "document",
        vkr_bakery_json_bool(arena, flags & VKR_SCENE_PARTITION_CELL_ON_DISK));
    ops_set(ctx, row, "proxy",
            vkr_bakery_json_bool(arena, record->proxy.u64 != 0u));
    ops_set(ctx, row, "unreadable",
            vkr_bakery_json_bool(arena,
                                 flags & VKR_SCENE_PARTITION_CELL_UNREADABLE));
    vkr_bakery_json_append(cells, row);
  }
  ops_set(ctx, result, "known", ops_number(ctx, (float64_t)count));
  ops_set(ctx, result, "loaded", ops_number(ctx, (float64_t)loaded));
  ops_set(ctx, result, "matched", ops_number(ctx, (float64_t)matched));
  ops_set(ctx, result, "cells", cells);
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

/* partition.load and partition.unload run as Cmd lines, which own the
   scene edit slot. */
static VkrEditorOpStatus ops_run_partition_change(OpsContext *ctx) {
  if (!ctx->call->stage) {
    const bool8_t unload = ops_equals(ctx->call->op, "partition.unload");
    const VkrScene *scene = ctx->frame->scene;
    SceneWorldPartition settings;
    if (!scene || !vkr_scene_partition_settings(scene, &settings)) {
      ops_fail(ctx, OPS_INVALID, "The open scene has no world partition");
      return VKR_EDITOR_OP_DONE;
    }
    bool8_t all = false_v;
    int32_t range[4];
    char line[160];
    if (unload && vkr_bakery_json_get_bool(ctx->call->args, "all", &all) &&
        all) {
      snprintf(line, sizeof(line), "partition.unload all");
    } else if (ops_partition_region(ctx, &settings, range)) {
      snprintf(line, sizeof(line), "%s %d %d %d %d",
               unload ? "partition.unload" : "partition.load", range[0],
               range[1], range[2], range[3]);
    } else {
      ops_fail(ctx, OPS_INVALID, "'region' needs 'min' and 'max' points%s",
               unload ? ", or pass 'all'" : "");
      return VKR_EDITOR_OP_DONE;
    }
    VkrBakeryJson *args = vkr_bakery_json_object(ops_arena(ctx));
    ops_set(ctx, args, "line", vkr_bakery_json_cstr(ops_arena(ctx), line));
    ctx->call->args = args;
  }
  return ops_run_cmd(ctx);
}

// -----------------------------------------------------------------------------
// view.capture
// -----------------------------------------------------------------------------

/* Views one capture composes into a sheet, world points it marks, and the
   gap between a sheet's cells in pixels. */
#define OPS_CAPTURE_VIEWS_MAX 4u
#define OPS_CAPTURE_MARKS_MAX 32u
#define OPS_CAPTURE_GAP 4u

/* One view of a capture: how to show it and, once taken, its pixels. */
typedef struct OpsCaptureView {
  VkrSampleViewRequest request;
  bool8_t has_eye;
  Vec3 eye;
  Vec3 target;
  char name[16];
  /* The matrix of the build that asked for the frame; the camera rests
     there while the capture waits. */
  Mat4 view_projection;
  /* RGBA8 pixels at the cell size, in the request arena. */
  uint8_t *pixels;
  uint32_t width;
  uint32_t height;
  /* Each mark's pixel in the cell, when it lies in front of the camera and
     inside the image. */
  Vec2 marks[OPS_CAPTURE_MARKS_MAX];
  bool8_t mark_shown[OPS_CAPTURE_MARKS_MAX];
  /* Collision lies between the camera and the mark. */
  bool8_t mark_hidden[OPS_CAPTURE_MARKS_MAX];
} OpsCaptureView;

typedef struct OpsPendingCapture {
  /* Capture the whole window instead of the Scene's image. */
  bool8_t window;
  VkrSampleViewState saved_view;
  VkrSampleSceneRecall saved_recall;
  bool8_t restore;
  char path[512];
  uint32_t width;
  uint32_t height;
  /* The widest image the agent wants, zero for the captured size. */
  uint32_t max_width;
  /* A sheet or marks report each view's place; a plain capture answers
     with its path and size alone. */
  bool8_t describe_views;
  /* When the capture switched to its current view. */
  float64_t view_since;
  uint32_t view_count;
  uint32_t view_index;
  OpsCaptureView views[OPS_CAPTURE_VIEWS_MAX];
  uint32_t mark_count;
  Vec3 marks[OPS_CAPTURE_MARKS_MAX];
  /* Each mark's label as the agent gave it, empty for none. */
  char mark_labels[OPS_CAPTURE_MARKS_MAX][16];
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
/* The captured frame as RGBA8 rows from the top, of the Scene's image
   rectangle or the whole window; heap memory the caller frees. */
static uint8_t *ops_capture_rgba(OpsContext *ctx,
                                 const OpsPendingCapture *pending,
                                 const VkrCaptureItemResult *item,
                                 uint32_t *out_width, uint32_t *out_height) {
  const bool8_t rgba8 = item->format == VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM ||
                        item->format == VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB;
  const bool8_t bgra8 = item->format == VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM ||
                        item->format == VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB;
  const bool8_t half = item->format == VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT;
  if (!rgba8 && !bgra8 && !half) {
    (void)ops_fail(ctx, OPS_CAPTURE, "Unsupported capture format %u",
                   (unsigned)item->format);
    return NULL;
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
    (void)ops_fail(ctx, OPS_CAPTURE, "The Scene has no visible area");
    return NULL;
  }
  uint8_t *pixels = malloc((size_t)width * height * 4u);
  if (!pixels) {
    (void)ops_fail(ctx, OPS_LIMIT, "Out of memory for the capture");
    return NULL;
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
  *out_width = width;
  *out_height = height;
  return pixels;
}

/* `source` (RGBA8, width by height) at `target_width` pixels wide, its
   aspect kept: each pixel averages the source pixels it covers. Arena
   memory; NULL when it does not fit. */
static uint8_t *ops_image_shrink(Arena *arena, const uint8_t *source,
                                 uint32_t width, uint32_t height,
                                 uint32_t target_width, uint32_t *out_height) {
  const uint32_t w = Max(1u, Min(target_width, width));
  const uint32_t h =
      Max(1u, (uint32_t)(((uint64_t)height * w + width / 2u) / width));
  uint8_t *out =
      arena_alloc(arena, (uint64_t)w * h * 4u, ARENA_MEMORY_TAG_ARRAY);
  if (!out) {
    return NULL;
  }
  for (uint32_t y = 0; y < h; ++y) {
    const uint32_t ya = (uint32_t)((uint64_t)y * height / h);
    const uint32_t yb =
        Max(ya + 1u, (uint32_t)((uint64_t)(y + 1u) * height / h));
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t xa = (uint32_t)((uint64_t)x * width / w);
      const uint32_t xb =
          Max(xa + 1u, (uint32_t)((uint64_t)(x + 1u) * width / w));
      uint32_t sum[3] = {0u, 0u, 0u};
      for (uint32_t sy = ya; sy < yb; ++sy) {
        const uint8_t *row = source + ((size_t)sy * width + xa) * 4u;
        for (uint32_t sx = xa; sx < xb; ++sx, row += 4) {
          sum[0] += row[0];
          sum[1] += row[1];
          sum[2] += row[2];
        }
      }
      const uint32_t count = (yb - ya) * (xb - xa);
      uint8_t *pixel = out + ((size_t)y * w + x) * 4u;
      pixel[0] = (uint8_t)(sum[0] / count);
      pixel[1] = (uint8_t)(sum[1] / count);
      pixel[2] = (uint8_t)(sum[2] / count);
      pixel[3] = 255u;
    }
  }
  *out_height = h;
  return out;
}

static void ops_image_dot(uint8_t *pixels, uint32_t width, uint32_t height,
                          int32_t x, int32_t y, const uint8_t color[3]) {
  if (x < 0 || y < 0 || x >= (int32_t)width || y >= (int32_t)height) {
    return;
  }
  uint8_t *pixel = pixels + ((size_t)y * width + (uint32_t)x) * 4u;
  pixel[0] = color[0];
  pixel[1] = color[1];
  pixel[2] = color[2];
}

/* The characters marks draw, each in a 3 by 5 grid, rows from the top,
   three bits a row; lower case draws as capitals and others as blanks. */
static const char s_ops_glyph_chars[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-./_: ";
static const uint16_t s_ops_glyphs[] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7292, 0x7BEF,
    0x7BCF, 0x2BED, 0x6BAE, 0x3923, 0x6B6E, 0x79A7, 0x79A4, 0x396B, 0x5BED,
    0x7497, 0x126A, 0x5BAD, 0x4927, 0x5FED, 0x6B6D, 0x7B6F, 0x6BA4, 0x2B73,
    0x6BAD, 0x388E, 0x7492, 0x5B6F, 0x5B6A, 0x5BFD, 0x5AAD, 0x5A92, 0x72A7,
    0x01C0, 0x0002, 0x12A4, 0x0007, 0x0410, 0x0000,
};

static uint16_t ops_glyph(char c) {
  if (c >= 'a' && c <= 'z') {
    c = (char)(c - 'a' + 'A');
  }
  const char *at = c ? strchr(s_ops_glyph_chars, c) : NULL;
  return at ? s_ops_glyphs[at - s_ops_glyph_chars] : 0u;
}

/* A mark at (x, y): a cross in a ring and `text` (its number and label)
   beside it, in `ink` outlined in black so it reads on any scene. */
static void ops_image_mark(uint8_t *pixels, uint32_t width, uint32_t height,
                           int32_t x, int32_t y, const char *text,
                           const uint8_t ink[3]) {
  static const uint8_t edge[3] = {0u, 0u, 0u};
  for (uint32_t pass = 0; pass < 2u; ++pass) {
    const uint8_t *color = pass ? ink : edge;
    const int32_t grow = pass ? 0 : 1;
    for (int32_t d = -7 - grow; d <= 7 + grow; ++d) {
      for (int32_t t = -grow; t <= grow; ++t) {
        ops_image_dot(pixels, width, height, x + d, y + t, color);
        ops_image_dot(pixels, width, height, x + t, y + d, color);
      }
    }
    for (int32_t a = 0; a < 48; ++a) {
      const float32_t angle = (float32_t)a * (6.2831853f / 48.0f);
      const int32_t rx = (int32_t)lroundf(cosf(angle) * 5.0f);
      const int32_t ry = (int32_t)lroundf(sinf(angle) * 5.0f);
      for (int32_t t = -grow; t <= grow; ++t) {
        ops_image_dot(pixels, width, height, x + rx + t, y + ry, color);
        ops_image_dot(pixels, width, height, x + rx, y + ry + t, color);
      }
    }
    /* The text, two pixels a grid cell, up and to the right. */
    int32_t left = x + 9;
    for (const char *c = text; *c; ++c, left += 8) {
      const uint16_t glyph = ops_glyph(*c);
      for (int32_t row = 0; row < 5; ++row) {
        for (int32_t column = 0; column < 3; ++column) {
          if (!(glyph & (1u << (14 - row * 3 - column)))) {
            continue;
          }
          for (int32_t py = -grow; py < 2 + grow; ++py) {
            for (int32_t px = -grow; px < 2 + grow; ++px) {
              ops_image_dot(pixels, width, height, left + column * 2 + px,
                            y - 14 + row * 2 + py, color);
            }
          }
        }
      }
    }
  }
}

/* Writes RGBA8 `pixels` as the next capture PNG in the private directory,
   keeping the newest OPS_CAPTURE_KEEP. */
static bool8_t ops_capture_png(OpsContext *ctx, OpsPendingCapture *pending,
                               const uint8_t *pixels, uint32_t width,
                               uint32_t height) {
  bool8_t written = false_v;
  char captures[300];
  if (ops_private_directory("captures", captures, sizeof(captures))) {
    const int pid = (int)vkr_platform_get_process_id();
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
  if (!written) {
    return ops_fail(ctx, OPS_CAPTURE, "The capture PNG could not be written");
  }
  pending->width = width;
  pending->height = height;
  return true_v;
}

// -----------------------------------------------------------------------------
// Level checks (ADR-084)
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

/* Time a level check samples in one build: a windowed editor keeps its
   frames smooth, and a headless one has no one watching. */
#define OPS_LEVEL_STEP_SECONDS 0.004
#define OPS_LEVEL_STEP_HEADLESS_SECONDS 0.050

/* The arguments level.lint and level.map share: a `region` box with volume,
   its container, the capsule and the walk's start (`start`, else the first
   enabled Player Start). */
typedef struct OpsLevelArgs {
  Vec3 min;
  Vec3 max;
  const VkrScene *scene;
  uint16_t container;
  VkrEditorLevelCapsule capsule;
  Vec3 start;
  bool8_t has_start;
} OpsLevelArgs;

/* A level check across builds: its arguments, the scene it samples and
   what the finished check needs. */
typedef struct OpsPendingLevel {
  OpsLevelArgs level;
  uint64_t generation;
  uint32_t limit;
  /* level.map */
  float64_t cell;
  float32_t edge;
  uint32_t columns;
  uint32_t rows;
  char *text;
  float32_t *heights;
  /* query.reachable */
  Vec3 from;
  Vec3 to;
} OpsPendingLevel;

static void ops_level_end(VkrEditorOps *ops) {
  vkr_editor_level_job_end(ops->level_job);
  ops->level_job = NULL;
}

/* Starts the request's job over [min, max]. */
static bool8_t ops_level_begin(OpsContext *ctx, OpsPendingLevel *pending,
                               Vec3 min, Vec3 max, float32_t cell) {
  ops_level_end(ctx->ops);
  ctx->ops->level_job =
      vkr_editor_level_job_begin(min, max, &pending->level.capsule, cell);
  pending->generation = ctx->frame->scene_generation;
  ctx->call->state = pending;
  ctx->call->stage = 1u;
  return ctx->ops->level_job
             ? true_v
             : ops_fail(ctx, OPS_LIMIT, "Out of memory for the check's grid");
}

typedef enum OpsLevelStep {
  OPS_LEVEL_FAILED = 0,
  OPS_LEVEL_WAITING,
  OPS_LEVEL_SAMPLED,
} OpsLevelStep;

/* Samples the job for this build's share; fails when its scene left. */
static OpsLevelStep ops_level_step(OpsContext *ctx, OpsPendingLevel *pending) {
  const VkrScene *scene = ops_scene(ctx->frame, pending->level.container);
  if (!ctx->ops->level_job || scene != pending->level.scene ||
      ctx->frame->scene_generation != pending->generation) {
    ops_fail(ctx, OPS_BUSY, "The scene changed during the check; run it again");
    ops_level_end(ctx->ops);
    return OPS_LEVEL_FAILED;
  }
  const float64_t seconds = ctx->editor->headless
                                ? OPS_LEVEL_STEP_HEADLESS_SECONDS
                                : OPS_LEVEL_STEP_SECONDS;
  return vkr_editor_level_job_step(ctx->ops->level_job, scene, seconds)
             ? OPS_LEVEL_SAMPLED
             : OPS_LEVEL_WAITING;
}

static bool8_t ops_level_args(OpsContext *ctx, OpsLevelArgs *out) {
  const VkrBakeryJson *args = ctx->call->args;
  const VkrBakeryJson *region = vkr_bakery_json_get(args, "region");
  bool8_t has_min = false_v;
  bool8_t has_max = false_v;
  uint16_t container = 0u;
  *out = (OpsLevelArgs){0};
  if (!region || region->type != VKR_BAKERY_JSON_OBJECT ||
      !ops_arg_vec3(ctx, region, "min", &out->min, &has_min) ||
      !ops_arg_vec3(ctx, region, "max", &out->max, &has_max) || !has_min ||
      !has_max || out->max.x <= out->min.x || out->max.y <= out->min.y ||
      out->max.z <= out->min.z) {
    return ctx->call->error_code
               ? false_v
               : ops_fail(ctx, OPS_INVALID,
                          "'region' is {\"min\": [..], \"max\": [..]}, a box "
                          "with volume");
  }
  if (!ops_arg_container(ctx, args, &container) ||
      !ops_arg_capsule(ctx, args, &out->capsule) ||
      !ops_arg_vec3(ctx, args, "start", &out->start, &out->has_start)) {
    return false_v;
  }
  out->scene = ops_scene(ctx->frame, container);
  out->container = container;
  if (!out->has_start) {
    out->has_start = ops_player_start(out->scene, &out->start);
  }
  return true_v;
}

static VkrEditorOpStatus ops_run_lint(OpsContext *ctx) {
  OpsPendingLevel *pending = ctx->call->state;
  if (!ctx->call->stage) {
    pending =
        arena_alloc(ops_arena(ctx), sizeof(*pending), ARENA_MEMORY_TAG_STRUCT);
    if (!pending) {
      ops_fail(ctx, OPS_LIMIT, "Out of request memory");
      return VKR_EDITOR_OP_DONE;
    }
    MemZero(pending, sizeof(*pending));
    float64_t limit = 100.0;
    (void)ops_arg_number(ctx->call->args, "limit", &limit);
    pending->limit = (uint32_t)vkr_clamp_f64(limit, 1.0, 500.0);
    if (!ops_level_args(ctx, &pending->level) ||
        !ops_level_begin(ctx, pending, pending->level.min, pending->level.max,
                         0.0f)) {
      return VKR_EDITOR_OP_DONE;
    }
  }
  const OpsLevelStep step = ops_level_step(ctx, pending);
  if (step != OPS_LEVEL_SAMPLED) {
    return step == OPS_LEVEL_WAITING ? VKR_EDITOR_OP_WAIT : VKR_EDITOR_OP_DONE;
  }
  const OpsLevelArgs level = pending->level;
  const VkrScene *scene = level.scene;
  const Vec3 start = level.start;
  const bool8_t has_start = level.has_start;
  const uint32_t capacity = pending->limit;
  VkrEditorLevelIssue *issues = arena_alloc(
      ops_arena(ctx), capacity * sizeof(*issues), ARENA_MEMORY_TAG_STRUCT);
  if (!issues) {
    ops_level_end(ctx->ops);
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  VkrEditorLevelStats stats = {0};
  const uint32_t found = vkr_editor_level_job_lint(ctx->ops->level_job, scene,
                                                   has_start ? &start : NULL,
                                                   issues, capacity, &stats);
  ops_level_end(ctx->ops);
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

/* Cells along a level map's longer side without `cell`, and the columns
   and rows one answers with at most. */
#define OPS_MAP_SIDE_DEFAULT 96u
#define OPS_MAP_SIDE_MAX 200u

/* level.map: the region's floors as rows of characters, a floor plan an
   agent reads as text. */
static VkrEditorOpStatus ops_run_map(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  Arena *arena = ops_arena(ctx);
  OpsPendingLevel *pending = ctx->call->state;
  if (!ctx->call->stage) {
    pending = arena_alloc(arena, sizeof(*pending), ARENA_MEMORY_TAG_STRUCT);
    if (!pending) {
      ops_fail(ctx, OPS_LIMIT, "Out of request memory");
      return VKR_EDITOR_OP_DONE;
    }
    MemZero(pending, sizeof(*pending));
    if (!ops_level_args(ctx, &pending->level)) {
      return VKR_EDITOR_OP_DONE;
    }
    const OpsLevelArgs *level = &pending->level;
    pending->cell = (float64_t)Max(level->max.x - level->min.x,
                                   level->max.z - level->min.z) /
                    (float64_t)OPS_MAP_SIDE_DEFAULT;
    (void)ops_arg_number(args, "cell", &pending->cell);
    if (!(pending->cell > 0.0)) {
      ops_fail(ctx, OPS_INVALID, "'cell' must be positive");
      return VKR_EDITOR_OP_DONE;
    }
    pending->edge = vkr_editor_level_map_size(
        level->min, level->max, &level->capsule, (float32_t)pending->cell,
        &pending->columns, &pending->rows);
    if (pending->columns > OPS_MAP_SIDE_MAX ||
        pending->rows > OPS_MAP_SIDE_MAX) {
      ops_fail(ctx, OPS_LIMIT,
               "A map of %u by %u cells of %.2f m passes %u a side; raise "
               "'cell' or split the region",
               pending->columns, pending->rows, (float64_t)pending->edge,
               OPS_MAP_SIDE_MAX);
      return VKR_EDITOR_OP_DONE;
    }
    const uint32_t cells = pending->columns * pending->rows;
    pending->text = arena_alloc(arena, cells, ARENA_MEMORY_TAG_STRING);
    pending->heights = ops_arg_bool(args, "heights", false_v)
                           ? arena_alloc(arena, sizeof(float32_t) * cells,
                                         ARENA_MEMORY_TAG_ARRAY)
                           : NULL;
    if (!pending->text ||
        (ops_arg_bool(args, "heights", false_v) && !pending->heights)) {
      ops_fail(ctx, OPS_LIMIT, "Out of request memory");
      return VKR_EDITOR_OP_DONE;
    }
    if (!ops_level_begin(ctx, pending, level->min, level->max,
                         (float32_t)pending->cell)) {
      return VKR_EDITOR_OP_DONE;
    }
  }
  const OpsLevelStep step = ops_level_step(ctx, pending);
  if (step != OPS_LEVEL_SAMPLED) {
    return step == OPS_LEVEL_WAITING ? VKR_EDITOR_OP_WAIT : VKR_EDITOR_OP_DONE;
  }
  const OpsLevelArgs level = pending->level;
  const uint32_t columns = pending->columns;
  const uint32_t rows = pending->rows;
  const float32_t edge = pending->edge;
  const uint32_t cells = columns * rows;
  char *text = pending->text;
  float32_t *heights = pending->heights;
  VkrEditorLevelStats stats = {0};
  bool8_t start_found = false_v;
  const bool8_t mapped = vkr_editor_level_job_map(
      ctx->ops->level_job, level.scene, level.has_start ? &level.start : NULL,
      text, heights, cells, &stats, &start_found);
  ops_level_end(ctx->ops);
  if (!mapped) {
    ops_fail(ctx, OPS_LIMIT, "The map's grid outgrew its text");
    return VKR_EDITOR_OP_DONE;
  }
  VkrBakeryJson *lines = vkr_bakery_json_array(arena);
  VkrBakeryJson *floors = vkr_bakery_json_array(arena);
  for (uint32_t z = 0; z < rows; ++z) {
    const String8 line = {.str = (uint8_t *)text + (size_t)z * columns,
                          .length = columns};
    vkr_bakery_json_append(lines, ops_string(ctx, line));
    if (!heights) {
      continue;
    }
    VkrBakeryJson *row = vkr_bakery_json_array(arena);
    for (uint32_t x = 0; x < columns; ++x) {
      const float32_t y = heights[(size_t)z * columns + x];
      vkr_bakery_json_append(
          row, isfinite(y)
                   ? ops_number(ctx, floor((float64_t)y * 100.0 + 0.5) / 100.0)
                   : vkr_bakery_json_null(arena));
    }
    vkr_bakery_json_append(floors, row);
  }
  VkrBakeryJson *first = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(first, ops_number(ctx, level.min.x + 0.5f * edge));
  vkr_bakery_json_append(first, ops_number(ctx, level.min.z + 0.5f * edge));
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  ops_set(ctx, result, "cell", ops_number(ctx, edge));
  ops_set(ctx, result, "first", first);
  ops_set(ctx, result, "columns", vkr_bakery_json_int(arena, columns));
  ops_set(ctx, result, "rows", lines);
  ops_set(ctx, result, "legend",
          vkr_bakery_json_cstr(
              arena, "'.' walkable; ',' walkable, out of reach of the start; "
                     "'S' start; '#' too close to a wall; 'n' gap too "
                     "narrow; '_' ceiling too low; '/' too steep; '-' no "
                     "floor"));
  ops_set(ctx, result, "walkable", vkr_bakery_json_int(arena, stats.walkable));
  if (start_found) {
    ops_set(ctx, result, "start", ops_vec3(ctx, level.start));
    ops_set(ctx, result, "reachable",
            vkr_bakery_json_int(arena, stats.reachable));
  }
  if (heights) {
    ops_set(ctx, result, "heights", floors);
  }
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_reachable(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  OpsPendingLevel *pending = ctx->call->state;
  if (!ctx->call->stage) {
    pending =
        arena_alloc(ops_arena(ctx), sizeof(*pending), ARENA_MEMORY_TAG_STRUCT);
    if (!pending) {
      ops_fail(ctx, OPS_LIMIT, "Out of request memory");
      return VKR_EDITOR_OP_DONE;
    }
    MemZero(pending, sizeof(*pending));
    bool8_t has_from = false_v;
    bool8_t has_to = false_v;
    if (!ops_arg_vec3(ctx, args, "from", &pending->from, &has_from) ||
        !ops_arg_vec3(ctx, args, "to", &pending->to, &has_to) ||
        !ops_arg_container(ctx, args, &pending->level.container) ||
        !ops_arg_capsule(ctx, args, &pending->level.capsule)) {
      return VKR_EDITOR_OP_DONE;
    }
    if (!has_from || !has_to) {
      ops_fail(ctx, OPS_INVALID,
               "query.reachable needs floor points 'from' and 'to'");
      return VKR_EDITOR_OP_DONE;
    }
    pending->level.scene = ops_scene(ctx->frame, pending->level.container);
    Vec3 min = {0};
    Vec3 max = {0};
    vkr_editor_level_reachable_region(pending->from, pending->to, &min, &max);
    if (!ops_level_begin(ctx, pending, min, max, 0.0f)) {
      return VKR_EDITOR_OP_DONE;
    }
  }
  const OpsLevelStep step = ops_level_step(ctx, pending);
  if (step != OPS_LEVEL_SAMPLED) {
    return step == OPS_LEVEL_WAITING ? VKR_EDITOR_OP_WAIT : VKR_EDITOR_OP_DONE;
  }
  Vec3 path[512];
  uint32_t count = 0u;
  float32_t length = 0.0f;
  const bool8_t reached = vkr_editor_level_job_reachable(
      ctx->ops->level_job, pending->level.scene, pending->from, pending->to,
      path, ArrayCount(path), &count, &length);
  ops_level_end(ctx->ops);
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
  bool8_t glide = false_v;
  if (vkr_bakery_json_get_bool(ctx->call->args, "glide", &glide) && glide) {
    /* Flight, as the free camera moves: no lens change and no cut. */
    request->move_camera = true_v;
    request->camera_position = eye;
    request->camera_pitch =
        asinf(vkr_clamp_f32(d.y, -1.0f, 1.0f)) * 57.29577951f;
    request->camera_yaw = atan2f(d.z, d.x) * 57.29577951f;
    ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
    ops_set(ctx, ctx->call->result, "eye", ops_vec3(ctx, eye));
    ops_set(ctx, ctx->call->result, "target", ops_vec3(ctx, target));
    return VKR_EDITOR_OP_DONE;
  }
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
  /* A large world's horizon needs a farther plane. */
  float64_t far_plane = 0.0;
  if (ops_arg_number(ctx->call->args, "far", &far_plane)) {
    if (!(far_plane > (float64_t)request->recall.near_plane) ||
        far_plane > 100000.0) {
      ops_fail(ctx, OPS_INVALID,
               "'far' must lie past the near plane, at most 100000 m");
      return VKR_EDITOR_OP_DONE;
    }
    request->recall.far_plane = (float32_t)far_plane;
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

/* One view of a capture from `spec` (the arguments, or one entry of
   `views`): a camera view, framing, eye and target, and grid labels. */
static bool8_t ops_capture_view_parse(OpsContext *ctx,
                                      const VkrBakeryJson *spec,
                                      OpsCaptureView *out) {
  const VkrSampleUiFrame *frame = ctx->frame;
  MemZero(out, sizeof(*out));
  VkrSampleViewState next = frame->view_state;
  String8 view = {0};
  snprintf(out->name, sizeof(out->name), "current");
  if (vkr_bakery_json_get_string(spec, "view", &view) &&
      !ops_equals(view, "current")) {
    if (!ops_capture_view(ctx, view, &next.camera_view)) {
      return false_v;
    }
    snprintf(out->name, sizeof(out->name), "%.*s",
             (int)Min(view.length, (uint64_t)15u), (const char *)view.str);
  }
  bool8_t labels = false_v;
  if (vkr_bakery_json_get_bool(spec, "grid_labels", &labels)) {
    next.grid_labels = labels;
    next.grid_enabled = next.grid_enabled || labels;
  }
  out->request = (VkrSampleViewRequest){.value = next, .apply = true_v};
  const VkrBakeryJson *focus = vkr_bakery_json_get(spec, "focus");
  if (focus && focus->type == VKR_BAKERY_JSON_OBJECT) {
    if (!ops_arg_vec3(ctx, focus, "min", &out->request.frame_min, NULL) ||
        !ops_arg_vec3(ctx, focus, "max", &out->request.frame_max, NULL)) {
      return false_v;
    }
    out->request.frame_box = true_v;
  } else if (focus) {
    OpsRef ref;
    if (!ops_ref(ctx, NULL, focus, "focus", &ref)) {
      return false_v;
    }
    if (!ops_world_bounds(ops_scene(frame, ref.container), ref.entity,
                          &out->request.frame_min, &out->request.frame_max)) {
      const SceneTransform *transform =
          ops_transform(ops_scene(frame, ref.container), ref.entity);
      const Vec3 at = transform ? mat4_position(transform->world) : vec3_zero();
      out->request.frame_min = vec3_sub(at, vec3_one());
      out->request.frame_max = vec3_add(at, vec3_one());
    }
    out->request.frame_box = true_v;
  }
  /* An explicit eye and target place the perspective camera. */
  bool8_t has_target = false_v;
  if (!ops_arg_vec3(ctx, spec, "eye", &out->eye, &out->has_eye) ||
      !ops_arg_vec3(ctx, spec, "target", &out->target, &has_target)) {
    return false_v;
  }
  if (out->has_eye != has_target ||
      (out->has_eye &&
       vec3_length(vec3_sub(out->target, out->eye)) < 1.0e-3f)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'eye' and 'target' come together and differ");
  }
  if (out->has_eye) {
    out->request.value.camera_view = VKR_SAMPLE_CAMERA_PERSPECTIVE;
    out->request.frame_box = false_v;
    snprintf(out->name, sizeof(out->name), "eye");
  }
  return true_v;
}

typedef enum OpsIdle {
  OPS_IDLE_READY = 0,
  OPS_IDLE_WAITING,
  OPS_IDLE_FAILED,
} OpsIdle;

/* A windowed editor captures only while the designer leaves it alone, so
   an agent never moves the camera under their hands. A headless editor has
   no one to wait for. */
static OpsIdle ops_capture_idle(OpsContext *ctx) {
  if (ctx->editor->headless) {
    return OPS_IDLE_READY;
  }
  const float64_t now = vkr_platform_get_absolute_time();
  if (ctx->call->idle_since <= 0.0) {
    ctx->call->idle_since = now;
  }
  if (now - ctx->ops->input_last >= OPS_CAPTURE_IDLE_SECONDS) {
    return OPS_IDLE_READY;
  }
  /* A request that waited its time outside the queue does not wait again. */
  if (ctx->call->waited ||
      now - ctx->call->idle_since >= OPS_CAPTURE_IDLE_LIMIT_SECONDS) {
    ops_fail(ctx, OPS_BUSY,
             "The designer kept working in the editor for %.0f s; capture "
             "again later, or in a headless editor",
             OPS_CAPTURE_IDLE_LIMIT_SECONDS);
    return OPS_IDLE_FAILED;
  }
  return OPS_IDLE_WAITING;
}

/* Restores the view the capture found. */
static void ops_capture_restore(const VkrSampleUiFrame *frame,
                                const OpsPendingCapture *pending) {
  if (!pending || !pending->restore) {
    return;
  }
  *frame->view_request =
      (VkrSampleViewRequest){.value = pending->saved_view, .apply = true_v};
  if (frame->editor_state_request) {
    frame->editor_state_request->apply_recall = true_v;
    frame->editor_state_request->recall = pending->saved_recall;
    frame->editor_state_request->recall.selection_valid = false_v;
  }
}

/* Shows `view` from the next build. */
static void ops_capture_view_apply(const VkrSampleUiFrame *frame,
                                   const OpsCaptureView *view) {
  *frame->view_request = view->request;
  if (view->has_eye && frame->editor_state_request) {
    const Vec3 d = vec3_normalize(vec3_sub(view->target, view->eye));
    VkrSampleEditorStateRequest *state_request = frame->editor_state_request;
    state_request->apply_recall = true_v;
    state_request->recall = frame->scene_recall;
    state_request->recall.camera_valid = true_v;
    state_request->recall.position = view->eye;
    state_request->recall.pitch =
        asinf(vkr_clamp_f32(d.y, -1.0f, 1.0f)) * 57.29577951f;
    state_request->recall.yaw = atan2f(d.z, d.x) * 57.29577951f;
    state_request->recall.selection_valid = false_v;
    if (state_request->recall.field_of_view <= 0.0f) {
      state_request->recall.field_of_view = 60.0f;
      state_request->recall.near_plane = 0.1f;
      state_request->recall.far_plane = 1000.0f;
    }
  }
}

/* Whether collision lies between the camera and the mark at `point`,
   whose clip position `clip` the view's matrix gave: a ray from the near
   plane under the mark (depth 0, mat4_perspective and mat4_ortho_zo_yinv)
   to the mark. Geometry without collision hides nothing. */
static bool8_t ops_capture_hidden(const VkrSampleUiFrame *frame,
                                  Mat4 view_projection, Vec4 clip, Vec3 point) {
  const Vec4 near_point =
      mat4_mul_vec4(mat4_inverse(view_projection),
                    (Vec4){clip.x / clip.w, clip.y / clip.w, 0.0f, 1.0f});
  if (!isfinite(near_point.w) || fabsf(near_point.w) < 1.0e-9f) {
    return false_v;
  }
  const Vec3 origin =
      vec3_new(near_point.x / near_point.w, near_point.y / near_point.w,
               near_point.z / near_point.w);
  const Vec3 displacement = vec3_sub(point, origin);
  const float32_t length = vec3_length(displacement);
  if (!isfinite(length) || length < 1.0e-3f) {
    return false_v;
  }
  /* A mark on a surface stays visible; the length term covers float
     error on the long rays of an orthographic view. */
  const float32_t slack = 0.05f + 1.0e-4f * length;
  const VkrScene *scenes[2] = {frame->scene, frame->world};
  for (uint32_t i = 0; i < ArrayCount(scenes); ++i) {
    VkrPhysicsQueryFilter filter = {.mask = UINT16_MAX};
    VkrPhysicsRayHit hit = {0};
    /* Physics queries take a mutable scene but change none of its state. */
    if (scenes[i] &&
        vkr_scene_physics_raycast_query((VkrScene *)scenes[i], origin,
                                        displacement, &filter, &hit) &&
        hit.fraction * length < length - slack) {
      return true_v;
    }
  }
  return false_v;
}

/* Keeps the captured frame of the current view at its cell size, with
   its marks drawn, and frees the full frame. */
static bool8_t ops_capture_keep(OpsContext *ctx, OpsPendingCapture *pending,
                                const VkrCaptureItemResult *item) {
  OpsCaptureView *view = &pending->views[pending->view_index];
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint8_t *full = ops_capture_rgba(ctx, pending, item, &width, &height);
  if (!full) {
    return false_v;
  }
  /* A sheet spreads the wanted width over two columns. */
  const uint32_t columns = pending->view_count > 1u ? 2u : 1u;
  const uint32_t sheet = pending->max_width ? pending->max_width : width;
  const uint32_t cell =
      Max(16u, (sheet - OPS_CAPTURE_GAP * (columns - 1u)) / columns);
  view->pixels = ops_image_shrink(ops_arena(ctx), full, width, height, cell,
                                  &view->height);
  view->width = Min(cell, width);
  free(full);
  if (!view->pixels) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory for the capture");
  }
  /* Marks through the captured camera, in this cell's pixels. A window
     capture places the Scene's image where the window shows it. */
  const Vec4 image = ctx->frame->mapping.image_rect_px;
  const float32_t scale = (float32_t)view->width / (float32_t)width;
  for (uint32_t i = 0; i < pending->mark_count; ++i) {
    const Vec4 clip = mat4_mul_vec4(view->view_projection,
                                    vec3_to_vec4(pending->marks[i], 1.0f));
    if (!isfinite(clip.w) || clip.w <= 1.0e-6f) {
      continue;
    }
    Vec2 at = {(clip.x / clip.w * 0.5f + 0.5f) * image.z,
               (clip.y / clip.w * 0.5f + 0.5f) * image.w};
    if (pending->window) {
      at = (Vec2){at.x + image.x, at.y + image.y};
    }
    at = (Vec2){at.x * scale, at.y * scale};
    if (!isfinite(at.x) || !isfinite(at.y) || at.x < 0.0f || at.y < 0.0f ||
        at.x >= (float32_t)view->width || at.y >= (float32_t)view->height) {
      continue;
    }
    view->marks[i] = at;
    view->mark_shown[i] = true_v;
    view->mark_hidden[i] = ops_capture_hidden(ctx->frame, view->view_projection,
                                              clip, pending->marks[i]);
    /* Magenta where the camera sees the mark, blue behind collision. */
    static const uint8_t seen[3] = {255u, 40u, 210u};
    static const uint8_t behind[3] = {70u, 170u, 255u};
    char text[24];
    snprintf(text, sizeof(text), "%u%s%s", i + 1u,
             pending->mark_labels[i][0] ? " " : "", pending->mark_labels[i]);
    ops_image_mark(view->pixels, view->width, view->height,
                   (int32_t)lroundf(at.x), (int32_t)lroundf(at.y), text,
                   view->mark_hidden[i] ? behind : seen);
  }
  return true_v;
}

/* Writes the capture: one view as it is, several as a sheet of two
   columns. Each view's place joins the answer when the agent asked for
   views or marks. */
static bool8_t ops_capture_finish(OpsContext *ctx, OpsPendingCapture *pending) {
  const uint32_t columns = pending->view_count > 1u ? 2u : 1u;
  const uint32_t rows = (pending->view_count + columns - 1u) / columns;
  uint32_t cell_w = 0u;
  uint32_t cell_h = 0u;
  for (uint32_t i = 0; i < pending->view_count; ++i) {
    cell_w = Max(cell_w, pending->views[i].width);
    cell_h = Max(cell_h, pending->views[i].height);
  }
  const uint32_t width = columns * cell_w + (columns - 1u) * OPS_CAPTURE_GAP;
  const uint32_t height = rows * cell_h + (rows - 1u) * OPS_CAPTURE_GAP;
  uint8_t *sheet = pending->views[0].pixels;
  if (pending->view_count > 1u) {
    sheet = arena_alloc(ops_arena(ctx), (uint64_t)width * height * 4u,
                        ARENA_MEMORY_TAG_ARRAY);
    if (!sheet) {
      return ops_fail(ctx, OPS_LIMIT, "Out of request memory for the sheet");
    }
    for (uint64_t i = 0; i < (uint64_t)width * height; ++i) {
      sheet[i * 4u + 0u] = 24u;
      sheet[i * 4u + 1u] = 24u;
      sheet[i * 4u + 2u] = 27u;
      sheet[i * 4u + 3u] = 255u;
    }
  }
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *views = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < pending->view_count; ++i) {
    const OpsCaptureView *view = &pending->views[i];
    const uint32_t x = (i % columns) * (cell_w + OPS_CAPTURE_GAP);
    const uint32_t y = (i / columns) * (cell_h + OPS_CAPTURE_GAP);
    for (uint32_t row = 0; sheet != view->pixels && row < view->height; ++row) {
      MemCopy(sheet + ((size_t)(y + row) * width + x) * 4u,
              view->pixels + (size_t)row * view->width * 4u,
              (size_t)view->width * 4u);
    }
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    ops_set(ctx, entry, "view", vkr_bakery_json_cstr(arena, view->name));
    ops_set(ctx, entry, "x", vkr_bakery_json_int(arena, x));
    ops_set(ctx, entry, "y", vkr_bakery_json_int(arena, y));
    ops_set(ctx, entry, "width", vkr_bakery_json_int(arena, view->width));
    ops_set(ctx, entry, "height", vkr_bakery_json_int(arena, view->height));
    if (pending->mark_count) {
      VkrBakeryJson *marks = vkr_bakery_json_array(arena);
      for (uint32_t m = 0; m < pending->mark_count; ++m) {
        if (!view->mark_shown[m]) {
          vkr_bakery_json_append(marks, vkr_bakery_json_null(arena));
          continue;
        }
        VkrBakeryJson *mark = vkr_bakery_json_object(arena);
        VkrBakeryJson *at = vkr_bakery_json_array(arena);
        vkr_bakery_json_append(
            at,
            vkr_bakery_json_int(arena, x + (int64_t)lroundf(view->marks[m].x)));
        vkr_bakery_json_append(
            at,
            vkr_bakery_json_int(arena, y + (int64_t)lroundf(view->marks[m].y)));
        ops_set(ctx, mark, "at", at);
        ops_set(ctx, mark, "hidden",
                vkr_bakery_json_bool(arena, view->mark_hidden[m]));
        vkr_bakery_json_append(marks, mark);
      }
      ops_set(ctx, entry, "marks", marks);
    }
    vkr_bakery_json_append(views, entry);
  }
  if (!ops_capture_png(ctx, pending, sheet, width, height)) {
    return false_v;
  }
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "path",
          vkr_bakery_json_cstr(arena, pending->path));
  ops_set(ctx, ctx->call->result, "width", vkr_bakery_json_int(arena, width));
  ops_set(ctx, ctx->call->result, "height", vkr_bakery_json_int(arena, height));
  if (pending->describe_views) {
    ops_set(ctx, ctx->call->result, "views", views);
  }
  return true_v;
}

/* view.capture: one view or a sheet of up to four, at most `max_width`
   wide, with numbered `marks` at world points. Each view switches the
   Scene's view, waits OPS_CAPTURE_SETTLE_FRAMES builds and for the scene to
   settle, then asks for one frame; the end restores the view. */
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
    const OpsIdle idle = ops_capture_idle(ctx);
    if (idle != OPS_IDLE_READY) {
      return idle == OPS_IDLE_WAITING ? VKR_EDITOR_OP_WAIT : VKR_EDITOR_OP_DONE;
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
    float64_t max_width = 0.0;
    if (ops_arg_number(call->args, "max_width", &max_width)) {
      if (!(max_width >= 64.0 && max_width <= 8192.0)) {
        ops_fail(ctx, OPS_INVALID, "'max_width' is 64 to 8192 pixels");
        return VKR_EDITOR_OP_DONE;
      }
      pending->max_width = (uint32_t)max_width;
    }
    const VkrBakeryJson *marks = vkr_bakery_json_get(call->args, "marks");
    if (marks && (marks->type != VKR_BAKERY_JSON_ARRAY ||
                  marks->count > OPS_CAPTURE_MARKS_MAX)) {
      ops_fail(ctx, OPS_INVALID, "'marks' is up to %u world points [x, y, z]",
               OPS_CAPTURE_MARKS_MAX);
      return VKR_EDITOR_OP_DONE;
    }
    for (const VkrBakeryJson *mark = marks ? marks->first : NULL; mark;
         mark = mark->next) {
      /* A mark is a point, or {"point": [x, y, z], "label": "door"}. */
      const VkrBakeryJson *point = mark;
      String8 label = {0};
      if (mark->type == VKR_BAKERY_JSON_OBJECT) {
        point = vkr_bakery_json_get(mark, "point");
        (void)vkr_bakery_json_get_string(mark, "label", &label);
      }
      snprintf(pending->mark_labels[pending->mark_count],
               sizeof(pending->mark_labels[0]), "%.*s",
               (int)Min(label.length, (uint64_t)15u), (const char *)label.str);
      float32_t xyz[3] = {0};
      uint32_t axis = 0u;
      for (const VkrBakeryJson *part =
               point && point->type == VKR_BAKERY_JSON_ARRAY &&
                       point->count == 3u
                   ? point->first
                   : NULL;
           part; part = part->next, ++axis) {
        xyz[axis] = part->type == VKR_BAKERY_JSON_INT ? (float32_t)part->integer
                    : part->type == VKR_BAKERY_JSON_FLOAT
                        ? (float32_t)part->number
                        : NAN;
      }
      if (axis != 3u || !isfinite(xyz[0]) || !isfinite(xyz[1]) ||
          !isfinite(xyz[2])) {
        ops_fail(ctx, OPS_INVALID,
                 "Each mark is a world point [x, y, z] or {\"point\": [x, y, "
                 "z], \"label\": \"text\"}");
        return VKR_EDITOR_OP_DONE;
      }
      pending->marks[pending->mark_count++] = vec3_new(xyz[0], xyz[1], xyz[2]);
    }
    const VkrBakeryJson *views = vkr_bakery_json_get(call->args, "views");
    if (views) {
      if (views->type != VKR_BAKERY_JSON_ARRAY || !views->count ||
          views->count > OPS_CAPTURE_VIEWS_MAX) {
        ops_fail(ctx, OPS_INVALID, "'views' is 1 to %u view objects",
                 OPS_CAPTURE_VIEWS_MAX);
        return VKR_EDITOR_OP_DONE;
      }
      for (const VkrBakeryJson *spec = views->first; spec; spec = spec->next) {
        if (spec->type != VKR_BAKERY_JSON_OBJECT ||
            !ops_capture_view_parse(ctx, spec,
                                    &pending->views[pending->view_count])) {
          if (!call->error_code) {
            ops_fail(ctx, OPS_INVALID, "Each view is an object");
          }
          return VKR_EDITOR_OP_DONE;
        }
        pending->view_count++;
      }
    } else {
      if (!ops_capture_view_parse(ctx, call->args, &pending->views[0])) {
        return VKR_EDITOR_OP_DONE;
      }
      pending->view_count = 1u;
    }
    pending->describe_views = views || pending->mark_count;
    const OpsCaptureView *first = &pending->views[0];
    pending->restore =
        pending->view_count > 1u || first->has_eye ||
        first->request.frame_box ||
        first->request.value.camera_view != frame->view_state.camera_view ||
        first->request.value.grid_labels != frame->view_state.grid_labels ||
        first->request.value.grid_enabled != frame->view_state.grid_enabled;
    ops_capture_view_apply(frame, first);
    pending->view_since = vkr_platform_get_absolute_time();
    call->state = pending;
    call->stage = 1u;
    call->frames = 0u;
    return VKR_EDITOR_OP_WAIT;
  }
  case 1u: {
    /* The designer touched the editor while the view was switched: give
       their view back now and wait for them to stop again. */
    if (!ctx->editor->headless && ctx->ops->input_last > pending->view_since) {
      ops_capture_restore(frame, pending);
      call->stage = 0u;
      call->frames = 0u;
      call->idle_since = 0.0;
      call->waited = false_v;
      return VKR_EDITOR_OP_WAIT;
    }
    /* The new view can stream terrain tiles in, so the scene settles again
       before the frame is taken. */
    const bool8_t settled =
        !ops_arg_bool(call->args, "settle", true_v) || ops_settled(frame, NULL);
    if (++call->frames < OPS_CAPTURE_SETTLE_FRAMES ||
        (!settled && call->frames < OPS_SCENE_SETTLE_FRAMES)) {
      return VKR_EDITOR_OP_WAIT;
    }
    call->settle_timeout = call->settle_timeout || !settled;
    pending->views[pending->view_index].view_projection =
        frame->view_projection;
    call->token = ++ctx->ops->next_token;
    *frame->capture_request =
        (VkrSampleCaptureRequest){.request = true_v, .token = call->token};
    call->stage = 2u;
    call->frames = 0u;
    return VKR_EDITOR_OP_WAIT;
  }
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
    } else if (ops_capture_keep(ctx, pending, ready->item)) {
      if (++pending->view_index < pending->view_count) {
        ops_capture_view_apply(frame, &pending->views[pending->view_index]);
        pending->view_since = vkr_platform_get_absolute_time();
        call->stage = 1u;
        call->frames = 0u;
        return VKR_EDITOR_OP_WAIT;
      }
      (void)ops_capture_finish(ctx, pending);
    }
    call->stage = 3u;
  }
    /* fall through */
  default:
    ops_capture_restore(frame, pending);
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

/* Records the world rectangle a terrain edit may change, for claims; it
   maps the edit to the terrain's space as the journal does
   (vkr_scene_edit_terrain). */
static void ops_terrain_touch(OpsContext *ctx, OpsBatch *batch,
                              const OpsRef *ref, const VkrHeightfieldOp *op) {
  const VkrScene *scene = ops_scene(ctx->frame, ref->container);
  const VkrHeightfield *field =
      scene ? vkr_scene_terrain_field(scene, ref->entity) : NULL;
  Vec3 origin = {0};
  if (!field || batch->touched_count >= OPS_TOUCHED_MAX ||
      !vkr_scene_terrain_to_local(scene, ref->entity, vec3_zero(), &origin)) {
    return;
  }
  VkrHeightfieldOp local = *op;
  local.a = vec3_add(op->a, origin);
  local.b = vec3_add(op->b, origin);
  local.min = vec2_new(op->min.x + origin.x, op->min.y + origin.z);
  local.max = vec2_new(op->max.x + origin.x, op->max.y + origin.z);
  local.height = op->height + origin.y;
  VkrHeightfieldRect rect;
  if (!vkr_heightfield_op_rect(field, &local, &rect)) {
    return;
  }
  /* Sample (0, 0) lies at local (-size / 2, -size / 2); heights are open. */
  const float32_t half = 0.5f * field->spacing * (float32_t)field->cells;
  const uint32_t n = batch->touched_count++;
  batch->touched_min[n] =
      vec3_new((float32_t)rect.x0 * field->spacing - half - origin.x, -1.0e30f,
               (float32_t)rect.z0 * field->spacing - half - origin.z);
  batch->touched_max[n] =
      vec3_new((float32_t)rect.x1 * field->spacing - half - origin.x, 1.0e30f,
               (float32_t)rect.z1 * field->spacing - half - origin.z);
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
    ops_terrain_touch(ctx, batch, ref, op);
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
      (cells > VKR_HEIGHTFIELD_RESIDENT_CELLS &&
       fmod(cells, (float64_t)VKR_HEIGHTFIELD_STREAMED_CELLS) != 0.0) ||
      !(height_max > height_min) || height < height_min ||
      height > height_max || !(texture_size > 0.0)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'size' / 'spacing' must be a multiple of %u cells, at "
                    "most %u (above %u, a multiple of %u), with height_min "
                    "<= height <= height_max",
                    VKR_HEIGHTFIELD_TILE_CELLS, VKR_HEIGHTFIELD_CELLS_MAX,
                    VKR_HEIGHTFIELD_RESIDENT_CELLS,
                    VKR_HEIGHTFIELD_STREAMED_CELLS);
  }
  /* The file, or its staged field, comes first, so the component finds it
     when it appears. */
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
  /* A resident terrain waits in memory for its scene's save, so a
     discarded scene leaves no file; a streamed one is written now. */
  if (cells <= VKR_HEIGHTFIELD_RESIDENT_CELLS) {
    if (!vkr_scene_terrain_stage(absolute, (uint32_t)cells, (float32_t)spacing,
                                 (float32_t)height_min, (float32_t)height_max,
                                 (float32_t)height)) {
      return ops_fail(ctx, OPS_LIMIT,
                      "Save the scene before creating more terrains");
    }
  } else {
    VkrAllocator scratch = {.ctx = ops_arena(ctx)};
    vkr_allocator_arena(&scratch);
    const String8 directory_text =
        string8_create_from_cstr((const uint8_t *)directory, strlen(directory));
    char error[160] = {0};
    if (!file_ensure_directory(&scratch, &directory_text) ||
        !vkr_heightfield_create_file(absolute, (uint32_t)cells,
                                     (float32_t)spacing, (float32_t)height_min,
                                     (float32_t)height_max, (float32_t)height,
                                     error, sizeof(error))) {
      return ops_fail(ctx, OPS_REJECTED,
                      "The terrain file could not be made%s%s",
                      error[0] ? ": " : "", error);
    }
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
      "raise", "lower", "smooth", "flatten", "paint", "hole", "fill"};
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
                    "'mode' is raise, lower, smooth, flatten, paint, hole or "
                    "fill, with a "
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

/* Heights one terrain.sample returns at most, as points or as a grid. */
#define OPS_TERRAIN_SAMPLE_MAX 4096u
/* Samples along the longer side of a grid without `step`. */
#define OPS_TERRAIN_GRID_SIDE 64u

/* The world ground height at world x and z, which `origin` maps to the
   terrain's local space; null outside the terrain or over a hole. A grid
   rounds it to centimetres to keep the answer short. */
static VkrBakeryJson *ops_terrain_height(OpsContext *ctx,
                                         const VkrHeightfield *field,
                                         Vec3 origin, float32_t x, float32_t z,
                                         bool8_t rounded) {
  float32_t h = 0.0f;
  /* A hole has no ground. */
  if (!vkr_heightfield_sample(field, x + origin.x, z + origin.z, &h) ||
      vkr_heightfield_open(field, x + origin.x, z + origin.z)) {
    return vkr_bakery_json_null(ops_arena(ctx));
  }
  const float64_t height = (float64_t)(h - origin.y);
  return ops_number(ctx,
                    rounded ? floor(height * 100.0 + 0.5) / 100.0 : height);
}

/* terrain.sample with `region`: rows of heights from min z to max z, each
   running +x, as level.map and a top capture lay them out. */
static bool8_t ops_terrain_grid(OpsContext *ctx, const VkrHeightfield *field,
                                Vec3 origin, const VkrBakeryJson *region) {
  Vec3 min = {0};
  Vec3 max = {0};
  bool8_t has_min = false_v;
  bool8_t has_max = false_v;
  if (region->type != VKR_BAKERY_JSON_OBJECT ||
      !ops_arg_vec3(ctx, region, "min", &min, &has_min) ||
      !ops_arg_vec3(ctx, region, "max", &max, &has_max) || !has_min ||
      !has_max || max.x <= min.x || max.z <= min.z) {
    return ctx->call->error_code
               ? false_v
               : ops_fail(ctx, OPS_INVALID,
                          "'region' is {\"min\": [x, y, z], \"max\": [x, y, "
                          "z]} with max beyond min in x and z");
  }
  const float32_t width = max.x - min.x;
  const float32_t depth = max.z - min.z;
  float64_t step = (float64_t)Max(Max(width, depth) /
                                      (float32_t)(OPS_TERRAIN_GRID_SIDE - 1u),
                                  field->spacing);
  (void)ops_arg_number(ctx->call->args, "step", &step);
  if (!(step > 0.0)) {
    return ops_fail(ctx, OPS_INVALID, "'step' must be positive");
  }
  const uint32_t columns = (uint32_t)floor((float64_t)width / step) + 1u;
  const uint32_t rows = (uint32_t)floor((float64_t)depth / step) + 1u;
  if ((uint64_t)columns * rows > OPS_TERRAIN_SAMPLE_MAX) {
    return ops_fail(ctx, OPS_LIMIT,
                    "A grid of %u by %u heights passes %u; raise 'step' or "
                    "split the region",
                    columns, rows, OPS_TERRAIN_SAMPLE_MAX);
  }
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *list = vkr_bakery_json_array(arena);
  for (uint32_t z = 0; z < rows; ++z) {
    VkrBakeryJson *row = vkr_bakery_json_array(arena);
    const float32_t pz = min.z + (float32_t)((float64_t)z * step);
    for (uint32_t x = 0; x < columns; ++x) {
      const float32_t px = min.x + (float32_t)((float64_t)x * step);
      vkr_bakery_json_append(
          row, ops_terrain_height(ctx, field, origin, px, pz, true_v));
    }
    vkr_bakery_json_append(list, row);
  }
  VkrBakeryJson *grid = vkr_bakery_json_object(arena);
  VkrBakeryJson *first = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(first, ops_number(ctx, min.x));
  vkr_bakery_json_append(first, ops_number(ctx, min.z));
  ops_set(ctx, grid, "first", first);
  ops_set(ctx, grid, "step", ops_number(ctx, step));
  ops_set(ctx, grid, "columns", vkr_bakery_json_int(arena, columns));
  ops_set(ctx, grid, "rows", list);
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "grid", grid);
  return true_v;
}

/* terrain.sample: ground heights at points, for placing things, or a grid of
   them over a region. */
static VkrEditorOpStatus ops_run_terrain_sample(OpsContext *ctx) {
  OpsRef ref;
  const VkrHeightfield *field = NULL;
  const VkrBakeryJson *points = vkr_bakery_json_get(ctx->call->args, "points");
  const VkrBakeryJson *region = vkr_bakery_json_get(ctx->call->args, "region");
  if (!ops_terrain_arg(ctx, NULL, ctx->call->args, &ref, &field)) {
    return VKR_EDITOR_OP_DONE;
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  Vec3 origin = {0};
  (void)vkr_scene_terrain_to_local(scene, ref.entity, vec3_zero(), &origin);
  if (region) {
    (void)ops_terrain_grid(ctx, field, origin, region);
    return VKR_EDITOR_OP_DONE;
  }
  if (!points || points->type != VKR_BAKERY_JSON_ARRAY ||
      points->count > OPS_TERRAIN_SAMPLE_MAX) {
    ops_fail(ctx, OPS_INVALID, "'points' is up to %u [x, z] pairs",
             OPS_TERRAIN_SAMPLE_MAX);
    return VKR_EDITOR_OP_DONE;
  }
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *heights = vkr_bakery_json_array(arena);
  for (const VkrBakeryJson *entry = points->first; entry; entry = entry->next) {
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
        heights, ops_terrain_height(ctx, field, origin, px, pz, false_v));
  }
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "heights", heights);
  return VKR_EDITOR_OP_DONE;
}

/* terrain.hole: holes cut, or filled, in a box or round each point. */
static bool8_t ops_build_terrain_hole(OpsContext *ctx,
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
  const bool8_t fill = ops_arg_bool(args, "fill", false_v);
  (void)field;
  if (has_min || has_max) {
    if (!has_min || !has_max || min.x == max.x || min.z == max.z) {
      return ops_fail(ctx, OPS_INVALID,
                      "terrain.hole needs both corners 'min' and 'max' of a "
                      "box with area");
    }
    const VkrHeightfieldOp op = {
        .kind = VKR_HEIGHTFIELD_OP_HOLE,
        .min = vec2_new(Min(min.x, max.x), Min(min.z, max.z)),
        .max = vec2_new(Max(min.x, max.x), Max(min.z, max.z)),
        .add = !fill};
    return ops_terrain_item(ctx, batch, &ref, &op) != NULL;
  }
  float64_t radius = 2.0;
  (void)ops_arg_number(args, "radius", &radius);
  const VkrBakeryJson *points = vkr_bakery_json_get(args, "points");
  const VkrBakeryJson *point = vkr_bakery_json_get(args, "point");
  if (!(radius > 0.0) || ((!points || points->type != VKR_BAKERY_JSON_ARRAY ||
                           !points->count || points->count > 256u) &&
                          !point)) {
    return ops_fail(ctx, OPS_INVALID,
                    "terrain.hole needs a 'min' and 'max' box, or a 'point' "
                    "or 1 to 256 'points' with a positive 'radius'");
  }
  VkrBakeryJson single = {0};
  const VkrBakeryJson *first = points ? points->first : NULL;
  if (!points) {
    single = *point;
    single.next = NULL;
    first = &single;
  }
  for (const VkrBakeryJson *entry = first; entry; entry = entry->next) {
    float32_t parts[3] = {0};
    if (entry->type != VKR_BAKERY_JSON_ARRAY || entry->count != 3u) {
      return ops_fail(ctx, OPS_INVALID, "Each point is [x, y, z]");
    }
    uint32_t i = 0u;
    for (const VkrBakeryJson *part = entry->first; part; part = part->next) {
      parts[i++] = (float32_t)(part->type == VKR_BAKERY_JSON_INT
                                   ? (float64_t)part->integer
                                   : part->number);
    }
    const VkrHeightfieldOp op = {.kind = VKR_HEIGHTFIELD_OP_BRUSH,
                                 .brush = fill ? VKR_HEIGHTFIELD_FILL
                                               : VKR_HEIGHTFIELD_HOLE,
                                 .a = vec3_new(parts[0], parts[1], parts[2]),
                                 .radius = (float32_t)radius};
    if (!ops_terrain_item(ctx, batch, &ref, &op)) {
      return false_v;
    }
  }
  return true_v;
}

// =============================================================================
// Population (ADR-084)
// =============================================================================

/* World points of `key`: 2 to VKR_SPLINE_POINT_MAX [x, y, z] arrays. */
static uint32_t ops_arg_points(OpsContext *ctx, const VkrBakeryJson *args,
                               const char *key, Vec3 *out, uint32_t min_count,
                               uint32_t max_count) {
  const VkrBakeryJson *points = vkr_bakery_json_get(args, key);
  if (!points || points->type != VKR_BAKERY_JSON_ARRAY ||
      points->count < min_count || points->count > max_count) {
    ops_fail(ctx, OPS_INVALID, "'%s' is %u to %u [x, y, z] points", key,
             min_count, max_count);
    return 0u;
  }
  uint32_t count = 0u;
  for (const VkrBakeryJson *entry = points->first; entry; entry = entry->next) {
    float32_t parts[3] = {0};
    if (entry->type != VKR_BAKERY_JSON_ARRAY || entry->count != 3u) {
      ops_fail(ctx, OPS_INVALID, "Each point of '%s' is [x, y, z]", key);
      return 0u;
    }
    uint32_t i = 0u;
    for (const VkrBakeryJson *part = entry->first; part; part = part->next) {
      parts[i++] = (float32_t)(part->type == VKR_BAKERY_JSON_INT
                                   ? (float64_t)part->integer
                                   : part->number);
    }
    if (!isfinite(parts[0]) || !isfinite(parts[1]) || !isfinite(parts[2])) {
      ops_fail(ctx, OPS_INVALID, "Points of '%s' must be finite", key);
      return 0u;
    }
    out[count++] = vec3_new(parts[0], parts[1], parts[2]);
  }
  return count;
}

/* Appends an ADD_COMPONENT of `type`, read from `values`, to the batch's
   created entity `item`. */
static bool8_t ops_add_created_component(OpsContext *ctx, OpsBatch *batch,
                                         uint16_t container, uint32_t item,
                                         const VkrTypeDesc *type,
                                         const VkrBakeryJson *values) {
  VkrSampleEditBatchItem *add =
      ops_batch_add(ctx, batch, container, VKR_SCENE_EDIT_ADD_COMPONENT);
  if (!add) {
    return false_v;
  }
  const OpsRef ref = {.container = container, .item = (int32_t)item};
  ops_item_target(add, &ref);
  add->request.values.component_type = type;
  vkr_type_defaults(type, add->request.values.component);
  if (!ops_component_read(ctx, type, values, add->request.values.component)) {
    return false_v;
  }
  char error[160] = {0};
  if (!vkr_type_validate(type, add->request.values.component, error,
                         sizeof(error))) {
    return ops_fail(ctx, OPS_INVALID, "%s: %s", type->name, error);
  }
  return true_v;
}

/* spline.create: a spline entity at its first point and a child
   spline_point per point; `mesh` adds a spline_mesh. */
static bool8_t ops_build_spline_create(OpsContext *ctx,
                                       const VkrBakeryJson *args,
                                       OpsBatch *batch) {
  uint16_t container = 0u;
  Vec3 points[VKR_SPLINE_POINT_MAX];
  if (!ops_arg_container(ctx, args, &container)) {
    return false_v;
  }
  const uint32_t count =
      ops_arg_points(ctx, args, "points", points, 2u, VKR_SPLINE_POINT_MAX);
  if (!count) {
    return false_v;
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, container, VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  const uint32_t spline_item = batch->count - 1u;
  VkrSceneEditValues *values = &item->request.values;
  values->fields =
      VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM | VKR_SCENE_EDIT_COMPONENT;
  snprintf(values->name, sizeof(values->name), "Spline");
  String8 name = {0};
  if (vkr_bakery_json_get_string(args, "name", &name) && name.length &&
      name.length < sizeof(values->name)) {
    snprintf(values->name, sizeof(values->name), "%.*s", (int)name.length,
             name.str);
  }
  values->position = points[0];
  values->rotation = vkr_quat_identity();
  values->scale = vec3_one();
  values->component_type = &vkr_scene_spline_type;
  *(SceneSpline *)values->component =
      (SceneSpline){.closed = ops_arg_bool(args, "closed", false_v)};
  for (uint32_t i = 0; i < count; ++i) {
    VkrSampleEditBatchItem *point =
        ops_batch_add(ctx, batch, container, VKR_SCENE_EDIT_CREATE);
    if (!point) {
      return false_v;
    }
    point->parent_ref = (int32_t)spline_item;
    VkrSceneEditValues *point_values = &point->request.values;
    point_values->fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM |
                           VKR_SCENE_EDIT_COMPONENT;
    snprintf(point_values->name, sizeof(point_values->name), "Point %u",
             i + 1u);
    point_values->position = vec3_sub(points[i], points[0]);
    point_values->rotation = vkr_quat_identity();
    point_values->scale = vec3_one();
    point_values->component_type = &vkr_scene_spline_point_type;
    *(SceneSplinePoint *)point_values->component =
        (SceneSplinePoint){.order = (float32_t)i};
  }
  const VkrBakeryJson *mesh = vkr_bakery_json_get(args, "mesh");
  if (mesh && !ops_add_created_component(ctx, batch, container, spline_item,
                                         &vkr_scene_spline_mesh_type, mesh)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = spline_item;
  return true_v;
}

/* scatter.create: an entity whose box drops seeded copies of a mesh. */
static bool8_t ops_build_scatter_create(OpsContext *ctx,
                                        const VkrBakeryJson *args,
                                        OpsBatch *batch) {
  uint16_t container = 0u;
  if (!ops_arg_container(ctx, args, &container)) {
    return false_v;
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, container, VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  VkrSceneEditValues *values = &item->request.values;
  values->fields =
      VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM | VKR_SCENE_EDIT_COMPONENT;
  snprintf(values->name, sizeof(values->name), "Scatter");
  String8 name = {0};
  if (vkr_bakery_json_get_string(args, "name", &name) && name.length &&
      name.length < sizeof(values->name)) {
    snprintf(values->name, sizeof(values->name), "%.*s", (int)name.length,
             name.str);
  }
  if (!ops_arg_vec3(ctx, args, "position", &values->position, NULL)) {
    return false_v;
  }
  values->rotation = vkr_quat_identity();
  values->scale = vec3_one();
  values->component_type = &vkr_scene_scatter_type;
  vkr_type_defaults(&vkr_scene_scatter_type, values->component);
  if (!ops_component_read(ctx, &vkr_scene_scatter_type,
                          vkr_bakery_json_get(args, "values"),
                          values->component)) {
    return false_v;
  }
  char error[160] = {0};
  if (!vkr_type_validate(&vkr_scene_scatter_type, values->component, error,
                         sizeof(error))) {
    return ops_fail(ctx, OPS_INVALID, "scatter: %s", error);
  }
  batch->op_item[batch->op_count] = batch->count - 1u;
  return ops_validate_values(ctx, values);
}

/* The world points of an existing spline argument `key`. */
static uint32_t ops_spline_points(OpsContext *ctx, const OpsBatch *batch,
                                  const VkrBakeryJson *args, const char *key,
                                  OpsRef *ref, Vec3 *out, bool8_t *closed) {
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, key), key, ref)) {
    return 0u;
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref->container);
  const uint32_t count =
      ref->item < 0 ? vkr_scene_spline_world_points(
                          scene, ref->entity, out, VKR_SPLINE_POINT_MAX, closed)
                    : 0u;
  if (count < 2u) {
    ops_fail(ctx, OPS_INVALID,
             "'%s' must name an existing spline with at least two points", key);
    return 0u;
  }
  return count;
}

/* terrain.road: flattens a band along a spline, following its heights. */
static bool8_t ops_build_terrain_road(OpsContext *ctx,
                                      const VkrBakeryJson *args,
                                      OpsBatch *batch) {
  OpsRef ref;
  OpsRef spline;
  const VkrHeightfield *field = NULL;
  Vec3 points[VKR_SPLINE_POINT_MAX];
  bool8_t closed = false_v;
  if (!ops_terrain_arg(ctx, batch, args, &ref, &field)) {
    return false_v;
  }
  const uint32_t count =
      ops_spline_points(ctx, batch, args, "spline", &spline, points, &closed);
  if (!count) {
    return false_v;
  }
  float64_t width = 6.0;
  float64_t falloff = 4.0;
  float64_t offset = 0.0;
  (void)ops_arg_number(args, "width", &width);
  (void)ops_arg_number(args, "falloff", &falloff);
  (void)ops_arg_number(args, "offset", &offset);
  if (!(width > 0.0) || !(falloff >= 0.0) || !isfinite(offset)) {
    return ops_fail(ctx, OPS_INVALID,
                    "terrain.road needs a positive 'width' and a "
                    "non-negative 'falloff'");
  }
  /* The centreline every half spacing, so curves stay round. */
  const float32_t spacing = Max(0.5f, field->spacing * 0.5f);
  const uint32_t capacity = 8192u;
  VkrSplineSample *samples =
      arena_alloc(ops_arena(ctx), sizeof(VkrSplineSample) * capacity,
                  ARENA_MEMORY_TAG_ARRAY);
  Vec3 *path = arena_alloc(ops_arena(ctx), sizeof(Vec3) * capacity,
                           ARENA_MEMORY_TAG_ARRAY);
  if (!samples || !path) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  const uint32_t sampled =
      vkr_spline_sample(points, count, closed, spacing, samples, capacity);
  for (uint32_t i = 0; i < sampled; ++i) {
    path[i] =
        vec3_add(samples[i].position, vec3_new(0.0f, (float32_t)offset, 0.0f));
  }
  const VkrHeightfieldOp op = {.kind = VKR_HEIGHTFIELD_OP_ROAD,
                               .width = (float32_t)width,
                               .falloff = (float32_t)falloff,
                               .path = path,
                               .path_count = sampled};
  return ops_terrain_item(ctx, batch, &ref, &op) != NULL;
}

/* spline.sample: points along a spline every `spacing` metres. */
static VkrEditorOpStatus ops_run_spline_sample(OpsContext *ctx) {
  OpsRef ref;
  Vec3 points[VKR_SPLINE_POINT_MAX];
  bool8_t closed = false_v;
  const uint32_t count = ops_spline_points(ctx, NULL, ctx->call->args, "spline",
                                           &ref, points, &closed);
  if (!count) {
    return VKR_EDITOR_OP_DONE;
  }
  float64_t spacing = 1.0;
  (void)ops_arg_number(ctx->call->args, "spacing", &spacing);
  if (!(spacing >= 0.05)) {
    ops_fail(ctx, OPS_INVALID, "'spacing' is at least 0.05 m");
    return VKR_EDITOR_OP_DONE;
  }
  const uint32_t capacity = 2048u;
  VkrSplineSample *samples =
      arena_alloc(ops_arena(ctx), sizeof(VkrSplineSample) * capacity,
                  ARENA_MEMORY_TAG_ARRAY);
  if (!samples) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  const uint32_t sampled = vkr_spline_sample(
      points, count, closed, (float32_t)spacing, samples, capacity);
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *list = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < sampled; ++i) {
    VkrBakeryJson *row = vkr_bakery_json_object(arena);
    ops_set(ctx, row, "position", ops_vec3(ctx, samples[i].position));
    ops_set(ctx, row, "tangent", ops_vec3(ctx, samples[i].tangent));
    ops_set(ctx, row, "distance", ops_number(ctx, samples[i].distance));
    vkr_bakery_json_append(list, row);
  }
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "length",
          ops_number(ctx, vkr_spline_length(points, count, closed)));
  ops_set(ctx, ctx->call->result, "samples", list);
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
    if (!frame->io_request || !frame->io_running) {
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

// -----------------------------------------------------------------------------
// claims.set, claims.release, claims.list, changes.feed
// -----------------------------------------------------------------------------

static VkrBakeryJson *ops_box(OpsContext *ctx, Vec3 lo, Vec3 hi) {
  VkrBakeryJson *box = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, box, "min", ops_vec3(ctx, lo));
  ops_set(ctx, box, "max", ops_vec3(ctx, hi));
  return box;
}

static VkrBakeryJson *ops_claim_json(OpsContext *ctx,
                                     const VkrEditorClaim *claim) {
  char slot[16];
  VkrBakeryJson *object = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, object, "claim", vkr_bakery_json_int(ops_arena(ctx), claim->id));
  ops_set(ctx, object, "author",
          vkr_bakery_json_cstr(ops_arena(ctx), claim->author));
  ops_set(ctx, object, "name",
          vkr_bakery_json_cstr(ops_arena(ctx), claim->name));
  ops_set(ctx, object, "container",
          vkr_bakery_json_cstr(
              ops_arena(ctx),
              ops_container_name(claim->container, slot, sizeof(slot))));
  ops_set(ctx, object, "region", ops_box(ctx, claim->min, claim->max));
  return object;
}

/* claims.set: the caller claims a box, or moves one of its claims. */
static VkrEditorOpStatus ops_run_claims_set(OpsContext *ctx) {
  VkrEditorOps *ops = ctx->ops;
  const VkrBakeryJson *args = ctx->call->args;
  if (!ctx->call->author[0]) {
    ops_fail(ctx, OPS_INVALID,
             "Only an agent claims a region; the designer edits anywhere");
    return VKR_EDITOR_OP_DONE;
  }
  const VkrBakeryJson *region = vkr_bakery_json_get(args, "region");
  Vec3 lo = {0};
  Vec3 hi = {0};
  bool8_t has_lo = false_v;
  bool8_t has_hi = false_v;
  uint16_t container = 0u;
  if (!region || region->type != VKR_BAKERY_JSON_OBJECT ||
      !ops_arg_vec3(ctx, region, "min", &lo, &has_lo) ||
      !ops_arg_vec3(ctx, region, "max", &hi, &has_hi) || !has_lo || !has_hi ||
      !(hi.x > lo.x && hi.y > lo.y && hi.z > lo.z)) {
    if (!ctx->call->error_code) {
      ops_fail(ctx, OPS_INVALID,
               "'region' is {\"min\": [..], \"max\": [..]}, a box with "
               "volume");
    }
    return VKR_EDITOR_OP_DONE;
  }
  if (!ops_arg_container(ctx, args, &container)) {
    return VKR_EDITOR_OP_DONE;
  }
  float64_t id_value = 0.0;
  const uint32_t id = ops_arg_number(args, "claim", &id_value)
                          ? (uint32_t)Max(id_value, 0.0)
                          : 0u;
  const VkrEditorClaim *other =
      ops_claim_hit(ops, ctx->call->author, container, lo, hi);
  if (other) {
    ops_fail(ctx, OPS_CLAIMED,
             "The box overlaps %s's claim '%s' (claim %u); claim beside it",
             other->author, other->name[0] ? other->name : "unnamed",
             other->id);
    return VKR_EDITOR_OP_DONE;
  }
  VkrEditorClaim *claim = NULL;
  for (uint32_t i = 0; id && i < ops->claim_count; ++i) {
    if (ops->claims[i].id == id) {
      claim = &ops->claims[i];
    }
  }
  if (id && (!claim || strcmp(claim->author, ctx->call->author))) {
    ops_fail(ctx, claim ? OPS_NOT_OWNER : OPS_NOT_FOUND,
             claim ? "Claim %u belongs to %s" : "No claim %u", id,
             claim ? claim->author : "");
    return VKR_EDITOR_OP_DONE;
  }
  if (!claim) {
    if (ops->claim_count == VKR_EDITOR_CLAIM_MAX) {
      ops_fail(ctx, OPS_LIMIT, "The editor holds at most %u claims",
               VKR_EDITOR_CLAIM_MAX);
      return VKR_EDITOR_OP_DONE;
    }
    claim = &ops->claims[ops->claim_count++];
    *claim = (VkrEditorClaim){.id = ++ops->next_claim_id};
    snprintf(claim->author, sizeof(claim->author), "%s", ctx->call->author);
  }
  claim->container = container;
  claim->min = lo;
  claim->max = hi;
  String8 name = {0};
  if (vkr_bakery_json_get_string(args, "name", &name)) {
    snprintf(claim->name, sizeof(claim->name), "%.*s",
             (int)Min(name.length, (uint64_t)47u), (const char *)name.str);
  }
  OpsFeedEvent *event = ops_feed_add(ops, OPS_FEED_CLAIMED, container,
                                     claim->author, claim->name);
  event->claim = claim->id;
  event->min = lo;
  event->max = hi;
  event->bounded = true_v;
  ctx->call->result = ops_claim_json(ctx, claim);
  ops_claims_save(ops, ctx->frame);
  return VKR_EDITOR_OP_DONE;
}

/* Releases the claim at `index` and feeds the event. */
static void ops_claim_release_at(VkrEditorOps *ops, uint32_t index) {
  const VkrEditorClaim *claim = &ops->claims[index];
  OpsFeedEvent *event = ops_feed_add(ops, OPS_FEED_RELEASED, claim->container,
                                     claim->author, claim->name);
  event->claim = claim->id;
  event->min = claim->min;
  event->max = claim->max;
  event->bounded = true_v;
  ops->claims[index] = ops->claims[--ops->claim_count];
}

/* claims.release: one claim by id, or every claim of the caller; the
   editor's own requests may release any claim, and all with 'all'. */
static VkrEditorOpStatus ops_run_claims_release(OpsContext *ctx) {
  VkrEditorOps *ops = ctx->ops;
  const char *author = ctx->call->author;
  float64_t id_value = 0.0;
  const uint32_t id = ops_arg_number(ctx->call->args, "claim", &id_value)
                          ? (uint32_t)Max(id_value, 0.0)
                          : 0u;
  const bool8_t all = ops_arg_bool(ctx->call->args, "all", false_v);
  if (!id && !author[0] && !all) {
    ops_fail(ctx, OPS_INVALID, "Name a 'claim', or release 'all'");
    return VKR_EDITOR_OP_DONE;
  }
  VkrBakeryJson *released = vkr_bakery_json_array(ops_arena(ctx));
  for (uint32_t i = 0; i < ops->claim_count;) {
    VkrEditorClaim *claim = &ops->claims[i];
    const bool8_t mine = !author[0] || strcmp(claim->author, author) == 0;
    const bool8_t named = id ? claim->id == id : (all || author[0]);
    if (!named) {
      ++i;
      continue;
    }
    if (!mine) {
      if (id) {
        ops_fail(ctx, OPS_NOT_OWNER, "Claim %u belongs to %s", id,
                 claim->author);
        return VKR_EDITOR_OP_DONE;
      }
      ++i;
      continue;
    }
    vkr_bakery_json_append(released,
                           vkr_bakery_json_int(ops_arena(ctx), claim->id));
    ops_claim_release_at(ops, i);
  }
  if (id && !released->count) {
    ops_fail(ctx, OPS_NOT_FOUND, "No claim %u", id);
    return VKR_EDITOR_OP_DONE;
  }
  ops_claims_save(ops, ctx->frame);
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "released", released);
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_claims_list(OpsContext *ctx) {
  VkrBakeryJson *claims = vkr_bakery_json_array(ops_arena(ctx));
  for (uint32_t i = 0; i < ctx->ops->claim_count; ++i) {
    vkr_bakery_json_append(claims, ops_claim_json(ctx, &ctx->ops->claims[i]));
  }
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "claims", claims);
  return VKR_EDITOR_OP_DONE;
}

/* changes.feed: the events after sequence `after`, oldest first. */
static VkrEditorOpStatus ops_run_feed(OpsContext *ctx) {
  static const char *const kinds[OPS_FEED_KIND_COUNT] = {
      "applied", "accepted", "rejected", "claimed", "released"};
  const VkrEditorOps *ops = ctx->ops;
  Arena *arena = ops_arena(ctx);
  float64_t after_value = 0.0;
  float64_t limit_value = 64.0;
  (void)ops_arg_number(ctx->call->args, "after", &after_value);
  (void)ops_arg_number(ctx->call->args, "limit", &limit_value);
  const uint64_t after = (uint64_t)Max(after_value, 0.0);
  const uint64_t limit =
      (uint64_t)vkr_clamp_f64(limit_value, 1.0, OPS_FEED_MAX);
  /* The ring holds the newest OPS_FEED_MAX events. */
  const uint64_t oldest =
      ops->feed_last > OPS_FEED_MAX ? ops->feed_last - OPS_FEED_MAX + 1u : 1u;
  const uint64_t first = Max(after + 1u, oldest);
  VkrBakeryJson *events = vkr_bakery_json_array(arena);
  uint64_t last = after;
  for (uint64_t sequence = first;
       sequence <= ops->feed_last && sequence < first + limit; ++sequence) {
    const OpsFeedEvent *event = &ops->feed[sequence % OPS_FEED_MAX];
    const VkrScene *scene = ops_scene(ctx->frame, event->container);
    char slot[16];
    VkrBakeryJson *row = vkr_bakery_json_object(arena);
    ops_set(ctx, row, "seq", vkr_bakery_json_int(arena, (int64_t)sequence));
    ops_set(ctx, row, "kind", vkr_bakery_json_cstr(arena, kinds[event->kind]));
    ops_set(ctx, row, "author",
            vkr_bakery_json_cstr(arena,
                                 event->author[0] ? event->author : "editor"));
    if (event->label[0]) {
      ops_set(ctx, row, "label", vkr_bakery_json_cstr(arena, event->label));
    }
    ops_set(
        ctx, row, "container",
        vkr_bakery_json_cstr(
            arena, ops_container_name(event->container, slot, sizeof(slot))));
    if (event->change) {
      ops_set(ctx, row, "change", vkr_bakery_json_int(arena, event->change));
    }
    if (event->claim) {
      ops_set(ctx, row, "claim", vkr_bakery_json_int(arena, event->claim));
    }
    if (event->bounded) {
      ops_set(ctx, row, "bounds", ops_box(ctx, event->min, event->max));
    }
    if (event->entity_count) {
      VkrBakeryJson *entities = vkr_bakery_json_array(arena);
      for (uint32_t i = 0; i < event->entity_count; ++i) {
        vkr_bakery_json_append(entities,
                               ops_entity(ctx, scene, event->entities[i]));
      }
      ops_set(ctx, row, "entities", entities);
    }
    vkr_bakery_json_append(events, row);
    last = sequence;
  }
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  ops_set(ctx, result, "events", events);
  ops_set(ctx, result, "next", vkr_bakery_json_int(arena, (int64_t)last));
  ops_set(ctx, result, "latest",
          vkr_bakery_json_int(arena, (int64_t)ops->feed_last));
  ops_set(ctx, result, "missed",
          vkr_bakery_json_bool(arena, after + 1u < oldest));
  /* The designer's own edits reach no feed; a revision that grows tells a
     client to look again. */
  VkrBakeryJson *revisions = vkr_bakery_json_object(arena);
  const VkrSceneEditState *edits = ops_journal(ctx->frame, 0u);
  const VkrSceneEditState *world =
      ops_journal(ctx->frame, VKR_SCENE_WORLD_ROOT_ID);
  ops_set(ctx, revisions, "scene",
          vkr_bakery_json_int(arena, edits ? (int64_t)edits->revision : 0));
  ops_set(ctx, revisions, "world",
          vkr_bakery_json_int(arena, world ? (int64_t)world->revision : 0));
  ops_set(ctx, result, "revisions", revisions);
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

// -----------------------------------------------------------------------------
// entity.place
// -----------------------------------------------------------------------------

/* entity.place: moves an object so its world box sits on, against or
   inside another's, as an entity.set of its position. */
static bool8_t ops_build_place(OpsContext *ctx, const VkrBakeryJson *args,
                               OpsBatch *batch) {
  OpsRef ref;
  OpsRef target;
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "entity"), "entity",
               &ref) ||
      !ops_ref(ctx, batch, vkr_bakery_json_get(args, "target"), "target",
               &target)) {
    return false_v;
  }
  if (ref.item >= 0 || target.item >= 0) {
    return ops_fail(ctx, OPS_INVALID,
                    "entity.place moves objects that exist; place what this "
                    "batch creates in a later request");
  }
  if (ref.container != target.container ||
      ref.entity.u64 == target.entity.u64) {
    return ops_fail(ctx, OPS_INVALID,
                    "'entity' and 'target' are two objects of one scene");
  }
  String8 mode = {0};
  (void)vkr_bakery_json_get_string(args, "mode", &mode);
  const bool8_t on = ops_equals(mode, "on");
  const bool8_t inside = ops_equals(mode, "inside");
  const bool8_t against = ops_equals(mode, "against");
  if (!on && !inside && !against) {
    return ops_fail(ctx, OPS_INVALID, "'mode' is on, against or inside");
  }
  String8 side = {0};
  (void)vkr_bakery_json_get_string(args, "side", &side);
  const int32_t axis =
      against ? (ops_equals(side, "+x") || ops_equals(side, "-x")   ? 0
                 : ops_equals(side, "+z") || ops_equals(side, "-z") ? 2
                                                                    : -1)
              : 1;
  if (axis < 0) {
    return ops_fail(ctx, OPS_INVALID,
                    "'against' needs 'side': +x, -x, +z or -z of the target");
  }
  float64_t gap = 0.0;
  (void)ops_arg_number(args, "gap", &gap);
  const bool8_t keep = ops_arg_bool(args, "keep", false_v);
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  const SceneTransform *transform = ops_transform(scene, ref.entity);
  VkrBrushGeometry *scratch = arena_alloc(
      ops_arena(ctx), sizeof(VkrBrushGeometry), ARENA_MEMORY_TAG_STRUCT);
  if (!transform || !scratch) {
    return ops_fail(ctx, OPS_INVALID, "'entity' has no pose to move");
  }
  Vec3 elo = {0};
  Vec3 ehi = {0};
  Vec3 tlo = {0};
  Vec3 thi = {0};
  ops_object_box(scene, ref.entity, scratch, &elo, &ehi);
  ops_object_box(scene, target.entity, scratch, &tlo, &thi);
  const Vec3 ec = vec3_scale(vec3_add(elo, ehi), 0.5f);
  const Vec3 tc = vec3_scale(vec3_add(tlo, thi), 0.5f);
  const float32_t g = (float32_t)gap;
  Vec3 delta = vec3_zero();
  if (on || inside) {
    /* On the top, or on the floor inside (a room's floor slab). */
    const float32_t floor =
        on ? thi.y
           : vkr_editor_entity_floor(scene, target.entity, scratch, tlo, thi);
    delta.y = floor + g - elo.y;
    if (!keep) {
      delta.x = tc.x - ec.x;
      delta.z = tc.z - ec.z;
    }
  } else if (axis == 0) {
    delta.x = side.str[0] == '+' ? thi.x + g - elo.x : tlo.x - g - ehi.x;
    delta.z = keep ? 0.0f : tc.z - ec.z;
  } else {
    delta.z = side.str[0] == '+' ? thi.z + g - elo.z : tlo.z - g - ehi.z;
    delta.x = keep ? 0.0f : tc.x - ec.x;
  }
  /* The new world position in the parent's space. */
  Vec3 position = vec3_add(mat4_position(transform->world), delta);
  const SceneTransform *parent =
      ops_transform(scene, ops_parent(scene, ref.entity));
  if (parent) {
    position = mat4_mul_vec3(mat4_inverse(parent->world), position);
  }
  char id[40];
  ops_id_text(ref.entity, id, sizeof(id));
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *set = vkr_bakery_json_object(arena);
  ops_set(ctx, set, "entity", vkr_bakery_json_cstr(arena, id));
  ops_set(ctx, set, "position", ops_vec3(ctx, position));
  return ops_build_set(ctx, set, batch);
}

#define OPS_ENTITY_SCHEMA                                                      \
  "{\"type\":\"string\",\"description\":\"An entity: world:index:generation, " \
  "an exact unique name, or $k for the entity operation k of the same batch "  \
  "created\"}"
#define OPS_CONTAINER_SCHEMA                                                   \
  "{\"description\":\"primary, world, or an added scene slot 1-6\","           \
  "\"oneOf\":[{\"type\":\"string\",\"enum\":[\"primary\",\"world\"]},"         \
  "{\"type\":\"integer\",\"minimum\":1,\"maximum\":6}]}"
#define OPS_AGENT_SCHEMA                                                       \
  "\"agent\":{\"type\":\"string\",\"maxLength\":31,\"description\":\"Your "    \
  "name: the author of the change, whose own steps alone its undo takes\"}"
#define OPS_REVIEW_SCHEMA                                                      \
  "\"review\":{\"type\":\"boolean\",\"description\":\"Keep the edit as a "     \
  "pending change the designer accepts or rejects (default true)\"},"          \
  "\"dry_run\":{\"type\":\"boolean\"},\"select\":{\"type\":\"boolean\","       \
  "\"description\":\"Select the first entity once applied\"},"                 \
  "\"label\":{\"type\":\"string\",\"description\":\"The change's name in "     \
  "the designer's review (default the operation)\"},"                          \
  "\"settle\":{\"type\":\"boolean\",\"description\":\"Answer once brushes, "   \
  "shapes, terrain and scatters rebuilt, with each entity's world bounds "     \
  "and build status (default false)\"}," OPS_AGENT_SCHEMA
#define OPS_SETTLE_SCHEMA                                                      \
  "\"settle\":{\"type\":\"boolean\",\"description\":\"Wait until brushes, "    \
  "shapes, terrain and scatters rebuilt after earlier edits (default "         \
  "true)\"}"
#define OPS_SETTLE_OFF_SCHEMA                                                  \
  "\"settle\":{\"type\":\"boolean\",\"description\":\"Wait until brushes, "    \
  "shapes, terrain and scatters rebuilt after earlier edits (default "         \
  "false)\"}"
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

#define OPS_CAPTURE_VIEW_SCHEMA                                                \
  "{\"type\":\"string\",\"enum\":[\"current\",\"perspective\",\"top\","        \
  "\"left\",\"right\",\"bottom\"]}"
#define OPS_CAPTURE_FOCUS_SCHEMA                                               \
  "{\"oneOf\":[" OPS_ENTITY_SCHEMA ",{\"type\":\"object\",\"properties\":{"    \
  "\"min\":" OPS_VEC3_SCHEMA ",\"max\":" OPS_VEC3_SCHEMA "},\"required\":["    \
  "\"min\",\"max\"]}]}"

static const OpsDef s_ops[] = {
    {"ops.list", "Every operation with its description and argument schema.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_list, NULL, OPS_QUICK},
    {"editor.status",
     "Loaded scenes, selection, simulation, view, pending changes, the "
     "active workbench and the running Scene tool.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_status, NULL,
     OPS_QUICK},
    {"workbench.list",
     "Each workbench's id, name, position, shortcut, dock panels, open "
     "windows and Scene mode, and the active one.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_workbench_list, NULL,
     OPS_QUICK},
    {"workbench.switch",
     "Switch to a workbench by id, name or position; answers once it shows "
     "with the Scene rectangle in window points [x, y, width, height].",
     "{\"type\":\"object\",\"properties\":{\"workbench\":{\"type\":"
     "\"string\"}},\"required\":[\"workbench\"]}",
     ops_run_workbench_switch, NULL},
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
     "faces and IO connections, which their owners otherwise "
     "summarise\"}," OPS_SETTLE_OFF_SCHEMA "}}",
     ops_run_describe, NULL, OPS_QUICK},
    {"entity.get",
     "One entity: pose, children, bounds and every component's values.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     "," OPS_SETTLE_OFF_SCHEMA "},\"required\":[\"entity\"]}",
     ops_run_get, NULL, OPS_QUICK},
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
     "null, keeping its world pose; 'snap' first sets it flush against the "
     "parent, lined up with it, unless it has free_placement.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"parent\":{\"oneOf\":[" OPS_ENTITY_SCHEMA
     ",{\"type\":\"null\"}]},\"snap\":{\"type\":\"boolean\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"entity\"]}",
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
     "Create editable stairs (a blockout shape): 'from' is the bottom front "
     "center (a spiral's pole), 'to' the top back center (its height is the "
     "stairs' height unless 'height' is set; a spiral's rim); 'kind' "
     "straight (default), l or u (two flights and a landing; the length from "
     "'from' to 'to' includes the landing), curved or spiral ('sweep' "
     "degrees; a spiral turns 22.5 degrees a step by default); 'turn' left "
     "or right; steps at most step_height (default 0.1875 m) high, 4096 at "
     "most, solid or 'thickness' thick slabs.",
     "{\"type\":\"object\",\"properties\":{\"from\":" OPS_VEC3_SCHEMA
     ",\"to\":" OPS_VEC3_SCHEMA ",\"width\":{\"type\":\"number\"},"
     "\"height\":{\"type\":\"number\"},\"kind\":{\"type\":\"string\","
     "\"enum\":[\"straight\",\"l\",\"u\",\"curved\",\"spiral\"]},"
     "\"turn\":{\"type\":\"string\",\"enum\":[\"left\",\"right\"]},"
     "\"sweep\":{\"type\":\"number\"},\"thickness\":{\"type\":"
     "\"number\"},\"step_height\":{\"type\":\"number\"}," OPS_BRUSH_SCHEMA
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
    {"entity.place",
     "Move 'entity' against 'target' by their world boxes: 'mode' on sets "
     "it on the target's top, inside on the target's floor (a room's floor "
     "slab), centred unless 'keep'; against sets it flush to the target's "
     "'side' (+x, -x, +z or -z), centred along that side unless 'keep'. "
     "'gap' leaves metres between them. Both objects must exist before the "
     "request.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"target\":" OPS_ENTITY_SCHEMA ",\"mode\":{\"type\":\"string\","
     "\"enum\":[\"on\",\"against\",\"inside\"]},\"side\":{\"type\":"
     "\"string\",\"enum\":[\"+x\",\"-x\",\"+z\",\"-z\"]},\"gap\":{\"type\":"
     "\"number\"},\"keep\":{\"type\":\"boolean\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"entity\",\"target\",\"mode\"]}",
     NULL, ops_build_place},
    {"brush.snap",
     "Snap objects together: the first stays, each other one (unless it has "
     "free_placement) moves the least distance that sets its world box "
     "flush against an object placed before it, 'gap' meters apart (default "
     "0), lined up with that object's sides or center.",
     "{\"type\":\"object\",\"properties\":{\"brushes\":{\"type\":"
     "\"array\",\"items\":" OPS_ENTITY_SCHEMA ",\"minItems\":2},"
     "\"gap\":{\"type\":\"number\",\"minimum\":0}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"brushes\"]}",
     NULL, ops_build_snap},
    {"brush.patch",
     "Pull a rectangle of a face's grid out by 'distance' meters, or push it "
     "in when negative. 'min' and 'max' are [u, v] corners on the face's "
     "grid axes (world X and Z on a floor). A pulled patch joins the brush "
     "when the result stays convex and is a new brush otherwise; a pushed "
     "one carves a recess or a hole.",
     "{\"type\":\"object\",\"properties\":{\"face\":" OPS_ENTITY_SCHEMA
     ",\"brush\":" OPS_ENTITY_SCHEMA "," OPS_SIDE_SCHEMA
     ",\"min\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},"
     "\"minItems\":2,\"maxItems\":2},\"max\":{\"type\":\"array\","
     "\"items\":{\"type\":\"number\"},\"minItems\":2,\"maxItems\":2},"
     "\"distance\":{\"type\":\"number\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"min\",\"max\",\"distance\"]}",
     NULL, ops_build_patch},
    {"brush.reshape",
     "Move corners of a brush by the world 'delta': one vertex, an edge's "
     "two ends, or with 'split' {point, normal} the ends of a grid line after "
     "cutting the brush along that plane. Each piece becomes the hull of its "
     "corners; a dent becomes up to eight convex pieces, refused when it "
     "needs more or folds the brush.",
     "{\"type\":\"object\",\"properties\":{\"brush\":" OPS_ENTITY_SCHEMA
     ",\"points\":{\"type\":\"array\",\"items\":" OPS_VEC3_SCHEMA
     ",\"minItems\":1,\"maxItems\":8},\"delta\":" OPS_VEC3_SCHEMA
     ",\"split\":{\"type\":\"object\",\"properties\":{"
     "\"point\":" OPS_VEC3_SCHEMA ",\"normal\":" OPS_VEC3_SCHEMA
     "}}," OPS_REVIEW_SCHEMA "},\"required\":[\"brush\",\"points\",\"delta\"]}",
     NULL, ops_build_reshape},
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
     "An editable corridor (a blockout shape) along 'points' (2 to 16 floor "
     "points), or from 'from' to 'to': per stretch a floor, two walls "
     "mitred at the bends and a ceiling unless 'ceiling' is false; 'radius' "
     "rounds its corners with arcs ('curved' means 2 m).",
     "{\"type\":\"object\",\"properties\":{\"points\":{\"type\":\"array\","
     "\"items\":" OPS_VEC3_SCHEMA ",\"minItems\":2,\"maxItems\":16},"
     "\"from\":" OPS_VEC3_SCHEMA ",\"to\":" OPS_VEC3_SCHEMA
     ",\"curved\":{\"type\":\"boolean\"},\"radius\":{\"type\":"
     "\"number\"},\"width\":{\"type\":\"number\"},"
     "\"height\":{\"type\":\"number\"},\"wall\":{\"type\":\"number\"},"
     "\"ceiling\":{\"type\":\"boolean\"},\"floor_material\":{\"type\":"
     "\"string\"}," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA "}}",
     NULL, ops_build_corridor},
    {"blockout.create",
     "Create an editable blockout shape: 'shape' stairs or corridor at "
     "'position' turned 'yaw' degrees, with 'values' over the blockout "
     "component's defaults, or a corridor along 'points' in the container's "
     "space.",
     "{\"type\":\"object\",\"properties\":{\"shape\":{\"type\":"
     "\"string\",\"enum\":[\"stairs\",\"corridor\"]},"
     "\"position\":" OPS_VEC3_SCHEMA
     ",\"yaw\":{\"type\":\"number\"},\"values\":{"
     "\"type\":\"object\"},\"points\":{\"type\":\"array\","
     "\"items\":" OPS_VEC3_SCHEMA "}," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA
     "},\"required\":[\"shape\"]}",
     NULL, ops_build_blockout_create},
    {"blockout.build",
     "Set a blockout shape's settings to 'values' over its current ones, "
     "which the scene builds it from, as one undo step.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"values\":{\"type\":\"object\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"entity\"]}",
     NULL, ops_build_blockout_build},
    {"blockout.bake",
     "Turn a blockout shape into plain brushes under its entity, to edit one "
     "by one, as one undo step.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"entity\"]}",
     NULL, ops_build_blockout_bake},
    {"blockout.doorway",
     "Cut a doorway through an existing box wall brush: it becomes up to "
     "three brushes around an opening 'width' wide and 'height' high, "
     "'offset' meters from the wall's center along its length.",
     "{\"type\":\"object\",\"properties\":{\"wall\":" OPS_ENTITY_SCHEMA
     ",\"offset\":{\"type\":\"number\"},\"width\":{\"type\":\"number\"},"
     "\"height\":{\"type\":\"number\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"wall\"]}",
     NULL, ops_build_doorway},
    {"mover.create",
     "Group 'objects' (brushes or any entities of one scene) under a new "
     "entity with a mover: in play it moves them 'distance' metres along "
     "'direction' (its own space; zero distance takes their size less "
     "'lip') at 'speed' m/s on its open, close, toggle and set_position "
     "inputs, waits 'wait' seconds (-1 stays open), and fires on_open, "
     "on_opened, on_close and on_closed. 'values' sets the mover component "
     "(direction, distance, lip, speed, wait, start_open, loop, locked).",
     "{\"type\":\"object\",\"properties\":{\"objects\":{\"type\":\"array\","
     "\"items\":" OPS_ENTITY_SCHEMA ",\"minItems\":1,\"maxItems\":64},"
     "\"name\":{\"type\":\"string\"},\"values\":" OPS_VALUES_SCHEMA
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"objects\"]}",
     NULL, ops_build_mover_create},
    {"terrain.create",
     "Create a heightfield terrain of 'size' metres a side (a multiple of 64 "
     "'spacing' cells, at most 8192 cells; above 1024 cells a multiple of "
     "1024, and the terrain streams around the camera) centred on "
     "'position', flat at "
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
     "or paint 'layer' 1-4. Falls off to zero at 'radius'. 'mode' hole cuts "
     "and fill closes every hole sample inside 'radius'.",
     "{\"type\":\"object\",\"properties\":{\"terrain\":" OPS_ENTITY_SCHEMA
     ",\"mode\":{\"type\":\"string\",\"enum\":[\"raise\",\"lower\","
     "\"smooth\",\"flatten\",\"paint\",\"hole\",\"fill\"]},"
     "\"point\":" OPS_VEC3_SCHEMA
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
     "Ground heights (world y) of a terrain at [x, z] points; null outside "
     "it or over a hole. With 'region' instead, a grid every 'step' metres "
     "(64 along the longer side by default, 4096 heights at most): rows from "
     "min z to max z, each running +x from 'first', rounded to "
     "centimetres.",
     "{\"type\":\"object\",\"properties\":{\"terrain\":" OPS_ENTITY_SCHEMA
     ",\"points\":{\"type\":\"array\",\"maxItems\":4096,\"items\":{"
     "\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":2,"
     "\"maxItems\":2}},\"region\":{\"type\":\"object\",\"properties\":{"
     "\"min\":" OPS_VEC3_SCHEMA ",\"max\":" OPS_VEC3_SCHEMA
     "},\"required\":[\"min\",\"max\"]},\"step\":{\"type\":\"number\","
     "\"exclusiveMinimum\":0}},\"required\":[\"terrain\"]}",
     ops_run_terrain_sample, NULL, OPS_QUICK},
    {"terrain.hole",
     "Cut a hole in the terrain's mesh and collision for an entrance: the "
     "samples strictly inside the box from 'min' to 'max' (world x and z; "
     "a box on grid lines opens exactly), or within 'radius' (default 2) of "
     "each of 'points' (or one 'point'). 'fill' closes the holes there "
     "again at their old heights, on layer 1.",
     "{\"type\":\"object\",\"properties\":{\"terrain\":" OPS_ENTITY_SCHEMA
     ",\"min\":" OPS_VEC3_SCHEMA ",\"max\":" OPS_VEC3_SCHEMA
     ",\"point\":" OPS_VEC3_SCHEMA ",\"points\":{\"type\":\"array\","
     "\"items\":" OPS_VEC3_SCHEMA ",\"maxItems\":256},\"radius\":{\"type\":"
     "\"number\"},\"fill\":{\"type\":\"boolean\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"terrain\"]}",
     NULL, ops_build_terrain_hole},
    {"spline.create",
     "Create a spline through 'points' (world [x, y, z], 2 to 256) as an "
     "entity at the first point with a child spline_point per point; "
     "'closed' loops it. 'mesh' (spline_mesh values: mesh, mesh_index, "
     "spacing, scale, offset, follow_slope) repeats a cooked mesh along it.",
     "{\"type\":\"object\",\"properties\":{\"points\":{\"type\":\"array\","
     "\"items\":" OPS_VEC3_SCHEMA ",\"minItems\":2,\"maxItems\":256},"
     "\"closed\":{\"type\":\"boolean\"},\"name\":{\"type\":\"string\"},"
     "\"mesh\":{\"type\":\"object\"},\"container\":" OPS_CONTAINER_SCHEMA
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"points\"]}",
     NULL, ops_build_spline_create},
    {"spline.sample",
     "Points along a spline every 'spacing' metres (default 1): position, "
     "unit tangent and distance, and the spline's length.",
     "{\"type\":\"object\",\"properties\":{\"spline\":" OPS_ENTITY_SCHEMA
     ",\"spacing\":{\"type\":\"number\"}},\"required\":[\"spline\"]}",
     ops_run_spline_sample, NULL, OPS_QUICK},
    {"scatter.create",
     "Create a scatter at 'position' whose box drops seeded copies of a "
     "cooked mesh onto the ground below; 'values' sets the scatter "
     "component (mesh, mesh_index, count up to 2048, seed, extents, "
     "scale_min, scale_max, align_to_surface, random_yaw).",
     "{\"type\":\"object\",\"properties\":{\"position\":" OPS_VEC3_SCHEMA
     ",\"name\":{\"type\":\"string\"},\"values\":{\"type\":\"object\"},"
     "\"container\":" OPS_CONTAINER_SCHEMA "," OPS_REVIEW_SCHEMA "}}",
     NULL, ops_build_scatter_create},
    {"terrain.road",
     "Shape a road 'width' metres wide (default 6) along an existing "
     "spline, following its heights plus 'offset', blending over "
     "'falloff' metres (default 4), as one undo step.",
     "{\"type\":\"object\",\"properties\":{\"terrain\":" OPS_ENTITY_SCHEMA
     ",\"spline\":" OPS_ENTITY_SCHEMA ",\"width\":{\"type\":\"number\"},"
     "\"falloff\":{\"type\":\"number\"},\"offset\":{\"type\":\"number\"}"
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"terrain\",\"spline\"]}",
     NULL, ops_build_terrain_road},
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
     ops_run_io_list, NULL, OPS_QUICK},
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
     "\"op\"]}}," OPS_REVIEW_SCHEMA "},\"required\":[\"ops\"]}",
     ops_run_batch, NULL},
    {"changes.feed",
     "What changed since sequence 'after' (0 for everything kept): batches "
     "applied by any author (the editor's own included), changes accepted "
     "or rejected, and claims set or released, each with its author, "
     "label, objects and box, oldest first. Pass 'next' back as 'after' to "
     "read on; 'missed' means older events left the ring. 'revisions' grow "
     "with every edit, including the designer's. 'wait' (up to 60 s) "
     "answers once something newer happens, an event or any edit, without "
     "holding other requests; pass back the 'revisions' you read so an edit "
     "made since then answers at once.",
     "{\"type\":\"object\",\"properties\":{\"after\":{\"type\":\"integer\","
     "\"minimum\":0},\"limit\":{\"type\":\"integer\",\"minimum\":1,"
     "\"maximum\":128},\"wait\":{\"type\":\"number\",\"minimum\":0,"
     "\"maximum\":60},\"revisions\":{\"type\":\"object\",\"properties\":{"
     "\"scene\":{\"type\":\"integer\"},\"world\":{\"type\":\"integer\"}}}}}",
     ops_run_feed, NULL, OPS_QUICK},
    {"claims.set",
     "Claim a box of a scene: other agents' writes that touch it are "
     "refused (VKR-AGENT-0010) until you release it, so a swarm builds side "
     "by side without conflicts. 'claim' moves one of your claims. Boxes "
     "that only touch do not overlap.",
     "{\"type\":\"object\",\"properties\":{\"region\":{\"type\":\"object\","
     "\"properties\":{\"min\":" OPS_VEC3_SCHEMA ",\"max\":" OPS_VEC3_SCHEMA
     "},\"required\":[\"min\",\"max\"]},\"name\":{\"type\":\"string\"},"
     "\"claim\":{\"type\":\"integer\"},\"container\":" OPS_CONTAINER_SCHEMA
     "," OPS_AGENT_SCHEMA "},\"required\":[\"region\"]}",
     ops_run_claims_set, NULL},
    {"claims.release",
     "Release one of your claims by 'claim', or all of yours.",
     "{\"type\":\"object\",\"properties\":{\"claim\":{\"type\":\"integer\"},"
     "\"all\":{\"type\":\"boolean\"}," OPS_AGENT_SCHEMA "}}",
     ops_run_claims_release, NULL},
    {"claims.list", "Every claim with its author, name, scene and box.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_claims_list, NULL,
     OPS_QUICK},
    {"changes.list", "Pending changes awaiting the designer's review.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_changes_list, NULL,
     OPS_QUICK},
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
    {"undo",
     "Undo the newest edit step. An agent undoes only its own batches; "
     "otherwise it rejects its change.",
     "{\"type\":\"object\",\"properties\":{" OPS_AGENT_SCHEMA "}}",
     ops_run_undo, NULL},
    {"redo", "Redo the next edit step; an agent redoes only its own batches.",
     "{\"type\":\"object\",\"properties\":{" OPS_AGENT_SCHEMA "}}",
     ops_run_undo, NULL},
    {"partition.describe",
     "The open scene's world partition: settings and its known cells, each "
     "loaded, pinned, saved as a document or drawn as a proxy; 'region' (in "
     "metres) narrows the list.",
     "{\"type\":\"object\",\"properties\":{\"region\":{\"type\":"
     "\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA "}}}}",
     ops_run_partition_describe, NULL, OPS_QUICK},
    {"partition.load",
     "Load and pin the world partition cells 'region' (in metres) covers, so "
     "they stay loaded for editing wherever the camera goes.",
     "{\"type\":\"object\",\"properties\":{\"region\":{\"type\":"
     "\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA "}}},\"required\":[\"region\"]}",
     ops_run_partition_change, NULL},
    {"partition.unload",
     "Unpin the cells 'region' covers, or every cell with 'all'; those "
     "without unsaved or undoable edits unload.",
     "{\"type\":\"object\",\"properties\":{\"region\":{\"type\":"
     "\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA "}},\"all\":{\"type\":\"boolean\"}}}",
     ops_run_partition_change, NULL},
    {"query.raycast",
     "First collision surface along a ray: entity, position, normal and "
     "distance.",
     "{\"type\":\"object\",\"properties\":{\"origin\":" OPS_VEC3_SCHEMA
     ",\"direction\":" OPS_VEC3_SCHEMA
     ",\"max_distance\":{\"type\":\"number\",\"exclusiveMinimum\":0},"
     "\"container\":" OPS_CONTAINER_SCHEMA "," OPS_SETTLE_SCHEMA
     "},\"required\":[\"origin\",\"direction\"]}",
     ops_run_raycast, NULL, OPS_QUICK | OPS_SETTLES},
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
     ",\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}"
     "," OPS_SETTLE_SCHEMA "},\"required\":[\"region\"]}",
     ops_run_lint, NULL, OPS_SETTLES},
    {"level.map",
     "The region's floors as text, a floor plan to read instead of a "
     "picture: 'rows' run from min z to max z, each character a cell along "
     "+x from 'first', as a top capture shows them. Each cell shows its "
     "highest walkable floor, else its highest floor, against the player "
     "capsule; set the region's top below ceilings to map the floor under "
     "them. 'cell' sets the cell edge (default: 96 cells along the longer "
     "side, 200 at most); 'heights' adds each shown floor's world y.",
     "{\"type\":\"object\",\"properties\":{\"region\":{\"type\":"
     "\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA
     "},\"required\":[\"min\",\"max\"]},\"start\":" OPS_VEC3_SCHEMA
     ",\"cell\":{\"type\":\"number\",\"exclusiveMinimum\":0},"
     "\"heights\":{\"type\":\"boolean\"},\"container\":" OPS_CONTAINER_SCHEMA
     "," OPS_CAPSULE_SCHEMA "," OPS_SETTLE_SCHEMA
     "},\"required\":[\"region\"]}",
     ops_run_map, NULL, OPS_SETTLES},
    {"query.reachable",
     "Whether the player capsule can walk from one floor point to another, "
     "with the route.",
     "{\"type\":\"object\",\"properties\":{\"from\":" OPS_VEC3_SCHEMA
     ",\"to\":" OPS_VEC3_SCHEMA ",\"container\":" OPS_CONTAINER_SCHEMA
     "," OPS_CAPSULE_SCHEMA "," OPS_SETTLE_SCHEMA
     "},\"required\":[\"from\",\"to\"]}",
     ops_run_reachable, NULL, OPS_SETTLES},
    {"query.bounds", "World bounds of an entity and its descendants.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     "," OPS_SETTLE_SCHEMA "},\"required\":[\"entity\"]}",
     ops_run_bounds, NULL, OPS_QUICK | OPS_SETTLES},
    {"view.capture",
     "Capture the Scene as a PNG, optionally from another view (top is an "
     "orthographic map), framed on an entity or box, from a perspective "
     "'eye' looking at 'target', with grid labels. 'views' takes up to 4 "
     "such views in one sheet of two columns; 'max_width' shrinks the image "
     "(a sheet's whole width); 'marks' draws up to 32 world points (or "
     "{point, label}) as numbered crosses with their labels in every view, "
     "magenta where the camera sees them and blue behind collision, and "
     "answers each one's pixel ('at') and 'hidden', or null where it lies "
     "outside the view.",
     "{\"type\":\"object\",\"properties\":{\"view\":" OPS_CAPTURE_VIEW_SCHEMA
     ",\"focus\":" OPS_CAPTURE_FOCUS_SCHEMA ",\"grid_labels\":{\"type\":"
     "\"boolean\"},\"eye\":" OPS_VEC3_SCHEMA ",\"target\":" OPS_VEC3_SCHEMA
     ",\"views\":{\"type\":\"array\",\"maxItems\":4,\"items\":{\"type\":"
     "\"object\",\"properties\":{\"view\":" OPS_CAPTURE_VIEW_SCHEMA
     ",\"focus\":" OPS_CAPTURE_FOCUS_SCHEMA ",\"grid_labels\":{\"type\":"
     "\"boolean\"},\"eye\":" OPS_VEC3_SCHEMA ",\"target\":" OPS_VEC3_SCHEMA
     "}}},\"max_width\":{\"type\":\"integer\",\"minimum\":64,\"maximum\":"
     "8192},\"marks\":{\"type\":\"array\",\"maxItems\":32,\"items\":{"
     "\"oneOf\":[" OPS_VEC3_SCHEMA ",{\"type\":\"object\",\"properties\":{"
     "\"point\":" OPS_VEC3_SCHEMA ",\"label\":{\"type\":\"string\","
     "\"maxLength\":15}},\"required\":[\"point\"]}]}"
     "},\"area\":{\"type\":\"string\",\"enum\":[\"scene\","
     "\"window\"],\"description\":\"The Scene image (default) or the whole "
     "editor window\"}," OPS_SETTLE_SCHEMA "}}",
     ops_run_capture, NULL, OPS_SETTLES},
    {"view.camera",
     "Place the perspective Scene camera at 'eye' looking at 'target'; "
     "'far' sets the far plane in metres. 'glide' moves it as free flight "
     "does, keeping the lens and temporal history, for motion tests.",
     "{\"type\":\"object\",\"properties\":{\"eye\":" OPS_VEC3_SCHEMA
     ",\"target\":" OPS_VEC3_SCHEMA ",\"far\":{\"type\":\"number\"},"
     "\"glide\":{\"type\":\"boolean\"}},"
     "\"required\":[\"eye\",\"target\"]}",
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
    ops_set(ctx, entry, "settles",
            vkr_bakery_json_bool(arena, (s_ops[i].flags & OPS_SETTLES) != 0u));
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
  ops->capacity = 256u;
  ops->items = calloc(ops->capacity, sizeof(*ops->items));
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
  ops_level_end(ops);
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
  /* A read of collision or built geometry first waits for the rebuilds that
     earlier edits started, so it never checks a stale scene. */
  if (!call->stage && !call->settle_done &&
      (def->flags & (OPS_QUICK | OPS_SETTLES)) &&
      ops_arg_bool(call->args, "settle", (def->flags & OPS_SETTLES) != 0u)) {
    const bool8_t settled = ops_settled(frame, NULL);
    if (!settled && call->settle_frames < OPS_SCENE_SETTLE_FRAMES) {
      call->settle_frames++;
      return VKR_EDITOR_OP_WAIT;
    }
    call->settle_done = true_v;
    call->settle_timeout = !settled;
  }
  const VkrEditorOpStatus status =
      def->run ? def->run(&ctx) : ops_run_write(&ctx, def);
  if (status == VKR_EDITOR_OP_DONE && call->settle_timeout && call->result &&
      call->result->type == VKR_BAKERY_JSON_OBJECT) {
    ops_set(&ctx, call->result, "settled",
            vkr_bakery_json_bool(call->arena, false_v));
  }
  return status;
}

bool8_t vkr_editor_ops_quick(String8 op) {
  const OpsDef *def = ops_find(op);
  return def && (def->flags & OPS_QUICK) != 0u;
}

/* Whether the designer gave any input this frame: a key or button held or
   pressed, the wheel or pointer motion. */
static bool8_t ops_input_active(InputState *input) {
  int32_t dx = 0;
  int32_t dy = 0;
  int32_t wheel = 0;
  input_get_mouse_delta(input, &dx, &dy);
  input_get_mouse_wheel(input, &wheel);
  if (dx || dy || wheel) {
    return true_v;
  }
  /* A tap pressed and released within one frame counts too. */
  for (uint32_t button = 0; button < BUTTON_MAX_BUTTONS; ++button) {
    if (input_is_button_down(input, (Buttons)button) ||
        input_button_just_pressed(input, (Buttons)button)) {
      return true_v;
    }
  }
  for (uint32_t key = 0; key < KEY_MAX_KEYS; ++key) {
    if (input_is_key_down(input, (Keys)key) ||
        input_key_just_pressed(input, (Keys)key)) {
      return true_v;
    }
  }
  return false_v;
}

void vkr_editor_ops_update(VkrEditorOps *ops, const VkrSampleUiFrame *frame) {
  if (ops && frame->input && ops_input_active(frame->input)) {
    ops->input_last = vkr_platform_get_absolute_time();
  }
  /* Claims name boxes of the scene they were made in: a newly loaded scene
     brings its own from the claims file. */
  if (ops && frame->scene && !frame->scene_loading &&
      ops->claims_generation != frame->scene_generation) {
    ops->claims_generation = frame->scene_generation;
    ops_claims_load(ops, frame);
  }
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

/* Feeds the acceptance of `change`. */
static void ops_feed_accept(VkrEditorOps *ops, const VkrEditorChange *change) {
  OpsFeedEvent *event = ops_feed_add(ops, OPS_FEED_ACCEPTED, change->container,
                                     change->author, change->label);
  event->change = change->id;
  for (uint32_t i = 0; i < Min(change->entity_count, OPS_FEED_ENTITIES); ++i) {
    event->entities[event->entity_count++] = change->entities[i];
  }
}

bool8_t vkr_editor_ops_accept(VkrEditorOps *ops, uint32_t id) {
  if (!ops) {
    return false_v;
  }
  if (!id) {
    for (uint32_t i = 0; i < ops->change_count; ++i) {
      ops_feed_accept(ops, &ops->changes[i]);
    }
    ops->change_count = 0u;
    return true_v;
  }
  const int32_t index = ops_change_find(ops, id);
  if (index < 0) {
    return false_v;
  }
  ops_feed_accept(ops, &ops->changes[index]);
  ops_change_remove(ops, (uint32_t)index);
  return true_v;
}

bool8_t vkr_editor_ops_idle(const VkrEditorOps *ops) {
  return !ops || vkr_platform_get_absolute_time() - ops->input_last >=
                     OPS_CAPTURE_IDLE_SECONDS;
}

uint64_t vkr_editor_ops_feed_latest(const VkrEditorOps *ops) {
  return ops ? ops->feed_last : 0u;
}

uint32_t vkr_editor_ops_claim_count(const VkrEditorOps *ops) {
  return ops ? ops->claim_count : 0u;
}

const VkrEditorClaim *vkr_editor_ops_claim(const VkrEditorOps *ops,
                                           uint32_t index) {
  return ops && index < ops->claim_count ? &ops->claims[index] : NULL;
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

/* Authors the window tells apart: chips, and the order that picks their
   colours. */
#define CHANGES_AUTHOR_CHIPS 8u

/* The authors with pending changes, in the order their first change came,
   so the first four keep distinct colours while they have changes. */
typedef struct ChangesAuthors {
  char names[CHANGES_AUTHOR_CHIPS][VKR_EDITOR_AUTHOR_CAPACITY];
  uint32_t counts[CHANGES_AUTHOR_CHIPS];
  uint32_t count;
} ChangesAuthors;

/* An author's pill colour by its place among the authors; the editor's own
   changes are neutral. Amber and red stay with warnings and errors. */
static Vec4 changes_author_color(const ChangesAuthors *authors,
                                 const char *author) {
  const VkrUiTheme *theme = vkr_ui_theme();
  if (!author[0]) {
    return theme->text_secondary;
  }
  const Vec4 colors[] = {
      theme->accent_hover,
      theme->success,
      vkr_ui_color_mix(theme->accent_hover, theme->error, 0.5f),
      vkr_ui_color_mix(theme->info, theme->success, 0.5f),
  };
  uint32_t index = 0u;
  while (index < authors->count && strcmp(authors->names[index], author) != 0) {
    ++index;
  }
  return colors[index % ArrayCount(colors)];
}

static String8 changes_author_name(const VkrEditorChange *change) {
  return change->author[0]
             ? string8_create_from_cstr((const uint8_t *)change->author,
                                        strlen(change->author))
             : string8_lit("Editor");
}

/* Whether `change` passes the window's author filter. */
static bool8_t changes_shown(const VkrEditorOps *ops,
                             const VkrEditorChange *change) {
  return !ops->changes_filter[0] ||
         strcmp(ops->changes_filter, change->author) == 0;
}

/* Whether a claim by `author` passes the window's author filter. */
static bool8_t changes_claim_shown(const VkrEditorOps *ops,
                                   const VkrEditorClaim *claim) {
  return !ops->changes_filter[0] ||
         strcmp(ops->changes_filter, claim->author) == 0;
}

/* The container a change edited, as the Outliner names it. */
static void changes_container(uint16_t container, char *out,
                              uint32_t capacity) {
  if (container == VKR_SCENE_WORLD_ROOT_ID) {
    snprintf(out, capacity, "World");
  } else if (container == 0u) {
    snprintf(out, capacity, "Scene");
  } else {
    snprintf(out, capacity, "Scene slot %u", (uint32_t)container);
  }
}

static void changes_age(float64_t seconds, char *out, uint32_t capacity) {
  if (seconds < 5.0) {
    snprintf(out, capacity, "just now");
  } else if (seconds < 60.0) {
    snprintf(out, capacity, "%.0f s ago", floor(seconds));
  } else if (seconds < 3600.0) {
    snprintf(out, capacity, "%.0f min ago", floor(seconds / 60.0));
  } else {
    snprintf(out, capacity, "%.0f h ago", floor(seconds / 3600.0));
  }
}

typedef enum ChangesButtonKind {
  CHANGES_BUTTON_ACTION = 0,
  CHANGES_BUTTON_PRIMARY,
  /* Reject reverts work and leaves no redo, so it reads as destructive. */
  CHANGES_BUTTON_DANGER,
} ChangesButtonKind;

/* A padded text button with a leading icon, centred in its grid cell. */
static bool8_t changes_button(VkrUiSystem *ui, VkrEditorUi *editor, String8 id,
                              String8 text, VkrUiIcon icon,
                              VkrUiPlacement placement, ChangesButtonKind kind,
                              bool8_t disabled, String8 tooltip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig button = vkr_ui_widget_config_default();
  if (kind == CHANGES_BUTTON_PRIMARY) {
    vkr_editor_primary_style(&button, editor->heading_font);
  } else {
    vkr_editor_action_style(&button, editor->heading_font);
  }
  if (kind == CHANGES_BUTTON_DANGER) {
    button.style.background_color = vkr_ui_color_alpha(theme->error, 0.14f);
    button.style.hover_background_color =
        vkr_ui_color_alpha(theme->error, 0.28f);
    button.style.active_background_color =
        vkr_ui_color_alpha(theme->error, 0.38f);
    button.style.border_color = vkr_ui_color_alpha(theme->error, 0.45f);
    button.icon_color = theme->error;
  }
  button.placement = placement;
  button.placement.align = VKR_UI_ALIGN_CENTER;
  button.style.padding_pt = (VkrUiEdges){4.0f, 12.0f, 4.0f, 10.0f};
  button.style.min_size_pt.y = theme->control_height + 4.0f;
  button.icon = icon;
  button.icon_size_pt = 13.0f;
  button.disabled = disabled;
  button.tooltip = tooltip;
  return vkr_ui_button(ui, id, text, &button);
}

/* Queues the revert of `change` as a request from the editor. Its card
   waits for the answer: a refusal shows there with its reason. */
static void changes_reject(VkrEditorUi *editor, VkrEditorChange *change) {
  char request[160];
  snprintf(request, sizeof(request),
           "{\"v\":1,\"id\":\"changes\",\"op\":\"changes.reject\","
           "\"args\":{\"change\":%u}}",
           change->id);
  change->problem[0] = '\0';
  change->rejecting = vkr_editor_agent_submit(editor->agent, request);
  if (!change->rejecting) {
    snprintf(change->problem, sizeof(change->problem),
             "The request queue is full; try again.");
  }
}

static VkrUiPanelConfig changes_grid(VkrUiPlacement placement,
                                     const VkrUiTrack *columns,
                                     uint32_t column_count,
                                     const VkrUiTrack *rows,
                                     uint32_t row_count) {
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = placement;
  panel.columns = columns;
  panel.column_count = column_count;
  panel.rows = rows;
  panel.row_count = row_count;
  panel.style.padding_pt = (VkrUiEdges){0};
  return panel;
}

static VkrUiPlacement changes_cell(uint32_t column, uint32_t row,
                                   uint32_t row_span) {
  return (VkrUiPlacement){.column = column,
                          .row = row,
                          .column_span = 1u,
                          .row_span = row_span,
                          .justify = VKR_UI_ALIGN_STRETCH,
                          .align = VKR_UI_ALIGN_CENTER};
}

/* The summary, Reject all for a filtered author (two clicks) and Accept
   all. */
static void changes_header(VkrEditorUi *editor, VkrEditorOps *ops,
                           VkrUiSystem *ui, uint32_t shown,
                           uint32_t claims_shown, uint32_t author_count) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.unit = VKR_UI_TRACK_AUTO},
                                {.unit = VKR_UI_TRACK_AUTO}};
  const VkrUiTrack row = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig header = changes_grid(changes_cell(0u, 0u, 1u), columns,
                                         ArrayCount(columns), &row, 1u);
  header.style.gap_pt = theme->space_sm + 2.0f;
  if (!vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    return;
  }
  char text[160];
  char claims[32] = "";
  if (claims_shown) {
    snprintf(claims, sizeof(claims), "  \xc2\xb7  %u claim%s", claims_shown,
             claims_shown == 1u ? "" : "s");
  }
  if (ops->changes_filter[0]) {
    snprintf(text, sizeof(text), "%u change%s from %s%s", shown,
             shown == 1u ? "" : "s", ops->changes_filter, claims);
  } else {
    snprintf(text, sizeof(text), "%u change%s to review from %u author%s%s",
             shown, shown == 1u ? "" : "s", author_count,
             author_count == 1u ? "" : "s", claims);
  }
  VkrUiWidgetConfig summary =
      vkr_editor_text_config(theme->font_body, theme->text);
  summary.placement = changes_cell(0u, 0u, 1u);
  summary.placement.justify = VKR_UI_ALIGN_START;
  summary.text.font = editor->heading_font;
  summary.icon = VKR_UI_ICON_TERMINAL;
  summary.icon_size_pt = 15.0f;
  summary.icon_color = theme->accent_hover;
  vkr_ui_label(ui, string8_lit("summary"),
               string8_create_from_cstr((const uint8_t *)text, strlen(text)),
               &summary);
  const float64_t now = vkr_platform_get_absolute_time();
  if (ops->changes_filter[0] && shown) {
    const bool8_t armed = now < ops->reject_all_armed_until;
    char label[48];
    snprintf(label, sizeof(label), armed ? "Confirm reject %u" : "Reject %u",
             shown);
    if (changes_button(
            ui, editor, string8_lit("reject_all"),
            string8_create_from_cstr((const uint8_t *)label, strlen(label)),
            VKR_UI_ICON_CLOSE, changes_cell(1u, 0u, 1u), CHANGES_BUTTON_DANGER,
            false_v,
            string8_lit("Revert every change of this author, newest first. "
                        "Rejected work leaves the undo history; click again "
                        "within 3 s to confirm."))) {
      if (!armed) {
        ops->reject_all_armed_until = now + 3.0;
      } else {
        ops->reject_all_armed_until = 0.0;
        /* Newest first, so a change never waits on a later one of its
           own author. */
        for (uint32_t i = ops->change_count; i-- > 0u;) {
          VkrEditorChange *change = &ops->changes[i];
          if (changes_shown(ops, change) && !change->rejecting) {
            changes_reject(editor, change);
          }
        }
      }
    }
  }
  if (changes_button(ui, editor, string8_lit("accept_all"),
                     ops->changes_filter[0] ? string8_lit("Accept shown")
                                            : string8_lit("Accept all"),
                     VKR_UI_ICON_CHECK, changes_cell(2u, 0u, 1u),
                     CHANGES_BUTTON_PRIMARY, !shown,
                     string8_lit("Keep these changes and clear their marks"))) {
    for (uint32_t i = ops->change_count; i-- > 0u;) {
      if (changes_shown(ops, &ops->changes[i]) && !ops->changes[i].rejecting) {
        (void)vkr_editor_ops_accept(ops, ops->changes[i].id);
      }
    }
  }
  (void)vkr_ui_panel_end(ui);
}

/* All, then one chip per author with its count; a click filters the list. */
static void changes_chips(VkrEditorOps *ops, VkrUiSystem *ui,
                          const ChangesAuthors *authors) {
  const uint32_t author_count = authors->count;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiTrack columns[CHANGES_AUTHOR_CHIPS + 2u];
  for (uint32_t i = 0; i <= author_count; ++i) {
    columns[i] = (VkrUiTrack){.unit = VKR_UI_TRACK_AUTO};
  }
  columns[author_count + 1u] =
      (VkrUiTrack){.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack row = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig chips = changes_grid(changes_cell(0u, 1u, 1u), columns,
                                        author_count + 2u, &row, 1u);
  chips.style.gap_pt = theme->space_sm;
  chips.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("chips"), &chips)) {
    return;
  }
  uint32_t total = 0u;
  for (uint32_t i = 0; i < author_count; ++i) {
    total += authors->counts[i];
  }
  for (uint32_t i = 0; i <= author_count; ++i) {
    const char *author = i ? authors->names[i - 1u] : "";
    const bool8_t selected = strcmp(ops->changes_filter, author) == 0;
    VkrUiWidgetConfig chip = vkr_ui_widget_config_default();
    vkr_editor_ghost_style(&chip);
    chip.placement = changes_cell(i, 0u, 1u);
    chip.placement.justify = VKR_UI_ALIGN_START;
    chip.style.min_size_pt.y = 22.0f;
    chip.style.padding_pt = (VkrUiEdges){2.0f, 9.0f, 2.0f, 8.0f};
    chip.style.corner_radius_pt = (Vec4){11.0f, 11.0f, 11.0f, 11.0f};
    chip.style.text_color = selected ? theme->text : theme->text_secondary;
    if (selected) {
      chip.style.background_color = theme->raised;
      chip.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
      chip.style.border_color = theme->border_strong;
    }
    if (i) {
      chip.icon = VKR_UI_ICON_DOT;
      chip.icon_size_pt = 12.0f;
      chip.icon_color = changes_author_color(authors, author);
    }
    const String8 text =
        i ? string8_create_formatted(ui->frame_allocator, "%s  %u",
                                     author[0] ? author : "Editor",
                                     authors->counts[i - 1u])
          : string8_create_formatted(ui->frame_allocator, "All  %u", total);
    chip.tooltip = i ? string8_lit("Show only this author's changes")
                     : string8_lit("Show every author's changes");
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("chip"), text, &chip)) {
      snprintf(ops->changes_filter, sizeof(ops->changes_filter), "%s", author);
      ops->reject_all_armed_until = 0.0;
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

/* Height of a change card: its title and meta rows (and a refused
   reject's reason), the gaps between them, padding and border. */
static float32_t changes_card_height(const VkrEditorChange *change) {
  return change->problem[0] ? 22.0f + 18.0f + 20.0f + 12.0f + 18.0f
                            : 22.0f + 18.0f + 6.0f + 18.0f;
}

/* One change: its author and label, what it touched and when, a refused
   reject's reason, and Focus, Reject and Accept. */
static void changes_card(VkrEditorUi *editor, VkrEditorOps *ops,
                         const VkrSampleUiFrame *frame, VkrUiSystem *ui,
                         const ChangesAuthors *authors, VkrEditorChange *change,
                         uint32_t row, float64_t now) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 86.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 86.0f, .unit = VKR_UI_TRACK_PX}};
  const VkrUiTrack rows[] = {{.value = 22.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 18.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 20.0f, .unit = VKR_UI_TRACK_PX}};
  const uint32_t row_count = change->problem[0] ? 3u : 2u;
  VkrUiPanelConfig card = changes_grid(changes_cell(0u, row, 1u), columns,
                                       ArrayCount(columns), rows, row_count);
  card.placement.align = VKR_UI_ALIGN_STRETCH;
  card.style.padding_pt = (VkrUiEdges){8.0f, 10.0f, 8.0f, 12.0f};
  card.style.gap_pt = theme->space_sm + 2.0f;
  card.style.background_color = theme->raised;
  card.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  card.style.border_color = change->problem[0]
                                ? vkr_ui_color_alpha(theme->error, 0.55f)
                                : theme->border;
  card.style.corner_radius_pt =
      (Vec4){theme->radius_large, theme->radius_large, theme->radius_large,
             theme->radius_large};
  card.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("card"), &card)) {
    return;
  }
  const VkrUiTrack title_columns[] = {{.unit = VKR_UI_TRACK_AUTO},
                                      {.value = 1.0f, .unit = VKR_UI_TRACK_FR}};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig title = changes_grid(changes_cell(0u, 0u, 1u), title_columns,
                                        ArrayCount(title_columns), &fill, 1u);
  title.style.gap_pt = theme->space_md;
  title.clip_children = true_v;
  if (vkr_ui_panel_begin(ui, string8_lit("title"), &title)) {
    const Vec4 color = changes_author_color(authors, change->author);
    VkrUiWidgetConfig pill = vkr_editor_text_config(theme->font_caption, color);
    pill.placement = changes_cell(0u, 0u, 1u);
    pill.placement.justify = VKR_UI_ALIGN_START;
    pill.style.padding_pt = (VkrUiEdges){1.0f, 8.0f, 1.0f, 8.0f};
    pill.style.background_color = vkr_ui_color_alpha(color, 0.18f);
    pill.style.corner_radius_pt = (Vec4){9.0f, 9.0f, 9.0f, 9.0f};
    pill.text.font = editor->heading_font;
    pill.center = true_v;
    vkr_ui_label(ui, string8_lit("author"), changes_author_name(change), &pill);
    const String8 label = string8_create_from_cstr(
        (const uint8_t *)change->label, strlen(change->label));
    VkrUiWidgetConfig name =
        vkr_editor_text_config(theme->font_body, theme->text);
    name.placement = changes_cell(1u, 0u, 1u);
    name.placement.justify = VKR_UI_ALIGN_START;
    name.text.font = editor->heading_font;
    name.tooltip = label;
    vkr_ui_label(ui, string8_lit("label"), label, &name);
    (void)vkr_ui_panel_end(ui);
  }
  char where[32];
  char age[32];
  changes_container(change->container, where, sizeof(where));
  changes_age(now - change->created, age, sizeof(age));
  VkrUiWidgetConfig meta =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  meta.placement = changes_cell(0u, 1u, 1u);
  meta.placement.justify = VKR_UI_ALIGN_START;
  vkr_ui_label(ui, string8_lit("meta"),
               string8_create_formatted(
                   ui->frame_allocator,
                   "%u object%s  \xc2\xb7  %s  \xc2\xb7  %s",
                   change->entity_count, change->entity_count == 1u ? "" : "s",
                   where, age),
               &meta);
  if (change->problem[0]) {
    const String8 problem = string8_create_from_cstr(
        (const uint8_t *)change->problem, strlen(change->problem));
    VkrUiWidgetConfig why =
        vkr_editor_text_config(theme->font_caption, theme->error);
    why.placement = changes_cell(0u, 2u, 1u);
    why.placement.column_span = ArrayCount(columns);
    why.placement.justify = VKR_UI_ALIGN_START;
    why.icon = VKR_UI_ICON_WARNING_FILL;
    why.icon_size_pt = 12.0f;
    why.icon_color = theme->error;
    why.tooltip = problem;
    vkr_ui_label(ui, string8_lit("problem"), problem, &why);
  }
  VkrUiWidgetConfig focus = vkr_editor_icon_button_config(
      1u, 0u, VKR_UI_ICON_FRAME,
      string8_lit("Frame this change's objects in the Scene"));
  focus.placement.row_span = 2u;
  if (vkr_ui_button(ui, string8_lit("focus"), (String8){0}, &focus)) {
    ops_change_focus(frame, change);
  }
  if (changes_button(
          ui, editor, string8_lit("reject"), string8_lit("Reject"),
          change->rejecting ? VKR_UI_ICON_SPINNER : VKR_UI_ICON_CLOSE,
          changes_cell(2u, 0u, 2u), CHANGES_BUTTON_DANGER, change->rejecting,
          string8_lit("Revert this change; refused while a later "
                      "edit depends on it"))) {
    changes_reject(editor, change);
  }
  if (changes_button(ui, editor, string8_lit("accept"), string8_lit("Accept"),
                     VKR_UI_ICON_CHECK, changes_cell(3u, 0u, 2u),
                     CHANGES_BUTTON_PRIMARY, change->rejecting,
                     string8_lit("Keep this change and clear its mark"))) {
    (void)vkr_editor_ops_accept(ops, change->id);
  }
  (void)vkr_ui_panel_end(ui);
}

/* One claim: its author and name, its size and scene, Focus and Release.
   The designer releases a claim an agent left, as after it stopped. */
static void changes_claim_card(VkrEditorUi *editor, VkrEditorOps *ops,
                               const VkrSampleUiFrame *frame, VkrUiSystem *ui,
                               const ChangesAuthors *authors,
                               uint32_t claim_index, uint32_t row) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrEditorClaim claim = ops->claims[claim_index];
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 178.0f, .unit = VKR_UI_TRACK_PX}};
  const VkrUiTrack rows[] = {{.value = 22.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 18.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig card =
      changes_grid(changes_cell(0u, row, 1u), columns, ArrayCount(columns),
                   rows, ArrayCount(rows));
  card.placement.align = VKR_UI_ALIGN_STRETCH;
  card.style.padding_pt = (VkrUiEdges){8.0f, 10.0f, 8.0f, 12.0f};
  card.style.gap_pt = theme->space_sm + 2.0f;
  card.style.background_color = vkr_ui_color_alpha(theme->info, 0.06f);
  card.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  card.style.border_color = vkr_ui_color_alpha(theme->info, 0.45f);
  card.style.corner_radius_pt =
      (Vec4){theme->radius_large, theme->radius_large, theme->radius_large,
             theme->radius_large};
  card.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("claim"), &card)) {
    return;
  }
  const VkrUiTrack title_columns[] = {{.unit = VKR_UI_TRACK_AUTO},
                                      {.value = 1.0f, .unit = VKR_UI_TRACK_FR}};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig title = changes_grid(changes_cell(0u, 0u, 1u), title_columns,
                                        ArrayCount(title_columns), &fill, 1u);
  title.style.gap_pt = theme->space_md;
  title.clip_children = true_v;
  if (vkr_ui_panel_begin(ui, string8_lit("title"), &title)) {
    const Vec4 color = changes_author_color(authors, claim.author);
    VkrUiWidgetConfig pill = vkr_editor_text_config(theme->font_caption, color);
    pill.placement = changes_cell(0u, 0u, 1u);
    pill.placement.justify = VKR_UI_ALIGN_START;
    pill.style.padding_pt = (VkrUiEdges){1.0f, 8.0f, 1.0f, 8.0f};
    pill.style.background_color = vkr_ui_color_alpha(color, 0.18f);
    pill.style.corner_radius_pt = (Vec4){9.0f, 9.0f, 9.0f, 9.0f};
    pill.text.font = editor->heading_font;
    pill.center = true_v;
    vkr_ui_label(ui, string8_lit("author"),
                 string8_create_from_cstr((const uint8_t *)claim.author,
                                          strlen(claim.author)),
                 &pill);
    VkrUiWidgetConfig name =
        vkr_editor_text_config(theme->font_body, theme->text);
    name.placement = changes_cell(1u, 0u, 1u);
    name.placement.justify = VKR_UI_ALIGN_START;
    name.text.font = editor->heading_font;
    name.icon = VKR_UI_ICON_LOCK;
    name.icon_size_pt = 13.0f;
    name.icon_color = theme->info;
    vkr_ui_label(
        ui, string8_lit("name"),
        string8_create_formatted(ui->frame_allocator, "Claims '%s'",
                                 claim.name[0] ? claim.name : "a region"),
        &name);
    (void)vkr_ui_panel_end(ui);
  }
  char where[32];
  changes_container(claim.container, where, sizeof(where));
  VkrUiWidgetConfig meta =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  meta.placement = changes_cell(0u, 1u, 1u);
  meta.placement.justify = VKR_UI_ALIGN_START;
  vkr_ui_label(
      ui, string8_lit("meta"),
      string8_create_formatted(
          ui->frame_allocator,
          "%.0f \xc3\x97 %.0f \xc3\x97 %.0f m  \xc2\xb7  %s  \xc2\xb7  other "
          "agents' writes inside are refused",
          (float64_t)(claim.max.x - claim.min.x),
          (float64_t)(claim.max.y - claim.min.y),
          (float64_t)(claim.max.z - claim.min.z), where),
      &meta);
  VkrUiWidgetConfig focus = vkr_editor_icon_button_config(
      1u, 0u, VKR_UI_ICON_FRAME, string8_lit("Frame this claim in the Scene"));
  focus.placement.row_span = 2u;
  if (vkr_ui_button(ui, string8_lit("focus"), (String8){0}, &focus) &&
      frame->view_request) {
    frame->view_request->frame_box = true_v;
    frame->view_request->frame_min = claim.min;
    frame->view_request->frame_max = claim.max;
  }
  VkrUiPlacement release = changes_cell(2u, 0u, 2u);
  release.justify = VKR_UI_ALIGN_END;
  if (changes_button(ui, editor, string8_lit("release"),
                     string8_lit("Release claim"), VKR_UI_ICON_UNLOCK, release,
                     CHANGES_BUTTON_ACTION, false_v,
                     string8_lit("Free this region for every agent, as when "
                                 "its agent stopped"))) {
    ops_claim_release_at(ops, claim_index);
    ops_claims_save(ops, frame);
  }
  (void)vkr_ui_panel_end(ui);
}

/* Nothing to review: what the window is for and where agents connect. */
static void changes_empty(VkrEditorUi *editor, VkrUiSystem *ui) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                             {.value = 34.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 24.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 18.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 18.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 1.2f, .unit = VKR_UI_TRACK_FR}};
  VkrUiPanelConfig empty = changes_grid(changes_cell(0u, 0u, 1u), &column, 1u,
                                        rows, ArrayCount(rows));
  empty.placement.align = VKR_UI_ALIGN_STRETCH;
  empty.style.padding_pt = (VkrUiEdges){0.0f, 24.0f, 0.0f, 24.0f};
  if (!vkr_ui_panel_begin(ui, string8_lit("empty"), &empty)) {
    return;
  }
  VkrUiWidgetConfig icon =
      vkr_editor_text_config(theme->font_body, theme->success);
  icon.placement = changes_cell(0u, 1u, 1u);
  icon.placement.justify = VKR_UI_ALIGN_CENTER;
  icon.icon = VKR_UI_ICON_CHECK_CIRCLE;
  icon.icon_size_pt = 26.0f;
  icon.icon_color = theme->success;
  vkr_ui_label(ui, string8_lit("icon"), (String8){0}, &icon);
  VkrUiWidgetConfig title =
      vkr_editor_text_config(theme->font_emphasis, theme->text);
  title.placement = changes_cell(0u, 2u, 1u);
  title.placement.justify = VKR_UI_ALIGN_CENTER;
  title.text.font = editor->heading_font;
  title.center = true_v;
  vkr_ui_label(ui, string8_lit("title"), string8_lit("Nothing to review"),
               &title);
  VkrUiWidgetConfig body =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  body.placement = changes_cell(0u, 3u, 1u);
  body.placement.justify = VKR_UI_ALIGN_CENTER;
  body.center = true_v;
  vkr_ui_label(ui, string8_lit("about"),
               string8_lit("Agent edits made with review appear here, each "
                           "with its author."),
               &body);
  /* The socket path can be long; the tooltip holds all of it. */
  const char *status = vkr_editor_agent_status(editor->agent);
  const String8 text =
      string8_create_from_cstr((const uint8_t *)status, strlen(status));
  body.placement.row = 4u;
  body.style.text_color = theme->text_disabled;
  body.tooltip = text;
  vkr_ui_label(ui, string8_lit("status"), text, &body);
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_changes_build(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, VkrUiRect bounds) {
  (void)bounds;
  VkrUiSystem *ui = frame->ui;
  VkrEditorOps *ops = vkr_editor_agent_ops(editor->agent);
  if (!ops) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  ChangesAuthors authors = {0};
  bool8_t filter_found = false_v;
  for (uint32_t i = 0; i < ops->change_count + ops->claim_count; ++i) {
    const char *author = i < ops->change_count
                             ? ops->changes[i].author
                             : ops->claims[i - ops->change_count].author;
    filter_found = filter_found || strcmp(ops->changes_filter, author) == 0;
    uint32_t a = 0u;
    while (a < authors.count && strcmp(authors.names[a], author) != 0) {
      ++a;
    }
    if (a == authors.count && authors.count < CHANGES_AUTHOR_CHIPS) {
      snprintf(authors.names[authors.count++], VKR_EDITOR_AUTHOR_CAPACITY, "%s",
               author);
    }
    if (a < authors.count) {
      authors.counts[a]++;
    }
  }
  if (ops->changes_filter[0] && !filter_found) {
    ops->changes_filter[0] = '\0';
  }
  /* Ids first: Accept and a finished reject remove changes while the cards
     build, so each card looks its change up again. Newest first. */
  uint32_t ids[VKR_EDITOR_CHANGE_MAX];
  VkrUiTrack card_rows[VKR_EDITOR_CHANGE_MAX + VKR_EDITOR_CLAIM_MAX];
  uint32_t shown = 0u;
  for (uint32_t i = ops->change_count; i-- > 0u;) {
    if (changes_shown(ops, &ops->changes[i])) {
      ids[shown] = ops->changes[i].id;
      card_rows[shown++] =
          (VkrUiTrack){.value = changes_card_height(&ops->changes[i]),
                       .unit = VKR_UI_TRACK_PX};
    }
  }
  /* Claims list after the changes, by id, as the filter shows them. */
  uint32_t claim_ids[VKR_EDITOR_CLAIM_MAX];
  uint32_t claims_shown = 0u;
  for (uint32_t i = 0; i < ops->claim_count; ++i) {
    if (changes_claim_shown(ops, &ops->claims[i])) {
      claim_ids[claims_shown] = ops->claims[i].id;
      card_rows[shown + claims_shown++] = (VkrUiTrack){
          .value = 22.0f + 18.0f + 6.0f + 18.0f, .unit = VKR_UI_TRACK_PX};
    }
  }
  const bool8_t chips = authors.count > 1u;
  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const bool8_t any = ops->change_count || ops->claim_count;
  const VkrUiTrack rows[] = {
      {.value = any ? 34.0f : 0.0f, .unit = VKR_UI_TRACK_PX},
      {.value = chips ? 26.0f : 0.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR}};
  VkrUiPanelConfig body = changes_grid(changes_cell(0u, 0u, 1u), &column, 1u,
                                       rows, ArrayCount(rows));
  body.placement.align = VKR_UI_ALIGN_STRETCH;
  body.style.padding_pt = (VkrUiEdges){10.0f, 12.0f, 10.0f, 12.0f};
  body.style.gap_pt = theme->space_md;
  body.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("changes.body"), &body)) {
    return;
  }
  if (!any) {
    VkrUiPanelConfig area =
        changes_grid(changes_cell(0u, 2u, 1u), &column, 1u, &column, 1u);
    area.placement.align = VKR_UI_ALIGN_STRETCH;
    if (vkr_ui_panel_begin(ui, string8_lit("area"), &area)) {
      changes_empty(editor, ui);
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_panel_end(ui);
    return;
  }
  changes_header(editor, ops, ui, shown, claims_shown, authors.count);
  if (chips) {
    changes_chips(ops, ui, &authors);
  }
  VkrUiPanelConfig list =
      changes_grid(changes_cell(0u, 2u, 1u), &column, 1u, card_rows,
                   Max(shown + claims_shown, 1u));
  list.placement.align = VKR_UI_ALIGN_STRETCH;
  list.style.gap_pt = theme->space_sm + 2.0f;
  if (vkr_ui_scroll_area_begin(ui, string8_lit("cards"), &list)) {
    const float64_t now = vkr_platform_get_absolute_time();
    for (uint32_t i = 0; i < shown; ++i) {
      const int32_t index = ops_change_find(ops, ids[i]);
      if (index < 0) {
        continue;
      }
      (void)vkr_ui_push_id_u64(ui, ids[i]);
      changes_card(editor, ops, frame, ui, &authors, &ops->changes[index], i,
                   now);
      (void)vkr_ui_pop_id(ui);
    }
    for (uint32_t i = 0; i < claims_shown; ++i) {
      for (uint32_t c = 0; c < ops->claim_count; ++c) {
        if (ops->claims[c].id != claim_ids[i]) {
          continue;
        }
        /* Claim ids and change ids share the id stack; claims sit apart. */
        (void)vkr_ui_push_id_u64(ui, 0x100000000ull + claim_ids[i]);
        changes_claim_card(editor, ops, frame, ui, &authors, c, shown + i);
        (void)vkr_ui_pop_id(ui);
        break;
      }
    }
    (void)vkr_ui_scroll_area_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}
