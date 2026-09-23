#include "core/vkr_hash.h"

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

static const uint32_t vkr_sha256_round_constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

vkr_internal uint32_t vkr_sha256_rotr(uint32_t value, uint32_t bits) {
  return (value >> bits) | (value << (32u - bits));
}

vkr_internal void vkr_sha256_transform(VkrSha256 *hash,
                                       const uint8_t block[64]) {
  uint32_t words[64];
  for (uint32_t i = 0; i < 16u; ++i) {
    words[i] = ((uint32_t)block[i * 4u] << 24u) |
               ((uint32_t)block[i * 4u + 1u] << 16u) |
               ((uint32_t)block[i * 4u + 2u] << 8u) |
               (uint32_t)block[i * 4u + 3u];
  }
  for (uint32_t i = 16u; i < 64u; ++i) {
    const uint32_t s0 = vkr_sha256_rotr(words[i - 15u], 7u) ^
                        vkr_sha256_rotr(words[i - 15u], 18u) ^
                        (words[i - 15u] >> 3u);
    const uint32_t s1 = vkr_sha256_rotr(words[i - 2u], 17u) ^
                        vkr_sha256_rotr(words[i - 2u], 19u) ^
                        (words[i - 2u] >> 10u);
    words[i] = words[i - 16u] + s0 + words[i - 7u] + s1;
  }
  uint32_t a = hash->state[0];
  uint32_t b = hash->state[1];
  uint32_t c = hash->state[2];
  uint32_t d = hash->state[3];
  uint32_t e = hash->state[4];
  uint32_t f = hash->state[5];
  uint32_t g = hash->state[6];
  uint32_t h = hash->state[7];
  for (uint32_t i = 0; i < 64u; ++i) {
    const uint32_t sum1 = vkr_sha256_rotr(e, 6u) ^ vkr_sha256_rotr(e, 11u) ^
                          vkr_sha256_rotr(e, 25u);
    const uint32_t choose = (e & f) ^ ((~e) & g);
    const uint32_t t1 =
        h + sum1 + choose + vkr_sha256_round_constants[i] + words[i];
    const uint32_t sum0 = vkr_sha256_rotr(a, 2u) ^ vkr_sha256_rotr(a, 13u) ^
                          vkr_sha256_rotr(a, 22u);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  hash->state[0] += a;
  hash->state[1] += b;
  hash->state[2] += c;
  hash->state[3] += d;
  hash->state[4] += e;
  hash->state[5] += f;
  hash->state[6] += g;
  hash->state[7] += h;
}

void vkr_sha256_init(VkrSha256 *hash) {
  *hash = (VkrSha256){
      .state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu,
                0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u},
  };
}

void vkr_sha256_update(VkrSha256 *hash, const void *data, uint64_t size) {
  const uint8_t *bytes = data;
  if (hash->block_length > 0u) {
    const uint64_t available = 64u - hash->block_length;
    const uint64_t copied = size < available ? size : available;
    MemCopy(hash->block + hash->block_length, bytes, copied);
    hash->block_length += (uint32_t)copied;
    bytes += copied;
    size -= copied;
    if (hash->block_length == 64u) {
      vkr_sha256_transform(hash, hash->block);
      hash->bit_length += 512u;
      hash->block_length = 0;
    }
  }
  while (size >= 64u) {
    vkr_sha256_transform(hash, bytes);
    hash->bit_length += 512u;
    bytes += 64u;
    size -= 64u;
  }
  if (size > 0u) {
    MemCopy(hash->block, bytes, size);
    hash->block_length = (uint32_t)size;
  }
}

void vkr_sha256_final(VkrSha256 *hash, uint8_t digest[VKR_SHA256_DIGEST_SIZE]) {
  uint32_t i = hash->block_length;
  hash->block[i++] = 0x80u;
  if (i > 56u) {
    while (i < 64u) {
      hash->block[i++] = 0;
    }
    vkr_sha256_transform(hash, hash->block);
    i = 0;
  }
  while (i < 56u) {
    hash->block[i++] = 0;
  }
  hash->bit_length += (uint64_t)hash->block_length * 8u;
  for (uint32_t byte = 0; byte < 8u; ++byte) {
    hash->block[63u - byte] = (uint8_t)(hash->bit_length >> (byte * 8u));
  }
  vkr_sha256_transform(hash, hash->block);
  for (uint32_t word = 0; word < 8u; ++word) {
    digest[word * 4u] = (uint8_t)(hash->state[word] >> 24u);
    digest[word * 4u + 1u] = (uint8_t)(hash->state[word] >> 16u);
    digest[word * 4u + 2u] = (uint8_t)(hash->state[word] >> 8u);
    digest[word * 4u + 3u] = (uint8_t)hash->state[word];
  }
}

void vkr_sha256(const void *data, uint64_t size,
                uint8_t digest[VKR_SHA256_DIGEST_SIZE]) {
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  vkr_sha256_update(&hash, data, size);
  vkr_sha256_final(&hash, digest);
}

void vkr_sha256_hex(const uint8_t digest[VKR_SHA256_DIGEST_SIZE],
                    char out[VKR_SHA256_HEX_SIZE]) {
  static const char hex[] = "0123456789abcdef";
  for (uint32_t i = 0; i < VKR_SHA256_DIGEST_SIZE; ++i) {
    out[i * 2u] = hex[digest[i] >> 4u];
    out[i * 2u + 1u] = hex[digest[i] & 0x0fu];
  }
  out[VKR_SHA256_HEX_SIZE - 1u] = '\0';
}

static const uint32_t vkr_crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u,
    0x706af48fu, 0xe963a535u, 0x9e6495a3u, 0x0edb8832u, 0x79dcb8a4u,
    0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u,
    0x90bf1d91u, 0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu,
    0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u, 0x136c9856u,
    0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u,
    0xfa0f3d63u, 0x8d080df5u, 0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u,
    0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u,
    0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u, 0x26d930acu, 0x51de003au,
    0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u,
    0xb8bda50fu, 0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u,
    0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du, 0x76dc4190u,
    0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu,
    0x9fbfe4a5u, 0xe8b8d433u, 0x7807c9a2u, 0x0f00f934u, 0x9609a88eu,
    0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu,
    0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u, 0x65b0d9c6u, 0x12b7e950u,
    0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u,
    0xfbd44c65u, 0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u,
    0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu, 0x4369e96au,
    0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u,
    0xaa0a4c5fu, 0xdd0d7cc9u, 0x5005713cu, 0x270241aau, 0xbe0b1010u,
    0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u,
    0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu, 0xedb88320u, 0x9abfb3b6u,
    0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u,
    0x73dc1683u, 0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u,
    0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u, 0xf00f9344u,
    0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu,
    0x196c3671u, 0x6e6b06e7u, 0xfed41b76u, 0x89d32be0u, 0x10da7a5au,
    0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u,
    0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu, 0xd80d2bdau, 0xaf0a1b4cu,
    0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu,
    0x4669be79u, 0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u,
    0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu, 0xc5ba3bbeu,
    0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u,
    0x2cd99e8bu, 0x5bdeae1du, 0x9b64c2b0u, 0xec63f226u, 0x756aa39cu,
    0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu,
    0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u, 0x86d3d2d4u, 0xf1d4e242u,
    0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u,
    0x18b74777u, 0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu,
    0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u, 0xa00ae278u,
    0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u,
    0x4969474du, 0x3e6e77dbu, 0xaed16a4au, 0xd9d65adcu, 0x40df0b66u,
    0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u,
    0xcdd70693u, 0x54de5729u, 0x23d967bfu, 0xb3667a2eu, 0xc4614ab8u,
    0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu,
    0x2d02ef8du,
};

uint32_t vkr_crc32_update_portable(uint32_t state, const void *data,
                                   uint64_t size) {
  const uint8_t *bytes = data;
  for (uint64_t i = 0; i < size; ++i) {
    state = vkr_crc32_table[(state ^ bytes[i]) & 0xffu] ^ (state >> 8u);
  }
  return state;
}

uint32_t vkr_crc32_update(uint32_t state, const void *data, uint64_t size) {
#if defined(__ARM_FEATURE_CRC32)
  // The ARMv8 CRC32 instructions implement this reflected polynomial and
  // consume a doubleword least-significant byte first, matching byte order.
  const uint8_t *bytes = data;
  while (size >= 8u) {
    uint64_t word = 0;
    MemCopy(&word, bytes, sizeof(word));
    state = __crc32d(state, word);
    bytes += 8u;
    size -= 8u;
  }
  while (size > 0u) {
    state = __crc32b(state, *bytes);
    bytes++;
    size--;
  }
  return state;
#else
  return vkr_crc32_update_portable(state, data, size);
#endif
}

uint32_t vkr_crc32(const void *data, uint64_t size) {
  return ~vkr_crc32_update(VKR_CRC32_INITIAL, data, size);
}
