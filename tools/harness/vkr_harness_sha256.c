#include "vkr_harness.h"

// Report digests are "sha256:" followed by the lowercase hexadecimal digest.
void vkr_harness_sha256_end(VkrSha256 *hash,
                            char out_digest[VKR_HARNESS_DIGEST_MAX]) {
  _Static_assert(VKR_HARNESS_DIGEST_MAX == 7u + VKR_SHA256_HEX_SIZE,
                 "Harness digests hold the prefix and one hex digest");
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
  vkr_sha256_final(hash, digest);
  MemCopy(out_digest, "sha256:", 7u);
  vkr_sha256_hex(digest, out_digest + 7u);
}

void vkr_harness_sha256_bytes(const void *data, uint64_t length,
                              char out_digest[VKR_HARNESS_DIGEST_MAX]) {
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  vkr_sha256_update(&hash, data, length);
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
  VkrSha256 hash;
  vkr_sha256_init(&hash);
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
    vkr_sha256_update(&hash, buffer, bytes_read);
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
