#pragma once

#include "defines.h"

/**
 * Content hashes shared by asset formats, caches, cookers and the harness.
 *
 * SHA-256 follows FIPS 180-4. CRC-32 is the reflected IEEE 802.3 polynomial
 * (0xEDB88320) with an all-ones initial state and final inversion, the variant
 * used by zlib and PNG, so every stored checksum keeps its existing value.
 */

#define VKR_SHA256_DIGEST_SIZE 32u
/** Lowercase hexadecimal digest plus its terminator. */
#define VKR_SHA256_HEX_SIZE 65u

/** Incremental SHA-256 state, so hashed input never needs one buffer. */
typedef struct VkrSha256 {
  uint32_t state[8];
  uint64_t bit_length;
  uint8_t block[64];
  uint32_t block_length;
} VkrSha256;

void vkr_sha256_init(VkrSha256 *hash);
void vkr_sha256_update(VkrSha256 *hash, const void *data, uint64_t size);
/** Completes the digest; `hash` must be re-initialized before reuse. */
void vkr_sha256_final(VkrSha256 *hash, uint8_t digest[VKR_SHA256_DIGEST_SIZE]);
void vkr_sha256(const void *data, uint64_t size,
                uint8_t digest[VKR_SHA256_DIGEST_SIZE]);
void vkr_sha256_hex(const uint8_t digest[VKR_SHA256_DIGEST_SIZE],
                    char out[VKR_SHA256_HEX_SIZE]);

/**
 * Continues a CRC-32 over more bytes. Start from VKR_CRC32_INITIAL and invert
 * the final state, or call vkr_crc32() for one contiguous buffer.
 */
#define VKR_CRC32_INITIAL 0xffffffffu
uint32_t vkr_crc32_update(uint32_t state, const void *data, uint64_t size);
uint32_t vkr_crc32(const void *data, uint64_t size);
/**
 * Table-driven reference used where CRC instructions are unavailable; tests
 * compare it with vkr_crc32_update on hosts that have the instructions.
 */
uint32_t vkr_crc32_update_portable(uint32_t state, const void *data,
                                   uint64_t size);
