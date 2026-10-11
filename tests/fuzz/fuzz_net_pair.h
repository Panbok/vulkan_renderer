#pragma once

#include "../src/container_test_allocator.h"
#include "fuzz_common.h"
#include "vkr_net_core.h"
#include "vkr_net_crypto.h"

/* A server and a client core joined in-process through the public core API:
 * datagrams pass straight from one core's transmit to the other's receive
 * on a virtual clock, with no socket. Static keys come from fixed secrets so
 * a crash input replays against the same identities. */

#define FUZZ_NET_DATAGRAM 1500u
#define FUZZ_NET_PUMP_ROUNDS 64u

typedef void (*FuzzNetEventFn)(void *context, bool8_t server_side,
                               const VkrNetEvent *event);

typedef struct FuzzNetPair {
  ContainerTestAllocator state;
  VkrAllocator allocator;
  VkrNetKeyPair server_keys;
  VkrNetKeyPair client_keys;
  VkrNetAddress server_address;
  VkrNetAddress client_address;
  VkrNetCore *server;
  VkrNetCore *client;
  /* The connection's ID on each side once CONNECTED arrived. */
  VkrNetConnectionId server_connection;
  VkrNetConnectionId client_connection;
  uint64_t now;
  FuzzNetEventFn on_event;
  void *event_context;
} FuzzNetPair;

static inline void fuzz_net_key(uint8_t label, VkrNetKeyPair *out) {
  uint8_t secret[VKR_NET_KEY_SIZE];
  for (uint32_t i = 0u; i < VKR_NET_KEY_SIZE; ++i) {
    secret[i] = (uint8_t)(label * 31u + i * 7u + 1u);
  }
  vkr_net_keypair_from_secret(secret, out);
}

static inline void fuzz_net_accept(void *context, const VkrNetAddress *address,
                                   const uint8_t peer_key[VKR_NET_KEY_SIZE],
                                   const uint8_t *credential,
                                   uint32_t credential_size,
                                   VkrNetAcceptResult *out_result) {
  (void)context;
  (void)address;
  (void)peer_key;
  /* A credential starting with 0xff is refused with the code after it. */
  if (credential_size >= 2u && credential[0] == 0xffu) {
    out_result->code =
        credential[1] ? credential[1] : VKR_NET_CLOSE_SERVER_FULL;
    return;
  }
  const uint32_t echoed = Min(credential_size, VKR_NET_ACCEPT_PAYLOAD_MAX);
  if (echoed > 0u) {
    MemCopy(out_result->payload, credential, echoed);
  }
  out_result->payload_size = echoed;
  out_result->user = credential_size;
}

/* Reads one event's bytes so ASan sees a stale or short view. */
static inline uint32_t fuzz_net_touch(const VkrNetEvent *event) {
  uint32_t sum = 0u;
  if (event->size > 0u) {
    FUZZ_CHECK(event->data);
    for (uint32_t i = 0u; i < event->size; ++i) {
      sum += event->data[i];
    }
  }
  return sum;
}

static inline void fuzz_net_poll(FuzzNetPair *pair, bool8_t server_side) {
  VkrNetCore *core = server_side ? pair->server : pair->client;
  VkrNetEvent event;
  while (vkr_net_core_poll(core, &event)) {
    (void)fuzz_net_touch(&event);
    if (event.type == VKR_NET_EVENT_CONNECTED) {
      FUZZ_CHECK(event.size <= VKR_NET_ACCEPT_PAYLOAD_MAX);
      if (server_side) {
        pair->server_connection = event.connection;
      } else {
        pair->client_connection = event.connection;
      }
    }
    if (pair->on_event) {
      pair->on_event(pair->event_context, server_side, &event);
    }
  }
}

/* Moves every pending datagram from `from` into `to`, or drops them when
   `to` is NULL. Returns how many moved. */
static inline uint32_t fuzz_net_drain(FuzzNetPair *pair, VkrNetCore *from,
                                      VkrNetCore *to,
                                      const VkrNetAddress *source) {
  uint32_t moved = 0u;
  uint8_t datagram[FUZZ_NET_DATAGRAM];
  for (uint32_t i = 0u; i < 256u; ++i) {
    VkrNetAddress destination = {0};
    const uint32_t size = vkr_net_core_transmit(from, pair->now, datagram,
                                                sizeof(datagram), &destination);
    if (size == 0u) {
      break;
    }
    FUZZ_CHECK(size <= sizeof(datagram));
    if (to) {
      vkr_net_core_receive(to, source, datagram, size, pair->now);
    }
    moved += 1u;
  }
  return moved;
}

/* Exchanges traffic and events until both sides go quiet, advancing the
   clock to the next deadline when nothing moves, at most to `limit`. */
static inline void fuzz_net_pump(FuzzNetPair *pair, uint64_t limit) {
  for (uint32_t round = 0u; round < FUZZ_NET_PUMP_ROUNDS; ++round) {
    vkr_net_core_update(pair->client, pair->now);
    vkr_net_core_update(pair->server, pair->now);
    uint32_t moved =
        fuzz_net_drain(pair, pair->client, pair->server, &pair->client_address);
    moved +=
        fuzz_net_drain(pair, pair->server, pair->client, &pair->server_address);
    fuzz_net_poll(pair, true_v);
    fuzz_net_poll(pair, false_v);
    if (moved > 0u) {
      continue;
    }
    const uint64_t next = Min(vkr_net_core_next_deadline(pair->client),
                              vkr_net_core_next_deadline(pair->server));
    if (next == UINT64_MAX || next > limit) {
      return;
    }
    pair->now = Max(pair->now + 1u, next);
  }
}

static inline bool8_t fuzz_net_pair_create(FuzzNetPair *pair,
                                           const VkrNetCoreConfig *server_base,
                                           FuzzNetEventFn on_event,
                                           void *event_context) {
  MemZero(pair, sizeof(*pair));
  pair->allocator = container_test_allocator(&pair->state);
  pair->on_event = on_event;
  pair->event_context = event_context;
  pair->now = 1000000u;
  fuzz_net_key(1u, &pair->server_keys);
  fuzz_net_key(2u, &pair->client_keys);
  FUZZ_CHECK(vkr_net_address_parse("10.0.0.1:4000", 0u, &pair->server_address));
  FUZZ_CHECK(vkr_net_address_parse("10.0.0.2:5000", 0u, &pair->client_address));

  VkrNetCoreConfig server_config =
      server_base ? *server_base : (VkrNetCoreConfig){0};
  server_config.static_keys = pair->server_keys;
  server_config.accept_incoming = true_v;
  if (!server_config.accept) {
    server_config.accept = fuzz_net_accept;
  }
  pair->server = vkr_net_core_create(&pair->allocator, &server_config);
  const VkrNetCoreConfig client_config = {.static_keys = pair->client_keys};
  pair->client = vkr_net_core_create(&pair->allocator, &client_config);
  FUZZ_CHECK(pair->server && pair->client);
  return true_v;
}

/* Connects the client and runs the handshake to completion on both sides. */
static inline void fuzz_net_pair_connect(FuzzNetPair *pair,
                                         const uint8_t *credential,
                                         uint32_t credential_size) {
  VkrNetConnectionId pending = VKR_NET_CONNECTION_NONE;
  FUZZ_CHECK(vkr_net_core_connect(pair->client, &pair->server_address,
                                  pair->server_keys.public_key, credential,
                                  credential_size, pair->now, &pending));
  fuzz_net_pump(pair, pair->now + 2000000u);
  FUZZ_CHECK(pair->server_connection != VKR_NET_CONNECTION_NONE);
  FUZZ_CHECK(pair->client_connection == pending);
}

/* Destroys both cores; every byte they allocated must have returned. */
static inline void fuzz_net_pair_destroy(FuzzNetPair *pair) {
  vkr_net_core_destroy(pair->client);
  vkr_net_core_destroy(pair->server);
  FUZZ_CHECK(pair->state.live_bytes == 0u);
}
