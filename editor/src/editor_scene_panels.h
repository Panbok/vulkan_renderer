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

bool8_t vkr_editor_scene_panels_write_json(const VkrEditorScenePanels *panels,
                                           VkrJsonWriter *writer);
bool8_t vkr_editor_scene_panels_read_json(VkrEditorScenePanels *panels,
                                          String8 json);
bool8_t
vkr_editor_scene_panels_write_scene_json(const VkrEditorScenePanels *panels,
                                         const VkrSampleUiFrame *frame,
                                         VkrJsonWriter *writer);
bool8_t vkr_editor_scene_panels_read_scene_json(VkrEditorScenePanels *panels,
                                                const VkrSampleUiFrame *frame,
                                                String8 json);
