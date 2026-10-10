#pragma once

#include "defines.h"

/* Nonblocking UDP sockets for the network transport
 * (docs/proposals/network-protocol.md). POSIX hosts use BSD sockets; Windows
 * uses Winsock 2, which the foundation starts once per process.
 *
 * A socket binds one address. A socket bound to the IPv6 wildcard also
 * accepts IPv4 peers (dual stack); their addresses report as IPv4, and a
 * send to an IPv4 address goes out through the same socket. Every socket
 * sets the don't-fragment bit, so a datagram larger than the path MTU is
 * dropped instead of fragmented. On Windows a socket ignores ICMP port
 * unreachable reports, which would otherwise fail the next receive.
 *
 * Batch calls move as many datagrams as the socket accepts without
 * blocking. Hosts without batched system calls (Windows, macOS) make one
 * call per datagram; the interface stays the same for a host that has them. */

typedef enum VkrNetAddressFamily {
  VKR_NET_ADDRESS_NONE = 0,
  VKR_NET_ADDRESS_IPV4 = 4,
  VKR_NET_ADDRESS_IPV6 = 6,
} VkrNetAddressFamily;

/* A socket handle and the family it was bound with; an IPv6 socket maps
   IPv4 peers. `handle` is VKR_UDP_SOCKET_INVALID for no socket. */
typedef struct VkrUdpSocket {
  uint64_t handle;
  uint8_t family; /**< VkrNetAddressFamily. */
} VkrUdpSocket;

#define VKR_UDP_SOCKET_INVALID UINT64_MAX
/* Largest UDP payload a datagram can carry over IPv4 or IPv6. */
#define VKR_UDP_DATAGRAM_MAX 65507u
/* Text of an address with its port, including the terminator:
   "[ffff:...:ffff%4294967295]:65535". */
#define VKR_NET_ADDRESS_TEXT 64u

/* An IP address and port in network byte order. IPv4 uses `bytes[0..3]`;
   the remaining bytes are zero, so two addresses compare with MemCompare.
   `scope` is the IPv6 zone (interface index) of a link-local address. */
typedef struct VkrNetAddress {
  uint8_t family; /**< VkrNetAddressFamily. */
  uint8_t reserved;
  uint16_t port; /**< Host byte order. */
  uint32_t scope;
  uint8_t bytes[16];
} VkrNetAddress;

typedef enum VkrUdpStatus {
  VKR_UDP_OK = 0,
  /* Nothing to receive or no room to send. */
  VKR_UDP_WOULD_BLOCK,
  /* The datagram did not fit the receive buffer or the path MTU. The
     datagram is consumed; the caller continues. */
  VKR_UDP_TRUNCATED,
  VKR_UDP_ERROR,
} VkrUdpStatus;

typedef struct VkrUdpSocketConfig {
  /* Kernel buffer sizes in bytes; zero keeps the host default. */
  uint32_t receive_buffer;
  uint32_t send_buffer;
} VkrUdpSocketConfig;

/* One datagram of a batch. For a receive, `data` and `capacity` are input
   and `address` and `size` are output; for a send all are input. */
typedef struct VkrUdpDatagram {
  VkrNetAddress address;
  uint8_t *data;
  uint32_t capacity;
  uint32_t size;
} VkrUdpDatagram;

/* True for the same family, bytes, port and scope. */
bool8_t vkr_net_address_equal(const VkrNetAddress *a, const VkrNetAddress *b);

/* A numeric address: "1.2.3.4", "1.2.3.4:5000", "::1", "[::1]:5000" or
   "[fe80::1%4]:5000". Without a port, `default_port` applies. Host names
   are refused; vkr_net_address_resolve looks them up. */
bool8_t vkr_net_address_parse(const char *text, uint16_t default_port,
                              VkrNetAddress *out_address);

/* Looks up `host` (a name or a numeric address, without a port) and returns
   its first address, preferring IPv4 when `prefer_ipv4` is set. Blocks on
   DNS; never call it from a frame or a network thread. */
bool8_t vkr_net_address_resolve(const char *host, uint16_t port,
                                bool8_t prefer_ipv4,
                                VkrNetAddress *out_address);

/* "1.2.3.4:5000" or "[::1]:5000" into `out`, truncated to `capacity`. */
void vkr_net_address_format(const VkrNetAddress *address, char *out,
                            uint64_t capacity);

/* The wildcard address of `family` with `port`; IPv6 serves both families. */
VkrNetAddress vkr_net_address_any(VkrNetAddressFamily family, uint16_t port);

/* The loopback address of `family` with `port`. */
VkrNetAddress vkr_net_address_loopback(VkrNetAddressFamily family,
                                       uint16_t port);

/* Opens a nonblocking socket bound to `address` (port zero picks one).
   `config` may be NULL. False on failure; see vkr_udp_socket_error_text. */
bool8_t vkr_udp_socket_open(const VkrNetAddress *address,
                            const VkrUdpSocketConfig *config,
                            VkrUdpSocket *out_socket);

/* Closes a socket and marks it invalid; an invalid socket is ignored. */
void vkr_udp_socket_close(VkrUdpSocket *socket);

/* The bound address, with the port the host picked. */
bool8_t vkr_udp_socket_local_address(VkrUdpSocket socket,
                                     VkrNetAddress *out_address);

/* Receives up to `count` datagrams. Stores how many arrived and returns
   VKR_UDP_OK when at least one did or the socket is empty
   (VKR_UDP_WOULD_BLOCK with zero received), VKR_UDP_ERROR on failure. A
   truncated datagram is skipped and counted in `out_truncated` when it is
   not NULL. */
VkrUdpStatus vkr_udp_socket_receive(VkrUdpSocket socket,
                                    VkrUdpDatagram *datagrams, uint32_t count,
                                    uint32_t *out_received,
                                    uint32_t *out_truncated);

/* Sends datagrams in order until one would block. Stores how many left.
   A datagram larger than the path MTU (VKR_UDP_TRUNCATED from the host) is
   counted as sent and dropped, as the network would drop it. */
VkrUdpStatus vkr_udp_socket_send(VkrUdpSocket socket,
                                 const VkrUdpDatagram *datagrams,
                                 uint32_t count, uint32_t *out_sent);

/* Waits up to `timeout_ms` (negative waits without limit) until `socket`
   is readable. Returns 1 when readable, 0 on timeout or interruption and -1
   on failure. */
int32_t vkr_udp_socket_wait(VkrUdpSocket socket, int32_t timeout_ms);

/* Describes the last failed socket call of this thread. */
void vkr_udp_socket_error_text(char *out, uint64_t capacity);
