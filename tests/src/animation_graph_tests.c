#include "animation_graph_tests.h"
#include "animation/vkr_animation_graph.h"
#include "memory/vkr_arena_allocator.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void graph_test_position(VkrAnimationPlayer *player, float32_t x) {
  assert(fabsf(vkr_animation_player_global_pose(player)[0].elements[12] - x) <
         0.00002f);
}

typedef struct GraphJsonTestBuffer {
  uint8_t data[16384];
  uint64_t length;
} GraphJsonTestBuffer;

static bool8_t graph_test_write(void *context, const uint8_t *data,
                                uint64_t size) {
  GraphJsonTestBuffer *buffer = context;
  if (size > sizeof(buffer->data) - buffer->length) {
    return false_v;
  }
  MemCopy(buffer->data + buffer->length, data, size);
  buffer->length += size;
  return true_v;
}

bool32_t run_animation_graph_tests(void) {
  printf("Running animation graph tests...\n");
  Arena *arena = arena_create(MB(1), KB(64));
  assert(arena);
  VkrAllocator scratch = {.ctx = arena};
  assert(vkr_allocator_arena(&scratch));
  VkrAnimationNode node = {
      .parent = VKR_ANIMATION_NO_NODE,
      .local = mat4_identity(),
      .rest = {.rotation = vkr_quat_identity(), .scale = vec3_one()}};
  uint32_t order = 0u, joint = 0u;
  Mat4 inverse_bind = mat4_identity();
  VkrAnimationSkin skin = {.skeleton_node = 0u,
                           .joint_count = 1u,
                           .joints = &joint,
                           .inverse_bind = &inverse_bind};
  float32_t times[3][2] = {{0, 1}, {0, 2}, {0, 4}};
  Vec4 values[5][2] = {{{0}, {0}},
                       {{10, 0, 0, 0}, {10, 0, 0, 0}},
                       {{20, 0, 0, 0}, {20, 0, 0, 0}},
                       {{0}, {8, 0, 0, 0}},
                       {{0}, {8, 0, 0, 0}}};
  VkrAnimationChannel channels[5];
  VkrAnimationClip clips[5];
  for (uint32_t i = 0u; i < 5u; ++i) {
    uint32_t time_index = i < 3u ? 0u : i - 2u;
    channels[i] = (VkrAnimationChannel){.node = 0u,
                                        .path = VKR_ANIMATION_TRANSLATION,
                                        .interpolation = VKR_ANIMATION_LINEAR,
                                        .key_count = 2u,
                                        .times = times[time_index],
                                        .values = values[i]};
    clips[i] = (VkrAnimationClip){.duration = times[time_index][1],
                                  .channel_count = 1u,
                                  .channels = &channels[i]};
  }
  VkrAnimationAsset asset = {.node_count = 1u,
                             .skin_count = 1u,
                             .clip_count = 5u,
                             .nodes = &node,
                             .node_order = &order,
                             .skins = &skin,
                             .clips = clips};
  const char *error = NULL;
  VkrAnimationPlayer *player = vkr_animation_player_create(
      &asset, &scratch, 0u, true_v, 1.0, true_v, &error);
  assert(player);
  VkrAnimationGraph graph = {.node_count = 3u,
                             .parameter_count = 2u,
                             .root = 2u,
                             .cycle_seconds = 1.0,
                             .parameters = {0.25f, 0.25f}};
  graph.nodes[0] =
      (VkrAnimationGraphNode){.kind = VKR_ANIMATION_GRAPH_CLIP, .clip = 0u};
  graph.nodes[1] =
      (VkrAnimationGraphNode){.kind = VKR_ANIMATION_GRAPH_CLIP, .clip = 1u};
  graph.nodes[2] = (VkrAnimationGraphNode){.kind = VKR_ANIMATION_GRAPH_BLEND2,
                                           .inputs = {0u, 1u},
                                           .parameter_x = 0u};
  VkrAnimationGraphInstance instance, other;
  assert(vkr_animation_graph_initialize(&instance, &graph, &asset, &error));
  assert(vkr_animation_graph_evaluate(&instance, player));
  graph_test_position(player, 2.5f);
  assert(vkr_animation_graph_set_parameter(&instance, 0u, 2.0f));
  assert(vkr_animation_graph_evaluate(&instance, player));
  graph_test_position(player, 10.0f);
  assert(!vkr_animation_graph_set_parameter(&instance, 2u, 0.0f));
  assert(!vkr_animation_graph_set_parameter(&instance, 0u, NAN));
  /* Unequal clip durations sample the same normalized phase. */
  graph.nodes[0].clip = 3u;
  graph.nodes[1].clip = 4u;
  assert(vkr_animation_graph_initialize(&instance, &graph, &asset, &error));
  assert(vkr_animation_graph_seek(&instance, player, 0.25));
  graph_test_position(player, 2.0f);
  assert(vkr_animation_graph_initialize(&other, &graph, &asset, &error));
  for (uint32_t i = 0; i < 30u; ++i) {
    assert(vkr_animation_graph_advance(&other, player, 0.75 / 30.0));
  }
  graph_test_position(player, 6.0f);
  assert(vkr_animation_graph_reset(&instance, player));
  for (uint32_t i = 0; i < 144u; ++i) {
    assert(vkr_animation_graph_advance(&instance, player, 0.75 / 144.0));
  }
  graph_test_position(player, 6.0f);
  assert(fabs(instance.time - other.time) < 1e-12);
  const uint32_t rates[] = {30u, 60u, 144u};
  for (uint32_t rate_index = 0; rate_index < ArrayCount(rates); ++rate_index) {
    assert(vkr_animation_graph_reset(&instance, player));
    uint64_t before_wrap = vkr_animation_player_discontinuity(player);
    for (uint32_t tick = 0; tick < rates[rate_index]; ++tick) {
      assert(vkr_animation_graph_advance(&instance, player,
                                         1.0 / rates[rate_index]));
    }
    graph_test_position(player, 0.0f);
    assert(instance.time == 1.0);
    assert(vkr_animation_player_discontinuity(player) == before_wrap + 1u);
  }
  uint64_t generation = vkr_animation_player_generation(player);
  assert(!vkr_animation_graph_advance(&instance, player, NAN));
  assert(!vkr_animation_graph_seek(&instance, player, -1.0));
  assert(vkr_animation_player_generation(player) == generation);
  graph.node_count = 1u;
  graph.root = 0u;
  graph.nodes[0] = (VkrAnimationGraphNode){
      .kind = VKR_ANIMATION_GRAPH_BLENDSPACE1D,
      .sample_count = 3u,
      .samples = {
          {.clip = 2u, .x = 2}, {.clip = 0u, .x = 0}, {.clip = 1u, .x = 1}}};
  graph.parameters[0] = 0.5f;
  assert(vkr_animation_graph_initialize(&instance, &graph, &asset, &error));
  assert(vkr_animation_graph_evaluate(&instance, player));
  graph_test_position(player, 5.0f);
  assert(vkr_animation_graph_set_parameter(&instance, 0u, -1.0f));
  assert(vkr_animation_graph_evaluate(&instance, player));
  graph_test_position(player, 0.0f);
  assert(vkr_animation_graph_set_parameter(&instance, 0u, 5.0f));
  assert(vkr_animation_graph_evaluate(&instance, player));
  graph_test_position(player, 20.0f);
  graph.nodes[0].samples[2].x = 0.0f;
  assert(!vkr_animation_graph_validate(&graph, &asset, &error));
  graph.nodes[0] =
      (VkrAnimationGraphNode){.kind = VKR_ANIMATION_GRAPH_BLENDSPACE2D,
                              .parameter_x = 0u,
                              .parameter_y = 1u,
                              .sample_count = 3u,
                              .triangle_count = 1u,
                              .samples = {{.clip = 0u, .x = 0, .y = 0},
                                          {.clip = 1u, .x = 1, .y = 0},
                                          {.clip = 2u, .x = 0, .y = 1}},
                              .triangles = {{{0, 1, 2}}}};
  graph.parameters[0] = graph.parameters[1] = 0.25f;
  assert(vkr_animation_graph_initialize(&instance, &graph, &asset, &error));
  assert(vkr_animation_graph_evaluate(&instance, player));
  graph_test_position(player, 7.5f);
  assert(vkr_animation_graph_set_parameter(&instance, 0u, 1.0f));
  assert(vkr_animation_graph_set_parameter(&instance, 1u, 1.0f));
  assert(vkr_animation_graph_evaluate(&instance, player));
  graph_test_position(player, 15.0f);
  graph.nodes[0].samples[2].y = 0.0f;
  assert(!vkr_animation_graph_validate(&graph, &asset, &error));
  graph.nodes[0].samples[2].y = 1.0f;
  GraphJsonTestBuffer buffer = {0};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, graph_test_write, &buffer);
  assert(vkr_animation_graph_write_json(&writer, &graph));
  assert(vkr_json_writer_complete(&writer));
  VkrJsonReader reader = vkr_json_reader_create(buffer.data, buffer.length);
  VkrAnimationGraph decoded;
  assert(vkr_animation_graph_read_json(&reader, NULL, &decoded, &error));
  assert(vkr_animation_graph_initialize(&instance, &decoded, &asset, &error));
  assert(vkr_animation_graph_evaluate(&instance, player));
  graph_test_position(player, 7.5f);
  static const char malformed[] = "{\"version\":1,\"version\":1}";
  reader = vkr_json_reader_create((const uint8_t *)malformed,
                                  sizeof(malformed) - 1u);
  assert(!vkr_animation_graph_read_json(&reader, &asset, &decoded, &error));
  assert(reader.pos == 0u);
  graph = (VkrAnimationGraph){.node_count = 3u,
                              .parameter_count = 1u,
                              .cycle_seconds = 1.0,
                              .state_count = 3u,
                              .transition_count = 2u,
                              .parameters = {1.0f},
                              .states = {{.root = 0u, .cycle_seconds = 1.0},
                                         {.root = 1u, .cycle_seconds = 1.0},
                                         {.root = 2u, .cycle_seconds = 1.0}},
                              .transitions = {{.from = 0u,
                                               .to = 1u,
                                               .duration = 1.0,
                                               .exit_time = 0.5,
                                               .threshold = 0.5f},
                                              {.from = 1u,
                                               .to = 2u,
                                               .duration = 1.0,
                                               .exit_time = -1.0,
                                               .threshold = 0.5f}}};
  for (uint32_t i = 0; i < 3u; ++i) {
    graph.nodes[i].clip = i;
  }
  assert(vkr_animation_graph_initialize(&instance, &graph, &asset, &error));
  assert(vkr_animation_graph_advance(&instance, player, 0.75));
  graph_test_position(player, 2.5f);
  assert(instance.transitioning && instance.state == 0u);
  assert(vkr_animation_graph_advance(&instance, player, 1.0));
  graph_test_position(player, 12.5f);
  assert(instance.state == 1u && instance.transitioning);
  assert(vkr_animation_graph_initialize(&other, &graph, &asset, &error));
  for (uint32_t i = 0; i < 7u; ++i) {
    assert(vkr_animation_graph_advance(&other, player, 0.25));
  }
  graph_test_position(player, 12.5f);
  assert(fabs(instance.transition_time - other.transition_time) < 1e-12);
  assert(vkr_animation_graph_initialize(&instance, &graph, &asset, &error));
  assert(vkr_animation_graph_advance(&instance, player, 0.75));
  assert(vkr_animation_graph_set_parameter(&instance, 0u, 0.0f));
  assert(vkr_animation_graph_advance(&instance, player, 0.25));
  graph_test_position(player,
                      5.0f); /* Parameter changes do not interrupt a fade. */
  graph.transitions[0].duration = 0.0;
  graph.transitions[0].exit_time = -1.0;
  graph.transition_count = 1u;
  assert(vkr_animation_graph_initialize(&instance, &graph, &asset, &error));
  assert(vkr_animation_graph_evaluate(&instance, player));
  uint64_t before_cut = vkr_animation_player_discontinuity(player);
  assert(vkr_animation_graph_advance(&instance, player, 0.0));
  graph_test_position(player, 10.0f);
  assert(vkr_animation_player_discontinuity(player) == before_cut + 1u);
  graph.transition_count = 2u;
  graph.transitions[1] = (VkrAnimationGraphTransition){
      .from = 1u, .to = 0u, .threshold = 0.5f, .exit_time = -1.0};
  assert(vkr_animation_graph_initialize(&instance, &graph, &asset, &error));
  generation = vkr_animation_player_generation(player);
  assert(!vkr_animation_graph_advance(&instance, player, 0.0));
  assert(instance.time == 0.0 && instance.state == 0u);
  assert(vkr_animation_player_generation(player) == generation);
  /* A pose cycle and exponential DAG expansion are rejected before playback. */
  graph.state_count = graph.transition_count = 0u;
  graph.nodes[0] = (VkrAnimationGraphNode){.kind = VKR_ANIMATION_GRAPH_BLEND2,
                                           .inputs = {0u, 1u}};
  assert(!vkr_animation_graph_validate(&graph, &asset, &error));
  graph.node_count = 7u;
  graph.root = 6u;
  graph.nodes[0] = (VkrAnimationGraphNode){0};
  for (uint32_t i = 1; i < 7u; ++i) {
    graph.nodes[i] = (VkrAnimationGraphNode){.kind = VKR_ANIMATION_GRAPH_BLEND2,
                                             .inputs = {i - 1u, i - 1u}};
  }
  assert(!vkr_animation_graph_validate(&graph, &asset, &error));
  vkr_animation_player_destroy(player);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(arena);
  return true_v;
}
