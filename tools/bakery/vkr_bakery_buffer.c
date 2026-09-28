#include "vkr_bakery_buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

vkr_internal bool8_t vkr_bakery_buffer_reserve(VkrBakeryBuffer *buffer,
                                               uint64_t extra) {
  if (buffer->failed) {
    return false_v;
  }
  const uint64_t required = buffer->length + extra + 1u;
  if (required <= buffer->capacity) {
    return true_v;
  }
  uint64_t capacity = buffer->capacity ? buffer->capacity : 256u;
  while (capacity < required) {
    capacity *= 2u;
  }
  uint8_t *data = (uint8_t *)realloc(buffer->data, (size_t)capacity);
  if (!data) {
    buffer->failed = true_v;
    return false_v;
  }
  buffer->data = data;
  buffer->capacity = capacity;
  return true_v;
}

void vkr_bakery_buffer_append(VkrBakeryBuffer *buffer, const void *bytes,
                              uint64_t length) {
  if (!vkr_bakery_buffer_reserve(buffer, length)) {
    return;
  }
  if (length) {
    MemCopy(buffer->data + buffer->length, bytes, (size_t)length);
  }
  buffer->length += length;
  buffer->data[buffer->length] = 0u;
}

void vkr_bakery_buffer_append_cstr(VkrBakeryBuffer *buffer, const char *text) {
  vkr_bakery_buffer_append(buffer, text, text ? strlen(text) : 0u);
}

void vkr_bakery_buffer_appendv(VkrBakeryBuffer *buffer, const char *format,
                               va_list arguments) {
  va_list measure;
  va_copy(measure, arguments);
  const int needed = vsnprintf(NULL, 0, format, measure);
  va_end(measure);
  if (needed < 0) {
    buffer->failed = true_v;
    return;
  }
  if (!vkr_bakery_buffer_reserve(buffer, (uint64_t)needed)) {
    return;
  }
  (void)vsnprintf((char *)buffer->data + buffer->length, (size_t)needed + 1u,
                  format, arguments);
  buffer->length += (uint64_t)needed;
}

void vkr_bakery_buffer_appendf(VkrBakeryBuffer *buffer, const char *format,
                               ...) {
  va_list arguments;
  va_start(arguments, format);
  vkr_bakery_buffer_appendv(buffer, format, arguments);
  va_end(arguments);
}

void vkr_bakery_buffer_reset(VkrBakeryBuffer *buffer) {
  buffer->length = 0u;
  buffer->failed = false_v;
  if (buffer->data) {
    buffer->data[0] = 0u;
  }
}

void vkr_bakery_buffer_free(VkrBakeryBuffer *buffer) {
  free(buffer->data);
  MemZero(buffer, sizeof(*buffer));
}
