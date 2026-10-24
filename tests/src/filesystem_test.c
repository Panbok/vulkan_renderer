#include "filesystem_test.h"

#include "container_test_allocator.h"

#include "containers/str.h"
#include "core/vkr_content_codec.h"
#include "core/vkr_hash.h"
#include "core/vkr_threads.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_vfs.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"

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

#include "ktx-software/external/basisu/zstd/zstd.h"

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
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);

  FilePath relative = file_path_create("tests/src/test_main.c", &allocator,
                                       FILE_PATH_TYPE_RELATIVE);
  String8 relative_expected = string8_create_formatted(
      &allocator, "%s%s", PROJECT_SOURCE_DIR, "tests/src/test_main.c");
  assert(relative.type == FILE_PATH_TYPE_RELATIVE);
  assert(strcmp((const char *)relative.path.str,
                (const char *)relative_expected.str) == 0);

  String8 absolute_input = string8_create_formatted(
      &allocator, "%s%s/absolute_target_%u.bin", PROJECT_SOURCE_DIR,
      FS_TEST_RELATIVE_DIR, ++g_fs_test_counter);
  FilePath absolute = file_path_create((const char *)absolute_input.str,
                                       &allocator, FILE_PATH_TYPE_ABSOLUTE);
  assert(absolute.type == FILE_PATH_TYPE_ABSOLUTE);
  assert(strcmp((const char *)absolute.path.str,
                (const char *)absolute_input.str) == 0);

  arena_destroy(arena);
  printf("  test_file_path_create PASSED\n");
}

vkr_internal void test_file_exists_and_stats(void) {
  printf("  Running test_file_exists_and_stats...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);

  FilePath existing = file_path_create("tests/src/test_main.c", &allocator,
                                       FILE_PATH_TYPE_RELATIVE);
  assert(file_exists(&existing) == true_v);
  FileStats stats = {0};
  assert(file_stats(&existing, &stats) == FILE_ERROR_NONE);
  assert(stats.size > 0);

  uint32_t id = ++g_fs_test_counter;
  char missing_relative[256];
  snprintf(missing_relative, sizeof(missing_relative), "%s/missing_%u.txt",
           FS_TEST_RELATIVE_DIR, id);
  FilePath missing =
      file_path_create(missing_relative, &allocator, FILE_PATH_TYPE_RELATIVE);
  fs_test_remove_file((const char *)missing.path.str);
  assert(file_exists(&missing) == false_v);
  assert(file_stats(&missing, &stats) == FILE_ERROR_NOT_FOUND);

  arena_destroy(arena);
  printf("  test_file_exists_and_stats PASSED\n");
}

vkr_internal void test_file_create_and_ensure_directory(void) {
  printf("  Running test_file_create_and_ensure_directory...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);

  uint32_t id = ++g_fs_test_counter;
  String8 create_target =
      string8_create_formatted(&allocator, "%s%s/create_dir_%u",
                               PROJECT_SOURCE_DIR, FS_TEST_RELATIVE_DIR, id);
  FilePath create_path = {.path = create_target,
                          .type = FILE_PATH_TYPE_ABSOLUTE};
  FilePath regular_file = file_path_create("tests/src/test_main.c", &allocator,
                                           FILE_PATH_TYPE_RELATIVE);
  assert(!file_create_directory(&regular_file));
  assert(file_create_directory(&create_path) == true_v);
  assert(file_create_directory(&create_path) == true_v);
  fs_test_remove_dir((const char *)create_target.str);

  id = ++g_fs_test_counter;
  String8 ensure_deep =
      string8_create_formatted(&allocator, "%s%s/ensure_dir_%u/inner/deeper",
                               PROJECT_SOURCE_DIR, FS_TEST_RELATIVE_DIR, id);
  bool8_t ensured = file_ensure_directory(&allocator, &ensure_deep);
  assert(ensured == true_v);

  String8 ensure_inner =
      string8_create_formatted(&allocator, "%s%s/ensure_dir_%u/inner",
                               PROJECT_SOURCE_DIR, FS_TEST_RELATIVE_DIR, id);
  String8 ensure_root =
      string8_create_formatted(&allocator, "%s%s/ensure_dir_%u",
                               PROJECT_SOURCE_DIR, FS_TEST_RELATIVE_DIR, id);

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
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  VkrAllocator read_allocator = {.ctx = read_arena};
  vkr_allocator_arena(&read_allocator);

  uint32_t id = ++g_fs_test_counter;
  char relative_path[256];
  snprintf(relative_path, sizeof(relative_path), "%s/io_binary_%u.bin",
           FS_TEST_RELATIVE_DIR, id);
  FilePath path =
      file_path_create(relative_path, &allocator, FILE_PATH_TYPE_RELATIVE);

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
    VkrAllocator temp_alloc = {.ctx = read_arena};
    vkr_allocator_arena(&temp_alloc);
    VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(&temp_alloc);
    uint8_t *buffer = NULL;
    uint64_t bytes_read = 0;
    assert(file_read_all(&handle, &temp_alloc, &buffer, &bytes_read) ==
           FILE_ERROR_NONE);
    assert(bytes_read == sizeof(data));
    assert(memcmp(buffer, data, sizeof(data)) == 0);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_UNKNOWN);
  }
  file_close(&handle);

  assert(file_open(&path, read_mode, &handle) == FILE_ERROR_NONE);
  {
    VkrAllocator temp_alloc = {.ctx = read_arena};
    vkr_allocator_arena(&temp_alloc);
    VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(&temp_alloc);
    uint8_t *partial_buffer = NULL;
    uint64_t partial_read = 0;
    assert(file_read(&handle, &temp_alloc, 3, &partial_read, &partial_buffer) ==
           FILE_ERROR_NONE);
    assert(partial_read == 3);
    assert(memcmp(partial_buffer, data, 3) == 0);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_UNKNOWN);
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
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);

  uint32_t id = ++g_fs_test_counter;
  char relative_path[256];
  snprintf(relative_path, sizeof(relative_path), "%s/text_lines_%u.txt",
           FS_TEST_RELATIVE_DIR, id);
  FilePath path =
      file_path_create(relative_path, &allocator, FILE_PATH_TYPE_RELATIVE);

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
  VkrAllocator line_allocator = {.ctx = line_arena};
  vkr_allocator_arena(&line_allocator);
  String8 line = {0};
  assert(file_read_line(&handle, &line_allocator, &line_allocator, 64, &line) ==
         FILE_ERROR_NONE);
  assert(strcmp((const char *)line.str, "alpha\n") == 0);

  Arena *another_arena = arena_create(MB(1), MB(1));
  VkrAllocator another_allocator = {.ctx = another_arena};
  vkr_allocator_arena(&another_allocator);
  assert(file_read_line(&handle, &another_allocator, &line_allocator, 64,
                        &line) == FILE_ERROR_NONE);
  assert(strcmp((const char *)line.str, "beta\n") == 0);

  assert(file_read_line(&handle, &line_allocator, &another_allocator, 64,
                        &line) == FILE_ERROR_NONE);
  assert(strcmp((const char *)line.str, "gamma\n") == 0);

  assert(file_read_line(&handle, &line_allocator, &another_allocator, 64,
                        &line) == FILE_ERROR_EOF &&
         "Expected EOF after last line");

  arena_destroy(line_arena);
  arena_destroy(another_arena);
  file_close(&handle);

  assert(file_open(&path, read_mode, &handle) == FILE_ERROR_NONE);
  String8 file_contents = {0};
  assert(file_read_string(&handle, &allocator, &file_contents) ==
         FILE_ERROR_NONE);
  assert(strcmp((const char *)file_contents.str, "alpha\nbeta\ngamma\n") == 0);
  file_close(&handle);

  fs_test_remove_file((const char *)path.path.str);
  arena_destroy(arena);
  printf("  test_file_read_line_and_write_line PASSED\n");
}

vkr_internal void test_file_load_spirv_shader(void) {
  printf("  Running test_file_load_spirv_shader...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);

  uint32_t id = ++g_fs_test_counter;
  char relative_path[256];
  snprintf(relative_path, sizeof(relative_path), "%s/spirv_shader_%u.spv",
           FS_TEST_RELATIVE_DIR, id);
  FilePath path =
      file_path_create(relative_path, &allocator, FILE_PATH_TYPE_RELATIVE);

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
  VkrAllocator shader_allocator = {.ctx = shader_arena};
  vkr_allocator_arena(&shader_allocator);
  uint8_t *shader_data = NULL;
  uint64_t shader_size = 0;
  assert(file_load_spirv_shader(&path, &shader_allocator, &shader_data,
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
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);

  String8 sample = string8_lit("/tmp/assets/output.bin");
  String8 dir = file_path_get_directory(&allocator, sample);
  assert(dir.length == string_length("/tmp/assets/"));
  assert(strncmp((const char *)dir.str, "/tmp/assets/",
                 string_length("/tmp/assets/")) == 0);

  String8 filename = string8_lit("shader.spv");
  String8 joined = file_path_join(&allocator, dir, filename);
  assert(strcmp((const char *)joined.str, "/tmp/assets/shader.spv") == 0);

  const String8 name = file_path_get_name(sample);
  assert(name.length == string_length("output.bin") &&
         MemCompare(name.str, "output.bin", name.length) == 0);
  assert(file_path_get_name(filename).length == filename.length);
  assert(file_path_get_name(string8_lit("/tmp/assets/")).length == 0);

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
  err = file_get_error_string(FILE_ERROR_UNSUPPORTED);
  assert(strcmp((const char *)err.str, "Not supported by this filesystem") ==
         0);
  printf("  test_file_get_error_strings PASSED\n");
}

/* Detects: a missing parent left uncreated, an existing tree reported as a
   failure, a file in the way accepted as a directory, and a trailing
   separator that stops the walk. */
vkr_internal void test_file_create_directories(void) {
  printf("  Running test_file_create_directories...\n");
  const uint32_t id = ++g_fs_test_counter;
  char root[512];
  char leaf[600];
  char trailing[600];
  char blocked[640];
  snprintf(root, sizeof(root), "%s%s/tree_%u", PROJECT_SOURCE_DIR,
           FS_TEST_RELATIVE_DIR, id);
  snprintf(leaf, sizeof(leaf), "%s/a/b/c", root);
  snprintf(trailing, sizeof(trailing), "%s/d/", root);
  const FilePath leaf_path = file_path_view(leaf);
  assert(file_create_directories(&leaf_path));
  assert(file_exists(&leaf_path));
  assert(file_create_directories(&leaf_path));
  const FilePath trailing_path = file_path_view(trailing);
  assert(file_create_directories(&trailing_path));

  /* A file named like a parent stops the walk. */
  char file_text[640];
  snprintf(file_text, sizeof(file_text), "%s/file", root);
  const FilePath file_path = file_path_view(file_text);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  FileHandle handle = {0};
  assert(file_open(&file_path, mode, &handle) == FILE_ERROR_NONE);
  file_close(&handle);
  snprintf(blocked, sizeof(blocked), "%s/file/below", root);
  const FilePath blocked_path = file_path_view(blocked);
  assert(!file_create_directories(&blocked_path));
  assert(!file_exists(&blocked_path));

  assert(file_remove(&file_path) == FILE_ERROR_NONE);
  const char *const remove_order[] = {"a/b/c", "a/b", "a", "d", ""};
  for (uint32_t i = 0u; i < sizeof(remove_order) / sizeof(remove_order[0]);
       ++i) {
    char path[640];
    snprintf(path, sizeof(path), "%s%s%s", root, remove_order[i][0] ? "/" : "",
             remove_order[i]);
    const FilePath directory = file_path_view(path);
    assert(file_remove_directory(&directory) == FILE_ERROR_NONE);
  }
  printf("  test_file_create_directories PASSED\n");
}

vkr_internal void test_file_clone(void) {
  printf("  Running test_file_clone...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  const uint32_t id = ++g_fs_test_counter;
  String8 directory_text =
      string8_create_formatted(&allocator, "%s%s/clone_%u", PROJECT_SOURCE_DIR,
                               FS_TEST_RELATIVE_DIR, id);
  FilePath directory = {.path = directory_text,
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory_exclusive(&directory) == FILE_ERROR_NONE);
  String8 source_text = string8_create_formatted(
      &allocator, "%s/source.bin", (const char *)directory_text.str);
  String8 clone_text = string8_create_formatted(
      &allocator, "%s/clone.bin", (const char *)directory_text.str);
  String8 missing_text = string8_create_formatted(
      &allocator, "%s/missing.bin", (const char *)directory_text.str);
  FilePath source = {.path = source_text, .type = FILE_PATH_TYPE_ABSOLUTE};
  FilePath clone = {.path = clone_text, .type = FILE_PATH_TYPE_ABSOLUTE};
  FilePath missing = {.path = missing_text, .type = FILE_PATH_TYPE_ABSOLUTE};

  FileMode write_mode = bitset8_create();
  bitset8_set(&write_mode, FILE_MODE_WRITE);
  bitset8_set(&write_mode, FILE_MODE_BINARY);
  bitset8_set(&write_mode, FILE_MODE_TRUNCATE);
  FileHandle file = {0};
  const uint8_t payload[] = {4u, 3u, 2u, 1u};
  uint64_t transferred = 0u;
  assert(file_open(&source, write_mode, &file) == FILE_ERROR_NONE);
  assert(file_write(&file, sizeof(payload), payload, &transferred) ==
         FILE_ERROR_NONE);
  file_close(&file);
#if !defined(_WIN32)
  // A read-only source must still yield an owner-writable clone.
  assert(chmod((const char *)source_text.str, 0444) == 0);
#endif

  const FileError cloned = file_clone(&source, &clone);
  if (cloned == FILE_ERROR_UNSUPPORTED) {
    // Callers copy on file systems without cloning; nothing was created.
    assert(!file_exists(&clone));
    assert(file_clone(&missing, &clone) == FILE_ERROR_NOT_FOUND);
  } else {
    assert(cloned == FILE_ERROR_NONE);
    FileMode read_mode = bitset8_create();
    bitset8_set(&read_mode, FILE_MODE_READ);
    bitset8_set(&read_mode, FILE_MODE_BINARY);
    uint8_t received[sizeof(payload)] = {0};
    assert(file_open(&clone, read_mode, &file) == FILE_ERROR_NONE);
    assert(file_read_into(&file, received, sizeof(received), &transferred) ==
           FILE_ERROR_NONE);
    file_close(&file);
    assert(transferred == sizeof(payload) &&
           MemCompare(received, payload, sizeof(payload)) == 0);
    // Writing the clone leaves the source bytes unchanged.
    const uint8_t replacement[] = {7u};
    assert(file_open(&clone, write_mode, &file) == FILE_ERROR_NONE);
    assert(file_write(&file, sizeof(replacement), replacement, &transferred) ==
           FILE_ERROR_NONE);
    file_close(&file);
    assert(file_open(&source, read_mode, &file) == FILE_ERROR_NONE);
    assert(file_read_into(&file, received, sizeof(received), &transferred) ==
           FILE_ERROR_NONE);
    file_close(&file);
    assert(MemCompare(received, payload, sizeof(payload)) == 0);
    assert(file_clone(&source, &clone) == FILE_ERROR_ALREADY_EXISTS);
    assert(file_clone(&missing, &missing) == FILE_ERROR_NOT_FOUND);
    assert(file_remove(&clone) == FILE_ERROR_NONE);
  }

#if !defined(_WIN32)
  assert(chmod((const char *)source_text.str, 0644) == 0);
#endif
  assert(file_remove(&source) == FILE_ERROR_NONE);
  fs_test_remove_dir((const char *)directory_text.str);
  arena_destroy(arena);
  printf("  test_file_clone PASSED\n");
}

vkr_internal void fs_test_expect_bytes(const FilePath *path,
                                       const uint8_t *expected, uint64_t size) {
  FileMode read_mode = bitset8_create();
  bitset8_set(&read_mode, FILE_MODE_READ);
  bitset8_set(&read_mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  uint8_t received[16] = {0};
  uint64_t transferred = 0u;
  assert(size <= sizeof(received));
  assert(file_open(path, read_mode, &file) == FILE_ERROR_NONE);
  assert(file_read_into(&file, received, sizeof(received), &transferred) ==
         FILE_ERROR_NONE);
  file_close(&file);
  assert(transferred == size && MemCompare(received, expected, size) == 0);
}

/* Workspaces hard link files that are only ever replaced by rename: a
 * replacement publishes a new file under that name and leaves every other
 * name of the old one holding the old bytes. */
vkr_internal void test_file_link(void) {
  printf("  Running test_file_link...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  const uint32_t id = ++g_fs_test_counter;
  String8 directory_text = string8_create_formatted(
      &allocator, "%s%s/link_%u", PROJECT_SOURCE_DIR, FS_TEST_RELATIVE_DIR, id);
  FilePath directory = {.path = directory_text,
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory_exclusive(&directory) == FILE_ERROR_NONE);
  const char *root = (const char *)directory_text.str;
  FilePath source = {
      .path = string8_create_formatted(&allocator, "%s/source.bin", root),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  FilePath linked = {
      .path = string8_create_formatted(&allocator, "%s/linked.bin", root),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  FilePath staged = {
      .path = string8_create_formatted(&allocator, "%s/staged.bin", root),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  FilePath missing = {
      .path = string8_create_formatted(&allocator, "%s/missing.bin", root),
      .type = FILE_PATH_TYPE_ABSOLUTE};

  FileMode write_mode = bitset8_create();
  bitset8_set(&write_mode, FILE_MODE_WRITE);
  bitset8_set(&write_mode, FILE_MODE_BINARY);
  bitset8_set(&write_mode, FILE_MODE_TRUNCATE);
  FileHandle file = {0};
  const uint8_t payload[] = {4u, 3u, 2u, 1u};
  const uint8_t replacement[] = {9u, 8u};
  uint64_t transferred = 0u;
  assert(file_open(&source, write_mode, &file) == FILE_ERROR_NONE);
  assert(file_write(&file, sizeof(payload), payload, &transferred) ==
         FILE_ERROR_NONE);
  file_close(&file);

  const FileError linked_result = file_link(&source, &linked);
  if (linked_result == FILE_ERROR_UNSUPPORTED) {
    // Callers copy on file systems without links; nothing was created.
    assert(!file_exists(&linked));
  } else {
    assert(linked_result == FILE_ERROR_NONE);
    fs_test_expect_bytes(&linked, payload, sizeof(payload));
    assert(file_link(&source, &linked) == FILE_ERROR_ALREADY_EXISTS);
    assert(file_link(&missing, &staged) == FILE_ERROR_NOT_FOUND);
    assert(!file_exists(&staged));

    assert(file_open(&staged, write_mode, &file) == FILE_ERROR_NONE);
    assert(file_write(&file, sizeof(replacement), replacement, &transferred) ==
           FILE_ERROR_NONE);
    file_close(&file);
    assert(file_rename(&staged, &source, true_v) == FILE_ERROR_NONE);
    fs_test_expect_bytes(&source, replacement, sizeof(replacement));
    fs_test_expect_bytes(&linked, payload, sizeof(payload));
    assert(file_remove(&linked) == FILE_ERROR_NONE);
  }

  assert(file_remove(&source) == FILE_ERROR_NONE);
  fs_test_remove_dir(root);
  arena_destroy(arena);
  printf("  test_file_link PASSED\n");
}

typedef struct FsTestHeldReader {
  FILE *file;
} FsTestHeldReader;

vkr_internal void *fs_test_release_reader(void *argument) {
  FsTestHeldReader *reader = (FsTestHeldReader *)argument;
  vkr_thread_sleep(100u);
  fclose(reader->file);
  return NULL;
}

/* A document another process is polling stays replaceable: on Windows the
   CRT reader shares no delete access, so the atomic replace must outlast a
   brief read instead of failing at once. */
vkr_internal void test_file_rename_waits_for_brief_reader(void) {
  printf("  Running test_file_rename_waits_for_brief_reader...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  const uint32_t id = ++g_fs_test_counter;
  String8 directory_text =
      string8_create_formatted(&allocator, "%s%s/reader_%u", PROJECT_SOURCE_DIR,
                               FS_TEST_RELATIVE_DIR, id);
  FilePath directory = {.path = directory_text,
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory_exclusive(&directory) == FILE_ERROR_NONE);
  const char *root = (const char *)directory_text.str;
  FilePath document = {
      .path = string8_create_formatted(&allocator, "%s/progress.json", root),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  FilePath staged = {
      .path = string8_create_formatted(&allocator, "%s/progress.tmp", root),
      .type = FILE_PATH_TYPE_ABSOLUTE};

  FileMode write_mode = bitset8_create();
  bitset8_set(&write_mode, FILE_MODE_WRITE);
  bitset8_set(&write_mode, FILE_MODE_BINARY);
  bitset8_set(&write_mode, FILE_MODE_TRUNCATE);
  const uint8_t before[] = {'o', 'l', 'd'};
  const uint8_t after[] = {'n', 'e', 'w', '!'};
  FileHandle file = {0};
  uint64_t transferred = 0;
  assert(file_open(&document, write_mode, &file) == FILE_ERROR_NONE);
  assert(file_write(&file, sizeof(before), before, &transferred) ==
         FILE_ERROR_NONE);
  file_close(&file);
  assert(file_open(&staged, write_mode, &file) == FILE_ERROR_NONE);
  assert(file_write(&file, sizeof(after), after, &transferred) ==
         FILE_ERROR_NONE);
  file_close(&file);

  FsTestHeldReader reader = {
      .file = file_fopen((const char *)document.path.str, "rb")};
  assert(reader.file);
  VkrThread thread = NULL;
  assert(
      vkr_thread_create(&allocator, &thread, fs_test_release_reader, &reader));
  assert(file_rename(&staged, &document, true_v) == FILE_ERROR_NONE);
  assert(vkr_thread_join(thread));
  fs_test_expect_bytes(&document, after, sizeof(after));
  assert(!file_exists(&staged));

  assert(file_remove(&document) == FILE_ERROR_NONE);
  fs_test_remove_dir(root);
  arena_destroy(arena);
  printf("  test_file_rename_waits_for_brief_reader PASSED\n");
}

vkr_internal void test_file_portable_publication_primitives(void) {
  printf("  Running test_file_portable_publication_primitives...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  const uint32_t id = ++g_fs_test_counter;
  String8 directory_text =
      string8_create_formatted(&allocator, "%s%s/portable_%u",
                               PROJECT_SOURCE_DIR, FS_TEST_RELATIVE_DIR, id);
  FilePath directory = {.path = directory_text,
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory_exclusive(&directory) == FILE_ERROR_NONE);
  assert(file_create_directory_exclusive(&directory) ==
         FILE_ERROR_ALREADY_EXISTS);

  String8 source_text = string8_create_formatted(
      &allocator, "%s/source.bin", (const char *)directory_text.str);
  String8 destination_text = string8_create_formatted(
      &allocator, "%s/destination.bin", (const char *)directory_text.str);
  FilePath source = {.path = source_text, .type = FILE_PATH_TYPE_ABSOLUTE};
  FilePath destination = {.path = destination_text,
                          .type = FILE_PATH_TYPE_ABSOLUTE};
  FileMode write_mode = bitset8_create();
  bitset8_set(&write_mode, FILE_MODE_WRITE);
  bitset8_set(&write_mode, FILE_MODE_BINARY);
  bitset8_set(&write_mode, FILE_MODE_TRUNCATE);
  FileHandle file = {0};
  assert(file_open(&source, write_mode, &file) == FILE_ERROR_NONE);
  const uint8_t payload[] = {9u, 8u, 7u, 6u};
  uint64_t bytes_written = 0u;
  assert(file_write(&file, sizeof(payload), payload, &bytes_written) ==
         FILE_ERROR_NONE);
  assert(bytes_written == sizeof(payload));
  assert(file_sync(&file) == FILE_ERROR_NONE);
  file_close(&file);

  FileMode exclusive_mode = bitset8_create();
  bitset8_set(&exclusive_mode, FILE_MODE_WRITE);
  bitset8_set(&exclusive_mode, FILE_MODE_CREATE);
  bitset8_set(&exclusive_mode, FILE_MODE_EXCLUSIVE);
  assert(file_open(&source, exclusive_mode, &file) ==
         FILE_ERROR_ALREADY_EXISTS);
  assert(!file.handle);

  char resolved[1024];
  char resolved_directory[1024];
  assert(file_path_resolve(&source, resolved, sizeof(resolved)) ==
         FILE_ERROR_NONE);
  assert(file_path_resolve(&directory, resolved_directory,
                           sizeof(resolved_directory)) == FILE_ERROR_NONE);
  assert(file_path_equals(resolved, resolved));
  assert(file_path_starts_with(resolved, resolved_directory));
  // Without overwrite, an existing destination fails and both files stay.
  String8 occupied_text = string8_create_formatted(
      &allocator, "%s/occupied.bin", (const char *)directory_text.str);
  FilePath occupied = {.path = occupied_text, .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_open(&occupied, write_mode, &file) == FILE_ERROR_NONE);
  const uint8_t occupant[] = {1u};
  assert(file_write(&file, sizeof(occupant), occupant, &bytes_written) ==
         FILE_ERROR_NONE);
  file_close(&file);
  assert(file_rename(&source, &occupied, false_v) == FILE_ERROR_ALREADY_EXISTS);
  assert(file_exists(&source));
  FileStats occupied_stats = {0};
  assert(file_stats(&occupied, &occupied_stats) == FILE_ERROR_NONE);
  assert(occupied_stats.size == sizeof(occupant));
  assert(file_remove(&occupied) == FILE_ERROR_NONE);
  assert(file_rename(&occupied, &destination, false_v) == FILE_ERROR_NOT_FOUND);

  assert(file_rename(&source, &destination, false_v) == FILE_ERROR_NONE);
  assert(!file_exists(&source));
  assert(file_exists(&destination));

  FileMode read_mode = bitset8_create();
  bitset8_set(&read_mode, FILE_MODE_READ);
  bitset8_set(&read_mode, FILE_MODE_BINARY);
  assert(file_open(&destination, read_mode, &file) == FILE_ERROR_NONE);
  uint8_t received[sizeof(payload)] = {0};
  uint64_t bytes_read = 0u;
  assert(file_read_into(&file, received, sizeof(received), &bytes_read) ==
         FILE_ERROR_NONE);
  assert(bytes_read == sizeof(payload));
  assert(MemCompare(payload, received, sizeof(payload)) == 0);
  file_close(&file);

  assert(file_remove(&destination) == FILE_ERROR_NONE);
  assert(file_remove(&destination) == FILE_ERROR_NOT_FOUND);
  fs_test_remove_dir((const char *)directory.path.str);
  arena_destroy(arena);
  printf("  test_file_portable_publication_primitives PASSED\n");
}

static void test_file_io_failures_release_owned_outputs(void) {
  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator paths = {.ctx = arena};
  assert(vkr_allocator_arena(&paths));
  VkrDMemory memory = {0};
  assert(vkr_dmemory_create(MB(1), MB(1), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  char relative_path[256];
  snprintf(relative_path, sizeof(relative_path), "%s/io_failure_%u.bin",
           FS_TEST_RELATIVE_DIR, ++g_fs_test_counter);
  FilePath path =
      file_path_create(relative_path, &paths, FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  FileHandle handle = {0};
  assert(file_open(&path, mode, &handle) == FILE_ERROR_NONE);
  String8 payload = string8_lit("payload");
  assert(file_write_line(&handle, &payload) == FILE_ERROR_NONE);
  file_close(&handle);
  bitset8_set(&mode, FILE_MODE_APPEND);
  assert(file_open(&path, mode, &handle) == FILE_ERROR_NONE);
  const uint64_t free_before = vkr_dmemory_get_free_space(&memory);
  uint8_t *bytes = NULL;
  uint64_t count = 0;
  assert(file_read(&handle, &allocator, 4u, &count, &bytes) ==
         FILE_ERROR_IO_ERROR);
  assert(bytes == NULL && count == 0);
  assert(vkr_dmemory_get_free_space(&memory) == free_before);
  String8 text = {0};
#if !defined(_WIN32)
  assert(file_read_all(&handle, &allocator, &bytes, &count) ==
         FILE_ERROR_IO_ERROR);
  assert(bytes == NULL && count == 0);
  assert(vkr_dmemory_get_free_space(&memory) == free_before);
  assert(file_read_string(&handle, &allocator, &text) == FILE_ERROR_IO_ERROR);
  assert(text.str == NULL && text.length == 0);
  assert(vkr_dmemory_get_free_space(&memory) == free_before);
#endif
  assert(file_read_line(&handle, &allocator, &allocator, 64u, &text) ==
         FILE_ERROR_IO_ERROR);
  assert(text.str == NULL && text.length == 0);
  assert(vkr_dmemory_get_free_space(&memory) == free_before);
  file_close(&handle);
  mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  assert(file_open(&path, mode, &handle) == FILE_ERROR_NONE);
  assert(file_write_line(&handle, &payload) == FILE_ERROR_IO_ERROR);
  file_close(&handle);
  FilePath directory =
      file_path_create(FS_TEST_RELATIVE_DIR, &paths, FILE_PATH_TYPE_RELATIVE);
  assert(file_load_spirv_shader(&directory, &allocator, &bytes, &count) !=
         FILE_ERROR_NONE);
  assert(bytes == NULL && count == 0);
  assert(vkr_dmemory_get_free_space(&memory) == free_before);
  fs_test_remove_file((const char *)path.path.str);
  vkr_dmemory_allocator_destroy(&allocator);
  arena_destroy(arena);
}

// Every allocation failure yields an empty result or an out-of-memory error.
static void test_file_allocation_failures(void) {
  printf("  Running test_file_allocation_failures...\n");
  ContainerTestAllocator failing_state = {.fail = true_v};
  VkrAllocator failing = container_test_allocator(&failing_state);

  FilePath created =
      file_path_create("tests/tmp", &failing, FILE_PATH_TYPE_RELATIVE);
  assert(created.path.str == NULL && created.path.length == 0);
  assert(created.type == FILE_PATH_TYPE_RELATIVE);
  String8 directory = file_path_get_directory(&failing, string8_lit("/a/b"));
  assert(directory.str == NULL && directory.length == 0);
  String8 joined =
      file_path_join(&failing, string8_lit("/a"), string8_lit("b.bin"));
  assert(joined.str == NULL && joined.length == 0);

  Arena *arena = arena_create(MB(1), MB(1));
  VkrAllocator paths = {.ctx = arena};
  assert(vkr_allocator_arena(&paths));
  FilePath source = file_path_create("tests/src/test_main.c", &paths,
                                     FILE_PATH_TYPE_RELATIVE);
  FileMode read_mode = bitset8_create();
  bitset8_set(&read_mode, FILE_MODE_READ);
  bitset8_set(&read_mode, FILE_MODE_BINARY);
  FileHandle handle = {0};
  assert(file_open(&source, read_mode, &handle) == FILE_ERROR_NONE);
  uint8_t *bytes = NULL;
  uint64_t count = 0;
  assert(file_read_all(&handle, &failing, &bytes, &count) ==
         FILE_ERROR_OUT_OF_MEMORY);
  assert(bytes == NULL && count == 0);
  String8 line = {0};
  assert(file_read_line(&handle, &failing, &failing, 64u, &line) ==
         FILE_ERROR_OUT_OF_MEMORY);
  assert(line.str == NULL);
  file_close(&handle);
  assert(failing_state.live_bytes == 0);
  assert(
      strcmp((const char *)file_get_error_string(FILE_ERROR_OUT_OF_MEMORY).str,
             "Out of memory") == 0);
  arena_destroy(arena);
  printf("  test_file_allocation_failures PASSED\n");
}

/* A two-entry archive written by the `.vkpak` layout in filesystem/vkr_vfs.h:
 * both identities share one chunk. Mounted, paths below the content root read
 * through every read API; a corrupted index or catalog order is rejected. */
vkr_internal void test_vfs_pack_mount(void) {
  printf("  Running test_vfs_pack_mount...\n");
  static const char text[] = "line one\nline two\n";
  static const char *const identities[] = {"assets/vfs/a.txt",
                                           "assets/vfs/b.txt"};
  const uint64_t chunk_offset = VKR_PACK_ALIGNMENT * 2u;
  const uint64_t text_size = sizeof(text) - 1u;
  uint8_t image[1024] = {0};

  VkrPackCatalogHeader catalog_header = {.entry_count = 2u};
  VkrPackEntry entries[2] = {
      {.identity_offset = 0u, .identity_length = 16u, .chunk = 0u},
      {.identity_offset = 16u, .identity_length = 16u, .chunk = 0u},
  };
  const uint64_t catalog_offset = chunk_offset + VKR_PACK_ALIGNMENT;
  const uint64_t catalog_size = sizeof(catalog_header) + sizeof(entries) + 32u;
  const uint64_t table_offset = catalog_offset + catalog_size;
  VkrPackChunk chunk = {.offset = chunk_offset,
                        .size = text_size,
                        .alignment = VKR_PACK_ALIGNMENT};
  vkr_sha256(text, text_size, chunk.sha256);

  MemCopy(image + chunk_offset, text, text_size);
  uint8_t *catalog = image + catalog_offset;
  MemCopy(catalog, &catalog_header, sizeof(catalog_header));
  MemCopy(catalog + sizeof(catalog_header), entries, sizeof(entries));
  MemCopy(catalog + sizeof(catalog_header) + sizeof(entries), identities[0],
          16u);
  MemCopy(catalog + sizeof(catalog_header) + sizeof(entries) + 16u,
          identities[1], 16u);
  MemCopy(image + table_offset, &chunk, sizeof(chunk));
  /* Version 1 archives, which have no compressed chunks, still mount. */
  VkrPackHeader header = {
      .version = VKR_PACK_VERSION_MIN,
      .catalog_offset = catalog_offset,
      .catalog_size = catalog_size,
      .chunk_table_offset = table_offset,
      .chunk_table_size = sizeof(chunk),
      .total_size = table_offset + sizeof(chunk),
  };
  MemCopy(header.magic, VKR_PACK_MAGIC, 4u);
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  vkr_sha256_update(&hash, catalog, catalog_size);
  vkr_sha256_update(&hash, image + table_offset, sizeof(chunk));
  vkr_sha256_final(&hash, header.index_sha256);
  MemCopy(image, &header, sizeof(header));
  const uint64_t size = header.total_size;

  char error[128];
  assert(vkr_pack_validate(image, size, error, sizeof(error)));
  /* A catalog byte the index hash no longer covers. */
  image[catalog_offset + catalog_size - 1u] ^= 1u;
  assert(!vkr_pack_validate(image, size, error, sizeof(error)));
  image[catalog_offset + catalog_size - 1u] ^= 1u;
  /* Identities out of order, even with a matching hash. */
  VkrPackEntry swapped[2] = {entries[1], entries[0]};
  uint8_t reordered[1024];
  MemCopy(reordered, image, size);
  MemCopy(reordered + catalog_offset + sizeof(catalog_header), swapped,
          sizeof(swapped));
  VkrPackHeader reordered_header = header;
  vkr_sha256_init(&hash);
  vkr_sha256_update(&hash, reordered + catalog_offset, catalog_size);
  vkr_sha256_update(&hash, reordered + table_offset, sizeof(chunk));
  vkr_sha256_final(&hash, reordered_header.index_sha256);
  MemCopy(reordered, &reordered_header, sizeof(reordered_header));
  assert(!vkr_pack_validate(reordered, size, error, sizeof(error)));

  char pack_path[1024];
  snprintf(pack_path, sizeof(pack_path), "%s%s/vfs_%u.vkpak",
           PROJECT_SOURCE_DIR, FS_TEST_RELATIVE_DIR, g_fs_test_counter++);
  FILE *file = fopen(pack_path, "wb");
  assert(file && fwrite(image, 1u, size, file) == size);
  fclose(file);
  assert(vkr_vfs_mount_pack(pack_path));

  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  FilePath path =
      file_path_create(identities[1], &allocator, FILE_PATH_TYPE_RELATIVE);
  assert(file_exists(&path));
  FileStats stats = {0};
  assert(file_stats(&path, &stats) == FILE_ERROR_NONE &&
         stats.size == text_size);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle handle = {0};
  assert(file_open(&path, mode, &handle) == FILE_ERROR_NONE && handle.memory);
  String8 line = {0};
  assert(file_read_line(&handle, &allocator, NULL, 64u, &line) ==
             FILE_ERROR_NONE &&
         line.length == 9u && MemCompare(line.str, "line one\n", 9u) == 0);
  uint8_t *rest = NULL;
  uint64_t rest_size = 0u;
  assert(file_read_all(&handle, &allocator, &rest, &rest_size) ==
             FILE_ERROR_NONE &&
         rest_size == 9u && MemCompare(rest, "line two\n", 9u) == 0);
  uint64_t written = 0u;
  assert(file_write(&handle, 1u, (const uint8_t *)"x", &written) ==
         FILE_ERROR_INVALID_HANDLE);
  file_close(&handle);
  FILE *stream = file_fopen((const char *)path.path.str, "rb");
  char streamed[32] = {0};
  assert(stream && fread(streamed, 1u, sizeof(streamed), stream) == text_size &&
         MemCompare(streamed, text, text_size) == 0);
  fclose(stream);
  /* An identity the archive lacks falls through to the disk. */
  FilePath absent =
      file_path_create("assets/vfs/c.txt", &allocator, FILE_PATH_TYPE_RELATIVE);
  assert(!file_exists(&absent));

  vkr_vfs_unmount_all();
  assert(!file_exists(&path));
  arena_destroy(arena);
  fs_test_remove_file(pack_path);
  printf("  test_vfs_pack_mount PASSED\n");
}

/* Writes a one-chunk table and the header, with its index hash, into an
 * archive image. */
vkr_internal void fs_test_seal_pack(uint8_t *image, VkrPackHeader *header,
                                    const VkrPackChunk *chunk) {
  MemCopy(image + header->chunk_table_offset, chunk, sizeof(*chunk));
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  vkr_sha256_update(&hash, image + header->catalog_offset,
                    header->catalog_size);
  vkr_sha256_update(&hash, image + header->chunk_table_offset, sizeof(*chunk));
  vkr_sha256_final(&hash, header->index_sha256);
  MemCopy(image, header, sizeof(*header));
}

/* A version 2 archive whose only chunk is a zstd frame: its entry stats and
 * reads as the decoded bytes, and the validator rejects a compressed chunk in
 * a version 1 archive or one without a decoded size. */
vkr_internal void test_vfs_pack_compressed(void) {
  printf("  Running test_vfs_pack_compressed...\n");
  static const char identity[] = "assets/vfs/z.bin";
  char text[2048];
  for (uint32_t i = 0u; i < sizeof(text); ++i) {
    text[i] = (char)('a' + i % 7u);
  }

  uint8_t image[4096] = {0};
  const uint64_t chunk_offset = VKR_PACK_HEADER_SIZE;
  const size_t stored_size =
      ZSTD_compress(image + chunk_offset, 1024u, text, sizeof(text), 3);
  assert(!ZSTD_isError(stored_size) && stored_size < sizeof(text));
  const uint64_t catalog_offset =
      chunk_offset + (stored_size + VKR_PACK_ALIGNMENT - 1u) /
                         VKR_PACK_ALIGNMENT * VKR_PACK_ALIGNMENT;
  const VkrPackCatalogHeader catalog_header = {.entry_count = 1u};
  const VkrPackEntry entry = {.identity_length = 16u};
  const uint64_t catalog_size = sizeof(catalog_header) + sizeof(entry) + 16u;
  const uint64_t table_offset = catalog_offset + catalog_size;
  VkrPackChunk chunk = {.offset = chunk_offset,
                        .size = stored_size,
                        .alignment = VKR_PACK_ALIGNMENT,
                        .flags = VKR_PACK_CHUNK_ZSTD,
                        .decoded_size = sizeof(text)};
  vkr_sha256((const uint8_t *)text, sizeof(text), chunk.sha256);

  uint8_t *catalog = image + catalog_offset;
  MemCopy(catalog, &catalog_header, sizeof(catalog_header));
  MemCopy(catalog + sizeof(catalog_header), &entry, sizeof(entry));
  MemCopy(catalog + sizeof(catalog_header) + sizeof(entry), identity, 16u);
  VkrPackHeader header = {
      .version = VKR_PACK_VERSION,
      .catalog_offset = catalog_offset,
      .catalog_size = catalog_size,
      .chunk_table_offset = table_offset,
      .chunk_table_size = sizeof(chunk),
      .total_size = table_offset + sizeof(chunk),
  };
  MemCopy(header.magic, VKR_PACK_MAGIC, 4u);
  const uint64_t size = header.total_size;
  char error[128];

  header.version = 1u;
  fs_test_seal_pack(image, &header, &chunk);
  assert(!vkr_pack_validate(image, size, error, sizeof(error)));
  header.version = VKR_PACK_VERSION;
  chunk.decoded_size = 0u;
  fs_test_seal_pack(image, &header, &chunk);
  assert(!vkr_pack_validate(image, size, error, sizeof(error)));
  chunk.decoded_size = sizeof(text);
  fs_test_seal_pack(image, &header, &chunk);
  assert(vkr_pack_validate(image, size, error, sizeof(error)));

  char pack_path[1024];
  snprintf(pack_path, sizeof(pack_path), "%s%s/vfs_%u.vkpak",
           PROJECT_SOURCE_DIR, FS_TEST_RELATIVE_DIR, g_fs_test_counter++);
  FILE *file = fopen(pack_path, "wb");
  assert(file && fwrite(image, 1u, size, file) == size);
  fclose(file);
  vkr_content_codec_install();
  assert(vkr_vfs_mount_pack(pack_path));

  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  FilePath path =
      file_path_create(identity, &allocator, FILE_PATH_TYPE_RELATIVE);
  FileStats stats = {0};
  assert(file_stats(&path, &stats) == FILE_ERROR_NONE &&
         stats.size == sizeof(text));
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  for (uint32_t open = 0u; open < 2u; ++open) {
    FileHandle handle = {0};
    assert(file_open(&path, mode, &handle) == FILE_ERROR_NONE);
    uint8_t *bytes = NULL;
    uint64_t count = 0u;
    assert(file_read_all(&handle, &allocator, &bytes, &count) ==
               FILE_ERROR_NONE &&
           count == sizeof(text) && MemCompare(bytes, text, sizeof(text)) == 0);
    file_close(&handle);
  }

  vkr_vfs_unmount_all();
  arena_destroy(arena);
  fs_test_remove_file(pack_path);
  printf("  test_vfs_pack_compressed PASSED\n");
}

vkr_internal void fs_test_write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  assert(file && fwrite(text, 1u, strlen(text), file) == strlen(text));
  fclose(file);
}

/* Reads `path` through file_open and returns whether it holds `expected`. */
vkr_internal bool8_t fs_test_reads(const FilePath *path,
                                   VkrAllocator *allocator,
                                   const char *expected) {
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle handle = {0};
  if (file_open(path, mode, &handle) != FILE_ERROR_NONE) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  const bool8_t read =
      file_read_all(&handle, allocator, &bytes, &size) == FILE_ERROR_NONE;
  file_close(&handle);
  return read && size == strlen(expected) &&
         MemCompare(bytes, expected, size) == 0;
}

/* A depot working copy used as the editor's workspace serves its own
 * `assets/` copies before the content root's, for reads only. */
vkr_internal void test_vfs_workspace_overlay(void) {
  printf("  Running test_vfs_workspace_overlay...\n");
  char root_dir[1024];
  snprintf(root_dir, sizeof(root_dir), "%sassets/zz_vfs_overlay_test",
           PROJECT_SOURCE_DIR);
  char overlay[1024];
  snprintf(overlay, sizeof(overlay), "%s%s/overlay", PROJECT_SOURCE_DIR,
           FS_TEST_RELATIVE_DIR);
  char overlay_assets[1024];
  snprintf(overlay_assets, sizeof(overlay_assets), "%s/assets", overlay);
  char overlay_dir[1024];
  snprintf(overlay_dir, sizeof(overlay_dir), "%s/zz_vfs_overlay_test",
           overlay_assets);
  assert(fs_test_make_dir(root_dir) && fs_test_make_dir(overlay) &&
         fs_test_make_dir(overlay_assets) && fs_test_make_dir(overlay_dir));

  /* `both` exists in the root and the overlay, `root` only in the root,
     `ws` only in the overlay. */
  char root_both[1024];
  char root_only[1024];
  char overlay_both[1024];
  char overlay_only[1024];
  snprintf(root_both, sizeof(root_both), "%s/both.txt", root_dir);
  snprintf(root_only, sizeof(root_only), "%s/root.txt", root_dir);
  snprintf(overlay_both, sizeof(overlay_both), "%s/both.txt", overlay_dir);
  snprintf(overlay_only, sizeof(overlay_only), "%s/ws.txt", overlay_dir);
  fs_test_write_text(root_both, "root");
  fs_test_write_text(root_only, "root only");
  fs_test_write_text(overlay_both, "workspace");
  fs_test_write_text(overlay_only, "workspace only");

  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  FilePath both = file_path_create("assets/zz_vfs_overlay_test/both.txt",
                                   &allocator, FILE_PATH_TYPE_RELATIVE);
  FilePath only_root = file_path_create("assets/zz_vfs_overlay_test/root.txt",
                                        &allocator, FILE_PATH_TYPE_RELATIVE);
  FilePath only_ws = file_path_create("assets/zz_vfs_overlay_test/ws.txt",
                                      &allocator, FILE_PATH_TYPE_RELATIVE);

  /* Without an overlay the root serves. */
  assert(fs_test_reads(&both, &allocator, "root"));
  assert(!file_exists(&only_ws));

  assert(vkr_vfs_set_overlay_root(overlay));
  assert(fs_test_reads(&both, &allocator, "workspace"));
  assert(fs_test_reads(&only_root, &allocator, "root only"));
  assert(fs_test_reads(&only_ws, &allocator, "workspace only"));
  assert(file_exists(&only_ws));
  FileStats stats = {0};
  assert(file_stats(&both, &stats) == FILE_ERROR_NONE &&
         stats.size == strlen("workspace"));
  FILE *stream = file_fopen((const char *)both.path.str, "rb");
  char streamed[32] = {0};
  assert(stream && fread(streamed, 1u, sizeof(streamed), stream) == 9u &&
         MemCompare(streamed, "workspace", 9u) == 0);
  fclose(stream);
  FileMapping mapping = {0};
  assert(file_map_readonly(&both, &mapping) == FILE_ERROR_NONE &&
         mapping.size == 9u && MemCompare(mapping.data, "workspace", 9u) == 0);
  file_unmap(&mapping);

  /* A strict overlay, as for a depot repository: the root's own file is
     missing to every reader, while the overlay's copies and engine content
     (files git tracks under assets/) still read. */
  if (vkr_vfs_engine_content_load()) {
    vkr_vfs_set_strict_overlay(true_v);
    assert(!file_exists(&only_root));
    FileStats missing = {0};
    assert(file_stats(&only_root, &missing) != FILE_ERROR_NONE);
    FileMode read = bitset8_create();
    bitset8_set(&read, FILE_MODE_READ);
    FileHandle blocked = {0};
    assert(file_open(&only_root, read, &blocked) != FILE_ERROR_NONE);
    FileMapping blocked_mapping = {0};
    assert(file_map_readonly(&only_root, &blocked_mapping) != FILE_ERROR_NONE);
    assert(!file_fopen((const char *)only_root.path.str, "rb"));
    assert(fs_test_reads(&both, &allocator, "workspace"));
    assert(fs_test_reads(&only_ws, &allocator, "workspace only"));
    FilePath engine = file_path_create("assets/materials/default.world.mt",
                                       &allocator, FILE_PATH_TYPE_RELATIVE);
    assert(vkr_vfs_is_engine_content("assets/materials/default.world.mt") &&
           !vkr_vfs_is_engine_content("assets/zz_vfs_overlay_test/root.txt"));
    /* The cooked sibling of a tracked image, which git ignores. */
    assert(vkr_vfs_is_engine_content(
               "assets/textures/greybox/greybox_grid.png.vkt") &&
           !vkr_vfs_is_engine_content("assets/textures/greybox/none.png.vkt"));
    assert(file_exists(&engine));
    vkr_vfs_set_strict_overlay(false_v);
    assert(fs_test_reads(&only_root, &allocator, "root only"));
  } else {
    printf("    git ls-files is unavailable; the strict overlay check is "
           "skipped\n");
  }

  /* Writes still go to the root. */
  FileMode write = bitset8_create();
  bitset8_set(&write, FILE_MODE_WRITE);
  bitset8_set(&write, FILE_MODE_TRUNCATE);
  FileHandle handle = {0};
  uint64_t written = 0u;
  assert(file_open(&both, write, &handle) == FILE_ERROR_NONE &&
         file_write(&handle, 7u, (const uint8_t *)"written", &written) ==
             FILE_ERROR_NONE);
  file_close(&handle);
  /* Plain stdio: the root's own path would read through the overlay. */
  char direct[32] = {0};
  FILE *root_file = fopen(root_both, "rb");
  assert(root_file && fread(direct, 1u, sizeof(direct), root_file) == 7u &&
         MemCompare(direct, "written", 7u) == 0);
  fclose(root_file);
  FilePath overlay_path = {
      .path = {.str = (uint8_t *)overlay_both, .length = strlen(overlay_both)},
      .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(fs_test_reads(&overlay_path, &allocator, "workspace"));

  /* The root itself, also spelled `root/.`, overlays nothing and must not
     redirect a path to itself. */
  assert(vkr_vfs_set_overlay_root(vkr_content_root()));
  assert(fs_test_reads(&both, &allocator, "written"));
  char root_dot[1024];
  snprintf(root_dot, sizeof(root_dot), "%s.", vkr_content_root());
  assert(vkr_vfs_set_overlay_root(root_dot));
  assert(fs_test_reads(&both, &allocator, "written"));
  assert(!file_exists(&only_ws));

  assert(vkr_vfs_set_overlay_root(NULL));
  assert(!file_exists(&only_ws));
  vkr_vfs_unmount_all();

  arena_destroy(arena);
  fs_test_remove_file(root_both);
  fs_test_remove_file(root_only);
  fs_test_remove_file(overlay_both);
  fs_test_remove_file(overlay_only);
  fs_test_remove_dir(root_dir);
  fs_test_remove_dir(overlay_dir);
  fs_test_remove_dir(overlay_assets);
  fs_test_remove_dir(overlay);
  printf("  test_vfs_workspace_overlay PASSED\n");
}

/* A remote that holds `held.txt` and `assets/zz_vfs_remote/b.txt`, both
   "fetched", and writes each into its directory when asked. */
typedef struct FsTestRemote {
  char directory[1024];
  uint32_t calls;
  uint32_t fetched;
  uint32_t reads;
  uint32_t prefetches;
  uint32_t cancels;
} FsTestRemote;

/* The path below the directory of a file the remote holds; false for one
   it lacks. */
vkr_internal bool8_t fs_test_remote_path(const FsTestRemote *remote,
                                         const char *relative, char *path,
                                         uint64_t capacity) {
  if (strcmp(relative, "held.txt") != 0 &&
      strcmp(relative, "assets/zz_vfs_remote/b.txt") != 0 &&
      strcmp(relative, "deep/inner/c.txt") != 0) {
    return false_v;
  }
  snprintf(path, capacity, "%s/%s", remote->directory, relative);
  return true_v;
}

vkr_internal bool8_t fs_test_path_on_disk(const char *path) {
  FILE *existing = fopen(path, "rb");
  if (existing) {
    fclose(existing);
  }
  return existing != NULL;
}

vkr_internal bool8_t fs_test_remote_fetch(void *context, const char *relative) {
  FsTestRemote *remote = context;
  remote->calls += 1u;
  char path[1100];
  if (!fs_test_remote_path(remote, relative, path, sizeof(path))) {
    return false_v;
  }
  if (fs_test_path_on_disk(path)) {
    return true_v;
  }
  fs_test_write_text(path, "fetched");
  remote->fetched += 1u;
  return true_v;
}

vkr_internal bool8_t fs_test_remote_pending(void *context, const char *relative,
                                            uint64_t *out_size) {
  FsTestRemote *remote = context;
  char path[1100];
  if (!fs_test_remote_path(remote, relative, path, sizeof(path)) ||
      fs_test_path_on_disk(path)) {
    return false_v;
  }
  *out_size = 7u;
  return true_v;
}

vkr_internal bool8_t fs_test_remote_read(void *context, const char *relative,
                                         uint64_t offset, uint64_t size,
                                         void *destination) {
  FsTestRemote *remote = context;
  char path[1100];
  if (!fs_test_remote_path(remote, relative, path, sizeof(path)) ||
      offset > 7u || size > 7u - offset) {
    return false_v;
  }
  remote->reads += 1u;
  const char *text = "fetched";
  MemCopy(destination, text + offset, (size_t)size);
  return true_v;
}

vkr_internal void fs_test_remote_prefetch(void *context, const char *relative) {
  (void)relative;
  ((FsTestRemote *)context)->prefetches += 1u;
}

vkr_internal void fs_test_remote_cancel(void *context, const char *relative) {
  (void)relative;
  ((FsTestRemote *)context)->cancels += 1u;
}

/* Every file the remote holds, `deep/inner/c.txt` never asked for, so its
   folders exist only through the remote. */
vkr_global const char *const fs_test_remote_files[] = {
    "held.txt", "assets/zz_vfs_remote/b.txt", "deep/inner/c.txt"};

vkr_internal bool8_t fs_test_remote_list(void *context, const char *relative,
                                         FileDirectoryVisit visit,
                                         void *visit_context) {
  FsTestRemote *remote = context;
  const size_t prefix = strlen(relative);
  bool8_t any = false_v;
  for (uint32_t i = 0u; i < ArrayCount(fs_test_remote_files); ++i) {
    const char *path = fs_test_remote_files[i];
    char disk[1100];
    snprintf(disk, sizeof(disk), "%s/%s", remote->directory, path);
    if (fs_test_path_on_disk(disk) ||
        (prefix &&
         (strncmp(path, relative, prefix) != 0 || path[prefix] != '/'))) {
      continue;
    }
    const char *name = path + (prefix ? prefix + 1u : 0u);
    const char *slash = strchr(name, '/');
    char child[64];
    snprintf(child, sizeof(child), "%.*s",
             (int)(slash ? (size_t)(slash - name) : strlen(name)), name);
    any = true_v;
    if (!visit(visit_context, child, slash != NULL)) {
      break;
    }
  }
  return any;
}

typedef struct FsTestNames {
  char names[8][64];
  bool8_t directories[8];
  uint32_t count;
} FsTestNames;

vkr_internal bool8_t fs_test_collect_name(void *context, const char *name,
                                          bool8_t is_directory) {
  FsTestNames *names = context;
  if (names->count < ArrayCount(names->names)) {
    snprintf(names->names[names->count], sizeof(names->names[0]), "%s", name);
    names->directories[names->count] = is_directory;
    names->count += 1u;
  }
  return true_v;
}

/* Whether `names` holds `name` once, as a folder or a file. */
vkr_internal bool8_t fs_test_names_hold(const FsTestNames *names,
                                        const char *name,
                                        bool8_t is_directory) {
  uint32_t found = 0u;
  for (uint32_t i = 0u; i < names->count; ++i) {
    found += strcmp(names->names[i], name) == 0 &&
             names->directories[i] == is_directory;
  }
  return found == 1u;
}

vkr_internal uint64_t fs_test_remote_arrivals(void *context) {
  return ((FsTestRemote *)context)->fetched;
}

vkr_global const VkrVfsRemoteCalls fs_test_remote_calls = {
    .fetch = fs_test_remote_fetch,
    .pending = fs_test_remote_pending,
    .read = fs_test_remote_read,
    .prefetch = fs_test_remote_prefetch,
    .cancel = fs_test_remote_cancel,
    .list = fs_test_remote_list,
    .arrivals = fs_test_remote_arrivals,
};

/* Asset streaming (vkr_vfs_set_remote). Fails when a read below the remote
   directory does not ask the remote before the disk, when a file the remote
   lacks appears, when a path outside the directory asks the remote, or when
   the workspace overlay does not ask for its copy there. Also fails when
   asking whether a file is pending, reading part of it or queueing it
   fetches it, when a whole file still counts as pending, or when the
   overlay's copy is not the one found pending. Then fails when a folder
   holding only files not yet written does not exist or list them, when a
   name on disk lists twice, when such a file or folder does not resolve
   below the directory's resolved path, when a path that doubles a
   separator does not find its file, or when any of these fetches. */
vkr_internal void test_vfs_remote_directory(void) {
  printf("  Running test_vfs_remote_directory...\n");
  FsTestRemote remote = {0};
  snprintf(remote.directory, sizeof(remote.directory), "%s%s/remote",
           PROJECT_SOURCE_DIR, FS_TEST_RELATIVE_DIR);
  char assets[1100];
  char assets_dir[1100];
  snprintf(assets, sizeof(assets), "%s/assets", remote.directory);
  snprintf(assets_dir, sizeof(assets_dir), "%s/zz_vfs_remote", assets);
  assert(fs_test_make_dir(remote.directory) && fs_test_make_dir(assets) &&
         fs_test_make_dir(assets_dir));
  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  char held_text[1100];
  char missing_text[1100];
  snprintf(held_text, sizeof(held_text), "%s/held.txt", remote.directory);
  snprintf(missing_text, sizeof(missing_text), "%s/missing.txt",
           remote.directory);
  const FilePath held = {
      .path = {.str = (uint8_t *)held_text, .length = strlen(held_text)},
      .type = FILE_PATH_TYPE_ABSOLUTE};
  const FilePath missing = {
      .path = {.str = (uint8_t *)missing_text, .length = strlen(missing_text)},
      .type = FILE_PATH_TYPE_ABSOLUTE};

  /* Without a remote nothing arrives. */
  assert(!file_exists(&held));
  assert(!vkr_vfs_remote_pending(&held, NULL));
  assert(vkr_vfs_set_remote(remote.directory, &fs_test_remote_calls, &remote));

  /* A pending file reads in part and queues without arriving. */
  uint64_t pending_size = 0u;
  assert(vkr_vfs_remote_pending(&held, &pending_size) && pending_size == 7u);
  char part[4] = {0};
  assert(vkr_vfs_remote_read(&held, 2u, 3u, part) &&
         MemCompare(part, "tch", 3u) == 0 && remote.reads == 1u);
  vkr_vfs_remote_prefetch(&held);
  vkr_vfs_remote_cancel(&held);
  assert(remote.prefetches == 1u && remote.cancels == 1u &&
         remote.calls == 0u && remote.fetched == 0u &&
         vkr_vfs_remote_arrivals() == 0u);
  assert(!vkr_vfs_remote_pending(&missing, NULL));
  assert(!vkr_vfs_remote_read(&missing, 0u, 1u, part));

  assert(file_exists(&held) && remote.fetched == 1u);
  assert(vkr_vfs_remote_arrivals() == 1u);
  assert(!vkr_vfs_remote_pending(&held, NULL));
  assert(!vkr_vfs_remote_read(&held, 0u, 1u, part));
  assert(fs_test_reads(&held, &allocator, "fetched"));
  FileStats stats = {0};
  assert(file_stats(&held, &stats) == FILE_ERROR_NONE && stats.size == 7u);
  FileMapping mapping = {0};
  assert(file_map_readonly(&held, &mapping) == FILE_ERROR_NONE &&
         mapping.size == 7u && MemCompare(mapping.data, "fetched", 7u) == 0);
  file_unmap(&mapping);
  assert(remote.fetched == 1u);
  const uint32_t calls = remote.calls;
  assert(!file_exists(&missing) && remote.calls == calls + 1u);

  /* A path outside the directory never asks. */
  FilePath outside = file_path_create("assets/zz_vfs_remote_none.txt",
                                      &allocator, FILE_PATH_TYPE_RELATIVE);
  assert(!file_exists(&outside) && remote.calls == calls + 1u);
  assert(!vkr_vfs_remote_pending(&outside, NULL));

  /* The directory as the workspace overlay: the overlay's copy of an
     `assets/` identity arrives before the root's is used. */
  assert(vkr_vfs_set_overlay_root(remote.directory));
  FilePath overlaid = file_path_create("assets/zz_vfs_remote/b.txt", &allocator,
                                       FILE_PATH_TYPE_RELATIVE);
  assert(vkr_vfs_remote_pending(&overlaid, NULL));
  assert(vkr_vfs_remote_read(&overlaid, 0u, 4u, part) &&
         MemCompare(part, "fetc", 4u) == 0 && remote.reads == 2u);
  assert(fs_test_reads(&overlaid, &allocator, "fetched"));
  assert(remote.fetched == 2u);
  assert(!vkr_vfs_remote_pending(&overlaid, NULL));

  /* Folders that hold only files the remote has not written exist and
     list them, without a fetch; the disk's names list once. */
  char deep_text[1100];
  char inner_text[1100];
  char nowhere_text[1100];
  snprintf(deep_text, sizeof(deep_text), "%s/deep", remote.directory);
  snprintf(inner_text, sizeof(inner_text), "%s/deep/inner", remote.directory);
  snprintf(nowhere_text, sizeof(nowhere_text), "%s/nowhere", remote.directory);
  const FilePath deep = file_path_view(deep_text);
  const FilePath inner = file_path_view(inner_text);
  const FilePath nowhere = file_path_view(nowhere_text);
  const uint32_t fetched_before = remote.fetched;
  assert(vkr_vfs_remote_directory(&deep) && file_exists(&deep) &&
         file_exists(&inner));
  assert(!vkr_vfs_remote_directory(&nowhere) && !file_exists(&nowhere));
  assert(!vkr_vfs_remote_directory(&held));
  FsTestNames names = {0};
  assert(file_list_directory(&deep, fs_test_collect_name, &names) ==
         FILE_ERROR_NONE);
  assert(names.count == 1u && fs_test_names_hold(&names, "inner", true_v));
  MemZero(&names, sizeof(names));
  assert(file_list_directory(&inner, fs_test_collect_name, &names) ==
         FILE_ERROR_NONE);
  assert(names.count == 1u && fs_test_names_hold(&names, "c.txt", false_v));
  MemZero(&names, sizeof(names));
  const FilePath root = file_path_view(remote.directory);
  assert(file_list_directory(&root, fs_test_collect_name, &names) ==
         FILE_ERROR_NONE);
  assert(names.count == 3u && fs_test_names_hold(&names, "held.txt", false_v) &&
         fs_test_names_hold(&names, "assets", true_v) &&
         fs_test_names_hold(&names, "deep", true_v));
  assert(file_list_directory(&nowhere, fs_test_collect_name, &names) ==
         FILE_ERROR_NOT_FOUND);
  char resolved_root[1100];
  char resolved[1100];
  char expected[1200];
  char deep_file_text[1100];
  snprintf(deep_file_text, sizeof(deep_file_text), "%s/c.txt", inner_text);
  const FilePath deep_file = file_path_view(deep_file_text);
  assert(file_path_resolve(&root, resolved_root, sizeof(resolved_root)) ==
         FILE_ERROR_NONE);
  assert(file_path_resolve(&deep_file, resolved, sizeof(resolved)) ==
         FILE_ERROR_NONE);
  snprintf(expected, sizeof(expected), "%s/deep/inner/c.txt", resolved_root);
  assert(file_path_equals(resolved, expected));
  assert(file_path_resolve(&inner, resolved, sizeof(resolved)) ==
         FILE_ERROR_NONE);
  snprintf(expected, sizeof(expected), "%s/deep/inner", resolved_root);
  assert(file_path_equals(resolved, expected));
  assert(file_path_resolve(&nowhere, resolved, sizeof(resolved)) ==
         FILE_ERROR_NOT_FOUND);
  assert(file_path_resolve(&missing, resolved, sizeof(resolved)) ==
         FILE_ERROR_NOT_FOUND);
  char doubled_text[1100];
  snprintf(doubled_text, sizeof(doubled_text), "%s//deep//inner/c.txt",
           remote.directory);
  const FilePath doubled = file_path_view(doubled_text);
  pending_size = 0u;
  assert(vkr_vfs_remote_pending(&doubled, &pending_size) && pending_size == 7u);
  assert(remote.fetched == fetched_before);

  assert(vkr_vfs_set_remote(NULL, NULL, NULL));
  assert(vkr_vfs_set_overlay_root(NULL));
  vkr_vfs_unmount_all();
  arena_destroy(arena);
  char overlaid_text[1100];
  snprintf(overlaid_text, sizeof(overlaid_text), "%s/b.txt", assets_dir);
  fs_test_remove_file(held_text);
  fs_test_remove_file(overlaid_text);
  fs_test_remove_dir(assets_dir);
  fs_test_remove_dir(assets);
  fs_test_remove_dir(remote.directory);
  printf("  test_vfs_remote_directory PASSED\n");
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
  test_file_create_directories();
  test_file_clone();
  test_file_link();
  test_file_rename_waits_for_brief_reader();
  test_file_portable_publication_primitives();
  test_file_io_failures_release_owned_outputs();
  test_file_allocation_failures();
  test_vfs_pack_mount();
  test_vfs_pack_compressed();
  test_vfs_workspace_overlay();
  test_vfs_remote_directory();

  printf("--- Filesystem Tests Completed ---\n");
  return true;
}
