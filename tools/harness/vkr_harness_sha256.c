#include "vkr_harness.h"

static const uint32_t vkr_harness_sha256_constants[64] = {
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

static uint32_t vkr_harness_rotr(uint32_t value, uint32_t bits) {
  return (value >> bits) | (value << (32u - bits));
}

static void vkr_harness_sha256_transform(VkrHarnessSha256 *hash,
                                         const uint8_t block[64]) {
  uint32_t words[64];
  for (uint32_t i = 0; i < 16; ++i) {
    words[i] = ((uint32_t)block[i * 4u] << 24u) |
               ((uint32_t)block[i * 4u + 1u] << 16u) |
               ((uint32_t)block[i * 4u + 2u] << 8u) |
               (uint32_t)block[i * 4u + 3u];
  }
  for (uint32_t i = 16; i < 64; ++i) {
    const uint32_t s0 = vkr_harness_rotr(words[i - 15u], 7u) ^
                        vkr_harness_rotr(words[i - 15u], 18u) ^
                        (words[i - 15u] >> 3u);
    const uint32_t s1 = vkr_harness_rotr(words[i - 2u], 17u) ^
                        vkr_harness_rotr(words[i - 2u], 19u) ^
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
  for (uint32_t i = 0; i < 64; ++i) {
    const uint32_t sum1 = vkr_harness_rotr(e, 6u) ^ vkr_harness_rotr(e, 11u) ^
                          vkr_harness_rotr(e, 25u);
    const uint32_t choose = (e & f) ^ ((~e) & g);
    const uint32_t t1 =
        h + sum1 + choose + vkr_harness_sha256_constants[i] + words[i];
    const uint32_t sum0 = vkr_harness_rotr(a, 2u) ^ vkr_harness_rotr(a, 13u) ^
                          vkr_harness_rotr(a, 22u);
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

void vkr_harness_sha256_begin(VkrHarnessSha256 *hash) {
  *hash = (VkrHarnessSha256){
      .state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu,
                0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u},
  };
}

void vkr_harness_sha256_update(VkrHarnessSha256 *hash, const void *data,
                               uint64_t length) {
  const uint8_t *bytes = data;
  if (hash->block_length > 0u) {
    const uint64_t available = 64u - hash->block_length;
    const uint64_t copied = length < available ? length : available;
    MemCopy(hash->block + hash->block_length, bytes, copied);
    hash->block_length += (uint32_t)copied;
    bytes += copied;
    length -= copied;
    if (hash->block_length == 64u) {
      vkr_harness_sha256_transform(hash, hash->block);
      hash->bit_length += 512u;
      hash->block_length = 0;
    }
  }

  while (length >= 64u) {
    vkr_harness_sha256_transform(hash, bytes);
    hash->bit_length += 512u;
    bytes += 64u;
    length -= 64u;
  }
  if (length > 0u) {
    MemCopy(hash->block, bytes, length);
    hash->block_length = (uint32_t)length;
  }
}

static void vkr_harness_sha256_final(VkrHarnessSha256 *hash,
                                     uint8_t digest[32]) {
  uint32_t i = hash->block_length;
  hash->block[i++] = 0x80u;
  if (i > 56u) {
    while (i < 64u) {
      hash->block[i++] = 0;
    }
    vkr_harness_sha256_transform(hash, hash->block);
    i = 0;
  }
  while (i < 56u) {
    hash->block[i++] = 0;
  }
  hash->bit_length += (uint64_t)hash->block_length * 8u;
  for (uint32_t byte = 0; byte < 8; ++byte) {
    hash->block[63u - byte] = (uint8_t)(hash->bit_length >> (byte * 8u));
  }
  vkr_harness_sha256_transform(hash, hash->block);
  for (uint32_t word = 0; word < 8; ++word) {
    digest[word * 4u] = (uint8_t)(hash->state[word] >> 24u);
    digest[word * 4u + 1u] = (uint8_t)(hash->state[word] >> 16u);
    digest[word * 4u + 2u] = (uint8_t)(hash->state[word] >> 8u);
    digest[word * 4u + 3u] = (uint8_t)hash->state[word];
  }
}

static void vkr_harness_sha256_format(const uint8_t digest[32],
                                      char out[VKR_HARNESS_DIGEST_MAX]) {
  static const char hex[] = "0123456789abcdef";
  MemCopy(out, "sha256:", 7u);
  for (uint32_t i = 0; i < 32; ++i) {
    out[7u + i * 2u] = hex[digest[i] >> 4u];
    out[8u + i * 2u] = hex[digest[i] & 0x0fu];
  }
  out[VKR_HARNESS_DIGEST_MAX - 1u] = '\0';
}

void vkr_harness_sha256_end(VkrHarnessSha256 *hash,
                            char out_digest[VKR_HARNESS_DIGEST_MAX]) {
  uint8_t digest[32];
  vkr_harness_sha256_final(hash, digest);
  vkr_harness_sha256_format(digest, out_digest);
}

void vkr_harness_sha256_bytes(const void *data, uint64_t length,
                              char out_digest[VKR_HARNESS_DIGEST_MAX]) {
  VkrHarnessSha256 hash;
  vkr_harness_sha256_begin(&hash);
  vkr_harness_sha256_update(&hash, data, length);
  vkr_harness_sha256_end(&hash, out_digest);
}

bool8_t vkr_harness_sha256_file_sized(const char *path,
                                      char out_digest[VKR_HARNESS_DIGEST_MAX],
                                      uint64_t *out_size) {
  if (!path || !out_digest) {
    return false_v;
  }
  FilePath file_path = vkr_harness_file_path(path);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (file_open(&file_path, mode, &file) != FILE_ERROR_NONE) {
    return false_v;
  }
  VkrHarnessSha256 hash;
  vkr_harness_sha256_begin(&hash);
  uint8_t buffer[16384];
  uint64_t size = 0u;
  bool8_t success = true_v;
  for (;;) {
    uint64_t bytes_read = 0u;
    if (file_read_into(&file, buffer, sizeof(buffer), &bytes_read) !=
        FILE_ERROR_NONE) {
      success = false_v;
      break;
    }
    if (bytes_read == 0u) {
      break;
    }
    vkr_harness_sha256_update(&hash, buffer, bytes_read);
    size += bytes_read;
  }
  file_close(&file);
  if (!success) {
    return false_v;
  }
  vkr_harness_sha256_end(&hash, out_digest);
  if (out_size) {
    *out_size = size;
  }
  return true_v;
}

bool8_t vkr_harness_sha256_file(const char *path,
                                char out_digest[VKR_HARNESS_DIGEST_MAX]) {
  return vkr_harness_sha256_file_sized(path, out_digest, NULL);
}
