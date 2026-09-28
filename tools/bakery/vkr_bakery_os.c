#include "vkr_bakery_os.h"

#include "core/vkr_atomic.h"
#include "core/vkr_hash.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
/* psapi.h requires the windows.h declarations above. */
#include <io.h>
#include <psapi.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

// =============================================================================
// Native path conversion
// =============================================================================

#if defined(_WIN32)
/* One extended native path per call site; 32768 wide characters is the
 * Windows path limit, so storage is heap-owned for the call's duration. */
vkr_internal wchar_t *vkr_bakery_native(const char *path) {
  wchar_t *native = (wchar_t *)malloc(sizeof(wchar_t) * 32768u);
  if (!native) {
    return NULL;
  }
  FilePath value = {.path = {.str = (uint8_t *)path, .length = strlen(path)}};
  if (!file_windows_native_path(&value, native)) {
    free(native);
    return NULL;
  }
  return native;
}
#endif

// =============================================================================
// Stat and directories
// =============================================================================

bool8_t vkr_bakery_stat(const char *path, VkrBakeryStat *out_stat) {
  MemZero(out_stat, sizeof(*out_stat));
#if defined(_WIN32)
  wchar_t *native = vkr_bakery_native(path);
  if (!native) {
    return false_v;
  }
  HANDLE handle =
      CreateFileW(native, FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(native);
  if (handle == INVALID_HANDLE_VALUE) {
    return true_v; /* Missing is a valid answer. */
  }
  BY_HANDLE_FILE_INFORMATION info;
  const BOOL ok = GetFileInformationByHandle(handle, &info);
  CloseHandle(handle);
  if (!ok) {
    return false_v;
  }
  out_stat->exists = true_v;
  out_stat->is_directory =
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? true_v : false_v;
  out_stat->size =
      ((uint64_t)info.nFileSizeHigh << 32) | (uint64_t)info.nFileSizeLow;
  const uint64_t ticks = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) |
                         (uint64_t)info.ftLastWriteTime.dwLowDateTime;
  /* FILETIME counts 100 ns ticks from 1601; shift to the Unix epoch. */
  out_stat->mtime_ns = ((int64_t)ticks - 116444736000000000LL) * 100LL;
  out_stat->device = info.dwVolumeSerialNumber;
  out_stat->file_id =
      ((uint64_t)info.nFileIndexHigh << 32) | (uint64_t)info.nFileIndexLow;
  return true_v;
#else
  struct stat info;
  if (stat(path, &info) != 0) {
    return errno == ENOENT || errno == ENOTDIR;
  }
  out_stat->exists = true_v;
  out_stat->is_directory = S_ISDIR(info.st_mode) ? true_v : false_v;
  out_stat->size = (uint64_t)info.st_size;
#if defined(__APPLE__)
  out_stat->mtime_ns = (int64_t)info.st_mtimespec.tv_sec * 1000000000LL +
                       (int64_t)info.st_mtimespec.tv_nsec;
#else
  out_stat->mtime_ns = (int64_t)info.st_mtim.tv_sec * 1000000000LL +
                       (int64_t)info.st_mtim.tv_nsec;
#endif
  out_stat->device = (uint64_t)info.st_dev;
  out_stat->file_id = (uint64_t)info.st_ino;
  return true_v;
#endif
}

bool8_t vkr_bakery_is_file(const char *path) {
  VkrBakeryStat info;
  return vkr_bakery_stat(path, &info) && info.exists && !info.is_directory;
}

bool8_t vkr_bakery_is_directory(const char *path) {
  VkrBakeryStat info;
  return vkr_bakery_stat(path, &info) && info.exists && info.is_directory;
}

typedef struct VkrBakeryDirectoryEntry {
  char *name;
  bool8_t is_directory;
} VkrBakeryDirectoryEntry;

vkr_internal int vkr_bakery_compare_entries(const void *lhs, const void *rhs) {
  const VkrBakeryDirectoryEntry *a = (const VkrBakeryDirectoryEntry *)lhs;
  const VkrBakeryDirectoryEntry *b = (const VkrBakeryDirectoryEntry *)rhs;
  return strcmp(a->name, b->name);
}

bool8_t vkr_bakery_list_directory(const char *path,
                                  VkrBakeryDirectoryVisitor visit,
                                  void *context) {
  VkrBakeryDirectoryEntry *entries = NULL;
  uint32_t count = 0u;
  uint32_t capacity = 0u;
  bool8_t ok = true_v;
#if defined(_WIN32)
  char pattern[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_join(pattern, sizeof(pattern), path, "*")) {
    return false_v;
  }
  wchar_t *native = vkr_bakery_native(pattern);
  if (!native) {
    return false_v;
  }
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW(native, &data);
  free(native);
  if (find == INVALID_HANDLE_VALUE) {
    return GetLastError() == ERROR_FILE_NOT_FOUND;
  }
  do {
    if (wcscmp(data.cFileName, L".") == 0 ||
        wcscmp(data.cFileName, L"..") == 0) {
      continue;
    }
    char name[1024];
    if (!WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name,
                             (int)sizeof(name), NULL, NULL)) {
      ok = false_v;
      break;
    }
    const bool8_t is_directory =
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? true_v : false_v;
#else
  DIR *directory = opendir(path);
  if (!directory) {
    return false_v;
  }
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    const char *name = entry->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    char child[VKR_BAKERY_PATH_CAPACITY];
    if (!vkr_bakery_path_join(child, sizeof(child), path, name)) {
      ok = false_v;
      break;
    }
    const bool8_t is_directory = vkr_bakery_is_directory(child);
#endif
    if (count == capacity) {
      capacity = capacity ? capacity * 2u : 64u;
      VkrBakeryDirectoryEntry *grown = (VkrBakeryDirectoryEntry *)realloc(
          entries, sizeof(*entries) * capacity);
      if (!grown) {
        ok = false_v;
        break;
      }
      entries = grown;
    }
    entries[count].name = strdup(name);
    entries[count].is_directory = is_directory;
    if (!entries[count].name) {
      ok = false_v;
      break;
    }
    count += 1u;
#if defined(_WIN32)
  } while (FindNextFileW(find, &data));
  FindClose(find);
#else
  }
  closedir(directory);
#endif
  if (ok && count) {
    qsort(entries, count, sizeof(*entries), vkr_bakery_compare_entries);
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (ok && !visit(context, entries[i].name, entries[i].is_directory)) {
      ok = false_v;
    }
    free(entries[i].name);
  }
  free(entries);
  return ok;
}

vkr_internal bool8_t vkr_bakery_make_directory(const char *path) {
#if defined(_WIN32)
  wchar_t *native = vkr_bakery_native(path);
  if (!native) {
    return false_v;
  }
  const BOOL created = CreateDirectoryW(native, NULL);
  const DWORD error = GetLastError();
  free(native);
  return created || error == ERROR_ALREADY_EXISTS;
#else
  return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

/* Host separators: Windows accepts both, POSIX only '/'. */
vkr_internal bool8_t vkr_bakery_is_separator(char c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

bool8_t vkr_bakery_make_directories(const char *path) {
  char partial[VKR_BAKERY_PATH_CAPACITY];
  const uint64_t length = strlen(path);
  if (length == 0u || length >= sizeof(partial)) {
    return length == 0u;
  }
  MemCopy(partial, path, length + 1u);
  for (uint64_t i = 1u; i <= length; ++i) {
    if (vkr_bakery_is_separator(partial[i]) || partial[i] == 0) {
      const char saved = partial[i];
      partial[i] = 0;
      /* Skip a Windows drive root such as "C:". */
      const bool8_t drive = i == 2u && partial[1] == ':';
      if (!drive && !vkr_bakery_is_directory(partial) &&
          !vkr_bakery_make_directory(partial)) {
        return false_v;
      }
      partial[i] = saved;
    }
  }
  return vkr_bakery_is_directory(path);
}

// =============================================================================
// Whole-file IO
// =============================================================================

bool8_t vkr_bakery_read_file(const char *path, uint64_t max_bytes,
                             uint8_t **out_data, uint64_t *out_length) {
  *out_data = NULL;
  *out_length = 0u;
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return false_v;
  }
  bool8_t ok = false_v;
  uint8_t *data = NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    goto cleanup;
  }
  const long long size = (long long)ftell(file);
  if (size < 0 || (max_bytes && (uint64_t)size > max_bytes) ||
      fseek(file, 0, SEEK_SET) != 0) {
    goto cleanup;
  }
  data = (uint8_t *)malloc((size_t)size + 1u);
  if (!data) {
    goto cleanup;
  }
  if (size && fread(data, 1u, (size_t)size, file) != (size_t)size) {
    goto cleanup;
  }
  data[size] = 0u;
  *out_data = data;
  *out_length = (uint64_t)size;
  data = NULL;
  ok = true_v;
cleanup:
  free(data);
  fclose(file);
  return ok;
}

vkr_internal bool8_t vkr_bakery_flush_file(FILE *file) {
  if (fflush(file) != 0) {
    return false_v;
  }
#if defined(_WIN32)
  return FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(file))) != 0;
#else
  return fsync(fileno(file)) == 0;
#endif
}

bool8_t vkr_bakery_write_file_atomic(const char *path, const void *data,
                                     uint64_t length) {
  char directory[VKR_BAKERY_PATH_CAPACITY];
  char temporary[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  if (directory[0] && !vkr_bakery_make_directories(directory)) {
    return false_v;
  }
  vkr_bakery_temp_path(temporary, sizeof(temporary),
                       directory[0] ? directory : ".",
                       vkr_bakery_path_name(path));
  FILE *file = file_fopen(temporary, "wb");
  if (!file) {
    return false_v;
  }
  bool8_t ok = (length == 0u ||
                fwrite(data, 1u, (size_t)length, file) == (size_t)length) &&
               vkr_bakery_flush_file(file);
  ok = fclose(file) == 0 && ok;
  ok = ok && vkr_bakery_rename(temporary, path, true_v);
  if (!ok) {
    (void)vkr_bakery_remove_file(temporary);
  }
  return ok;
}

bool8_t vkr_bakery_append_file(const char *path, const void *data,
                               uint64_t length) {
  FILE *file = file_fopen(path, "ab");
  if (!file) {
    return false_v;
  }
  bool8_t ok =
      length == 0u || fwrite(data, 1u, (size_t)length, file) == (size_t)length;
  ok = fclose(file) == 0 && ok;
  return ok;
}

bool8_t vkr_bakery_remove_file(const char *path) {
  FilePath value = {.path = {.str = (uint8_t *)path, .length = strlen(path)}};
  const FileError error = file_remove(&value);
  return error == FILE_ERROR_NONE || error == FILE_ERROR_NOT_FOUND;
}

typedef struct VkrBakeryRemoveContext {
  const char *parent;
  bool8_t ok;
} VkrBakeryRemoveContext;

vkr_internal bool8_t vkr_bakery_remove_visit(void *context, const char *name,
                                             bool8_t is_directory) {
  VkrBakeryRemoveContext *remove = (VkrBakeryRemoveContext *)context;
  char child[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_join(child, sizeof(child), remove->parent, name)) {
    remove->ok = false_v;
    return true_v;
  }
  (void)is_directory;
  if (!vkr_bakery_remove_tree(child)) {
    remove->ok = false_v;
  }
  return true_v;
}

bool8_t vkr_bakery_remove_tree(const char *path) {
  VkrBakeryStat info;
  if (!vkr_bakery_stat(path, &info)) {
    return false_v;
  }
  if (!info.exists) {
    return true_v;
  }
  if (!info.is_directory) {
    return vkr_bakery_remove_file(path);
  }
  VkrBakeryRemoveContext context = {.parent = path, .ok = true_v};
  if (!vkr_bakery_list_directory(path, vkr_bakery_remove_visit, &context) ||
      !context.ok) {
    return false_v;
  }
#if defined(_WIN32)
  wchar_t *native = vkr_bakery_native(path);
  if (!native) {
    return false_v;
  }
  const BOOL removed = RemoveDirectoryW(native);
  free(native);
  return removed != 0;
#else
  return rmdir(path) == 0 || errno == ENOENT;
#endif
}

bool8_t vkr_bakery_rename(const char *source, const char *destination,
                          bool8_t overwrite) {
  FilePath from = {
      .path = {.str = (uint8_t *)source, .length = strlen(source)}};
  FilePath to = {
      .path = {.str = (uint8_t *)destination, .length = strlen(destination)}};
  return file_rename(&from, &to, overwrite) == FILE_ERROR_NONE;
}

vkr_internal bool8_t vkr_bakery_copy_bytes(const char *source,
                                           const char *destination) {
  FILE *input = file_fopen(source, "rb");
  if (!input) {
    return false_v;
  }
  FILE *output = file_fopen(destination, "wb");
  if (!output) {
    fclose(input);
    return false_v;
  }
  bool8_t ok = true_v;
  uint8_t *chunk = (uint8_t *)malloc(MB(4));
  if (!chunk) {
    ok = false_v;
  }
  while (ok) {
    const size_t read = fread(chunk, 1u, MB(4), input);
    if (read && fwrite(chunk, 1u, read, output) != read) {
      ok = false_v;
    }
    if (read < MB(4)) {
      ok = ok && !ferror(input);
      break;
    }
  }
  free(chunk);
  ok = vkr_bakery_flush_file(output) && ok;
  ok = fclose(output) == 0 && ok;
  fclose(input);
  return ok;
}

bool8_t vkr_bakery_clone_or_copy(const char *source, const char *destination) {
  char directory[VKR_BAKERY_PATH_CAPACITY];
  char temporary[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), destination);
  if (directory[0] && !vkr_bakery_make_directories(directory)) {
    return false_v;
  }
  vkr_bakery_temp_path(temporary, sizeof(temporary),
                       directory[0] ? directory : ".",
                       vkr_bakery_path_name(destination));
  FilePath from = {
      .path = {.str = (uint8_t *)source, .length = strlen(source)}};
  FilePath to = {
      .path = {.str = (uint8_t *)temporary, .length = strlen(temporary)}};
  bool8_t ok = file_clone(&from, &to) == FILE_ERROR_NONE ||
               vkr_bakery_copy_bytes(source, temporary);
  ok = ok && vkr_bakery_rename(temporary, destination, true_v);
  if (!ok) {
    (void)vkr_bakery_remove_file(temporary);
  }
  return ok;
}

// =============================================================================
// Hashing
// =============================================================================

bool8_t vkr_bakery_hash_file(const char *path,
                             char out_hex[VKR_BAKERY_SHA256_HEX],
                             uint64_t *out_size) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return false_v;
  }
  uint8_t *chunk = (uint8_t *)malloc(MB(1));
  if (!chunk) {
    fclose(file);
    return false_v;
  }
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  uint64_t total = 0u;
  bool8_t ok = true_v;
  for (;;) {
    const size_t read = fread(chunk, 1u, MB(1), file);
    if (read) {
      vkr_sha256_update(&hash, chunk, read);
      total += read;
    }
    if (read < MB(1)) {
      ok = !ferror(file);
      break;
    }
  }
  free(chunk);
  fclose(file);
  if (!ok) {
    return false_v;
  }
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
  vkr_sha256_final(&hash, digest);
  vkr_sha256_hex(digest, out_hex);
  if (out_size) {
    *out_size = total;
  }
  return true_v;
}

void vkr_bakery_hash_bytes(const void *data, uint64_t length,
                           char out_hex[VKR_BAKERY_SHA256_HEX]) {
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
  vkr_sha256(data, length, digest);
  vkr_sha256_hex(digest, out_hex);
}

// =============================================================================
// Path text
// =============================================================================

bool8_t vkr_bakery_path_join(char *out, uint32_t capacity, const char *lhs,
                             const char *rhs) {
  if (!lhs || !lhs[0]) {
    return (uint32_t)snprintf(out, capacity, "%s", rhs) < capacity;
  }
  const uint64_t length = strlen(lhs);
  const bool8_t separated = vkr_bakery_is_separator(lhs[length - 1u]);
  const int written =
      snprintf(out, capacity, "%s%s%s", lhs, separated ? "" : "/", rhs);
  return written >= 0 && (uint32_t)written < capacity;
}

void vkr_bakery_path_parent(char *out, uint32_t capacity, const char *path) {
  const char *slash = NULL;
  for (const char *c = path; *c; ++c) {
    if (vkr_bakery_is_separator(*c)) {
      slash = c;
    }
  }
  if (!slash) {
    out[0] = 0;
    return;
  }
  uint64_t length = (uint64_t)(slash - path);
  if (length == 0u) {
    length = 1u; /* Root "/". */
  }
  if (length >= capacity) {
    length = capacity - 1u;
  }
  MemCopy(out, path, (size_t)length);
  out[length] = 0;
}

const char *vkr_bakery_path_name(const char *path) {
  const char *name = path;
  for (const char *c = path; *c; ++c) {
    if (vkr_bakery_is_separator(*c)) {
      name = c + 1;
    }
  }
  return name;
}

void vkr_bakery_path_extension(const char *path, char *out, uint32_t capacity) {
  const char *name = vkr_bakery_path_name(path);
  const char *dot = strrchr(name, '.');
  out[0] = 0;
  if (!dot || dot == name) {
    return;
  }
  uint32_t i = 0u;
  for (; dot[i] && i + 1u < capacity; ++i) {
    const char c = dot[i];
    out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  out[i] = 0;
}

void vkr_bakery_path_portable(char *path) {
#if defined(_WIN32)
  for (char *c = path; *c; ++c) {
    if (*c == '\\') {
      *c = '/';
    }
  }
#else
  /* A POSIX backslash is a filename byte, never a separator. */
  (void)path;
#endif
}

bool8_t vkr_bakery_path_is_absolute(const char *path) {
#if defined(_WIN32)
  if (path[0] == '/' || path[0] == '\\') {
    return true_v;
  }
  return ((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) &&
         path[1] == ':' && (path[2] == '/' || path[2] == '\\');
#else
  return path[0] == '/';
#endif
}

bool8_t vkr_bakery_path_absolute(const char *path, char *out,
                                 uint32_t capacity) {
  char joined[VKR_BAKERY_PATH_CAPACITY];
  if (vkr_bakery_path_is_absolute(path)) {
    if ((uint32_t)snprintf(joined, sizeof(joined), "%s", path) >=
        sizeof(joined)) {
      return false_v;
    }
  } else {
    char cwd[VKR_BAKERY_PATH_CAPACITY];
#if defined(_WIN32)
    wchar_t wide[4096];
    if (!GetCurrentDirectoryW(4096u, wide) ||
        !WideCharToMultiByte(CP_UTF8, 0, wide, -1, cwd, (int)sizeof(cwd), NULL,
                             NULL)) {
      return false_v;
    }
#else
    if (!getcwd(cwd, sizeof(cwd))) {
      return false_v;
    }
#endif
    if (!vkr_bakery_path_join(joined, sizeof(joined), cwd, path)) {
      return false_v;
    }
  }
  vkr_bakery_path_portable(joined);
  /* Lexically resolve "." and ".." segments. */
  char *segments[512];
  uint32_t count = 0u;
  const bool8_t drive = joined[1] == ':';
  char *cursor = joined + (drive ? 3 : 1);
  while (*cursor) {
    char *slash = strchr(cursor, '/');
    if (slash) {
      *slash = 0;
    }
    if (strcmp(cursor, "..") == 0) {
      if (count) {
        count -= 1u;
      }
    } else if (cursor[0] && strcmp(cursor, ".") != 0) {
      if (count == ArrayCount(segments)) {
        return false_v;
      }
      segments[count++] = cursor;
    }
    if (!slash) {
      break;
    }
    cursor = slash + 1;
  }
  uint32_t length = 0u;
  if (drive) {
    if (capacity < 4u) {
      return false_v;
    }
    out[0] = joined[0];
    out[1] = ':';
    length = 2u;
  }
  if (count == 0u) {
    if (length + 2u > capacity) {
      return false_v;
    }
    out[length++] = '/';
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const uint32_t segment = (uint32_t)strlen(segments[i]);
    if (length + segment + 2u > capacity) {
      return false_v;
    }
    out[length++] = '/';
    MemCopy(out + length, segments[i], segment);
    length += segment;
  }
  out[length] = 0;
  return true_v;
}

bool8_t vkr_bakery_path_relative(const char *root, const char *path, char *out,
                                 uint32_t capacity) {
  char absolute_root[VKR_BAKERY_PATH_CAPACITY];
  char absolute_path[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_absolute(root, absolute_root, sizeof(absolute_root)) ||
      !vkr_bakery_path_absolute(path, absolute_path, sizeof(absolute_path))) {
    return false_v;
  }
  const uint64_t root_length = strlen(absolute_root);
  if (strcmp(absolute_root, "/") == 0) {
    return (uint32_t)snprintf(out, capacity, "%s", absolute_path + 1) <
           capacity;
  }
  if (strncmp(absolute_path, absolute_root, root_length) != 0) {
    return false_v;
  }
  const char *rest = absolute_path + root_length;
  if (*rest == 0) {
    return (uint32_t)snprintf(out, capacity, ".") < capacity;
  }
  if (*rest != '/') {
    return false_v;
  }
  return (uint32_t)snprintf(out, capacity, "%s", rest + 1) < capacity;
}

void vkr_bakery_temp_path(char *out, uint32_t capacity, const char *directory,
                          const char *label) {
  static VkrAtomicUint32 counter = 0u;
  const uint32_t sequence =
      vkr_atomic_uint32_fetch_add(&counter, 1u, VKR_MEMORY_ORDER_RELAXED) + 1u;
  (void)snprintf(out, capacity, "%s/.%s.%u.%u.%llx.tmp", directory, label,
                 vkr_platform_get_process_id(), sequence,
                 (unsigned long long)vkr_bakery_unix_seconds());
}

// =============================================================================
// Process and host
// =============================================================================

bool8_t vkr_bakery_executable_path(char *out, uint32_t capacity) {
  return vkr_platform_executable_path(out, capacity);
}

bool8_t vkr_bakery_user_cache_directory(char *out, uint32_t capacity) {
#if defined(_WIN32)
  wchar_t wide[4096];
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", wide, 4096u);
  char base[VKR_BAKERY_PATH_CAPACITY];
  if (!length || length >= 4096u ||
      !WideCharToMultiByte(CP_UTF8, 0, wide, -1, base, (int)sizeof(base), NULL,
                           NULL)) {
    return false_v;
  }
  vkr_bakery_path_portable(base);
  return vkr_bakery_path_join(out, capacity, base, "vkr_bakery");
#else
  const char *home = getenv("HOME");
  if (!home || !home[0]) {
    return false_v;
  }
#if defined(__APPLE__)
  return (uint32_t)snprintf(out, capacity, "%s/Library/Caches/vkr_bakery",
                            home) < capacity;
#else
  const char *xdg = getenv("XDG_CACHE_HOME");
  if (xdg && xdg[0]) {
    return (uint32_t)snprintf(out, capacity, "%s/vkr_bakery", xdg) < capacity;
  }
  return (uint32_t)snprintf(out, capacity, "%s/.cache/vkr_bakery", home) <
         capacity;
#endif
#endif
}

uint64_t vkr_bakery_physical_memory_bytes(void) {
#if defined(_WIN32)
  MEMORYSTATUSEX status = {.dwLength = sizeof(status)};
  return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0u;
#elif defined(__APPLE__)
  uint64_t memory = 0u;
  size_t size = sizeof(memory);
  return sysctlbyname("hw.memsize", &memory, &size, NULL, 0) == 0 ? memory : 0u;
#else
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page = sysconf(_SC_PAGESIZE);
  return pages > 0 && page > 0 ? (uint64_t)pages * (uint64_t)page : 0u;
#endif
}

uint32_t vkr_bakery_logical_cores(void) {
  const uint32_t cores = vkr_platform_get_logical_core_count();
  return cores ? cores : 1u;
}

bool8_t vkr_bakery_self_usage(uint64_t *out_cpu_ms,
                              uint64_t *out_peak_rss_bytes) {
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  PROCESS_MEMORY_COUNTERS counters;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel,
                       &user) ||
      !GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    return false_v;
  }
  const uint64_t ticks =
      (((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
      (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime);
  *out_cpu_ms = ticks / 10000u;
  *out_peak_rss_bytes = counters.PeakWorkingSetSize;
  return true_v;
#else
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return false_v;
  }
  *out_cpu_ms = (uint64_t)usage.ru_utime.tv_sec * 1000u +
                (uint64_t)usage.ru_utime.tv_usec / 1000u +
                (uint64_t)usage.ru_stime.tv_sec * 1000u +
                (uint64_t)usage.ru_stime.tv_usec / 1000u;
#if defined(__APPLE__)
  *out_peak_rss_bytes = (uint64_t)usage.ru_maxrss; /* Bytes on macOS. */
#else
  *out_peak_rss_bytes = (uint64_t)usage.ru_maxrss * 1024u;
#endif
  return true_v;
#endif
}

bool8_t vkr_bakery_children_usage(uint64_t *out_cpu_ms,
                                  uint64_t *out_peak_rss_bytes) {
#if defined(_WIN32)
  /* Windows children are measured by their own vkr_bakery process. */
  *out_cpu_ms = 0u;
  *out_peak_rss_bytes = 0u;
  return false_v;
#else
  struct rusage usage;
  if (getrusage(RUSAGE_CHILDREN, &usage) != 0) {
    return false_v;
  }
  *out_cpu_ms = (uint64_t)usage.ru_utime.tv_sec * 1000u +
                (uint64_t)usage.ru_utime.tv_usec / 1000u +
                (uint64_t)usage.ru_stime.tv_sec * 1000u +
                (uint64_t)usage.ru_stime.tv_usec / 1000u;
#if defined(__APPLE__)
  *out_peak_rss_bytes = (uint64_t)usage.ru_maxrss;
#else
  *out_peak_rss_bytes = (uint64_t)usage.ru_maxrss * 1024u;
#endif
  return true_v;
#endif
}

void vkr_bakery_utc_timestamp(char out[32]) {
  const time_t now = time(NULL);
  struct tm utc;
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  (void)strftime(out, 32u, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

int64_t vkr_bakery_unix_seconds(void) { return (int64_t)time(NULL); }

float64_t vkr_bakery_monotonic_seconds(void) {
  return vkr_platform_get_absolute_time();
}
