#pragma once

#include "platform/vkr_udp_socket.h"

/* LAN discovery of depot servers (ADR-105). A server answers probes on
 * UDP port VKR_DEPOT_DISCOVERY_PORT unless it names another, as a second
 * server on one machine must; a client broadcasts a probe to every
 * IPv4 subnet it is on, or sends it to an address it names, and collects
 * the answers. Both are binary datagrams, little-endian:
 *
 *   PROBE   "VKDP", kind 1 u8, version u8, nonce u64
 *   ANSWER  "VKDP", kind 2 u8, version u8, nonce u64, service port u16,
 *           server key[32], name length u8, name, host length u8, host
 *
 * An answer is not authenticated: it says where a server claims to be and
 * which key it claims. The transport then proves the key, so a client that
 * checks the key once against the one the server prints is safe.
 *
 * Some endpoint security drops answers to broadcast probes but passes
 * answers to probes sent to one address (seen on a managed Mac), so a
 * browser also probes the servers a user connected to before. */

#define VKR_DEPOT_DISCOVERY_PORT 7319u
/* A server's name or host name with its terminator. */
#define VKR_DEPOT_DISCOVERY_TEXT 64u

#define VKR_DEPOT_BROWSE_TARGETS 16u
#define VKR_DEPOT_BROWSE_MAX 32u
/* A server a browser has not heard from for this long leaves `found`. */
#define VKR_DEPOT_BROWSE_EXPIRY_SECONDS 30.0

typedef struct VkrDepotDiscovered {
  /* The answering address with the service port. */
  VkrNetAddress address;
  uint8_t server_key[32];
  char name[VKR_DEPOT_DISCOVERY_TEXT];
  char host[VKR_DEPOT_DISCOVERY_TEXT];
  /* Platform seconds (vkr_platform_get_absolute_time) of the latest
     answer. */
  float64_t last_seen;
} VkrDepotDiscovered;

/* A server's side: a socket on a discovery port that answers with the
   server's port, key, name and host. */
typedef struct VkrDepotResponder {
  VkrUdpSocket socket;
  uint16_t service_port;
  uint8_t server_key[32];
  char name[VKR_DEPOT_DISCOVERY_TEXT];
  char host[VKR_DEPOT_DISCOVERY_TEXT];
} VkrDepotResponder;

/* Answers on `discovery_port` with `name` and `host` (NULL for this
   machine's name); false when the port is taken, as by another server on
   this machine. */
bool8_t vkr_depot_responder_open(VkrDepotResponder *responder,
                                 uint16_t discovery_port, uint16_t service_port,
                                 const uint8_t server_key[32], const char *name,
                                 const char *host);
void vkr_depot_responder_close(VkrDepotResponder *responder);

/* Answers every probe waiting on the socket; never blocks. */
void vkr_depot_responder_update(VkrDepotResponder *responder);

/* This machine's host name. */
void vkr_depot_host_name(char *out, uint32_t capacity);

/* A client's side that never blocks, for a UI thread to drive each frame:
   probe sends, update collects the answers that arrived. Every probe of
   one browser carries the same nonce, so a late answer still counts. */
typedef struct VkrDepotBrowser {
  VkrUdpSocket socket;
  uint16_t discovery_port;
  uint64_t nonce;
  /* Addresses probed one by one, beside the broadcast. */
  VkrNetAddress targets[VKR_DEPOT_BROWSE_TARGETS];
  uint32_t target_count;
  /* Distinct servers by address and key. */
  VkrDepotDiscovered found[VKR_DEPOT_BROWSE_MAX];
  uint32_t found_count;
} VkrDepotBrowser;

/* Opens a socket for probing `discovery_port` (zero for the default). */
bool8_t vkr_depot_browser_open(VkrDepotBrowser *browser,
                               uint16_t discovery_port);
void vkr_depot_browser_close(VkrDepotBrowser *browser);

/* Adds an address to probe by unicast at the browser's discovery port (its
   own port is ignored); false when the list is full. A known address is
   kept once. */
bool8_t vkr_depot_browser_add_target(VkrDepotBrowser *browser,
                                     const VkrNetAddress *address);

/* Sends a probe to every target and, with `broadcast`, to the broadcast
   address of each IPv4 subnet this machine is on. */
void vkr_depot_browser_probe(VkrDepotBrowser *browser, bool8_t broadcast);

/* Reads every answer waiting on the socket into `found` and drops servers
   silent for VKR_DEPOT_BROWSE_EXPIRY_SECONDS; never blocks. Returns how
   many servers it saw for the first time. */
uint32_t vkr_depot_browser_update(VkrDepotBrowser *browser);

/* Probes `discovery_port` on the IPv4 subnets this machine is on, or only
   on `target` when it is not NULL, and collects distinct answers for
   `timeout_ms`. Returns how many it stored, at most `capacity`. */
uint32_t vkr_depot_discover(const VkrNetAddress *target,
                            uint16_t discovery_port, uint32_t timeout_ms,
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
