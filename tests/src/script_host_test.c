#include "script_host_test.h"

#include "core/vkr_job_system.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_types.h"
#include "script/vkr_script_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Registered component types outlive every host and scene of the process, so
   this suite registers each name once, from storage that lives as long. */
static VkrDMemory s_memory;
static VkrAllocator s_allocator;

static void script_test_frame(VkrScriptHost *host, VkrScene *scene) {
  VkrScriptFrame frame = {.now = 1.0,
                          .scene_delta = 0.0,
                          .simulation_running = true_v,
                          .camera_available = true_v};
  vkr_script_host_frame(host, &frame);
  vkr_scene_update(scene, frame.scene_delta);
  VkrScriptView view;
  vkr_script_host_present(host, &frame, &view);
}

// =============================================================================
// Lifecycle: start, ledger release, ticks, faults and restart
// =============================================================================

typedef enum LifeMode { LIFE_FAIL, LIFE_IDLE, LIFE_ACTIVE } LifeMode;

typedef struct LifeData {
  uint32_t starts;
  uint32_t fixed;
  uint32_t late_fixed;
  uint64_t last_tick;
} LifeData;

static LifeMode s_life_mode;
static LifeData *s_life;
static VkrEntity s_life_spawned;
static uint32_t s_life_stops;
static bool8_t s_life_deferred_spawn;
static bool8_t s_life_temp_zeroed;

static void life_start(VkrCtx *ctx, LifeData *data) {
  assert(data->starts == 0 && data->fixed == 0); // Zeroed for every start.
  data->starts++;
  s_life = data;
  /* Persistent: released when the instance ends, or by a failed start. */
  s_life_spawned = vkr_spawn(ctx, &(VkrSpawnDesc){.name = "LifeMarker"});
  assert(vkr_entity_valid(s_life_spawned));
  if (s_life_mode == LIFE_FAIL) {
    vkr_fail(ctx, "refused %u", 7u);
  } else if (s_life_mode == LIFE_IDLE) {
    vkr_disable(ctx);
  }
}

static void life_stop(VkrCtx *ctx, LifeData *data) {
  (void)ctx;
  (void)data;
  s_life_stops++;
}

static void life_update(VkrCtx *ctx, LifeData *data, float32_t dt) {
  (void)data;
  (void)dt;
  uint32_t *scratch = vkr_temp(ctx, uint32_t, 64);
  s_life_temp_zeroed = scratch && scratch[0] == 0u && scratch[63] == 0u;
  vkr_set_time_step(ctx, 2.0 * vkr_fixed_dt(ctx));
}

static void life_fixed_update(VkrCtx *ctx, LifeData *data) {
  data->fixed++;
  data->last_tick = vkr_ticks(ctx) + 1u;
  /* A spawn in a tick is pending: alive, with no transform until the tick
     ends. */
  if (data->fixed == 1u) {
    Mat4 world;
    const VkrEntity spawned = vkr_spawn(ctx, &(VkrSpawnDesc){0});
    s_life_deferred_spawn = vkr_entity_valid(spawned) &&
                            vkr_alive(ctx, spawned) &&
                            !vkr_world_matrix(ctx, spawned, &world);
  }
}

static void life_late_fixed_update(VkrCtx *ctx, LifeData *data) {
  data->late_fixed++;
  if (data->last_tick == 3u) {
    vkr_fail(ctx, "tick three");
  }
}

VKR_MODULE(life, LifeData, , .start = life_start, .stop = life_stop,
           .update = life_update, .fixed_update = life_fixed_update,
           .late_fixed_update = life_late_fixed_update)

static const VkrModuleDesc *life_old_entry(uint32_t sdk_version) {
  return vkr_module_life(sdk_version + 1u);
}

static void test_script_host_lifecycle(VkrScriptHost *host) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, &s_allocator, 49, 16, NULL));
  InputState input = {0};
  const VkrScriptSessionDesc session = {.active = &scene, .input = &input};
  const char *error = NULL;

  // A failed start releases what it acquired and leaves no callbacks.
  s_life_mode = LIFE_FAIL;
  assert(!vkr_script_host_start(host, &session, &error));
  assert(error && strstr(error, "life: refused 7"));
  assert(!host->started && !scene.simulation.enabled);
  assert(
      !vkr_scene_entity_alive(&scene, (VkrEntityId){.u64 = s_life_spawned.id}));
  assert(s_life_stops == 1u);

  // A disabled instance receives no hooks, and the scene keeps no callbacks.
  s_life_mode = LIFE_IDLE;
  assert(vkr_script_host_start(host, &session, &error));
  assert(host->started && !vkr_script_host_active(host));
  assert(!scene.simulation.enabled);
  vkr_script_host_stop(host);
  assert(s_life_stops == 2u && !host->started);

  // An active instance runs on the shared clock; its update sets the elapsed
  // time and its temp memory rewinds after the hook.
  s_life_mode = LIFE_ACTIVE;
  assert(vkr_script_host_start(host, &session, &error));
  assert(vkr_script_host_active(host) && scene.simulation.enabled);
  const VkrEntityId marker = {.u64 = s_life_spawned.id};
  assert(vkr_scene_entity_alive(&scene, marker));
  vkr_scene_physics_set_paused(&scene, false_v);
  const uint64_t temp = arena_pos(host->temp);
  script_test_frame(host, &scene);
  assert(arena_pos(host->temp) == temp && s_life_temp_zeroed);
  assert(s_life->fixed == 2u && s_life->late_fixed == 2u);
  assert(s_life->last_tick == 2u && s_life_deferred_spawn);

  // A hook failure faults the simulation with the module's name.
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(scene.simulation.faulted && s_life->late_fixed == 3u);
  assert(strstr(scene.simulation.error_storage, "life: tick three"));

  // A native reset restarts the instances at the next frame on fresh data;
  // the old instance's marker is released and a new one spawned.
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_reset(&scene, NULL));
  assert(host->restart_pending);
  VkrScriptFrame frame = {.now = 2.0};
  vkr_script_host_frame(host, &frame);
  assert(!host->restart_pending && s_life_stops == 3u);
  assert(s_life->starts == 1u && s_life->fixed == 0u);
  assert(!vkr_scene_entity_alive(&scene, marker));
  const VkrEntityId restarted = {.u64 = s_life_spawned.id};
  assert(vkr_scene_entity_alive(&scene, restarted));

  // Stop releases the instance's acquisitions.
  vkr_script_host_stop(host);
  assert(s_life_stops == 4u && !scene.simulation.enabled);
  assert(!vkr_scene_entity_alive(&scene, restarted));
  vkr_scene_shutdown(&scene, NULL);
}

// =============================================================================
// Behaviors: per-entity hooks and scoped resources
// =============================================================================

typedef struct SpinData {
  uint32_t unused;
} SpinData;

static uint32_t s_spin_starts;
static uint32_t s_spin_stops;
static uint32_t s_spin_updates;
static uint32_t s_spin_destroys;
static VkrEntity s_spin_child;
/* What the last destroy hook saw: the component's speed and whether the
   child its start spawned still lived. */
static float32_t s_spin_destroy_speed;
static bool8_t s_spin_destroy_child_alive;
static uint32_t s_spin_stops_at_destroy;

#define SPIN_FIELDS VKR_FIELD(F32, speed, "Speed", 0.5f, .unit = "turns/s")
VKR_COMPONENT(Spin, spin, "Spin", SPIN_FIELDS)

static void spin_start(VkrCtx *ctx, VkrEntity self, Spin *spin) {
  assert(spin && spin->speed == 0.5f);
  s_spin_starts++;
  /* Scoped to this entity's behavior. */
  s_spin_child =
      vkr_spawn(ctx, &(VkrSpawnDesc){.name = "SpinChild", .parent = self});
  assert(vkr_entity_valid(s_spin_child));
}

static void spin_update(VkrCtx *ctx, VkrEntity self, Spin *spin, float32_t dt) {
  (void)ctx;
  (void)self;
  (void)dt;
  spin->speed += 1.0f;
  s_spin_updates++;
}

static void spin_stop(VkrCtx *ctx, VkrEntity self, Spin *spin) {
  (void)ctx;
  (void)self;
  (void)spin;
  s_spin_stops++;
}

static void spin_destroy(VkrCtx *ctx, VkrEntity self, Spin *spin) {
  assert(vkr_alive(ctx, self) && spin);
  s_spin_destroys++;
  s_spin_destroy_speed = spin->speed;
  s_spin_destroy_child_alive = vkr_alive(ctx, s_spin_child);
  s_spin_stops_at_destroy = s_spin_stops;
}

VKR_BEHAVIOR(spin, .start = spin_start, .update = spin_update,
             .stop = spin_stop, .destroy = spin_destroy)
VKR_MODULE(spinner, SpinData, VKR_EXPORT_BEHAVIOR(spin))

static void test_script_behaviors(VkrScriptHost *host) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, &s_allocator, 53, 16, NULL));
  InputState input = {0};
  const VkrTypeDesc *type = vkr_scene_world_type_named(string8_lit("spin"));
  assert(type);
  Spin value;
  type->defaults(&value);
  const VkrEntityId first = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, first, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  assert(vkr_scene_set_typed(&scene, first, type, &value));

  // Entities carrying the component start with the session.
  const VkrScriptSessionDesc session = {.active = &scene, .input = &input};
  const char *error = NULL;
  s_life_mode = LIFE_IDLE;
  assert(vkr_script_host_start(host, &session, &error));
  assert(vkr_script_host_active(host) && s_spin_starts == 1u);
  const VkrEntityId first_child = {.u64 = s_spin_child.id};
  assert(vkr_scene_entity_alive(&scene, first_child));
  script_test_frame(host, &scene);
  assert(s_spin_updates == 1u);
  assert(((const Spin *)vkr_scene_get_typed(&scene, first, type))->speed ==
         1.5f);

  // An entity that gains the component starts at the next frame.
  const VkrEntityId second = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, second, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  assert(vkr_scene_set_typed(&scene, second, type, &value));
  script_test_frame(host, &scene);
  assert(s_spin_starts == 2u && s_spin_updates == 3u);
  const VkrEntityId second_child = {.u64 = s_spin_child.id};

  // Losing the component stops the behavior and releases its scope only.
  assert(vkr_scene_remove_typed(&scene, first, type));
  script_test_frame(host, &scene);
  assert(s_spin_stops == 1u && s_spin_updates == 4u);
  assert(!vkr_scene_entity_alive(&scene, first_child));
  assert(vkr_scene_entity_alive(&scene, second_child));

  // Destroying an entity, as the editor's delete does, runs destroy while
  // the component and the behavior's spawn still exist, then stop, then
  // releases the scope.
  const VkrEntityId third = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, third, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  assert(vkr_scene_set_typed(&scene, third, type, &value));
  script_test_frame(host, &scene);
  assert(s_spin_starts == 3u);
  const VkrEntityId third_child = {.u64 = s_spin_child.id};
  vkr_scene_destroy_entity(&scene, third);
  assert(s_spin_destroys == 1u && s_spin_destroy_speed == 1.5f);
  assert(s_spin_destroy_child_alive && s_spin_stops_at_destroy == 1u);
  assert(s_spin_stops == 2u);
  assert(!vkr_scene_entity_alive(&scene, third));
  assert(!vkr_scene_entity_alive(&scene, third_child));
  assert(vkr_scene_entity_alive(&scene, second_child));
  script_test_frame(host, &scene);
  assert(s_spin_stops == 2u && s_spin_destroys == 1u);

  // Stop ends the remaining behaviors without destroy hooks.
  vkr_script_host_stop(host);
  assert(s_spin_stops == 3u && s_spin_destroys == 1u);
  assert(!vkr_scene_entity_alive(&scene, second_child));
  assert(!scene.destroy_observer);
  vkr_scene_shutdown(&scene, NULL);
}

// =============================================================================
// Containers: World and zone instances
// =============================================================================

typedef struct ZoneData {
  VkrEntity spawned;
} ZoneData;

static uint32_t s_zone_starts;
static uint32_t s_game_starts;
static VkrEntity s_zone_spawned[2];
static VkrEntity s_game_spawned;

static void zone_start(VkrCtx *ctx, ZoneData *data) {
  data->spawned = vkr_spawn(ctx, &(VkrSpawnDesc){.name = "ZoneMarker"});
  assert(vkr_container_of(ctx, data->spawned).id == vkr_container_self(ctx).id);
  /* The module stays registered for later suites; keep only the starts the
     container test reads. */
  if (s_zone_starts < ArrayCount(s_zone_spawned)) {
    s_zone_spawned[s_zone_starts] = data->spawned;
  }
  s_zone_starts++;
}

static void game_start(VkrCtx *ctx, ZoneData *data) {
  assert(vkr_container_self(ctx).id == vkr_container_world(ctx).id);
  data->spawned = vkr_spawn(ctx, &(VkrSpawnDesc){.name = "GameMarker"});
  s_game_spawned = data->spawned;
  s_game_starts++;
}

VKR_MODULE(zone, ZoneData, , .start = zone_start)
VKR_MODULE(game, ZoneData, , .scope = VKR_SCOPE_WORLD, .start = game_start)

static VkrScene *test_scene_holding(VkrScene *a, VkrScene *b, VkrEntity e) {
  const VkrEntityId id = {.u64 = e.id};
  return vkr_scene_entity_alive(a, id)   ? a
         : vkr_scene_entity_alive(b, id) ? b
                                         : NULL;
}

static void test_script_containers(VkrScriptHost *host) {
  VkrScene world;
  VkrScene scene;
  assert(
      vkr_scene_init(&world, &s_allocator, VKR_SCENE_WORLD_ROOT_ID, 16, NULL));
  assert(vkr_scene_init(&scene, &s_allocator, 0, 16, NULL));
  InputState input = {0};
  const VkrScriptSessionDesc session = {
      .active = &scene, .world = &world, .input = &input};
  const char *error = NULL;
  s_life_mode = LIFE_IDLE;
  assert(vkr_script_host_start(host, &session, &error));

  // A container-scoped module runs on the World and on the played scene; a
  // World-scoped one runs once, on the World.
  assert(s_zone_starts == 2u && s_game_starts == 1u);
  assert(test_scene_holding(&world, &scene, s_zone_spawned[0]) == &world);
  assert(test_scene_holding(&world, &scene, s_zone_spawned[1]) == &scene);
  assert(test_scene_holding(&world, &scene, s_game_spawned) == &world);
  assert(vkr_script_host_instance_data(host, "zone", &scene));
  assert(!vkr_script_host_instance_data(host, "game", &scene));

  // Unloading the World ends its instances and releases what they spawned;
  // the scene's instance lives on.
  vkr_script_host_detach(host, &world);
  assert(!test_scene_holding(&world, &scene, s_zone_spawned[0]));
  assert(!test_scene_holding(&world, &scene, s_game_spawned));
  assert(test_scene_holding(&world, &scene, s_zone_spawned[1]) == &scene);
  assert(host->started && host->container_count == 1u);

  vkr_script_host_stop(host);
  assert(!test_scene_holding(&world, &scene, s_zone_spawned[1]));
  vkr_scene_shutdown(&scene, NULL);
  vkr_scene_shutdown(&world, NULL);
}

// =============================================================================
// Deferred edits and lifetimes
// =============================================================================

typedef struct BurstData {
  uint32_t ticks;
} BurstData;

static VkrEntity s_burst_shot;
static VkrEntity s_burst_trail;
static VkrEntity s_burst_victim;
static bool8_t s_burst_pending_ok;
static bool8_t s_burst_victim_alive_in_tick;

static void burst_fixed_update(VkrCtx *ctx, BurstData *data) {
  data->ticks++;
  if (data->ticks == 1u) {
    /* A shot that lives 0.1 simulated seconds, and a trail it owns. */
    s_burst_shot =
        vkr_spawn(ctx, &(VkrSpawnDesc){.name = "Shot", .lifetime = 0.1f});
    s_burst_trail = vkr_spawn(
        ctx, &(VkrSpawnDesc){.name = "ShotTrail", .owner = s_burst_shot});
    BurstData *state =
        vkr_state_add(ctx, s_burst_shot, VKR_STATE_TYPE(ctx, BurstData), NULL);
    if (state) {
      state->ticks = 42u;
    }
    Mat4 world;
    s_burst_pending_ok =
        state && vkr_alive(ctx, s_burst_shot) &&
        !vkr_world_matrix(ctx, s_burst_shot, &world) &&
        vkr_set_transform(ctx, s_burst_shot,
                          &(VkrTRS){.position = vec3_new(1, 2, 3)});
  } else if (data->ticks == 2u) {
    vkr_destroy(ctx, s_burst_victim);
    s_burst_victim_alive_in_tick = vkr_alive(ctx, s_burst_victim);
  }
}

VKR_MODULE(burst, BurstData, , .fixed_update = burst_fixed_update)

static void test_script_deferred(VkrScriptHost *host) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, &s_allocator, 54, 16, NULL));
  InputState input = {0};
  const VkrEntityId victim = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, victim, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  s_burst_victim = (VkrEntity){.id = victim.u64};
  const VkrScriptSessionDesc session = {.active = &scene, .input = &input};
  const char *error = NULL;
  s_life_mode = LIFE_IDLE;
  assert(vkr_script_host_start(host, &session, &error));
  vkr_scene_physics_set_paused(&scene, false_v);

  // A spawn in a fixed update reads as alive at once; it and the calls on it
  // apply right after the tick.
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(vkr_scene_simulation_completed_ticks(&scene) == 1u);
  assert(s_burst_pending_ok && !scene.simulation.faulted);
  const VkrEntityId shot = {.u64 = s_burst_shot.id};
  const VkrEntityId trail = {.u64 = s_burst_trail.id};
  assert(vkr_scene_entity_alive(&scene, shot));
  assert(vkr_scene_entity_alive(&scene, trail));
  assert(vkr_scene_entity_transient(&scene, shot));
  const SceneTransform *transform = vkr_scene_get_transform(&scene, shot);
  assert(transform && transform->position.y == 2.0f);
  const VkrComponentTypeId burst_state =
      vkr_entity_find_component(scene.world, "BurstData");
  const BurstData *state =
      vkr_entity_get_component(scene.world, shot, burst_state);
  assert(state && state->ticks == 42u);

  // A destroy in a fixed update applies after the tick.
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(s_burst_victim_alive_in_tick);
  assert(!vkr_scene_entity_alive(&scene, victim));

  // The shot expires on the simulated clock and takes the trail it owns.
  for (uint32_t i = 0; i < 4u; ++i) {
    vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  }
  assert(vkr_scene_entity_alive(&scene, shot));
  for (uint32_t i = 0; i < 4u; ++i) {
    vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  }
  assert(!vkr_scene_entity_alive(&scene, shot));
  assert(!vkr_scene_entity_alive(&scene, trail));
  vkr_script_host_stop(host);
  vkr_scene_shutdown(&scene, NULL);
}

// =============================================================================
// Tasks
// =============================================================================

typedef struct CrunchWork {
  uint64_t count;
  uint64_t sum;
  VkrThreadId thread;
} CrunchWork;

typedef struct CrunchData {
  VkrTask sum;
  VkrTask slow;
  CrunchWork result;
  CrunchWork waited;
  bool8_t taken;
} CrunchData;

static VkrAtomicBool s_crunch_slow_done;

static void crunch_sum(void *data) {
  CrunchWork *work = data;
  work->thread = vkr_thread_current_id();
  for (uint64_t i = 1; i <= work->count; ++i) {
    work->sum += i;
  }
}

/* Touches only a flag nothing else writes until the task ends. */
static void crunch_slow(void *data) {
  (void)data;
  vkr_thread_sleep(50u);
  vkr_atomic_bool_store(&s_crunch_slow_done, true_v, VKR_MEMORY_ORDER_RELEASE);
}

static void crunch_start(VkrCtx *ctx, CrunchData *data) {
  data->sum = vkr_task_run(ctx, crunch_sum, &(CrunchWork){.count = 100000u},
                           sizeof(CrunchWork));
  /* Never taken: Stop waits for it. */
  data->slow = vkr_task_run(ctx, crunch_slow, NULL, 0u);
  /* A task larger than the limit does not start. */
  if (vkr_task_run(ctx, crunch_sum, &data->result, VKR_TASK_DATA_MAX + 1u).id) {
    vkr_fail(ctx, "oversized task started");
  }
}

static void crunch_fixed_update(VkrCtx *ctx, CrunchData *data) {
  if (!data->taken) {
    data->taken =
        vkr_task_take(ctx, data->sum, &data->result, sizeof(data->result));
  }
  if (!data->waited.count) {
    /* Fan out and join inside one tick. */
    const VkrTask task = vkr_task_run(
        ctx, crunch_sum, &(CrunchWork){.count = 10u}, sizeof(CrunchWork));
    if (!vkr_task_wait(ctx, task, &data->waited, sizeof(data->waited)) ||
        vkr_task_take(ctx, task, NULL, 0u)) {
      vkr_fail(ctx, "a waited task was not taken exactly once");
    }
  }
}

VKR_MODULE(crunch, CrunchData, , .start = crunch_start,
           .fixed_update = crunch_fixed_update)

static void test_script_tasks(VkrScriptHost *host) {
  VkrJobSystem jobs = {0};
  VkrJobSystemConfig config = vkr_job_system_config_default();
  config.worker_count = 2u;
  config.max_jobs = 32u;
  config.queue_capacity = 32u;
  assert(vkr_job_system_init(&config, &jobs));
  VkrScene scene;
  assert(vkr_scene_init(&scene, &s_allocator, 56, 16, NULL));
  InputState input = {0};
  const VkrScriptSessionDesc session = {
      .active = &scene, .input = &input, .jobs = &jobs};
  const char *error = NULL;
  s_life_mode = LIFE_IDLE;
  vkr_atomic_bool_store(&s_crunch_slow_done, false_v, VKR_MEMORY_ORDER_RELAXED);
  assert(vkr_script_host_start(host, &session, &error));
  const CrunchData *data =
      vkr_script_host_instance_data(host, "crunch", &scene);
  assert(data && data->sum.id && data->slow.id);
  vkr_scene_physics_set_paused(&scene, false_v);

  // The sum runs on a worker while ticks continue; a tick takes it once it
  // has finished, and a task waited for inside a tick is taken there.
  for (uint32_t i = 0; i < 200u && !data->taken; ++i) {
    vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
    if (!data->taken) {
      vkr_thread_sleep(1u);
    }
  }
  assert(!scene.simulation.faulted);
  assert(data->taken && data->result.sum == 5000050000ull);
  assert(data->result.thread != vkr_thread_current_id());
  assert(data->waited.sum == 55u);

  // Stop waits for the task nothing took and frees every task.
  vkr_script_host_stop(host);
  assert(vkr_atomic_bool_load(&s_crunch_slow_done, VKR_MEMORY_ORDER_ACQUIRE));
  assert(host->task_count == 0u);

  // Without workers, a task runs when it is created, on this thread.
  const VkrScriptSessionDesc inline_session = {.active = &scene,
                                               .input = &input};
  assert(vkr_script_host_start(host, &inline_session, &error));
  data = vkr_script_host_instance_data(host, "crunch", &scene);
  vkr_scene_physics_set_paused(&scene, false_v);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(data->taken && data->result.thread == vkr_thread_current_id());
  vkr_script_host_stop(host);
  vkr_scene_shutdown(&scene, NULL);
  vkr_job_system_shutdown(&jobs);
}

// =============================================================================
// Engine queries through a tool context
// =============================================================================

static void test_player_start_resolution(void) {
  VkrScene world;
  VkrScene scene;
  assert(
      vkr_scene_init(&world, &s_allocator, VKR_SCENE_WORLD_ROOT_ID, 16, NULL));
  assert(vkr_scene_init(&scene, &s_allocator, 50, 16, NULL));
  vkr_scene_set_world_fallback(&scene, &world);
  Mat4 pose;
  assert(!vkr_scene_player_start(&scene, &pose));

  // The World's start applies while the scene has no enabled one.
  const ScenePlayerStart enabled = {.enabled = true_v};
  const ScenePlayerStart disabled = {.enabled = false_v};
  const VkrEntityId shared = vkr_scene_create_entity(&world, NULL);
  assert(vkr_scene_set_transform(&world, shared, vec3_new(7, 0, 0),
                                 vkr_quat_identity(), vec3_one()));
  assert(vkr_scene_set_typed(&world, shared, &vkr_scene_player_start_type,
                             &enabled));
  const VkrEntityId skipped = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_typed(&scene, skipped, &vkr_scene_player_start_type,
                             &disabled));
  vkr_scene_update_transforms(&world);
  assert(vkr_scene_player_start(&scene, &pose));
  assert(pose.elements[12] == 7);

  // The scene's own enabled start wins.
  const VkrEntityId own = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, own, vec3_new(1, 2, 3),
                                 vkr_quat_identity(), vec3_one()));
  assert(
      vkr_scene_set_typed(&scene, own, &vkr_scene_player_start_type, &enabled));
  vkr_scene_update_transforms(&scene);
  assert(vkr_scene_player_start(&scene, &pose));
  assert(pose.elements[12] == 1 && pose.elements[13] == 2 &&
         pose.elements[14] == 3);
  VkrEntityId found[1];
  assert(vkr_scene_find_typed(&scene, &vkr_scene_player_start_type, found,
                              ArrayCount(found)) == 2);
  assert(found[0].u64 == skipped.u64);
  vkr_scene_shutdown(&scene, NULL);
  vkr_scene_shutdown(&world, NULL);
}

/* A placed model is the player's body when its entity or a descendant
   carries a mesh or a shape; an empty entity is not. */
static void test_has_visual(VkrScriptHost *host) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, &s_allocator, 52, 16, NULL));
  const VkrEntityId root = vkr_scene_create_entity(&scene, NULL);
  const VkrEntityId node = vkr_scene_create_entity(&scene, NULL);
  const VkrEntityId empty = vkr_scene_create_entity(&scene, NULL);
  const VkrEntityId cube = vkr_scene_create_entity(&scene, NULL);
  const VkrEntityId entities[] = {root, node, empty, cube};
  for (uint32_t i = 0; i < ArrayCount(entities); ++i) {
    assert(vkr_scene_set_transform(&scene, entities[i], vec3_zero(),
                                   vkr_quat_identity(), vec3_one()));
  }
  vkr_scene_set_parent(&scene, node, root);
  assert(vkr_scene_set_mesh_renderer(
      &scene, node, (VkrMeshInstanceHandle){.id = 1, .generation = 1}));
  const SceneShape shape = {.type = SCENE_SHAPE_TYPE_CUBE,
                            .dimensions = vec3_one(),
                            .mesh_index = VKR_INVALID_ID};
  assert(vkr_entity_add_component(scene.world, cube, scene.comp_shape, &shape));
  VkrCtx *ctx = vkr_script_host_open_context(host, &scene, NULL, NULL);
  assert(ctx);
  assert(vkr_has_visual(ctx, (VkrEntity){.id = root.u64}));
  assert(vkr_has_visual(ctx, (VkrEntity){.id = cube.u64}));
  assert(vkr_has_visual(ctx, (VkrEntity){.id = node.u64}));
  assert(!vkr_has_visual(ctx, (VkrEntity){.id = empty.u64}));
  assert(!vkr_has_visual(ctx, VKR_ENTITY_NONE));

  // A tool context's acquisitions are released when it closes.
  const VkrEntity spawned =
      vkr_spawn(ctx, &(VkrSpawnDesc){.parent = {root.u64}});
  assert(vkr_alive(ctx, spawned));
  vkr_destroy(ctx, (VkrEntity){.id = root.u64});
  assert(!vkr_alive(ctx, spawned)); // Destroying a parent takes its children.
  vkr_script_host_close_context(host);
  vkr_scene_shutdown(&scene, NULL);
}

// =============================================================================
// Authoring macros
// =============================================================================

/* The descriptors the macros generate: member offsets, kinds, options,
   defaults, behaviors and the module description. */
static const char *const s_macro_modes[] = {"Walk", "Run", NULL};

#define MACRO_GATE_FIELDS                                                      \
  VKR_FIELD(F32, speed, "Speed", 0.25f, .unit = "turns/s", .min = -10.0f,      \
            .max = 10.0f)                                                      \
  VKR_FIELD(BOOL, locked, "Locked", false_v)                                   \
  VKR_FIELD(U32, count, "Count", 3u)                                           \
  VKR_FIELD(I32, offset, "Offset", -2)                                         \
  VKR_FIELD(VEC3, axis, "Axis", vec3_new(0.0f, 1.0f, 0.0f))                    \
  VKR_FIELD(ENUM, mode, "Mode", 1, .names = s_macro_modes)
VKR_COMPONENT(MacroGate, macro_gate, "Macro gate", MACRO_GATE_FIELDS)

#define MACRO_LAMP_FIELDS VKR_FIELD(F32, glow, "Glow", 1.0f)
VKR_COMPONENT(MacroLamp, macro_lamp, "Macro lamp", MACRO_LAMP_FIELDS)

typedef struct MacroData {
  uint32_t ticks;
} MacroData;

static void macro_lamp_update(VkrCtx *ctx, VkrEntity self, MacroLamp *lamp,
                              float32_t dt) {
  (void)ctx;
  (void)self;
  (void)lamp;
  (void)dt;
}

static void macro_start(VkrCtx *ctx, MacroData *data) {
  (void)ctx;
  (void)data;
}

VKR_BEHAVIOR(macro_lamp, .update = macro_lamp_update)
VKR_MODULE(MacroProbe, MacroData,
           VKR_EXPORT_COMPONENT(macro_gate) VKR_EXPORT_BEHAVIOR(macro_lamp),
           .data_version = 3, .start = macro_start)

static void test_script_authoring_macros(void) {
  const VkrModuleDesc *desc = vkr_module_MacroProbe(VKR_SDK_VERSION);
  assert(desc && !strcmp(desc->name, "MacroProbe"));
  assert(desc->component_count == 1u && desc->behavior_count == 1u);
  assert(desc->start && !desc->stop && !desc->update);
  assert(desc->data_size == sizeof(MacroData) && desc->data_version == 3u);
  assert(desc->scope == VKR_SCOPE_CONTAINER);
  const VkrComponentDesc *gate = desc->components[0];
  assert(gate == macro_gate_type());
  assert(!strcmp(gate->name, "macro_gate") && gate->field_count == 6u);
  assert(gate->size == sizeof(MacroGate));
  const VkrFieldDesc *axis = &gate->fields[4];
  assert(!strcmp(axis->name, "axis") && axis->kind == VKR_FIELD_KIND_VEC3 &&
         axis->offset == offsetof(MacroGate, axis));
  assert(gate->fields[0].max == 10.0f &&
         !strcmp(gate->fields[0].unit, "turns/s"));
  assert(gate->fields[5].names == s_macro_modes);
  MacroGate value;
  memset(&value, 0xff, sizeof(value));
  gate->defaults(&value);
  assert(value.speed == 0.25f && !value.locked && value.count == 3u &&
         value.offset == -2 && value.axis.y == 1.0f && value.mode == 1);
  const VkrBehaviorDesc *lamp = desc->behaviors[0];
  assert(lamp->component == macro_lamp_type() && lamp->update && !lamp->start);
  assert(!vkr_module_MacroProbe(VKR_SDK_VERSION + 1u));
}

bool32_t run_script_host_tests(void) {
  assert(vkr_dmemory_create(MB(4), MB(32), &s_memory));
  s_allocator = (VkrAllocator){.ctx = &s_memory};
  vkr_dmemory_allocator_create(&s_allocator);
  test_script_authoring_macros();
  test_player_start_resolution();

  /* One host for the suite: its modules register their types once. */
  static VkrScriptHost host;
  assert(vkr_script_host_init(&host, &s_allocator));
  const char *error = NULL;
  assert(!vkr_script_host_add_module(&host, life_old_entry, &error));
  assert(vkr_script_host_add_module(&host, vkr_module_life, &error));
  test_script_host_lifecycle(&host);
  assert(vkr_script_host_add_module(&host, vkr_module_spinner, &error));
  test_script_behaviors(&host);
  assert(vkr_script_host_add_module(&host, vkr_module_zone, &error));
  assert(vkr_script_host_add_module(&host, vkr_module_game, &error));
  test_script_containers(&host);
  assert(vkr_script_host_add_module(&host, vkr_module_burst, &error));
  test_script_deferred(&host);
  assert(vkr_script_host_add_module(&host, vkr_module_crunch, &error));
  test_script_tasks(&host);
  test_has_visual(&host);
  vkr_script_host_shutdown(&host);
  printf("Script host tests passed\n");
  return true_v;
}
