#pragma once

#include "editor_ui.h"
#include "vkr_sample_runtime.h"

typedef struct VkrEditorApplication {
  VkrEditorUi ui;
  const char *layout_path;
} VkrEditorApplication;

VkrSampleRuntimeConfig
vkr_editor_application_config(VkrEditorApplication *editor, int argc,
                              char **argv);
