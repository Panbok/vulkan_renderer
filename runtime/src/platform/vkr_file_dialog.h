#pragma once

#include "memory/vkr_allocator.h"

typedef struct VkrWindow VkrWindow;

typedef enum VkrFileDialogKind {
  VKR_FILE_DIALOG_OPEN_FILE,
  VKR_FILE_DIALOG_OPEN_FOLDER,
  VKR_FILE_DIALOG_SAVE_FILE,
} VkrFileDialogKind;

typedef enum VkrFileDialogStatus {
  VKR_FILE_DIALOG_ERROR,
  VKR_FILE_DIALOG_CANCELLED,
  VKR_FILE_DIALOG_SELECTED,
} VkrFileDialogStatus;

typedef struct VkrFileDialogRequest {
  VkrFileDialogKind kind;
  const char *title;
  const char *initial_directory;
  const char *suggested_name;
  /** Plain extensions without dots or wildcards, e.g. {"gltf", "glb"}. */
  const char *const *extensions;
  uint32_t extension_count;
  bool8_t multiple;
} VkrFileDialogRequest;

typedef struct VkrFileDialogResult {
  VkrFileDialogStatus status;
  char **paths;
  uint32_t path_count;
  char diagnostic[512];
} VkrFileDialogResult;

/** Synchronous modal operation on the window's UI thread. Request strings are
 * borrowed for this call. Pass an empty result. Selected paths are individually
 * allocated, null-terminated UTF-8, stable until result_destroy with the same
 * allocator. The caller owns that allocator and its lifetime. No GPU resources.
 * Filters do not validate imported formats; callers must validate selected
 * input. After dismissal the caller must skip the covered UI's remaining frame
 * work. */
void vkr_file_dialog_show(VkrWindow *window,
                          const VkrFileDialogRequest *request,
                          VkrAllocator *allocator, VkrFileDialogResult *result);
void vkr_file_dialog_result_destroy(VkrAllocator *allocator,
                                    VkrFileDialogResult *result);
