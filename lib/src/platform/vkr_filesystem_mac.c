#include "filesystem/filesystem.h"

#if defined(PLATFORM_APPLE)

#include "core/logger.h"

#include <limits.h>

vkr_internal int fs_file_descriptor(const FileHandle *handle) {
  return (int)(intptr_t)handle->handle - 1;
}

vkr_internal String8 fs_string_duplicate(VkrAllocator *allocator,
                                         const String8 *src) {
  if (!src || !src->str || src->length == 0)
    return (String8){0};
  uint8_t *mem = vkr_allocator_alloc(allocator, src->length + 1,
                                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
  MemCopy(mem, src->str, src->length);
  mem[src->length] = '\0';
  return (String8){.str = mem, .length = src->length};
}

FilePath file_path_create(const char *path, VkrAllocator *allocator,
                          FilePathType type) {
  if (type == FILE_PATH_TYPE_RELATIVE) {
    const char *root = PROJECT_SOURCE_DIR;
    uint64_t root_len = string_length(root);
    uint64_t path_len = string_length(path);
    uint64_t full_len = root_len + path_len;

    uint8_t *buf = vkr_allocator_alloc(allocator, full_len + 1,
                                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    MemCopy(buf, root, root_len);
    MemCopy(buf + root_len, path, path_len);
    buf[full_len] = '\0';

    return (FilePath){.path = (String8){.str = buf, .length = full_len},
                      .type = type};
  } else {
    uint64_t len = string_length(path);
    uint8_t *buf = vkr_allocator_alloc(allocator, len + 1,
                                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    MemCopy(buf, path, len);
    buf[len] = '\0';
    return (FilePath){.path = (String8){.str = buf, .length = len},
                      .type = type};
  }
}

String8 file_path_get_directory(VkrAllocator *allocator, String8 path) {
  if (!path.str || path.length == 0)
    return (String8){0};
  uint64_t last_slash = path.length;
  for (uint64_t i = path.length; i > 0; --i) {
    if (path.str[i - 1] == '/') {
      last_slash = i;
      break;
    }
  }
  if (last_slash == path.length)
    return (String8){0};
  String8 dir = {.str = path.str, .length = last_slash};
  return fs_string_duplicate(allocator, &dir);
}

String8 file_path_join(VkrAllocator *allocator, String8 dir, String8 file) {
  if (!dir.str || dir.length == 0)
    return fs_string_duplicate(allocator, &file);
  if (!file.str || file.length == 0)
    return fs_string_duplicate(allocator, &dir);
  bool8_t needs_sep = (dir.str[dir.length - 1] != '/');
  uint64_t len = dir.length + (needs_sep ? 1 : 0) + file.length;
  uint8_t *buf =
      vkr_allocator_alloc(allocator, len + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  uint64_t offset = 0;
  MemCopy(buf, dir.str, dir.length);
  offset += dir.length;
  if (needs_sep)
    buf[offset++] = '/';
  MemCopy(buf + offset, file.str, file.length);
  buf[len] = '\0';
  return (String8){.str = buf, .length = len};
}

bool8_t file_exists(const FilePath *path) {
  struct stat buffer;
  return stat((char *)path->path.str, &buffer) == 0;
}

FileError file_stats(const FilePath *path, FileStats *out_stats) {
  struct stat buffer;
  if (stat((char *)path->path.str, &buffer) == 0) {
    out_stats->size = (uint64_t)buffer.st_size;
    out_stats->last_modified = (uint64_t)buffer.st_mtime;
    return FILE_ERROR_NONE;
  }
  return FILE_ERROR_NOT_FOUND;
}

bool8_t file_create_directory(const FilePath *path) {
  if (mkdir((char *)path->path.str, 0755) == 0)
    return true_v;
  if (errno == EEXIST)
    return true_v;
  return false_v;
}

FileError file_create_directory_exclusive(const FilePath *path) {
  if (!path || !path->path.str) {
    return FILE_ERROR_INVALID_PATH;
  }
  if (mkdir((char *)path->path.str, 0755) == 0) {
    return FILE_ERROR_NONE;
  }
  return errno == EEXIST ? FILE_ERROR_ALREADY_EXISTS : FILE_ERROR_IO_ERROR;
}

FileError file_path_resolve(const FilePath *path, char *out_path,
                            uint64_t out_capacity) {
  if (!path || !path->path.str || !out_path || out_capacity == 0u) {
    return FILE_ERROR_INVALID_PATH;
  }
  char resolved[PATH_MAX];
  if (!realpath((const char *)path->path.str, resolved)) {
    return errno == ENOENT ? FILE_ERROR_NOT_FOUND : FILE_ERROR_IO_ERROR;
  }
  const uint64_t length = string_length(resolved);
  if (length + 1u > out_capacity) {
    return FILE_ERROR_INVALID_PATH;
  }
  MemCopy(out_path, resolved, length + 1u);
  return FILE_ERROR_NONE;
}

bool8_t file_path_equals(const char *lhs, const char *rhs) {
  return string_equals(lhs, rhs);
}

bool8_t file_path_starts_with(const char *path, const char *prefix) {
  return path && prefix && string_n_equals(path, prefix, string_length(prefix));
}

bool8_t file_ensure_directory(VkrAllocator *allocator, const String8 *path) {
  assert_log(allocator != NULL, "allocator is NULL");
  assert_log(path != NULL, "path is NULL");
  assert_log(path->str != NULL, "path string is NULL");
  assert_log(path->length > 0, "path length is 0");

  VkrAllocatorScope scope = vkr_allocator_begin_scope(allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return false_v;
  }

  // POSIX Optimized Implementation
  char *buffer = (char *)vkr_allocator_alloc(allocator, path->length + 1,
                                             VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }
  MemCopy(buffer, path->str, (size_t)path->length);
  buffer[path->length] = '\0';

  const char sep = '/';

  for (uint64_t i = 0; i < path->length; ++i) {
    char c = buffer[i];
    if (c != sep)
      continue;

    if (i == 0) {
      buffer[i] = sep;
      continue;
    } // Root slash

    char saved = buffer[i];
    buffer[i] = '\0';

    // POSIX specific absolute check
    FilePathType path_type =
        (buffer[0] == '/') ? FILE_PATH_TYPE_ABSOLUTE : FILE_PATH_TYPE_RELATIVE;

    String8 path_str = string8_create_from_cstr((const uint8_t *)buffer,
                                                string_length(buffer));
    FilePath file_path = {.path = path_str, .type = path_type};

    if (!file_create_directory(&file_path)) {
      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      return false_v;
    }
    buffer[i] = sep;
  }

  // Final directory check
  String8 final_path_str =
      string8_create_from_cstr((const uint8_t *)buffer, string_length(buffer));
  FilePathType final_path_type =
      (buffer[0] == '/') ? FILE_PATH_TYPE_ABSOLUTE : FILE_PATH_TYPE_RELATIVE;
  FilePath final_file_path = {.path = final_path_str, .type = final_path_type};

  bool8_t result = file_create_directory(&final_file_path);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return result;
}

FileError file_open(const FilePath *path, FileMode mode,
                    FileHandle *out_handle) {
  int flags = 0;
  mode_t access_mode = 0644;

  bool8_t has_read = bitset8_is_set(&mode, FILE_MODE_READ);
  bool8_t has_write = bitset8_is_set(&mode, FILE_MODE_WRITE);
  bool8_t has_append = bitset8_is_set(&mode, FILE_MODE_APPEND);
  bool8_t has_create = bitset8_is_set(&mode, FILE_MODE_CREATE);
  bool8_t has_truncate = bitset8_is_set(&mode, FILE_MODE_TRUNCATE);
  bool8_t has_exclusive = bitset8_is_set(&mode, FILE_MODE_EXCLUSIVE);

  if (has_exclusive && (!has_create || has_truncate || has_append)) {
    return FILE_ERROR_INVALID_MODE;
  }

  if (has_read && has_write)
    flags |= O_RDWR;
  else if (has_write)
    flags |= O_WRONLY;
  else if (has_read)
    flags |= O_RDONLY;

  bool8_t implies_create = has_create || has_append ||
                           (has_write && has_truncate) ||
                           (has_write && !has_read);
  bool8_t implies_truncate =
      has_truncate || (has_write && !has_read && !has_append);

  if (implies_create)
    flags |= O_CREAT;
  if (has_exclusive)
    flags |= O_EXCL;
  if (implies_truncate)
    flags |= O_TRUNC;
  if (has_append)
    flags |= O_APPEND;

  int fd = open((char *)path->path.str, flags, access_mode);
  if (fd == -1) {
    if (errno == EEXIST)
      return FILE_ERROR_ALREADY_EXISTS;
    log_error("Failed to open file '%s': %s", path->path.str, strerror(errno));
    return FILE_ERROR_OPEN_FAILED;
  }

  /* Offset by one so a valid descriptor zero is not confused with NULL. */
  out_handle->handle = (void *)(intptr_t)(fd + 1);
  out_handle->path = path;
  out_handle->mode = mode;
  return FILE_ERROR_NONE;
}

void file_close(FileHandle *handle) {
  if (handle && handle->handle) {
    close(fs_file_descriptor(handle));
    handle->handle = NULL;
  }
}

FileError file_write(FileHandle *handle, uint64_t size, const uint8_t *buffer,
                     uint64_t *bytes_written) {
  if (!handle || !handle->handle || (!buffer && size > 0u) || !bytes_written) {
    return FILE_ERROR_INVALID_HANDLE;
  }
  *bytes_written = 0u;
  while (*bytes_written < size) {
    const ssize_t written =
        write(fs_file_descriptor(handle), buffer + *bytes_written,
              (size_t)(size - *bytes_written));
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return FILE_ERROR_IO_ERROR;
    }
    *bytes_written += (uint64_t)written;
  }
  return FILE_ERROR_NONE;
}

FileError file_read_into(FileHandle *handle, void *buffer, uint64_t size,
                         uint64_t *bytes_read) {
  if (!handle || !handle->handle || (!buffer && size > 0u) || !bytes_read) {
    return FILE_ERROR_INVALID_HANDLE;
  }
  *bytes_read = 0u;
  while (*bytes_read < size) {
    const ssize_t count =
        read(fs_file_descriptor(handle), (uint8_t *)buffer + *bytes_read,
             (size_t)(size - *bytes_read));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      return FILE_ERROR_IO_ERROR;
    }
    if (count == 0) {
      break;
    }
    *bytes_read += (uint64_t)count;
  }
  return FILE_ERROR_NONE;
}

FileError file_read(FileHandle *handle, VkrAllocator *allocator, uint64_t size,
                    uint64_t *bytes_read, uint8_t **out_buffer) {
  *out_buffer =
      vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!*out_buffer && size > 0u) {
    return FILE_ERROR_IO_ERROR;
  }
  return file_read_into(handle, *out_buffer, size, bytes_read);
}

FileError file_read_all(FileHandle *handle, VkrAllocator *allocator,
                        uint8_t **out_buffer, uint64_t *bytes_read) {
  int fd = fs_file_descriptor(handle);
  struct stat st;
  if (fstat(fd, &st) == -1)
    return FILE_ERROR_IO_ERROR;

  uint64_t size = (uint64_t)st.st_size;
  off_t current_pos = lseek(fd, 0, SEEK_CUR);

  if (current_pos < 0 || (uint64_t)current_pos > size) {
    return FILE_ERROR_IO_ERROR;
  }

  *out_buffer = vkr_allocator_alloc(allocator, size - current_pos,
                                    VKR_ALLOCATOR_MEMORY_TAG_FILE);
  return file_read_into(handle, *out_buffer, size - current_pos, bytes_read);
}

FileError file_sync(FileHandle *handle) {
  if (!handle || !handle->handle) {
    return FILE_ERROR_INVALID_HANDLE;
  }
  return fsync(fs_file_descriptor(handle)) == 0 ? FILE_ERROR_NONE
                                                : FILE_ERROR_IO_ERROR;
}

FileError file_remove(const FilePath *path) {
  if (!path || !path->path.str) {
    return FILE_ERROR_INVALID_PATH;
  }
  if (unlink((const char *)path->path.str) == 0) {
    return FILE_ERROR_NONE;
  }
  return errno == ENOENT ? FILE_ERROR_NOT_FOUND : FILE_ERROR_IO_ERROR;
}

FileError file_rename(const FilePath *source, const FilePath *destination,
                      bool8_t overwrite) {
  if (!source || !source->path.str || !destination || !destination->path.str) {
    return FILE_ERROR_INVALID_PATH;
  }
  if (!overwrite && file_exists(destination)) {
    return FILE_ERROR_ALREADY_EXISTS;
  }
  return rename((const char *)source->path.str,
                (const char *)destination->path.str) == 0
             ? FILE_ERROR_NONE
             : FILE_ERROR_IO_ERROR;
}

FileError file_read_line(FileHandle *handle, VkrAllocator *allocator,
                         VkrAllocator *line_allocator, uint64_t max_line_length,
                         String8 *out_line) {
  int fd = fs_file_descriptor(handle);
  VkrAllocator *target_alloc = line_allocator ? line_allocator : allocator;

  char chunk[128];
  uint64_t total_len = 0;

  uint8_t *result_buf = vkr_allocator_alloc(target_alloc, max_line_length + 1,
                                            VKR_ALLOCATOR_MEMORY_TAG_STRING);

  while (total_len < max_line_length) {
    // Record start position of this chunk read
    off_t start_pos = lseek(fd, 0, SEEK_CUR);
    ssize_t n = read(fd, chunk, sizeof(chunk));

    if (n <= 0)
      break; // EOF or Error

    int newline_idx = -1;
    for (int i = 0; i < n; i++) {
      if (chunk[i] == '\n') {
        newline_idx = i;
        break;
      }
    }

    uint64_t amount_available =
        (newline_idx != -1) ? (uint64_t)(newline_idx + 1) : (uint64_t)n;
    uint64_t amount_to_copy = amount_available;

    // Clamp to max line length
    if (total_len + amount_to_copy > max_line_length) {
      amount_to_copy = max_line_length - total_len;
    }

    MemCopy(result_buf + total_len, chunk, amount_to_copy);
    total_len += amount_to_copy;

    // If we found a newline OR we hit the max buffer size, we are done.
    // We must reset the file pointer to exactly after what we copied.
    if (newline_idx != -1 || total_len == max_line_length) {
      lseek(fd, start_pos + amount_to_copy, SEEK_SET);
      break;
    }

    // If no newline and not full, we continue.
    // File pointer is already at start_pos + n (from read), which matches our
    // progress.
  }

  if (total_len == 0)
    return FILE_ERROR_EOF;

  result_buf[total_len] = '\0';
  *out_line = (String8){.str = result_buf, .length = total_len};
  return FILE_ERROR_NONE;
}

FileError file_write_line(FileHandle *handle, const String8 *text) {
  int fd = fs_file_descriptor(handle);
  if (write(fd, text->str, text->length) == -1)
    return FILE_ERROR_IO_ERROR;
  if (write(fd, "\n", 1) == -1)
    return FILE_ERROR_IO_ERROR;
  return FILE_ERROR_NONE;
}

FileError file_read_string(FileHandle *handle, VkrAllocator *allocator,
                           String8 *out_data) {
  uint8_t *buffer = NULL;
  uint64_t bytes_read = 0;
  FileError err = file_read_all(handle, allocator, &buffer, &bytes_read);
  if (err != FILE_ERROR_NONE)
    return err;

  uint8_t *str_buf = vkr_allocator_alloc(allocator, bytes_read + 1,
                                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
  MemCopy(str_buf, buffer, bytes_read);
  str_buf[bytes_read] = '\0';
  *out_data = (String8){.str = str_buf, .length = bytes_read};
  return FILE_ERROR_NONE;
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
  default:
    return string8_lit("Unknown error");
  }
}

FileError file_load_spirv_shader(const FilePath *path, VkrAllocator *allocator,
                                 uint8_t **out_data, uint64_t *out_size) {
  FileHandle handle;
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  if (file_open(path, mode, &handle) != FILE_ERROR_NONE)
    return FILE_ERROR_OPEN_FAILED;

  FileError err = file_read_all(&handle, allocator, out_data, out_size);
  file_close(&handle);

  if ((uintptr_t)(*out_data) % 4 != 0) {
    uint8_t *old_buffer = *out_data;
    uint8_t *aligned = vkr_allocator_alloc(allocator, *out_size,
                                           VKR_ALLOCATOR_MEMORY_TAG_FILE);
    MemCopy(aligned, old_buffer, *out_size);
    vkr_allocator_free(allocator, old_buffer, *out_size,
                       VKR_ALLOCATOR_MEMORY_TAG_FILE);
    *out_data = aligned;
  }
  return err;
}
#endif
