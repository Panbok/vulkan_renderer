#include "script_reload_test.h"

#include "core/vkr_threads.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_types.h"
#include "script/vkr_script_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The probe's data layout (tests/scripts/reload_probe.c). */
typedef struct ReloadData {
  uint32_t ticks;
  uint32_t starts;
} ReloadData;

static void reload_ticks(VkrScene *scene, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    vkr_scene_update(scene, VKR_SCENE_SIMULATION_FIXED_DT);
  }
}

/* Hot reload of a real library: code-only changes keep the running data, a
 * new data version restarts the session, and changed component fields are
 * refused while the previous code keeps running. */
static void test_script_library_reload(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 51, 16, NULL));
  InputState input = {0};
  static VkrScriptHost host;
  assert(vkr_script_host_init(&host, allocator));
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

  const VkrScriptSessionDesc session = {.active = &scene, .input = &input};
  assert(vkr_script_host_start(&host, &session, &error));
  const ReloadData *state =
      vkr_script_host_instance_data(&host, "reload_probe", &scene);
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 3u);
  assert(state->ticks == 3u && state->starts == 1u);

  /* Code only: the same data continues under the new code. */
  assert(vkr_script_host_load_library(&host, "reload_probe",
                                      VKR_TEST_RELOAD_PROBE_2,
                                      &error) == VKR_SCRIPT_RELOAD_KEPT_STATE);
  assert(host.started && module->generation == 2u && host.retired_count == 1u);
  assert(state->starts == 1u);
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 2u);
  assert(state->ticks == 23u);

  /* A new data version: the session restarts on fresh data. */
  assert(vkr_script_host_load_library(&host, "reload_probe",
                                      VKR_TEST_RELOAD_PROBE_3,
                                      &error) == VKR_SCRIPT_RELOAD_RESTARTED);
  state = vkr_script_host_instance_data(&host, "reload_probe", &scene);
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

/* The project probe's data layout (tests/scripts/project_probe.c). */
typedef struct ProbeData {
  uint32_t ticks;
} ProbeData;

/* What the test's migrator reaches, as an application's reaches its scenes,
 * journals and pending edit. */
typedef struct MigrateTarget {
  VkrScene *scene;
  VkrSceneEditState *edits;
  VkrSceneEditValues *pending;
  uint32_t calls;
} MigrateTarget;

static void migrate_target(void *context, const VkrTypeDesc *type,
                           const VkrTypeDesc *previous) {
  MigrateTarget *target = context;
  assert(vkr_scene_migrate_world_type(target->scene, type, previous));
  vkr_scene_edit_migrate_type(target->edits, target->pending, type, previous);
  target->calls++;
}

/* A number property of a registered type's value, by name. */
static float64_t probe_number(const VkrTypeDesc *type, const void *value,
                              const char *name) {
  const uint32_t index = vkr_type_find_property(
      type, string8_create_from_cstr((const uint8_t *)name, strlen(name)));
  float64_t number = -1.0;
  assert(index != UINT32_MAX &&
         vkr_property_get_number(&type->properties[index], value, &number));
  return number;
}

static void probe_set_number(const VkrTypeDesc *type, void *value,
                             const char *name, float64_t number) {
  const uint32_t index = vkr_type_find_property(
      type, string8_create_from_cstr((const uint8_t *)name, strlen(name)));
  assert(index != UINT32_MAX &&
         vkr_property_set_number(&type->properties[index], value, number));
}

static void *prepare_thread(void *prepared) {
  vkr_script_prepare_run(prepared);
  return NULL;
}

/* A project library lists several modules and reloads them together: code
 * changes keep data, a dropped module retires and an added one registers by
 * restarting the session, and one refused module keeps every module on its
 * previous code. */
static void test_project_library(VkrAllocator *allocator) {
  static VkrScriptHost host;
  assert(vkr_script_host_init(&host, allocator));
  const char *error = NULL;
  assert(vkr_script_host_load_project(&host, "probe_project",
                                      VKR_TEST_PROJECT_PROBE_1,
                                      &error) == VKR_SCRIPT_RELOAD_LOADED);
  const VkrScriptModule *a = vkr_script_host_module(&host, "ProbeA");
  const VkrScriptModule *b = vkr_script_host_module(&host, "ProbeB");
  assert(a && b && a->dynamic && a->library == b->library);
  assert(host.libraries[a->library].project);
  assert(vkr_scene_world_type_named(string8_lit("probe_a")) &&
         vkr_scene_world_type_named(string8_lit("probe_b")));

  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 55, 16, NULL));
  InputState input = {0};
  const VkrScriptSessionDesc session = {.active = &scene, .input = &input};
  assert(vkr_script_host_start(&host, &session, &error));
  const ProbeData *data =
      vkr_script_host_instance_data(&host, "ProbeA", &scene);
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 3u);
  assert(data->ticks == 3u);

  // Code only, prepared on another thread while the session keeps ticking:
  // the running data continues under the new code once it commits.
  static VkrScriptPrepared prepared;
  assert(vkr_script_host_prepare(&host, &prepared, "probe_project",
                                 VKR_TEST_PROJECT_PROBE_2, true_v, false_v));
  VkrThread thread = NULL;
  assert(vkr_thread_create(allocator, &thread, prepare_thread, &prepared));
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 1u);
  assert(vkr_thread_join(thread));
  vkr_thread_destroy(allocator, &thread);
  assert(prepared.ready && prepared.count == 2u);
  assert(data->ticks == 4u);
  assert(vkr_script_host_commit(&host, &prepared, &error) ==
         VKR_SCRIPT_RELOAD_KEPT_STATE);
  assert(host.libraries[a->library].generation == 2u);
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 2u);
  assert(data->ticks == 24u);

  // A discarded preparation changes nothing and removes its copy.
  assert(vkr_script_host_prepare(&host, &prepared, "probe_project",
                                 VKR_TEST_PROJECT_PROBE_3, true_v, false_v));
  vkr_script_prepare_run(&prepared);
  char copy[VKR_SCRIPT_PATH_CAPACITY];
  snprintf(copy, sizeof(copy), "%s", prepared.loaded_path);
  vkr_script_host_discard(&host, &prepared);
  FILE *removed = fopen(copy, "rb");
  assert(!removed);
  assert(vkr_script_host_module(&host, "ProbeB") &&
         !vkr_script_host_module(&host, "ProbeC"));
  // A failed run reports through the commit.
  assert(vkr_script_host_prepare(&host, &prepared, "probe_project",
                                 "/missing/project.dll", true_v, false_v));
  vkr_script_prepare_run(&prepared);
  assert(!prepared.ready);
  assert(vkr_script_host_commit(&host, &prepared, &error) ==
         VKR_SCRIPT_RELOAD_FAILED);
  assert(error && strstr(error, "copied"));

  // ProbeB leaves and ProbeC arrives: B retires, C registers, and the
  // session restarts on fresh data.
  assert(vkr_script_host_load_project(&host, "probe_project",
                                      VKR_TEST_PROJECT_PROBE_3,
                                      &error) == VKR_SCRIPT_RELOAD_RESTARTED);
  b = vkr_script_host_module(&host, "ProbeB");
  const VkrScriptModule *c = vkr_script_host_module(&host, "ProbeC");
  assert(b && b->retired && !b->desc);
  assert(c && !c->retired && c->generation == 1u);
  assert(vkr_scene_world_type_named(string8_lit("probe_b")));
  data = vkr_script_host_instance_data(&host, "ProbeA", &scene);
  const ProbeData *c_data =
      vkr_script_host_instance_data(&host, "ProbeC", &scene);
  assert(data && c_data && data->ticks == 0u);
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 1u);
  assert(data->ticks == 1u && c_data->ticks == 1u);

  // One refused module refuses the whole load: ProbeC keeps its code too.
  const uint32_t c_generation = c->generation;
  assert(vkr_script_host_load_project(&host, "probe_project",
                                      VKR_TEST_PROJECT_PROBE_4,
                                      &error) == VKR_SCRIPT_RELOAD_FAILED);
  assert(error && strstr(error, "probe_a changed its fields"));
  c = vkr_script_host_module(&host, "ProbeC");
  assert(c->generation == c_generation);
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 1u);
  assert(data->ticks == 2u && c_data->ticks == 2u);

  // With a migrator, the same change moves the live component, the undo
  // journal and a pending edit to the new field instead of refusing.
  const VkrTypeDesc *probe_a =
      vkr_scene_world_type_named(string8_lit("probe_a"));
  assert(probe_a && probe_a->size == sizeof(float32_t));
  const VkrEntityId holder = vkr_scene_create_entity(&scene, NULL);
  _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
  vkr_type_defaults(probe_a, value);
  probe_set_number(probe_a, value, "value", 7.0);
  assert(vkr_scene_set_typed(&scene, holder, probe_a, value));
  static VkrSceneEditState edits;
  vkr_scene_edit_reset(&edits, allocator, 1u);
  VkrSceneEditValues edit = {0};
  assert(vkr_scene_edit_read_component(&scene, holder, probe_a, &edit));
  probe_set_number(probe_a, edit.component, "value", 9.0);
  assert(vkr_scene_edit_apply(&edits, &scene, holder, &edit));
  VkrSceneEditValues pending = edit;
  MigrateTarget target = {
      .scene = &scene, .edits = &edits, .pending = &pending};
  vkr_script_host_set_migrator(&host, migrate_target, &target);
  assert(vkr_script_host_load_project(&host, "probe_project",
                                      VKR_TEST_PROJECT_PROBE_4,
                                      &error) == VKR_SCRIPT_RELOAD_KEPT_STATE);
  assert(target.calls == 1u && probe_a->size == 2u * sizeof(float32_t));
  const void *moved = vkr_scene_get_typed(&scene, holder, probe_a);
  assert(moved && probe_number(probe_a, moved, "value") == 9.0 &&
         probe_number(probe_a, moved, "extra") == 0.5);
  const VkrComponentTypeId id = vkr_scene_type_id(&scene, probe_a);
  assert(vkr_entity_get_component_info(scene.world, id)->size ==
         2u * sizeof(float32_t));
  assert(probe_number(probe_a, pending.component, "value") == 9.0 &&
         probe_number(probe_a, pending.component, "extra") == 0.5);
  assert(vkr_scene_edit_undo(&edits, &scene, false_v));
  moved = vkr_scene_get_typed(&scene, holder, probe_a);
  assert(moved && probe_number(probe_a, moved, "value") == 7.0 &&
         probe_number(probe_a, moved, "extra") == 0.5);
  vkr_scene_physics_set_paused(&scene, false_v);
  reload_ticks(&scene, 1u);
  assert(data->ticks == 3u && c_data->ticks == 102u);

  // A packaged game opens its library where it lies: no copy beside it,
  // and closing leaves the file. Back on V3, ProbeA drops `extra` again.
  assert(vkr_script_host_prepare(&host, &prepared, "probe_project",
                                 VKR_TEST_PROJECT_PROBE_3, true_v, true_v));
  assert(!prepared.loaded_path[0]);
  vkr_script_prepare_run(&prepared);
  assert(vkr_script_host_commit(&host, &prepared, &error) ==
         VKR_SCRIPT_RELOAD_KEPT_STATE);
  assert(target.calls == 2u && probe_a->size == sizeof(float32_t));
  moved = vkr_scene_get_typed(&scene, holder, probe_a);
  assert(moved && probe_number(probe_a, moved, "value") == 7.0);
  vkr_scene_edit_reset(&edits, allocator, 2u);
  c = vkr_script_host_module(&host, "ProbeC");
  assert(host.libraries[c->library].handle.handle &&
         !host.libraries[c->library].loaded_path[0]);

  vkr_script_host_shutdown(&host);
  FILE *kept = fopen(VKR_TEST_PROJECT_PROBE_3, "rb");
  assert(kept);
  fclose(kept);
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
  test_project_library(&allocator);
  printf("Script reload tests passed\n");
  return true_v;
}
