#pragma once

#include "defines.h"

/* Cryptography of the network transport (docs/proposals/network-protocol.md,
 * "Handshake"): the Noise IK handshake with X25519, ChaCha20-Poly1305 and
 * BLAKE2b, AES-256-GCM packet protection with the keys it derives, and
 * stateless retry cookies. Every primitive comes from libsodium; this module
 * only composes them as the Noise specification (revision 34) describes. */

#define VKR_NET_KEY_SIZE 32u
#define VKR_NET_TAG_SIZE 16u
#define VKR_NET_NOISE_HASH_SIZE 64u
/* Ephemeral key plus the encrypted static key: e, s with its tag. */
#define VKR_NET_NOISE_MESSAGE1_OVERHEAD (32u + 32u + 16u + 16u)
/* Ephemeral key plus the payload tag. */
#define VKR_NET_NOISE_MESSAGE2_OVERHEAD (32u + 16u)
/* A retry cookie: issue time in seconds and a 16-byte MAC. */
#define VKR_NET_COOKIE_SIZE 20u

typedef struct VkrNetKeyPair {
  uint8_t public_key[VKR_NET_KEY_SIZE];
  uint8_t secret_key[VKR_NET_KEY_SIZE];
} VkrNetKeyPair;

/* Noise symmetric and handshake state of one side of one handshake. */
typedef struct VkrNetNoise {
  uint8_t h[VKR_NET_NOISE_HASH_SIZE];
  uint8_t ck[VKR_NET_NOISE_HASH_SIZE];
  uint8_t k[VKR_NET_KEY_SIZE];
  uint64_t n;
  bool8_t has_key;
  bool8_t initiator;
  VkrNetKeyPair s;
  VkrNetKeyPair e;
  /* The peer's static and ephemeral public keys. */
  uint8_t rs[VKR_NET_KEY_SIZE];
  uint8_t re[VKR_NET_KEY_SIZE];
} VkrNetNoise;

/* One direction's packet protection: the expanded AES-256-GCM key and the
   IV that each packet number is XORed into. */
typedef struct VkrNetPacketKey {
  _Alignas(16) uint8_t state[512];
  uint8_t iv[12];
} VkrNetPacketKey;

/* Starts libsodium. False when it fails or the CPU has no AES-GCM hardware
   path; every CPU of ADR-083 has one. Safe to call from any thread, more
   than once. */
bool8_t vkr_net_crypto_init(void);

void vkr_net_random(void *out, uint64_t size);

void vkr_net_keypair_generate(VkrNetKeyPair *out);

/* The key pair of an existing X25519 secret key. */
void vkr_net_keypair_from_secret(const uint8_t secret[VKR_NET_KEY_SIZE],
                                 VkrNetKeyPair *out);

/* Starts a handshake: InitializeSymmetric, the prologue and the responder's
   static key as the pre-message. The initiator passes the responder's
   public key in `remote_static`; the responder passes NULL. */
void vkr_net_noise_init(VkrNetNoise *noise, bool8_t initiator,
                        const uint8_t *prologue, uint32_t prologue_size,
                        const VkrNetKeyPair *static_keys,
                        const uint8_t remote_static[VKR_NET_KEY_SIZE]);

/* Initiator: `-> e, es, s, ss` and the encrypted payload into `out`.
   `ephemeral` NULL generates a fresh key (tests pass fixed keys). Returns
   the message size, or zero when `out` is too small or a DH result is zero. */
uint32_t vkr_net_noise_write_message1(VkrNetNoise *noise,
                                      const VkrNetKeyPair *ephemeral,
                                      const uint8_t *payload,
                                      uint32_t payload_size, uint8_t *out,
                                      uint32_t capacity);

/* Responder: reads message 1, learns the initiator's static key (`rs`) and
   decrypts the payload into `payload`. False when authentication fails or
   the payload does not fit. */
bool8_t vkr_net_noise_read_message1(VkrNetNoise *noise, const uint8_t *message,
                                    uint32_t size, uint8_t *payload,
                                    uint32_t payload_capacity,
                                    uint32_t *out_payload_size);

/* Responder: `<- e, ee, se` and the encrypted payload. */
uint32_t vkr_net_noise_write_message2(VkrNetNoise *noise,
                                      const VkrNetKeyPair *ephemeral,
                                      const uint8_t *payload,
                                      uint32_t payload_size, uint8_t *out,
                                      uint32_t capacity);

/* Initiator: reads message 2. */
bool8_t vkr_net_noise_read_message2(VkrNetNoise *noise, const uint8_t *message,
                                    uint32_t size, uint8_t *payload,
                                    uint32_t payload_capacity,
                                    uint32_t *out_payload_size);

/* Split(): the initiator-to-responder and responder-to-initiator keys. */
void vkr_net_noise_split(const VkrNetNoise *noise,
                         uint8_t initiator_key[VKR_NET_KEY_SIZE],
                         uint8_t responder_key[VKR_NET_KEY_SIZE]);

/* Overwrites the handshake state, including its secret keys. */
void vkr_net_noise_wipe(VkrNetNoise *noise);

/* Expands one direction's key; the IV is a keyed BLAKE2b of the key. */
void vkr_net_packet_key_init(VkrNetPacketKey *key,
                             const uint8_t secret[VKR_NET_KEY_SIZE]);

void vkr_net_packet_key_wipe(VkrNetPacketKey *key);

/* Encrypts `payload` in place and writes its tag. `header` is the
   associated data. */
void vkr_net_packet_seal(const VkrNetPacketKey *key, uint64_t packet_number,
                         const uint8_t *header, uint32_t header_size,
                         uint8_t *payload, uint32_t payload_size,
                         uint8_t tag[VKR_NET_TAG_SIZE]);

/* Decrypts `payload` in place; false, with `payload` unspecified, when the
   tag does not verify. */
bool8_t vkr_net_packet_open(const VkrNetPacketKey *key, uint64_t packet_number,
                            const uint8_t *header, uint32_t header_size,
                            uint8_t *payload, uint32_t payload_size,
                            const uint8_t tag[VKR_NET_TAG_SIZE]);

/* A retry cookie for `address_bytes` issued at `now_seconds`. */
void vkr_net_cookie_make(const uint8_t secret[VKR_NET_KEY_SIZE],
                         const void *address_bytes, uint32_t address_size,
                         uint32_t now_seconds,
                         uint8_t out[VKR_NET_COOKIE_SIZE]);

/* True when `cookie` was made for `address_bytes` with `secret` within the
   last `lifetime_seconds`. */
bool8_t vkr_net_cookie_check(const uint8_t secret[VKR_NET_KEY_SIZE],
                             const void *address_bytes, uint32_t address_size,
                             uint32_t now_seconds, uint32_t lifetime_seconds,
                             const uint8_t cookie[VKR_NET_COOKIE_SIZE]);

/* Constant-time comparison. */
bool8_t vkr_net_equal_secret(const void *a, const void *b, uint64_t size);
