#include "array.h"
#include "logger.h"
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
    log_info(arena, "Static array: %d", *array_get_uint32_t(&array, i));
  }
  scratch_destroy(scratch);
  array_destroy_uint32_t(&array);

  Vector_uint32_t vector = vector_create_uint32_t(arena);

  for (size_t i = 0; i < 100; i++) {
    vector_push_uint32_t(&vector, i * 32);
  }
  for (size_t i = 0; i < vector.length; i++) {
    log_info(arena, "Dynamic array: %d", *vector_get_uint32_t(&vector, i));
  }
  scratch_destroy(scratch);
  vector_destroy_uint32_t(&vector);

  String8 str1 = string8_lit("Hello, ");
  String8 str2 = string8_lit("World!");
  String8 str = string8_concat(arena, &str1, &str2);
  log_trace(arena, "%s", string8_cstr(&str));
  log_info(arena, "%s", string8_cstr(&str));
  log_debug(arena, "%s", string8_cstr(&str));
  log_warn(arena, "%s", string8_cstr(&str));
  log_error(arena, "%s", string8_cstr(&str));
  log_fatal(arena, "%s", string8_cstr(&str));
  scratch_destroy(scratch);
  string8_destroy(&str);

  arena_destroy(arena);
  return 0;
}