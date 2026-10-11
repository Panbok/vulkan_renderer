#include "vkr_depot_discovery.h"

#include "platform/vkr_platform.h"
#include "vkr_net_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define DISCOVERY_HOST_VARIABLE "COMPUTERNAME"
#else
#include <unistd.h>
#endif

#define DISCOVERY_VERSION 1u
#define DISCOVERY_PROBE 1u
#define DISCOVERY_ANSWER 2u
#define DISCOVERY_PROBE_SIZE 14u
#define DISCOVERY_DATAGRAM_MAX 192u
/* How often a client sends its probes again while it waits. */
#define DISCOVERY_RESEND_MS 400u

static const uint8_t discovery_magic[4] = {'V', 'K', 'D', 'P'};

// =============================================================================
// Wire form
// =============================================================================

static void discovery_put_u64(uint8_t *out, uint64_t value) {
  for (uint32_t i = 0u; i < 8u; ++i) {
    out[i] = (uint8_t)(value >> (8u * i));
  }
}

static uint64_t discovery_get_u64(const uint8_t *bytes) {
  uint64_t value = 0u;
  for (uint32_t i = 0u; i < 8u; ++i) {
    value |= (uint64_t)bytes[i] << (8u * i);
  }
  return value;
}

static uint32_t discovery_header(uint8_t kind, uint64_t nonce, uint8_t *out) {
  MemCopy(out, discovery_magic, 4u);
  out[4] = kind;
  out[5] = DISCOVERY_VERSION;
  discovery_put_u64(out + 6, nonce);
  return DISCOVERY_PROBE_SIZE;
}

static bool8_t discovery_read_header(const uint8_t *bytes, uint32_t size,
                                     uint8_t kind, uint64_t *out_nonce) {
  if (size < DISCOVERY_PROBE_SIZE ||
      MemCompare(bytes, discovery_magic, 4u) != 0 || bytes[4] != kind ||
      bytes[5] != DISCOVERY_VERSION) {
    return false_v;
  }
  *out_nonce = discovery_get_u64(bytes + 6);
  return true_v;
}

/* A length-prefixed text of at most VKR_DEPOT_DISCOVERY_TEXT - 1 bytes. */
static uint32_t discovery_put_text(uint8_t *out, const char *text) {
  const uint32_t length =
      (uint32_t)Min(strlen(text), (size_t)VKR_DEPOT_DISCOVERY_TEXT - 1u);
  out[0] = (uint8_t)length;
  MemCopy(out + 1, text, length);
  return 1u + length;
}

/* Reads a text at `*offset`, keeping printable bytes; false when it runs
   past `size` or is too long. */
static bool8_t discovery_read_text(const uint8_t *bytes, uint32_t size,
                                   uint32_t *offset, char *out) {
  if (*offset >= size) {
    return false_v;
  }
  const uint32_t length = bytes[*offset];
  if (length >= VKR_DEPOT_DISCOVERY_TEXT || *offset + 1u + length > size) {
    return false_v;
  }
  for (uint32_t i = 0u; i < length; ++i) {
    const uint8_t c = bytes[*offset + 1u + i];
    out[i] = c >= 0x20u && c < 0x7fu ? (char)c : '?';
  }
  out[length] = '\0';
  *offset += 1u + length;
  return true_v;
}

uint32_t vkr_depot_discovery_probe(uint64_t nonce, uint8_t *out) {
  return discovery_header(DISCOVERY_PROBE, nonce, out);
}

uint32_t vkr_depot_discovery_answer(const VkrDepotResponder *responder,
                                    uint64_t nonce, uint8_t *out) {
  uint32_t size = discovery_header(DISCOVERY_ANSWER, nonce, out);
  out[size++] = (uint8_t)(responder->service_port & 0xffu);
  out[size++] = (uint8_t)(responder->service_port >> 8u);
  MemCopy(out + size, responder->server_key, 32u);
  size += 32u;
  size += discovery_put_text(out + size, responder->name);
  size += discovery_put_text(out + size, responder->host);
  return size;
}

bool8_t vkr_depot_discovery_read_probe(const uint8_t *bytes, uint32_t size,
                                       uint64_t *out_nonce) {
  return size == DISCOVERY_PROBE_SIZE &&
         discovery_read_header(bytes, size, DISCOVERY_PROBE, out_nonce);
}

bool8_t vkr_depot_discovery_read_answer(const uint8_t *bytes, uint32_t size,
                                        uint64_t nonce,
                                        VkrDepotDiscovered *out) {
  uint64_t answered = 0u;
  if (!discovery_read_header(bytes, size, DISCOVERY_ANSWER, &answered) ||
      answered != nonce || size < DISCOVERY_PROBE_SIZE + 2u + 32u) {
    return false_v;
  }
  MemZero(out, sizeof(*out));
  uint32_t offset = DISCOVERY_PROBE_SIZE;
  out->address.port =
      (uint16_t)(bytes[offset] | ((uint16_t)bytes[offset + 1u] << 8u));
  offset += 2u;
  MemCopy(out->server_key, bytes + offset, 32u);
  offset += 32u;
  return out->address.port != 0u &&
         discovery_read_text(bytes, size, &offset, out->name) &&
         discovery_read_text(bytes, size, &offset, out->host) && offset == size;
}

// =============================================================================
// Server side
// =============================================================================

void vkr_depot_host_name(char *out, uint32_t capacity) {
#if defined(_WIN32)
  const char *name = getenv(DISCOVERY_HOST_VARIABLE);
  snprintf(out, capacity, "%s", name && name[0] ? name : "windows");
#else
  if (gethostname(out, capacity) != 0) {
    snprintf(out, capacity, "host");
  }
  out[capacity - 1u] = '\0';
#endif
}

bool8_t vkr_depot_responder_open(VkrDepotResponder *responder,
                                 uint16_t discovery_port, uint16_t service_port,
                                 const uint8_t server_key[32], const char *name,
                                 const char *host) {
  MemZero(responder, sizeof(*responder));
  responder->socket.handle = VKR_UDP_SOCKET_INVALID;
  responder->service_port = service_port;
  MemCopy(responder->server_key, server_key, 32u);
  snprintf(responder->name, sizeof(responder->name), "%s", name ? name : "");
  if (host) {
    snprintf(responder->host, sizeof(responder->host), "%s", host);
  } else {
    vkr_depot_host_name(responder->host, sizeof(responder->host));
  }
  const VkrNetAddress bind =
      vkr_net_address_any(VKR_NET_ADDRESS_IPV4, discovery_port);
  return vkr_udp_socket_open(&bind, NULL, &responder->socket);
}

void vkr_depot_responder_close(VkrDepotResponder *responder) {
  vkr_udp_socket_close(&responder->socket);
}

void vkr_depot_responder_update(VkrDepotResponder *responder) {
  if (responder->socket.handle == VKR_UDP_SOCKET_INVALID) {
    return;
  }
  for (uint32_t round = 0u; round < 16u; ++round) {
    uint8_t bytes[DISCOVERY_DATAGRAM_MAX];
    VkrUdpDatagram datagram = {.data = bytes, .capacity = sizeof(bytes)};
    uint32_t received = 0u;
    if (vkr_udp_socket_receive(responder->socket, &datagram, 1u, &received,
                               NULL) != VKR_UDP_OK ||
        received == 0u) {
      return;
    }
    uint64_t nonce = 0u;
    if (!vkr_depot_discovery_read_probe(bytes, datagram.size, &nonce)) {
      continue;
    }
    uint8_t answer[DISCOVERY_DATAGRAM_MAX];
    const VkrUdpDatagram reply = {
        .address = datagram.address,
        .data = answer,
        .size = vkr_depot_discovery_answer(responder, nonce, answer)};
    uint32_t sent = 0u;
    (void)vkr_udp_socket_send(responder->socket, &reply, 1u, &sent);
  }
}

// =============================================================================
// Client side
// =============================================================================

bool8_t vkr_depot_browser_open(VkrDepotBrowser *browser,
                               uint16_t discovery_port) {
  MemZero(browser, sizeof(*browser));
  browser->socket.handle = VKR_UDP_SOCKET_INVALID;
  browser->discovery_port =
      discovery_port ? discovery_port : (uint16_t)VKR_DEPOT_DISCOVERY_PORT;
  vkr_net_random(&browser->nonce, sizeof(browser->nonce));
  const VkrNetAddress bind = vkr_net_address_any(VKR_NET_ADDRESS_IPV4, 0u);
  if (!vkr_udp_socket_open(&bind, NULL, &browser->socket)) {
    return false_v;
  }
  (void)vkr_udp_socket_enable_broadcast(browser->socket);
  return true_v;
}

void vkr_depot_browser_close(VkrDepotBrowser *browser) {
  vkr_udp_socket_close(&browser->socket);
}

bool8_t vkr_depot_browser_add_target(VkrDepotBrowser *browser,
                                     const VkrNetAddress *address) {
  VkrNetAddress target = *address;
  target.port = browser->discovery_port;
  for (uint32_t i = 0u; i < browser->target_count; ++i) {
    if (vkr_net_address_equal(&browser->targets[i], &target)) {
      return true_v;
    }
  }
  if (browser->target_count == VKR_DEPOT_BROWSE_TARGETS) {
    return false_v;
  }
  browser->targets[browser->target_count++] = target;
  return true_v;
}

static void discovery_send_probes(VkrUdpSocket socket, const uint8_t *probe,
                                  const VkrNetAddress *targets,
                                  uint32_t target_count) {
  for (uint32_t i = 0u; i < target_count; ++i) {
    const VkrUdpDatagram datagram = {.address = targets[i],
                                     .data = (uint8_t *)probe,
                                     .size = DISCOVERY_PROBE_SIZE};
    uint32_t sent = 0u;
    (void)vkr_udp_socket_send(socket, &datagram, 1u, &sent);
  }
}

void vkr_depot_browser_probe(VkrDepotBrowser *browser, bool8_t broadcast) {
  if (browser->socket.handle == VKR_UDP_SOCKET_INVALID) {
    return;
  }
  uint8_t probe[DISCOVERY_PROBE_SIZE];
  (void)vkr_depot_discovery_probe(browser->nonce, probe);
  discovery_send_probes(browser->socket, probe, browser->targets,
                        browser->target_count);
  if (!broadcast) {
    return;
  }
  /* Every subnet this machine is on and the limited broadcast address. */
  VkrNetAddress targets[17];
  uint32_t count = vkr_net_ipv4_broadcast_addresses(
      browser->discovery_port, targets, ArrayCount(targets) - 1u);
  VkrNetAddress limited = {.family = VKR_NET_ADDRESS_IPV4,
                           .port = browser->discovery_port};
  MemSet(limited.bytes, 0xff, 4u);
  targets[count++] = limited;
  discovery_send_probes(browser->socket, probe, targets, count);
}

uint32_t vkr_depot_browser_update(VkrDepotBrowser *browser) {
  if (browser->socket.handle == VKR_UDP_SOCKET_INVALID) {
    return 0u;
  }
  uint32_t added = 0u;
  for (;;) {
    uint8_t bytes[DISCOVERY_DATAGRAM_MAX];
    VkrUdpDatagram datagram = {.data = bytes, .capacity = sizeof(bytes)};
    uint32_t received = 0u;
    if (vkr_udp_socket_receive(browser->socket, &datagram, 1u, &received,
                               NULL) != VKR_UDP_OK ||
        received == 0u) {
      break;
    }
    VkrDepotDiscovered found;
    if (!vkr_depot_discovery_read_answer(bytes, datagram.size, browser->nonce,
                                         &found)) {
      continue;
    }
    const uint16_t port = found.address.port;
    found.address = datagram.address;
    found.address.port = port;
    found.last_seen = vkr_platform_get_absolute_time();
    VkrDepotDiscovered *known = NULL;
    for (uint32_t i = 0u; i < browser->found_count && !known; ++i) {
      VkrDepotDiscovered *entry = &browser->found[i];
      if (vkr_net_address_equal(&entry->address, &found.address) &&
          MemCompare(entry->server_key, found.server_key, 32u) == 0) {
        known = entry;
      }
    }
    if (known) {
      *known = found;
    } else if (browser->found_count < VKR_DEPOT_BROWSE_MAX) {
      browser->found[browser->found_count++] = found;
      added += 1u;
    }
  }
  return added;
}

uint32_t vkr_depot_discover(const VkrNetAddress *target,
                            uint16_t discovery_port, uint32_t timeout_ms,
                            VkrDepotDiscovered *out, uint32_t capacity) {
  VkrDepotBrowser browser;
  if (!vkr_depot_browser_open(&browser, discovery_port) ||
      (target && !vkr_depot_browser_add_target(&browser, target))) {
    vkr_depot_browser_close(&browser);
    return 0u;
  }
  const float64_t start = vkr_platform_get_absolute_time();
  float64_t resend_at = start;
  for (;;) {
    const float64_t now = vkr_platform_get_absolute_time();
    const float64_t elapsed_ms = (now - start) * 1000.0;
    if (elapsed_ms >= (float64_t)timeout_ms) {
      break;
    }
    if (now >= resend_at) {
      vkr_depot_browser_probe(&browser, target == NULL);
      resend_at = now + DISCOVERY_RESEND_MS / 1000.0;
    }
    const int32_t wait = (int32_t)Min((float64_t)DISCOVERY_RESEND_MS,
                                      (float64_t)timeout_ms - elapsed_ms) +
                         1;
    if (vkr_udp_socket_wait(browser.socket, wait) > 0) {
      (void)vkr_depot_browser_update(&browser);
    }
  }
  vkr_depot_browser_close(&browser);
  const uint32_t count = Min(browser.found_count, capacity);
  MemCopy(out, browser.found, (uint64_t)count * sizeof(*out));
  return count;
}
