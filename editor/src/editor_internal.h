#pragma once

#include "editor_ui.h"

#define VKR_EDITOR_SCENE_TOOLBAR_LAYER 1u
#define VKR_EDITOR_METRICS_MENU_X_PT 110.0f
#define VKR_EDITOR_NAVIGATION_HEIGHT_PT VKR_UI_DOCK_TOOLBAR_PT

VkrUiStyle vkr_editor_glass_style(void);
VkrUiWidgetConfig vkr_editor_text_config(float32_t size_pt, Vec4 color);
void vkr_editor_field_style(VkrUiWidgetConfig *config);
void vkr_editor_action_style(VkrUiWidgetConfig *config, VkrFontHandle heading);

void vkr_editor_dock_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame);
void vkr_editor_dock_show(VkrUiDockTree *dock, VkrUiDockPanelKind kind);
void vkr_editor_dock_toggle(VkrUiDockTree *dock, VkrUiDockPanelKind kind);
void vkr_editor_scene_toolbar_update(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame);
void vkr_editor_scene_toolbar_build(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame);
void vkr_editor_windows_register_input_layers(VkrEditorUi *editor,
                                              VkrUiSystem *ui);
void vkr_editor_windows_build_navigation(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame);
void vkr_editor_windows_build_floating(VkrEditorUi *editor, VkrUiSystem *ui,
                                       InputState *input,
                                       const VkrSampleUiText *text);
void vkr_editor_windows_build_menu(VkrEditorUi *editor, VkrUiSystem *ui);

void vkr_editor_commands_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
void vkr_editor_commands_build(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame);
