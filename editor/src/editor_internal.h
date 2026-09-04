#pragma once

#include "editor_ui.h"

#define VKR_EDITOR_METRICS_MENU_X_PT 110.0f
#define VKR_EDITOR_NAVIGATION_HEIGHT_PT 35.0f

VkrUiStyle vkr_editor_glass_style(void);
VkrUiWidgetConfig vkr_editor_text_config(float32_t size_pt, Vec4 color);

void vkr_editor_dock_build(VkrUiSystem *ui, VkrUiDockTree *dock);
void vkr_editor_windows_register_input_layers(VkrEditorUi *editor,
                                              VkrUiSystem *ui);
void vkr_editor_windows_build_navigation(VkrEditorUi *editor, VkrUiSystem *ui);
void vkr_editor_windows_build_floating(VkrEditorUi *editor, VkrUiSystem *ui,
                                       InputState *input,
                                       const VkrSampleUiText *text);
void vkr_editor_windows_build_menu(VkrEditorUi *editor, VkrUiSystem *ui);
