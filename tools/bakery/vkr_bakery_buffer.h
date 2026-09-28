#pragma once

#include "defines.h"

#include <stdarg.h>

/* Growable contiguous byte buffer for serialized text. The owner calls
 * vkr_bakery_buffer_free once; growth invalidates earlier `data` pointers.
 * A failed allocation latches `failed` and turns later appends into no-ops,
 * so callers check once after building. The contents are always followed by
 * a NUL byte that is not counted in `length`. */
typedef struct VkrBakeryBuffer {
  uint8_t *data;
  uint64_t length;
  uint64_t capacity;
  bool8_t failed;
} VkrBakeryBuffer;

void vkr_bakery_buffer_append(VkrBakeryBuffer *buffer, const void *bytes,
                              uint64_t length);
void vkr_bakery_buffer_append_cstr(VkrBakeryBuffer *buffer, const char *text);
void vkr_bakery_buffer_appendf(VkrBakeryBuffer *buffer, const char *format,
                               ...);
void vkr_bakery_buffer_appendv(VkrBakeryBuffer *buffer, const char *format,
                               va_list arguments);
void vkr_bakery_buffer_reset(VkrBakeryBuffer *buffer);
void vkr_bakery_buffer_free(VkrBakeryBuffer *buffer);
