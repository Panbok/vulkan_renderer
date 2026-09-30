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

/* A placed model is the player's body when its entity or a descendant
   carries a mesh or a shape; an empty entity is not. */
static void test_renders_mesh(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 52, 16, NULL));
  VkrScriptHost host;
  vkr_script_host_init(&host, allocator);
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
  vkr_scene_update_transforms(&scene);
  assert(host.api.renders_mesh(&scene, root));
  assert(host.api.renders_mesh(&scene, cube));
  assert(host.api.renders_mesh(&scene, node));
  assert(!host.api.renders_mesh(&scene, empty));
  assert(!host.api.renders_mesh(&scene, VKR_ENTITY_ID_INVALID));
  vkr_script_host_shutdown(&host);
  vkr_scene_shutdown(&scene, NULL);
}

/* The authoring macros, read back through the descriptors they generate:
   member offsets, kinds, options, defaults and the module description. */
static const char *const s_macro_modes[] = {"Walk", "Run", NULL};

#define MACRO_GATE_FIELDS                                                      \
  VKR_FIELD(F32, speed, "Speed", 0.25f, .unit = "turns/s", .min = -10.0f,      \
            .max = 10.0f)                                                      \
  VKR_FIELD(BOOL, locked, "Locked", false_v)                                   \
  VKR_FIELD(U32, count, "Count", 3u)                                           \
  VKR_FIELD(I32, offset, "Offset", -2)                                         \
  VKR_FIELD(VEC3, axis, "Axis", vec3_new(0.0f, 1.0f, 0.0f))                    \
  VKR_FIELD(ENUM, mode, "Mode", 1, .names = s_macro_modes)
VKR_SCRIPT_COMPONENT(MacroGate, macro_gate, "Macro gate", MACRO_GATE_FIELDS)

#define MACRO_LAMP_FIELDS VKR_FIELD(F32, glow, "Glow", 1.0f)
VKR_SCRIPT_COMPONENT(MacroLamp, macro_lamp, "Macro lamp", MACRO_LAMP_FIELDS)

typedef struct MacroState {
  uint32_t ticks;
} MacroState;

static VkrScriptStart macro_start(const VkrScriptSession *session, void *state,
                                  const char **error) {
  (void)session;
  (void)state;
  (void)error;
  return VKR_SCRIPT_START_IDLE;
}

static void macro_stop(const VkrScriptSession *session, void *state) {
  (void)session;
  (void)state;
}

VKR_SCRIPT_MODULE(MacroProbe, MacroState, 3, (macro_gate)(macro_lamp),
                  .start = macro_start, .stop = macro_stop)

static void test_script_authoring_macros(void) {
  VkrScriptApi api = {.version = VKR_SCRIPT_ABI_VERSION,
                      .size = sizeof(VkrScriptApi)};
  const VkrScriptModuleDesc *desc = vkr_script_module_MacroProbe(&api);
  assert(desc && !strcmp(desc->name, "MacroProbe") && desc->type_count == 2u);
  assert(desc->start == macro_start && desc->stop == macro_stop);
  assert(desc->state_size == sizeof(MacroState) && desc->state_version == 3u);
  const VkrTypeDesc *gate = desc->types[0];
  assert(gate == macro_gate_type() && gate == macro_gate_type());
  assert(!strcmp(gate->name, "macro_gate") && gate->property_count == 6u);
  assert(gate->size == sizeof(MacroGate));
  const VkrPropertyDesc *axis = &gate->properties[4];
  assert(!strcmp(axis->name, "axis") && axis->kind == VKR_PROPERTY_VEC3 &&
         axis->offset == offsetof(MacroGate, axis));
  assert(gate->properties[0].max == 10.0f &&
         !strcmp(gate->properties[0].unit, "turns/s"));
  assert(gate->properties[5].names == s_macro_modes);
  MacroGate value;
  memset(&value, 0xff, sizeof(value));
  gate->defaults(&value);
  assert(value.speed == 0.25f && !value.locked && value.count == 3u &&
         value.offset == -2 && value.axis.y == 1.0f && value.mode == 1);
  assert(vkr_type_validate(gate, &value, NULL, 0u));
  assert(desc->types[1]->property_count == 1u);
  api.version = VKR_SCRIPT_ABI_VERSION + 1u;
  assert(!vkr_script_module_MacroProbe(&api));
}

bool32_t run_script_host_tests(void) {
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  test_script_host_lifecycle(&allocator);
  test_player_start_resolution(&allocator);
  test_renders_mesh(&allocator);
  test_script_authoring_macros();
  vkr_dmemory_allocator_destroy(&allocator);
  printf("Script host tests passed\n");
  return true_v;
}
