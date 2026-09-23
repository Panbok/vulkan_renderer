#include "filesystem/filesystem.h"
#include "platform/vkr_filesystem_internal.h"

// Path, whole-file and error-text helpers shared by the macOS and Windows
// filesystem implementations. Whole-file reads build on the native
// file_read_into and file_remaining_size. Every path or string allocation
// failure returns an empty result; callers treat a null `str` as failure.

vkr_internal bool8_t fs_is_separator(uint8_t c) {
#if defined(PLATFORM_WINDOWS)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

vkr_internal String8 fs_string_duplicate(VkrAllocator *allocator,
                                         const String8 *src) {
  if (!src || !src->str || src->length == 0) {
    return (String8){0};
  }
  uint8_t *mem = vkr_allocator_alloc(allocator, src->length + 1,
                                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!mem) {
    return (String8){0};
  }
  MemCopy(mem, src->str, src->length);
  mem[src->length] = '\0';
  return (String8){.str = mem, .length = src->length};
}

FilePath file_path_create(const char *path, VkrAllocator *allocator,
                          FilePathType type) {
  const char *root = type == FILE_PATH_TYPE_RELATIVE ? PROJECT_SOURCE_DIR : "";
  const uint64_t root_length = string_length(root);
  const uint64_t path_length = string_length(path);
  const uint64_t length = root_length + path_length;
  uint8_t *buffer = vkr_allocator_alloc(allocator, length + 1,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer) {
    return (FilePath){.type = type};
  }
  MemCopy(buffer, root, root_length);
  MemCopy(buffer + root_length, path, path_length);
  buffer[length] = '\0';
  return (FilePath){.path = (String8){.str = buffer, .length = length},
                    .type = type};
}

String8 file_path_get_directory(VkrAllocator *allocator, String8 path) {
  if (!path.str || path.length == 0) {
    return (String8){0};
  }
  uint64_t directory_length = 0;
  for (uint64_t i = path.length; i > 0; --i) {
    if (fs_is_separator(path.str[i - 1])) {
      directory_length = i;
      break;
    }
  }
  // A path without a separator, or whose only separator is its last byte,
  // has no separate directory component.
  if (directory_length == 0 || directory_length == path.length) {
    return (String8){0};
  }
  const String8 directory = {.str = path.str, .length = directory_length};
  return fs_string_duplicate(allocator, &directory);
}

String8 file_path_join(VkrAllocator *allocator, String8 dir, String8 file) {
  if (!dir.str || dir.length == 0) {
    return fs_string_duplicate(allocator, &file);
  }
  if (!file.str || file.length == 0) {
    return fs_string_duplicate(allocator, &dir);
  }
#if defined(PLATFORM_WINDOWS)
  const uint8_t separator = '\\';
#else
  const uint8_t separator = '/';
#endif
  const bool8_t needs_separator = !fs_is_separator(dir.str[dir.length - 1]);
  const uint64_t length =
      dir.length + (needs_separator ? 1u : 0u) + file.length;
  uint8_t *buffer = vkr_allocator_alloc(allocator, length + 1,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer) {
    return (String8){0};
  }
  uint64_t offset = 0;
  MemCopy(buffer, dir.str, dir.length);
  offset += dir.length;
  if (needs_separator) {
    buffer[offset++] = separator;
  }
  MemCopy(buffer + offset, file.str, file.length);
  buffer[length] = '\0';
  return (String8){.str = buffer, .length = length};
}

FileError file_read(FileHandle *handle, VkrAllocator *allocator, uint64_t size,
                    uint64_t *bytes_read, uint8_t **out_buffer) {
  *out_buffer = NULL;
  *bytes_read = 0;
  uint8_t *buffer =
      size ? vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_FILE)
           : NULL;
  if (!buffer && size > 0u) {
    return FILE_ERROR_OUT_OF_MEMORY;
  }
  FileError error = file_read_into(handle, buffer, size, bytes_read);
  if (error != FILE_ERROR_NONE) {
    if (buffer) {
      vkr_allocator_free(allocator, buffer, size,
                         VKR_ALLOCATOR_MEMORY_TAG_FILE);
    }
    *bytes_read = 0;
    return error;
  }
  *out_buffer = buffer;
  return FILE_ERROR_NONE;
}

FileError file_read_all(FileHandle *handle, VkrAllocator *allocator,
                        uint8_t **out_buffer, uint64_t *bytes_read) {
  *out_buffer = NULL;
  *bytes_read = 0;
  uint64_t size = 0;
  FileError error = file_remaining_size(handle, &size);
  if (error != FILE_ERROR_NONE) {
    return error;
  }
  error = file_read(handle, allocator, size, bytes_read, out_buffer);
  if (error == FILE_ERROR_NONE && *bytes_read != size) {
    vkr_allocator_free(allocator, *out_buffer, size,
                       VKR_ALLOCATOR_MEMORY_TAG_FILE);
    *out_buffer = NULL;
    *bytes_read = 0;
    return FILE_ERROR_IO_ERROR;
  }
  return error;
}

FileError file_read_string(FileHandle *handle, VkrAllocator *allocator,
                           String8 *out_data) {
  *out_data = (String8){0};
  uint64_t size = 0;
  FileError error = file_remaining_size(handle, &size);
  if (error != FILE_ERROR_NONE) {
    return error;
  }
  if (size == UINT64_MAX) {
    return FILE_ERROR_IO_ERROR;
  }
  uint8_t *buffer =
      vkr_allocator_alloc(allocator, size + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer) {
    return FILE_ERROR_OUT_OF_MEMORY;
  }
  uint64_t bytes_read = 0;
  error = file_read_into(handle, buffer, size, &bytes_read);
  if (error != FILE_ERROR_NONE || bytes_read != size) {
    vkr_allocator_free(allocator, buffer, size + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return error != FILE_ERROR_NONE ? error : FILE_ERROR_IO_ERROR;
  }
  buffer[bytes_read] = '\0';
  *out_data = (String8){.str = buffer, .length = bytes_read};
  return FILE_ERROR_NONE;
}

FileError file_write_line(FileHandle *handle, const String8 *text) {
  uint64_t written = 0;
  FileError error = file_write(handle, text->length, text->str, &written);
  if (error != FILE_ERROR_NONE) {
    return error;
  }
  return file_write(handle, 1, (const uint8_t *)"\n", &written);
}

FileError file_load_spirv_shader(const FilePath *path, VkrAllocator *allocator,
                                 uint8_t **out_data, uint64_t *out_size) {
  *out_data = NULL;
  *out_size = 0;
  FileHandle handle;
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  if (file_open(path, mode, &handle) != FILE_ERROR_NONE) {
    return FILE_ERROR_OPEN_FAILED;
  }

  FileError err = file_read_all(&handle, allocator, out_data, out_size);
  file_close(&handle);
  if (err != FILE_ERROR_NONE) {
    return err;
  }

  if ((uintptr_t)(*out_data) % 4 != 0) {
    uint8_t *old_buffer = *out_data;
    uint8_t *aligned = vkr_allocator_alloc_aligned(
        allocator, *out_size, 4, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    if (!aligned) {
      vkr_allocator_free(allocator, old_buffer, *out_size,
                         VKR_ALLOCATOR_MEMORY_TAG_FILE);
      *out_data = NULL;
      *out_size = 0;
      return FILE_ERROR_OUT_OF_MEMORY;
    }
    MemCopy(aligned, old_buffer, *out_size);
    vkr_allocator_free(allocator, old_buffer, *out_size,
                       VKR_ALLOCATOR_MEMORY_TAG_FILE);
    *out_data = aligned;
  }
  return err;
}

String8 file_get_error_string(FileError error) {
  switch (error) {
  case FILE_ERROR_NONE:
    return string8_lit("No error");
  case FILE_ERROR_NOT_FOUND:
    return string8_lit("File not found");
  case FILE_ERROR_ACCESS_DENIED:
    return string8_lit("Access denied");
  case FILE_ERROR_IO_ERROR:
    return string8_lit("I/O error");
  case FILE_ERROR_EOF:
    return string8_lit("End of file");
  case FILE_ERROR_LINE_TOO_LONG:
    return string8_lit("Line too long");
  case FILE_ERROR_INVALID_MODE:
    return string8_lit("Invalid mode");
  case FILE_ERROR_INVALID_PATH:
    return string8_lit("Invalid path");
  case FILE_ERROR_OPEN_FAILED:
    return string8_lit("Open failed");
  case FILE_ERROR_INVALID_HANDLE:
    return string8_lit("Invalid handle");
  case FILE_ERROR_INVALID_SPIR_V:
    return string8_lit("Invalid SPIR-V file format");
  case FILE_ERROR_FILE_EMPTY:
    return string8_lit("File is empty");
  case FILE_ERROR_ALREADY_EXISTS:
    return string8_lit("Already exists");
  case FILE_ERROR_OUT_OF_MEMORY:
    return string8_lit("Out of memory");
  case FILE_ERROR_COUNT:
    break;
  }
  return string8_lit("Unknown error");
}
