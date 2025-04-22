#include <stdio.h>

#include "array.h"
#include "string.h"

int main(int argc, char **argv) {
  Arena *arena = arena_create();
  Scratch scratch = scratch_create(arena);
  Array_uint32_t array = array_create_uint32_t(arena, 10);
  for (size_t i = 0; i < 10; i++) {
    array_set_uint32_t(&array, i, i);
  }
  for (size_t i = 0; i < array.length; i++) {
    printf("%d\n", *array_get_uint32_t(&array, i));
  }
  scratch_destroy(scratch);

  String8 str1 = string8_lit("Hello, ");
  String8 str2 = string8_lit("World!");
  String8 str = string8_concat(arena, &str1, &str2);
  printf("%s\n", string8_cstr(&str));

  string8_destroy(&str);
  arena_destroy(arena);
  return 0;
}