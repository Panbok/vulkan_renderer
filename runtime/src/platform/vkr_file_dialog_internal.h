#pragma once
#include "platform/vkr_file_dialog.h"

void vkr_file_dialog_native_show(VkrWindow *window,
                                 const VkrFileDialogRequest *request,
                                 VkrAllocator *allocator,
                                 VkrFileDialogResult *result);
bool8_t vkr_file_dialog_allocate_paths(VkrAllocator *allocator,
                                       VkrFileDialogResult *result,
                                       uint32_t count);
