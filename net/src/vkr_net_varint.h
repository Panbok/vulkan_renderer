#pragma once

#include "defines.h"

/* Variable-length integers of the network transport: the QUIC encoding
 * (RFC 9000, section 16). The two high bits of the first byte select 1, 2,
 * 4 or 8 bytes; the value follows in network byte order and holds up to
 * 2^62 - 1. Frame-level fields use it; fixed-width fields are
 * little-endian. */

#define VKR_NET_VARINT_MAX ((1ull << 62) - 1u)

static inline uint32_t vkr_net_varint_size(uint64_t value) {
  if (value < (1ull << 6)) {
    return 1u;
  }
  if (value < (1ull << 14)) {
    return 2u;
  }
  if (value < (1ull << 30)) {
    return 4u;
  }
  return 8u;
}

/* Writes `value` (at most VKR_NET_VARINT_MAX) and returns its size, or zero
   when it does not fit before `end`. */
static inline uint32_t vkr_net_varint_write(uint8_t *dst, const uint8_t *end,
                                            uint64_t value) {
  const uint32_t size = vkr_net_varint_size(value);
  if (value > VKR_NET_VARINT_MAX || (uint64_t)(end - dst) < size) {
    return 0u;
  }
  const uint8_t prefix = size == 1u   ? 0x00u
                         : size == 2u ? 0x40u
                         : size == 4u ? 0x80u
                                      : 0xc0u;
  for (uint32_t i = 0u; i < size; ++i) {
    dst[i] = (uint8_t)(value >> (8u * (size - 1u - i)));
  }
  dst[0] |= prefix;
  return size;
}

/* Reads one value and returns its size, or zero when `src` ends first. */
static inline uint32_t vkr_net_varint_read(const uint8_t *src,
                                           const uint8_t *end,
                                           uint64_t *out_value) {
  if (src >= end) {
    return 0u;
  }
  const uint32_t size = 1u << (src[0] >> 6);
  if ((uint64_t)(end - src) < size) {
    return 0u;
  }
  uint64_t value = src[0] & 0x3fu;
  for (uint32_t i = 1u; i < size; ++i) {
    value = (value << 8) | src[i];
  }
  *out_value = value;
  return size;
}

/* The full packet number nearest the next expected one (RFC 9000, appendix
   A.3), from its low `bits` bits. */
static inline uint64_t vkr_net_packet_number_decode(uint64_t largest_received,
                                                    uint64_t truncated,
                                                    uint32_t bits) {
  const uint64_t expected = largest_received + 1u;
  const uint64_t window = 1ull << bits;
  const uint64_t half = window / 2u;
  const uint64_t mask = window - 1u;
  const uint64_t candidate = (expected & ~mask) | truncated;
  if (candidate + half <= expected && candidate < (1ull << 62) - window) {
    return candidate + window;
  }
  if (candidate > expected + half && candidate >= window) {
    return candidate - window;
  }
  return candidate;
}
