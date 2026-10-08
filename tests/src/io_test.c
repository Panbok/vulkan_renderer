#include "io_test.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_types.h"
#include "script/vkr_io_router.h"

#include <assert.h>
#include <math.h>
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

/* A refresh after entities came (as a world partition cell loads) routes
 * their connections and keeps a counter's value: without the carried state
 * the next add would report 1 again. */
static void io_test_refresh(void) {
  printf("  Running io_test_refresh...\n");
  IoTest test;
  io_test_begin(&test);
  const SceneCounter range = {.start = 0, .min = 0, .max = 10};
  const VkrEntityId counter =
      io_test_entity(&test, "counter", &vkr_scene_counter_type, &range);
  const VkrEntityId probe = io_test_entity(&test, "probe", &s_probe_type, NULL);
  (void)io_test_connect(&test, counter, "on_changed", probe, "record", "", 0.0f,
                        0u);
  io_test_publish(&test);
  const VkrIoEndpoint add = io_test_input_of(&test, counter, "add");
  const VkrIoValue one = {.kind = VKR_IO_I32, .i32 = 1};
  assert(vkr_io_router_send(&test.router, counter, add, &one, false_v));
  assert(test.record_count == 1u && test.records[0].value == 1);
  const VkrEntityId later = io_test_entity(&test, "later", &s_probe_type, NULL);
  (void)io_test_connect(&test, counter, "on_changed", later, "record", "", 0.0f,
                        0u);
  assert(vkr_io_router_refresh(&test.router));
  assert(vkr_io_router_send(&test.router, counter, add, &one, false_v));
  assert(test.record_count == 3u && test.records[1].value == 2 &&
         test.records[2].value == 2 && test.records[2].target.u64 == later.u64);
  io_test_end(&test);
  printf("  io_test_refresh PASSED\n");
}

/* The mover's offset from its saved position, read from its evaluated
   pose. */
static float32_t io_test_mover_offset(IoTest *test, VkrEntityId mover,
                                      Vec3 saved) {
  const SceneEvaluatedTransform *pose = vkr_entity_get_component(
      test->scene.world, mover, test->scene.comp_evaluated_transform);
  assert(pose);
  assert(fabsf(pose->world.elements[13] - saved.y) < 1e-6f &&
         fabsf(pose->world.elements[14] - saved.z) < 1e-6f);
  return pose->world.elements[12] - saved.x;
}

/* A mover (ADR-084) 2 m along +X at 1 m/s waiting 1 s. Oracles: the
 * evaluated pose each tick against the travel profile (open over 2 s, rest
 * 1 s, close over 2 s), the outputs a probe records with the tick they
 * reached it, a saved transform that stays bit-identical, and the authored
 * world pose once the router clears. */
static void io_test_mover(void) {
  printf("  Running io_test_mover...\n");
  IoTest test;
  io_test_begin(&test);
  const SceneMover settings = {.direction = vec3_new(1.0f, 0.0f, 0.0f),
                               .distance = 2.0f,
                               .speed = 1.0f,
                               .wait = 1.0f};
  const VkrEntityId mover =
      io_test_entity(&test, "door", &vkr_scene_mover_type, &settings);
  const Vec3 saved = vec3_new(5.0f, 1.0f, -3.0f);
  assert(vkr_scene_set_transform(&test.scene, mover, saved, vkr_quat_identity(),
                                 vec3_one()));
  const VkrEntityId probe = io_test_entity(&test, "probe", &s_probe_type, NULL);
  static const char *const outputs[4] = {"on_open", "on_opened", "on_close",
                                         "on_closed"};
  for (uint32_t i = 0; i < 4u; ++i) {
    char value[4];
    snprintf(value, sizeof(value), "%u", i + 1u);
    (void)io_test_connect(&test, mover, outputs[i], probe, "record", value,
                          0.0f, 0u);
  }
  vkr_scene_update_transforms(&test.scene);
  const SceneTransform authored = *vkr_scene_get_transform(&test.scene, mover);
  io_test_publish(&test);
  assert(test.router.problems == 0u);
  assert(io_test_mover_offset(&test, mover, saved) == 0.0f);

  assert(vkr_io_router_send(&test.router, mover,
                            io_test_input_of(&test, mover, "open"), NULL,
                            false_v));
  assert(test.record_count == 1u && test.records[0].value == 1);
  /* The tick each output reached the probe at. */
  uint32_t reached[5] = {0};
  const float64_t dt = 1.0 / 60.0;
  for (uint32_t tick = 1; tick <= 330u; ++tick) {
    const uint32_t before = test.record_count;
    assert(vkr_io_router_step(&test.router, dt));
    assert(vkr_io_router_tick(&test.router, &test.scene, tick * dt));
    for (uint32_t r = before; r < test.record_count; ++r) {
      const int32_t value = test.records[r].value;
      assert(value >= 1 && value <= 4 && !reached[value]);
      reached[value] = tick;
    }
    const float32_t t = (float32_t)tick / 60.0f;
    const float32_t expected = tick <= 120u   ? t
                               : tick <= 180u ? 2.0f
                               : tick <= 300u ? 2.0f - (t - 3.0f)
                                              : 0.0f;
    assert(fabsf(io_test_mover_offset(&test, mover, saved) - expected) < 1e-5f);
    const SceneTransform *now = vkr_scene_get_transform(&test.scene, mover);
    assert(
        MemCompare(&now->position, &authored.position,
                   sizeof(authored.position)) == 0 &&
        MemCompare(&now->rotation, &authored.rotation,
                   sizeof(authored.rotation)) == 0 &&
        MemCompare(&now->scale, &authored.scale, sizeof(authored.scale)) == 0 &&
        MemCompare(&now->local, &authored.local, sizeof(authored.local)) == 0);
  }
  /* on_opened at 2 s, on_close when the wait ends at 3 s, on_closed after
     the travel back at 5 s; each once. */
  assert(test.record_count == 4u);
  assert(reached[2] == 120u && reached[3] == 180u && reached[4] == 300u);

  vkr_io_router_clear(&test.router);
  assert(!vkr_entity_has_component(test.scene.world, mover,
                                   test.scene.comp_evaluated_transform));
  vkr_scene_update_transforms(&test.scene);
  assert(MemCompare(&vkr_scene_get_transform(&test.scene, mover)->world,
                    &authored.world, sizeof(authored.world)) == 0);
  io_test_end(&test);
  printf("  io_test_mover PASSED\n");
}

/* The evaluated world position of `mover`. */
static Vec3 io_test_mover_position(IoTest *test, VkrEntityId mover) {
  const SceneEvaluatedTransform *pose = vkr_entity_get_component(
      test->scene.world, mover, test->scene.comp_evaluated_transform);
  assert(pose);
  return vec3_new(pose->world.elements[12], pose->world.elements[13],
                  pose->world.elements[14]);
}

/* A turning mover (ADR-084): a door 90 degrees about +Y through a hinge
 * 1 m along its +X at 45 degrees per second, and a fan spinning at 90.
 * Oracles: the door's origin swings about the hinge from (5, 1, -3) to
 * (6, 1, -2), at 45 degrees after 1 s and open after 2 s when on_opened
 * reaches the probe; the fan is back at rest after one 4 s turn without
 * on_opened, and a close stops it where it is. */
static void io_test_mover_turn(void) {
  printf("  Running io_test_mover_turn...\n");
  IoTest test;
  io_test_begin(&test);
  const SceneMover door_settings = {.angle = 90.0f,
                                    .axis = vec3_new(0.0f, 1.0f, 0.0f),
                                    .pivot = vec3_new(1.0f, 0.0f, 0.0f),
                                    .speed = 45.0f,
                                    .wait = -1.0f};
  const VkrEntityId door =
      io_test_entity(&test, "door", &vkr_scene_mover_type, &door_settings);
  const SceneMover fan_settings = {.axis = vec3_new(0.0f, 1.0f, 0.0f),
                                   .pivot = vec3_new(1.0f, 0.0f, 0.0f),
                                   .spin = true_v,
                                   .speed = 90.0f,
                                   .wait = -1.0f};
  const VkrEntityId fan =
      io_test_entity(&test, "fan", &vkr_scene_mover_type, &fan_settings);
  const Vec3 saved = vec3_new(5.0f, 1.0f, -3.0f);
  assert(vkr_scene_set_transform(&test.scene, door, saved, vkr_quat_identity(),
                                 vec3_one()));
  assert(vkr_scene_set_transform(&test.scene, fan, saved, vkr_quat_identity(),
                                 vec3_one()));
  const VkrEntityId probe = io_test_entity(&test, "probe", &s_probe_type, NULL);
  (void)io_test_connect(&test, door, "on_opened", probe, "record", "1", 0.0f,
                        0u);
  (void)io_test_connect(&test, fan, "on_opened", probe, "record", "2", 0.0f,
                        0u);
  vkr_scene_update_transforms(&test.scene);
  io_test_publish(&test);
  assert(test.router.problems == 0u);
  assert(vkr_io_router_send(&test.router, door,
                            io_test_input_of(&test, door, "open"), NULL,
                            false_v));
  assert(vkr_io_router_send(
      &test.router, fan, io_test_input_of(&test, fan, "open"), NULL, false_v));
  const float64_t dt = 1.0 / 60.0;
  const float32_t half = 0.70710678f;
  for (uint32_t tick = 1; tick <= 240u; ++tick) {
    assert(vkr_io_router_step(&test.router, dt));
    assert(vkr_io_router_tick(&test.router, &test.scene, tick * dt));
    const Vec3 at = io_test_mover_position(&test, door);
    if (tick == 60u) {
      assert(fabsf(at.x - (6.0f - half)) < 1e-4f &&
             fabsf(at.z - (-3.0f + half)) < 1e-4f);
    }
    if (tick == 119u) {
      assert(test.record_count == 0u);
    }
    if (tick == 120u) {
      assert(test.record_count == 1u && test.records[0].value == 1);
      assert(fabsf(at.x - 6.0f) < 1e-4f && fabsf(at.y - 1.0f) < 1e-4f &&
             fabsf(at.z + 2.0f) < 1e-4f);
    }
  }
  /* One whole turn of the fan, and no on_opened from it. */
  const Vec3 turned = io_test_mover_position(&test, fan);
  assert(vec3_length(vec3_sub(turned, saved)) < 1e-3f);
  assert(test.record_count == 1u);

  assert(vkr_io_router_step(&test.router, 30.0 * dt));
  assert(vkr_io_router_send(
      &test.router, fan, io_test_input_of(&test, fan, "close"), NULL, false_v));
  const Vec3 stopped = io_test_mover_position(&test, fan);
  assert(vec3_length(vec3_sub(stopped, saved)) > 0.5f);
  for (uint32_t tick = 0; tick < 30u; ++tick) {
    assert(vkr_io_router_step(&test.router, dt));
  }
  assert(vec3_length(vec3_sub(io_test_mover_position(&test, fan), stopped)) <
         1e-6f);
  io_test_end(&test);
  printf("  io_test_mover_turn PASSED\n");
}

bool32_t run_io_tests(void) {
  printf("--- Starting IO Tests ---\n");
  io_test_order();
  io_test_delay_and_limit();
  io_test_stale_target();
  io_test_chain_fault();
  io_test_counter();
  io_test_refresh();
  io_test_timer();
  io_test_problems();
  io_test_mover();
  io_test_mover_turn();
  printf("--- IO Tests Completed ---\n");
  return true_v;
}
