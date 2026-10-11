#pragma once

#include "vkr_net_core.h"

/* A transport core on a real UDP socket (docs/proposals/network-protocol.md,
 * "Threads"). The owner pumps it from one thread: each pump receives a batch
 * of datagrams, runs the core's timers, sends what the core builds and, when
 * asked, waits for a datagram or the core's next deadline. Times come from
 * the platform's monotonic clock in microseconds.
 *
 * Instead, a host thread (vkr_net_host_thread_start) can pump it. The
 * thread holds a lock while it touches the core and releases it while it
 * waits on the socket; any other thread takes the lock first.
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

/* With a host thread running, read the statistics with its lock held. */
VkrNetHostStats vkr_net_host_stats(const VkrNetHost *host);

/* Called by the host thread after every pump, with the lock held, at
   `now_us` of the host's clock. It polls core events and runs the service
   the thread drives; it must not take the lock itself. */
typedef void (*VkrNetHostStep)(void *context, uint64_t now_us);

typedef struct VkrNetHostThread VkrNetHostThread;

/* Pumps `host` on a new thread. After every pump the thread calls `step`
   (may be NULL) with the host lock held; `step` polls core events and runs
   the service. The thread waits without the lock until the core's next
   deadline, a datagram, a wake or 100 ms, so the step runs at least ten
   times a second. The caller keeps owning `host` and destroys it after
   stopping the thread. NULL on failure. */
VkrNetHostThread *vkr_net_host_thread_start(VkrAllocator *allocator,
                                            VkrNetHost *host,
                                            VkrNetHostStep step, void *context);

/* Any other thread takes the lock before touching the host's core, session
   or the service the step drives. */
void vkr_net_host_thread_lock(VkrNetHostThread *thread);

/* Releases the lock and wakes the thread so queued sends leave at once. */
void vkr_net_host_thread_unlock(VkrNetHostThread *thread);

/* True once the socket failed; the thread then stops pumping and calls
   the step no more. Call with the lock held. */
bool8_t vkr_net_host_thread_failed(const VkrNetHostThread *thread);

/* Stops and joins the thread; returns with no lock held. Call it without
   the lock. */
void vkr_net_host_thread_stop(VkrNetHostThread *thread);
