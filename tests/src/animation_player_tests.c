#include "animation_player_tests.h"

#include "animation/vkr_animation_player.h"
#include "memory/vkr_arena_allocator.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

static void player_test_x(const VkrAnimationPlayer *player, float64_t time,
                          float32_t x) {
  assert(fabs(vkr_animation_player_time(player) - time) < 1e-12);
  assert(fabsf(vkr_animation_player_global_pose(player)[0].elements[12] - x) <
         0.00001f);
  assert(fabsf(vkr_animation_player_skin_palette(player, 0)[0].elements[12] -
               x) < 0.00001f);
}

static void player_test_weighted_trs(VkrAllocator *scratch) {
  VkrAnimationNode nodes[2] = {
      {.parent = VKR_ANIMATION_NO_NODE,
       .local = mat4_identity(),
       .rest = {.rotation = vkr_quat_identity(), .scale = vec3_one()}},
      {.parent = 0u,
       .local = mat4_identity(),
       .rest = {.rotation = vkr_quat_identity(), .scale = vec3_one()},
       .matrix_authored = true_v},
  };
  nodes[1].local.elements[12] = 1.0f;
  nodes[1].local.elements[13] = 2.0f;
  nodes[1].local.elements[4] = 0.25f;
  uint32_t order[] = {0u, 1u};
  uint32_t joint = 0u;
  Mat4 inverse = mat4_identity();
  VkrAnimationSkin skin = {.skeleton_node = 0u,
                           .joint_count = 1u,
                           .joints = &joint,
                           .inverse_bind = &inverse};
  float32_t zero = 0.0f;
  Vec4 keys[3][3] = {
      {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 0}},
      {{8, 4, 0, 0}, {0, 0, 1, 0}, {3, 5, 1, 0}},
      {{8, 4, 0, 0}, {0, 0, -1, 0}, {3, 5, 1, 0}},
  };
  VkrAnimationChannel channels[3][3];
  VkrAnimationClip clips[3];
  for (uint32_t clip = 0u; clip < ArrayCount(clips); ++clip) {
    for (uint32_t path = 0u; path < 3u; ++path) {
      channels[clip][path] = (VkrAnimationChannel){
          .node = 0u,
          .path = (VkrAnimationPath)path,
          .interpolation = VKR_ANIMATION_LINEAR,
          .key_count = 1u,
          .times = &zero,
          .values = &keys[clip][path],
      };
    }
    clips[clip] =
        (VkrAnimationClip){.channel_count = 3u, .channels = channels[clip]};
  }
  VkrAnimationAsset asset = {.node_count = 2u,
                             .skin_count = 1u,
                             .clip_count = ArrayCount(clips),
                             .nodes = nodes,
                             .node_order = order,
                             .skins = &skin,
                             .clips = clips};
  VkrAnimationPlayer *player = vkr_animation_player_create(
      &asset, scratch, 0u, false_v, 1.0, true_v, NULL);
  assert(player);
  VkrAnimationSample samples[2] = {
      {.clip = 0u, .time = 0.0, .weight = FLT_MAX},
      {.clip = 1u, .time = 0.0, .weight = FLT_MAX},
  };
  assert(vkr_animation_player_sample_blend(player, samples, 2u, false_v));
  const Mat4 *global = vkr_animation_player_global_pose(player);
  /* Halfway between identity and 180 degrees is 90 degrees. The blended
     scale is (2,3,1), before rotation; translation is (4,2,0). */
  assert(fabsf(global[0].elements[0]) < 1e-5f);
  assert(fabsf(global[0].elements[1] - 2.0f) < 1e-5f);
  assert(fabsf(global[0].elements[4] + 3.0f) < 1e-5f);
  assert(fabsf(global[0].elements[5]) < 1e-5f);
  assert(fabsf(global[0].elements[12] - 4.0f) < 1e-5f);
  assert(fabsf(global[0].elements[13] - 2.0f) < 1e-5f);
  /* The child's authored translation and shear survive blending. */
  assert(fabsf(global[1].elements[12] + 2.0f) < 1e-5f);
  assert(fabsf(global[1].elements[13] - 4.0f) < 1e-5f);
  assert(fabsf(global[1].elements[4] + 3.0f) < 1e-5f);
  assert(fabsf(global[1].elements[5] - 0.5f) < 1e-5f);
  assert(MemCompare(&global[0], vkr_animation_player_skin_palette(player, 0u),
                    sizeof(Mat4)) == 0);
  samples[0].clip = 1u;
  samples[1].clip = 2u;
  assert(vkr_animation_player_sample_blend(player, samples, 2u, false_v));
  global = vkr_animation_player_global_pose(player);
  assert(fabsf(global[0].elements[0] + 3.0f) < 1e-5f);
  assert(fabsf(global[0].elements[5] + 5.0f) < 1e-5f);
  assert(fabsf(global[0].elements[12] - 8.0f) < 1e-5f);
  const uint64_t generation = vkr_animation_player_generation(player);
  const Mat4 saved = global[0];
  samples[0].weight = 0.0f;
  samples[1].weight = 0.0f;
  assert(!vkr_animation_player_sample_blend(player, samples, 2u, true_v));
  samples[0].weight = -1.0f;
  assert(!vkr_animation_player_sample_blend(player, samples, 2u, true_v));
  samples[0].weight = NAN;
  assert(!vkr_animation_player_sample_blend(player, samples, 2u, true_v));
  samples[0].weight = 1.0f;
  samples[0].time = INFINITY;
  assert(!vkr_animation_player_sample_blend(player, samples, 2u, true_v));
  samples[0].time = 0.0;
  samples[0].clip = 99u;
  assert(!vkr_animation_player_sample_blend(player, samples, 2u, true_v));
  assert(!vkr_animation_player_sample_blend(player, samples, 33u, true_v));
  assert(vkr_animation_player_generation(player) == generation);
  assert(vkr_animation_player_global_pose(player) == global);
  assert(MemCompare(&saved, global, sizeof(saved)) == 0);
  /* An externally blended pose is captured, rather than restarting the
     control clip (which is still clip zero). */
  assert(vkr_animation_player_clip(player) == 0u);
  assert(vkr_animation_player_crossfade(player, 0u, false_v, 1.0));
  assert(MemCompare(&saved, vkr_animation_player_global_pose(player),
                    sizeof(saved)) == 0);
  assert(vkr_animation_player_advance(player, 0.5));
  assert(fabsf(vkr_animation_player_global_pose(player)[0].elements[12] -
               4.0f) < 1e-5f);
  vkr_animation_player_destroy(player);
}

static void player_test_crossfades(const VkrAnimationAsset *asset,
                                   VkrAllocator *scratch) {
  VkrAnimationPlayer *player = vkr_animation_player_create(
      asset, scratch, 0u, false_v, 1.0, true_v, NULL);
  assert(player);
  assert(vkr_animation_player_seek(player, 0.5));
  const Mat4 start = vkr_animation_player_global_pose(player)[0];
  assert(vkr_animation_player_crossfade(player, 0u, false_v, 1.0));
  assert(MemCompare(&start, vkr_animation_player_global_pose(player),
                    sizeof(start)) == 0);
  assert(vkr_animation_player_crossfade_active(player));
  assert(vkr_animation_player_crossfade_duration(player) == 1.0);
  assert(vkr_animation_player_duration(player) == 2.0);
  assert(vkr_animation_player_advance(player, 0.25));
  player_test_x(player, 0.25, 2.5f);
  assert(vkr_animation_player_crossfade_progress(player) == 0.25);
  /* Interrupt at x=2.5. The new source remains exactly that displayed pose. */
  assert(vkr_animation_player_crossfade(player, 2u, true_v, 0.5));
  player_test_x(player, 0.0, 2.5f);
  assert(vkr_animation_player_advance(player, 0.25));
  player_test_x(player, 0.0, 1.25f);
  assert(vkr_animation_player_advance(player, 0.25));
  player_test_x(player, 0.0, 0.0f);
  assert(!vkr_animation_player_crossfade_active(player));
  assert(vkr_animation_player_crossfade_progress(player) == 1.0);

  assert(vkr_animation_player_select_clip(player, 0u, false_v));
  assert(vkr_animation_player_seek(player, 1.0));
  assert(vkr_animation_player_crossfade(player, 2u, false_v, 1.0));
  assert(vkr_animation_player_set_rate(player, 0.0));
  assert(vkr_animation_player_advance(player, 0.5));
  player_test_x(player, 0.0, 2.0f);
  vkr_animation_player_set_playing(player, false_v);
  assert(vkr_animation_player_advance(player, 100.0));
  player_test_x(player, 0.0, 2.0f);
  assert(vkr_animation_player_crossfade_progress(player) == 0.5);
  vkr_animation_player_set_playing(player, true_v);
  assert(vkr_animation_player_set_rate(player, -1.0));
  assert(vkr_animation_player_rate(player) == -1.0);
  assert(vkr_animation_player_advance(player, 0.25));
  player_test_x(player, 0.0, 0.75f);
  assert(vkr_animation_player_advance(player, 0.25));
  player_test_x(player, 0.0, 0.0f);
  assert(vkr_animation_player_crossfade(player, 0u, true_v, 0.5));
  assert(vkr_animation_player_advance(player, 0.25));
  player_test_x(player, 1.75, 3.5f);
  assert(vkr_animation_player_advance(player, 0.25));
  player_test_x(player, 1.5, 6.0f);
  assert(vkr_animation_player_loop(player));

  assert(vkr_animation_player_set_rate(player, 1.0));
  assert(vkr_animation_player_select_clip(player, 0u, false_v));
  assert(vkr_animation_player_crossfade(player, 1u, false_v, 2.0));
  assert(vkr_animation_player_advance(player, 0.5));
  const Mat4 *before = vkr_animation_player_global_pose(player);
  const Mat4 last = before[0];
  const uint64_t generation = vkr_animation_player_generation(player);
  const uint64_t discontinuity = vkr_animation_player_discontinuity(player);
  assert(!vkr_animation_player_advance(player, 0.5));
  assert(!vkr_animation_player_crossfade(player, 99u, true_v, 0.5));
  assert(!vkr_animation_player_crossfade(player, 0u, true_v, NAN));
  assert(!vkr_animation_player_crossfade(player, 0u, true_v, -1.0));
  VkrAnimationSample samples[] = {
      {.clip = 0u, .time = 0.5, .weight = 1.0f},
      {.clip = 1u, .time = 1.0, .weight = 1.0f},
  };
  assert(!vkr_animation_player_sample_blend(player, samples, 2u, true_v));
  assert(!vkr_animation_player_seek(player, 1.0));
  assert(vkr_animation_player_global_pose(player) == before);
  assert(MemCompare(&last, before, sizeof(last)) == 0);
  assert(vkr_animation_player_generation(player) == generation);
  assert(vkr_animation_player_discontinuity(player) == discontinuity);
  assert(vkr_animation_player_crossfade_active(player));
  assert(vkr_animation_player_crossfade_progress(player) == 0.25);
  assert(vkr_animation_player_time(player) == 0.5);
  assert(vkr_animation_player_crossfade(player, 0u, false_v, 1.0));
  assert(vkr_animation_player_advance(player, 0.25));
  player_test_x(player, 0.25, 1.375f);
  assert(vkr_animation_player_seek(player, 1.0));
  assert(!vkr_animation_player_crossfade_active(player));
  player_test_x(player, 1.0, 4.0f);
  assert(vkr_animation_player_crossfade(player, 2u, true_v, 0.0));
  assert(!vkr_animation_player_crossfade_active(player));
  player_test_x(player, 0.0, 0.0f);
  assert(vkr_animation_player_crossfade(player, 0u, true_v, 1.0));
  assert(vkr_animation_player_select_clip(player, 0u, false_v));
  assert(!vkr_animation_player_crossfade_active(player));
  samples[0] = (VkrAnimationSample){.clip = 0u, .time = 0.5, .weight = 1.0f};
  samples[1] = (VkrAnimationSample){.clip = 0u, .time = 1.5, .weight = 3.0f};
  assert(vkr_animation_player_sample_blend(player, samples, 2u, false_v));
  player_test_x(player, 0.0, 5.0f);
  vkr_animation_player_destroy(player);

  const uint32_t rates[] = {30u, 60u, 144u};
  for (uint32_t i = 0u; i < ArrayCount(rates); ++i) {
    player = vkr_animation_player_create(asset, scratch, 0u, false_v, 1.0,
                                         true_v, NULL);
    assert(player);
    assert(vkr_animation_player_seek(player, 0.25));
    assert(vkr_animation_player_crossfade(player, 0u, false_v, 1.5));
    for (uint32_t frame = 0u; frame < rates[i]; ++frame) {
      assert(vkr_animation_player_advance(player, 1.0 / (float64_t)rates[i]));
    }
    player_test_x(player, 1.0, 13.0f / 3.0f);
    assert(fabs(vkr_animation_player_crossfade_progress(player) - 2.0 / 3.0) <
           1e-12);
    vkr_animation_player_destroy(player);
  }
}

bool32_t run_animation_player_tests(void) {
  printf("Running animation player tests...\n");
  Arena *arena = arena_create(MB(1), KB(64));
  assert(arena);
  VkrAllocator scratch = {.ctx = arena};
  assert(vkr_allocator_arena(&scratch));
  VkrAnimationNode node = {
      .parent = VKR_ANIMATION_NO_NODE,
      .local = mat4_identity(),
      .rest = {.rotation = vkr_quat_identity(), .scale = vec3_one()}};
  uint32_t order = 0;
  uint32_t joint = 0;
  Mat4 inverse_bind = mat4_identity();
  VkrAnimationSkin skin = {.skeleton_node = 0,
                           .joint_count = 1,
                           .joints = &joint,
                           .inverse_bind = &inverse_bind};
  float32_t times[] = {0, 2};
  Vec4 values[] = {vec4_zero(), vec4_new(8, 0, 0, 0)};
  VkrAnimationChannel channel = {.node = 0,
                                 .path = VKR_ANIMATION_TRANSLATION,
                                 .interpolation = VKR_ANIMATION_LINEAR,
                                 .key_count = 2,
                                 .times = times,
                                 .values = values};
  /* Opposite cubic key values with zero tangents are legal source data but
   * produce an unnormalizable zero quaternion exactly halfway through. */
  Vec4 cubic_values[] = {vec4_zero(), vec4_new(0, 0, 0, 1),  vec4_zero(),
                         vec4_zero(), vec4_new(0, 0, 0, -1), vec4_zero()};
  VkrAnimationChannel cubic = {.node = 0,
                               .path = VKR_ANIMATION_ROTATION,
                               .interpolation = VKR_ANIMATION_CUBIC_SPLINE,
                               .key_count = 2,
                               .times = times,
                               .values = cubic_values};
  VkrAnimationChannel constant = channel;
  constant.key_count = 1;
  VkrAnimationClip clips[] = {
      {.duration = 2, .channel_count = 1, .channels = &channel},
      {.duration = 2, .channel_count = 1, .channels = &cubic},
      {.duration = 0, .channel_count = 1, .channels = &constant}};
  VkrAnimationAsset asset = {.node_count = 1,
                             .skin_count = 1,
                             .clip_count = ArrayCount(clips),
                             .nodes = &node,
                             .node_order = &order,
                             .skins = &skin,
                             .clips = clips};
  const char *error = NULL;
  uint32_t rates[] = {30, 60, 144};
  for (uint32_t i = 0; i < ArrayCount(rates); ++i) {
    VkrAnimationPlayer *player = vkr_animation_player_create(
        &asset, &scratch, 0, true_v, 1, true_v, &error);
    assert(player && !error);
    for (uint32_t frame = 0; frame < rates[i]; ++frame) {
      assert(vkr_animation_player_advance(player, 1.0 / rates[i]));
    }
    player_test_x(player, 1, 4);
    vkr_animation_player_destroy(player);
  }
  VkrAnimationPlayer *a = vkr_animation_player_create(
      &asset, &scratch, 0, true_v, 1, true_v, &error);
  VkrAnimationPlayer *b = vkr_animation_player_create(
      &asset, &scratch, 0, false_v, 1, true_v, &error);
  assert(a && b && !error);
  assert(vkr_animation_player_advance(a, 2.5));
  player_test_x(a, 0.5, 2);
  player_test_x(b, 0, 0);
  assert(vkr_animation_player_discontinuity(a) == 2);
  vkr_animation_player_set_playing(a, false_v);
  uint64_t generation = vkr_animation_player_generation(a);
  assert(vkr_animation_player_advance(a, 12));
  assert(vkr_animation_player_generation(a) == generation);
  assert(vkr_animation_player_seek(a, 2));
  player_test_x(a, 2, 8);
  assert(!vkr_animation_player_playing(a));
  assert(vkr_animation_player_set_rate(a, -1));
  vkr_animation_player_set_playing(a, true_v);
  assert(vkr_animation_player_advance(a, 2.25));
  player_test_x(a, 1.75, 7);
  assert(vkr_animation_player_advance(b, 10));
  player_test_x(b, 2, 8);
  assert(vkr_animation_player_set_rate(b, -1));
  assert(vkr_animation_player_advance(b, 10));
  player_test_x(b, 0, 0);
  assert(vkr_animation_player_seek(b, -20));
  player_test_x(b, 0, 0);
  assert(vkr_animation_player_select_clip(a, 1, false_v));
  assert(vkr_animation_player_set_rate(a, 1));
  const Mat4 *before = vkr_animation_player_global_pose(a);
  Mat4 saved = before[0];
  generation = vkr_animation_player_generation(a);
  uint64_t discontinuity = vkr_animation_player_discontinuity(a);
  assert(!vkr_animation_player_advance(a, 1));
  assert(!vkr_animation_player_seek(a, 1));
  assert(!vkr_animation_player_advance(a, NAN));
  assert(!vkr_animation_player_advance(a, INFINITY));
  assert(!vkr_animation_player_advance(a, -1));
  assert(!vkr_animation_player_seek(a, NAN));
  assert(!vkr_animation_player_select_clip(a, 99, true_v));
  assert(!vkr_animation_player_set_rate(a, INFINITY));
  assert(vkr_animation_player_time(a) == 0);
  assert(vkr_animation_player_clip(a) == 1);
  assert(vkr_animation_player_global_pose(a) == before);
  assert(MemCompare(&saved, before, sizeof(saved)) == 0);
  assert(vkr_animation_player_generation(a) == generation);
  assert(vkr_animation_player_discontinuity(a) == discontinuity);
  assert(vkr_animation_player_set_rate(a, DBL_MAX));
  assert(!vkr_animation_player_advance(a, 2));
  assert(vkr_animation_player_time(a) == 0);
  assert(vkr_animation_player_set_rate(a, 1));
  assert(vkr_animation_player_advance(a, 0.5));
  assert(vkr_animation_player_time(a) == 0.5);
  assert(vkr_animation_player_select_clip(a, 2, true_v));
  assert(vkr_animation_player_advance(a, 100));
  assert(vkr_animation_player_time(a) == 0);
  vkr_animation_player_destroy(a);
  assert(vkr_animation_player_seek(b, 1));
  player_test_x(b, 1, 4);
  vkr_animation_player_destroy(b);
  assert(!vkr_animation_player_create(&asset, &scratch, 99, false_v, 1, true_v,
                                      &error));
  assert(error);
  assert(!vkr_animation_player_create(&asset, &scratch, 0, false_v, NAN, true_v,
                                      &error));
  assert(error);
  player_test_weighted_trs(&scratch);
  player_test_crossfades(&asset, &scratch);
  assert(scratch.stats.total_allocated == 0);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(arena);
  printf("Animation player tests PASSED\n");
  return true_v;
}
