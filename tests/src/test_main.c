#include "test_main.h"

int main(int argc, char **argv) {
  (void)argc; // Unused
  (void)argv; // Unused
  printf("Running tests...\n\n");

  Arena *log_arena = arena_create(MB(1), MB(1));
  log_init(log_arena);

  bool32_t all_passed = true;

  // Run all tests
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
  all_passed &= run_mmemory_tests();
  printf("\n"); // Add spacing
  all_passed &= run_event_data_buffer_tests();
  printf("\n"); // Add spacing
  all_passed &= run_event_tests();
  printf("\n"); // Add spacing
  all_passed &= run_input_tests();
  printf("\n"); // Add spacing
  all_passed &= run_clock_tests();
  printf("\n"); // Add spacing
  all_passed &= run_string_tests();

  printf("\nAll tests completed.\n");
  return all_passed ? 0 : 1; // Return 0 on success, 1 on failure
}