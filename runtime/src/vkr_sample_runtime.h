#pragma once

#include "core/input.h"
#include "core/ui/vkr_ui_dock.h"
#include "core/vkr_text.h"
#include "renderer/systems/vkr_editor_viewport.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_ui_system.h"
#include "renderer/vkr_renderer.h"

typedef struct VkrSampleUiText {
  String8 camera;
  String8 performance;
  String8 metrics;
  String8 memory;
  /** CPU/GPU identity and periodically sampled process/device memory. */
  String8 system;
} VkrSampleUiText;

typedef enum VkrSampleTransportAction {
  VKR_SAMPLE_TRANSPORT_NONE = 0,
  VKR_SAMPLE_TRANSPORT_START_SIMULATION,
  VKR_SAMPLE_TRANSPORT_PAUSE_SIMULATION,
  VKR_SAMPLE_TRANSPORT_START_RENDERING,
  VKR_SAMPLE_TRANSPORT_STOP_RENDERING,
  VKR_SAMPLE_TRANSPORT_TOGGLE_CAMERA,
} VkrSampleTransportAction;

typedef struct VkrSampleUiFrame {
  VkrUiSystem *ui;
  VkrUiDockTree *dock;
  InputState *input;
  VkrViewportMapping mapping;
  Mat4 view_projection; /* Unjittered camera, same Y-down convention as picking.
                         */
  VkrSampleUiText text;
  const VkrScene *scene; /* Borrowed until build returns; edits are requests. */
  VkrEntityId selected_entity;
  uint64_t scene_generation;
  const VkrSceneEditState *edits;
  VkrSceneEditRequest *scene_edit;
  float64_t simulation_time;
  bool8_t simulation_running;
  bool8_t scene_rendering_stopped;
  VkrRendererError scene_error;
  float32_t scene_output_scale;
  uint32_t scene_render_width;
  uint32_t scene_render_height;
  uint32_t scene_output_width;
  uint32_t scene_output_height;
  uint32_t texture_pending_count;
  uint32_t texture_demanded_missing_count;
  /** One typed request, consumed by the runtime after build returns. */
  VkrSampleTransportAction *transport_action;
  bool8_t mapping_valid;
  bool8_t scene_only;
  bool8_t mouse_captured;
  /* Runtime owns Scene keyboard focus; editor updates it during build. */
  bool8_t *scene_keyboard_focus;
} VkrSampleUiFrame;

typedef struct VkrSampleUiClient {
  void *state;
  bool8_t (*initialize)(void *state, VkrUiDockTree *dock, VkrUiSystem *ui);
  void (*handle_input)(void *state, const InputState *input);
  VkrUiDockInputCapture (*build)(void *state, const VkrSampleUiFrame *frame);
  /** Optional projection of current UI anchors after Scene camera input. */
  void (*project_scene)(void *state, const VkrSampleUiFrame *frame);
  bool8_t (*shutdown)(void *state, const VkrUiDockTree *dock, VkrUiSystem *ui);
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
