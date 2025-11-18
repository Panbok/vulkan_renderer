#include "filesystem.h"

#include "core/logger.h"

#if defined(PLATFORM_WINDOWS)
#include <direct.h>
#endif

FilePath file_path_create(const char *path, Arena *arena, FilePathType type) {
  assert_log(path != NULL, "path is NULL");
  assert_log(arena != NULL, "arena is NULL");
  assert_log(string_length(path) > 0, "path is empty");
  assert_log(type == FILE_PATH_TYPE_RELATIVE || type == FILE_PATH_TYPE_ABSOLUTE,
             "invalid file path type");

  FilePath result = {0};

  if (type == FILE_PATH_TYPE_RELATIVE) {
    assert_log(PROJECT_SOURCE_DIR != NULL, "PROJECT_SOURCE_DIR is NULL");
    uint64_t full_path_len =
        string_length(PROJECT_SOURCE_DIR) + string_length(path) + 1;
    uint8_t *full_path =
        (uint8_t *)arena_alloc(arena, full_path_len, ARENA_MEMORY_TAG_STRING);
    snprintf((char *)full_path, full_path_len, "%s%s", PROJECT_SOURCE_DIR,
             path);

    result.path = string8_create(full_path, full_path_len);
  } else {
    uint64_t path_len = string_length(path) + 1;
    uint8_t *path_str =
        (uint8_t *)arena_alloc(arena, path_len, ARENA_MEMORY_TAG_STRING);
    snprintf((char *)path_str, path_len, "%s", path);

    result.path = string8_create(path_str, path_len);
  }

  result.type = type;
  return result;
}

bool8_t file_exists(const FilePath *path) {
  assert_log(path != NULL, "path is NULL");
  assert_log(path->path.length > 0, "path is empty");

  log_debug("Checking if file exists: %s", path->path.str);

  struct stat buffer;
  return stat((char *)path->path.str, &buffer) == 0;
}

FileError file_stats(const FilePath *path, FileStats *out_stats) {
  assert_log(path != NULL, "path is NULL");
  assert_log(out_stats != NULL, "out_stats is NULL");

  struct stat buffer;
  if (stat((char *)path->path.str, &buffer) == 0) {
    out_stats->size = buffer.st_size;
    out_stats->last_modified = buffer.st_mtime;
    return FILE_ERROR_NONE;
  }

  return FILE_ERROR_NOT_FOUND;
}

bool8_t file_create_directory(const FilePath *path) {
  assert_log(path != NULL, "path is NULL");

  const char *path_str = (const char *)path->path.str;
  if (path_str[0] == '\0')
    return false_v;

  struct stat st = {0};
  if (stat(path_str, &st) == 0) {
    if ((st.st_mode & S_IFDIR) != 0) {
      return true_v;
    }
    log_error("Filesystem: path exists but is not a directory '%s'", path_str);
    return false_v;
  }

#if defined(PLATFORM_WINDOWS)
  int result = _mkdir(path_str);
#else
  int result = mkdir(path_str, 0755);
#endif
  if (result == 0) {
    return true_v;
  }

  if (errno == EEXIST) {
    struct stat st_retry = {0};
    if (stat(path_str, &st_retry) == 0 && (st_retry.st_mode & S_IFDIR) != 0) {
      return true_v;
    }
    log_error("Filesystem: path exists but is not a directory '%s'", path_str);
    return false_v;
  }

  log_error("Filesystem: failed to create directory '%s': %s", path_str,
            strerror(errno));
  return false_v;
}

bool8_t file_ensure_directory(Arena *arena, const String8 *path) {
  assert_log(arena != NULL, "arena is NULL");
  assert_log(path != NULL, "path is NULL");
  assert_log(path->str != NULL, "path string is NULL");
  assert_log(path->length > 0, "path length is 0");

  Scratch scratch = scratch_create(arena);
  char *buffer = (char *)arena_alloc(scratch.arena, path->length + 1,
                                     ARENA_MEMORY_TAG_STRING);
  if (!buffer) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    log_error("Filesystem: failed to allocate directory buffer");
    return false_v;
  }
  MemCopy(buffer, path->str, (size_t)path->length);
  buffer[path->length] = '\0';

#if defined(PLATFORM_WINDOWS)
  const char sep = '\\';
#else
  const char sep = '/';
#endif

  for (uint64_t i = 0; i < path->length; ++i) {
    char c = buffer[i];
    bool8_t is_separator = (c == '/' || c == '\\');
    if (!is_separator)
      continue;

    if (i == 0) {
      buffer[i] = sep;
      continue;
    }
#if defined(PLATFORM_WINDOWS)
    if (i > 0 && buffer[i - 1] == ':') {
      buffer[i] = sep;
      continue;
    }
#endif

    char saved = buffer[i];
    buffer[i] = '\0';
    if (buffer[0] != '\0') {
      // Create FilePath from buffer
      String8 path_str = string8_create_from_cstr((const uint8_t *)buffer,
                                                  string_length(buffer));
      FilePathType path_type = FILE_PATH_TYPE_RELATIVE;
#if defined(PLATFORM_WINDOWS)
      if (buffer[0] == '/' || buffer[0] == '\\' ||
          (string_length(buffer) >= 3 && buffer[1] == ':' &&
           (buffer[2] == '/' || buffer[2] == '\\'))) {
        path_type = FILE_PATH_TYPE_ABSOLUTE;
      }
#else
      if (buffer[0] == '/') {
        path_type = FILE_PATH_TYPE_ABSOLUTE;
      }
#endif
      FilePath file_path = {.path = path_str, .type = path_type};
      if (!file_create_directory(&file_path)) {
        buffer[i] = saved;
        scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
        return false_v;
      }
    }
    buffer[i] = sep;
  }

  uint64_t final_len = string_length(buffer);
  if (final_len > 1 &&
      (buffer[final_len - 1] == '/' || buffer[final_len - 1] == '\\')) {
#if defined(PLATFORM_WINDOWS)
    // Don't strip if it's a drive root like "C:\"
    if (!(final_len == 3 && buffer[1] == ':')) {
#endif
      buffer[final_len - 1] = '\0';
#if defined(PLATFORM_WINDOWS)
    }
#endif
  }

  // Create FilePath from final buffer
  String8 final_path_str =
      string8_create_from_cstr((const uint8_t *)buffer, string_length(buffer));
  FilePathType final_path_type = FILE_PATH_TYPE_RELATIVE;
#if defined(PLATFORM_WINDOWS)
  if (buffer[0] == '/' || buffer[0] == '\\' ||
      (string_length(buffer) >= 3 && buffer[1] == ':' &&
       (buffer[2] == '/' || buffer[2] == '\\'))) {
    final_path_type = FILE_PATH_TYPE_ABSOLUTE;
  }
#else
  if (buffer[0] == '/') {
    final_path_type = FILE_PATH_TYPE_ABSOLUTE;
  }
#endif
  FilePath final_file_path = {.path = final_path_str, .type = final_path_type};
  bool8_t result = file_create_directory(&final_file_path);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
  return result;
}

FileError file_open(const FilePath *path, FileMode mode,
                    FileHandle *out_handle) {
  assert_log(path != NULL, "path is NULL");
  assert_log(out_handle != NULL, "out_handle is NULL");

  char mode_str[4] = {0}; // Max: "r+b\0" or "w+b\0" or "a+b\0"
  int mode_idx = 0;

  bool8_t has_read = bitset8_is_set(&mode, FILE_MODE_READ);
  bool8_t has_write = bitset8_is_set(&mode, FILE_MODE_WRITE);
  bool8_t has_append = bitset8_is_set(&mode, FILE_MODE_APPEND);
  bool8_t has_binary = bitset8_is_set(&mode, FILE_MODE_BINARY);
  bool8_t has_truncate = bitset8_is_set(&mode, FILE_MODE_TRUNCATE);

  // Determine base mode
  if (has_append) {
    mode_str[mode_idx++] = 'a';
    // Append mode automatically creates file if it doesn't exist
    // and positions at end for writing
  } else if (has_write && has_truncate) {
    mode_str[mode_idx++] = 'w';
    // Write mode truncates file and creates if doesn't exist
  } else if (has_write && !has_read) {
    mode_str[mode_idx++] = 'w';
    // Write-only mode
  } else if (has_read && !has_write) {
    mode_str[mode_idx++] = 'r';
    // Read-only mode
  } else if (has_read && has_write) {
    if (has_truncate) {
      mode_str[mode_idx++] = 'w';
    } else {
      mode_str[mode_idx++] = 'r';
    }
  } else {
    log_error("Invalid file mode: no read, write, or append flags set");
    return FILE_ERROR_INVALID_MODE;
  }

  // Add '+' for read+write modes
  if ((has_read && has_write) || (has_append && has_read)) {
    mode_str[mode_idx++] = '+';
  }

  // Add 'b' for binary mode
  if (has_binary) {
    mode_str[mode_idx++] = 'b';
  }

  mode_str[mode_idx] = '\0';

  FILE *file = fopen((char *)path->path.str, mode_str);
  if (!file) {
    log_error("Error opening file: '%s' with mode '%s'", path->path.str,
              mode_str);
    return FILE_ERROR_OPEN_FAILED;
  }

  out_handle->handle = file;
  out_handle->path = path;
  out_handle->mode = mode;

  return FILE_ERROR_NONE;
}

void file_close(FileHandle *handle) {
  assert_log(handle != NULL, "handle is NULL");

  if (handle->handle) {
    fclose((FILE *)handle->handle);
    handle->handle = 0;
  }
}

FileError file_read_line(FileHandle *handle, Arena *arena, Arena *line_arena,
                         uint64_t max_line_length, String8 *out_line) {
  assert(handle != NULL && "File handle is NULL");
  assert(out_line != NULL && "Out line is NULL");

  if (!handle->handle) {
    return FILE_ERROR_INVALID_HANDLE;
  }

  // If the same arena is passed for both, fall back to a scratch copy to
  // avoid conflicts
  bool8_t same_arena = (arena == line_arena);
  Scratch scratch = {0};
  Arena *target_arena = line_arena;
  if (same_arena) {
    scratch = scratch_create(arena);
    target_arena = scratch.arena;
  }

  // Read into a temporary dynamic buffer
  uint64_t capacity = max_line_length + 1; // include terminator
  uint8_t *buf =
      (uint8_t *)arena_alloc(target_arena, capacity, ARENA_MEMORY_TAG_STRING);
  if (!buf) {
    if (same_arena)
      scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    return FILE_ERROR_IO_ERROR;
  }

  uint64_t len = 0;
  int ch = 0;
  while (len < max_line_length) {
    ch = fgetc((FILE *)handle->handle);
    if (ch == EOF)
      break;
    buf[len++] = (uint8_t)ch;
    if (ch == '\n')
      break;
  }

  if (len == 0 && ch == EOF) {
    if (same_arena)
      scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    return FILE_ERROR_EOF;
  }

  buf[len] = '\0';

  // If we used scratch (same_arena), duplicate into the provided arena
  if (same_arena) {
    uint8_t *dup =
        (uint8_t *)arena_alloc(arena, len + 1, ARENA_MEMORY_TAG_STRING);
    if (!dup) {
      scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
      return FILE_ERROR_IO_ERROR;
    }
    MemCopy(dup, buf, len + 1);
    *out_line = (String8){.str = dup, .length = len};
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
  } else {
    *out_line = (String8){.str = buf, .length = len};
  }

  return FILE_ERROR_NONE;
}

FileError file_write_line(FileHandle *handle, const String8 *text) {
  assert_log(handle != NULL, "handle is NULL");
  assert_log(text != NULL, "text is NULL");

  if (handle->handle && text->length > 0) {
    int result = fputs((char *)text->str, (FILE *)handle->handle);
    if (result != EOF) {
      result = fputc('\n', (FILE *)handle->handle);
      if (result == EOF) {
        log_error("Error writing line to file: '%s'",
                  (char *)handle->path->path.str);
        return FILE_ERROR_IO_ERROR;
      }
    } else {
      log_error("Error writing line to file: '%s'",
                (char *)handle->path->path.str);
      return FILE_ERROR_IO_ERROR;
    }

    // Make sure to flush the stream so it is written to the file immediately.
    // This prevents data loss in the event of a crash.
    fflush((FILE *)handle->handle);
    return FILE_ERROR_NONE;
  }

  return FILE_ERROR_INVALID_HANDLE;
}

FileError file_read(FileHandle *handle, Arena *arena, uint64_t size,
                    uint64_t *bytes_read, uint8_t **out_buffer) {
  assert_log(handle != NULL, "handle is NULL");
  assert_log(arena != NULL, "arena is NULL");
  assert_log(bytes_read != NULL, "bytes_read is NULL");
  assert_log(out_buffer != NULL, "out_buffer is NULL");

  if (handle->handle && size > 0) {
    *out_buffer = (uint8_t *)arena_alloc(arena, size, ARENA_MEMORY_TAG_FILE);
    *bytes_read = fread(*out_buffer, 1, size, (FILE *)handle->handle);
    if (*bytes_read != size && !feof((FILE *)handle->handle)) {
      return FILE_ERROR_IO_ERROR;
    }

    return FILE_ERROR_NONE;
  }

  return FILE_ERROR_INVALID_HANDLE;
}

FileError file_read_string(FileHandle *handle, Arena *arena,
                           String8 *out_data) {
  assert_log(handle != NULL, "handle is NULL");
  assert_log(arena != NULL, "arena is NULL");
  assert_log(out_data != NULL, "out_data is NULL");

  if (handle->handle) {
    uint8_t *buffer = NULL;
    uint64_t bytes_read = 0;

    FileError error = file_read_all(handle, arena, &buffer, &bytes_read);
    if (error != FILE_ERROR_NONE) {
      return error;
    }

    // Allocate space for string + null terminator
    out_data->str =
        (uint8_t *)arena_alloc(arena, bytes_read + 1, ARENA_MEMORY_TAG_STRING);
    MemCopy(out_data->str, buffer, bytes_read);
    out_data->str[bytes_read] = '\0';
    out_data->length = bytes_read;

    return FILE_ERROR_NONE;
  }

  return FILE_ERROR_INVALID_HANDLE;
}

FileError file_read_all(FileHandle *handle, Arena *arena, uint8_t **out_buffer,
                        uint64_t *bytes_read) {
  assert_log(handle != NULL, "handle is NULL");
  assert_log(arena != NULL, "arena is NULL");
  assert_log(out_buffer != NULL, "out_buffer is NULL");
  assert_log(bytes_read != NULL, "bytes_read is NULL");

  if (handle->handle) {
    off_t current_pos = FTELL64((FILE *)handle->handle);
    if (current_pos == (off_t)-1) {
      return FILE_ERROR_IO_ERROR;
    }

    if (FSEEK64((FILE *)handle->handle, 0, SEEK_END) != 0) {
      return FILE_ERROR_IO_ERROR;
    }

    off_t file_end = FTELL64((FILE *)handle->handle);
    if (file_end == (off_t)-1) {
      return FILE_ERROR_IO_ERROR;
    }

    uint64_t file_size = (uint64_t)(file_end - current_pos);

    if (FSEEK64((FILE *)handle->handle, current_pos, SEEK_SET) != 0) {
      return FILE_ERROR_IO_ERROR;
    }

    *out_buffer =
        (uint8_t *)arena_alloc(arena, file_size, ARENA_MEMORY_TAG_FILE);
    *bytes_read = fread(*out_buffer, 1, file_size, (FILE *)handle->handle);

    if (*bytes_read != file_size && !feof((FILE *)handle->handle)) {
      return FILE_ERROR_IO_ERROR;
    }

    return FILE_ERROR_NONE;
  }

  return FILE_ERROR_INVALID_HANDLE;
}

FileError file_write(FileHandle *handle, uint64_t size, const uint8_t *buffer,
                     uint64_t *bytes_written) {
  assert_log(handle != NULL, "handle is NULL");
  assert_log(buffer != NULL, "buffer is NULL");
  assert_log(bytes_written != NULL, "bytes_written is NULL");

  if (handle->handle && size > 0) {
    *bytes_written = fwrite(buffer, 1, size, (FILE *)handle->handle);
    if (*bytes_written != size) {
      return FILE_ERROR_IO_ERROR;
    }

    fflush((FILE *)handle->handle);
    return FILE_ERROR_NONE;
  }

  return FILE_ERROR_INVALID_HANDLE;
}

FileError file_load_spirv_shader(const FilePath *path, Arena *arena,
                                 uint8_t **shader_data, uint64_t *shader_size) {
  assert_log(path != NULL, "path is NULL");
  assert_log(arena != NULL, "arena is NULL");
  assert_log(shader_data != NULL, "shader_data is NULL");
  assert_log(shader_size != NULL, "shader_size is NULL");

  FileMode shader_mode = bitset8_create();
  bitset8_set(&shader_mode, FILE_MODE_READ);
  bitset8_set(&shader_mode, FILE_MODE_BINARY);

  FileHandle shader_handle;
  FileError file_error = file_open(path, shader_mode, &shader_handle);
  if (file_error != FILE_ERROR_NONE) {
    log_error("Failed to open shader: %s", file_get_error_string(file_error));
    return file_error;
  }

  file_error = file_read_all(&shader_handle, arena, shader_data, shader_size);
  if (file_error != FILE_ERROR_NONE) {
    file_close(&shader_handle);
    log_error("Failed to read shader file: %s",
              file_get_error_string(file_error));
    return file_error;
  }

  if (*shader_data == NULL || *shader_size == 0) {
    file_close(&shader_handle);
    log_error("Shader file is empty or failed to load");
    return FILE_ERROR_FILE_EMPTY;
  }

  // Ensure 4-byte alignment for SPIR-V data
  if ((uintptr_t)(*shader_data) % 4 != 0) {
    log_warn("Shader data not 4-byte aligned, copying to aligned buffer");
    uint8_t *aligned_data =
        (uint8_t *)arena_alloc(arena, *shader_size, ARENA_MEMORY_TAG_RENDERER);
    // Ensure the new allocation is 4-byte aligned
    if ((uintptr_t)aligned_data % 4 != 0) {
      file_close(&shader_handle);
      log_fatal("Failed to allocate 4-byte aligned memory for shader data");
      return FILE_ERROR_INVALID_SPIR_V;
    }
    MemCopy(aligned_data, *shader_data, *shader_size);
    *shader_data = aligned_data;
  }

  // Validate SPIR-V magic number (0x07230203)
  if (*shader_size >= 4) {
    uint32_t *magic = (uint32_t *)(*shader_data);
    if (*magic != 0x07230203) {
      file_close(&shader_handle);
      log_fatal("Invalid SPIR-V magic number: 0x%08X (expected 0x07230203)",
                *magic);
      return FILE_ERROR_INVALID_SPIR_V;
    }
    log_debug("SPIR-V magic number validated: 0x%08X", *magic);
  } else {
    file_close(&shader_handle);
    log_error("Shader file too small to contain valid SPIR-V header");
    return FILE_ERROR_INVALID_SPIR_V;
  }

  file_close(&shader_handle);
  return FILE_ERROR_NONE;
}

// Returns the directory portion of the given path including the trailing path
// separator (e.g. "/foo/bar.txt" -> "/foo/"). Returns an empty String8 if no
// separator is present. Note: This trailing-separator convention is relied upon
// by file_path_join so callers and future maintainers understand the contract.
String8 file_path_get_directory(Arena *arena, String8 path) {
  assert_log(arena != NULL, "arena is NULL");

  if (!path.str || path.length == 0)
    return (String8){0};
  uint64_t last_slash = path.length;
  for (uint64_t i = path.length; i > 0; --i) {
    uint8_t ch = path.str[i - 1];
    if (ch == '/' || ch == '\\') {
      last_slash = i;
      break;
    }
  }
  if (last_slash == path.length)
    return (String8){0};
  return string8_duplicate(arena,
                           &(String8){.str = path.str, .length = last_slash});
}

String8 file_path_join(Arena *arena, String8 dir, String8 file) {
  assert_log(arena != NULL, "arena is NULL");

  if (!dir.str || dir.length == 0)
    return string8_duplicate(arena, &file);
#if defined(PLATFORM_WINDOWS)
  char sep = '\\';
#else
  char sep = '/';
#endif
  uint64_t needs_sep =
      (dir.str[dir.length - 1] == '/' || dir.str[dir.length - 1] == '\\') ? 0
                                                                          : 1;
  uint64_t len = dir.length + needs_sep + file.length;
  uint8_t *buf = arena_alloc(arena, len + 1, ARENA_MEMORY_TAG_STRING);
  assert_log(buf != NULL, "Failed to allocate join buffer");
  uint64_t offset = 0;
  MemCopy(buf, dir.str, dir.length);
  offset += dir.length;
  if (needs_sep) {
    buf[offset++] = sep;
  }
  MemCopy(buf + offset, file.str, file.length);
  offset += file.length;
  buf[offset] = '\0';
  return string8_create(buf, offset);
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
  case FILE_ERROR_LINE_TOO_LONG:
    return string8_lit("Line too long");
  case FILE_ERROR_EOF:
    return string8_lit("End of file");
  case FILE_ERROR_COUNT:
  default:
    return string8_lit("Unknown error");
    break;
  }
}
