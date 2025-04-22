#include <stdio.h>

#include "array.h"
#include "string.h"
#include "vector.h"

int main(int argc, char **argv) {
  Arena *arena = arena_create();
  Scratch scratch = scratch_create(arena);
  Array_uint32_t array = array_create_uint32_t(arena, 10);
  for (size_t i = 0; i < 10; i++) {
    array_set_uint32_t(&array, i, i * 1024);
  }
  for (size_t i = 0; i < array.length; i++) {
    printf("Static array: %d\n", *array_get_uint32_t(&array, i));
  }
  scratch_destroy(scratch);

  Vector_uint32_t vector = vector_create_uint32_t(arena);
  Scratch scratch2 = scratch_create(arena);
  for (size_t i = 0; i < 100; i++) {
    vector_push_uint32_t(&vector, i * 32);
  }
  for (size_t i = 0; i < vector.length; i++) {
    printf("Dynamic array: %d\n", *vector_get_uint32_t(&vector, i));
  }
  scratch_destroy(scratch2);

  String8 str1 = string8_lit("Hello, ");
  String8 str2 = string8_lit("World!");
  String8 str = string8_concat(arena, &str1, &str2);
  printf("%s\n", string8_cstr(&str));

  string8_destroy(&str);
  arena_destroy(arena);
  return 0;
}