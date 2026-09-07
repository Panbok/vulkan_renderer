/**
 * @file vkr_ui_id.h
 * @brief Stable hierarchical identities for immediate-mode UI calls.
 */
#pragma once

#include "containers/str.h"
#include "defines.h"

typedef uint64_t VkrUiId;

#define VKR_UI_ID_NONE UINT64_C(0)
#define VKR_UI_ID_STACK_CAPACITY 32u

typedef struct VkrUiIdStack {
  VkrUiId ids[VKR_UI_ID_STACK_CAPACITY];
  uint32_t count;
} VkrUiIdStack;

/** Root identity used when no caller scope has been pushed. */
VkrUiId vkr_ui_id_root(void);

/** FNV-1a extension used by subtree and draw-content hashing. */
uint64_t vkr_ui_hash_bytes(uint64_t hash, const void *bytes,
                           uint64_t byte_count);

VkrUiId vkr_ui_id_from_label(VkrUiId parent, String8 label);
VkrUiId vkr_ui_id_from_u64(VkrUiId parent, uint64_t key);
VkrUiId vkr_ui_id_from_pointer(VkrUiId parent, const void *pointer);

void vkr_ui_id_stack_init(VkrUiIdStack *stack);
VkrUiId vkr_ui_id_stack_current(const VkrUiIdStack *stack);
VkrUiId vkr_ui_id_stack_widget_label(const VkrUiIdStack *stack, String8 label);
VkrUiId vkr_ui_id_stack_widget_u64(const VkrUiIdStack *stack, uint64_t key);
bool8_t vkr_ui_id_stack_push_label(VkrUiIdStack *stack, String8 label);
bool8_t vkr_ui_id_stack_push_u64(VkrUiIdStack *stack, uint64_t key);
bool8_t vkr_ui_id_stack_push_pointer(VkrUiIdStack *stack, const void *pointer);
bool8_t vkr_ui_id_stack_pop(VkrUiIdStack *stack);
