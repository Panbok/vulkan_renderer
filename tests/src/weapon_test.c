#include "weapon_test.h"

#include "gameplay/vkr_weapon.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_simulation.h"

typedef struct WeaponTestSink {
  VkrWeaponShot shots[2];
  uint32_t count;
} WeaponTestSink;

static bool8_t weapon_test_reserve(const VkrWeaponShot *shot, void *context) {
  WeaponTestSink *sink = context;
  if (sink->count == ArrayCount(sink->shots)) {
    return false_v;
  }
  sink->shots[sink->count++] = *shot;
  return true_v;
}

static VkrWeaponState weapon_test_create(uint32_t rounds) {
  const VkrWeaponConfig config = {
      .magazine_capacity = 5,
      .fire_interval_ticks = 3,
      .reload_ticks = 7,
  };
  VkrWeaponState weapon = {0};
  assert(vkr_weapon_initialize(&weapon, &config, rounds, 41));
  return weapon;
}

static void test_weapon_creation(void) {
  VkrWeaponState weapon = weapon_test_create(2);
  VkrWeaponConfig config = weapon.config;
  config.fire_interval_ticks = 0;
  assert(!vkr_weapon_initialize(&weapon, &config, 2, 42));
  config = weapon.config;
  config.reload_ticks = 0;
  assert(!vkr_weapon_initialize(&weapon, &config, 2, 42));
  config = weapon.config;
  config.magazine_capacity = 0;
  assert(!vkr_weapon_initialize(&weapon, &config, 0, 42));
  assert(!vkr_weapon_initialize(&weapon, &weapon.config, 6, 42));
  assert(!vkr_weapon_initialize(&weapon, &weapon.config, 2, 0));
  assert(weapon.instance_id == 41 && weapon.magazine_rounds == 2);
  assert(weapon.config.magazine_capacity == 5);
  assert(weapon.config.fire_interval_ticks == 3);
  assert(weapon.config.reload_ticks == 7);
}

static void test_weapon_shot_admission_and_cooldown(void) {
  VkrWeaponState weapon = weapon_test_create(3);
  WeaponTestSink sink = {0};
  VkrWeaponShot shot = {0};

  assert(vkr_weapon_try_fire(&weapon, 10, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_OK);
  assert(weapon.magazine_rounds == 2 && weapon.next_fire_tick == 13);
  assert(shot.instance_id == 41 && shot.sequence == 1 && shot.tick == 10);
  assert(sink.count == 1 && sink.shots[0].sequence == 1);
  assert(vkr_weapon_try_fire(&weapon, 12, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_COOLDOWN);
  assert(sink.count == 1 && weapon.magazine_rounds == 2);

  assert(vkr_weapon_try_fire(&weapon, 13, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_OK);
  assert(shot.sequence == 2 && weapon.magazine_rounds == 1);
  assert(vkr_weapon_try_fire(&weapon, 16, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_CAPACITY);
  assert(weapon.magazine_rounds == 1 && weapon.shot_sequence == 2);
  assert(weapon.next_fire_tick == 16 && shot.sequence == 2);

  /* The consumer retires earlier shots; retrying failed admission uses the same
   * next identity and spends exactly the one remaining round. */
  sink.count = 0;
  assert(vkr_weapon_try_fire(&weapon, 16, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_OK);
  assert(shot.sequence == 3 && weapon.magazine_rounds == 0);
  assert(vkr_weapon_try_fire(&weapon, 19, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_EMPTY);
  assert(sink.count == 1 && weapon.shot_sequence == 3);
}

static void test_weapon_reload_conservation(void) {
  VkrWeaponState weapon = weapon_test_create(2);
  VkrWeaponReloadToken reload = {0};
  uint32_t reserve = 9;
  uint32_t transferred = 99;
  WeaponTestSink sink = {0};
  VkrWeaponShot shot = {0};

  assert(vkr_weapon_reload_start(&weapon, 20, reserve, &reload) ==
         VKR_WEAPON_OK);
  assert(weapon.magazine_rounds == 2 && reserve == 9);
  assert(vkr_weapon_try_fire(&weapon, 21, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_RELOADING);
  assert(vkr_weapon_reload_complete(&weapon, reload, 26, &reserve,
                                    &transferred) == VKR_WEAPON_NOT_READY);
  assert(weapon.magazine_rounds == 2 && reserve == 9 && transferred == 99);
  assert(vkr_weapon_reload_complete(&weapon, reload, 27, &reserve,
                                    &transferred) == VKR_WEAPON_OK);
  assert(weapon.magazine_rounds == 5 && reserve == 6 && transferred == 3);
  assert(vkr_weapon_reload_complete(&weapon, reload, 27, &reserve,
                                    &transferred) == VKR_WEAPON_STALE_ACTION);
  assert(weapon.magazine_rounds == 5 && reserve == 6);
  assert(vkr_weapon_reload_start(&weapon, 30, reserve, &reload) ==
         VKR_WEAPON_FULL);

  weapon = weapon_test_create(0);
  assert(vkr_weapon_reload_start(&weapon, 0, 0, &reload) ==
         VKR_WEAPON_NO_RESERVE);
  assert(vkr_weapon_reload_start(&weapon, 30, 9, &reload) == VKR_WEAPON_OK);
  /* Another inventory operation consumed rounds during this reload. */
  reserve = 2;
  assert(vkr_weapon_reload_complete(&weapon, reload, 37, &reserve,
                                    &transferred) == VKR_WEAPON_OK);
  assert(weapon.magazine_rounds == 2 && reserve == 0 && transferred == 2);
  assert(vkr_weapon_reload_start(&weapon, 40, 1, &reload) == VKR_WEAPON_OK);
  assert(vkr_weapon_reload_complete(&weapon, reload, 47, &reserve,
                                    &transferred) == VKR_WEAPON_OK);
  assert(weapon.magazine_rounds == 2 && reserve == 0 && transferred == 0);
  assert(!weapon.reloading);
}

static void test_weapon_reload_cancellation(void) {
  VkrWeaponState weapon = weapon_test_create(1);
  VkrWeaponReloadToken old = {0};
  VkrWeaponReloadToken current = {0};
  uint32_t reserve = 4;
  uint32_t transferred = 99;

  assert(vkr_weapon_reload_start(&weapon, 0, reserve, &old) == VKR_WEAPON_OK);
  assert(vkr_weapon_reload_cancel(&weapon, old));
  assert(weapon.magazine_rounds == 1 && reserve == 4);
  assert(!vkr_weapon_reload_cancel(&weapon, old));
  assert(vkr_weapon_reload_start(&weapon, 2, reserve, &current) ==
         VKR_WEAPON_OK);
  assert(!vkr_weapon_reload_cancel(&weapon, old));
  assert(vkr_weapon_reload_complete(&weapon, old, 20, &reserve, &transferred) ==
         VKR_WEAPON_STALE_ACTION);
  assert(weapon.reloading && weapon.magazine_rounds == 1 && reserve == 4);
  assert(transferred == 99);
  assert(vkr_weapon_reload_complete(&weapon, current, 9, &reserve,
                                    &transferred) == VKR_WEAPON_OK);
  assert(weapon.magazine_rounds == 5 && reserve == 0 && transferred == 4);

  /* Reinitializing a reused slot receives a different caller-owned identity. */
  assert(vkr_weapon_initialize(&weapon, &weapon.config, 1, 42));
  reserve = 4;
  assert(vkr_weapon_reload_start(&weapon, 0, reserve, &current) ==
         VKR_WEAPON_OK);
  assert(!vkr_weapon_reload_cancel(&weapon, old));
  assert(vkr_weapon_reload_complete(&weapon, old, 20, &reserve, &transferred) ==
         VKR_WEAPON_STALE_ACTION);
  assert(weapon.reloading && weapon.magazine_rounds == 1 && reserve == 4);
}

static void test_weapon_overlapping_blocks(void) {
  VkrWeaponState weapon = weapon_test_create(2);
  VkrWeaponBlockToken stun = {0};
  VkrWeaponBlockToken cinematic = {0};
  VkrWeaponBlockToken replacement = {0};
  WeaponTestSink sink = {0};
  VkrWeaponShot shot = {0};

  assert(vkr_weapon_block_acquire(&weapon, &stun) == VKR_WEAPON_OK);
  assert(vkr_weapon_block_acquire(&weapon, &cinematic) == VKR_WEAPON_OK);
  assert(vkr_weapon_try_fire(&weapon, 0, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_BLOCKED);
  assert(vkr_weapon_block_release(&weapon, cinematic));
  assert(vkr_weapon_try_fire(&weapon, 0, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_BLOCKED);
  assert(vkr_weapon_block_acquire(&weapon, &replacement) == VKR_WEAPON_OK);
  assert(!vkr_weapon_block_release(&weapon, cinematic));
  assert(vkr_weapon_block_release(&weapon, stun));
  assert(vkr_weapon_try_fire(&weapon, 0, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_BLOCKED);
  assert(weapon.magazine_rounds == 2 && sink.count == 0);
  assert(vkr_weapon_block_release(&weapon, replacement));
  assert(vkr_weapon_try_fire(&weapon, 0, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_OK);

  assert(vkr_weapon_initialize(&weapon, &weapon.config, 2, 42));
  assert(vkr_weapon_block_acquire(&weapon, &replacement) == VKR_WEAPON_OK);
  assert(!vkr_weapon_block_release(&weapon, stun));
  assert(vkr_weapon_try_fire(&weapon, 0, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_BLOCKED);
}

static void test_weapon_capacity_and_overflow(void) {
  VkrWeaponState weapon = weapon_test_create(1);
  VkrWeaponBlockToken blocks[VKR_WEAPON_MAX_BLOCKS] = {0};
  VkrWeaponBlockToken extra = {0};
  for (uint32_t i = 0; i < ArrayCount(blocks); ++i) {
    assert(vkr_weapon_block_acquire(&weapon, &blocks[i]) == VKR_WEAPON_OK);
  }
  assert(vkr_weapon_block_acquire(&weapon, &extra) == VKR_WEAPON_CAPACITY);
  assert(!extra.sequence);
  for (uint32_t i = 0; i < ArrayCount(blocks); ++i) {
    assert(vkr_weapon_block_release(&weapon, blocks[i]));
  }

  /* Force numerical boundaries independently of impractical lifetime lengths.
   */
  weapon.block_sequence = UINT64_MAX;
  assert(vkr_weapon_block_acquire(&weapon, &extra) == VKR_WEAPON_LIMIT);
  WeaponTestSink sink = {0};
  VkrWeaponShot shot = {0};
  assert(vkr_weapon_try_fire(&weapon, UINT64_MAX - 2, weapon_test_reserve,
                             &sink, &shot) == VKR_WEAPON_LIMIT);
  weapon.shot_sequence = UINT64_MAX;
  assert(vkr_weapon_try_fire(&weapon, 0, weapon_test_reserve, &sink, &shot) ==
         VKR_WEAPON_LIMIT);
  assert(weapon.magazine_rounds == 1 && sink.count == 0);

  VkrWeaponReloadToken reload = {0};
  assert(vkr_weapon_reload_start(&weapon, UINT64_MAX - 6, 1, &reload) ==
         VKR_WEAPON_LIMIT);
  weapon.reload_sequence = UINT64_MAX;
  assert(vkr_weapon_reload_start(&weapon, 0, 1, &reload) == VKR_WEAPON_LIMIT);
  assert(!weapon.reloading && !reload.sequence);
}

typedef struct WeaponSceneTest {
  VkrEntityId entity;
  VkrComponentTypeId component;
  WeaponTestSink sink;
  VkrWeaponShot consumed[2];
  VkrWeaponResult result;
  uint64_t instance_id;
  uint32_t before_count;
  uint32_t after_count;
  uint32_t consumed_count;
  uint32_t reset_count;
} WeaponSceneTest;

static bool8_t weapon_scene_before(VkrScene *scene, uint64_t tick,
                                   void *context) {
  WeaponSceneTest *test = context;
  assert(test->before_count == test->after_count);
  assert(test->sink.count == 0);
  VkrWeaponState *weapon =
      vkr_entity_get_component_mut(scene->world, test->entity, test->component);
  assert(weapon != NULL);
  VkrWeaponShot shot = {0};
  test->result = vkr_weapon_try_fire(weapon, tick, weapon_test_reserve,
                                     &test->sink, &shot);
  test->before_count++;
  return true_v;
}

static bool8_t weapon_scene_after(VkrScene *scene, uint64_t tick,
                                  void *context) {
  WeaponSceneTest *test = context;
  const VkrWeaponResult expected[] = {
      VKR_WEAPON_OK,    VKR_WEAPON_COOLDOWN, VKR_WEAPON_COOLDOWN,
      VKR_WEAPON_OK,    VKR_WEAPON_COOLDOWN, VKR_WEAPON_COOLDOWN,
      VKR_WEAPON_EMPTY,
  };
  assert(tick >= 1 && tick <= ArrayCount(expected));
  assert(test->before_count == test->after_count + 1);
  assert(test->result == expected[tick - 1]);
  assert(test->sink.count == (test->result == VKR_WEAPON_OK ? 1u : 0u));
  for (uint32_t i = 0; i < test->sink.count; ++i) {
    assert(test->consumed_count < ArrayCount(test->consumed));
    assert(test->sink.shots[i].tick == tick);
    assert(test->sink.shots[i].instance_id == test->instance_id);
    test->consumed[test->consumed_count++] = test->sink.shots[i];
  }
  test->sink.count = 0;
  const VkrWeaponState *weapon = vkr_entity_get_component_if_alive_const(
      scene->world, test->entity, test->component);
  assert(weapon != NULL);
  assert(weapon->magazine_rounds == 2 - test->consumed_count);
  assert(weapon->shot_sequence == test->consumed_count);
  test->after_count++;
  return true_v;
}

static void weapon_scene_reset(VkrScene *scene, void *context) {
  WeaponSceneTest *test = context;
  VkrWeaponState *weapon =
      vkr_entity_get_component_mut(scene->world, test->entity, test->component);
  assert(weapon != NULL);
  test->instance_id++;
  assert(vkr_weapon_initialize(weapon, &weapon->config, 2, test->instance_id));
  test->sink.count = 0;
  test->before_count = 0;
  test->after_count = 0;
  test->consumed_count = 0;
  test->reset_count++;
}

static void test_weapon_scene_component_ticks(void) {
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  assert(vkr_scene_init(&scene, &allocator, 38, 16, NULL));
  WeaponSceneTest test = {.instance_id = 41};
  test.entity = vkr_scene_create_entity(&scene, NULL);
  assert(test.entity.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_transform(&scene, test.entity, vec3_new(2, 3, 4),
                                 vkr_quat_identity(), vec3_one()));
  vkr_scene_update(&scene, 0);
  test.component = vkr_entity_register_component(scene.world, "WeaponState",
                                                 sizeof(VkrWeaponState),
                                                 AlignOf(VkrWeaponState));
  assert(test.component != VKR_COMPONENT_TYPE_INVALID);
  const VkrWeaponState initial = weapon_test_create(2);
  assert(vkr_entity_add_component(scene.world, test.entity, test.component,
                                  &initial));
  const VkrSceneSimulationCallbacks callbacks = {
      .before_physics = weapon_scene_before,
      .after_physics = weapon_scene_after,
      .reset = weapon_scene_reset,
      .context = &test,
  };
  assert(vkr_scene_simulation_configure(&scene, &callbacks, NULL));
  vkr_scene_physics_set_paused(&scene, false_v);

  /* No input/frame update loop is involved: ECS state advances only through
   * the scene's common ticks, including a frame with no completed tick. */
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT * 0.5);
  assert(test.before_count == 0 && test.after_count == 0);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT * 0.5);
  assert(test.after_count == 1 && test.consumed_count == 1);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT * 3);
  assert(test.after_count == 4 && test.consumed_count == 2);
  assert(test.consumed[0].tick == 1 && test.consumed[0].sequence == 1);
  assert(test.consumed[1].tick == 4 && test.consumed[1].sequence == 2);
  vkr_scene_update(&scene, 0);
  assert(test.after_count == 4);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT * 3);
  assert(test.after_count == 7 && test.result == VKR_WEAPON_EMPTY);
  assert(vkr_scene_simulation_completed_ticks(&scene) == 7);
  assert(scene.physics == NULL);
  assert(vkr_scene_get_transform(&scene, test.entity)->world.elements[12] == 2);

  const VkrWeaponShot previous = test.consumed[0];
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_reset(&scene, NULL));
  assert(test.reset_count == 1 && test.after_count == 0);
  assert(vkr_scene_simulation_completed_ticks(&scene) == 0);
  vkr_scene_physics_set_paused(&scene, false_v);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(test.after_count == 1 && test.consumed_count == 1);
  assert(test.consumed[0].sequence == 1 && test.consumed[0].tick == 1);
  assert(test.consumed[0].instance_id != previous.instance_id);

  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_allocator_destroy(&allocator);
}

bool32_t run_weapon_tests(void) {
  printf("Running weapon tests...\n");
  test_weapon_creation();
  test_weapon_shot_admission_and_cooldown();
  test_weapon_reload_conservation();
  test_weapon_reload_cancellation();
  test_weapon_overlapping_blocks();
  test_weapon_capacity_and_overflow();
  test_weapon_scene_component_ticks();
  printf("Weapon tests PASSED\n");
  return true_v;
}
