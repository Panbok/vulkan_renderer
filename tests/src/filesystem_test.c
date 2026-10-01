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
  test_file_clone();
  test_file_link();
  test_file_rename_waits_for_brief_reader();
  test_file_portable_publication_primitives();
  test_file_io_failures_release_owned_outputs();
  test_file_allocation_failures();
  test_vfs_pack_mount();
  test_vfs_pack_compressed();

  printf("--- Filesystem Tests Completed ---\n");
  return true;
}
