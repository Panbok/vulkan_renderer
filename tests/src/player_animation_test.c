#include "player_animation_test.h"

#include "gameplay/vkr_player_animation.h"
#include "memory/vkr_arena_allocator.h"
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
  VkrPlayerAnimation animation = {0};
  const char *error = NULL;
  assert(vkr_player_animation_initialize(&animation, player, 1.0 / 60, &error));
  assert(!error && animation.reload_ticks == 168);
  assert(vkr_animation_player_clip(player) == TEST_IDLE);
  assert(player_animation_x(player) == TEST_IDLE + 1);

  VkrPlayerAnimationInput input = {.grounded = true_v, .speed = 5};
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RUN);
  assert(fabs(vkr_animation_player_rate(player) - 5.0 / 3.4) < 1e-6);
  assert(vkr_animation_player_time(player) == 0);
  assert(player_animation_x(player) == TEST_IDLE + 1);
  assert(vkr_animation_player_advance(player, 0.06));
  assert(fabsf(player_animation_x(player) - 2.5f) < 1e-5f);
  const float64_t time = vkr_animation_player_time(player);
  const uint64_t generation = vkr_animation_player_generation(player);
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_time(player) == time);
  assert(vkr_animation_player_generation(player) == generation);
  assert(vkr_animation_player_advance(player, 0.1));
  assert(player_animation_x(player) == TEST_RUN + 1);

  input.shot_sequence = 1;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_FIRE);
  assert(!vkr_animation_player_loop(player));
  assert(vkr_animation_player_rate(player) == 1);
  assert(vkr_animation_player_advance(player, 0.02));
  const float32_t interrupted_pose = player_animation_x(player);
  assert(fabsf(interrupted_pose - 6.5f) < 1e-5f);
  input.shot_sequence = 2;
  assert(vkr_player_animation_update(&animation, &input));
  assert(player_animation_x(player) == interrupted_pose);
  assert(vkr_animation_player_time(player) == 0);
  assert(vkr_animation_player_advance(player, 0.04));
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_time(player) == 0.04);
  assert(vkr_animation_player_advance(player, 0.4));
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RUN);

  input.reloading = true_v;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RELOAD);
  assert(vkr_animation_player_rate(player) == 1);
  assert(vkr_animation_player_advance(player, 1.4));
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_time(player) == 1.4);
  assert(vkr_animation_player_advance(player, 2));
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RELOAD);
  assert(vkr_animation_player_time(player) ==
         vkr_animation_player_duration(player));
  input.reloading = false_v;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RUN);
  input.reloading = true_v;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_time(player) == 0);
  assert(vkr_animation_player_advance(player, 0.2));
  input.reloading = false_v;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_RUN);

  input.speed = 0;
  input.shot_sequence = 0;
  assert(!vkr_player_animation_update(&animation, &input));
  assert(vkr_player_animation_reset(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_IDLE);
  assert(vkr_animation_player_time(player) == 0);
  input.grounded = false_v;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_AIR);
  assert(vkr_animation_player_loop(player));
  assert(vkr_animation_player_advance(player, 0.8));
  input.grounded = true_v;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_LAND);
  assert(vkr_animation_player_advance(player, 0.8));
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_IDLE);

  input.crouched = true_v;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_CROUCH_DOWN);
  assert(vkr_animation_player_advance(player, 0.1));
  const float32_t crouch_pose = player_animation_x(player);
  input.crouched = false_v;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_CROUCH_UP);
  assert(player_animation_x(player) == crouch_pose);
  input.crouched = true_v;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_advance(player, 0.5));
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_CROUCH_IDLE);
  input.speed = 0.6f;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_CROUCH_WALK);
  assert(vkr_animation_player_rate(player) == 1);
  input.shot_sequence = 1;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == TEST_CROUCH_FIRE);

  const VkrPlayerAnimation saved = animation;
  const uint64_t saved_generation = vkr_animation_player_generation(player);
  input.speed = NAN;
  assert(!vkr_player_animation_update(&animation, &input));
  assert(MemCompare(&saved, &animation, sizeof(saved)) == 0);
  assert(vkr_animation_player_generation(player) == saved_generation);
  assert(!vkr_player_animation_initialize(&animation, player, 0, &error));
  assert(error && MemCompare(&saved, &animation, sizeof(saved)) == 0);
  assert(!vkr_player_animation_initialize(&animation, player, 1e-100, &error));
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
  VkrPlayerAnimation animation;
  assert(vkr_player_animation_initialize(&animation, player, 1.0 / 60, NULL));
  VkrPlayerAnimationInput input = {.grounded = true_v, .speed = 5};
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == 2);
  assert(fabs(vkr_animation_player_rate(player) - 5.0 / 1.3) < 1e-6);
  input.shot_sequence = 1;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == 3);
  input.reloading = true_v;
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == 0);
  vkr_animation_player_destroy(player);

  fixture.asset.clip_count = 1;
  fixture.clips[0].name = string8_lit("Idle");
  player = vkr_animation_player_create(&fixture.asset, scratch, 0, true_v, 1,
                                       true_v, NULL);
  assert(player);
  assert(vkr_player_animation_initialize(&animation, player, 1.0 / 60, NULL));
  assert(animation.reload_ticks == 0);
  assert(vkr_player_animation_update(&animation, &input));
  assert(vkr_animation_player_clip(player) == 0);
  assert(vkr_animation_player_loop(player));
  vkr_animation_player_destroy(player);
}

bool32_t run_player_animation_tests(void) {
  printf("Running player animation tests...\n");
  Arena *arena = arena_create(MB(1), KB(64));
  assert(arena);
  VkrAllocator scratch = {.ctx = arena};
  assert(vkr_allocator_arena(&scratch));
  player_animation_actions(&scratch);
  player_animation_fallback(&scratch);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(arena);
  printf("Player animation tests passed.\n");
  return true_v;
}
