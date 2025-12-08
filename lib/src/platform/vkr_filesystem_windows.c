#include "filesystem/filesystem.h"

#if defined(PLATFORM_WINDOWS)

#include "core/logger.h"

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
    if (path.str[i - 1] == '/' || path.str[i - 1] == '\\') {
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
  char last = (char)dir.str[dir.length - 1];
  bool8_t needs_sep = (last != '/' && last != '\\');
  uint64_t len = dir.length + (needs_sep ? 1 : 0) + file.length;

  uint8_t *buf =
      vkr_allocator_alloc(allocator, len + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  uint64_t offset = 0;
  MemCopy(buf, dir.str, dir.length);
  offset += dir.length;
  if (needs_sep)
    buf[offset++] = '\\';
  MemCopy(buf + offset, file.str, file.length);
  buf[len] = '\0';
  return (String8){.str = buf, .length = len};
}

bool8_t file_exists(const FilePath *path) {
  DWORD dwAttrib = GetFileAttributesA((char *)path->path.str);
  return (dwAttrib != INVALID_FILE_ATTRIBUTES);
}

FileError file_stats(const FilePath *path, FileStats *out_stats) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (GetFileAttributesExA((char *)path->path.str, GetFileExInfoStandard,
                           &data)) {
    LARGE_INTEGER size;
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    out_stats->size = (uint64_t)size.QuadPart;
    ULARGE_INTEGER ull;
    ull.LowPart = data.ftLastWriteTime.dwLowDateTime;
    ull.HighPart = data.ftLastWriteTime.dwHighDateTime;
    out_stats->last_modified = (ull.QuadPart / 10000000ULL) - 11644473600ULL;
    return FILE_ERROR_NONE;
  }
  return FILE_ERROR_NOT_FOUND;
}

bool8_t file_create_directory(const FilePath *path) {
  if (CreateDirectoryA((char *)path->path.str, NULL))
    return true_v;
  if (GetLastError() == ERROR_ALREADY_EXISTS)
    return true_v;
  return false_v;
}

bool8_t file_ensure_directory(VkrAllocator *allocator, const String8 *path) {
  assert_log(allocator != NULL, "allocator is NULL");
  assert_log(path != NULL, "path is NULL");
  assert_log(path->str != NULL, "path string is NULL");
  assert_log(path->length > 0, "path length is 0");

  VkrAllocatorScope scope = vkr_allocator_begin_scope(allocator);
  if (!vkr_allocator_scope_is_valid(&scope))
    return false_v;

  char *buffer = (char *)vkr_allocator_alloc(allocator, path->length + 1,
                                             VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }
  MemCopy(buffer, path->str, (size_t)path->length);
  buffer[path->length] = '\0';

  const char sep = '\\';

  for (uint64_t i = 0; i < path->length; ++i) {
    char c = buffer[i];
    bool8_t is_separator = (c == '/' || c == '\\');
    if (!is_separator)
      continue;

    if (i == 0) {
      buffer[i] = sep;
      continue;
    }
    if (i > 0 && buffer[i - 1] == ':') {
      buffer[i] = sep;
      continue;
    }

    char saved = buffer[i];
    buffer[i] = '\0';

    if (buffer[0] != '\0') {
      String8 path_str = string8_create_from_cstr((const uint8_t *)buffer,
                                                  string_length(buffer));
      FilePathType path_type = FILE_PATH_TYPE_RELATIVE;
      if (buffer[0] == '/' || buffer[0] == '\\' ||
          (string_length(buffer) >= 3 && buffer[1] == ':' &&
           (buffer[2] == '/' || buffer[2] == '\\'))) {
        path_type = FILE_PATH_TYPE_ABSOLUTE;
      }

      FilePath file_path = {.path = path_str, .type = path_type};
      if (!file_create_directory(&file_path)) {
        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
        return false_v;
      }
    }
    buffer[i] = sep;
  }

  uint64_t final_len = string_length(buffer);
  if (final_len > 1 &&
      (buffer[final_len - 1] == '/' || buffer[final_len - 1] == '\\')) {
    if (!(final_len == 3 && buffer[1] == ':')) {
      buffer[final_len - 1] = '\0';
    }
  }

  String8 final_path_str =
      string8_create_from_cstr((const uint8_t *)buffer, string_length(buffer));
  FilePathType final_path_type = FILE_PATH_TYPE_RELATIVE;
  if (buffer[0] == '/' || buffer[0] == '\\' ||
      (string_length(buffer) >= 3 && buffer[1] == ':' &&
       (buffer[2] == '/' || buffer[2] == '\\'))) {
    final_path_type = FILE_PATH_TYPE_ABSOLUTE;
  }
  FilePath final_file_path = {.path = final_path_str, .type = final_path_type};

  bool8_t result = file_create_directory(&final_file_path);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return result;
}

FileError file_open(const FilePath *path, FileMode mode,
                    FileHandle *out_handle) {
  DWORD access = 0;
  DWORD share = FILE_SHARE_READ;
  DWORD disposition = OPEN_EXISTING;
  DWORD flags = FILE_ATTRIBUTE_NORMAL;

  bool8_t has_read = bitset8_is_set(&mode, FILE_MODE_READ);
  bool8_t has_write = bitset8_is_set(&mode, FILE_MODE_WRITE);
  bool8_t has_append = bitset8_is_set(&mode, FILE_MODE_APPEND);
  bool8_t has_create = bitset8_is_set(&mode, FILE_MODE_CREATE);
  bool8_t has_truncate = bitset8_is_set(&mode, FILE_MODE_TRUNCATE);

  if (has_read)
    access |= GENERIC_READ;
  if (has_write)
    access |= GENERIC_WRITE;

  if (has_create && has_truncate) {
    disposition = CREATE_ALWAYS;
  } else if (has_write && has_truncate) {
    disposition = CREATE_ALWAYS; // Implicit Create on Truncate
  } else if (has_write && !has_read && !has_append) {
    disposition = CREATE_ALWAYS; // Implicit Create on Write-Only ("w")
  } else if (has_create || has_append) {
    disposition = OPEN_ALWAYS; // Create if missing, open if exists
  } else if (has_truncate) {
    disposition = TRUNCATE_EXISTING; // Only truncate if exists (rare "w+" case
                                     // without create?)
  } else {
    disposition = OPEN_EXISTING;
  }

  HANDLE hFile = CreateFileA((char *)path->path.str, access, share, NULL,
                             disposition, flags, NULL);
  if (hFile == INVALID_HANDLE_VALUE) {
    // Optional: log_error with GetLastError()
    return FILE_ERROR_OPEN_FAILED;
  }

  if (has_append) {
    SetFilePointer(hFile, 0, NULL, FILE_END);
  }

  out_handle->handle = hFile;
  out_handle->path = path;
  out_handle->mode = mode;

  return FILE_ERROR_NONE;
}

void file_close(FileHandle *handle) {
  if (handle && handle->handle) {
    CloseHandle((HANDLE)handle->handle);
    handle->handle = NULL;
  }
}

FileError file_write(FileHandle *handle, uint64_t size, const uint8_t *buffer,
                     uint64_t *bytes_written) {
  *bytes_written = 0;
  const uint8_t *current = buffer;
  uint64_t remaining = size;

  while (remaining > 0) {
    DWORD chunk = (DWORD)(remaining > 0xFFFFFFFF ? 0xFFFFFFFF : remaining);
    DWORD written = 0;
    if (!WriteFile((HANDLE)handle->handle, current, chunk, &written, NULL))
      return FILE_ERROR_IO_ERROR;
    *bytes_written += written;
    if (written < chunk)
      break; // Partial write
    current += written;
    remaining -= written;
  }
  return FILE_ERROR_NONE;
}

FileError file_read(FileHandle *handle, VkrAllocator *allocator, uint64_t size,
                    uint64_t *bytes_read, uint8_t **out_buffer) {
  *out_buffer =
      vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  *bytes_read = 0;
  uint8_t *current = *out_buffer;
  uint64_t remaining = size;

  while (remaining > 0) {
    DWORD chunk = (DWORD)(remaining > 0xFFFFFFFF ? 0xFFFFFFFF : remaining);
    DWORD read_len = 0;
    if (!ReadFile((HANDLE)handle->handle, current, chunk, &read_len, NULL))
      return FILE_ERROR_IO_ERROR;
    *bytes_read += read_len;
    if (read_len < chunk)
      break; // EOF or partial read
    current += read_len;
    remaining -= read_len;
  }
  return FILE_ERROR_NONE;
}

FileError file_read_all(FileHandle *handle, VkrAllocator *allocator,
                        uint8_t **out_buffer, uint64_t *bytes_read) {
  HANDLE hFile = (HANDLE)handle->handle;
  LARGE_INTEGER fileSize;
  if (!GetFileSizeEx(hFile, &fileSize))
    return FILE_ERROR_IO_ERROR;

  LARGE_INTEGER currentPos = {0}, zero = {0};
  if (!SetFilePointerEx(hFile, zero, &currentPos, FILE_CURRENT))
    return FILE_ERROR_IO_ERROR;

  uint64_t bytesToRead = (uint64_t)(fileSize.QuadPart - currentPos.QuadPart);
  *out_buffer = vkr_allocator_alloc(allocator, bytesToRead,
                                    VKR_ALLOCATOR_MEMORY_TAG_FILE);

  *bytes_read = 0;
  uint8_t *current = *out_buffer;
  uint64_t remaining = bytesToRead;

  while (remaining > 0) {
    DWORD chunk = (DWORD)(remaining > 0xFFFFFFFF ? 0xFFFFFFFF : remaining);
    DWORD read_len = 0;
    if (!ReadFile(hFile, current, chunk, &read_len, NULL))
      return FILE_ERROR_IO_ERROR;
    *bytes_read += read_len;
    if (read_len < chunk)
      break;
    current += read_len;
    remaining -= read_len;
  }
  return FILE_ERROR_NONE;
}

FileError file_read_line(FileHandle *handle, VkrAllocator *allocator,
                         VkrAllocator *line_allocator, uint64_t max_line_length,
                         String8 *out_line) {
  HANDLE hFile = (HANDLE)handle->handle;
  VkrAllocator *target_alloc = line_allocator ? line_allocator : allocator;

  uint8_t *result_buf = vkr_allocator_alloc(target_alloc, max_line_length + 1,
                                            VKR_ALLOCATOR_MEMORY_TAG_STRING);

  char chunk[128];
  uint64_t total_len = 0;

  while (total_len < max_line_length) {
    LARGE_INTEGER startPos, zero = {0};
    SetFilePointerEx(hFile, zero, &startPos, FILE_CURRENT);

    DWORD read_len = 0;
    if (!ReadFile(hFile, chunk, sizeof(chunk), &read_len, NULL) ||
        read_len == 0)
      break;

    int newline_idx = -1;
    for (DWORD i = 0; i < read_len; i++) {
      if (chunk[i] == '\n') {
        newline_idx = i;
        break;
      }
    }

    // FIX: Include the newline in the count
    uint64_t amount_available =
        (newline_idx != -1) ? (uint64_t)(newline_idx + 1) : (uint64_t)read_len;
    uint64_t amount_to_copy = amount_available;

    if (total_len + amount_to_copy > max_line_length) {
      amount_to_copy = max_line_length - total_len;
    }

    MemCopy(result_buf + total_len, chunk, amount_to_copy);
    total_len += amount_to_copy;

    if (newline_idx != -1 || total_len == max_line_length) {
      LARGE_INTEGER move;
      move.QuadPart = startPos.QuadPart + amount_to_copy;
      SetFilePointerEx(hFile, move, NULL, FILE_BEGIN);
      break;
    }
  }

  if (total_len == 0)
    return FILE_ERROR_EOF;

  result_buf[total_len] = '\0';
  *out_line = (String8){.str = result_buf, .length = total_len};
  return FILE_ERROR_NONE;
}

FileError file_write_line(FileHandle *handle, const String8 *text) {
  uint64_t written;
  if (file_write(handle, text->length, text->str, &written) != FILE_ERROR_NONE)
    return FILE_ERROR_IO_ERROR;
  DWORD nl_written;
  if (!WriteFile((HANDLE)handle->handle, "\n", 1, &nl_written, NULL))
    return FILE_ERROR_IO_ERROR;
  return FILE_ERROR_NONE;
}

FileError file_read_string(FileHandle *handle, VkrAllocator *allocator,
                           String8 *out_data) {
  uint8_t *buffer = NULL;
  uint64_t bytes_read = 0;
  if (file_read_all(handle, allocator, &buffer, &bytes_read) != FILE_ERROR_NONE)
    return FILE_ERROR_IO_ERROR;

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
    uint8_t *aligned = vkr_allocator_alloc(allocator, *out_size,
                                           VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    MemCopy(aligned, *out_data, *out_size);
    *out_data = aligned;
  }
  return err;
}

#endif