#pragma once

#include "vkr_sample_runtime.h"

typedef enum VkrEditorMenu {
  VKR_EDITOR_MENU_NONE = 0,
  VKR_EDITOR_MENU_METRICS,
} VkrEditorMenu;

typedef enum VkrEditorWindowKind {
  VKR_EDITOR_WINDOW_DRAWS = 0,
  VKR_EDITOR_WINDOW_MEMORY,
  VKR_EDITOR_WINDOW_HELP,
  VKR_EDITOR_WINDOW_COUNT,
} VkrEditorWindowKind;

typedef struct VkrEditorWindowState {
  Vec2 position_pt;
  Vec2 size_pt;
  uint32_t z_order;
  bool8_t visible;
} VkrEditorWindowState;

typedef struct VkrEditorUi {
  VkrEditorMenu menu;
  VkrEditorWindowState windows[VKR_EDITOR_WINDOW_COUNT];
} VkrEditorUi;

void vkr_editor_ui_init(VkrEditorUi *editor);
VkrUiDockInputCapture vkr_editor_ui_build(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame);
