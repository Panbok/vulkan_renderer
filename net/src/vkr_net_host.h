#pragma once

#include "vkr_net_core.h"

/* A transport core on a real UDP socket (docs/proposals/network-protocol.md,
 * "Threads"). The owner pumps it from one thread: each pump receives a batch
 * of datagrams, runs the core's timers, sends what the core builds and, when
 * asked, waits for a datagram or the core's next deadline. Times come from
 * the platform's monotonic clock in microseconds.
 *
 * vkr_platform_init must run before create. */

typedef struct VkrNetHostConfig {
  VkrNetCoreConfig core;
  /* Address to bind; port zero picks one. */
  VkrNetAddress bind;
  VkrUdpSocketConfig socket;
} VkrNetHostConfig;

typedef struct VkrNetHostStats {
  uint64_t datagrams_received;
  uint64_t datagrams_sent;
  /* Sends the socket refused because its buffer was full; the transport
     treats them as lost. */
  uint64_t send_dropped;
  uint64_t truncated;
} VkrNetHostStats;

typedef struct VkrNetHost VkrNetHost;

VkrNetHost *vkr_net_host_create(VkrAllocator *allocator,
                                const VkrNetHostConfig *config);
void vkr_net_host_destroy(VkrNetHost *host);

VkrNetCore *vkr_net_host_core(VkrNetHost *host);

/* Microseconds of the monotonic clock the host passes to its core. */
uint64_t vkr_net_host_now(const VkrNetHost *host);

bool8_t vkr_net_host_local_address(const VkrNetHost *host,
                                   VkrNetAddress *out_address);

/* Receives, runs timers and sends. With `timeout_ms` other than zero it then
   waits up to that long (negative: until the next deadline) for a datagram
   and processes again. False when the socket failed. */
bool8_t vkr_net_host_pump(VkrNetHost *host, int32_t timeout_ms);

/* Wakes a pump waiting on another thread. */
void vkr_net_host_wake(VkrNetHost *host);

VkrNetHostStats vkr_net_host_stats(const VkrNetHost *host);
