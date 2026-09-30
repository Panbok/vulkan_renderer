#include "script_reload_test.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_types.h"
#include "script/vkr_script_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The probe's state layout (tests/scripts/reload_probe.c). */
typedef struct ReloadState {
  uint32_t ticks;
  uint32_t starts;
  uint32_t reloads;
} ReloadState;

static void reload_ticks(VkrScene *scene, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    vkr_scene_update(scene, VKR_SCENE_SIMULATION_FIXED_DT);
  }
}

/* Hot reload of a real library: code-only changes keep the running state, a
 * new state version restarts the session, and changed component fields are
 * refused while the previous code keeps running. */
static void test_script_library_reload(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 51, 16, NULL));
  InputState input = {0};
  VkrScriptHost host;
  vkr_script_host_init(&host, allocator);
  const char *error = NULL;

  assert(vkr_script_host_load_library(&host, "reload_probe",
                                      VKR_TEST_RELOAD_PROBE_1,
                                      &error) == VKR_SCRIPT_RELOAD_LOADED);
  assert(!vkr_script_host_load_library(&host, "reload_probe", "/missing.dylib",
                                       &error));
  const VkrScriptModule *module = vkr_script_host_module(&host, "reload_probe");
  assert(module && module->dynamic && module->generation == 1u);
  /* Scenes see the host's copy of the component type, not the library's. */
  const VkrTypeDesc *type =
      vkr_scene_world_type_named(string8_lit("reload_probe"));
  assert(type == module->types[0] && vkr_scene_world_type_registered(type));
  assert(vkr_scene_sync_world_types(&scene));
  assert(vkr_scene_type_id(&scene, type) != VKR_COMPONENT_TYPE_INVALID);

  assert(vkr_script_host_start(&host, &scene, &input, NULL, 0u, &error));
  const ReloadState *state = module->state;
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 3u);
  assert(state->ticks == 3u && state->starts == 1u);

  /* Code only: the same state continues under the new code. */
  assert(vkr_script_host_load_library(&host, "reload_probe",
                                      VKR_TEST_RELOAD_PROBE_2,
                                      &error) == VKR_SCRIPT_RELOAD_KEPT_STATE);
  assert(host.started && module->generation == 2u && host.retired_count == 1u);
  assert(state->reloads == 1u && state->starts == 1u);
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 2u);
  assert(state->ticks == 23u);

  /* A new state version: the session restarts on fresh state. */
  assert(vkr_script_host_load_library(&host, "reload_probe",
                                      VKR_TEST_RELOAD_PROBE_3,
                                      &error) == VKR_SCRIPT_RELOAD_RESTARTED);
  state = module->state;
  assert(host.started && state->starts == 1u && state->ticks == 0u);
  assert(host.retired_count == 0u);
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 1u);
  assert(state->ticks == 1u);

  /* Changed fields cannot apply to live components: refused, old code on. */
  assert(vkr_script_host_load_library(&host, "reload_probe",
                                      VKR_TEST_RELOAD_PROBE_4,
                                      &error) == VKR_SCRIPT_RELOAD_FAILED);
  assert(error && strstr(error, "reload_probe changed its fields"));
  assert(module->generation == 3u && type->property_count == 1u);
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 1u);
  assert(state->ticks == 2u);

  /* Retiring keeps the type registered without the library's hooks; a later
     load of the same module adopts it. */
  vkr_script_host_retire_libraries(&host);
  assert(!host.started && module->retired && !module->desc);
  assert(vkr_scene_world_type_named(string8_lit("reload_probe")) == type);
  assert(vkr_script_host_load_library(&host, "reload_probe",
                                      VKR_TEST_RELOAD_PROBE_1,
                                      &error) == VKR_SCRIPT_RELOAD_KEPT_STATE);
  assert(!module->retired && module->desc);
  vkr_script_host_shutdown(&host);
  vkr_scene_shutdown(&scene, NULL);
}

bool32_t run_script_reload_tests(void) {
  /* The registered component type copies outlive this suite, as every host
     allocator must: later suites' scenes read them. */
  static VkrDMemory memory;
  static VkrAllocator allocator;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  allocator = (VkrAllocator){.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  test_script_library_reload(&allocator);
  printf("Script reload tests passed\n");
  return true_v;
}
