/**
 * @file vkr_io_router.h
 * @brief Entity IO routing (ADR-084): outputs to inputs through connections.
 *
 * Components declare outputs and inputs on their type descriptors
 * (VkrTypeDesc.outputs and .inputs); script behaviors declare theirs with
 * VKR_OUTPUTS and VKR_INPUTS, which the script host copies onto its type
 * copies. An `io_connection` child of a source entity wires one output to
 * one input of a target in the same container.
 *
 * The script host owns one router per session. Publication resolves every
 * connection to entities and ports once; an invalid one is reported and
 * never routes. Each completed tick then drains the simulated scene's sensor
 * events into trigger outputs and script trigger hooks, fires due timers,
 * adds outputs scripts fired during the tick, and delivers first-in,
 * first-out, with delayed deliveries joining the first tick at or after
 * their deadline in simulation time. A tick that delivers more than
 * VKR_IO_DELIVERIES_PER_TICK or chains more than VKR_IO_CHAIN_MAX zero-delay
 * deliveries faults the router; nothing is dropped or deferred silently. A
 * delivery to a destroyed target is dropped and counted.
 *
 * The router keeps engine components' runtime state (occupants, running
 * timers, counter values) itself; components hold only authored values.
 */
#pragma once

#include "physics/vkr_physics.h"
#include "renderer/systems/vkr_scene_system.h"
#include "sdk.h"

/* Containers one session routes: the played one and the root World. */
#define VKR_IO_SCENE_MAX 2u
#define VKR_IO_QUEUE_MAX 4096u
#define VKR_IO_DELIVERIES_PER_TICK 4096u
#define VKR_IO_CHAIN_MAX 64u
/* Filtered entities one trigger tracks at once. */
#define VKR_IO_TRIGGER_OCCUPANTS 16u
#define VKR_IO_ERROR_CAPACITY 256u

/* Inputs every entity has, after its components' inputs. */
typedef enum VkrIoBuiltin {
  VKR_IO_BUILTIN_SHOW = 0,
  VKR_IO_BUILTIN_HIDE,
  VKR_IO_BUILTIN_DESTROY,
  VKR_IO_BUILTIN_COUNT,
} VkrIoBuiltin;

/* The inputs every entity has, as a port list. */
extern const VkrIoPort vkr_io_builtin_inputs[];

/* One output or input of an entity: port `port` of component `type`, or
 * with no type the built-in input `port`. */
typedef struct VkrIoEndpoint {
  const VkrTypeDesc *type;
  uint32_t port;
} VkrIoEndpoint;

/* What the router asks of its owner. */
typedef struct VkrIoRouterHooks {
  void *context;
  /* Runs input `input` of script type `type` on `target`; false when no
     running behavior handles it. */
  bool8_t (*script_input)(void *context, VkrScene *scene, VkrEntityId target,
                          const VkrTypeDesc *type, uint32_t input,
                          const VkrIoValue *value);
  /* Runs trigger_enter or trigger_exit of the behaviors on `entity`. */
  void (*trigger_hook)(void *context, VkrScene *scene, VkrEntityId entity,
                       VkrEntityId other, bool8_t enter);
  /* Destroys an entity and its children, running their destroy hooks. */
  void (*destroy)(void *context, VkrScene *scene, VkrEntityId entity);
} VkrIoRouterHooks;

struct IoConnection;
struct IoDelivery;
struct IoFired;
struct IoTrigger;
struct IoRelay;
struct IoTimer;
struct IoCounter;

typedef struct VkrIoRouter {
  VkrAllocator *allocator;
  VkrIoRouterHooks hooks;
  VkrScene *scenes[VKR_IO_SCENE_MAX];
  uint32_t scene_count;
  bool8_t published;
  bool8_t trace;
  bool8_t faulted;
  char error[VKR_IO_ERROR_CAPACITY];
  /* Simulation seconds of the last tick. */
  float64_t now;
  uint64_t sequence;
  /* Sorted by source entity and output, so one firing finds its
     connections by binary search. */
  struct IoConnection *connections;
  uint32_t connection_count;
  struct IoTrigger *triggers;
  uint32_t trigger_count;
  struct IoRelay *relays;
  uint32_t relay_count;
  struct IoTimer *timers;
  uint32_t timer_count;
  struct IoCounter *counters;
  uint32_t counter_count;
  /* What publication reserved for each list. */
  uint32_t capacities[5];
  /* Zero-delay deliveries, a ring. */
  struct IoDelivery *queue;
  uint32_t queue_head;
  uint32_t queue_count;
  /* Delayed deliveries by deadline, then sequence. */
  struct IoDelivery *delayed;
  uint32_t delayed_count;
  /* Outputs scripts fired during a tick, delivered after it. */
  struct IoFired *fired;
  uint32_t fired_count;
  VkrPhysicsSensorEvent *events;
  uint32_t event_capacity;
  /* Connections publication refused, and deliveries whose target died. */
  uint32_t problems;
  uint64_t dropped;
  uint32_t delivered;
} VkrIoRouter;

/** The VkrIoKind a port of property kind `kind` carries. */
VkrIoKind vkr_io_kind(uint32_t kind);

/** Output `name` of `entity`, as `on_enter` or `trigger.on_enter`: the first
 * of its component types, in registration order, that declares it. */
bool8_t vkr_io_find_output(const VkrScene *scene, VkrEntityId entity,
                           String8 name, VkrIoEndpoint *out);
/** Input `name` of `entity`, a component's or a built-in one. */
bool8_t vkr_io_find_input(const VkrScene *scene, VkrEntityId entity,
                          String8 name, VkrIoEndpoint *out);
/** The port an endpoint names, or NULL. */
const VkrIoPort *vkr_io_endpoint_port(VkrIoEndpoint endpoint);

/** Parses connection value text for a port of property kind `kind`: a
 * number, `true` or `false`, space-separated components, or an entity's
 * name or id in `scene`. */
bool8_t vkr_io_parse_value(const VkrScene *scene, uint32_t kind,
                           const char *text, VkrIoValue *out);

/** Why connection entity `connection` cannot route, as publication would
 * report it; false and an empty `error` when it can. */
bool8_t vkr_io_connection_problem(const VkrScene *scene, VkrEntityId connection,
                                  char *error, uint32_t capacity);

/** Resolves every connection and engine component of `scenes` and reserves
 * the queues. Invalid connections are logged and counted in `problems`.
 * False only when storage could not be reserved. */
bool8_t vkr_io_router_publish(VkrIoRouter *router, VkrScene *const *scenes,
                              uint32_t scene_count,
                              const VkrIoRouterHooks *hooks,
                              VkrAllocator *allocator);
/** Resolves the router's scenes anew after entities came or went, as world
 * partition cells do during a session. Engine components and connections
 * that stayed keep their runtime state, and pending deliveries stay queued.
 * False, routing as before, when storage could not be reserved. */
bool8_t vkr_io_router_refresh(VkrIoRouter *router);
/** Frees everything publication reserved; pending deliveries and fire
 * counts end with it. */
void vkr_io_router_clear(VkrIoRouter *router);
/** Published with something to route or sense. */
bool8_t vkr_io_router_active(const VkrIoRouter *router);

/** `source` fired `output`. With `deferred`, as inside a tick, the
 * deliveries wait for the tick's end; otherwise they run now. False when
 * the router faulted. */
bool8_t vkr_io_router_fire(VkrIoRouter *router, VkrEntityId source,
                           VkrIoEndpoint output, const VkrIoValue *value,
                           bool8_t deferred);
/** Delivers `input` to `target` as a connection would. */
bool8_t vkr_io_router_send(VkrIoRouter *router, VkrEntityId target,
                           VkrIoEndpoint input, const VkrIoValue *value,
                           bool8_t deferred);
/** After a completed tick of `scene` at simulation time `now`: sensors,
 * timers, deferred fires and deliveries. False when the router faulted;
 * `error` says why. */
bool8_t vkr_io_router_tick(VkrIoRouter *router, VkrScene *scene, float64_t now);
