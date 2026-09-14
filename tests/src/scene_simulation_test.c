#include "scene_simulation_test.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_physics.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

typedef struct SimulationTest {
  uint64_t before;
  uint64_t after;
  uint32_t resets;
  bool8_t fail_after;
  VkrEntityId body;
  float32_t previous_y;
} SimulationTest;

static bool8_t simulation_test_before(VkrScene *scene, uint64_t tick,
                                      void *context) {
  SimulationTest *test = context;
  assert(tick == test->before + 1);
  assert(test->before == test->after);
  assert(vkr_scene_simulation_completed_ticks(scene) == tick - 1);
  assert(vkr_scene_create_entity(scene, NULL).u64 == VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_entity_create_entity(scene->world).u64 ==
         VKR_ENTITY_ID_INVALID.u64);
  assert(!vkr_scene_simulation_configure(scene, NULL, NULL));
  assert(!vkr_scene_physics_step(scene, NULL));
  assert(!vkr_scene_physics_reset(scene, NULL));
  assert(!vkr_scene_physics_set_disabled(scene, true_v, NULL));
  VkrScenePhysics *physics = scene->physics;
  vkr_scene_physics_shutdown(scene);
  assert(scene->physics == physics);
  vkr_scene_physics_set_contact_callback(scene, NULL, NULL);
  assert(scene->physics == physics);
  vkr_scene_update(scene, 1.0); // Reentrant update must not create more ticks.
  if (test->body.u64 != VKR_ENTITY_ID_INVALID.u64) {
    VkrPhysicsPose pose;
    assert(vkr_scene_physics_get_pose(scene, test->body, &pose));
    test->previous_y = pose.position[1];
  }
  test->before++;
  return true_v;
}

static bool8_t simulation_test_after(VkrScene *scene, uint64_t tick,
                                     void *context) {
  SimulationTest *test = context;
  assert(tick == test->before && tick == test->after + 1);
  if (test->body.u64 != VKR_ENTITY_ID_INVALID.u64) {
    VkrPhysicsPose pose;
    assert(vkr_scene_physics_get_pose(scene, test->body, &pose));
    assert(pose.position[1] < test->previous_y);
  }
  test->after++;
  return !test->fail_after;
}

static void simulation_test_reset(VkrScene *scene, void *context) {
  SimulationTest *test = context;
  assert(vkr_scene_simulation_completed_ticks(scene) == 0);
  test->before = 0;
  test->after = 0;
  test->fail_after = false_v;
  test->resets++;
}

bool32_t run_scene_simulation_tests(void) {
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  assert(vkr_scene_init(&scene, &allocator, 37, 16, NULL));
  SimulationTest test = {.body = VKR_ENTITY_ID_INVALID};
  const VkrSceneSimulationCallbacks callbacks = {
      .before_physics = simulation_test_before,
      .after_physics = simulation_test_after,
      .reset = simulation_test_reset,
      .context = &test};
  assert(vkr_scene_simulation_configure(&scene, &callbacks, NULL));
  vkr_scene_update(&scene, 1.0);
  assert(test.before == 0); // Paused scenes do not admit time.
  vkr_scene_physics_set_paused(&scene, false_v);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT * 0.5);
  assert(test.before == 0);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT * 2.5);
  assert(test.before == 3 && test.after == 3 && scene.physics == NULL);
  assert(fabs(vkr_scene_physics_time(&scene) - 0.05) < 1e-12);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT * 10);
  assert(test.after == 11);
  assert(fabs(vkr_scene_physics_debt(&scene) -
              VKR_SCENE_SIMULATION_FIXED_DT * 2) < 1e-12);
  vkr_scene_update(&scene, 0);
  assert(test.after == 13);
  vkr_scene_update(&scene, NAN);
  vkr_scene_update(&scene, -1);
  assert(test.after == 13);
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_step(&scene, NULL));
  assert(test.after == 14);
  assert(vkr_scene_physics_reset(&scene, NULL));
  assert(test.resets == 1 && vkr_scene_physics_debt(&scene) == 0);

  // Post-tick failure may follow state changes; resume must never replay it.
  test.fail_after = true_v;
  assert(!vkr_scene_physics_step(&scene, NULL));
  assert(test.after == 1 && vkr_scene_simulation_completed_ticks(&scene) == 0);
  assert(vkr_scene_physics_error(&scene));
  vkr_scene_physics_set_paused(&scene, false_v);
  vkr_scene_update(&scene, 1.0);
  assert(test.after == 1);
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_reset(&scene, NULL));
  assert(test.resets == 2 && !vkr_scene_physics_error(&scene));

  // Attach custom behavior data after render queries were already compiled.
  VkrEntityId entity = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, entity, vec3_new(2, 3, 4),
                                 vkr_quat_identity(), vec3_one()));
  vkr_scene_update(&scene, 0);
  VkrComponentTypeId behavior = vkr_entity_register_component(
      scene.world, "TestBehavior", sizeof(uint32_t), AlignOf(uint32_t));
  uint32_t value = 7;
  assert(vkr_entity_add_component(scene.world, entity, behavior, &value));
  vkr_scene_update(&scene, 0);
  assert(vkr_entity_query_compiled_is_current(scene.world,
                                              &scene.query_transforms));
  assert(scene.topo_count == 1);
  assert(vkr_scene_get_transform(&scene, entity)->world.elements[12] == 2);

  // Native physics and gameplay share the exact same two completed ticks.
  test.body = entity;
  VkrScenePhysicsSnapshot config = vkr_scene_physics_default();
  assert(vkr_scene_physics_apply(&scene, entity, &config, NULL));
  VkrScenePhysicsPrepared *prepared = NULL;
  assert(vkr_scene_physics_prepare(&scene, entity, &config, &prepared, NULL));
  assert(!vkr_scene_physics_step(&scene, NULL));
  assert(test.before == 0 && !scene.simulation.faulted);
  vkr_scene_physics_discard(prepared);
  vkr_scene_physics_set_paused(&scene, false_v);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT * 2);
  assert(test.before == 2 && test.after == 2);
  assert(fabs(vkr_scene_physics_time(&scene) -
              VKR_SCENE_SIMULATION_FIXED_DT * 2) < 1e-12);
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_reset(&scene, NULL));
  assert(test.resets == 3);
  assert(vkr_scene_simulation_configure(&scene, NULL, NULL));
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_allocator_destroy(&allocator);
  printf("Scene simulation tests passed\n");
  return true_v;
}
