#pragma once

#include "platform/vkr_udp_socket.h"

/* LAN discovery of depot servers (ADR-105). A server answers probes on
 * UDP port VKR_DEPOT_DISCOVERY_PORT; a client broadcasts a probe to every
 * IPv4 subnet it is on, or sends it to an address it names, and collects
 * the answers. Both are binary datagrams, little-endian:
 *
 *   PROBE   "VKDP", kind 1 u8, version u8, nonce u64
 *   ANSWER  "VKDP", kind 2 u8, version u8, nonce u64, service port u16,
 *           server key[32], name length u8, name, host length u8, host
 *
 * An answer is not authenticated: it says where a server claims to be and
 * which key it claims. The transport then proves the key, so a client that
 * checks the key once against the one the server prints is safe. */

#define VKR_DEPOT_DISCOVERY_PORT 7319u
/* A server's name or host name with its terminator. */
#define VKR_DEPOT_DISCOVERY_TEXT 64u

typedef struct VkrDepotDiscovered {
  /* The answering address with the service port. */
  VkrNetAddress address;
  uint8_t server_key[32];
  char name[VKR_DEPOT_DISCOVERY_TEXT];
  char host[VKR_DEPOT_DISCOVERY_TEXT];
} VkrDepotDiscovered;

/* A server's side: a socket on VKR_DEPOT_DISCOVERY_PORT that answers with
   the server's port, key, name and host. */
typedef struct VkrDepotResponder {
  VkrUdpSocket socket;
  uint16_t service_port;
  uint8_t server_key[32];
  char name[VKR_DEPOT_DISCOVERY_TEXT];
  char host[VKR_DEPOT_DISCOVERY_TEXT];
} VkrDepotResponder;

/* False when the port is taken, as by another server on this machine. */
bool8_t vkr_depot_responder_open(VkrDepotResponder *responder,
                                 uint16_t service_port,
                                 const uint8_t server_key[32],
                                 const char *name);
void vkr_depot_responder_close(VkrDepotResponder *responder);

/* Answers every probe waiting on the socket; never blocks. */
void vkr_depot_responder_update(VkrDepotResponder *responder);

/* Probes the IPv4 subnets this machine is on, or only `target` when it is
   not NULL, and collects distinct answers for `timeout_ms`. Returns how
   many it stored, at most `capacity`. */
uint32_t vkr_depot_discover(const VkrNetAddress *target, uint32_t timeout_ms,
                            VkrDepotDiscovered *out, uint32_t capacity);

/* The wire form, for tests: a probe or an answer into `out` (at least 192
   bytes), returning its size. */
uint32_t vkr_depot_discovery_probe(uint64_t nonce, uint8_t *out);
uint32_t vkr_depot_discovery_answer(const VkrDepotResponder *responder,
                                    uint64_t nonce, uint8_t *out);

/* Reads a probe's nonce; false for anything else. */
bool8_t vkr_depot_discovery_read_probe(const uint8_t *bytes, uint32_t size,
                                       uint64_t *out_nonce);

/* Reads an answer to the probe `nonce`; false for anything else. The
   address is left for the caller. */
bool8_t vkr_depot_discovery_read_answer(const uint8_t *bytes, uint32_t size,
                                        uint64_t nonce,
                                        VkrDepotDiscovered *out);
