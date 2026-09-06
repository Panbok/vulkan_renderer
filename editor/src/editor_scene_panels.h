#pragma once
#include "vkr_sample_runtime.h"

typedef struct VkrEditorScenePanels VkrEditorScenePanels;
VkrEditorScenePanels *vkr_editor_scene_panels_create(VkrAllocator *allocator);
void vkr_editor_scene_panels_destroy(VkrEditorScenePanels *panels);
void vkr_editor_hierarchy_build(VkrEditorScenePanels *panels,
                                const VkrSampleUiFrame *frame, VkrUiRect rect,
                                VkrFontHandle heading);
void vkr_editor_inspector_build(VkrEditorScenePanels *panels,
                                const VkrSampleUiFrame *frame, VkrUiRect rect,
                                VkrFontHandle heading);
