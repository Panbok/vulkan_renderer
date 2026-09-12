#include "editor_application.h"
#include "editor_internal.h"
#include "editor_projects.h"
#include "editor_content.h"

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

static bool8_t editor_application_initialize(void *state, VkrUiDockTree *dock,
                                             VkrUiSystem *ui) {
  VkrEditorApplication *editor = state;
  vkr_editor_ui_init(&editor->ui);
  if (!vkr_editor_console_init(&editor->ui.console, &ui->retained_allocator))
    goto cleanup;
  editor->ui.bakery = vkr_editor_bakery_create(&ui->retained_allocator);
  editor->ui.scene_panels =
      vkr_editor_scene_panels_create(&ui->retained_allocator);
  if (!editor->ui.bakery || !editor->ui.scene_panels)
    goto cleanup;
  if (editor->project_managed) {
    editor->ui.projects = vkr_editor_projects_create(
        &ui->retained_allocator, editor->argc, editor->argv);
    if (!editor->ui.projects) {
      goto cleanup;
    }
  }
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  if (editor->project_managed && !vkr_font_system_load_from_file(
          ui->fonts, string8_lit("default-scene-font"),
          vkr_font_system_bootstrap_path(ui->fonts, "UbuntuMono-cooked.fontcfg", &ui->retained_allocator), &error)) {
    goto cleanup;
  }
  const String8 name = string8_lit("editor-heading");
  if (!vkr_font_system_load_from_file(
          ui->fonts, name,
          vkr_font_system_bootstrap_path(ui->fonts, "UbuntuMono-Bold-cooked.fontcfg", &ui->retained_allocator), &error)) {
    log_error("Failed to load the editor heading font (%u)", (uint32_t)error);
    goto cleanup;
  }
  // The font system retains storage until UI text borrowers have shut down.
  editor->ui.heading_font =
      vkr_font_system_acquire(ui->fonts, name, false_v, &error);
  if (error != VKR_RENDERER_ERROR_NONE)
    goto cleanup;
  const String8 label_name = string8_lit("editor-light-labels");
  if (!vkr_font_system_load_from_file(
          ui->fonts, label_name,
          vkr_font_system_bootstrap_path(ui->fonts, "editor-light-labels.fontcfg", &ui->retained_allocator), &error)) {
    log_error("Failed to load editor light labels (%u)", (uint32_t)error);
    goto cleanup;
  }
  editor->ui.label_font =
      vkr_font_system_acquire(ui->fonts, label_name, false_v, &error);
  if (error != VKR_RENDERER_ERROR_NONE)
    goto cleanup;
  if (!editor->layout_path || editor->layout_path[0] == '\0')
    return true_v;

  const String8 path = string8_create_from_cstr(
      (const uint8_t *)editor->layout_path, string_length(editor->layout_path));
  if (vkr_ui_dock_load_file(dock, path)) {
    log_info("Loaded editor layout from '%s'", editor->layout_path);
  } else {
    log_warn("Using the default editor layout; '%s' could not be loaded",
             editor->layout_path);
  }
  return true_v;
cleanup:
  (void)vkr_editor_projects_destroy(editor->ui.projects, &editor->ui, dock);
  editor->ui.projects = NULL;
  if (editor->ui.heading_font.id)
    vkr_font_system_release_by_handle(ui->fonts, editor->ui.heading_font);
  if (editor->ui.label_font.id)
    vkr_font_system_release_by_handle(ui->fonts, editor->ui.label_font);
  vkr_editor_bakery_destroy(editor->ui.bakery);
  vkr_editor_scene_panels_destroy(editor->ui.scene_panels);
  vkr_editor_console_shutdown(&editor->ui.console);
  editor->ui.bakery = NULL;
  editor->ui.scene_panels = NULL;
  return false_v;
}

static void editor_application_handle_input(void *state,
                                            const InputState *input) {
  (void)state;
  (void)input;
}

static VkrUiDockInputCapture
editor_application_build(void *state, const VkrSampleUiFrame *frame) {
  VkrEditorApplication *editor = state;
  /* Labels borrow this frame's allocator. A full Projects modal skips the
   * normal scene UI, so no anchors may survive from the previous frame. */
  editor->ui.label_anchors = NULL;
  editor->ui.label_anchor_count = 0;
  if (!editor->ui.content) {
    editor->ui.content = vkr_editor_content_create(
        &frame->ui->retained_allocator, frame->assets);
  }
  vkr_editor_content_update(editor->ui.content);
  vkr_editor_projects_update(editor->ui.projects, &editor->ui, frame);
  VkrSampleUiFrame editor_frame = *frame;
  editor_frame.scene_loading |= vkr_editor_projects_loading(editor->ui.projects);
  frame = &editor_frame;
  VkrUiDockInputCapture capture = {0};
  if (!vkr_editor_projects_modal(editor->ui.projects)) {
    capture = vkr_editor_ui_build(&editor->ui, frame);
  }
  vkr_editor_projects_scene_action(editor->ui.projects, &editor->ui, frame);
  vkr_editor_projects_build(editor->ui.projects, &editor->ui, frame);
  if (vkr_editor_projects_modal(editor->ui.projects)) {
    capture.mouse = true_v;
  }
  return capture;
}

static bool8_t editor_application_save_scene(void *state, VkrSceneEditState *edits,
                                              const VkrScene *scene,
                                              String8 runtime_scene_path) {
  VkrEditorApplication *editor = state;
  return vkr_editor_projects_save_scene(editor->ui.projects, edits, scene, runtime_scene_path);
}

static bool8_t editor_application_shutdown(void *state,
                                           const VkrUiDockTree *dock,
                                           VkrUiSystem *ui) {
  VkrEditorApplication *editor = state;
  const bool8_t projects_saved = vkr_editor_projects_flush(editor->ui.projects, &editor->ui, dock);
  /* Stop and join every workspace writer before releasing its lifetime lease. */
  vkr_editor_content_destroy(editor->ui.content);
  editor->ui.content = NULL;
  vkr_editor_bakery_destroy(editor->ui.bakery);
  editor->ui.bakery = NULL;
  (void)vkr_editor_projects_destroy(editor->ui.projects, &editor->ui, dock);
  editor->ui.projects = NULL;
  vkr_editor_scene_panels_destroy(editor->ui.scene_panels);
  vkr_editor_console_shutdown(&editor->ui.console);
  vkr_font_system_release_by_handle(ui->fonts, editor->ui.heading_font);
  vkr_font_system_release_by_handle(ui->fonts, editor->ui.label_font);
  if (!editor->layout_path || editor->layout_path[0] == '\0')
    return projects_saved;

  const String8 path = string8_create_from_cstr(
      (const uint8_t *)editor->layout_path, string_length(editor->layout_path));
  if (vkr_ui_dock_save_file(dock, path))
    return true_v;

  log_error("Failed to save editor layout to '%s'", editor->layout_path);
  return false_v;
}

static void editor_application_project_scene(void *state,
                                             const VkrSampleUiFrame *frame) {
  VkrEditorApplication *editor = state;
  vkr_editor_labels_project(&editor->ui, frame);
}

VkrSampleRuntimeConfig
vkr_editor_application_config(VkrEditorApplication *editor, int argc,
                              char **argv) {
  *editor = (VkrEditorApplication){
      .layout_path = getenv("VKR_EDITOR_LAYOUT_PATH"),
      .argc = argc,
      .argv = argv,
      .project_managed = true_v,
  };
  if (!editor->layout_path)
    editor->layout_path = ".vkr-editor-layout.json";
  bool8_t scene_only = editor_env_flag("VKR_EDITOR_SCENE_ONLY", false_v);
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--scene") == 0) {
      editor->project_managed = false_v;
    }
    if (strcmp(argv[i], "--scene-only") == 0)
      scene_only = true_v;
    else if (strcmp(argv[i], "--paneled") == 0)
      scene_only = false_v;
  }

  VkrSampleRuntimeConfig config = vkr_sample_runtime_config_default();
  config.project_managed = editor->project_managed;
  if (editor->project_managed) {
    editor->layout_path = NULL;
  }
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
      .save_scene_edits = editor->project_managed ? editor_application_save_scene : NULL,
      .project_scene = editor_application_project_scene,
      .shutdown = editor_application_shutdown,
  };
  return config;
}
