#include <stdio.h>

#include "core.h"
#include "string.h"

int main(int argc, char **argv) {
  Arena *arena = arena_create();
  int *nums = (int *)arena_alloc(arena, sizeof(int) * 10);
  for (int i = 0; i < 10; i++) {
    nums[i] = i;
  }
  for (int i = 0; i < 10; i++) {
    printf("%d\n", nums[i]);
  }

  String str1 = string_lit("Hello, ");
  String str2 = string_lit("World!");
  String str = string_concat(arena, &str1, &str2);
  printf("%s\n", string_cstr(&str));

  string_destroy(&str);
  arena_destroy(arena);
  return 0;
}