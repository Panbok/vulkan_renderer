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
 *   version 2 adds, after the host:
 *           flags u8 (bit 0: outbound only), repository count u8, and per
 *           repository: id[16], name length u8, name, default branch
 *           length u8, default branch
 *
 * A responder answers each probe in the probe's version, so a version 1
 * client, which reads no other version and no trailing bytes, still finds
 * it. A browser sends a probe of each version and keeps the version 2
 * answer of a server that sends both.
 *
 * An answer is not authenticated: it says where a server claims to be and
 * which key it claims. The transport then proves the key, so a client that
 * checks the key once against the one the server prints is safe.
 *
 * Some endpoint security drops answers to broadcast probes but passes
 * answers to probes sent to one address (seen on a managed Mac), so a
 * browser also probes the servers a user connected to before. */

#define VKR_DEPOT_DISCOVERY_PORT 7319u
/* The newest probe and answer version. */
#define VKR_DEPOT_DISCOVERY_VERSION 2u
/* A server's name or host name with its terminator. */
#define VKR_DEPOT_DISCOVERY_TEXT 64u
/* Repositories one answer lists at most. */
#define VKR_DEPOT_DISCOVERY_REPOSITORIES 8u
/* The largest probe or answer: a version 2 answer with every text at its
   longest fits one Ethernet frame. */
#define VKR_DEPOT_DISCOVERY_DATAGRAM_MAX 1400u
#define VKR_DEPOT_REPOSITORY_ID_SIZE 16u

#define VKR_DEPOT_BROWSE_TARGETS 16u
#define VKR_DEPOT_BROWSE_MAX 32u
/* A server a browser has not heard from for this long leaves `found`. */
#define VKR_DEPOT_BROWSE_EXPIRY_SECONDS 30.0

/* A repository a service serves, as discovery lists it. */
typedef struct VkrDepotDiscoveredRepository {
  uint8_t id[VKR_DEPOT_REPOSITORY_ID_SIZE];
  char name[VKR_DEPOT_DISCOVERY_TEXT];
  char default_branch[VKR_DEPOT_DISCOVERY_TEXT];
} VkrDepotDiscoveredRepository;

typedef struct VkrDepotDiscovered {
  /* The answering address with the service port. */
  VkrNetAddress address;
  uint8_t server_key[32];
  char name[VKR_DEPOT_DISCOVERY_TEXT];
  char host[VKR_DEPOT_DISCOVERY_TEXT];
  /* Platform seconds (vkr_platform_get_absolute_time) of the latest
     answer. */
  float64_t last_seen;
  /* The answer's version: 1 from a server that predates repositories, which
     lists none and never says it is outbound only. */
  uint8_t version;
  bool8_t outbound_only;
  VkrDepotDiscoveredRepository repositories[VKR_DEPOT_DISCOVERY_REPOSITORIES];
  uint32_t repository_count;
} VkrDepotDiscovered;

/* A server's side: a socket on a discovery port that answers with the
   server's port, key, name and host, and in version 2 its repositories. */
typedef struct VkrDepotResponder {
  VkrUdpSocket socket;
  uint16_t service_port;
  uint8_t server_key[32];
  char name[VKR_DEPOT_DISCOVERY_TEXT];
  char host[VKR_DEPOT_DISCOVERY_TEXT];
  bool8_t outbound_only;
  VkrDepotDiscoveredRepository repositories[VKR_DEPOT_DISCOVERY_REPOSITORIES];
  uint32_t repository_count;
} VkrDepotResponder;

/* Answers on `discovery_port` with `name` and `host` (NULL for this
   machine's name); false when the port is taken, as by another server on
   this machine. */
bool8_t vkr_depot_responder_open(VkrDepotResponder *responder,
                                 uint16_t discovery_port, uint16_t service_port,
                                 const uint8_t server_key[32], const char *name,
                                 const char *host);
void vkr_depot_responder_close(VkrDepotResponder *responder);

/* Sets what version 2 answers list: the first
   VKR_DEPOT_DISCOVERY_REPOSITORIES of `repositories` and the outbound-only
   flag. */
void vkr_depot_responder_set_repositories(
    VkrDepotResponder *responder,
    const VkrDepotDiscoveredRepository *repositories, uint32_t count,
    bool8_t outbound_only);

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

/* The wire form, for tests: a probe or an answer of `version` (1 or 2) into
   `out` (at least VKR_DEPOT_DISCOVERY_DATAGRAM_MAX bytes), returning its
   size. */
uint32_t vkr_depot_discovery_probe(uint8_t version, uint64_t nonce,
                                   uint8_t *out);
uint32_t vkr_depot_discovery_answer(const VkrDepotResponder *responder,
                                    uint8_t version, uint64_t nonce,
                                    uint8_t *out);

/* Reads a probe's nonce and version; false for anything else. */
bool8_t vkr_depot_discovery_read_probe(const uint8_t *bytes, uint32_t size,
                                       uint64_t *out_nonce,
                                       uint8_t *out_version);

/* Reads an answer of either version to the probe `nonce`; false for
   anything else. The address and `last_seen` are left for the caller. */
bool8_t vkr_depot_discovery_read_answer(const uint8_t *bytes, uint32_t size,
                                        uint64_t nonce,
                                        VkrDepotDiscovered *out);
