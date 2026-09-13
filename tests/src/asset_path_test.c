#include "../../editor/src/editor_project_store.h"
#include "core/vkr_json.h"
#include "filesystem/vkr_asset_path.h"
#include "memory/vkr_arena_allocator.h"
#include <assert.h>
#include <stdio.h>

bool32_t run_asset_path_tests(void);

static String8 path_test_string(const char *value) {
  return (String8){.str = (uint8_t *)value, .length = strlen(value)};
}

static void path_test_corpus(VkrAllocator *allocator, const char *filename,
                             bool8_t managed) {
  String8 bytes = {0};
  uint64_t fingerprint = 0;
  assert(vkr_editor_project_json_read_file(filename, allocator, &bytes,
                                           &fingerprint, NULL));
  VkrJsonReader reader = vkr_json_reader_from_string(bytes);
  assert(vkr_json_find_array(&reader, "cases"));
  uint32_t count = 0;
  while (vkr_json_next_array_element(&reader)) {
    VkrJsonReader object;
    assert(vkr_json_enter_object(&reader, &object));
    String8 record = {.str = (uint8_t *)object.data, .length = object.length};
    char value[1024];
    if (managed) {
      bool8_t expected;
      assert(vkr_json_find_field(&object, "valid"));
      assert(vkr_json_parse_bool(&object, &expected));
      bool8_t decoded = vkr_editor_project_json_string(record, "value", value,
                                                       sizeof(value), NULL);
      assert((decoded && vkr_asset_path_managed_valid(
                             path_test_string(value))) == expected);
    } else {
      char platform[32];
      if (vkr_editor_project_json_string(record, "platform", platform,
                                         sizeof(platform), NULL)) {
#if defined(PLATFORM_WINDOWS)
        if (strcmp(platform, "windows")) {
#else
        if (strcmp(platform, "posix")) {
#endif
          continue;
        }
      }
      char owner[1024];
      char expected[1024];
      assert(vkr_editor_project_json_string(record, "owner", owner,
                                            sizeof(owner), NULL));
      assert(vkr_editor_project_json_string(record, "reference", value,
                                            sizeof(value), NULL));
      bool8_t valid = vkr_editor_project_json_string(
          record, "expected", expected, sizeof(expected), NULL);
      String8 actual = vkr_asset_path_resolve(
          allocator, path_test_string(owner), path_test_string(value));
      if (valid) {
        assert(actual.str && strcmp((char *)actual.str, expected) == 0);
      } else {
        assert(!actual.str);
      }
    }
    ++count;
  }
  assert(count >= 7);
}

bool32_t run_asset_path_tests(void) {
  printf("--- Starting Asset Path Tests ---\n");
  Arena *arena = arena_create(MB(4), MB(4));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  path_test_corpus(&allocator,
                   PROJECT_SOURCE_DIR "tests/fixtures/paths/managed.json",
                   true_v);
  path_test_corpus(&allocator,
                   PROJECT_SOURCE_DIR "tests/fixtures/paths/native.json",
                   false_v);
  uint8_t embedded_nul[] = {'a', 0, 'b'};
  assert(!vkr_asset_path_managed_valid(
      (String8){.str = embedded_nul, .length = sizeof(embedded_nul)}));
  uint8_t boundary[32769];
  MemSet(boundary, 'a', sizeof(boundary));
  String8 limit = {.str = boundary, .length = 32767};
  String8 actual = vkr_asset_path_resolve(&allocator, (String8){0}, limit);
  assert(actual.str && actual.length == limit.length);
  limit.length = 32768;
  assert(!vkr_asset_path_resolve(&allocator, (String8){0}, limit).str);
  boundary[0] = '.';
  boundary[1] = '/';
  limit.length = 32767;
  assert(!vkr_asset_path_resolve(&allocator, path_test_string("/owner.json"),
                                 limit)
              .str);
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  printf("--- Asset Path Tests Completed ---\n");
  return true_v;
}
