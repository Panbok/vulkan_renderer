#include "gameplay_player_test.h"
#include "gameplay/vkr_gameplay_player.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void player_command(VkrGameplayPlayer *player, uint64_t tick,
                           VkrGameplayAction action, bool8_t pressed) {
  assert(vkr_gameplay_input_push(
      &player->commands,
      (VkrGameplayCommand){.tick = tick,
                           .sequence = player->commands.last_sequence + 1,
                           .action = action,
                           .pressed = pressed}));
}

static void player_expect_world_position(VkrScene *scene, VkrEntityId entity,
                                         Vec3 expected) {
  const SceneTransform *transform = vkr_scene_get_transform(scene, entity);
  assert(transform);
  assert(fabsf(transform->world.elements[12] - expected.x) < 1e-5f);
  assert(fabsf(transform->world.elements[13] - expected.y) < 1e-5f);
  assert(fabsf(transform->world.elements[14] - expected.z) < 1e-5f);
}

static void test_player_evaluated_transforms(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 45, 16, NULL));
  VkrEntityId root = vkr_scene_create_entity(&scene, NULL);
  VkrEntityId child = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, root, vec3_new(2, 3, 4),
                                 vkr_quat_identity(), vec3_one()));
  assert(vkr_scene_set_transform(&scene, child, vec3_new(1, 2, 3),
                                 vkr_quat_identity(), vec3_one()));
  vkr_scene_set_parent(&scene, child, root);
  vkr_scene_update_transforms(&scene);
  player_expect_world_position(&scene, child, vec3_new(3, 5, 7));

  // Explicit 90-degree Y rotation and translation: local (1,2,3) maps to
  // world (13,22,29), independently of the production composition helpers.
  Mat4 evaluated = mat4_identity();
  evaluated.elements[0] = 0;
  evaluated.elements[2] = -1;
  evaluated.elements[8] = 1;
  evaluated.elements[10] = 0;
  evaluated.elements[12] = 10;
  evaluated.elements[13] = 20;
  evaluated.elements[14] = 30;
  assert(vkr_scene_set_evaluated_transform(&scene, root, &evaluated));
  vkr_scene_update_transforms(&scene);
  player_expect_world_position(&scene, root, vec3_new(10, 20, 30));
  player_expect_world_position(&scene, child, vec3_new(13, 22, 29));
  const SceneTransform *authored = vkr_scene_get_transform(&scene, root);
  assert(authored->position.x == 2 && authored->position.y == 3 &&
         authored->position.z == 4);
  assert(authored->rotation.x == 0 && authored->rotation.y == 0 &&
         authored->rotation.z == 0 && authored->rotation.w == 1);
  assert(authored->scale.x == 1 && authored->scale.y == 1 &&
         authored->scale.z == 1);
  assert(authored->local.elements[12] == 2 &&
         authored->local.elements[13] == 3 &&
         authored->local.elements[14] == 4);

  Mat4 invalid = evaluated;
  invalid.elements[12] = NAN;
  assert(!vkr_scene_set_evaluated_transform(&scene, root, &invalid));
  invalid = evaluated;
  invalid.elements[3] = 0.1f; // Perspective is not a world transform.
  assert(!vkr_scene_set_evaluated_transform(&scene, root, &invalid));
  vkr_scene_update_transforms(&scene);
  player_expect_world_position(&scene, root, vec3_new(10, 20, 30));
  player_expect_world_position(&scene, child, vec3_new(13, 22, 29));
  assert(vkr_scene_set_evaluated_transform(&scene, root, NULL));
  vkr_scene_update_transforms(&scene);
  player_expect_world_position(&scene, root, vec3_new(2, 3, 4));
  player_expect_world_position(&scene, child, vec3_new(3, 5, 7));

  VkrEntityId falling = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, falling, vec3_new(0, 8, 0),
                                 vkr_quat_identity(), vec3_one()));
  const VkrScenePhysicsSnapshot body = vkr_scene_physics_default();
  assert(vkr_scene_physics_apply(&scene, falling, &body, NULL));
  vkr_scene_physics_set_paused(&scene, false_v);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT * 10.5);
  assert(vkr_scene_simulation_completed_ticks(&scene) == 8);
  const float64_t debt = vkr_scene_physics_debt(&scene);
  assert(fabs(debt - VKR_SCENE_SIMULATION_FIXED_DT * 2.5) < 1e-12);
  VkrPhysicsPose native_pose;
  assert(vkr_scene_physics_get_pose(&scene, falling, &native_pose));
  assert(native_pose.position[1] < 8);
  const Vec3 native_position =
      vec3_new(native_pose.position[0], native_pose.position[1],
               native_pose.position[2]);
  player_expect_world_position(&scene, falling, native_position);
  assert(vkr_scene_set_evaluated_transform(&scene, falling, &evaluated));
  vkr_scene_update_transforms(&scene);
  player_expect_world_position(&scene, falling, vec3_new(10, 20, 30));
  assert(vkr_scene_simulation_completed_ticks(&scene) == 8);
  assert(vkr_scene_physics_debt(&scene) == debt);
  assert(vkr_scene_set_evaluated_transform(&scene, falling, NULL));
  vkr_scene_update_transforms(&scene);
  player_expect_world_position(&scene, falling, native_position);
  assert(vkr_scene_simulation_completed_ticks(&scene) == 8);
  assert(vkr_scene_physics_debt(&scene) == debt);
  assert(vkr_scene_get_transform(&scene, falling)->position.y == 8);
  vkr_scene_shutdown(&scene, NULL);
}

static void test_player_observer_bursts(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 46, 16, NULL));
  InputState input = {0};
  VkrEntityId floor = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, floor, vec3_new(0, -.5f, 0),
                                 vkr_quat_identity(), vec3_one()));
  VkrScenePhysicsSnapshot body = vkr_scene_physics_default();
  body.motion = VKR_PHYSICS_STATIC;
  body.colliders[0].half_extent = vec3_new(20, .5f, 20);
  assert(vkr_scene_physics_apply(&scene, floor, &body, NULL));
  // A target on yaw .32 radians; the final yaw zero ray cannot hit this box.
  VkrEntityId target = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, target,
                                 vec3_new(3.796942f, 1.6f, 1.258267f),
                                 vkr_quat_identity(), vec3_one()));
  body.colliders[0].half_extent = vec3_new(.3f, .3f, .3f);
  assert(vkr_scene_physics_apply(&scene, target, &body, NULL));
  VkrEntityId entity = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, entity, vec3_new(0, .1f, 0),
                                 vkr_quat_identity(), vec3_one()));
  VkrGameplayPlayer player = {0};
  assert(
      vkr_gameplay_player_attach(&player, &scene, &input, entity, 90, 0, NULL));
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(vkr_gameplay_player_frame(&player, 100, true_v) == 0);
  const float64_t boundary = 100 + VKR_SCENE_SIMULATION_FIXED_DT;
  const float64_t elapsed =
      vkr_gameplay_player_frame(&player, boundary, true_v);
  assert(elapsed < VKR_SCENE_SIMULATION_FIXED_DT);
  vkr_scene_update(&scene, elapsed);
  assert(vkr_scene_simulation_completed_ticks(&scene) == 1);
  VkrInputTransition event = {.kind = VKR_INPUT_TRANSITION_LOOK,
                              .time_seconds = boundary};
  input.observer(&event, input.observer_context);
  assert(!player.commands.faulted && player.commands.count == 1);
  assert(player.commands.commands[player.commands.head].tick == 2);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(vkr_scene_simulation_completed_ticks(&scene) == 2);

  for (uint32_t i = 0; i < 1024; ++i) {
    event.time_seconds = 100.040 + i * 1e-7;
    event.delta_x = 0.125;
    input.observer(&event, input.observer_context);
  }
  assert(!player.commands.faulted && player.commands.count == 1);
  event = (VkrInputTransition){.kind = VKR_INPUT_TRANSITION_BUTTON,
                               .time_seconds = 100.041,
                               .code = BUTTON_LEFT,
                               .pressed = true_v};
  input.observer(&event, input.observer_context);
  event = (VkrInputTransition){.kind = VKR_INPUT_TRANSITION_LOOK,
                               .delta_x = -0.125};
  for (uint32_t i = 0; i < 1024; ++i) {
    event.time_seconds = 100.042 + i * 1e-7;
    input.observer(&event, input.observer_context);
  }
  event = (VkrInputTransition){.kind = VKR_INPUT_TRANSITION_BUTTON,
                               .time_seconds = 100.043,
                               .code = BUTTON_LEFT,
                               .pressed = false_v};
  input.observer(&event, input.observer_context);
  assert(!player.commands.faulted && player.commands.count == 4);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(!scene.simulation.faulted);
  assert(player.shots_fired == 1 && player.hits == 1);
  const VkrPlayerState *state = vkr_entity_get_component_if_alive_const(
      scene.world, entity, player.component);
  assert(state && state->weapon.magazine_rounds == 11);
  assert(fabsf(state->yaw) < 1e-5f);
  assert(!(state->held & (1u << VKR_GAMEPLAY_FIRE)));
  assert(player.commands.count == 0);

  // The observer runs after InputState records each physical key transition.
  // Releasing one Ctrl while the other stays held must keep the motor crouched.
  event = (VkrInputTransition){.kind = VKR_INPUT_TRANSITION_KEY,
                               .time_seconds = 100.060,
                               .code = KEY_LCONTROL,
                               .pressed = true_v};
  input.current_keys.keys[KEY_LCONTROL] = true_v;
  input.observer(&event, input.observer_context);
  event.time_seconds = 100.061;
  event.code = KEY_RCONTROL;
  input.current_keys.keys[KEY_RCONTROL] = true_v;
  input.observer(&event, input.observer_context);
  event.time_seconds = 100.062;
  event.code = KEY_LCONTROL;
  event.pressed = false_v;
  input.current_keys.keys[KEY_LCONTROL] = false_v;
  input.observer(&event, input.observer_context);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(!scene.simulation.faulted);
  VkrPhysicsCharacterState motor;
  assert(vkr_scene_character_get_state(&scene, entity, &motor, NULL));
  assert(motor.crouched && state->crouched);
  VkrCameraRigPose camera;
  assert(vkr_gameplay_player_camera(&player, &camera));
  assert(camera.position.y - player.current_foot.y < 1.5f);
  event.time_seconds = 100.070;
  event.code = KEY_RCONTROL;
  input.current_keys.keys[KEY_RCONTROL] = false_v;
  input.observer(&event, input.observer_context);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(!scene.simulation.faulted);
  assert(vkr_scene_character_get_state(&scene, entity, &motor, NULL));
  assert(!motor.crouched && !state->crouched);
  vkr_gameplay_player_shutdown(&player);
  vkr_scene_shutdown(&scene, NULL);
}

static void test_player_unfocused_simulation(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 47, 16, NULL));
  InputState input = {0};
  const VkrEntityId falling = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, falling, vec3_new(4, 8, 0),
                                 vkr_quat_identity(), vec3_one()));
  const VkrScenePhysicsSnapshot body = vkr_scene_physics_default();
  assert(vkr_scene_physics_apply(&scene, falling, &body, NULL));
  const VkrEntityId entity = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, entity, vec3_new(0, 8, 0),
                                 vkr_quat_identity(), vec3_one()));
  VkrGameplayPlayer player = {0};
  assert(vkr_gameplay_player_attach(&player, &scene, &input, entity, 100, 0,
                                    NULL));
  VkrPlayerState *state =
      vkr_entity_get_component_mut(scene.world, entity, player.component);
  assert(state);
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(vkr_gameplay_player_frame(&player, 100, true_v) == 0);
  const VkrInputTransition press = {.time_seconds = 100.001,
                                    .kind = VKR_INPUT_TRANSITION_BUTTON,
                                    .code = BUTTON_LEFT,
                                    .pressed = true_v};
  input.observer(&press, input.observer_context);
  state->held = 1u << VKR_GAMEPLAY_FORWARD;

  // UI input capture cancels player intent, but cannot freeze a running world.
  for (uint32_t tick = 1; tick <= 8; ++tick) {
    const float64_t now = 100 + tick * VKR_SCENE_SIMULATION_FIXED_DT;
    const float64_t dt = vkr_gameplay_player_frame(&player, now, false_v);
    assert(fabs(dt - VKR_SCENE_SIMULATION_FIXED_DT) < 1e-12);
    vkr_scene_update(&scene, dt);
    assert(vkr_scene_simulation_completed_ticks(&scene) == tick);
    assert(!scene.physics_paused && !scene.simulation.faulted);
    assert(!player.active && !state->held && !player.commands.count);
  }
  VkrPhysicsPose pose;
  assert(vkr_scene_physics_get_pose(&scene, falling, &pose));
  assert(pose.position[1] < 8 && player.current_foot.y < 8);
  assert(player.shots_fired == 0);

  VkrInputTransition ignored = press;
  ignored.time_seconds = 100 + 8.5 * VKR_SCENE_SIMULATION_FIXED_DT;
  input.observer(&ignored, input.observer_context);
  assert(!player.commands.count);
  const float64_t resumed = 100 + 9 * VKR_SCENE_SIMULATION_FIXED_DT;
  vkr_scene_update(&scene, vkr_gameplay_player_frame(&player, resumed, true_v));
  assert(vkr_scene_simulation_completed_ticks(&scene) == 9);
  assert(player.active && player.shots_fired == 0);

  // A fresh press after focus returns retains the shared scene tick mapping.
  VkrInputTransition fresh = press;
  fresh.time_seconds = resumed + 0.5 * VKR_SCENE_SIMULATION_FIXED_DT;
  input.observer(&fresh, input.observer_context);
  assert(!player.commands.faulted && player.commands.count == 1);
  assert(player.commands.commands[player.commands.head].tick == 10);
  vkr_scene_update(
      &scene, vkr_gameplay_player_frame(
                  &player, 100 + 10 * VKR_SCENE_SIMULATION_FIXED_DT, true_v));
  assert(vkr_scene_simulation_completed_ticks(&scene) == 10);
  assert(!scene.simulation.faulted && player.shots_fired == 1);

  // Explicit pause still excludes the paused wall-time span on resume.
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_gameplay_player_frame(&player, 150, true_v) == 0);
  assert(!player.active && !state->held && !player.commands.count);
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(vkr_gameplay_player_frame(&player, 200, true_v) == 0);
  fresh.time_seconds = 200 + 0.5 * VKR_SCENE_SIMULATION_FIXED_DT;
  input.observer(&fresh, input.observer_context);
  assert(!player.commands.faulted && player.commands.count == 1);
  assert(player.commands.commands[player.commands.head].tick == 11);
  vkr_scene_update(&scene,
                   vkr_gameplay_player_frame(
                       &player, 200 + VKR_SCENE_SIMULATION_FIXED_DT, true_v));
  assert(vkr_scene_simulation_completed_ticks(&scene) == 11);
  assert(!scene.simulation.faulted);
  vkr_gameplay_player_shutdown(&player);
  vkr_scene_shutdown(&scene, NULL);
}

bool32_t run_gameplay_player_tests(void) {
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  test_player_evaluated_transforms(&allocator);
  test_player_observer_bursts(&allocator);
  test_player_unfocused_simulation(&allocator);
  VkrScene scene;
  assert(vkr_scene_init(&scene, &allocator, 44, 16, NULL));
  InputState input = {0};
  VkrEntityId floor = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, floor, vec3_new(0, -.5f, 0),
                                 vkr_quat_identity(), vec3_one()));
  VkrScenePhysicsSnapshot body = vkr_scene_physics_default();
  body.motion = VKR_PHYSICS_STATIC;
  body.colliders[0].half_extent = vec3_new(20, .5f, 20);
  assert(vkr_scene_physics_apply(&scene, floor, &body, NULL));
  VkrEntityId entity = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, entity, vec3_new(0, .1f, 0),
                                 vkr_quat_identity(), vec3_one()));
  VkrGameplayPlayer player = {0};
  assert(vkr_gameplay_player_attach(&player, &scene, &input, entity, 70, 0.25f,
                                    NULL));
  assert(!vkr_gameplay_player_attach(&player, &scene, &input, entity, 80, 0,
                                     NULL));
  VkrPlayerState *state =
      vkr_entity_get_component_mut(scene.world, entity, player.component);
  assert(state && state->weapon.magazine_rounds == 12);
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(vkr_gameplay_player_frame(&player, 100, true_v) == 0);
  // Complete short presses must survive a frame with no simulation tick.
  const VkrInputTransition press = {.time_seconds = 100.001,
                                    .kind = VKR_INPUT_TRANSITION_BUTTON,
                                    .code = BUTTON_LEFT,
                                    .pressed = true_v};
  input.observer(&press, input.observer_context);
  VkrInputTransition release = press;
  release.time_seconds = 100.002;
  release.pressed = false_v;
  input.observer(&release, input.observer_context);
  vkr_scene_update(&scene, 0);
  assert(player.shots_fired == 0);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(player.shots_fired == 1 && state->weapon.magazine_rounds == 11);
  assert(!(state->held & (1u << VKR_GAMEPLAY_FIRE)));
  player_command(&player, 2, VKR_GAMEPLAY_FORWARD, true_v);
  player_command(&player, 2, VKR_GAMEPLAY_RELOAD, true_v);
  player_command(&player, 32, VKR_GAMEPLAY_FORWARD, false_v);
  for (uint32_t tick = 2; tick <= 62; ++tick) {
    vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
    assert(!scene.simulation.faulted);
  }
  assert(fabsf(player.current_foot.x - 2.422281f) < .05f);
  assert(fabsf(player.current_foot.z - 0.618510f) < .05f);
  assert(fabsf(player.current_foot.y) < .05f);
  assert(state->weapon.magazine_rounds == 12 && state->reserve_rounds == 119);
  VkrCameraRigPose camera;
  assert(vkr_gameplay_player_camera(&player, &camera));
  assert(fabsf(camera.position.y - 1.6f) < .06f);
  // Pause invalidates pending/held input without admitting the paused wall
  // span.
  state->held = 1u << VKR_GAMEPLAY_FIRE;
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_gameplay_player_frame(&player, 150, true_v) == 0);
  assert(!player.active && state->held == 0 && player.commands.count == 0);
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(vkr_gameplay_player_frame(&player, 200, true_v) == 0);
  assert(player.active);
  assert(!vkr_scene_simulation_detach(&scene, &input));
  vkr_scene_physics_set_paused(&scene, true_v);
  player.render_yaw = 1.0f;
  player.render_pitch = 0.5f;
  state->yaw = 1.0f;
  state->pitch = 0.5f;
  assert(vkr_scene_physics_reset(&scene, NULL));
  state = vkr_entity_get_component_mut(scene.world, entity, player.component);
  assert(state->weapon.instance_id == 71 &&
         state->weapon.magazine_rounds == 12);
  assert(player.commands.consumed_tick == 0 && player.shots_fired == 0);
  assert(player.render_yaw == 0.25f && player.render_pitch == 0);
  assert(state->yaw == 0.25f && state->pitch == 0);
  vkr_gameplay_player_shutdown(&player);
  assert(!input.observer && !scene.simulation.enabled);
  assert(!vkr_scene_character_get_state(&scene, entity,
                                        &(VkrPhysicsCharacterState){0}, NULL));
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_allocator_destroy(&allocator);
  printf("Gameplay player tests passed\n");
  return true_v;
}
