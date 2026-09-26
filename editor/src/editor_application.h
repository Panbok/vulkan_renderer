#pragma once

#include "editor_ui.h"
#include "vkr_sample_runtime.h"

typedef struct VkrEditorApplication {
  VkrEditorUi ui;
  const char *layout_path;
  int argc;
  char **argv;
  bool8_t project_managed;
  /** `--exec` Cmd script from argv; VKR_EDITOR_EXEC runs before it. */
  const char *exec_script;
  /** Window shape last requested: the compact launcher or the editor. */
  bool8_t window_launcher;
} VkrEditorApplication;

VkrSampleRuntimeConfig
vkr_editor_application_config(VkrEditorApplication *editor, int argc,
                              char **argv);
