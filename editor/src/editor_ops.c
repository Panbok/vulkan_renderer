#include "editor_ops.h"

#include "editor_agent.h"
#include "editor_environment.h"
#include "editor_internal.h"
#include "editor_level.h"
#include "editor_lighting.h"
#include "editor_material.h"
#include "editor_projects.h"
#include "editor_session.h"
#include "editor_workbench.h"

#include "assets/vkr_light_layers.h"
#include "assets/vkr_material_codegen.h"
#include "core/logger.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"
#include "filesystem/filesystem.h"
#include "level/vkr_brush.h"
#include "renderer/resources/loaders/material_loader.h"
#include "filesystem/vkr_asset_path.h"
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
  /* Another editor's agent batch its author reverted after it applied. */
  OPS_FEED_REVERTED,
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

typedef struct OpsTaskSlots {
  char editor[VKR_EDITOR_SESSION_NAME_MAX];
  uint32_t slots;
} OpsTaskSlots;

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
  /* The task board (task.*). */
  VkrEditorTask tasks[VKR_EDITOR_TASK_MAX];
  uint32_t task_count;
  uint32_t next_task_id;
  /* How many tasks each editor's agents may hold at once (task.slots), by
     the editor's session name; empty outside a session. */
  OpsTaskSlots task_slots[VKR_EDITOR_SESSION_PEERS_MAX];
  uint32_t task_slot_count;
  /* Grows with every change of claims or tasks. A session host sends them
     when it differs from what it sent in this session (`published_*`); a
     participant shows the host's copy `mirror_revision` instead of its own
     (ADR-106). */
  uint64_t shared_revision;
  uint64_t published_epoch;
  uint64_t published_revision;
  bool8_t mirroring;
  uint64_t mirror_epoch;
  uint64_t mirror_revision;
  /* The editor's session, from the last update, so an accept reaches the
     other editors. */
  VkrEditorSession *session;
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
  /* The first edit each operation added; its edits run to the next one's. */
  uint32_t op_first[VKR_SAMPLE_EDIT_BATCH_MAX];
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
    uint32_t found = ops_find_named(ctx->frame, text, &entity);
    /* An object an earlier operation of this batch creates answers to its
       name too, so a room and its doorways fit one batch. */
    int32_t made = -1;
    for (uint32_t i = 0; batch && i < batch->count; ++i) {
      const VkrSampleEditBatchItem *item = &ctx->ops->items[i];
      const char *name = item->request.values.name;
      if (item->request.action == VKR_SCENE_EDIT_CREATE &&
          (item->request.values.fields & VKR_SCENE_EDIT_NAME) &&
          strlen(name) == text.length &&
          MemCompare(name, text.str, text.length) == 0) {
        made = (int32_t)i;
        found++;
      }
    }
    if (found > 1u) {
      return ops_fail(ctx, OPS_NOT_FOUND,
                      "%u entities are named '%.*s'; use an id", found,
                      (int)text.length, text.str);
    }
    if (!found) {
      return ops_fail(ctx, OPS_NOT_FOUND, "No entity is named '%.*s'",
                      (int)text.length, text.str);
    }
    if (made >= 0) {
      out->item = made;
      out->container = (uint16_t)batch->container;
      return true_v;
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
  /* A participant shows the session host's claims, which are not its own
     to keep. */
  if (ops->mirroring || !ops_claims_path(frame, path, sizeof(path))) {
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

/* A brush as its faces read it, after the batch's earlier edits: the
   brush and each face are scene entities or items the batch creates. */
typedef struct OpsBrushRead {
  OpsRef ref;
  OpsRef parent;
  /* Local pose and world matrix by value: a component pointer would move
     with the entity's row. */
  SceneTransform transform;
  char name[VKR_SCENE_EDIT_NAME_CAPACITY];
  SceneBrushRole role;
  uint32_t count;
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  OpsRef faces[VKR_BRUSH_FACE_MAX];
  SceneBrushFace values[VKR_BRUSH_FACE_MAX];
} OpsBrushRead;

static bool8_t ops_ref_same(const OpsRef *a, const OpsRef *b) {
  return a->item >= 0 ? a->item == b->item
                      : b->item < 0 && a->entity.u64 == b->entity.u64;
}

/* Whether batch edit `item` acts on `ref`. */
static bool8_t ops_item_names(const VkrSampleEditBatchItem *item,
                              const OpsRef *ref) {
  return ref->item >= 0 ? item->entity_ref == ref->item
                        : item->entity_ref < 0 &&
                              item->request.entity.u64 == ref->entity.u64;
}

/* Whether batch creation `item` goes under `ref`. */
static bool8_t ops_item_under(const VkrSampleEditBatchItem *item,
                              const OpsRef *ref) {
  return ref->item >= 0 ? item->parent_ref == ref->item
                        : item->parent_ref < 0 &&
                              item->request.parent.u64 == ref->entity.u64;
}

/* An object's values and parent after the batch's earlier edits: what the
   scene or its creation holds, with each later set, reparent and delete of
   the batch applied. False when it does not exist then. */
static bool8_t ops_view(OpsContext *ctx, const OpsBatch *batch,
                        const OpsRef *ref, VkrSceneEditValues *out,
                        OpsRef *out_parent) {
  const VkrScene *scene = ops_scene(ctx->frame, ref->container);
  uint32_t first = 0u;
  *out_parent = (OpsRef){.item = -1, .container = ref->container};
  if (ref->item >= 0) {
    const VkrSampleEditBatchItem *made = &ctx->ops->items[ref->item];
    *out = made->request.values;
    out_parent->entity = made->request.parent;
    out_parent->item = made->parent_ref;
    first = (uint32_t)ref->item + 1u;
  } else {
    if (!scene || !vkr_scene_edit_read(scene, ref->entity, out)) {
      return false_v;
    }
    out_parent->entity = ops_parent(scene, ref->entity);
  }
  for (uint32_t i = first; batch && i < batch->count; ++i) {
    const VkrSampleEditBatchItem *item = &ctx->ops->items[i];
    if (!ops_item_names(item, ref)) {
      continue;
    }
    const VkrSceneEditValues *v = &item->request.values;
    switch (item->request.action) {
    case VKR_SCENE_EDIT_DELETE:
      return false_v;
    case VKR_SCENE_EDIT_REPARENT:
      out_parent->entity = item->request.parent;
      out_parent->item = item->parent_ref;
      break;
    case VKR_SCENE_EDIT_APPLY:
      if (v->fields & VKR_SCENE_EDIT_NAME) {
        MemCopy(out->name, v->name, sizeof(out->name));
      }
      if (v->fields & VKR_SCENE_EDIT_TRANSFORM) {
        out->position = v->position;
        out->rotation = v->rotation;
        out->scale = v->scale;
      }
      if ((v->fields & VKR_SCENE_EDIT_COMPONENT) && v->component_type) {
        out->fields |= VKR_SCENE_EDIT_COMPONENT;
        out->component_type = v->component_type;
        MemCopy(out->component, v->component, v->component_type->size);
      }
      break;
    default:
      break;
    }
  }
  return true_v;
}

/* World matrix of `ref` after the batch's earlier edits. */
static bool8_t ops_view_world(OpsContext *ctx, const OpsBatch *batch,
                              const OpsRef *ref, Mat4 *out, uint32_t depth) {
  VkrSceneEditValues values;
  OpsRef parent;
  if (depth > 64u || !ops_view(ctx, batch, ref, &values, &parent)) {
    return false_v;
  }
  const Mat4 local =
      mat4_mul(mat4_mul(mat4_translate(values.position),
                        vkr_quat_to_mat4(vkr_quat_normalize(values.rotation))),
               mat4_scale(values.scale));
  Mat4 parent_world = mat4_identity();
  if ((parent.item >= 0 || parent.entity.u64) &&
      !ops_view_world(ctx, batch, &parent, &parent_world, depth + 1u)) {
    return false_v;
  }
  *out = mat4_mul(parent_world, local);
  return true_v;
}

/* The face values of `ref` after the batch's earlier edits, or NULL when it
   is no face then. */
static const SceneBrushFace *ops_view_face(OpsContext *ctx,
                                           const OpsBatch *batch,
                                           const OpsRef *ref,
                                           VkrSceneEditValues *scratch) {
  OpsRef parent;
  if (!ops_view(ctx, batch, ref, scratch, &parent)) {
    return NULL;
  }
  if ((scratch->fields & VKR_SCENE_EDIT_COMPONENT) &&
      scratch->component_type == &vkr_scene_brush_face_type) {
    return (const SceneBrushFace *)scratch->component;
  }
  return ref->item < 0
             ? vkr_scene_get_typed(ops_scene(ctx->frame, ref->container),
                                   ref->entity, &vkr_scene_brush_face_type)
             : NULL;
}

/* Reads brush `ref` as the batch leaves it before the current operation,
   so an operation may edit a brush the same batch made or changed. */
static bool8_t ops_brush_read(OpsContext *ctx, const OpsBatch *batch,
                              const OpsRef *ref, OpsBrushRead *out) {
  VkrSceneEditValues *values =
      arena_alloc(ops_arena(ctx), sizeof(*values), ARENA_MEMORY_TAG_STRUCT);
  if (!values) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref->container);
  OpsRef parent;
  if (!ops_view(ctx, batch, ref, values, &parent)) {
    return ops_fail(ctx, OPS_INVALID, "That brush does not exist");
  }
  const SceneBrushSettings *settings =
      (values->fields & VKR_SCENE_EDIT_COMPONENT) &&
              values->component_type == &vkr_scene_brush_type
          ? (const SceneBrushSettings *)values->component
      : ref->item < 0
          ? vkr_scene_get_typed(scene, ref->entity, &vkr_scene_brush_type)
          : NULL;
  if (!settings) {
    return ops_fail(ctx, OPS_INVALID, "That entity is not a brush");
  }
  out->ref = *ref;
  out->parent = parent;
  out->role = settings->role;
  MemCopy(out->name, values->name, sizeof(out->name));
  out->transform = (SceneTransform){.position = values->position,
                                    .rotation = values->rotation,
                                    .scale = values->scale,
                                    .parent = parent.entity};
  if (!ops_view_world(ctx, batch, ref, &out->transform.world, 0u)) {
    return ops_fail(ctx, OPS_INVALID, "The brush's pose cannot be read");
  }
  /* Faces: the scene's, then those the batch made under it; any the batch
     deleted are gone. */
  OpsRef faces[2u * VKR_BRUSH_FACE_MAX];
  uint32_t candidates = 0u;
  if (ref->item < 0) {
    VkrEntityId ids[VKR_BRUSH_FACE_MAX + 1u];
    const uint32_t total =
        vkr_scene_brush_faces(scene, ref->entity, ids, ArrayCount(ids));
    if (total > VKR_BRUSH_FACE_MAX) {
      return ops_fail(ctx, OPS_INVALID, "The brush has more than %u faces",
                      VKR_BRUSH_FACE_MAX);
    }
    for (uint32_t i = 0; i < total; ++i) {
      faces[candidates++] =
          (OpsRef){.entity = ids[i], .item = -1, .container = ref->container};
    }
  }
  for (uint32_t i = 0; batch && i < batch->count; ++i) {
    const VkrSampleEditBatchItem *item = &ctx->ops->items[i];
    if (item->request.action == VKR_SCENE_EDIT_CREATE &&
        item->request.values.component_type == &vkr_scene_brush_face_type &&
        ops_item_under(item, ref)) {
      if (candidates == ArrayCount(faces)) {
        return ops_fail(ctx, OPS_INVALID, "The brush has more than %u faces",
                        VKR_BRUSH_FACE_MAX);
      }
      faces[candidates++] =
          (OpsRef){.item = (int32_t)i, .container = ref->container};
    }
  }
  out->count = 0u;
  for (uint32_t i = 0; i < candidates; ++i) {
    const SceneBrushFace *face = ops_view_face(ctx, batch, &faces[i], values);
    if (!face) {
      continue;
    }
    if (out->count == VKR_BRUSH_FACE_MAX) {
      return ops_fail(ctx, OPS_INVALID, "The brush has more than %u faces",
                      VKR_BRUSH_FACE_MAX);
    }
    out->faces[out->count] = faces[i];
    out->values[out->count] = *face;
    out->planes[out->count] =
        (VkrBrushPlane){.normal = face->normal, .distance = face->distance};
    out->count++;
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
  return ops_brush_read(ctx, batch, &ref, out);
}

/* Points batch edit `item` at the entity `ref` names. */
static void ops_item_parent(VkrSampleEditBatchItem *item, const OpsRef *ref) {
  item->request.parent = ref->entity;
  item->parent_ref = ref->item;
}

/* Names `ref` as the current operation's result. */
static void ops_op_result(OpsBatch *batch, const OpsRef *ref) {
  if (ref->item >= 0) {
    if (batch->op_item[batch->op_count] == UINT32_MAX) {
      batch->op_item[batch->op_count] = (uint32_t)ref->item;
    }
  } else if (!batch->op_target[batch->op_count].u64) {
    batch->op_target[batch->op_count] = ref->entity;
  }
}

// -----------------------------------------------------------------------------
// Tags (ADR-084)
// -----------------------------------------------------------------------------

/* Canonical tags from one tag argument: a string of tags or an array of
   strings. False with an error when malformed or over a limit. */
static bool8_t ops_tags_arg(OpsContext *ctx, const VkrBakeryJson *value,
                            const char *key, SceneTags *out) {
  String8 text = {0};
  if (value->type == VKR_BAKERY_JSON_STRING) {
    text = value->string;
  } else if (value->type == VKR_BAKERY_JSON_ARRAY) {
    uint64_t length = 1u;
    for (const VkrBakeryJson *part = value->first; part; part = part->next) {
      if (part->type != VKR_BAKERY_JSON_STRING) {
        return ops_fail(ctx, OPS_INVALID, "'%s' holds only strings", key);
      }
      length += part->string.length + 1u;
    }
    uint8_t *joined =
        arena_alloc(ops_arena(ctx), length, ARENA_MEMORY_TAG_STRING);
    if (!joined) {
      return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    }
    for (const VkrBakeryJson *part = value->first; part; part = part->next) {
      MemCopy(joined + text.length, part->string.str, part->string.length);
      text.length += part->string.length;
      joined[text.length++] = ' ';
    }
    text.str = joined;
  } else {
    return ops_fail(ctx, OPS_INVALID,
                    "'%s' is a string of tags or an array of tags", key);
  }
  char error[160] = {0};
  if (!vkr_scene_tags_parse(text, out, error, sizeof(error))) {
    return ops_fail(ctx, OPS_INVALID, "'%s': %s", key, error);
  }
  return true_v;
}

static VkrBakeryJson *ops_tags_json(OpsContext *ctx, const SceneTags *tags) {
  VkrBakeryJson *array = vkr_bakery_json_array(ops_arena(ctx));
  uint32_t cursor = 0u;
  String8 tag = {0};
  while (tags && vkr_scene_tags_next(tags, &cursor, &tag)) {
    vkr_bakery_json_append(array, ops_string(ctx, tag));
  }
  return array;
}

/* The tags `ref` holds after the batch's earlier edits: its creation's or
   the scene's, then each later tags edit of the batch. Returns the batch
   values that hold them, or NULL when the scene holds them or none do. */
static VkrSceneEditValues *ops_tags_view(OpsContext *ctx, const OpsBatch *batch,
                                         const OpsRef *ref, SceneTags *out,
                                         bool8_t *present) {
  VkrSceneEditValues *holder = NULL;
  MemZero(out, sizeof(*out));
  *present = false_v;
  uint32_t first = 0u;
  if (ref->item >= 0) {
    VkrSceneEditValues *made = &ctx->ops->items[ref->item].request.values;
    if ((made->fields & VKR_SCENE_EDIT_COMPONENT) &&
        made->component_type == &vkr_scene_tags_type) {
      MemCopy(out, made->component, sizeof(*out));
      *present = true_v;
      holder = made;
    }
    first = (uint32_t)ref->item + 1u;
  } else {
    const SceneTags *held =
        vkr_scene_get_typed(ops_scene(ctx->frame, ref->container), ref->entity,
                            &vkr_scene_tags_type);
    if (held) {
      *out = *held;
      *present = true_v;
    }
  }
  for (uint32_t i = first; batch && i < batch->count; ++i) {
    VkrSampleEditBatchItem *item = &ctx->ops->items[i];
    VkrSceneEditValues *values = &item->request.values;
    if (!ops_item_names(item, ref) ||
        values->component_type != &vkr_scene_tags_type) {
      continue;
    }
    const VkrSceneEditAction action = item->request.action;
    if (action == VKR_SCENE_EDIT_ADD_COMPONENT ||
        (action == VKR_SCENE_EDIT_APPLY &&
         (values->fields & VKR_SCENE_EDIT_COMPONENT))) {
      MemCopy(out, values->component, sizeof(*out));
      *present = true_v;
      holder = values;
    } else if (action == VKR_SCENE_EDIT_REMOVE_COMPONENT) {
      MemZero(out, sizeof(*out));
      *present = false_v;
      holder = NULL;
    }
  }
  return holder;
}

/* Gives `ref` exactly `tags`: rewrites the batch edit that already holds
   its tags, or adds, sets or removes its tags component. No tags remove
   it. */
static bool8_t ops_tags_write(OpsContext *ctx, OpsBatch *batch,
                              const OpsRef *ref, const SceneTags *tags) {
  SceneTags current;
  bool8_t present = false_v;
  VkrSceneEditValues *holder =
      ops_tags_view(ctx, batch, ref, &current, &present);
  const bool8_t empty = !tags->text[0];
  if (present ? !empty && strcmp(current.text, tags->text) == 0 : empty) {
    return true_v;
  }
  if (holder && !empty) {
    MemCopy(holder->component, tags, sizeof(*tags));
    return true_v;
  }
  /* A creation without a component of its own carries the tags itself. */
  if (ref->item >= 0) {
    VkrSceneEditValues *made = &ctx->ops->items[ref->item].request.values;
    if (!empty && !(made->fields & VKR_SCENE_EDIT_COMPONENT)) {
      made->fields |= VKR_SCENE_EDIT_COMPONENT;
      made->component_type = &vkr_scene_tags_type;
      MemCopy(made->component, tags, sizeof(*tags));
      return true_v;
    }
    if (empty && holder == made) {
      made->fields &= ~(uint32_t)VKR_SCENE_EDIT_COMPONENT;
      made->component_type = NULL;
      return true_v;
    }
  }
  const VkrSceneEditAction action = empty     ? VKR_SCENE_EDIT_REMOVE_COMPONENT
                                    : present ? VKR_SCENE_EDIT_APPLY
                                              : VKR_SCENE_EDIT_ADD_COMPONENT;
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, ref->container, action);
  if (!item) {
    return false_v;
  }
  ops_item_target(item, ref);
  item->request.values.component_type = &vkr_scene_tags_type;
  if (!empty) {
    MemCopy(item->request.values.component, tags, sizeof(*tags));
  }
  if (action == VKR_SCENE_EDIT_APPLY) {
    item->request.values.fields = VKR_SCENE_EDIT_COMPONENT;
  }
  return true_v;
}

static bool8_t ops_tags_given(const VkrBakeryJson *args) {
  return vkr_bakery_json_get(args, "tags") ||
         vkr_bakery_json_get(args, "tags_add") ||
         vkr_bakery_json_get(args, "tags_remove");
}

/* An operation's `tags` (the whole set), then `tags_add` and `tags_remove`,
   over the tags `ref` holds after the batch's earlier edits. */
static bool8_t ops_tags_args(OpsContext *ctx, const VkrBakeryJson *args,
                             OpsBatch *batch, const OpsRef *ref) {
  if (!ops_tags_given(args)) {
    return true_v;
  }
  if (ref->item < 0 &&
      vkr_scene_entity_is_part(ops_scene(ctx->frame, ref->container),
                               ref->entity)) {
    return ops_fail(ctx, OPS_INVALID,
                    "A brush face or connection belongs to its owner; tag "
                    "the owner");
  }
  SceneTags tags;
  bool8_t present = false_v;
  (void)ops_tags_view(ctx, batch, ref, &tags, &present);
  const VkrBakeryJson *replace = vkr_bakery_json_get(args, "tags");
  const VkrBakeryJson *add = vkr_bakery_json_get(args, "tags_add");
  const VkrBakeryJson *remove = vkr_bakery_json_get(args, "tags_remove");
  if (replace && !ops_tags_arg(ctx, replace, "tags", &tags)) {
    return false_v;
  }
  if (add) {
    SceneTags added;
    if (!ops_tags_arg(ctx, add, "tags_add", &added)) {
      return false_v;
    }
    /* Parsing both again drops repeats and applies the count limit. */
    char joined[2u * SCENE_TAGS_CAPACITY];
    const int32_t length =
        snprintf(joined, sizeof(joined), "%s %s", tags.text, added.text);
    char error[160] = {0};
    if (!vkr_scene_tags_parse(
            string8_create((uint8_t *)joined, (uint64_t)Max(length, 0)), &tags,
            error, sizeof(error))) {
      return ops_fail(ctx, OPS_INVALID, "'tags_add': %s", error);
    }
  }
  if (remove) {
    SceneTags removed;
    if (!ops_tags_arg(ctx, remove, "tags_remove", &removed)) {
      return false_v;
    }
    SceneTags kept = {0};
    uint32_t length = 0u;
    uint32_t cursor = 0u;
    String8 tag = {0};
    while (vkr_scene_tags_next(&tags, &cursor, &tag)) {
      if (vkr_scene_tags_has(&removed, tag)) {
        continue;
      }
      if (length) {
        kept.text[length++] = ' ';
      }
      MemCopy(&kept.text[length], tag.str, tag.length);
      length += (uint32_t)tag.length;
    }
    tags = kept;
  }
  return ops_tags_write(ctx, batch, ref, &tags);
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

/* entity.create: a new entity with a pose and optionally one component and
   tags. */
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
  const OpsRef created = {.item = (int32_t)(batch->count - 1u),
                          .container = container};
  batch->op_item[batch->op_count] = (uint32_t)created.item;
  /* Tags may add an edit, which can move the batch's items. */
  return ops_validate_values(ctx, values) &&
         ops_tags_args(ctx, args, batch, &created);
}

/* entity.set: name, pose, visibility or tags of one entity. A batch creation
   takes the change into its own values. */
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
  if (!values.fields && !ops_tags_given(args)) {
    return ops_fail(ctx, OPS_INVALID,
                    "entity.set needs name, position, rotation, scale, "
                    "visible, tags, tags_add or tags_remove");
  }
  if ((values.fields & VKR_SCENE_EDIT_TRANSFORM) && ref.item < 0 &&
      vkr_scene_get_typed(ops_scene(ctx->frame, ref.container), ref.entity,
                          &vkr_scene_brush_face_type)) {
    return ops_fail(ctx, OPS_INVALID,
                    "A brush face moves with its brush; set its plane with "
                    "component.set brush_face");
  }
  if (ref.item >= 0) {
    batch->op_item[batch->op_count] = (uint32_t)ref.item;
  } else {
    batch->op_target[batch->op_count] = ref.entity;
  }
  if (values.fields) {
    if (!ops_validate_values(ctx, &values)) {
      return false_v;
    }
    if (ref.item >= 0) {
      values.fields |= created_fields;
      ctx->ops->items[ref.item].request.values = values;
    } else {
      VkrSampleEditBatchItem *item =
          ops_batch_add(ctx, batch, ref.container, VKR_SCENE_EDIT_APPLY);
      if (!item) {
        return false_v;
      }
      ops_item_target(item, &ref);
      item->request.values = values;
    }
  }
  return ops_tags_args(ctx, args, batch, &ref);
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
static void ops_ref_box(OpsContext *ctx, const OpsBatch *batch,
                        const OpsRef *ref, VkrBrushGeometry *scratch, Vec3 *lo,
                        Vec3 *hi);
static float32_t ops_ref_floor(OpsContext *ctx, const OpsBatch *batch,
                               const OpsRef *ref, VkrBrushGeometry *scratch,
                               Vec3 lo, Vec3 hi);
static bool8_t ops_view_parent(OpsContext *ctx, const OpsBatch *batch,
                               const OpsRef *ref, OpsRef *out);

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
  const bool8_t free =
      ref.item < 0 && scene && vkr_editor_entity_free(scene, ref.entity);
  if (ops_arg_bool(args, "snap", false_v) &&
      (parent.entity.u64 || parent.item >= 0) && scene && !free) {
    VkrBrushGeometry *geometry =
        arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
    if (!geometry) {
      return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    }
    /* Boxes as the batch leaves them; a parent brush's own box decides
       whether it is a slab to stand on. */
    Vec3 lo = vec3_zero();
    Vec3 hi = vec3_zero();
    Vec3 plo = vec3_zero();
    Vec3 phi = vec3_zero();
    ops_ref_box(ctx, batch, &ref, geometry, &lo, &hi);
    ops_ref_box(ctx, batch, &parent, geometry, &plo, &phi);
    const float32_t floor = ops_ref_floor(ctx, batch, &ref, geometry, lo, hi);
    const float32_t pfloor =
        ops_ref_floor(ctx, batch, &parent, geometry, plo, phi);
    const Vec3 shift =
        ops_snap_best(lo, hi, floor, &plo, &phi, &pfloor, 1u, 0.0f);
    if (vec3_length(shift) > 1.0e-6f &&
        !ops_move_world(ctx, batch, &ref, scene, shift)) {
      return false_v;
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

/* What a new brush face shows (vkr_surface.h): its surface tag and mark,
   which the level toolkit sets, and an art-owned material, which only the
   pieces of an existing brush carry over. */
typedef struct OpsFaceLook {
  VkrSurface surface;
  VkrSurfaceMark mark;
  const char *material;
} OpsFaceLook;

/* Arguments every brush and blockout operation shares. */
typedef struct OpsBrushArgs {
  OpsRef parent;
  uint16_t container;
  SceneBrushRole role;
  float32_t grid;
  OpsFaceLook look;
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

/* `surface` and `mark` from `args`, each kept when absent. */
static bool8_t ops_arg_look(OpsContext *ctx, const VkrBakeryJson *args,
                            const char *surface_key, OpsFaceLook *out) {
  if (vkr_bakery_json_get(args, "material")) {
    return ops_fail(ctx, OPS_INVALID,
                    "Brushes take 'surface' and 'mark'; 'material' is "
                    "art-owned and set by face.set_material");
  }
  char name[32];
  name[0] = '\0';
  if (!ops_arg_string(ctx, args, surface_key, name, sizeof(name))) {
    return false_v;
  }
  if (name[0] && !vkr_surface_find(name, &out->surface)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'%s' is none, concrete, metal, wood, tile, plaster, "
                    "brick, rock, dirt, grass, glass, fabric, water or "
                    "emissive",
                    surface_key);
  }
  name[0] = '\0';
  if (!ops_arg_string(ctx, args, "mark", name, sizeof(name))) {
    return false_v;
  }
  if (name[0] && !vkr_surface_mark_find(name, &out->mark)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'mark' is none, hazard, orange, blue, red, green or dark");
  }
  return true_v;
}

/* A face's look as a new face copies it. */
static OpsFaceLook ops_face_look(const SceneBrushFace *face) {
  return (OpsFaceLook){
      .surface = face->surface, .mark = face->mark, .material = face->material};
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
  return ops_arg_look(ctx, args, "surface", &out->look) &&
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
   (or -1 for `brush->parent`) names the parent. Face i shows `looks[i]`,
   or the brush's look without `looks`. The solid is validated first, so a
   bad brush fails before anything is submitted. */
static bool8_t ops_brush_add(OpsContext *ctx, OpsBatch *batch,
                             const OpsBrushArgs *brush, int32_t parent_item,
                             const char *name, Vec3 position, VkrQuat rotation,
                             const VkrBrushPlane *planes, uint32_t count,
                             const OpsFaceLook *looks, uint32_t *out_item) {
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  uint32_t failed = UINT32_MAX;
  if (!geometry) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  const VkrBrushError error = vkr_brush_build(planes, count, geometry, &failed);
  if (error != VKR_BRUSH_OK && failed != UINT32_MAX) {
    return ops_fail(ctx, OPS_INVALID, "Brush '%s': %s (plane %u, from 0)", name,
                    vkr_brush_error_text(error), failed);
  }
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
    const OpsFaceLook *look = looks ? &looks[i] : &brush->look;
    settings->surface = look->surface;
    settings->mark = look->mark;
    snprintf(settings->material, sizeof(settings->material), "%s",
             look->material ? look->material : "");
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
    return ops_fail(ctx, OPS_INVALID,
                    "Every side of the box must be positive after snapping "
                    "its corners to the %.4g m grid; pass a finer 'grid', or "
                    "0 to keep them",
                    (float64_t)grid);
  }
  return true_v;
}

/* 'rotation' in degrees XYZ, identity when absent. */
static bool8_t ops_arg_rotation(OpsContext *ctx, const VkrBakeryJson *args,
                                VkrQuat *out) {
  Vec3 degrees = {0};
  bool8_t has_rotation = false_v;
  if (!ops_arg_vec3(ctx, args, "rotation", &degrees, &has_rotation)) {
    return false_v;
  }
  const float32_t euler[3] = {degrees.x, degrees.y, degrees.z};
  *out =
      has_rotation ? vkr_property_quat_from_euler(euler) : vkr_quat_identity();
  return true_v;
}

/* A box brush between parent-space corners, its origin at the center,
   every face showing `look`, or the brush's look when it is NULL. */
static bool8_t ops_box_add(OpsContext *ctx, OpsBatch *batch,
                           const OpsBrushArgs *brush, int32_t parent_item,
                           const char *name, Vec3 min, Vec3 max,
                           const OpsFaceLook *look, uint32_t *out_item) {
  const Vec3 center = vec3_scale(vec3_add(min, max), 0.5f);
  VkrBrushPlane planes[6];
  const uint32_t count = vkr_brush_box_planes(vec3_sub(min, center),
                                              vec3_sub(max, center), planes);
  OpsFaceLook looks[6];
  for (uint32_t i = 0; i < ArrayCount(looks); ++i) {
    looks[i] = look ? *look : brush->look;
  }
  return ops_brush_add(ctx, batch, brush, parent_item, name, center,
                       vkr_quat_identity(), planes, count, looks, out_item);
}

/* look.volume: a look volume over a world box (ADR-097). Each key of
   'look' sets that value and turns its override on. */
static bool8_t ops_build_look_volume(OpsContext *ctx, const VkrBakeryJson *args,
                                     OpsBatch *batch) {
  uint16_t container = 0u;
  Vec3 min = {0};
  Vec3 max = {0};
  VkrQuat rotation = vkr_quat_identity();
  if (!ops_arg_container(ctx, args, &container) ||
      !ops_arg_box(ctx, args, 0.0f, &min, &max) ||
      !ops_arg_rotation(ctx, args, &rotation)) {
    return false_v;
  }
  SceneLookVolume volume;
  vkr_type_defaults(&vkr_scene_look_volume_type, &volume);
  float64_t number = 0.0;
  if (ops_arg_number(args, "priority", &number)) {
    volume.priority = (int32_t)number;
  }
  if (ops_arg_number(args, "blend_distance", &number)) {
    volume.blend_distance = (float32_t)number;
  }
  const VkrBakeryJson *look = vkr_bakery_json_get(args, "look");
  if (!look || look->type != VKR_BAKERY_JSON_OBJECT || look->count == 0u) {
    return ops_fail(ctx, OPS_INVALID,
                    "'look' must name at least one value to override");
  }
  /* Scalar overrides: key, flag and value. */
  const struct {
    const char *key;
    bool8_t *flag;
    float32_t *value;
  } scalars[] = {
      {"exposure_compensation_ev", &volume.override_exposure,
       &volume.exposure_compensation_ev},
      {"contrast", &volume.override_contrast, &volume.contrast},
      {"saturation", &volume.override_saturation, &volume.saturation},
      {"bloom_intensity", &volume.override_bloom, &volume.bloom_intensity},
      {"fog_density", &volume.override_fog_density, &volume.fog_density},
      {"sky_light_intensity", &volume.override_sky_light,
       &volume.sky_light_intensity},
  };
  uint32_t known = 0u;
  for (uint32_t i = 0; i < ArrayCount(scalars); ++i) {
    if (vkr_bakery_json_get(look, scalars[i].key)) {
      if (!ops_arg_number(look, scalars[i].key, &number) || !isfinite(number)) {
        return ops_fail(ctx, OPS_INVALID, "'look.%s' must be a number",
                        scalars[i].key);
      }
      *scalars[i].flag = true_v;
      *scalars[i].value = (float32_t)number;
      known++;
    }
  }
  float32_t pair[2] = {0};
  bool8_t present = false_v;
  if (!ops_arg_floats(ctx, look, "metering", pair, 2u, &present)) {
    return false_v;
  }
  if (present) {
    volume.override_metering = true_v;
    volume.metering_min_ev = pair[0];
    volume.metering_max_ev = pair[1];
    known++;
  }
  if (!ops_arg_floats(ctx, look, "white_balance", pair, 2u, &present)) {
    return false_v;
  }
  if (present) {
    volume.override_white_balance = true_v;
    volume.white_balance_temperature = pair[0];
    volume.white_balance_tint = pair[1];
    known++;
  }
  if (!ops_arg_vec3(ctx, look, "fog_color", &volume.fog_color, &present)) {
    return false_v;
  }
  if (present) {
    volume.override_fog_color = true_v;
    known++;
  }
  if (known != look->count) {
    return ops_fail(ctx, OPS_INVALID,
                    "'look' takes exposure_compensation_ev, metering [min, "
                    "max], white_balance [temperature, tint], contrast, "
                    "saturation, bloom_intensity, fog_color, fog_density "
                    "and sky_light_intensity");
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, container, VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  VkrSceneEditValues *values = &item->request.values;
  values->fields =
      VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM | VKR_SCENE_EDIT_COMPONENT;
  snprintf(values->name, sizeof(values->name), "Look volume");
  if (!ops_arg_string(ctx, args, "name", values->name, sizeof(values->name))) {
    return false_v;
  }
  /* The unit box scaled to the corners, turned about its center. */
  values->position = vec3_scale(vec3_add(min, max), 0.5f);
  values->rotation = rotation;
  values->scale = vec3_sub(max, min);
  values->component_type = &vkr_scene_look_volume_type;
  MemCopy(values->component, &volume, sizeof(volume));
  batch->op_item[batch->op_count] = batch->count - 1u;
  return ops_validate_values(ctx, values);
}

/* probe.create: a reflection probe for the next bake (ADR-103). Probes are
   load-baked into the project scene's document, so only a managed scene
   takes one; its save writes the document's probes back. */
static bool8_t ops_build_probe_create(OpsContext *ctx,
                                      const VkrBakeryJson *args,
                                      OpsBatch *batch) {
  const VkrSampleUiFrame *frame = ctx->frame;
  if (!vkr_editor_projects_managed_scene(ctx->editor->projects, frame)) {
    return ops_fail(ctx, OPS_REJECTED,
                    "Reflection probes are baked into a project scene: open "
                    "one to add a probe");
  }
  if (vkr_scene_find_typed(frame->scene, &vkr_scene_reflection_probe_type, NULL,
                           0u) >= VKR_SCENE_REFLECTION_PROBE_MAX) {
    return ops_fail(ctx, OPS_LIMIT, "A scene holds at most %u probes",
                    VKR_SCENE_REFLECTION_PROBE_MAX);
  }
  SceneReflectionProbeSettings probe;
  vkr_type_defaults(&vkr_scene_reflection_probe_type, &probe);
  probe.extents = vec3_new(4.0f, 2.5f, 4.0f);
  bool8_t has_center = false_v;
  bool8_t has_extents = false_v;
  Vec3 center = {0};
  Vec3 extents = {0};
  if (!ops_arg_vec3(ctx, args, "center", &center, &has_center) ||
      !ops_arg_vec3(ctx, args, "extents", &extents, &has_extents) ||
      !ops_component_read(ctx, &vkr_scene_reflection_probe_type,
                          vkr_bakery_json_get(args, "values"), &probe)) {
    return false_v;
  }
  if (!has_center) {
    return ops_fail(ctx, OPS_INVALID, "'center' is the probe's world center");
  }
  probe.center = center;
  if (has_extents) {
    probe.extents = extents;
  }
  char error[160] = {0};
  if (!vkr_type_validate(&vkr_scene_reflection_probe_type, &probe, error,
                         sizeof(error))) {
    return ops_fail(ctx, OPS_INVALID, "reflection_probe: %s", error);
  }
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, 0u, VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  VkrSceneEditValues *values = &item->request.values;
  values->fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_COMPONENT;
  snprintf(values->name, sizeof(values->name), "Reflection Probe");
  if (!ops_arg_string(ctx, args, "name", values->name, sizeof(values->name))) {
    return false_v;
  }
  values->component_type = &vkr_scene_reflection_probe_type;
  MemCopy(values->component, &probe, sizeof(probe));
  batch->op_item[batch->op_count] = batch->count - 1u;
  return true_v;
}

/* decal.place: a decal box centred on a surface point and facing it
   (ADR-101), at 'position' and 'normal' or where a ray from 'origin' along
   'direction' first meets collision. */
static bool8_t ops_build_decal_place(OpsContext *ctx, const VkrBakeryJson *args,
                                     OpsBatch *batch) {
  uint16_t container = 0u;
  Vec3 position = {0};
  Vec3 normal = vec3_new(0.0f, 1.0f, 0.0f);
  Vec3 facing = vec3_new(0.0f, 0.0f, -1.0f);
  Vec3 origin = {0};
  Vec3 direction = {0};
  bool8_t has_position = false_v;
  bool8_t has_normal = false_v;
  bool8_t has_origin = false_v;
  bool8_t has_direction = false_v;
  bool8_t has_facing = false_v;
  if (!ops_arg_container(ctx, args, &container) ||
      !ops_arg_vec3(ctx, args, "position", &position, &has_position) ||
      !ops_arg_vec3(ctx, args, "normal", &normal, &has_normal) ||
      !ops_arg_vec3(ctx, args, "origin", &origin, &has_origin) ||
      !ops_arg_vec3(ctx, args, "direction", &direction, &has_direction) ||
      !ops_arg_vec3(ctx, args, "facing", &facing, &has_facing)) {
    return false_v;
  }
  if (has_position == (has_origin && has_direction) ||
      has_position != has_normal) {
    return ops_fail(ctx, OPS_INVALID,
                    "decal.place takes 'position' and 'normal', or 'origin' "
                    "and 'direction' to place it where that ray meets a "
                    "surface");
  }
  if (!has_position) {
    if (vec3_length(direction) < 1.0e-6f) {
      return ops_fail(ctx, OPS_INVALID, "'direction' must be nonzero");
    }
    /* Physics queries take a mutable scene but change none of its state. */
    VkrScene *scene = (VkrScene *)ops_scene(ctx->frame, container);
    VkrPhysicsQueryFilter filter = {.mask = UINT16_MAX};
    VkrPhysicsRayHit hit = {0};
    if (!scene ||
        !vkr_scene_physics_raycast_query(
            scene, origin, vec3_scale(vec3_normalize(direction), 1000.0f),
            &filter, &hit)) {
      return ops_fail(ctx, OPS_NOT_FOUND,
                      "The ray meets no collision surface within 1000 m");
    }
    position = vec3_new(hit.position[0], hit.position[1], hit.position[2]);
    normal = vec3_new(hit.normal[0], hit.normal[1], hit.normal[2]);
    if (!has_facing) {
      facing = direction;
    }
  }
  if (vec3_length(normal) < 1.0e-6f) {
    return ops_fail(ctx, OPS_INVALID, "'normal' must be nonzero");
  }
  /* 'size' is the width and height across the surface, one number for a
     square. */
  float32_t size[2] = {1.0f, 1.0f};
  float64_t number = 0.0;
  bool8_t has_size = false_v;
  if (ops_arg_number(args, "size", &number)) {
    size[0] = size[1] = (float32_t)number;
  } else if (!ops_arg_floats(ctx, args, "size", size, 2u, &has_size)) {
    return false_v;
  }
  float64_t depth = 0.5;
  float64_t angle = 0.0;
  (void)ops_arg_number(args, "depth", &depth);
  (void)ops_arg_number(args, "angle", &angle);
  if (!(size[0] > 0.0f) || !(size[1] > 0.0f) || !isfinite(size[0]) ||
      !isfinite(size[1]) || !(depth > 0.0) || !isfinite(depth) ||
      !isfinite(angle)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'size' and 'depth' must be positive metres and 'angle' "
                    "finite degrees");
  }
  SceneDecal decal;
  vkr_type_defaults(&vkr_scene_decal_type, &decal);
  if (!ops_component_read(ctx, &vkr_scene_decal_type,
                          vkr_bakery_json_get(args, "values"), &decal) ||
      !ops_arg_string(ctx, args, "material", decal.material,
                      sizeof(decal.material))) {
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
  snprintf(values->name, sizeof(values->name), "Decal");
  if (!ops_arg_string(ctx, args, "name", values->name, sizeof(values->name))) {
    return false_v;
  }
  /* The unit box: X across, Y along the normal (it projects along -Y), Z
     toward the image's top. */
  values->position = position;
  values->rotation = vkr_editor_decal_orientation(
      normal, facing, (float32_t)angle * (VKR_PI / 180.0f));
  values->scale = vec3_new(size[0], (float32_t)depth, size[1]);
  values->component_type = &vkr_scene_decal_type;
  MemCopy(values->component, &decal, sizeof(decal));
  batch->op_item[batch->op_count] = batch->count - 1u;
  return ops_validate_values(ctx, values);
}

static bool8_t ops_build_brush_box(OpsContext *ctx, const VkrBakeryJson *args,
                                   OpsBatch *batch) {
  OpsBrushArgs brush;
  Vec3 min = {0};
  Vec3 max = {0};
  VkrQuat rotation = vkr_quat_identity();
  if (!ops_brush_args(ctx, args, batch, "Brush", &brush) ||
      !ops_arg_box(ctx, args, brush.grid, &min, &max) ||
      !ops_arg_rotation(ctx, args, &rotation)) {
    return false_v;
  }
  /* A turned box turns about its center. */
  const Vec3 center = vec3_scale(vec3_add(min, max), 0.5f);
  VkrBrushPlane planes[6];
  const uint32_t count = vkr_brush_box_planes(vec3_sub(min, center),
                                              vec3_sub(max, center), planes);
  uint32_t item = 0u;
  if (!ops_brush_add(ctx, batch, &brush, -1, brush.name, center, rotation,
                     planes, count, NULL, &item)) {
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
  VkrQuat rotation = vkr_quat_identity();
  if (!ops_brush_args(ctx, args, batch, "Wedge", &brush) ||
      !ops_arg_box(ctx, args, brush.grid, &min, &max) ||
      !ops_arg_rotation(ctx, args, &rotation)) {
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
  if (!ops_brush_add(ctx, batch, &brush, -1, brush.name, center, rotation,
                     planes, count, NULL, &item)) {
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
  VkrQuat rotation = vkr_quat_identity();
  if (!ops_brush_args(ctx, args, batch, "Cylinder", &brush) ||
      !ops_arg_vec3(ctx, args, "center", &center, &has_center) ||
      !ops_arg_rotation(ctx, args, &rotation)) {
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
  if (!ops_brush_add(ctx, batch, &brush, -1, brush.name, center, rotation,
                     planes, count, NULL, &item)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = item;
  return true_v;
}

static uint32_t ops_arg_points(OpsContext *ctx, const VkrBakeryJson *args,
                               const char *key, Vec3 *out, uint32_t min_count,
                               uint32_t max_count);

/* brush.planes: a brush bounded by parent-space planes, its origin at the
   center of its box. */
static bool8_t ops_build_brush_planes(OpsContext *ctx,
                                      const VkrBakeryJson *args,
                                      OpsBatch *batch) {
  OpsBrushArgs brush;
  if (!ops_brush_args(ctx, args, batch, "Brush", &brush)) {
    return false_v;
  }
  const VkrBakeryJson *list = vkr_bakery_json_get(args, "planes");
  if (!list || list->type != VKR_BAKERY_JSON_ARRAY ||
      list->count < VKR_BRUSH_FACE_MIN || list->count > VKR_BRUSH_FACE_MAX) {
    return ops_fail(ctx, OPS_INVALID, "'planes' is %u to %u planes",
                    VKR_BRUSH_FACE_MIN, VKR_BRUSH_FACE_MAX);
  }
  VkrBrushPlane *planes =
      arena_alloc(ops_arena(ctx), VKR_BRUSH_FACE_MAX * sizeof(*planes),
                  ARENA_MEMORY_TAG_STRUCT);
  OpsFaceLook *looks =
      arena_alloc(ops_arena(ctx), VKR_BRUSH_FACE_MAX * sizeof(*looks),
                  ARENA_MEMORY_TAG_STRUCT);
  VkrBrushGeometry *geometry =
      arena_alloc(ops_arena(ctx), sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  if (!planes || !looks || !geometry) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }

  /* Each plane: an outward normal and a distance or a point on it. */
  uint32_t count = 0u;
  for (const VkrBakeryJson *entry = list->first; entry; entry = entry->next) {
    Vec3 normal = {0};
    Vec3 point = {0};
    bool8_t has_normal = false_v;
    bool8_t has_point = false_v;
    float64_t distance = 0.0;
    if (entry->type != VKR_BAKERY_JSON_OBJECT ||
        !ops_arg_vec3(ctx, entry, "normal", &normal, &has_normal) ||
        !ops_arg_vec3(ctx, entry, "point", &point, &has_point)) {
      return ops_fail(ctx, OPS_INVALID,
                      "planes[%u] is {normal, distance or point, surface, "
                      "mark}",
                      count);
    }
    const bool8_t has_distance = ops_arg_number(entry, "distance", &distance);
    const float32_t length = vec3_length(normal);
    if (!has_normal || has_point == has_distance || !(length > 1.0e-6f) ||
        !isfinite(distance)) {
      return ops_fail(ctx, OPS_INVALID,
                      "planes[%u] needs a nonzero outward 'normal' and either "
                      "'distance' (normal . p <= distance inside) or a "
                      "'point' on the plane",
                      count);
    }
    normal = vec3_scale(normal, 1.0f / length);
    looks[count] = brush.look;
    if (!ops_arg_look(ctx, entry, "surface", &looks[count])) {
      return false_v;
    }
    planes[count] = (VkrBrushPlane){
        normal, has_point ? vec3_dot(normal, point)
                          : (float32_t)(distance / (float64_t)length)};
    count++;
  }

  /* Move the origin to the solid's center so the pivot sits on it. */
  uint32_t failed = UINT32_MAX;
  const VkrBrushError error = vkr_brush_build(planes, count, geometry, &failed);
  if (error != VKR_BRUSH_OK) {
    return failed != UINT32_MAX
               ? ops_fail(ctx, OPS_INVALID, "Brush '%s': %s (planes[%u])",
                          brush.name, vkr_brush_error_text(error), failed)
               : ops_fail(ctx, OPS_INVALID, "Brush '%s': %s", brush.name,
                          vkr_brush_error_text(error));
  }
  const Vec3 center = vec3_scale(vec3_add(geometry->min, geometry->max), 0.5f);
  for (uint32_t i = 0; i < count; ++i) {
    planes[i].distance -= vec3_dot(planes[i].normal, center);
  }
  uint32_t item = 0u;
  if (!ops_brush_add(ctx, batch, &brush, -1, brush.name, center,
                     vkr_quat_identity(), planes, count, looks, &item)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = item;
  return true_v;
}

/* brush.hull: the convex hull of parent-space points, snapped to the grid,
   its origin at the center of their box. */
static bool8_t ops_build_brush_hull(OpsContext *ctx, const VkrBakeryJson *args,
                                    OpsBatch *batch) {
  OpsBrushArgs brush;
  Vec3 points[VKR_BRUSH_HULL_POINT_MAX];
  if (!ops_brush_args(ctx, args, batch, "Brush", &brush)) {
    return false_v;
  }
  const uint32_t count =
      ops_arg_points(ctx, args, "points", points, 4u, VKR_BRUSH_HULL_POINT_MAX);
  if (!count) {
    return false_v;
  }
  Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  for (uint32_t i = 0; i < count; ++i) {
    points[i] = ops_snap3(points[i], brush.grid);
    lo = vec3_new(Min(lo.x, points[i].x), Min(lo.y, points[i].y),
                  Min(lo.z, points[i].z));
    hi = vec3_new(Max(hi.x, points[i].x), Max(hi.y, points[i].y),
                  Max(hi.z, points[i].z));
  }

  /* The hull is computed about the center, where float steps are small. */
  const Vec3 center = vec3_scale(vec3_add(lo, hi), 0.5f);
  for (uint32_t i = 0; i < count; ++i) {
    points[i] = vec3_sub(points[i], center);
  }
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  const uint32_t plane_count =
      vkr_brush_hull(points, count, planes, VKR_BRUSH_FACE_MAX);
  if (!plane_count) {
    return ops_fail(ctx, OPS_INVALID,
                    "The points span no volume after snapping to the %.4g m "
                    "grid, or their hull needs more than %u faces",
                    (float64_t)brush.grid, VKR_BRUSH_FACE_MAX);
  }
  uint32_t item = 0u;
  if (!ops_brush_add(ctx, batch, &brush, -1, brush.name, center,
                     vkr_quat_identity(), planes, plane_count, NULL, &item)) {
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

/* Adds layout `pieces` as brushes under batch item `group`, or under
   `brush->parent` when it is negative; each brush sits at its piece's
   center. Floors show `floor`, the other pieces the brush's look. */
static bool8_t ops_pieces_add(OpsContext *ctx, OpsBatch *batch,
                              const OpsBrushArgs *brush, int32_t group,
                              const VkrBlockoutPiece *pieces, uint32_t count,
                              const OpsFaceLook *floor) {
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
    const OpsFaceLook *look =
        piece->kind == VKR_BLOCKOUT_PIECE_FLOOR ? floor : &brush->look;
    OpsFaceLook looks[VKR_BRUSH_FACE_MAX];
    for (uint32_t f = 0; f < plane_count; ++f) {
      looks[f] = *look;
    }
    char name[64];
    snprintf(name, sizeof(name), "%s %u", names[piece->kind],
             ++numbers[piece->kind]);
    uint32_t item = 0u;
    if (!ops_brush_add(ctx, batch, brush, group, name, center,
                       vkr_quat_identity(), planes, plane_count, looks,
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
  shape.surface = brush.look.surface;
  shape.mark = brush.look.mark;
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
  OpsBrushArgs brush = {
      .parent = {.entity = ref.entity, .item = -1},
      .container = ref.container,
      .role = SCENE_BRUSH_ROLE_SOLID,
      .look = {.surface = shape->surface, .mark = shape->mark}};
  brush.parent.container = ref.container;
  const OpsFaceLook floor = {.surface = shape->floor_surface != VKR_SURFACE_NONE
                                            ? shape->floor_surface
                                            : shape->surface,
                             .mark = shape->mark};
  if (!ops_pieces_add(ctx, batch, &brush, -1, pieces, count, &floor)) {
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
  shape.surface = brush.look.surface;
  shape.mark = brush.look.mark;
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
  VkrQuat yaw = ops_yaw_toward(run > 0.0f ? delta : vec3_new(0, 0, 1));
  if (shape.stairs == SCENE_STAIRS_SPIRAL) {
    /* 'to' is the rim where the climb ends: an asked width leaves the pole
       the rest of the run, and the stairs turn so the top step's far edge
       and the landing past it lie on the radius through 'to'. */
    if (vkr_bakery_json_get(args, "width")) {
      shape.radius = run - shape.width;
      if (shape.radius < 0.1f) {
        return ops_fail(ctx, OPS_INVALID,
                        "A spiral's 'width' must leave a 0.1 m pole inside "
                        "the rim 'to' sets; move 'to' out or narrow it");
      }
    } else {
      shape.radius = vkr_clamp_f32(run * 0.15f, 0.1f, 0.5f);
      shape.width = Max(run - shape.radius, 0.25f);
    }
    const float32_t side = shape.left ? -1.0f : 1.0f;
    const float32_t exit = run > 0.0f ? atan2f(delta.x, delta.z) : 0.0f;
    yaw = vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f),
                                   exit - side * shape.turn * 0.0174532925f);
  }
  uint32_t group = 0u;
  if (!ops_blockout_add(ctx, batch, &brush, &shape, from, yaw, &group)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = group;
  return true_v;
}

/* Box walls, floor and ceiling around an interior box in group space; the
   group sits at the floor's center. Each part's name starts with `prefix`
   and a slash ("north/Store/Wall East +X"), so parts of two rooms differ. */
static bool8_t ops_room_shell(OpsContext *ctx, OpsBatch *batch,
                              const OpsBrushArgs *brush, uint32_t group,
                              const char *prefix, Vec3 min, Vec3 max,
                              float32_t wall, bool8_t ceiling, bool8_t ends,
                              const OpsFaceLook *floor) {
  enum {
    PART_FLOOR,
    PART_CEILING,
    PART_WEST,
    PART_EAST,
    PART_NORTH,
    PART_SOUTH
  };
  static const char *const parts[] = {"Floor",         "Ceiling",
                                      "Wall West -X",  "Wall East +X",
                                      "Wall North -Z", "Wall South +Z"};
  char names[ArrayCount(parts)][VKR_SCENE_EDIT_NAME_CAPACITY];
  for (uint32_t i = 0; i < ArrayCount(parts); ++i) {
    snprintf(names[i], sizeof(names[i]), "%s%s%s", prefix, prefix[0] ? "/" : "",
             parts[i]);
  }
  uint32_t item = 0u;
  const float32_t t = wall;
  if (!ops_box_add(ctx, batch, brush, (int32_t)group, names[PART_FLOOR],
                   vec3_new(min.x - t, min.y - t, min.z - (ends ? t : 0.0f)),
                   vec3_new(max.x + t, min.y, max.z + (ends ? t : 0.0f)), floor,
                   &item) ||
      (ceiling &&
       !ops_box_add(ctx, batch, brush, (int32_t)group, names[PART_CEILING],
                    vec3_new(min.x - t, max.y, min.z - (ends ? t : 0.0f)),
                    vec3_new(max.x + t, max.y + t, max.z + (ends ? t : 0.0f)),
                    NULL, &item)) ||
      !ops_box_add(ctx, batch, brush, (int32_t)group, names[PART_WEST],
                   vec3_new(min.x - t, min.y, min.z),
                   vec3_new(min.x, max.y, max.z), NULL, &item) ||
      !ops_box_add(ctx, batch, brush, (int32_t)group, names[PART_EAST],
                   vec3_new(max.x, min.y, min.z),
                   vec3_new(max.x + t, max.y, max.z), NULL, &item)) {
    return false_v;
  }
  if (!ends) {
    return true_v;
  }
  return ops_box_add(ctx, batch, brush, (int32_t)group, names[PART_NORTH],
                     vec3_new(min.x - t, min.y, min.z - t),
                     vec3_new(max.x + t, max.y, min.z), NULL, &item) &&
         ops_box_add(ctx, batch, brush, (int32_t)group, names[PART_SOUTH],
                     vec3_new(min.x - t, min.y, max.z),
                     vec3_new(max.x + t, max.y, max.z + t), NULL, &item);
}

static bool8_t ops_build_room(OpsContext *ctx, const VkrBakeryJson *args,
                              OpsBatch *batch) {
  OpsBrushArgs brush;
  Vec3 min = {0};
  Vec3 size = {0};
  bool8_t has_min = false_v;
  bool8_t has_size = false_v;
  if (!ops_brush_args(ctx, args, batch, "Room", &brush) ||
      !ops_arg_vec3(ctx, args, "min", &min, &has_min) ||
      !ops_arg_vec3(ctx, args, "size", &size, &has_size)) {
    return false_v;
  }
  OpsFaceLook floor = brush.look;
  if (!ops_arg_look(ctx, args, "floor_surface", &floor)) {
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
  const Vec3 half = vec3_new(size.x * 0.5f, 0.0f, size.z * 0.5f);
  if (!ops_room_shell(ctx, batch, &brush, group, brush.name,
                      vec3_new(-half.x, 0.0f, -half.z),
                      vec3_new(half.x, size.y, half.z),
                      ops_snap((float32_t)wall, brush.grid),
                      ops_arg_bool(args, "ceiling", true_v), true_v, &floor)) {
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
  OpsFaceLook floor = {0};
  if (!ops_brush_args(ctx, args, batch, "Corridor", &brush) ||
      !ops_arg_look(ctx, args, "floor_surface", &floor)) {
    return false_v;
  }
  shape.surface = brush.look.surface;
  shape.floor_surface = floor.surface;
  shape.mark = brush.look.mark;
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
  OpsRef parent = {.item = -1};
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
    if (refs[i].container != refs[0].container) {
      return ops_fail(ctx, OPS_INVALID, "A mover's objects share one scene");
    }
    OpsRef above;
    if (!ops_view_parent(ctx, batch, &refs[i], &above)) {
      return ops_fail(ctx, OPS_NOT_FOUND, "A mover's object does not exist");
    }
    if (i == 0u) {
      parent = above;
    } else if (!ops_ref_same(&above, &parent)) {
      return ops_fail(ctx, OPS_INVALID,
                      "A mover's objects share one parent; move them under "
                      "one first");
    }
    /* Its world box, or its origin when it has no geometry. */
    Vec3 a = vec3_zero();
    Vec3 b = vec3_zero();
    ops_ref_box(ctx, batch, &refs[i], geometry, &a, &b);
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
  Mat4 parent_world = mat4_identity();
  if ((parent.item >= 0 || parent.entity.u64) &&
      ops_view_world(ctx, batch, &parent, &parent_world, 0u)) {
    center = mat4_mul_vec3(mat4_inverse_affine(parent_world), center);
  }
  /* 'hinge' turns it as a door about the vertical line through the middle
     of that side of its objects' box, 90 degrees unless 'values' says. */
  String8 hinge = {0};
  if (vkr_bakery_json_get_string(args, "hinge", &hinge)) {
    const bool8_t along_x = ops_equals(hinge, "+x") || ops_equals(hinge, "-x");
    if (!along_x && !ops_equals(hinge, "+z") && !ops_equals(hinge, "-z")) {
      return ops_fail(ctx, OPS_INVALID, "'hinge' is +x, -x, +z or -z");
    }
    Vec3 point = vec3_scale(vec3_add(lo, hi), 0.5f);
    const bool8_t plus = hinge.str[0] == '+';
    if (along_x) {
      point.x = plus ? hi.x : lo.x;
    } else {
      point.z = plus ? hi.z : lo.z;
    }
    SceneMover *settings = (SceneMover *)mover;
    settings->pivot = vec3_sub(
        mat4_mul_vec3(mat4_inverse_affine(parent_world), point), center);
    settings->axis = vec3_new(0.0f, 1.0f, 0.0f);
    if (settings->angle == 0.0f && !settings->spin) {
      settings->angle = 90.0f;
    }
  }

  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, refs[0].container, VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  ops_item_parent(item, &parent);
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
    OpsRef above;
    if (!ops_view(ctx, batch, &refs[i], &pose, &above)) {
      return ops_fail(ctx, OPS_NOT_FOUND, "A mover's object does not exist");
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

/* Face selectors of brush.set_surface and face.set_material: top, bottom,
   sides, +x, -x, +z, -z. */
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

// -----------------------------------------------------------------------------
// Brush editing (ADR-084)
// -----------------------------------------------------------------------------

static uint32_t ops_arg_points(OpsContext *ctx, const VkrBakeryJson *args,
                               const char *key, Vec3 *out, uint32_t min_count,
                               uint32_t max_count);

/* A face entity named directly, or by `brush` and `side`. */
static bool8_t ops_face_arg(OpsContext *ctx, OpsBatch *batch,
                            const VkrBakeryJson *args, OpsBrushRead *brush,
                            uint32_t *out_face) {
  const VkrBakeryJson *face = vkr_bakery_json_get(args, "face");
  if (face) {
    OpsRef ref;
    if (!ops_ref(ctx, batch, face, "face", &ref)) {
      return false_v;
    }
    VkrSceneEditValues *values =
        arena_alloc(ops_arena(ctx), sizeof(*values), ARENA_MEMORY_TAG_STRUCT);
    OpsRef owner;
    if (!values || !ops_view_face(ctx, batch, &ref, values) ||
        !ops_view(ctx, batch, &ref, values, &owner) ||
        !ops_brush_read(ctx, batch, &owner, brush)) {
      return ops_fail(ctx, OPS_INVALID, "'face' must name a brush face");
    }
    for (uint32_t i = 0; i < brush->count; ++i) {
      if (ops_ref_same(&brush->faces[i], &ref)) {
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
  const uint16_t container = brush->ref.container;
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
      ops_item_target(item, &brush->faces[source]);
      values->fields = VKR_SCENE_EDIT_COMPONENT;
    } else {
      ops_item_parent(item, &brush->ref);
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
    ops_item_target(item, &brush->faces[i]);
  }
  return true_v;
}

/* Replaces `brush` with `pieces`, which lie in its space; each piece face
   copies the look and projection of the face its source names (target
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
  OpsBrushArgs args = {.parent = brush->parent,
                       .container = brush->ref.container,
                       .role = brush->role};
  *out_item = UINT32_MAX;
  const bool8_t keep = delete_original && piece_count > 0u;
  if (keep) {
    if (!ops_brush_edit(ctx, batch, brush, &pieces[0], extra, extra_count,
                        fallback)) {
      return false_v;
    }
    ops_op_result(batch, &brush->ref);
  }
  const String8 base = string8_create_from_cstr((const uint8_t *)brush->name,
                                                strlen(brush->name));
  for (uint32_t p = keep ? 1u : 0u; p < piece_count; ++p) {
    OpsFaceLook looks[VKR_BRUSH_FACE_MAX];
    const SceneBrushFace *sources[VKR_BRUSH_FACE_MAX];
    for (uint32_t f = 0; f < pieces[p].count; ++f) {
      sources[f] =
          ops_piece_source(brush, &pieces[p], f, extra, extra_count, fallback);
      looks[f] = ops_face_look(sources[f]);
    }
    /* Pieces of one brush read as its parts unless the caller names them. */
    char name[96];
    snprintf(name, sizeof(name), "%.*s%s", (int)Min(base.length, 80u),
             (const char *)base.str,
             suffix             ? suffix
             : piece_count > 1u ? " part"
                                : "");
    uint32_t item = 0u;
    if (!ops_brush_add(ctx, batch, &args, -1, name, brush->transform.position,
                       brush->transform.rotation, pieces[p].planes,
                       pieces[p].count, looks, &item)) {
      return false_v;
    }
    /* Carry each face's texture projection along with its look. */
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
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, brush->ref.container, VKR_SCENE_EDIT_DELETE);
  if (!item) {
    return false_v;
  }
  ops_item_target(item, &brush->ref);
  return true_v;
}

static bool8_t ops_brush_unscaled(OpsContext *ctx, const OpsBrushRead *brush) {
  return vec3_length(vec3_sub(brush->transform.scale, vec3_one())) < 1.0e-4f
             ? true_v
             : ops_fail(ctx, OPS_INVALID,
                        "Editing needs an unscaled brush; move its faces "
                        "instead of scaling it");
}

/* brush.set_surface and face.set_material: the surface tag and mark, or
   the art-owned material, of one `face`, or of a `brush`'s faces, all or
   those its `faces` selectors match. */
static bool8_t ops_build_face_look(OpsContext *ctx, const VkrBakeryJson *args,
                                   OpsBatch *batch, bool8_t material) {
  const VkrBakeryJson *selectors = vkr_bakery_json_get(args, "faces");
  if (selectors && selectors->type != VKR_BAKERY_JSON_ARRAY) {
    return ops_fail(ctx, OPS_INVALID,
                    "'faces' lists top, bottom, sides, +x, -x, +z or -z");
  }
  char path[SCENE_BRUSH_MATERIAL_CAPACITY] = "";
  OpsFaceLook look = {0};
  bool8_t has_surface = false_v;
  bool8_t has_mark = false_v;
  if (material) {
    if (!vkr_bakery_json_get(args, "material")) {
      return ops_fail(ctx, OPS_INVALID,
                      "'material' is required; an empty one clears it");
    }
    if (!ops_arg_string(ctx, args, "material", path, sizeof(path))) {
      return false_v;
    }
  } else {
    has_surface = vkr_bakery_json_get(args, "surface") != NULL;
    has_mark = vkr_bakery_json_get(args, "mark") != NULL;
    if (!has_surface && !has_mark) {
      return ops_fail(ctx, OPS_INVALID, "Give a 'surface', a 'mark' or both");
    }
    if (!ops_arg_look(ctx, args, "surface", &look)) {
      return false_v;
    }
  }
  OpsBrushRead *brush =
      arena_alloc(ops_arena(ctx), sizeof(*brush), ARENA_MEMORY_TAG_STRUCT);
  if (!brush) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  uint32_t only = UINT32_MAX;
  if (vkr_bakery_json_get(args, "face")) {
    if (!ops_face_arg(ctx, batch, args, brush, &only)) {
      return false_v;
    }
  } else if (!ops_brush_arg(ctx, batch, args, "brush", brush)) {
    return false_v;
  }
  uint32_t changed = 0u;
  for (uint32_t i = 0; i < brush->count; ++i) {
    if (only != UINT32_MAX ? i != only
                           : !ops_face_matches(&brush->values[i], selectors)) {
      continue;
    }
    VkrSampleEditBatchItem *item =
        ops_batch_add(ctx, batch, brush->ref.container, VKR_SCENE_EDIT_APPLY);
    if (!item) {
      return false_v;
    }
    ops_item_target(item, &brush->faces[i]);
    item->request.values.fields = VKR_SCENE_EDIT_COMPONENT;
    item->request.values.component_type = &vkr_scene_brush_face_type;
    SceneBrushFace *value = (SceneBrushFace *)item->request.values.component;
    *value = brush->values[i];
    if (material) {
      snprintf(value->material, sizeof(value->material), "%s", path);
    }
    if (has_surface) {
      value->surface = look.surface;
    }
    if (has_mark) {
      value->mark = look.mark;
    }
    changed++;
  }
  if (!changed) {
    return ops_fail(ctx, OPS_NOT_FOUND, "No face of the brush matched");
  }
  ops_op_result(batch, &brush->ref);
  return true_v;
}

static bool8_t ops_build_set_surface(OpsContext *ctx, const VkrBakeryJson *args,
                                     OpsBatch *batch) {
  return ops_build_face_look(ctx, args, batch, false_v);
}

static bool8_t ops_build_face_material(OpsContext *ctx,
                                       const VkrBakeryJson *args,
                                       OpsBatch *batch) {
  return ops_build_face_look(ctx, args, batch, true_v);
}

/* The local box of a read brush with six axis-aligned faces; false for any
   other shape. */
static bool8_t ops_brush_box_of(const OpsBrushRead *brush, Vec3 *out_min,
                                Vec3 *out_max) {
  if (brush->count != 6u) {
    return false_v;
  }
  Vec3 lo = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  Vec3 hi = vec3_new(INFINITY, INFINITY, INFINITY);
  for (uint32_t i = 0; i < 6u; ++i) {
    const SceneBrushFace *face = &brush->values[i];
    const Vec3 n = vec3_normalize(face->normal);
    const float32_t d = face->distance / vec3_length(face->normal);
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
  OpsBrushRead *wall =
      arena_alloc(ops_arena(ctx), sizeof(*wall), ARENA_MEMORY_TAG_STRUCT);
  if (!wall) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  if (!ops_brush_arg(ctx, batch, args, "wall", wall)) {
    return false_v;
  }
  const VkrScene *scene = ops_scene(ctx->frame, wall->ref.container);
  Vec3 lo = {0};
  Vec3 hi = {0};
  const SceneTransform *transform = &wall->transform;
  if (!ops_brush_box_of(wall, &lo, &hi) ||
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
  /* The pieces show the wall's first face's look. */
  OpsBrushArgs brush = {.parent = wall->parent,
                        .container = wall->ref.container,
                        .role = wall->role,
                        .look = ops_face_look(&wall->values[0])};
  /* Pieces in the wall's space, placed in its parent's space. */
  Vec3 piece_lo[3];
  Vec3 piece_hi[3];
  uint32_t pieces = 0u;
  /* The pieces keep the wall's name, so a prefix such as "lab/" stays. */
  const String8 wall_name =
      string8_create_from_cstr((const uint8_t *)wall->name, strlen(wall->name));
  char names[3][VKR_SCENE_EDIT_NAME_CAPACITY];
  const char *suffixes[3] = {"left", "right", "lintel"};
  for (uint32_t i = 0; i < 3u; ++i) {
    if (wall_name.length) {
      snprintf(names[i], sizeof(names[i]), "%.*s %s", (int)wall_name.length,
               (const char *)wall_name.str, suffixes[i]);
    } else {
      snprintf(names[i], sizeof(names[i]), "Wall %s", suffixes[i]);
    }
  }
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
  /* A wall the batch made goes with its faces; a scene wall takes its
     other children along. */
  if (wall->ref.item >= 0) {
    VkrSampleEditBatchItem *removal =
        ops_batch_add(ctx, batch, wall->ref.container, VKR_SCENE_EDIT_DELETE);
    if (!removal) {
      return false_v;
    }
    ops_item_target(removal, &wall->ref);
  } else if (!ops_delete_tree(ctx, batch, scene, wall->ref.entity, true_v,
                              0u)) {
    return false_v;
  }
  batch->op_item[batch->op_count] = first;
  return true_v;
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
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, brush->ref.container, VKR_SCENE_EDIT_APPLY);
  if (!item) {
    return false_v;
  }
  ops_item_target(item, &brush->faces[face]);
  item->request.values.fields = VKR_SCENE_EDIT_COMPONENT;
  item->request.values.component_type = &vkr_scene_brush_face_type;
  SceneBrushFace *value = (SceneBrushFace *)item->request.values.component;
  *value = brush->values[face];
  value->distance = brush->planes[face].distance;
  ops_op_result(batch, &brush->faces[face]);
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
  const Mat4 inverse = mat4_inverse_affine(brush->transform.world);
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
  if (item != UINT32_MAX) {
    batch->op_item[batch->op_count] = item;
  }
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
        mat4_transpose(mat4_inverse_affine(cutter->transform.world)),
        vec3_to_vec4(n, 0.0f));
    local[i] = ops_plane_to_brush(
        target, vec3_new(world_normal.x, world_normal.y, world_normal.z),
        mat4_mul_vec3(cutter->transform.world, on_plane));
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
                    "Carving '%s' needs a piece of more than %u faces",
                    target->name, VKR_BRUSH_FACE_MAX);
  }
  return ops_brush_replace(ctx, batch, target, pieces, count, cutter->values,
                           cutter->count, &target->values[0], true_v, NULL,
                           out_item);
}

/* World box of a read brush, from its planes and pose. */
static bool8_t ops_brush_world_box(const OpsBrushRead *brush,
                                   VkrBrushGeometry *scratch, Vec3 *out_min,
                                   Vec3 *out_max) {
  if (vkr_brush_build(brush->planes, brush->count, scratch, NULL) !=
      VKR_BRUSH_OK) {
    return false_v;
  }
  Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  for (uint32_t i = 0; i < scratch->vertex_count; ++i) {
    const Vec3 p = mat4_mul_vec3(brush->transform.world, scratch->vertices[i]);
    lo = vec3_new(Min(lo.x, p.x), Min(lo.y, p.y), Min(lo.z, p.z));
    hi = vec3_new(Max(hi.x, p.x), Max(hi.y, p.y), Max(hi.z, p.z));
  }
  *out_min = lo;
  *out_max = hi;
  return true_v;
}

/* Whether the batch made, edits or adds under `ref` before the current
   operation, so the scene alone does not show it. */
static bool8_t ops_ref_touched(OpsContext *ctx, const OpsBatch *batch,
                               const OpsRef *ref) {
  if (ref->item >= 0) {
    return true_v;
  }
  for (uint32_t i = 0; batch && i < batch->count; ++i) {
    const VkrSampleEditBatchItem *item = &ctx->ops->items[i];
    if (ops_item_names(item, ref) ||
        (item->request.action == VKR_SCENE_EDIT_CREATE &&
         ops_item_under(item, ref))) {
      return true_v;
    }
  }
  return false_v;
}

/* Visits the world boxes of the brushes at and below `ref` as the batch
   leaves them; a part of the tree the batch does not touch reads the
   scene's box of it. */
typedef void (*OpsBoxVisit)(void *context, Vec3 lo, Vec3 hi);

static void ops_ref_boxes(OpsContext *ctx, const OpsBatch *batch,
                          const OpsRef *ref, VkrBrushGeometry *scratch,
                          OpsBoxVisit visit, void *context, uint32_t depth) {
  const VkrScene *scene = ops_scene(ctx->frame, ref->container);
  if (depth > 16u || !scene) {
    return;
  }
  Vec3 lo = {0};
  Vec3 hi = {0};
  if (!ops_ref_touched(ctx, batch, ref)) {
    if (vkr_editor_entity_world_box(scene, ref->entity, scratch, &lo, &hi)) {
      visit(context, lo, hi);
    }
    return;
  }
  VkrSceneEditValues *values =
      arena_alloc(ops_arena(ctx), sizeof(*values), ARENA_MEMORY_TAG_STRUCT);
  OpsRef parent;
  if (!values || !ops_view(ctx, batch, ref, values, &parent)) {
    return;
  }
  const bool8_t brush =
      ((values->fields & VKR_SCENE_EDIT_COMPONENT) &&
       values->component_type == &vkr_scene_brush_type) ||
      (ref->item < 0 &&
       vkr_scene_get_typed(scene, ref->entity, &vkr_scene_brush_type));
  OpsBrushRead *read = brush ? arena_alloc(ops_arena(ctx), sizeof(*read),
                                           ARENA_MEMORY_TAG_STRUCT)
                             : NULL;
  if (read && ops_brush_read(ctx, batch, ref, read) &&
      ops_brush_world_box(read, scratch, &lo, &hi)) {
    visit(context, lo, hi);
  }
  for (uint32_t i = 0; batch && i < batch->count; ++i) {
    const VkrSampleEditBatchItem *item = &ctx->ops->items[i];
    if (item->request.action == VKR_SCENE_EDIT_CREATE &&
        item->request.values.component_type != &vkr_scene_brush_face_type &&
        ops_item_under(item, ref)) {
      const OpsRef child = {.item = (int32_t)i, .container = ref->container};
      ops_ref_boxes(ctx, batch, &child, scratch, visit, context, depth + 1u);
    }
  }
  for (uint32_t i = 0; ref->item < 0 && i < scene->world->dir.living; ++i) {
    const VkrEntityId child = vkr_entity_id_from_index(scene->world, i);
    if (vkr_scene_entity_alive(scene, child) &&
        ops_parent(scene, child).u64 == ref->entity.u64 &&
        !vkr_scene_entity_is_part(scene, child)) {
      const OpsRef next = {
          .entity = child, .item = -1, .container = ref->container};
      ops_ref_boxes(ctx, batch, &next, scratch, visit, context, depth + 1u);
    }
  }
}

typedef struct OpsBoxUnion {
  Vec3 lo;
  Vec3 hi;
  bool8_t found;
} OpsBoxUnion;

static void ops_box_union_visit(void *context, Vec3 lo, Vec3 hi) {
  OpsBoxUnion *u = context;
  u->lo = vec3_new(Min(u->lo.x, lo.x), Min(u->lo.y, lo.y), Min(u->lo.z, lo.z));
  u->hi = vec3_new(Max(u->hi.x, hi.x), Max(u->hi.y, hi.y), Max(u->hi.z, hi.z));
  u->found = true_v;
}

/* World box of `ref` as the batch leaves it before the current operation:
   its brushes' and the scene's boxes at and below it, else its origin. */
static void ops_ref_box(OpsContext *ctx, const OpsBatch *batch,
                        const OpsRef *ref, VkrBrushGeometry *scratch, Vec3 *lo,
                        Vec3 *hi) {
  if (!ops_ref_touched(ctx, batch, ref)) {
    ops_object_box(ops_scene(ctx->frame, ref->container), ref->entity, scratch,
                   lo, hi);
    return;
  }
  OpsBoxUnion u = {.lo = vec3_new(INFINITY, INFINITY, INFINITY),
                   .hi = vec3_new(-INFINITY, -INFINITY, -INFINITY)};
  ops_ref_boxes(ctx, batch, ref, scratch, ops_box_union_visit, &u, 0u);
  if (u.found) {
    *lo = u.lo;
    *hi = u.hi;
    return;
  }
  Mat4 world = mat4_identity();
  (void)ops_view_world(ctx, batch, ref, &world, 0u);
  *lo = *hi = mat4_position(world);
}

typedef struct OpsFloorVisit {
  Vec3 lo;
  Vec3 hi;
  float32_t floor;
} OpsFloorVisit;

static void ops_floor_visit(void *context, Vec3 lo, Vec3 hi) {
  OpsFloorVisit *f = context;
  if (fabsf(lo.y - f->lo.y) < 0.01f && vkr_editor_box_slab(lo, hi)) {
    f->floor = Max(f->floor, hi.y);
  }
}

/* The floor of `ref`'s box `lo`-`hi`: the top of the slabs at its bottom,
   as a room's floor, else its bottom (vkr_editor_entity_floor). */
static float32_t ops_ref_floor(OpsContext *ctx, const OpsBatch *batch,
                               const OpsRef *ref, VkrBrushGeometry *scratch,
                               Vec3 lo, Vec3 hi) {
  const VkrScene *scene = ops_scene(ctx->frame, ref->container);
  if (!ops_ref_touched(ctx, batch, ref)) {
    return vkr_editor_entity_floor(scene, ref->entity, scratch, lo, hi);
  }
  OpsFloorVisit f = {.lo = lo, .hi = hi, .floor = lo.y};
  ops_ref_boxes(ctx, batch, ref, scratch, ops_floor_visit, &f, 0u);
  return f.floor;
}

static bool8_t ops_view_parent(OpsContext *ctx, const OpsBatch *batch,
                               const OpsRef *ref, OpsRef *out) {
  VkrSceneEditValues *values =
      arena_alloc(ops_arena(ctx), sizeof(*values), ARENA_MEMORY_TAG_STRUCT);
  return values && ops_view(ctx, batch, ref, values, out);
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
  const VkrScene *scene = ops_scene(ctx->frame, cutter->ref.container);
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
    VkrBrushGeometry *scratch =
        arena_alloc(ops_arena(ctx), sizeof(*scratch), ARENA_MEMORY_TAG_STRUCT);
    if (!scratch || !ops_brush_world_box(cutter, scratch, &lo, &hi)) {
      return ops_fail(ctx, OPS_INVALID, "The cutter is not a valid solid");
    }
    for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
      const VkrEntityId other = vkr_entity_id_from_index(scene->world, i);
      Vec3 a = {0};
      Vec3 b = {0};
      bool8_t touched = false_v;
      uint32_t item = UINT32_MAX;
      if ((cutter->ref.item < 0 && other.u64 == cutter->ref.entity.u64) ||
          !vkr_scene_entity_alive(scene, other) ||
          !vkr_scene_get_typed(scene, other, &vkr_scene_brush_type) ||
          !ops_world_bounds(scene, other, &a, &b) ||
          !ops_in_box(a, b, lo, hi)) {
        continue;
      }
      const OpsRef other_ref = {
          .entity = other, .item = -1, .container = cutter->ref.container};
      if (!ops_brush_read(ctx, batch, &other_ref, target) ||
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
    VkrSampleEditBatchItem *item =
        ops_batch_add(ctx, batch, cutter->ref.container, VKR_SCENE_EDIT_DELETE);
    if (!item) {
      return false_v;
    }
    ops_item_target(item, &cutter->ref);
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
  if (item != UINT32_MAX) {
    batch->op_item[batch->op_count] = item;
  }
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
  (void)scene;
  VkrSceneEditValues values;
  OpsRef parent;
  if (!ops_view(ctx, batch, ref, &values, &parent)) {
    return ops_fail(ctx, OPS_NOT_FOUND, "That entity cannot be read");
  }
  /* The shift in the parent's space, as the batch leaves the parent. */
  Vec3 local = shift;
  Mat4 parent_world = mat4_identity();
  if ((parent.item >= 0 || parent.entity.u64) &&
      ops_view_world(ctx, batch, &parent, &parent_world, 0u)) {
    const Vec4 moved = mat4_mul_vec4(mat4_inverse_affine(parent_world),
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
    if (!scene) {
      return ops_fail(ctx, OPS_INVALID, "'brushes' must name objects");
    }
    ops_ref_box(ctx, batch, &ref, geometry, &lo, &hi);
    float32_t floor = ops_ref_floor(ctx, batch, &ref, geometry, lo, hi);
    const bool8_t free =
        ref.item < 0 && vkr_editor_entity_free(scene, ref.entity);
    if (placed > 0u && !free) {
      const Vec3 best = ops_snap_best(lo, hi, floor, lows, highs, floors,
                                      placed, (float32_t)gap);
      lo = vec3_add(lo, best);
      hi = vec3_add(hi, best);
      floor += best.y;
      if (vec3_length(best) > 1.0e-6f) {
        if (!ops_move_world(ctx, batch, &ref, scene, best)) {
          return false_v;
        }
        ops_op_result(batch, &ref);
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
        !ops_brush_read(ctx, batch, &ref, &brushes[index]) ||
        !ops_brush_unscaled(ctx, &brushes[index]) ||
        ref.container != brushes[0].ref.container) {
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
          mat4_transpose(mat4_inverse_affine(brushes[index].transform.world)),
          vec3_to_vec4(n, 0.0f));
      planes[total + i] = ops_plane_to_brush(
          &brushes[0], vec3_new(world_normal.x, world_normal.y, world_normal.z),
          mat4_mul_vec3(brushes[index].transform.world,
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
        ctx, batch, brushes[b].ref.container, VKR_SCENE_EDIT_DELETE);
    if (!removal) {
      return false_v;
    }
    ops_item_target(removal, &brushes[b].ref);
  }
  batch->op_item[batch->op_count] = item;
  return true_v;
}

/* A face's world-space unit normal. */
static Vec3 ops_face_world_normal(const OpsBrushRead *brush, uint32_t face) {
  const Vec3 n = vec3_normalize(brush->planes[face].normal);
  const Vec4 world =
      mat4_mul_vec4(mat4_transpose(mat4_inverse_affine(brush->transform.world)),
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
    /* Name the face's own range so the next try lands on it. */
    if (vkr_brush_build(brush->planes, brush->count, geometry, NULL) !=
        VKR_BRUSH_OK) {
      return ops_fail(ctx, OPS_INVALID,
                      "The rectangle does not lie on the face");
    }
    const VkrBrushPolygon polygon = geometry->polygons[face];
    float32_t lo[2] = {INFINITY, INFINITY};
    float32_t hi[2] = {-INFINITY, -INFINITY};
    for (uint32_t i = 0; i < polygon.count; ++i) {
      const Vec3 p = mat4_mul_vec3(brush->transform.world,
                                   geometry->vertices[polygon.first + i]);
      const float32_t at[2] = {vec3_dot(p, u), vec3_dot(p, v)};
      for (uint32_t k = 0; k < 2u; ++k) {
        lo[k] = Min(lo[k], at[k]);
        hi[k] = Max(hi[k], at[k]);
      }
    }
    return ops_fail(ctx, OPS_INVALID,
                    "The rectangle does not lie on the face: it spans u "
                    "[%.4g, %.4g] and v [%.4g, %.4g], along u (%.3g, %.3g, "
                    "%.3g) and v (%.3g, %.3g, %.3g) in world space",
                    lo[0], hi[0], lo[1], hi[1], u.x, u.y, u.z, v.x, v.y, v.z);
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
  const Mat4 inverse = mat4_inverse_affine(brush->transform.world);
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
  if (item != UINT32_MAX) {
    batch->op_item[batch->op_count] = item;
  }
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
  if (change->document[0]) {
    ops_set(ctx, object, "document",
            vkr_bakery_json_cstr(ops_arena(ctx), change->document));
    return object;
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
  batch->op_first[batch->op_count] = batch->count;
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
  {
    char label[96];
    snprintf(label, sizeof(label), "%.*s",
             (int)Min(batch->label.length, (uint64_t)95u),
             batch->label.length ? (const char *)batch->label.str : "");
    vkr_editor_session_note_batch(ctx->editor->session, pending->token,
                                  call->author, label, batch->review);
  }
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
    /* Every other object the operation made, as a room's walls or a
       doorway's pieces; faces and connections are parts of them. */
    VkrBakeryJson *made = vkr_bakery_json_array(call->arena);
    const uint32_t end =
        op + 1u < batch->op_count ? batch->op_first[op + 1u] : batch->count;
    for (uint32_t i = batch->op_first[op]; scene && i < end; ++i) {
      const VkrEntityId other = result->created[i];
      if (other.u64 && other.u64 != entity.u64 &&
          vkr_scene_entity_alive(scene, other) &&
          !vkr_scene_entity_is_part(scene, other)) {
        vkr_bakery_json_append(made, ops_entity(ctx, scene, other));
      }
    }
    if (made->first) {
      ops_set(ctx, entry, "created", made);
    }
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
  /* The Scene image in window points, where ui.click reaches it. */
  if (frame->mapping_valid && frame->ui && frame->ui->content_scale > 0.0f) {
    const Vec4 image = frame->mapping.image_rect_px;
    const float32_t scale = frame->ui->content_scale;
    VkrBakeryJson *rect = vkr_bakery_json_array(arena);
    const float32_t parts[4] = {image.x / scale, image.y / scale,
                                image.z / scale, image.w / scale};
    for (uint32_t i = 0; i < 4u; ++i) {
      vkr_bakery_json_append(rect, ops_number(ctx, parts[i]));
    }
    ops_set(ctx, view, "image", rect);
  }
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

/* Role, face count, build status, and the surfaces, marks and art-owned
   materials its faces use. */
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
  VkrBakeryJson *lists[3] = {vkr_bakery_json_array(arena),
                             vkr_bakery_json_array(arena),
                             vkr_bakery_json_array(arena)};
  for (uint32_t i = 0; i < Min(count, (uint32_t)ArrayCount(faces)); ++i) {
    const SceneBrushFace *face =
        vkr_scene_get_typed(scene, faces[i], &vkr_scene_brush_face_type);
    if (!face) {
      continue;
    }
    const char *values[3] = {(uint32_t)face->surface < VKR_SURFACE_COUNT
                                 ? vkr_surface_names[face->surface]
                                 : "none",
                             (uint32_t)face->mark < VKR_SURFACE_MARK_COUNT
                                 ? vkr_surface_mark_names[face->mark]
                                 : "none",
                             face->material};
    for (uint32_t l = 0; l < ArrayCount(lists); ++l) {
      if (!values[l][0] || (l == 1u && face->mark == VKR_SURFACE_MARK_NONE)) {
        continue;
      }
      VkrBakeryJson *name = vkr_bakery_json_cstr(arena, values[l]);
      bool8_t seen = false_v;
      for (const VkrBakeryJson *m = lists[l]->first; m && !seen; m = m->next) {
        seen = vkr_bakery_json_equal(m, name);
      }
      if (!seen) {
        vkr_bakery_json_append(lists[l], name);
      }
    }
  }
  ops_set(ctx, summary, "surfaces", lists[0]);
  ops_set(ctx, summary, "marks", lists[1]);
  ops_set(ctx, summary, "materials", lists[2]);
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

/* The entities a read lists: those of one container, optionally under
   `root`, overlapping a world `region` and carrying every tag of `tags`. */
typedef struct OpsSelection {
  uint16_t container;
  OpsRef root;
  bool8_t region;
  Vec3 region_min;
  Vec3 region_max;
  SceneTags tags;
} OpsSelection;

static bool8_t ops_selection_read(OpsContext *ctx, const VkrBakeryJson *args,
                                  OpsSelection *out) {
  MemZero(out, sizeof(*out));
  out->root.item = -1;
  const VkrBakeryJson *root_value = vkr_bakery_json_get(args, "root");
  if (root_value) {
    if (!ops_ref(ctx, NULL, root_value, "root", &out->root)) {
      return false_v;
    }
    out->container = out->root.container;
  } else if (!ops_arg_container(ctx, args, &out->container)) {
    return false_v;
  }
  const VkrBakeryJson *region = vkr_bakery_json_get(args, "region");
  if (region && (region->type != VKR_BAKERY_JSON_OBJECT ||
                 !ops_arg_vec3(ctx, region, "min", &out->region_min, NULL) ||
                 !ops_arg_vec3(ctx, region, "max", &out->region_max, NULL))) {
    return ctx->call->error_code
               ? false_v
               : ops_fail(ctx, OPS_INVALID,
                          "'region' is {\"min\": [..], \"max\": [..]}");
  }
  out->region = region != NULL;
  const VkrBakeryJson *tags = vkr_bakery_json_get(args, "tags");
  return !tags || ops_tags_arg(ctx, tags, "tags", &out->tags);
}

/* Whether the selection holds `entity`, with its world bounds when it has
   them. */
static bool8_t ops_selection_has(const VkrScene *scene,
                                 const OpsSelection *selection,
                                 VkrEntityId entity, Vec3 *lo, Vec3 *hi,
                                 bool8_t *bounded) {
  if (selection->root.entity.u64 &&
      !ops_under(scene, entity, selection->root.entity)) {
    return false_v;
  }
  if (selection->tags.text[0]) {
    const SceneTags *held =
        vkr_scene_get_typed(scene, entity, &vkr_scene_tags_type);
    uint32_t cursor = 0u;
    String8 tag = {0};
    while (vkr_scene_tags_next(&selection->tags, &cursor, &tag)) {
      if (!held || !vkr_scene_tags_has(held, tag)) {
        return false_v;
      }
    }
  }
  *bounded = ops_world_bounds(scene, entity, lo, hi);
  if (selection->region) {
    const SceneTransform *transform = ops_transform(scene, entity);
    const Vec3 at = transform ? mat4_position(transform->world) : vec3_zero();
    return *bounded ? ops_in_box(*lo, *hi, selection->region_min,
                                 selection->region_max)
                    : ops_in_box(at, at, selection->region_min,
                                 selection->region_max);
  }
  return true_v;
}

static VkrEditorOpStatus ops_run_describe(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  Arena *arena = ops_arena(ctx);
  OpsSelection selection;
  if (!ops_selection_read(ctx, args, &selection)) {
    return VKR_EDITOR_OP_DONE;
  }
  const uint16_t container = selection.container;
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
        (!faces && vkr_scene_entity_is_part(scene, entity))) {
      continue;
    }
    Vec3 lo = {0};
    Vec3 hi = {0};
    bool8_t bounded = false_v;
    if (!ops_selection_has(scene, &selection, entity, &lo, &hi, &bounded)) {
      continue;
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
    const SceneTags *tags =
        vkr_scene_get_typed(scene, entity, &vkr_scene_tags_type);
    if (tags && tags->text[0]) {
      ops_set(ctx, row, "tags", ops_tags_json(ctx, tags));
    }
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
  ops_set(ctx, result, "tags",
          ops_tags_json(ctx, vkr_scene_get_typed(scene, ref.entity,
                                                 &vkr_scene_tags_type)));
  if (vkr_scene_get_typed(scene, ref.entity, &vkr_scene_brush_type)) {
    ops_set(ctx, result, "brush", ops_brush_summary(ctx, scene, ref.entity));
  }
  /* A spline mesh's or scatter's placed copies and why it placed fewer. */
  if (vkr_scene_get_typed(scene, ref.entity, &vkr_scene_spline_mesh_type) ||
      vkr_scene_get_typed(scene, ref.entity, &vkr_scene_scatter_type)) {
    const char *status = vkr_scene_population_status(scene, ref.entity);
    ops_set(ctx, result, "copies",
            vkr_bakery_json_int(
                arena, vkr_scene_population_instances(scene, ref.entity)));
    ops_set(ctx, result, "status",
            vkr_bakery_json_cstr(arena, status ? status : "placed"));
  }
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

static int ops_tag_compare(const void *a, const void *b) {
  const String8 *left = a;
  const String8 *right = b;
  const int order =
      MemCompare(left->str, right->str, Min(left->length, right->length));
  return order
             ? order
             : (left->length > right->length) - (left->length < right->length);
}

/* One tag and the entities carrying it, for tag.list. */
typedef struct OpsTagCount {
  String8 tag;
  uint32_t count;
} OpsTagCount;

static int ops_tag_count_compare(const void *a, const void *b) {
  const OpsTagCount *left = a;
  const OpsTagCount *right = b;
  if (left->count != right->count) {
    return left->count > right->count ? -1 : 1;
  }
  return ops_tag_compare(&left->tag, &right->tag);
}

/* tag.list: every tag the selected entities carry, most used first. */
static VkrEditorOpStatus ops_run_tag_list(OpsContext *ctx) {
  Arena *arena = ops_arena(ctx);
  OpsSelection selection;
  if (!ops_selection_read(ctx, ctx->call->args, &selection)) {
    return VKR_EDITOR_OP_DONE;
  }
  const VkrScene *scene = ops_scene(ctx->frame, selection.container);
  const uint32_t tagged =
      vkr_scene_find_typed(scene, &vkr_scene_tags_type, NULL, 0u);
  const uint64_t capacity = (uint64_t)tagged * SCENE_TAG_COUNT_MAX;
  String8 *held = capacity ? arena_alloc(arena, capacity * sizeof(*held),
                                         ARENA_MEMORY_TAG_ARRAY)
                           : NULL;
  if (capacity && !held) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  uint64_t count = 0u;
  for (uint32_t i = 0; capacity && i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    const SceneTags *tags =
        vkr_scene_entity_alive(scene, entity)
            ? vkr_scene_get_typed(scene, entity, &vkr_scene_tags_type)
            : NULL;
    Vec3 lo = {0};
    Vec3 hi = {0};
    bool8_t bounded = false_v;
    if (!tags ||
        !ops_selection_has(scene, &selection, entity, &lo, &hi, &bounded)) {
      continue;
    }
    uint32_t cursor = 0u;
    String8 tag = {0};
    while (count < capacity && vkr_scene_tags_next(tags, &cursor, &tag)) {
      held[count++] = tag;
    }
  }
  /* Sorted, equal tags are neighbours: one entry per run. */
  if (count) {
    qsort(held, count, sizeof(*held), ops_tag_compare);
  }
  OpsTagCount *counts = count ? arena_alloc(arena, count * sizeof(*counts),
                                            ARENA_MEMORY_TAG_ARRAY)
                              : NULL;
  if (count && !counts) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  uint64_t distinct = 0u;
  for (uint64_t i = 0; i < count; ++i) {
    if (distinct &&
        ops_tag_compare(&counts[distinct - 1u].tag, &held[i]) == 0) {
      counts[distinct - 1u].count++;
    } else {
      counts[distinct++] = (OpsTagCount){.tag = held[i], .count = 1u};
    }
  }
  if (distinct) {
    qsort(counts, distinct, sizeof(*counts), ops_tag_count_compare);
  }
  VkrBakeryJson *list = vkr_bakery_json_array(arena);
  for (uint64_t i = 0; i < distinct; ++i) {
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    ops_set(ctx, entry, "tag", ops_string(ctx, counts[i].tag));
    ops_set(ctx, entry, "count", vkr_bakery_json_int(arena, counts[i].count));
    vkr_bakery_json_append(list, entry);
  }
  char slot[16];
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  ops_set(ctx, result, "container",
          vkr_bakery_json_cstr(arena, ops_container_name(selection.container,
                                                         slot, sizeof(slot))));
  ops_set(ctx, result, "tags", list);
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

/* query.measure: the distance between two points, with its horizontal run,
   rise and slope, or the world size of an entity and its descendants. */
static VkrEditorOpStatus ops_run_measure(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  Arena *arena = ops_arena(ctx);
  if (vkr_bakery_json_get(args, "entity")) {
    OpsRef ref;
    if (!ops_ref(ctx, NULL, vkr_bakery_json_get(args, "entity"), "entity",
                 &ref)) {
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
    ctx->call->result = vkr_bakery_json_object(arena);
    ops_set(ctx, ctx->call->result, "size", ops_vec3(ctx, vec3_sub(hi, lo)));
    ops_set(ctx, ctx->call->result, "min", ops_vec3(ctx, lo));
    ops_set(ctx, ctx->call->result, "max", ops_vec3(ctx, hi));
    return VKR_EDITOR_OP_DONE;
  }
  Vec3 from = {0};
  Vec3 to = {0};
  bool8_t has_from = false_v;
  bool8_t has_to = false_v;
  if (!ops_arg_vec3(ctx, args, "from", &from, &has_from) ||
      !ops_arg_vec3(ctx, args, "to", &to, &has_to)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!has_from || !has_to) {
    ops_fail(ctx, OPS_INVALID,
             "query.measure needs 'from' and 'to' points, or an 'entity'");
    return VKR_EDITOR_OP_DONE;
  }
  const Vec3 delta = vec3_sub(to, from);
  const float32_t run = sqrtf(delta.x * delta.x + delta.z * delta.z);
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "distance",
          vkr_bakery_json_float(arena, vec3_length(delta)));
  ops_set(ctx, ctx->call->result, "horizontal",
          vkr_bakery_json_float(arena, run));
  ops_set(ctx, ctx->call->result, "rise",
          vkr_bakery_json_float(arena, delta.y));
  ops_set(ctx, ctx->call->result, "slope_degrees",
          vkr_bakery_json_float(arena,
                                atan2f(delta.y, run) * (180.0f / 3.14159265f)));
  ops_set(ctx, ctx->call->result, "delta", ops_vec3(ctx, delta));
  return VKR_EDITOR_OP_DONE;
}

/* view.greybox: every brush face of every loaded container shows its
   surface's greybox look, or its art-owned material again. */
static VkrEditorOpStatus ops_run_greybox(OpsContext *ctx) {
  const VkrBakeryJson *on = vkr_bakery_json_get(ctx->call->args, "on");
  if (!on || on->type != VKR_BAKERY_JSON_BOOL) {
    ops_fail(ctx, OPS_INVALID, "'on' is true or false");
    return VKR_EDITOR_OP_DONE;
  }
  if (!ctx->frame->view_request) {
    ops_fail(ctx, OPS_REJECTED, "This editor has no Scene view");
    return VKR_EDITOR_OP_DONE;
  }
  VkrSampleViewState next = ctx->frame->view_request->apply
                                ? ctx->frame->view_request->value
                                : ctx->frame->view_state;
  next.greybox_view = on->boolean;
  ctx->frame->view_request->value = next;
  ctx->frame->view_request->apply = true_v;
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "greybox",
          vkr_bakery_json_bool(ops_arena(ctx), on->boolean));
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
  /* Brushes share collision bodies by world cell, whose owner is just the
     cell's first brush; the brush hit is the collider. */
  const VkrEntityId collider = {.u64 = hit.collider_entity_id};
  const bool8_t brush =
      collider.u64 &&
      vkr_scene_get_typed(scene, collider, &vkr_scene_brush_type);
  ops_set(ctx, ctx->call->result, "entity",
          ops_entity(ctx, scene,
                     brush ? collider : (VkrEntityId){.u64 = hit.entity_id}));
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
    /* A document change reverts at once through the material journal. */
    if (target->document[0]) {
      char problem[160] = {0};
      if (!vkr_editor_material_group_revert(ctx->editor->materials, ctx->frame,
                                            target->group, problem,
                                            sizeof(problem))) {
        snprintf(target->problem, sizeof(target->problem), "%s", problem);
        target->rejecting = false_v;
        ops_fail(ctx, OPS_REJECTED, "%s", problem);
        return VKR_EDITOR_OP_DONE;
      }
      ops_change_remove(ops, (uint32_t)index);
      call->result = vkr_bakery_json_object(ops_arena(ctx));
      ops_set(ctx, call->result, "rejected", vkr_bakery_json_int(ops_arena(ctx), id));
      return VKR_EDITOR_OP_DONE;
    }
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
  /* A material document step newer (or, redoing, older) than every scene
     step goes first, as the Undo command picks it. */
  const VkrEditorMaterials *materials = ctx->editor->materials;
  const uint64_t material =
      vkr_editor_material_next_sequence(materials, redo);
  const char *owner = NULL;
  if (material && (!best_sequence || (redo ? material < best_sequence
                                           : material > best_sequence))) {
    owner = vkr_editor_material_next_author(materials, redo);
    owner = owner[0] ? owner : NULL;
  } else if (!best) {
    return true_v;
  } else {
    const VkrSceneEditEntry *entry =
        &best->undo[redo ? best->undo_cursor : best->undo_cursor - 1u];
    owner = ops_authored_find(ctx->ops, entry->group, (uint16_t)best_container);
  }
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
  /* query.luminance: measure the Scene's HDR colour at the marks and over
     `region` (fractions of the image: x0, y0, x1, y1) instead of writing
     an image (ADR-099). */
  bool8_t luminance;
  float32_t region[4];
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
  if (ops_arg_number(args, "crouch_height", &value)) {
    out->crouch_height = (float32_t)value;
  }
  if (ops_arg_number(args, "step_up", &value)) {
    out->step_up = (float32_t)value;
  }
  if (ops_arg_number(args, "max_slope", &value)) {
    out->max_slope_radians = (float32_t)value * 0.01745329252f;
  }
  if (!(out->radius > 0.05f) || !(out->height > 2.0f * out->radius) ||
      !(out->crouch_height >= 0.0f) || !(out->step_up >= 0.0f) ||
      !(out->max_slope_radians > 0.0f) ||
      out->max_slope_radians >= 1.5707963f) {
    return ops_fail(ctx, OPS_INVALID,
                    "The capsule needs radius > 0.05, height > 2 radius, "
                    "crouch_height >= 0 (0 cannot crouch), step_up >= 0 and "
                    "max_slope (degrees) between 0 and 90");
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
  /* level.lint: bits of the issue kinds reported, every kind when zero. */
  uint32_t kinds;
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
    /* 'kinds' narrows the report, and the checks run, to those kinds. */
    const VkrBakeryJson *kinds = vkr_bakery_json_get(ctx->call->args, "kinds");
    if (kinds && kinds->type != VKR_BAKERY_JSON_ARRAY) {
      ops_fail(ctx, OPS_INVALID, "'kinds' is an array of issue kinds");
      return VKR_EDITOR_OP_DONE;
    }
    for (uint32_t i = 0; kinds && i < kinds->count; ++i) {
      const VkrBakeryJson *name = vkr_bakery_json_at(kinds, i);
      const VkrEditorLevelIssueKind kind =
          name && name->type == VKR_BAKERY_JSON_STRING
              ? vkr_editor_level_issue_kind(name->string)
              : VKR_EDITOR_LEVEL_ISSUE_COUNT;
      if (kind == VKR_EDITOR_LEVEL_ISSUE_COUNT) {
        ops_fail(ctx, OPS_INVALID,
                 "'kinds' names issue kinds, as z_fight or mover_timing");
        return VKR_EDITOR_OP_DONE;
      }
      pending->kinds |= 1u << kind;
    }
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
  const uint32_t found = vkr_editor_level_job_lint(
      ctx->ops->level_job, scene, has_start ? &start : NULL, pending->kinds,
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
    if (issues[i].kind == VKR_EDITOR_LEVEL_Z_FIGHT) {
      ops_set(ctx, issue, "normal", ops_vec3(ctx, issues[i].normal));
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
                     "'c' passable crouched; ';' passable crouched, out of "
                     "reach; 'S' start; '#' too close to a wall, or a top "
                     "too thin to stand on; 'n' gap too narrow; '_' ceiling "
                     "too low; '/' too steep; '-' no floor"));
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
  VkrEditorLevelRoute route;
  const bool8_t reached = vkr_editor_level_job_reachable(
      ctx->ops->level_job, pending->level.scene, pending->from, pending->to,
      path, ArrayCount(path), &count, &length, &route);
  ops_level_end(ctx->ops);
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *points = vkr_bakery_json_array(arena);
  /* About 'points' (default 32) points along the route, the last, and both
     ends of each change in height beyond a step, as a ladder or a drop, so
     a player driven along the points meets the ladder at its foot. */
  float64_t wanted = 32.0;
  (void)ops_arg_number(args, "points", &wanted);
  const uint32_t budget =
      (uint32_t)vkr_clamp_f64(wanted, 2.0, (float64_t)ArrayCount(path));
  const uint32_t stride = Max(1u, (count + budget - 1u) / budget);
  const float32_t step_up = pending->level.capsule.step_up;
  for (uint32_t i = 0; i < count; ++i) {
    const bool8_t climbs_in =
        i > 0u && fabsf(path[i].y - path[i - 1u].y) > step_up;
    const bool8_t climbs_out =
        i + 1u < count && fabsf(path[i + 1u].y - path[i].y) > step_up;
    if (i % stride == 0u || i + 1u == count || climbs_in || climbs_out) {
      vkr_bakery_json_append(points, ops_vec3(ctx, path[i]));
    }
  }
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "reachable",
          vkr_bakery_json_bool(arena, reached));
  if (reached) {
    ops_set(ctx, ctx->call->result, "length", ops_number(ctx, length));
    ops_set(ctx, ctx->call->result, "path", points);
    /* What the route asks of the player beyond walking. */
    if (route.crouch) {
      ops_set(ctx, ctx->call->result, "crouch", vkr_bakery_json_bool(arena, 1));
    }
    if (route.ladders) {
      ops_set(ctx, ctx->call->result, "ladders",
              vkr_bakery_json_int(arena, route.ladders));
    }
    return VKR_EDITOR_OP_DONE;
  }
  /* Where the way stops: the ends' floors, and the route to the reached
     floor nearest 'to'. */
  ops_set(ctx, ctx->call->result, "from_floor",
          vkr_bakery_json_bool(arena, route.from_found));
  ops_set(ctx, ctx->call->result, "to_floor",
          vkr_bakery_json_bool(arena, route.to_found));
  if (route.from_found) {
    ops_set(ctx, ctx->call->result, "closest", ops_vec3(ctx, route.closest));
    ops_set(ctx, ctx->call->result, "gap", ops_number(ctx, route.gap));
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
  /* A render mode by its Cmd name, such as an artist view (ADR-099). */
  String8 mode = {0};
  if (vkr_bakery_json_get_string(spec, "mode", &mode)) {
    uint32_t i = 0u;
    while (vkr_editor_cmd_render_modes[i] &&
           !ops_equals(mode, vkr_editor_cmd_render_modes[i])) {
      ++i;
    }
    if (!vkr_editor_cmd_render_modes[i]) {
      return ops_fail(ctx, OPS_INVALID,
                      "'mode' is lit, unlit, detail-lighting, lighting-only, "
                      "wireframe, base-color, roughness, metallic, normals, "
                      "material-cost, texel-density or exposure");
    }
    next.render_mode = vkr_editor_cmd_render_mode_values[i];
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
  const Mat4 inverse_view_projection = mat4_inverse(view->view_projection);
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
    /* Collision of every loaded scene hides a mark, as it hides an object
       icon. */
    view->mark_hidden[i] = vkr_editor_label_occluded(
        ctx->frame, view->view_projection, inverse_view_projection,
        pending->marks[i], NULL, VKR_ENTITY_ID_INVALID);
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
/* Scene-linear luminance of the HDR capture at `pixel`: Rec. 709 weights
   over the pre-exposed colour, divided by its pre-exposure. */
static float64_t ops_luminance_at(const VkrCaptureItemResult *item, uint32_t x,
                                  uint32_t y) {
  const uint32_t row = item->origin == VKR_CAPTURE_ORIGIN_BOTTOM_LEFT
                           ? item->height - 1u - y
                           : y;
  const uint16_t *texel = (const uint16_t *)((const uint8_t *)item->data +
                                             (uint64_t)row * item->row_pitch) +
                          (uint64_t)x * 4u;
  const float64_t luminance = 0.2126 * ops_half_to_float(texel[0]) +
                              0.7152 * ops_half_to_float(texel[1]) +
                              0.0722 * ops_half_to_float(texel[2]);
  return item->pre_exposure > 0.0f
             ? Max(luminance, 0.0) / (float64_t)item->pre_exposure
             : 0.0;
}

/* One measurement into `out`: scene-linear luminance, EV100 (log2 of L
   times 100 over the meter constant 12.5) and stops from middle grey after
   the frame's exposure, which the exposure view bands. */
static VkrBakeryJson *ops_luminance_json(OpsContext *ctx, VkrBakeryJson *out,
                                         float64_t luminance,
                                         float32_t exposure) {
  Arena *arena = ops_arena(ctx);
  const float64_t floor = 1.0e-9;
  ops_set(ctx, out, "luminance", vkr_bakery_json_float(arena, luminance));
  ops_set(ctx, out, "ev100",
          vkr_bakery_json_float(arena, log2(Max(luminance, floor) * 8.0)));
  ops_set(ctx, out, "stops",
          vkr_bakery_json_float(
              arena, log2(Max(luminance * (float64_t)exposure, floor) / 0.18)));
  return out;
}

/* query.luminance's answer from the HDR capture: each mark's pixel and
   luminance, and the region's mean, log-average and peak. */
static bool8_t ops_luminance_measure(OpsContext *ctx,
                                     OpsPendingCapture *pending,
                                     const VkrCaptureItemResult *item) {
  if (item->format != VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT || !item->width ||
      !item->height || !item->data) {
    return ops_fail(ctx, OPS_CAPTURE,
                    "The Scene's HDR colour is not available to measure");
  }
  Arena *arena = ops_arena(ctx);
  const float32_t exposure = item->display_exposure;
  VkrBakeryJson *marks = vkr_bakery_json_array(arena);
  const Mat4 view_projection = pending->views[0].view_projection;
  for (uint32_t i = 0; i < pending->mark_count; ++i) {
    VkrBakeryJson *mark = vkr_bakery_json_object(arena);
    if (pending->mark_labels[i][0]) {
      ops_set(ctx, mark, "label",
              vkr_bakery_json_cstr(arena, pending->mark_labels[i]));
    }
    const Vec4 clip =
        mat4_mul_vec4(view_projection, vec3_to_vec4(pending->marks[i], 1.0f));
    const float32_t u = clip.x / clip.w * 0.5f + 0.5f;
    const float32_t v = clip.y / clip.w * 0.5f + 0.5f;
    if (!(clip.w > 1.0e-6f) || !(u >= 0.0f && u < 1.0f) ||
        !(v >= 0.0f && v < 1.0f)) {
      ops_set(ctx, mark, "at", vkr_bakery_json_null(arena));
      vkr_bakery_json_append(marks, mark);
      continue;
    }
    const uint32_t x =
        Min((uint32_t)(u * (float32_t)item->width), item->width - 1u);
    const uint32_t y =
        Min((uint32_t)(v * (float32_t)item->height), item->height - 1u);
    VkrBakeryJson *at = vkr_bakery_json_array(arena);
    vkr_bakery_json_append(at, vkr_bakery_json_float(arena, u));
    vkr_bakery_json_append(at, vkr_bakery_json_float(arena, v));
    ops_set(ctx, mark, "at", at);
    (void)ops_luminance_json(ctx, mark, ops_luminance_at(item, x, y), exposure);
    vkr_bakery_json_append(marks, mark);
  }
  /* The region, a whole number of pixels within the image. */
  const uint32_t x0 = (uint32_t)(pending->region[0] * (float32_t)item->width);
  const uint32_t y0 = (uint32_t)(pending->region[1] * (float32_t)item->height);
  const uint32_t x1 =
      Max(x0 + 1u, Min(item->width, (uint32_t)ceilf(pending->region[2] *
                                                    (float32_t)item->width)));
  const uint32_t y1 =
      Max(y0 + 1u, Min(item->height, (uint32_t)ceilf(pending->region[3] *
                                                     (float32_t)item->height)));
  float64_t sum = 0.0;
  float64_t log_sum = 0.0;
  float64_t peak = 0.0;
  uint64_t count = 0u;
  for (uint32_t y = y0; y < y1 && y < item->height; ++y) {
    for (uint32_t x = x0; x < x1 && x < item->width; ++x) {
      const float64_t luminance = ops_luminance_at(item, x, y);
      sum += luminance;
      log_sum += log2(Max(luminance, 1.0e-9));
      peak = Max(peak, luminance);
      count++;
    }
  }
  VkrBakeryJson *region =
      ops_luminance_json(ctx, vkr_bakery_json_object(arena),
                         count ? sum / (float64_t)count : 0.0, exposure);
  ops_set(ctx, region, "log_average",
          vkr_bakery_json_float(arena, count ? exp2(log_sum / (float64_t)count)
                                             : 0.0));
  ops_set(ctx, region, "peak", vkr_bakery_json_float(arena, peak));
  ops_set(ctx, region, "pixels", vkr_bakery_json_int(arena, (int64_t)count));
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "width",
          vkr_bakery_json_int(arena, item->width));
  ops_set(ctx, ctx->call->result, "height",
          vkr_bakery_json_int(arena, item->height));
  ops_set(ctx, ctx->call->result, "exposure",
          vkr_bakery_json_float(arena, exposure));
  ops_set(ctx, ctx->call->result, "region", region);
  ops_set(ctx, ctx->call->result, "marks", marks);
  return true_v;
}

static VkrEditorOpStatus ops_run_capture(OpsContext *ctx);

/* query.luminance: a capture of the Scene's HDR colour from the current or
   a given view, measured instead of written. */
static VkrEditorOpStatus ops_run_luminance(OpsContext *ctx) {
  VkrEditorOpCall *call = ctx->call;
  if (call->stage == 0u) {
    if (vkr_bakery_json_get(call->args, "views") ||
        vkr_bakery_json_get(call->args, "area")) {
      ops_fail(ctx, OPS_INVALID, "query.luminance measures one Scene view");
      return VKR_EDITOR_OP_DONE;
    }
    float32_t region[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    bool8_t has_region = false_v;
    if (!ops_arg_floats(ctx, call->args, "region", region, 4u, &has_region)) {
      return VKR_EDITOR_OP_DONE;
    }
    if (!(region[0] >= 0.0f && region[1] >= 0.0f && region[2] <= 1.0f &&
          region[3] <= 1.0f && region[0] < region[2] &&
          region[1] < region[3])) {
      ops_fail(ctx, OPS_INVALID,
               "'region' is [x0, y0, x1, y1] in fractions of the image, "
               "x0 < x1 and y0 < y1");
      return VKR_EDITOR_OP_DONE;
    }
    const VkrEditorOpStatus status = ops_run_capture(ctx);
    OpsPendingCapture *pending = call->state;
    if (pending && call->stage == 1u) {
      pending->luminance = true_v;
      MemCopy(pending->region, region, sizeof(region));
    }
    return status;
  }
  return ops_run_capture(ctx);
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
        first->request.value.grid_enabled != frame->view_state.grid_enabled ||
        first->request.value.render_mode != frame->view_state.render_mode;
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
        (VkrSampleCaptureRequest){.request = true_v,
                                  .token = call->token,
                                  .scene_hdr = pending->luminance};
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
    } else if (pending->luminance) {
      (void)ops_luminance_measure(ctx, pending, ready->item);
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
    /* Other editors of a session stage the same field (ADR-106). */
    vkr_editor_session_note_terrain(
        ctx->editor->session, relative, (uint32_t)cells, (float32_t)spacing,
        (float32_t)height_min, (float32_t)height_max, (float32_t)height);
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
  /* Unnamed layers start as the greybox looks of ground surfaces, so paint
     shows at once. */
  const char *const dev_layers[VKR_HEIGHTFIELD_LAYERS] = {
      vkr_surface_greybox_material(VKR_SURFACE_NONE, VKR_SURFACE_MARK_NONE,
                                   VKR_SURFACE_FLOOR),
      vkr_surface_greybox_material(VKR_SURFACE_GRASS, VKR_SURFACE_MARK_NONE,
                                   VKR_SURFACE_FLOOR),
      vkr_surface_greybox_material(VKR_SURFACE_DIRT, VKR_SURFACE_MARK_NONE,
                                   VKR_SURFACE_FLOOR),
      vkr_surface_greybox_material(VKR_SURFACE_ROCK, VKR_SURFACE_MARK_NONE,
                                   VKR_SURFACE_FLOOR)};
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

/* Points of a brush stroke: `points` as up to 256 world positions. */
#define OPS_SCATTER_POINT_MAX 256u

/* scatter.paint (ADR-102): painted areas under a scatter at each point of
   a stroke, or with 'erase' the deletion of its areas under the brush. */
static bool8_t ops_build_scatter_paint(OpsContext *ctx,
                                       const VkrBakeryJson *args,
                                       OpsBatch *batch) {
  OpsRef ref;
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "scatter"), "scatter",
               &ref)) {
    return false_v;
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  const SceneTransform *parent =
      ref.item < 0 && scene ? ops_transform(scene, ref.entity) : NULL;
  if (!parent ||
      !vkr_scene_get_typed(scene, ref.entity, &vkr_scene_scatter_type)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'scatter' must name an existing entity with a scatter "
                    "component");
  }
  Vec3 points[OPS_SCATTER_POINT_MAX];
  const uint32_t count =
      ops_arg_points(ctx, args, "points", points, 1u, OPS_SCATTER_POINT_MAX);
  if (!count) {
    return false_v;
  }
  SceneScatterArea area;
  vkr_type_defaults(&vkr_scene_scatter_area_type, &area);
  float64_t number = 0.0;
  if (ops_arg_number(args, "radius", &number)) {
    area.radius = (float32_t)number;
  }
  if (ops_arg_number(args, "density", &number)) {
    area.density = (float32_t)number;
  }
  if (ops_arg_number(args, "spacing", &number)) {
    area.spacing = (float32_t)number;
  }
  char error[160] = {0};
  if (!vkr_type_validate(&vkr_scene_scatter_area_type, &area, error,
                         sizeof(error))) {
    return ops_fail(ctx, OPS_INVALID, "scatter_area: %s", error);
  }
  uint32_t child_count = 0u;
  const VkrEntityId *children =
      vkr_scene_get_children(scene, ref.entity, &child_count);
  batch->op_target[batch->op_count] = ref.entity;
  /* New areas come after every existing one. */
  uint32_t next_order = 0u;
  for (uint32_t c = 0; c < child_count; ++c) {
    const SceneScatterArea *existing =
        vkr_scene_get_typed(scene, children[c], &vkr_scene_scatter_area_type);
    if (existing && existing->order >= next_order) {
      next_order = existing->order + 1u;
    }
  }

  if (ops_arg_bool(args, "erase", false_v)) {
    /* Areas whose centre lies within the brush of a point, across the
       ground. */
    uint32_t erased = 0u;
    for (uint32_t c = 0; c < child_count; ++c) {
      const SceneTransform *child = ops_transform(scene, children[c]);
      if (!child || !vkr_scene_get_typed(scene, children[c],
                                         &vkr_scene_scatter_area_type)) {
        continue;
      }
      const Vec3 center = mat4_position(child->world);
      bool8_t under = false_v;
      for (uint32_t i = 0; i < count && !under; ++i) {
        const float32_t dx = center.x - points[i].x;
        const float32_t dz = center.z - points[i].z;
        under = dx * dx + dz * dz <= area.radius * area.radius;
      }
      if (!under) {
        continue;
      }
      VkrSampleEditBatchItem *item =
          ops_batch_add(ctx, batch, ref.container, VKR_SCENE_EDIT_DELETE);
      if (!item) {
        return false_v;
      }
      item->request.entity = children[c];
      erased++;
    }
    return erased ||
           ops_fail(ctx, OPS_NOT_FOUND, "No painted area lies under the brush");
  }

  /* Children sit in the scatter's space; seeds follow the point and the
     scatter's area count, so a repeated stroke lands the same way. */
  const Mat4 to_local = mat4_inverse(parent->world);
  for (uint32_t i = 0; i < count; ++i) {
    VkrSampleEditBatchItem *item =
        ops_batch_add(ctx, batch, ref.container, VKR_SCENE_EDIT_CREATE);
    if (!item) {
      return false_v;
    }
    item->request.parent = ref.entity;
    VkrSceneEditValues *values = &item->request.values;
    values->fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM |
                     VKR_SCENE_EDIT_COMPONENT;
    snprintf(values->name, sizeof(values->name), "Area");
    values->position = mat4_mul_vec3(to_local, points[i]);
    values->rotation = vkr_quat_identity();
    values->scale = vec3_one();
    const int32_t key[4] = {(int32_t)lroundf(points[i].x * 1000.0f),
                            (int32_t)lroundf(points[i].y * 1000.0f),
                            (int32_t)lroundf(points[i].z * 1000.0f),
                            (int32_t)(child_count + i)};
    uint32_t seed = 2166136261u;
    const uint8_t *bytes = (const uint8_t *)key;
    for (uint32_t b = 0; b < sizeof(key); ++b) {
      seed = (seed ^ bytes[b]) * 16777619u;
    }
    area.seed = seed ? seed : 1u;
    area.order = next_order + i;
    values->component_type = &vkr_scene_scatter_area_type;
    MemCopy(values->component, &area, sizeof(area));
    if (i == 0u) {
      batch->op_item[batch->op_count] = batch->count - 1u;
    }
    if (!ops_validate_values(ctx, values)) {
      return false_v;
    }
  }
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
/* The static error code the agent channel names `code`; an answer from a
   session host carries it as text. */
static const char *ops_code_named(const char *code) {
  static const char *const codes[] = {
      OPS_MALFORMED, OPS_UNKNOWN, OPS_INVALID, OPS_NOT_FOUND, OPS_REJECTED,
      OPS_BUSY,      OPS_CAPTURE, OPS_LIMIT,   OPS_NOT_OWNER, OPS_CLAIMED};
  for (uint32_t i = 0; i < ArrayCount(codes); ++i) {
    if (strcmp(code, codes[i]) == 0) {
      return codes[i];
    }
  }
  return OPS_REJECTED;
}

/* Sends `ask` to the session host and waits in later builds; see
   ops_host_answer. */
static VkrEditorOpStatus ops_ask_host(OpsContext *ctx,
                                      VkrEditorSessionAsk *ask) {
  if (!ask->author[0]) {
    snprintf(ask->author, sizeof(ask->author), "%s", ctx->call->author);
  }
  const uint64_t token = vkr_editor_session_ask(ctx->editor->session, ask);
  if (!token) {
    ops_fail(ctx, OPS_BUSY, "The request could not reach the session host");
    return VKR_EDITOR_OP_DONE;
  }
  ctx->call->token = token;
  ctx->call->stage = 1u;
  ctx->call->frames = 0u;
  return VKR_EDITOR_OP_WAIT;
}

/* Builds the host may take to answer before the request fails. */
#define OPS_HOST_ANSWER_FRAMES 1800u

/* The host's answer to the request ops_ask_host sent: DONE with `out` set,
   DONE with the host's error, or WAIT. */
static VkrEditorOpStatus ops_host_answer(OpsContext *ctx,
                                         VkrEditorSessionAnswer *out) {
  if (vkr_editor_session_take_answer(ctx->editor->session, ctx->call->token,
                                     out)) {
    if (!out->ok) {
      ops_fail(ctx, ops_code_named(out->code), "%s", out->error);
    }
    return VKR_EDITOR_OP_DONE;
  }
  if (!vkr_editor_session_forwards(ctx->editor->session) ||
      ++ctx->call->frames > OPS_HOST_ANSWER_FRAMES) {
    ops_fail(ctx, OPS_BUSY, "The session host did not answer");
  }
  return ctx->call->error_code ? VKR_EDITOR_OP_DONE : VKR_EDITOR_OP_WAIT;
}

/* Sets `author`'s claim from `want`: a new one when `want->id` is zero,
   else that claim, which must be the author's. Fails with `code` and a
   message in `error`. */
static bool8_t ops_claim_put(VkrEditorOps *ops, const char *author,
                             const VkrEditorClaim *want, VkrEditorClaim *out,
                             const char **code, char *error,
                             uint32_t capacity) {
  const VkrEditorClaim *other =
      ops_claim_hit(ops, author, want->container, want->min, want->max);
  if (other) {
    *code = OPS_CLAIMED;
    snprintf(error, capacity,
             "The box overlaps %s's claim '%s' (claim %u); claim beside it",
             other->author, other->name[0] ? other->name : "unnamed",
             other->id);
    return false_v;
  }
  VkrEditorClaim *claim = NULL;
  for (uint32_t i = 0; want->id && i < ops->claim_count; ++i) {
    if (ops->claims[i].id == want->id) {
      claim = &ops->claims[i];
    }
  }
  if (want->id && (!claim || strcmp(claim->author, author))) {
    *code = claim ? OPS_NOT_OWNER : OPS_NOT_FOUND;
    if (claim) {
      snprintf(error, capacity, "Claim %u belongs to %s", want->id,
               claim->author);
    } else {
      snprintf(error, capacity, "No claim %u", want->id);
    }
    return false_v;
  }
  if (!claim) {
    if (ops->claim_count == VKR_EDITOR_CLAIM_MAX) {
      *code = OPS_LIMIT;
      snprintf(error, capacity, "The editor holds at most %u claims",
               VKR_EDITOR_CLAIM_MAX);
      return false_v;
    }
    claim = &ops->claims[ops->claim_count++];
    *claim = (VkrEditorClaim){.id = ++ops->next_claim_id};
    snprintf(claim->author, sizeof(claim->author), "%s", author);
  }
  claim->container = want->container;
  claim->min = want->min;
  claim->max = want->max;
  if (want->name[0]) {
    snprintf(claim->name, sizeof(claim->name), "%s", want->name);
  }
  OpsFeedEvent *event = ops_feed_add(ops, OPS_FEED_CLAIMED, claim->container,
                                     claim->author, claim->name);
  event->claim = claim->id;
  event->min = claim->min;
  event->max = claim->max;
  event->bounded = true_v;
  ops->shared_revision++;
  *out = *claim;
  return true_v;
}

static VkrEditorOpStatus ops_run_claims_set(OpsContext *ctx) {
  VkrEditorOps *ops = ctx->ops;
  const VkrBakeryJson *args = ctx->call->args;
  if (ctx->call->stage) {
    VkrEditorSessionAnswer answer;
    const VkrEditorOpStatus status = ops_host_answer(ctx, &answer);
    if (status == VKR_EDITOR_OP_DONE && !ctx->call->error_code) {
      ctx->call->result = ops_claim_json(ctx, &answer.claim);
    }
    return status;
  }
  if (!ctx->call->author[0]) {
    ops_fail(ctx, OPS_INVALID,
             "Only an agent claims a region; the designer edits anywhere");
    return VKR_EDITOR_OP_DONE;
  }
  const VkrBakeryJson *region = vkr_bakery_json_get(args, "region");
  VkrEditorClaim want = {0};
  bool8_t has_lo = false_v;
  bool8_t has_hi = false_v;
  if (!region || region->type != VKR_BAKERY_JSON_OBJECT ||
      !ops_arg_vec3(ctx, region, "min", &want.min, &has_lo) ||
      !ops_arg_vec3(ctx, region, "max", &want.max, &has_hi) || !has_lo ||
      !has_hi ||
      !(want.max.x > want.min.x && want.max.y > want.min.y &&
        want.max.z > want.min.z)) {
    if (!ctx->call->error_code) {
      ops_fail(ctx, OPS_INVALID,
               "'region' is {\"min\": [..], \"max\": [..]}, a box with "
               "volume");
    }
    return VKR_EDITOR_OP_DONE;
  }
  if (!ops_arg_container(ctx, args, &want.container)) {
    return VKR_EDITOR_OP_DONE;
  }
  float64_t id_value = 0.0;
  want.id = ops_arg_number(args, "claim", &id_value)
                ? (uint32_t)Max(id_value, 0.0)
                : 0u;
  String8 name = {0};
  if (vkr_bakery_json_get_string(args, "name", &name)) {
    snprintf(want.name, sizeof(want.name), "%.*s",
             (int)Min(name.length, (uint64_t)47u), (const char *)name.str);
  }
  /* In a session the host keeps every editor's claims. */
  if (vkr_editor_session_forwards(ctx->editor->session)) {
    VkrEditorSessionAsk ask = {.kind = VKR_EDITOR_ASK_CLAIM_SET, .claim = want};
    return ops_ask_host(ctx, &ask);
  }
  VkrEditorClaim claim;
  const char *code = NULL;
  char error[256];
  if (!ops_claim_put(ops, ctx->call->author, &want, &claim, &code, error,
                     sizeof(error))) {
    ops_fail(ctx, code, "%s", error);
    return VKR_EDITOR_OP_DONE;
  }
  ctx->call->result = ops_claim_json(ctx, &claim);
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
  ops->shared_revision++;
}

/* Releases claim `id`, or every claim of `author` (any author's with `all`
   from the editor's own requests). The editor's own requests, with an empty
   author, may release any claim. */
static bool8_t ops_claim_drop(VkrEditorOps *ops, const char *author,
                              uint32_t id, bool8_t all, uint32_t *released,
                              uint32_t *released_count, const char **code,
                              char *error, uint32_t capacity) {
  *released_count = 0u;
  if (!id && !author[0] && !all) {
    *code = OPS_INVALID;
    snprintf(error, capacity, "Name a 'claim', or release 'all'");
    return false_v;
  }
  for (uint32_t i = 0; i < ops->claim_count;) {
    const VkrEditorClaim *claim = &ops->claims[i];
    const bool8_t mine = !author[0] || strcmp(claim->author, author) == 0;
    const bool8_t named = id ? claim->id == id : (all || author[0]);
    if (!named) {
      ++i;
      continue;
    }
    if (!mine) {
      if (id) {
        *code = OPS_NOT_OWNER;
        snprintf(error, capacity, "Claim %u belongs to %s", id, claim->author);
        return false_v;
      }
      ++i;
      continue;
    }
    released[(*released_count)++] = claim->id;
    ops_claim_release_at(ops, i);
  }
  if (id && !*released_count) {
    *code = OPS_NOT_FOUND;
    snprintf(error, capacity, "No claim %u", id);
    return false_v;
  }
  return true_v;
}

static VkrBakeryJson *
ops_released_json(OpsContext *ctx, const uint32_t *released, uint32_t count) {
  VkrBakeryJson *ids = vkr_bakery_json_array(ops_arena(ctx));
  for (uint32_t i = 0; i < count; ++i) {
    vkr_bakery_json_append(ids,
                           vkr_bakery_json_int(ops_arena(ctx), released[i]));
  }
  VkrBakeryJson *result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, result, "released", ids);
  return result;
}

/* claims.release: one claim by id, or every claim of the caller; the
   editor's own requests may release any claim, and all with 'all'. */
static VkrEditorOpStatus ops_run_claims_release(OpsContext *ctx) {
  VkrEditorOps *ops = ctx->ops;
  if (ctx->call->stage) {
    VkrEditorSessionAnswer answer;
    const VkrEditorOpStatus status = ops_host_answer(ctx, &answer);
    if (status == VKR_EDITOR_OP_DONE && !ctx->call->error_code) {
      ctx->call->result =
          ops_released_json(ctx, answer.released, answer.released_count);
    }
    return status;
  }
  float64_t id_value = 0.0;
  const uint32_t id = ops_arg_number(ctx->call->args, "claim", &id_value)
                          ? (uint32_t)Max(id_value, 0.0)
                          : 0u;
  const bool8_t all = ops_arg_bool(ctx->call->args, "all", false_v);
  if (vkr_editor_session_forwards(ctx->editor->session)) {
    VkrEditorSessionAsk ask = {
        .kind = VKR_EDITOR_ASK_CLAIM_RELEASE, .claim = {.id = id}, .all = all};
    return ops_ask_host(ctx, &ask);
  }
  uint32_t released[VKR_EDITOR_CLAIM_MAX];
  uint32_t count = 0u;
  const char *code = NULL;
  char error[256];
  if (!ops_claim_drop(ops, ctx->call->author, id, all, released, &count, &code,
                      error, sizeof(error))) {
    ops_fail(ctx, code, "%s", error);
    return VKR_EDITOR_OP_DONE;
  }
  ops_claims_save(ops, ctx->frame);
  ctx->call->result = ops_released_json(ctx, released, count);
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

// -----------------------------------------------------------------------------
// Task board (ADR-106)
// -----------------------------------------------------------------------------

static const char *const s_task_states[] = {"open", "assigned", "done",
                                            "failed"};

static VkrEditorTask *ops_task_find(VkrEditorOps *ops, uint32_t id) {
  for (uint32_t i = 0; id && i < ops->task_count; ++i) {
    if (ops->tasks[i].id == id) {
      return &ops->tasks[i];
    }
  }
  return NULL;
}

/* Adds an open task of `want`'s kind, title and region. A full board drops
   its oldest finished task. */
static bool8_t ops_task_add(VkrEditorOps *ops, const VkrEditorTask *want,
                            VkrEditorTask *out, const char **code, char *error,
                            uint32_t capacity) {
  if (!want->kind[0]) {
    *code = OPS_INVALID;
    snprintf(error, capacity, "A task needs a 'kind'");
    return false_v;
  }
  if (ops->task_count == VKR_EDITOR_TASK_MAX) {
    uint32_t oldest = UINT32_MAX;
    for (uint32_t i = 0; i < ops->task_count; ++i) {
      if (ops->tasks[i].state >= VKR_EDITOR_TASK_DONE &&
          (oldest == UINT32_MAX || ops->tasks[i].id < ops->tasks[oldest].id)) {
        oldest = i;
      }
    }
    if (oldest == UINT32_MAX) {
      *code = OPS_LIMIT;
      snprintf(error, capacity, "The board holds %u unfinished tasks",
               VKR_EDITOR_TASK_MAX);
      return false_v;
    }
    ops->tasks[oldest] = ops->tasks[--ops->task_count];
  }
  VkrEditorTask *task = &ops->tasks[ops->task_count++];
  *task = *want;
  task->id = ++ops->next_task_id;
  task->state = VKR_EDITOR_TASK_OPEN;
  task->assignee[0] = '\0';
  task->note[0] = '\0';
  ops->shared_revision++;
  *out = *task;
  return true_v;
}

/* Whether `kind` is one of the comma-separated `kinds`; empty takes any. */
static bool8_t ops_task_kind_in(const char *kind, const char *kinds) {
  if (!kinds[0]) {
    return true_v;
  }
  const uint64_t length = strlen(kind);
  for (const char *at = kinds; *at;) {
    while (*at == ' ' || *at == ',') {
      ++at;
    }
    const char *end = at;
    while (*end && *end != ',') {
      ++end;
    }
    const char *last = end;
    while (last > at && last[-1] == ' ') {
      --last;
    }
    if ((uint64_t)(last - at) == length && length &&
        strncmp(at, kind, length) == 0) {
      return true_v;
    }
    at = end;
  }
  return false_v;
}

/* Whether every comma-separated item of `needed` is in `have`. */
static bool8_t ops_task_covers(const char *have, const char *needed) {
  char item[64];
  for (const char *at = needed; *at;) {
    while (*at == ' ' || *at == ',') {
      ++at;
    }
    const char *end = at;
    while (*end && *end != ',') {
      ++end;
    }
    const char *last = end;
    while (last > at && last[-1] == ' ') {
      --last;
    }
    const uint64_t length = Min((uint64_t)(last - at), sizeof(item) - 1u);
    if (length) {
      MemCopy(item, at, length);
      item[length] = '\0';
      if (!have[0] || !ops_task_kind_in(item, have)) {
        return false_v;
      }
    }
    at = end;
  }
  return true_v;
}

/* The capabilities this editor gives its agents: its platform, and its
   graphics pipeline class (ADR-087). */
static const char *ops_editor_capabilities(void) {
#if defined(__APPLE__)
  return "macos,tiled";
#elif defined(_WIN32)
  return "windows,desktop";
#else
  return "linux,desktop";
#endif
}

/* The editor part of an author, `agent@editor`; empty for this editor's
   authors outside a session. */
static const char *ops_author_editor(const char *author) {
  const char *at = strchr(author, '@');
  return at ? at + 1 : "";
}

static OpsTaskSlots *ops_task_slots(VkrEditorOps *ops, const char *editor) {
  for (uint32_t i = 0; i < ops->task_slot_count; ++i) {
    if (strcmp(ops->task_slots[i].editor, editor) == 0) {
      return &ops->task_slots[i];
    }
  }
  return NULL;
}

/* Sets how many tasks `editor`'s agents may hold at once; zero lifts the
   limit. */
static bool8_t ops_task_set_slots(VkrEditorOps *ops, const char *editor,
                                  uint32_t slots, const char **code,
                                  char *error, uint32_t capacity) {
  OpsTaskSlots *entry = ops_task_slots(ops, editor);
  if (!entry) {
    if (ops->task_slot_count == ArrayCount(ops->task_slots)) {
      *code = OPS_LIMIT;
      snprintf(error, capacity, "The board tracks %u editors",
               (uint32_t)ArrayCount(ops->task_slots));
      return false_v;
    }
    entry = &ops->task_slots[ops->task_slot_count++];
    snprintf(entry->editor, sizeof(entry->editor), "%s", editor);
  }
  entry->slots = slots;
  return true_v;
}

/* Gives `author` its assigned task, else assigns it the oldest open task of
   one of `kinds` whose requirements `capabilities` covers, unless its
   editor's agents hold as many tasks as its slots. `*out_has` is false when
   none is open or the editor is full. */
static bool8_t ops_task_take(VkrEditorOps *ops, const char *author,
                             const char *kinds, const char *capabilities,
                             VkrEditorTask *out, bool8_t *out_has,
                             const char **code, char *error,
                             uint32_t capacity) {
  *out_has = false_v;
  if (!author[0]) {
    *code = OPS_INVALID;
    snprintf(error, capacity, "Only an agent takes a task");
    return false_v;
  }
  VkrEditorTask *pick = NULL;
  for (uint32_t i = 0; i < ops->task_count; ++i) {
    VkrEditorTask *task = &ops->tasks[i];
    if (task->state == VKR_EDITOR_TASK_ASSIGNED &&
        strcmp(task->assignee, author) == 0) {
      *out = *task;
      *out_has = true_v;
      return true_v;
    }
    if (task->state == VKR_EDITOR_TASK_OPEN &&
        ops_task_kind_in(task->kind, kinds) &&
        ops_task_covers(capabilities, task->requires) &&
        (!pick || task->id < pick->id)) {
      pick = task;
    }
  }
  /* An editor with slots takes no more tasks than it has, so the others go
     to editors with room. */
  const char *editor = ops_author_editor(author);
  const OpsTaskSlots *slots = ops_task_slots(ops, editor);
  uint32_t held = 0u;
  for (uint32_t i = 0; pick && slots && slots->slots && i < ops->task_count;
       ++i) {
    held += ops->tasks[i].state == VKR_EDITOR_TASK_ASSIGNED &&
            strcmp(ops_author_editor(ops->tasks[i].assignee), editor) == 0;
  }
  if (pick && slots && slots->slots && held >= slots->slots) {
    pick = NULL;
  }
  if (pick) {
    pick->state = VKR_EDITOR_TASK_ASSIGNED;
    snprintf(pick->assignee, sizeof(pick->assignee), "%s", author);
    ops->shared_revision++;
    *out = *pick;
    *out_has = true_v;
  }
  return true_v;
}

/* Finishes task `want->id` as `want->state` (done or failed) with its note;
   an agent finishes only its own task, the designer any unfinished one. */
static bool8_t ops_task_finish(VkrEditorOps *ops, const char *author,
                               const VkrEditorTask *want, VkrEditorTask *out,
                               const char **code, char *error,
                               uint32_t capacity) {
  VkrEditorTask *task = ops_task_find(ops, want->id);
  if (!task) {
    *code = OPS_NOT_FOUND;
    snprintf(error, capacity, "No task %u", want->id);
    return false_v;
  }
  if (task->state >= VKR_EDITOR_TASK_DONE ||
      (author[0] && (task->state != VKR_EDITOR_TASK_ASSIGNED ||
                     strcmp(task->assignee, author)))) {
    *code = OPS_NOT_OWNER;
    snprintf(error, capacity, "Task %u is %s%s%s", task->id,
             s_task_states[task->state], task->assignee[0] ? " by " : "",
             task->assignee);
    return false_v;
  }
  task->state = want->state == VKR_EDITOR_TASK_FAILED ? VKR_EDITOR_TASK_FAILED
                                                      : VKR_EDITOR_TASK_DONE;
  snprintf(task->note, sizeof(task->note), "%s", want->note);
  ops->shared_revision++;
  *out = *task;
  return true_v;
}

static VkrBakeryJson *ops_task_json(OpsContext *ctx,
                                    const VkrEditorTask *task) {
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *object = vkr_bakery_json_object(arena);
  ops_set(ctx, object, "task", vkr_bakery_json_int(arena, task->id));
  ops_set(ctx, object, "kind", vkr_bakery_json_cstr(arena, task->kind));
  ops_set(ctx, object, "title", vkr_bakery_json_cstr(arena, task->title));
  ops_set(ctx, object, "state",
          vkr_bakery_json_cstr(arena, s_task_states[task->state & 3u]));
  if (task->assignee[0]) {
    ops_set(ctx, object, "assignee",
            vkr_bakery_json_cstr(arena, task->assignee));
  }
  if (task->note[0]) {
    ops_set(ctx, object, "note", vkr_bakery_json_cstr(arena, task->note));
  }
  if (task->requires[0]) {
    ops_set(ctx, object, "requires",
            vkr_bakery_json_cstr(arena, task->requires));
  }
  if (task->has_region) {
    char slot[16];
    ops_set(
        ctx, object, "container",
        vkr_bakery_json_cstr(
            arena, ops_container_name(task->container, slot, sizeof(slot))));
    ops_set(ctx, object, "region", ops_box(ctx, task->min, task->max));
  }
  return object;
}

/* Answers a task operation from the host's answer, after a forward. */
static VkrEditorOpStatus ops_task_answered(OpsContext *ctx) {
  VkrEditorSessionAnswer answer;
  const VkrEditorOpStatus status = ops_host_answer(ctx, &answer);
  if (status == VKR_EDITOR_OP_DONE && !ctx->call->error_code) {
    ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
    ops_set(ctx, ctx->call->result, "task",
            answer.has_task ? ops_task_json(ctx, &answer.task)
                            : vkr_bakery_json_null(ops_arena(ctx)));
  }
  return status;
}

static VkrEditorOpStatus ops_task_done_with(OpsContext *ctx, bool8_t ok,
                                            bool8_t has,
                                            const VkrEditorTask *task,
                                            const char *code,
                                            const char *error) {
  if (!ok) {
    ops_fail(ctx, code, "%s", error);
    return VKR_EDITOR_OP_DONE;
  }
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "task",
          has ? ops_task_json(ctx, task)
              : vkr_bakery_json_null(ops_arena(ctx)));
  return VKR_EDITOR_OP_DONE;
}

/* Joins the strings of array argument `key` with commas after `out`'s
   text; false, with the failure set, when they do not fit or are not
   strings. */
static bool8_t ops_arg_list(OpsContext *ctx, const char *key, char *out,
                            uint32_t capacity) {
  const VkrBakeryJson *value = vkr_bakery_json_get(ctx->call->args, key);
  if (value && value->type != VKR_BAKERY_JSON_ARRAY) {
    return ops_fail(ctx, OPS_INVALID, "'%s' is an array of strings", key);
  }
  for (const VkrBakeryJson *entry = value ? value->first : NULL; entry;
       entry = entry->next) {
    const uint64_t used = strlen(out);
    if (entry->type != VKR_BAKERY_JSON_STRING ||
        used + entry->string.length + 2u > capacity) {
      return ops_fail(ctx, OPS_INVALID, "'%s' is a short array of strings",
                      key);
    }
    snprintf(out + used, capacity - used, "%s%.*s", used ? "," : "",
             (int)entry->string.length, (const char *)entry->string.str);
  }
  return true_v;
}

static VkrEditorOpStatus ops_run_task_add(OpsContext *ctx) {
  if (ctx->call->stage) {
    return ops_task_answered(ctx);
  }
  const VkrBakeryJson *args = ctx->call->args;
  VkrEditorTask want = {0};
  if (!ops_arg_string(ctx, args, "kind", want.kind, sizeof(want.kind)) ||
      !ops_arg_string(ctx, args, "title", want.title, sizeof(want.title)) ||
      !ops_arg_list(ctx, "requires", want.requires, sizeof(want.requires))) {
    return VKR_EDITOR_OP_DONE;
  }
  const VkrBakeryJson *region = vkr_bakery_json_get(args, "region");
  if (region) {
    bool8_t has_lo = false_v;
    bool8_t has_hi = false_v;
    if (region->type != VKR_BAKERY_JSON_OBJECT ||
        !ops_arg_vec3(ctx, region, "min", &want.min, &has_lo) ||
        !ops_arg_vec3(ctx, region, "max", &want.max, &has_hi) || !has_lo ||
        !has_hi) {
      if (!ctx->call->error_code) {
        ops_fail(ctx, OPS_INVALID,
                 "'region' is {\"min\": [..], \"max\": [..]}");
      }
      return VKR_EDITOR_OP_DONE;
    }
    if (!ops_arg_container(ctx, args, &want.container)) {
      return VKR_EDITOR_OP_DONE;
    }
    want.has_region = true_v;
  }
  if (vkr_editor_session_forwards(ctx->editor->session)) {
    VkrEditorSessionAsk ask = {.kind = VKR_EDITOR_ASK_TASK_ADD, .task = want};
    return ops_ask_host(ctx, &ask);
  }
  VkrEditorTask task;
  const char *code = NULL;
  char error[256] = {0};
  const bool8_t ok =
      ops_task_add(ctx->ops, &want, &task, &code, error, sizeof(error));
  return ops_task_done_with(ctx, ok, true_v, &task, code, error);
}

static VkrEditorOpStatus ops_run_task_next(OpsContext *ctx) {
  if (ctx->call->stage) {
    return ops_task_answered(ctx);
  }
  char kinds[128] = {0};
  char capabilities[160] = {0};
  /* The agent's own capabilities join its editor's. */
  snprintf(capabilities, sizeof(capabilities), "%s", ops_editor_capabilities());
  if (!ops_arg_list(ctx, "kinds", kinds, sizeof(kinds)) ||
      !ops_arg_list(ctx, "capabilities", capabilities, sizeof(capabilities))) {
    return VKR_EDITOR_OP_DONE;
  }
  if (vkr_editor_session_forwards(ctx->editor->session)) {
    VkrEditorSessionAsk ask = {.kind = VKR_EDITOR_ASK_TASK_NEXT};
    snprintf(ask.kinds, sizeof(ask.kinds), "%s", kinds);
    snprintf(ask.capabilities, sizeof(ask.capabilities), "%s", capabilities);
    return ops_ask_host(ctx, &ask);
  }
  VkrEditorTask task;
  bool8_t has = false_v;
  const char *code = NULL;
  char error[256] = {0};
  const bool8_t ok =
      ops_task_take(ctx->ops, ctx->call->author, kinds, capabilities, &task,
                    &has, &code, error, sizeof(error));
  return ops_task_done_with(ctx, ok, has, &task, code, error);
}

static VkrEditorOpStatus ops_run_task_done(OpsContext *ctx) {
  if (ctx->call->stage) {
    return ops_task_answered(ctx);
  }
  const VkrBakeryJson *args = ctx->call->args;
  float64_t id_value = 0.0;
  VkrEditorTask want = {0};
  if (!ops_arg_number(args, "task", &id_value) || id_value < 1.0) {
    ops_fail(ctx, OPS_INVALID, "'task' names a task by id");
    return VKR_EDITOR_OP_DONE;
  }
  want.id = (uint32_t)id_value;
  want.state = ops_arg_bool(args, "ok", true_v) ? VKR_EDITOR_TASK_DONE
                                                : VKR_EDITOR_TASK_FAILED;
  if (!ops_arg_string(ctx, args, "note", want.note, sizeof(want.note))) {
    return VKR_EDITOR_OP_DONE;
  }
  if (vkr_editor_session_forwards(ctx->editor->session)) {
    VkrEditorSessionAsk ask = {.kind = VKR_EDITOR_ASK_TASK_DONE, .task = want};
    return ops_ask_host(ctx, &ask);
  }
  VkrEditorTask task;
  const char *code = NULL;
  char error[256] = {0};
  const bool8_t ok = ops_task_finish(ctx->ops, ctx->call->author, &want, &task,
                                     &code, error, sizeof(error));
  return ops_task_done_with(ctx, ok, true_v, &task, code, error);
}

/* task.slots: how many tasks this editor's agents may hold at once. */
static VkrEditorOpStatus ops_run_task_slots(OpsContext *ctx) {
  if (ctx->call->stage) {
    VkrEditorSessionAnswer answer;
    const VkrEditorOpStatus status = ops_host_answer(ctx, &answer);
    if (status == VKR_EDITOR_OP_DONE && !ctx->call->error_code) {
      ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
    }
    return status;
  }
  float64_t value = -1.0;
  if (!ops_arg_number(ctx->call->args, "slots", &value) || value < 0.0 ||
      value > 1024.0) {
    ops_fail(ctx, OPS_INVALID, "'slots' is 0 (no limit) to 1024");
    return VKR_EDITOR_OP_DONE;
  }
  const uint32_t slots = (uint32_t)value;
  if (vkr_editor_session_forwards(ctx->editor->session)) {
    /* The host reads the editor from the author, so the request carries
       one even for the designer. */
    VkrEditorSessionAsk ask = {.kind = VKR_EDITOR_ASK_TASK_SLOTS,
                               .slots = slots};
    vkr_editor_session_author(ctx->editor->session, "editor", ask.author,
                              sizeof(ask.author));
    return ops_ask_host(ctx, &ask);
  }
  const char *code = NULL;
  char error[256] = {0};
  if (!ops_task_set_slots(ctx->ops,
                          vkr_editor_session_name(ctx->editor->session), slots,
                          &code, error, sizeof(error))) {
    ops_fail(ctx, code, "%s", error);
    return VKR_EDITOR_OP_DONE;
  }
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_task_list(OpsContext *ctx) {
  const VkrEditorOps *ops = ctx->ops;
  String8 state = {0};
  (void)vkr_bakery_json_get_string(ctx->call->args, "state", &state);
  VkrBakeryJson *tasks = vkr_bakery_json_array(ops_arena(ctx));
  for (uint32_t i = 0; i < ops->task_count; ++i) {
    const VkrEditorTask *task = &ops->tasks[i];
    if (!state.length || ops_equals(state, s_task_states[task->state & 3u])) {
      vkr_bakery_json_append(tasks, ops_task_json(ctx, task));
    }
  }
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "tasks", tasks);
  return VKR_EDITOR_OP_DONE;
}

/* changes.feed: the events after sequence `after`, oldest first. */
static VkrEditorOpStatus ops_run_feed(OpsContext *ctx) {
  static const char *const kinds[OPS_FEED_KIND_COUNT] = {
      "applied", "accepted", "rejected", "claimed", "released", "reverted"};
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
  if (ref.container != target.container || ops_ref_same(&ref, &target)) {
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
  VkrBrushGeometry *scratch = arena_alloc(
      ops_arena(ctx), sizeof(VkrBrushGeometry), ARENA_MEMORY_TAG_STRUCT);
  if (!scratch) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  /* Boxes as the batch leaves them, so a batch places what it made. */
  Vec3 elo = {0};
  Vec3 ehi = {0};
  Vec3 tlo = {0};
  Vec3 thi = {0};
  ops_ref_box(ctx, batch, &ref, scratch, &elo, &ehi);
  ops_ref_box(ctx, batch, &target, scratch, &tlo, &thi);
  const Vec3 ec = vec3_scale(vec3_add(elo, ehi), 0.5f);
  const Vec3 tc = vec3_scale(vec3_add(tlo, thi), 0.5f);
  const float32_t g = (float32_t)gap;
  Vec3 delta = vec3_zero();
  if (on || inside) {
    /* On the top, or on the floor inside (a room's floor slab). */
    const float32_t floor =
        on ? thi.y : ops_ref_floor(ctx, batch, &target, scratch, tlo, thi);
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
  if (!ops_move_world(ctx, batch, &ref, scene, delta)) {
    return false_v;
  }
  ops_op_result(batch, &ref);
  return true_v;
}

/* entity.move: an entity shifted by a world offset, in its parent's space,
   as the arrow keys nudge the selection. */
static bool8_t ops_build_move(OpsContext *ctx, const VkrBakeryJson *args,
                              OpsBatch *batch) {
  OpsRef ref;
  Vec3 offset = {0};
  bool8_t has_offset = false_v;
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "entity"), "entity",
               &ref) ||
      !ops_arg_vec3(ctx, args, "offset", &offset, &has_offset)) {
    return false_v;
  }
  if (!has_offset) {
    return ops_fail(ctx, OPS_INVALID,
                    "'offset' is the world [x, y, z] move in meters");
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  if (ref.item < 0 &&
      vkr_scene_get_typed(scene, ref.entity, &vkr_scene_brush_face_type)) {
    return ops_fail(ctx, OPS_INVALID,
                    "A brush face moves with its brush; move the brush, or "
                    "its face with brush.move_face");
  }
  if (!ops_move_world(ctx, batch, &ref, scene, offset)) {
    return false_v;
  }
  ops_op_result(batch, &ref);
  return true_v;
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
/* Surface tags and marks (vkr_surface.h). */
#define OPS_SURFACE_SCHEMA                                                     \
  "{\"type\":\"string\",\"enum\":[\"none\",\"concrete\",\"metal\","            \
  "\"wood\",\"tile\",\"plaster\",\"brick\",\"rock\",\"dirt\",\"grass\","       \
  "\"glass\",\"fabric\",\"water\",\"emissive\"],\"description\":\"What the "   \
  "faces are made of; each shows its fixed greybox look until the art "        \
  "pass binds a material\"}"
#define OPS_MARK_SCHEMA                                                        \
  "{\"type\":\"string\",\"enum\":[\"none\",\"hazard\",\"orange\",\"blue\","    \
  "\"red\",\"green\",\"dark\"],\"description\":\"Level-design accent over "    \
  "the surface's greybox tone: hazard stripes or a wayfinding colour\"}"
#define OPS_BRUSH_SCHEMA                                                       \
  "\"name\":{\"type\":\"string\"},\"parent\":" OPS_ENTITY_SCHEMA               \
  ",\"container\":" OPS_CONTAINER_SCHEMA ",\"role\":{\"type\":\"string\","     \
  "\"enum\":[\"solid\",\"visual\",\"clip\",\"trigger\"]},"                     \
  "\"surface\":" OPS_SURFACE_SCHEMA ",\"mark\":" OPS_MARK_SCHEMA               \
  ",\"grid\":{\"type\":\"number\",\"description\":\"Snap step in "             \
  "meters; 0 turns snapping off\"}"
#define OPS_CAPSULE_SCHEMA                                                     \
  "\"radius\":{\"type\":\"number\"},\"height\":{\"type\":\"number\"},"         \
  "\"crouch_height\":{\"type\":\"number\",\"description\":\"0 cannot "         \
  "crouch\"},"                                                                 \
  "\"step_up\":{\"type\":\"number\"},\"max_slope\":{\"type\":\"number\","      \
  "\"description\":\"Degrees\"}"
#define OPS_SIDE_SCHEMA                                                        \
  "\"side\":{\"type\":\"string\",\"enum\":[\"top\",\"bottom\",\"+x\","         \
  "\"-x\",\"+z\",\"-z\"]}"
#define OPS_VALUES_SCHEMA                                                      \
  "{\"type\":\"object\",\"description\":\"Property values by descriptor "      \
  "name, as scene documents store them\"}"
#define OPS_TAGS_SCHEMA                                                        \
  "{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"array\",\"items\":{"          \
  "\"type\":\"string\"}}],\"description\":\"Tags such as #labs #chair, as "    \
  "one string or an array; lowercased, with '#' added\"}"
#define OPS_TAGS_REGION_SCHEMA                                                 \
  "\"container\":" OPS_CONTAINER_SCHEMA ",\"root\":" OPS_ENTITY_SCHEMA         \
  ",\"region\":{\"type\":\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA  \
  ",\"max\":" OPS_VEC3_SCHEMA                                                  \
  "},\"required\":[\"min\",\"max\"]},\"tags\":" OPS_TAGS_SCHEMA

#define OPS_CAPTURE_VIEW_SCHEMA                                                \
  "{\"type\":\"string\",\"enum\":[\"current\",\"perspective\",\"top\","        \
  "\"left\",\"right\",\"bottom\"]}"
#define OPS_CAPTURE_MODE_SCHEMA                                                \
  "{\"type\":\"string\",\"enum\":[\"lit\",\"unlit\",\"detail-lighting\","      \
  "\"lighting-only\",\"wireframe\",\"base-color\",\"roughness\","              \
  "\"metallic\",\"normals\",\"material-cost\",\"texel-density\","              \
  "\"exposure\"]}"
#define OPS_CAPTURE_FOCUS_SCHEMA                                               \
  "{\"oneOf\":[" OPS_ENTITY_SCHEMA ",{\"type\":\"object\",\"properties\":{"    \
  "\"min\":" OPS_VEC3_SCHEMA ",\"max\":" OPS_VEC3_SCHEMA "},\"required\":["    \
  "\"min\",\"max\"]}]}"

// =============================================================================
// Materials (docs/proposals/artist-toolkit.md, part 2)
// =============================================================================

/* The call's arena as an allocator for the material graph module. */
static bool8_t ops_allocator(OpsContext *ctx, VkrAllocator *out) {
  *out = (VkrAllocator){.ctx = ops_arena(ctx)};
  return vkr_allocator_arena(out) ||
         ops_fail(ctx, OPS_LIMIT, "Out of request memory");
}

static bool8_t ops_material_ends(const char *path, const char *suffix) {
  const size_t length = strlen(path);
  const size_t suffix_length = strlen(suffix);
  return length > suffix_length &&
         strcmp(path + length - suffix_length, suffix) == 0;
}

/* A content-root relative path in `args[key]` naming a material file or,
   with `graph`, a graph; no absolute paths, '..' or backslashes. */
static bool8_t ops_material_path(OpsContext *ctx, const VkrBakeryJson *args,
                                 const char *key, bool8_t graph,
                                 char out[VKR_EDITOR_MATERIAL_PATH]) {
  out[0] = '\0';
  if (!ops_arg_string(ctx, args, key, out, VKR_EDITOR_MATERIAL_PATH)) {
    return false_v;
  }
  const bool8_t kind_ok = graph ? ops_material_ends(out, ".mtg")
                                : (ops_material_ends(out, ".mt") ||
                                   ops_material_ends(out, ".mtg"));
  if (!out[0] || out[0] == '/' || strstr(out, "..") || strchr(out, '\\') ||
      strchr(out, ':') || !kind_ok) {
    return ops_fail(ctx, OPS_INVALID,
                    "'%s' is a content-root relative path to a %s", key,
                    graph ? "graph (.mtg)" : "material (.mt) or graph (.mtg)");
  }
  return true_v;
}

/* Writes one document as this call's journal group; an agent's write with
   `review` (default true) becomes a pending change. */
static bool8_t ops_material_write(OpsContext *ctx, const char *path,
                                  String8 text, bool8_t remove,
                                  const char *label) {
  VkrEditorOpCall *call = ctx->call;
  const uint64_t group = vkr_editor_material_group();
  char error[160] = {0};
  if (!vkr_editor_material_write(ctx->editor->materials, ctx->frame, path, text,
                                 remove, group, call->author, error,
                                 sizeof(error))) {
    return ops_fail(ctx, OPS_REJECTED, "%s", error);
  }
  VkrEditorOps *ops = ctx->ops;
  if (!call->author[0] || !ops_arg_bool(call->args, "review", true_v) ||
      ops->change_count == VKR_EDITOR_CHANGE_MAX) {
    return true_v;
  }
  VkrEditorChange *change = &ops->changes[ops->change_count++];
  MemZero(change, sizeof(*change));
  change->id = ++ops->next_change_id;
  change->group = group;
  change->created = vkr_platform_get_absolute_time();
  snprintf(change->label, sizeof(change->label), "%s", label);
  snprintf(change->author, sizeof(change->author), "%s", call->author);
  snprintf(change->document, sizeof(change->document), "%s", path);
  if (!call->result) {
    call->result = vkr_bakery_json_object(ops_arena(ctx));
  }
  ops_set(ctx, call->result, "change",
          vkr_bakery_json_int(ops_arena(ctx), change->id));
  char toast[160];
  snprintf(toast, sizeof(toast), "Agent change to review: %s", label);
  vkr_editor_toast(ctx->editor, VKR_UI_ICON_MATERIAL,
                   vkr_ui_theme()->accent_hover, toast);
  return true_v;
}

/* Reads graph document `path` into `out`. */
static bool8_t ops_material_graph_load(OpsContext *ctx, const char *path,
                                       VkrMaterialGraph *out) {
  VkrAllocator allocator;
  String8 text = {0};
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  if (!ops_allocator(ctx, &allocator)) {
    return false_v;
  }
  if (!vkr_editor_material_read(&allocator, path, &text)) {
    return ops_fail(ctx, OPS_NOT_FOUND, "Graph '%s' does not open", path);
  }
  if (!vkr_material_graph_read(text, out, error, sizeof(error))) {
    return ops_fail(ctx, OPS_INVALID, "Graph '%s': %s", path, error);
  }
  return true_v;
}

static VkrMaterialGraph *ops_material_graph_alloc(OpsContext *ctx) {
  VkrMaterialGraph *graph =
      arena_alloc(ops_arena(ctx), sizeof(*graph), ARENA_MEMORY_TAG_STRUCT);
  if (!graph) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  return graph;
}

/* How `graph` lowers with `params`: tier, reason, cost and, with
   `definition`, the `.mt` text the loader reads. */
static VkrBakeryJson *ops_material_lowering(OpsContext *ctx,
                                            const VkrMaterialGraph *graph,
                                            const char *graph_path,
                                            const VkrMaterialParam *params,
                                            uint32_t param_count,
                                            bool8_t definition) {
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *report = vkr_bakery_json_object(arena);
  VkrAllocator allocator;
  if (!ops_allocator(ctx, &allocator)) {
    return report;
  }
  VkrMaterialLowering lowering = {0};
  String8 text = {0};
  const bool8_t lowered = vkr_material_graph_lower(
      graph, string8_create_from_cstr((const uint8_t *)graph_path,
                                      strlen(graph_path)),
      params, param_count, &allocator, &text, &lowering);
  const bool8_t custom = lowered && lowering.tier == VKR_MATERIAL_TIER_CUSTOM;
  ops_set(ctx, report, "tier",
          vkr_bakery_json_cstr(arena, !lowered ? "unsupported"
                                      : custom ? "custom"
                                               : "standard"));
  /* Why a graph is not Standard: what stopped it, or what it uses. */
  if (!lowered || custom) {
    ops_set(ctx, report, "reason", vkr_bakery_json_cstr(arena, lowering.reason));
    if (lowering.node[0]) {
      ops_set(ctx, report, "node", vkr_bakery_json_cstr(arena, lowering.node));
    }
  }
  if (custom) {
    ops_set(ctx, report, "function",
            vkr_bakery_json_cstr(arena, lowering.function));
  }
  ops_set(ctx, report, "samples", vkr_bakery_json_int(arena, lowering.samples));
  ops_set(ctx, report, "alu", vkr_bakery_json_int(arena, lowering.alu));
  ops_set(ctx, report, "layers", vkr_bakery_json_int(arena, lowering.layers));
  /* Standard graphs lower to row data: no shader and no pipeline. A Custom
     graph's pipelines serve every instance of it (ADR-096). */
  ops_set(
      ctx, report, "pipelines",
      vkr_bakery_json_int(arena, custom ? VKR_MATERIAL_CUSTOM_PIPELINES : 0));
  if (lowered && definition) {
    ops_set(ctx, report, "definition", ops_string(ctx, text));
  }
  return report;
}

/* The exposed parameters of `graph`: name, type and default. */
static VkrBakeryJson *ops_material_params(OpsContext *ctx,
                                          const VkrMaterialGraph *graph) {
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *list = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    const VkrMaterialNode *node = &graph->nodes[i];
    if (!node->parameter[0]) {
      continue;
    }
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    ops_set(ctx, entry, "name", vkr_bakery_json_cstr(arena, node->parameter));
    ops_set(ctx, entry, "type",
            vkr_bakery_json_cstr(arena,
                                 vkr_material_node_desc(node->kind)->name));
    if (node->kind == VKR_MATERIAL_NODE_SCALAR) {
      ops_set(ctx, entry, "default", ops_number(ctx, node->value.x));
    } else if (node->kind == VKR_MATERIAL_NODE_COLOR) {
      ops_set(ctx, entry, "default",
              ops_vec3(ctx, vec3_new(node->value.x, node->value.y,
                                     node->value.z)));
    } else {
      ops_set(ctx, entry, "default", vkr_bakery_json_cstr(arena, node->path));
    }
    vkr_bakery_json_append(list, entry);
  }
  return list;
}

/* An instance parameter value as `.mt` text: a number, r,g,b or a path. */
static bool8_t ops_material_param_text(OpsContext *ctx,
                                       const VkrBakeryJson *value, char *out,
                                       uint32_t capacity) {
  if (value->type == VKR_BAKERY_JSON_INT) {
    snprintf(out, capacity, "%lld", (long long)value->integer);
    return true_v;
  }
  if (value->type == VKR_BAKERY_JSON_FLOAT) {
    snprintf(out, capacity, "%.9g", value->number);
    return true_v;
  }
  if (value->type == VKR_BAKERY_JSON_STRING &&
      value->string.length < capacity && value->string.length) {
    snprintf(out, capacity, "%.*s", (int)value->string.length,
             (const char *)value->string.str);
    return true_v;
  }
  if (value->type == VKR_BAKERY_JSON_ARRAY && value->count == 3u) {
    float64_t c[3];
    uint32_t i = 0u;
    for (const VkrBakeryJson *item = value->first; item; item = item->next) {
      if (item->type == VKR_BAKERY_JSON_INT) {
        c[i++] = (float64_t)item->integer;
      } else if (item->type == VKR_BAKERY_JSON_FLOAT) {
        c[i++] = item->number;
      } else {
        return ops_fail(ctx, OPS_INVALID, "A colour is three numbers");
      }
    }
    snprintf(out, capacity, "%.9g,%.9g,%.9g", c[0], c[1], c[2]);
    return true_v;
  }
  return ops_fail(ctx, OPS_INVALID,
                  "A parameter value is a number, [r, g, b] or a file path");
}

/* `.mt` text of an instance, in the request's memory. */
static bool8_t ops_material_instance_text(OpsContext *ctx,
                                          const VkrMaterialInstance *instance,
                                          String8 *out) {
  VkrAllocator allocator;
  if (!ops_allocator(ctx, &allocator)) {
    return false_v;
  }
  return vkr_material_instance_write(instance, &allocator, out) ||
         ops_fail(ctx, OPS_LIMIT, "Out of request memory");
}

/* Applies `params` (an object of name to value, null to remove) over the
   instance's overrides. */
static bool8_t ops_material_apply_params(OpsContext *ctx,
                                         const VkrBakeryJson *params,
                                         VkrMaterialInstance *instance) {
  if (!params) {
    return true_v;
  }
  if (params->type != VKR_BAKERY_JSON_OBJECT) {
    return ops_fail(ctx, OPS_INVALID, "'params' maps names to values");
  }
  for (const VkrBakeryJson *item = params->first; item; item = item->next) {
    char name[VKR_MATERIAL_GRAPH_ID_CAPACITY];
    if (!item->key.length || item->key.length >= sizeof(name)) {
      return ops_fail(ctx, OPS_INVALID, "A parameter name is 1 to 31 bytes");
    }
    snprintf(name, sizeof(name), "%.*s", (int)item->key.length,
             (const char *)item->key.str);
    uint32_t at = 0u;
    while (at < instance->param_count &&
           strcmp(instance->params[at].name, name) != 0) {
      at++;
    }
    if (item->type == VKR_BAKERY_JSON_NULL) {
      if (at < instance->param_count) {
        instance->params[at] = instance->params[--instance->param_count];
      }
      continue;
    }
    if (at == instance->param_count) {
      if (instance->param_count == VKR_MATERIAL_GRAPH_PARAM_MAX) {
        return ops_fail(ctx, OPS_LIMIT, "An instance sets at most %u parameters",
                        VKR_MATERIAL_GRAPH_PARAM_MAX);
      }
      instance->param_count++;
      snprintf(instance->params[at].name, sizeof(instance->params[at].name),
               "%s", name);
    }
    if (!ops_material_param_text(ctx, item, instance->params[at].value,
                                 sizeof(instance->params[at].value))) {
      return false_v;
    }
  }
  return true_v;
}

/* Reads instance `path` and its graph. */
static bool8_t ops_material_instance_load(OpsContext *ctx, const char *path,
                                          VkrMaterialInstance *instance,
                                          VkrMaterialGraph *graph,
                                          char graph_path[VKR_EDITOR_MATERIAL_PATH]) {
  VkrAllocator allocator;
  String8 text = {0};
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  if (!ops_allocator(ctx, &allocator)) {
    return false_v;
  }
  if (!vkr_editor_material_read(&allocator, path, &text)) {
    return ops_fail(ctx, OPS_NOT_FOUND, "'%s' does not open", path);
  }
  if (!vkr_material_instance_read(text, instance, error, sizeof(error))) {
    return ops_fail(ctx, OPS_INVALID, "'%s' is no graph instance%s%s", path,
                    error[0] ? ": " : "", error);
  }
  const String8 resolved = vkr_asset_path_resolve(
      &allocator, string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      string8_create_from_cstr((const uint8_t *)instance->graph,
                               strlen(instance->graph)));
  if (!resolved.str || resolved.length >= VKR_EDITOR_MATERIAL_PATH) {
    return ops_fail(ctx, OPS_INVALID, "'%s' names an invalid graph", path);
  }
  snprintf(graph_path, VKR_EDITOR_MATERIAL_PATH, "%s",
           (const char *)resolved.str);
  return ops_material_graph_load(ctx, graph_path, graph);
}

static VkrEditorOpStatus ops_run_material_list(OpsContext *ctx) {
  char filter[128] = "";
  if (!ops_arg_string(ctx, ctx->call->args, "contains", filter,
                      sizeof(filter))) {
    return VKR_EDITOR_OP_DONE;
  }
  enum { LIST_MAX = 512 };
  const char *paths[VKR_EDITOR_MATERIAL_INDEX_MAX];
  const char *graphs[VKR_EDITOR_MATERIAL_INDEX_MAX];
  const uint32_t count = vkr_editor_material_index(
      ctx->editor->materials, ctx->frame->ui->frame_allocator, paths, graphs,
      VKR_EDITOR_MATERIAL_INDEX_MAX);
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *list = vkr_bakery_json_array(arena);
  uint32_t matched = 0u;
  for (uint32_t i = 0; i < Min(count, VKR_EDITOR_MATERIAL_INDEX_MAX); ++i) {
    if (filter[0] && !strstr(paths[i], filter) && !strstr(graphs[i], filter)) {
      continue;
    }
    if (++matched > LIST_MAX) {
      continue;
    }
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    ops_set(ctx, entry, "path", vkr_bakery_json_cstr(arena, paths[i]));
    ops_set(
        ctx, entry, "kind",
        vkr_bakery_json_cstr(
            arena, ops_material_ends(paths[i], ".mtg") ? "graph"
                   : ops_material_ends(paths[i], VKR_SURFACE_THEME_EXTENSION)
                       ? "theme"
                   : graphs[i][0] ? "instance"
                                  : "definition"));
    if (graphs[i][0]) {
      ops_set(ctx, entry, "graph", vkr_bakery_json_cstr(arena, graphs[i]));
    }
    vkr_bakery_json_append(list, entry);
  }
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "materials", list);
  ops_set(ctx, ctx->call->result, "count", vkr_bakery_json_int(arena, matched));
  if (matched > LIST_MAX) {
    ops_set(ctx, ctx->call->result, "truncated", vkr_bakery_json_bool(arena, 1));
  }
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_material_describe(OpsContext *ctx) {
  char path[VKR_EDITOR_MATERIAL_PATH];
  if (!ops_material_path(ctx, ctx->call->args, "path", false_v, path)) {
    return VKR_EDITOR_OP_DONE;
  }
  Arena *arena = ops_arena(ctx);
  VkrAllocator allocator;
  if (!ops_allocator(ctx, &allocator)) {
    return VKR_EDITOR_OP_DONE;
  }
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  ops_set(ctx, result, "path", vkr_bakery_json_cstr(arena, path));
  const VkrEditorMaterialKind kind =
      vkr_editor_material_kind(&allocator, path);
  VkrMaterialGraph *graph = ops_material_graph_alloc(ctx);
  if (!graph) {
    return VKR_EDITOR_OP_DONE;
  }
  if (kind == VKR_EDITOR_MATERIAL_GRAPH) {
    if (!ops_material_graph_load(ctx, path, graph)) {
      return VKR_EDITOR_OP_DONE;
    }
    String8 text = {0};
    ops_set(ctx, result, "kind", vkr_bakery_json_cstr(arena, "graph"));
    if (vkr_material_graph_describe(graph, &allocator, &text)) {
      ops_set(ctx, result, "nodes", ops_string(ctx, text));
    }
    ops_set(ctx, result, "parameters", ops_material_params(ctx, graph));
    ops_set(ctx, result, "lowering",
            ops_material_lowering(ctx, graph, path, NULL, 0u, false_v));
    /* The material files that are instances of this graph. */
    const char *paths[VKR_EDITOR_MATERIAL_INDEX_MAX];
    const char *graphs[VKR_EDITOR_MATERIAL_INDEX_MAX];
    const uint32_t count = Min(
        vkr_editor_material_index(ctx->editor->materials,
                                  ctx->frame->ui->frame_allocator, paths, graphs,
                                  VKR_EDITOR_MATERIAL_INDEX_MAX),
        VKR_EDITOR_MATERIAL_INDEX_MAX);
    VkrBakeryJson *instances = vkr_bakery_json_array(arena);
    for (uint32_t i = 0; i < count; ++i) {
      if (strcmp(graphs[i], path) == 0) {
        vkr_bakery_json_append(instances, vkr_bakery_json_cstr(arena, paths[i]));
      }
    }
    ops_set(ctx, result, "instances", instances);
  } else if (kind == VKR_EDITOR_MATERIAL_INSTANCE) {
    VkrMaterialInstance instance;
    char graph_path[VKR_EDITOR_MATERIAL_PATH];
    if (!ops_material_instance_load(ctx, path, &instance, graph, graph_path)) {
      return VKR_EDITOR_OP_DONE;
    }
    ops_set(ctx, result, "kind", vkr_bakery_json_cstr(arena, "instance"));
    ops_set(ctx, result, "graph", vkr_bakery_json_cstr(arena, graph_path));
    VkrBakeryJson *params = vkr_bakery_json_object(arena);
    for (uint32_t i = 0; i < instance.param_count; ++i) {
      ops_set(ctx, params, instance.params[i].name,
              vkr_bakery_json_cstr(arena, instance.params[i].value));
    }
    ops_set(ctx, result, "params", params);
    if (instance.world_size[0]) {
      ops_set(ctx, result, "world_size",
              vkr_bakery_json_cstr(arena, instance.world_size));
    }
    if (instance.surface[0]) {
      ops_set(ctx, result, "surface",
              vkr_bakery_json_cstr(arena, instance.surface));
    }
    ops_set(ctx, result, "parameters", ops_material_params(ctx, graph));
    ops_set(ctx, result, "lowering",
            ops_material_lowering(ctx, graph, graph_path, instance.params,
                                  instance.param_count, false_v));
  } else {
    String8 text = {0};
    if (!vkr_editor_material_read(&allocator, path, &text)) {
      ops_fail(ctx, OPS_NOT_FOUND, "'%s' does not open", path);
      return VKR_EDITOR_OP_DONE;
    }
    char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
    ops_set(ctx, result, "kind", vkr_bakery_json_cstr(arena, "definition"));
    ops_set(ctx, result, "text", ops_string(ctx, text));
    ops_set(ctx, result, "graph_form",
            vkr_bakery_json_bool(arena, vkr_material_graph_from_definition(
                                            text, graph, error, sizeof(error))));
    if (error[0]) {
      ops_set(ctx, result, "graph_form_reason",
              vkr_bakery_json_cstr(arena, error));
    }
  }
  ctx->call->result = result;
  return VKR_EDITOR_OP_DONE;
}

/* A new graph's starting nodes: a grey base colour, metallic 0 and
   roughness 0.5 into the surface output. */
static void ops_material_template(VkrMaterialGraph *graph) {
  vkr_material_graph_init(graph);
  const uint32_t base =
      vkr_material_graph_add(graph, VKR_MATERIAL_NODE_COLOR, "base_color");
  const uint32_t metallic =
      vkr_material_graph_add(graph, VKR_MATERIAL_NODE_SCALAR, "metallic");
  const uint32_t roughness =
      vkr_material_graph_add(graph, VKR_MATERIAL_NODE_SCALAR, "roughness");
  const uint32_t output =
      vkr_material_graph_add(graph, VKR_MATERIAL_NODE_SURFACE_OUTPUT, "output");
  graph->nodes[base].value = vec4_new(0.8f, 0.8f, 0.8f, 0.0f);
  snprintf(graph->nodes[base].parameter, sizeof(graph->nodes[base].parameter),
           "base_color");
  graph->nodes[metallic].value.x = 0.0f;
  graph->nodes[roughness].value.x = 0.5f;
  snprintf(graph->nodes[roughness].parameter,
           sizeof(graph->nodes[roughness].parameter), "roughness");
  graph->nodes[base].position = vec2_new(0.0f, 0.0f);
  graph->nodes[metallic].position = vec2_new(0.0f, 120.0f);
  graph->nodes[roughness].position = vec2_new(0.0f, 240.0f);
  graph->nodes[output].position = vec2_new(320.0f, 0.0f);
  char error[64];
  (void)vkr_material_graph_connect(graph, base, 0u, output, 0u, error,
                                   sizeof(error));
  (void)vkr_material_graph_connect(graph, metallic, 0u, output, 2u, error,
                                   sizeof(error));
  (void)vkr_material_graph_connect(graph, roughness, 0u, output, 3u, error,
                                   sizeof(error));
}

static VkrEditorOpStatus ops_run_material_create(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  char path[VKR_EDITOR_MATERIAL_PATH];
  String8 kind = {0};
  if (!ops_material_path(ctx, args, "path", false_v, path)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!vkr_bakery_json_get_string(args, "kind", &kind) ||
      (!ops_equals(kind, "graph") && !ops_equals(kind, "instance") &&
       !ops_equals(kind, "definition"))) {
    ops_fail(ctx, OPS_INVALID, "'kind' is graph, instance or definition");
    return VKR_EDITOR_OP_DONE;
  }
  const bool8_t graph_kind = ops_equals(kind, "graph");
  if (graph_kind != ops_material_ends(path, ".mtg")) {
    ops_fail(ctx, OPS_INVALID, "A graph's path ends in .mtg, a material's .mt");
    return VKR_EDITOR_OP_DONE;
  }
  VkrAllocator allocator;
  if (!ops_allocator(ctx, &allocator)) {
    return VKR_EDITOR_OP_DONE;
  }
  String8 existing = {0};
  if (vkr_editor_material_read(&allocator, path, &existing) &&
      !ops_arg_bool(args, "overwrite", false_v)) {
    ops_fail(ctx, OPS_INVALID, "'%s' exists; pass \"overwrite\": true", path);
    return VKR_EDITOR_OP_DONE;
  }
  char from[VKR_EDITOR_MATERIAL_PATH] = "";
  if (vkr_bakery_json_get(args, "from") &&
      !ops_material_path(ctx, args, "from", false_v, from)) {
    return VKR_EDITOR_OP_DONE;
  }
  String8 text = {0};
  if (graph_kind) {
    VkrMaterialGraph *graph = ops_material_graph_alloc(ctx);
    if (!graph) {
      return VKR_EDITOR_OP_DONE;
    }
    if (from[0]) {
      String8 definition = {0};
      char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
      if (!vkr_editor_material_read(&allocator, from, &definition)) {
        ops_fail(ctx, OPS_NOT_FOUND, "'%s' does not open", from);
        return VKR_EDITOR_OP_DONE;
      }
      if (!vkr_material_graph_from_definition(definition, graph, error,
                                              sizeof(error))) {
        ops_fail(ctx, OPS_INVALID, "'%s' has no graph form: %s", from, error);
        return VKR_EDITOR_OP_DONE;
      }
      /* Texture paths were relative to the material; make them
         content-root relative for the graph's own folder. */
      for (uint32_t i = 0; i < graph->node_count; ++i) {
        VkrMaterialNode *node = &graph->nodes[i];
        if (node->kind != VKR_MATERIAL_NODE_TEXTURE) {
          continue;
        }
        const String8 resolved = vkr_asset_path_resolve(
            &allocator,
            string8_create_from_cstr((const uint8_t *)from, strlen(from)),
            string8_create_from_cstr((const uint8_t *)node->path,
                                     strlen(node->path)));
        if (resolved.str && resolved.length < sizeof(node->path)) {
          snprintf(node->path, sizeof(node->path), "%s",
                   (const char *)resolved.str);
        }
      }
    } else {
      ops_material_template(graph);
    }
    if (!vkr_material_graph_write(graph, &allocator, &text)) {
      ops_fail(ctx, OPS_LIMIT, "Out of request memory");
      return VKR_EDITOR_OP_DONE;
    }
  } else if (ops_equals(kind, "instance")) {
    VkrMaterialInstance *instance =
        arena_alloc(ops_arena(ctx), sizeof(*instance), ARENA_MEMORY_TAG_STRUCT);
    VkrMaterialGraph *graph = ops_material_graph_alloc(ctx);
    if (!instance || !graph) {
      return VKR_EDITOR_OP_DONE;
    }
    MemZero(instance, sizeof(*instance));
    if (!ops_material_path(ctx, args, "graph", true_v, instance->graph) ||
        !ops_material_graph_load(ctx, instance->graph, graph) ||
        !ops_arg_string(ctx, args, "name", instance->name,
                        sizeof(instance->name)) ||
        !ops_material_apply_params(ctx, vkr_bakery_json_get(args, "params"),
                                   instance) ||
        !ops_material_instance_text(ctx, instance, &text)) {
      return VKR_EDITOR_OP_DONE;
    }
    VkrMaterialLowering lowering = {0};
    String8 definition = {0};
    if (!vkr_material_graph_lower(
            graph,
            string8_create_from_cstr((const uint8_t *)instance->graph,
                                     strlen(instance->graph)),
            instance->params, instance->param_count, &allocator, &definition,
            &lowering)) {
      ops_fail(ctx, OPS_INVALID, "The instance does not lower: %s%s%s",
               lowering.reason, lowering.node[0] ? ", node " : "",
               lowering.node);
      return VKR_EDITOR_OP_DONE;
    }
  } else if (from[0]) {
    if (!vkr_editor_material_read(&allocator, from, &text)) {
      ops_fail(ctx, OPS_NOT_FOUND, "'%s' does not open", from);
      return VKR_EDITOR_OP_DONE;
    }
  } else {
    text = string8_lit("type=pbr\nbase_color=0.8,0.8,0.8,1\nmetallic=0\n"
                       "roughness=0.5\n");
  }
  char label[96];
  snprintf(label, sizeof(label), "Create %.80s", path);
  if (!ops_material_write(ctx, path, text, false_v, label)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!ctx->call->result) {
    ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  }
  ops_set(ctx, ctx->call->result, "path",
          vkr_bakery_json_cstr(ops_arena(ctx), path));
  return VKR_EDITOR_OP_DONE;
}

/* A link argument "node.port" of `graph`: the node and the output port,
   or with `input`, the input port. */
static bool8_t ops_material_port(OpsContext *ctx, const VkrMaterialGraph *graph,
                                 const VkrBakeryJson *edit, const char *key,
                                 bool8_t input, uint32_t *out_node,
                                 uint32_t *out_port) {
  String8 text = {0};
  if (!vkr_bakery_json_get_string(edit, key, &text)) {
    return ops_fail(ctx, OPS_INVALID, "'%s' is \"node.port\"", key);
  }
  uint64_t dot = 0u;
  while (dot < text.length && text.str[dot] != '.') {
    dot++;
  }
  const String8 id = {.str = text.str, .length = dot};
  *out_node = vkr_material_graph_find(graph, id);
  if (*out_node == UINT32_MAX) {
    return ops_fail(ctx, OPS_NOT_FOUND, "No node '%.*s'", (int)id.length,
                    (const char *)id.str);
  }
  const VkrMaterialNodeKind kind = graph->nodes[*out_node].kind;
  if (dot == text.length) {
    if (input) {
      return ops_fail(ctx, OPS_INVALID, "'%s' names an input: \"node.port\"",
                      key);
    }
    *out_port = 0u;
    return true_v;
  }
  const String8 port = {.str = text.str + dot + 1u,
                        .length = text.length - dot - 1u};
  *out_port = input ? vkr_material_node_input(kind, port)
                    : vkr_material_node_output(kind, port);
  if (*out_port == UINT32_MAX) {
    return ops_fail(ctx, OPS_NOT_FOUND, "'%.*s' has no %s '%.*s'",
                    (int)id.length, (const char *)id.str,
                    input ? "input" : "output", (int)port.length,
                    (const char *)port.str);
  }
  return true_v;
}

/* Sets the fields an edit names on node `index`. */
static bool8_t ops_material_node_fields(OpsContext *ctx,
                                        VkrMaterialGraph *graph, uint32_t index,
                                        const VkrBakeryJson *edit) {
  VkrMaterialNode *node = &graph->nodes[index];
  const VkrBakeryJson *value = vkr_bakery_json_get(edit, "value");
  if (value) {
    if (node->kind == VKR_MATERIAL_NODE_SCALAR) {
      float64_t number = 0.0;
      if (!ops_arg_number(edit, "value", &number) || !isfinite(number)) {
        return ops_fail(ctx, OPS_INVALID, "A scalar's value is a number");
      }
      node->value.x = (float32_t)number;
    } else if (node->kind == VKR_MATERIAL_NODE_COLOR) {
      bool8_t has = false_v;
      Vec3 color = {0};
      if (!ops_arg_vec3(ctx, edit, "value", &color, &has)) {
        return false_v;
      }
      node->value = vec4_new(color.x, color.y, color.z, 0.0f);
    } else if (node->kind == VKR_MATERIAL_NODE_LAYER_BLEND) {
      float32_t range[2] = {0.0f, 1.0f};
      if (!ops_arg_floats(ctx, edit, "value", range, 2u, NULL)) {
        return false_v;
      }
      node->value = vec4_new(range[0], range[1], 0.0f, 0.0f);
    } else if (node->kind == VKR_MATERIAL_NODE_TILE_OFFSET) {
      float32_t tile[4] = {1.0f, 1.0f, 0.0f, 0.0f};
      if (!ops_arg_floats(ctx, edit, "value", tile, 4u, NULL)) {
        return false_v;
      }
      node->value = vec4_new(tile[0], tile[1], tile[2], tile[3]);
    } else if (node->kind == VKR_MATERIAL_NODE_WORLD_PLANAR) {
      float64_t meters = 0.0;
      if (!ops_arg_number(edit, "value", &meters) || !(meters > 0.0)) {
        return ops_fail(ctx, OPS_INVALID,
                        "World planar's value is meters a repeat, above zero");
      }
      node->value.x = (float32_t)meters;
    } else {
      return ops_fail(ctx, OPS_INVALID,
                      "Only scalars, colours and layer blends (a [from, to] "
                      "range) have values");
    }
  }
  const bool8_t has_path = node->kind == VKR_MATERIAL_NODE_TEXTURE ||
                           node->kind == VKR_MATERIAL_NODE_LAYER ||
                           node->kind == VKR_MATERIAL_NODE_LAYER_BLEND;
  if (vkr_bakery_json_get(edit, "path")) {
    if (!has_path) {
      return ops_fail(ctx, OPS_INVALID,
                      "Only textures, layers and layer blends (a mask "
                      "texture) have a path");
    }
    if (!ops_arg_string(ctx, edit, "path", node->path, sizeof(node->path))) {
      return false_v;
    }
  }
  String8 mask = {0};
  if (vkr_bakery_json_get_string(edit, "mask", &mask)) {
    uint32_t found = VKR_MATERIAL_GRAPH_MASK_COUNT;
    for (uint32_t m = 0; m < VKR_MATERIAL_GRAPH_MASK_COUNT; ++m) {
      found = ops_equals(mask, vkr_material_graph_mask_names[m]) ? m : found;
    }
    if (node->kind != VKR_MATERIAL_NODE_LAYER_BLEND ||
        found == VKR_MATERIAL_GRAPH_MASK_COUNT) {
      return ops_fail(ctx, OPS_INVALID,
                      "A layer blend's 'mask' is vertex_color, texture, slope "
                      "or height");
    }
    node->mask = (VkrMaterialGraphMask)found;
  }
  if (!ops_arg_string(ctx, edit, "parameter", node->parameter,
                      sizeof(node->parameter))) {
    return false_v;
  }
  String8 space = {0};
  if (vkr_bakery_json_get_string(edit, "color_space", &space)) {
    node->color_space = ops_equals(space, "srgb")     ? VKR_MATERIAL_COLOR_SPACE_SRGB
                        : ops_equals(space, "linear") ? VKR_MATERIAL_COLOR_SPACE_LINEAR
                                                      : VKR_MATERIAL_COLOR_SPACE_AUTO;
  }
  float32_t position[2];
  bool8_t has_position = false_v;
  if (!ops_arg_floats(ctx, edit, "position", position, 2u, &has_position)) {
    return false_v;
  }
  if (has_position) {
    node->position = vec2_new(position[0], position[1]);
  }
  char rename[VKR_MATERIAL_GRAPH_ID_CAPACITY] = "";
  if (!ops_arg_string(ctx, edit, "rename", rename, sizeof(rename))) {
    return false_v;
  }
  if (rename[0]) {
    snprintf(node->id, sizeof(node->id), "%s", rename);
  }
  return true_v;
}

/* `world_size` (meters a texture repeat covers: a number, [x, y], or 0 to
   unset) and `surface` (a tag name, empty to unset) over `world_size` and
   `surface`; each kept when absent. */
static bool8_t ops_material_art_keys(OpsContext *ctx, const VkrBakeryJson *args,
                                     Vec2 *world_size, char *surface,
                                     uint32_t surface_capacity) {
  float64_t uniform = 0.0;
  float32_t pair[2] = {0.0f, 0.0f};
  bool8_t has_pair = false_v;
  const VkrBakeryJson *size = vkr_bakery_json_get(args, "world_size");
  if (size && size->type == VKR_BAKERY_JSON_ARRAY) {
    if (!ops_arg_floats(ctx, args, "world_size", pair, 2u, &has_pair)) {
      return false_v;
    }
  } else if (ops_arg_number(args, "world_size", &uniform)) {
    pair[0] = pair[1] = (float32_t)uniform;
    has_pair = true_v;
  } else if (size) {
    return ops_fail(ctx, OPS_INVALID,
                    "'world_size' is meters per repeat: a number or [x, y]");
  }
  if (has_pair) {
    const bool8_t unset = pair[0] == 0.0f && pair[1] == 0.0f;
    if (!unset && (pair[0] < 0.01f || pair[1] < 0.01f || pair[0] > 1000.0f ||
                   pair[1] > 1000.0f)) {
      return ops_fail(ctx, OPS_INVALID,
                      "'world_size' is 0.01 to 1000 m, or 0 to unset");
    }
    *world_size = vec2_new(pair[0], pair[1]);
  }
  char name[32] = "";
  if (vkr_bakery_json_get(args, "surface")) {
    VkrSurface tag = VKR_SURFACE_NONE;
    if (!ops_arg_string(ctx, args, "surface", name, sizeof(name))) {
      return false_v;
    }
    if (name[0] && !vkr_surface_find(name, &tag)) {
      return ops_fail(ctx, OPS_INVALID,
                      "'surface' is a tag surface.list names, or empty");
    }
    snprintf(surface, surface_capacity, "%s", name);
  }
  return true_v;
}

static bool8_t ops_material_settings(OpsContext *ctx, VkrMaterialGraph *graph,
                                     const VkrBakeryJson *edit) {
  VkrMaterialGraphSettings *settings = &graph->settings;
  String8 alpha = {0};
  if (vkr_bakery_json_get_string(edit, "alpha_mode", &alpha)) {
    static const char *const modes[] = {"infer", "opaque", "mask", "blend"};
    uint32_t mode = UINT32_MAX;
    for (uint32_t i = 0; i < ArrayCount(modes); ++i) {
      mode = ops_equals(alpha, modes[i]) ? i : mode;
    }
    if (mode == UINT32_MAX) {
      return ops_fail(ctx, OPS_INVALID,
                      "'alpha_mode' is infer, opaque, mask or blend");
    }
    settings->alpha_mode = (VkrMaterialGraphAlpha)mode;
  }
  float64_t number = 0.0;
  if (ops_arg_number(edit, "alpha_cutoff", &number)) {
    settings->alpha_cutoff = (float32_t)number;
  }
  if (ops_arg_number(edit, "temporal_reactivity", &number)) {
    settings->temporal_reactivity = (float32_t)number;
  }
  if (ops_arg_number(edit, "roughness_max", &number)) {
    settings->roughness_max = (float32_t)number;
  }
  if (ops_arg_number(edit, "subsurface_profile", &number)) {
    if (number < 0.0 || number > 7.0 || number != floor(number)) {
      return ops_fail(ctx, OPS_INVALID, "'subsurface_profile' is 0 to 7");
    }
    settings->subsurface_profile = (uint32_t)number;
  }
  settings->double_sided =
      ops_arg_bool(edit, "double_sided", settings->double_sided);
  return ops_material_art_keys(ctx, edit, &settings->world_size,
                               settings->surface, sizeof(settings->surface));
}

/* Applies one edit of material.patch to `graph`. */
static bool8_t ops_material_edit(OpsContext *ctx, VkrMaterialGraph *graph,
                                 const VkrBakeryJson *edit) {
  String8 op = {0};
  if (!edit || edit->type != VKR_BAKERY_JSON_OBJECT ||
      !vkr_bakery_json_get_string(edit, "op", &op)) {
    return ops_fail(ctx, OPS_INVALID,
                    "Each edit is an object with an 'op': add, remove, "
                    "connect, disconnect, set or settings");
  }
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  if (ops_equals(op, "add")) {
    String8 type = {0};
    VkrMaterialNodeKind kind = VKR_MATERIAL_NODE_SCALAR;
    char id[VKR_MATERIAL_GRAPH_ID_CAPACITY] = "";
    if (!vkr_bakery_json_get_string(edit, "type", &type) ||
        !vkr_material_node_find(type, &kind)) {
      return ops_fail(ctx, OPS_INVALID,
                      "'type' is scalar, color, texture, multiply, normal_map "
                      "or surface_output");
    }
    if (!ops_arg_string(ctx, edit, "id", id, sizeof(id))) {
      return false_v;
    }
    if (id[0] && vkr_material_graph_find(
                     graph, string8_create_from_cstr((const uint8_t *)id,
                                                     strlen(id))) != UINT32_MAX) {
      return ops_fail(ctx, OPS_INVALID, "Node '%s' exists", id);
    }
    const uint32_t index = vkr_material_graph_add(
        graph, kind, id[0] ? id : vkr_material_node_desc(kind)->name);
    if (index == UINT32_MAX) {
      return ops_fail(ctx, OPS_LIMIT, "A graph holds at most %u nodes",
                      VKR_MATERIAL_GRAPH_NODE_MAX);
    }
    return ops_material_node_fields(ctx, graph, index, edit);
  }
  if (ops_equals(op, "remove") || ops_equals(op, "set")) {
    String8 id = {0};
    if (!vkr_bakery_json_get_string(edit, "id", &id)) {
      return ops_fail(ctx, OPS_INVALID, "'id' names the node");
    }
    const uint32_t index = vkr_material_graph_find(graph, id);
    if (index == UINT32_MAX) {
      return ops_fail(ctx, OPS_NOT_FOUND, "No node '%.*s'", (int)id.length,
                      (const char *)id.str);
    }
    if (ops_equals(op, "remove")) {
      vkr_material_graph_remove(graph, index);
      return true_v;
    }
    return ops_material_node_fields(ctx, graph, index, edit);
  }
  if (ops_equals(op, "connect")) {
    uint32_t from = 0u;
    uint32_t from_port = 0u;
    uint32_t to = 0u;
    uint32_t to_port = 0u;
    if (!ops_material_port(ctx, graph, edit, "from", false_v, &from,
                           &from_port) ||
        !ops_material_port(ctx, graph, edit, "to", true_v, &to, &to_port)) {
      return false_v;
    }
    return vkr_material_graph_connect(graph, from, from_port, to, to_port,
                                      error, sizeof(error))
               ? true_v
               : ops_fail(ctx, OPS_INVALID, "%s", error);
  }
  if (ops_equals(op, "disconnect")) {
    uint32_t to = 0u;
    uint32_t to_port = 0u;
    if (!ops_material_port(ctx, graph, edit, "to", true_v, &to, &to_port)) {
      return false_v;
    }
    graph->nodes[to].inputs[to_port] = (VkrMaterialLink){0};
    return true_v;
  }
  if (ops_equals(op, "settings")) {
    return ops_material_settings(ctx, graph, edit);
  }
  return ops_fail(ctx, OPS_INVALID,
                  "Unknown edit '%.*s': add, remove, connect, disconnect, set "
                  "or settings",
                  (int)op.length, (const char *)op.str);
}

static VkrEditorOpStatus ops_run_material_patch(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  char path[VKR_EDITOR_MATERIAL_PATH];
  if (!ops_material_path(ctx, args, "path", true_v, path)) {
    return VKR_EDITOR_OP_DONE;
  }
  const VkrBakeryJson *edits = vkr_bakery_json_get(args, "edits");
  if (!edits || edits->type != VKR_BAKERY_JSON_ARRAY || !edits->count) {
    ops_fail(ctx, OPS_INVALID, "'edits' lists one or more edits");
    return VKR_EDITOR_OP_DONE;
  }
  VkrMaterialGraph *graph = ops_material_graph_alloc(ctx);
  if (!graph || !ops_material_graph_load(ctx, path, graph)) {
    return VKR_EDITOR_OP_DONE;
  }
  uint32_t index = 0u;
  for (const VkrBakeryJson *edit = edits->first; edit;
       edit = edit->next, ++index) {
    if (!ops_material_edit(ctx, graph, edit)) {
      char message[256];
      snprintf(message, sizeof(message), "edits[%u]: %s", index,
               ctx->call->error);
      snprintf(ctx->call->error, sizeof(ctx->call->error), "%s", message);
      return VKR_EDITOR_OP_DONE;
    }
  }
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  if (!vkr_material_graph_validate(graph, error, sizeof(error))) {
    ops_fail(ctx, OPS_INVALID, "%s; nothing changed", error);
    return VKR_EDITOR_OP_DONE;
  }
  VkrAllocator allocator;
  String8 text = {0};
  if (!ops_allocator(ctx, &allocator) ||
      !vkr_material_graph_write(graph, &allocator, &text)) {
    return VKR_EDITOR_OP_DONE;
  }
  /* The change reads as the agent's label, else as the graph's name. */
  String8 label = {0};
  char label_text[96];
  if (vkr_bakery_json_get_string(args, "label", &label) && label.length) {
    snprintf(label_text, sizeof(label_text), "%.*s",
             (int)Min(label.length, (uint64_t)95u), (const char *)label.str);
  } else {
    snprintf(label_text, sizeof(label_text), "Edit %.80s", path);
  }
  if (!ops_material_write(ctx, path, text, false_v, label_text)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!ctx->call->result) {
    ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  }
  ops_set(ctx, ctx->call->result, "nodes",
          vkr_bakery_json_int(ops_arena(ctx), graph->node_count));
  ops_set(ctx, ctx->call->result, "lowering",
          ops_material_lowering(ctx, graph, path, NULL, 0u, false_v));
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_material_set_param(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  char path[VKR_EDITOR_MATERIAL_PATH];
  if (!ops_material_path(ctx, args, "path", false_v, path)) {
    return VKR_EDITOR_OP_DONE;
  }
  VkrMaterialInstance *instance =
      arena_alloc(ops_arena(ctx), sizeof(*instance), ARENA_MEMORY_TAG_STRUCT);
  VkrMaterialGraph *graph = ops_material_graph_alloc(ctx);
  char graph_path[VKR_EDITOR_MATERIAL_PATH];
  String8 text = {0};
  Vec2 world_size = {0};
  if (!instance || !graph ||
      !ops_material_instance_load(ctx, path, instance, graph, graph_path) ||
      !ops_material_apply_params(ctx, vkr_bakery_json_get(args, "params"),
                                 instance)) {
    return VKR_EDITOR_OP_DONE;
  }
  /* The instance's own world size and surface, written as text. */
  if (instance->world_size[0]) {
    const String8 current = string8_create_from_cstr(
        (const uint8_t *)instance->world_size, strlen(instance->world_size));
    if (!string8_to_vec2(&current, &world_size) &&
        string8_to_f32(&current, &world_size.x)) {
      world_size.y = world_size.x;
    }
  }
  if (!ops_material_art_keys(ctx, args, &world_size, instance->surface,
                             sizeof(instance->surface))) {
    return VKR_EDITOR_OP_DONE;
  }
  if (world_size.x > 0.0f) {
    snprintf(instance->world_size, sizeof(instance->world_size), "%g,%g",
             (double)world_size.x, (double)world_size.y);
  } else {
    instance->world_size[0] = '\0';
  }
  if (!ops_material_instance_text(ctx, instance, &text)) {
    return VKR_EDITOR_OP_DONE;
  }
  VkrAllocator allocator;
  VkrMaterialLowering lowering = {0};
  String8 definition = {0};
  if (!ops_allocator(ctx, &allocator)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!vkr_material_graph_lower(
          graph,
          string8_create_from_cstr((const uint8_t *)graph_path,
                                   strlen(graph_path)),
          instance->params, instance->param_count, &allocator, &definition,
          &lowering)) {
    ops_fail(ctx, OPS_INVALID, "%s%s%s; nothing changed", lowering.reason,
             lowering.node[0] ? ", node " : "", lowering.node);
    return VKR_EDITOR_OP_DONE;
  }
  char label[96];
  snprintf(label, sizeof(label), "Parameters of %.70s", path);
  if (!ops_material_write(ctx, path, text, false_v, label)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!ctx->call->result) {
    ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  }
  ops_set(ctx, ctx->call->result, "lowering",
          ops_material_lowering(ctx, graph, graph_path, instance->params,
                                instance->param_count, false_v));
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_material_compile(OpsContext *ctx) {
  char path[VKR_EDITOR_MATERIAL_PATH];
  if (!ops_material_path(ctx, ctx->call->args, "path", false_v, path)) {
    return VKR_EDITOR_OP_DONE;
  }
  VkrAllocator allocator;
  if (!ops_allocator(ctx, &allocator)) {
    return VKR_EDITOR_OP_DONE;
  }
  VkrMaterialGraph *graph = ops_material_graph_alloc(ctx);
  if (!graph) {
    return VKR_EDITOR_OP_DONE;
  }
  const VkrEditorMaterialKind kind = vkr_editor_material_kind(&allocator, path);
  if (kind == VKR_EDITOR_MATERIAL_GRAPH) {
    if (ops_material_graph_load(ctx, path, graph)) {
      ctx->call->result =
          ops_material_lowering(ctx, graph, path, NULL, 0u, true_v);
    }
    return VKR_EDITOR_OP_DONE;
  }
  if (kind == VKR_EDITOR_MATERIAL_INSTANCE) {
    VkrMaterialInstance *instance =
        arena_alloc(ops_arena(ctx), sizeof(*instance), ARENA_MEMORY_TAG_STRUCT);
    char graph_path[VKR_EDITOR_MATERIAL_PATH];
    if (instance &&
        ops_material_instance_load(ctx, path, instance, graph, graph_path)) {
      ctx->call->result =
          ops_material_lowering(ctx, graph, graph_path, instance->params,
                                instance->param_count, true_v);
    }
    return VKR_EDITOR_OP_DONE;
  }
  /* A plain definition checks as the loader reads it. */
  String8 text = {0};
  VkrParsedMaterialData *parsed =
      arena_alloc(ops_arena(ctx), sizeof(*parsed), ARENA_MEMORY_TAG_STRUCT);
  if (!parsed || !vkr_editor_material_read(&allocator, path, &text)) {
    ops_fail(ctx, OPS_NOT_FOUND, "'%s' does not open", path);
    return VKR_EDITOR_OP_DONE;
  }
  const bool8_t ok = vkr_material_loader_parse_definition(
      &allocator, string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      text, parsed);
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "tier",
          vkr_bakery_json_cstr(ops_arena(ctx), ok ? "standard" : "invalid"));
  ops_set(ctx, ctx->call->result, "pipelines",
          vkr_bakery_json_int(ops_arena(ctx), 0));
  return VKR_EDITOR_OP_DONE;
}

/* material.assign: `material` on one `face`, on a `brush`'s faces (all or
   those `faces` selects), or on an `entity`: a brush's faces, else a mesh's
   submesh `slot` or every submesh through its `material_override`. An
   empty `material` returns faces to their greybox looks and submeshes to
   their own materials. */
static bool8_t ops_build_material_assign(OpsContext *ctx,
                                         const VkrBakeryJson *args,
                                         OpsBatch *batch) {
  char material[VKR_EDITOR_MATERIAL_PATH] = "";
  if (!ops_arg_string(ctx, args, "material", material, sizeof(material))) {
    return false_v;
  }
  if (material[0] && (!ops_material_ends(material, ".mt") ||
                      material[0] == '/' || strstr(material, ".."))) {
    return ops_fail(ctx, OPS_INVALID,
                    "'material' is a content-root relative .mt, or empty to "
                    "clear");
  }
  if (vkr_bakery_json_get(args, "face") || vkr_bakery_json_get(args, "brush")) {
    return ops_build_face_look(ctx, args, batch, true_v);
  }
  OpsRef ref;
  if (!ops_ref(ctx, batch, vkr_bakery_json_get(args, "entity"), "entity",
               &ref)) {
    return false_v;
  }
  const VkrScene *scene = ops_scene(ctx->frame, ref.container);
  Arena *arena = ops_arena(ctx);
  if (scene && (vkr_scene_get_typed(scene, ref.entity, &vkr_scene_brush_type) ||
                vkr_scene_get_typed(scene, ref.entity,
                                    &vkr_scene_brush_face_type))) {
    VkrBakeryJson *face_args = vkr_bakery_json_clone(arena, args);
    const bool8_t face = vkr_scene_get_typed(scene, ref.entity,
                                             &vkr_scene_brush_face_type) != NULL;
    ops_set(ctx, face_args, face ? "face" : "brush",
            vkr_bakery_json_clone(arena, vkr_bakery_json_get(args, "entity")));
    return ops_build_face_look(ctx, face_args, batch, true_v);
  }
  SceneMeshInfo info;
  if (!scene || !vkr_scene_mesh_info(scene, ref.entity, &info) ||
      !info.submeshes) {
    return ops_fail(ctx, OPS_INVALID,
                    "The entity is neither a brush nor a loaded mesh");
  }
  float64_t slot = -1.0;
  if (ops_arg_number(args, "slot", &slot) &&
      (slot < 0.0 || slot >= (float64_t)VKR_MESH_MATERIAL_OVERRIDE_MAX ||
       slot != floor(slot))) {
    return ops_fail(ctx, OPS_INVALID, "'slot' is a submesh from 0 to %u",
                    VKR_MESH_MATERIAL_OVERRIDE_MAX - 1u);
  }
  const uint32_t first = slot >= 0.0 ? (uint32_t)slot : 0u;
  const uint32_t last =
      slot >= 0.0 ? first + 1u
                  : Min(info.submeshes, (uint32_t)VKR_MESH_MATERIAL_OVERRIDE_MAX);
  VkrBakeryJson *values = vkr_bakery_json_object(arena);
  for (uint32_t i = first; i < last; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "material_%u", i);
    ops_set(ctx, values, key, vkr_bakery_json_cstr(arena, material));
  }
  VkrBakeryJson *component = vkr_bakery_json_object(arena);
  ops_set(ctx, component, "entity",
          vkr_bakery_json_clone(arena, vkr_bakery_json_get(args, "entity")));
  ops_set(ctx, component, "type",
          vkr_bakery_json_cstr(arena, vkr_scene_material_override_type.name));
  ops_set(ctx, component, "values", values);
  _Alignas(16) uint8_t current[VKR_TYPE_VALUE_MAX];
  const bool8_t present = ops_component_get(
      scene, ref.entity, &vkr_scene_material_override_type, 0u, current);
  return ops_build_component(ctx, component, batch,
                             present ? VKR_SCENE_EDIT_APPLY
                                     : VKR_SCENE_EDIT_ADD_COMPONENT);
}

/* material.open: shows a document in the Material panel for the
   designer, and with `workbench`, switches to the Art workbench. */
static bool8_t ops_theme_path(OpsContext *ctx, const VkrBakeryJson *args,
                              const char *key,
                              char out[VKR_EDITOR_MATERIAL_PATH]);

static VkrEditorOpStatus ops_run_material_open(OpsContext *ctx) {
  char path[VKR_EDITOR_MATERIAL_PATH];
  String8 given = {0};
  const bool8_t theme =
      vkr_bakery_json_get_string(ctx->call->args, "path", &given) &&
      given.length > 9u &&
      MemCompare(given.str + given.length - 9u, VKR_SURFACE_THEME_EXTENSION,
                 9u) == 0;
  if (theme ? !ops_theme_path(ctx, ctx->call->args, "path", path)
            : !ops_material_path(ctx, ctx->call->args, "path", false_v, path)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!vkr_editor_material_open(ctx->editor->materials, ctx->frame, path)) {
    ops_fail(ctx, OPS_INVALID, "%s does not open in the Material panel", path);
    return VKR_EDITOR_OP_DONE;
  }
  if (ops_arg_bool(ctx->call->args, "workbench", false_v)) {
    const VkrEditorWorkbenches *workbenches = &ctx->editor->workbenches;
    for (uint32_t i = 0; i < workbenches->count; ++i) {
      const VkrEditorWorkbench *item = &workbenches->items[i];
      if (item->kind == VKR_EDITOR_WORKBENCH_ART && !item->custom) {
        char message[128];
        (void)vkr_editor_workbench_request(ctx->editor, ctx->frame, i,
                                           message, sizeof(message));
        break;
      }
    }
  }
  vkr_editor_dock_show(ctx->frame->dock, VKR_UI_DOCK_PANEL_MATERIAL);
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "path",
          vkr_bakery_json_cstr(ops_arena(ctx), path));
  return VKR_EDITOR_OP_DONE;
}

// =============================================================================
// Surface themes and art lint (docs/proposals/artist-toolkit.md, part 6)
// =============================================================================

/* A content-root relative path in `out`: no absolute paths, '..',
   backslashes, colons or quotes, ending in `suffix`. */
static bool8_t ops_content_path(const char *path, const char *suffix,
                                uint32_t capacity) {
  return path[0] && path[0] != '/' && !strstr(path, "..") &&
         !strchr(path, '\\') && !strchr(path, ':') && !strchr(path, '"') &&
         strlen(path) < capacity && ops_material_ends(path, suffix);
}

/* A theme path in `args[key]`; empty when absent. */
static bool8_t ops_theme_path(OpsContext *ctx, const VkrBakeryJson *args,
                              const char *key,
                              char out[VKR_EDITOR_MATERIAL_PATH]) {
  out[0] = '\0';
  if (!ops_arg_string(ctx, args, key, out, VKR_EDITOR_MATERIAL_PATH)) {
    return false_v;
  }
  if (out[0] && !ops_content_path(out, VKR_SURFACE_THEME_EXTENSION,
                                  VKR_SURFACE_THEME_PATH_CAPACITY)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'%s' is a content-root relative theme (.surfaces) under "
                    "%u bytes",
                    key, VKR_SURFACE_THEME_PATH_CAPACITY);
  }
  return true_v;
}

/* Reads theme `path` into `out`; a file that does not exist reads empty
   when `missing_ok`. */
static bool8_t ops_theme_load(OpsContext *ctx, const char *path,
                              VkrSurfaceTheme *out, bool8_t missing_ok) {
  MemZero(out, sizeof(*out));
  VkrAllocator allocator;
  String8 text = {0};
  char error[160] = {0};
  if (!ops_allocator(ctx, &allocator)) {
    return false_v;
  }
  if (!vkr_editor_material_read(&allocator, path, &text)) {
    return missing_ok ||
           ops_fail(ctx, OPS_NOT_FOUND, "Theme '%s' does not open", path);
  }
  if (!vkr_surface_theme_read(text, out, error, sizeof(error))) {
    return ops_fail(ctx, OPS_INVALID, "Theme '%s': %s", path, error);
  }
  return true_v;
}

/* The container's own theme path, and the World's beneath it, as the
   brushes resolve them (vkr_scene_brush.c); empty for none. */
static void ops_theme_paths(const VkrSampleUiFrame *frame, uint16_t container,
                            const char **out_own, const char **out_world) {
  const VkrScene *scene = ops_scene(frame, container);
  const VkrSceneWorldState *state = &scene->world_state;
  *out_own = state->surface_theme_entity.u64 &&
                     state->surface_theme_entity.parts.world == scene->world_id
                 ? state->surface_theme.theme
                 : "";
  *out_world = frame->world && container != VKR_SCENE_WORLD_ROOT_ID
                   ? frame->world->world_state.surface_theme.theme
                   : "";
}

/* Applies `materials` (tag to a .mt path, empty or null to unbind) over
   `theme`. */
static bool8_t ops_theme_apply(OpsContext *ctx, const VkrBakeryJson *materials,
                               VkrSurfaceTheme *theme) {
  if (!materials) {
    return true_v;
  }
  if (materials->type != VKR_BAKERY_JSON_OBJECT) {
    return ops_fail(ctx, OPS_INVALID, "'materials' maps tags to .mt paths");
  }
  for (const VkrBakeryJson *item = materials->first; item; item = item->next) {
    char tag_name[32];
    VkrSurface tag = VKR_SURFACE_NONE;
    snprintf(tag_name, sizeof(tag_name), "%.*s",
             (int)Min(item->key.length, (uint64_t)sizeof(tag_name) - 1u),
             (const char *)item->key.str);
    if (item->key.length >= sizeof(tag_name) ||
        !vkr_surface_find(tag_name, &tag) || tag == VKR_SURFACE_NONE) {
      return ops_fail(ctx, OPS_INVALID,
                      "'%s' is no surface tag; surface.list names them",
                      tag_name);
    }
    char path[VKR_SURFACE_THEME_PATH_CAPACITY] = "";
    String8 text = {0};
    if (item->type == VKR_BAKERY_JSON_STRING) {
      text = item->string;
    } else if (item->type != VKR_BAKERY_JSON_NULL) {
      return ops_fail(ctx, OPS_INVALID,
                      "materials.%s is a .mt path, or empty or null to "
                      "unbind",
                      tag_name);
    }
    if (text.length) {
      snprintf(path, sizeof(path), "%.*s",
               (int)Min(text.length, (uint64_t)sizeof(path) - 1u),
               (const char *)text.str);
      if (text.length >= sizeof(path) ||
          !ops_content_path(path, ".mt", sizeof(path))) {
        return ops_fail(ctx, OPS_INVALID,
                        "materials.%s is a content-root relative .mt under "
                        "%u bytes",
                        tag_name, VKR_SURFACE_THEME_PATH_CAPACITY);
      }
    }
    snprintf(theme->materials[tag], sizeof(theme->materials[tag]), "%s", path);
  }
  return true_v;
}

/* Writes `theme` to `path` as one journal group. */
static bool8_t ops_theme_write(OpsContext *ctx, const char *path,
                               const VkrSurfaceTheme *theme,
                               const char *label) {
  VkrAllocator allocator;
  String8 text = {0};
  if (!ops_allocator(ctx, &allocator)) {
    return false_v;
  }
  if (!vkr_surface_theme_write(theme, &allocator, &text)) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  return ops_material_write(ctx, path, text, false_v, label);
}

static VkrBakeryJson *ops_theme_json(OpsContext *ctx,
                                     const VkrSurfaceTheme *theme) {
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *object = vkr_bakery_json_object(arena);
  for (uint32_t tag = 1; tag < VKR_SURFACE_COUNT; ++tag) {
    if (theme->materials[tag][0]) {
      ops_set(ctx, object, vkr_surface_names[tag],
              vkr_bakery_json_cstr(arena, theme->materials[tag]));
    }
  }
  return object;
}

/* surface.list: the tags and marks, the container's themes, and per tag
   its faces and the material bound to it. */
static VkrEditorOpStatus ops_run_surface_list(OpsContext *ctx) {
  uint16_t container = 0u;
  if (!ops_arg_container(ctx, ctx->call->args, &container)) {
    return VKR_EDITOR_OP_DONE;
  }
  const VkrScene *scene = ops_scene(ctx->frame, container);
  const char *own_path = "";
  const char *world_path = "";
  ops_theme_paths(ctx->frame, container, &own_path, &world_path);
  VkrSurfaceTheme *themes = arena_alloc(ops_arena(ctx), 2u * sizeof(*themes),
                                        ARENA_MEMORY_TAG_STRUCT);
  if (!themes) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  MemZero(themes, 2u * sizeof(*themes));
  if ((own_path[0] && !ops_theme_load(ctx, own_path, &themes[0], true_v)) ||
      (world_path[0] && !ops_theme_load(ctx, world_path, &themes[1], true_v))) {
    return VKR_EDITOR_OP_DONE;
  }

  /* Faces per tag, and those with an art-owned material of their own. */
  uint32_t faces[VKR_SURFACE_COUNT] = {0};
  uint32_t owned = 0u;
  const VkrWorld *world = scene->world;
  for (uint32_t i = 0; i < world->dir.capacity; ++i) {
    if (!world->dir.records[i].chunk) {
      continue;
    }
    const VkrEntityId entity = vkr_entity_id_from_index(world, i);
    const SceneBrushFace *face =
        vkr_scene_get_typed(scene, entity, &vkr_scene_brush_face_type);
    if (!face) {
      continue;
    }
    if (face->material[0]) {
      owned++;
    } else if ((uint32_t)face->surface < VKR_SURFACE_COUNT) {
      faces[face->surface]++;
    }
  }

  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *tags = vkr_bakery_json_array(arena);
  for (uint32_t tag = 0; tag < VKR_SURFACE_COUNT; ++tag) {
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    ops_set(ctx, entry, "name",
            vkr_bakery_json_cstr(arena, vkr_surface_names[tag]));
    ops_set(ctx, entry, "label",
            vkr_bakery_json_cstr(arena, vkr_surface_labels[tag]));
    ops_set(ctx, entry, "faces", vkr_bakery_json_int(arena, faces[tag]));
    const char *bound =
        vkr_surface_theme_material(&themes[0], &themes[1], (VkrSurface)tag);
    if (bound) {
      ops_set(ctx, entry, "material", vkr_bakery_json_cstr(arena, bound));
      ops_set(ctx, entry, "from",
              vkr_bakery_json_cstr(arena, bound == themes[0].materials[tag]
                                              ? "container"
                                              : "world"));
    }
    vkr_bakery_json_append(tags, entry);
  }
  VkrBakeryJson *marks = vkr_bakery_json_array(arena);
  for (uint32_t mark = 0; mark < VKR_SURFACE_MARK_COUNT; ++mark) {
    vkr_bakery_json_append(
        marks, vkr_bakery_json_cstr(arena, vkr_surface_mark_names[mark]));
  }
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "container",
          vkr_bakery_json_int(arena, container));
  ops_set(ctx, ctx->call->result, "theme",
          vkr_bakery_json_cstr(arena, own_path));
  ops_set(ctx, ctx->call->result, "world_theme",
          vkr_bakery_json_cstr(arena, world_path));
  ops_set(ctx, ctx->call->result, "tags", tags);
  ops_set(ctx, ctx->call->result, "marks", marks);
  ops_set(ctx, ctx->call->result, "own_material_faces",
          vkr_bakery_json_int(arena, owned));
  return VKR_EDITOR_OP_DONE;
}

// =============================================================================
// Environment (ADR-098)
// =============================================================================

/* An environment preset path in `args["path"]`. */
static bool8_t ops_environment_path(OpsContext *ctx, const VkrBakeryJson *args,
                                    char out[VKR_EDITOR_MATERIAL_PATH]) {
  out[0] = '\0';
  if (!ops_arg_string(ctx, args, "path", out, VKR_EDITOR_MATERIAL_PATH)) {
    return false_v;
  }
  if (!ops_content_path(out, VKR_EDITOR_ENVIRONMENT_EXTENSION,
                        VKR_EDITOR_MATERIAL_PATH)) {
    return ops_fail(ctx, OPS_INVALID,
                    "'path' is a content-root relative environment preset "
                    "(.environment)");
  }
  return true_v;
}

/* env.describe: each part of a container's environment, where it comes
   from and its values. */
static VkrEditorOpStatus ops_run_env_describe(OpsContext *ctx) {
  uint16_t container = 0u;
  if (!ops_arg_container(ctx, ctx->call->args, &container)) {
    return VKR_EDITOR_OP_DONE;
  }
  Arena *arena = ops_arena(ctx);
  VkrEditorEnvironmentSource *sources =
      arena_alloc(arena, sizeof(*sources) * VKR_EDITOR_ENVIRONMENT_PART_COUNT,
                  ARENA_MEMORY_TAG_STRUCT);
  if (!sources ||
      !vkr_editor_environment_resolve(ctx->frame, container, sources)) {
    ops_fail(ctx, OPS_NOT_FOUND, "The container is not loaded");
    return VKR_EDITOR_OP_DONE;
  }
  VkrBakeryJson *parts = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < VKR_EDITOR_ENVIRONMENT_PART_COUNT; ++i) {
    const VkrEditorEnvironmentPart part = (VkrEditorEnvironmentPart)i;
    const VkrEditorEnvironmentSource *source = &sources[i];
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    ops_set(ctx, entry, "part",
            vkr_bakery_json_cstr(arena, vkr_editor_environment_key(part)));
    ops_set(ctx, entry, "label",
            vkr_bakery_json_cstr(arena, vkr_editor_environment_label(part)));
    const char *from = !source->entity.u64                    ? "unset"
                       : source->from_world                   ? "world"
                       : container == VKR_SCENE_WORLD_ROOT_ID ? "world"
                                                              : "container";
    ops_set(ctx, entry, "from", vkr_bakery_json_cstr(arena, from));
    if (source->entity.u64) {
      ops_set(ctx, entry, "entity",
              ops_entity(ctx, source->owner, source->entity));
    }
    ops_set(ctx, entry, "values",
            ops_component_json(ctx, vkr_editor_environment_type(part),
                               source->value));
    vkr_bakery_json_append(parts, entry);
  }
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "container",
          vkr_bakery_json_int(arena, container));
  ops_set(ctx, ctx->call->result, "parts", parts);
  return VKR_EDITOR_OP_DONE;
}

/* env.preset.save: the container's resolved environment as a preset
   document, through the document journal. */
static VkrEditorOpStatus ops_run_env_preset_save(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  char path[VKR_EDITOR_MATERIAL_PATH];
  uint16_t container = 0u;
  VkrAllocator allocator;
  if (!ops_environment_path(ctx, args, path) ||
      !ops_arg_container(ctx, args, &container) ||
      !ops_allocator(ctx, &allocator)) {
    return VKR_EDITOR_OP_DONE;
  }
  String8 existing = {0};
  if (vkr_editor_material_read(&allocator, path, &existing) &&
      !ops_arg_bool(args, "overwrite", false_v)) {
    ops_fail(ctx, OPS_REJECTED, "'%s' exists; pass \"overwrite\": true", path);
    return VKR_EDITOR_OP_DONE;
  }
  Arena *arena = ops_arena(ctx);
  VkrEditorEnvironmentSource *sources =
      arena_alloc(arena, sizeof(*sources) * VKR_EDITOR_ENVIRONMENT_PART_COUNT,
                  ARENA_MEMORY_TAG_STRUCT);
  VkrEditorEnvironmentPreset *preset =
      arena_alloc(arena, sizeof(*preset), ARENA_MEMORY_TAG_STRUCT);
  String8 text = {0};
  if (!sources || !preset ||
      !vkr_editor_environment_resolve(ctx->frame, container, sources)) {
    ops_fail(ctx, OPS_NOT_FOUND, "The container is not loaded");
    return VKR_EDITOR_OP_DONE;
  }
  vkr_editor_environment_capture(sources, preset);
  char label[96];
  snprintf(label, sizeof(label), "Save environment %.60s", path);
  if (!vkr_editor_environment_write(preset, &allocator, &text)) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  if (!ops_material_write(ctx, path, text, false_v, label)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!ctx->call->result) {
    ctx->call->result = vkr_bakery_json_object(arena);
  }
  VkrBakeryJson *saved = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < VKR_EDITOR_ENVIRONMENT_PART_COUNT; ++i) {
    if (preset->parts & (1u << i)) {
      vkr_bakery_json_append(
          saved, vkr_bakery_json_cstr(arena, vkr_editor_environment_key(
                                                 (VkrEditorEnvironmentPart)i)));
    }
  }
  ops_set(ctx, ctx->call->result, "path", vkr_bakery_json_cstr(arena, path));
  ops_set(ctx, ctx->call->result, "parts", saved);
  return VKR_EDITOR_OP_DONE;
}

/* env.preset.apply: copies a preset into a container as edits of this
   batch. */
static bool8_t ops_build_env_preset_apply(OpsContext *ctx,
                                          const VkrBakeryJson *args,
                                          OpsBatch *batch) {
  char path[VKR_EDITOR_MATERIAL_PATH];
  uint16_t container = 0u;
  VkrAllocator allocator;
  String8 text = {0};
  if (!ops_environment_path(ctx, args, path) ||
      !ops_arg_container(ctx, args, &container) ||
      !ops_allocator(ctx, &allocator)) {
    return false_v;
  }
  if (!vkr_editor_material_read(&allocator, path, &text)) {
    return ops_fail(ctx, OPS_NOT_FOUND, "'%s' does not open", path);
  }
  Arena *arena = ops_arena(ctx);
  VkrEditorEnvironmentPreset *preset =
      arena_alloc(arena, sizeof(*preset), ARENA_MEMORY_TAG_STRUCT);
  VkrSampleEditBatchItem *items =
      arena_alloc(arena, sizeof(*items) * VKR_EDITOR_ENVIRONMENT_PART_COUNT,
                  ARENA_MEMORY_TAG_STRUCT);
  char error[192] = {0};
  if (!preset || !items) {
    return ops_fail(ctx, OPS_LIMIT, "Out of request memory");
  }
  if (!vkr_editor_environment_read(text, arena, preset, error, sizeof(error))) {
    return ops_fail(ctx, OPS_INVALID, "'%s': %s", path, error);
  }
  uint32_t skipped = 0u;
  const uint32_t count = vkr_editor_environment_apply_items(
      ctx->frame, container, preset, items, VKR_EDITOR_ENVIRONMENT_PART_COUNT,
      &skipped);
  if (count == 0u) {
    return ops_fail(ctx, OPS_INVALID,
                    "Nothing of '%s' applies to this container", path);
  }
  for (uint32_t i = 0; i < count; ++i) {
    VkrSampleEditBatchItem *item =
        ops_batch_add(ctx, batch, container, items[i].request.action);
    if (!item) {
      return false_v;
    }
    const VkrEntityRef ref = item->request.values.ref;
    item->request = items[i].request;
    if (item->request.action == VKR_SCENE_EDIT_CREATE) {
      item->request.values.ref = ref;
    }
    if (!ops_validate_values(ctx, &item->request.values)) {
      return false_v;
    }
  }
  batch->op_item[batch->op_count] = batch->count - 1u;
  return true_v;
}

// =============================================================================
// Lighting (ADR-100)
// =============================================================================

/* lighting.time: runs the live clock from 'hour', as Cmd time.hour does,
   until the simulation resets. */
static VkrEditorOpStatus ops_run_lighting_time(OpsContext *ctx) {
  float64_t hour = 0.0;
  if (!ops_arg_number(ctx->call->args, "hour", &hour) || !isfinite(hour)) {
    ops_fail(ctx, OPS_INVALID, "'hour' is a number of hours");
    return VKR_EDITOR_OP_DONE;
  }
  if (!ctx->frame->time_of_day_request) {
    ops_fail(ctx, OPS_INVALID, "The time of day is not available");
    return VKR_EDITOR_OP_DONE;
  }
  ctx->frame->time_of_day_request->set_hour = true_v;
  ctx->frame->time_of_day_request->hour = hour;
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "hour",
          vkr_bakery_json_float(ops_arena(ctx),
                                fmod(fmod(hour, 24.0) + 24.0, 24.0)));
  return VKR_EDITOR_OP_DONE;
}

/* lighting.list: the primary scene's and the World's lights with their
   values, and the scene's light groups with their live intensities. */
static VkrEditorOpStatus ops_run_lighting_list(OpsContext *ctx) {
  const VkrSampleUiFrame *frame = ctx->frame;
  VkrAllocator allocator;
  if (!frame->scene) {
    ops_fail(ctx, OPS_NOT_FOUND, "No scene is open");
    return VKR_EDITOR_OP_DONE;
  }
  if (!ops_allocator(ctx, &allocator)) {
    return VKR_EDITOR_OP_DONE;
  }
  static const char *const kinds[] = {"directional", "point", "spot",
                                      "rectangle"};
  Arena *arena = ops_arena(ctx);
  VkrEditorLight *lights = NULL;
  const uint32_t count = vkr_editor_lighting_list(frame, &allocator, &lights);
  VkrBakeryJson *list = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < count; ++i) {
    const VkrTypeDesc *type = vkr_editor_light_type(lights[i].kind);
    const void *value = vkr_editor_light_value(&lights[i]);
    if (!value) {
      continue;
    }
    VkrBakeryJson *entry = ops_entity(ctx, lights[i].scene, lights[i].entity);
    ops_set(ctx, entry, "kind",
            vkr_bakery_json_cstr(arena, kinds[lights[i].kind]));
    ops_set(ctx, entry, "from",
            vkr_bakery_json_cstr(
                arena, lights[i].scene == frame->world ? "world" : "scene"));
    ops_set(ctx, entry, "values", ops_component_json(ctx, type, value));
    vkr_bakery_json_append(list, entry);
  }
  VkrBakeryJson *groups = vkr_bakery_json_array(arena);
  const VkrSceneLightGroups *registry = &frame->scene->light_groups;
  for (uint32_t i = 0; i < registry->count; ++i) {
    VkrBakeryJson *group = vkr_bakery_json_object(arena);
    ops_set(ctx, group, "name",
            vkr_bakery_json_cstr(arena, registry->names[i]));
    ops_set(ctx, group, "intensity",
            vkr_bakery_json_float(arena, registry->intensities[i]));
    ops_set(ctx, group, "factor",
            vkr_bakery_json_float(arena, registry->factors[i]));
    vkr_bakery_json_append(groups, group);
  }
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "lights", list);
  ops_set(ctx, ctx->call->result, "groups", groups);
  return VKR_EDITOR_OP_DONE;
}

/* lighting.group: a light group's live intensity in every loaded
   container, as scripts set it, until the simulation resets. */
static VkrEditorOpStatus ops_run_lighting_group(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  char group[VKR_LIGHT_GROUP_NAME_BYTES] = {0};
  float64_t intensity = 0.0;
  if (!ops_arg_string(ctx, args, "group", group, sizeof(group))) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!group[0] || !vkr_light_group_name_valid(group, strlen(group))) {
    ops_fail(ctx, OPS_INVALID, "'group' is a light group name");
    return VKR_EDITOR_OP_DONE;
  }
  if (!ops_arg_number(args, "intensity", &intensity) || !isfinite(intensity) ||
      intensity < 0.0) {
    ops_fail(ctx, OPS_INVALID, "'intensity' is a number of at least 0");
    return VKR_EDITOR_OP_DONE;
  }
  if (!ctx->frame->time_of_day_request) {
    ops_fail(ctx, OPS_INVALID, "Light groups are not available");
    return VKR_EDITOR_OP_DONE;
  }
  VkrSampleTimeOfDayRequest *request = ctx->frame->time_of_day_request;
  request->set_group = true_v;
  snprintf(request->group, sizeof(request->group), "%s", group);
  request->intensity = (float32_t)intensity;
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "group",
          vkr_bakery_json_cstr(ops_arena(ctx), group));
  ops_set(ctx, ctx->call->result, "intensity",
          vkr_bakery_json_float(ops_arena(ctx), intensity));
  return VKR_EDITOR_OP_DONE;
}

/* lighting.bake: the project scene's Bake lighting job with the given
   settings, which also become the Bake settings window's. */
static VkrEditorOpStatus ops_run_lighting_bake(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  VkrEditorBakeSettings *settings =
      vkr_editor_projects_bake_settings(ctx->editor->projects);
  if (!settings) {
    ops_fail(ctx, OPS_INVALID, "Baking needs a project");
    return VKR_EDITOR_OP_DONE;
  }
  VkrEditorBakeSettings next = *settings;
  if (!ops_component_read(ctx, &vkr_editor_lightmap_settings_type,
                          vkr_bakery_json_get(args, "lightmap_settings"),
                          &next.lightmap) ||
      !ops_component_read(ctx, &vkr_editor_diffuse_settings_type,
                          vkr_bakery_json_get(args, "diffuse_settings"),
                          &next.diffuse)) {
    return VKR_EDITOR_OP_DONE;
  }
  *settings = next;
  if (!vkr_editor_projects_bake_lighting(
          ctx->editor->projects, ctx->editor, ctx->frame,
          ops_arg_bool(args, "lightmap",
                       vkr_editor_bakery_lightmap(ctx->editor->bakery)),
          0u)) {
    ops_fail(ctx, OPS_REJECTED, "%s",
             vkr_editor_projects_message(ctx->editor->projects));
    return VKR_EDITOR_OP_DONE;
  }
  ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  ops_set(ctx, ctx->call->result, "started",
          vkr_bakery_json_bool(ops_arena(ctx), true_v));
  ops_set(ctx, ctx->call->result, "lightmap_settings",
          ops_component_json(ctx, &vkr_editor_lightmap_settings_type,
                             &settings->lightmap));
  ops_set(ctx, ctx->call->result, "diffuse_settings",
          ops_component_json(ctx, &vkr_editor_diffuse_settings_type,
                             &settings->diffuse));
  return VKR_EDITOR_OP_DONE;
}

/* surface.theme.create: a new theme document. */
static VkrEditorOpStatus ops_run_theme_create(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  char path[VKR_EDITOR_MATERIAL_PATH];
  if (!ops_theme_path(ctx, args, "path", path)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!path[0]) {
    ops_fail(ctx, OPS_INVALID, "'path' names the new theme");
    return VKR_EDITOR_OP_DONE;
  }
  VkrAllocator allocator;
  String8 existing = {0};
  if (!ops_allocator(ctx, &allocator)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (vkr_editor_material_read(&allocator, path, &existing) &&
      !ops_arg_bool(args, "overwrite", false_v)) {
    ops_fail(ctx, OPS_REJECTED, "'%s' exists; pass \"overwrite\": true", path);
    return VKR_EDITOR_OP_DONE;
  }
  VkrSurfaceTheme *theme =
      arena_alloc(ops_arena(ctx), sizeof(*theme), ARENA_MEMORY_TAG_STRUCT);
  if (!theme) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  MemZero(theme, sizeof(*theme));
  char label[96];
  snprintf(label, sizeof(label), "Create theme %.70s", path);
  if (!ops_theme_apply(ctx, vkr_bakery_json_get(args, "materials"), theme) ||
      !ops_theme_write(ctx, path, theme, label)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!ctx->call->result) {
    ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  }
  ops_set(ctx, ctx->call->result, "path",
          vkr_bakery_json_cstr(ops_arena(ctx), path));
  ops_set(ctx, ctx->call->result, "materials", ops_theme_json(ctx, theme));
  return VKR_EDITOR_OP_DONE;
}

/* surface.theme.bind: binds tags to materials in a theme, by default the
   container's own. */
static VkrEditorOpStatus ops_run_theme_bind(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  char path[VKR_EDITOR_MATERIAL_PATH];
  uint16_t container = 0u;
  if (!ops_theme_path(ctx, args, "path", path) ||
      (!path[0] && !ops_arg_container(ctx, args, &container))) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!path[0]) {
    const char *own = "";
    const char *world = "";
    ops_theme_paths(ctx->frame, container, &own, &world);
    if (!own[0]) {
      ops_fail(ctx, OPS_INVALID,
               "The container selects no theme; give 'path' or run "
               "surface.theme.select first");
      return VKR_EDITOR_OP_DONE;
    }
    snprintf(path, sizeof(path), "%s", own);
  }
  VkrSurfaceTheme *theme =
      arena_alloc(ops_arena(ctx), sizeof(*theme), ARENA_MEMORY_TAG_STRUCT);
  if (!theme || !ops_theme_load(ctx, path, theme, false_v)) {
    if (!theme) {
      ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    }
    return VKR_EDITOR_OP_DONE;
  }
  /* `tag` and `material` bind one tag; `materials` binds several. */
  String8 tag = {0};
  const VkrBakeryJson *materials = vkr_bakery_json_get(args, "materials");
  if (vkr_bakery_json_get_string(args, "tag", &tag)) {
    VkrBakeryJson *one = vkr_bakery_json_object(ops_arena(ctx));
    const VkrBakeryJson *material = vkr_bakery_json_get(args, "material");
    if (!material) {
      ops_fail(ctx, OPS_INVALID, "'tag' needs a 'material', empty to unbind");
      return VKR_EDITOR_OP_DONE;
    }
    char key[32];
    snprintf(key, sizeof(key), "%.*s",
             (int)Min(tag.length, (uint64_t)sizeof(key) - 1u),
             (const char *)tag.str);
    ops_set(ctx, one, key, vkr_bakery_json_clone(ops_arena(ctx), material));
    materials = one;
  } else if (!materials) {
    ops_fail(ctx, OPS_INVALID, "Give 'tag' and 'material', or 'materials'");
    return VKR_EDITOR_OP_DONE;
  }
  char label[96];
  snprintf(label, sizeof(label), "Bind tags in %.70s", path);
  if (!ops_theme_apply(ctx, materials, theme) ||
      !ops_theme_write(ctx, path, theme, label)) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!ctx->call->result) {
    ctx->call->result = vkr_bakery_json_object(ops_arena(ctx));
  }
  ops_set(ctx, ctx->call->result, "path",
          vkr_bakery_json_cstr(ops_arena(ctx), path));
  ops_set(ctx, ctx->call->result, "materials", ops_theme_json(ctx, theme));
  return VKR_EDITOR_OP_DONE;
}

/* surface.theme.select: the container's surface_theme names `theme`; an
   entity "Surface theme" carries it when no entity of the container does. */
static bool8_t ops_build_theme_select(OpsContext *ctx,
                                      const VkrBakeryJson *args,
                                      OpsBatch *batch) {
  char theme[VKR_EDITOR_MATERIAL_PATH];
  uint16_t container = 0u;
  if (!vkr_bakery_json_get(args, "theme")) {
    return ops_fail(ctx, OPS_INVALID,
                    "'theme' names a .surfaces file, or is empty to clear");
  }
  if (!ops_theme_path(ctx, args, "theme", theme) ||
      !ops_arg_container(ctx, args, &container)) {
    return false_v;
  }
  const VkrScene *scene = ops_scene(ctx->frame, container);
  const VkrEntityId holder = scene->world_state.surface_theme_entity;
  const bool8_t own =
      holder.u64 && holder.parts.world == scene->world_id &&
      vkr_scene_get_typed(scene, holder, &vkr_scene_surface_theme_type);
  SceneSurfaceTheme value = {0};
  snprintf(value.theme, sizeof(value.theme), "%s", theme);
  VkrSampleEditBatchItem *item =
      ops_batch_add(ctx, batch, container,
                    own ? VKR_SCENE_EDIT_APPLY : VKR_SCENE_EDIT_CREATE);
  if (!item) {
    return false_v;
  }
  VkrSceneEditValues *values = &item->request.values;
  if (own) {
    const OpsRef ref = {.entity = holder, .container = container, .item = -1};
    ops_item_target(item, &ref);
    values->fields = VKR_SCENE_EDIT_COMPONENT;
  } else {
    values->fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM |
                     VKR_SCENE_EDIT_COMPONENT;
    snprintf(values->name, sizeof(values->name), "Surface theme");
    values->rotation = vkr_quat_identity();
    values->scale = vec3_one();
  }
  values->component_type = &vkr_scene_surface_theme_type;
  MemCopy(values->component, &value, sizeof(value));
  if (!own) {
    batch->op_item[batch->op_count] = batch->count - 1u;
  }
  return ops_validate_values(ctx, values);
}

/* One material art.lint checked: whether it opens and parses, its textures,
   its untextured base colour and metallic, and the width in pixels of the
   texture that sets texel density (base colour, else any). */
typedef struct OpsArtMaterial {
  char path[VKR_EDITOR_MATERIAL_PATH];
  bool8_t opens;
  bool8_t parses;
  char missing_texture[VKR_MATERIAL_PATH_MAX];
  bool8_t uncooked;
  uint32_t texture_width;
  Vec2 world_size;
  bool8_t textured_base;
  bool8_t textured_metallic;
  Vec4 base_color;
  float32_t metallic;
} OpsArtMaterial;

/* The pixel width a texture's cooked `.vkt` (KTX2) or PNG source records;
   zero when neither header reads. */
static uint32_t ops_art_texture_width(VkrAllocator *allocator,
                                      const char *path) {
  char cooked[VKR_MATERIAL_PATH_MAX + 8u];
  snprintf(cooked, sizeof(cooked), "%s.vkt", path);
  const char *candidates[2] = {cooked, path};
  for (uint32_t i = 0; i < 2u; ++i) {
    FilePath file_path = vkr_asset_path_file(
        allocator, string8_create_from_cstr((const uint8_t *)candidates[i],
                                            strlen(candidates[i])));
    FileMode mode = bitset8_create();
    bitset8_set(&mode, FILE_MODE_READ);
    bitset8_set(&mode, FILE_MODE_BINARY);
    FileHandle file = {0};
    if (!file_path.path.length ||
        file_open(&file_path, mode, &file) != FILE_ERROR_NONE) {
      continue;
    }
    uint8_t header[32] = {0};
    uint64_t read = 0u;
    const bool8_t ok = file_read_into(&file, header, sizeof(header), &read) ==
                           FILE_ERROR_NONE &&
                       read == sizeof(header);
    file_close(&file);
    if (!ok) {
      continue;
    }
    /* KTX2: identifier, vkFormat, typeSize, then pixelWidth (LE). */
    if (i == 0u && header[0] == 0xABu && header[1] == 'K') {
      return (uint32_t)header[20] | (uint32_t)header[21] << 8u |
             (uint32_t)header[22] << 16u | (uint32_t)header[23] << 24u;
    }
    /* PNG: signature, then the IHDR chunk's width (BE). */
    if (i == 1u && header[0] == 0x89u && header[1] == 'P') {
      return (uint32_t)header[16] << 24u | (uint32_t)header[17] << 16u |
             (uint32_t)header[18] << 8u | (uint32_t)header[19];
    }
  }
  return 0u;
}

static bool8_t ops_art_file_exists(VkrAllocator *allocator, const char *path) {
  FilePath file_path = vkr_asset_path_file(
      allocator, string8_create_from_cstr((const uint8_t *)path, strlen(path)));
  return file_path.path.length && file_exists(&file_path);
}

static void ops_art_material_check(OpsContext *ctx, OpsArtMaterial *out) {
  VkrAllocator allocator;
  String8 text = {0};
  out->world_size = vec2_new(1.0f, 1.0f);
  if (!ops_allocator(ctx, &allocator) ||
      !vkr_editor_material_read(&allocator, out->path, &text)) {
    return;
  }
  out->opens = true_v;
  VkrParsedMaterialData *parsed =
      arena_alloc(ops_arena(ctx), sizeof(*parsed), ARENA_MEMORY_TAG_STRUCT);
  if (!parsed) {
    return;
  }
  MemZero(parsed, sizeof(*parsed));
  out->parses = vkr_material_loader_parse_definition(
      &allocator,
      string8_create_from_cstr((const uint8_t *)out->path, strlen(out->path)),
      text, parsed);
  if (!out->parses) {
    return;
  }
  if (parsed->world_size.x > 0.0f && parsed->world_size.y > 0.0f) {
    out->world_size = parsed->world_size;
  }
  out->base_color = parsed->pbr.base_color;
  out->metallic = parsed->pbr.metallic;
  out->textured_base =
      parsed->texture_paths[VKR_TEXTURE_SLOT_DIFFUSE][0] != '\0';
  out->textured_metallic =
      parsed->texture_paths[VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS][0] != '\0';
  for (uint32_t slot = 0; slot < VKR_TEXTURE_SLOT_COUNT; ++slot) {
    char texture[VKR_MATERIAL_PATH_MAX];
    snprintf(texture, sizeof(texture), "%s", parsed->texture_paths[slot]);
    char *query = strchr(texture, '?');
    if (query) {
      *query = '\0';
    }
    if (!texture[0]) {
      continue;
    }
    if (!ops_art_file_exists(&allocator, texture)) {
      if (!out->missing_texture[0]) {
        snprintf(out->missing_texture, sizeof(out->missing_texture), "%s",
                 texture);
      }
      continue;
    }
    char cooked[VKR_MATERIAL_PATH_MAX + 8u];
    snprintf(cooked, sizeof(cooked), "%s.vkt", texture);
    if (!ops_material_ends(texture, ".vkt") &&
        !ops_art_file_exists(&allocator, cooked)) {
      out->uncooked = true_v;
    }
    if (!out->texture_width || slot == VKR_TEXTURE_SLOT_DIFFUSE) {
      const uint32_t width = ops_art_texture_width(&allocator, texture);
      out->texture_width = width ? width : out->texture_width;
    }
  }
}

/* art.lint's issues, up to `limit`, counting past it. */
typedef struct OpsArtLint {
  OpsContext *ctx;
  const VkrScene *scene;
  VkrBakeryJson *issues;
  uint32_t found;
  uint32_t limit;
  uint32_t kinds;
} OpsArtLint;

typedef enum OpsArtIssue {
  OPS_ART_UNBOUND_TAG = 0,
  OPS_ART_MISSING_MATERIAL,
  OPS_ART_MISSING_TEXTURE,
  OPS_ART_UNCOOKED_TEXTURE,
  OPS_ART_TEXEL_DENSITY,
  OPS_ART_BASE_COLOR,
  OPS_ART_METALLIC,
  OPS_ART_ISSUE_COUNT,
} OpsArtIssue;

static const char *const s_art_issue_names[OPS_ART_ISSUE_COUNT] = {
    "unbound_tag",   "missing_material", "missing_texture", "uncooked_texture",
    "texel_density", "base_color",       "metallic"};

static VkrBakeryJson *ops_art_issue(OpsArtLint *lint, OpsArtIssue kind) {
  if (!(lint->kinds & (1u << kind))) {
    return NULL;
  }
  if (lint->found++ >= lint->limit) {
    return NULL;
  }
  VkrBakeryJson *issue = vkr_bakery_json_object(ops_arena(lint->ctx));
  ops_set(lint->ctx, issue, "kind",
          vkr_bakery_json_cstr(ops_arena(lint->ctx), s_art_issue_names[kind]));
  vkr_bakery_json_append(lint->issues, issue);
  return issue;
}

/* The issues of one material, reported once, naming who uses it. */
static void ops_art_material_issues(OpsArtLint *lint,
                                    const OpsArtMaterial *material,
                                    const char *used_by, VkrEntityId entity) {
  OpsContext *ctx = lint->ctx;
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *issue = NULL;
  if (!material->opens || !material->parses) {
    if ((issue = ops_art_issue(lint, OPS_ART_MISSING_MATERIAL))) {
      ops_set(ctx, issue, "material",
              vkr_bakery_json_cstr(arena, material->path));
      ops_set(ctx, issue, "problem",
              vkr_bakery_json_cstr(arena, material->opens ? "does not parse"
                                                          : "does not open"));
      ops_set(ctx, issue, "used_by", vkr_bakery_json_cstr(arena, used_by));
      if (entity.u64) {
        ops_set(ctx, issue, "entity", ops_entity(ctx, lint->scene, entity));
      }
    }
    return;
  }
  if (material->missing_texture[0] &&
      (issue = ops_art_issue(lint, OPS_ART_MISSING_TEXTURE))) {
    ops_set(ctx, issue, "material",
            vkr_bakery_json_cstr(arena, material->path));
    ops_set(ctx, issue, "texture",
            vkr_bakery_json_cstr(arena, material->missing_texture));
  }
  if (material->uncooked &&
      (issue = ops_art_issue(lint, OPS_ART_UNCOOKED_TEXTURE))) {
    ops_set(ctx, issue, "material",
            vkr_bakery_json_cstr(arena, material->path));
    ops_set(ctx, issue, "problem",
            vkr_bakery_json_cstr(arena, "a texture has no cooked .vkt; build "
                                        "assets with vkr_bakery"));
  }
  /* Plausible PBR albedo: a non-metal's untextured base colour between sRGB
     30 and 240 in its brightest channel; metallic near 0 or 1. */
  if (!material->textured_base && material->metallic < 0.5f) {
    const Vec4 c = material->base_color;
    const float32_t linear = Max(c.x, Max(c.y, c.z));
    const float32_t srgb =
        255.0f * (linear <= 0.0031308f
                      ? 12.92f * linear
                      : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f);
    if ((srgb < 30.0f || srgb > 240.0f) &&
        (issue = ops_art_issue(lint, OPS_ART_BASE_COLOR))) {
      ops_set(ctx, issue, "material",
              vkr_bakery_json_cstr(arena, material->path));
      ops_set(ctx, issue, "value", ops_number(ctx, srgb));
    }
  }
  if (!material->textured_metallic && material->metallic > 0.1f &&
      material->metallic < 0.9f &&
      (issue = ops_art_issue(lint, OPS_ART_METALLIC))) {
    ops_set(ctx, issue, "material",
            vkr_bakery_json_cstr(arena, material->path));
    ops_set(ctx, issue, "value", ops_number(ctx, material->metallic));
  }
}

/* art.lint: the art pass's problems in one container. */
static VkrEditorOpStatus ops_run_art_lint(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  uint16_t container = 0u;
  if (!ops_arg_container(ctx, args, &container)) {
    return VKR_EDITOR_OP_DONE;
  }
  float64_t limit = 100.0;
  float64_t texel_min = 128.0;
  float64_t texel_max = 2048.0;
  (void)ops_arg_number(args, "limit", &limit);
  (void)ops_arg_number(args, "texel_min", &texel_min);
  (void)ops_arg_number(args, "texel_max", &texel_max);
  if (limit < 1.0 || limit > 500.0 || texel_min <= 0.0 ||
      texel_max < texel_min) {
    ops_fail(ctx, OPS_INVALID,
             "'limit' is 1 to 500; 0 < 'texel_min' <= 'texel_max' (px/m)");
    return VKR_EDITOR_OP_DONE;
  }
  uint32_t kinds = (1u << OPS_ART_ISSUE_COUNT) - 1u;
  const VkrBakeryJson *kind_list = vkr_bakery_json_get(args, "kinds");
  if (kind_list) {
    kinds = 0u;
    for (uint32_t i = 0;
         kind_list->type == VKR_BAKERY_JSON_ARRAY && i < kind_list->count;
         ++i) {
      const VkrBakeryJson *name = vkr_bakery_json_at(kind_list, i);
      for (uint32_t k = 0; k < OPS_ART_ISSUE_COUNT; ++k) {
        if (name->type == VKR_BAKERY_JSON_STRING &&
            ops_equals(name->string, s_art_issue_names[k])) {
          kinds |= 1u << k;
        }
      }
    }
    if (!kinds) {
      ops_fail(ctx, OPS_INVALID,
               "'kinds' lists unbound_tag, missing_material, "
               "missing_texture, uncooked_texture, "
               "texel_density, base_color or metallic");
      return VKR_EDITOR_OP_DONE;
    }
  }

  const VkrScene *scene = ops_scene(ctx->frame, container);
  const char *own_path = "";
  const char *world_path = "";
  ops_theme_paths(ctx->frame, container, &own_path, &world_path);
  VkrSurfaceTheme *themes = arena_alloc(ops_arena(ctx), 2u * sizeof(*themes),
                                        ARENA_MEMORY_TAG_STRUCT);
  enum { ART_MATERIALS = 256 };
  OpsArtMaterial *materials =
      arena_alloc(ops_arena(ctx), ART_MATERIALS * sizeof(*materials),
                  ARENA_MEMORY_TAG_ARRAY);
  if (!themes || !materials) {
    ops_fail(ctx, OPS_LIMIT, "Out of request memory");
    return VKR_EDITOR_OP_DONE;
  }
  MemZero(themes, 2u * sizeof(*themes));
  if ((own_path[0] && !ops_theme_load(ctx, own_path, &themes[0], true_v)) ||
      (world_path[0] && !ops_theme_load(ctx, world_path, &themes[1], true_v))) {
    return VKR_EDITOR_OP_DONE;
  }
  OpsArtLint lint = {.ctx = ctx,
                     .scene = scene,
                     .issues = vkr_bakery_json_array(ops_arena(ctx)),
                     .limit = (uint32_t)limit,
                     .kinds = kinds};
  uint32_t material_count = 0u;
  uint32_t unbound[VKR_SURFACE_COUNT] = {0};
  VkrEntityId unbound_face[VKR_SURFACE_COUNT] = {0};
  uint32_t faces = 0u;

  /* Faces of solid and visual brushes, with the material each shows. */
  const VkrWorld *world = scene->world;
  for (uint32_t i = 0; i < world->dir.capacity; ++i) {
    if (!world->dir.records[i].chunk) {
      continue;
    }
    const VkrEntityId entity = vkr_entity_id_from_index(world, i);
    const SceneBrushFace *face =
        vkr_scene_get_typed(scene, entity, &vkr_scene_brush_face_type);
    const SceneMaterialOverride *override_paths =
        vkr_scene_get_typed(scene, entity, &vkr_scene_material_override_type);
    const char *paths[VKR_MESH_MATERIAL_OVERRIDE_MAX] = {0};
    uint32_t path_count = 0u;
    Vec2 uv_scale = vec2_new(1.0f, 1.0f);
    if (face) {
      const SceneBrushSettings *brush = vkr_scene_get_typed(
          scene, ops_parent(scene, entity), &vkr_scene_brush_type);
      if (!brush || brush->role == SCENE_BRUSH_ROLE_CLIP ||
          brush->role == SCENE_BRUSH_ROLE_TRIGGER) {
        continue;
      }
      faces++;
      const char *look =
          face->material[0] ? face->material
                            : vkr_surface_theme_material(&themes[0], &themes[1],
                                                         face->surface);
      if (!look) {
        if (face->surface != VKR_SURFACE_NONE &&
            (uint32_t)face->surface < VKR_SURFACE_COUNT &&
            !unbound[face->surface]++) {
          unbound_face[face->surface] = entity;
        }
        continue;
      }
      paths[path_count++] = look;
      uv_scale = face->uv_scale;
    } else if (override_paths) {
      for (uint32_t slot = 0; slot < VKR_MESH_MATERIAL_OVERRIDE_MAX; ++slot) {
        if (override_paths->materials[slot][0]) {
          paths[path_count++] = override_paths->materials[slot];
        }
      }
    }
    for (uint32_t p = 0; p < path_count; ++p) {
      uint32_t at = 0u;
      while (at < material_count && strcmp(materials[at].path, paths[p]) != 0) {
        at++;
      }
      if (at == material_count) {
        if (material_count == ART_MATERIALS) {
          continue;
        }
        MemZero(&materials[at], sizeof(materials[at]));
        snprintf(materials[at].path, sizeof(materials[at].path), "%s",
                 paths[p]);
        ops_art_material_check(ctx, &materials[at]);
        material_count++;
        ops_art_material_issues(&lint, &materials[at],
                                face ? (face->material[0] ? "face" : "theme")
                                     : "material_override",
                                entity);
      }
      /* Texel density: pixels per meter along the texture's width. */
      const OpsArtMaterial *material = &materials[at];
      VkrBakeryJson *issue = NULL;
      if (face && material->texture_width) {
        const float32_t meters = fabsf(uv_scale.x) * material->world_size.x;
        const float64_t density =
            meters > 0.0f ? (float64_t)material->texture_width / meters : 0.0;
        if ((density < texel_min || density > texel_max) &&
            (issue = ops_art_issue(&lint, OPS_ART_TEXEL_DENSITY))) {
          ops_set(ctx, issue, "entity", ops_entity(ctx, scene, entity));
          ops_set(ctx, issue, "material",
                  vkr_bakery_json_cstr(ops_arena(ctx), material->path));
          ops_set(ctx, issue, "value", ops_number(ctx, density));
        }
      }
    }
  }
  for (uint32_t tag = 1; tag < VKR_SURFACE_COUNT; ++tag) {
    VkrBakeryJson *issue = NULL;
    if (unbound[tag] && (issue = ops_art_issue(&lint, OPS_ART_UNBOUND_TAG))) {
      ops_set(ctx, issue, "tag",
              vkr_bakery_json_cstr(ops_arena(ctx), vkr_surface_names[tag]));
      ops_set(ctx, issue, "value",
              vkr_bakery_json_int(ops_arena(ctx), unbound[tag]));
      ops_set(ctx, issue, "entity", ops_entity(ctx, scene, unbound_face[tag]));
    }
  }
  /* Theme bindings that name a material no face uses yet still must
     load. */
  for (uint32_t t = 0; t < 2u; ++t) {
    for (uint32_t tag = 1; tag < VKR_SURFACE_COUNT; ++tag) {
      const char *path = themes[t].materials[tag];
      uint32_t at = 0u;
      while (path[0] && at < material_count &&
             strcmp(materials[at].path, path) != 0) {
        at++;
      }
      if (!path[0] || at < material_count || material_count == ART_MATERIALS) {
        continue;
      }
      MemZero(&materials[at], sizeof(materials[at]));
      snprintf(materials[at].path, sizeof(materials[at].path), "%s", path);
      ops_art_material_check(ctx, &materials[at]);
      material_count++;
      ops_art_material_issues(&lint, &materials[at], "theme",
                              VKR_ENTITY_ID_INVALID);
    }
  }

  Arena *arena = ops_arena(ctx);
  ctx->call->result = vkr_bakery_json_object(arena);
  ops_set(ctx, ctx->call->result, "container",
          vkr_bakery_json_int(arena, container));
  ops_set(ctx, ctx->call->result, "theme",
          vkr_bakery_json_cstr(arena, own_path));
  ops_set(ctx, ctx->call->result, "world_theme",
          vkr_bakery_json_cstr(arena, world_path));
  ops_set(ctx, ctx->call->result, "faces", vkr_bakery_json_int(arena, faces));
  ops_set(ctx, ctx->call->result, "materials",
          vkr_bakery_json_int(arena, material_count));
  ops_set(ctx, ctx->call->result, "found",
          vkr_bakery_json_int(arena, lint.found));
  ops_set(ctx, ctx->call->result, "issues", lint.issues);
  return VKR_EDITOR_OP_DONE;
}

// =============================================================================
// Collaborative session (editor_session.h)
// =============================================================================

static VkrBakeryJson *ops_session_status(OpsContext *ctx) {
  static const char *const modes[] = {"none", "host", "participant"};
  VkrEditorSessionStatus status;
  vkr_editor_session_status(ctx->editor->session, &status);
  Arena *arena = ops_arena(ctx);
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  ops_set(ctx, result, "mode",
          vkr_bakery_json_cstr(arena, modes[Min(status.mode, 2u)]));
  ops_set(ctx, result, "joined", vkr_bakery_json_bool(arena, status.joined));
  ops_set(ctx, result, "self", vkr_bakery_json_int(arena, status.self_id));
  ops_set(ctx, result, "sequence",
          vkr_bakery_json_int(arena, (int64_t)status.sequence));
  ops_set(ctx, result, "pending", vkr_bakery_json_int(arena, status.pending));
  ops_set(ctx, result, "address", vkr_bakery_json_cstr(arena, status.address));
  ops_set(ctx, result, "key", vkr_bakery_json_cstr(arena, status.key));
  ops_set(ctx, result, "error", vkr_bakery_json_cstr(arena, status.error));
  /* What a joining editor's scenes must match, from this editor's scenes:
     equal digests mean equal names, poses and parents. */
  uint8_t digest[32];
  char digest_hex[65];
  vkr_editor_session_digest(ctx->frame, digest);
  for (uint32_t i = 0; i < 32u; ++i) {
    snprintf(digest_hex + 2u * i, 3u, "%02x", digest[i]);
  }
  ops_set(ctx, result, "digest", vkr_bakery_json_cstr(arena, digest_hex));
  VkrBakeryJson *peers = vkr_bakery_json_array(arena);
  for (uint32_t i = 0; i < status.peer_count; ++i) {
    const VkrEditorSessionPeer *peer = &status.peers[i];
    VkrBakeryJson *row = vkr_bakery_json_object(arena);
    ops_set(ctx, row, "id", vkr_bakery_json_int(arena, peer->id));
    ops_set(ctx, row, "name", vkr_bakery_json_cstr(arena, peer->name));
    ops_set(ctx, row, "tool", vkr_bakery_json_cstr(arena, peer->tool));
    ops_set(ctx, row, "dragging", vkr_bakery_json_bool(arena, peer->dragging));
    if (peer->has_camera) {
      VkrBakeryJson *camera = vkr_bakery_json_object(arena);
      ops_set(ctx, camera, "position", ops_vec3(ctx, peer->camera_position));
      ops_set(ctx, camera, "yaw", ops_number(ctx, peer->camera_yaw));
      ops_set(ctx, camera, "pitch", ops_number(ctx, peer->camera_pitch));
      ops_set(ctx, row, "camera", camera);
    } else {
      ops_set(ctx, row, "camera", vkr_bakery_json_null(arena));
    }
    if (peer->has_selection) {
      char ref[37];
      vkr_entity_ref_format(&peer->selection, ref);
      VkrBakeryJson *selection = vkr_bakery_json_object(arena);
      ops_set(ctx, selection, "container",
              vkr_bakery_json_int(arena, peer->selection_container));
      ops_set(ctx, selection, "ref", vkr_bakery_json_cstr(arena, ref));
      ops_set(ctx, row, "selection", selection);
    } else {
      ops_set(ctx, row, "selection", vkr_bakery_json_null(arena));
    }
    vkr_bakery_json_append(peers, row);
  }
  ops_set(ctx, result, "peers", peers);
  return result;
}

static VkrEditorOpStatus ops_run_session_status(OpsContext *ctx) {
  ctx->call->result = ops_session_status(ctx);
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_session_host(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  char bind[64] = {0};
  char name[VKR_EDITOR_SESSION_NAME_MAX] = {0};
  char error[192] = {0};
  if (!ops_arg_string(ctx, args, "bind", bind, sizeof(bind)) ||
      !ops_arg_string(ctx, args, "name", name, sizeof(name))) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!vkr_editor_session_host(ctx->editor->session, ctx->frame, bind, name,
                               error, sizeof(error))) {
    ops_fail(ctx, OPS_BUSY, "%s", error);
    return VKR_EDITOR_OP_DONE;
  }
  ctx->call->result = ops_session_status(ctx);
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_session_join(OpsContext *ctx) {
  const VkrBakeryJson *args = ctx->call->args;
  char address[256] = {0};
  char key[80] = {0};
  char name[VKR_EDITOR_SESSION_NAME_MAX] = {0};
  char error[192] = {0};
  if (!ops_arg_string(ctx, args, "address", address, sizeof(address)) ||
      !ops_arg_string(ctx, args, "key", key, sizeof(key)) ||
      !ops_arg_string(ctx, args, "name", name, sizeof(name))) {
    return VKR_EDITOR_OP_DONE;
  }
  if (!vkr_editor_session_join(ctx->editor->session, ctx->frame, address, key,
                               name, error, sizeof(error))) {
    ops_fail(ctx, OPS_INVALID, "%s", error);
    return VKR_EDITOR_OP_DONE;
  }
  ctx->call->result = ops_session_status(ctx);
  return VKR_EDITOR_OP_DONE;
}

static VkrEditorOpStatus ops_run_session_leave(OpsContext *ctx) {
  vkr_editor_session_leave(ctx->editor->session);
  ctx->call->result = ops_session_status(ctx);
  return VKR_EDITOR_OP_DONE;
}

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
     "Entities of one scene with id, name, parent, components, tags, local "
     "pose and world bounds; 'tags' keeps those carrying every tag named. "
     "Page with offset and limit (at most 500).",
     "{\"type\":\"object\",\"properties\":{" OPS_TAGS_REGION_SCHEMA ","
     "\"offset\":{\"type\":\"integer\",\"minimum\":0},"
     "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500},"
     "\"faces\":{\"type\":\"boolean\",\"description\":\"Also list brush "
     "faces and IO connections, which their owners otherwise "
     "summarise\"}," OPS_SETTLE_OFF_SCHEMA "}}",
     ops_run_describe, NULL, OPS_QUICK},
    {"entity.get",
     "One entity: pose, children, bounds, tags and every component's values; "
     "a spline mesh or scatter adds its placed 'copies' and 'status'.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     "," OPS_SETTLE_OFF_SCHEMA "},\"required\":[\"entity\"]}",
     ops_run_get, NULL, OPS_QUICK},
    {"tag.list",
     "Each tag the entities of one scene carry, with how many carry it, most "
     "used first; root, region and tags narrow the entities as in "
     "scene.describe.",
     "{\"type\":\"object\",\"properties\":{" OPS_TAGS_REGION_SCHEMA "}}",
     ops_run_tag_list, NULL, OPS_QUICK},
    {"entity.create",
     "Create an entity with a name, pose (rotation in degrees XYZ), "
     "optionally one component, and tags.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
     "\"parent\":" OPS_ENTITY_SCHEMA ",\"container\":" OPS_CONTAINER_SCHEMA
     ",\"position\":" OPS_VEC3_SCHEMA ",\"rotation\":" OPS_VEC3_SCHEMA
     ",\"scale\":" OPS_VEC3_SCHEMA
     ",\"component\":{\"type\":\"object\",\"properties\":{\"type\":{\"type\":"
     "\"string\"},\"values\":" OPS_VALUES_SCHEMA
     "},\"required\":[\"type\"]},\"tags\":" OPS_TAGS_SCHEMA
     "," OPS_REVIEW_SCHEMA "}}",
     NULL, ops_build_create},
    {"entity.set",
     "Change an entity's name, pose, visibility or tags: 'tags' replaces its "
     "tags (empty removes them), then 'tags_add' and 'tags_remove' apply.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"name\":{\"type\":\"string\"},\"position\":" OPS_VEC3_SCHEMA
     ",\"rotation\":" OPS_VEC3_SCHEMA ",\"scale\":" OPS_VEC3_SCHEMA
     ",\"visible\":{\"type\":\"boolean\"},\"tags\":" OPS_TAGS_SCHEMA
     ",\"tags_add\":" OPS_TAGS_SCHEMA ",\"tags_remove\":" OPS_TAGS_SCHEMA
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"entity\"]}",
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
    {"look.volume",
     "Create a look volume between two world corners, turned by 'rotation' "
     "(degrees XYZ) about its center: inside it, fading over "
     "'blend_distance' metres (default 1) outside, each value 'look' names "
     "replaces the scene's: exposure_compensation_ev, metering [min, max] "
     "EV, white_balance [temperature, tint], contrast, saturation, "
     "bloom_intensity, fog_color, fog_density, sky_light_intensity. "
     "Overlapping volumes blend in ascending 'priority'.",
     "{\"type\":\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA ",\"rotation\":" OPS_VEC3_SCHEMA
     ",\"name\":{\"type\":\"string\"},\"container\":" OPS_CONTAINER_SCHEMA
     ",\"priority\":{\"type\":\"integer\"},\"blend_distance\":{\"type\":"
     "\"number\"},\"look\":{\"type\":\"object\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"min\",\"max\",\"look\"]}",
     NULL, ops_build_look_volume},
    {"probe.create",
     "Add a reflection probe to the open project scene: a world 'center', "
     "half 'extents' (default [4, 2.5, 4]) and 'values' (blend_distance, "
     "intensity, diffuse_intensity, specular_intensity). It shows its box "
     "at once and reflects after the scene is saved and Bake lighting "
     "captures it; a scene holds at most 16. Move or delete it as an "
     "entity; component.set edits its values.",
     "{\"type\":\"object\",\"properties\":{\"center\":" OPS_VEC3_SCHEMA
     ",\"extents\":" OPS_VEC3_SCHEMA ",\"values\":{\"type\":\"object\"},"
     "\"name\":{\"type\":\"string\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"center\"]}",
     NULL, ops_build_probe_create},
    {"decal.place",
     "Place a decal facing a surface: centred on 'position' with the "
     "surface's 'normal', or where a ray from 'origin' along 'direction' "
     "first meets collision. 'size' is its width and height across the "
     "surface (metres, one number for a square, default 1), 'depth' its "
     "reach along the normal (default 0.5). The image's top points up a "
     "wall, or along 'facing' (default the ray, else -Z) on a floor; "
     "'angle' turns it about the normal in degrees. 'material' is a .mt "
     "file (empty: the default decal) and 'values' other decal fields "
     "(opacity, sort_order, fade_angle_start, fade_angle_end, depth_fade).",
     "{\"type\":\"object\",\"properties\":{\"position\":" OPS_VEC3_SCHEMA
     ",\"normal\":" OPS_VEC3_SCHEMA ",\"origin\":" OPS_VEC3_SCHEMA
     ",\"direction\":" OPS_VEC3_SCHEMA ",\"facing\":" OPS_VEC3_SCHEMA
     ",\"size\":{},\"depth\":{\"type\":\"number\"},\"angle\":{\"type\":"
     "\"number\"},\"material\":{\"type\":\"string\"},\"values\":{\"type\":"
     "\"object\"},\"name\":{\"type\":\"string\"},"
     "\"container\":" OPS_CONTAINER_SCHEMA "," OPS_REVIEW_SCHEMA "}}",
     NULL, ops_build_decal_place},
    {"brush.box",
     "Create a box brush between two corners, snapped to 'grid' (default "
     "1/16 m), turned by 'rotation' (degrees XYZ) about its center; with "
     "'parent' the corners are in the parent's space. Role solid renders "
     "and collides, visual only renders, clip only collides, trigger is a "
     "sensor volume.",
     "{\"type\":\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA ",\"rotation\":" OPS_VEC3_SCHEMA
     "," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA
     "},\"required\":[\"min\",\"max\"]}",
     NULL, ops_build_brush_box},
    {"brush.wedge",
     "Create a wedge brush in a box: its top slopes down to the floor at the "
     "side 'slope' names (+x, -x, +z or -z); 'rotation' (degrees XYZ) turns "
     "it about the box's center.",
     "{\"type\":\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA ",\"rotation\":" OPS_VEC3_SCHEMA
     ",\"slope\":{\"type\":\"string\",\"enum\":"
     "[\"+x\",\"-x\",\"+z\",\"-z\"]}," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA
     "},\"required\":[\"min\",\"max\"]}",
     NULL, ops_build_brush_wedge},
    {"brush.cylinder",
     "Create a vertical prism brush standing on 'center' with corners on "
     "a circle of 'radius'. 'rotation' (degrees XYZ) turns it about 'center': "
     "[0, 0, -90] lays its axis along +X, [90, 0, 0] along +Z.",
     "{\"type\":\"object\",\"properties\":{\"center\":" OPS_VEC3_SCHEMA
     ",\"radius\":{\"type\":\"number\",\"exclusiveMinimum\":0},"
     "\"height\":{\"type\":\"number\",\"exclusiveMinimum\":0},\"sides\":"
     "{\"type\":\"integer\",\"minimum\":3,\"maximum\":32},"
     "\"rotation\":" OPS_VEC3_SCHEMA "," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA
     "},\"required\":[\"center\",\"radius\",\"height\"]}",
     NULL, ops_build_brush_cylinder},
    {"brush.planes",
     "Create a convex brush bounded by 4 to 64 planes in its parent's space "
     "(world space at the root). Each plane has an outward 'normal' and "
     "either 'distance' (inside where normal . p <= distance) or a 'point' "
     "on it, and optionally its face's 'surface' and 'mark'. Every plane "
     "must touch "
     "the solid; a failure names planes[i]. The brush's origin goes to the "
     "center of its box.",
     "{\"type\":\"object\",\"properties\":{\"planes\":{\"type\":"
     "\"array\",\"minItems\":4,\"maxItems\":64,\"items\":{\"type\":"
     "\"object\",\"properties\":{\"normal\":" OPS_VEC3_SCHEMA
     ",\"distance\":{\"type\":\"number\"},\"point\":" OPS_VEC3_SCHEMA
     ",\"surface\":" OPS_SURFACE_SCHEMA ",\"mark\":" OPS_MARK_SCHEMA
     "},\"required\":[\"normal\"]}}"
     "," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA "},\"required\":[\"planes\"]}",
     NULL, ops_build_brush_planes},
    {"brush.hull",
     "Create the convex brush around 4 to 128 points in its parent's space "
     "(world space at the root), snapped to 'grid' (default 1/16 m; 0 keeps "
     "them). Points inside the hull are ignored. The brush's origin goes to "
     "the center of their box.",
     "{\"type\":\"object\",\"properties\":{\"points\":{\"type\":"
     "\"array\",\"minItems\":4,\"maxItems\":128,\"items\":" OPS_VEC3_SCHEMA
     "}," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA "},\"required\":[\"points\"]}",
     NULL, ops_build_brush_hull},
    {"brush.stairs",
     "Create editable stairs (a blockout shape): 'from' is the bottom front "
     "center (a spiral's pole), 'to' the top back center (its height is the "
     "stairs' height unless 'height' is set; for a spiral the rim point "
     "where the climb ends: the top step ends on the radius through it and "
     "a landing 'width' deep follows); 'kind' straight (default), l or u "
     "(two flights and a landing; the length from 'from' to 'to' includes "
     "the landing), curved or spiral ('sweep' degrees; a spiral turns 22.5 "
     "degrees a step by default; 'width' leaves the pole the rest of the "
     "radius); 'turn' left or right; steps at most step_height (default "
     "0.1875 m) high, 4096 at most, solid or 'thickness' thick slabs.",
     "{\"type\":\"object\",\"properties\":{\"from\":" OPS_VEC3_SCHEMA
     ",\"to\":" OPS_VEC3_SCHEMA ",\"width\":{\"type\":\"number\"},"
     "\"height\":{\"type\":\"number\"},\"kind\":{\"type\":\"string\","
     "\"enum\":[\"straight\",\"l\",\"u\",\"curved\",\"spiral\"]},"
     "\"turn\":{\"type\":\"string\",\"enum\":[\"left\",\"right\"]},"
     "\"sweep\":{\"type\":\"number\"},\"thickness\":{\"type\":"
     "\"number\"},\"step_height\":{\"type\":\"number\"}," OPS_BRUSH_SCHEMA
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"from\",\"to\"]}",
     NULL, ops_build_brush_stairs},
    {"brush.set_surface",
     "Set the surface tag, the mark or both of one 'face', or of a brush's "
     "faces, all or those 'faces' selects (top, bottom, sides, +x, -x, +z, "
     "-z). Faces show the fixed greybox look of their surface and mark "
     "until the art pass binds a material.",
     "{\"type\":\"object\",\"properties\":{\"brush\":" OPS_ENTITY_SCHEMA
     ",\"face\":" OPS_ENTITY_SCHEMA ",\"surface\":" OPS_SURFACE_SCHEMA
     ",\"mark\":" OPS_MARK_SCHEMA ",\"faces\":{\"type\":\"array\","
     "\"items\":{\"type\":\"string\"}}," OPS_REVIEW_SCHEMA "}}",
     NULL, ops_build_set_surface},
    {"face.set_material",
     "Art pass: give one 'face', or a brush's faces (all or those 'faces' "
     "selects), an art-owned material file over its surface's greybox "
     "look; an empty 'material' returns the faces to their greybox look. "
     "The greybox view (view.greybox) still shows the greybox looks.",
     "{\"type\":\"object\",\"properties\":{\"brush\":" OPS_ENTITY_SCHEMA
     ",\"face\":" OPS_ENTITY_SCHEMA ",\"material\":{\"type\":\"string\"},"
     "\"faces\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
     "," OPS_REVIEW_SCHEMA "},\"required\":[\"material\"]}",
     NULL, ops_build_face_material},
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
     "'gap' leaves metres between them. Either object may be one an earlier "
     "operation of the same batch made.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"target\":" OPS_ENTITY_SCHEMA ",\"mode\":{\"type\":\"string\","
     "\"enum\":[\"on\",\"against\",\"inside\"]},\"side\":{\"type\":"
     "\"string\",\"enum\":[\"+x\",\"-x\",\"+z\",\"-z\"]},\"gap\":{\"type\":"
     "\"number\"},\"keep\":{\"type\":\"boolean\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"entity\",\"target\",\"mode\"]}",
     NULL, ops_build_place},
    {"entity.move",
     "Move an entity by the world 'offset' [x, y, z] in meters, whatever its "
     "parent's pose; its children follow. A batch of these moves a group as "
     "one undo step.",
     "{\"type\":\"object\",\"properties\":{\"entity\":" OPS_ENTITY_SCHEMA
     ",\"offset\":" OPS_VEC3_SCHEMA "," OPS_REVIEW_SCHEMA
     "},\"required\":[\"entity\",\"offset\"]}",
     NULL, ops_build_move},
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
     "in when negative. 'min' and 'max' are [u, v] corners in world meters "
     "along the face's grid axes: on a floor u is world +X and v is world "
     "-Z, so v = -z; a rectangle off the face fails with the face's u and v "
     "ranges and axes. A pulled patch joins the brush "
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
     "\"ceiling\":{\"type\":\"boolean\"},\"floor_surface\":" OPS_SURFACE_SCHEMA
     "," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA
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
     "\"ceiling\":{\"type\":\"boolean\"},\"floor_surface\":" OPS_SURFACE_SCHEMA
     "," OPS_BRUSH_SCHEMA "," OPS_REVIEW_SCHEMA "}}",
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
     "on_opened, on_close and on_closed. A nonzero 'angle' turns them that "
     "many degrees about 'axis' through 'pivot' (its own space) at 'speed' "
     "degrees per second instead, and 'spin' turns them without end while "
     "open, as a fan. 'hinge' (+x, -x, +z or -z) makes a door: the pivot on "
     "the vertical line through the middle of that side of their box, 90 "
     "degrees unless 'values' gives an angle. 'values' sets the mover "
     "component (direction, distance, lip, angle, axis, pivot, spin, speed, "
     "acceleration, wait, start_open, loop, locked).",
     "{\"type\":\"object\",\"properties\":{\"objects\":{\"type\":\"array\","
     "\"items\":" OPS_ENTITY_SCHEMA ",\"minItems\":1,\"maxItems\":64},"
     "\"hinge\":{\"type\":\"string\",\"enum\":[\"+x\",\"-x\",\"+z\","
     "\"-z\"]},"
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
    {"scatter.paint",
     "Paint a scatter: one scatter_area child per world point of 'points' "
     "(up to 256), each filling a disc of 'radius' metres (default 2) with "
     "'density' copies per square metre (default 1), leaving out copies "
     "within 'spacing' metres (default 0) of earlier ones. Once a scatter "
     "has areas it places copies only in them. With 'erase' it deletes the "
     "areas whose centre lies within 'radius' of a point instead. One undo "
     "step.",
     "{\"type\":\"object\",\"properties\":{\"scatter\":" OPS_ENTITY_SCHEMA
     ",\"points\":{\"type\":\"array\",\"items\":" OPS_VEC3_SCHEMA
     "},\"radius\":{\"type\":\"number\"},\"density\":{\"type\":"
     "\"number\"},\"spacing\":{\"type\":\"number\"},\"erase\":{\"type\":"
     "\"boolean\"}," OPS_REVIEW_SCHEMA "},\"required\":[\"scatter\","
     "\"points\"]}",
     NULL, ops_build_scatter_paint},
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
     "operation k created, and a name also finds what an earlier operation "
     "made, so later operations edit, cut, place or group it (a room's walls "
     "are '<room>/Wall East +X'). A failure rolls the whole batch back.",
     "{\"type\":\"object\",\"properties\":{\"ops\":{\"type\":\"array\","
     "\"maxItems\":2048,\"items\":{\"type\":\"object\",\"properties\":{\"op\":"
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
     "high, slopes too steep, low ceilings, passages only a crouched capsule "
     "fits (crouch_only), gaps too narrow, edges into the void, areas the "
     "start (or the Player Start) cannot reach, overlapping solid brushes, "
     "brushes that did not build, IO connections that will not route, "
     "z_fight: drawn faces of two brushes or blockout pieces that share a "
     "plane and face the same way, so they flicker ('entity' and 'other', "
     "'value' the shared square meters, 'normal' the way they face), and "
     "mover_timing: a looping mover "
     "with no stay, or a door its arrival opens that is still open when it "
     "leaves ('value' the seconds). 'kinds' reports only the named kinds; "
     "'found' above the issues returned means 'limit' cut the list.",
     "{\"type\":\"object\",\"properties\":{\"region\":{\"type\":"
     "\"object\",\"properties\":{\"min\":" OPS_VEC3_SCHEMA
     ",\"max\":" OPS_VEC3_SCHEMA
     "},\"required\":[\"min\",\"max\"]},\"start\":" OPS_VEC3_SCHEMA
     ",\"container\":" OPS_CONTAINER_SCHEMA "," OPS_CAPSULE_SCHEMA
     ",\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}"
     ",\"kinds\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
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
     "standing or crouched and up or down ladders, with the route: about "
     "'points' points (default 32; 512 keeps each grid step of a route of "
     "about that many, as a driven player needs), always with both ends of "
     "each ladder or drop; 'crouch' says it passes floor only a crouched "
     "capsule fits and 'ladders' how many ladders it climbs.",
     "{\"type\":\"object\",\"properties\":{\"from\":" OPS_VEC3_SCHEMA
     ",\"to\":" OPS_VEC3_SCHEMA
     ",\"points\":{\"type\":\"integer\",\"minimum\":2,\"maximum\":512}"
     ",\"container\":" OPS_CONTAINER_SCHEMA "," OPS_CAPSULE_SCHEMA
     "," OPS_SETTLE_SCHEMA "},\"required\":[\"from\",\"to\"]}",
     ops_run_reachable, NULL, OPS_SETTLES},
    {"material.list",
     "Material files, graphs and surface themes under the content root's "
     "assets: path, kind (graph, instance, definition or theme) and an "
     "instance's graph; 'contains' filters by path.",
     "{\"type\":\"object\",\"properties\":{\"contains\":{\"type\":"
     "\"string\"}}}",
     ops_run_material_list, NULL, OPS_QUICK},
    {"material.describe",
     "One material document: a graph's nodes (one line each), exposed "
     "parameters, how it lowers (tier, reason, node, samples, pipelines) and "
     "its instances; an instance's graph, overrides and lowering; or a plain "
     "definition's text and whether it has a graph form.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
     "\"string\"}},\"required\":[\"path\"]}",
     ops_run_material_describe, NULL, OPS_QUICK},
    {"material.create",
     "Create a material document at a content path: a 'graph' (.mtg) from a "
     "starting template or 'from' a PBR .mt; an 'instance' (.mt) of a "
     "'graph' with 'params' overrides and a 'name'; or a 'definition' (.mt), "
     "copied 'from' another. Fails on an existing file unless 'overwrite'.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
     "\"string\"},\"kind\":{\"type\":\"string\",\"enum\":[\"graph\","
     "\"instance\",\"definition\"]},\"from\":{\"type\":\"string\"},"
     "\"graph\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},"
     "\"params\":{\"type\":\"object\"},\"overwrite\":{\"type\":"
     "\"boolean\"}," OPS_REVIEW_SCHEMA "},\"required\":[\"path\","
     "\"kind\"]}",
     ops_run_material_create, NULL},
    {"material.patch",
     "Edit a graph (.mtg) as one undo step: 'edits' in order, each an 'op': "
     "add {type, id?, value?, path?, parameter?, color_space?, mask?, "
     "position?} (types: scalar, color, texture (input uv), multiply, "
     "normal_map, layer (path to a layer .mt), layer_blend (mask "
     "vertex_color, texture with path, slope or height with value [from, "
     "to]; into output.layers), surface_output; Custom-tier nodes, which "
     "cost a shader: uv, vertex_color, world_position, world_normal, time, "
     "camera_distance, add, subtract, divide, min, max, lerp, power, "
     "one_minus, saturate, abs, sine, dot, split, combine, tile_offset "
     "(value [tile x, y, offset x, y]), world_planar (value meters)), "
     "remove {id}, connect {from: \"node.output\", to: \"node.input\"}, "
     "disconnect {to}, set {id, value?, path?, parameter?, color_space?, "
     "position?, rename?} or settings {alpha_mode?, alpha_cutoff?, "
     "double_sided?, subsurface_profile?, temporal_reactivity?, "
     "roughness_max?, world_size? (m per repeat on brush faces), surface? "
     "(tag)}. A failed edit or an invalid graph changes nothing. "
     "Answers how the graph lowers.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
     "\"string\"},\"edits\":{\"type\":\"array\",\"items\":{\"type\":"
     "\"object\"}},\"label\":{\"type\":\"string\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"path\",\"edits\"]}",
     ops_run_material_patch, NULL},
    {"material.set_param",
     "Set an instance's parameter overrides: 'params' maps names to a "
     "number, [r, g, b] or a texture path (relative to the instance with ./), "
     "null removing one. 'world_size' (meters a texture repeat covers on "
     "brush faces, a number or [x, y], 0 unsets) and 'surface' (a tag) "
     "replace the graph's.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
     "\"string\"},\"params\":{\"type\":\"object\"},\"world_size\":{},"
     "\"surface\":{\"type\":\"string\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"path\"]}",
     ops_run_material_set_param, NULL},
    {"material.compile",
     "Lower a graph or instance without changing it: tier, reason, node, "
     "samples, pipelines and the .mt definition the loader reads; a plain "
     "definition checks as the loader reads it.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
     "\"string\"}},\"required\":[\"path\"]}",
     ops_run_material_compile, NULL, OPS_QUICK},
    {"material.assign",
     "Assign a material (.mt) to a 'face', a 'brush' (all faces or those "
     "'faces' selects) or an 'entity': a brush's faces, else a mesh's "
     "submesh 'slot' (0 to 7) or every submesh, through its "
     "material_override; an empty 'material' clears.",
     "{\"type\":\"object\",\"properties\":{\"material\":{\"type\":"
     "\"string\"},\"face\":" OPS_ENTITY_SCHEMA ",\"brush\":" OPS_ENTITY_SCHEMA
     ",\"entity\":" OPS_ENTITY_SCHEMA ",\"slot\":{"
     "\"type\":\"integer\"},\"faces\":{\"type\":\"array\",\"items\":"
     "{\"type\":\"string\"}}," OPS_REVIEW_SCHEMA "},\"required\":["
     "\"material\"]}",
     NULL, ops_build_material_assign},
    {"material.open",
     "Show a material document or surface theme in the Material panel, and "
     "with "
     "'workbench' switch to the Art workbench, so the designer sees it.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
     "\"string\"},\"workbench\":{\"type\":\"boolean\"}},"
     "\"required\":[\"path\"]}",
     ops_run_material_open, NULL},
    {"surface.list",
     "The surface tags (name, label, faces in the container and the material "
     "its theme binds, from the container's theme or the World's) and marks, "
     "with the container's 'theme' and the 'world_theme' beneath it.",
     "{\"type\":\"object\",\"properties\":{\"container\":" OPS_CONTAINER_SCHEMA
     "}}",
     ops_run_surface_list, NULL, OPS_QUICK},
    {"surface.theme.create",
     "Create a theme (.surfaces) at a content path: 'materials' maps surface "
     "tags to .mt paths. Fails on an existing file unless 'overwrite'.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
     "\"string\"},\"materials\":{\"type\":\"object\"},\"overwrite\":{"
     "\"type\":\"boolean\"}," OPS_REVIEW_SCHEMA "},\"required\":["
     "\"path\"]}",
     ops_run_theme_create, NULL},
    {"surface.theme.bind",
     "Bind surface tags to materials in a theme ('path', else the "
     "container's own): 'tag' and 'material', or 'materials' mapping tags to "
     ".mt paths; an empty or null material unbinds. Every face of a bound tag "
     "without its own material shows it.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
     "\"string\"},\"container\":" OPS_CONTAINER_SCHEMA ",\"tag\":{"
     "\"type\":\"string\"},\"material\":{\"type\":\"string\"},"
     "\"materials\":{\"type\":\"object\"}," OPS_REVIEW_SCHEMA "}}",
     ops_run_theme_bind, NULL},
    {"surface.theme.select",
     "Make a container's brush faces take 'theme' (.surfaces; empty clears): "
     "its surface_theme component, on a new 'Surface theme' entity when none "
     "carries one. Tags it leaves unbound take the World's theme.",
     "{\"type\":\"object\",\"properties\":{\"theme\":{\"type\":"
     "\"string\"},\"container\":" OPS_CONTAINER_SCHEMA "," OPS_REVIEW_SCHEMA
     "},\"required\":[\"theme\"]}",
     NULL, ops_build_theme_select},
    {"lighting.time",
     "Run the live time of day from 'hour' until the simulation resets, as "
     "Cmd time.hour does; to keep an hour, set the World's time_of_day "
     "component.",
     "{\"type\":\"object\",\"properties\":{\"hour\":{\"type\":"
     "\"number\"}},\"required\":[\"hour\"]}",
     ops_run_lighting_time, NULL, OPS_QUICK},
    {"lighting.list",
     "The open scene's and the World's lights (kind directional, point, "
     "spot or rectangle; 'from' scene or world; component values, with "
     "mobility and light_group) and the scene's light groups with their "
     "live intensity and current factor.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_lighting_list, NULL,
     OPS_QUICK},
    {"lighting.group",
     "Set light group 'group' to 'intensity' (0 switches its lights off) "
     "live in every loaded container, as scripts do, until the simulation "
     "resets.",
     "{\"type\":\"object\",\"properties\":{\"group\":{\"type\":"
     "\"string\"},\"intensity\":{\"type\":\"number\"}},"
     "\"required\":[\"group\",\"intensity\"]}",
     ops_run_lighting_group, NULL, OPS_QUICK},
    {"lighting.bake",
     "Start the open project scene's Bake lighting job: reflection probes, "
     "the diffuse volume and, with 'lightmap' (default the Bakery panel's "
     "option), lightmaps. 'lightmap_settings' (samples, max_depth, seed, "
     "page_size, texels_per_unit, denoise default|on|off, "
     "denoise_iterations, indirect_clamp) and 'diffuse_settings' "
     "(spacing, face_size, samples, max_depth, seed, photons, "
     "photon_radius) replace the Bake settings window's values; 0 keeps "
     "Bakery's default.",
     "{\"type\":\"object\",\"properties\":{\"lightmap\":{\"type\":"
     "\"boolean\"},\"lightmap_settings\":{\"type\":\"object\"},"
     "\"diffuse_settings\":{\"type\":\"object\"}}}",
     ops_run_lighting_bake, NULL},
    {"env.describe",
     "A container's environment by part (environment, atmosphere, clouds, "
     "fog, volumetric_fog, post_process, time_of_day, sun, moon): whether "
     "the container, the World or nothing sets it, the entity and the "
     "values.",
     "{\"type\":\"object\",\"properties\":{\"container\":" OPS_CONTAINER_SCHEMA
     "}}",
     ops_run_env_describe, NULL, OPS_QUICK},
    {"env.preset.save",
     "Save a container's resolved environment, World values included, as a "
     "preset document (.environment); 'overwrite' replaces one.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
     "\"string\"},\"container\":" OPS_CONTAINER_SCHEMA
     ",\"overwrite\":{\"type\":\"boolean\"}," OPS_REVIEW_SCHEMA
     "},\"required\":[\"path\"]}",
     ops_run_env_preset_save, NULL},
    {"env.preset.apply",
     "Copy a preset's values into a container: each part onto the "
     "container's own entity that holds it, else onto a new entity. The "
     "time of day applies to the World only, a sky light only where one "
     "exists.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
     "\"string\"},\"container\":" OPS_CONTAINER_SCHEMA "," OPS_REVIEW_SCHEMA
     "},\"required\":[\"path\"]}",
     NULL, ops_build_env_preset_apply},
    {"art.lint",
     "The art pass's problems in a container: tags no theme binds, "
     "materials that do not open or parse, missing or uncooked textures, "
     "texel density outside 'texel_min' to 'texel_max' px/m (128 and 2048) "
     "on faces, implausible untextured non-metal base colours (sRGB 30 to "
     "240) and metallic between 0.1 and 0.9; 'kinds' filters, 'limit' caps "
     "(100).",
     "{\"type\":\"object\",\"properties\":{\"container\":" OPS_CONTAINER_SCHEMA
     ",\"kinds\":{\"type\":\"array\",\"items\":{"
     "\"type\":\"string\"}},\"limit\":{\"type\":\"integer\"},"
     "\"texel_min\":{\"type\":\"number\"},\"texel_max\":{\"type\":"
     "\"number\"}}}",
     ops_run_art_lint, NULL, OPS_QUICK},
    {"query.measure",
     "Measure in meters: from 'from' to 'to' the distance, horizontal run, "
     "rise and slope in degrees; or the world 'size' of an 'entity' and its "
     "descendants.",
     "{\"type\":\"object\",\"properties\":{\"from\":" OPS_VEC3_SCHEMA
     ",\"to\":" OPS_VEC3_SCHEMA ",\"entity\":" OPS_ENTITY_SCHEMA
     "," OPS_SETTLE_SCHEMA "}}",
     ops_run_measure, NULL, OPS_QUICK | OPS_SETTLES},
    {"view.greybox",
     "Show every brush face's surface greybox look, art-owned materials "
     "too ('on' true), or return to them ('on' false). The view persists "
     "until changed; captures show what the Scene shows.",
     "{\"type\":\"object\",\"properties\":{\"on\":{\"type\":"
     "\"boolean\"}},\"required\":[\"on\"]}",
     ops_run_greybox, NULL},
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
     "outside the view. 'mode' renders a view in a render mode, such as the "
     "artist views base-color, roughness, metallic, normals, material-cost, "
     "texel-density and exposure (false colour by stops).",
     "{\"type\":\"object\",\"properties\":{\"view\":" OPS_CAPTURE_VIEW_SCHEMA
     ",\"focus\":" OPS_CAPTURE_FOCUS_SCHEMA ",\"grid_labels\":{\"type\":"
     "\"boolean\"},\"eye\":" OPS_VEC3_SCHEMA ",\"target\":" OPS_VEC3_SCHEMA
     ",\"mode\":" OPS_CAPTURE_MODE_SCHEMA
     ",\"views\":{\"type\":\"array\",\"maxItems\":4,\"items\":{\"type\":"
     "\"object\",\"properties\":{\"view\":" OPS_CAPTURE_VIEW_SCHEMA
     ",\"focus\":" OPS_CAPTURE_FOCUS_SCHEMA ",\"grid_labels\":{\"type\":"
     "\"boolean\"},\"eye\":" OPS_VEC3_SCHEMA ",\"target\":" OPS_VEC3_SCHEMA
     ",\"mode\":" OPS_CAPTURE_MODE_SCHEMA
     "}}},\"max_width\":{\"type\":\"integer\",\"minimum\":64,\"maximum\":"
     "8192},\"marks\":{\"type\":\"array\",\"maxItems\":32,\"items\":{"
     "\"oneOf\":[" OPS_VEC3_SCHEMA ",{\"type\":\"object\",\"properties\":{"
     "\"point\":" OPS_VEC3_SCHEMA ",\"label\":{\"type\":\"string\","
     "\"maxLength\":15}},\"required\":[\"point\"]}]}"
     "},\"area\":{\"type\":\"string\",\"enum\":[\"scene\","
     "\"window\"],\"description\":\"The Scene image (default) or the whole "
     "editor window\"}," OPS_SETTLE_SCHEMA "}}",
     ops_run_capture, NULL, OPS_SETTLES},
    {"query.luminance",
     "Measure the Scene's scene-linear HDR colour from the current view or "
     "one given as view.capture takes it ('view', 'focus', 'eye' and "
     "'target', 'mode'): at up to 32 'marks' (world points or {point, "
     "label}) and over 'region' ([x0, y0, x1, y1] fractions of the image, "
     "default the whole image). Each answers 'luminance', 'ev100' and "
     "'stops' from middle grey after the frame's exposure; the region adds "
     "'log_average' and 'peak'. A mark outside the view answers null.",
     "{\"type\":\"object\",\"properties\":{\"view\":" OPS_CAPTURE_VIEW_SCHEMA
     ",\"focus\":" OPS_CAPTURE_FOCUS_SCHEMA ",\"eye\":" OPS_VEC3_SCHEMA
     ",\"target\":" OPS_VEC3_SCHEMA ",\"mode\":" OPS_CAPTURE_MODE_SCHEMA
     ",\"region\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},"
     "\"minItems\":4,\"maxItems\":4},\"marks\":{\"type\":\"array\","
     "\"maxItems\":32,\"items\":{\"oneOf\":[" OPS_VEC3_SCHEMA
     ",{\"type\":\"object\",\"properties\":{\"point\":" OPS_VEC3_SCHEMA
     ",\"label\":{\"type\":\"string\",\"maxLength\":15}},\"required\":["
     "\"point\"]}]}}," OPS_SETTLE_SCHEMA "}}",
     ops_run_luminance, NULL, OPS_SETTLES},
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
    {"session.host",
     "Host a collaborative editing session of the open scene on 'bind' "
     "(default every interface, port 7330); answers the address and the key "
     "others join with. Every edit then applies in the host's order.",
     "{\"type\":\"object\",\"properties\":{\"bind\":{\"type\":"
     "\"string\"},\"name\":{\"type\":\"string\"}}}",
     ops_run_session_host, NULL},
    {"session.join",
     "Join the session at 'address' with the host's 64-digit 'key'. This "
     "editor must have the host's session scene open, unedited; the "
     "session's edits replay first. Poll session.status for 'joined'.",
     "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":"
     "\"string\"},\"key\":{\"type\":\"string\"},\"name\":{\"type\":"
     "\"string\"}},\"required\":[\"address\",\"key\"]}",
     ops_run_session_join, NULL},
    {"session.leave", "Leave or stop the collaborative session.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_session_leave, NULL},
    {"task.add",
     "Add an open task of a 'kind' (layout, material, lighting and so on) "
     "with a 'title', optionally a 'region' box of a 'container', and the "
     "capabilities it 'requires' of the editor that takes it (macos, "
     "windows, tiled, desktop, or an agent's own). In a collaborative "
     "session the host keeps the board for every editor.",
     "{\"type\":\"object\",\"properties\":{\"kind\":{\"type\":"
     "\"string\"},\"title\":{\"type\":\"string\"},\"region\":"
     "{\"type\":\"object\"},\"container\":" OPS_CONTAINER_SCHEMA
     ",\"requires\":{\"type\":\"array\",\"items\":{\"type\":"
     "\"string\"}}},\"required\":[\"kind\"]}",
     ops_run_task_add, NULL},
    {"task.next",
     "Take your next task: the one assigned to you, else the oldest open "
     "task of one of 'kinds' (any kind when absent) whose requirements your "
     "editor's capabilities (its platform and pipeline class) and your "
     "'capabilities' meet; null when none is open.",
     "{\"type\":\"object\",\"properties\":{\"kinds\":{\"type\":"
     "\"array\",\"items\":{\"type\":\"string\"}},\"capabilities\":"
     "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}",
     ops_run_task_next, NULL},
    {"task.done",
     "Finish your task with 'ok' (default true; false marks it failed) and "
     "a 'note'.",
     "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":"
     "\"integer\"},\"ok\":{\"type\":\"boolean\"},\"note\":{\"type\":"
     "\"string\"}},\"required\":[\"task\"]}",
     ops_run_task_done, NULL},
    {"task.slots",
     "Set how many tasks this editor's agents may hold at once (0 for no "
     "limit). In a collaborative session the host gives an editor no more "
     "tasks than its slots, so a machine with one slot, such as a bake host, "
     "takes one task at a time and the rest go to others.",
     "{\"type\":\"object\",\"properties\":{\"slots\":{\"type\":"
     "\"integer\",\"minimum\":0}},\"required\":[\"slots\"]}",
     ops_run_task_slots, NULL},
    {"task.list",
     "Every task of the board with its kind, title, state, assignee, note "
     "and region; 'state' keeps one state (open, assigned, done, failed).",
     "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":"
     "\"string\"}}}",
     ops_run_task_list, NULL, OPS_QUICK},
    {"session.status",
     "The session's mode, address and key, this editor's place in the edit "
     "order, edits waiting to apply, the last error, the digest of this "
     "editor's scenes and each peer's name, camera and selection.",
     "{\"type\":\"object\",\"properties\":{}}", ops_run_session_status, NULL,
     OPS_QUICK},
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
  /* In a collaborative session an agent's name carries its editor's, so
     claims, tasks and the feed tell agents of different machines apart;
     agent names never hold `@` (editor_agent.c). */
  if (call->author[0] && !strchr(call->author, '@')) {
    vkr_editor_session_author(editor->session, call->author, call->author,
                              sizeof(call->author));
  }
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

static void ops_feed_accept(VkrEditorOps *ops, const VkrEditorChange *change);

/* Answers one participant's request on the session host. */
static void ops_serve(VkrEditorOps *ops, const VkrSampleUiFrame *frame,
                      const VkrEditorSessionAsk *ask,
                      VkrEditorSessionAnswer *answer) {
  const char *code = NULL;
  switch (ask->kind) {
  case VKR_EDITOR_ASK_CLAIM_SET:
    answer->ok = ask->author[0] && ops_scene(frame, ask->claim.container) &&
                 ops_claim_put(ops, ask->author, &ask->claim, &answer->claim,
                               &code, answer->error, sizeof(answer->error));
    if (!ask->author[0] || !ops_scene(frame, ask->claim.container)) {
      code = OPS_INVALID;
      snprintf(answer->error, sizeof(answer->error),
               "The claim needs an agent and a scene the host has loaded");
    }
    break;
  case VKR_EDITOR_ASK_CLAIM_RELEASE:
    answer->ok = ops_claim_drop(ops, ask->author, ask->claim.id, ask->all,
                                answer->released, &answer->released_count,
                                &code, answer->error, sizeof(answer->error));
    break;
  case VKR_EDITOR_ASK_TASK_ADD:
    answer->has_task = true_v;
    answer->ok = ops_task_add(ops, &ask->task, &answer->task, &code,
                              answer->error, sizeof(answer->error));
    break;
  case VKR_EDITOR_ASK_TASK_NEXT:
    answer->ok = ops_task_take(ops, ask->author, ask->kinds, ask->capabilities,
                               &answer->task, &answer->has_task, &code,
                               answer->error, sizeof(answer->error));
    break;
  case VKR_EDITOR_ASK_TASK_SLOTS:
    answer->ok =
        ops_task_set_slots(ops, ops_author_editor(ask->author), ask->slots,
                           &code, answer->error, sizeof(answer->error));
    break;
  case VKR_EDITOR_ASK_TASK_DONE:
    answer->has_task = true_v;
    answer->ok = ops_task_finish(ops, ask->author, &ask->task, &answer->task,
                                 &code, answer->error, sizeof(answer->error));
    break;
  default:
    code = OPS_INVALID;
    snprintf(answer->error, sizeof(answer->error), "Unknown request");
    break;
  }
  snprintf(answer->code, sizeof(answer->code), "%s",
           !answer->ok && code ? code : "");
}

/* Shows the session host's claims and tasks in place of this editor's,
   feeding the claims that appeared, moved or left. */
static void ops_mirror(VkrEditorOps *ops, const VkrEditorClaim *claims,
                       uint32_t claim_count, const VkrEditorTask *tasks,
                       uint32_t task_count) {
  for (uint32_t i = 0; i < ops->claim_count; ++i) {
    const VkrEditorClaim *old = &ops->claims[i];
    bool8_t kept = false_v;
    for (uint32_t k = 0; !kept && k < claim_count; ++k) {
      kept = claims[k].id == old->id;
    }
    if (!kept) {
      OpsFeedEvent *event = ops_feed_add(ops, OPS_FEED_RELEASED, old->container,
                                         old->author, old->name);
      event->claim = old->id;
      event->min = old->min;
      event->max = old->max;
      event->bounded = true_v;
    }
  }
  for (uint32_t k = 0; k < claim_count; ++k) {
    const VkrEditorClaim *claim = &claims[k];
    bool8_t same = false_v;
    for (uint32_t i = 0; !same && i < ops->claim_count; ++i) {
      same = MemCompare(&ops->claims[i], claim, sizeof(*claim)) == 0;
    }
    if (!same) {
      OpsFeedEvent *event = ops_feed_add(
          ops, OPS_FEED_CLAIMED, claim->container, claim->author, claim->name);
      event->claim = claim->id;
      event->min = claim->min;
      event->max = claim->max;
      event->bounded = true_v;
    }
  }
  ops->claim_count = Min(claim_count, (uint32_t)VKR_EDITOR_CLAIM_MAX);
  MemCopy(ops->claims, claims, ops->claim_count * sizeof(*claims));
  ops->task_count = Min(task_count, (uint32_t)VKR_EDITOR_TASK_MAX);
  MemCopy(ops->tasks, tasks, ops->task_count * sizeof(*tasks));
}

/* The session's share of the agent channel (ADR-106): the host answers
   participants' requests and sends its claims and tasks when they change;
   a participant shows the host's; every editor feeds the agent batches
   other editors applied here. */
static void ops_session_update(VkrEditorOps *ops, VkrEditorSession *session,
                               const VkrSampleUiFrame *frame) {
  if (vkr_editor_session_hosting(session)) {
    VkrEditorSessionAsk ask;
    while (vkr_editor_session_take_ask(session, &ask)) {
      VkrEditorSessionAnswer answer = {0};
      ops_serve(ops, frame, &ask, &answer);
      vkr_editor_session_reply(session, &ask, &answer);
    }
    const uint64_t epoch = vkr_editor_session_epoch(session);
    if (ops->published_epoch != epoch ||
        ops->published_revision != ops->shared_revision) {
      ops->published_epoch = epoch;
      ops->published_revision = ops->shared_revision;
      vkr_editor_session_publish(session, ops->claims, ops->claim_count,
                                 ops->tasks, ops->task_count);
      ops_claims_save(ops, frame);
    }
  }
  const VkrEditorClaim *claims = NULL;
  const VkrEditorTask *tasks = NULL;
  uint32_t claim_count = 0u;
  uint32_t task_count = 0u;
  const uint64_t revision = vkr_editor_session_shared(
      session, &claims, &claim_count, &tasks, &task_count);
  const uint64_t epoch = vkr_editor_session_epoch(session);
  if (revision && (!ops->mirroring || ops->mirror_epoch != epoch ||
                   ops->mirror_revision != revision)) {
    ops->mirroring = true_v;
    ops->mirror_epoch = epoch;
    ops->mirror_revision = revision;
    ops_mirror(ops, claims, claim_count, tasks, task_count);
  } else if (ops->mirroring && !vkr_editor_session_forwards(session)) {
    /* Out of the session: this editor's own claims return; the host's
       tasks stay with the host. */
    ops->mirroring = false_v;
    ops->task_count = 0u;
    ops_claims_load(ops, frame);
  }
  VkrEditorSessionApplied applied;
  while (vkr_editor_session_take_applied(session, &applied)) {
    if (applied.reverted) {
      (void)ops_feed_add(ops, OPS_FEED_REVERTED, applied.container,
                         applied.author, applied.label);
      continue;
    }
    const VkrScene *scene = ops_scene(frame, applied.container);
    OpsFeedEvent *event = ops_feed_add(ops, OPS_FEED_APPLIED, applied.container,
                                       applied.author, applied.label);
    VkrBrushGeometry *scratch = malloc(sizeof(VkrBrushGeometry));
    for (uint32_t i = 0; i < applied.entity_count; ++i) {
      ops_feed_entity(event, scene, applied.entities[i], scratch);
    }
    free(scratch);
    /* Another editor's agent batch waits for review here too. */
    if (applied.review && applied.group &&
        ops->change_count < VKR_EDITOR_CHANGE_MAX) {
      VkrEditorChange change = {.id = ++ops->next_change_id,
                                .group = applied.group,
                                .container = applied.container,
                                .created = vkr_platform_get_absolute_time()};
      snprintf(change.author, sizeof(change.author), "%s", applied.author);
      snprintf(change.label, sizeof(change.label), "%s", applied.label);
      if (!change.label[0]) {
        snprintf(change.label, sizeof(change.label), "Agent edit %u",
                 change.id);
      }
      for (uint32_t i = 0; i < applied.entity_count; ++i) {
        ops_change_touch(&change, applied.entities[i]);
      }
      ops->changes[ops->change_count++] = change;
      event->change = change.id;
    }
  }
  /* A batch accepted on another editor leaves review here. */
  uint16_t container = 0u;
  uint64_t group = 0u;
  while (vkr_editor_session_take_accepted(session, &container, &group)) {
    for (uint32_t i = 0; i < ops->change_count; ++i) {
      const VkrEditorChange *change = &ops->changes[i];
      if (!change->document[0] && change->group == group &&
          change->container == container) {
        ops_feed_accept(ops, change);
        ops_change_remove(ops, i);
        break;
      }
    }
  }
}

void vkr_editor_ops_update(VkrEditorOps *ops, const VkrEditorUi *editor,
                           const VkrSampleUiFrame *frame) {
  if (ops && editor) {
    ops->session = editor->session;
    ops_session_update(ops, editor->session, frame);
  }
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
    const VkrEditorChange *change = &ops->changes[i];
    const VkrSceneEditState *journal = ops_journal(frame, change->container);
    const bool8_t present =
        change->document[0]
            ? vkr_editor_material_group_present(editor->materials,
                                                change->group)
            : journal && vkr_scene_edit_group_present(journal, change->group);
    if (!present) {
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

/* Accepts a change here and, for a scene change, on every editor of the
   session. */
static void ops_change_accept(VkrEditorOps *ops,
                              const VkrEditorChange *change) {
  ops_feed_accept(ops, change);
  if (!change->document[0]) {
    vkr_editor_session_accepted(ops->session, change->container, change->group);
  }
}

bool8_t vkr_editor_ops_accept(VkrEditorOps *ops, uint32_t id) {
  if (!ops) {
    return false_v;
  }
  if (!id) {
    for (uint32_t i = 0; i < ops->change_count; ++i) {
      ops_change_accept(ops, &ops->changes[i]);
    }
    ops->change_count = 0u;
    return true_v;
  }
  const int32_t index = ops_change_find(ops, id);
  if (index < 0) {
    return false_v;
  }
  ops_change_accept(ops, &ops->changes[index]);
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
