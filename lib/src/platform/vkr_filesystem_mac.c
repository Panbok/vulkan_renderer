#include "filesystem/filesystem.h"

#if defined(PLATFORM_APPLE)

#include "core/logger.h"

#include <errno.h>
#include <limits.h>

FILE *file_fopen(const char *utf8_path, const char *mode) {
  if (!utf8_path || !mode) {
    errno = EINVAL;
    return NULL;
  }
  return fopen(utf8_path, mode);
}

vkr_internal int fs_file_descriptor(const FileHandle *handle) {
  return (int)(intptr_t)handle->handle - 1;
}

bool8_t file_exists(const FilePath *path) {
  if (!path || !path->path.str || !path->path.length) {
    return false_v;
  }
  struct stat buffer;
  return stat((char *)path->path.str, &buffer) == 0;
}

FileError file_stats(const FilePath *path, FileStats *out_stats) {
  if (!path || !path->path.str || !path->path.length || !out_stats) {
    return FILE_ERROR_INVALID_PATH;
  }
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
  if (errno != EEXIST)
    return false_v;
  struct stat existing;
  return stat((const char *)path->path.str, &existing) == 0 &&
         S_ISDIR(existing.st_mode);
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
  if (!path || !path->path.str || !path->path.length || !out_handle) {
    return FILE_ERROR_INVALID_PATH;
  }
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
    if (errno == ENOENT || errno == ENOTDIR) {
      return FILE_ERROR_NOT_FOUND;
    }
    if (errno == EACCES || errno == EPERM) {
      return FILE_ERROR_ACCESS_DENIED;
    }
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
  *out_buffer = NULL;
  *bytes_read = 0;
  uint8_t *buffer =
      size ? vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_FILE)
           : NULL;
  if (!buffer && size > 0u)
    return FILE_ERROR_OUT_OF_MEMORY;
  FileError error = file_read_into(handle, buffer, size, bytes_read);
  if (error != FILE_ERROR_NONE) {
    if (buffer)
      vkr_allocator_free(allocator, buffer, size,
                         VKR_ALLOCATOR_MEMORY_TAG_FILE);
    *bytes_read = 0;
    return error;
  }
  *out_buffer = buffer;
  return FILE_ERROR_NONE;
}

static FileError fs_remaining_size(FileHandle *handle, uint64_t *out_size) {
  if (!handle || !handle->handle)
    return FILE_ERROR_INVALID_HANDLE;
  struct stat stats;
  const int fd = fs_file_descriptor(handle);
  if (fstat(fd, &stats) != 0 || stats.st_size < 0)
    return FILE_ERROR_IO_ERROR;
  const off_t position = lseek(fd, 0, SEEK_CUR);
  if (position < 0 || position > stats.st_size)
    return FILE_ERROR_IO_ERROR;
  *out_size = (uint64_t)(stats.st_size - position);
  return FILE_ERROR_NONE;
}

FileError file_read_all(FileHandle *handle, VkrAllocator *allocator,
                        uint8_t **out_buffer, uint64_t *bytes_read) {
  *out_buffer = NULL;
  *bytes_read = 0;
  uint64_t size = 0;
  FileError error = fs_remaining_size(handle, &size);
  if (error != FILE_ERROR_NONE)
    return error;
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
  FileError error = fs_remaining_size(handle, &size);
  if (error != FILE_ERROR_NONE)
    return error;
  if (size == UINT64_MAX)
    return FILE_ERROR_IO_ERROR;
  uint8_t *buffer =
      vkr_allocator_alloc(allocator, size + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer)
    return FILE_ERROR_OUT_OF_MEMORY;
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
  const char *from = (const char *)source->path.str;
  const char *to = (const char *)destination->path.str;
  // RENAME_EXCL makes the existence check part of the rename, so a concurrent
  // writer cannot create the destination between a check and the move.
  const int result =
      overwrite ? rename(from, to) : renamex_np(from, to, RENAME_EXCL);
  if (result == 0) {
    return FILE_ERROR_NONE;
  }
  if (errno == EEXIST) {
    return FILE_ERROR_ALREADY_EXISTS;
  }
  return errno == ENOENT ? FILE_ERROR_NOT_FOUND : FILE_ERROR_IO_ERROR;
}

FileError file_read_line(FileHandle *handle, VkrAllocator *allocator,
                         VkrAllocator *line_allocator, uint64_t max_line_length,
                         String8 *out_line) {
  *out_line = (String8){0};
  if (!handle || !handle->handle)
    return FILE_ERROR_INVALID_HANDLE;
  int fd = fs_file_descriptor(handle);
  if (max_line_length == 0 || max_line_length == UINT64_MAX)
    return FILE_ERROR_LINE_TOO_LONG;
  VkrAllocator *target_alloc = line_allocator ? line_allocator : allocator;
  FileError error = FILE_ERROR_NONE;

  char chunk[128];
  uint64_t total_len = 0;

  uint8_t *result_buf = vkr_allocator_alloc(target_alloc, max_line_length + 1,
                                            VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!result_buf)
    return FILE_ERROR_OUT_OF_MEMORY;

  while (total_len < max_line_length) {
    // Record start position of this chunk read
    off_t start_pos = lseek(fd, 0, SEEK_CUR);
    if (start_pos < 0) {
      error = FILE_ERROR_IO_ERROR;
      goto cleanup;
    }
    ssize_t n;
    do {
      n = read(fd, chunk, sizeof(chunk));
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
      error = FILE_ERROR_IO_ERROR;
      goto cleanup;
    }
    if (n == 0)
      break;

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
      if (lseek(fd, start_pos + amount_to_copy, SEEK_SET) < 0) {
        error = FILE_ERROR_IO_ERROR;
        goto cleanup;
      }
      break;
    }

    // If no newline and not full, we continue.
    // File pointer is already at start_pos + n (from read), which matches our
    // progress.
  }

  if (total_len == 0) {
    error = FILE_ERROR_EOF;
    goto cleanup;
  }

  result_buf[total_len] = '\0';
  *out_line = (String8){.str = result_buf, .length = total_len};
  return FILE_ERROR_NONE;
cleanup:
  vkr_allocator_free(target_alloc, result_buf, max_line_length + 1,
                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return error;
}

FileError file_write_line(FileHandle *handle, const String8 *text) {
  uint64_t written = 0;
  FileError error = file_write(handle, text->length, text->str, &written);
  if (error != FILE_ERROR_NONE)
    return error;
  return file_write(handle, 1, (const uint8_t *)"\n", &written);
}

#endif
