#include "script/vkr_io_router.h"

#include "core/logger.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_types.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY

const VkrIoPort vkr_io_builtin_inputs[] = {
    {"show", "Show", VKR_IO_PORT_NONE},
    {"hide", "Hide", VKR_IO_PORT_NONE},
    {"destroy", "Destroy", VKR_IO_PORT_NONE},
    {NULL, NULL, 0u}};

typedef struct IoConnection {
  /* The io_connection entity, for the trace and fire counts. */
  VkrEntityId entity;
  VkrEntityId source;
  VkrIoEndpoint output;
  VkrEntityId target;
  VkrIoEndpoint input;
  bool8_t has_value;
  VkrIoValue value;
  float32_t delay;
  uint32_t limit;
  uint32_t fired;
} IoConnection;

typedef struct IoDelivery {
  /* The connection, or UINT32_MAX for a send. */
  uint32_t connection;
  VkrEntityId source;
  VkrEntityId target;
  VkrIoEndpoint input;
  VkrIoValue value;
  float64_t deadline;
  uint64_t sequence;
  /* Zero-delay hops since the chain's first delivery. */
  uint32_t depth;
} IoDelivery;

typedef struct IoFired {
  VkrEntityId source;
  VkrIoEndpoint output;
  VkrIoValue value;
  /* A send instead of a firing. */
  bool8_t send;
} IoFired;

typedef struct IoTrigger {
  VkrEntityId entity;
  bool8_t enabled;
  bool8_t once;
  /* The component the other entity must carry, or NULL for any. */
  const VkrTypeDesc *filter;
  bool8_t filter_missing;
  VkrEntityId occupants[VKR_IO_TRIGGER_OCCUPANTS];
  uint32_t occupant_count;
} IoTrigger;

typedef struct IoRelay {
  VkrEntityId entity;
  bool8_t enabled;
} IoRelay;

typedef struct IoTimer {
  VkrEntityId entity;
  float32_t interval;
  float64_t next;
  bool8_t running;
  bool8_t once;
} IoTimer;

typedef struct IoCounter {
  VkrEntityId entity;
  int32_t value;
  int32_t min;
  int32_t max;
} IoCounter;

// =============================================================================
// Ports and values
// =============================================================================

VkrIoKind vkr_io_kind(uint32_t kind) {
  switch (kind) {
  case VKR_PROPERTY_BOOL:
    return VKR_IO_BOOL;
  case VKR_PROPERTY_I32:
    return VKR_IO_I32;
  case VKR_PROPERTY_U32:
    return VKR_IO_U32;
  case VKR_PROPERTY_F32:
    return VKR_IO_F32;
  case VKR_PROPERTY_ANGLE:
    return VKR_IO_ANGLE;
  case VKR_PROPERTY_VEC2:
    return VKR_IO_VEC2;
  case VKR_PROPERTY_VEC3:
    return VKR_IO_VEC3;
  case VKR_PROPERTY_VEC4:
    return VKR_IO_VEC4;
  case VKR_PROPERTY_QUAT:
    return VKR_IO_QUAT;
  case VKR_PROPERTY_COLOR:
    return VKR_IO_COLOR;
  case VKR_PROPERTY_DIRECTION:
    return VKR_IO_DIRECTION;
  case VKR_PROPERTY_ENUM:
    return VKR_IO_ENUM;
  case VKR_PROPERTY_ENTITY:
    return VKR_IO_ENTITY;
  default:
    return VKR_IO_NONE;
  }
}

static bool8_t io_kind_numeric(VkrIoKind kind) {
  return kind == VKR_IO_BOOL || kind == VKR_IO_I32 || kind == VKR_IO_U32 ||
         kind == VKR_IO_F32 || kind == VKR_IO_ANGLE || kind == VKR_IO_ENUM;
}

static float64_t io_value_number(const VkrIoValue *value) {
  switch (value->kind) {
  case VKR_IO_BOOL:
    return value->boolean ? 1.0 : 0.0;
  case VKR_IO_I32:
  case VKR_IO_ENUM:
    return value->i32;
  case VKR_IO_U32:
    return value->u32;
  case VKR_IO_F32:
  case VKR_IO_ANGLE:
    return value->f32;
  default:
    return 0.0;
  }
}

/* `value` as `kind`: numbers convert among the scalar kinds, anything else
   passes only as itself. */
static bool8_t io_value_convert(const VkrIoValue *value, VkrIoKind kind,
                                VkrIoValue *out) {
  if (kind == VKR_IO_NONE) {
    *out = (VkrIoValue){.kind = VKR_IO_NONE};
    return true_v;
  }
  if (value->kind == kind) {
    *out = *value;
    return true_v;
  }
  if (!io_kind_numeric(value->kind) || !io_kind_numeric(kind)) {
    return false_v;
  }
  const float64_t number = io_value_number(value);
  *out = (VkrIoValue){.kind = kind};
  switch (kind) {
  case VKR_IO_BOOL:
    out->boolean = number != 0.0;
    break;
  case VKR_IO_I32:
  case VKR_IO_ENUM:
    out->i32 = (int32_t)llround(number);
    break;
  case VKR_IO_U32:
    out->u32 = number > 0.0 ? (uint32_t)llround(number) : 0u;
    break;
  default:
    out->f32 = (float32_t)number;
    break;
  }
  return true_v;
}

static bool8_t io_names_equal(const char *name, String8 text) {
  return strlen(name) == text.length &&
         MemCompare(name, text.str, text.length) == 0;
}

/* Splits `component.port` at its dot; no dot leaves the component empty. */
static void io_split(String8 name, String8 *component, String8 *port) {
  for (uint64_t i = 0; i < name.length; ++i) {
    if (name.str[i] == '.') {
      *component = string8_create(name.str, i);
      *port = string8_create(name.str + i + 1u, name.length - i - 1u);
      return;
    }
  }
  *component = (String8){0};
  *port = name;
}

static bool8_t io_find(const VkrScene *scene, VkrEntityId entity, String8 name,
                       bool8_t inputs, VkrIoEndpoint *out) {
  if (!scene || !vkr_scene_entity_alive(scene, entity)) {
    return false_v;
  }
  String8 component = {0};
  String8 port = {0};
  io_split(name, &component, &port);
  for (uint32_t t = 0; t < scene->type_count; ++t) {
    const VkrTypeDesc *type = scene->types[t].type;
    const VkrIoPort *ports = inputs ? type->inputs : type->outputs;
    if (!ports ||
        (component.length && !io_names_equal(type->name, component)) ||
        !vkr_entity_has_component(scene->world, entity, scene->types[t].id)) {
      continue;
    }
    const uint32_t index = vkr_io_port_find(ports, port);
    if (index != UINT32_MAX) {
      *out = (VkrIoEndpoint){.type = type, .port = index};
      return true_v;
    }
  }
  if (inputs && (!component.length || io_names_equal("entity", component))) {
    const uint32_t index = vkr_io_port_find(vkr_io_builtin_inputs, port);
    if (index != UINT32_MAX) {
      *out = (VkrIoEndpoint){.type = NULL, .port = index};
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_io_find_output(const VkrScene *scene, VkrEntityId entity,
                           String8 name, VkrIoEndpoint *out) {
  return io_find(scene, entity, name, false_v, out);
}

bool8_t vkr_io_find_input(const VkrScene *scene, VkrEntityId entity,
                          String8 name, VkrIoEndpoint *out) {
  return io_find(scene, entity, name, true_v, out);
}

const VkrIoPort *vkr_io_endpoint_port(VkrIoEndpoint endpoint) {
  const VkrIoPort *ports =
      endpoint.type ? endpoint.type->inputs : vkr_io_builtin_inputs;
  return ports && endpoint.port < vkr_io_port_count(ports)
             ? &ports[endpoint.port]
             : NULL;
}

static const VkrIoPort *io_output_port(VkrIoEndpoint endpoint) {
  return endpoint.type &&
                 endpoint.port < vkr_io_port_count(endpoint.type->outputs)
             ? &endpoint.type->outputs[endpoint.port]
             : NULL;
}

/* The one entity of `scene` named exactly `name`. */
static VkrEntityId io_named(const VkrScene *scene, const char *name) {
  const String8 word =
      string8_create_from_cstr((const uint8_t *)name, strlen(name));
  VkrEntityId found = VKR_ENTITY_ID_INVALID;
  uint32_t count = 0u;
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    if (!vkr_scene_entity_alive(scene, entity)) {
      continue;
    }
    const String8 entity_name = vkr_scene_get_name(scene, entity);
    if (entity_name.length == word.length &&
        MemCompare(entity_name.str, word.str, word.length) == 0) {
      found = entity;
      count++;
    }
  }
  return count == 1u ? found : VKR_ENTITY_ID_INVALID;
}

bool8_t vkr_io_parse_value(const VkrScene *scene, uint32_t kind,
                           const char *text, VkrIoValue *out) {
  const VkrIoKind io = vkr_io_kind(kind);
  *out = (VkrIoValue){.kind = io};
  char *end = NULL;
  switch (io) {
  case VKR_IO_NONE:
    return true_v;
  case VKR_IO_BOOL:
    if (!strcmp(text, "true") || !strcmp(text, "1")) {
      out->boolean = true_v;
      return true_v;
    }
    out->boolean = false_v;
    return !strcmp(text, "false") || !strcmp(text, "0");
  case VKR_IO_I32:
  case VKR_IO_ENUM: {
    const long number = strtol(text, &end, 10);
    out->i32 = (int32_t)number;
    return end != text && !*end && number >= INT32_MIN && number <= INT32_MAX;
  }
  case VKR_IO_U32: {
    const long long number = strtoll(text, &end, 10);
    out->u32 = (uint32_t)number;
    return end != text && !*end && number >= 0 && number <= UINT32_MAX;
  }
  case VKR_IO_F32:
  case VKR_IO_ANGLE:
    out->f32 = strtof(text, &end);
    return end != text && !*end && isfinite(out->f32);
  case VKR_IO_ENTITY: {
    VkrEntityRef ref = {0};
    const VkrEntityId entity = vkr_entity_ref_parse(text, strlen(text), &ref)
                                   ? vkr_scene_find_entity_ref(scene, &ref)
                                   : io_named(scene, text);
    out->entity = (VkrEntity){.id = entity.u64};
    return entity.u64 != 0u;
  }
  default: {
    /* VEC2 to QUAT, COLOR and DIRECTION: two to four numbers. */
    const uint32_t count = io == VKR_IO_VEC2                        ? 2u
                           : io == VKR_IO_VEC4 || io == VKR_IO_QUAT ? 4u
                                                                    : 3u;
    const char *at = text;
    for (uint32_t i = 0; i < count; ++i) {
      out->vector.elements[i] = strtof(at, &end);
      if (end == at || !isfinite(out->vector.elements[i])) {
        return false_v;
      }
      at = end;
      while (*at == ' ' || *at == ',') {
        ++at;
      }
    }
    return !*at;
  }
  }
}

/* Resolves one connection entity; on failure `error` says why. */
static bool8_t io_connection_resolve(const VkrScene *scene, VkrEntityId entity,
                                     IoConnection *out, char *error,
                                     uint32_t capacity) {
  const SceneIoConnection *connection =
      vkr_scene_get_typed(scene, entity, &vkr_scene_io_connection_type);
  const SceneTransform *transform =
      connection ? vkr_entity_get_component(scene->world, entity,
                                            scene->comp_transform)
                 : NULL;
  *out = (IoConnection){.entity = entity};
  if (!connection) {
    snprintf(error, capacity, "Not a connection");
    return false_v;
  }
  out->source = transform ? transform->parent : VKR_ENTITY_ID_INVALID;
  if (!vkr_scene_entity_alive(scene, out->source)) {
    snprintf(error, capacity, "A connection needs its source as parent");
    return false_v;
  }
  const String8 output = string8_create_from_cstr(
      (const uint8_t *)connection->output, strlen(connection->output));
  if (!vkr_io_find_output(scene, out->source, output, &out->output)) {
    snprintf(error, capacity, "The source has no output '%s'",
             connection->output);
    return false_v;
  }
  out->target = vkr_scene_find_entity_ref(scene, &connection->target);
  if (!out->target.u64) {
    snprintf(error, capacity, "%s",
             vkr_entity_ref_empty(&connection->target)
                 ? "The connection has no target"
                 : "The target is missing from this scene");
    return false_v;
  }
  const String8 input = string8_create_from_cstr(
      (const uint8_t *)connection->input, strlen(connection->input));
  if (!vkr_io_find_input(scene, out->target, input, &out->input)) {
    snprintf(error, capacity, "The target has no input '%s'",
             connection->input);
    return false_v;
  }
  const VkrIoPort *in = vkr_io_endpoint_port(out->input);
  const VkrIoPort *from = io_output_port(out->output);
  const VkrIoKind wanted = vkr_io_kind(in->kind);
  if (connection->value[0]) {
    out->has_value = true_v;
    if (!vkr_io_parse_value(scene, in->kind, connection->value, &out->value)) {
      snprintf(error, capacity, "'%s' is not a value for '%s'",
               connection->value, in->name);
      return false_v;
    }
  } else if (wanted != VKR_IO_NONE) {
    VkrIoValue probe = {.kind = vkr_io_kind(from->kind)};
    VkrIoValue converted;
    if (!io_value_convert(&probe, wanted, &converted)) {
      snprintf(error, capacity, "'%s' needs a value '%s' does not carry",
               in->name, from->name);
      return false_v;
    }
  }
  out->delay = connection->delay;
  out->limit = connection->limit;
  return true_v;
}

bool8_t vkr_io_connection_problem(const VkrScene *scene, VkrEntityId connection,
                                  char *error, uint32_t capacity) {
  IoConnection resolved;
  error[0] = '\0';
  return !io_connection_resolve(scene, connection, &resolved, error, capacity);
}

// =============================================================================
// Publication
// =============================================================================

static int io_connection_compare(const void *left, const void *right) {
  const IoConnection *a = left;
  const IoConnection *b = right;
  if (a->source.u64 != b->source.u64) {
    return a->source.u64 < b->source.u64 ? -1 : 1;
  }
  if (a->output.type != b->output.type) {
    return (uintptr_t)a->output.type < (uintptr_t)b->output.type ? -1 : 1;
  }
  if (a->output.port != b->output.port) {
    return a->output.port < b->output.port ? -1 : 1;
  }
  /* Connections of one output deliver in their creation order. */
  return a->entity.parts.index < b->entity.parts.index   ? -1
         : a->entity.parts.index > b->entity.parts.index ? 1
                                                         : 0;
}

static VkrScene *io_scene_of(const VkrIoRouter *router, VkrEntityId entity) {
  for (uint32_t i = 0; i < router->scene_count; ++i) {
    if (router->scenes[i]->world_id == entity.parts.world) {
      return router->scenes[i];
    }
  }
  return NULL;
}

/* Entities of `scene` carrying `type`, appended to `out` past `*count`. */
static uint32_t io_count_typed(VkrScene *const *scenes, uint32_t scene_count,
                               const VkrTypeDesc *type) {
  uint32_t count = 0u;
  for (uint32_t s = 0; s < scene_count; ++s) {
    count += vkr_scene_find_typed(scenes[s], type, NULL, 0u);
  }
  return count;
}

static void *io_alloc(VkrIoRouter *router, uint64_t count, uint64_t size,
                      bool8_t *ok) {
  if (!count) {
    return NULL;
  }
  void *memory = vkr_allocator_alloc(router->allocator, count * size, IO_TAG);
  if (!memory) {
    *ok = false_v;
  } else {
    MemZero(memory, count * size);
  }
  return memory;
}

static void io_free(VkrIoRouter *router, void *memory, uint64_t count,
                    uint64_t size) {
  if (memory) {
    vkr_allocator_free(router->allocator, memory, count * size, IO_TAG);
  }
}

static String8 io_name(const VkrScene *scene, VkrEntityId entity) {
  const String8 name = scene ? vkr_scene_get_name(scene, entity) : (String8){0};
  return name.length ? name : string8_lit("(unnamed)");
}

/* Warns about connections whose zero-delay hops lead back to their source:
   such a chain only ends when an input stops passing it on. */
static void io_warn_cycles(VkrIoRouter *router) {
  for (uint32_t c = 0; c < router->connection_count; ++c) {
    const IoConnection *start = &router->connections[c];
    if (start->delay > 0.0f) {
      continue;
    }
    /* Walk at most connection_count zero-delay hops from its target. */
    VkrEntityId at = start->target;
    for (uint32_t hop = 0; hop < router->connection_count; ++hop) {
      const IoConnection *next = NULL;
      for (uint32_t n = 0; n < router->connection_count && !next; ++n) {
        if (router->connections[n].source.u64 == at.u64 &&
            router->connections[n].delay <= 0.0f) {
          next = &router->connections[n];
        }
      }
      if (!next) {
        break;
      }
      if (next->target.u64 == start->source.u64) {
        const VkrScene *scene = io_scene_of(router, start->source);
        const String8 name = io_name(scene, start->source);
        log_warn("[io] Zero-delay connections from '%.*s' lead back to it",
                 (int)name.length, name.str);
        break;
      }
      at = next->target;
    }
  }
}

bool8_t vkr_io_router_publish(VkrIoRouter *router, VkrScene *const *scenes,
                              uint32_t scene_count,
                              const VkrIoRouterHooks *hooks,
                              VkrAllocator *allocator) {
  vkr_io_router_clear(router);
  *router = (VkrIoRouter){
      .allocator = allocator, .hooks = *hooks, .trace = router->trace};
  scene_count = Min(scene_count, VKR_IO_SCENE_MAX);
  for (uint32_t s = 0; s < scene_count; ++s) {
    if (scenes[s] && (s == 0u || scenes[s] != scenes[0])) {
      router->scenes[router->scene_count++] = scenes[s];
    }
  }
  const uint32_t connections = io_count_typed(
      router->scenes, router->scene_count, &vkr_scene_io_connection_type);
  const uint32_t triggers = io_count_typed(router->scenes, router->scene_count,
                                           &vkr_scene_trigger_type);
  const uint32_t relays = io_count_typed(router->scenes, router->scene_count,
                                         &vkr_scene_relay_type);
  const uint32_t timers = io_count_typed(router->scenes, router->scene_count,
                                         &vkr_scene_timer_type);
  const uint32_t counters = io_count_typed(router->scenes, router->scene_count,
                                           &vkr_scene_counter_type);
  router->capacities[0] = connections;
  router->capacities[1] = triggers;
  router->capacities[2] = relays;
  router->capacities[3] = timers;
  router->capacities[4] = counters;
  bool8_t ok = true_v;
  router->connections =
      io_alloc(router, connections, sizeof(IoConnection), &ok);
  router->triggers = io_alloc(router, triggers, sizeof(IoTrigger), &ok);
  router->relays = io_alloc(router, relays, sizeof(IoRelay), &ok);
  router->timers = io_alloc(router, timers, sizeof(IoTimer), &ok);
  router->counters = io_alloc(router, counters, sizeof(IoCounter), &ok);
  router->queue = io_alloc(router, VKR_IO_QUEUE_MAX, sizeof(IoDelivery), &ok);
  router->delayed = io_alloc(router, VKR_IO_QUEUE_MAX, sizeof(IoDelivery), &ok);
  router->fired = io_alloc(router, VKR_IO_QUEUE_MAX, sizeof(IoFired), &ok);
  router->event_capacity =
      VKR_SCENE_PHYSICS_MAX_BODIES * VKR_PHYSICS_SENSOR_EVENTS_PER_BODY;
  router->events = io_alloc(router, router->event_capacity,
                            sizeof(VkrPhysicsSensorEvent), &ok);
  if (!ok) {
    vkr_io_router_clear(router);
    return false_v;
  }

  for (uint32_t s = 0; s < router->scene_count; ++s) {
    VkrScene *scene = router->scenes[s];
    for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
      const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
      if (!vkr_scene_entity_alive(scene, entity)) {
        continue;
      }
      const SceneTrigger *trigger =
          vkr_scene_get_typed(scene, entity, &vkr_scene_trigger_type);
      if (trigger && router->trigger_count < triggers) {
        IoTrigger *state = &router->triggers[router->trigger_count++];
        *state = (IoTrigger){.entity = entity,
                             .enabled = trigger->enabled,
                             .once = trigger->once};
        if (trigger->filter[0]) {
          state->filter = vkr_scene_world_type_named(string8_create_from_cstr(
              (const uint8_t *)trigger->filter, strlen(trigger->filter)));
          state->filter_missing = state->filter == NULL;
          if (state->filter_missing) {
            const String8 name = io_name(scene, entity);
            log_warn("[io] Trigger '%.*s' filters on unknown component '%s'",
                     (int)name.length, name.str, trigger->filter);
            router->problems++;
          }
        }
      }
      const SceneRelay *relay =
          vkr_scene_get_typed(scene, entity, &vkr_scene_relay_type);
      if (relay && router->relay_count < relays) {
        router->relays[router->relay_count++] =
            (IoRelay){.entity = entity, .enabled = relay->enabled};
      }
      const SceneTimer *timer =
          vkr_scene_get_typed(scene, entity, &vkr_scene_timer_type);
      if (timer && router->timer_count < timers) {
        router->timers[router->timer_count++] =
            (IoTimer){.entity = entity,
                      .interval = timer->interval,
                      .next = timer->interval,
                      .running = timer->start_running,
                      .once = timer->once};
      }
      const SceneCounter *counter =
          vkr_scene_get_typed(scene, entity, &vkr_scene_counter_type);
      if (counter && router->counter_count < counters) {
        router->counters[router->counter_count++] =
            (IoCounter){.entity = entity,
                        .value = counter->start,
                        .min = counter->min,
                        .max = counter->max};
      }
      if (router->connection_count < connections &&
          vkr_scene_get_typed(scene, entity, &vkr_scene_io_connection_type)) {
        char error[160];
        IoConnection resolved;
        if (io_connection_resolve(scene, entity, &resolved, error,
                                  sizeof(error))) {
          router->connections[router->connection_count++] = resolved;
        } else {
          const SceneTransform *transform = vkr_entity_get_component(
              scene->world, entity, scene->comp_transform);
          const String8 source =
              io_name(scene, transform ? transform->parent : entity);
          log_warn("[io] Connection on '%.*s' does not route: %s",
                   (int)source.length, source.str, error);
          router->problems++;
        }
      }
    }
  }
  qsort(router->connections, router->connection_count, sizeof(IoConnection),
        io_connection_compare);
  io_warn_cycles(router);
  router->published = true_v;
  return true_v;
}

void vkr_io_router_clear(VkrIoRouter *router) {
  if (!router->allocator) {
    *router = (VkrIoRouter){.trace = router->trace};
    return;
  }
  io_free(router, router->connections, router->capacities[0],
          sizeof(IoConnection));
  io_free(router, router->triggers, router->capacities[1], sizeof(IoTrigger));
  io_free(router, router->relays, router->capacities[2], sizeof(IoRelay));
  io_free(router, router->timers, router->capacities[3], sizeof(IoTimer));
  io_free(router, router->counters, router->capacities[4], sizeof(IoCounter));
  io_free(router, router->queue, VKR_IO_QUEUE_MAX, sizeof(IoDelivery));
  io_free(router, router->delayed, VKR_IO_QUEUE_MAX, sizeof(IoDelivery));
  io_free(router, router->fired, VKR_IO_QUEUE_MAX, sizeof(IoFired));
  io_free(router, router->events, router->event_capacity,
          sizeof(VkrPhysicsSensorEvent));
  *router = (VkrIoRouter){.trace = router->trace};
}

bool8_t vkr_io_router_active(const VkrIoRouter *router) {
  return router->published && (router->connection_count ||
                               router->trigger_count || router->timer_count);
}

// =============================================================================
// Delivery
// =============================================================================

static bool8_t io_fault(VkrIoRouter *router, const char *format, ...) {
  if (!router->faulted) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(router->error, sizeof(router->error), format, args);
    va_end(args);
    router->faulted = true_v;
    log_error("[io] %s", router->error);
  }
  return false_v;
}

static void io_value_text(const VkrIoRouter *router, const VkrIoValue *value,
                          char *out, uint64_t capacity) {
  switch (value->kind) {
  case VKR_IO_NONE:
    out[0] = '\0';
    break;
  case VKR_IO_ENTITY: {
    const VkrEntityId entity = {.u64 = value->entity.id};
    const String8 name = io_name(io_scene_of(router, entity), entity);
    snprintf(out, capacity, "%.*s", (int)name.length, name.str);
    break;
  }
  case VKR_IO_BOOL:
    snprintf(out, capacity, "%s", value->boolean ? "true" : "false");
    break;
  case VKR_IO_I32:
  case VKR_IO_ENUM:
    snprintf(out, capacity, "%d", value->i32);
    break;
  case VKR_IO_U32:
    snprintf(out, capacity, "%u", value->u32);
    break;
  case VKR_IO_F32:
  case VKR_IO_ANGLE:
    snprintf(out, capacity, "%g", value->f32);
    break;
  default:
    snprintf(out, capacity, "%g %g %g", value->vector.x, value->vector.y,
             value->vector.z);
    break;
  }
}

/* `[io] 12.350 trigger_lobby.on_enter(player) -> door_a.open`. */
static void io_trace(const VkrIoRouter *router, const IoDelivery *delivery,
                     const char *output, const VkrIoValue *fired) {
  if (!router->trace) {
    return;
  }
  const VkrIoPort *input = vkr_io_endpoint_port(delivery->input);
  const String8 source =
      io_name(io_scene_of(router, delivery->source), delivery->source);
  const String8 target =
      io_name(io_scene_of(router, delivery->target), delivery->target);
  char value[96];
  io_value_text(router, fired, value, sizeof(value));
  log_info("[io] %.3f %.*s.%s(%s) -> %.*s.%s", router->now, (int)source.length,
           source.str, output, value, (int)target.length, target.str,
           input ? input->name : "?");
}

static bool8_t io_enqueue(VkrIoRouter *router, const IoDelivery *delivery,
                          bool8_t delayed) {
  if (!delayed) {
    if (router->queue_count == VKR_IO_QUEUE_MAX) {
      return io_fault(router, "More than %u deliveries are waiting",
                      VKR_IO_QUEUE_MAX);
    }
    router->queue[(router->queue_head + router->queue_count++) %
                  VKR_IO_QUEUE_MAX] = *delivery;
    return true_v;
  }
  if (router->delayed_count == VKR_IO_QUEUE_MAX) {
    return io_fault(router, "More than %u delayed deliveries are waiting",
                    VKR_IO_QUEUE_MAX);
  }
  /* Insert by deadline; equal deadlines keep sequence order. */
  uint32_t at = router->delayed_count;
  while (at > 0u && router->delayed[at - 1u].deadline > delivery->deadline) {
    router->delayed[at] = router->delayed[at - 1u];
    --at;
  }
  router->delayed[at] = *delivery;
  router->delayed_count++;
  return true_v;
}

/* Queues the deliveries of `source`'s `output`, `depth` hops into a chain. */
static bool8_t io_emit(VkrIoRouter *router, VkrEntityId source,
                       VkrIoEndpoint output, const VkrIoValue *value,
                       uint32_t depth) {
  const VkrIoValue none = {.kind = VKR_IO_NONE};
  if (!value) {
    value = &none;
  }
  /* The first connection of (source, output) by binary search. */
  uint32_t low = 0u;
  uint32_t high = router->connection_count;
  const IoConnection key = {.source = source, .output = output};
  while (low < high) {
    const uint32_t middle = (low + high) / 2u;
    IoConnection probe = router->connections[middle];
    probe.entity.u64 = 0u;
    if (io_connection_compare(&probe, &key) < 0) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  for (uint32_t c = low; c < router->connection_count; ++c) {
    IoConnection *connection = &router->connections[c];
    if (connection->source.u64 != source.u64 ||
        connection->output.type != output.type ||
        connection->output.port != output.port) {
      break;
    }
    if (connection->limit && connection->fired >= connection->limit) {
      continue;
    }
    connection->fired++;
    const VkrIoPort *in = vkr_io_endpoint_port(connection->input);
    IoDelivery delivery = {.connection = c,
                           .source = source,
                           .target = connection->target,
                           .input = connection->input,
                           .deadline = router->now + connection->delay,
                           .sequence = ++router->sequence,
                           .depth = connection->delay > 0.0f ? 0u : depth};
    if (connection->has_value) {
      delivery.value = connection->value;
    } else if (!io_value_convert(value, vkr_io_kind(in->kind),
                                 &delivery.value)) {
      return io_fault(router, "A value of the wrong kind reached '%s'",
                      in->name);
    }
    io_trace(router, &delivery, io_output_port(output)->name, value);
    if (!io_enqueue(router, &delivery, connection->delay > 0.0f)) {
      return false_v;
    }
  }
  return true_v;
}

static IoTrigger *io_trigger(VkrIoRouter *router, VkrEntityId entity) {
  for (uint32_t i = 0; i < router->trigger_count; ++i) {
    if (router->triggers[i].entity.u64 == entity.u64) {
      return &router->triggers[i];
    }
  }
  return NULL;
}

static IoRelay *io_relay(VkrIoRouter *router, VkrEntityId entity) {
  for (uint32_t i = 0; i < router->relay_count; ++i) {
    if (router->relays[i].entity.u64 == entity.u64) {
      return &router->relays[i];
    }
  }
  return NULL;
}

static IoTimer *io_timer(VkrIoRouter *router, VkrEntityId entity) {
  for (uint32_t i = 0; i < router->timer_count; ++i) {
    if (router->timers[i].entity.u64 == entity.u64) {
      return &router->timers[i];
    }
  }
  return NULL;
}

static IoCounter *io_counter(VkrIoRouter *router, VkrEntityId entity) {
  for (uint32_t i = 0; i < router->counter_count; ++i) {
    if (router->counters[i].entity.u64 == entity.u64) {
      return &router->counters[i];
    }
  }
  return NULL;
}

static VkrIoEndpoint io_out(const VkrTypeDesc *type, const char *name) {
  return (VkrIoEndpoint){
      .type = type,
      .port = vkr_io_port_find(
          type->outputs,
          string8_create_from_cstr((const uint8_t *)name, strlen(name)))};
}

/* Engine component inputs; false only when the router faulted. */
static bool8_t io_engine_input(VkrIoRouter *router, VkrScene *scene,
                               const IoDelivery *delivery) {
  const VkrIoPort *port = vkr_io_endpoint_port(delivery->input);
  const VkrTypeDesc *type = delivery->input.type;
  const char *name = port->name;
  const uint32_t depth = delivery->depth + 1u;
  if (!type) {
    if (delivery->input.port == VKR_IO_BUILTIN_DESTROY) {
      if (router->hooks.destroy) {
        router->hooks.destroy(router->hooks.context, scene, delivery->target);
      } else {
        vkr_scene_destroy_entity(scene, delivery->target);
      }
    } else {
      const SceneVisibility *visibility = vkr_entity_get_component(
          scene->world, delivery->target, scene->comp_visibility);
      vkr_scene_set_visibility(
          scene, delivery->target, delivery->input.port == VKR_IO_BUILTIN_SHOW,
          visibility ? visibility->inherit_parent : true_v);
    }
    return true_v;
  }
  if (type == &vkr_scene_trigger_type) {
    IoTrigger *trigger = io_trigger(router, delivery->target);
    if (trigger) {
      trigger->enabled = !strcmp(name, "enable")    ? true_v
                         : !strcmp(name, "disable") ? false_v
                                                    : !trigger->enabled;
    }
    return true_v;
  }
  if (type == &vkr_scene_relay_type) {
    IoRelay *relay = io_relay(router, delivery->target);
    if (!relay) {
      return true_v;
    }
    if (!strcmp(name, "trigger")) {
      return !relay->enabled ||
             io_emit(router, delivery->target, io_out(type, "on_trigger"), NULL,
                     depth);
    }
    relay->enabled = !strcmp(name, "enable");
    return true_v;
  }
  if (type == &vkr_scene_timer_type) {
    IoTimer *timer = io_timer(router, delivery->target);
    if (!timer) {
      return true_v;
    }
    if (!strcmp(name, "set_interval")) {
      timer->interval = Max(0.01f, delivery->value.f32);
    } else {
      timer->running = !strcmp(name, "start");
    }
    timer->next = router->now + timer->interval;
    return true_v;
  }
  if (type == &vkr_scene_counter_type) {
    IoCounter *counter = io_counter(router, delivery->target);
    if (!counter) {
      return true_v;
    }
    const int64_t amount = delivery->value.i32;
    int64_t next = !strcmp(name, "add")        ? counter->value + amount
                   : !strcmp(name, "subtract") ? counter->value - amount
                                               : amount;
    next = Max((int64_t)counter->min, Min((int64_t)counter->max, next));
    if (next == counter->value) {
      return true_v;
    }
    counter->value = (int32_t)next;
    const VkrIoValue changed = {.kind = VKR_IO_I32, .i32 = counter->value};
    if (!io_emit(router, delivery->target, io_out(type, "on_changed"), &changed,
                 depth)) {
      return false_v;
    }
    if (counter->value == counter->max) {
      return io_emit(router, delivery->target, io_out(type, "on_max"), NULL,
                     depth);
    }
    if (counter->value == counter->min) {
      return io_emit(router, delivery->target, io_out(type, "on_min"), NULL,
                     depth);
    }
    return true_v;
  }
  /* A script type: its behavior's handler. */
  if (router->hooks.script_input &&
      !router->hooks.script_input(router->hooks.context, scene,
                                  delivery->target, type, delivery->input.port,
                                  &delivery->value)) {
    const String8 target = io_name(scene, delivery->target);
    log_warn("[io] No running behavior on '%.*s' handles '%s'",
             (int)target.length, target.str, name);
  }
  return !router->faulted;
}

/* Delivers queued deliveries until none wait; the chain and per-tick
   limits fault the router. */
static bool8_t io_deliver(VkrIoRouter *router) {
  while (router->queue_count && !router->faulted) {
    const IoDelivery delivery = router->queue[router->queue_head];
    router->queue_head = (router->queue_head + 1u) % VKR_IO_QUEUE_MAX;
    router->queue_count--;
    if (++router->delivered > VKR_IO_DELIVERIES_PER_TICK) {
      return io_fault(router, "More than %u deliveries in one tick",
                      VKR_IO_DELIVERIES_PER_TICK);
    }
    if (delivery.depth > VKR_IO_CHAIN_MAX) {
      const VkrIoPort *input = vkr_io_endpoint_port(delivery.input);
      const String8 source =
          io_name(io_scene_of(router, delivery.source), delivery.source);
      const String8 target =
          io_name(io_scene_of(router, delivery.target), delivery.target);
      return io_fault(router,
                      "A chain of zero-delay connections ran past %u hops at "
                      "%.*s -> %.*s.%s",
                      VKR_IO_CHAIN_MAX, (int)source.length, source.str,
                      (int)target.length, target.str,
                      input ? input->name : "?");
    }
    VkrScene *scene = io_scene_of(router, delivery.target);
    if (!scene || !vkr_scene_entity_alive(scene, delivery.target)) {
      router->dropped++;
      continue;
    }
    if (!io_engine_input(router, scene, &delivery)) {
      return false_v;
    }
  }
  return !router->faulted;
}

bool8_t vkr_io_router_fire(VkrIoRouter *router, VkrEntityId source,
                           VkrIoEndpoint output, const VkrIoValue *value,
                           bool8_t deferred) {
  if (!router->published || router->faulted || !io_output_port(output)) {
    return false_v;
  }
  if (deferred) {
    if (router->fired_count == VKR_IO_QUEUE_MAX) {
      return io_fault(router, "More than %u outputs fired in one tick",
                      VKR_IO_QUEUE_MAX);
    }
    router->fired[router->fired_count++] =
        (IoFired){.source = source,
                  .output = output,
                  .value = value ? *value : (VkrIoValue){.kind = VKR_IO_NONE}};
    return true_v;
  }
  return io_emit(router, source, output, value, 0u) && io_deliver(router);
}

bool8_t vkr_io_router_send(VkrIoRouter *router, VkrEntityId target,
                           VkrIoEndpoint input, const VkrIoValue *value,
                           bool8_t deferred) {
  const VkrIoPort *port = vkr_io_endpoint_port(input);
  if (!router->published || router->faulted || !port) {
    return false_v;
  }
  IoDelivery delivery = {.connection = UINT32_MAX,
                         .source = target,
                         .target = target,
                         .input = input,
                         .deadline = router->now,
                         .sequence = ++router->sequence};
  const VkrIoValue none = {.kind = VKR_IO_NONE};
  if (!io_value_convert(value ? value : &none, vkr_io_kind(port->kind),
                        &delivery.value)) {
    return false_v;
  }
  if (deferred) {
    if (router->fired_count == VKR_IO_QUEUE_MAX) {
      return io_fault(router, "More than %u outputs fired in one tick",
                      VKR_IO_QUEUE_MAX);
    }
    router->fired[router->fired_count++] = (IoFired){.source = target,
                                                     .output = input,
                                                     .value = delivery.value,
                                                     .send = true_v};
    return true_v;
  }
  return io_enqueue(router, &delivery, false_v) && io_deliver(router);
}

// =============================================================================
// Ticks
// =============================================================================

static bool8_t io_trigger_passes(const VkrIoRouter *router,
                                 const IoTrigger *trigger, VkrEntityId other) {
  if (trigger->filter_missing) {
    return false_v;
  }
  if (!trigger->filter) {
    return true_v;
  }
  const VkrScene *scene = io_scene_of(router, other);
  return scene && vkr_scene_get_typed(scene, other, trigger->filter) != NULL;
}

/* One side of a sensor pair: `entity` saw `other` enter or leave. */
static bool8_t io_sense(VkrIoRouter *router, VkrScene *scene,
                        VkrEntityId entity, VkrEntityId other, bool8_t began) {
  if (!vkr_scene_entity_alive(scene, entity)) {
    return true_v;
  }
  if (router->hooks.trigger_hook) {
    router->hooks.trigger_hook(router->hooks.context, scene, entity, other,
                               began);
  }
  IoTrigger *trigger = io_trigger(router, entity);
  if (!trigger || !io_trigger_passes(router, trigger, other)) {
    return !router->faulted;
  }
  uint32_t index = trigger->occupant_count;
  for (uint32_t i = 0; i < trigger->occupant_count; ++i) {
    if (trigger->occupants[i].u64 == other.u64) {
      index = i;
    }
  }
  const VkrIoValue who = {.kind = VKR_IO_ENTITY,
                          .entity = (VkrEntity){.id = other.u64}};
  if (began) {
    if (index < trigger->occupant_count ||
        trigger->occupant_count == VKR_IO_TRIGGER_OCCUPANTS) {
      return true_v;
    }
    trigger->occupants[trigger->occupant_count++] = other;
    if (!trigger->enabled) {
      return true_v;
    }
    if (trigger->once) {
      trigger->enabled = false_v;
    }
    return io_emit(router, entity, io_out(&vkr_scene_trigger_type, "on_enter"),
                   &who, 0u);
  }
  if (index == trigger->occupant_count) {
    return true_v;
  }
  trigger->occupants[index] = trigger->occupants[--trigger->occupant_count];
  if (!trigger->enabled) {
    return true_v;
  }
  if (!io_emit(router, entity, io_out(&vkr_scene_trigger_type, "on_exit"), &who,
               0u)) {
    return false_v;
  }
  return trigger->occupant_count ||
         io_emit(router, entity, io_out(&vkr_scene_trigger_type, "on_empty"),
                 NULL, 0u);
}

bool8_t vkr_io_router_tick(VkrIoRouter *router, VkrScene *scene,
                           float64_t now) {
  if (!router->published || router->faulted) {
    return !router->faulted;
  }
  router->now = now;
  router->delivered = 0u;
  /* Deliveries whose deadline came, in deadline order. */
  uint32_t due = 0u;
  while (due < router->delayed_count && router->delayed[due].deadline <= now) {
    IoDelivery delivery = router->delayed[due++];
    delivery.depth = 0u;
    if (!io_enqueue(router, &delivery, false_v)) {
      return false_v;
    }
  }
  if (due) {
    router->delayed_count -= due;
    MemCopy(router->delayed, router->delayed + due,
            router->delayed_count * sizeof(IoDelivery));
  }
  /* Sensor pairs in the order physics reported them; both sides hear. */
  uint32_t count = 0u;
  if (scene && !vkr_scene_physics_sensor_events(
                   scene, router->events, router->event_capacity, &count)) {
    return io_fault(router, "Sensor events could not be read: %s",
                    vkr_scene_physics_error(scene));
  }
  for (uint32_t i = 0; i < count; ++i) {
    const VkrPhysicsSensorEvent *event = &router->events[i];
    const VkrEntityId a = {.u64 = event->entity_a};
    const VkrEntityId b = {.u64 = event->entity_b};
    if (!io_sense(router, scene, a, b, event->began) ||
        !io_sense(router, scene, b, a, event->began)) {
      return false_v;
    }
  }
  for (uint32_t i = 0; i < router->timer_count; ++i) {
    IoTimer *timer = &router->timers[i];
    /* One firing per tick; a long tick does not burst. */
    if (!timer->running || timer->next > now) {
      continue;
    }
    timer->next = Max(timer->next + timer->interval, now);
    timer->running = !timer->once;
    if (!io_emit(router, timer->entity,
                 io_out(&vkr_scene_timer_type, "on_timer"), NULL, 0u)) {
      return false_v;
    }
  }
  for (uint32_t i = 0; i < router->fired_count; ++i) {
    const IoFired *fired = &router->fired[i];
    if (fired->send) {
      const IoDelivery delivery = {.connection = UINT32_MAX,
                                   .source = fired->source,
                                   .target = fired->source,
                                   .input = fired->output,
                                   .value = fired->value,
                                   .deadline = now,
                                   .sequence = ++router->sequence};
      if (!io_enqueue(router, &delivery, false_v)) {
        return false_v;
      }
    } else if (!io_emit(router, fired->source, fired->output, &fired->value,
                        0u)) {
      return false_v;
    }
  }
  router->fired_count = 0u;
  return io_deliver(router);
}
