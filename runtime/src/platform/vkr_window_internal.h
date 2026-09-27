#pragma once

#include "core/vkr_window.h"

/** Initializes the cross-platform scale snapshot before native creation. */
void vkr_window_content_scale_init(VkrWindow *window);

/** Publishes a validated platform scale and advances its revision on change. */
bool8_t vkr_window_content_scale_publish(VkrWindow *window,
                                         float32_t content_scale);

/** Starts a file drop at window pixel (`x`, `y`); false while an earlier drop
 * is still pending, in which case the new drop is ignored. */
bool8_t vkr_window_file_drop_begin(VkrWindow *window, int32_t x, int32_t y);
/** Adds one UTF-8 path of `length` bytes to the drop being built. */
void vkr_window_file_drop_add(VkrWindow *window, const char *path,
                              uint64_t length);
/** Publishes the drop being built when it holds a path. */
void vkr_window_file_drop_publish(VkrWindow *window);
