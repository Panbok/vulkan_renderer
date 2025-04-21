#include <stdio.h>

#include "string.h"

int main(int argc, char **argv) {
  Arena *arena = arena_create();
  uint32_t *nums = (uint32_t *)arena_alloc(arena, sizeof(uint32_t) * 10);
  for (size_t i = 0; i < 10; i++) {
    nums[i] = i;
  }
  for (size_t i = 0; i < 10; i++) {
    printf("%d\n", nums[i]);
  }

  String8 str1 = string8_lit("Hello, ");
  String8 str2 = string8_lit("World!");
  String8 str = string8_concat(arena, &str1, &str2);
  printf("%s\n", string8_cstr(&str));

  string8_destroy(&str);
  arena_destroy(arena);
  return 0;
}