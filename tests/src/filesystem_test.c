#include "filesystem_test.h"

#include "containers/str.h"
#include "defines.h"
#include "filesystem/filesystem.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

vkr_global const char *FS_TEST_RELATIVE_DIR = "tests/tmp/fs_tests";
vkr_global uint32_t g_fs_test_counter = 0;

vkr_internal bool8_t fs_test_make_dir(const char *path) {
  if (!path || path[0] == '\0') {
    return false_v;
  }
#if defined(_WIN32)
  int result = _mkdir(path);
#else
  int result = mkdir(path, 0755);
#endif
  if (result == 0) {
    return true_v;
  }
  if (errno == EEXIST) {
    return true_v;
  }
  return false_v;
}

vkr_internal void fs_test_ensure_base_dir(void) {
  char tmp_dir[1024];
  snprintf(tmp_dir, sizeof(tmp_dir), "%stests/tmp", PROJECT_SOURCE_DIR);
  assert(fs_test_make_dir(tmp_dir) == true_v &&
         "failed to create tmp test dir");

  char fs_dir[1024];
  snprintf(fs_dir, sizeof(fs_dir), "%s/fs_tests", tmp_dir);
  assert(fs_test_make_dir(fs_dir) == true_v &&
         "failed to create filesystem test dir");
}

vkr_internal void fs_test_remove_file(const char *path) {
  if (!path || path[0] == '\0') {
    return;
  }
#if defined(_WIN32)
  _unlink(path);
#else
  unlink(path);
#endif
}

vkr_internal void fs_test_remove_dir(const char *path) {
  if (!path || path[0] == '\0') {
    return;
  }
#if defined(_WIN32)
  _rmdir(path);
#else
  rmdir(path);
#endif
}

vkr_internal void test_file_path_create(void) {
  printf("  Running test_file_path_create...\n");
  Arena *arena = arena_create(MB(1), MB(1));

  FilePath relative =
      file_path_create("tests/src/test_main.c", arena, FILE_PATH_TYPE_RELATIVE);
  String8 relative_expected = string8_create_formatted(
      arena, "%s%s", PROJECT_SOURCE_DIR, "tests/src/test_main.c");
  assert(relative.type == FILE_PATH_TYPE_RELATIVE);
  assert(strcmp((const char *)relative.path.str,
                (const char *)relative_expected.str) == 0);

  String8 absolute_input = string8_create_formatted(
      arena, "%s%s/absolute_target_%u.bin", PROJECT_SOURCE_DIR,
      FS_TEST_RELATIVE_DIR, ++g_fs_test_counter);
  FilePath absolute = file_path_create((const char *)absolute_input.str, arena,
                                       FILE_PATH_TYPE_ABSOLUTE);
  assert(absolute.type == FILE_PATH_TYPE_ABSOLUTE);
  assert(strcmp((const char *)absolute.path.str,
                (const char *)absolute_input.str) == 0);

  arena_destroy(arena);
  printf("  test_file_path_create PASSED\n");
}

vkr_internal void test_file_exists_and_stats(void) {
  printf("  Running test_file_exists_and_stats...\n");
  Arena *arena = arena_create(MB(1), MB(1));

  FilePath existing =
      file_path_create("tests/src/test_main.c", arena, FILE_PATH_TYPE_RELATIVE);
  assert(file_exists(&existing) == true_v);
  FileStats stats = {0};
  assert(file_stats(&existing, &stats) == FILE_ERROR_NONE);
  assert(stats.size > 0);

  uint32_t id = ++g_fs_test_counter;
  char missing_relative[256];
  snprintf(missing_relative, sizeof(missing_relative), "%s/missing_%u.txt",
           FS_TEST_RELATIVE_DIR, id);
  FilePath missing =
      file_path_create(missing_relative, arena, FILE_PATH_TYPE_RELATIVE);
  fs_test_remove_file((const char *)missing.path.str);
  assert(file_exists(&missing) == false_v);
  assert(file_stats(&missing, &stats) == FILE_ERROR_NOT_FOUND);

  arena_destroy(arena);
  printf("  test_file_exists_and_stats PASSED\n");
}

vkr_internal void test_file_create_and_ensure_directory(void) {
  printf("  Running test_file_create_and_ensure_directory...\n");
  Arena *arena = arena_create(MB(1), MB(1));

  uint32_t id = ++g_fs_test_counter;
  String8 create_target =
      string8_create_formatted(arena, "%s%s/create_dir_%u", PROJECT_SOURCE_DIR,
                               FS_TEST_RELATIVE_DIR, id);
  FilePath create_path = {.path = create_target,
                          .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory(&create_path) == true_v);
  assert(file_create_directory(&create_path) == true_v);
  fs_test_remove_dir((const char *)create_target.str);

  id = ++g_fs_test_counter;
  String8 ensure_deep =
      string8_create_formatted(arena, "%s%s/ensure_dir_%u/inner/deeper",
                               PROJECT_SOURCE_DIR, FS_TEST_RELATIVE_DIR, id);
  bool8_t ensured = file_ensure_directory(arena, &ensure_deep);
  assert(ensured == true_v);

  String8 ensure_inner =
      string8_create_formatted(arena, "%s%s/ensure_dir_%u/inner",
                               PROJECT_SOURCE_DIR, FS_TEST_RELATIVE_DIR, id);
  String8 ensure_root =
      string8_create_formatted(arena, "%s%s/ensure_dir_%u", PROJECT_SOURCE_DIR,
                               FS_TEST_RELATIVE_DIR, id);

  fs_test_remove_dir((const char *)ensure_deep.str);
  fs_test_remove_dir((const char *)ensure_inner.str);
  fs_test_remove_dir((const char *)ensure_root.str);

  arena_destroy(arena);
  printf("  test_file_create_and_ensure_directory PASSED\n");
}

vkr_internal void test_file_write_and_read_binary(void) {
  printf("  Running test_file_write_and_read_binary...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  Arena *read_arena = arena_create(MB(1), MB(1));

  uint32_t id = ++g_fs_test_counter;
  char relative_path[256];
  snprintf(relative_path, sizeof(relative_path), "%s/io_binary_%u.bin",
           FS_TEST_RELATIVE_DIR, id);
  FilePath path =
      file_path_create(relative_path, arena, FILE_PATH_TYPE_RELATIVE);

  FileMode write_mode = bitset8_create();
  bitset8_set(&write_mode, FILE_MODE_WRITE);
  bitset8_set(&write_mode, FILE_MODE_BINARY);
  bitset8_set(&write_mode, FILE_MODE_TRUNCATE);

  FileHandle handle = {0};
  assert(file_open(&path, write_mode, &handle) == FILE_ERROR_NONE);

  uint8_t data[] = {0, 1, 2, 3, 4, 5, 6, 7};
  uint64_t bytes_written = 0;
  assert(file_write(&handle, sizeof(data), data, &bytes_written) ==
         FILE_ERROR_NONE);
  assert(bytes_written == sizeof(data));
  file_close(&handle);

  FileMode read_mode = bitset8_create();
  bitset8_set(&read_mode, FILE_MODE_READ);
  bitset8_set(&read_mode, FILE_MODE_BINARY);
  assert(file_open(&path, read_mode, &handle) == FILE_ERROR_NONE);

  {
    Scratch scratch = scratch_create(read_arena);
    uint8_t *buffer = NULL;
    uint64_t bytes_read = 0;
    assert(file_read_all(&handle, scratch.arena, &buffer, &bytes_read) ==
           FILE_ERROR_NONE);
    assert(bytes_read == sizeof(data));
    assert(memcmp(buffer, data, sizeof(data)) == 0);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_UNKNOWN);
  }
  file_close(&handle);

  assert(file_open(&path, read_mode, &handle) == FILE_ERROR_NONE);
  {
    Scratch scratch = scratch_create(read_arena);
    uint8_t *partial_buffer = NULL;
    uint64_t partial_read = 0;
    assert(file_read(&handle, scratch.arena, 3, &partial_read,
                     &partial_buffer) == FILE_ERROR_NONE);
    assert(partial_read == 3);
    assert(memcmp(partial_buffer, data, 3) == 0);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_UNKNOWN);
  }
  file_close(&handle);

  fs_test_remove_file((const char *)path.path.str);
  arena_destroy(read_arena);
  arena_destroy(arena);
  printf("  test_file_write_and_read_binary PASSED\n");
}

vkr_internal void test_file_read_line_and_write_line(void) {
  printf("  Running test_file_read_line_and_write_line...\n");
  Arena *arena = arena_create(MB(1), MB(1));

  uint32_t id = ++g_fs_test_counter;
  char relative_path[256];
  snprintf(relative_path, sizeof(relative_path), "%s/text_lines_%u.txt",
           FS_TEST_RELATIVE_DIR, id);
  FilePath path =
      file_path_create(relative_path, arena, FILE_PATH_TYPE_RELATIVE);

  FileMode write_mode = bitset8_create();
  bitset8_set(&write_mode, FILE_MODE_WRITE);
  bitset8_set(&write_mode, FILE_MODE_TRUNCATE);
  FileHandle handle = {0};
  assert(file_open(&path, write_mode, &handle) == FILE_ERROR_NONE);

  String8 lines[] = {string8_lit("alpha"), string8_lit("beta"),
                     string8_lit("gamma")};
  for (uint32_t i = 0; i < ArrayCount(lines); ++i) {
    assert(file_write_line(&handle, &lines[i]) == FILE_ERROR_NONE);
  }
  file_close(&handle);

  FileMode read_mode = bitset8_create();
  bitset8_set(&read_mode, FILE_MODE_READ);
  assert(file_open(&path, read_mode, &handle) == FILE_ERROR_NONE);

  Arena *line_arena = arena_create(MB(1), MB(1));
  String8 line = {0};
  assert(file_read_line(&handle, line_arena, line_arena, 64, &line) ==
         FILE_ERROR_NONE);
  assert(strcmp((const char *)line.str, "alpha\n") == 0);

  Arena *another_arena = arena_create(MB(1), MB(1));
  assert(file_read_line(&handle, another_arena, line_arena, 64, &line) ==
         FILE_ERROR_NONE);
  assert(strcmp((const char *)line.str, "beta\n") == 0);

  assert(file_read_line(&handle, line_arena, another_arena, 64, &line) ==
         FILE_ERROR_NONE);
  assert(strcmp((const char *)line.str, "gamma\n") == 0);

  assert(file_read_line(&handle, line_arena, another_arena, 64, &line) ==
             FILE_ERROR_EOF &&
         "Expected EOF after last line");

  arena_destroy(line_arena);
  arena_destroy(another_arena);
  file_close(&handle);

  assert(file_open(&path, read_mode, &handle) == FILE_ERROR_NONE);
  String8 file_contents = {0};
  assert(file_read_string(&handle, arena, &file_contents) == FILE_ERROR_NONE);
  assert(strcmp((const char *)file_contents.str, "alpha\nbeta\ngamma\n") == 0);
  file_close(&handle);

  fs_test_remove_file((const char *)path.path.str);
  arena_destroy(arena);
  printf("  test_file_read_line_and_write_line PASSED\n");
}

vkr_internal void test_file_load_spirv_shader(void) {
  printf("  Running test_file_load_spirv_shader...\n");
  Arena *arena = arena_create(MB(1), MB(1));

  uint32_t id = ++g_fs_test_counter;
  char relative_path[256];
  snprintf(relative_path, sizeof(relative_path), "%s/spirv_shader_%u.spv",
           FS_TEST_RELATIVE_DIR, id);
  FilePath path =
      file_path_create(relative_path, arena, FILE_PATH_TYPE_RELATIVE);

  FileMode write_mode = bitset8_create();
  bitset8_set(&write_mode, FILE_MODE_WRITE);
  bitset8_set(&write_mode, FILE_MODE_BINARY);
  bitset8_set(&write_mode, FILE_MODE_TRUNCATE);
  FileHandle handle = {0};
  assert(file_open(&path, write_mode, &handle) == FILE_ERROR_NONE);

  const uint32_t spirv_words[] = {0x07230203, 0x00010000, 0x0000000B,
                                  0x00000000};
  uint64_t bytes_written = 0;
  assert(file_write(&handle, sizeof(spirv_words), (const uint8_t *)spirv_words,
                    &bytes_written) == FILE_ERROR_NONE);
  assert(bytes_written == sizeof(spirv_words));
  file_close(&handle);

  Arena *shader_arena = arena_create(MB(1), MB(1));
  uint8_t *shader_data = NULL;
  uint64_t shader_size = 0;
  assert(file_load_spirv_shader(&path, shader_arena, &shader_data,
                                &shader_size) == FILE_ERROR_NONE);
  assert(shader_size == sizeof(spirv_words));
  assert(((uint32_t *)shader_data)[0] == 0x07230203);

  fs_test_remove_file((const char *)path.path.str);
  arena_destroy(shader_arena);
  arena_destroy(arena);
  printf("  test_file_load_spirv_shader PASSED\n");
}

vkr_internal void test_file_path_helpers(void) {
  printf("  Running test_file_path_helpers...\n");
  Arena *arena = arena_create(MB(1), MB(1));

  String8 sample = string8_lit("/tmp/assets/output.bin");
  String8 dir = file_path_get_directory(arena, sample);
  assert(dir.length == string_length("/tmp/assets/"));
  assert(strncmp((const char *)dir.str, "/tmp/assets/",
                 string_length("/tmp/assets/")) == 0);

  String8 filename = string8_lit("shader.spv");
  String8 joined = file_path_join(arena, dir, filename);
  assert(strcmp((const char *)joined.str, "/tmp/assets/shader.spv") == 0);

  arena_destroy(arena);
  printf("  test_file_path_helpers PASSED\n");
}

vkr_internal void test_file_get_error_strings(void) {
  printf("  Running test_file_get_error_strings...\n");
  String8 err = file_get_error_string(FILE_ERROR_NOT_FOUND);
  assert(strcmp((const char *)err.str, "File not found") == 0);
  err = file_get_error_string(FILE_ERROR_INVALID_HANDLE);
  assert(strcmp((const char *)err.str, "Invalid handle") == 0);
  err = file_get_error_string(FILE_ERROR_IO_ERROR);
  assert(strcmp((const char *)err.str, "I/O error") == 0);
  printf("  test_file_get_error_strings PASSED\n");
}

bool32_t run_filesystem_tests(void) {
  printf("--- Starting Filesystem Tests ---\n");
  g_fs_test_counter = 0;
  fs_test_ensure_base_dir();

  test_file_path_create();
  test_file_exists_and_stats();
  test_file_create_and_ensure_directory();
  test_file_write_and_read_binary();
  test_file_read_line_and_write_line();
  test_file_load_spirv_shader();
  test_file_path_helpers();
  test_file_get_error_strings();

  printf("--- Filesystem Tests Completed ---\n");
  return true;
}
