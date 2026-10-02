#include "player_animation_test.h"

#include "fps_locomotion.h"
#include "fps_mannequin.h"
#include "fps_player.h"
#include "fps_player_animation.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "script/vkr_script_host.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
  TEST_RELOAD,
  TEST_IDLE,
  TEST_RUN,
  TEST_CROUCH_FIRE,
  TEST_WALK,
  TEST_AIR,
  TEST_CROUCH_DOWN,
  TEST_CROUCH_IDLE,
  TEST_CROUCH_UP,
  TEST_FIRE,
  TEST_CROUCH_WALK,
  TEST_LAND,
  TEST_START,
  TEST_UNARMED_IDLE,
  TEST_CLIPS
};

/* The FPS animation code drives an entity's animation through the SDK; a
   tool context binds each test's player to one entity. */
static VkrScriptHost s_script_host;
static VkrCtx *s_ctx;
static VkrEntity s_entity;
static VkrAnimationPlayer *s_bound;

/* Binds `player` to the test entity and returns the context. */
static VkrCtx *test_bind(VkrAnimationPlayer *player) {
  assert(vkr_script_host_bind_animation(
      &s_script_host, (VkrEntityId){.u64 = s_entity.id}, player));
  s_bound = player;
  return s_ctx;
}

typedef struct PlayerAnimationFixture {
  VkrAnimationNode node;
  uint32_t order;
  uint32_t joint;
  Mat4 inverse_bind;
  VkrAnimationSkin skin;
  float32_t times[TEST_CLIPS][2];
  Vec4 values[TEST_CLIPS][2];
  VkrAnimationChannel channels[TEST_CLIPS];
  VkrAnimationClip clips[TEST_CLIPS];
  VkrAnimationAsset asset;
} PlayerAnimationFixture;

static void player_animation_fixture(PlayerAnimationFixture *fixture) {
  *fixture = (PlayerAnimationFixture){0};
  fixture->node = (VkrAnimationNode){
      .parent = VKR_ANIMATION_NO_NODE,
      .local = mat4_identity(),
      .rest = {.rotation = vkr_quat_identity(), .scale = vec3_one()}};
  fixture->inverse_bind = mat4_identity();
  fixture->skin = (VkrAnimationSkin){.skeleton_node = 0,
                                     .joint_count = 1,
                                     .joints = &fixture->joint,
                                     .inverse_bind = &fixture->inverse_bind};
  /* Scrambled bank order proves resolution by name, not Testbed clip indices.
   * Constant, distinct x poses give an independent oracle for real blending. */
  static const char *const names[] = {
      "Rifle_Reload", "Rifle_Aim_Idle", "Rifle_Run",   "Crouch_Rifle_Fire",
      "Rifle_Walk",   "Jump_Loop",      "Crouch_Down", "Crouch_Rifle_Aim",
      "Crouch_Up",    "Rifle_Fire",     "Crouch_Walk", "Jump_Land",
      "Jump_Start",   "Idle",
  };
  const float32_t durations[] = {
      2.8f, 2,    0.7166667f, 0.2333333f, 1,          0.6f, 0.5f,
      2,    0.5f, 0.2333333f, 1.3f,       0.7166667f, 0.6f, 2};
  for (uint32_t i = 0; i < TEST_CLIPS; ++i) {
    fixture->times[i][1] = durations[i];
    fixture->values[i][0].x = (float32_t)i + 1;
    fixture->values[i][1].x = (float32_t)i + 1;
    fixture->channels[i] =
        (VkrAnimationChannel){.node = 0,
                              .path = VKR_ANIMATION_TRANSLATION,
                              .interpolation = VKR_ANIMATION_LINEAR,
                              .key_count = 2,
                              .times = fixture->times[i],
                              .values = fixture->values[i]};
    fixture->clips[i] =
        (VkrAnimationClip){.name = string8_create_from_cstr(
                               (const uint8_t *)names[i], strlen(names[i])),
                           .duration = durations[i],
                           .channel_count = 1,
                           .channels = &fixture->channels[i]};
  }
  fixture->asset = (VkrAnimationAsset){.node_count = 1,
                                       .skin_count = 1,
                                       .clip_count = TEST_CLIPS,
                                       .nodes = &fixture->node,
                                       .node_order = &fixture->order,
                                       .skins = &fixture->skin,
                                       .clips = fixture->clips};
}

static float32_t player_animation_x(VkrAnimationPlayer *player) {
  return vkr_animation_player_global_pose(player)[0].elements[12];
}

static void player_animation_actions(VkrAllocator *scratch) {
  PlayerAnimationFixture fixture;
  player_animation_fixture(&fixture);
  VkrAnimationPlayer *player = vkr_animation_player_create(
      &fixture.asset, scratch, TEST_UNARMED_IDLE, true_v, 1, true_v, NULL);
  assert(player);
  FpsPlayerAnimation animation = {0};
  const char *error = NULL;
  assert(fps_player_animation_initialize(test_bind(player), &animation,
                                         s_entity, 1.0 / 60, &error));
  assert(!error && animation.reload_ticks == 168);
  assert(vkr_animation_player_clip(player) == TEST_IDLE);
  assert(player_animation_x(player) == TEST_IDLE + 1);

  FpsPlayerAnimationInput input = {.grounded = true_v, .speed = 5};
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RUN);
  assert(fabs(vkr_animation_player_rate(player) - 5.0 / 3.4) < 1e-6);
  assert(vkr_animation_player_time(player) == 0);
  assert(player_animation_x(player) == TEST_IDLE + 1);
  assert(vkr_animation_player_advance(player, 0.06));
  assert(fabsf(player_animation_x(player) - 2.5f) < 1e-5f);
  const float64_t time = vkr_animation_player_time(player);
  const uint64_t generation = vkr_animation_player_generation(player);
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_time(player) == time);
  assert(vkr_animation_player_generation(player) == generation);
  assert(vkr_animation_player_advance(player, 0.1));
  assert(player_animation_x(player) == TEST_RUN + 1);

  input.shot_sequence = 1;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_FIRE);
  assert(!vkr_animation_player_loop(player));
  assert(vkr_animation_player_rate(player) == 1);
  assert(vkr_animation_player_advance(player, 0.02));
  const float32_t interrupted_pose = player_animation_x(player);
  assert(fabsf(interrupted_pose - 6.5f) < 1e-5f);
  input.shot_sequence = 2;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(player_animation_x(player) == interrupted_pose);
  assert(vkr_animation_player_time(player) == 0);
  assert(vkr_animation_player_advance(player, 0.04));
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_time(player) == 0.04);
  assert(vkr_animation_player_advance(player, 0.4));
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RUN);

  input.reloading = true_v;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RELOAD);
  assert(vkr_animation_player_rate(player) == 1);
  assert(vkr_animation_player_advance(player, 1.4));
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_time(player) == 1.4);
  assert(vkr_animation_player_advance(player, 2));
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RELOAD);
  assert(vkr_animation_player_time(player) ==
         vkr_animation_player_duration(player));
  input.reloading = false_v;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RUN);
  input.reloading = true_v;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_time(player) == 0);
  assert(vkr_animation_player_advance(player, 0.2));
  input.reloading = false_v;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RUN);

  input.speed = 0;
  input.shot_sequence = 0;
  assert(!fps_player_animation_update(s_ctx, &animation, &input));
  assert(fps_player_animation_reset(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_IDLE);
  assert(vkr_animation_player_time(player) == 0);
  input.grounded = false_v;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_AIR);
  assert(vkr_animation_player_loop(player));
  assert(vkr_animation_player_advance(player, 0.8));
  input.grounded = true_v;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_LAND);
  assert(vkr_animation_player_advance(player, 0.8));
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_IDLE);

  input.crouched = true_v;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_CROUCH_DOWN);
  assert(vkr_animation_player_advance(player, 0.1));
  const float32_t crouch_pose = player_animation_x(player);
  input.crouched = false_v;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_CROUCH_UP);
  assert(player_animation_x(player) == crouch_pose);
  input.crouched = true_v;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_advance(player, 0.5));
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_CROUCH_IDLE);
  input.speed = 0.6f;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_CROUCH_WALK);
  assert(vkr_animation_player_rate(player) == 1);
  input.shot_sequence = 1;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_CROUCH_FIRE);

  const FpsPlayerAnimation saved = animation;
  const uint64_t saved_generation = vkr_animation_player_generation(player);
  input.speed = NAN;
  assert(!fps_player_animation_update(s_ctx, &animation, &input));
  assert(MemCompare(&saved, &animation, sizeof(saved)) == 0);
  assert(vkr_animation_player_generation(player) == saved_generation);
  assert(!fps_player_animation_initialize(test_bind(player), &animation,
                                          s_entity, 0, &error));
  assert(error && MemCompare(&saved, &animation, sizeof(saved)) == 0);
  assert(!fps_player_animation_initialize(test_bind(player), &animation,
                                          s_entity, 1e-100, &error));
  assert(error && MemCompare(&saved, &animation, sizeof(saved)) == 0);
  vkr_animation_player_destroy(player);
}

static void player_animation_fallback(VkrAllocator *scratch) {
  PlayerAnimationFixture fixture;
  player_animation_fixture(&fixture);
  fixture.asset.clip_count = 4;
  fixture.clips[0].name = string8_lit("Pistol_Reload");
  fixture.clips[1].name = string8_lit("Idle");
  fixture.clips[2].name = string8_lit("Walk_Forward");
  fixture.clips[3].name = string8_lit("Pistol_Fire");
  VkrAnimationPlayer *player = vkr_animation_player_create(
      &fixture.asset, scratch, 0, false_v, 1, true_v, NULL);
  assert(player);
  FpsPlayerAnimation animation;
  assert(fps_player_animation_initialize(test_bind(player), &animation,
                                         s_entity, 1.0 / 60, NULL));
  FpsPlayerAnimationInput input = {.grounded = true_v, .speed = 5};
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == 2);
  assert(fabs(vkr_animation_player_rate(player) - 5.0 / 1.3) < 1e-6);
  input.shot_sequence = 1;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == 3);
  input.reloading = true_v;
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == 0);
  vkr_animation_player_destroy(player);

  fixture.asset.clip_count = 1;
  fixture.clips[0].name = string8_lit("Idle");
  player = vkr_animation_player_create(&fixture.asset, scratch, 0, true_v, 1,
                                       true_v, NULL);
  assert(player);
  assert(fps_player_animation_initialize(test_bind(player), &animation,
                                         s_entity, 1.0 / 60, NULL));
  assert(animation.reload_ticks == 0);
  assert(fps_player_animation_update(s_ctx, &animation, &input));
  assert(vkr_animation_player_clip(player) == 0);
  assert(vkr_animation_player_loop(player));
  vkr_animation_player_destroy(player);
}

/* Mannequin clip names in scrambled bank order, without the backward, left
 * and crouch strafe loops. Each clip translates only its own node, x from
 * zero to its duration with y = 1, so a node's blended pose reads back that
 * clip's weight (y) and sample time (x / y) through the real player. */
static const char *const s_locomotion_names[] = {
    "Jump_Land",  "Walk_Right", "Idle",        "Run_Fwd", "Crouch_Walk_Fwd",
    "Jump_Start", "Walk_Fwd",   "Crouch_Idle", "Jog_Fwd", "Jump_Loop"};

enum { LOCOMOTION_CLIPS = ArrayCount(s_locomotion_names) };

typedef struct LocomotionFixture {
  VkrAnimationNode nodes[LOCOMOTION_CLIPS];
  uint32_t order[LOCOMOTION_CLIPS];
  float32_t times[LOCOMOTION_CLIPS][2];
  Vec4 values[LOCOMOTION_CLIPS][2];
  VkrAnimationChannel channels[LOCOMOTION_CLIPS];
  VkrAnimationClip clips[LOCOMOTION_CLIPS];
  VkrAnimationAsset asset;
} LocomotionFixture;

static const FpsMannequinClip *locomotion_table(const char *name) {
  for (uint32_t i = 0; i < ArrayCount(fps_mannequin_clips); ++i) {
    if (strcmp(fps_mannequin_clips[i].name, name) == 0) {
      return &fps_mannequin_clips[i];
    }
  }
  assert(!"clip missing from the mannequin table");
  return NULL;
}

static uint32_t locomotion_index(const char *name) {
  for (uint32_t i = 0; i < LOCOMOTION_CLIPS; ++i) {
    if (strcmp(s_locomotion_names[i], name) == 0) {
      return i;
    }
  }
  assert(!"clip missing from the fixture");
  return 0;
}

static void locomotion_fixture(LocomotionFixture *fixture) {
  *fixture = (LocomotionFixture){0};
  for (uint32_t i = 0; i < LOCOMOTION_CLIPS; ++i) {
    const char *name = s_locomotion_names[i];
    const float32_t duration = locomotion_table(name)->duration;
    fixture->nodes[i] = (VkrAnimationNode){
        .parent = VKR_ANIMATION_NO_NODE,
        .local = mat4_identity(),
        .rest = {.rotation = vkr_quat_identity(), .scale = vec3_one()}};
    fixture->order[i] = i;
    fixture->times[i][1] = duration;
    fixture->values[i][0].y = 1;
    fixture->values[i][1].x = duration;
    fixture->values[i][1].y = 1;
    fixture->channels[i] =
        (VkrAnimationChannel){.node = i,
                              .path = VKR_ANIMATION_TRANSLATION,
                              .interpolation = VKR_ANIMATION_LINEAR,
                              .key_count = 2,
                              .times = fixture->times[i],
                              .values = fixture->values[i]};
    fixture->clips[i] = (VkrAnimationClip){
        .name = string8_create_from_cstr((const uint8_t *)name, strlen(name)),
        .duration = duration,
        .channel_count = 1,
        .channels = &fixture->channels[i]};
  }
  fixture->asset = (VkrAnimationAsset){.node_count = LOCOMOTION_CLIPS,
                                       .clip_count = LOCOMOTION_CLIPS,
                                       .nodes = fixture->nodes,
                                       .node_order = fixture->order,
                                       .clips = fixture->clips};
}

static float32_t locomotion_weight(VkrAnimationPlayer *player,
                                   const char *name) {
  const uint32_t node = locomotion_index(name);
  return vkr_animation_player_global_pose(player)[node].elements[13];
}

static float64_t locomotion_time(VkrAnimationPlayer *player, const char *name) {
  const Mat4 pose =
      vkr_animation_player_global_pose(player)[locomotion_index(name)];
  assert(pose.elements[13] > 1e-4f);
  return (float64_t)pose.elements[12] / pose.elements[13];
}

static void locomotion_run(FpsLocomotion *locomotion,
                           const FpsLocomotionInput *input, uint32_t frames) {
  for (uint32_t i = 0; i < frames; ++i) {
    assert(fps_locomotion_update(s_ctx, locomotion, input, 1.0 / 60));
  }
}

/* Time a loop advanced over one frame, across its wrap. */
static float64_t locomotion_advance(FpsLocomotion *locomotion,
                                    const FpsLocomotionInput *input,
                                    const char *name) {
  const float64_t duration = locomotion_table(name)->duration;
  const float64_t before = locomotion_time(s_bound, name);
  locomotion_run(locomotion, input, 1);
  const float64_t after = locomotion_time(s_bound, name);
  return fmod(after - before + duration, duration);
}

static bool8_t locomotion_near(float64_t value, float64_t expected) {
  return fabs(value - expected) < 1e-4;
}

static void player_animation_locomotion(VkrAllocator *scratch) {
  LocomotionFixture fixture;
  locomotion_fixture(&fixture);
  VkrAnimationPlayer *player = vkr_animation_player_create(
      &fixture.asset, scratch, 0, true_v, 1, true_v, NULL);
  assert(player);
  assert(fps_locomotion_supported(test_bind(player), s_entity));
  FpsLocomotion locomotion;
  const char *error = NULL;
  assert(fps_locomotion_initialize(test_bind(player), &locomotion, s_entity,
                                   &error));
  // Paused, so the scene clock never replaces the pose; posed at Idle.
  assert(!vkr_animation_player_playing(player));
  assert(locomotion_weight(player, "Idle") == 1);
  const float64_t dt = 1.0 / 60;
  const float64_t walk = locomotion_table("Walk_Fwd")->speed;
  const float64_t jog = locomotion_table("Jog_Fwd")->speed;
  const float64_t run = locomotion_table("Run_Fwd")->speed;
  const float64_t walk_duration = locomotion_table("Walk_Fwd")->duration;
  const float64_t jog_duration = locomotion_table("Jog_Fwd")->duration;

  // Standing plays Idle on its own clock.
  FpsLocomotionInput input = {.grounded = true_v};
  locomotion_run(&locomotion, &input, 30);
  assert(locomotion_weight(player, "Idle") == 1);
  assert(locomotion_near(locomotion_time(player, "Idle"), 30 * dt));

  // At a loop's authored speed it plays at its own rate: planted feet stay
  // planted. Three seconds settle the velocity smoothing.
  input.forward = (float32_t)walk;
  locomotion_run(&locomotion, &input, 180);
  assert(locomotion_near(locomotion_weight(player, "Walk_Fwd"), 1));
  assert(locomotion_weight(player, "Idle") < 1e-4f);
  assert(
      locomotion_near(locomotion_advance(&locomotion, &input, "Walk_Fwd"), dt));

  // Between gaits both loops share one phase, which advances by distance
  // over the blended stride.
  input.forward = (float32_t)((walk + jog) * 0.5);
  locomotion_run(&locomotion, &input, 180);
  assert(locomotion_near(locomotion_weight(player, "Walk_Fwd"), 0.5));
  assert(locomotion_near(locomotion_weight(player, "Jog_Fwd"), 0.5));
  assert(locomotion_near(locomotion_time(player, "Walk_Fwd") / walk_duration,
                         locomotion_time(player, "Jog_Fwd") / jog_duration));
  const float64_t stride =
      0.5 * walk * walk_duration + 0.5 * jog * jog_duration;
  assert(locomotion_near(locomotion_advance(&locomotion, &input, "Walk_Fwd"),
                         input.forward * dt / stride * walk_duration));

  // Past the fastest loop the run plays faster instead of sliding.
  input.forward = 5;
  locomotion_run(&locomotion, &input, 180);
  assert(locomotion_near(locomotion_weight(player, "Run_Fwd"), 1));
  assert(locomotion_near(locomotion_advance(&locomotion, &input, "Run_Fwd"),
                         5 * dt / run));

  // A sidestep uses its direction's loop; a missing one falls back to the
  // forward walk.
  input.forward = 0;
  input.right = locomotion_table("Walk_Right")->speed;
  locomotion_run(&locomotion, &input, 180);
  assert(locomotion_near(locomotion_weight(player, "Walk_Right"), 1));
  input.right = -(float32_t)walk;
  locomotion_run(&locomotion, &input, 180);
  assert(locomotion_near(locomotion_weight(player, "Walk_Fwd"), 1));

  // A jump plays Jump_Start, then Jump_Loop once the start runs out.
  input = (FpsLocomotionInput){.grounded = true_v};
  locomotion_run(&locomotion, &input, 180);
  input = (FpsLocomotionInput){.up = 5};
  locomotion_run(&locomotion, &input, 12);
  assert(locomotion_near(locomotion_weight(player, "Jump_Start"), 1));
  assert(locomotion_near(locomotion_time(player, "Jump_Start"), 11 * dt));
  locomotion_run(&locomotion, &input, 30);
  assert(locomotion_near(locomotion_weight(player, "Jump_Loop"), 1));
  assert(locomotion_weight(player, "Jump_Start") < 1e-4f);

  // Touchdown after a real fall hands the air share to Jump_Land alone; no
  // ground loop leaks into it.
  input = (FpsLocomotionInput){.grounded = true_v};
  locomotion_run(&locomotion, &input, 1);
  const float64_t air_left = 1.0 - dt / 0.06;
  assert(locomotion_near(locomotion_weight(player, "Jump_Loop"), air_left));
  assert(locomotion_near(locomotion_weight(player, "Jump_Land"), 1 - air_left));
  assert(locomotion_near(locomotion_time(player, "Jump_Land"), 0.1));
  assert(locomotion_weight(player, "Idle") < 1e-4f);
  locomotion_run(&locomotion, &input, 4);
  assert(locomotion_near(locomotion_weight(player, "Jump_Land"), 1));
  locomotion_run(&locomotion, &input, 60);
  assert(locomotion_near(locomotion_weight(player, "Idle"), 1));

  // A short hop lands without Jump_Land.
  input = (FpsLocomotionInput){0};
  locomotion_run(&locomotion, &input, 6);
  input.grounded = true_v;
  locomotion_run(&locomotion, &input, 1);
  assert(locomotion_weight(player, "Jump_Land") < 1e-4f);
  assert(locomotion_weight(player, "Idle") > 0.2f);

  // Crouching blends into the crouch stance's own loops.
  input = (FpsLocomotionInput){.grounded = true_v, .crouched = true_v};
  locomotion_run(&locomotion, &input, 30);
  assert(locomotion_near(locomotion_weight(player, "Crouch_Idle"), 1));
  input.forward = locomotion_table("Crouch_Walk_Fwd")->speed;
  locomotion_run(&locomotion, &input, 180);
  assert(locomotion_near(locomotion_weight(player, "Crouch_Walk_Fwd"), 1));

  // Nonfinite input and negative time are refused.
  input.forward = NAN;
  assert(!fps_locomotion_update(s_ctx, &locomotion, &input, dt));
  input.forward = 0;
  assert(!fps_locomotion_update(s_ctx, &locomotion, &input, -dt));

  // A body poses at the root's render time, `alpha` into the latest tick,
  // never ahead of it and never twice for one time.
  static FpsPlayer body;
  body = (FpsPlayer){.locomotion = locomotion,
                     .locomotion_input = {.grounded = true_v},
                     .locomotion_active = true_v,
                     .steps = 3};
  assert(fps_locomotion_reset(s_ctx, &body.locomotion));
  fps_player_animate(s_ctx, &body, 0.5f);
  assert(locomotion_near(body.pose_time, 2.5 * dt));
  assert(locomotion_near(locomotion_time(player, "Idle"), 2.5 * dt));
  fps_player_animate(s_ctx, &body, 0.5f);
  assert(locomotion_near(locomotion_time(player, "Idle"), 2.5 * dt));
  body.steps = 4;
  fps_player_animate(s_ctx, &body, 0.25f);
  assert(locomotion_near(locomotion_time(player, "Idle"), 3.25 * dt));
  vkr_animation_player_destroy(player);
}

bool32_t run_player_animation_tests(void) {
  printf("Running player animation tests...\n");
  Arena *arena = arena_create(MB(1), KB(64));
  assert(arena);
  VkrAllocator scratch = {.ctx = arena};
  assert(vkr_allocator_arena(&scratch));
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  assert(vkr_script_host_init(&s_script_host, &allocator));
  VkrScene scene;
  assert(vkr_scene_init(&scene, &allocator, 48, 16, NULL));
  s_entity = (VkrEntity){.id = vkr_scene_create_entity(&scene, NULL).u64};
  s_ctx = vkr_script_host_open_context(&s_script_host, &scene, NULL, NULL);
  assert(s_ctx);
  player_animation_actions(&scratch);
  player_animation_fallback(&scratch);
  player_animation_locomotion(&scratch);
  vkr_script_host_close_context(&s_script_host);
  vkr_scene_shutdown(&scene, NULL);
  vkr_script_host_shutdown(&s_script_host);
  vkr_dmemory_allocator_destroy(&allocator);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(arena);
  printf("Player animation tests passed.\n");
  return true_v;
}
