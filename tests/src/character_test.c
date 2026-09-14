#include "character_test.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "physics/vkr_physics.h"
#include "renderer/systems/vkr_scene_physics.h"

static VkrPhysicsBody character_test_box(VkrPhysicsWorld *world, uint64_t id,
                                         Vec3 position, Vec3 extent,
                                         uint16_t mask) {
  const VkrPhysicsColliderDesc collider = {
      .entity_id = id + 1000,
      .shape = VKR_PHYSICS_BOX,
      .rotation = {0, 0, 0, 1},
      .scale = {1, 1, 1},
      .half_extent = {extent.x, extent.y, extent.z},
      .enabled = true_v,
  };
  const VkrPhysicsBodyDesc desc = {
      .entity_id = id,
      .motion = VKR_PHYSICS_STATIC,
      .position = {position.x, position.y, position.z},
      .rotation = {0, 0, 0, 1},
      .mass = 1,
      .friction = 0.5f,
      .enabled = true_v,
      .collision_layer = 1,
      .collision_mask = mask,
      .colliders = &collider,
      .collider_count = 1,
  };
  VkrPhysicsBody body = VKR_PHYSICS_BODY_INVALID;
  assert(vkr_physics_body_create(world, &desc, &body));
  return body;
}

static void test_character_identity_capacity_and_free_motion(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(2);
  assert(world != NULL);
  VkrPhysicsCharacterDesc desc = vkr_physics_character_default();
  desc.entity_id = 10;
  desc.foot_position[1] = 2;
  VkrPhysicsCharacter character = VKR_PHYSICS_CHARACTER_INVALID;
  const float32_t radius = desc.radius;
  desc.radius = 0;
  assert(!vkr_physics_character_create(world, &desc, &character));
  assert(character == VKR_PHYSICS_CHARACTER_INVALID);
  desc.radius = radius;
  assert(vkr_physics_character_create(world, &desc, &character));
  VkrPhysicsCharacterState state;
  assert(vkr_physics_character_get_state(world, character, &state));
  assert(state.foot_position[1] == 2);
  assert(state.ground == VKR_PHYSICS_CHARACTER_IN_AIR);
  VkrPhysicsCharacterInput input = {
      .velocity = {2, 0, 0},
      .gravity = {0, -6, 0},
      .dt = 1.0f / 60.0f,
  };
  assert(vkr_physics_character_step(world, character, &input, &state));
  assert(fabsf(state.foot_position[0] - 1.0f / 30.0f) < 1e-5f);
  assert(fabsf(state.foot_position[1] - (2 - 1.0f / 600.0f)) < 1e-5f);
  assert(fabsf(state.velocity[1] + 0.1f) < 1e-5f);
  input.dt = NAN;
  assert(!vkr_physics_character_step(world, character, &input, &state));
  assert(vkr_physics_character_get_state(world, character, &state));
  assert(fabsf(state.foot_position[0] - 1.0f / 30.0f) < 1e-5f);

  const VkrPhysicsCharacter stale = character;
  assert(vkr_physics_character_destroy(world, character));
  assert(vkr_physics_character_create(world, &desc, &character));
  assert(character != stale);
  assert(!vkr_physics_character_get_state(world, stale, &state));
  assert(!vkr_physics_character_destroy(world, stale));
  assert(vkr_physics_character_get_state(world, character, &state));
  vkr_physics_world_destroy(world);

  world = vkr_physics_world_create(2);
  assert(world != NULL);
  VkrPhysicsCharacter slots[VKR_PHYSICS_MAX_CHARACTERS];
  for (uint32_t i = 0; i < ArrayCount(slots); ++i) {
    desc.entity_id = 20 + i;
    assert(vkr_physics_character_create(world, &desc, &slots[i]));
  }
  assert(!vkr_physics_character_get_state(world, character, &state));
  assert(!vkr_physics_character_create(world, &desc, &character));
  for (uint32_t i = 0; i < ArrayCount(slots); ++i) {
    assert(vkr_physics_character_destroy(world, slots[i]));
  }
  assert(vkr_physics_character_create(world, &desc, &character));
  vkr_physics_world_destroy(world);
}

static void test_character_ground_wall_jump_and_sphere_query(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(4);
  assert(world != NULL);
  character_test_box(world, 100, vec3_new(0, -0.5f, 0), vec3_new(10, 0.5f, 10),
                     UINT16_MAX);
  character_test_box(world, 200, vec3_new(2, 1.5f, 0), vec3_new(0.25f, 1.5f, 5),
                     UINT16_MAX);
  VkrPhysicsCharacterDesc desc = vkr_physics_character_default();
  desc.entity_id = 10;
  desc.foot_position[1] = 0.1f;
  VkrPhysicsCharacter character;
  assert(vkr_physics_character_create(world, &desc, &character));
  VkrPhysicsCharacterState state;
  assert(vkr_physics_character_get_state(world, character, &state));
  for (uint32_t i = 0; i < 120; ++i) {
    const VkrPhysicsCharacterInput input = {
        .velocity = {2, state.velocity[1], 0},
        .gravity = {0, -9.81f, 0},
        .dt = 1.0f / 60.0f,
    };
    assert(vkr_physics_character_step(world, character, &input, &state));
  }
  assert(state.ground == VKR_PHYSICS_CHARACTER_ON_GROUND);
  assert(state.ground_entity_id == 100);
  assert(fabsf(state.foot_position[1]) < 0.05f);
  assert(state.foot_position[0] > 1.35f && state.foot_position[0] < 1.46f);
  const VkrPhysicsCharacterInput jump = {
      .velocity = {0, 5, 0},
      .gravity = {0, -9.81f, 0},
      .dt = 1.0f / 60.0f,
  };
  const float32_t grounded_y = state.foot_position[1];
  assert(vkr_physics_character_step(world, character, &jump, &state));
  assert(state.foot_position[1] > grounded_y + 0.06f);
  assert(state.velocity[1] > 4.8f);

  const float32_t origin[3] = {0, 1, 0};
  const float32_t displacement[3] = {4, 0, 0};
  VkrPhysicsRayHit hit;
  bool8_t found = false_v;
  assert(vkr_physics_sweep_sphere(world, origin, displacement, 0.1f, NULL, &hit,
                                  &found));
  assert(found && hit.entity_id == 200);
  assert(fabsf(hit.fraction - 0.4125f) < 0.001f);
  const uint64_t ignored[] = {200};
  const VkrPhysicsQueryFilter filter = {
      .mask = UINT16_MAX,
      .ignored_entities = ignored,
      .ignored_count = ArrayCount(ignored),
  };
  assert(vkr_physics_sweep_sphere(world, origin, displacement, 0.1f, &filter,
                                  &hit, &found));
  assert(!found); // The CharacterVirtual itself has no query proxy.
  assert(!vkr_physics_sweep_sphere(world, origin, displacement, -1, NULL, &hit,
                                   &found));
  vkr_physics_world_destroy(world);
}

static void test_character_crouch_clearance_and_foot_anchor(void) {
  /* A low ceiling must reject standing without faulting or lifting the feet.
   * Movement out from below it must make the same held stand request succeed. */
  VkrPhysicsWorld *world = vkr_physics_world_create(4);
  assert(world != NULL);
  character_test_box(world, 100, vec3_new(0, -0.5f, 0), vec3_new(10, 0.5f, 10),
                     UINT16_MAX);
  VkrPhysicsCharacterDesc desc = vkr_physics_character_default();
  desc.entity_id = 10;
  desc.foot_position[1] = 0.02f;
  VkrPhysicsCharacter character;
  assert(vkr_physics_character_create(world, &desc, &character));
  VkrPhysicsCharacterInput input = {
      .gravity = {0, -9.81f, 0},
      .dt = 1.0f / 60.0f,
  };
  VkrPhysicsCharacterState state;
  for (uint32_t i = 0; i < 30; ++i) {
    assert(vkr_physics_character_step(world, character, &input, &state));
  }
  assert(!state.crouched);
  const float32_t foot_y = state.foot_position[1];
  for (uint32_t i = 0; i < 20; ++i) {
    input.crouch = true_v;
    assert(vkr_physics_character_step(world, character, &input, &state));
    assert(state.crouched);
    assert(fabsf(state.foot_position[1] - foot_y) < 0.01f);
    input.crouch = false_v;
    assert(vkr_physics_character_step(world, character, &input, &state));
    assert(!state.crouched);
    assert(fabsf(state.foot_position[1] - foot_y) < 0.01f);
  }
  input.crouch = true_v;
  assert(vkr_physics_character_step(world, character, &input, &state));
  character_test_box(world, 200, vec3_new(0, 1.5f, 0), vec3_new(1, 0.2f, 1),
                     UINT16_MAX);
  input.crouch = false_v;
  for (uint32_t i = 0; i < 30; ++i) {
    assert(vkr_physics_character_step(world, character, &input, &state));
    assert(state.crouched);
    assert(fabsf(state.foot_position[1] - foot_y) < 0.01f);
  }
  assert(vkr_physics_character_get_state(world, character, &state));
  assert(state.crouched);
  input.velocity[0] = 2;
  for (uint32_t i = 0; i < 120; ++i) {
    assert(vkr_physics_character_step(world, character, &input, &state));
  }
  assert(state.foot_position[0] > 3.9f);
  assert(!state.crouched);
  assert(fabsf(state.foot_position[1] - foot_y) < 0.01f);
  const VkrPhysicsCharacter stale = character;
  assert(vkr_physics_character_destroy(world, character));
  desc.foot_position[0] = 4;
  assert(vkr_physics_character_create(world, &desc, &character));
  assert(!vkr_physics_character_step(world, stale, &input, &state));
  assert(vkr_physics_character_get_state(world, character, &state));
  assert(!state.crouched);
  vkr_physics_world_destroy(world);
}

static void test_character_bilateral_filter(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(2);
  assert(world != NULL);
  /* Character wants layer 1, but this floor only admits opposing layer 2. */
  character_test_box(world, 100, vec3_new(0, -0.5f, 0), vec3_new(10, 0.5f, 10),
                     2);
  VkrPhysicsCharacterDesc desc = vkr_physics_character_default();
  desc.entity_id = 10;
  desc.foot_position[1] = 0.1f;
  VkrPhysicsCharacter character;
  assert(vkr_physics_character_create(world, &desc, &character));
  VkrPhysicsCharacterState state;
  assert(vkr_physics_character_get_state(world, character, &state));
  for (uint32_t i = 0; i < 60; ++i) {
    const VkrPhysicsCharacterInput input = {
        .velocity = {0, state.velocity[1], 0},
        .gravity = {0, -9.81f, 0},
        .dt = 1.0f / 60.0f,
    };
    assert(vkr_physics_character_step(world, character, &input, &state));
  }
  assert(state.foot_position[1] < -4.0f);
  assert(state.ground == VKR_PHYSICS_CHARACTER_IN_AIR);
  vkr_physics_world_destroy(world);
}

typedef struct CharacterSceneTest {
  VkrEntityId entity;
  uint32_t ticks;
  uint32_t resets;
} CharacterSceneTest;

static bool8_t character_scene_before(VkrScene *scene, uint64_t tick,
                                      void *context) {
  CharacterSceneTest *test = context;
  const VkrPhysicsCharacterInput input = {
      .velocity = {3, 0, 0},
      .dt = (float32_t)VKR_SCENE_PHYSICS_FIXED_DT,
  };
  VkrPhysicsCharacterState state;
  assert(vkr_scene_character_step(scene, test->entity, &input, &state, NULL));
  assert(!vkr_scene_character_step(scene, test->entity, &input, &state, NULL));
  assert(!vkr_scene_character_destroy(scene, test->entity, NULL));
  assert(fabsf(state.foot_position[0] - tick * 0.05f) < 1e-5f);
  test->ticks++;
  return true_v;
}

static void character_scene_reset(VkrScene *scene, void *context) {
  CharacterSceneTest *test = context;
  VkrPhysicsCharacterState state;
  assert(vkr_scene_character_get_state(scene, test->entity, &state, NULL));
  assert(state.foot_position[0] == 0);
  test->ticks = 0;
  test->resets++;
}

static void test_character_scene_reset_and_evaluated_pose(void) {
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  assert(vkr_scene_init(&scene, &allocator, 39, 16, NULL));
  CharacterSceneTest test = {.entity = vkr_scene_create_entity(&scene, NULL)};
  assert(test.entity.u64 != 0);
  assert(vkr_scene_set_transform(&scene, test.entity, vec3_new(0, 2, 0),
                                 vkr_quat_identity(), vec3_one()));
  const VkrPhysicsCharacterDesc settings = vkr_physics_character_default();
  assert(vkr_scene_character_create(&scene, test.entity, &settings, NULL));
  assert(!vkr_scene_character_create(&scene, test.entity, &settings, NULL));
  const VkrScenePhysicsSnapshot body = vkr_scene_physics_default();
  assert(!vkr_scene_physics_apply(&scene, test.entity, &body, NULL));
  const VkrSceneSimulationCallbacks callbacks = {
      .before_physics = character_scene_before,
      .reset = character_scene_reset,
      .context = &test,
  };
  assert(vkr_scene_simulation_configure(&scene, &callbacks, NULL));
  vkr_scene_physics_set_paused(&scene, false_v);
  vkr_scene_update(&scene, VKR_SCENE_PHYSICS_FIXED_DT * 2.5);
  assert(test.ticks == 2 && vkr_scene_physics_body_count(&scene) == 0);
  const SceneTransform *transform =
      vkr_scene_get_transform(&scene, test.entity);
  assert(transform->position.x == 0 && transform->position.y == 2);
  assert(fabsf(transform->world.elements[12] - 0.075f) < 1e-5f);
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_reset(&scene, NULL));
  assert(test.resets == 1 && test.ticks == 0);
  vkr_scene_physics_set_paused(&scene, false_v);
  vkr_scene_update(&scene, VKR_SCENE_PHYSICS_FIXED_DT);
  assert(test.ticks == 1);
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_character_destroy(&scene, test.entity, NULL));
  vkr_scene_update(&scene, 0);
  assert(vkr_scene_get_transform(&scene, test.entity)->world.elements[12] == 0);
  VkrPhysicsCharacterState state;
  assert(!vkr_scene_character_get_state(&scene, test.entity, &state, NULL));
  assert(vkr_scene_character_create(&scene, test.entity, &settings, NULL));
  vkr_scene_destroy_entity(&scene, test.entity);
  assert(!vkr_scene_entity_alive(&scene, test.entity));
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_allocator_destroy(&allocator);
}

bool32_t run_character_tests(void) {
  printf("Running character tests...\n");
  test_character_identity_capacity_and_free_motion();
  test_character_ground_wall_jump_and_sphere_query();
  test_character_crouch_clearance_and_foot_anchor();
  test_character_bilateral_filter();
  test_character_scene_reset_and_evaluated_pose();
  printf("Character tests PASSED\n");
  return true_v;
}
