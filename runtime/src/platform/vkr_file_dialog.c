#include "core/vkr_window.h"
#include "platform/vkr_file_dialog_internal.h"

void vkr_file_dialog_result_destroy(VkrAllocator *allocator,
                                    VkrFileDialogResult *result) {
  if (!result) {
    return;
  }
  for (uint32_t i = 0; i < result->path_count; ++i) {
    if (result->paths[i]) {
      vkr_allocator_free(allocator, result->paths[i],
                         strlen(result->paths[i]) + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
  }
  if (result->paths) {
    vkr_allocator_free(allocator, result->paths,
                       sizeof(*result->paths) * result->path_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  MemZero(result, sizeof(*result));
}

bool8_t vkr_file_dialog_allocate_paths(VkrAllocator *allocator,
                                       VkrFileDialogResult *result,
                                       uint32_t count) {
  if (count == 0) {
    return false_v;
  }
  result->paths = vkr_allocator_alloc(allocator, sizeof(*result->paths) * count,
                                      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!result->paths) {
    return false_v;
  }
  result->path_count = count;
  MemZero(result->paths, sizeof(*result->paths) * count);
  return true_v;
}

void vkr_file_dialog_show(VkrWindow *window,
                          const VkrFileDialogRequest *request,
                          VkrAllocator *allocator,
                          VkrFileDialogResult *result) {
  if (!result) {
    return;
  }
  MemZero(result, sizeof(*result));
  if (!window || !window->platform_state || !request || !allocator ||
      request->kind < VKR_FILE_DIALOG_OPEN_FILE ||
      request->kind > VKR_FILE_DIALOG_SAVE_FILE ||
      (request->multiple && request->kind != VKR_FILE_DIALOG_OPEN_FILE) ||
      request->extension_count > 64 ||
      (request->extension_count && !request->extensions)) {
    snprintf(result->diagnostic, sizeof(result->diagnostic),
             "Invalid native file dialog request.");
    return;
  }
  for (uint32_t i = 0; i < request->extension_count; ++i) {
    const char *extension = request->extensions[i];
    if (!extension || !extension[0] || strlen(extension) > 32 ||
        strspn(
            extension,
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") !=
            strlen(extension)) {
      snprintf(result->diagnostic, sizeof(result->diagnostic),
               "Invalid file extension filter.");
      return;
    }
  }
  vkr_file_dialog_native_show(window, request, allocator, result);
  if (result->status != VKR_FILE_DIALOG_SELECTED && result->paths) {
    VkrFileDialogStatus status = result->status;
    char diagnostic[sizeof(result->diagnostic)];
    MemCopy(diagnostic, result->diagnostic, sizeof(diagnostic));
    vkr_file_dialog_result_destroy(allocator, result);
    result->status = status;
    MemCopy(result->diagnostic, diagnostic, sizeof(diagnostic));
  }
}
