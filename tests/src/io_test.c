#include "io_test.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_types.h"
#include "script/vkr_io_router.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Entity IO routing (ADR-084). The oracle is what reaches inputs: a probe
 * type whose input records each delivery in order, and the engine counters'
 * observable values. Nothing here reads the router's internal tables. */

typedef struct IoProbe {
  int32_t unused;
} IoProbe;

static const VkrIoPort s_probe_inputs[] = {
    {"record", "Record", VKR_PROPERTY_I32}, {NULL, NULL, 0u}};
static const VkrIoPort s_probe_outputs[] = {
    {"on_value", "On value", VKR_PROPERTY_I32}, {NULL, NULL, 0u}};

static const VkrTypeDesc s_probe_type = {
    .name = "io_probe",
    .label = "IO probe",
    .size = sizeof(IoProbe),
    .align = _Alignof(IoProbe),
    .outputs = s_probe_outputs,
    .inputs = s_probe_inputs,
};

typedef struct IoRecord {
  VkrEntityId target;
  int32_t value;
} IoRecord;

typedef struct IoTest {
  VkrDMemory memory;
  VkrAllocator allocator;
  VkrScene scene;
  VkrIoRouter router;
  IoRecord records[64];
  uint32_t record_count;
} IoTest;

static bool8_t io_test_input(void *context, VkrScene *scene, VkrEntityId target,
                             const VkrTypeDesc *type, uint32_t input,
                             const VkrIoValue *value) {
  (void)scene;
  IoTest *test = context;
  assert(type == &s_probe_type && input == 0u && value->kind == VKR_IO_I32);
  assert(test->record_count < ArrayCount(test->records));
  test->records[test->record_count++] =
      (IoRecord){.target = target, .value = value->i32};
  return true_v;
}

static void io_test_begin(IoTest *test) {
  static bool8_t registered = false_v;
  if (!registered) {
    assert(vkr_scene_register_world_type(&s_probe_type));
    registered = true_v;
  }
  MemZero(test, sizeof(*test));
  assert(vkr_dmemory_create(MB(4), MB(8), &test->memory));
  test->allocator = (VkrAllocator){.ctx = &test->memory};
  vkr_dmemory_allocator_create(&test->allocator);
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&test->scene, &test->allocator, 0, 64, &error));
}

static void io_test_end(IoTest *test) {
  vkr_io_router_clear(&test->router);
  vkr_scene_shutdown(&test->scene, NULL);
  vkr_dmemory_allocator_destroy(&test->allocator);
}

static void io_test_publish(IoTest *test) {
  VkrScene *scenes[1] = {&test->scene};
  const VkrIoRouterHooks hooks = {.context = test,
                                  .script_input = io_test_input};
  assert(vkr_io_router_publish(&test->router, scenes, 1u, &hooks,
                               &test->allocator));
}

/* An entity with a name and an id, carrying `type` when given. */
static VkrEntityId io_test_entity(IoTest *test, const char *name,
                                  const VkrTypeDesc *type, const void *value) {
  VkrScene *scene = &test->scene;
  VkrEntityId entity = vkr_scene_create_entity(scene, NULL);
  assert(entity.u64);
  assert(vkr_scene_set_name(scene, entity,
                            string8_create((uint8_t *)name, strlen(name))));
  assert(vkr_scene_set_transform(scene, entity, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  VkrEntityRef ref;
  vkr_scene_entity_ref_generate(&ref);
  assert(vkr_scene_set_entity_ref(scene, entity, &ref));
  if (type) {
    _Alignas(16) uint8_t defaults[VKR_TYPE_VALUE_MAX];
    if (!value) {
      vkr_type_defaults(type, defaults);
      value = defaults;
    }
    assert(vkr_scene_set_typed(scene, entity, type, value));
  }
  return entity;
}

/* A connection under `source` to `target`. */
static VkrEntityId io_test_connect(IoTest *test, VkrEntityId source,
                                   const char *output, VkrEntityId target,
                                   const char *input, const char *value,
                                   float32_t delay, uint32_t limit) {
  SceneIoConnection connection = {.delay = delay, .limit = limit};
  snprintf(connection.output, sizeof(connection.output), "%s", output);
  snprintf(connection.input, sizeof(connection.input), "%s", input);
  snprintf(connection.value, sizeof(connection.value), "%s", value);
  if (target.u64) {
    assert(vkr_scene_entity_ref(&test->scene, target, &connection.target));
  }
  const VkrEntityId entity = io_test_entity(
      test, "connection", &vkr_scene_io_connection_type, &connection);
  vkr_scene_set_parent(&test->scene, entity, source);
  return entity;
}

static VkrIoEndpoint io_test_input_of(IoTest *test, VkrEntityId entity,
                                      const char *name) {
  VkrIoEndpoint endpoint;
  assert(vkr_io_find_input(
      &test->scene, entity,
      string8_create_from_cstr((const uint8_t *)name, strlen(name)),
      &endpoint));
  return endpoint;
}

/* Connections of one output deliver in creation order, chained deliveries
 * after the ones already waiting, and a value override replaces the
 * fired value. */
static void io_test_order(void) {
  printf("  Running io_test_order...\n");
  IoTest test;
  io_test_begin(&test);
  const VkrEntityId relay =
      io_test_entity(&test, "relay", &vkr_scene_relay_type, NULL);
  const VkrEntityId first = io_test_entity(&test, "first", &s_probe_type, NULL);
  const VkrEntityId second =
      io_test_entity(&test, "second", &s_probe_type, NULL);
  (void)io_test_connect(&test, relay, "on_trigger", first, "record", "1", 0.0f,
                        0u);
  (void)io_test_connect(&test, relay, "on_trigger", second, "record", "2", 0.0f,
                        0u);
  (void)io_test_connect(&test, relay, "relay.on_trigger", first,
                        "io_probe.record", "3", 0.0f, 0u);
  io_test_publish(&test);
  assert(test.router.problems == 0u);
  assert(vkr_io_router_send(&test.router, relay,
                            io_test_input_of(&test, relay, "trigger"), NULL,
                            true_v));
  assert(test.record_count == 0u);
  assert(vkr_io_router_tick(&test.router, &test.scene, 1.0 / 60.0));
  assert(test.record_count == 3u);
  assert(test.records[0].target.u64 == first.u64 && test.records[0].value == 1);
  assert(test.records[1].target.u64 == second.u64 &&
         test.records[1].value == 2);
  assert(test.records[2].value == 3);
  io_test_end(&test);
  printf("  io_test_order PASSED\n");
}

/* A delayed delivery waits for the first tick at or after its deadline,
 * and a limit stops a connection after that many firings. */
static void io_test_delay_and_limit(void) {
  printf("  Running io_test_delay_and_limit...\n");
  IoTest test;
  io_test_begin(&test);
  const VkrEntityId relay =
      io_test_entity(&test, "relay", &vkr_scene_relay_type, NULL);
  const VkrEntityId probe = io_test_entity(&test, "probe", &s_probe_type, NULL);
  (void)io_test_connect(&test, relay, "on_trigger", probe, "record", "7", 0.5f,
                        0u);
  (void)io_test_connect(&test, relay, "on_trigger", probe, "record", "9", 0.0f,
                        1u);
  io_test_publish(&test);
  const VkrIoEndpoint trigger = io_test_input_of(&test, relay, "trigger");
  assert(vkr_io_router_send(&test.router, relay, trigger, NULL, false_v));
  /* The undelayed connection delivers at once, outside a tick. */
  assert(test.record_count == 1u && test.records[0].value == 9);
  assert(vkr_io_router_tick(&test.router, &test.scene, 0.25));
  assert(test.record_count == 1u);
  assert(vkr_io_router_tick(&test.router, &test.scene, 0.5));
  assert(test.record_count == 2u && test.records[1].value == 7);
  /* The second firing skips the spent connection. */
  assert(vkr_io_router_send(&test.router, relay, trigger, NULL, false_v));
  assert(vkr_io_router_tick(&test.router, &test.scene, 1.1));
  assert(test.record_count == 3u && test.records[2].value == 7);
  io_test_end(&test);
  printf("  io_test_delay_and_limit PASSED\n");
}

/* A delivery to a destroyed target is dropped, not delivered elsewhere. */
static void io_test_stale_target(void) {
  printf("  Running io_test_stale_target...\n");
  IoTest test;
  io_test_begin(&test);
  const VkrEntityId relay =
      io_test_entity(&test, "relay", &vkr_scene_relay_type, NULL);
  const VkrEntityId probe = io_test_entity(&test, "probe", &s_probe_type, NULL);
  (void)io_test_connect(&test, relay, "on_trigger", probe, "record", "1", 0.25f,
                        0u);
  io_test_publish(&test);
  assert(vkr_io_router_send(&test.router, relay,
                            io_test_input_of(&test, relay, "trigger"), NULL,
                            false_v));
  vkr_scene_destroy_entity(&test.scene, probe);
  /* A new entity may reuse the slot; the generation keeps them apart. */
  (void)io_test_entity(&test, "newcomer", &s_probe_type, NULL);
  assert(vkr_io_router_tick(&test.router, &test.scene, 0.5));
  assert(test.record_count == 0u && test.router.dropped == 1u);
  io_test_end(&test);
  printf("  io_test_stale_target PASSED\n");
}

/* A relay wired to itself without delay faults with the chain's place
 * instead of looping or dropping deliveries. */
static void io_test_chain_fault(void) {
  printf("  Running io_test_chain_fault...\n");
  IoTest test;
  io_test_begin(&test);
  const VkrEntityId relay =
      io_test_entity(&test, "loop", &vkr_scene_relay_type, NULL);
  (void)io_test_connect(&test, relay, "on_trigger", relay, "trigger", "", 0.0f,
                        0u);
  io_test_publish(&test);
  assert(!vkr_io_router_send(&test.router, relay,
                             io_test_input_of(&test, relay, "trigger"), NULL,
                             false_v));
  assert(test.router.faulted);
  assert(strstr(test.router.error, "chain") &&
         strstr(test.router.error, "loop"));
  /* A faulted router routes nothing more. */
  assert(!vkr_io_router_tick(&test.router, &test.scene, 1.0));
  io_test_end(&test);
  printf("  io_test_chain_fault PASSED\n");
}

/* Counters clamp to their range and announce reaching it; built-in hide
 * reaches any entity. */
static void io_test_counter(void) {
  printf("  Running io_test_counter...\n");
  IoTest test;
  io_test_begin(&test);
  const SceneCounter range = {.start = 0, .min = 0, .max = 2};
  const VkrEntityId counter =
      io_test_entity(&test, "counter", &vkr_scene_counter_type, &range);
  const VkrEntityId probe = io_test_entity(&test, "probe", &s_probe_type, NULL);
  const VkrEntityId lamp = io_test_entity(&test, "lamp", NULL, NULL);
  vkr_scene_set_visibility(&test.scene, lamp, true_v, true_v);
  (void)io_test_connect(&test, counter, "on_changed", probe, "record", "", 0.0f,
                        0u);
  (void)io_test_connect(&test, counter, "on_max", lamp, "hide", "", 0.0f, 0u);
  io_test_publish(&test);
  const VkrIoEndpoint add = io_test_input_of(&test, counter, "add");
  const VkrIoValue one = {.kind = VKR_IO_I32, .i32 = 1};
  for (uint32_t i = 0; i < 3u; ++i) {
    assert(vkr_io_router_send(&test.router, counter, add, &one, false_v));
  }
  /* The third add changes nothing past the maximum. */
  assert(test.record_count == 2u && test.records[0].value == 1 &&
         test.records[1].value == 2);
  const SceneVisibility *visibility = vkr_entity_get_component(
      test.scene.world, lamp, test.scene.comp_visibility);
  assert(visibility && !visibility->visible);
  io_test_end(&test);
  printf("  io_test_counter PASSED\n");
}

/* Timers fire once per interval of simulation time. */
static void io_test_timer(void) {
  printf("  Running io_test_timer...\n");
  IoTest test;
  io_test_begin(&test);
  const SceneTimer every = {.interval = 0.5f, .start_running = true_v};
  const VkrEntityId timer =
      io_test_entity(&test, "timer", &vkr_scene_timer_type, &every);
  const VkrEntityId probe = io_test_entity(&test, "probe", &s_probe_type, NULL);
  (void)io_test_connect(&test, timer, "on_timer", probe, "record", "4", 0.0f,
                        0u);
  io_test_publish(&test);
  for (uint32_t tick = 1; tick <= 60u; ++tick) {
    assert(vkr_io_router_tick(&test.router, &test.scene, tick / 60.0));
  }
  assert(test.record_count == 2u);
  io_test_end(&test);
  printf("  io_test_timer PASSED\n");
}

/* Publication refuses what cannot route and says why; the rest routes. */
static void io_test_problems(void) {
  printf("  Running io_test_problems...\n");
  IoTest test;
  io_test_begin(&test);
  const VkrEntityId relay =
      io_test_entity(&test, "relay", &vkr_scene_relay_type, NULL);
  const VkrEntityId probe = io_test_entity(&test, "probe", &s_probe_type, NULL);
  const VkrEntityId wrong_input =
      io_test_connect(&test, relay, "on_trigger", probe, "open", "", 0.0f, 0u);
  const VkrEntityId no_value = io_test_connect(&test, relay, "on_trigger",
                                               probe, "record", "", 0.0f, 0u);
  const VkrEntityId no_target =
      io_test_connect(&test, relay, "on_trigger", VKR_ENTITY_ID_INVALID,
                      "record", "1", 0.0f, 0u);
  (void)io_test_connect(&test, relay, "on_trigger", probe, "record", "5", 0.0f,
                        0u);
  char error[160];
  assert(vkr_io_connection_problem(&test.scene, wrong_input, error,
                                   sizeof(error)) &&
         strstr(error, "open"));
  assert(
      vkr_io_connection_problem(&test.scene, no_value, error, sizeof(error)) &&
      strstr(error, "needs a value"));
  assert(
      vkr_io_connection_problem(&test.scene, no_target, error, sizeof(error)) &&
      strstr(error, "no target"));
  io_test_publish(&test);
  assert(test.router.problems == 3u);
  assert(vkr_io_router_send(&test.router, relay,
                            io_test_input_of(&test, relay, "trigger"), NULL,
                            false_v));
  assert(test.record_count == 1u && test.records[0].value == 5);
  io_test_end(&test);
  printf("  io_test_problems PASSED\n");
}

bool32_t run_io_tests(void) {
  printf("--- Starting IO Tests ---\n");
  io_test_order();
  io_test_delay_and_limit();
  io_test_stale_target();
  io_test_chain_fault();
  io_test_counter();
  io_test_timer();
  io_test_problems();
  printf("--- IO Tests Completed ---\n");
  return true_v;
}
