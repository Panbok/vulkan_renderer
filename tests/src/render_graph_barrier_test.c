#include "render_graph_barrier_test.h"
#include "renderer/vkr_render_packet.h"
#include "renderer/vkr_rg_json.h"

/**
 * Barrier planning is a deterministic function of the declared graph, so it is
 * testable on the CPU. What is *emitted* to Vulkan is not — that belongs to a
 * validation-layer run. These tests pin the planning rules that a layout-pair
 * comparison could not express:
 *
 *  - a write followed by a read in the same layout still needs a barrier;
 *  - two writes with the same access still need ordering;
 *  - a read after a read in the same layout needs nothing;
 *  - per-layer attachment slices produce per-layer barriers that coalesce again
 *    when a later pass reads the whole image.
 */

static void rg_barrier_test_execute(VkrRgPassContext *ctx, void *user_data) {
  (void)ctx;
  (void)user_data;
}

static bool8_t rg_barrier_test_write_json(const char *path,
                                          const char *contents) {
  FILE *file = fopen(path, "wb");
  if (!file)
    return false_v;
  const size_t size = strlen(contents);
  const bool8_t ok =
      fwrite(contents, 1u, size, file) == size ? true_v : false_v;
  fclose(file);
  return ok;
}

static bool8_t rg_barrier_test_load_json(VkrAllocator *allocator,
                                         const char *contents,
                                         VkrRgJsonGraph *out_graph) {
  const char *path = "build/test_render_graph_contract.rendergraph.json";
  if (!rg_barrier_test_write_json(path, contents))
    return false_v;
  const bool8_t result = vkr_rg_json_load_file(allocator, path, out_graph);
  remove(path);
  return result;
}

/**
 * @brief Declares a pass; caller adds uses via the returned builder.
 *
 * Attachment-less passes must be declared compute: validate_pass rejects a
 * graphics pass with no attachments, and storage read/write is compute work
 * anyway.
 */
static VkrRgPassBuilder rg_barrier_test_add_pass(VkrRenderGraph *graph,
                                                 VkrRgPassType type,
                                                 const char *name) {
  // Name storage must outlive the graph; every caller passes a string literal.
  String8 pass_name =
      string8_create_from_cstr((const uint8_t *)name, string_length(name));
  VkrRgPassBuilder pb = vkr_rg_add_pass(graph, type, pass_name);
  vkr_rg_pass_set_execute(&pb, rg_barrier_test_execute, NULL);
  // Culling removes passes whose outputs nobody consumes; these synthetic
  // graphs exist to be scheduled, so keep every pass.
  vkr_rg_pass_set_flags(&pb, VKR_RG_PASS_FLAG_NO_CULL);
  return pb;
}

static const VkrRgPass *rg_barrier_test_pass(const VkrRenderGraph *graph,
                                             uint32_t index) {
  return vector_get_VkrRgPass((Vector_VkrRgPass *)&graph->passes, index);
}

static void test_resource_instance_domains(void) {
  printf("  Running test_resource_instance_domains...\n");
  assert(vkr_rg_resource_instance_domain(VKR_RG_RESOURCE_FLAG_NONE) ==
         VKR_RG_RESOURCE_INSTANCE_SINGLE);
  assert(vkr_rg_resource_instance_domain(VKR_RG_RESOURCE_FLAG_PERSISTENT) ==
         VKR_RG_RESOURCE_INSTANCE_SINGLE);
  assert(vkr_rg_resource_instance_domain(VKR_RG_RESOURCE_FLAG_TRANSIENT) ==
         VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT);
  assert(vkr_rg_resource_instance_domain(VKR_RG_RESOURCE_FLAG_PER_FRAME_SLOT) ==
         VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT);
  assert(vkr_rg_resource_instance_domain(VKR_RG_RESOURCE_FLAG_HISTORY) ==
         VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT);
  assert(vkr_rg_resource_instance_domain(VKR_RG_RESOURCE_FLAG_TRANSIENT |
                                         VKR_RG_RESOURCE_FLAG_PER_IMAGE) ==
         VKR_RG_RESOURCE_INSTANCE_PER_IMAGE);
  assert(vkr_rg_resource_instance_domain(VKR_RG_RESOURCE_FLAG_PERSISTENT |
                                         VKR_RG_RESOURCE_FLAG_PER_IMAGE) ==
         VKR_RG_RESOURCE_INSTANCE_PER_IMAGE);
  printf("  test_resource_instance_domains PASSED\n");
}

static void test_json_bindings_and_condition_parity(void) {
  printf("  Running test_json_bindings_and_condition_parity...\n");
  Arena *arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  const char *valid =
      "{\"version\":1,\"name\":\"conditions\",\"resources\":["
      "{\"name\":\"image\",\"type\":\"image\",\"extent\":{\"mode\":\"fixed\","
      "\"width\":1,\"height\":1},\"format\":\"R8G8B8A8_UNORM\",\"usage\":["
      "\"SAMPLED\"]}],"
      "\"passes\":["
      "{\"name\":\"editor\",\"type\":\"compute\",\"condition\":\"editor_"
      "enabled\","
      "\"reads\":[{\"image\":\"image\",\"access\":\"SAMPLED\",\"binding\":2}],"
      "\"execute\":\"test\"},"
      "{\"name\":\"history\",\"type\":\"compute\",\"condition\":"
      "\"!hzb_history_valid\",\"execute\":\"test\"},"
      "{\"name\":\"transmission\",\"type\":\"compute\",\"condition\":"
      "\"transmission_pending\",\"execute\":\"test\"},"
      "{\"name\":\"picking-no-transmission\",\"type\":\"compute\","
      "\"condition\":\"picking_pending && !transmission_pending\","
      "\"execute\":\"test\"},"
      "{\"name\":\"picking-transmission\",\"type\":\"compute\","
      "\"condition\":\"picking_pending && transmission_pending\","
      "\"execute\":\"test\"}]}";
  VkrRgJsonGraph graph = {0};
  assert(rg_barrier_test_load_json(&allocator, valid, &graph));
  assert(graph.passes.length == 5u);
  assert(vector_get_VkrRgJsonPass(&graph.passes, 0)->condition.kind ==
         VKR_RG_JSON_CONDITION_EDITOR_ENABLED);
  assert(vector_get_VkrRgJsonPass(&graph.passes, 1)->condition.kind ==
         VKR_RG_JSON_CONDITION_HZB_HISTORY_INVALID);
  assert(vector_get_VkrRgJsonPass(&graph.passes, 2)->condition.kind ==
         VKR_RG_JSON_CONDITION_TRANSMISSION_PENDING);
  assert(vector_get_VkrRgJsonPass(&graph.passes, 3)->condition.kind ==
         VKR_RG_JSON_CONDITION_PICKING_PENDING_NO_TRANSMISSION);
  assert(vector_get_VkrRgJsonPass(&graph.passes, 4)->condition.kind ==
         VKR_RG_JSON_CONDITION_PICKING_PENDING_TRANSMISSION);
  const VkrRgJsonResourceUse *use = vector_get_VkrRgJsonResourceUse(
      &vector_get_VkrRgJsonPass(&graph.passes, 0)->reads, 0u);
  assert(use && use->binding.is_set && use->binding.value == 2u);
  vkr_rg_json_destroy(&graph);

  const char *retired_condition =
      "{\"version\":1,\"name\":\"retired\",\"resources\":[],\"passes\":["
      "{\"name\":\"pass\",\"type\":\"compute\",\"condition\":"
      "\"deferred_enabled\",\"execute\":\"test\"}]}";
  assert(!rg_barrier_test_load_json(&allocator, retired_condition, &graph));

  const char *missing_binding =
      "{\"version\":1,\"name\":\"missing\",\"resources\":["
      "{\"name\":\"image\",\"type\":\"image\",\"extent\":{\"mode\":\"fixed\","
      "\"width\":1,\"height\":1},\"format\":\"R8G8B8A8_UNORM\",\"usage\":["
      "\"SAMPLED\"]}],"
      "\"passes\":[{\"name\":\"pass\",\"type\":\"compute\",\"reads\":[{"
      "\"image\":\"image\",\"access\":\"SAMPLED\"}],\"execute\":\"test\"}]}";
  assert(!rg_barrier_test_load_json(&allocator, missing_binding, &graph));

  const char *type_mismatch =
      "{\"version\":1,\"name\":\"type\",\"resources\":["
      "{\"name\":\"buffer\",\"type\":\"buffer\",\"size\":16,\"usage\":["
      "\"STORAGE\"],\"flags\":[\"PER_FRAME_SLOT\"]}],"
      "\"passes\":[{\"name\":\"pass\",\"type\":\"compute\",\"reads\":[{"
      "\"image\":\"buffer\",\"access\":\"SAMPLED\",\"binding\":0}],\"execute\":"
      "\"test\"}]}";
  assert(!rg_barrier_test_load_json(&allocator, type_mismatch, &graph));

  arena_destroy(arena);
  printf("  test_json_bindings_and_condition_parity PASSED\n");
}

static void test_transmission_condition(void) {
  printf("  Running test_transmission_condition...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  const char *source =
      "{\"version\":1,\"name\":\"transmission\",\"resources\":[],"
      "\"passes\":[{\"name\":\"conditional\",\"type\":\"compute\","
      "\"flags\":[\"NO_CULL\"],\"condition\":\"transmission_pending\","
      "\"execute\":\"test.compute\"}]}";
  VkrRgJsonGraph json = {0};
  assert(rg_barrier_test_load_json(&allocator, source, &json));

  VkrRgExecutorRegistry registry = {0};
  assert(vkr_rg_executor_registry_init(&registry, &allocator));
  const VkrRgPassExecutor executor = {
      .name = string8_lit("test.compute"),
      .id = 1u,
      .type = VKR_RG_PASS_TYPE_COMPUTE,
      .execute = rg_barrier_test_execute,
  };
  assert(vkr_rg_executor_registry_register(&registry, &executor));
  assert(vkr_rg_json_bind_executors(&json, &registry));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  VkrRenderGraphFrameInfo frame = {.target_width = 1u, .target_height = 1u};
  vkr_rg_begin_frame(graph, &frame);
  assert(vkr_rg_build_from_json(graph, &json, &frame));
  assert(graph->passes.length == 0u);
  frame.transmission_pending = true_v;
  vkr_rg_begin_frame(graph, &frame);
  assert(vkr_rg_build_from_json(graph, &json, &frame));
  assert(graph->passes.length == 1u);

  vkr_rg_destroy(graph);
  vkr_rg_executor_registry_destroy(&registry);
  vkr_rg_json_destroy(&json);

  const char *resource_mask_source =
      "{\"version\":1,\"name\":\"resource-mask\",\"resources\":[{"
      "\"name\":\"image.${i}\",\"type\":\"image\",\"extent\":{"
      "\"mode\":\"fixed\",\"width\":1,\"height\":1},\"format\":"
      "\"R8G8B8A8_UNORM\",\"usage\":[\"SAMPLED\"],\"repeat\":{"
      "\"count_source\":\"shadow_cascade_count\","
      "\"condition_mask_source\":\"shadow_cascade_render_mask\"}}],"
      "\"passes\":[]}";
  json = (VkrRgJsonGraph){0};
  assert(!rg_barrier_test_load_json(&allocator, resource_mask_source, &json));
  arena_destroy(arena);
  printf("  test_transmission_condition PASSED\n");
}

static void test_transmission_compact_conditions_and_viewport_buffer(void) {
  printf("  Running "
         "test_transmission_compact_conditions_and_viewport_buffer...\n");
  Arena *arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  const char *source =
      "{\"version\":1,\"name\":\"compact\",\"resources\":[{"
      "\"name\":\"pixels\",\"type\":\"buffer\",\"condition\":"
      "\"transmission_compact_enabled\",\"size\":{\"mode\":"
      "\"viewport_pixels\",\"bytes_per_pixel\":4},\"usage\":["
      "\"STORAGE\"],\"flags\":[\"PER_FRAME_SLOT\"]}],\"passes\":[{"
      "\"name\":\"compact\",\"type\":\"compute\",\"condition\":"
      "\"transmission_compact_enabled\",\"flags\":[\"NO_CULL\"],"
      "\"execute\":\"test.compute\"},{\"name\":\"compact-editor\","
      "\"type\":\"compute\",\"condition\":"
      "\"editor_enabled && transmission_compact_enabled\",\"flags\":["
      "\"NO_CULL\"],\"execute\":\"test.compute\"},{\"name\":"
      "\"compact-fullscreen\",\"type\":\"compute\",\"condition\":"
      "\"!editor_enabled && transmission_compact_enabled\",\"flags\":["
      "\"NO_CULL\"],\"execute\":\"test.compute\"},{\"name\":"
      "\"shade-editor\",\"type\":\"compute\",\"condition\":"
      "\"editor_enabled && transmission_pending && "
      "!transmission_compact_enabled\",\"flags\":[\"NO_CULL\"],"
      "\"execute\":\"test.compute\"},{\"name\":\"shade-fullscreen\","
      "\"type\":\"compute\",\"condition\":"
      "\"!editor_enabled && transmission_pending && "
      "!transmission_compact_enabled\",\"flags\":[\"NO_CULL\"],"
      "\"execute\":\"test.compute\"},{\"name\":\"coverage\","
      "\"type\":\"compute\",\"condition\":"
      "\"transmission_pending && timing_enabled && "
      "!transmission_compact_enabled\",\"flags\":[\"NO_CULL\"],"
      "\"execute\":\"test.compute\"}]}";
  VkrRgJsonGraph json = {0};
  assert(rg_barrier_test_load_json(&allocator, source, &json));
  assert(json.resources.data[0].buffer.size_mode ==
         VKR_RG_JSON_BUFFER_SIZE_VIEWPORT_PIXELS);
  assert(json.resources.data[0].buffer.bytes_per_pixel == 4u);
  assert(json.passes.data[0].condition.kind ==
         VKR_RG_JSON_CONDITION_TRANSMISSION_COMPACT_ENABLED);
  assert(json.passes.data[1].condition.kind ==
         VKR_RG_JSON_CONDITION_EDITOR_ENABLED_TRANSMISSION_COMPACT);
  assert(json.passes.data[2].condition.kind ==
         VKR_RG_JSON_CONDITION_EDITOR_DISABLED_TRANSMISSION_COMPACT);
  assert(json.passes.data[3].condition.kind ==
         VKR_RG_JSON_CONDITION_EDITOR_ENABLED_TRANSMISSION_FULLSCREEN);
  assert(json.passes.data[4].condition.kind ==
         VKR_RG_JSON_CONDITION_EDITOR_DISABLED_TRANSMISSION_FULLSCREEN);
  assert(json.passes.data[5].condition.kind ==
         VKR_RG_JSON_CONDITION_TRANSMISSION_FULLSCREEN_TIMING);

  VkrRgExecutorRegistry registry = {0};
  assert(vkr_rg_executor_registry_init(&registry, &allocator));
  const VkrRgPassExecutor executor = {
      .name = string8_lit("test.compute"),
      .id = 1u,
      .type = VKR_RG_PASS_TYPE_COMPUTE,
      .execute = rg_barrier_test_execute,
  };
  assert(vkr_rg_executor_registry_register(&registry, &executor));
  assert(vkr_rg_json_bind_executors(&json, &registry));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  VkrRenderGraphFrameInfo frame = {
      .viewport_width = 320u,
      .viewport_height = 200u,
      .transmission_pending = true_v,
      .transmission_compact_enabled = true_v,
  };
  vkr_rg_begin_frame(graph, &frame);
  assert(vkr_rg_build_from_json(graph, &json, &frame));
  assert(graph->buffers.length == 1u);
  assert(graph->buffers.data[0].desc.size == 320u * 200u * 4u);
  assert(graph->passes.length == 2u);

  frame.viewport_width = UINT32_MAX;
  frame.viewport_height = UINT32_MAX;
  vkr_rg_begin_frame(graph, &frame);
  assert(!vkr_rg_build_from_json(graph, &json, &frame));

  const char *bad_stride =
      "{\"version\":1,\"name\":\"bad\",\"resources\":[{\"name\":"
      "\"pixels\",\"type\":\"buffer\",\"size\":{\"mode\":"
      "\"viewport_pixels\",\"bytes_per_pixel\":0},\"usage\":["
      "\"STORAGE\"],\"flags\":[\"PER_FRAME_SLOT\"]}],\"passes\":[]}";
  VkrRgJsonGraph rejected = {0};
  assert(!rg_barrier_test_load_json(&allocator, bad_stride, &rejected));

  vkr_rg_destroy(graph);
  vkr_rg_executor_registry_destroy(&registry);
  vkr_rg_json_destroy(&json);
  arena_destroy(arena);
  printf("  test_transmission_compact_conditions_and_viewport_buffer "
         "PASSED\n");
}

static void test_shadow_map_capacity_is_independent_of_active_cascades(void) {
  printf("  Running "
         "test_shadow_map_capacity_is_independent_of_active_cascades...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  const char *source =
      "{\"version\":1,\"name\":\"shadow-capacity\",\"resources\":[{"
      "\"name\":\"shadow\",\"type\":\"image\",\"extent\":{\"mode\":"
      "\"fixed\",\"width\":16,\"height\":16},\"layers_source\":"
      "\"shadow_map_layer_count\",\"format\":\"D32_SFLOAT\",\"usage\":["
      "\"DEPTH_STENCIL_ATTACHMENT\"]}],\"passes\":[]}";
  VkrRgJsonGraph json = {0};
  assert(rg_barrier_test_load_json(&allocator, source, &json));
  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);

  const VkrRenderGraphFrameInfo frame = {
      .target_width = 16u,
      .target_height = 16u,
      .shadow_map_layer_count = 4u,
      .shadow_cascade_count = 0u,
  };
  vkr_rg_begin_frame(graph, &frame);
  assert(vkr_rg_build_from_json(graph, &json, &frame));
  assert(graph->images.length == 1u);
  assert(graph->images.data[0].desc.layers == 4u);

  vkr_rg_destroy(graph);
  vkr_rg_json_destroy(&json);
  arena_destroy(arena);
  printf("  test_shadow_map_capacity_is_independent_of_active_cascades "
         "PASSED\n");
}

static void test_shadow_reads_follow_active_cascades(void) {
  printf("  Running test_shadow_reads_follow_active_cascades...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  const char *source =
      "{\"version\":1,\"name\":\"conditional-shadow-read\",\"resources\":[{"
      "\"name\":\"shadow\",\"type\":\"image\",\"extent\":{\"mode\":"
      "\"fixed\",\"width\":16,\"height\":16},\"format\":\"D32_SFLOAT\","
      "\"usage\":[\"SAMPLED\"]}],\"passes\":[{\"name\":\"consumer\","
      "\"type\":\"compute\",\"flags\":[\"NO_CULL\"],\"reads\":[{\"image\":"
      "\"shadow\",\"access\":\"SAMPLED\",\"binding\":0,\"when\":"
      "\"shadow_cascades_active\"}],\"execute\":\"test.compute\"}]}";
  VkrRgJsonGraph json = {0};
  assert(rg_barrier_test_load_json(&allocator, source, &json));
  const VkrRgJsonResourceUse *parsed_use =
      vector_get_VkrRgJsonResourceUse(&json.passes.data[0].reads, 0u);
  assert(parsed_use && parsed_use->condition.kind ==
                           VKR_RG_JSON_CONDITION_SHADOW_CASCADES_ACTIVE);

  VkrRgExecutorRegistry registry = {0};
  assert(vkr_rg_executor_registry_init(&registry, &allocator));
  const VkrRgPassExecutor executor = {
      .name = string8_lit("test.compute"),
      .id = 1u,
      .type = VKR_RG_PASS_TYPE_COMPUTE,
      .execute = rg_barrier_test_execute,
  };
  assert(vkr_rg_executor_registry_register(&registry, &executor));
  assert(vkr_rg_json_bind_executors(&json, &registry));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  VkrRenderGraphFrameInfo frame = {
      .target_width = 16u,
      .target_height = 16u,
  };
  vkr_rg_begin_frame(graph, &frame);
  assert(vkr_rg_build_from_json(graph, &json, &frame));
  assert(graph->passes.length == 1u);
  assert(graph->passes.data[0].desc.image_reads.length == 0u);

  frame.shadow_cascade_count = 4u;
  vkr_rg_begin_frame(graph, &frame);
  assert(vkr_rg_build_from_json(graph, &json, &frame));
  assert(graph->passes.length == 1u);
  assert(graph->passes.data[0].desc.image_reads.length == 1u);

  vkr_rg_destroy(graph);
  vkr_rg_executor_registry_destroy(&registry);
  vkr_rg_json_destroy(&json);
  arena_destroy(arena);
  printf("  test_shadow_reads_follow_active_cascades PASSED\n");
}

static void test_repeat_condition_mask_filters_iterations(void) {
  printf("  Running test_repeat_condition_mask_filters_iterations...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  const char *source =
      "{\"version\":1,\"name\":\"masked-repeat\",\"resources\":[],"
      "\"passes\":[{\"name\":\"cascade.${i}\",\"type\":\"compute\","
      "\"flags\":[\"NO_CULL\"],\"repeat\":{\"count_source\":"
      "\"shadow_cascade_count\",\"condition_mask_source\":"
      "\"shadow_cascade_render_mask\"},\"execute\":\"test.compute\"}]}";
  VkrRgJsonGraph json = {0};
  assert(rg_barrier_test_load_json(&allocator, source, &json));
  assert(json.passes.length == 1u);
  assert(json.passes.data[0].repeat.enabled);
  assert(
      vkr_string8_equals_cstr(&json.passes.data[0].repeat.condition_mask_source,
                              "shadow_cascade_render_mask"));

  VkrRgExecutorRegistry registry = {0};
  assert(vkr_rg_executor_registry_init(&registry, &allocator));
  const VkrRgPassExecutor executor = {
      .name = string8_lit("test.compute"),
      .id = 1u,
      .type = VKR_RG_PASS_TYPE_COMPUTE,
      .execute = rg_barrier_test_execute,
  };
  assert(vkr_rg_executor_registry_register(&registry, &executor));
  assert(vkr_rg_json_bind_executors(&json, &registry));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  const VkrRenderGraphFrameInfo frame = {
      .target_width = 1u,
      .target_height = 1u,
      .shadow_cascade_count = 4u,
      .shadow_cascade_render_mask = 0x5u,
  };
  vkr_rg_begin_frame(graph, &frame);
  assert(vkr_rg_build_from_json(graph, &json, &frame));
  assert(graph->passes.length == 2u);
  assert(
      vkr_string8_equals_cstr(&graph->passes.data[0].desc.name, "cascade.0"));
  assert(
      vkr_string8_equals_cstr(&graph->passes.data[1].desc.name, "cascade.2"));

  vkr_rg_destroy(graph);
  vkr_rg_json_destroy(&json);

  const char *unknown_source =
      "{\"version\":1,\"name\":\"unknown-mask\",\"resources\":[],"
      "\"passes\":[{\"name\":\"cascade.${i}\",\"type\":\"compute\","
      "\"flags\":[\"NO_CULL\"],\"repeat\":{\"count_source\":"
      "\"shadow_cascade_count\",\"condition_mask_source\":"
      "\"not_a_mask\"},\"execute\":\"test.compute\"}]}";
  json = (VkrRgJsonGraph){0};
  assert(rg_barrier_test_load_json(&allocator, unknown_source, &json));
  assert(vkr_rg_json_bind_executors(&json, &registry));
  graph = vkr_rg_create(&allocator);
  assert(graph);
  vkr_rg_begin_frame(graph, &frame);
  assert(!vkr_rg_build_from_json(graph, &json, &frame));
  vkr_rg_destroy(graph);
  vkr_rg_executor_registry_destroy(&registry);
  vkr_rg_json_destroy(&json);
  arena_destroy(arena);
  printf("  test_repeat_condition_mask_filters_iterations PASSED\n");
}

static void test_conflicting_runtime_bindings_are_rejected(void) {
  printf("  Running test_conflicting_runtime_bindings_are_rejected...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 1u;
  desc.height = 1u;
  desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_SAMPLED);
  VkrRgImageHandle first =
      vkr_rg_create_image(graph, string8_lit("first"), &desc);
  VkrRgImageHandle second =
      vkr_rg_create_image(graph, string8_lit("second"), &desc);
  VkrRgPassBuilder pass =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "Bindings");
  vkr_rg_pass_read_image(&pass, first, VKR_RG_IMAGE_ACCESS_SAMPLED, 3u, 0u);
  vkr_rg_pass_read_image(&pass, second, VKR_RG_IMAGE_ACCESS_SAMPLED, 3u, 0u);
  assert(!vkr_rg_compile_schedule(graph));
  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_conflicting_runtime_bindings_are_rejected PASSED\n");
}

static void test_typed_executor_and_direct_dispatch_contract(void) {
  printf("  Running test_typed_executor_and_direct_dispatch_contract...\n");
  Arena *arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  const char *valid =
      "{\"version\":1,\"name\":\"dispatch\",\"resources\":[],\"passes\":["
      "{\"name\":\"compute\",\"type\":\"compute\",\"flags\":[\"NO_CULL\"],"
      "\"dispatch\":{\"type\":\"direct\",\"x\":8,\"y\":4,\"z\":2},"
      "\"execute\":\"test.compute\"}]}";
  VkrRgJsonGraph json = {0};
  assert(rg_barrier_test_load_json(&allocator, valid, &json));
  assert(json.passes.length == 1u);
  assert(json.passes.data[0].dispatch.kind == VKR_RG_DISPATCH_DIRECT);
  assert(json.passes.data[0].dispatch.group_count_x == 8u);
  assert(json.passes.data[0].dispatch.group_count_y == 4u);
  assert(json.passes.data[0].dispatch.group_count_z == 2u);

  VkrRgExecutorRegistry registry = {0};
  assert(vkr_rg_executor_registry_init(&registry, &allocator));
  VkrRgPassExecutor executor = {
      .name = string8_lit("test.compute"),
      .id = 7u,
      .type = VKR_RG_PASS_TYPE_COMPUTE,
      .execute = rg_barrier_test_execute,
  };
  assert(vkr_rg_executor_registry_register(&registry, &executor));
  VkrRgPassExecutor duplicate_id = executor;
  duplicate_id.name = string8_lit("test.other");
  assert(!vkr_rg_executor_registry_register(&registry, &duplicate_id));
  assert(vkr_rg_json_bind_executors(&json, &registry));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  VkrRenderGraphFrameInfo frame = {.target_width = 1u, .target_height = 1u};
  vkr_rg_begin_frame(graph, &frame);
  assert(vkr_rg_build_from_json(graph, &json, &frame));
  assert(graph->passes.length == 1u);
  assert(graph->passes.data[0].desc.executor_id == 7u);
  assert(graph->passes.data[0].desc.dispatch.kind == VKR_RG_DISPATCH_DIRECT);
  assert(graph->passes.data[0].desc.dispatch.group_count_x == 8u);
  assert(vkr_rg_compile_schedule(graph));

  const char *invalid =
      "{\"version\":1,\"name\":\"bad\",\"resources\":[],\"passes\":["
      "{\"name\":\"compute\",\"type\":\"compute\",\"dispatch\":{"
      "\"type\":\"direct\",\"x\":0,\"y\":1,\"z\":1},"
      "\"execute\":\"test.compute\"}]}";
  VkrRgJsonGraph rejected = {0};
  assert(!rg_barrier_test_load_json(&allocator, invalid, &rejected));

  vkr_rg_destroy(graph);
  vkr_rg_executor_registry_destroy(&registry);
  vkr_rg_json_destroy(&json);
  arena_destroy(arena);
  printf("  test_typed_executor_and_direct_dispatch_contract PASSED\n");
}

static void test_indirect_dispatch_dependency_contract(void) {
  printf("  Running test_indirect_dispatch_dependency_contract...\n");
  Arena *arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  const char *source =
      "{\"version\":1,\"name\":\"indirect\",\"resources\":[{"
      "\"name\":\"arguments\",\"type\":\"buffer\",\"size\":64,"
      "\"usage\":[\"STORAGE\",\"INDIRECT\"],\"flags\":[\"PER_FRAME_SLOT\"]"
      "}],\"passes\":[{\"name\":\"write\",\"type\":\"compute\","
      "\"flags\":[\"NO_CULL\"],\"writes\":[{\"buffer\":\"arguments\","
      "\"access\":\"STORAGE_WRITE\",\"binding\":0}],\"dispatch\":{"
      "\"type\":\"direct\",\"x\":1,\"y\":1,\"z\":1},\"execute\":"
      "\"test.compute\"},{\"name\":\"consume\",\"type\":\"compute\","
      "\"flags\":[\"NO_CULL\"],\"reads\":[{\"buffer\":\"arguments\","
      "\"access\":\"INDIRECT_READ\",\"binding\":2}],\"dispatch\":{"
      "\"type\":\"indirect\",\"binding\":2,\"offset\":16},\"execute\":"
      "\"test.compute\"}]}";
  VkrRgJsonGraph json = {0};
  assert(rg_barrier_test_load_json(&allocator, source, &json));
  assert(json.passes.data[1].dispatch.kind == VKR_RG_DISPATCH_INDIRECT);
  assert(json.passes.data[1].dispatch.indirect_binding == 2u);
  assert(json.passes.data[1].dispatch.indirect_offset == 16u);

  VkrRgExecutorRegistry registry = {0};
  assert(vkr_rg_executor_registry_init(&registry, &allocator));
  const VkrRgPassExecutor executor = {
      .name = string8_lit("test.compute"),
      .id = 1u,
      .type = VKR_RG_PASS_TYPE_COMPUTE,
      .execute = rg_barrier_test_execute,
  };
  assert(vkr_rg_executor_registry_register(&registry, &executor));
  assert(vkr_rg_json_bind_executors(&json, &registry));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  const VkrRenderGraphFrameInfo frame = {.target_width = 1u,
                                         .target_height = 1u};
  vkr_rg_begin_frame(graph, &frame);
  assert(vkr_rg_build_from_json(graph, &json, &frame));
  assert(vkr_rg_compile_schedule(graph));
  const VkrRgPass *consumer = rg_barrier_test_pass(graph, 1u);
  assert(consumer->pre_buffer_barriers.length == 1u);
  const VkrRgBufferBarrier *barrier =
      vector_get_VkrRgBufferBarrier(&consumer->pre_buffer_barriers, 0u);
  assert(barrier->src_access == VKR_RG_BUFFER_ACCESS_STORAGE_WRITE);
  assert(barrier->dst_access == VKR_RG_BUFFER_ACCESS_INDIRECT_READ);
  assert(barrier->dependency.dst_stages == VKR_GPU_STAGE_DRAW_INDIRECT);

  vkr_rg_destroy(graph);
  vkr_rg_executor_registry_destroy(&registry);
  vkr_rg_json_destroy(&json);
  arena_destroy(arena);
  printf("  test_indirect_dispatch_dependency_contract PASSED\n");
}

static void test_json_mip_chain_and_subresource_uses(void) {
  printf("  Running test_json_mip_chain_and_subresource_uses...\n");
  Arena *arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  const char *source =
      "{\"version\":1,\"name\":\"mips\",\"resources\":[{\"name\":\"hzb\","
      "\"type\":\"image\",\"extent\":{\"mode\":\"fixed\",\"width\":16,"
      "\"height\":8},\"format\":\"R32_SFLOAT\",\"mip_levels\":\"full_chain\","
      "\"usage\":[\"STORAGE\"],\"flags\":[\"PER_FRAME_SLOT\"]}],\"passes\":["
      "{\"name\":\"write\",\"type\":\"compute\",\"flags\":[\"NO_CULL\"],"
      "\"writes\":[{\"image\":\"hzb\",\"access\":\"STORAGE_WRITE\","
      "\"binding\":0,\"subresource\":{\"base_mip\":1,\"mip_count\":2,"
      "\"base_layer\":0,\"layer_count\":1}}],\"dispatch\":{\"type\":"
      "\"direct\",\"x\":1,\"y\":1,\"z\":1},\"execute\":\"test.compute\"},"
      "{\"name\":\"read\",\"type\":\"compute\",\"flags\":[\"NO_CULL\"],"
      "\"reads\":[{\"image\":\"hzb\",\"access\":\"STORAGE_READ\","
      "\"binding\":0,\"subresource\":{\"base_mip\":1,\"mip_count\":2,"
      "\"base_layer\":0,\"layer_count\":1}}],\"dispatch\":{\"type\":"
      "\"direct\",\"x\":1,\"y\":1,\"z\":1},\"execute\":\"test.compute\"}]}";
  VkrRgJsonGraph json = {0};
  assert(rg_barrier_test_load_json(&allocator, source, &json));
  assert(json.resources.data[0].image.mip_levels_full);
  assert(json.passes.data[0].writes.data[0].has_slice);

  VkrRgExecutorRegistry registry = {0};
  assert(vkr_rg_executor_registry_init(&registry, &allocator));
  const VkrRgPassExecutor executor = {
      .name = string8_lit("test.compute"),
      .id = 1u,
      .type = VKR_RG_PASS_TYPE_COMPUTE,
      .execute = rg_barrier_test_execute,
  };
  assert(vkr_rg_executor_registry_register(&registry, &executor));
  assert(vkr_rg_json_bind_executors(&json, &registry));
  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  const VkrRenderGraphFrameInfo frame = {.target_width = 16u,
                                         .target_height = 8u};
  vkr_rg_begin_frame(graph, &frame);
  assert(vkr_rg_build_from_json(graph, &json, &frame));
  assert(graph->images.data[0].desc.mip_levels == 5u);
  assert(graph->passes.data[0].desc.image_writes.data[0].slice.mip_level == 1u);
  assert(graph->passes.data[0].desc.image_writes.data[0].slice.mip_count == 2u);
  assert(vkr_rg_compile_schedule(graph));
  assert(graph->passes.data[1].pre_image_barriers.length == 2u);
  assert(graph->passes.data[1].pre_image_barriers.data[0].range.base_mip == 1u);
  assert(graph->passes.data[1].pre_image_barriers.data[1].range.base_mip == 2u);

  vkr_rg_destroy(graph);
  vkr_rg_executor_registry_destroy(&registry);
  vkr_rg_json_destroy(&json);
  arena_destroy(arena);
  printf("  test_json_mip_chain_and_subresource_uses PASSED\n");
}

static void test_deferred_image_formats(void) {
  printf("  Running test_deferred_image_formats...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  const char *source =
      "{\"version\":1,\"name\":\"deferred_formats\",\"resources\":["
      "{\"name\":\"visibility\",\"type\":\"image\",\"extent\":{"
      "\"mode\":\"fixed\",\"width\":4,\"height\":4},\"format\":"
      "\"R32G32_UINT\",\"usage\":[\"COLOR_ATTACHMENT\",\"STORAGE\"]},"
      "{\"name\":\"normal\",\"type\":\"image\",\"extent\":{"
      "\"mode\":\"fixed\",\"width\":4,\"height\":4},\"format\":"
      "\"R16G16_SNORM\",\"usage\":[\"SAMPLED\",\"STORAGE\"]}],"
      "\"passes\":[]}";
  VkrRgJsonGraph graph = {0};
  assert(rg_barrier_test_load_json(&allocator, source, &graph));
  assert(graph.resources.length == 2u);
  assert(graph.resources.data[0].image.format ==
         VKR_TEXTURE_FORMAT_R32G32_UINT);
  assert(graph.resources.data[1].image.format ==
         VKR_TEXTURE_FORMAT_R16G16_SNORM);

  VkrTextureFormatInfo info = {0};
  assert(vkr_texture_format_get_info(VKR_TEXTURE_FORMAT_R32G32_UINT, &info));
  assert(info.channel_count == 2u && info.bytes_per_block == 8u);
  assert(vkr_texture_format_get_info(VKR_TEXTURE_FORMAT_R16G16_SNORM, &info));
  assert(info.channel_count == 2u && info.bytes_per_block == 4u);

  vkr_rg_json_destroy(&graph);
  arena_destroy(arena);
  printf("  test_deferred_image_formats PASSED\n");
}

static void test_same_layout_write_then_read_emits_barrier(void) {
  printf("  Running test_same_layout_write_then_read_emits_barrier...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph != NULL);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 64;
  desc.height = 64;
  desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_STORAGE);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("storage"), &desc);

  // Both passes use STORAGE, which resolves to the same GENERAL layout, so a
  // layout comparison alone would see no transition and emit nothing.
  VkrRgPassBuilder writer =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "Writer");
  vkr_rg_pass_write_image(&writer, image, VKR_RG_IMAGE_ACCESS_STORAGE_WRITE, 0,
                          0);
  VkrRgPassBuilder reader =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "Reader");
  vkr_rg_pass_read_image(&reader, image, VKR_RG_IMAGE_ACCESS_STORAGE_READ, 0,
                         0);

  assert(vkr_rg_compile_schedule(graph));

  const VkrRgPass *reader_pass = rg_barrier_test_pass(graph, 1);
  assert(reader_pass->pre_image_barriers.length == 1);
  const VkrRgImageBarrier *barrier = vector_get_VkrRgImageBarrier(
      (Vector_VkrRgImageBarrier *)&reader_pass->pre_image_barriers, 0);
  assert(barrier->src_access == VKR_RG_IMAGE_ACCESS_STORAGE_WRITE);
  assert(barrier->dst_access == VKR_RG_IMAGE_ACCESS_STORAGE_READ);
  assert(barrier->src_layout == barrier->dst_layout);
  assert(barrier->dependency.src_stages ==
         (VKR_GPU_STAGE_ALL_GRAPHICS | VKR_GPU_STAGE_COMPUTE_SHADER));
  assert(barrier->dependency.dst_stages ==
         (VKR_GPU_STAGE_ALL_GRAPHICS | VKR_GPU_STAGE_COMPUTE_SHADER));
  assert(barrier->dependency.visibility == VKR_GPU_VISIBILITY_DEVICE);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_same_layout_write_then_read_emits_barrier PASSED\n");
}

static void test_explicit_stage_and_subresource_dependency(void) {
  printf("  Running test_explicit_stage_and_subresource_dependency...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph != NULL);

  VkrRgImageDesc source_desc = VKR_RG_IMAGE_DESC_DEFAULT;
  source_desc.width = 32;
  source_desc.height = 32;
  source_desc.layers = 3;
  source_desc.usage = vkr_texture_usage_flags_from_bits(
      VKR_TEXTURE_USAGE_STORAGE | VKR_TEXTURE_USAGE_SAMPLED);
  VkrRgImageHandle source =
      vkr_rg_create_image(graph, string8_lit("staged_source"), &source_desc);

  VkrRgImageDesc target_desc = VKR_RG_IMAGE_DESC_DEFAULT;
  target_desc.width = 32;
  target_desc.height = 32;
  target_desc.usage =
      vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_COLOR_ATTACHMENT);
  VkrRgImageHandle target =
      vkr_rg_create_image(graph, string8_lit("staged_target"), &target_desc);

  VkrRgPassBuilder writer =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "ComputeWrite");
  vkr_rg_pass_write_image_at_stages(&writer, source,
                                    VKR_RG_IMAGE_ACCESS_STORAGE_WRITE,
                                    VKR_GPU_STAGE_COMPUTE_SHADER, 0, 0);

  VkrRgPassBuilder reader = rg_barrier_test_add_pass(
      graph, VKR_RG_PASS_TYPE_GRAPHICS, "FragmentRead");
  vkr_rg_pass_read_image_slice_at_stages(
      &reader, source, VKR_RG_IMAGE_ACCESS_SAMPLED,
      VKR_GPU_STAGE_FRAGMENT_SHADER, 0, 0,
      (VkrRgImageSlice){.mip_level = 0, .base_layer = 1, .layer_count = 1});
  VkrRgAttachmentDesc attachment = {.slice = VKR_RG_IMAGE_SLICE_DEFAULT};
  vkr_rg_pass_add_color_attachment(&reader, target, &attachment);

  assert(vkr_rg_compile_schedule(graph));
  const VkrRgPass *compiled = rg_barrier_test_pass(graph, 1);
  assert(compiled->pre_image_barriers.length == 2);

  const VkrRgImageBarrier *source_barrier = NULL;
  for (uint64_t i = 0; i < compiled->pre_image_barriers.length; ++i) {
    const VkrRgImageBarrier *candidate = vector_get_VkrRgImageBarrier(
        (Vector_VkrRgImageBarrier *)&compiled->pre_image_barriers, i);
    if (candidate->image.id == source.id) {
      source_barrier = candidate;
      break;
    }
  }
  assert(source_barrier != NULL);
  assert(source_barrier->range.base_layer == 1);
  assert(source_barrier->range.layer_count == 1);
  assert(source_barrier->dependency.src_stages == VKR_GPU_STAGE_COMPUTE_SHADER);
  assert(source_barrier->dependency.dst_stages ==
         VKR_GPU_STAGE_FRAGMENT_SHADER);
  assert(source_barrier->dependency.visibility == VKR_GPU_VISIBILITY_DEVICE);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_explicit_stage_and_subresource_dependency PASSED\n");
}

static void test_present_target_import_and_terminal_states(void) {
  printf("  Running test_present_target_import_and_terminal_states...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  for (uint32_t windowed = 0; windowed < 2; ++windowed) {
    VkrRenderGraph *graph = vkr_rg_create(&allocator);
    assert(graph != NULL);
    VkrRenderGraphFrameInfo frame = {
        .target_terminal_state =
            windowed
                ? (VkrPresentTargetImageState){
                      .access = VKR_IMAGE_ACCESS_PRESENT,
                      .layout = VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR,
                  }
                : (VkrPresentTargetImageState){
                      .access = VKR_IMAGE_ACCESS_COLOR_ATTACHMENT,
                      .layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  },
    };
    vkr_rg_begin_frame(graph, &frame);

    VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
    desc.width = 32;
    desc.height = 32;
    desc.usage = vkr_texture_usage_flags_from_bits(
        VKR_TEXTURE_USAGE_COLOR_ATTACHMENT | VKR_TEXTURE_USAGE_TRANSFER_SRC);
    VkrRgImageHandle target =
        vkr_rg_import_image(graph, string8_lit("swapchain"), NULL,
                            windowed ? VKR_RG_IMAGE_ACCESS_PRESENT
                                     : VKR_RG_IMAGE_ACCESS_TRANSFER_SRC,
                            windowed ? VKR_TEXTURE_LAYOUT_UNDEFINED
                                     : VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            &desc);
    VkrRgPassBuilder pass = rg_barrier_test_add_pass(
        graph, VKR_RG_PASS_TYPE_GRAPHICS, "TargetWrite");
    VkrRgAttachmentDesc attachment = {
        .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .slice = VKR_RG_IMAGE_SLICE_DEFAULT,
    };
    vkr_rg_pass_add_color_attachment(&pass, target, &attachment);
    vkr_rg_set_present_image(graph, target);

    assert(vkr_rg_compile_schedule(graph));
    const VkrRgImageBarrier *initial = vector_get_VkrRgImageBarrier(
        &graph->passes.data[0].pre_image_barriers, 0);
    assert(initial->src_layout ==
           (windowed ? VKR_TEXTURE_LAYOUT_UNDEFINED
                     : VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL));
    assert(graph->terminal_image_barriers.length == 1);
    const VkrRgImageBarrier *terminal =
        vector_get_VkrRgImageBarrier(&graph->terminal_image_barriers, 0);
    assert(terminal->dst_layout ==
           (windowed ? VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR
                     : VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
    assert(terminal->dst_layout !=
           (windowed ? VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                     : VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR));
    vkr_rg_destroy(graph);
  }

  arena_destroy(arena);
  printf("  test_present_target_import_and_terminal_states PASSED\n");
}

static void test_write_after_write_emits_barrier(void) {
  printf("  Running test_write_after_write_emits_barrier...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph != NULL);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 64;
  desc.height = 64;
  desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_STORAGE);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("color"), &desc);

  // Identical access and identical layout on both passes: the old rule
  // (access != desired || layout != desired) emitted nothing here, leaving two
  // overlapping writes unordered.
  VkrRgPassBuilder first =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "First");
  vkr_rg_pass_write_image(&first, image, VKR_RG_IMAGE_ACCESS_STORAGE_WRITE, 0,
                          0);
  VkrRgPassBuilder second =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "Second");
  vkr_rg_pass_write_image(&second, image, VKR_RG_IMAGE_ACCESS_STORAGE_WRITE, 0,
                          0);

  assert(vkr_rg_compile_schedule(graph));

  const VkrRgPass *second_pass = rg_barrier_test_pass(graph, 1);
  assert(second_pass->pre_image_barriers.length == 1);
  const VkrRgImageBarrier *barrier = vector_get_VkrRgImageBarrier(
      (Vector_VkrRgImageBarrier *)&second_pass->pre_image_barriers, 0);
  assert(barrier->src_access == VKR_RG_IMAGE_ACCESS_STORAGE_WRITE);
  assert(barrier->dst_access == VKR_RG_IMAGE_ACCESS_STORAGE_WRITE);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_write_after_write_emits_barrier PASSED\n");
}

static void test_read_after_read_emits_nothing(void) {
  printf("  Running test_read_after_read_emits_nothing...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph != NULL);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 64;
  desc.height = 64;
  desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_SAMPLED);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("sampled"), &desc);

  VkrRgPassBuilder first =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "ReadA");
  vkr_rg_pass_read_image(&first, image, VKR_RG_IMAGE_ACCESS_SAMPLED, 0, 0);
  VkrRgPassBuilder second =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "ReadB");
  vkr_rg_pass_read_image(&second, image, VKR_RG_IMAGE_ACCESS_SAMPLED, 0, 0);

  assert(vkr_rg_compile_schedule(graph));

  // The first read still transitions from UNDEFINED; the second must not add
  // anything, or every sampled resource would pay a barrier per consumer.
  assert(rg_barrier_test_pass(graph, 0)->pre_image_barriers.length == 1);
  assert(rg_barrier_test_pass(graph, 1)->pre_image_barriers.length == 0);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_read_after_read_emits_nothing PASSED\n");
}

static void test_same_pass_storage_read_write_combines(void) {
  printf("  Running test_same_pass_storage_read_write_combines...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph != NULL);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 64;
  desc.height = 64;
  desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_STORAGE);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("read_write"), &desc);

  VkrRgPassBuilder pass =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "ReadWrite");
  vkr_rg_pass_read_image(&pass, image, VKR_RG_IMAGE_ACCESS_STORAGE_READ, 0, 0);
  vkr_rg_pass_write_image(&pass, image, VKR_RG_IMAGE_ACCESS_STORAGE_WRITE, 0,
                          0);

  assert(vkr_rg_compile_schedule(graph));

  const VkrRgPass *compiled = rg_barrier_test_pass(graph, 0);
  assert(compiled->pre_image_barriers.length == 1);
  const VkrRgImageBarrier *barrier = vector_get_VkrRgImageBarrier(
      (Vector_VkrRgImageBarrier *)&compiled->pre_image_barriers, 0);
  assert(barrier->dst_access == (VKR_RG_IMAGE_ACCESS_STORAGE_READ |
                                 VKR_RG_IMAGE_ACCESS_STORAGE_WRITE));
  assert(barrier->dst_layout == VKR_TEXTURE_LAYOUT_GENERAL);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_same_pass_storage_read_write_combines PASSED\n");
}

static void test_same_pass_incompatible_layouts_are_rejected(void) {
  printf("  Running test_same_pass_incompatible_layouts_are_rejected...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph != NULL);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 64;
  desc.height = 64;
  desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_SAMPLED |
                                                 VKR_TEXTURE_USAGE_STORAGE);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("incompatible"), &desc);

  VkrRgPassBuilder pass =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "Incompatible");
  vkr_rg_pass_read_image(&pass, image, VKR_RG_IMAGE_ACCESS_SAMPLED, 0, 0);
  vkr_rg_pass_write_image(&pass, image, VKR_RG_IMAGE_ACCESS_STORAGE_WRITE, 0,
                          0);

  assert(!vkr_rg_compile_schedule(graph));

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_same_pass_incompatible_layouts_are_rejected PASSED\n");
}

static void test_cascade_slices_are_per_layer_then_coalesce(void) {
  printf("  Running test_cascade_slices_are_per_layer_then_coalesce...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph != NULL);

  // The CSM shape: one 4-layer depth image written by four per-layer passes,
  // then sampled whole by a fifth.
  const uint32_t cascade_count = 4;
  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 128;
  desc.height = 128;
  desc.layers = cascade_count;
  desc.format = VKR_TEXTURE_FORMAT_R32_SFLOAT;
  desc.usage = vkr_texture_usage_flags_from_bits(
      VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT | VKR_TEXTURE_USAGE_SAMPLED);
  VkrRgImageHandle shadow_map =
      vkr_rg_create_image(graph, string8_lit("shadow_map"), &desc);

  static const char *cascade_names[4] = {"Cascade0", "Cascade1", "Cascade2",
                                         "Cascade3"};
  for (uint32_t i = 0; i < cascade_count; ++i) {
    VkrRgPassBuilder pb = rg_barrier_test_add_pass(
        graph, VKR_RG_PASS_TYPE_GRAPHICS, cascade_names[i]);
    VkrRgAttachmentDesc att = {.slice = VKR_RG_IMAGE_SLICE_DEFAULT};
    att.slice.base_layer = i;
    att.slice.layer_count = 1;
    att.load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR;
    att.store_op = VKR_ATTACHMENT_STORE_OP_STORE;
    vkr_rg_pass_set_depth_attachment(&pb, shadow_map, &att, false_v);
  }

  VkrRgPassBuilder consumer =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "World");
  vkr_rg_pass_read_image(&consumer, shadow_map, VKR_RG_IMAGE_ACCESS_SAMPLED, 0,
                         0);

  assert(vkr_rg_compile_schedule(graph));

  // Each cascade barriers only its own layer. Before subresource tracking,
  // cascades 1..3 emitted nothing at all and cascade 0 transitioned all four.
  for (uint32_t i = 0; i < cascade_count; ++i) {
    const VkrRgPass *pass = rg_barrier_test_pass(graph, i);
    assert(pass->pre_image_barriers.length == 1);
    const VkrRgImageBarrier *barrier = vector_get_VkrRgImageBarrier(
        (Vector_VkrRgImageBarrier *)&pass->pre_image_barriers, 0);
    assert(barrier->range.base_layer == i);
    assert(barrier->range.layer_count == 1);
    assert(barrier->dst_access == VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT);
  }

  // All four layers now share a state, so the whole-image read coalesces back
  // into a single barrier rather than one per layer.
  const VkrRgPass *world = rg_barrier_test_pass(graph, cascade_count);
  assert(world->pre_image_barriers.length == 1);
  const VkrRgImageBarrier *read_barrier = vector_get_VkrRgImageBarrier(
      (Vector_VkrRgImageBarrier *)&world->pre_image_barriers, 0);
  assert(read_barrier->range.base_layer == 0);
  assert(read_barrier->range.layer_count == cascade_count);
  assert(read_barrier->src_access == VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT);
  assert(read_barrier->dst_access == VKR_RG_IMAGE_ACCESS_SAMPLED);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_cascade_slices_are_per_layer_then_coalesce PASSED\n");
}

static void test_disjoint_layer_writes_coalesce_on_read(void) {
  printf("  Running test_disjoint_layer_writes_coalesce_on_read...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph != NULL);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 64;
  desc.height = 64;
  desc.layers = 4;
  desc.format = VKR_TEXTURE_FORMAT_R32_SFLOAT;
  desc.usage = vkr_texture_usage_flags_from_bits(
      VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT | VKR_TEXTURE_USAGE_SAMPLED);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("layered"), &desc);

  // Two passes covering layers 0-1 and 2-3 leave all four in the same state, so
  // the reader must still get one barrier, not two.
  for (uint32_t i = 0; i < 2; ++i) {
    VkrRgPassBuilder pb = rg_barrier_test_add_pass(
        graph, VKR_RG_PASS_TYPE_GRAPHICS, i == 0 ? "LowHalf" : "HighHalf");
    VkrRgAttachmentDesc att = {.slice = VKR_RG_IMAGE_SLICE_DEFAULT};
    att.slice.base_layer = i * 2;
    att.slice.layer_count = 2;
    att.load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR;
    att.store_op = VKR_ATTACHMENT_STORE_OP_STORE;
    vkr_rg_pass_set_depth_attachment(&pb, image, &att, false_v);
  }

  VkrRgPassBuilder consumer =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "Consumer");
  vkr_rg_pass_read_image(&consumer, image, VKR_RG_IMAGE_ACCESS_SAMPLED, 0, 0);

  assert(vkr_rg_compile_schedule(graph));

  const VkrRgPass *reader = rg_barrier_test_pass(graph, 2);
  assert(reader->pre_image_barriers.length == 1);
  const VkrRgImageBarrier *barrier = vector_get_VkrRgImageBarrier(
      (Vector_VkrRgImageBarrier *)&reader->pre_image_barriers, 0);
  assert(barrier->range.base_layer == 0);
  assert(barrier->range.layer_count == 4);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_disjoint_layer_writes_coalesce_on_read PASSED\n");
}

static void test_capture_read_uses_exact_array_slice(void) {
  printf("  Running test_capture_read_uses_exact_array_slice...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph != NULL);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = desc.height = 64u;
  desc.layers = 4u;
  desc.usage = vkr_texture_usage_flags_from_bits(
      VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT |
      VKR_TEXTURE_USAGE_TRANSFER_SRC);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("capture_layers"), &desc);

  VkrRgPassBuilder writer =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_GRAPHICS, "Writer");
  VkrRgAttachmentDesc attachment = {.slice = VKR_RG_IMAGE_SLICE_DEFAULT};
  attachment.slice.base_layer = 2u;
  attachment.slice.layer_count = 1u;
  vkr_rg_pass_set_depth_attachment(&writer, image, &attachment, false_v);

  VkrRgPassBuilder capture =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_TRANSFER, "Capture");
  vkr_rg_pass_read_image_slice(
      &capture, image, VKR_RG_IMAGE_ACCESS_TRANSFER_SRC, 0u, 0u,
      (VkrRgImageSlice){.mip_level = 0u, .base_layer = 2u, .layer_count = 1u});

  assert(vkr_rg_compile_schedule(graph));
  const VkrRgPass *compiled = rg_barrier_test_pass(graph, 1u);
  assert(compiled->pre_image_barriers.length == 1u);
  const VkrRgImageBarrier *barrier = vector_get_VkrRgImageBarrier(
      (Vector_VkrRgImageBarrier *)&compiled->pre_image_barriers, 0u);
  assert(barrier->range.base_layer == 2u);
  assert(barrier->range.layer_count == 1u);
  assert(barrier->range.base_mip == 0u);
  assert(barrier->range.mip_count == 1u);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_capture_read_uses_exact_array_slice PASSED\n");
}

static void test_subresource_range_resolve(void) {
  printf("  Running test_subresource_range_resolve...\n");

  uint32_t base_mip = 0;
  uint32_t mip_count = 0;
  uint32_t base_layer = 0;
  uint32_t layer_count = 0;

  // A zeroed range means the whole image; this is what a use that never
  // declared a slice looks like, so getting it wrong would silently narrow
  // every default barrier.
  VkrImageSubresourceRange whole = {0};
  vkr_image_subresource_range_resolve(&whole, 3, 6, &base_mip, &mip_count,
                                      &base_layer, &layer_count);
  assert(base_mip == 0 && mip_count == 3);
  assert(base_layer == 0 && layer_count == 6);

  // NULL behaves the same as a zeroed range.
  vkr_image_subresource_range_resolve(NULL, 2, 4, &base_mip, &mip_count,
                                      &base_layer, &layer_count);
  assert(base_mip == 0 && mip_count == 2);
  assert(base_layer == 0 && layer_count == 4);

  // A count of 0 from a non-zero base means "the rest".
  VkrImageSubresourceRange tail = {
      .base_mip = 1, .mip_count = 0, .base_layer = 2, .layer_count = 0};
  vkr_image_subresource_range_resolve(&tail, 3, 6, &base_mip, &mip_count,
                                      &base_layer, &layer_count);
  assert(base_mip == 1 && mip_count == 2);
  assert(base_layer == 2 && layer_count == 4);

  // Counts are clamped to what the image actually has.
  VkrImageSubresourceRange overlong = {
      .base_mip = 0, .mip_count = 99, .base_layer = 1, .layer_count = 99};
  vkr_image_subresource_range_resolve(&overlong, 2, 3, &base_mip, &mip_count,
                                      &base_layer, &layer_count);
  assert(base_mip == 0 && mip_count == 2);
  assert(base_layer == 1 && layer_count == 2);

  printf("  test_subresource_range_resolve PASSED\n");
}

static void test_image_access_is_write(void) {
  printf("  Running test_image_access_is_write...\n");

  assert(!vkr_image_access_is_write(VKR_IMAGE_ACCESS_NONE));
  assert(!vkr_image_access_is_write(VKR_IMAGE_ACCESS_SAMPLED));
  assert(!vkr_image_access_is_write(VKR_IMAGE_ACCESS_STORAGE_READ));
  assert(!vkr_image_access_is_write(VKR_IMAGE_ACCESS_DEPTH_READ_ONLY));
  assert(!vkr_image_access_is_write(VKR_IMAGE_ACCESS_TRANSFER_SRC));
  assert(!vkr_image_access_is_write(VKR_IMAGE_ACCESS_PRESENT));

  assert(vkr_image_access_is_write(VKR_IMAGE_ACCESS_STORAGE_WRITE));
  assert(vkr_image_access_is_write(VKR_IMAGE_ACCESS_COLOR_ATTACHMENT));
  assert(vkr_image_access_is_write(VKR_IMAGE_ACCESS_DEPTH_ATTACHMENT));
  assert(vkr_image_access_is_write(VKR_IMAGE_ACCESS_TRANSFER_DST));
  assert(vkr_image_access_is_write(VKR_IMAGE_ACCESS_SAMPLED |
                                   VKR_IMAGE_ACCESS_STORAGE_WRITE));

  printf("  test_image_access_is_write PASSED\n");
}

static void test_main_graph_declares_transmission_stages(void) {
  printf("  Running test_main_graph_declares_transmission_stages...\n");
  Arena *arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  VkrRgJsonGraph graph = {0};
  assert(vkr_rg_json_load_file(&allocator,
                               "assets/render_graphs/main.rendergraph.json",
                               &graph) == true_v);

  const char *deferred_ordered[] = {
      "VBuffer.Opaque", "GBuffer.Resolve.Fullscreen",
      "Lighting.Deferred.Fullscreen", "World.FeedbackCopy.Fullscreen"};
  uint32_t found = 0u;
  for (uint64_t i = 0u;
       i < graph.passes.length && found < ArrayCount(deferred_ordered); ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&graph.passes, i);
    if (!pass || !vkr_string8_equals_cstr(&pass->name, deferred_ordered[found]))
      continue;
    if (found == 1u) {
      assert(pass->writes.length == 7u);
      VkrRgJsonResourceUse *hdr_seed =
          vector_get_VkrRgJsonResourceUse(&pass->writes, 6u);
      assert(hdr_seed && hdr_seed->binding.is_set &&
             hdr_seed->binding.value == 8u &&
             hdr_seed->image_access == VKR_RG_JSON_IMAGE_ACCESS_STORAGE_WRITE);
    } else if (found == 2u) {
      assert(pass->type == VKR_RG_JSON_PASS_COMPUTE);
      assert(pass->reads.length == 7u && pass->writes.length == 1u);
      VkrRgJsonResourceUse *hdr_read =
          vector_get_VkrRgJsonResourceUse(&pass->reads, 6u);
      VkrRgJsonResourceUse *hdr_write =
          vector_get_VkrRgJsonResourceUse(&pass->writes, 0u);
      assert(
          hdr_read && hdr_write && hdr_read->binding.is_set &&
          hdr_write->binding.is_set && hdr_read->binding.value == 6u &&
          hdr_write->binding.value == 6u &&
          vkr_string8_equals_cstr(&hdr_read->name, "hdr_pre_transmission") &&
          vkr_string8_equals_cstr(&hdr_write->name, "hdr_pre_transmission") &&
          hdr_read->image_access == VKR_RG_JSON_IMAGE_ACCESS_STORAGE_READ &&
          hdr_write->image_access == VKR_RG_JSON_IMAGE_ACCESS_STORAGE_WRITE);
    }
    found++;
  }
  assert(found == ArrayCount(deferred_ordered));

  bool8_t found_blend = false_v;
  for (uint64_t i = 0u; i < graph.passes.length; ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&graph.passes, i);
    if (!pass ||
        !vkr_string8_equals_cstr(&pass->name, "World.Blend.Fullscreen"))
      continue;
    assert(pass->attachments.has_depth);
    assert(vkr_string8_equals_cstr(&pass->attachments.depth.image,
                                   "opaque_vbuffer_depth"));
    assert(pass->condition.kind == VKR_RG_JSON_CONDITION_EDITOR_DISABLED);
    found_blend = true_v;
    break;
  }
  assert(found_blend);

  const char *transmission_deferred_ordered[] = {
      "Transmission.Cull.Upload",        "Transmission.Cull.Classify",
      "Transmission.Cull.Prefix",        "Transmission.Cull.Encode",
      "Transmission.DepthSeed.0",        "Transmission.DepthSeed.3",
      "VBuffer.Transmission.0",          "VBuffer.Transmission.3",
      "Transmission.Shade.Fullscreen.3", "Transmission.Shade.Fullscreen.0"};
  found = 0u;
  for (uint64_t i = 0u; i < graph.passes.length &&
                        found < ArrayCount(transmission_deferred_ordered);
       ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&graph.passes, i);
    if (pass && vkr_string8_equals_cstr(&pass->name,
                                        transmission_deferred_ordered[found])) {
      const VkrRgJsonConditionKind expected_condition =
          found < 8u
              ? VKR_RG_JSON_CONDITION_TRANSMISSION_PENDING
              : VKR_RG_JSON_CONDITION_EDITOR_DISABLED_TRANSMISSION_FULLSCREEN;
      assert(pass->condition.kind == expected_condition);
      found++;
    }
  }
  assert(found == ArrayCount(transmission_deferred_ordered));

  uint32_t transmission_coverage_passes = 0u;
  for (uint64_t i = 0u; i < graph.passes.length; ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&graph.passes, i);
    if (!pass ||
        !vkr_string8_starts_with(&pass->name, "Transmission.Coverage."))
      continue;
    assert(pass->condition.kind ==
           VKR_RG_JSON_CONDITION_TRANSMISSION_FULLSCREEN_TIMING);
    assert(pass->type == VKR_RG_JSON_PASS_COMPUTE && pass->reads.length == 2u &&
           pass->writes.length == 0u);
    transmission_coverage_passes++;
  }
  assert(transmission_coverage_passes == 4u);

  uint32_t transmission_compact_passes = 0u;
  uint32_t transmission_compact_finalize_passes = 0u;
  uint32_t transmission_compact_shade_passes = 0u;
  for (uint64_t i = 0u; i < graph.passes.length; ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&graph.passes, i);
    if (!pass)
      continue;
    if (vkr_string8_starts_with(&pass->name,
                                "Transmission.Compact.Finalize.")) {
      assert(pass->condition.kind ==
             VKR_RG_JSON_CONDITION_TRANSMISSION_COMPACT_ENABLED);
      assert(pass->reads.length == 3u && pass->writes.length == 1u);
      transmission_compact_finalize_passes++;
    } else if (vkr_string8_starts_with(&pass->name, "Transmission.Compact.")) {
      assert(pass->condition.kind ==
                 VKR_RG_JSON_CONDITION_EDITOR_DISABLED_TRANSMISSION_COMPACT ||
             pass->condition.kind ==
                 VKR_RG_JSON_CONDITION_EDITOR_ENABLED_TRANSMISSION_COMPACT);
      assert(pass->reads.length == 2u && pass->writes.length == 4u);
      assert(!pass->writes.data[2].is_image &&
             pass->writes.data[2].binding.value == 3u &&
             pass->writes.data[2].buffer_access ==
                 VKR_RG_JSON_BUFFER_ACCESS_STORAGE_WRITE);
      transmission_compact_passes++;
    } else if (vkr_string8_starts_with(&pass->name,
                                       "Transmission.Shade.Compact.")) {
      assert(pass->dispatch.kind == VKR_RG_DISPATCH_INDIRECT);
      assert(pass->dispatch.indirect_binding == 7u);
      transmission_compact_shade_passes++;
    }
  }
  assert(transmission_compact_passes == 8u);
  assert(transmission_compact_finalize_passes == 0u);
  assert(transmission_compact_shade_passes == 8u);

  uint32_t layered_resource_count = 0u;
  bool8_t found_transmission_feedback = false_v;
  bool8_t found_transmission_compact_pixels = false_v;
  for (uint64_t i = 0u; i < graph.resources.length; ++i) {
    VkrRgJsonResource *resource =
        vector_get_VkrRgJsonResource(&graph.resources, i);
    if (!resource)
      continue;
    if (vkr_string8_equals_cstr(&resource->name, "transmission_vbuffer") ||
        vkr_string8_equals_cstr(&resource->name,
                                "transmission_vbuffer_depth")) {
      assert(resource->condition.kind ==
             VKR_RG_JSON_CONDITION_TRANSMISSION_PENDING);
      assert(resource->image.layers_is_set && resource->image.layers == 4u);
      layered_resource_count++;
    } else if (vkr_string8_equals_cstr(&resource->name,
                                       "transmission_feedback")) {
      assert(resource->condition.kind ==
             VKR_RG_JSON_CONDITION_TRANSMISSION_PENDING);
      found_transmission_feedback = true_v;
    } else if (vkr_string8_equals_cstr(&resource->name,
                                       "transmission_compact_pixels")) {
      assert(resource->condition.kind ==
             VKR_RG_JSON_CONDITION_TRANSMISSION_COMPACT_ENABLED);
      assert(resource->buffer.size_mode ==
             VKR_RG_JSON_BUFFER_SIZE_VIEWPORT_PIXELS);
      assert(resource->buffer.bytes_per_pixel == sizeof(uint32_t));
      found_transmission_compact_pixels = true_v;
    }
  }
  assert(layered_resource_count == 2u && found_transmission_feedback &&
         found_transmission_compact_pixels);

  const char *multi_view_ordered[] = {"Cull.Classify", "Cull.Prefix",
                                      "Cull.Encode", "Shadow.Cascade.${i}"};
  found = 0u;
  for (uint64_t i = 0u;
       i < graph.passes.length && found < ArrayCount(multi_view_ordered); ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&graph.passes, i);
    if (!pass ||
        !vkr_string8_equals_cstr(&pass->name, multi_view_ordered[found]))
      continue;
    if (found == 3u) {
      assert(pass->condition.kind == VKR_RG_JSON_CONDITION_NONE);
      assert(pass->repeat.enabled &&
             vkr_string8_equals_cstr(&pass->repeat.count_source,
                                     "shadow_cascade_count"));
      assert(vkr_string8_equals_cstr(&pass->repeat.condition_mask_source,
                                     "shadow_cascade_render_mask"));
      assert(pass->reads.length == 4u);
      VkrRgJsonResourceUse *visible =
          vector_get_VkrRgJsonResourceUse(&pass->reads, 0u);
      VkrRgJsonResourceUse *instances =
          vector_get_VkrRgJsonResourceUse(&pass->reads, 1u);
      VkrRgJsonResourceUse *state =
          vector_get_VkrRgJsonResourceUse(&pass->reads, 2u);
      VkrRgJsonResourceUse *arguments =
          vector_get_VkrRgJsonResourceUse(&pass->reads, 3u);
      assert(
          visible && instances && state && arguments &&
          visible->binding.value == 1u && instances->binding.value == 9u &&
          state->binding.value == 2u && arguments->binding.value == 3u &&
          visible->buffer_access == VKR_RG_JSON_BUFFER_ACCESS_STORAGE_READ &&
          instances->buffer_access == VKR_RG_JSON_BUFFER_ACCESS_STORAGE_READ &&
          state->buffer_access == VKR_RG_JSON_BUFFER_ACCESS_INDIRECT_READ &&
          arguments->buffer_access == VKR_RG_JSON_BUFFER_ACCESS_INDIRECT_READ);
    }
    found++;
  }
  assert(found == ArrayCount(multi_view_ordered));

  bool8_t found_hzb_resource = false_v;
  bool8_t found_stable_shadow_capacity = false_v;
  bool8_t found_sdsm_resource = false_v;
  bool8_t found_gpu_instances = false_v;
  bool8_t found_transmission_gpu_instances = false_v;
  for (uint64_t i = 0u; i < graph.resources.length; ++i) {
    VkrRgJsonResource *resource =
        vector_get_VkrRgJsonResource(&graph.resources, i);
    if (!resource)
      continue;
    if (vkr_string8_equals_cstr(&resource->name, "hzb_history")) {
      assert(resource->image.mip_levels_full);
      assert((resource->flags & VKR_RG_JSON_RESOURCE_FLAG_HISTORY) != 0u);
      found_hzb_resource = true_v;
    } else if (vkr_string8_equals_cstr(&resource->name, "sdsm_reduce_state")) {
      assert(resource->buffer.size == 16u);
      assert((resource->flags & VKR_RG_JSON_RESOURCE_FLAG_PER_FRAME_SLOT) !=
             0u);
      found_sdsm_resource = true_v;
    } else if (vkr_string8_equals_cstr(&resource->name, "gpu_draw_instances")) {
      assert(resource->buffer.size ==
             (uint64_t)VKR_GPU_DRAW_CANDIDATE_CAPACITY *
                 sizeof(VkrInstanceDataGPU));
      assert((resource->flags & VKR_RG_JSON_RESOURCE_FLAG_PER_FRAME_SLOT) !=
             0u);
      found_gpu_instances = true_v;
    } else if (vkr_string8_equals_cstr(&resource->name,
                                       "transmission_gpu_draw_instances")) {
      assert(resource->condition.kind ==
             VKR_RG_JSON_CONDITION_TRANSMISSION_PENDING);
      assert(resource->buffer.size ==
             (uint64_t)VKR_GPU_DRAW_CANDIDATE_CAPACITY *
                 sizeof(VkrInstanceDataGPU));
      assert((resource->flags & VKR_RG_JSON_RESOURCE_FLAG_PER_FRAME_SLOT) !=
             0u);
      found_transmission_gpu_instances = true_v;
    } else if (vkr_string8_equals_cstr(&resource->name, "shadow_map")) {
      assert(vkr_string8_equals_cstr(&resource->image.layers_source,
                                     "shadow_map_layer_count"));
      /* RETAINED since ADR-029: cascade contents must survive across frames
         for reuse to be possible at all. TRANSIENT would discard them, and
         PERSISTENT only suppresses a diagnostic without preserving anything. */
      assert((resource->flags & VKR_RG_JSON_RESOURCE_FLAG_RETAINED) != 0u);
      assert((resource->flags & VKR_RG_JSON_RESOURCE_FLAG_TRANSIENT) == 0u);
      assert((resource->flags & VKR_RG_JSON_RESOURCE_FLAG_PER_IMAGE) != 0u);
      assert((resource->flags & VKR_RG_JSON_RESOURCE_FLAG_PERSISTENT) == 0u);
      found_stable_shadow_capacity = true_v;
    }
  }
  assert(found_hzb_resource && found_sdsm_resource && found_gpu_instances &&
         found_transmission_gpu_instances && found_stable_shadow_capacity);

  bool8_t found_hzb_reduce = false_v;
  bool8_t found_sdsm_reduce = false_v;
  for (uint64_t i = 0u; i < graph.passes.length; ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&graph.passes, i);
    if (!pass)
      continue;
    if (vkr_string8_equals_cstr(&pass->name, "SDSM.Reduce")) {
      assert(pass->condition.kind == VKR_RG_JSON_CONDITION_SDSM_ENABLED);
      assert(pass->reads.length == 2u && pass->writes.length == 1u);
      VkrRgJsonResourceUse *depth =
          vector_get_VkrRgJsonResourceUse(&pass->reads, 0u);
      VkrRgJsonResourceUse *vbuffer =
          vector_get_VkrRgJsonResourceUse(&pass->reads, 1u);
      VkrRgJsonResourceUse *state =
          vector_get_VkrRgJsonResourceUse(&pass->writes, 0u);
      assert(depth && vbuffer && state && depth->binding.value == 0u &&
             vbuffer->binding.value == 1u && state->binding.value == 2u &&
             depth->image_access == VKR_RG_JSON_IMAGE_ACCESS_SAMPLED &&
             vbuffer->image_access == VKR_RG_JSON_IMAGE_ACCESS_SAMPLED &&
             state->buffer_access == VKR_RG_JSON_BUFFER_ACCESS_STORAGE_WRITE);
      found_sdsm_reduce = true_v;
      continue;
    }
    if (!vkr_string8_equals_cstr(&pass->name, "HZB.BuildMip.${i}"))
      continue;
    assert(pass->repeat.enabled &&
           vkr_string8_equals_cstr(&pass->repeat.count_source,
                                   "hzb_reduce_pass_count"));
    assert(pass->reads.length == 1u && pass->writes.length == 1u);
    VkrRgJsonResourceUse *read =
        vector_get_VkrRgJsonResourceUse(&pass->reads, 0u);
    VkrRgJsonResourceUse *write =
        vector_get_VkrRgJsonResourceUse(&pass->writes, 0u);
    assert(read && write && read->has_slice && write->has_slice);
    assert(vkr_string8_equals_cstr(&read->slice_base_mip.token, "${i}"));
    assert(vkr_string8_equals_cstr(&write->slice_base_mip.token, "${i+1}"));
    found_hzb_reduce = true_v;
  }
  assert(found_hzb_reduce && found_sdsm_reduce);

  const char *picking_deferred_ordered[] = {
      "Picking.DepthSeed.Opaque", "Picking.Resolve.Opaque", "Picking.Features",
      "Picking.Readback"};
  found = 0u;
  for (uint64_t i = 0u;
       i < graph.passes.length && found < ArrayCount(picking_deferred_ordered);
       ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&graph.passes, i);
    if (!pass ||
        !vkr_string8_equals_cstr(&pass->name, picking_deferred_ordered[found]))
      continue;
    if (found == 0u) {
      assert(pass->type == VKR_RG_JSON_PASS_TRANSFER);
      assert(pass->reads.length == 1u && pass->writes.length == 1u);
    } else if (found == 1u) {
      assert(pass->type == VKR_RG_JSON_PASS_COMPUTE);
      assert(pass->reads.length == 5u && pass->writes.length == 1u);
    } else if (found == 2u) {
      assert(pass->type == VKR_RG_JSON_PASS_GRAPHICS);
      assert(pass->attachments.colors.length == 1u &&
             pass->attachments.has_depth);
      VkrRgJsonAttachment *color =
          vector_get_VkrRgJsonAttachment(&pass->attachments.colors, 0u);
      assert(color && color->load_op == VKR_ATTACHMENT_LOAD_OP_LOAD);
      assert(pass->attachments.depth.load_op == VKR_ATTACHMENT_LOAD_OP_LOAD);
    }
    found++;
  }
  assert(found == ArrayCount(picking_deferred_ordered));

  vkr_rg_json_destroy(&graph);
  arena_destroy(arena);
  printf("  test_main_graph_declares_transmission_stages PASSED\n");
}

static void test_main_graph_fits_runtime_pass_capacity(void) {
  printf("  Running test_main_graph_fits_runtime_pass_capacity...\n");
  Arena *arena = arena_create(MB(16), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrRgJsonGraph graph = {0};
  assert(vkr_rg_json_load_file(
      &allocator, "assets/render_graphs/main.rendergraph.json", &graph));
  VkrRgExecutorRegistry registry = {0};
  assert(vkr_rg_executor_registry_init(&registry, &allocator));
  uint32_t executor_id = 1u;
  for (uint64_t i = 0u; i < graph.passes.length; ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&graph.passes, i);
    if (!pass || vkr_rg_executor_registry_find(&registry, pass->execute))
      continue;
    const VkrRgPassExecutor executor = {
        .name = pass->execute,
        .id = executor_id++,
        .type = (VkrRgPassType)pass->type,
        .execute = rg_barrier_test_execute,
    };
    assert(vkr_rg_executor_registry_register(&registry, &executor));
  }
  assert(vkr_rg_json_bind_executors(&graph, &registry));

  VkrRenderGraph *runtime = vkr_rg_create(&allocator);
  assert(runtime);
  VkrRenderGraphFrameInfo frame = {
      .target_width = 3840u,
      .target_height = 2160u,
      .window_width = 3840u,
      .window_height = 2160u,
      .viewport_width = 3840u,
      .viewport_height = 2160u,
      .target_color_format = VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB,
      .target_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
      .shadow_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
      .shadow_map_size = 2048u,
      .shadow_map_layer_count = 4u,
      .shadow_cascade_count = 4u,
      .shadow_cascade_render_mask = 0xfu,
      .hzb_reduce_pass_count = 11u,
      .sdsm_enabled = true_v,
      .transmission_pending = true_v,
      .timing_enabled = true_v,
      .picking_pending = true_v,
  };
  vkr_rg_begin_frame(runtime, &frame);
  assert(vkr_rg_build_from_json(runtime, &graph, &frame));
  assert(runtime->passes.length <= 64u);
  assert(vkr_rg_compile_schedule(runtime));
  vkr_rg_end_frame(runtime);

  frame.transmission_compact_enabled = true_v;
  vkr_rg_begin_frame(runtime, &frame);
  assert(vkr_rg_build_from_json(runtime, &graph, &frame));
  assert(runtime->passes.length <= 64u);
  assert(vkr_rg_compile_schedule(runtime));
  vkr_rg_end_frame(runtime);

  frame = (VkrRenderGraphFrameInfo){
      .target_width = 3840u,
      .target_height = 2160u,
      .window_width = 3840u,
      .window_height = 2160u,
      .viewport_width = 3840u,
      .viewport_height = 2160u,
      .target_color_format = VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB,
      .target_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
      .shadow_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
      .shadow_map_size = 2048u,
      .shadow_map_layer_count = 4u,
  };
  vkr_rg_begin_frame(runtime, &frame);
  assert(vkr_rg_build_from_json(runtime, &graph, &frame));
  bool8_t found_vbuffer = false_v;
  for (uint64_t i = 0u; i < runtime->passes.length; ++i) {
    VkrRgPass *pass = vector_get_VkrRgPass(&runtime->passes, i);
    if (!pass || !vkr_string8_equals_cstr(&pass->desc.name, "VBuffer.Opaque"))
      continue;
    found_vbuffer = true_v;
    break;
  }
  assert(found_vbuffer);
  assert(vkr_rg_compile_schedule(runtime));
  vkr_rg_end_frame(runtime);

  vkr_rg_destroy(runtime);
  vkr_rg_executor_registry_destroy(&registry);
  vkr_rg_json_destroy(&graph);
  arena_destroy(arena);
  printf("  test_main_graph_fits_runtime_pass_capacity PASSED\n");
}

static void test_frame_allocator_reclaims_authored_passes(void) {
  printf("  Running test_frame_allocator_reclaims_authored_passes...\n");
  Arena *persistent_arena = arena_create(MB(1), MB(1));
  Arena *frame_arena = arena_create(KB(64), KB(64));
  assert(persistent_arena && frame_arena);
  VkrAllocator persistent_allocator = {.ctx = persistent_arena};
  VkrAllocator frame_allocator = {.ctx = frame_arena};
  assert(vkr_allocator_arena(&persistent_allocator));
  assert(vkr_allocator_arena(&frame_allocator));

  VkrRenderGraph *graph = vkr_rg_create(&persistent_allocator);
  assert(graph && vkr_rg_set_frame_allocator(graph, &frame_allocator));
  VkrRenderGraphFrameInfo frame = {.target_width = 64, .target_height = 64};
  uint64_t first_frame_end = 0;
  uint64_t first_persistent_end = 0;

  // Without a frame scope, these identical authored graphs exhaust 64 KiB
  // after only a few iterations. The scoped allocator must return to the same
  // high-water position on every rebuild.
  for (uint32_t iteration = 0; iteration < 500; ++iteration) {
    vkr_rg_begin_frame(graph, &frame);
    VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
    desc.width = 64;
    desc.height = 64;
    desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_STORAGE);
    VkrRgImageHandle image =
        vkr_rg_create_image(graph, string8_lit("Scoped.Storage"), &desc);
    assert(vkr_rg_image_handle_valid(image));

    for (uint32_t pass_index = 0; pass_index < 16; ++pass_index) {
      VkrRgPassBuilder pass = rg_barrier_test_add_pass(
          graph, VKR_RG_PASS_TYPE_COMPUTE, "Scoped.Write");
      vkr_rg_pass_write_image(&pass, image, VKR_RG_IMAGE_ACCESS_STORAGE_WRITE,
                              0, 0);
    }
    assert(vkr_rg_compile_schedule(graph));
    assert(graph->passes.length == 16);
    uint64_t frame_end = arena_pos(frame_arena);
    if (iteration == 0) {
      first_frame_end = frame_end;
      first_persistent_end = arena_pos(persistent_arena);
    } else {
      assert(frame_end == first_frame_end);
      assert(arena_pos(persistent_arena) == first_persistent_end);
    }
    vkr_rg_end_frame(graph);
  }

  vkr_rg_destroy(graph);
  arena_destroy(frame_arena);
  arena_destroy(persistent_arena);
  printf("  test_frame_allocator_reclaims_authored_passes PASSED\n");
}

/* ===========================================================================
   Retained graph resources (ADR-029).

   These pin the contract that makes cascade reuse possible: contents survive
   across frames per instance and per subresource, a reader with no writer is a
   compile error unless the backend vouches for the contents, and nothing is
   committed until a submit is proven.
   ===========================================================================
 */

typedef struct RgRetainedTestStore {
  VkrRgRetainedState states[8][4][64];
  uint32_t read_calls;
  uint32_t commit_calls;
} RgRetainedTestStore;

static RgRetainedTestStore g_retained_store;

static void rg_retained_test_read(void *context, uint32_t image_index,
                                  uint32_t instance_index, uint32_t subresource,
                                  VkrRgRetainedState *out_state) {
  RgRetainedTestStore *store = context;
  store->read_calls++;
  if (image_index >= 8u || instance_index >= 4u || subresource >= 64u)
    return;
  *out_state = store->states[image_index][instance_index][subresource];
}

static void rg_retained_test_commit(void *context, uint32_t image_index,
                                    uint32_t instance_index,
                                    uint32_t subresource,
                                    const VkrRgRetainedState *state) {
  RgRetainedTestStore *store = context;
  store->commit_calls++;
  if (image_index >= 8u || instance_index >= 4u || subresource >= 64u)
    return;
  store->states[image_index][instance_index][subresource] = *state;
}

static void test_retained_flag_rejects_conflicting_lifetimes(void) {
  printf("  Running test_retained_flag_rejects_conflicting_lifetimes...\n");
  Arena *arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  /* RETAINED describes content lifetime; each of these describes where
     instances come from or who owns them, and contradicts in-place retention.
   */
  static const char *const conflicts[] = {"TRANSIENT", "EXTERNAL", "HISTORY",
                                          "PER_FRAME_SLOT"};
  for (uint32_t i = 0; i < ArrayCount(conflicts); ++i) {
    char source[512];
    snprintf(source, sizeof(source),
             "{\"version\":1,\"name\":\"retained\",\"resources\":[{"
             "\"name\":\"img\",\"type\":\"image\",\"extent\":{\"mode\":"
             "\"viewport\"},\"format\":\"D32_SFLOAT\",\"usage\":["
             "\"DEPTH_STENCIL_ATTACHMENT\",\"SAMPLED\"],\"flags\":["
             "\"RETAINED\",\"%s\"]}],\"passes\":[]}",
             conflicts[i]);
    VkrRgJsonGraph json = {0};
    assert(!rg_barrier_test_load_json(&allocator, source, &json));
    vkr_rg_json_destroy(&json);
  }

  /* PER_IMAGE and RESIZABLE are orthogonal axes and must still compose. */
  const char *ok_source =
      "{\"version\":1,\"name\":\"retained\",\"resources\":[{"
      "\"name\":\"img\",\"type\":\"image\",\"extent\":{\"mode\":\"viewport\"},"
      "\"format\":\"D32_SFLOAT\",\"usage\":[\"DEPTH_STENCIL_ATTACHMENT\","
      "\"SAMPLED\"],\"flags\":[\"RETAINED\",\"PER_IMAGE\",\"RESIZABLE\"]}],"
      "\"passes\":[]}";
  VkrRgJsonGraph json = {0};
  assert(rg_barrier_test_load_json(&allocator, ok_source, &json));
  vkr_rg_json_destroy(&json);

  const char *buffer_source =
      "{\"version\":1,\"name\":\"retained\",\"resources\":[{"
      "\"name\":\"buffer\",\"type\":\"buffer\",\"size\":16,"
      "\"usage\":[\"STORAGE\"],\"flags\":[\"RETAINED\"]}],"
      "\"passes\":[]}";
  assert(!rg_barrier_test_load_json(&allocator, buffer_source, &json));
  vkr_rg_json_destroy(&json);

  arena_destroy(arena);
  printf("  test_retained_flag_rejects_conflicting_lifetimes PASSED\n");
}

static void test_retained_is_not_an_instance_domain(void) {
  printf("  Running test_retained_is_not_an_instance_domain...\n");
  /* RETAINED alone must not move a resource off the single-instance domain,
     and must not displace PER_IMAGE when both are set. */
  assert(vkr_rg_resource_instance_domain(VKR_RG_RESOURCE_FLAG_RETAINED) ==
         VKR_RG_RESOURCE_INSTANCE_SINGLE);
  assert(vkr_rg_resource_instance_domain(VKR_RG_RESOURCE_FLAG_RETAINED |
                                         VKR_RG_RESOURCE_FLAG_PER_IMAGE) ==
         VKR_RG_RESOURCE_INSTANCE_PER_IMAGE);
  printf("  test_retained_is_not_an_instance_domain PASSED\n");
}

static void test_retained_read_without_contents_fails_compile(void) {
  printf("  Running test_retained_read_without_contents_fails_compile...\n");
  Arena *arena = arena_create(MB(4), MB(4));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  MemZero(&g_retained_store, sizeof(g_retained_store));
  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  const VkrRgRetainedStateProvider provider = {
      .context = &g_retained_store,
      .read = rg_retained_test_read,
      .commit = rg_retained_test_commit,
  };
  vkr_rg_set_retained_state_provider(graph, &provider);

  VkrRenderGraphFrameInfo frame = {.target_width = 4u, .target_height = 4u};
  vkr_rg_begin_frame(graph, &frame);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 4u;
  desc.height = 4u;
  desc.format = VKR_TEXTURE_FORMAT_R32_SFLOAT;
  desc.flags = VKR_RG_RESOURCE_FLAG_RETAINED;
  desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_STORAGE |
                                                 VKR_TEXTURE_USAGE_SAMPLED);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("retained"), &desc);
  assert(vkr_rg_image_handle_valid(image));

  /* A sampled read with no writer anywhere in the frame. The backend reports
     no contents, so scheduling this would sample undefined memory. */
  VkrRgPassBuilder reader =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "reader");
  vkr_rg_pass_read_image(&reader, image, VKR_RG_IMAGE_ACCESS_STORAGE_READ, 0u,
                         0u);

  assert(!vkr_rg_compile_schedule(graph));
  assert(g_retained_store.read_calls > 0u);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_retained_read_without_contents_fails_compile PASSED\n");
}

static void test_retained_commit_then_read_succeeds(void) {
  printf("  Running test_retained_commit_then_read_succeeds...\n");
  Arena *arena = arena_create(MB(4), MB(4));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  MemZero(&g_retained_store, sizeof(g_retained_store));
  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  const VkrRgRetainedStateProvider provider = {
      .context = &g_retained_store,
      .read = rg_retained_test_read,
      .commit = rg_retained_test_commit,
  };
  vkr_rg_set_retained_state_provider(graph, &provider);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 4u;
  desc.height = 4u;
  desc.format = VKR_TEXTURE_FORMAT_R32_SFLOAT;
  desc.flags = VKR_RG_RESOURCE_FLAG_RETAINED;
  desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_STORAGE |
                                                 VKR_TEXTURE_USAGE_SAMPLED);

  /* Frame 1 writes the image, then commits as a successful submit would. */
  VkrRenderGraphFrameInfo frame = {.target_width = 4u, .target_height = 4u};
  vkr_rg_begin_frame(graph, &frame);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("retained"), &desc);
  assert(vkr_rg_image_handle_valid(image));
  VkrRgPassBuilder writer =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "writer");
  vkr_rg_pass_write_image(&writer, image, VKR_RG_IMAGE_ACCESS_STORAGE_WRITE, 0u,
                          0u);
  assert(vkr_rg_compile_schedule(graph));
  vkr_rg_commit_retained_state(graph);
  assert(g_retained_store.commit_calls > 0u);
  assert(g_retained_store.states[0][0][0].content_valid);

  /* Frame 2 only reads. This is the case that must now compile: a reader whose
     producer ran in an earlier frame is exactly what retention is for. */
  vkr_rg_begin_frame(graph, &frame);
  image = vkr_rg_create_image(graph, string8_lit("retained"), &desc);
  assert(vkr_rg_image_handle_valid(image));
  VkrRgPassBuilder reader =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "reader");
  vkr_rg_pass_read_image(&reader, image, VKR_RG_IMAGE_ACCESS_STORAGE_READ, 0u,
                         0u);
  assert(vkr_rg_compile_schedule(graph));

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_retained_commit_then_read_succeeds PASSED\n");
}

static void test_retained_uncommitted_frame_rolls_back(void) {
  printf("  Running test_retained_uncommitted_frame_rolls_back...\n");
  Arena *arena = arena_create(MB(4), MB(4));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  MemZero(&g_retained_store, sizeof(g_retained_store));
  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  const VkrRgRetainedStateProvider provider = {
      .context = &g_retained_store,
      .read = rg_retained_test_read,
      .commit = rg_retained_test_commit,
  };
  vkr_rg_set_retained_state_provider(graph, &provider);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 4u;
  desc.height = 4u;
  desc.format = VKR_TEXTURE_FORMAT_R32_SFLOAT;
  desc.flags = VKR_RG_RESOURCE_FLAG_RETAINED;
  desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_STORAGE |
                                                 VKR_TEXTURE_USAGE_SAMPLED);

  VkrRenderGraphFrameInfo frame = {.target_width = 4u, .target_height = 4u};
  vkr_rg_begin_frame(graph, &frame);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("retained"), &desc);
  VkrRgPassBuilder writer =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "writer");
  vkr_rg_pass_write_image(&writer, image, VKR_RG_IMAGE_ACCESS_STORAGE_WRITE, 0u,
                          0u);
  assert(vkr_rg_compile_schedule(graph));

  /* Frame compiled and planned a write, but never submitted, so commit is not
     called. Contents must stay invalid: the write never reached the GPU. */
  assert(g_retained_store.commit_calls == 0u);
  assert(!g_retained_store.states[0][0][0].content_valid);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_retained_uncommitted_frame_rolls_back PASSED\n");
}

static void test_retained_state_is_per_instance_and_per_layer(void) {
  printf("  Running test_retained_state_is_per_instance_and_per_layer...\n");
  Arena *arena = arena_create(MB(4), MB(4));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  MemZero(&g_retained_store, sizeof(g_retained_store));
  /* Layer 1 of instance 1 has contents; nothing else does. If seeding ignored
     either axis, the reader below would be allowed to run. */
  g_retained_store.states[0][1][1] = (VkrRgRetainedState){
      .layout = VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .content_valid = true_v,
  };

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  const VkrRgRetainedStateProvider provider = {
      .context = &g_retained_store,
      .read = rg_retained_test_read,
      .commit = rg_retained_test_commit,
  };
  vkr_rg_set_retained_state_provider(graph, &provider);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 4u;
  desc.height = 4u;
  desc.layers = 4u;
  desc.format = VKR_TEXTURE_FORMAT_R32_SFLOAT;
  desc.flags = VKR_RG_RESOURCE_FLAG_RETAINED | VKR_RG_RESOURCE_FLAG_PER_IMAGE;
  desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_STORAGE |
                                                 VKR_TEXTURE_USAGE_SAMPLED);

  /* Selecting target image 1 must consult instance 1. An exact read of its
     valid layer succeeds. */
  VkrRenderGraphFrameInfo frame = {
      .target_width = 4u, .target_height = 4u, .image_index = 1u};
  vkr_rg_begin_frame(graph, &frame);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("retained"), &desc);
  VkrRgPassBuilder reader =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "reader");
  vkr_rg_pass_read_image_slice(
      &reader, image, VKR_RG_IMAGE_ACCESS_STORAGE_READ, 0u, 0u,
      (VkrRgImageSlice){.base_layer = 1u, .layer_count = 1u});
  assert(vkr_rg_compile_schedule(graph));
  vkr_rg_end_frame(graph);

  /* A whole-image read still fails because the other layers have nothing. */
  vkr_rg_begin_frame(graph, &frame);
  image = vkr_rg_create_image(graph, string8_lit("retained"), &desc);
  reader = rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "reader");
  vkr_rg_pass_read_image(&reader, image, VKR_RG_IMAGE_ACCESS_STORAGE_READ, 0u,
                         0u);
  assert(!vkr_rg_compile_schedule(graph));

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_retained_state_is_per_instance_and_per_layer PASSED\n");
}

static void test_retained_write_does_not_validate_other_layers(void) {
  printf("  Running test_retained_write_does_not_validate_other_layers...\n");
  Arena *arena = arena_create(MB(4), MB(4));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  MemZero(&g_retained_store, sizeof(g_retained_store));
  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph);
  const VkrRgRetainedStateProvider provider = {
      .context = &g_retained_store,
      .read = rg_retained_test_read,
      .commit = rg_retained_test_commit,
  };
  vkr_rg_set_retained_state_provider(graph, &provider);

  VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
  desc.width = 4u;
  desc.height = 4u;
  desc.layers = 2u;
  desc.format = VKR_TEXTURE_FORMAT_R32_SFLOAT;
  desc.flags = VKR_RG_RESOURCE_FLAG_RETAINED;
  desc.usage = vkr_texture_usage_flags_from_bits(VKR_TEXTURE_USAGE_STORAGE);

  VkrRenderGraphFrameInfo frame = {.target_width = 4u, .target_height = 4u};
  vkr_rg_begin_frame(graph, &frame);
  VkrRgImageHandle image =
      vkr_rg_create_image(graph, string8_lit("retained"), &desc);
  VkrRgPassBuilder writer =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "writer");
  vkr_rg_pass_write_image_slice_at_stages(
      &writer, image, VKR_RG_IMAGE_ACCESS_STORAGE_WRITE,
      VKR_GPU_STAGE_COMPUTE_SHADER, 0u, 0u,
      (VkrRgImageSlice){.base_layer = 0u, .layer_count = 1u});
  VkrRgPassBuilder reader =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "reader");
  vkr_rg_pass_read_image_slice(
      &reader, image, VKR_RG_IMAGE_ACCESS_STORAGE_READ, 0u, 0u,
      (VkrRgImageSlice){.base_layer = 1u, .layer_count = 1u});
  assert(!vkr_rg_compile_schedule(graph));

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_retained_write_does_not_validate_other_layers PASSED\n");
}

bool32_t run_render_graph_barrier_tests() {
  printf("--- Running RenderGraph barrier tests... ---\n");

  test_resource_instance_domains();
  test_retained_flag_rejects_conflicting_lifetimes();
  test_retained_is_not_an_instance_domain();
  test_retained_read_without_contents_fails_compile();
  test_retained_commit_then_read_succeeds();
  test_retained_uncommitted_frame_rolls_back();
  test_retained_state_is_per_instance_and_per_layer();
  test_retained_write_does_not_validate_other_layers();
  test_image_access_is_write();
  test_json_bindings_and_condition_parity();
  test_transmission_condition();
  test_transmission_compact_conditions_and_viewport_buffer();
  test_shadow_map_capacity_is_independent_of_active_cascades();
  test_shadow_reads_follow_active_cascades();
  test_repeat_condition_mask_filters_iterations();
  test_conflicting_runtime_bindings_are_rejected();
  test_typed_executor_and_direct_dispatch_contract();
  test_indirect_dispatch_dependency_contract();
  test_json_mip_chain_and_subresource_uses();
  test_deferred_image_formats();
  test_frame_allocator_reclaims_authored_passes();
  test_main_graph_declares_transmission_stages();
  test_main_graph_fits_runtime_pass_capacity();
  test_subresource_range_resolve();
  test_same_layout_write_then_read_emits_barrier();
  test_explicit_stage_and_subresource_dependency();
  test_present_target_import_and_terminal_states();
  test_write_after_write_emits_barrier();
  test_read_after_read_emits_nothing();
  test_same_pass_storage_read_write_combines();
  test_same_pass_incompatible_layouts_are_rejected();
  test_cascade_slices_are_per_layer_then_coalesce();
  test_disjoint_layer_writes_coalesce_on_read();
  test_capture_read_uses_exact_array_slice();

  printf("--- RenderGraph barrier tests completed. ---\n");
  return true;
}
