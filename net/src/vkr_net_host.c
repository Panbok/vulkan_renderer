#include "vkr_net_host.h"

#include "platform/vkr_platform.h"

/* Datagrams moved per system-call batch. */
#define NET_HOST_BATCH 32u
/* Datagrams one pump receives before it runs timers and sends. */
#define NET_HOST_RECEIVE_BUDGET 1024u
/* Below this, a pump spins instead of sleeping: platform waits are in
   milliseconds and pacing releases come sooner. */
#define NET_HOST_SPIN_US 1000u

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
  const uint64_t now = vkr_net_host_now(host);
  const uint64_t deadline = vkr_net_core_next_deadline(host->core);
  int64_t wait_us = timeout_ms < 0 ? INT64_MAX : (int64_t)timeout_ms * 1000;
  if (deadline != UINT64_MAX) {
    wait_us = deadline > now ? Min(wait_us, (int64_t)(deadline - now)) : 0;
  } else if (timeout_ms < 0) {
    wait_us = 1000000;
  }
  if (wait_us >= (int64_t)NET_HOST_SPIN_US) {
    const int32_t wait_ms = (int32_t)Min(wait_us / 1000, (int64_t)INT32_MAX);
    if (vkr_udp_socket_wait(host->socket, wait_ms) < 0) {
      return false_v;
    }
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
