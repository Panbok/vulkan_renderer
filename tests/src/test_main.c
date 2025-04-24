#include "arena_test.h"
#include <stdio.h>

int main(int argc, char **argv) {
  printf("Running tests...\n");

  // Run all tests
  run_arena_tests();

  // Additional test functions would be called here

  printf("All tests completed.\n");
  return 0;
}