#include "vkr_net_crypto.h"

#include "core/vkr_byte_io.h"

#include <sodium.h>
#include <string.h>

_Static_assert(sizeof(((VkrNetPacketKey *)0)->state) ==
                   sizeof(crypto_aead_aes256gcm_state),
               "VkrNetPacketKey state must hold libsodium's AES-GCM state");
_Static_assert(crypto_aead_aes256gcm_KEYBYTES == VKR_NET_KEY_SIZE,
               "AES-256-GCM key size");
_Static_assert(crypto_aead_aes256gcm_ABYTES == VKR_NET_TAG_SIZE,
               "AES-256-GCM tag size");
_Static_assert(crypto_aead_aes256gcm_NPUBBYTES == 12u, "AES-GCM nonce size");

#define NOISE_PROTOCOL_NAME "Noise_IK_25519_ChaChaPoly_BLAKE2b"
#define NOISE_BLOCK_SIZE 128u

bool8_t vkr_net_crypto_init(void) {
  /* sodium_init is thread-safe and returns 1 once it already ran. */
  if (sodium_init() < 0) {
    return false_v;
  }
  return crypto_aead_aes256gcm_is_available() ? true_v : false_v;
}

void vkr_net_random(void *out, uint64_t size) {
  randombytes_buf(out, (size_t)size);
}

void vkr_net_keypair_generate(VkrNetKeyPair *out) {
  uint8_t secret[VKR_NET_KEY_SIZE];
  randombytes_buf(secret, sizeof(secret));
  vkr_net_keypair_from_secret(secret, out);
  sodium_memzero(secret, sizeof(secret));
}

void vkr_net_keypair_from_secret(const uint8_t secret[VKR_NET_KEY_SIZE],
                                 VkrNetKeyPair *out) {
  MemCopy(out->secret_key, secret, VKR_NET_KEY_SIZE);
  crypto_scalarmult_base(out->public_key, out->secret_key);
}

// =============================================================================
// Noise symmetric state (specification sections 4 and 5)
// =============================================================================

static void noise_hash2(const uint8_t *a, uint64_t a_size, const uint8_t *b,
                        uint64_t b_size, uint8_t out[VKR_NET_NOISE_HASH_SIZE]) {
  crypto_generichash_blake2b_state state;
  crypto_generichash_blake2b_init(&state, NULL, 0u, VKR_NET_NOISE_HASH_SIZE);
  crypto_generichash_blake2b_update(&state, a, a_size);
  crypto_generichash_blake2b_update(&state, b, b_size);
  crypto_generichash_blake2b_final(&state, out, VKR_NET_NOISE_HASH_SIZE);
}

/* HMAC with BLAKE2b (HASHLEN 64, BLOCKLEN 128); `key` is at most 64 bytes. */
static void noise_hmac(const uint8_t *key, uint64_t key_size,
                       const uint8_t *data, uint64_t data_size,
                       uint8_t out[VKR_NET_NOISE_HASH_SIZE]) {
  uint8_t pad[NOISE_BLOCK_SIZE];
  uint8_t inner[VKR_NET_NOISE_HASH_SIZE];

  MemZero(pad, sizeof(pad));
  MemCopy(pad, key, key_size);
  for (uint32_t i = 0u; i < NOISE_BLOCK_SIZE; ++i) {
    pad[i] ^= 0x36u;
  }
  noise_hash2(pad, sizeof(pad), data, data_size, inner);

  for (uint32_t i = 0u; i < NOISE_BLOCK_SIZE; ++i) {
    pad[i] ^= 0x36u ^ 0x5cu;
  }
  noise_hash2(pad, sizeof(pad), inner, sizeof(inner), out);

  sodium_memzero(pad, sizeof(pad));
  sodium_memzero(inner, sizeof(inner));
}

/* HKDF(chaining_key, input, 2): two 64-byte outputs. */
static void noise_hkdf2(const uint8_t ck[VKR_NET_NOISE_HASH_SIZE],
                        const uint8_t *input, uint64_t input_size,
                        uint8_t out1[VKR_NET_NOISE_HASH_SIZE],
                        uint8_t out2[VKR_NET_NOISE_HASH_SIZE]) {
  uint8_t temp_key[VKR_NET_NOISE_HASH_SIZE];
  uint8_t block[VKR_NET_NOISE_HASH_SIZE + 1u];

  noise_hmac(ck, VKR_NET_NOISE_HASH_SIZE, input, input_size, temp_key);

  block[0] = 0x01u;
  noise_hmac(temp_key, sizeof(temp_key), block, 1u, out1);

  MemCopy(block, out1, VKR_NET_NOISE_HASH_SIZE);
  block[VKR_NET_NOISE_HASH_SIZE] = 0x02u;
  noise_hmac(temp_key, sizeof(temp_key), block, sizeof(block), out2);

  sodium_memzero(temp_key, sizeof(temp_key));
  sodium_memzero(block, sizeof(block));
}

static void noise_mix_hash(VkrNetNoise *noise, const uint8_t *data,
                           uint64_t size) {
  noise_hash2(noise->h, sizeof(noise->h), data, size, noise->h);
}

static void noise_mix_key(VkrNetNoise *noise,
                          const uint8_t input[VKR_NET_KEY_SIZE]) {
  uint8_t temp_k[VKR_NET_NOISE_HASH_SIZE];
  noise_hkdf2(noise->ck, input, VKR_NET_KEY_SIZE, noise->ck, temp_k);
  /* HASHLEN 64: the cipher key is the first 32 bytes. */
  MemCopy(noise->k, temp_k, VKR_NET_KEY_SIZE);
  noise->n = 0u;
  noise->has_key = true_v;
  sodium_memzero(temp_k, sizeof(temp_k));
}

/* ChaChaPoly nonce: four zero bytes and the 64-bit counter, little-endian. */
static void noise_nonce(uint64_t n, uint8_t nonce[12]) {
  MemZero(nonce, 4u);
  vkr_store_le_u64(nonce + 4, n);
}

/* EncryptAndHash: writes `size` + 16 bytes (or `size` without a key). */
static uint32_t noise_encrypt_and_hash(VkrNetNoise *noise,
                                       const uint8_t *plaintext, uint32_t size,
                                       uint8_t *out) {
  uint32_t written = size;
  if (noise->has_key) {
    uint8_t nonce[12];
    unsigned long long length = 0u;
    noise_nonce(noise->n, nonce);
    crypto_aead_chacha20poly1305_ietf_encrypt(out, &length, plaintext, size,
                                              noise->h, sizeof(noise->h), NULL,
                                              nonce, noise->k);
    noise->n += 1u;
    written = (uint32_t)length;
  } else {
    MemCopy(out, plaintext, size);
  }
  noise_mix_hash(noise, out, written);
  return written;
}

/* DecryptAndHash: `size` includes the tag when a key is set. */
static bool8_t noise_decrypt_and_hash(VkrNetNoise *noise,
                                      const uint8_t *ciphertext, uint32_t size,
                                      uint8_t *out, uint32_t *out_size) {
  uint8_t h_before[VKR_NET_NOISE_HASH_SIZE];
  MemCopy(h_before, noise->h, sizeof(h_before));
  noise_mix_hash(noise, ciphertext, size);
  if (!noise->has_key) {
    MemCopy(out, ciphertext, size);
    *out_size = size;
    return true_v;
  }
  if (size < VKR_NET_TAG_SIZE) {
    return false_v;
  }
  uint8_t nonce[12];
  unsigned long long length = 0u;
  noise_nonce(noise->n, nonce);
  if (crypto_aead_chacha20poly1305_ietf_decrypt(
          out, &length, NULL, ciphertext, size, h_before, sizeof(h_before),
          nonce, noise->k) != 0) {
    return false_v;
  }
  noise->n += 1u;
  *out_size = (uint32_t)length;
  return true_v;
}

static bool8_t noise_dh(const uint8_t secret[VKR_NET_KEY_SIZE],
                        const uint8_t public_key[VKR_NET_KEY_SIZE],
                        uint8_t out[VKR_NET_KEY_SIZE]) {
  /* A low-order peer key yields zero; libsodium refuses it. */
  return crypto_scalarmult(out, secret, public_key) == 0 ? true_v : false_v;
}

static bool8_t noise_mix_dh(VkrNetNoise *noise,
                            const uint8_t secret[VKR_NET_KEY_SIZE],
                            const uint8_t public_key[VKR_NET_KEY_SIZE]) {
  uint8_t shared[VKR_NET_KEY_SIZE];
  if (!noise_dh(secret, public_key, shared)) {
    return false_v;
  }
  noise_mix_key(noise, shared);
  sodium_memzero(shared, sizeof(shared));
  return true_v;
}

void vkr_net_noise_init(VkrNetNoise *noise, bool8_t initiator,
                        const uint8_t *prologue, uint32_t prologue_size,
                        const VkrNetKeyPair *static_keys,
                        const uint8_t remote_static[VKR_NET_KEY_SIZE]) {
  MemZero(noise, sizeof(*noise));
  noise->initiator = initiator;
  noise->s = *static_keys;

  /* The protocol name fits HASHLEN, so h is the name padded with zeros. */
  _Static_assert(sizeof(NOISE_PROTOCOL_NAME) - 1u <= VKR_NET_NOISE_HASH_SIZE,
                 "Noise protocol name must fit HASHLEN");
  MemCopy(noise->h, NOISE_PROTOCOL_NAME, sizeof(NOISE_PROTOCOL_NAME) - 1u);
  MemCopy(noise->ck, noise->h, sizeof(noise->ck));
  noise_mix_hash(noise, prologue, prologue_size);

  /* Pre-message `<- s`: both sides hash the responder's static key. */
  if (initiator) {
    MemCopy(noise->rs, remote_static, VKR_NET_KEY_SIZE);
    noise_mix_hash(noise, noise->rs, VKR_NET_KEY_SIZE);
  } else {
    noise_mix_hash(noise, noise->s.public_key, VKR_NET_KEY_SIZE);
  }
}

static void noise_set_ephemeral(VkrNetNoise *noise,
                                const VkrNetKeyPair *ephemeral) {
  if (ephemeral) {
    noise->e = *ephemeral;
  } else {
    vkr_net_keypair_generate(&noise->e);
  }
}

uint32_t vkr_net_noise_write_message1(VkrNetNoise *noise,
                                      const VkrNetKeyPair *ephemeral,
                                      const uint8_t *payload,
                                      uint32_t payload_size, uint8_t *out,
                                      uint32_t capacity) {
  if (!noise->initiator ||
      capacity < VKR_NET_NOISE_MESSAGE1_OVERHEAD + payload_size) {
    return 0u;
  }
  uint32_t offset = 0u;

  /* e */
  noise_set_ephemeral(noise, ephemeral);
  MemCopy(out, noise->e.public_key, VKR_NET_KEY_SIZE);
  noise_mix_hash(noise, noise->e.public_key, VKR_NET_KEY_SIZE);
  offset += VKR_NET_KEY_SIZE;

  /* es */
  if (!noise_mix_dh(noise, noise->e.secret_key, noise->rs)) {
    return 0u;
  }

  /* s */
  offset += noise_encrypt_and_hash(noise, noise->s.public_key, VKR_NET_KEY_SIZE,
                                   out + offset);

  /* ss */
  if (!noise_mix_dh(noise, noise->s.secret_key, noise->rs)) {
    return 0u;
  }

  offset += noise_encrypt_and_hash(noise, payload, payload_size, out + offset);
  return offset;
}

bool8_t vkr_net_noise_read_message1(VkrNetNoise *noise, const uint8_t *message,
                                    uint32_t size, uint8_t *payload,
                                    uint32_t payload_capacity,
                                    uint32_t *out_payload_size) {
  if (noise->initiator || size < VKR_NET_NOISE_MESSAGE1_OVERHEAD ||
      size - VKR_NET_NOISE_MESSAGE1_OVERHEAD > payload_capacity) {
    return false_v;
  }
  uint32_t offset = 0u;
  uint32_t length = 0u;

  /* e */
  MemCopy(noise->re, message, VKR_NET_KEY_SIZE);
  noise_mix_hash(noise, noise->re, VKR_NET_KEY_SIZE);
  offset += VKR_NET_KEY_SIZE;

  /* es */
  if (!noise_mix_dh(noise, noise->s.secret_key, noise->re)) {
    return false_v;
  }

  /* s */
  if (!noise_decrypt_and_hash(noise, message + offset,
                              VKR_NET_KEY_SIZE + VKR_NET_TAG_SIZE, noise->rs,
                              &length) ||
      length != VKR_NET_KEY_SIZE) {
    return false_v;
  }
  offset += VKR_NET_KEY_SIZE + VKR_NET_TAG_SIZE;

  /* ss */
  if (!noise_mix_dh(noise, noise->s.secret_key, noise->rs)) {
    return false_v;
  }

  return noise_decrypt_and_hash(noise, message + offset, size - offset, payload,
                                out_payload_size);
}

uint32_t vkr_net_noise_write_message2(VkrNetNoise *noise,
                                      const VkrNetKeyPair *ephemeral,
                                      const uint8_t *payload,
                                      uint32_t payload_size, uint8_t *out,
                                      uint32_t capacity) {
  if (noise->initiator ||
      capacity < VKR_NET_NOISE_MESSAGE2_OVERHEAD + payload_size) {
    return 0u;
  }
  uint32_t offset = 0u;

  /* e */
  noise_set_ephemeral(noise, ephemeral);
  MemCopy(out, noise->e.public_key, VKR_NET_KEY_SIZE);
  noise_mix_hash(noise, noise->e.public_key, VKR_NET_KEY_SIZE);
  offset += VKR_NET_KEY_SIZE;

  /* ee */
  if (!noise_mix_dh(noise, noise->e.secret_key, noise->re)) {
    return 0u;
  }

  /* se: the initiator's static key with the responder's ephemeral key. */
  if (!noise_mix_dh(noise, noise->e.secret_key, noise->rs)) {
    return 0u;
  }

  offset += noise_encrypt_and_hash(noise, payload, payload_size, out + offset);
  return offset;
}

bool8_t vkr_net_noise_read_message2(VkrNetNoise *noise, const uint8_t *message,
                                    uint32_t size, uint8_t *payload,
                                    uint32_t payload_capacity,
                                    uint32_t *out_payload_size) {
  if (!noise->initiator || size < VKR_NET_NOISE_MESSAGE2_OVERHEAD ||
      size - VKR_NET_NOISE_MESSAGE2_OVERHEAD > payload_capacity) {
    return false_v;
  }

  /* e */
  MemCopy(noise->re, message, VKR_NET_KEY_SIZE);
  noise_mix_hash(noise, noise->re, VKR_NET_KEY_SIZE);

  /* ee */
  if (!noise_mix_dh(noise, noise->e.secret_key, noise->re)) {
    return false_v;
  }

  /* se */
  if (!noise_mix_dh(noise, noise->s.secret_key, noise->re)) {
    return false_v;
  }

  return noise_decrypt_and_hash(noise, message + VKR_NET_KEY_SIZE,
                                size - VKR_NET_KEY_SIZE, payload,
                                out_payload_size);
}

void vkr_net_noise_split(const VkrNetNoise *noise,
                         uint8_t initiator_key[VKR_NET_KEY_SIZE],
                         uint8_t responder_key[VKR_NET_KEY_SIZE]) {
  uint8_t out1[VKR_NET_NOISE_HASH_SIZE];
  uint8_t out2[VKR_NET_NOISE_HASH_SIZE];
  noise_hkdf2(noise->ck, NULL, 0u, out1, out2);
  MemCopy(initiator_key, out1, VKR_NET_KEY_SIZE);
  MemCopy(responder_key, out2, VKR_NET_KEY_SIZE);
  sodium_memzero(out1, sizeof(out1));
  sodium_memzero(out2, sizeof(out2));
}

void vkr_net_noise_wipe(VkrNetNoise *noise) {
  sodium_memzero(noise, sizeof(*noise));
}

// =============================================================================
// Packet protection
// =============================================================================

void vkr_net_packet_key_init(VkrNetPacketKey *key,
                             const uint8_t secret[VKR_NET_KEY_SIZE]) {
  static const uint8_t label[] = "vkr net packet iv";
  crypto_aead_aes256gcm_beforenm((crypto_aead_aes256gcm_state *)key->state,
                                 secret);
  crypto_generichash_blake2b(key->iv, sizeof(key->iv), label,
                             sizeof(label) - 1u, secret, VKR_NET_KEY_SIZE);
}

void vkr_net_packet_key_wipe(VkrNetPacketKey *key) {
  sodium_memzero(key, sizeof(*key));
}

static void packet_nonce(const VkrNetPacketKey *key, uint64_t packet_number,
                         uint8_t nonce[12]) {
  MemCopy(nonce, key->iv, 12u);
  for (uint32_t i = 0u; i < 8u; ++i) {
    nonce[4u + i] ^= (uint8_t)(packet_number >> (8u * i));
  }
}

void vkr_net_packet_seal(const VkrNetPacketKey *key, uint64_t packet_number,
                         const uint8_t *header, uint32_t header_size,
                         uint8_t *payload, uint32_t payload_size,
                         uint8_t tag[VKR_NET_TAG_SIZE]) {
  uint8_t nonce[12];
  unsigned long long tag_size = 0u;
  packet_nonce(key, packet_number, nonce);
  crypto_aead_aes256gcm_encrypt_detached_afternm(
      payload, tag, &tag_size, payload, payload_size, header, header_size, NULL,
      nonce, (const crypto_aead_aes256gcm_state *)key->state);
}

bool8_t vkr_net_packet_open(const VkrNetPacketKey *key, uint64_t packet_number,
                            const uint8_t *header, uint32_t header_size,
                            uint8_t *payload, uint32_t payload_size,
                            const uint8_t tag[VKR_NET_TAG_SIZE]) {
  uint8_t nonce[12];
  packet_nonce(key, packet_number, nonce);
  return crypto_aead_aes256gcm_decrypt_detached_afternm(
             payload, NULL, payload, payload_size, tag, header, header_size,
             nonce, (const crypto_aead_aes256gcm_state *)key->state) == 0
             ? true_v
             : false_v;
}

// =============================================================================
// Retry cookies
// =============================================================================

static void cookie_mac(const uint8_t secret[VKR_NET_KEY_SIZE],
                       const void *address_bytes, uint32_t address_size,
                       const uint8_t time_bytes[4], uint8_t out[16]) {
  crypto_generichash_blake2b_state state;
  crypto_generichash_blake2b_init(&state, secret, VKR_NET_KEY_SIZE, 16u);
  crypto_generichash_blake2b_update(&state, time_bytes, 4u);
  crypto_generichash_blake2b_update(&state, address_bytes, address_size);
  crypto_generichash_blake2b_final(&state, out, 16u);
}

void vkr_net_cookie_make(const uint8_t secret[VKR_NET_KEY_SIZE],
                         const void *address_bytes, uint32_t address_size,
                         uint32_t now_seconds,
                         uint8_t out[VKR_NET_COOKIE_SIZE]) {
  vkr_store_le_u32(out, now_seconds);
  cookie_mac(secret, address_bytes, address_size, out, out + 4);
}

bool8_t vkr_net_cookie_check(const uint8_t secret[VKR_NET_KEY_SIZE],
                             const void *address_bytes, uint32_t address_size,
                             uint32_t now_seconds, uint32_t lifetime_seconds,
                             const uint8_t cookie[VKR_NET_COOKIE_SIZE]) {
  const uint32_t issued = vkr_load_le_u32(cookie);
  if (issued > now_seconds || now_seconds - issued > lifetime_seconds) {
    return false_v;
  }
  uint8_t expected[16];
  cookie_mac(secret, address_bytes, address_size, cookie, expected);
  return sodium_memcmp(expected, cookie + 4, sizeof(expected)) == 0 ? true_v
                                                                    : false_v;
}

bool8_t vkr_net_equal_secret(const void *a, const void *b, uint64_t size) {
  return sodium_memcmp(a, b, (size_t)size) == 0 ? true_v : false_v;
}
