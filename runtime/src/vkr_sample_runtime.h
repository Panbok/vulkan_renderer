#pragma once

#include "core/input.h"
#include "core/ui/vkr_ui_dock.h"
#include "core/vkr_text.h"
#include "renderer/systems/vkr_editor_viewport.h"
#include "renderer/systems/vkr_ui_system.h"

typedef struct VkrSampleUiText {
  String8 camera;
  String8 performance;
  String8 metrics;
  String8 memory;
} VkrSampleUiText;

typedef struct VkrSampleUiFrame {
  VkrUiSystem *ui;
  VkrUiDockTree *dock;
  InputState *input;
  VkrViewportMapping mapping;
  VkrSampleUiText text;
  bool8_t mapping_valid;
  bool8_t scene_only;
  bool8_t mouse_captured;
} VkrSampleUiFrame;

typedef struct VkrSampleUiClient {
  void *state;
  void (*initialize)(void *state, VkrUiDockTree *dock);
  void (*handle_input)(void *state, const InputState *input);
  VkrUiDockInputCapture (*build)(void *state, const VkrSampleUiFrame *frame);
  bool8_t (*shutdown)(void *state, const VkrUiDockTree *dock);
} VkrSampleUiClient;

typedef struct VkrSamplePresentationConfig {
  float32_t render_scale;
  bool8_t paneled;
  bool8_t scene_only;
} VkrSamplePresentationConfig;

typedef struct VkrSampleRuntimeConfig {
  const char *title;
  VkrSamplePresentationConfig presentation;
  VkrSampleUiClient ui;
} VkrSampleRuntimeConfig;

VkrSampleRuntimeConfig vkr_sample_runtime_config_default(void);
int vkr_sample_runtime_run(int argc, char **argv,
                           const VkrSampleRuntimeConfig *config);
