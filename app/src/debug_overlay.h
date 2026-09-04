#pragma once

#include "core/input.h"
#include "core/vkr_text.h"
#include "renderer/systems/vkr_ui_system.h"

bool8_t vkr_debug_overlay_toggle_requested(const InputState *input);
void vkr_debug_overlay_build(VkrUiSystem *ui, String8 camera_text,
                             String8 performance_text);
