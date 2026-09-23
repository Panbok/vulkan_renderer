#pragma once

#include "defines.h"

#include <float.h>

/**
 * Little-endian fixed-width encoding shared by cooked asset formats, caches
 * and tools. Loads and stores work byte by byte, so they are independent of
 * host byte order and alignment.
 *
 * VkrByteReader and VkrByteWriter are bounded cursors over caller-owned
 * storage: an access that would pass `size` fails and leaves `offset` and the
 * output unchanged.
 */

static inline VKR_MAYBE_UNUSED bool8_t vkr_checked_add_u64(uint64_t lhs,
                                                           uint64_t rhs,
                                                           uint64_t *out) {
  if (UINT64_MAX - lhs < rhs) {
    return false_v;
  }
  *out = lhs + rhs;
  return true_v;
}

static inline VKR_MAYBE_UNUSED bool8_t vkr_checked_mul_u64(uint64_t lhs,
                                                           uint64_t rhs,
                                                           uint64_t *out) {
  if (lhs != 0u && rhs > UINT64_MAX / lhs) {
    return false_v;
  }
  *out = lhs * rhs;
  return true_v;
}

/** Rounds `value` up to a power-of-two `alignment`; callers bound `value`. */
static inline VKR_MAYBE_UNUSED uint64_t vkr_align_up_u64(uint64_t value,
                                                         uint64_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

static inline VKR_MAYBE_UNUSED void vkr_store_le_u32(uint8_t *dst,
                                                     uint32_t value) {
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8u);
  dst[2] = (uint8_t)(value >> 16u);
  dst[3] = (uint8_t)(value >> 24u);
}

static inline VKR_MAYBE_UNUSED void vkr_store_le_u64(uint8_t *dst,
                                                     uint64_t value) {
  vkr_store_le_u32(dst, (uint32_t)value);
  vkr_store_le_u32(dst + 4u, (uint32_t)(value >> 32u));
}

static inline VKR_MAYBE_UNUSED uint32_t vkr_load_le_u32(const uint8_t *src) {
  return (uint32_t)src[0] | ((uint32_t)src[1] << 8u) |
         ((uint32_t)src[2] << 16u) | ((uint32_t)src[3] << 24u);
}

static inline VKR_MAYBE_UNUSED uint64_t vkr_load_le_u64(const uint8_t *src) {
  return (uint64_t)vkr_load_le_u32(src) |
         ((uint64_t)vkr_load_le_u32(src + 4u) << 32u);
}

static inline VKR_MAYBE_UNUSED uint32_t vkr_f32_bits(float32_t value) {
  uint32_t bits = 0;
  MemCopy(&bits, &value, sizeof(bits));
  return bits;
}

static inline VKR_MAYBE_UNUSED float32_t vkr_f32_from_bits(uint32_t bits) {
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

/**
 * True when float32_t holds IEEE-754 binary32 bit patterns. Fixed binary
 * formats copy float bits verbatim, so their codecs check this at the cold
 * serialization boundary; the FLT_* limits alone do not prove the object
 * representation on every C11 implementation.
 */
static inline VKR_MAYBE_UNUSED bool8_t vkr_f32_is_binary32(void) {
  const float32_t one = 1.0f;
  const float32_t negative_half = -0.5f;
  return FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128 &&
         vkr_f32_bits(one) == 0x3f800000u &&
         vkr_f32_bits(negative_half) == 0xbf000000u;
}

static inline VKR_MAYBE_UNUSED float32_t vkr_load_le_f32(const uint8_t *src) {
  return vkr_f32_from_bits(vkr_load_le_u32(src));
}

static inline VKR_MAYBE_UNUSED void vkr_store_le_f32(uint8_t *dst,
                                                     float32_t value) {
  vkr_store_le_u32(dst, vkr_f32_bits(value));
}

typedef struct VkrByteReader {
  const uint8_t *data;
  uint64_t size;
  uint64_t offset;
} VkrByteReader;

/** Copies `size` bytes into `out`, or skips them when `out` is NULL. */
static inline VKR_MAYBE_UNUSED bool8_t
vkr_byte_reader_bytes(VkrByteReader *reader, void *out, uint64_t size) {
  if (reader->offset > reader->size || size > reader->size - reader->offset) {
    return false_v;
  }
  if (out && size > 0u) {
    MemCopy(out, reader->data + reader->offset, size);
  }
  reader->offset += size;
  return true_v;
}

static inline VKR_MAYBE_UNUSED bool8_t
vkr_byte_reader_u32(VkrByteReader *reader, uint32_t *out) {
  uint8_t bytes[4];
  if (!vkr_byte_reader_bytes(reader, bytes, sizeof(bytes))) {
    return false_v;
  }
  *out = vkr_load_le_u32(bytes);
  return true_v;
}

static inline VKR_MAYBE_UNUSED bool8_t
vkr_byte_reader_i32(VkrByteReader *reader, int32_t *out) {
  uint32_t value = 0;
  if (!vkr_byte_reader_u32(reader, &value)) {
    return false_v;
  }
  *out = (int32_t)value;
  return true_v;
}

static inline VKR_MAYBE_UNUSED bool8_t
vkr_byte_reader_u64(VkrByteReader *reader, uint64_t *out) {
  uint8_t bytes[8];
  if (!vkr_byte_reader_bytes(reader, bytes, sizeof(bytes))) {
    return false_v;
  }
  *out = vkr_load_le_u64(bytes);
  return true_v;
}

static inline VKR_MAYBE_UNUSED bool8_t
vkr_byte_reader_f32(VkrByteReader *reader, float32_t *out) {
  uint32_t bits = 0;
  if (!vkr_byte_reader_u32(reader, &bits)) {
    return false_v;
  }
  *out = vkr_f32_from_bits(bits);
  return true_v;
}

typedef struct VkrByteWriter {
  uint8_t *data;
  uint64_t size;
  uint64_t offset;
} VkrByteWriter;

static inline VKR_MAYBE_UNUSED bool8_t
vkr_byte_writer_bytes(VkrByteWriter *writer, const void *data, uint64_t size) {
  if (writer->offset > writer->size || size > writer->size - writer->offset) {
    return false_v;
  }
  if (size > 0u) {
    MemCopy(writer->data + writer->offset, data, size);
  }
  writer->offset += size;
  return true_v;
}

static inline VKR_MAYBE_UNUSED bool8_t
vkr_byte_writer_u32(VkrByteWriter *writer, uint32_t value) {
  uint8_t bytes[4];
  vkr_store_le_u32(bytes, value);
  return vkr_byte_writer_bytes(writer, bytes, sizeof(bytes));
}

static inline VKR_MAYBE_UNUSED bool8_t
vkr_byte_writer_i32(VkrByteWriter *writer, int32_t value) {
  return vkr_byte_writer_u32(writer, (uint32_t)value);
}

static inline VKR_MAYBE_UNUSED bool8_t
vkr_byte_writer_u64(VkrByteWriter *writer, uint64_t value) {
  uint8_t bytes[8];
  vkr_store_le_u64(bytes, value);
  return vkr_byte_writer_bytes(writer, bytes, sizeof(bytes));
}

static inline VKR_MAYBE_UNUSED bool8_t
vkr_byte_writer_f32(VkrByteWriter *writer, float32_t value) {
  return vkr_byte_writer_u32(writer, vkr_f32_bits(value));
}
