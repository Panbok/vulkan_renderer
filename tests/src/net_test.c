#include "net_test.h"

#include "container_test_allocator.h"
#include "vkr_net_core.h"
#include "vkr_net_crypto.h"
#include "vkr_net_host.h"
#include "vkr_net_session.h"
#include "vkr_net_sim.h"
#include "vkr_net_varint.h"

#include <sodium.h>
#include <stdio.h>
#include <string.h>

/* The transport tests (docs/proposals/network-protocol.md, "Verification").
 * Each test names the failure it detects. Two cores talk through the
 * simulated network on a virtual clock, so every run repeats exactly; the
 * counting allocator proves that destroying a core returns every byte. */

#define NET_TEST_SECOND 1000000ull
#define NET_TEST_MESSAGES_MAX 4096u
/* Steps at one virtual time before the run counts as a livelock. */
#define NET_TEST_STUCK_STEPS 100000u

// =============================================================================
// Helpers
// =============================================================================

static uint32_t net_test_hex(const char *hex, uint8_t *out, uint32_t capacity) {
  const uint32_t length = (uint32_t)strlen(hex) / 2u;
  assert(length <= capacity);
  for (uint32_t i = 0u; i < length; ++i) {
    unsigned int value = 0u;
    assert(sscanf(hex + 2u * i, "%2x", &value) == 1);
    out[i] = (uint8_t)value;
  }
  return length;
}

/* Message `index` of `size` bytes: the index, then bytes derived from it,
   eight per generator step. */
static void net_test_fill(uint8_t *data, uint32_t size, uint64_t index) {
  uint64_t state = index * 0x9e3779b97f4a7c15ull + 1u;
  for (uint32_t i = 0u; i < size; i += 8u) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    memcpy(data + i, &state, Min(8u, size - i));
  }
  if (size >= 8u) {
    memcpy(data, &index, 8u);
  }
}

static bool8_t net_test_check(const uint8_t *data, uint32_t size,
                              uint64_t *out_index) {
  if (size < 8u) {
    return false_v;
  }
  uint64_t index = 0u;
  memcpy(&index, data, 8u);
  static uint8_t expected[1024u * 1024u];
  if (size > sizeof(expected)) {
    return false_v;
  }
  net_test_fill(expected, size, index);
  *out_index = index;
  return memcmp(expected, data, size) == 0 ? true_v : false_v;
}

typedef struct NetTestAccept {
  uint16_t refuse_code;
  uint32_t calls;
  uint8_t last_credential[64];
  uint32_t last_credential_size;
} NetTestAccept;

static void net_test_accept(void *context, const VkrNetAddress *address,
                            const uint8_t peer_key[VKR_NET_KEY_SIZE],
                            const uint8_t *credential, uint32_t credential_size,
                            VkrNetAcceptResult *out_result) {
  (void)address;
  (void)peer_key;
  NetTestAccept *accept = context;
  accept->calls += 1u;
  accept->last_credential_size = Min(credential_size, 64u);
  memcpy(accept->last_credential, credential, accept->last_credential_size);
  out_result->code = accept->refuse_code;
  memcpy(out_result->payload, "welcome", 7u);
  out_result->payload_size = 7u;
  out_result->user = 42u;
}

/* Events one side saw, plus the messages it received per channel. */
typedef struct NetTestSide {
  VkrNetCore *core;
  VkrNetKeyPair keys;
  VkrNetAddress address;
  uint32_t endpoint;
  VkrNetConnectionId connection;
  bool8_t connected;
  bool8_t closed;
  uint16_t close_code;
  uint8_t peer_key[VKR_NET_KEY_SIZE];
  uint8_t connect_payload[16];
  uint32_t connect_payload_size;
  uint32_t connected_events;

  uint8_t seen[VKR_NET_CHANNEL_MAX][NET_TEST_MESSAGES_MAX];
  uint32_t received[VKR_NET_CHANNEL_MAX];
  uint64_t next_index[VKR_NET_CHANNEL_MAX];
  bool8_t duplicate;
  bool8_t out_of_order;
  bool8_t corrupt;
  uint32_t acked_tags[NET_TEST_MESSAGES_MAX];
  uint32_t lost_tags[NET_TEST_MESSAGES_MAX];
  /* Do not take messages: leave them in the core's queue. */
  bool8_t hold;
  /* Events pass through this session first when set. */
  VkrNetSession *session;
} NetTestSide;

typedef struct NetTestWorld {
  ContainerTestAllocator state;
  VkrAllocator allocator;
  VkrNetSim *sim;
  uint64_t now;
  uint32_t stuck;
  NetTestAccept accept;
  NetTestSide server;
  NetTestSide client;
} NetTestWorld;

static VkrNetCore *net_test_core(NetTestWorld *world, NetTestSide *side,
                                 bool8_t server, const VkrNetCoreConfig *base) {
  VkrNetCoreConfig config = base ? *base : (VkrNetCoreConfig){0};
  config.static_keys = side->keys;
  if (server) {
    config.accept_incoming = true_v;
    config.accept = net_test_accept;
    config.accept_context = &world->accept;
  }
  VkrNetCore *core = vkr_net_core_create(&world->allocator, &config);
  assert(core);
  return core;
}

static void net_world_init(NetTestWorld *world, uint64_t seed,
                           const VkrNetCoreConfig *server_config,
                           const VkrNetCoreConfig *client_config) {
  memset(world, 0, sizeof(*world));
  world->allocator = container_test_allocator(&world->state);
  world->sim = vkr_net_sim_create(&world->allocator, seed);
  assert(world->sim);
  vkr_net_keypair_generate(&world->server.keys);
  vkr_net_keypair_generate(&world->client.keys);
  assert(vkr_net_address_parse("10.0.0.1:4000", 0u, &world->server.address));
  assert(vkr_net_address_parse("10.0.0.2:5000", 0u, &world->client.address));
  world->server.core =
      net_test_core(world, &world->server, true_v, server_config);
  world->client.core =
      net_test_core(world, &world->client, false_v, client_config);
  world->server.endpoint =
      vkr_net_sim_add(world->sim, world->server.core, &world->server.address);
  world->client.endpoint =
      vkr_net_sim_add(world->sim, world->client.core, &world->client.address);
}

static void net_world_link(NetTestWorld *world, const VkrNetSimLink *link) {
  vkr_net_sim_set_link(world->sim, world->client.endpoint,
                       world->server.endpoint, link);
  vkr_net_sim_set_link(world->sim, world->server.endpoint,
                       world->client.endpoint, link);
}

/* Destroys both cores and the simulation; every byte must return. */
static void net_world_destroy(NetTestWorld *world) {
  vkr_net_core_destroy(world->client.core);
  vkr_net_core_destroy(world->server.core);
  vkr_net_sim_destroy(world->sim);
  assert(world->state.live_bytes == 0u);
}

static void net_side_poll(NetTestSide *side, uint64_t now) {
  if (side->hold) {
    return;
  }
  VkrNetEvent event;
  while (vkr_net_core_poll(side->core, &event)) {
    if (side->session && vkr_net_session_dispatch(side->session, &event, now)) {
      continue;
    }
    switch (event.type) {
    case VKR_NET_EVENT_CONNECTED:
      side->connected = true_v;
      side->connected_events += 1u;
      side->connection = event.connection;
      memcpy(side->peer_key, event.peer_key, VKR_NET_KEY_SIZE);
      side->connect_payload_size = Min(event.size, 16u);
      memcpy(side->connect_payload, event.data, side->connect_payload_size);
      break;
    case VKR_NET_EVENT_CLOSED:
      side->closed = true_v;
      side->close_code = event.code;
      break;
    case VKR_NET_EVENT_MESSAGE: {
      uint64_t index = 0u;
      if (!net_test_check(event.data, event.size, &index) ||
          index >= NET_TEST_MESSAGES_MAX) {
        side->corrupt = true_v;
        break;
      }
      if (side->seen[event.channel][index]) {
        side->duplicate = true_v;
      }
      if (index < side->next_index[event.channel]) {
        side->out_of_order = true_v;
      }
      side->next_index[event.channel] = index + 1u;
      side->seen[event.channel][index] = 1u;
      side->received[event.channel] += 1u;
      break;
    }
    case VKR_NET_EVENT_ACKED:
      assert(event.tag < NET_TEST_MESSAGES_MAX);
      side->acked_tags[event.tag] += 1u;
      break;
    case VKR_NET_EVENT_LOST:
      assert(event.tag < NET_TEST_MESSAGES_MAX);
      side->lost_tags[event.tag] += 1u;
      break;
    default:
      break;
    }
  }
}

/* Advances to the next event, at most to `limit`, and polls both sides. */
static void net_world_step(NetTestWorld *world, uint64_t limit) {
  uint64_t next = vkr_net_sim_next_time(world->sim);
  if (next < world->now) {
    next = world->now;
  }
  if (next > limit) {
    next = limit;
  }
  if (next == world->now) {
    world->stuck += 1u;
    assert(world->stuck < NET_TEST_STUCK_STEPS && "transport livelock");
  } else {
    world->stuck = 0u;
  }
  world->now = next;
  vkr_net_sim_step(world->sim, world->now);
  net_side_poll(&world->server, world->now);
  net_side_poll(&world->client, world->now);
}

static void net_world_connect(NetTestWorld *world) {
  assert(vkr_net_core_connect(
      world->client.core, &world->server.address, world->server.keys.public_key,
      (const uint8_t *)"token", 5u, world->now, &world->client.connection));
  const uint64_t limit = world->now + 30u * NET_TEST_SECOND;
  while (!(world->client.connected && world->server.connected) &&
         !world->client.closed && world->now < limit) {
    net_world_step(world, limit);
  }
  /* Let the server see the client's first packet. */
  const uint64_t settle = world->now + NET_TEST_SECOND / 2u;
  while (world->now < settle) {
    net_world_step(world, settle);
  }
}

static void net_world_run(NetTestWorld *world, uint64_t duration) {
  const uint64_t limit = world->now + duration;
  while (world->now < limit) {
    net_world_step(world, limit);
  }
}

static void net_open_both(NetTestWorld *world, uint8_t channel,
                          const VkrNetChannelConfig *config) {
  assert(vkr_net_core_open_channel(world->client.core, world->client.connection,
                                   channel, config));
  assert(vkr_net_core_open_channel(world->server.core, world->server.connection,
                                   channel, config));
}

static uint32_t net_test_size(uint64_t index) {
  static const uint32_t sizes[] = {8u, 100u, 1100u, 5000u, 30000u, 64u, 2500u};
  return sizes[index % ArrayCount(sizes)];
}

/* Sends messages [0, count) on `channel`, as fast as the queue accepts,
   until the receiver has them all or `duration` passes. */
static void net_world_send_all(NetTestWorld *world, uint8_t channel,
                               uint32_t count, uint64_t duration,
                               uint32_t flags) {
  static uint8_t buffer[256u * 1024u];
  uint32_t next = 0u;
  uint32_t filled = UINT32_MAX;
  const uint64_t limit = world->now + duration;
  while (world->now < limit) {
    while (next < count) {
      const uint32_t size = net_test_size(next);
      if (filled != next) {
        net_test_fill(buffer, size, next);
        filled = next;
      }
      const VkrNetSendOptions options = {.flags = flags | VKR_NET_SEND_TAG,
                                         .tag = next};
      const VkrNetSendStatus status =
          vkr_net_core_send(world->client.core, world->client.connection,
                            channel, buffer, size, &options, world->now);
      if (status == VKR_NET_SEND_QUEUE_FULL) {
        break;
      }
      assert(status == VKR_NET_SEND_OK);
      ++next;
    }
    vkr_net_core_flush(world->client.core);
    if (next == count && world->server.received[channel] == count) {
      break;
    }
    net_world_step(world, limit);
  }
}

static const VkrNetSimLink net_test_lossy = {
    .latency_us = 10000u,
    .jitter_us = 5000u,
    .loss = 0.2f,
    .duplicate = 0.1f,
    .reorder = 0.1f,
};

// =============================================================================
// Encoding and cryptography
// =============================================================================

/* Fails when a varint boundary or the RFC 9000 packet number example
   decodes to a different value. */
static void test_net_varint(void) {
  printf("  Running test_net_varint...\n");
  const uint64_t values[] = {0u,         63u,
                             64u,        16383u,
                             16384u,     (1ull << 30) - 1u,
                             1ull << 30, VKR_NET_VARINT_MAX};
  const uint32_t sizes[] = {1u, 1u, 2u, 2u, 4u, 4u, 8u, 8u};
  for (uint32_t i = 0u; i < ArrayCount(values); ++i) {
    uint8_t buffer[8];
    const uint32_t written =
        vkr_net_varint_write(buffer, buffer + sizeof(buffer), values[i]);
    assert(written == sizes[i]);
    uint64_t decoded = 0u;
    assert(vkr_net_varint_read(buffer, buffer + written, &decoded) == written);
    assert(decoded == values[i]);
    assert(vkr_net_varint_read(buffer, buffer + written - 1u, &decoded) == 0u ||
           written == 1u);
  }
  uint8_t tiny[1];
  assert(vkr_net_varint_write(tiny, tiny + 1, 64u) == 0u);
  /* RFC 9000, appendix A.3. */
  assert(vkr_net_packet_number_decode(0xa82f30eaull, 0x9b32u, 16u) ==
         0xa82f9b32ull);
  assert(vkr_net_packet_number_decode(UINT64_MAX, 0u, 8u) == 0u);
  printf("  test_net_varint PASSED\n");
}

/* Fails when the Noise IK handshake differs from the cacophony vector for
   Noise_IK_25519_ChaChaPoly_BLAKE2b in any message, the handshake hash or
   the transport keys. */
static void test_net_noise_vector(void) {
  printf("  Running test_net_noise_vector...\n");
  uint8_t prologue[16];
  const uint32_t prologue_size =
      net_test_hex("4a6f686e2047616c74", prologue, sizeof(prologue));
  uint8_t secret[32];
  VkrNetKeyPair init_static, init_ephemeral, resp_static, resp_ephemeral;
  net_test_hex(
      "e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1",
      secret, 32u);
  vkr_net_keypair_from_secret(secret, &init_static);
  net_test_hex(
      "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a",
      secret, 32u);
  vkr_net_keypair_from_secret(secret, &init_ephemeral);
  net_test_hex(
      "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893",
      secret, 32u);
  vkr_net_keypair_from_secret(secret, &resp_static);
  net_test_hex(
      "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b",
      secret, 32u);
  vkr_net_keypair_from_secret(secret, &resp_ephemeral);
  uint8_t remote_static[32];
  net_test_hex(
      "31e0303fd6418d2f8c0e78b91f22e8caed0fbe48656dcf4767e4834f701b8f62",
      remote_static, 32u);
  assert(memcmp(remote_static, resp_static.public_key, 32u) == 0);

  VkrNetNoise initiator, responder;
  vkr_net_noise_init(&initiator, true_v, prologue, prologue_size, &init_static,
                     remote_static);
  vkr_net_noise_init(&responder, false_v, prologue, prologue_size, &resp_static,
                     NULL);

  uint8_t expected[256];
  uint8_t message[256];
  uint8_t payload[64];
  uint32_t payload_size = 0u;

  const char *m0 = "Ludwig von Mises";
  uint32_t size = vkr_net_noise_write_message1(
      &initiator, &init_ephemeral, (const uint8_t *)m0, (uint32_t)strlen(m0),
      message, sizeof(message));
  uint32_t expected_size = net_test_hex(
      "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944ba83"
      "a447b38c83e327ad936929812f624884847b7831e95e197b2f797088efdd2f88f1db"
      "7e1fb0e99c64419097af91cee64e470f4b6fcd9298ce0b56fe20f86e13bf70439c53"
      "8e3602a7127af71a29cc",
      expected, sizeof(expected));
  assert(size == expected_size && memcmp(message, expected, size) == 0);
  assert(vkr_net_noise_read_message1(&responder, message, size, payload,
                                     sizeof(payload), &payload_size));
  assert(payload_size == strlen(m0) && memcmp(payload, m0, payload_size) == 0);
  assert(memcmp(responder.rs, init_static.public_key, 32u) == 0);

  const char *m1 = "Murray Rothbard";
  size = vkr_net_noise_write_message2(&responder, &resp_ephemeral,
                                      (const uint8_t *)m1, (uint32_t)strlen(m1),
                                      message, sizeof(message));
  expected_size = net_test_hex(
      "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f1448088439f06"
      "9b267a06b3de3ecb1043bcb098e9af91d9c64748d998c7b47890871571",
      expected, sizeof(expected));
  assert(size == expected_size && memcmp(message, expected, size) == 0);
  assert(vkr_net_noise_read_message2(&initiator, message, size, payload,
                                     sizeof(payload), &payload_size));
  assert(payload_size == strlen(m1) && memcmp(payload, m1, payload_size) == 0);

  net_test_hex("1c8fa891cb414fedba6daa7c6f4ae0a6d98e5f9768cc9cecd27e805614943e"
               "e9c8a1b27fbfb76dc197255c8aa69f6b4285c423840b8bedf45e652ca64f79"
               "7d81",
               expected, sizeof(expected));
  assert(memcmp(initiator.h, expected, 64u) == 0);
  assert(memcmp(responder.h, expected, 64u) == 0);

  /* Transport messages with the split keys, ChaChaPoly nonce zero. */
  uint8_t k1[32], k2[32], r1[32], r2[32];
  vkr_net_noise_split(&initiator, k1, k2);
  vkr_net_noise_split(&responder, r1, r2);
  assert(memcmp(k1, r1, 32u) == 0 && memcmp(k2, r2, 32u) == 0);
  const uint8_t nonce[12] = {0};
  unsigned long long length = 0u;
  const char *m2 = "F. A. Hayek";
  crypto_aead_chacha20poly1305_ietf_encrypt(message, &length,
                                            (const uint8_t *)m2, strlen(m2),
                                            NULL, 0u, NULL, nonce, k1);
  expected_size =
      net_test_hex("cd54383060e7a28434cca27fb1cc524cfbabeb18181589df219d07",
                   expected, sizeof(expected));
  assert(length == expected_size && memcmp(message, expected, length) == 0);
  const char *m3 = "Carl Menger";
  crypto_aead_chacha20poly1305_ietf_encrypt(message, &length,
                                            (const uint8_t *)m3, strlen(m3),
                                            NULL, 0u, NULL, nonce, k2);
  expected_size =
      net_test_hex("a856d3bf0246bfc476c655009cd1ed677b8dcc5b349ae8ef2a05f2",
                   expected, sizeof(expected));
  assert(length == expected_size && memcmp(message, expected, length) == 0);

  /* A modified first message fails authentication. */
  VkrNetNoise fresh_initiator, fresh_responder;
  vkr_net_noise_init(&fresh_initiator, true_v, prologue, prologue_size,
                     &init_static, remote_static);
  vkr_net_noise_init(&fresh_responder, false_v, prologue, prologue_size,
                     &resp_static, NULL);
  size = vkr_net_noise_write_message1(&fresh_initiator, NULL,
                                      (const uint8_t *)m0, (uint32_t)strlen(m0),
                                      message, sizeof(message));
  message[40] ^= 0x01u;
  assert(!vkr_net_noise_read_message1(&fresh_responder, message, size, payload,
                                      sizeof(payload), &payload_size));
  printf("  test_net_noise_vector PASSED\n");
}

/* Fails when packet protection accepts a modified header, payload, packet
   number or key, or when a retry cookie outlives its lifetime or address. */
static void test_net_packet_protection(void) {
  printf("  Running test_net_packet_protection...\n");
  uint8_t secret[32];
  vkr_net_random(secret, sizeof(secret));
  VkrNetPacketKey key, other;
  vkr_net_packet_key_init(&key, secret);
  secret[0] ^= 1u;
  vkr_net_packet_key_init(&other, secret);

  uint8_t header[6] = {0x40, 1, 2, 3, 4, 7};
  uint8_t payload[100];
  uint8_t original[100];
  for (uint32_t i = 0u; i < sizeof(payload); ++i) {
    payload[i] = (uint8_t)i;
  }
  memcpy(original, payload, sizeof(payload));
  uint8_t tag[VKR_NET_TAG_SIZE];
  vkr_net_packet_seal(&key, 7u, header, sizeof(header), payload,
                      sizeof(payload), tag);
  assert(memcmp(payload, original, sizeof(payload)) != 0);

  uint8_t copy[100];
  memcpy(copy, payload, sizeof(copy));
  assert(vkr_net_packet_open(&key, 7u, header, sizeof(header), copy,
                             sizeof(copy), tag));
  assert(memcmp(copy, original, sizeof(copy)) == 0);

  memcpy(copy, payload, sizeof(copy));
  assert(!vkr_net_packet_open(&key, 8u, header, sizeof(header), copy,
                              sizeof(copy), tag));
  memcpy(copy, payload, sizeof(copy));
  assert(!vkr_net_packet_open(&other, 7u, header, sizeof(header), copy,
                              sizeof(copy), tag));
  memcpy(copy, payload, sizeof(copy));
  header[2] ^= 1u;
  assert(!vkr_net_packet_open(&key, 7u, header, sizeof(header), copy,
                              sizeof(copy), tag));
  header[2] ^= 1u;
  memcpy(copy, payload, sizeof(copy));
  copy[50] ^= 0x80u;
  assert(!vkr_net_packet_open(&key, 7u, header, sizeof(header), copy,
                              sizeof(copy), tag));

  uint8_t cookie[VKR_NET_COOKIE_SIZE];
  const uint8_t subject[] = {1, 2, 3, 4};
  const uint8_t elsewhere[] = {1, 2, 3, 5};
  vkr_net_cookie_make(secret, subject, sizeof(subject), 100u, cookie);
  assert(vkr_net_cookie_check(secret, subject, sizeof(subject), 110u, 30u,
                              cookie));
  assert(!vkr_net_cookie_check(secret, subject, sizeof(subject), 131u, 30u,
                               cookie));
  assert(!vkr_net_cookie_check(secret, elsewhere, sizeof(elsewhere), 110u, 30u,
                               cookie));
  assert(!vkr_net_cookie_check(secret, subject, sizeof(subject), 99u, 30u,
                               cookie));
  printf("  test_net_packet_protection PASSED\n");
}

/* Fails when an address round trip through text changes it. */
static void test_net_address(void) {
  printf("  Running test_net_address...\n");
  VkrNetAddress address;
  char text[VKR_NET_ADDRESS_TEXT];
  assert(vkr_net_address_parse("192.168.1.20:7777", 0u, &address));
  assert(address.family == VKR_NET_ADDRESS_IPV4 && address.port == 7777u);
  vkr_net_address_format(&address, text, sizeof(text));
  assert(strcmp(text, "192.168.1.20:7777") == 0);
  assert(vkr_net_address_parse("[fe80::1%4]:5000", 0u, &address));
  assert(address.family == VKR_NET_ADDRESS_IPV6 && address.scope == 4u);
  vkr_net_address_format(&address, text, sizeof(text));
  assert(strcmp(text, "[fe80::1%4]:5000") == 0);
  assert(vkr_net_address_parse("::1", 9u, &address));
  assert(address.port == 9u && address.bytes[15] == 1u);
  assert(!vkr_net_address_parse("example.com:80", 0u, &address));
  assert(!vkr_net_address_parse("1.2.3.4:70000", 0u, &address));
  printf("  test_net_address PASSED\n");
}

// =============================================================================
// Handshake
// =============================================================================

/* Fails when a handshake over a 10 ms path does not connect both sides,
   exchange static keys, carry the credential and the acceptance payload, or
   measure the RTT. */
static void test_net_handshake(void) {
  printf("  Running test_net_handshake...\n");
  NetTestWorld world;
  net_world_init(&world, 1u, NULL, NULL);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 10000u});
  net_world_connect(&world);
  assert(world.client.connected && world.server.connected);
  assert(world.server.connected_events == 1u);
  assert(memcmp(world.server.peer_key, world.client.keys.public_key, 32u) == 0);
  assert(memcmp(world.client.peer_key, world.server.keys.public_key, 32u) == 0);
  assert(world.accept.calls == 1u && world.accept.last_credential_size == 5u &&
         memcmp(world.accept.last_credential, "token", 5u) == 0);
  assert(world.client.connect_payload_size == 7u &&
         memcmp(world.client.connect_payload, "welcome", 7u) == 0);
  assert(vkr_net_core_user(world.server.core, world.server.connection) == 42u);
  VkrNetPathStats stats;
  assert(vkr_net_core_path(world.client.core, world.client.connection, &stats));
  assert(stats.srtt_us >= 19000u && stats.srtt_us <= 30000u);
  net_world_destroy(&world);
  printf("  test_net_handshake PASSED\n");
}

/* Fails when a refused client is not told the server's code, or when the
   server keeps state for it. */
static void test_net_handshake_refused(void) {
  printf("  Running test_net_handshake_refused...\n");
  NetTestWorld world;
  net_world_init(&world, 2u, NULL, NULL);
  world.accept.refuse_code = VKR_NET_CLOSE_ACCESS_DENIED;
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u});
  net_world_connect(&world);
  assert(world.client.closed && !world.client.connected);
  assert(world.client.close_code == VKR_NET_CLOSE_ACCESS_DENIED);
  assert(!world.server.connected);
  net_world_destroy(&world);
  printf("  test_net_handshake_refused PASSED\n");
}

/* Fails when a client with the wrong server key connects, or never gives up
   once the handshake timeout passes. */
static void test_net_handshake_wrong_key(void) {
  printf("  Running test_net_handshake_wrong_key...\n");
  const VkrNetCoreConfig config = {.handshake_timeout_ms = 2000u};
  NetTestWorld world;
  net_world_init(&world, 3u, &config, &config);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u});
  VkrNetKeyPair impostor;
  vkr_net_keypair_generate(&impostor);
  assert(vkr_net_core_connect(world.client.core, &world.server.address,
                              impostor.public_key, NULL, 0u, world.now,
                              &world.client.connection));
  net_world_run(&world, 5u * NET_TEST_SECOND);
  assert(!world.client.connected && !world.server.connected);
  assert(world.client.closed &&
         world.client.close_code == VKR_NET_CLOSE_TIMEOUT);
  assert(world.accept.calls == 0u);
  net_world_destroy(&world);
  printf("  test_net_handshake_wrong_key PASSED\n");
}

/* Fails when a server that demands a retry cookie never accepts the client
   or skips the Retry round trip. */
static void test_net_retry(void) {
  printf("  Running test_net_retry...\n");
  const VkrNetCoreConfig server_config = {.require_retry = true_v};
  NetTestWorld world;
  net_world_init(&world, 4u, &server_config, NULL);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u});
  net_world_connect(&world);
  assert(world.client.connected && world.server.connected);
  /* Initial, Initial with the cookie, then 1-RTT packets. */
  const VkrNetSimStats up = vkr_net_sim_stats(world.sim, world.client.endpoint,
                                              world.server.endpoint);
  assert(up.datagrams_sent >= 3u);
  assert(world.accept.calls == 1u);
  net_world_destroy(&world);
  printf("  test_net_retry PASSED\n");
}

/* Fails when a server sends more than three times the bytes it received to
   a client address it has not validated. */
static void test_net_amplification(void) {
  printf("  Running test_net_amplification...\n");
  const VkrNetCoreConfig config = {.handshake_timeout_ms = 3000u};
  NetTestWorld world;
  net_world_init(&world, 5u, &config, &config);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u});
  assert(vkr_net_core_connect(world.client.core, &world.server.address,
                              world.server.keys.public_key, NULL, 0u, world.now,
                              &world.client.connection));
  /* The client's first Initial arrives; later client packets vanish. */
  net_world_step(&world, world.now);
  vkr_net_sim_set_link(world.sim, world.client.endpoint, world.server.endpoint,
                       &(VkrNetSimLink){.latency_us = 5000u, .loss = 1.0f});
  /* The server sends data to the unvalidated client meanwhile. */
  net_world_run(&world, 20000u);
  assert(world.server.connected);
  const VkrNetChannelConfig channel = {.delivery = VKR_NET_RELIABLE_UNORDERED};
  assert(vkr_net_core_open_channel(world.server.core, world.server.connection,
                                   1u, &channel));
  uint8_t data[30000];
  memset(data, 7, sizeof(data));
  assert(vkr_net_core_send(world.server.core, world.server.connection, 1u, data,
                           sizeof(data), NULL, world.now) == VKR_NET_SEND_OK);
  vkr_net_core_flush(world.server.core);
  net_world_run(&world, 5u * NET_TEST_SECOND);
  const VkrNetSimStats up = vkr_net_sim_stats(world.sim, world.client.endpoint,
                                              world.server.endpoint);
  const VkrNetSimStats down = vkr_net_sim_stats(
      world.sim, world.server.endpoint, world.client.endpoint);
  const uint64_t received = up.bytes_delivered;
  assert(received >= VKR_NET_DATAGRAM_INITIAL);
  assert(down.bytes_sent <= received * 3u);
  net_world_destroy(&world);
  printf("  test_net_amplification PASSED\n");
}

// =============================================================================
// Delivery classes
// =============================================================================

/* Fails when a reliable ordered channel loses, duplicates, corrupts or
   reorders a message over a path with loss, duplication and jitter. */
static void test_net_reliable_ordered(void) {
  printf("  Running test_net_reliable_ordered...\n");
  NetTestWorld world;
  net_world_init(&world, 6u, NULL, NULL);
  net_world_link(&world, &net_test_lossy);
  net_world_connect(&world);
  assert(world.client.connected && world.server.connected);
  const VkrNetChannelConfig channel = {.delivery = VKR_NET_RELIABLE_ORDERED,
                                       .max_message_size = 64u * 1024u,
                                       .send_queue = 64u};
  net_open_both(&world, 1u, &channel);
  const uint32_t count = 400u;
  net_world_send_all(&world, 1u, count, 600u * NET_TEST_SECOND, 0u);
  assert(world.server.received[1] == count);
  assert(!world.server.duplicate && !world.server.out_of_order &&
         !world.server.corrupt);
  net_world_destroy(&world);
  printf("  test_net_reliable_ordered PASSED\n");
}

/* Fails when a reliable unordered channel loses, duplicates or corrupts a
   message, including fragmented ones, over a lossy path. */
static void test_net_reliable_unordered(void) {
  printf("  Running test_net_reliable_unordered...\n");
  NetTestWorld world;
  net_world_init(&world, 7u, NULL, NULL);
  net_world_link(&world, &net_test_lossy);
  net_world_connect(&world);
  const VkrNetChannelConfig channel = {.delivery = VKR_NET_RELIABLE_UNORDERED,
                                       .max_message_size = 64u * 1024u,
                                       .send_queue = 256u};
  net_open_both(&world, 2u, &channel);
  const uint32_t count = 400u;
  net_world_send_all(&world, 2u, count, 600u * NET_TEST_SECOND, 0u);
  assert(world.server.received[2] == count);
  assert(!world.server.duplicate && !world.server.corrupt);
  net_world_destroy(&world);
  printf("  test_net_reliable_unordered PASSED\n");
}

/* Fails when a sequenced channel delivers a message older than one it
   delivered before, or delivers nothing. */
static void test_net_sequenced(void) {
  printf("  Running test_net_sequenced...\n");
  NetTestWorld world;
  net_world_init(&world, 8u, NULL, NULL);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 10000u,
                                          .jitter_us = 20000u,
                                          .loss = 0.1f,
                                          .reorder = 0.3f});
  net_world_connect(&world);
  const VkrNetChannelConfig channel = {.delivery = VKR_NET_SEQUENCED};
  net_open_both(&world, 3u, &channel);
  uint8_t data[64];
  for (uint32_t i = 0u; i < 300u; ++i) {
    net_test_fill(data, sizeof(data), i);
    assert(vkr_net_core_send(world.client.core, world.client.connection, 3u,
                             data, sizeof(data), NULL,
                             world.now) == VKR_NET_SEND_OK);
    vkr_net_core_flush(world.client.core);
    net_world_run(&world, 2000u);
  }
  net_world_run(&world, NET_TEST_SECOND);
  assert(world.server.received[3] > 100u && world.server.received[3] < 300u);
  assert(!world.server.out_of_order && !world.server.duplicate &&
         !world.server.corrupt);
  net_world_destroy(&world);
  printf("  test_net_sequenced PASSED\n");
}

/* Fails when an unreliable message sent with NOTIFY does not get exactly one
   ACKED or LOST report, or is reported acknowledged without arriving. */
static void test_net_unreliable_notify(void) {
  printf("  Running test_net_unreliable_notify...\n");
  NetTestWorld world;
  net_world_init(&world, 9u, NULL, NULL);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 10000u, .loss = 0.3f});
  net_world_connect(&world);
  const VkrNetChannelConfig channel = {.delivery = VKR_NET_UNRELIABLE};
  net_open_both(&world, 4u, &channel);
  uint8_t data[200];
  const uint32_t count = 200u;
  for (uint32_t i = 0u; i < count; ++i) {
    net_test_fill(data, sizeof(data), i);
    const VkrNetSendOptions options = {
        .flags = VKR_NET_SEND_NOTIFY | VKR_NET_SEND_TAG, .tag = i};
    assert(vkr_net_core_send(world.client.core, world.client.connection, 4u,
                             data, sizeof(data), &options,
                             world.now) == VKR_NET_SEND_OK);
    vkr_net_core_flush(world.client.core);
    net_world_run(&world, 5000u);
  }
  net_world_run(&world, 5u * NET_TEST_SECOND);
  uint32_t acked = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    assert(world.client.acked_tags[i] + world.client.lost_tags[i] == 1u);
    if (world.client.acked_tags[i]) {
      assert(world.server.seen[4][i]);
      ++acked;
    }
  }
  assert(acked > count / 2u && acked < count);
  assert(world.server.received[4] >= acked);
  net_world_destroy(&world);
  printf("  test_net_unreliable_notify PASSED\n");
}

/* Fails when a deadline channel delivers a duplicate or corrupt message, or
   keeps expired messages queued at the sender. */
static void test_net_deadline(void) {
  printf("  Running test_net_deadline...\n");
  NetTestWorld world;
  net_world_init(&world, 10u, NULL, NULL);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 30000u, .loss = 0.4f});
  net_world_connect(&world);
  const VkrNetChannelConfig channel = {.delivery = VKR_NET_DEADLINE,
                                       .max_message_size = 64u * 1024u,
                                       .deadline_ms = 50u};
  net_open_both(&world, 5u, &channel);
  static uint8_t data[20000];
  const uint32_t count = 60u;
  for (uint32_t i = 0u; i < count; ++i) {
    net_test_fill(data, sizeof(data), i);
    assert(vkr_net_core_send(world.client.core, world.client.connection, 5u,
                             data, sizeof(data), NULL,
                             world.now) == VKR_NET_SEND_OK);
    vkr_net_core_flush(world.client.core);
    net_world_run(&world, 16667u);
  }
  net_world_run(&world, 10u * NET_TEST_SECOND);
  assert(!world.server.duplicate && !world.server.corrupt);
  assert(world.server.received[5] < count);
  assert(vkr_net_core_queued_bytes(world.client.core, world.client.connection,
                                   5u) == 0u);
  net_world_destroy(&world);
  printf("  test_net_deadline PASSED\n");
}

/* Fails when a sender exceeds the receive window of a receiver that takes
   no messages, or does not resume once the receiver takes them. */
static void test_net_flow_control(void) {
  printf("  Running test_net_flow_control...\n");
  NetTestWorld world;
  net_world_init(&world, 11u, NULL, NULL);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u});
  net_world_connect(&world);
  const VkrNetChannelConfig channel = {.delivery = VKR_NET_RELIABLE_ORDERED,
                                       .max_message_size = 16u * 1024u,
                                       .receive_window = 64u * 1024u,
                                       .send_queue = 128u};
  net_open_both(&world, 6u, &channel);
  world.server.hold = true_v;
  static uint8_t data[16u * 1024u];
  for (uint32_t i = 0u; i < 64u; ++i) {
    net_test_fill(data, sizeof(data), i);
    assert(vkr_net_core_send(world.client.core, world.client.connection, 6u,
                             data, sizeof(data), NULL,
                             world.now) == VKR_NET_SEND_OK);
  }
  vkr_net_core_flush(world.client.core);
  net_world_run(&world, 5u * NET_TEST_SECOND);
  /* 1 MiB queued; at most the window (and one message) may have left. */
  const uint64_t queued =
      vkr_net_core_queued_bytes(world.client.core, world.client.connection, 6u);
  assert(queued >= 64u * 16u * 1024u - 64u * 1024u - 16u * 1024u);
  world.server.hold = false_v;
  net_world_run(&world, 10u * NET_TEST_SECOND);
  assert(world.server.received[6] == 64u);
  assert(!world.server.out_of_order && !world.server.duplicate);
  assert(vkr_net_core_queued_bytes(world.client.core, world.client.connection,
                                   6u) == 0u);
  net_world_destroy(&world);
  printf("  test_net_flow_control PASSED\n");
}

// =============================================================================
// Path behavior
// =============================================================================

/* Fails when PMTU discovery does not reach the path's 1,452-byte limit, or
   counts its lost jumbo probes as congestion loss. */
static void test_net_mtu_discovery(void) {
  printf("  Running test_net_mtu_discovery...\n");
  const VkrNetCoreConfig config = {.max_datagram = VKR_NET_DATAGRAM_JUMBO};
  NetTestWorld world;
  net_world_init(&world, 12u, &config, &config);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u,
                                          .mtu = VKR_NET_DATAGRAM_ETHERNET});
  net_world_connect(&world);
  net_world_run(&world, 10u * NET_TEST_SECOND);
  VkrNetPathStats stats;
  assert(vkr_net_core_path(world.client.core, world.client.connection, &stats));
  assert(stats.max_datagram == VKR_NET_DATAGRAM_ETHERNET);
  assert(stats.packets_lost == 0u);
  net_world_destroy(&world);
  printf("  test_net_mtu_discovery PASSED\n");
}

/* Fails when a bulk transfer over a 20 MB/s, 40 ms path reaches less than
   70% of the link rate or loses more than 5% of its packets. */
static void test_net_bulk_throughput(void) {
  printf("  Running test_net_bulk_throughput...\n");
  NetTestWorld world;
  net_world_init(&world, 13u, NULL, NULL);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 20000u,
                                          .bandwidth = 20u * 1000u * 1000u,
                                          .queue_bytes = 800u * 1000u});
  net_world_connect(&world);
  const VkrNetChannelConfig channel = {.delivery = VKR_NET_RELIABLE_UNORDERED,
                                       .max_message_size = 256u * 1024u,
                                       .receive_window = 8u * 1024u * 1024u,
                                       .send_queue = 64u};
  net_open_both(&world, 7u, &channel);
  static uint8_t data[256u * 1024u];
  const uint32_t count = 80u;
  const uint64_t start = world.now;
  uint32_t next = 0u;
  uint32_t filled = UINT32_MAX;
  const uint64_t limit = world.now + 60u * NET_TEST_SECOND;
  while (world.server.received[7] < count && world.now < limit) {
    while (next < count) {
      if (filled != next) {
        net_test_fill(data, sizeof(data), next);
        filled = next;
      }
      if (vkr_net_core_send(world.client.core, world.client.connection, 7u,
                            data, sizeof(data), NULL,
                            world.now) != VKR_NET_SEND_OK) {
        break;
      }
      ++next;
    }
    vkr_net_core_flush(world.client.core);
    net_world_step(&world, limit);
  }
  assert(world.server.received[7] == count && !world.server.corrupt);
  const float64_t seconds = (float64_t)(world.now - start) / NET_TEST_SECOND;
  const float64_t rate = (float64_t)count * sizeof(data) / seconds;
  VkrNetPathStats stats;
  assert(vkr_net_core_path(world.client.core, world.client.connection, &stats));
  const float64_t loss =
      (float64_t)stats.packets_lost / (float64_t)Max(stats.packets_sent, 1u);
  printf("    %.1f MB/s of 20 MB/s, loss %.2f%%, srtt %.1f ms\n", rate / 1e6,
         loss * 100.0, (float64_t)stats.srtt_us / 1000.0);
  assert(rate >= 0.7 * 20e6);
  assert(loss <= 0.05);
  net_world_destroy(&world);
  printf("  test_net_bulk_throughput PASSED\n");
}

/* Fails when a quiet connection is not kept alive, or when a path that
   goes silent does not time out on both sides. */
static void test_net_idle(void) {
  printf("  Running test_net_idle...\n");
  const VkrNetCoreConfig config = {.idle_timeout_ms = 2000u};
  NetTestWorld world;
  net_world_init(&world, 14u, &config, &config);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u});
  net_world_connect(&world);
  net_world_run(&world, 10u * NET_TEST_SECOND);
  assert(!world.client.closed && !world.server.closed);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u, .loss = 1.0f});
  net_world_run(&world, 4u * NET_TEST_SECOND);
  assert(world.client.closed &&
         world.client.close_code == VKR_NET_CLOSE_TIMEOUT);
  assert(world.server.closed &&
         world.server.close_code == VKR_NET_CLOSE_TIMEOUT);
  net_world_destroy(&world);
  printf("  test_net_idle PASSED\n");
}

/* Fails when a close code does not reach the peer. */
static void test_net_close(void) {
  printf("  Running test_net_close...\n");
  NetTestWorld world;
  net_world_init(&world, 15u, NULL, NULL);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u});
  net_world_connect(&world);
  vkr_net_core_close(world.client.core, world.client.connection, 0x123u,
                     world.now);
  net_world_run(&world, 2u * NET_TEST_SECOND);
  assert(world.client.closed && world.client.close_code == 0x123u);
  assert(world.server.closed && world.server.close_code == 0x123u);
  net_world_destroy(&world);
  printf("  test_net_close PASSED\n");
}

/* Fails when a transfer stops after the client's address changes, as after
   a NAT rebinding. */
static void test_net_migration(void) {
  printf("  Running test_net_migration...\n");
  NetTestWorld world;
  net_world_init(&world, 16u, NULL, NULL);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u});
  net_world_connect(&world);
  const VkrNetChannelConfig channel = {.delivery = VKR_NET_RELIABLE_ORDERED,
                                       .max_message_size = 64u * 1024u};
  net_open_both(&world, 8u, &channel);
  static uint8_t data[30000];
  for (uint32_t i = 0u; i < 40u; ++i) {
    if (i == 20u) {
      VkrNetAddress moved;
      assert(vkr_net_address_parse("10.0.0.2:6000", 0u, &moved));
      vkr_net_sim_set_address(world.sim, world.client.endpoint, &moved);
    }
    net_test_fill(data, sizeof(data), i);
    assert(vkr_net_core_send(world.client.core, world.client.connection, 8u,
                             data, sizeof(data), NULL,
                             world.now) == VKR_NET_SEND_OK);
    vkr_net_core_flush(world.client.core);
    net_world_run(&world, 50000u);
  }
  net_world_run(&world, 5u * NET_TEST_SECOND);
  assert(world.server.received[8] == 40u && !world.server.out_of_order);
  VkrNetAddress peer;
  assert(vkr_net_core_peer_address(world.server.core, world.server.connection,
                                   &peer));
  assert(peer.port == 6000u);
  net_world_destroy(&world);
  printf("  test_net_migration PASSED\n");
}

/* Fails when large fragmented messages on a low-priority unordered channel
   stop completing over a lossy, bandwidth-limited path once the sender has
   nothing else to send (the shape of a depot upload). */
static void test_net_large_tail(void) {
  printf("  Running test_net_large_tail...\n");
  NetTestWorld world;
  const VkrNetCoreConfig config = {.sent_packet_capacity = 8192u};
  net_world_init(&world, 19u, &config, &config);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 10000u,
                                          .jitter_us = 3000u,
                                          .loss = 0.05f,
                                          .bandwidth = 50u * 1000u * 1000u});
  net_world_connect(&world);
  const VkrNetChannelConfig channel = {.delivery = VKR_NET_RELIABLE_UNORDERED,
                                       .priority = 7u,
                                       .max_message_size = 32u << 20,
                                       .receive_window = 64u << 20,
                                       .send_queue = 512u};
  net_open_both(&world, 9u, &channel);
  static uint8_t data[440000];
  static const uint32_t sizes[] = {95u,     121u,    61u,     20u,     67u,
                                   349u,    348955u, 258824u, 322541u, 405689u,
                                   355245u, 301214u, 430780u, 341000u, 290000u};
  for (uint32_t i = 0u; i < ArrayCount(sizes); ++i) {
    net_test_fill(data, sizes[i], i);
    const VkrNetSendOptions options = {.flags = VKR_NET_SEND_TAG, .tag = i};
    assert(vkr_net_core_send(world.client.core, world.client.connection, 9u,
                             data, sizes[i], &options,
                             world.now) == VKR_NET_SEND_OK);
  }
  vkr_net_core_flush(world.client.core);
  const uint64_t limit = world.now + 60u * NET_TEST_SECOND;
  while (world.server.received[9] < ArrayCount(sizes) - 4u &&
         world.now < limit) {
    net_world_step(&world, limit);
  }
  /* The small first messages fail the content check (under 8 bytes they
     carry no index); count the large ones. */
  uint32_t large = 0u;
  for (uint32_t i = 6u; i < ArrayCount(sizes); ++i) {
    large += world.server.seen[9][i];
  }
  while (large < ArrayCount(sizes) - 6u && world.now < limit) {
    net_world_step(&world, limit);
    large = 0u;
    for (uint32_t i = 6u; i < ArrayCount(sizes); ++i) {
      large += world.server.seen[9][i];
    }
  }
  if (large < ArrayCount(sizes) - 6u) {
    VkrNetPathStats stats;
    (void)vkr_net_core_path(world.client.core, world.client.connection, &stats);
    printf("    stalled: %u of 9 large messages, queued %llu, in flight %llu, "
           "cwnd %llu, pacing %llu, srtt %llu, lost %llu of %llu\n",
           large,
           (unsigned long long)vkr_net_core_queued_bytes(
               world.client.core, world.client.connection, 9u),
           (unsigned long long)stats.bytes_in_flight,
           (unsigned long long)stats.cwnd,
           (unsigned long long)stats.pacing_rate,
           (unsigned long long)stats.srtt_us,
           (unsigned long long)stats.packets_lost,
           (unsigned long long)stats.packets_sent);
  }
  assert(large == ArrayCount(sizes) - 6u);
  net_world_destroy(&world);
  printf("  test_net_large_tail PASSED\n");
}

/* Fails when random or truncated datagrams crash the core, produce events or
   disturb an established connection. */
static void test_net_garbage(void) {
  printf("  Running test_net_garbage...\n");
  NetTestWorld world;
  net_world_init(&world, 17u, NULL, NULL);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u});
  net_world_connect(&world);
  const uint32_t server_cid = world.server.connection;
  uint8_t datagram[1500];
  VkrNetAddress attacker;
  assert(vkr_net_address_parse("10.9.9.9:1234", 0u, &attacker));
  for (uint32_t i = 0u; i < 20000u; ++i) {
    const uint32_t size =
        (uint32_t)(vkr_net_sim_random(world.sim) % sizeof(datagram));
    for (uint32_t b = 0u; b < size; ++b) {
      datagram[b] = (uint8_t)vkr_net_sim_random(world.sim);
    }
    if (size > 5u && (i % 3u) == 0u) {
      /* A short header naming the live connection. */
      datagram[0] = 0x40u | (datagram[0] & 0x03u);
      memcpy(datagram + 1, &server_cid, 4u);
    } else if (size > 13u && (i % 3u) == 1u) {
      /* A long header of every type with the right version. */
      datagram[0] = (uint8_t)(0xc0u | (datagram[0] & 0x30u));
      const uint32_t version = VKR_NET_PROTOCOL_VERSION;
      memcpy(datagram + 1, &version, 4u);
    }
    vkr_net_core_receive(world.server.core, &attacker, datagram, size,
                         world.now);
  }
  net_world_run(&world, NET_TEST_SECOND);
  assert(world.server.connected_events == 1u && !world.server.closed);
  assert(!world.client.closed);
  net_world_destroy(&world);
  printf("  test_net_garbage PASSED\n");
}

// =============================================================================
// Real sockets
// =============================================================================

/* Waits up to one second for `count` datagrams on `socket`. */
static uint32_t net_test_receive(VkrUdpSocket socket, VkrUdpDatagram *batch,
                                 uint32_t count, uint32_t *out_truncated) {
  uint32_t total = 0u;
  uint32_t truncated = 0u;
  for (uint32_t attempt = 0u; attempt < 100u && total + truncated < count;
       ++attempt) {
    (void)vkr_udp_socket_wait(socket, 10);
    uint32_t received = 0u;
    uint32_t cut = 0u;
    assert(vkr_udp_socket_receive(socket, batch + total, count - total,
                                  &received, &cut) != VKR_UDP_ERROR);
    total += received;
    truncated += cut;
  }
  if (out_truncated) {
    *out_truncated = truncated;
  }
  return total;
}

/* Fails when loopback datagrams between an IPv4 socket and a dual-stack
   IPv6 socket lose bytes or addresses, or when an oversized datagram is not
   reported as truncated. */
static void test_net_udp_loopback(void) {
  printf("  Running test_net_udp_loopback...\n");
  VkrUdpSocket v4, v6;
  const VkrNetAddress bind4 =
      vkr_net_address_loopback(VKR_NET_ADDRESS_IPV4, 0u);
  const VkrNetAddress bind6 = vkr_net_address_any(VKR_NET_ADDRESS_IPV6, 0u);
  assert(vkr_udp_socket_open(&bind4, NULL, &v4));
  assert(vkr_udp_socket_open(&bind6, NULL, &v6));
  VkrNetAddress v4_local, v6_local;
  assert(vkr_udp_socket_local_address(v4, &v4_local));
  assert(vkr_udp_socket_local_address(v6, &v6_local));
  assert(v4_local.port != 0u && v6_local.port != 0u);

  /* IPv4 sender to the dual-stack socket through 127.0.0.1. */
  const VkrNetAddress to_v6 =
      vkr_net_address_loopback(VKR_NET_ADDRESS_IPV4, v6_local.port);
  uint8_t payload[8][300];
  VkrUdpDatagram outgoing[8];
  for (uint32_t i = 0u; i < 8u; ++i) {
    memset(payload[i], (int)i + 1, sizeof(payload[i]));
    outgoing[i] = (VkrUdpDatagram){.address = to_v6,
                                   .data = payload[i],
                                   .capacity = sizeof(payload[i]),
                                   .size = 100u + i};
  }
  uint32_t sent = 0u;
  assert(vkr_udp_socket_send(v4, outgoing, 8u, &sent) == VKR_UDP_OK &&
         sent == 8u);
  uint8_t storage[8][512];
  VkrUdpDatagram incoming[8];
  for (uint32_t i = 0u; i < 8u; ++i) {
    incoming[i] =
        (VkrUdpDatagram){.data = storage[i], .capacity = sizeof(storage[i])};
  }
  assert(net_test_receive(v6, incoming, 8u, NULL) == 8u);
  for (uint32_t i = 0u; i < 8u; ++i) {
    /* Loopback keeps order; the source reads as IPv4. */
    assert(incoming[i].size == 100u + i);
    assert(incoming[i].data[0] == (uint8_t)(i + 1u));
    assert(incoming[i].address.family == VKR_NET_ADDRESS_IPV4);
    assert(incoming[i].address.port == v4_local.port);
  }

  /* The dual-stack socket answers the IPv4 peer. */
  VkrUdpDatagram reply = {.address = incoming[0].address,
                          .data = payload[0],
                          .capacity = sizeof(payload[0]),
                          .size = 42u};
  assert(vkr_udp_socket_send(v6, &reply, 1u, &sent) == VKR_UDP_OK);
  assert(net_test_receive(v4, incoming, 1u, NULL) == 1u);
  assert(incoming[0].size == 42u);

  /* A datagram larger than the receive buffer is skipped and counted. */
  static uint8_t large[2000];
  VkrUdpDatagram big = {.address = to_v6,
                        .data = large,
                        .capacity = sizeof(large),
                        .size = sizeof(large)};
  assert(vkr_udp_socket_send(v4, &big, 1u, &sent) == VKR_UDP_OK);
  uint32_t truncated = 0u;
  incoming[0].capacity = 512u;
  assert(net_test_receive(v6, incoming, 1u, &truncated) == 0u);
  assert(truncated == 1u);

  vkr_udp_socket_close(&v4);
  vkr_udp_socket_close(&v6);
  assert(v4.handle == VKR_UDP_SOCKET_INVALID);
  printf("  test_net_udp_loopback PASSED\n");
}

/* Fails when two hosts on loopback sockets do not complete a handshake and
   exchange a reliable message through vkr_net_host_pump. */
static void test_net_host_loopback(void) {
  printf("  Running test_net_host_loopback...\n");
  ContainerTestAllocator state = {0};
  VkrAllocator allocator = container_test_allocator(&state);
  VkrNetHostConfig server_config = {
      .bind = vkr_net_address_loopback(VKR_NET_ADDRESS_IPV4, 0u)};
  server_config.core.accept_incoming = true_v;
  vkr_net_keypair_generate(&server_config.core.static_keys);
  VkrNetHostConfig client_config = {
      .bind = vkr_net_address_loopback(VKR_NET_ADDRESS_IPV4, 0u)};
  vkr_net_keypair_generate(&client_config.core.static_keys);
  VkrNetHost *server = vkr_net_host_create(&allocator, &server_config);
  VkrNetHost *client = vkr_net_host_create(&allocator, &client_config);
  assert(server && client);
  VkrNetAddress server_address;
  assert(vkr_net_host_local_address(server, &server_address));

  VkrNetCore *server_core = vkr_net_host_core(server);
  VkrNetCore *client_core = vkr_net_host_core(client);
  VkrNetConnectionId connection = VKR_NET_CONNECTION_NONE;
  VkrNetConnectionId accepted = VKR_NET_CONNECTION_NONE;
  assert(vkr_net_core_connect(client_core, &server_address,
                              server_config.core.static_keys.public_key, NULL,
                              0u, vkr_net_host_now(client), &connection));
  const VkrNetChannelConfig channel = {.delivery = VKR_NET_RELIABLE_ORDERED};
  bool8_t client_ready = false_v;
  bool8_t sent = false_v;
  bool8_t delivered = false_v;
  for (uint32_t round = 0u; round < 2000u && !delivered; ++round) {
    assert(vkr_net_host_pump(server, 1));
    assert(vkr_net_host_pump(client, 1));
    VkrNetEvent event;
    while (vkr_net_core_poll(server_core, &event)) {
      if (event.type == VKR_NET_EVENT_CONNECTED) {
        accepted = event.connection;
        assert(vkr_net_core_open_channel(server_core, accepted, 1u, &channel));
      } else if (event.type == VKR_NET_EVENT_MESSAGE) {
        assert(event.connection == accepted && event.channel == 1u);
        assert(event.size == 5u && memcmp(event.data, "hello", 5u) == 0);
        delivered = true_v;
      }
    }
    while (vkr_net_core_poll(client_core, &event)) {
      if (event.type == VKR_NET_EVENT_CONNECTED) {
        assert(
            vkr_net_core_open_channel(client_core, connection, 1u, &channel));
        client_ready = true_v;
      }
    }
    if (client_ready && !sent) {
      const VkrNetSendOptions options = {.flags = VKR_NET_SEND_IMMEDIATE};
      assert(vkr_net_core_send(client_core, connection, 1u, "hello", 5u,
                               &options,
                               vkr_net_host_now(client)) == VKR_NET_SEND_OK);
      sent = true_v;
    }
  }
  assert(delivered);
  vkr_net_host_destroy(client);
  vkr_net_host_destroy(server);
  assert(state.live_bytes == 0u);
  printf("  test_net_host_loopback PASSED\n");
}

// =============================================================================
// Session and services
// =============================================================================

typedef struct NetTestServiceLog {
  uint32_t opened;
  uint16_t version;
  uint32_t closed;
  uint16_t close_code;
  uint32_t messages;
  uint8_t last_channel;
  uint32_t last_size;
  uint8_t last[64];
  uint16_t refuse;
} NetTestServiceLog;

static uint16_t net_test_service_accept(void *context,
                                        VkrNetConnectionId connection,
                                        uint16_t version) {
  (void)connection;
  (void)version;
  return ((NetTestServiceLog *)context)->refuse;
}

static void net_test_service_opened(void *context,
                                    VkrNetConnectionId connection,
                                    uint16_t version) {
  (void)connection;
  NetTestServiceLog *log = context;
  log->opened += 1u;
  log->version = version;
}

static void net_test_service_event(void *context, VkrNetConnectionId connection,
                                   uint8_t channel, const VkrNetEvent *event,
                                   uint64_t now_us) {
  (void)connection;
  (void)now_us;
  NetTestServiceLog *log = context;
  if (event->type != VKR_NET_EVENT_MESSAGE) {
    return;
  }
  log->messages += 1u;
  log->last_channel = channel;
  log->last_size = Min(event->size, 64u);
  memcpy(log->last, event->data, log->last_size);
}

static void net_test_service_closed(void *context,
                                    VkrNetConnectionId connection,
                                    uint16_t code) {
  (void)connection;
  NetTestServiceLog *log = context;
  log->closed += 1u;
  log->close_code = code;
}

static VkrNetService net_test_service(uint16_t id, NetTestServiceLog *log,
                                      const VkrNetServiceVersion *versions,
                                      uint32_t version_count) {
  VkrNetService service = {
      .id = id,
      .version_count = version_count,
      .channel_count = 2u,
      .context = log,
      .accept = net_test_service_accept,
      .opened = net_test_service_opened,
      .event = net_test_service_event,
      .closed = net_test_service_closed,
  };
  for (uint32_t i = 0u; i < version_count; ++i) {
    service.versions[i] = versions[i];
  }
  service.channels[0] = (VkrNetChannelConfig){
      .delivery = VKR_NET_RELIABLE_ORDERED, .priority = 2u};
  service.channels[1] =
      (VkrNetChannelConfig){.delivery = VKR_NET_UNRELIABLE, .priority = 1u};
  return service;
}

/* Fails when a service does not open with the highest common version,
   carry messages both ways on its channels, refuse an unknown service, a
   schema without a common version, differing channels or the acceptor's
   veto with the right codes, or report its end on close and disconnect. */
static void test_net_session_services(void) {
  printf("  Running test_net_session_services...\n");
  NetTestWorld world;
  net_world_init(&world, 18u, NULL, NULL);
  net_world_link(&world, &(VkrNetSimLink){.latency_us = 5000u});
  world.server.session =
      vkr_net_session_create(&world.allocator, world.server.core);
  world.client.session =
      vkr_net_session_create(&world.allocator, world.client.core);
  assert(world.server.session && world.client.session);

  const VkrNetServiceVersion both[] = {{1u, 0x1111u}, {2u, 0x2222u}};
  const VkrNetServiceVersion server_one[] = {{1u, 0x1111u}, {2u, 0x9999u}};
  const VkrNetServiceVersion only_three[] = {{3u, 0x3333u}};
  NetTestServiceLog s_main = {0}, c_main = {0}, s_old = {0}, c_old = {0};
  NetTestServiceLog c_unknown = {0}, s_mismatch = {0}, c_mismatch = {0};
  NetTestServiceLog s_veto = {.refuse = 0x150u}, c_veto = {0};
  NetTestServiceLog s_channels = {0}, c_channels = {0};

  /* 10: both speak 1 and 2. 11: the server shares only version 1's schema.
     12: the client speaks only version 3. 13: the server vetoes. 14: the
     channels differ. 15: the server lacks it. */
  assert(vkr_net_session_register(world.server.session, &(VkrNetService){0}) ==
         false_v);
  VkrNetService service = net_test_service(10u, &s_main, both, 2u);
  assert(vkr_net_session_register(world.server.session, &service));
  service = net_test_service(10u, &c_main, both, 2u);
  assert(vkr_net_session_register(world.client.session, &service));
  service = net_test_service(11u, &s_old, server_one, 2u);
  assert(vkr_net_session_register(world.server.session, &service));
  service = net_test_service(11u, &c_old, both, 2u);
  assert(vkr_net_session_register(world.client.session, &service));
  service = net_test_service(12u, &s_mismatch, both, 2u);
  assert(vkr_net_session_register(world.server.session, &service));
  service = net_test_service(12u, &c_mismatch, only_three, 1u);
  assert(vkr_net_session_register(world.client.session, &service));
  service = net_test_service(13u, &s_veto, both, 2u);
  assert(vkr_net_session_register(world.server.session, &service));
  service = net_test_service(13u, &c_veto, both, 2u);
  assert(vkr_net_session_register(world.client.session, &service));
  service = net_test_service(14u, &s_channels, both, 2u);
  service.channels[1].priority = 5u;
  assert(vkr_net_session_register(world.server.session, &service));
  service = net_test_service(14u, &c_channels, both, 2u);
  assert(vkr_net_session_register(world.client.session, &service));
  service = net_test_service(15u, &c_unknown, both, 2u);
  assert(vkr_net_session_register(world.client.session, &service));
  /* A second registration of an ID fails. */
  assert(!vkr_net_session_register(world.client.session, &service));

  net_world_connect(&world);
  assert(world.client.connected && world.server.connected);
  const VkrNetConnectionId client = world.client.connection;
  const VkrNetConnectionId server = world.server.connection;
  for (uint16_t id = 10u; id <= 15u; ++id) {
    assert(vkr_net_session_open(world.client.session, client, id, world.now));
  }
  /* Sending before the service opens fails. */
  assert(vkr_net_session_send(world.client.session, client, 10u, 0u, "x", 1u,
                              NULL, world.now) == VKR_NET_SEND_CLOSED);
  net_world_run(&world, NET_TEST_SECOND);

  assert(c_main.opened == 1u && s_main.opened == 1u);
  assert(c_main.version == 2u && s_main.version == 2u);
  assert(vkr_net_session_version(world.client.session, client, 10u) == 2u);
  assert(c_old.opened == 1u && c_old.version == 1u && s_old.version == 1u);
  assert(c_mismatch.opened == 0u && c_mismatch.closed == 1u &&
         c_mismatch.close_code == VKR_NET_CLOSE_SCHEMA_MISMATCH);
  assert(c_veto.closed == 1u && c_veto.close_code == 0x150u &&
         s_veto.opened == 0u);
  assert(c_channels.closed == 1u &&
         c_channels.close_code == VKR_NET_CLOSE_SCHEMA_MISMATCH);
  assert(c_unknown.closed == 1u &&
         c_unknown.close_code == VKR_NET_CLOSE_SERVICE_REFUSED);

  /* Messages both ways, on the service's own channel indices. */
  assert(vkr_net_session_send(world.client.session, client, 10u, 0u, "ping", 4u,
                              NULL, world.now) == VKR_NET_SEND_OK);
  assert(vkr_net_session_send(world.server.session, server, 10u, 1u, "pong!",
                              5u, NULL, world.now) == VKR_NET_SEND_OK);
  assert(vkr_net_session_send(world.client.session, client, 11u, 1u, "old", 3u,
                              NULL, world.now) == VKR_NET_SEND_OK);
  vkr_net_core_flush(world.client.core);
  vkr_net_core_flush(world.server.core);
  net_world_run(&world, NET_TEST_SECOND);
  assert(s_main.messages == 1u && s_main.last_channel == 0u &&
         s_main.last_size == 4u && memcmp(s_main.last, "ping", 4u) == 0);
  assert(c_main.messages == 1u && c_main.last_channel == 1u &&
         memcmp(c_main.last, "pong!", 5u) == 0);
  assert(s_old.messages == 1u && s_old.last_channel == 1u);
  /* The application saw none of these messages. */
  assert(world.server.received[1] == 0u && world.client.received[1] == 0u);

  /* The server closes service 11 with a code. */
  vkr_net_session_close(world.server.session, server, 11u, 0x177u, world.now);
  assert(s_old.closed == 1u && s_old.close_code == 0x177u);
  net_world_run(&world, NET_TEST_SECOND);
  assert(c_old.closed == 1u && c_old.close_code == 0x177u);
  assert(!vkr_net_session_is_open(world.client.session, client, 11u));
  /* Reopening after a close works. */
  assert(vkr_net_session_open(world.client.session, client, 11u, world.now));
  net_world_run(&world, NET_TEST_SECOND);
  assert(c_old.opened == 2u);

  /* The connection's end closes every open service. */
  vkr_net_core_close(world.client.core, client, 0x200u, world.now);
  net_world_run(&world, 2u * NET_TEST_SECOND);
  assert(c_main.closed == 1u && c_main.close_code == 0x200u);
  assert(s_main.closed == 1u && s_main.close_code == 0x200u);
  assert(c_old.closed == 2u && s_old.closed == 2u);

  vkr_net_session_destroy(world.client.session);
  vkr_net_session_destroy(world.server.session);
  net_world_destroy(&world);
  printf("  test_net_session_services PASSED\n");
}

bool32_t run_net_tests(void) {
  /* Unbuffered, so a stopped run shows the test it stopped in. */
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("--- Starting network transport tests ---\n");
  assert(vkr_net_crypto_init());
  test_net_varint();
  test_net_noise_vector();
  test_net_packet_protection();
  test_net_address();
  test_net_handshake();
  test_net_handshake_refused();
  test_net_handshake_wrong_key();
  test_net_retry();
  test_net_amplification();
  test_net_reliable_ordered();
  test_net_reliable_unordered();
  test_net_sequenced();
  test_net_unreliable_notify();
  test_net_deadline();
  test_net_flow_control();
  test_net_mtu_discovery();
  test_net_bulk_throughput();
  test_net_idle();
  test_net_close();
  test_net_migration();
  test_net_large_tail();
  test_net_garbage();
  test_net_udp_loopback();
  test_net_host_loopback();
  test_net_session_services();
  printf("--- Network transport tests completed ---\n");
  return true;
}
