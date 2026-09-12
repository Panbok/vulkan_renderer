#pragma once

#include "editor_ui.h"
#include "vkr_sample_runtime.h"

typedef struct VkrEditorApplication {
  VkrEditorUi ui;
  const char *layout_path;
  int argc;
  char **argv;
  bool8_t project_managed;
} VkrEditorApplication;

VkrSampleRuntimeConfig
vkr_editor_application_config(VkrEditorApplication *editor, int argc,
                              char **argv);
