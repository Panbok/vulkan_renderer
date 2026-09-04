#include "test_main.h"

typedef bool32_t (*VkrTestSuite)(void);

static const VkrTestSuite VKR_TEST_SUITES[] = {
    run_allocator_tests,
    run_atomic_tests,
    run_metrics_tests,
    run_arena_tests,
    run_array_tests,
    run_vector_tests,
    run_queue_tests,
    run_bitset_tests,
    run_event_data_buffer_tests,
    run_threads_tests,
    run_job_system_tests,
    run_input_tests,
    run_debug_overlay_tests,
    run_json_tests,
    run_json_writer_tests,
    run_harness_tests,
    run_event_tests,
    run_math_tests,
    run_vec_tests,
    run_mat_tests,
    run_quat_tests,
    run_transform_tests,
    run_simd_tests,
    run_clock_tests,
    run_string_tests,
    run_text_tests,
    run_font_cooked_tests,
    run_texture_format_tests,
    run_texture_hdr_tests,
    run_texture_lifetime_tests,
    run_ibl_math_tests,
    run_lighting_system_tests,
    run_texture_vkt_tests,
    run_renderer_impl_tests,
    run_vulkan_tests,
    run_packet_constants_tests,
    run_transmission_tests,
    run_temporal_tests,
    run_exposure_tests,
    run_bloom_tests,
    run_gtao_tests,
    run_picking_state_tests,
    run_visibility_tests,
    run_editor_viewport_tests,
    run_ui_layout_tests,
    run_shadow_system_tests,
    run_render_graph_barrier_tests,
    run_resource_async_state_tests,
    run_scene_loader_tests,
    run_gltf_importer_tests,
    run_material_pbr_tests,
    run_mesh_cooked_tests,
    run_filesystem_tests,
    run_hashtable_tests,
    run_freelist_tests,
    run_metal_memory_tests,
    run_metal_packet_abi_tests,
    run_metal_capture_ring_tests,
    run_metal_material_tests,
    run_pool_tests,
    run_dmemory_tests,
    run_entity_tests,
};

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("Running tests...\n\n");

  vkr_platform_init();

  Arena *log_arena = arena_create(MB(1), MB(1));
  log_init(log_arena);

  bool32_t all_passed = true;
  for (uint32_t i = 0u; i < ArrayCount(VKR_TEST_SUITES); ++i) {
    all_passed &= VKR_TEST_SUITES[i]();
    printf("\n");
  }

  vkr_platform_shutdown();

  printf("\nAll tests completed.\n");
  return all_passed ? 0 : 1;
}
