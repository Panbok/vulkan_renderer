#pragma once

#include "vkr_sample_runtime.h"

typedef struct VkrEditorPhysicsSettings VkrEditorPhysicsSettings;
VkrEditorPhysicsSettings *
vkr_editor_physics_settings_create(VkrAllocator *allocator);
void vkr_editor_physics_settings_destroy(VkrEditorPhysicsSettings *settings);
void vkr_editor_physics_settings_build(VkrEditorPhysicsSettings *settings,
                                       const VkrSampleUiFrame *frame,
                                       VkrUiRect rect, VkrFontHandle heading);
