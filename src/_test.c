#include "arena_test.h"

int main() {
  if (run_arena_tests()) {
    printf("All arena tests PASSED.\n");
    return 0;
  } else {
    printf("Some arena tests FAILED.\n");
    return 1; // Should not be reached if asserts are active
  }
}