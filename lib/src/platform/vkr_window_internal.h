#pragma once

#include "core/vkr_window.h"

/** Initializes the cross-platform scale snapshot before native creation. */
void vkr_window_content_scale_init(VkrWindow *window);

/** Publishes a validated platform scale and advances its revision on change. */
bool8_t vkr_window_content_scale_publish(VkrWindow *window,
                                         float32_t content_scale);
