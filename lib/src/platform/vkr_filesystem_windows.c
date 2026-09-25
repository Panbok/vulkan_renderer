#include "filesystem/filesystem.h"
#include "platform/vkr_filesystem_internal.h"

#if defined(PLATFORM_WINDOWS)

#include "core/logger.h"

#include <errno.h>
#include <wchar.h>

#define VKR_WINDOWS_PATH_WCHARS 32768u

/* Native conversion is bounded cold-path stack storage. No path bytes or
 * platform buffer escape an operation. Normalize before adding the long-path
 * prefix because Win32 extended paths do not interpret dot components. */
bool8_t file_windows_native_path(const FilePath *path,
                                 wchar_t output[VKR_WINDOWS_PATH_WCHARS]) {
  if (!path || !path->path.str || !path->path.length ||
      path->path.length > INT_MAX ||
      memchr(path->path.str, 0, path->path.length)) {
    return false_v;
  }
  wchar_t input[VKR_WINDOWS_PATH_WCHARS];
  int32_t length = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)path->path.str,
      (int32_t)path->path.length, input, VKR_WINDOWS_PATH_WCHARS - 1);
  if (length <= 0) {
    return false_v;
  }
  input[length] = 0;
  for (int32_t i = 0; i < length; ++i) {
    if (input[i] == L'/') {
      input[i] = L'\\';
    }
  }
  if (length >= 4 && !wcsncmp(input, L"\\\\.\\", 4)) {
    return false_v;
  }
  if (length >= 8 && !_wcsnicmp(input, L"\\\\?\\UNC\\", 8)) {
    MemCopy(input + 2, input + 8, ((uint64_t)length - 8 + 1) * sizeof(wchar_t));
    input[0] = L'\\';
    input[1] = L'\\';
  } else if (length >= 4 && !wcsncmp(input, L"\\\\?\\", 4)) {
    if (length < 7 || input[5] != L':' || input[6] != L'\\') {
      return false_v;
    }
    MemCopy(input, input + 4, ((uint64_t)length - 4 + 1) * sizeof(wchar_t));
  }
  if (input[0] && input[1] == L':' && input[2] != L'\\') {
    return false_v;
  }
  DWORD used =
      GetFullPathNameW(input, VKR_WINDOWS_PATH_WCHARS - 8, output + 8, NULL);
  if (!used || used >= VKR_WINDOWS_PATH_WCHARS - 8) {
    return false_v;
  }
  if (output[8] == L'\\' && output[9] == L'\\') {
    MemCopy(output + 8, output + 10,
            ((uint64_t)used - 2 + 1) * sizeof(wchar_t));
    MemCopy(output, L"\\\\?\\UNC\\", 8 * sizeof(wchar_t));
  } else {
    if (used < 3 || output[9] != L':' || output[10] != L'\\') {
      return false_v;
    }
    MemCopy(output + 4, output + 8, ((uint64_t)used + 1) * sizeof(wchar_t));
    MemCopy(output, L"\\\\?\\", 4 * sizeof(wchar_t));
  }
  return true_v;
}

FILE *file_fopen(const char *utf8_path, const char *mode) {
  if (!utf8_path || !mode) {
    errno = EINVAL;
    return NULL;
  }
  FilePath path = {
      .path = {.str = (uint8_t *)utf8_path, .length = strlen(utf8_path)}};
  wchar_t native[VKR_WINDOWS_PATH_WCHARS];
  wchar_t native_mode[16];
  if (!file_windows_native_path(&path, native) ||
      !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1, native_mode,
                           ArrayCount(native_mode))) {
    errno = EINVAL;
    return NULL;
  }
  return _wfopen(native, native_mode);
}

static FileError fs_windows_error(DWORD error) {
  switch (error) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
    return FILE_ERROR_NOT_FOUND;
  case ERROR_ACCESS_DENIED:
  case ERROR_SHARING_VIOLATION:
    return FILE_ERROR_ACCESS_DENIED;
  case ERROR_ALREADY_EXISTS:
  case ERROR_FILE_EXISTS:
    return FILE_ERROR_ALREADY_EXISTS;
  case ERROR_INVALID_NAME:
  case ERROR_FILENAME_EXCED_RANGE:
    return FILE_ERROR_INVALID_PATH;
  default:
    return FILE_ERROR_IO_ERROR;
  }
}

bool8_t file_exists(const FilePath *path) {
  wchar_t native[VKR_WINDOWS_PATH_WCHARS];
  return file_windows_native_path(path, native) &&
         GetFileAttributesW(native) != INVALID_FILE_ATTRIBUTES;
}

FileError file_stats(const FilePath *path, FileStats *out_stats) {
  wchar_t native[VKR_WINDOWS_PATH_WCHARS];
  if (!out_stats || !file_windows_native_path(path, native)) {
    return FILE_ERROR_INVALID_PATH;
  }
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(native, GetFileExInfoStandard, &data)) {
    return fs_windows_error(GetLastError());
  }
  LARGE_INTEGER size;
  size.HighPart = data.nFileSizeHigh;
  size.LowPart = data.nFileSizeLow;
  out_stats->size = (uint64_t)size.QuadPart;
  ULARGE_INTEGER time;
  time.LowPart = data.ftLastWriteTime.dwLowDateTime;
  time.HighPart = data.ftLastWriteTime.dwHighDateTime;
  out_stats->last_modified = (time.QuadPart / 10000000ULL) - 11644473600ULL;
  return FILE_ERROR_NONE;
}

bool8_t file_create_directory(const FilePath *path) {
  wchar_t native[VKR_WINDOWS_PATH_WCHARS];
  if (!file_windows_native_path(path, native)) {
    return false_v;
  }
  if (CreateDirectoryW(native, NULL)) {
    return true_v;
  }
  if (GetLastError() != ERROR_ALREADY_EXISTS) {
    return false_v;
  }
  DWORD attributes = GetFileAttributesW(native);
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

FileError file_create_directory_exclusive(const FilePath *path) {
  wchar_t native[VKR_WINDOWS_PATH_WCHARS];
  if (!file_windows_native_path(path, native)) {
    return FILE_ERROR_INVALID_PATH;
  }
  return CreateDirectoryW(native, NULL) ? FILE_ERROR_NONE
                                        : fs_windows_error(GetLastError());
}

FileError file_path_resolve(const FilePath *path, char *out_path,
                            uint64_t out_capacity) {
  wchar_t native[VKR_WINDOWS_PATH_WCHARS];
  if (!out_path || !out_capacity || out_capacity > INT_MAX ||
      !file_windows_native_path(path, native)) {
    return FILE_ERROR_INVALID_PATH;
  }
  out_path[0] = 0;
  HANDLE handle =
      CreateFileW(native, FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    return fs_windows_error(GetLastError());
  }
  wchar_t resolved[VKR_WINDOWS_PATH_WCHARS];
  DWORD length =
      GetFinalPathNameByHandleW(handle, resolved, ArrayCount(resolved),
                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  CloseHandle(handle);
  if (!length || length >= ArrayCount(resolved)) {
    return FILE_ERROR_INVALID_PATH;
  }
  const wchar_t *source = resolved;
  if (length >= 8 && !wcsncmp(source, L"\\\\?\\UNC\\", 8)) {
    resolved[6] = L'\\';
    source += 6;
  } else if (length >= 4 && !wcsncmp(source, L"\\\\?\\", 4)) {
    source += 4;
  }
  return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source, -1,
                             out_path, (int32_t)out_capacity, NULL, NULL) > 0
             ? FILE_ERROR_NONE
             : FILE_ERROR_INVALID_PATH;
}

bool8_t file_path_equals(const char *lhs, const char *rhs) {
  if (!lhs || !rhs) {
    return false_v;
  }
  FilePath left = {
      .path = string8_create_from_cstr((const uint8_t *)lhs, strlen(lhs))};
  FilePath right = {
      .path = string8_create_from_cstr((const uint8_t *)rhs, strlen(rhs))};
  wchar_t a[VKR_WINDOWS_PATH_WCHARS];
  wchar_t b[VKR_WINDOWS_PATH_WCHARS];
  return file_windows_native_path(&left, a) &&
         file_windows_native_path(&right, b) &&
         CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

bool8_t file_path_starts_with(const char *path, const char *prefix) {
  if (!path || !prefix) {
    return false_v;
  }
  FilePath full = {
      .path = string8_create_from_cstr((const uint8_t *)path, strlen(path))};
  FilePath start = {.path = string8_create_from_cstr((const uint8_t *)prefix,
                                                     strlen(prefix))};
  wchar_t a[VKR_WINDOWS_PATH_WCHARS];
  wchar_t b[VKR_WINDOWS_PATH_WCHARS];
  if (!file_windows_native_path(&full, a) ||
      !file_windows_native_path(&start, b)) {
    return false_v;
  }
  size_t length = wcslen(b);
  return wcslen(a) >= length &&
         CompareStringOrdinal(a, (int32_t)length, b, (int32_t)length, TRUE) ==
             CSTR_EQUAL;
}

bool8_t file_ensure_directory(VkrAllocator *allocator, const String8 *path) {
  (void)allocator;
  if (!path) {
    return false_v;
  }
  wchar_t native[VKR_WINDOWS_PATH_WCHARS];
  FilePath input = {.path = *path};
  if (!file_windows_native_path(&input, native)) {
    return false_v;
  }
  size_t start = 7; // \\?\C:\ drive root
  if (!wcsncmp(native, L"\\\\?\\UNC\\", 8)) {
    start = 8;
    for (uint32_t component = 0; component < 2; ++component) {
      while (native[start] && native[start] != L'\\') {
        ++start;
      }
      if (native[start]) {
        ++start;
      }
    }
  }
  for (size_t i = start;; ++i) {
    wchar_t saved = native[i];
    if (saved != L'\\' && saved != 0) {
      continue;
    }
    native[i] = 0;
    if (!CreateDirectoryW(native, NULL)) {
      DWORD error = GetLastError();
      DWORD attributes = GetFileAttributesW(native);
      if (error != ERROR_ALREADY_EXISTS ||
          attributes == INVALID_FILE_ATTRIBUTES ||
          !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false_v;
      }
    }
    native[i] = saved;
    if (!saved) {
      break;
    }
  }
  return true_v;
}

FileError file_open(const FilePath *path, FileMode mode,
                    FileHandle *out_handle) {
  if (!path || !path->path.str || !path->path.length || !out_handle) {
    return FILE_ERROR_INVALID_PATH;
  }
  DWORD access = 0;
  DWORD share = FILE_SHARE_READ;
  DWORD disposition = OPEN_EXISTING;
  DWORD flags = FILE_ATTRIBUTE_NORMAL;

  bool8_t has_read = bitset8_is_set(&mode, FILE_MODE_READ);
  bool8_t has_write = bitset8_is_set(&mode, FILE_MODE_WRITE);
  bool8_t has_append = bitset8_is_set(&mode, FILE_MODE_APPEND);
  bool8_t has_create = bitset8_is_set(&mode, FILE_MODE_CREATE);
  bool8_t has_truncate = bitset8_is_set(&mode, FILE_MODE_TRUNCATE);
  bool8_t has_exclusive = bitset8_is_set(&mode, FILE_MODE_EXCLUSIVE);

  if (has_exclusive && (!has_create || has_truncate || has_append)) {
    return FILE_ERROR_INVALID_MODE;
  }

  if (has_read) {
    access |= GENERIC_READ;
  }
  if (has_write) {
    access |= GENERIC_WRITE;
  }

  if (has_exclusive) {
    disposition = CREATE_NEW;
  } else if (has_create && has_truncate) {
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

  wchar_t native[VKR_WINDOWS_PATH_WCHARS];
  if (!file_windows_native_path(path, native)) {
    return FILE_ERROR_INVALID_PATH;
  }
  HANDLE hFile =
      CreateFileW(native, access, share, NULL, disposition, flags, NULL);
  if (hFile == INVALID_HANDLE_VALUE) {
    return fs_windows_error(GetLastError());
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
  if (!handle || !handle->handle || (!buffer && size > 0u) || !bytes_written) {
    return FILE_ERROR_INVALID_HANDLE;
  }
  *bytes_written = 0;
  const uint8_t *current = buffer;
  uint64_t remaining = size;

  while (remaining > 0) {
    DWORD chunk = (DWORD)(remaining > 0xFFFFFFFF ? 0xFFFFFFFF : remaining);
    DWORD written = 0;
    if (!WriteFile((HANDLE)handle->handle, current, chunk, &written, NULL) ||
        written == 0) {
      return FILE_ERROR_IO_ERROR;
    }
    *bytes_written += written;
    current += written;
    remaining -= written;
  }
  return FILE_ERROR_NONE;
}

FileError file_read_into(FileHandle *handle, void *buffer, uint64_t size,
                         uint64_t *bytes_read) {
  if (!handle || !handle->handle || (!buffer && size > 0u) || !bytes_read) {
    return FILE_ERROR_INVALID_HANDLE;
  }
  *bytes_read = 0;
  uint8_t *current = buffer;
  uint64_t remaining = size;

  while (remaining > 0) {
    DWORD chunk = (DWORD)(remaining > 0xFFFFFFFF ? 0xFFFFFFFF : remaining);
    DWORD read_len = 0;
    if (!ReadFile((HANDLE)handle->handle, current, chunk, &read_len, NULL)) {
      return FILE_ERROR_IO_ERROR;
    }
    *bytes_read += read_len;
    if (read_len < chunk) {
      break; // EOF or partial read
    }
    current += read_len;
    remaining -= read_len;
  }
  return FILE_ERROR_NONE;
}

FileError file_remaining_size(FileHandle *handle, uint64_t *out_size) {
  if (!handle || !handle->handle) {
    return FILE_ERROR_INVALID_HANDLE;
  }
  const HANDLE file = (HANDLE)handle->handle;
  LARGE_INTEGER size;
  LARGE_INTEGER position;
  const LARGE_INTEGER zero = {0};
  if (!GetFileSizeEx(file, &size) ||
      !SetFilePointerEx(file, zero, &position, FILE_CURRENT) ||
      position.QuadPart < 0 || position.QuadPart > size.QuadPart) {
    return FILE_ERROR_IO_ERROR;
  }
  *out_size = (uint64_t)(size.QuadPart - position.QuadPart);
  return FILE_ERROR_NONE;
}

FileError file_sync(FileHandle *handle) {
  if (!handle || !handle->handle) {
    return FILE_ERROR_INVALID_HANDLE;
  }
  return FlushFileBuffers((HANDLE)handle->handle) ? FILE_ERROR_NONE
                                                  : FILE_ERROR_IO_ERROR;
}

FileError file_remove(const FilePath *path) {
  wchar_t native[VKR_WINDOWS_PATH_WCHARS];
  if (!file_windows_native_path(path, native)) {
    return FILE_ERROR_INVALID_PATH;
  }
  return DeleteFileW(native) ? FILE_ERROR_NONE
                             : fs_windows_error(GetLastError());
}

FileError file_rename(const FilePath *source, const FilePath *destination,
                      bool8_t overwrite) {
  wchar_t from[VKR_WINDOWS_PATH_WCHARS];
  wchar_t to[VKR_WINDOWS_PATH_WCHARS];
  if (!file_windows_native_path(source, from) ||
      !file_windows_native_path(destination, to)) {
    return FILE_ERROR_INVALID_PATH;
  }
  DWORD flags = MOVEFILE_WRITE_THROUGH;
  if (overwrite) {
    flags |= MOVEFILE_REPLACE_EXISTING;
  }
  return MoveFileExW(from, to, flags) ? FILE_ERROR_NONE
                                      : fs_windows_error(GetLastError());
}

FileError file_clone(const FilePath *source, const FilePath *destination) {
  if (!source || !source->path.str || !destination || !destination->path.str) {
    return FILE_ERROR_INVALID_PATH;
  }
  // Block cloning needs ReFS/Dev Drive extent duplication; callers copy.
  return FILE_ERROR_UNSUPPORTED;
}

FileError file_read_line(FileHandle *handle, VkrAllocator *allocator,
                         VkrAllocator *line_allocator, uint64_t max_line_length,
                         String8 *out_line) {
  *out_line = (String8){0};
  if (!handle || !handle->handle) {
    return FILE_ERROR_INVALID_HANDLE;
  }
  HANDLE hFile = (HANDLE)handle->handle;
  if (max_line_length == 0 || max_line_length == UINT64_MAX) {
    return FILE_ERROR_LINE_TOO_LONG;
  }
  VkrAllocator *target_alloc = line_allocator ? line_allocator : allocator;
  FileError error = FILE_ERROR_NONE;

  uint8_t *result_buf = vkr_allocator_alloc(target_alloc, max_line_length + 1,
                                            VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!result_buf) {
    return FILE_ERROR_OUT_OF_MEMORY;
  }

  char chunk[128];
  uint64_t total_len = 0;

  while (total_len < max_line_length) {
    LARGE_INTEGER startPos, zero = {0};
    if (!SetFilePointerEx(hFile, zero, &startPos, FILE_CURRENT)) {
      error = FILE_ERROR_IO_ERROR;
      goto cleanup;
    }

    DWORD read_len = 0;
    if (!ReadFile(hFile, chunk, sizeof(chunk), &read_len, NULL)) {
      error = FILE_ERROR_IO_ERROR;
      goto cleanup;
    }
    if (read_len == 0) {
      break;
    }

    int newline_idx = -1;
    for (DWORD i = 0; i < read_len; i++) {
      if (chunk[i] == '\n') {
        newline_idx = i;
        break;
      }
    }

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
      if (!SetFilePointerEx(hFile, move, NULL, FILE_BEGIN)) {
        error = FILE_ERROR_IO_ERROR;
        goto cleanup;
      }
      break;
    }
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

#endif
