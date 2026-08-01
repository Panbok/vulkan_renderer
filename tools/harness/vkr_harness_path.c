#include "vkr_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

/**
 * Fully resolves an existing path, following symlinks, into a harness-sized
 * buffer. The allocating form of `realpath` is used deliberately: the caller's
 * buffer is VKR_HARNESS_PATH_MAX, which is smaller than `PATH_MAX` on some
 * platforms, and the in-place form would write past it.
 */
bool8_t vkr_harness_realpath(const char *path,
                             char out_path[VKR_HARNESS_PATH_MAX]) {
  if (!path || !out_path) {
    return false_v;
  }
#if defined(_WIN32)
  HANDLE handle =
      CreateFileA(path, FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    return false_v;
  }
  char resolved[VKR_HARNESS_PATH_MAX + 8u];
  const DWORD length =
      GetFinalPathNameByHandleA(handle, resolved, sizeof(resolved),
                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  CloseHandle(handle);
  if (length == 0u || length >= sizeof(resolved)) {
    return false_v;
  }
  const char *source = resolved;
  int written = 0;
  if (strncmp(source, "\\\\?\\UNC\\", 8u) == 0) {
    written = snprintf(out_path, VKR_HARNESS_PATH_MAX, "\\\\%s", source + 8u);
  } else {
    if (strncmp(source, "\\\\?\\", 4u) == 0) {
      source += 4u;
    }
    written = snprintf(out_path, VKR_HARNESS_PATH_MAX, "%s", source);
  }
#else
  char *resolved = realpath(path, NULL);
  if (!resolved) {
    return false_v;
  }
  const int written = snprintf(out_path, VKR_HARNESS_PATH_MAX, "%s", resolved);
  free(resolved);
#endif
  return written >= 0 && written < (int)VKR_HARNESS_PATH_MAX;
}

bool8_t vkr_harness_path_is_safe_relative(const char *path) {
  if (!path || path[0] == '\0' || path[0] == '/' || path[0] == '\\' ||
      (path[0] && path[1] == ':')) {
    return false_v;
  }
  const char *component = path;
  for (const char *cursor = path;; ++cursor) {
    if (*cursor != '/' && *cursor != '\\' && *cursor != '\0') {
      continue;
    }
    const size_t length = (size_t)(cursor - component);
    if (length == 0 || (length == 1 && component[0] == '.') ||
        (length == 2 && component[0] == '.' && component[1] == '.')) {
      return false_v;
    }
    if (*cursor == '\0') {
      break;
    }
    component = cursor + 1;
  }
  return true_v;
}

static bool8_t vkr_harness_path_below(const char *root, const char *path) {
  const size_t root_length = strlen(root);
#if defined(_WIN32)
  if (_strnicmp(root, path, root_length) != 0) {
#else
  if (strncmp(root, path, root_length) != 0) {
#endif
    return false_v;
  }
  return path[root_length] == '\0' || path[root_length] == '/' ||
         path[root_length] == '\\';
}

bool8_t vkr_harness_existing_path_is_below(const char *root, const char *path) {
  char resolved_root[VKR_HARNESS_PATH_MAX];
  char resolved_path[VKR_HARNESS_PATH_MAX];
  if (!root || !path || !vkr_harness_realpath(root, resolved_root) ||
      !vkr_harness_realpath(path, resolved_path) ||
      !vkr_harness_path_below(resolved_root, resolved_path)) {
    return false_v;
  }
#if defined(_WIN32)
  return _stricmp(resolved_root, resolved_path) != 0;
#else
  return strcmp(resolved_root, resolved_path) != 0;
#endif
}

bool8_t vkr_harness_resolve_existing_path(const char *root,
                                          const char *relative_path,
                                          char out_path[VKR_HARNESS_PATH_MAX],
                                          VkrHarnessError *out_error) {
  if (!root || !out_path || !vkr_harness_path_is_safe_relative(relative_path)) {
    vkr_harness_error_set(
        out_error, "path.unsafe", "$",
        "Path must be a nonempty repository-relative path without '.' or '..'");
    return false_v;
  }
  char resolved_root[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_realpath(root, resolved_root)) {
    vkr_harness_error_set(out_error, "path.root", "$",
                          "Unable to resolve root '%s'", root);
    return false_v;
  }
  char joined[VKR_HARNESS_PATH_MAX];
  const int written =
      snprintf(joined, sizeof(joined), "%s/%s", resolved_root, relative_path);
  if (written < 0 || (uint32_t)written >= sizeof(joined) ||
      !vkr_harness_realpath(joined, out_path) ||
      !vkr_harness_path_below(resolved_root, out_path)) {
    vkr_harness_error_set(out_error, "path.escape", "$",
                          "Path '%s' is missing or escapes root",
                          relative_path);
    return false_v;
  }
  return true_v;
}

bool8_t vkr_harness_resolve_output_path(const char *root,
                                        const char *relative_path,
                                        char out_path[VKR_HARNESS_PATH_MAX],
                                        VkrHarnessError *out_error) {
  if (!root || !out_path || !vkr_harness_path_is_safe_relative(relative_path)) {
    vkr_harness_error_set(out_error, "path.unsafe", "$",
                          "Output path must be safely relative");
    return false_v;
  }
  char resolved_root[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_realpath(root, resolved_root)) {
    vkr_harness_error_set(out_error, "path.root", "$",
                          "Unable to resolve output root '%s'", root);
    return false_v;
  }
  char relative_parent[VKR_HARNESS_PATH_MAX];
  snprintf(relative_parent, sizeof(relative_parent), "%s", relative_path);
  char *separator = strrchr(relative_parent, '/');
  char *backslash = strrchr(relative_parent, '\\');
  if (backslash && (!separator || backslash > separator)) {
    separator = backslash;
  }
  const char *filename = relative_path;
  if (separator) {
    *separator = '\0';
    filename = relative_path + (separator - relative_parent) + 1;
  } else {
    snprintf(relative_parent, sizeof(relative_parent), "%s", ".");
  }
  char joined_parent[VKR_HARNESS_PATH_MAX];
  snprintf(joined_parent, sizeof(joined_parent), "%s/%s", resolved_root,
           relative_parent);
  char resolved_parent[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_realpath(joined_parent, resolved_parent) ||
      !vkr_harness_path_below(resolved_root, resolved_parent)) {
    vkr_harness_error_set(out_error, "path.escape", "$",
                          "Output parent is missing or escapes root");
    return false_v;
  }
  const int written = snprintf(out_path, VKR_HARNESS_PATH_MAX, "%s/%s",
                               resolved_parent, filename);
  if (written < 0 || written >= (int)VKR_HARNESS_PATH_MAX) {
    vkr_harness_error_set(out_error, "path.length", "$",
                          "Output path is too long");
    return false_v;
  }
  return true_v;
}
