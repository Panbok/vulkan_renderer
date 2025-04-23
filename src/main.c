#include "array.h"
#include "logger.h"
#include "string.h"
#include "vector.h"

int main(int argc, char **argv) {
  Arena *log_arena = arena_create(MB(1), MB(1));
  log_init(log_arena);

  Arena *arena = arena_create();
  Scratch scratch = scratch_create(arena);

  Array_uint32_t array = array_create_uint32_t(arena, 10);
  for (size_t i = 0; i < 10; i++) {
    array_set_uint32_t(&array, i, i * 1024);
  }
  for (size_t i = 0; i < array.length; i++) {
    log_info("Static array: %d", *array_get_uint32_t(&array, i));
  }
  scratch_destroy(scratch);
  array_destroy_uint32_t(&array);

  Vector_uint32_t vector = vector_create_uint32_t(arena);

  for (size_t i = 0; i < 100; i++) {
    vector_push_uint32_t(&vector, i * 32);
  }
  for (size_t i = 0; i < vector.length; i++) {
    log_info("Dynamic array: %d", *vector_get_uint32_t(&vector, i));
  }
  scratch_destroy(scratch);
  vector_destroy_uint32_t(&vector);

  String8 str1 = string8_lit("Hello, ");
  String8 str2 = string8_lit("World!");
  String8 str = string8_concat(arena, &str1, &str2);
  log_trace("%s", string8_cstr(&str));
  log_info("%s", string8_cstr(&str));
  log_debug("%s", string8_cstr(&str));
  log_warn("%s", string8_cstr(&str));
  log_error("%s", string8_cstr(&str));
  assert_log(0 == 1, string8_cstr(&str));
  scratch_destroy(scratch);
  string8_destroy(&str);

  arena_destroy(arena);
  return 0;
}