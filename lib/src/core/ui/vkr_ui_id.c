#include "core/ui/vkr_ui_id.h"

#include <stdint.h>

#define VKR_UI_FNV1A64_OFFSET_BASIS UINT64_C(14695981039346656037)
#define VKR_UI_FNV1A64_PRIME UINT64_C(1099511628211)

typedef enum VkrUiIdDomain {
  VKR_UI_ID_DOMAIN_LABEL = 1,
  VKR_UI_ID_DOMAIN_U64 = 2,
  VKR_UI_ID_DOMAIN_POINTER = 3,
} VkrUiIdDomain;

uint64_t vkr_ui_hash_bytes(uint64_t hash, const void *bytes,
                           uint64_t byte_count) {
  const uint8_t *data = bytes;
  for (uint64_t i = 0u; i < byte_count; ++i) {
    hash ^= data[i];
    hash *= VKR_UI_FNV1A64_PRIME;
  }
  return hash;
}

VkrUiId vkr_ui_id_root(void) { return VKR_UI_FNV1A64_OFFSET_BASIS; }

static uint64_t vkr_ui_hash_u64_le(uint64_t hash, uint64_t value) {
  uint8_t bytes[8];
  for (uint32_t i = 0u; i < ArrayCount(bytes); ++i)
    bytes[i] = (uint8_t)(value >> (i * 8u));
  return vkr_ui_hash_bytes(hash, bytes, sizeof(bytes));
}

static VkrUiId vkr_ui_id_begin(VkrUiId parent, VkrUiIdDomain domain) {
  uint64_t hash = vkr_ui_hash_u64_le(VKR_UI_FNV1A64_OFFSET_BASIS, parent);
  const uint8_t tag = (uint8_t)domain;
  return vkr_ui_hash_bytes(hash, &tag, sizeof(tag));
}

static VkrUiId vkr_ui_id_nonzero(VkrUiId id) {
  return id == VKR_UI_ID_NONE ? vkr_ui_id_root() : id;
}

VkrUiId vkr_ui_id_from_label(VkrUiId parent, String8 label) {
  if (label.length > 0u && !label.str)
    return VKR_UI_ID_NONE;
  const VkrUiId hash = vkr_ui_hash_bytes(
      vkr_ui_id_begin(parent, VKR_UI_ID_DOMAIN_LABEL), label.str, label.length);
  return vkr_ui_id_nonzero(hash);
}

VkrUiId vkr_ui_id_from_u64(VkrUiId parent, uint64_t key) {
  return vkr_ui_id_nonzero(
      vkr_ui_hash_u64_le(vkr_ui_id_begin(parent, VKR_UI_ID_DOMAIN_U64), key));
}

VkrUiId vkr_ui_id_from_pointer(VkrUiId parent, const void *pointer) {
  return vkr_ui_id_nonzero(
      vkr_ui_hash_u64_le(vkr_ui_id_begin(parent, VKR_UI_ID_DOMAIN_POINTER),
                         (uint64_t)(uintptr_t)pointer));
}

void vkr_ui_id_stack_init(VkrUiIdStack *stack) {
  if (!stack)
    return;
  *stack = (VkrUiIdStack){.ids = {vkr_ui_id_root()}, .count = 1u};
}

VkrUiId vkr_ui_id_stack_current(const VkrUiIdStack *stack) {
  return stack && stack->count > 0u && stack->count <= ArrayCount(stack->ids)
             ? stack->ids[stack->count - 1u]
             : VKR_UI_ID_NONE;
}

VkrUiId vkr_ui_id_stack_widget_label(const VkrUiIdStack *stack, String8 label) {
  const VkrUiId parent = vkr_ui_id_stack_current(stack);
  return parent ? vkr_ui_id_from_label(parent, label) : VKR_UI_ID_NONE;
}

VkrUiId vkr_ui_id_stack_widget_u64(const VkrUiIdStack *stack, uint64_t key) {
  const VkrUiId parent = vkr_ui_id_stack_current(stack);
  return parent ? vkr_ui_id_from_u64(parent, key) : VKR_UI_ID_NONE;
}

static bool8_t vkr_ui_id_stack_push(VkrUiIdStack *stack, VkrUiId id) {
  if (!stack || id == VKR_UI_ID_NONE || stack->count == 0u ||
      stack->count >= ArrayCount(stack->ids))
    return false_v;
  stack->ids[stack->count++] = id;
  return true_v;
}

bool8_t vkr_ui_id_stack_push_label(VkrUiIdStack *stack, String8 label) {
  return vkr_ui_id_stack_push(stack,
                              vkr_ui_id_stack_widget_label(stack, label));
}

bool8_t vkr_ui_id_stack_push_u64(VkrUiIdStack *stack, uint64_t key) {
  return vkr_ui_id_stack_push(stack, vkr_ui_id_stack_widget_u64(stack, key));
}

bool8_t vkr_ui_id_stack_push_pointer(VkrUiIdStack *stack, const void *pointer) {
  const VkrUiId parent = vkr_ui_id_stack_current(stack);
  return vkr_ui_id_stack_push(
      stack, parent ? vkr_ui_id_from_pointer(parent, pointer) : VKR_UI_ID_NONE);
}

bool8_t vkr_ui_id_stack_pop(VkrUiIdStack *stack) {
  if (!stack || stack->count <= 1u || stack->count > ArrayCount(stack->ids))
    return false_v;
  stack->count--;
  return true_v;
}
