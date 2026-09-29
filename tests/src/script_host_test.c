#include "script_host_test.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_types.h"
#include "script/vkr_script_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A module that records its calls; `start_result` selects its start. */
typedef struct ScriptTestState {
  uint32_t starts;
  uint32_t stops;
  uint32_t before;
  uint32_t after;
  uint32_t resets;
  uint64_t last_tick;
} ScriptTestState;

static VkrScriptStart s_start_result;
static ScriptTestState *s_state;

static VkrScriptStart script_test_start(const VkrScriptSession *session,
                                        void *state, const char **error) {
  assert(session->scene && session->instance_id);
  ScriptTestState *test = state;
  assert(test->starts == 0 && test->before == 0); // Zeroed for every session.
  test->starts++;
  s_state = test;
  *error = "refused";
  return s_start_result;
}

static void script_test_stop(const VkrScriptSession *session, void *state) {
  (void)session;
  ((ScriptTestState *)state)->stops++;
}

static bool8_t script_test_before(const VkrScriptSession *session, void *state,
                                  uint64_t tick, const char **error) {
  (void)session;
  (void)error;
  ScriptTestState *test = state;
  test->before++;
  test->last_tick = tick;
  return true_v;
}

static bool8_t script_test_after(const VkrScriptSession *session, void *state,
                                 uint64_t tick, const char **error) {
  (void)session;
  ScriptTestState *test = state;
  test->after++;
  if (tick == 3) {
    *error = "tick three";
    return false_v;
  }
  return true_v;
}

static void script_test_reset(const VkrScriptSession *session, void *state) {
  (void)session;
  ((ScriptTestState *)state)->resets++;
}

static void script_test_frame(const VkrScriptSession *session, void *state,
                              VkrScriptFrame *frame) {
  (void)session;
  (void)state;
  frame->scene_delta = 2.0 * VKR_SCENE_SIMULATION_FIXED_DT;
}

static const VkrScriptModuleDesc s_test_module = {
    .abi_version = VKR_SCRIPT_ABI_VERSION,
    .size = sizeof(VkrScriptModuleDesc),
    .name = "test",
    .state_size = sizeof(ScriptTestState),
    .state_align = AlignOf(ScriptTestState),
    .start = script_test_start,
    .stop = script_test_stop,
    .before_physics = script_test_before,
    .after_physics = script_test_after,
    .reset = script_test_reset,
    .frame = script_test_frame,
};

static const VkrScriptModuleDesc *script_test_entry(const VkrScriptApi *api) {
  return api->version == VKR_SCRIPT_ABI_VERSION ? &s_test_module : NULL;
}

static const VkrScriptModuleDesc *
script_test_old_entry(const VkrScriptApi *api) {
  (void)api;
  static const VkrScriptModuleDesc old = {.abi_version = 0};
  return &old;
}

static void test_script_host_lifecycle(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 49, 16, NULL));
  InputState input = {0};
  VkrScriptHost host;
  vkr_script_host_init(&host, allocator);
  const char *error = NULL;
  assert(!vkr_script_host_add_module(&host, script_test_old_entry, &error));
  assert(vkr_script_host_add_module(&host, script_test_entry, &error));

  // A failed start leaves no session and no scene callbacks.
  s_start_result = VKR_SCRIPT_START_FAILED;
  assert(!vkr_script_host_start(&host, &scene, &input, NULL, 0, &error));
  assert(error && !host.started && !scene.simulation.enabled);
  assert(s_state->stops == 0);

  // An idle module receives no hooks, and the scene keeps no callbacks.
  s_start_result = VKR_SCRIPT_START_IDLE;
  assert(vkr_script_host_start(&host, &scene, &input, NULL, 0, &error));
  assert(host.started && !vkr_script_host_active(&host));
  assert(!scene.simulation.enabled);
  vkr_script_host_stop(&host);
  assert(s_state->stops == 0 && !host.started);

  // An active module runs on the shared clock and its frame hook sets the
  // admitted elapsed time.
  s_start_result = VKR_SCRIPT_START_ACTIVE;
  assert(vkr_script_host_start(&host, &scene, &input, NULL, 0, &error));
  assert(vkr_script_host_active(&host) && scene.simulation.enabled);
  const uint64_t first_instance = host.session.instance_id;
  vkr_scene_physics_set_paused(&scene, false_v);
  VkrScriptFrame frame = {.scene_delta = 0.0};
  vkr_script_host_frame(&host, &frame);
  vkr_scene_update(&scene, frame.scene_delta);
  assert(s_state->before == 2 && s_state->after == 2);
  assert(s_state->last_tick == 2);

  // A hook failure faults the simulation with the module's name.
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(scene.simulation.faulted && s_state->after == 3);
  assert(strstr(scene.simulation.error_storage, "test: tick three"));

  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_reset(&scene, NULL));
  assert(s_state->resets == 1);
  vkr_script_host_stop(&host);
  assert(s_state->stops == 1 && !scene.simulation.enabled);

  // A new session gets a new identity and freshly zeroed state.
  assert(vkr_script_host_start(&host, &scene, &input, NULL, 0, &error));
  assert(host.session.instance_id != first_instance && s_state->starts == 1);
  vkr_script_host_stop(&host);
  vkr_scene_shutdown(&scene, NULL);
}

static void test_player_start_resolution(VkrAllocator *allocator) {
  VkrScene world;
  VkrScene scene;
  assert(vkr_scene_init(&world, allocator, VKR_SCENE_WORLD_ROOT_ID, 16, NULL));
  assert(vkr_scene_init(&scene, allocator, 50, 16, NULL));
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

bool32_t run_script_host_tests(void) {
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  test_script_host_lifecycle(&allocator);
  test_player_start_resolution(&allocator);
  vkr_dmemory_allocator_destroy(&allocator);
  printf("Script host tests passed\n");
  return true_v;
}
