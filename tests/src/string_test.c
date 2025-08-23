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

/////////////////////
// String8 Tests
/////////////////////

static void test_str8_create(void) {
  printf("  Running test_str8_create...\n");
  char *test_string = "Hello, World!";
  String8 str =
      string8_create((uint8_t *)test_string, string_length(test_string));
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
  const char *cstr = string8_cstr(&str);
  assert(strcmp(cstr, "Hello, World!") == 0 && "String is not 'Hello, World!'");

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
  setup_suite();

  String8 str = string8_lit("Hello, World!");
  string8_destroy(&str);

  assert(str.str == NULL && "String is not NULL");
  assert(str.length == 0 && "String length is not 0");

  teardown_suite();
  printf("  test_str8_destroy PASSED\n");
}

static void test_str8_substring(void) {
  printf("  Running test_str8_substring...\n");
  setup_suite();

  String8 str = string8_lit("Hello, World!");
  String8 sub = string8_substring(&str, 0, 5);
  String8 res = string8_lit("Hello");

  assert(sub.length == 5 && "Substring length is not 5");
  assert(string8_equals(&sub, &res) && "Substring is not equal to string");

  string8_destroy(&str);
  string8_destroy(&res);
  if (sub.str != str.str) {
    string8_destroy(&sub);
  }
  teardown_suite();
  printf("  test_str8_substring PASSED\n");
}

static void test_str8_contains(void) {
  printf("  Running test_str8_contains...\n");
  setup_suite();

  String8 str = string8_lit("Hello, World!");
  String8 sub = string8_substring(&str, 0, 5);
  assert(string8_contains(&str, &sub) && "String does not contain substring");

  if (sub.str != str.str) {
    string8_destroy(&sub);
  }
  teardown_suite();
  printf("  test_str8_contains PASSED\n");
}

static void test_str8_contains_cstr(void) {
  printf("  Running test_str8_contains_cstr...\n");
  setup_suite();

  String8 str = string8_lit("Hello, World!");
  assert(string8_contains_cstr(&str, "Hello") &&
         "String does not contain substring");
  teardown_suite();
  printf("  test_str8_contains_cstr PASSED\n");
}

static void test_str8_equals(void) {
  printf("  Running test_str8_equals...\n");
  setup_suite();

  String8 str1 = string8_lit("Hello, World!");
  String8 str2 = string8_lit("Hello, World!");
  assert(string8_equals(&str1, &str2) && "Strings are not equal");
  teardown_suite();
  printf("  test_str8_equals PASSED\n");
}

/////////////////////
// CString Tests
/////////////////////

// Helper for calling string_format_v
static int helper_string_format_v(char *dest, uint64_t size, const char *fmt,
                                  ...) {
  va_list args;
  va_start(args, fmt);
  int result = string_format_v(dest, size, fmt, args);
  va_end(args);
  return result;
}

static void test_cstring_equals(void) {
  printf("  Running test_cstring_equals...\n");
  assert(string_equals("abc", "abc"));
  assert(!string_equals("abc", "abcd"));
  printf("  test_cstring_equals PASSED\n");
}

static void test_cstring_equalsi(void) {
  printf("  Running test_cstring_equalsi...\n");
  assert(string_equalsi("AbC", "aBc"));
  assert(!string_equalsi("abc", "abD"));
  printf("  test_cstring_equalsi PASSED\n");
}

static void test_cstring_length(void) {
  printf("  Running test_cstring_length...\n");
  assert(string_length("") == 0);
  assert(string_length("hello") == 5);
  printf("  test_cstring_length PASSED\n");
}

static void test_cstring_duplicate(void) {
  printf("  Running test_cstring_duplicate...\n");
  const char *src = "duplicate me";
  char *dup = string_duplicate(src);
  assert(dup != NULL);
  assert(strcmp(dup, src) == 0);
  free(dup);
  printf("  test_cstring_duplicate PASSED\n");
}

static void test_cstring_contains(void) {
  printf("  Running test_cstring_contains...\n");
  const char *src = "Hello, World!";
  assert(string_contains(src, "World"));
  assert(string_contains(src, ""));
  assert(!string_contains(src, "earth"));
  printf("  test_cstring_contains PASSED\n");
}

static void test_cstring_substring(void) {
  printf("  Running test_cstring_substring...\n");
  setup_suite();

  const char *src = "Hello, World!";
  char *mid = string_substring(arena, src, 7, 5);
  assert(strcmp(mid, "World") == 0);

  char *clamped = string_substring(arena, src, 7, 100);
  assert(strcmp(clamped, "World!") == 0);

  char *empty = string_substring(arena, src, 100, 10);
  assert(strcmp(empty, "") == 0);

  teardown_suite();
  printf("  test_cstring_substring PASSED\n");
}

static void test_cstring_format(void) {
  printf("  Running test_cstring_format...\n");
  char buf[32];
  int r = string_format(buf, sizeof(buf), "%s %d", "Hello", 42);
  assert(strcmp(buf, "Hello 42") == 0);
  assert(r == 8);

  char small[6]; // Can hold at most 5 chars + NUL
  int r2 = string_format(small, sizeof(small), "%s", "abcdefg");
  assert(r2 == 7); // would-have-written length
  assert(strcmp(small, "abcde") == 0);
  printf("  test_cstring_format PASSED\n");
}

static void test_cstring_format_v(void) {
  printf("  Running test_cstring_format_v...\n");
  char buf[32];
  int r = helper_string_format_v(buf, sizeof(buf), "%s %d", "World", 7);
  assert(strcmp(buf, "World 7") == 0);
  assert(r == 7);
  printf("  test_cstring_format_v PASSED\n");
}

static void test_cstring_empty(void) {
  printf("  Running test_cstring_empty...\n");
  char buf[8] = "abc";
  char *ret = string_empty(buf);
  assert(ret == buf);
  assert(buf[0] == '\0');
  printf("  test_cstring_empty PASSED\n");
}

static void test_cstring_copy(void) {
  printf("  Running test_cstring_copy...\n");
  char buf[16];
  char *ret = string_copy(buf, "copy");
  assert(ret == buf);
  assert(strcmp(buf, "copy") == 0);
  printf("  test_cstring_copy PASSED\n");
}

static void test_cstring_ncopy(void) {
  printf("  Running test_cstring_ncopy...\n");
  char buf[8];
  memset(buf, 'X', sizeof(buf));
  char *ret = string_ncopy(buf, "abcdef", 3);
  (void)ret;
  assert(buf[0] == 'a' && buf[1] == 'b' && buf[2] == 'c');
  // strncpy does not guarantee NUL-termination when truncated
  printf("  test_cstring_ncopy PASSED\n");
}

static void test_cstring_trim(void) {
  printf("  Running test_cstring_trim...\n");
  char buf[32] = "  \t  hello  \n  ";
  char *trimmed = string_trim(buf);
  assert(strcmp(trimmed, "hello") == 0);
  printf("  test_cstring_trim PASSED\n");
}

static void test_cstring_mid(void) {
  printf("  Running test_cstring_mid...\n");
  char dest[32];
  string_mid(dest, "Hello, World!", 7, 5);
  assert(strcmp(dest, "World") == 0);

  char dest2[32];
  string_mid(dest2, "Hello, World!", 7, -1);
  assert(strcmp(dest2, "World!") == 0);

  char dest3[8];
  memset(dest3, 'Z', sizeof(dest3));
  string_mid(dest3, "Hello", 1, 0);
  // length == 0 should not modify dest3
  assert(dest3[0] == 'Z');
  printf("  test_cstring_mid PASSED\n");
}

static void test_cstring_index_of(void) {
  printf("  Running test_cstring_index_of...\n");
  assert(string_index_of("Hello, World!", 'W') == 7);
  assert(string_index_of("Hello", 'z') == -1);
  assert(string_index_of(NULL, 'a') == -1);
  printf("  test_cstring_index_of PASSED\n");
}

bool32_t run_string_tests(void) {
  printf("--- Starting String Tests ---\n");

  test_str8_create();
  test_str8_create_literal();
  test_str8_create_formatted();
  test_str8_create_formatted_v();
  test_str8_cstr();
  test_str8_concat();
  test_str8_substring();
  test_str8_contains();
  test_str8_contains_cstr();
  test_str8_equals();
  test_str8_destroy();

  // CString tests
  test_cstring_equals();
  test_cstring_equalsi();
  test_cstring_length();
  test_cstring_duplicate();
  test_cstring_contains();
  test_cstring_substring();
  test_cstring_format();
  test_cstring_format_v();
  test_cstring_empty();
  test_cstring_copy();
  test_cstring_ncopy();
  test_cstring_trim();
  test_cstring_mid();
  test_cstring_index_of();

  printf("--- String Tests Completed ---\n");
  return true;
}
