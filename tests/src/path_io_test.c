#include "core/ui/vkr_ui_dock.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include "vkr_graphics_settings.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

bool32_t run_path_io_tests(void);

bool32_t run_path_io_tests(void) {
#if defined(PLATFORM_WINDOWS)
  errno = ENOENT;
  assert(!file_fopen("C:ambiguous.json", "rb") && errno == EINVAL);
  const char invalid_utf8[] = {(char)0xff, 0};
  errno = ENOENT;
  assert(!file_fopen(invalid_utf8, "rb") && errno == EINVAL);
#endif
  FilePath directory = {.path = string8_lit(PROJECT_SOURCE_DIR "tests/tmp"),
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory(&directory));
  char name[1024];
  snprintf(name, sizeof(name),
           PROJECT_SOURCE_DIR "tests/tmp/настройки_%u.json",
           vkr_platform_get_process_id());
  FilePath path = {.path = string8_create((uint8_t *)name, strlen(name)),
                   .type = FILE_PATH_TYPE_ABSOLUTE};

  VkrGraphicsSettings written =
      vkr_graphics_settings_defaults(VKR_RENDERER_BACKEND_TYPE_VULKAN);
  written.brightness = 0.25f;
  written.frame_limit = 73u;
  assert(vkr_graphics_settings_save(name, &written));
  assert(file_exists(&path));
  VkrGraphicsSettings loaded =
      vkr_graphics_settings_defaults(VKR_RENDERER_BACKEND_TYPE_VULKAN);
  assert(vkr_graphics_settings_load(name, &loaded));
  assert(loaded.brightness == written.brightness && loaded.frame_limit == 73u);

  VkrUiDockTree tree = {0};
  VkrUiDockTree restored = {0};
  vkr_ui_dock_default_editor_layout(&tree);
  assert(vkr_ui_dock_save_file(&tree, path.path));
  assert(vkr_ui_dock_load_file(&restored, path.path));
  assert(vkr_ui_dock_validate(&restored));
  assert(file_remove(&path) == FILE_ERROR_NONE);
  assert(!vkr_ui_dock_load_file(&restored, path.path));
  return true_v;
}
