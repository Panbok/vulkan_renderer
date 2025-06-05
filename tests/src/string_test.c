#include "string_test.h"
#include <stdarg.h> // Ensure va_list and related macros are available

static Arena *arena = NULL;
static const uint64_t ARENA_SIZE = MB(1);

// Setup function called before each test function in this suite
static void setup_suite(void) { arena = arena_create(ARENA_SIZE, ARENA_SIZE); }

// Teardown function called after each test function in this suite
static void teardown_suite(void) {
  if (arena) {
    arena_destroy(arena);
    arena = NULL;
  }
}

// Helper function to correctly call string8_create_formatted_v for testing
static String8 invoke_test_string8_create_formatted_v(Arena *test_arena,
                                                      const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  String8 result = string8_create_formatted_v(test_arena, fmt, args);
  va_end(args);
  return result;
}

static void test_str8_create(void) {
  printf("  Running test_str8_create...\n");
  char *test_string = "Hello, World!";
  String8 str = string8_create((uint8_t *)test_string, strlen(test_string));
  assert(str.length == 13 && "String length is not 13");
  assert(strcmp((char *)str.str, "Hello, World!") == 0 &&
         "String is not 'Hello, World!'");
  printf("  test_str8_create PASSED\n");
}

static void test_str8_create_literal(void) {
  printf("  Running test_str8_create_literal...\n");
  String8 str = string8_lit("Hello, World!");
  assert(str.length == 13 && "String length is not 13");
  assert(strcmp((char *)str.str, "Hello, World!") == 0 &&
         "String is not 'Hello, World!'");
  printf("  test_str8_create_literal PASSED\n");
}

static void test_str8_create_formatted(void) {
  printf("  Running test_str8_create_formatted...\n");
  setup_suite();

  String8 str = string8_create_formatted(arena, "Hello, %s!", "World");
  assert(str.length == 13 && "String length is not 13");
  assert(strcmp((char *)str.str, "Hello, World!") == 0 &&
         "String is not 'Hello, World!'");

  string8_destroy(&str);
  teardown_suite();
  printf("  test_str8_create_formatted PASSED\n");
}

static void test_str8_create_formatted_v(void) {
  printf("  Running test_str8_create_formatted_v...\n");
  setup_suite();

  // Corrected call using the helper:
  String8 str =
      invoke_test_string8_create_formatted_v(arena, "Hello, %s!", "World");
  assert(str.length == 13 && "String length is not 13");
  assert(strcmp((char *)str.str, "Hello, World!") == 0 &&
         "String is not 'Hello, World!'");

  string8_destroy(&str);
  teardown_suite();
  printf("  test_str8_create_formatted_v PASSED\n");
}

static void test_str8_cstr(void) {
  printf("  Running test_str8_cstr...\n");
  setup_suite();

  String8 str = string8_create_formatted(arena, "Hello, %s!", "World");
  const uint8_t *cstr = string8_cstr(&str);
  assert(strcmp((char *)cstr, "Hello, World!") == 0 &&
         "String is not 'Hello, World!'");

  string8_destroy(&str);
  teardown_suite();
  printf("  test_str8_cstr PASSED\n");
}

static void test_str8_concat(void) {
  printf("  Running test_str8_concat...\n");
  setup_suite();

  String8 str1 = string8_create_formatted(arena, "Hello, ");
  String8 str2 = string8_create_formatted(arena, "World!");
  String8 str = string8_concat(arena, &str1, &str2);
  assert(str.length == 13 && "String length is not 13");
  assert(strcmp((char *)str.str, "Hello, World!") == 0 &&
         "String is not 'Hello, World!'");

  string8_destroy(&str);
  string8_destroy(&str1);
  string8_destroy(&str2);
  teardown_suite();
  printf("  test_str8_concat PASSED\n");
}

static void test_str8_destroy(void) {
  printf("  Running test_str8_destroy...\n");

  String8 str = string8_lit("Hello, World!");
  string8_destroy(&str);

  assert(str.str == NULL && "String is not NULL");
  assert(str.length == 0 && "String length is not 0");

  printf("  test_str8_destroy PASSED\n");
}

bool32_t run_string_tests(void) {
  printf("--- Starting String Tests ---\n");

  test_str8_create();
  test_str8_create_literal();
  test_str8_create_formatted();
  test_str8_create_formatted_v();
  test_str8_cstr();
  test_str8_concat();
  test_str8_destroy();

  printf("--- String Tests Completed ---\n");
  return true;
}
