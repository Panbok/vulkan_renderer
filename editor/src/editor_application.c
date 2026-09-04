#include "editor_application.h"

#include "core/logger.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool8_t editor_env_flag(const char *name, bool8_t default_value) {
  const char *value = getenv(name);
  if (!value || value[0] == '\0')
    return default_value;

  switch (value[0]) {
  case '1':
  case 'y':
  case 'Y':
  case 't':
  case 'T':
    return true_v;
  case '0':
  case 'n':
  case 'N':
  case 'f':
  case 'F':
    return false_v;
  default:
    return default_value;
  }
}

static float32_t editor_env_render_scale(void) {
  const char *value = getenv("VKR_EDITOR_RENDER_SCALE");
  if (!value || value[0] == '\0')
    return 0.8f;

  char *end = NULL;
  const float32_t parsed = strtof(value, &end);
  if (end == value || *end != '\0' || !isfinite(parsed))
    return 0.8f;
  return vkr_clamp_f32(parsed, 0.25f, 1.0f);
}

static void editor_application_initialize(void *state, VkrUiDockTree *dock) {
  VkrEditorApplication *editor = state;
  vkr_editor_ui_init(&editor->ui);
  if (!editor->layout_path || editor->layout_path[0] == '\0')
    return;

  const String8 path = string8_create_from_cstr(
      (const uint8_t *)editor->layout_path, string_length(editor->layout_path));
  if (vkr_editor_ui_load_layout(dock, path)) {
    log_info("Loaded editor layout from '%s'", editor->layout_path);
  } else {
    log_warn("Using the default editor layout; '%s' could not be loaded",
             editor->layout_path);
  }
}

static void editor_application_handle_input(void *state,
                                            const InputState *input) {
  (void)state;
  (void)input;
}

static VkrUiDockInputCapture
editor_application_build(void *state, const VkrSampleUiFrame *frame) {
  VkrEditorApplication *editor = state;
  return vkr_editor_ui_build(&editor->ui, frame);
}

static bool8_t editor_application_shutdown(void *state,
                                           const VkrUiDockTree *dock) {
  const VkrEditorApplication *editor = state;
  if (!editor->layout_path || editor->layout_path[0] == '\0')
    return true_v;

  const String8 path = string8_create_from_cstr(
      (const uint8_t *)editor->layout_path, string_length(editor->layout_path));
  if (vkr_editor_ui_save_layout(dock, path))
    return true_v;

  log_error("Failed to save editor layout to '%s'", editor->layout_path);
  return false_v;
}

VkrSampleRuntimeConfig
vkr_editor_application_config(VkrEditorApplication *editor, int argc,
                              char **argv) {
  *editor = (VkrEditorApplication){
      .layout_path = getenv("VKR_EDITOR_LAYOUT_PATH"),
  };
  bool8_t scene_only = editor_env_flag("VKR_EDITOR_SCENE_ONLY", false_v);
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--scene-only") == 0)
      scene_only = true_v;
    else if (strcmp(argv[i], "--paneled") == 0)
      scene_only = false_v;
  }

  VkrSampleRuntimeConfig config = vkr_sample_runtime_config_default();
  config.title = "VKR Editor";
  config.presentation = (VkrSamplePresentationConfig){
      .render_scale = editor_env_render_scale(),
      .paneled = true_v,
      .scene_only = scene_only,
  };
  config.ui = (VkrSampleUiClient){
      .state = editor,
      .initialize = editor_application_initialize,
      .handle_input = editor_application_handle_input,
      .build = editor_application_build,
      .shutdown = editor_application_shutdown,
  };
  return config;
}
