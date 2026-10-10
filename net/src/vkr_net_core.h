#pragma once

#include "defines.h"
#include "memory/vkr_allocator.h"
#include "platform/vkr_udp_socket.h"
#include "vkr_net_crypto.h"

/* The sans-I/O transport core (docs/proposals/network-protocol.md,
 * "Transport"). The core takes datagrams and a time and returns datagrams
 * and a next deadline; it opens no socket, starts no thread and reads no
 * clock. A driver (vkr_net_host.h) or a test feeds it.
 *
 * One core holds up to 65,535 connections, each identified by the 32-bit
 * connection ID its peer puts in every packet (VkrNetConnectionId). A core
 * is single-threaded: every call must come from one thread at a time.
 *
 * Times are microseconds of one monotonic clock chosen by the caller. */

#define VKR_NET_PROTOCOL_VERSION 1u
#define VKR_NET_CHANNEL_MAX 64u
/* The first UDP payload size of every path: it fits IPv6's 1,280-byte
   minimum MTU and common tunnels. */
#define VKR_NET_DATAGRAM_INITIAL 1200u
/* Ethernet's 1,500-byte MTU less IPv6 and UDP headers. */
#define VKR_NET_DATAGRAM_ETHERNET 1452u
/* A 9,000-byte jumbo MTU less IPv6 and UDP headers. */
#define VKR_NET_DATAGRAM_JUMBO 8952u
/* Credential bytes a client sends in its first packet. */
#define VKR_NET_CREDENTIAL_MAX 1024u
/* Bytes a server returns with its acceptance. */
#define VKR_NET_ACCEPT_PAYLOAD_MAX 256u
/* The hard cap of one message. */
#define VKR_NET_MESSAGE_MAX (64u * 1024u * 1024u)

typedef uint32_t VkrNetConnectionId;
#define VKR_NET_CONNECTION_NONE 0u

typedef enum VkrNetDelivery {
  /* At most once, in any order. */
  VKR_NET_UNRELIABLE = 0,
  /* At most once; an older message than the newest delivered is dropped. */
  VKR_NET_SEQUENCED,
  /* Exactly once, in send order. */
  VKR_NET_RELIABLE_ORDERED,
  /* Exactly once, in arrival order. */
  VKR_NET_RELIABLE_UNORDERED,
  /* As RELIABLE_UNORDERED until the message's deadline; then the sender
     drops and cancels it. */
  VKR_NET_DEADLINE,
  VKR_NET_DELIVERY_COUNT
} VkrNetDelivery;

/* Numeric close and refusal codes; no reason text crosses the wire. Codes
   from 0x100 belong to services. */
typedef enum VkrNetCloseCode {
  VKR_NET_CLOSE_NONE = 0,
  /* The application closed the connection. */
  VKR_NET_CLOSE_APPLICATION = 1,
  VKR_NET_CLOSE_PROTOCOL_VIOLATION = 2,
  VKR_NET_CLOSE_FLOW_CONTROL = 3,
  VKR_NET_CLOSE_ACCESS_DENIED = 4,
  VKR_NET_CLOSE_SERVER_FULL = 5,
  VKR_NET_CLOSE_TIMEOUT = 6,
  VKR_NET_CLOSE_HANDSHAKE_FAILED = 7,
  VKR_NET_CLOSE_VERSION = 8,
  VKR_NET_CLOSE_SCHEMA_MISMATCH = 9,
  VKR_NET_CLOSE_SCHEMA_VIOLATION = 10,
  VKR_NET_CLOSE_SERVICE_REFUSED = 11,
  VKR_NET_CLOSE_INTERNAL = 12,
  VKR_NET_CLOSE_SERVICE_BASE = 0x100,
} VkrNetCloseCode;

typedef struct VkrNetChannelConfig {
  uint8_t delivery; /**< VkrNetDelivery. */
  /** 0 is sent first, 7 last. */
  uint8_t priority;
  /** Share of its priority level, at least 1. */
  uint16_t weight;
  /** Largest message; zero means 64 KiB. Unreliable and sequenced
      messages must fit one packet. */
  uint32_t max_message_size;
  /** Bytes the receiver accepts in flight for reliable classes; at least
      `max_message_size`. Zero means four times the message size. */
  uint32_t receive_window;
  /** Messages queued for sending; a power of two, zero means 1,024. */
  uint32_t send_queue;
  /** DEADLINE: default milliseconds from send to deadline. */
  uint32_t deadline_ms;
} VkrNetChannelConfig;

typedef enum VkrNetSendFlags {
  VKR_NET_SEND_NONE = 0u,
  /* Pack and send without waiting for vkr_net_core_flush. */
  VKR_NET_SEND_IMMEDIATE = 1u << 0,
  /* Report VKR_NET_EVENT_ACKED or VKR_NET_EVENT_LOST for the packet that
     carried this unreliable message, with its tag. */
  VKR_NET_SEND_NOTIFY = 1u << 1,
  /* `tag` travels with the message. */
  VKR_NET_SEND_TAG = 1u << 2,
} VkrNetSendFlags;

typedef struct VkrNetSendOptions {
  uint32_t flags; /**< VkrNetSendFlags. */
  uint64_t tag;
  /** DEADLINE: absolute deadline; zero uses the channel's default. */
  uint64_t deadline_us;
} VkrNetSendOptions;

typedef enum VkrNetSendStatus {
  VKR_NET_SEND_OK = 0,
  /* The channel's send queue is full; retry after acknowledgments. */
  VKR_NET_SEND_QUEUE_FULL,
  VKR_NET_SEND_TOO_LARGE,
  VKR_NET_SEND_NO_MEMORY,
  /* No such connection, channel or an invalid argument. */
  VKR_NET_SEND_INVALID,
  /* The connection is closing or has no keys yet for this channel's data. */
  VKR_NET_SEND_CLOSED,
} VkrNetSendStatus;

typedef enum VkrNetEventType {
  VKR_NET_EVENT_NONE = 0,
  /* A handshake completed. Servers report it when they accept a client. */
  VKR_NET_EVENT_CONNECTED,
  /* The connection ended; `code` says why. The ID is invalid afterwards. */
  VKR_NET_EVENT_CLOSED,
  /* A message arrived. `data` stays valid until the next poll call. */
  VKR_NET_EVENT_MESSAGE,
  /* The packet that carried a VKR_NET_SEND_NOTIFY message was acknowledged
     or declared lost. */
  VKR_NET_EVENT_ACKED,
  VKR_NET_EVENT_LOST,
} VkrNetEventType;

typedef struct VkrNetEvent {
  uint8_t type; /**< VkrNetEventType. */
  uint8_t channel;
  uint16_t code; /**< VkrNetCloseCode for CLOSED. */
  VkrNetConnectionId connection;
  uint64_t sequence;
  uint64_t tag;
  bool8_t has_tag;
  const uint8_t *data;
  uint32_t size;
  /* CONNECTED: the peer's static public key. */
  uint8_t peer_key[VKR_NET_KEY_SIZE];
} VkrNetEvent;

/* A server's decision about a client. Fill `code` with a close code to
   refuse; leave it zero to accept. `payload` returns to the client. */
typedef struct VkrNetAcceptResult {
  uint16_t code;
  uint8_t payload[VKR_NET_ACCEPT_PAYLOAD_MAX];
  uint32_t payload_size;
  uint64_t user;
} VkrNetAcceptResult;

/* Called during vkr_net_core_receive for each authenticated first message.
   `peer_key` is the client's static key and `credential` its first-message
   payload, both borrowed for the call. */
typedef void (*VkrNetAcceptFn)(void *context, const VkrNetAddress *address,
                               const uint8_t peer_key[VKR_NET_KEY_SIZE],
                               const uint8_t *credential,
                               uint32_t credential_size,
                               VkrNetAcceptResult *out_result);

typedef struct VkrNetCoreConfig {
  /** This endpoint's static key pair, copied. Required. */
  VkrNetKeyPair static_keys;
  /** Connections at once, at most 65,535; zero means 64. */
  uint32_t max_connections;
  /** Largest UDP payload PMTU discovery probes up to; zero means
      VKR_NET_DATAGRAM_ETHERNET. VKR_NET_DATAGRAM_JUMBO for jumbo LANs. */
  uint32_t max_datagram;
  /** Zero means 15,000. */
  uint32_t idle_timeout_ms;
  /** Zero means 10,000. */
  uint32_t handshake_timeout_ms;
  /** Zero means 5,000 µs; bulk connections use 25,000. */
  uint32_t max_ack_delay_us;
  /** Sent packets tracked per connection; a power of two, zero means
      4,096. */
  uint32_t sent_packet_capacity;
  /** Accept clients; a server sets it with `accept`. */
  bool8_t accept_incoming;
  /** Demand a retry cookie from every client. */
  bool8_t require_retry;
  /** Demand a cookie once this many handshakes are half-open; zero means
      256. */
  uint32_t retry_threshold;
  /** Yield to other traffic: keep queueing delay below 25 ms. */
  bool8_t background;
  /** Top four bits of every local connection ID. */
  uint8_t shard;
  VkrNetAcceptFn accept;
  void *accept_context;
} VkrNetCoreConfig;

typedef struct VkrNetPathStats {
  uint64_t srtt_us;
  uint64_t rttvar_us;
  uint64_t min_rtt_us;
  /** Bottleneck bandwidth estimate in bytes per second. */
  uint64_t bandwidth;
  uint64_t pacing_rate;
  uint64_t cwnd;
  uint64_t bytes_in_flight;
  uint32_t max_datagram;
  uint64_t packets_sent;
  uint64_t packets_received;
  uint64_t packets_lost;
  uint64_t bytes_sent;
  uint64_t bytes_received;
} VkrNetPathStats;

typedef struct VkrNetCore VkrNetCore;

/* False from create when libsodium has no AES-GCM hardware path. */
VkrNetCore *vkr_net_core_create(VkrAllocator *allocator,
                                const VkrNetCoreConfig *config);
void vkr_net_core_destroy(VkrNetCore *core);

/* Starts a handshake with the server at `address` whose static public key
   is `server_key`. `credential` (at most VKR_NET_CREDENTIAL_MAX bytes) goes
   to the server's accept callback. CONNECTED or CLOSED follows. */
bool8_t vkr_net_core_connect(VkrNetCore *core, const VkrNetAddress *address,
                             const uint8_t server_key[VKR_NET_KEY_SIZE],
                             const uint8_t *credential,
                             uint32_t credential_size, uint64_t now_us,
                             VkrNetConnectionId *out_connection);

/* Processes one received datagram. `bytes` is modified (decrypted in
   place). */
void vkr_net_core_receive(VkrNetCore *core, const VkrNetAddress *from,
                          uint8_t *bytes, uint32_t size, uint64_t now_us);

/* Runs timers due at `now_us`: retransmission, probes, acknowledgments,
   idle and drain timeouts. */
void vkr_net_core_update(VkrNetCore *core, uint64_t now_us);

/* Builds the next datagram into `buffer` and returns its size, or zero when
   nothing may be sent now. `capacity` should hold the largest datagram
   (`max_datagram`). Call until it returns zero. */
uint32_t vkr_net_core_transmit(VkrNetCore *core, uint64_t now_us,
                               uint8_t *buffer, uint32_t capacity,
                               VkrNetAddress *out_address);

/* The earliest time vkr_net_core_update or vkr_net_core_transmit has work,
   or UINT64_MAX. */
uint64_t vkr_net_core_next_deadline(const VkrNetCore *core);

/* Opens `channel` (1 to 63; channel 0 is the session channel, reliable and
   ordered) on a connection. Both peers must open it with the same
   configuration before either sends on it. */
bool8_t vkr_net_core_open_channel(VkrNetCore *core,
                                  VkrNetConnectionId connection,
                                  uint8_t channel,
                                  const VkrNetChannelConfig *config);

/* Closes a channel and drops its queued messages. Data the peer still
   sends on it is dropped on arrival. */
void vkr_net_core_close_channel(VkrNetCore *core, VkrNetConnectionId connection,
                                uint8_t channel);

/* Copies `data` into the channel's send queue. `options` may be NULL. */
VkrNetSendStatus
vkr_net_core_send(VkrNetCore *core, VkrNetConnectionId connection,
                  uint8_t channel, const void *data, uint32_t size,
                  const VkrNetSendOptions *options, uint64_t now_us);

/* Marks every connection with queued messages ready to send. */
void vkr_net_core_flush(VkrNetCore *core);

/* Takes the next event; false when none is queued. The previous event's
   data is released. */
bool8_t vkr_net_core_poll(VkrNetCore *core, VkrNetEvent *out_event);

/* Sends CLOSE with `code`; CLOSED follows once the peer stops or the drain
   period ends. */
void vkr_net_core_close(VkrNetCore *core, VkrNetConnectionId connection,
                        uint16_t code, uint64_t now_us);

bool8_t vkr_net_core_path(const VkrNetCore *core, VkrNetConnectionId connection,
                          VkrNetPathStats *out_stats);

/* The user value the accept callback set (servers) or zero. */
uint64_t vkr_net_core_user(const VkrNetCore *core,
                           VkrNetConnectionId connection);

bool8_t vkr_net_core_peer_address(const VkrNetCore *core,
                                  VkrNetConnectionId connection,
                                  VkrNetAddress *out_address);

/* Messages and bytes queued and not yet acknowledged on a channel. */
uint64_t vkr_net_core_queued_bytes(const VkrNetCore *core,
                                   VkrNetConnectionId connection,
                                   uint8_t channel);
