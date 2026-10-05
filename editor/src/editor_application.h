#pragma once

#include "editor_project_store.h"
#include "editor_ui.h"
#include "vkr_sample_runtime.h"

typedef struct VkrEditorApplication {
  VkrEditorUi ui;
  const char *layout_path;
  int argc;
  char **argv;
  bool8_t project_managed;
  /** Machine-local Graphics settings a project-managed editor starts with;
   * empty when the local directory is unavailable. */
  char graphics_path[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  /** `--exec` Cmd script from argv; VKR_EDITOR_EXEC runs before it. */
  const char *exec_script;
  /** `--headless`: no window; the editor quits when its Cmd script ends. */
  bool8_t headless;
  /** `--agent-socket <path>` and `--no-agent-socket`: the agent channel's
   * socket (ADR-084). */
  const char *agent_socket;
  bool8_t agent_disabled;
  /** `--scripts <dir>`: a Scripts folder of C modules to build, load and
   * hot reload without a project (ADR-079); opened on the first frame. */
  const char *scripts_directory;
  bool8_t scripts_opened;
  /** Window shape last requested: the compact launcher or the editor. */
  bool8_t window_launcher;
  /** Plain frames left around a launcher/editor resize; the resize is
   * requested on the last one. */
  uint8_t window_resize_frames;
} VkrEditorApplication;

VkrSampleRuntimeConfig
vkr_editor_application_config(VkrEditorApplication *editor, int argc,
                              char **argv);
