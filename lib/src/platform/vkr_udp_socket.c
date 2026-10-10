#if defined(_WIN32)
/* Winsock 2 must precede windows.h (vkr_local_socket_windows.c). */
// clang-format off
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "platform/vkr_udp_socket.h"

#if defined(_WIN32)
#include "platform/vkr_winsock.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* Older SDKs declare these in mstcpip.h only. */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef SIO_UDP_NETRESET
#define SIO_UDP_NETRESET _WSAIOW(IOC_VENDOR, 15)
#endif
typedef SOCKET UdpNative;
typedef int UdpLength;
#define UDP_NATIVE_INVALID INVALID_SOCKET
#else
typedef int UdpNative;
typedef socklen_t UdpLength;
#define UDP_NATIVE_INVALID (-1)
#endif

static const uint8_t udp_v4_mapped_prefix[12] = {0, 0, 0, 0, 0,    0,
                                                 0, 0, 0, 0, 0xff, 0xff};

static bool8_t udp_ready(void) {
#if defined(_WIN32)
  return vkr_winsock_ready();
#else
  return true_v;
#endif
}

static UdpNative udp_native(VkrUdpSocket socket) {
  return (UdpNative)socket.handle;
}

static int32_t udp_last_error(void) {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

static bool8_t udp_error_would_block(int32_t error) {
#if defined(_WIN32)
  return error == WSAEWOULDBLOCK;
#else
  return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

static bool8_t udp_error_interrupted(int32_t error) {
#if defined(_WIN32)
  return error == WSAEINTR;
#else
  return error == EINTR;
#endif
}

static bool8_t udp_error_too_large(int32_t error) {
#if defined(_WIN32)
  return error == WSAEMSGSIZE;
#else
  return error == EMSGSIZE;
#endif
}

/* ICMP reports from an earlier send that a host returns on a later call of
   an unconnected socket; the transport detects loss itself. */
static bool8_t udp_error_transient(int32_t error) {
#if defined(_WIN32)
  return error == WSAECONNRESET || error == WSAENETRESET ||
         error == WSAEHOSTUNREACH || error == WSAENETUNREACH;
#else
  return error == ECONNREFUSED || error == EHOSTUNREACH ||
         error == ENETUNREACH || error == EHOSTDOWN;
#endif
}

static UdpLength udp_to_sockaddr(const VkrNetAddress *address,
                                 uint8_t socket_family,
                                 struct sockaddr_storage *out) {
  MemZero(out, sizeof(*out));
  if (socket_family == VKR_NET_ADDRESS_IPV6) {
    struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)out;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(address->port);
    if (address->family == VKR_NET_ADDRESS_IPV4) {
      MemCopy(&in6->sin6_addr, udp_v4_mapped_prefix, 12);
      MemCopy((uint8_t *)&in6->sin6_addr + 12, address->bytes, 4);
    } else {
      MemCopy(&in6->sin6_addr, address->bytes, 16);
      in6->sin6_scope_id = address->scope;
    }
    return (UdpLength)sizeof(*in6);
  }
  struct sockaddr_in *in4 = (struct sockaddr_in *)out;
  in4->sin_family = AF_INET;
  in4->sin_port = htons(address->port);
  MemCopy(&in4->sin_addr, address->bytes, 4);
  return (UdpLength)sizeof(*in4);
}

static bool8_t udp_from_sockaddr(const struct sockaddr *address,
                                 VkrNetAddress *out) {
  MemZero(out, sizeof(*out));
  if (address->sa_family == AF_INET) {
    const struct sockaddr_in *in4 = (const struct sockaddr_in *)address;
    out->family = VKR_NET_ADDRESS_IPV4;
    out->port = ntohs(in4->sin_port);
    MemCopy(out->bytes, &in4->sin_addr, 4);
    return true_v;
  }
  if (address->sa_family == AF_INET6) {
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)address;
    const uint8_t *bytes = (const uint8_t *)&in6->sin6_addr;
    out->port = ntohs(in6->sin6_port);
    if (memcmp(bytes, udp_v4_mapped_prefix, 12) == 0) {
      out->family = VKR_NET_ADDRESS_IPV4;
      MemCopy(out->bytes, bytes + 12, 4);
    } else {
      out->family = VKR_NET_ADDRESS_IPV6;
      MemCopy(out->bytes, bytes, 16);
      out->scope = in6->sin6_scope_id;
    }
    return true_v;
  }
  return false_v;
}

bool8_t vkr_net_address_equal(const VkrNetAddress *a, const VkrNetAddress *b) {
  return a->family == b->family && a->port == b->port && a->scope == b->scope &&
         memcmp(a->bytes, b->bytes, 16) == 0;
}

VkrNetAddress vkr_net_address_any(VkrNetAddressFamily family, uint16_t port) {
  return (VkrNetAddress){.family = (uint8_t)family, .port = port};
}

VkrNetAddress vkr_net_address_loopback(VkrNetAddressFamily family,
                                       uint16_t port) {
  VkrNetAddress address = {.family = (uint8_t)family, .port = port};
  if (family == VKR_NET_ADDRESS_IPV4) {
    address.bytes[0] = 127u;
    address.bytes[3] = 1u;
  } else {
    address.bytes[15] = 1u;
  }
  return address;
}

/* Parses a decimal value of at most `max` from `text[0..length)`. */
static bool8_t udp_parse_decimal(const char *text, uint64_t length,
                                 uint32_t max, uint32_t *out_value) {
  if (length == 0u || length > 10u) {
    return false_v;
  }
  uint64_t value = 0u;
  for (uint64_t i = 0u; i < length; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return false_v;
    }
    value = value * 10u + (uint64_t)(text[i] - '0');
  }
  if (value > max) {
    return false_v;
  }
  *out_value = (uint32_t)value;
  return true_v;
}

bool8_t vkr_net_address_parse(const char *text, uint16_t default_port,
                              VkrNetAddress *out_address) {
  if (!text || !out_address || !udp_ready()) {
    return false_v;
  }
  const uint64_t length = strlen(text);
  char host[VKR_NET_ADDRESS_TEXT];
  uint32_t port = default_port;
  uint32_t scope = 0u;
  uint64_t host_length = 0u;

  if (length > 0u && text[0] == '[') {
    /* "[v6]" or "[v6]:port", with an optional "%zone" inside. */
    const char *close = strchr(text, ']');
    if (!close) {
      return false_v;
    }
    host_length = (uint64_t)(close - text - 1);
    if (host_length == 0u || host_length >= sizeof(host)) {
      return false_v;
    }
    MemCopy(host, text + 1, host_length);
    host[host_length] = '\0';
    const char *rest = close + 1;
    if (*rest == ':') {
      if (!udp_parse_decimal(rest + 1, strlen(rest + 1), 65535u, &port)) {
        return false_v;
      }
    } else if (*rest != '\0') {
      return false_v;
    }
  } else {
    const char *colon = strchr(text, ':');
    const bool8_t one_colon = colon && strchr(colon + 1, ':') == NULL;
    host_length = one_colon ? (uint64_t)(colon - text) : length;
    if (host_length == 0u || host_length >= sizeof(host)) {
      return false_v;
    }
    MemCopy(host, text, host_length);
    host[host_length] = '\0';
    if (one_colon &&
        !udp_parse_decimal(colon + 1, strlen(colon + 1), 65535u, &port)) {
      return false_v;
    }
  }

  char *zone = strchr(host, '%');
  if (zone) {
    *zone = '\0';
    if (!udp_parse_decimal(zone + 1, strlen(zone + 1), UINT32_MAX, &scope)) {
      return false_v;
    }
  }

  VkrNetAddress address = {.port = (uint16_t)port};
  struct in_addr v4;
  struct in6_addr v6;
  if (!zone && inet_pton(AF_INET, host, &v4) == 1) {
    address.family = VKR_NET_ADDRESS_IPV4;
    MemCopy(address.bytes, &v4, 4);
  } else if (inet_pton(AF_INET6, host, &v6) == 1) {
    address.family = VKR_NET_ADDRESS_IPV6;
    MemCopy(address.bytes, &v6, 16);
    address.scope = scope;
  } else {
    return false_v;
  }
  *out_address = address;
  return true_v;
}

bool8_t vkr_net_address_resolve(const char *host, uint16_t port,
                                bool8_t prefer_ipv4,
                                VkrNetAddress *out_address) {
  if (!host || !out_address || !udp_ready()) {
    return false_v;
  }
  if (vkr_net_address_parse(host, port, out_address)) {
    return true_v;
  }
  struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM};
  struct addrinfo *list = NULL;
  if (getaddrinfo(host, NULL, &hints, &list) != 0 || !list) {
    return false_v;
  }
  bool8_t found = false_v;
  for (int32_t pass = 0; pass < 2 && !found; ++pass) {
    for (const struct addrinfo *entry = list; entry; entry = entry->ai_next) {
      const bool8_t v4 = entry->ai_family == AF_INET;
      if (pass == 0 && v4 != prefer_ipv4) {
        continue;
      }
      if (udp_from_sockaddr(entry->ai_addr, out_address)) {
        out_address->port = port;
        found = true_v;
        break;
      }
    }
  }
  freeaddrinfo(list);
  return found;
}

void vkr_net_address_format(const VkrNetAddress *address, char *out,
                            uint64_t capacity) {
  if (!out || capacity == 0u) {
    return;
  }
  char host[INET6_ADDRSTRLEN] = "?";
  if (address->family == VKR_NET_ADDRESS_IPV4) {
    (void)inet_ntop(AF_INET, (void *)address->bytes, host, sizeof(host));
    snprintf(out, capacity, "%s:%u", host, (uint32_t)address->port);
  } else if (address->family == VKR_NET_ADDRESS_IPV6) {
    (void)inet_ntop(AF_INET6, (void *)address->bytes, host, sizeof(host));
    if (address->scope) {
      snprintf(out, capacity, "[%s%%%u]:%u", host, address->scope,
               (uint32_t)address->port);
    } else {
      snprintf(out, capacity, "[%s]:%u", host, (uint32_t)address->port);
    }
  } else {
    snprintf(out, capacity, "(none)");
  }
}

static void udp_close_native(UdpNative native) {
#if defined(_WIN32)
  closesocket(native);
#else
  close(native);
#endif
}

static bool8_t udp_set_option(UdpNative native, int level, int name,
                              int value) {
  return setsockopt(native, level, name, (const char *)&value,
                    (UdpLength)sizeof(value)) == 0;
}

/* Sets the don't-fragment bit for every family the socket sends. */
static void udp_set_dont_fragment(UdpNative native, uint8_t family) {
#if defined(_WIN32)
  if (family == VKR_NET_ADDRESS_IPV4) {
    (void)udp_set_option(native, IPPROTO_IP, IP_DONTFRAGMENT, 1);
  } else {
    (void)udp_set_option(native, IPPROTO_IPV6, IPV6_DONTFRAG, 1);
    (void)udp_set_option(native, IPPROTO_IP, IP_DONTFRAGMENT, 1);
  }
#else
  /* A dual-stack IPv6 socket sends IPv4 datagrams too. */
#if defined(IP_DONTFRAG)
  (void)udp_set_option(native, IPPROTO_IP, IP_DONTFRAG, 1);
#elif defined(IP_MTU_DISCOVER)
  (void)udp_set_option(native, IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_DO);
#endif
#if defined(IPV6_DONTFRAG)
  if (family == VKR_NET_ADDRESS_IPV6) {
    (void)udp_set_option(native, IPPROTO_IPV6, IPV6_DONTFRAG, 1);
  }
#elif defined(IPV6_MTU_DISCOVER)
  if (family == VKR_NET_ADDRESS_IPV6) {
    (void)udp_set_option(native, IPPROTO_IPV6, IPV6_MTU_DISCOVER,
                         IPV6_PMTUDISC_DO);
  }
#endif
#endif
}

static bool8_t udp_set_nonblocking(UdpNative native) {
#if defined(_WIN32)
  u_long enabled = 1u;
  return ioctlsocket(native, FIONBIO, &enabled) == 0;
#else
  const int flags = fcntl(native, F_GETFL, 0);
  if (flags < 0 || fcntl(native, F_SETFL, flags | O_NONBLOCK) != 0) {
    return false_v;
  }
  (void)fcntl(native, F_SETFD, FD_CLOEXEC);
  return true_v;
#endif
}

bool8_t vkr_udp_socket_open(const VkrNetAddress *address,
                            const VkrUdpSocketConfig *config,
                            VkrUdpSocket *out_socket) {
  if (!address || !out_socket || !udp_ready()) {
    return false_v;
  }
  *out_socket = (VkrUdpSocket){.handle = VKR_UDP_SOCKET_INVALID};
  const bool8_t v6 = address->family == VKR_NET_ADDRESS_IPV6;
  if (!v6 && address->family != VKR_NET_ADDRESS_IPV4) {
    return false_v;
  }
  const UdpNative native =
      socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (native == UDP_NATIVE_INVALID) {
    return false_v;
  }
  /* Dual stack: an IPv6 socket accepts and reaches IPv4 peers too. */
  if (v6 && !udp_set_option(native, IPPROTO_IPV6, IPV6_V6ONLY, 0)) {
    udp_close_native(native);
    return false_v;
  }
  if (!udp_set_nonblocking(native)) {
    udp_close_native(native);
    return false_v;
  }
#if defined(_WIN32)
  /* Without this, an ICMP port unreachable for one peer fails the next
     receive with WSAECONNRESET, whatever peer sent the next datagram. */
  BOOL report = FALSE;
  DWORD returned = 0u;
  (void)WSAIoctl(native, SIO_UDP_CONNRESET, &report, sizeof(report), NULL, 0,
                 &returned, NULL, NULL);
  (void)WSAIoctl(native, SIO_UDP_NETRESET, &report, sizeof(report), NULL, 0,
                 &returned, NULL, NULL);
#endif
  if (config && config->receive_buffer) {
    (void)udp_set_option(native, SOL_SOCKET, SO_RCVBUF,
                         (int)config->receive_buffer);
  }
  if (config && config->send_buffer) {
    (void)udp_set_option(native, SOL_SOCKET, SO_SNDBUF,
                         (int)config->send_buffer);
  }
  udp_set_dont_fragment(native, address->family);

  struct sockaddr_storage bound;
  const UdpLength bound_length =
      udp_to_sockaddr(address, address->family, &bound);
  if (bind(native, (const struct sockaddr *)&bound, bound_length) != 0) {
    const int32_t error = udp_last_error();
    udp_close_native(native);
#if defined(_WIN32)
    WSASetLastError(error);
#else
    errno = error;
#endif
    return false_v;
  }
  *out_socket =
      (VkrUdpSocket){.handle = (uint64_t)native, .family = address->family};
  return true_v;
}

void vkr_udp_socket_close(VkrUdpSocket *socket) {
  if (!socket || socket->handle == VKR_UDP_SOCKET_INVALID) {
    return;
  }
  udp_close_native(udp_native(*socket));
  socket->handle = VKR_UDP_SOCKET_INVALID;
}

bool8_t vkr_udp_socket_local_address(VkrUdpSocket socket,
                                     VkrNetAddress *out_address) {
  struct sockaddr_storage address;
  UdpLength length = (UdpLength)sizeof(address);
  if (socket.handle == VKR_UDP_SOCKET_INVALID ||
      getsockname(udp_native(socket), (struct sockaddr *)&address, &length) !=
          0) {
    return false_v;
  }
  return udp_from_sockaddr((const struct sockaddr *)&address, out_address);
}

VkrUdpStatus vkr_udp_socket_receive(VkrUdpSocket socket,
                                    VkrUdpDatagram *datagrams, uint32_t count,
                                    uint32_t *out_received,
                                    uint32_t *out_truncated) {
  uint32_t received = 0u;
  uint32_t truncated = 0u;
  VkrUdpStatus status = VKR_UDP_OK;
  const UdpNative native = udp_native(socket);
  while (received < count) {
    VkrUdpDatagram *datagram = &datagrams[received];
    struct sockaddr_storage from;
#if defined(_WIN32)
    int from_length = (int)sizeof(from);
    const int result =
        recvfrom(native, (char *)datagram->data, (int)datagram->capacity, 0,
                 (struct sockaddr *)&from, &from_length);
    const bool8_t cut = result < 0 && udp_error_too_large(udp_last_error());
#else
    struct iovec vector = {.iov_base = datagram->data,
                           .iov_len = datagram->capacity};
    struct msghdr message = {.msg_name = &from,
                             .msg_namelen = sizeof(from),
                             .msg_iov = &vector,
                             .msg_iovlen = 1};
    const ssize_t result = recvmsg(native, &message, 0);
    const bool8_t cut = result >= 0 && (message.msg_flags & MSG_TRUNC);
#endif
    if (cut) {
      ++truncated;
      continue;
    }
    if (result < 0) {
      const int32_t error = udp_last_error();
      if (udp_error_interrupted(error) || udp_error_transient(error)) {
        continue;
      }
      status =
          udp_error_would_block(error) ? VKR_UDP_WOULD_BLOCK : VKR_UDP_ERROR;
      break;
    }
    if (!udp_from_sockaddr((const struct sockaddr *)&from,
                           &datagram->address)) {
      continue;
    }
    datagram->size = (uint32_t)result;
    ++received;
  }
  if (out_received) {
    *out_received = received;
  }
  if (out_truncated) {
    *out_truncated = truncated;
  }
  if (status == VKR_UDP_WOULD_BLOCK && received > 0u) {
    status = VKR_UDP_OK;
  }
  return status;
}

VkrUdpStatus vkr_udp_socket_send(VkrUdpSocket socket,
                                 const VkrUdpDatagram *datagrams,
                                 uint32_t count, uint32_t *out_sent) {
  uint32_t sent = 0u;
  VkrUdpStatus status = VKR_UDP_OK;
  const UdpNative native = udp_native(socket);
  while (sent < count) {
    const VkrUdpDatagram *datagram = &datagrams[sent];
    struct sockaddr_storage to;
    const UdpLength to_length =
        udp_to_sockaddr(&datagram->address, socket.family, &to);
#if defined(_WIN32)
    const int result =
        sendto(native, (const char *)datagram->data, (int)datagram->size, 0,
               (const struct sockaddr *)&to, to_length);
#else
    const ssize_t result = sendto(native, datagram->data, datagram->size, 0,
                                  (const struct sockaddr *)&to, to_length);
#endif
    if (result < 0) {
      const int32_t error = udp_last_error();
      if (udp_error_interrupted(error)) {
        continue;
      }
      if (udp_error_too_large(error) || udp_error_transient(error)) {
        /* The network would drop it too; loss recovery handles it. */
        ++sent;
        continue;
      }
      status =
          udp_error_would_block(error) ? VKR_UDP_WOULD_BLOCK : VKR_UDP_ERROR;
      break;
    }
    ++sent;
  }
  if (out_sent) {
    *out_sent = sent;
  }
  return status;
}

int32_t vkr_udp_socket_wait(VkrUdpSocket socket, int32_t timeout_ms) {
#if defined(_WIN32)
  WSAPOLLFD entry = {.fd = udp_native(socket), .events = POLLRDNORM};
  const int result = WSAPoll(&entry, 1u, timeout_ms < 0 ? -1 : timeout_ms);
  if (result == SOCKET_ERROR) {
    return udp_error_interrupted(udp_last_error()) ? 0 : -1;
  }
#else
  struct pollfd entry = {.fd = udp_native(socket), .events = POLLIN};
  const int result = poll(&entry, 1u, timeout_ms < 0 ? -1 : timeout_ms);
  if (result < 0) {
    return udp_error_interrupted(udp_last_error()) ? 0 : -1;
  }
#endif
  return result > 0 ? 1 : 0;
}

void vkr_udp_socket_error_text(char *out, uint64_t capacity) {
  if (!out || capacity == 0u) {
    return;
  }
  const int32_t error = udp_last_error();
#if defined(_WIN32)
  wchar_t message[256];
  const DWORD length =
      FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     NULL, (DWORD)error, 0, message, ArrayCount(message), NULL);
  char text[512] = {0};
  if (length) {
    (void)WideCharToMultiByte(CP_UTF8, 0, message, (int)length, text,
                              (int)sizeof(text) - 1, NULL, NULL);
  }
  /* System messages end with a line break and often a period. */
  uint64_t used = strlen(text);
  while (used && (text[used - 1u] == '\n' || text[used - 1u] == '\r' ||
                  text[used - 1u] == ' ' || text[used - 1u] == '.')) {
    text[--used] = '\0';
  }
  snprintf(out, capacity, "%s (WSA %d)", used ? text : "socket error", error);
#else
  snprintf(out, capacity, "%s (errno %d)", strerror(error), error);
#endif
}
