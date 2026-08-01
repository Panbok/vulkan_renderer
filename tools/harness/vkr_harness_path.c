#include "vkr_harness.h"

/**
 * Fully resolves an existing path, following symlinks, into a harness-sized
 * buffer through the renderer filesystem boundary.
 */
bool8_t vkr_harness_realpath(const char *path,
                             char out_path[VKR_HARNESS_PATH_MAX]) {
  if (!path || !out_path) {
    return false_v;
  }
  FilePath file_path = vkr_harness_file_path(path);
  return file_path_resolve(&file_path, out_path, VKR_HARNESS_PATH_MAX) ==
         FILE_ERROR_NONE;
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
    const uint64_t length = (uint64_t)(cursor - component);
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
  const uint64_t root_length = string_length(root);
  if (!file_path_starts_with(path, root)) {
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
  return !file_path_equals(resolved_root, resolved_path);
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
  const int32_t written = string_format(joined, sizeof(joined), "%s/%s",
                                        resolved_root, relative_path);
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
  string_format(relative_parent, sizeof(relative_parent), "%s", relative_path);
  char *separator = string_get_last_char_occurrence(relative_parent, '/');
  char *backslash = string_get_last_char_occurrence(relative_parent, '\\');
  if (backslash && (!separator || backslash > separator)) {
    separator = backslash;
  }
  const char *filename = relative_path;
  if (separator) {
    *separator = '\0';
    filename = relative_path + (separator - relative_parent) + 1;
  } else {
    string_format(relative_parent, sizeof(relative_parent), "%s", ".");
  }
  char joined_parent[VKR_HARNESS_PATH_MAX];
  string_format(joined_parent, sizeof(joined_parent), "%s/%s", resolved_root,
                relative_parent);
  char resolved_parent[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_realpath(joined_parent, resolved_parent) ||
      !vkr_harness_path_below(resolved_root, resolved_parent)) {
    vkr_harness_error_set(out_error, "path.escape", "$",
                          "Output parent is missing or escapes root");
    return false_v;
  }
  const int32_t written = string_format(out_path, VKR_HARNESS_PATH_MAX, "%s/%s",
                                        resolved_parent, filename);
  if (written < 0 || written >= (int32_t)VKR_HARNESS_PATH_MAX) {
    vkr_harness_error_set(out_error, "path.length", "$",
                          "Output path is too long");
    return false_v;
  }
  return true_v;
}
