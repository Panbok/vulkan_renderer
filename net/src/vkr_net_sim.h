#pragma once

#include "vkr_net_core.h"

/* A simulated network for transport tests and vkr_net_bench
 * (docs/proposals/network-protocol.md, "Verification"). Endpoints are
 * cores with addresses; each direction between two endpoints is a link
 * with latency, jitter, loss, duplication, a bandwidth limit with a
 * drop-tail router queue, and an MTU. Time is virtual and every random
 * choice comes from one seeded generator, so a run repeats exactly. */

typedef struct VkrNetSimLink {
  uint64_t latency_us;
  /* Uniform extra delay in [0, jitter_us]. A link keeps its datagrams in
     order, as a router queue does, unless `reorder` lets one overtake. */
  uint64_t jitter_us;
  /* Probabilities in [0, 1]. */
  float32_t loss;
  float32_t duplicate;
  /* A datagram ignores the order of the datagrams before it. */
  float32_t reorder;
  /* Bytes per second; zero is unlimited. */
  uint64_t bandwidth;
  /* Router queue in bytes before drop-tail; zero means one bandwidth-delay
     product or 64 KiB. */
  uint64_t queue_bytes;
  /* Largest datagram; larger ones drop. Zero means VKR_UDP_DATAGRAM_MAX. */
  uint32_t mtu;
} VkrNetSimLink;

typedef struct VkrNetSimStats {
  uint64_t datagrams_sent;
  uint64_t datagrams_delivered;
  uint64_t datagrams_dropped;
  uint64_t bytes_sent;
  uint64_t bytes_delivered;
} VkrNetSimStats;

typedef struct VkrNetSim VkrNetSim;

VkrNetSim *vkr_net_sim_create(VkrAllocator *allocator, uint64_t seed);
void vkr_net_sim_destroy(VkrNetSim *sim);

/* Adds a core at `address`; returns its endpoint index. The core stays
   owned by the caller. */
uint32_t vkr_net_sim_add(VkrNetSim *sim, VkrNetCore *core,
                         const VkrNetAddress *address);

/* Moves an endpoint to a new address, as a NAT rebinding would. */
void vkr_net_sim_set_address(VkrNetSim *sim, uint32_t endpoint,
                             const VkrNetAddress *address);

/* Sets the link from one endpoint to another. Links start unlimited with
   no delay. */
void vkr_net_sim_set_link(VkrNetSim *sim, uint32_t from, uint32_t to,
                          const VkrNetSimLink *link);

/* Runs every core's timers, takes every datagram they can send and
   delivers the datagrams due by `now_us`. */
void vkr_net_sim_step(VkrNetSim *sim, uint64_t now_us);

/* The next time anything happens: a core deadline or a delivery. */
uint64_t vkr_net_sim_next_time(const VkrNetSim *sim);

/* Statistics of the link from one endpoint to another. */
VkrNetSimStats vkr_net_sim_stats(const VkrNetSim *sim, uint32_t from,
                                 uint32_t to);

/* A deterministic value from the simulation's generator. */
uint64_t vkr_net_sim_random(VkrNetSim *sim);
