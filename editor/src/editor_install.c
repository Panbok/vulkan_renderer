#include "editor_install.h"

#include "filesystem/filesystem.h"
#include "filesystem/vkr_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

vkr_global char vkr_editor_tools[VKR_EDITOR_TOOL_COUNT]
                                [VKR_EDITOR_INSTALL_PATH_CAPACITY];

/* Where this build tree produced each program; a distribution carries the
 * same file names beside the editor. */
vkr_global const char *const vkr_editor_built_tools[VKR_EDITOR_TOOL_COUNT] = {
    [VKR_EDITOR_TOOL_BAKERY] = VKR_EDITOR_BAKERY_PATH,
    [VKR_EDITOR_TOOL_HARNESS] = VKR_EDITOR_HARNESS_PATH,
    [VKR_EDITOR_TOOL_ASSET_PREVIEW] = VKR_EDITOR_ASSET_PREVIEW_PATH,
};

vkr_internal const char *vkr_editor_install_file_name(const char *path) {
  const char *name = path;
  for (const char *c = path; *c; ++c) {
    if (*c == '/' || *c == '\\') {
      name = c + 1;
    }
  }
  return name;
}

void vkr_editor_install_resolve(void) {
  char directory[VKR_EDITOR_INSTALL_PATH_CAPACITY] = {0};
  if (vkr_platform_executable_path(directory, sizeof(directory))) {
    char *separator = NULL;
    for (char *c = directory; *c; ++c) {
      if (*c == '/' || *c == '\\') {
        separator = c;
      }
    }
    if (separator) {
      *separator = '\0';
    } else {
      directory[0] = '\0';
    }
  }

  for (uint32_t i = 0u; i < VKR_EDITOR_TOOL_COUNT; ++i) {
    const char *built = vkr_editor_built_tools[i];
    char beside[VKR_EDITOR_INSTALL_PATH_CAPACITY];
    const int written = snprintf(beside, sizeof(beside), "%s/%s", directory,
                                 vkr_editor_install_file_name(built));
    const FilePath file = {.path = string8_create_from_cstr(
                               (const uint8_t *)beside, strlen(beside)),
                           .type = FILE_PATH_TYPE_ABSOLUTE};
    const bool8_t use_beside = directory[0] && written > 0 &&
                               (uint32_t)written < sizeof(beside) &&
                               file_exists(&file);
    (void)snprintf(vkr_editor_tools[i], sizeof(vkr_editor_tools[i]), "%s",
                   use_beside ? beside : built);
  }
}

const char *vkr_editor_tool_path(VkrEditorTool tool) {
  if (tool >= VKR_EDITOR_TOOL_COUNT) {
    return "";
  }
  return vkr_editor_tools[tool][0] ? vkr_editor_tools[tool]
                                   : vkr_editor_built_tools[tool];
}

bool8_t vkr_editor_user_path(VkrPlatformUserDirectory kind, const char *leaf,
                             char *out, uint32_t capacity) {
  char base[VKR_EDITOR_INSTALL_PATH_CAPACITY];
  if (!out || !capacity ||
      !vkr_platform_user_directory(kind, base, sizeof(base))) {
    return false_v;
  }
  const int written = snprintf(out, capacity, "%s/VKR/%s", base, leaf);
  return written > 0 && (uint32_t)written < capacity;
}
