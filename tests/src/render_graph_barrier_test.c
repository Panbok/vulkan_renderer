#include "render_graph_barrier_test.h"
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

static void rg_barrier_test_fail_execute(VkrRgPassContext *ctx,
                                         void *user_data) {
  (void)user_data;
  ctx->error = VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED;
}

typedef struct RgTargetCapture {
  VkrRenderTargetHandle expected;
  bool8_t called;
} RgTargetCapture;

static void rg_barrier_test_capture_target(VkrRgPassContext *ctx,
                                           void *user_data) {
  RgTargetCapture *capture = (RgTargetCapture *)user_data;
  assert(ctx->render_target == capture->expected);
  capture->called = true_v;
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

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_same_layout_write_then_read_emits_barrier PASSED\n");
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

static void test_executor_error_reaches_graph_caller(void) {
  printf("  Running test_executor_error_reaches_graph_caller...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph != NULL);
  VkrRenderGraphFrameInfo frame = {0};
  vkr_rg_begin_frame(graph, &frame);

  VkrRgPassBuilder pass = rg_barrier_test_add_pass(
      graph, VKR_RG_PASS_TYPE_COMPUTE, "FailingExecutor");
  vkr_rg_pass_set_execute(&pass, rg_barrier_test_fail_execute, NULL);
  assert(vkr_rg_compile_schedule(graph));
  // Renderer allocation is intentionally outside this CPU-only test. Marking
  // the renderer-independent schedule compiled lets execute exercise the
  // executor/error contract without a Vulkan backend.
  graph->compiled = true_v;

  assert(vkr_rg_execute(graph, NULL) ==
         VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_executor_error_reaches_graph_caller PASSED\n");
}

static void test_single_render_target_serves_every_swapchain_image(void) {
  printf(
      "  Running test_single_render_target_serves_every_swapchain_image...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrRenderGraph *graph = vkr_rg_create(&allocator);
  assert(graph != NULL);
  VkrRenderGraphFrameInfo frame = {.image_index = 2u};
  vkr_rg_begin_frame(graph, &frame);

  VkrRenderTargetHandle target = (VkrRenderTargetHandle)(uintptr_t)1u;
  VkrRenderTargetHandle targets[1] = {target};
  RgTargetCapture capture = {.expected = target};
  VkrRgPassBuilder builder =
      rg_barrier_test_add_pass(graph, VKR_RG_PASS_TYPE_COMPUTE, "SingleTarget");
  vkr_rg_pass_set_execute(&builder, rg_barrier_test_capture_target, &capture);
  assert(vkr_rg_compile_schedule(graph));

  VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, 0);
  pass->render_targets = targets;
  pass->render_target_count = 1u;
  // Renderer allocation is outside this CPU test; execution only needs the
  // already-built target array to verify image-index resolution.
  graph->compiled = true_v;

  assert(vkr_rg_execute(graph, NULL) == VKR_RENDERER_ERROR_NONE);
  assert(capture.called);

  vkr_rg_destroy(graph);
  arena_destroy(arena);
  printf("  test_single_render_target_serves_every_swapchain_image PASSED\n");
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
  desc.format = VKR_TEXTURE_FORMAT_D32_SFLOAT;
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
  desc.format = VKR_TEXTURE_FORMAT_D32_SFLOAT;
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

  const char *ordered[] = {
      "World.Opaque.Fullscreen", "World.FeedbackCopy.Fullscreen",
      "World.Transmission.Fullscreen", "World.Blend.Fullscreen"};
  uint32_t found = 0u;
  for (uint64_t i = 0u; i < graph.passes.length && found < ArrayCount(ordered);
       ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&graph.passes, i);
    if (pass && vkr_string8_equals_cstr(&pass->name, ordered[found])) {
      if (found == 1u) {
        assert(pass->type == VKR_RG_JSON_PASS_TRANSFER);
        assert(pass->reads.length == 1u && pass->writes.length == 1u);
        VkrRgJsonResourceUse *read =
            vector_get_VkrRgJsonResourceUse(&pass->reads, 0u);
        VkrRgJsonResourceUse *write =
            vector_get_VkrRgJsonResourceUse(&pass->writes, 0u);
        assert(read && write);
        assert(read->image_access == VKR_RG_JSON_IMAGE_ACCESS_TRANSFER_SRC);
        assert(write->image_access == VKR_RG_JSON_IMAGE_ACCESS_TRANSFER_DST);
      }
      found++;
    }
  }
  assert(found == ArrayCount(ordered));
  vkr_rg_json_destroy(&graph);
  arena_destroy(arena);
  printf("  test_main_graph_declares_transmission_stages PASSED\n");
}

bool32_t run_render_graph_barrier_tests() {
  printf("--- Running RenderGraph barrier tests... ---\n");

  test_image_access_is_write();
  test_main_graph_declares_transmission_stages();
  test_subresource_range_resolve();
  test_same_layout_write_then_read_emits_barrier();
  test_present_target_import_and_terminal_states();
  test_write_after_write_emits_barrier();
  test_read_after_read_emits_nothing();
  test_same_pass_storage_read_write_combines();
  test_same_pass_incompatible_layouts_are_rejected();
  test_executor_error_reaches_graph_caller();
  test_single_render_target_serves_every_swapchain_image();
  test_cascade_slices_are_per_layer_then_coalesce();
  test_disjoint_layer_writes_coalesce_on_read();
  test_capture_read_uses_exact_array_slice();

  printf("--- RenderGraph barrier tests completed. ---\n");
  return true;
}
