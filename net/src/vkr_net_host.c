#include "vkr_net_host.h"

#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "platform/vkr_platform.h"

/* Datagrams moved per system-call batch. */
#define NET_HOST_BATCH 32u
/* Datagrams one pump receives before it runs timers and sends. */
#define NET_HOST_RECEIVE_BUDGET 1024u
/* The longest wait of a pump with no deadline and no limit. */
#define NET_HOST_IDLE_WAIT_US 1000000
/* The longest wait of the host thread, so its step runs at least this
   often while nothing arrives. */
#define NET_HOST_THREAD_WAIT_US 100000
/* Yields the host thread offers an application thread that waits for the
   lock before it takes the lock back. */
#define NET_HOST_THREAD_YIELDS 64u

struct VkrNetHost {
  VkrAllocator *allocator;
  VkrNetCore *core;
  VkrUdpSocket socket;
  VkrNetAddress local;
  uint32_t datagram_size;
  uint8_t *rx;
  uint8_t *tx;
  VkrUdpDatagram rx_batch[NET_HOST_BATCH];
  VkrUdpDatagram tx_batch[NET_HOST_BATCH];
  VkrNetHostStats stats;
};

uint64_t vkr_net_host_now(const VkrNetHost *host) {
  (void)host;
  return (uint64_t)(vkr_platform_get_absolute_time() * 1000000.0);
}

VkrNetHost *vkr_net_host_create(VkrAllocator *allocator,
                                const VkrNetHostConfig *config) {
  if (!allocator || !config) {
    return NULL;
  }
  VkrNetHost *host = vkr_allocator_alloc(allocator, sizeof(VkrNetHost),
                                         VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!host) {
    return NULL;
  }
  MemZero(host, sizeof(*host));
  host->allocator = allocator;
  host->socket.handle = VKR_UDP_SOCKET_INVALID;
  host->datagram_size = config->core.max_datagram ? config->core.max_datagram
                                                  : VKR_NET_DATAGRAM_ETHERNET;
  host->datagram_size = Max(host->datagram_size, VKR_NET_DATAGRAM_INITIAL);

  const uint64_t buffer_size = (uint64_t)host->datagram_size * NET_HOST_BATCH;
  host->rx = vkr_allocator_alloc(allocator, buffer_size,
                                 VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  host->tx = vkr_allocator_alloc(allocator, buffer_size,
                                 VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  host->core = vkr_net_core_create(allocator, &config->core);
  if (!host->rx || !host->tx || !host->core ||
      !vkr_udp_socket_open(&config->bind, &config->socket, &host->socket) ||
      !vkr_udp_socket_local_address(host->socket, &host->local)) {
    vkr_net_host_destroy(host);
    return NULL;
  }
  for (uint32_t i = 0u; i < NET_HOST_BATCH; ++i) {
    host->rx_batch[i] =
        (VkrUdpDatagram){.data = host->rx + (uint64_t)i * host->datagram_size,
                         .capacity = host->datagram_size};
    host->tx_batch[i] =
        (VkrUdpDatagram){.data = host->tx + (uint64_t)i * host->datagram_size,
                         .capacity = host->datagram_size};
  }
  return host;
}

void vkr_net_host_destroy(VkrNetHost *host) {
  if (!host) {
    return;
  }
  vkr_udp_socket_close(&host->socket);
  vkr_net_core_destroy(host->core);
  const uint64_t buffer_size = (uint64_t)host->datagram_size * NET_HOST_BATCH;
  if (host->rx) {
    vkr_allocator_free(host->allocator, host->rx, buffer_size,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
  if (host->tx) {
    vkr_allocator_free(host->allocator, host->tx, buffer_size,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
  vkr_allocator_free(host->allocator, host, sizeof(VkrNetHost),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

VkrNetCore *vkr_net_host_core(VkrNetHost *host) {
  return host ? host->core : NULL;
}

bool8_t vkr_net_host_local_address(const VkrNetHost *host,
                                   VkrNetAddress *out_address) {
  if (!host || !out_address) {
    return false_v;
  }
  *out_address = host->local;
  return true_v;
}

static bool8_t net_host_receive(VkrNetHost *host) {
  uint32_t total = 0u;
  while (total < NET_HOST_RECEIVE_BUDGET) {
    uint32_t received = 0u;
    uint32_t truncated = 0u;
    const VkrUdpStatus status = vkr_udp_socket_receive(
        host->socket, host->rx_batch, NET_HOST_BATCH, &received, &truncated);
    host->stats.truncated += truncated;
    if (status == VKR_UDP_ERROR) {
      return false_v;
    }
    const uint64_t now = vkr_net_host_now(host);
    for (uint32_t i = 0u; i < received; ++i) {
      VkrUdpDatagram *datagram = &host->rx_batch[i];
      vkr_net_core_receive(host->core, &datagram->address, datagram->data,
                           datagram->size, now);
    }
    host->stats.datagrams_received += received;
    total += received;
    if (received < NET_HOST_BATCH) {
      break;
    }
  }
  return true_v;
}

static bool8_t net_host_send(VkrNetHost *host) {
  for (;;) {
    const uint64_t now = vkr_net_host_now(host);
    uint32_t count = 0u;
    while (count < NET_HOST_BATCH) {
      VkrUdpDatagram *datagram = &host->tx_batch[count];
      datagram->size =
          vkr_net_core_transmit(host->core, now, datagram->data,
                                datagram->capacity, &datagram->address);
      if (datagram->size == 0u) {
        break;
      }
      ++count;
    }
    if (count == 0u) {
      return true_v;
    }
    uint32_t sent = 0u;
    const VkrUdpStatus status =
        vkr_udp_socket_send(host->socket, host->tx_batch, count, &sent);
    host->stats.datagrams_sent += sent;
    if (status == VKR_UDP_ERROR) {
      return false_v;
    }
    /* A full socket buffer drops the rest; loss recovery resends them. */
    host->stats.send_dropped += count - sent;
    if (count < NET_HOST_BATCH) {
      return true_v;
    }
  }
}

static bool8_t net_host_process(VkrNetHost *host) {
  if (!net_host_receive(host)) {
    return false_v;
  }
  vkr_net_core_update(host->core, vkr_net_host_now(host));
  return net_host_send(host);
}

/* Microseconds until the core's next deadline, at most `limit_us`. A
   negative limit waits for the deadline, or a second without one. */
static int64_t net_host_wait_time(const VkrNetHost *host, int64_t limit_us) {
  const uint64_t now = vkr_net_host_now(host);
  const uint64_t deadline = vkr_net_core_next_deadline(host->core);
  int64_t wait_us = limit_us < 0 ? INT64_MAX : limit_us;
  if (deadline != UINT64_MAX) {
    wait_us = deadline > now ? Min(wait_us, (int64_t)(deadline - now)) : 0;
  } else if (limit_us < 0) {
    wait_us = NET_HOST_IDLE_WAIT_US;
  }
  return wait_us;
}

bool8_t vkr_net_host_pump(VkrNetHost *host, int32_t timeout_ms) {
  if (!host) {
    return false_v;
  }
  if (!net_host_process(host)) {
    return false_v;
  }
  if (timeout_ms == 0) {
    return true_v;
  }
  /* A deadline closer than a millisecond waits only that long where the
     socket can (POSIX; Windows rounds it up to one, see
     vkr_udp_socket_wait_us), so sub-millisecond pacing and loss timers on
     loopback neither spin the pump nor sleep a whole millisecond. */
  const int64_t wait_us = net_host_wait_time(
      host, timeout_ms < 0 ? -1 : (int64_t)timeout_ms * 1000);
  if (wait_us > 0 && vkr_udp_socket_wait_us(host->socket, wait_us) < 0) {
    return false_v;
  }
  return net_host_process(host);
}

void vkr_net_host_wake(VkrNetHost *host) {
  if (!host) {
    return;
  }
  /* A zero-length datagram to this socket ends a wait; the core ignores it.
     A wildcard socket receives it through loopback. */
  VkrNetAddress target = host->local;
  const VkrNetAddress wildcard =
      vkr_net_address_any((VkrNetAddressFamily)target.family, target.port);
  if (vkr_net_address_equal(&target, &wildcard)) {
    target = vkr_net_address_loopback((VkrNetAddressFamily)target.family,
                                      target.port);
  }
  uint8_t empty = 0u;
  const VkrUdpDatagram datagram = {
      .address = target, .data = &empty, .capacity = 0u, .size = 0u};
  (void)vkr_udp_socket_send(host->socket, &datagram, 1u, NULL);
}

VkrNetHostStats vkr_net_host_stats(const VkrNetHost *host) {
  return host ? host->stats : (VkrNetHostStats){0};
}

// =============================================================================
// Host thread
// =============================================================================

struct VkrNetHostThread {
  VkrAllocator *allocator;
  VkrNetHost *host;
  VkrNetHostStep step;
  void *context;
  VkrMutex mutex;
  VkrThread thread;
  /* Application threads inside vkr_net_host_thread_lock. */
  VkrAtomicUint32 contenders;
  /* The fields below are guarded by `mutex`. */
  bool8_t stopping;
  /* The thread waits, or is about to, on the socket. */
  bool8_t waiting;
  /* A wake datagram is on its way for this wait. */
  bool8_t woken;
  bool8_t failed;
};

/* The lock is free; let an application thread that waits for it take it
   before the host thread does. A mutex need not be fair, and a busy host
   thread that relocks at once would starve the application. */
static void net_host_thread_yield(VkrNetHostThread *thread) {
  for (uint32_t i = 0u; i < NET_HOST_THREAD_YIELDS; ++i) {
    if (vkr_atomic_uint32_load(&thread->contenders, VKR_MEMORY_ORDER_ACQUIRE) ==
        0u) {
      return;
    }
    vkr_thread_yield();
  }
}

static void *net_host_thread_main(void *argument) {
  VkrNetHostThread *thread = argument;
  VkrNetHost *host = thread->host;
  vkr_mutex_lock(thread->mutex);
  while (!thread->stopping) {
    if (!net_host_process(host)) {
      thread->failed = true_v;
      break;
    }
    if (thread->step) {
      thread->step(thread->context, vkr_net_host_now(host));
      /* What the step queued leaves now, not after the wait. */
      if (!net_host_send(host)) {
        thread->failed = true_v;
        break;
      }
    }
    const int64_t wait_us = net_host_wait_time(host, NET_HOST_THREAD_WAIT_US);
    thread->waiting = true_v;
    thread->woken = false_v;
    vkr_mutex_unlock(thread->mutex);

    /* The socket handle never changes while the thread runs, so the wait
       needs no lock. A datagram, the deadline or a wake ends it. */
    int32_t result = 0;
    if (wait_us > 0) {
      result = vkr_udp_socket_wait_us(host->socket, wait_us);
    }
    net_host_thread_yield(thread);

    vkr_mutex_lock(thread->mutex);
    thread->waiting = false_v;
    if (result < 0) {
      thread->failed = true_v;
      break;
    }
  }
  thread->waiting = false_v;
  vkr_mutex_unlock(thread->mutex);
  return NULL;
}

VkrNetHostThread *vkr_net_host_thread_start(VkrAllocator *allocator,
                                            VkrNetHost *host,
                                            VkrNetHostStep step,
                                            void *context) {
  if (!allocator || !host) {
    return NULL;
  }
  VkrNetHostThread *thread = vkr_allocator_alloc(
      allocator, sizeof(VkrNetHostThread), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!thread) {
    return NULL;
  }
  MemZero(thread, sizeof(*thread));
  thread->allocator = allocator;
  thread->host = host;
  thread->step = step;
  thread->context = context;
  vkr_atomic_uint32_store(&thread->contenders, 0u, VKR_MEMORY_ORDER_RELAXED);
  if (!vkr_mutex_create(allocator, &thread->mutex)) {
    goto cleanup;
  }
  if (!vkr_thread_create(allocator, &thread->thread, net_host_thread_main,
                         thread)) {
    goto cleanup;
  }
  return thread;

cleanup:
  if (thread->mutex) {
    vkr_mutex_destroy(allocator, &thread->mutex);
  }
  vkr_allocator_free(allocator, thread, sizeof(VkrNetHostThread),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  return NULL;
}

void vkr_net_host_thread_lock(VkrNetHostThread *thread) {
  if (!thread) {
    return;
  }
  vkr_atomic_uint32_fetch_add(&thread->contenders, 1u,
                              VKR_MEMORY_ORDER_ACQ_REL);
  vkr_mutex_lock(thread->mutex);
  vkr_atomic_uint32_fetch_sub(&thread->contenders, 1u,
                              VKR_MEMORY_ORDER_ACQ_REL);
}

void vkr_net_host_thread_unlock(VkrNetHostThread *thread) {
  if (!thread) {
    return;
  }
  /* One wake per wait: later unlocks find the datagram already sent. */
  const bool8_t wake = thread->waiting && !thread->woken;
  if (wake) {
    thread->woken = true_v;
  }
  vkr_mutex_unlock(thread->mutex);
  if (wake) {
    vkr_net_host_wake(thread->host);
  }
}

bool8_t vkr_net_host_thread_failed(const VkrNetHostThread *thread) {
  return thread ? thread->failed : true_v;
}

void vkr_net_host_thread_stop(VkrNetHostThread *thread) {
  if (!thread) {
    return;
  }
  vkr_net_host_thread_lock(thread);
  thread->stopping = true_v;
  vkr_net_host_thread_unlock(thread);
  vkr_thread_join(thread->thread);
  vkr_thread_destroy(thread->allocator, &thread->thread);
  vkr_mutex_destroy(thread->allocator, &thread->mutex);
  vkr_allocator_free(thread->allocator, thread, sizeof(VkrNetHostThread),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}
