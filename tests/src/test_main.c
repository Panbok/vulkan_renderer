#include "test_main.h"

int main(int argc, char **argv) {
  (void)argc; // Unused
  (void)argv; // Unused
  printf("Running tests...\n\n");

  vkr_platform_init();

  Arena *log_arena = arena_create(MB(1), MB(1));
  log_init(log_arena);

  bool32_t all_passed = true;

  // Run all tests
  all_passed &= run_allocator_tests();
  printf("\n"); // Add spacing
  all_passed &= run_atomic_tests();
  printf("\n"); // Add spacing
  all_passed &= run_arena_tests();
  printf("\n"); // Add spacing
  all_passed &= run_array_tests();
  printf("\n"); // Add spacing
  all_passed &= run_vector_tests();
  printf("\n"); // Add spacing
  all_passed &= run_queue_tests();
  printf("\n"); // Add spacing
  all_passed &= run_bitset_tests();
  printf("\n"); // Add spacing
  all_passed &= run_event_data_buffer_tests();
  printf("\n"); // Add spacing
  all_passed &= run_threads_tests();
  printf("\n"); // Add spacing
  all_passed &= run_job_system_tests();
  printf("\n"); // Add spacing
  all_passed &= run_input_tests();
  printf("\n"); // Add spacing
  all_passed &= run_json_tests();
  printf("\n"); // Add spacing
  all_passed &= run_event_tests();
  printf("\n"); // Add spacing
  all_passed &= run_math_tests();
  printf("\n"); // Add spacing
  all_passed &= run_vec_tests();
  printf("\n"); // Add spacing
  all_passed &= run_mat_tests();
  printf("\n"); // Add spacing
  all_passed &= run_quat_tests();
  printf("\n"); // Add spacing
  all_passed &= run_transform_tests();
  printf("\n"); // Add spacing
  all_passed &= run_simd_tests();
  printf("\n"); // Add spacing
  all_passed &= run_clock_tests();
  printf("\n"); // Add spacing
  all_passed &= run_string_tests();
  printf("\n"); // Add spacing
  all_passed &= run_text_tests();
  printf("\n"); // Add spacing
  all_passed &= run_texture_format_tests();
  printf("\n"); // Add spacing
  all_passed &= run_texture_vkt_tests();
  printf("\n"); // Add spacing
  all_passed &= run_renderer_batch_tests();
  printf("\n"); // Add spacing
  all_passed &= run_resource_async_state_tests();
  printf("\n"); // Add spacing
  all_passed &= run_filesystem_tests();
  printf("\n"); // Add spacing
  all_passed &= run_reflection_pipeline_tests();
  printf("\n"); // Add spacing
  all_passed &= run_hashtable_tests();
  printf("\n"); // Add spacing
  all_passed &= run_freelist_tests();
  printf("\n"); // Add spacing
  all_passed &= run_pool_tests();
  printf("\n"); // Add spacing
  all_passed &= run_dmemory_tests();
  printf("\n"); // Add spacing
  all_passed &= run_entity_tests();

  vkr_platform_shutdown();

  printf("\nAll tests completed.\n");
  return all_passed ? 0 : 1; // Return 0 on success, 1 on failure
}
