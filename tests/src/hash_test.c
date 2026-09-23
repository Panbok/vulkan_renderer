#include "hash_test.h"

#include "core/vkr_hash.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Published test vectors: FIPS 180-4 examples for SHA-256 and the CRC-32
// check value for the ASCII digits "123456789".

static void hash_test_expect_sha256(const void *data, uint64_t size,
                                    const char *expected_hex) {
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
  char hex[VKR_SHA256_HEX_SIZE];
  vkr_sha256(data, size, digest);
  vkr_sha256_hex(digest, hex);
  assert(strcmp(hex, expected_hex) == 0);
}

static void test_sha256_known_answers(void) {
  printf("  Running test_sha256_known_answers...\n");
  hash_test_expect_sha256(
      "", 0u,
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  hash_test_expect_sha256(
      "abc", 3u,
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const char *two_blocks =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  hash_test_expect_sha256(
      two_blocks, strlen(two_blocks),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // One million 'a' fed in uneven pieces exercises every buffering path.
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  uint8_t chunk[997];
  memset(chunk, 'a', sizeof(chunk));
  uint64_t remaining = 1000000u;
  uint64_t piece = 1u;
  while (remaining > 0u) {
    const uint64_t size = piece < remaining ? piece : remaining;
    vkr_sha256_update(&hash, chunk, size);
    remaining -= size;
    // Pieces stay within 1..sizeof(chunk) and vary across block edges.
    piece = (piece + 61u) % sizeof(chunk) + 1u;
  }
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
  char hex[VKR_SHA256_HEX_SIZE];
  vkr_sha256_final(&hash, digest);
  vkr_sha256_hex(digest, hex);
  assert(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d3"
                     "9ccc7112cd0") == 0);
  printf("  test_sha256_known_answers PASSED\n");
}

static void test_crc32_known_answers(void) {
  printf("  Running test_crc32_known_answers...\n");
  assert(vkr_crc32("123456789", 9u) == 0xcbf43926u);
  assert(vkr_crc32("", 0u) == 0u);

  // Streaming, the accelerated path and the portable table agree on lengths
  // that cover every tail size of an eight-byte step.
  uint8_t bytes[259];
  for (uint32_t i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = (uint8_t)(i * 131u + 7u);
  }
  for (uint32_t length = 0; length <= sizeof(bytes); ++length) {
    const uint32_t whole = vkr_crc32(bytes, length);
    const uint32_t portable =
        ~vkr_crc32_update_portable(VKR_CRC32_INITIAL, bytes, length);
    const uint32_t split = length / 3u;
    uint32_t state = vkr_crc32_update(VKR_CRC32_INITIAL, bytes, split);
    state = vkr_crc32_update(state, bytes + split, length - split);
    assert(whole == portable);
    assert(whole == ~state);
  }
  printf("  test_crc32_known_answers PASSED\n");
}

bool32_t run_hash_tests(void) {
  printf("--- Starting Hash Tests ---\n");
  test_sha256_known_answers();
  test_crc32_known_answers();
  printf("--- Hash Tests Completed ---\n");
  return true;
}
