#include "animation_tests.h"

#include "assets/vkr_animation.h"
#include "memory/vkr_arena_allocator.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

static VkrAnimationNode animation_test_node(uint32_t parent) {
  return (VkrAnimationNode){
      .parent = parent,
      .local = mat4_identity(),
      .rest = {.rotation = vkr_quat_identity(), .scale = vec3_one()},
  };
}

static void animation_test_near(float32_t actual, float32_t expected) {
  assert(isfinite(actual));
  assert(fabsf(actual - expected) < 0.0001f);
}

static void animation_test_valid(VkrAnimationAsset *asset,
                                 VkrAllocator *scratch) {
  const uint64_t bytes = scratch->stats.total_allocated;
  const uint32_t depth = scratch->scope_depth;
  const char *error = "not cleared";
  assert(vkr_animation_validate(asset, scratch, &error));
  assert(!error);
  assert(scratch->stats.total_allocated == bytes);
  assert(scratch->scope_depth == depth);
}

static void animation_test_invalid(VkrAnimationAsset *asset,
                                   VkrAllocator *scratch) {
  const uint64_t bytes = scratch->stats.total_allocated;
  const char *error = NULL;
  assert(!vkr_animation_validate(asset, scratch, &error));
  assert(error);
  assert(scratch->stats.total_allocated == bytes);
}

/* Uneven source times distinguish elapsed-time sampling from key indexing;
 * the Hermite expectations are analytical values of a two-second curve. */
static void animation_test_sampling(VkrAllocator *scratch) {
  VkrAnimationNode node = animation_test_node(VKR_ANIMATION_NO_NODE);
  node.rest.scale = vec3_new(0.0001f, 1.0f, 2.0f);
  uint32_t order = 0;
  float32_t times[] = {1.0f, 2.0f, 5.0f};
  Vec4 values[] = {vec4_new(0, 0, 0, 0), vec4_new(4, 0, 0, 0),
                   vec4_new(10, 0, 0, 0)};
  VkrAnimationChannel channel = {
      .node = 0,
      .path = VKR_ANIMATION_TRANSLATION,
      .interpolation = VKR_ANIMATION_LINEAR,
      .key_count = ArrayCount(times),
      .times = times,
      .values = values,
  };
  VkrAnimationClip clip = {
      .duration = 5.0f, .channel_count = 1, .channels = &channel};
  VkrAnimationAsset asset = {.node_count = 1,
                             .clip_count = 1,
                             .nodes = &node,
                             .node_order = &order,
                             .clips = &clip};
  animation_test_valid(&asset, scratch);
  VkrAnimationTrs pose;
  assert(vkr_animation_sample(&asset, 0, 3.5, &pose));
  animation_test_near(pose.translation.x, 7.0f);
  animation_test_near(pose.scale.x, 0.0001f);
  assert(vkr_animation_sample(&asset, 0, -100.0, &pose));
  animation_test_near(pose.translation.x, 0.0f);
  assert(vkr_animation_sample(&asset, 0, 100.0, &pose));
  animation_test_near(pose.translation.x, 10.0f);
  assert(!vkr_animation_sample(&asset, 0, NAN, &pose));
  assert(!vkr_animation_sample(&asset, 1, 0.0, &pose));

  channel.interpolation = VKR_ANIMATION_STEP;
  animation_test_valid(&asset, scratch);
  assert(vkr_animation_sample(&asset, 0, 1.999, &pose));
  animation_test_near(pose.translation.x, 0.0f);
  assert(vkr_animation_sample(&asset, 0, 2.0, &pose));
  animation_test_near(pose.translation.x, 4.0f);
  assert(vkr_animation_sample(&asset, 0, 4.99, &pose));
  animation_test_near(pose.translation.x, 4.0f);

  float32_t cubic_times[] = {2.0f, 4.0f};
  Vec4 cubic_values[] = {
      vec4_new(0, 0, 0, 0), vec4_new(0, 0, 0, 0), vec4_new(2, 0, 0, 0),
      vec4_new(0, 0, 0, 0), vec4_new(4, 0, 0, 0), vec4_new(0, 0, 0, 0),
  };
  channel.interpolation = VKR_ANIMATION_CUBIC_SPLINE;
  channel.key_count = ArrayCount(cubic_times);
  channel.times = cubic_times;
  channel.values = cubic_values;
  clip.duration = 4.0f;
  animation_test_valid(&asset, scratch);
  assert(vkr_animation_sample(&asset, 0, 3.0, &pose));
  animation_test_near(pose.translation.x, 2.5f);
  assert(vkr_animation_sample(&asset, 0, 2.5, &pose));
  animation_test_near(pose.translation.x, 1.1875f);

  channel.interpolation = VKR_ANIMATION_LINEAR;
  channel.path = VKR_ANIMATION_ROTATION;
  Vec4 rotation_values[] = {vec4_new(0, 0, 0, 1),
                            vec4_new(0, 0, -0.70710678f, -0.70710678f)};
  channel.values = rotation_values;
  animation_test_valid(&asset, scratch);
  assert(vkr_animation_sample(&asset, 0, 3.0, &pose));
  animation_test_near(pose.rotation.z, 0.38268343f);
  animation_test_near(pose.rotation.w, 0.92387953f);
  rotation_values[1] = vec4_new(0, 0, 0, -1);
  animation_test_valid(&asset, scratch);
  assert(vkr_animation_sample(&asset, 0, 3.0, &pose));
  animation_test_near(fabsf(pose.rotation.w), 1.0f);

  channel.interpolation = VKR_ANIMATION_CUBIC_SPLINE;
  channel.values = cubic_values;
  MemZero(cubic_values, sizeof(cubic_values));
  cubic_values[1] = vec4_new(0, 0, 0, 1);
  cubic_values[4] = vec4_new(0, 0, -0.70710678f, -0.70710678f);
  animation_test_valid(&asset, scratch);
  assert(vkr_animation_sample(&asset, 0, 3.0, &pose));
  /* Authored negative quaternion signs give -135 degrees here; silently
   * flipping the cubic endpoint would incorrectly produce +45 degrees. */
  animation_test_near(pose.rotation.z, -0.92387953f);
  animation_test_near(pose.rotation.w, 0.38268343f);
  cubic_values[4] = vec4_new(0, 0, 0, -1);
  animation_test_valid(&asset, scratch);
  assert(!vkr_animation_sample(&asset, 0, 3.0, &pose));
  assert(vkr_animation_sample(&asset, 0, 4.0, &pose));
  animation_test_near(pose.rotation.w, -1.0f);

  cubic_times[1] = 18.0f;
  clip.duration = 18.0f;
  cubic_values[4] = vec4_new(0, 0, 0, 1);
  cubic_values[2].z = FLT_MAX;
  animation_test_valid(&asset, scratch);
  assert(vkr_animation_sample(&asset, 0, 10.0, &pose));
  animation_test_near(pose.rotation.z, 1.0f);
  animation_test_near(pose.rotation.w, 0.0f);
  /* The same finite Hermite tangent has an unrepresentable translation;
   * rotations remain representable because normalization removes magnitude. */
  channel.path = VKR_ANIMATION_TRANSLATION;
  cubic_values[1].w = 0.0f;
  cubic_values[4].w = 0.0f;
  animation_test_valid(&asset, scratch);
  assert(!vkr_animation_sample(&asset, 0, 10.0, &pose));
}

/* The skeleton deliberately contains a nonjoint matrix-authored ancestor,
 * reordered source indices, nonuniform scale, and two independent skin maps. */
static void animation_test_hierarchy(VkrAllocator *scratch) {
  VkrAnimationNode nodes[] = {animation_test_node(2),
                              animation_test_node(VKR_ANIMATION_NO_NODE),
                              animation_test_node(1), animation_test_node(0)};
  nodes[0].rest.translation = vec3_new(1, 0, 0);
  nodes[1].rest.translation = vec3_new(10, 0, 0);
  nodes[1].rest.rotation = vec4_new(0, 0, 0.70710678f, 0.70710678f);
  nodes[1].rest.scale = vec3_new(2, 3, 1);
  nodes[2].matrix_authored = true_v;
  nodes[2].local.m01 = 0.5f;
  nodes[2].local.m13 = 1.0f;
  nodes[3].rest.translation = vec3_new(0, 2, 0);
  uint32_t order[] = {1, 2, 0, 3};
  uint32_t joints_a[] = {0, 3};
  uint32_t joints_b[] = {3, 0};
  Mat4 inverse_a[] = {mat4_translate(vec3_new(-1, 0, 0)), mat4_identity()};
  Mat4 inverse_b[] = {mat4_identity(), mat4_identity()};
  VkrAnimationSkin skins[] = {
      {.skeleton_node = 1,
       .joint_count = 2,
       .joints = joints_a,
       .inverse_bind = inverse_a},
      {.skeleton_node = VKR_ANIMATION_NO_NODE,
       .joint_count = 2,
       .joints = joints_b,
       .inverse_bind = inverse_b},
  };
  VkrAnimationAsset asset = {.node_count = ArrayCount(nodes),
                             .skin_count = ArrayCount(skins),
                             .nodes = nodes,
                             .node_order = order,
                             .skins = skins};
  animation_test_valid(&asset, scratch);
  VkrAnimationTrs pose[ArrayCount(nodes)];
  for (uint32_t i = 0; i < ArrayCount(nodes); ++i) {
    pose[i] = nodes[i].rest;
  }
  Mat4 global[ArrayCount(nodes)];
  Mat4 palette[2];
  assert(vkr_animation_global_pose(&asset, pose, global));
  animation_test_near(global[0].m03, 7.0f);
  animation_test_near(global[0].m13, 2.0f);
  animation_test_near(global[3].m03, 1.0f);
  animation_test_near(global[3].m13, 4.0f);
  assert(vkr_animation_skin_palette(&asset, 0, global, palette));
  animation_test_near(palette[0].m03, 7.0f);
  animation_test_near(palette[0].m13, 0.0f);
  assert(vkr_animation_skin_palette(&asset, 1, global, palette));
  animation_test_near(palette[0].m03, 1.0f);
  animation_test_near(palette[0].m13, 4.0f);
  animation_test_near(palette[1].m03, 7.0f);
  animation_test_near(palette[1].m13, 2.0f);

  order[0] = 0;
  animation_test_invalid(&asset, scratch);
  order[0] = 1;
  order[3] = 1;
  animation_test_invalid(&asset, scratch);
  order[3] = 3;
  nodes[1].parent = 3;
  animation_test_invalid(&asset, scratch);
  nodes[1].parent = VKR_ANIMATION_NO_NODE;
  joints_a[1] = 0;
  animation_test_invalid(&asset, scratch);
  joints_a[1] = 3;
  skins[0].skeleton_node = 3;
  animation_test_invalid(&asset, scratch);
  skins[0].skeleton_node = 1;
  nodes[3].parent = VKR_ANIMATION_NO_NODE;
  skins[0].skeleton_node = VKR_ANIMATION_NO_NODE;
  animation_test_invalid(&asset, scratch);
  nodes[3].parent = 0;
  skins[0].skeleton_node = 1;
  inverse_a[0].m00 = 0.0f;
  animation_test_invalid(&asset, scratch);
  inverse_a[0].m00 = 0.0001f;
  animation_test_valid(&asset, scratch);
  inverse_a[0].m30 = 0.01f;
  animation_test_invalid(&asset, scratch);
}

static void animation_test_malformed_channels(VkrAllocator *scratch) {
  VkrAnimationNode node = animation_test_node(VKR_ANIMATION_NO_NODE);
  uint32_t order = 0;
  float32_t times[] = {0.0f, 1.0f};
  Vec4 values[] = {vec4_new(0, 0, 0, 1), vec4_new(0, 0, 0, 1)};
  VkrAnimationChannel channels[2] = {
      {.path = VKR_ANIMATION_ROTATION,
       .interpolation = VKR_ANIMATION_LINEAR,
       .key_count = 2,
       .times = times,
       .values = values},
  };
  channels[1] = channels[0];
  VkrAnimationClip clip = {
      .duration = 1.0f, .channel_count = 1, .channels = channels};
  VkrAnimationAsset asset = {.node_count = 1,
                             .clip_count = 1,
                             .nodes = &node,
                             .node_order = &order,
                             .clips = &clip};
  animation_test_valid(&asset, scratch);
  clip.channel_count = 2;
  animation_test_invalid(&asset, scratch);
  clip.channel_count = 1;
  clip.duration = 2.0f;
  animation_test_invalid(&asset, scratch);
  clip.duration = 1.0f;
  times[1] = 0.0f;
  animation_test_invalid(&asset, scratch);
  times[1] = NAN;
  animation_test_invalid(&asset, scratch);
  times[1] = 1.0f;
  values[1].w = 2.0f;
  animation_test_invalid(&asset, scratch);
  values[1].w = 1.0f;
  channels[0].node = 1;
  animation_test_invalid(&asset, scratch);
  channels[0].node = 0;
  channels[0].path = (VkrAnimationPath)-1;
  animation_test_invalid(&asset, scratch);
  channels[0].path = VKR_ANIMATION_ROTATION;
  channels[0].interpolation = (VkrAnimationInterpolation)3;
  animation_test_invalid(&asset, scratch);
  channels[0].interpolation = VKR_ANIMATION_LINEAR;
  channels[0].key_count = 0;
  animation_test_invalid(&asset, scratch);
  channels[0].interpolation = VKR_ANIMATION_CUBIC_SPLINE;
  channels[0].key_count = 1;
  animation_test_invalid(&asset, scratch);
  channels[0].interpolation = VKR_ANIMATION_LINEAR;
  channels[0].key_count = 2;
  node.matrix_authored = true_v;
  animation_test_invalid(&asset, scratch);
  node.matrix_authored = false_v;
  node.rest.translation.x = INFINITY;
  animation_test_invalid(&asset, scratch);
  node.rest.translation.x = 0;
  node.name.length = 1;
  animation_test_invalid(&asset, scratch);
  node.name = (String8){0};
  animation_test_valid(&asset, scratch);
}

bool32_t run_animation_tests(void) {
  printf("Running animation reference tests...\n");
  Arena *arena = arena_create(MB(1), KB(64));
  assert(arena);
  VkrAllocator scratch = {.ctx = arena};
  assert(vkr_allocator_arena(&scratch));
  animation_test_sampling(&scratch);
  animation_test_hierarchy(&scratch);
  animation_test_malformed_channels(&scratch);
  assert(scratch.stats.total_allocated == 0);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(arena);
  printf("Animation reference tests PASSED\n");
  return true_v;
}
