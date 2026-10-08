#include "gameplay_player_test.h"
#include "fps_module.h"
#include "fps_player.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_simulation.h"
#include "renderer/systems/vkr_scene_types.h"
#include "script/vkr_script_host.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

/* The host supplies the SDK through a tool context; these tests install the
 * player's tick hooks directly, as the FPS module's hooks call them. */
static VkrScriptHost s_script_host;
/* The scene type table has no removal, so the FPS module's registered type
 * copies outlive this suite: the host's memory is never released, as in the
 * script host suite. */
static VkrDMemory s_module_memory;
static VkrAllocator s_module_allocator;

typedef struct PlayerTest PlayerTest;

/* A kinematic deck a test drives as the IO router drives a mover's brushes
 * (ADR-084): before the player's tick, its body's target and its drawn pose
 * move to `motion(tick)`, a turn about the world origin then an offset. */
typedef struct PlayerTestDeck {
  uint64_t key;
  VkrEntityId entity;
  void (*motion)(uint64_t tick, Vec3 *offset, VkrQuat *rotation);
} PlayerTestDeck;

struct PlayerTest {
  VkrCtx *ctx;
  VkrScene *scene;
  FpsPlayer player;
  /* The player's entity and the deck under it, when a test has one. */
  VkrEntityId entity;
  PlayerTestDeck deck;
};

static bool8_t player_test_before(VkrScene *scene, uint64_t tick,
                                  void *context) {
  PlayerTest *test = context;
  const char *error = NULL;
  /* The router steps movers before behaviors (script_host_before_physics). */
  if (test->deck.motion) {
    Vec3 offset = vec3_zero();
    VkrQuat rotation = vkr_quat_identity();
    test->deck.motion(tick, &offset, &rotation);
    if (!vkr_scene_physics_generated_move(scene, test->deck.key, offset,
                                          rotation, vec3_zero(), &error)) {
      scene->simulation.error = error;
      return false_v;
    }
    const Mat4 pose =
        mat4_mul(mat4_translate(offset), vkr_quat_to_mat4(rotation));
    if (!vkr_scene_set_evaluated_transform(scene, test->deck.entity, &pose)) {
      scene->simulation.error = "Deck pose refused";
      return false_v;
    }
  }
  if (!fps_player_before_physics(test->ctx, &test->player, tick, &error)) {
    scene->simulation.error = error;
    return false_v;
  }
  return true_v;
}

static bool8_t player_test_after(VkrScene *scene, uint64_t tick,
                                 void *context) {
  (void)scene;
  (void)tick;
  PlayerTest *test = context;
  fps_player_after_physics(test->ctx, &test->player);
  return true_v;
}

static void player_test_reset(VkrScene *scene, void *context) {
  (void)scene;
  PlayerTest *test = context;
  fps_player_reset(test->ctx, &test->player);
}

/* Opens the test's context on `scene` once; player_shutdown closes it. */
static VkrCtx *player_context(PlayerTest *test, VkrScene *scene,
                              InputState *input) {
  if (!test->ctx) {
    test->ctx =
        vkr_script_host_open_context(&s_script_host, scene, input, NULL);
    test->scene = scene;
  }
  return test->ctx;
}

static bool8_t player_attach(PlayerTest *test, VkrScene *scene,
                             InputState *input, VkrEntityId entity,
                             uint64_t instance_id, float32_t yaw) {
  VkrCtx *ctx = player_context(test, scene, input);
  const FpsPlayerConfig config = {.entity = {entity.u64},
                                  .settings = fps_player_settings_default(),
                                  .yaw = yaw,
                                  .weapon_bone = UINT32_MAX,
                                  .instance_id = instance_id};
  if (!ctx || !fps_player_attach(ctx, &test->player, &config, NULL)) {
    return false_v;
  }
  const VkrSceneSimulationCallbacks callbacks = {
      .before_physics = player_test_before,
      .after_physics = player_test_after,
      .reset = player_test_reset,
      .context = test};
  if (!vkr_scene_simulation_configure(scene, &callbacks, NULL)) {
    fps_player_shutdown(ctx, &test->player);
    return false_v;
  }
  return true_v;
}

static void player_shutdown(PlayerTest *test) {
  VkrScene *scene = test->scene;
  vkr_scene_physics_set_paused(scene, true_v);
  assert(vkr_scene_simulation_detach(scene, test));
  fps_player_shutdown(test->ctx, &test->player);
  vkr_script_host_close_context(&s_script_host);
  test->ctx = NULL;
}

static void player_command(FpsPlayer *player, uint64_t tick, FpsAction action,
                           bool8_t pressed) {
  assert(fps_input_push(
      &player->commands,
      (FpsCommand){.tick = tick,
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
  body.body.motion = VKR_PHYSICS_STATIC;
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
  PlayerTest test = {0};
  FpsPlayer *const player = &test.player;
  assert(player_attach(&test, &scene, &input, entity, 90, 0));
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(fps_player_frame(test.ctx, player, 100, true_v) == 0);
  const float64_t boundary = 100 + VKR_SCENE_SIMULATION_FIXED_DT;
  const float64_t elapsed =
      fps_player_frame(test.ctx, player, boundary, true_v);
  assert(elapsed < VKR_SCENE_SIMULATION_FIXED_DT);
  vkr_scene_update(&scene, elapsed);
  assert(vkr_scene_simulation_completed_ticks(&scene) == 1);
  VkrInputEvent event = {.kind = VKR_INPUT_LOOK, .time = boundary};
  fps_player_observe(test.ctx, player, &event);
  assert(!player->commands.faulted && player->commands.count == 1);
  assert(player->commands.commands[player->commands.head].tick == 2);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(vkr_scene_simulation_completed_ticks(&scene) == 2);

  for (uint32_t i = 0; i < 1024; ++i) {
    event.time = 100.040 + i * 1e-7;
    event.dx = 0.125;
    fps_player_observe(test.ctx, player, &event);
  }
  assert(!player->commands.faulted && player->commands.count == 1);
  event = (VkrInputEvent){.kind = VKR_INPUT_BUTTON,
                          .time = 100.041,
                          .code = VKR_MOUSE_LEFT,
                          .pressed = true_v};
  fps_player_observe(test.ctx, player, &event);
  event = (VkrInputEvent){.kind = VKR_INPUT_LOOK, .dx = -0.125};
  for (uint32_t i = 0; i < 1024; ++i) {
    event.time = 100.042 + i * 1e-7;
    fps_player_observe(test.ctx, player, &event);
  }
  event = (VkrInputEvent){.kind = VKR_INPUT_BUTTON,
                          .time = 100.043,
                          .code = VKR_MOUSE_LEFT,
                          .pressed = false_v};
  fps_player_observe(test.ctx, player, &event);
  assert(!player->commands.faulted && player->commands.count == 4);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(!scene.simulation.faulted);
  assert(player->shots_fired == 1 && player->hits == 1);
  const FpsPlayerState *state = fps_player_state(test.ctx, player);
  assert(state && state->weapon.magazine_rounds == 11);
  assert(fabsf(state->yaw) < 1e-5f);
  assert(!(state->held & (1u << FPS_ACTION_FIRE)));
  assert(player->commands.count == 0);

  // The observer runs after InputState records each physical key transition.
  // Releasing one Ctrl while the other stays held must keep the motor crouched.
  event = (VkrInputEvent){.kind = VKR_INPUT_KEY,
                          .time = 100.060,
                          .code = VKR_KEY_LCONTROL,
                          .pressed = true_v};
  input.current_keys.keys[KEY_LCONTROL] = true_v;
  fps_player_observe(test.ctx, player, &event);
  event.time = 100.061;
  event.code = VKR_KEY_RCONTROL;
  input.current_keys.keys[KEY_RCONTROL] = true_v;
  fps_player_observe(test.ctx, player, &event);
  event.time = 100.062;
  event.code = VKR_KEY_LCONTROL;
  event.pressed = false_v;
  input.current_keys.keys[KEY_LCONTROL] = false_v;
  fps_player_observe(test.ctx, player, &event);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(!scene.simulation.faulted);
  VkrPhysicsCharacterState motor;
  assert(vkr_scene_character_get_state(&scene, entity, &motor, NULL));
  assert(motor.crouched && state->crouched);
  FpsCameraRigPose camera;
  assert(fps_player_camera(test.ctx, player, &camera));
  assert(camera.position.y - player->current_foot.y < 1.5f);
  event.time = 100.070;
  event.code = VKR_KEY_RCONTROL;
  input.current_keys.keys[KEY_RCONTROL] = false_v;
  fps_player_observe(test.ctx, player, &event);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(!scene.simulation.faulted);
  assert(vkr_scene_character_get_state(&scene, entity, &motor, NULL));
  assert(!motor.crouched && !state->crouched);

  // Captured look reports upward motion as positive delta_y; it looks up.
  const float32_t pitch_before = player->render_pitch;
  event = (VkrInputEvent){.kind = VKR_INPUT_LOOK, .time = 100.080, .dy = 40};
  fps_player_observe(test.ctx, player, &event);
  assert(player->render_pitch > pitch_before);
  player_shutdown(&test);
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
  PlayerTest test = {0};
  FpsPlayer *const player = &test.player;
  assert(player_attach(&test, &scene, &input, entity, 100, 0));
  FpsPlayerState *state = fps_player_state(test.ctx, player);
  assert(state);
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(fps_player_frame(test.ctx, player, 100, true_v) == 0);
  const VkrInputEvent press = {.time = 100.001,
                               .kind = VKR_INPUT_BUTTON,
                               .code = VKR_MOUSE_LEFT,
                               .pressed = true_v};
  fps_player_observe(test.ctx, player, &press);
  state->held = 1u << FPS_ACTION_FORWARD;

  // UI input capture cancels player intent, but cannot freeze a running world.
  for (uint32_t tick = 1; tick <= 8; ++tick) {
    const float64_t now = 100 + tick * VKR_SCENE_SIMULATION_FIXED_DT;
    const float64_t dt = fps_player_frame(test.ctx, player, now, false_v);
    assert(fabs(dt - VKR_SCENE_SIMULATION_FIXED_DT) < 1e-12);
    vkr_scene_update(&scene, dt);
    assert(vkr_scene_simulation_completed_ticks(&scene) == tick);
    assert(!scene.physics_paused && !scene.simulation.faulted);
    assert(!player->active && !state->held && !player->commands.count);
  }
  VkrPhysicsPose pose;
  assert(vkr_scene_physics_get_pose(&scene, falling, &pose));
  assert(pose.position[1] < 8 && player->current_foot.y < 8);
  assert(player->shots_fired == 0);

  VkrInputEvent ignored = press;
  ignored.time = 100 + 8.5 * VKR_SCENE_SIMULATION_FIXED_DT;
  fps_player_observe(test.ctx, player, &ignored);
  assert(!player->commands.count);
  const float64_t resumed = 100 + 9 * VKR_SCENE_SIMULATION_FIXED_DT;
  vkr_scene_update(&scene, fps_player_frame(test.ctx, player, resumed, true_v));
  assert(vkr_scene_simulation_completed_ticks(&scene) == 9);
  assert(player->active && player->shots_fired == 0);

  // A fresh press after focus returns retains the shared scene tick mapping.
  VkrInputEvent fresh = press;
  fresh.time = resumed + 0.5 * VKR_SCENE_SIMULATION_FIXED_DT;
  fps_player_observe(test.ctx, player, &fresh);
  assert(!player->commands.faulted && player->commands.count == 1);
  assert(player->commands.commands[player->commands.head].tick == 10);
  vkr_scene_update(&scene,
                   fps_player_frame(test.ctx, player,
                                    100 + 10 * VKR_SCENE_SIMULATION_FIXED_DT,
                                    true_v));
  assert(vkr_scene_simulation_completed_ticks(&scene) == 10);
  assert(!scene.simulation.faulted && player->shots_fired == 1);

  // Explicit pause still excludes the paused wall-time span on resume.
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(fps_player_frame(test.ctx, player, 150, true_v) == 0);
  assert(!player->active && !state->held && !player->commands.count);
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(fps_player_frame(test.ctx, player, 200, true_v) == 0);
  fresh.time = 200 + 0.5 * VKR_SCENE_SIMULATION_FIXED_DT;
  fps_player_observe(test.ctx, player, &fresh);
  assert(!player->commands.faulted && player->commands.count == 1);
  assert(player->commands.commands[player->commands.head].tick == 11);
  vkr_scene_update(&scene, fps_player_frame(test.ctx, player,
                                            200 + VKR_SCENE_SIMULATION_FIXED_DT,
                                            true_v));
  assert(vkr_scene_simulation_completed_ticks(&scene) == 11);
  assert(!scene.simulation.faulted);
  player_shutdown(&test);
  vkr_scene_shutdown(&scene, NULL);
}

/* A static box body for a test level. */
static VkrEntityId player_test_box(VkrScene *scene, Vec3 center, Vec3 half) {
  const VkrEntityId entity = vkr_scene_create_entity(scene, NULL);
  assert(vkr_scene_set_transform(scene, entity, center, vkr_quat_identity(),
                                 vec3_one()));
  VkrScenePhysicsSnapshot body = vkr_scene_physics_default();
  body.body.motion = VKR_PHYSICS_STATIC;
  body.colliders[0].half_extent = half;
  assert(vkr_scene_physics_apply(scene, entity, &body, NULL));
  return entity;
}

/* A ladder (ADR-073): a sensor carrying the FPS module's fps_ladder in
 * front of a wall the player faces. Oracle: holding forward for one second
 * lifts the feet above 1.5 m at the 2.5 m/s climb speed; the same push
 * against the bare wall leaves them on the floor, so the climb comes from
 * the ladder, not a jump or the wall. */
static void test_player_ladder(VkrAllocator *allocator, bool8_t ladder) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 48, 16, NULL));
  InputState input = {0};
  (void)player_test_box(&scene, vec3_new(0, -.5f, 0), vec3_new(20, .5f, 20));
  (void)player_test_box(&scene, vec3_new(1.2f, 2, 0), vec3_new(.2f, 2, 2));
  if (ladder) {
    const VkrTypeDesc *type =
        vkr_scene_world_type_named(string8_lit("fps_ladder"));
    assert(type);
    const VkrEntityId rungs = vkr_scene_create_entity(&scene, NULL);
    assert(vkr_scene_set_transform(&scene, rungs, vec3_new(.7f, 2, 0),
                                   vkr_quat_identity(), vec3_one()));
    const FpsLadder settings = {.climb_speed = 2.5f};
    assert(vkr_scene_set_typed(&scene, rungs, type, &settings));
    const VkrPhysicsColliderDesc sensor = {.entity_id = rungs.u64,
                                           .shape = VKR_PHYSICS_BOX,
                                           .position = {.7f, 2, 0},
                                           .rotation = {0, 0, 0, 1},
                                           .scale = {1, 1, 1},
                                           .half_extent = {.3f, 2, .6f},
                                           .enabled = true_v};
    assert(vkr_scene_physics_generated_set(&scene, 21u, rungs, &sensor, 1u,
                                           true_v, NULL));
  }
  const VkrEntityId entity = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, entity, vec3_new(0, .1f, 0),
                                 vkr_quat_identity(), vec3_one()));
  PlayerTest test = {0};
  FpsPlayer *const player = &test.player;
  assert(player_attach(&test, &scene, &input, entity, 90, 0));
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(fps_player_frame(test.ctx, player, 100, true_v) == 0);
  player_command(player, 1, FPS_ACTION_FORWARD, true_v);
  for (uint32_t tick = 0; tick < 60u; ++tick) {
    vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  }
  assert(!scene.simulation.faulted);
  assert(ladder ? player->current_foot.y > 1.5f
                : player->current_foot.y < 0.2f);
  player_shutdown(&test);
  vkr_scene_shutdown(&scene, NULL);
}

/* Ticks a player stands on a deck before it sets off. */
#define DECK_SETTLE_TICKS 10u

/* Ticks of `count` from the one after `start` that have run by `tick`. */
static float32_t deck_ticks(uint64_t tick, uint64_t start, uint64_t count) {
  if (tick <= start) {
    return 0.0f;
  }
  return (float32_t)Min(tick - start, count);
}

/* A tram 8 m/s east for two seconds, then stopped. */
static void deck_tram(uint64_t tick, Vec3 *offset, VkrQuat *rotation) {
  (void)rotation;
  const float32_t dt = (float32_t)VKR_SCENE_SIMULATION_FIXED_DT;
  *offset =
      vec3_new(8.0f * dt * deck_ticks(tick, DECK_SETTLE_TICKS, 120u), 0, 0);
}

/* An elevator 1.5 m/s up for two seconds, half a second at the top, then
   down again. */
static void deck_elevator(uint64_t tick, Vec3 *offset, VkrQuat *rotation) {
  (void)rotation;
  const float32_t dt = (float32_t)VKR_SCENE_SIMULATION_FIXED_DT;
  const float32_t up = deck_ticks(tick, DECK_SETTLE_TICKS, 120u);
  const float32_t down = deck_ticks(tick, DECK_SETTLE_TICKS + 150u, 120u);
  *offset = vec3_new(0, 1.5f * dt * (up - down), 0);
}

/* A floor turning 90 degrees a second about +Y for one second. */
static void deck_turn(uint64_t tick, Vec3 *offset, VkrQuat *rotation) {
  (void)offset;
  const float32_t dt = (float32_t)VKR_SCENE_SIMULATION_FIXED_DT;
  *rotation = vkr_quat_from_axis_angle(
      vec3_new(0, 1, 0), 0.5f * VKR_PI * dt * deck_ticks(tick, 10u, 60u));
}

/* A player standing at `foot` on a kinematic box deck whose top rests at
   y = 0, its drawn pose and body moved by `motion` each tick. */
static void player_deck_begin(PlayerTest *test, VkrScene *scene,
                              VkrAllocator *allocator, InputState *input,
                              uint32_t seed, Vec3 half, Vec3 foot,
                              void (*motion)(uint64_t, Vec3 *, VkrQuat *)) {
  assert(vkr_scene_init(scene, allocator, seed, 16, NULL));
  const VkrEntityId deck = vkr_scene_create_entity(scene, NULL);
  assert(vkr_scene_set_transform(scene, deck, vec3_zero(), vkr_quat_identity(),
                                 vec3_one()));
  /* Publication adds a mover's evaluated pose; a tick only rewrites it. */
  const Mat4 rest = mat4_identity();
  assert(vkr_scene_set_evaluated_transform(scene, deck, &rest));
  const VkrPhysicsColliderDesc box = {.entity_id = deck.u64,
                                      .shape = VKR_PHYSICS_BOX,
                                      .position = {0, -half.y, 0},
                                      .rotation = {0, 0, 0, 1},
                                      .scale = {1, 1, 1},
                                      .half_extent = {half.x, half.y, half.z},
                                      .enabled = true_v};
  const uint64_t key = 22u;
  assert(vkr_scene_physics_generated_set_kinematic(scene, key, deck, &box, 1u,
                                                   false_v, NULL));
  const VkrEntityId entity = vkr_scene_create_entity(scene, NULL);
  assert(vkr_scene_set_transform(scene, entity, foot, vkr_quat_identity(),
                                 vec3_one()));
  assert(player_attach(test, scene, input, entity, 90u + seed, 0));
  test->entity = entity;
  test->deck = (PlayerTestDeck){.key = key, .entity = deck, .motion = motion};
  vkr_scene_physics_set_paused(scene, false_v);
  assert(fps_player_frame(test->ctx, &test->player, 100, true_v) == 0);
}

static void player_deck_end(PlayerTest *test, VkrScene *scene) {
  assert(!scene->simulation.faulted);
  player_shutdown(test);
  vkr_scene_shutdown(scene, NULL);
}

/* A tram deck at 8 m/s (ADR-084 movers, ADR-073 riding). Oracle: the
 * deck's own pose each tick; the player's offset from it moves less than a
 * millimetre in any tick, while it sets off, rides and stops, and the
 * player stays grounded. A player one tick behind the deck slips back
 * 8/60 m as it sets off and on as it stops. */
static void test_player_tram(VkrAllocator *allocator) {
  VkrScene scene;
  InputState input = {0};
  PlayerTest test = {0};
  player_deck_begin(&test, &scene, allocator, &input, 50, vec3_new(20, .25f, 3),
                    vec3_new(0, .05f, 0), deck_tram);
  const FpsPlayerState *state = fps_player_state(test.ctx, &test.player);
  assert(state);
  Vec3 last = vec3_zero();
  float32_t worst = 0.0f;
  bool8_t grounded = true_v;
  for (uint64_t tick = 1; tick <= 180u; ++tick) {
    vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
    assert(scene.simulation.completed_ticks == tick);
    Vec3 deck = vec3_zero();
    VkrQuat rotation = vkr_quat_identity();
    deck_tram(tick, &deck, &rotation);
    const Vec3 offset = vec3_sub(test.player.current_foot, deck);
    if (tick > DECK_SETTLE_TICKS) {
      grounded = grounded && state->grounded;
      worst = Max(worst, vec3_length(vec3_sub(offset, last)));
    }
    last = offset;
  }
  assert(grounded && worst < 1e-3f);
  assert(test.player.current_foot.x > 15.9f);
  player_deck_end(&test, &scene);
}

/* The drawn tram deck and player (ADR-084, ADR-073), read every half
 * tick. Oracle: the player's drawn offset from the drawn deck stays within
 * a millimetre of where it stood as the deck sets off, rides at 8 m/s and
 * stops: both draw the same share of the way between their last two
 * ticks. A deck drawn at its latest tick runs half a tick, 6.7 cm, ahead of
 * the player at each half tick. */
static void test_player_tram_drawn(VkrAllocator *allocator) {
  VkrScene scene;
  InputState input = {0};
  PlayerTest test = {0};
  player_deck_begin(&test, &scene, allocator, &input, 52, vec3_new(20, .25f, 3),
                    vec3_new(0, .05f, 0), deck_tram);
  Vec3 rest = vec3_zero();
  float32_t worst = 0.0f;
  for (uint32_t frame = 1; frame <= 360u; ++frame) {
    vkr_scene_update(&scene, 0.5 * VKR_SCENE_SIMULATION_FIXED_DT);
    const SceneTransform *player = vkr_scene_get_transform(&scene, test.entity);
    const SceneTransform *deck =
        vkr_scene_get_transform(&scene, test.deck.entity);
    assert(player && deck);
    const Vec3 offset =
        vec3_sub(mat4_position(player->world), mat4_position(deck->world));
    if (frame == 2u * DECK_SETTLE_TICKS) {
      rest = offset;
    } else if (frame > 2u * DECK_SETTLE_TICKS) {
      worst = Max(worst, vec3_length(vec3_sub(offset, rest)));
    }
  }
  assert(scene.simulation.completed_ticks == 180u);
  assert(worst < 1e-3f);
  player_deck_end(&test, &scene);
}

/* A jump on the tram deck at 8 m/s (ADR-073). Oracle: the deck's pose
 * each tick; the player leaves the deck for at least a quarter second and
 * lands within 2 cm of where it took off on the deck, which moved about
 * 4 m meanwhile. A player that drops the deck's velocity on lift-off lands
 * metres behind. */
static void test_player_tram_jump(VkrAllocator *allocator) {
  VkrScene scene;
  InputState input = {0};
  PlayerTest test = {0};
  player_deck_begin(&test, &scene, allocator, &input, 53, vec3_new(20, .25f, 3),
                    vec3_new(0, .05f, 0), deck_tram);
  const FpsPlayerState *state = fps_player_state(test.ctx, &test.player);
  assert(state);
  player_command(&test.player, 30u, FPS_ACTION_JUMP, true_v);
  player_command(&test.player, 31u, FPS_ACTION_JUMP, false_v);
  Vec3 takeoff = vec3_zero();
  uint32_t airborne = 0u;
  bool8_t landed = false_v;
  for (uint64_t tick = 1; tick <= 120u && !landed; ++tick) {
    vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
    Vec3 deck = vec3_zero();
    VkrQuat rotation = vkr_quat_identity();
    deck_tram(tick, &deck, &rotation);
    const Vec3 offset = vec3_sub(test.player.current_foot, deck);
    if (tick == 29u) {
      takeoff = offset;
    } else if (tick >= 30u && !state->grounded) {
      airborne++;
    } else if (tick > 30u) {
      landed = true_v;
      assert(fabsf(offset.x - takeoff.x) < 2e-2f &&
             fabsf(offset.z - takeoff.z) < 2e-2f);
    }
  }
  assert(landed && airborne >= 15u);
  player_deck_end(&test, &scene);
}

/* An elevator deck 1.5 m/s up and down. Oracle: the deck's height each
 * tick; the player stays grounded on it every tick, its feet within 5 mm of
 * where they rested on the deck. A player stepped against the deck's pose
 * before the step floats a tick's descent, 2.5 cm, above it going down. */
static void test_player_elevator(VkrAllocator *allocator) {
  VkrScene scene;
  InputState input = {0};
  PlayerTest test = {0};
  player_deck_begin(&test, &scene, allocator, &input, 51, vec3_new(2, .25f, 2),
                    vec3_new(0, .05f, 0), deck_elevator);
  const FpsPlayerState *state = fps_player_state(test.ctx, &test.player);
  assert(state);
  float32_t rest = 0.0f;
  float32_t worst = 0.0f;
  bool8_t grounded = true_v;
  for (uint64_t tick = 1; tick <= 300u; ++tick) {
    vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
    Vec3 deck = vec3_zero();
    VkrQuat rotation = vkr_quat_identity();
    deck_elevator(tick, &deck, &rotation);
    const float32_t gap = test.player.current_foot.y - deck.y;
    if (tick == DECK_SETTLE_TICKS) {
      rest = gap;
    } else if (tick > DECK_SETTLE_TICKS) {
      grounded = grounded && state->grounded;
      worst = Max(worst, fabsf(gap - rest));
    }
  }
  assert(grounded && worst < 5e-3f);
  player_deck_end(&test, &scene);
}

/* A turning platform (ADR-084 movers): a kinematic floor turning 90 degrees
 * a second about +Y under a player standing 1.5 m east of its axis. Oracle:
 * after one second the player stands a quarter turn on, 1.5 m north (+X
 * turns to -Z), within a millimetre, and its yaw turned with it from 0 to
 * -pi/2; a player a tick behind the floor stays 4 cm and 1.5 degrees
 * short, and one it neither carries nor turns stays east, facing east. */
static void test_player_turning_platform(VkrAllocator *allocator) {
  VkrScene scene;
  InputState input = {0};
  PlayerTest test = {0};
  player_deck_begin(&test, &scene, allocator, &input, 49, vec3_new(3, .25f, 3),
                    vec3_new(1.5f, .05f, 0), deck_turn);
  for (uint32_t tick = 0; tick < 70u; ++tick) {
    vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  }
  const FpsPlayer *player = &test.player;
  const FpsPlayerState *state = fps_player_state(test.ctx, player);
  assert(state);
  assert(fabsf(player->current_foot.x) < 1e-3f &&
         fabsf(player->current_foot.z + 1.5f) < 1e-3f);
  assert(fabsf(state->yaw + 0.5f * VKR_PI) < 1e-3f);
  player_deck_end(&test, &scene);
}

static void test_player_authored_camera_mode(VkrAllocator *allocator) {
  VkrScene scene = {0};
  assert(vkr_scene_init(&scene, allocator, 1, 8, NULL));
  const VkrEntityId entity = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, entity, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  vkr_scene_physics_set_paused(&scene, true_v);
  InputState input = {0};
  VkrCtx *ctx =
      vkr_script_host_open_context(&s_script_host, &scene, &input, NULL);
  FpsPlayerConfig config = {.entity = {entity.u64},
                            .settings = fps_player_settings_default(),
                            .weapon_bone = UINT32_MAX,
                            .instance_id = 101};
  config.settings.camera_mode = FPS_CAMERA_RIG_THIRD_PERSON;
  FpsPlayer player = {0};
  assert(fps_player_attach(ctx, &player, &config, NULL));
  FpsCameraRigPose pose = {0};
  assert(fps_player_camera(ctx, &player, &pose));
  assert(player.camera.mode == FPS_CAMERA_RIG_THIRD_PERSON);
  assert(fabsf(pose.position.x + 4.0f) < 0.001f);
  fps_player_shutdown(ctx, &player);

  // A mode outside the rig's range fails attach and leaves nothing behind.
  config.settings.camera_mode = 3;
  assert(!fps_player_attach(ctx, &player, &config, NULL));
  config.settings.camera_mode = FPS_CAMERA_RIG_FIRST_PERSON;
  assert(fps_player_attach(ctx, &player, &config, NULL));
  assert(player.camera.mode == FPS_CAMERA_RIG_FIRST_PERSON);
  fps_player_shutdown(ctx, &player);
  vkr_script_host_close_context(&s_script_host);
  vkr_scene_shutdown(&scene, NULL);
}

bool32_t run_gameplay_player_tests(void) {
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  assert(vkr_dmemory_create(MB(1), MB(4), &s_module_memory));
  s_module_allocator = (VkrAllocator){.ctx = &s_module_memory};
  vkr_dmemory_allocator_create(&s_module_allocator);
  assert(vkr_script_host_init(&s_script_host, &s_module_allocator));
  /* The FPS module's component types, as the editor registers them. */
  const char *module_error = NULL;
  assert(vkr_script_host_add_module(&s_script_host, vkr_module_fps,
                                    &module_error));
  test_player_ladder(&allocator, true_v);
  test_player_ladder(&allocator, false_v);
  test_player_turning_platform(&allocator);
  test_player_tram(&allocator);
  test_player_elevator(&allocator);
  test_player_tram_drawn(&allocator);
  test_player_tram_jump(&allocator);
  test_player_evaluated_transforms(&allocator);
  test_player_observer_bursts(&allocator);
  test_player_unfocused_simulation(&allocator);
  test_player_authored_camera_mode(&allocator);
  VkrScene scene;
  assert(vkr_scene_init(&scene, &allocator, 44, 16, NULL));
  InputState input = {0};
  VkrEntityId floor = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, floor, vec3_new(0, -.5f, 0),
                                 vkr_quat_identity(), vec3_one()));
  VkrScenePhysicsSnapshot body = vkr_scene_physics_default();
  body.body.motion = VKR_PHYSICS_STATIC;
  body.colliders[0].half_extent = vec3_new(20, .5f, 20);
  assert(vkr_scene_physics_apply(&scene, floor, &body, NULL));
  VkrEntityId entity = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, entity, vec3_new(0, .1f, 0),
                                 vkr_quat_identity(), vec3_one()));
  PlayerTest test = {0};
  FpsPlayer *const player = &test.player;
  assert(player_attach(&test, &scene, &input, entity, 70, 0.25f));
  assert(!player_attach(&test, &scene, &input, entity, 80, 0));
  FpsPlayerState *state = fps_player_state(test.ctx, player);
  assert(state && state->weapon.magazine_rounds == 12);
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(fps_player_frame(test.ctx, player, 100, true_v) == 0);
  // Complete short presses must survive a frame with no simulation tick.
  const VkrInputEvent press = {.time = 100.001,
                               .kind = VKR_INPUT_BUTTON,
                               .code = VKR_MOUSE_LEFT,
                               .pressed = true_v};
  fps_player_observe(test.ctx, player, &press);
  VkrInputEvent release = press;
  release.time = 100.002;
  release.pressed = false_v;
  fps_player_observe(test.ctx, player, &release);
  vkr_scene_update(&scene, 0);
  assert(player->shots_fired == 0);
  vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
  assert(player->shots_fired == 1 && state->weapon.magazine_rounds == 11);
  assert(!(state->held & (1u << FPS_ACTION_FIRE)));
  player_command(player, 2, FPS_ACTION_FORWARD, true_v);
  player_command(player, 2, FPS_ACTION_RELOAD, true_v);
  player_command(player, 32, FPS_ACTION_FORWARD, false_v);
  for (uint32_t tick = 2; tick <= 62; ++tick) {
    vkr_scene_update(&scene, VKR_SCENE_SIMULATION_FIXED_DT);
    assert(!scene.simulation.faulted);
  }
  assert(fabsf(player->current_foot.x - 2.422281f) < .05f);
  assert(fabsf(player->current_foot.z - 0.618510f) < .05f);
  assert(fabsf(player->current_foot.y) < .05f);
  assert(state->weapon.magazine_rounds == 12 && state->reserve_rounds == 119);
  FpsCameraRigPose camera;
  assert(fps_player_camera(test.ctx, player, &camera));
  assert(fabsf(camera.position.y - 1.6f) < .06f);
  // Pause invalidates pending/held input without admitting the paused wall
  // span.
  state->held = 1u << FPS_ACTION_FIRE;
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(fps_player_frame(test.ctx, player, 150, true_v) == 0);
  assert(!player->active && state->held == 0 && player->commands.count == 0);
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(fps_player_frame(test.ctx, player, 200, true_v) == 0);
  assert(player->active);
  assert(!vkr_scene_simulation_detach(&scene, &input));
  vkr_scene_physics_set_paused(&scene, true_v);
  player->render_yaw = 1.0f;
  player->render_pitch = 0.5f;
  state->yaw = 1.0f;
  state->pitch = 0.5f;
  assert(vkr_scene_physics_reset(&scene, NULL));
  state = fps_player_state(test.ctx, player);
  assert(state->weapon.instance_id == 71 &&
         state->weapon.magazine_rounds == 12);
  assert(player->commands.consumed_tick == 0 && player->shots_fired == 0);
  assert(player->render_yaw == 0.25f && player->render_pitch == 0);
  assert(state->yaw == 0.25f && state->pitch == 0);
  player_shutdown(&test);
  assert(!input.observer && !scene.simulation.enabled);
  assert(!vkr_scene_character_get_state(&scene, entity,
                                        &(VkrPhysicsCharacterState){0}, NULL));
  vkr_scene_shutdown(&scene, NULL);
  vkr_script_host_shutdown(&s_script_host);
  vkr_dmemory_allocator_destroy(&allocator);
  printf("Gameplay player tests passed\n");
  return true_v;
}
