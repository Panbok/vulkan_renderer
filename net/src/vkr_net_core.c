#include "vkr_net_core.h"

#include "core/vkr_byte_io.h"
#include "vkr_net_cc.h"
#include "vkr_net_varint.h"

#include <string.h>

// =============================================================================
// Wire constants (docs/proposals/network-protocol.md, "Transport")
// =============================================================================

/* First byte of every packet: bit 7 selects the long header, bit 6 is
   always set (it separates the protocol from STUN on a shared port). */
#define NET_FLAG_LONG 0x80u
#define NET_FLAG_FIXED 0x40u
#define NET_LONG_TYPE_SHIFT 4u
#define NET_LONG_INITIAL 0u
#define NET_LONG_HANDSHAKE 1u
#define NET_LONG_RETRY 2u
/* Short header bits 1-0: packet number length minus one. */
#define NET_SHORT_PN_MASK 0x03u
#define NET_SHORT_RESERVED_MASK 0x3cu

#define NET_CID_SIZE 4u
#define NET_SHORT_HEADER_MIN (1u + NET_CID_SIZE + 1u)
#define NET_SHORT_HEADER_MAX (1u + NET_CID_SIZE + 4u)
/* flags, version, destination and source connection IDs. */
#define NET_LONG_HEADER_SIZE (1u + 4u + NET_CID_SIZE + NET_CID_SIZE)

/* Frame types. The first byte selects the frame: 0x00-0x3f basic frames,
   0x40-0x7f MESSAGE with flags in the low six bits, 0xc0-0xff extension
   frames with a length that an unknown receiver skips. */
#define FRAME_PADDING 0x00u
#define FRAME_PING 0x01u
#define FRAME_ACK 0x02u
#define FRAME_WINDOW 0x04u
#define FRAME_BLOCKED 0x05u
#define FRAME_CANCEL 0x06u
#define FRAME_PATH_CHALLENGE 0x08u
#define FRAME_PATH_RESPONSE 0x09u
#define FRAME_CLOSE 0x0au
#define FRAME_MESSAGE 0x40u
#define FRAME_EXTENSION 0xc0u

#define MESSAGE_LEN 0x01u
#define MESSAGE_SEQ 0x02u
#define MESSAGE_FRAG 0x04u
#define MESSAGE_TAG 0x08u

/* Largest MESSAGE frame header: type, channel, sequence, fragment index,
   fragment size, total size, tag and length. */
#define NET_MESSAGE_HEADER_MAX (1u + 1u + 8u + 4u + 4u + 4u + 8u + 4u)
/* Room a full fragment leaves for an ACK frame in the same packet. */
#define NET_FRAGMENT_ACK_ROOM 48u

#define NET_ACK_RANGES_MAX 64u
#define NET_REPLAY_WINDOW 2048u
#define NET_RX_DONE_WINDOW 4096u
#define NET_REASSEMBLY_INITIAL 16u
#define NET_CANCEL_QUEUE 64u
#define NET_STATELESS_QUEUE 32u
#define NET_STATELESS_SIZE 512u
#define NET_SEND_QUEUE_MAX 4096u
#define NET_EVENT_UNRELIABLE_MAX 65536u
#define NET_RECORDS_PER_PACKET 8u
#define NET_POOL_CLASSES 8u
#define NET_POOL_CACHE_MAX (64ull * 1024ull * 1024ull)

#define NET_INITIAL_RTT_US 100000ull
#define NET_HANDSHAKE_RETRY_US 250000ull
#define NET_GRANULARITY_US 1000ull
#define NET_COOKIE_LIFETIME_S 30u
#define NET_RETRY_MAX 2u
#define NET_MTU_PROBE_RETRY_US 1000000ull
#define NET_MTU_PROBE_GIVE_UP_US 60000000ull
#define NET_MTU_PROBE_FAILURES 3u
#define NET_PACING_BURST_PACKETS 10u
/* How far behind the oldest live packet an acknowledgment can still reveal
   a spurious loss. */
#define NET_SPURIOUS_WINDOW 512u

static const uint8_t net_prologue[] = "vkr-net v1";

// =============================================================================
// Internal types
// =============================================================================

typedef enum NetState {
  NET_STATE_FREE = 0,
  /* Client: Initial sent, waiting for the server's Handshake. */
  NET_STATE_CLIENT_INITIAL,
  /* Server: keys derived and Handshake sent; the client has not yet proved
     it holds them. */
  NET_STATE_SERVER_HANDSHAKE,
  NET_STATE_ESTABLISHED,
  /* This side sent CLOSE and waits out the drain period. */
  NET_STATE_CLOSING,
  /* The peer sent CLOSE; nothing more is sent. */
  NET_STATE_DRAINING,
} NetState;

typedef enum NetMessageState {
  NET_MESSAGE_FREE = 0,
  NET_MESSAGE_QUEUED,
  NET_MESSAGE_DONE,
} NetMessageState;

typedef enum NetRecordType {
  NET_RECORD_NONE = 0,
  /* `seq`, `fragment` of a reliable or deadline message. */
  NET_RECORD_FRAGMENT,
  /* An unreliable message sent with VKR_NET_SEND_NOTIFY; `seq` holds the
     tag. */
  NET_RECORD_NOTIFY,
  NET_RECORD_WINDOW,
  /* `seq` and its size in `fragment`. */
  NET_RECORD_CANCEL,
} NetRecordType;

typedef enum NetPacketFlags {
  NET_PACKET_LIVE = 1u << 0,
  NET_PACKET_ACK_ELICITING = 1u << 1,
  NET_PACKET_IN_FLIGHT = 1u << 2,
  NET_PACKET_MTU_PROBE = 1u << 3,
  NET_PACKET_APP_LIMITED = 1u << 4,
  NET_PACKET_HAS_ACK = 1u << 5,
  /* Declared lost; an acknowledgment later reveals reordering. */
  NET_PACKET_DECLARED_LOST = 1u << 6,
} NetPacketFlags;

typedef struct NetPool {
  VkrAllocator *allocator;
  void *free[NET_POOL_CLASSES];
  uint64_t cached_bytes;
} NetPool;

typedef struct NetSendMessage {
  uint8_t *data;
  /* Acknowledged then pending-retransmission bits, `words` each. */
  uint64_t *bits;
  uint64_t inline_bits[2];
  uint64_t seq;
  uint64_t tag;
  uint64_t deadline_us;
  /* Flow-control offset of the message's first byte. */
  uint64_t offset;
  uint32_t size;
  /* Zero for a message sent as one frame. */
  uint32_t fragment_size;
  uint32_t fragment_count;
  uint32_t next_fragment;
  uint32_t acked_count;
  uint32_t words;
  uint32_t flags;
  uint8_t state; /**< NetMessageState. */
} NetSendMessage;

typedef struct NetRetransmit {
  uint64_t seq;
  uint32_t fragment;
} NetRetransmit;

typedef struct NetReassembly {
  uint64_t seq;
  uint64_t tag;
  uint8_t *data;
  /* Received fragments: `inline_bits` for up to 64, else `heap_bits`. Entries
     move when the table grows, so nothing points into one. */
  uint64_t *heap_bits;
  uint64_t inline_bits;
  uint32_t size;
  uint32_t fragment_size;
  uint32_t fragment_count;
  uint32_t received;
  bool8_t has_tag;
  bool8_t complete;
  bool8_t live;
} NetReassembly;

typedef struct NetChannel {
  VkrNetChannelConfig config;

  /* Send side: a ring of messages indexed by sequence. */
  NetSendMessage *ring;
  uint32_t ring_mask;
  uint64_t head_seq;
  uint64_t tail_seq;
  uint64_t unsent_seq;
  uint64_t send_offset;
  uint64_t peer_limit;
  uint64_t queued_bytes;
  NetRetransmit *retransmit;
  uint32_t retransmit_count;
  uint32_t retransmit_capacity;
  bool8_t window_dirty;

  /* Receive side. */
  uint64_t rx_base_seq;
  uint64_t rx_done[NET_RX_DONE_WINDOW / 64u];
  uint64_t rx_newest_seq;
  bool8_t rx_any;
  uint64_t rx_consumed;
  uint64_t rx_advertised;
  uint64_t rx_buffered;
  /* Messages being reassembled or waiting for their turn, direct-mapped by
     sequence. Live sequences span less than NET_RX_DONE_WINDOW, so the table
     doubles until no two collide. */
  NetReassembly *reassembly;
  uint32_t reassembly_mask;
  uint32_t reassembly_count;
} NetChannel;

typedef struct NetSentPacket {
  uint64_t pn;
  uint64_t time_sent;
  uint64_t delivered;
  uint64_t delivered_time;
  uint64_t first_sent_time;
  uint64_t ack_largest;
  uint64_t record_first;
  uint32_t size;
  uint16_t record_count;
  uint16_t flags;
} NetSentPacket;

typedef struct NetRecord {
  uint8_t type;
  uint8_t channel;
  uint16_t reserved;
  uint32_t fragment;
  uint64_t seq;
} NetRecord;

typedef struct NetAckRange {
  uint64_t lo;
  uint64_t hi;
} NetAckRange;

typedef struct NetCancel {
  uint64_t seq;
  uint32_t size;
  uint8_t channel;
} NetCancel;

typedef struct NetConnection {
  uint8_t state; /**< NetState. */
  bool8_t is_client;
  bool8_t validated;
  bool8_t closed_reported;
  bool8_t in_ready;
  bool8_t in_dirty;
  bool8_t has_keys;
  VkrNetConnectionId local_cid;
  uint32_t remote_cid;
  VkrNetAddress peer;
  uint64_t amp_received;
  uint64_t amp_sent;
  uint64_t user;
  uint8_t peer_key[VKR_NET_KEY_SIZE];

  /* Handshake. */
  VkrNetNoise *noise;
  uint8_t *hs_bytes;
  uint32_t hs_size;
  uint32_t initial_dcid;
  uint8_t token[VKR_NET_COOKIE_SIZE];
  bool8_t has_token;
  uint8_t retries;
  bool8_t hs_send_pending;
  uint64_t hs_started;
  uint64_t hs_deadline;
  uint64_t hs_interval;

  VkrNetPacketKey tx;
  VkrNetPacketKey rx;

  /* Packet numbers and the replay window. */
  uint64_t next_pn;
  uint64_t rx_largest;
  uint64_t rx_largest_time;
  bool8_t rx_any;
  uint64_t rx_window[NET_REPLAY_WINDOW / 64u];

  /* Received packet numbers to acknowledge, newest range first. */
  NetAckRange ranges[NET_ACK_RANGES_MAX];
  uint32_t range_count;
  uint64_t ack_floor;
  bool8_t ack_ranges_dirty;
  bool8_t ack_now;
  uint32_t ack_eliciting_pending;
  uint64_t ack_deadline;
  uint64_t max_ack_delay_us;

  /* Sent packets and their frame records. */
  NetSentPacket *sent;
  uint32_t sent_mask;
  uint64_t sent_tail;
  NetRecord *records;
  uint32_t record_mask;
  uint64_t record_head;
  uint64_t record_tail;

  /* Recovery. */
  uint64_t largest_acked;
  bool8_t any_acked;
  uint64_t srtt;
  uint64_t rttvar;
  uint64_t latest_rtt;
  uint64_t min_rtt;
  bool8_t has_rtt;
  uint64_t loss_time;
  uint32_t pto_count;
  /* Loss thresholds; spurious losses raise them (RFC 9002, 6.1). */
  uint32_t reorder_packets;
  uint32_t reorder_time_eighths;
  uint64_t last_ack_eliciting_sent;
  uint32_t probes_pending;
  bool8_t ping_pending;
  uint64_t bytes_in_flight;
  uint64_t ack_eliciting_in_flight;

  /* Delivery-rate sampling. */
  uint64_t delivered;
  uint64_t delivered_time;
  uint64_t first_sent_time;
  uint64_t app_limited_until;
  VkrNetCc cc;

  int64_t pacing_tokens;
  uint64_t pacing_stamp;
  uint64_t pacing_release;

  /* Path MTU discovery. */
  uint32_t mtu;
  uint32_t probe_target;
  bool8_t probe_in_flight;
  uint8_t probe_failures;
  uint64_t probe_next_time;

  /* Path validation. */
  uint8_t challenge[8];
  bool8_t challenge_pending;
  bool8_t challenge_waiting;
  uint8_t response[8];
  bool8_t response_pending;

  NetCancel cancels[NET_CANCEL_QUEUE];
  uint32_t cancel_count;

  uint16_t close_code;
  bool8_t close_pending;
  uint64_t drain_deadline;

  uint64_t last_rx;
  uint64_t idle_deadline;

  uint64_t timer;
  uint64_t scheduled;

  NetChannel *channels[VKR_NET_CHANNEL_MAX];
  uint32_t rr_cursor[8];

  uint64_t packets_sent;
  uint64_t packets_received;
  uint64_t packets_lost;
  uint64_t bytes_sent;
  uint64_t bytes_received;
} NetConnection;

typedef struct NetTimer {
  uint64_t deadline;
  uint32_t slot;
  uint32_t cid;
} NetTimer;

typedef struct NetStateless {
  VkrNetAddress address;
  uint32_t size;
  uint8_t bytes[NET_STATELESS_SIZE];
} NetStateless;

typedef struct NetEventEntry {
  VkrNetEvent event;
  uint8_t *block;
  uint32_t block_size;
  /* Reliable deliveries count toward their channel's window on poll. */
  bool8_t counts;
} NetEventEntry;

struct VkrNetCore {
  VkrAllocator *allocator;
  VkrNetCoreConfig config;
  /* The latest time a caller passed in; timers compare against it. */
  uint64_t now;
  NetPool pool;

  NetConnection **slots;
  uint32_t slot_count;
  uint32_t *free_slots;
  uint32_t free_count;
  uint32_t live_count;
  uint32_t half_open;

  uint32_t *ready;
  uint32_t ready_head;
  uint32_t ready_count;
  uint32_t *dirty;
  uint32_t dirty_count;

  NetTimer *heap;
  uint32_t heap_count;
  uint32_t heap_capacity;

  NetStateless stateless[NET_STATELESS_QUEUE];
  uint32_t stateless_head;
  uint32_t stateless_count;

  NetEventEntry *events;
  uint32_t event_head;
  uint32_t event_count;
  uint32_t event_capacity;
  uint32_t unreliable_events;
  NetEventEntry current;
  bool8_t has_current;

  uint8_t cookie_secret[VKR_NET_KEY_SIZE];
  uint8_t scratch[VKR_NET_CREDENTIAL_MAX + 64u];
};

// =============================================================================
// Memory pool: size classes from 256 bytes to 4 MiB, freelists bounded by
// NET_POOL_CACHE_MAX; larger blocks go straight to the allocator.
// =============================================================================

static uint32_t pool_class(uint64_t size) {
  uint64_t class_size = 256u;
  for (uint32_t i = 0u; i < NET_POOL_CLASSES; ++i) {
    if (size <= class_size) {
      return i;
    }
    class_size <<= 2;
  }
  return NET_POOL_CLASSES;
}

static uint64_t pool_class_size(uint32_t cls) { return 256ull << (2u * cls); }

static void *pool_alloc(NetPool *pool, uint64_t size) {
  if (size == 0u) {
    size = 1u;
  }
  const uint32_t cls = pool_class(size);
  if (cls < NET_POOL_CLASSES) {
    void *block = pool->free[cls];
    if (block) {
      pool->free[cls] = *(void **)block;
      pool->cached_bytes -= pool_class_size(cls);
      return block;
    }
    return vkr_allocator_alloc(pool->allocator, pool_class_size(cls),
                               VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
  return vkr_allocator_alloc(pool->allocator, size,
                             VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
}

static void pool_free(NetPool *pool, void *block, uint64_t size) {
  if (!block) {
    return;
  }
  if (size == 0u) {
    size = 1u;
  }
  const uint32_t cls = pool_class(size);
  if (cls < NET_POOL_CLASSES &&
      pool->cached_bytes + pool_class_size(cls) <= NET_POOL_CACHE_MAX) {
    *(void **)block = pool->free[cls];
    pool->free[cls] = block;
    pool->cached_bytes += pool_class_size(cls);
    return;
  }
  vkr_allocator_free(pool->allocator, block,
                     cls < NET_POOL_CLASSES ? pool_class_size(cls) : size,
                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
}

static void pool_destroy(NetPool *pool) {
  for (uint32_t cls = 0u; cls < NET_POOL_CLASSES; ++cls) {
    void *block = pool->free[cls];
    while (block) {
      void *next = *(void **)block;
      vkr_allocator_free(pool->allocator, block, pool_class_size(cls),
                         VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
      block = next;
    }
    pool->free[cls] = NULL;
  }
  pool->cached_bytes = 0u;
}

static void *net_alloc_zero(VkrNetCore *core, uint64_t size) {
  void *memory = vkr_allocator_alloc(core->allocator, size,
                                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (memory) {
    MemZero(memory, size);
  }
  return memory;
}

static void net_free(VkrNetCore *core, void *memory, uint64_t size) {
  if (memory) {
    vkr_allocator_free(core->allocator, memory, size,
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }
}

// =============================================================================
// Small helpers
// =============================================================================

static uint32_t bits_words(uint32_t count) { return (count + 63u) / 64u; }

static bool8_t bit_get(const uint64_t *bits, uint32_t index) {
  return (bits[index / 64u] >> (index % 64u)) & 1u ? true_v : false_v;
}

static void bit_set(uint64_t *bits, uint32_t index) {
  bits[index / 64u] |= 1ull << (index % 64u);
}

static void bit_clear(uint64_t *bits, uint32_t index) {
  bits[index / 64u] &= ~(1ull << (index % 64u));
}

static uint32_t net_slot_of(VkrNetConnectionId cid) { return cid & 0xffffu; }

static NetConnection *net_connection(const VkrNetCore *core,
                                     VkrNetConnectionId cid) {
  const uint32_t slot = net_slot_of(cid);
  if (slot == 0u || slot >= core->slot_count) {
    return NULL;
  }
  NetConnection *conn = core->slots[slot];
  if (!conn || conn->state == NET_STATE_FREE || conn->local_cid != cid) {
    return NULL;
  }
  return conn;
}

static uint64_t net_pto(const NetConnection *conn) {
  const uint64_t srtt = conn->has_rtt ? conn->srtt : NET_INITIAL_RTT_US;
  const uint64_t rttvar = conn->has_rtt ? conn->rttvar : srtt / 2u;
  return srtt + Max(4u * rttvar, NET_GRANULARITY_US) + conn->max_ack_delay_us;
}

/* Bytes of data one fragment carries at the connection's current MTU. */
static uint32_t net_fragment_size(const NetConnection *conn) {
  const uint32_t room = conn->mtu - NET_SHORT_HEADER_MAX - VKR_NET_TAG_SIZE -
                        NET_MESSAGE_HEADER_MAX - NET_FRAGMENT_ACK_ROOM;
  return room & ~15u;
}

// =============================================================================
// Events
// =============================================================================

static NetEventEntry *net_event_push(VkrNetCore *core) {
  if (core->event_count == core->event_capacity) {
    const uint32_t capacity =
        core->event_capacity ? core->event_capacity * 2u : 256u;
    NetEventEntry *events =
        net_alloc_zero(core, (uint64_t)capacity * sizeof(NetEventEntry));
    if (!events) {
      return NULL;
    }
    for (uint32_t i = 0u; i < core->event_count; ++i) {
      events[i] = core->events[(core->event_head + i) % core->event_capacity];
    }
    net_free(core, core->events,
             (uint64_t)core->event_capacity * sizeof(NetEventEntry));
    core->events = events;
    core->event_capacity = capacity;
    core->event_head = 0u;
  }
  NetEventEntry *entry = &core->events[(core->event_head + core->event_count) %
                                       core->event_capacity];
  MemZero(entry, sizeof(*entry));
  core->event_count += 1u;
  return entry;
}

static void net_report_closed(VkrNetCore *core, NetConnection *conn,
                              uint16_t code) {
  if (conn->closed_reported) {
    return;
  }
  conn->closed_reported = true_v;
  NetEventEntry *entry = net_event_push(core);
  if (entry) {
    entry->event.type = VKR_NET_EVENT_CLOSED;
    entry->event.connection = conn->local_cid;
    entry->event.code = code;
  }
}

static void net_report_connected(VkrNetCore *core, NetConnection *conn,
                                 const uint8_t *payload, uint32_t size) {
  NetEventEntry *entry = net_event_push(core);
  if (!entry) {
    return;
  }
  entry->event.type = VKR_NET_EVENT_CONNECTED;
  entry->event.connection = conn->local_cid;
  MemCopy(entry->event.peer_key, conn->peer_key, VKR_NET_KEY_SIZE);
  if (size > 0u) {
    entry->block = pool_alloc(&core->pool, size);
    if (entry->block) {
      MemCopy(entry->block, payload, size);
      entry->block_size = size;
      entry->event.data = entry->block;
      entry->event.size = size;
    }
  }
}

static void net_report_notify(VkrNetCore *core, NetConnection *conn,
                              uint8_t channel, uint64_t tag, bool8_t acked) {
  NetEventEntry *entry = net_event_push(core);
  if (entry) {
    entry->event.type = acked ? VKR_NET_EVENT_ACKED : VKR_NET_EVENT_LOST;
    entry->event.connection = conn->local_cid;
    entry->event.channel = channel;
    entry->event.tag = tag;
    entry->event.has_tag = true_v;
  }
}

// =============================================================================
// Ready list, dirty list and timers
// =============================================================================

static void net_mark_ready(VkrNetCore *core, NetConnection *conn) {
  if (conn->in_ready || conn->state == NET_STATE_FREE) {
    return;
  }
  const uint32_t capacity = core->slot_count;
  core->ready[(core->ready_head + core->ready_count) % capacity] =
      net_slot_of(conn->local_cid);
  core->ready_count += 1u;
  conn->in_ready = true_v;
}

static void net_mark_dirty(VkrNetCore *core, NetConnection *conn) {
  if (conn->in_dirty) {
    return;
  }
  core->dirty[core->dirty_count++] = net_slot_of(conn->local_cid);
  conn->in_dirty = true_v;
}

static void heap_swap(NetTimer *a, NetTimer *b) {
  const NetTimer t = *a;
  *a = *b;
  *b = t;
}

static void heap_push(VkrNetCore *core, NetTimer timer) {
  if (core->heap_count == core->heap_capacity) {
    const uint32_t capacity = core->heap_capacity * 2u;
    NetTimer *heap =
        net_alloc_zero(core, (uint64_t)capacity * sizeof(NetTimer));
    if (!heap) {
      return;
    }
    MemCopy(heap, core->heap, (uint64_t)core->heap_count * sizeof(NetTimer));
    net_free(core, core->heap,
             (uint64_t)core->heap_capacity * sizeof(NetTimer));
    core->heap = heap;
    core->heap_capacity = capacity;
  }
  uint32_t index = core->heap_count++;
  core->heap[index] = timer;
  while (index > 0u) {
    const uint32_t parent = (index - 1u) / 2u;
    if (core->heap[parent].deadline <= core->heap[index].deadline) {
      break;
    }
    heap_swap(&core->heap[parent], &core->heap[index]);
    index = parent;
  }
}

static NetTimer heap_pop(VkrNetCore *core) {
  const NetTimer top = core->heap[0];
  core->heap[0] = core->heap[--core->heap_count];
  uint32_t index = 0u;
  for (;;) {
    const uint32_t left = index * 2u + 1u;
    const uint32_t right = left + 1u;
    uint32_t smallest = index;
    if (left < core->heap_count &&
        core->heap[left].deadline < core->heap[smallest].deadline) {
      smallest = left;
    }
    if (right < core->heap_count &&
        core->heap[right].deadline < core->heap[smallest].deadline) {
      smallest = right;
    }
    if (smallest == index) {
      break;
    }
    heap_swap(&core->heap[smallest], &core->heap[index]);
    index = smallest;
  }
  return top;
}

static bool8_t net_channel_has_data(const NetChannel *channel, uint64_t now);

/* The earliest future time a timer of `conn` fires. Work that is already
   due (an ACK to send, a keep-alive PING, a PMTU probe) waits in the ready
   list instead, so a timer never fires twice for one action. */
static uint64_t net_compute_timer(const NetConnection *conn, uint64_t now) {
  uint64_t timer = UINT64_MAX;
  if (conn->state == NET_STATE_CLOSING || conn->state == NET_STATE_DRAINING) {
    return conn->drain_deadline;
  }
  if (conn->state == NET_STATE_CLIENT_INITIAL ||
      conn->state == NET_STATE_SERVER_HANDSHAKE) {
    timer = Min(timer, conn->hs_deadline);
  }
  if (conn->ack_eliciting_pending > 0u && !conn->ack_now) {
    timer = Min(timer, conn->ack_deadline);
  }
  if (conn->loss_time) {
    timer = Min(timer, conn->loss_time);
  }
  if (conn->ack_eliciting_in_flight > 0u && conn->has_keys) {
    const uint64_t pto = net_pto(conn) << Min(conn->pto_count, 16u);
    timer = Min(timer, conn->last_ack_eliciting_sent + pto);
  }
  if (conn->has_keys && conn->idle_deadline) {
    timer = Min(timer, conn->idle_deadline);
    /* Keep-alive at a third of the idle timeout without sending. */
    if (!conn->ping_pending) {
      const uint64_t idle = conn->idle_deadline - conn->last_rx;
      timer = Min(timer, conn->last_ack_eliciting_sent + idle / 3u);
    }
  }
  if (conn->pacing_release) {
    timer = Min(timer, conn->pacing_release);
  }
  if (conn->state == NET_STATE_ESTABLISHED && conn->probe_target &&
      !conn->probe_in_flight && conn->probe_next_time > now) {
    timer = Min(timer, conn->probe_next_time);
  }
  return timer;
}

static void net_schedule(VkrNetCore *core, NetConnection *conn) {
  conn->timer = net_compute_timer(conn, core->now);
  /* A later timer keeps its earlier heap entry, which fires early and
     schedules again; an earlier one needs a new entry. */
  if (conn->timer != UINT64_MAX && conn->timer < conn->scheduled) {
    conn->scheduled = conn->timer;
    heap_push(core, (NetTimer){.deadline = conn->timer,
                               .slot = net_slot_of(conn->local_cid),
                               .cid = conn->local_cid});
  }
}

// =============================================================================
// Connections
// =============================================================================

static void net_channel_destroy(VkrNetCore *core, NetChannel *channel);

static NetConnection *net_connection_alloc(VkrNetCore *core) {
  if (core->free_count == 0u) {
    return NULL;
  }
  const uint32_t slot = core->free_slots[--core->free_count];
  NetConnection *conn = core->slots[slot];
  if (!conn) {
    conn = net_alloc_zero(core, sizeof(NetConnection));
    if (!conn) {
      core->free_slots[core->free_count++] = slot;
      return NULL;
    }
    core->slots[slot] = conn;
  } else {
    MemZero(conn, sizeof(*conn));
  }
  uint16_t random_bits = 0u;
  vkr_net_random(&random_bits, sizeof(random_bits));
  conn->local_cid = ((uint32_t)(core->config.shard & 0x0fu) << 28) |
                    ((uint32_t)(random_bits & 0x0fffu) << 16) | slot;
  conn->mtu = VKR_NET_DATAGRAM_INITIAL;
  conn->reorder_packets = 3u;
  conn->reorder_time_eighths = 9u;
  conn->max_ack_delay_us = core->config.max_ack_delay_us;
  conn->scheduled = UINT64_MAX;
  core->live_count += 1u;
  return conn;
}

static bool8_t net_connection_alloc_rings(VkrNetCore *core,
                                          NetConnection *conn) {
  const uint32_t capacity = core->config.sent_packet_capacity;
  conn->sent = net_alloc_zero(core, (uint64_t)capacity * sizeof(NetSentPacket));
  conn->records = net_alloc_zero(
      core, (uint64_t)capacity * NET_RECORDS_PER_PACKET * sizeof(NetRecord));
  if (!conn->sent || !conn->records) {
    return false_v;
  }
  conn->sent_mask = capacity - 1u;
  conn->record_mask = capacity * NET_RECORDS_PER_PACKET - 1u;
  return true_v;
}

static void net_handshake_release(VkrNetCore *core, NetConnection *conn) {
  if (conn->noise) {
    vkr_net_noise_wipe(conn->noise);
    net_free(core, conn->noise, sizeof(VkrNetNoise));
    conn->noise = NULL;
  }
  if (conn->hs_bytes) {
    pool_free(&core->pool, conn->hs_bytes, conn->hs_size);
    conn->hs_bytes = NULL;
    conn->hs_size = 0u;
  }
}

static void net_connection_free(VkrNetCore *core, NetConnection *conn) {
  if (conn->state == NET_STATE_SERVER_HANDSHAKE && core->half_open > 0u) {
    core->half_open -= 1u;
  }
  net_handshake_release(core, conn);
  for (uint32_t i = 0u; i < VKR_NET_CHANNEL_MAX; ++i) {
    if (conn->channels[i]) {
      net_channel_destroy(core, conn->channels[i]);
      conn->channels[i] = NULL;
    }
  }
  const uint32_t capacity = core->config.sent_packet_capacity;
  net_free(core, conn->sent, (uint64_t)capacity * sizeof(NetSentPacket));
  net_free(core, conn->records,
           (uint64_t)capacity * NET_RECORDS_PER_PACKET * sizeof(NetRecord));
  conn->sent = NULL;
  conn->records = NULL;
  vkr_net_packet_key_wipe(&conn->tx);
  vkr_net_packet_key_wipe(&conn->rx);
  const uint32_t slot = net_slot_of(conn->local_cid);
  conn->state = NET_STATE_FREE;
  conn->local_cid = 0u;
  core->free_slots[core->free_count++] = slot;
  core->live_count -= 1u;
}

/* Ends a connection now: reports CLOSED and frees it. */
static void net_connection_drop(VkrNetCore *core, NetConnection *conn,
                                uint16_t code) {
  net_report_closed(core, conn, code);
  net_connection_free(core, conn);
}

/* This side closes: CLOSE goes out once, then the drain period. */
static void net_close_local(VkrNetCore *core, NetConnection *conn,
                            uint16_t code, uint64_t now) {
  if (conn->state == NET_STATE_CLOSING || conn->state == NET_STATE_DRAINING) {
    return;
  }
  if (!conn->has_keys) {
    net_connection_drop(core, conn, code);
    return;
  }
  conn->state = NET_STATE_CLOSING;
  conn->close_code = code;
  conn->close_pending = true_v;
  conn->drain_deadline = now + 3u * net_pto(conn);
  net_report_closed(core, conn, code);
  net_mark_ready(core, conn);
  net_schedule(core, conn);
}

static void net_install_keys(NetConnection *conn, const uint8_t tx[32],
                             const uint8_t rx[32]) {
  vkr_net_packet_key_init(&conn->tx, tx);
  vkr_net_packet_key_init(&conn->rx, rx);
  conn->has_keys = true_v;
}

// =============================================================================
// Channels
// =============================================================================

static NetChannel *net_channel_create(VkrNetCore *core,
                                      const VkrNetChannelConfig *config) {
  NetChannel *channel = net_alloc_zero(core, sizeof(NetChannel));
  if (!channel) {
    return NULL;
  }
  channel->config = *config;
  const uint32_t capacity = config->send_queue;
  channel->ring =
      net_alloc_zero(core, (uint64_t)capacity * sizeof(NetSendMessage));
  if (!channel->ring) {
    net_free(core, channel, sizeof(NetChannel));
    return NULL;
  }
  channel->ring_mask = capacity - 1u;
  channel->peer_limit = config->receive_window;
  channel->rx_advertised = config->receive_window;
  return channel;
}

static void net_message_release(VkrNetCore *core, NetChannel *channel,
                                NetSendMessage *message) {
  if (message->data) {
    pool_free(&core->pool, message->data, message->size);
    message->data = NULL;
  }
  if (message->bits && message->bits != message->inline_bits) {
    pool_free(&core->pool, message->bits,
              (uint64_t)message->words * 2u * sizeof(uint64_t));
  }
  message->bits = NULL;
  if (message->state == NET_MESSAGE_QUEUED) {
    channel->queued_bytes -= message->size;
  }
  message->state = NET_MESSAGE_DONE;
}

static uint64_t *net_reassembly_bits(NetReassembly *entry) {
  return entry->heap_bits ? entry->heap_bits : &entry->inline_bits;
}

/* Frees an entry. `delivered` keeps its bytes counted as buffered until the
   application polls the message that took its data. */
static void net_reassembly_release(VkrNetCore *core, NetChannel *channel,
                                   NetReassembly *entry, bool8_t delivered) {
  if (entry->data) {
    pool_free(&core->pool, entry->data, entry->size);
  }
  if (entry->heap_bits) {
    pool_free(&core->pool, entry->heap_bits,
              (uint64_t)bits_words(entry->fragment_count) * sizeof(uint64_t));
  }
  if (!delivered) {
    channel->rx_buffered -= Min(channel->rx_buffered, (uint64_t)entry->size);
  }
  MemZero(entry, sizeof(*entry));
  channel->reassembly_count -= 1u;
}

static void net_channel_destroy(VkrNetCore *core, NetChannel *channel) {
  for (uint64_t seq = channel->head_seq; seq < channel->tail_seq; ++seq) {
    NetSendMessage *message = &channel->ring[seq & channel->ring_mask];
    if (message->state == NET_MESSAGE_QUEUED) {
      net_message_release(core, channel, message);
    }
  }
  if (channel->reassembly) {
    for (uint32_t i = 0u; i <= channel->reassembly_mask; ++i) {
      if (channel->reassembly[i].live) {
        net_reassembly_release(core, channel, &channel->reassembly[i], false_v);
      }
    }
    net_free(core, channel->reassembly,
             (uint64_t)(channel->reassembly_mask + 1u) * sizeof(NetReassembly));
  }
  net_free(core, channel->retransmit,
           (uint64_t)channel->retransmit_capacity * sizeof(NetRetransmit));
  net_free(core, channel->ring,
           (uint64_t)(channel->ring_mask + 1u) * sizeof(NetSendMessage));
  net_free(core, channel, sizeof(NetChannel));
}

static bool8_t net_delivery_reliable(uint8_t delivery) {
  return delivery == VKR_NET_RELIABLE_ORDERED ||
         delivery == VKR_NET_RELIABLE_UNORDERED || delivery == VKR_NET_DEADLINE;
}

static bool8_t net_channel_normalize(VkrNetChannelConfig *config) {
  if (config->delivery >= VKR_NET_DELIVERY_COUNT || config->priority > 7u) {
    return false_v;
  }
  if (config->weight == 0u) {
    config->weight = 1u;
  }
  if (config->max_message_size == 0u) {
    config->max_message_size = 64u * 1024u;
  }
  if (config->max_message_size > VKR_NET_MESSAGE_MAX) {
    return false_v;
  }
  if (!net_delivery_reliable(config->delivery)) {
    /* An unreliable message is one frame of the smallest packet. */
    const uint32_t limit = VKR_NET_DATAGRAM_INITIAL - NET_SHORT_HEADER_MAX -
                           VKR_NET_TAG_SIZE - NET_MESSAGE_HEADER_MAX;
    config->max_message_size = Min(config->max_message_size, limit);
  }
  if (config->receive_window == 0u) {
    const uint64_t window = (uint64_t)config->max_message_size * 4u;
    config->receive_window = (uint32_t)Min(window, (uint64_t)UINT32_MAX);
  }
  if (config->receive_window < config->max_message_size) {
    return false_v;
  }
  if (config->send_queue == 0u) {
    config->send_queue = 1024u;
  }
  if ((config->send_queue & (config->send_queue - 1u)) != 0u ||
      config->send_queue > NET_SEND_QUEUE_MAX) {
    return false_v;
  }
  return true_v;
}

static bool8_t net_open_channel(VkrNetCore *core, NetConnection *conn,
                                uint8_t index,
                                const VkrNetChannelConfig *config) {
  if (index >= VKR_NET_CHANNEL_MAX || conn->channels[index]) {
    return false_v;
  }
  VkrNetChannelConfig normalized = *config;
  if (!net_channel_normalize(&normalized)) {
    return false_v;
  }
  conn->channels[index] = net_channel_create(core, &normalized);
  return conn->channels[index] != NULL;
}

static bool8_t net_open_session_channel(VkrNetCore *core, NetConnection *conn) {
  const VkrNetChannelConfig session = {
      .delivery = VKR_NET_RELIABLE_ORDERED,
      .priority = 0u,
      .weight = 1u,
      .max_message_size = 64u * 1024u,
      .receive_window = 256u * 1024u,
      .send_queue = 1024u,
  };
  return net_open_channel(core, conn, 0u, &session);
}

static bool8_t net_retransmit_push(VkrNetCore *core, NetChannel *channel,
                                   uint64_t seq, uint32_t fragment) {
  if (channel->retransmit_count == channel->retransmit_capacity) {
    const uint32_t capacity =
        channel->retransmit_capacity ? channel->retransmit_capacity * 2u : 64u;
    NetRetransmit *entries =
        net_alloc_zero(core, (uint64_t)capacity * sizeof(NetRetransmit));
    if (!entries) {
      return false_v;
    }
    MemCopy(entries, channel->retransmit,
            (uint64_t)channel->retransmit_count * sizeof(NetRetransmit));
    net_free(core, channel->retransmit,
             (uint64_t)channel->retransmit_capacity * sizeof(NetRetransmit));
    channel->retransmit = entries;
    channel->retransmit_capacity = capacity;
  }
  channel->retransmit[channel->retransmit_count++] =
      (NetRetransmit){.seq = seq, .fragment = fragment};
  return true_v;
}

static NetSendMessage *net_live_message(NetChannel *channel, uint64_t seq) {
  if (seq < channel->head_seq || seq >= channel->tail_seq) {
    return NULL;
  }
  NetSendMessage *message = &channel->ring[seq & channel->ring_mask];
  if (message->seq != seq || message->state != NET_MESSAGE_QUEUED) {
    return NULL;
  }
  return message;
}

static void net_channel_advance_head(VkrNetCore *core, NetChannel *channel) {
  (void)core;
  while (channel->head_seq < channel->tail_seq) {
    NetSendMessage *message =
        &channel->ring[channel->head_seq & channel->ring_mask];
    if (message->state == NET_MESSAGE_QUEUED) {
      break;
    }
    message->state = NET_MESSAGE_FREE;
    channel->head_seq += 1u;
  }
  if (channel->unsent_seq < channel->head_seq) {
    channel->unsent_seq = channel->head_seq;
  }
}

/* Abandons a deadline message: the receiver learns through CANCEL. */
static void net_message_abandon(VkrNetCore *core, NetConnection *conn,
                                NetChannel *channel, uint8_t channel_index,
                                NetSendMessage *message) {
  if (conn->cancel_count < NET_CANCEL_QUEUE) {
    conn->cancels[conn->cancel_count++] = (NetCancel){
        .seq = message->seq, .size = message->size, .channel = channel_index};
  }
  net_message_release(core, channel, message);
  net_channel_advance_head(core, channel);
}

// =============================================================================
// Receive window and delivery
// =============================================================================

static void net_rx_mark_done(NetChannel *channel, uint64_t seq) {
  const uint64_t offset = seq - channel->rx_base_seq;
  bit_set(channel->rx_done, (uint32_t)offset);
  /* Slide the window over the delivered prefix. */
  while (bit_get(channel->rx_done, 0u)) {
    uint32_t shift = 0u;
    while (shift < 64u && bit_get(channel->rx_done, shift)) {
      ++shift;
    }
    const uint32_t words = NET_RX_DONE_WINDOW / 64u;
    for (uint32_t i = 0u; i < words; ++i) {
      const uint64_t next = i + 1u < words ? channel->rx_done[i + 1u] : 0u;
      channel->rx_done[i] = shift == 64u ? next
                                         : (channel->rx_done[i] >> shift) |
                                               (next << (64u - shift));
    }
    channel->rx_base_seq += shift;
  }
}

/* The application took `size` bytes, or the sender cancelled them: count
   them and advertise more room once a quarter of the window was consumed. */
static void net_rx_consume(VkrNetCore *core, NetConnection *conn,
                           NetChannel *channel, uint64_t size) {
  channel->rx_consumed += size;
  const uint64_t limit = channel->rx_consumed + channel->config.receive_window;
  if (limit - channel->rx_advertised >= channel->config.receive_window / 4u) {
    channel->window_dirty = true_v;
    net_mark_ready(core, conn);
  }
}

static bool8_t net_rx_is_done(const NetChannel *channel, uint64_t seq) {
  if (seq < channel->rx_base_seq) {
    return true_v;
  }
  const uint64_t offset = seq - channel->rx_base_seq;
  return offset < NET_RX_DONE_WINDOW &&
         bit_get(channel->rx_done, (uint32_t)offset);
}

/* Queues a MESSAGE event that owns `block`. */
static void net_deliver(VkrNetCore *core, NetConnection *conn,
                        uint8_t channel_index, NetChannel *channel,
                        uint64_t seq, uint8_t *block, uint32_t size,
                        bool8_t has_tag, uint64_t tag) {
  const bool8_t reliable = net_delivery_reliable(channel->config.delivery);
  if (!reliable && core->unreliable_events >= NET_EVENT_UNRELIABLE_MAX) {
    pool_free(&core->pool, block, size);
    return;
  }
  NetEventEntry *entry = net_event_push(core);
  if (!entry) {
    pool_free(&core->pool, block, size);
    return;
  }
  entry->event.type = VKR_NET_EVENT_MESSAGE;
  entry->event.connection = conn->local_cid;
  entry->event.channel = channel_index;
  entry->event.sequence = seq;
  entry->event.tag = tag;
  entry->event.has_tag = has_tag;
  entry->event.data = block;
  entry->event.size = size;
  entry->block = block;
  entry->block_size = size;
  entry->counts = reliable;
  if (!reliable) {
    core->unreliable_events += 1u;
  }
}

static NetReassembly *net_reassembly_find(NetChannel *channel, uint64_t seq) {
  if (!channel->reassembly) {
    return NULL;
  }
  NetReassembly *entry = &channel->reassembly[seq & channel->reassembly_mask];
  return entry->live && entry->seq == seq ? entry : NULL;
}

/* Delivers completed ordered messages from the channel's next sequence. */
static void net_deliver_ordered(VkrNetCore *core, NetConnection *conn,
                                uint8_t channel_index, NetChannel *channel) {
  for (;;) {
    NetReassembly *entry = net_reassembly_find(channel, channel->rx_base_seq);
    if (!entry || !entry->complete) {
      return;
    }
    uint8_t *block = entry->data;
    const uint32_t size = entry->size;
    const uint64_t seq = entry->seq;
    entry->data = NULL;
    net_deliver(core, conn, channel_index, channel, seq, block, size,
                entry->has_tag, entry->tag);
    net_reassembly_release(core, channel, entry, true_v);
    net_rx_mark_done(channel, seq);
  }
}

// =============================================================================
// Acknowledgment ranges and the replay window
// =============================================================================

static bool8_t net_replay_seen(const NetConnection *conn, uint64_t pn) {
  if (!conn->rx_any || pn > conn->rx_largest) {
    return false_v;
  }
  const uint64_t distance = conn->rx_largest - pn;
  if (distance >= NET_REPLAY_WINDOW) {
    return true_v;
  }
  return bit_get(conn->rx_window, (uint32_t)distance);
}

static void net_replay_record(NetConnection *conn, uint64_t pn, uint64_t now) {
  const uint32_t words = NET_REPLAY_WINDOW / 64u;
  if (!conn->rx_any) {
    conn->rx_any = true_v;
    conn->rx_largest = pn;
    conn->rx_largest_time = now;
    MemZero(conn->rx_window, sizeof(conn->rx_window));
    bit_set(conn->rx_window, 0u);
    return;
  }
  if (pn > conn->rx_largest) {
    const uint64_t shift = pn - conn->rx_largest;
    if (shift >= NET_REPLAY_WINDOW) {
      MemZero(conn->rx_window, sizeof(conn->rx_window));
    } else {
      /* Bit k is rx_largest - k: shift toward higher indices. */
      const uint32_t word_shift = (uint32_t)(shift / 64u);
      const uint32_t bit_shift = (uint32_t)(shift % 64u);
      for (int32_t i = (int32_t)words - 1; i >= 0; --i) {
        const int32_t source = i - (int32_t)word_shift;
        uint64_t value = 0u;
        if (source >= 0) {
          value = conn->rx_window[source] << bit_shift;
          if (bit_shift && source >= 1) {
            value |= conn->rx_window[source - 1] >> (64u - bit_shift);
          }
        }
        conn->rx_window[i] = value;
      }
    }
    conn->rx_largest = pn;
    conn->rx_largest_time = now;
    bit_set(conn->rx_window, 0u);
    return;
  }
  bit_set(conn->rx_window, (uint32_t)(conn->rx_largest - pn));
}

static void net_ack_insert(NetConnection *conn, uint64_t pn) {
  if (pn <= conn->ack_floor && conn->ack_floor != 0u) {
    return;
  }
  uint32_t i = 0u;
  while (i < conn->range_count && conn->ranges[i].lo > pn + 1u) {
    ++i;
  }
  if (i < conn->range_count) {
    NetAckRange *range = &conn->ranges[i];
    if (pn >= range->lo && pn <= range->hi) {
      return;
    }
    if (pn == range->hi + 1u) {
      range->hi = pn;
      /* Merge with the newer neighbour when the gap closed. */
      if (i > 0u && conn->ranges[i - 1u].lo == pn + 1u) {
        conn->ranges[i - 1u].lo = range->lo;
        MemCopy(&conn->ranges[i], &conn->ranges[i + 1u],
                (uint64_t)(conn->range_count - i - 1u) * sizeof(NetAckRange));
        conn->range_count -= 1u;
      }
      conn->ack_ranges_dirty = true_v;
      return;
    }
    if (pn + 1u == range->lo) {
      range->lo = pn;
      if (i + 1u < conn->range_count && conn->ranges[i + 1u].hi + 1u == pn) {
        range->lo = conn->ranges[i + 1u].lo;
        MemCopy(&conn->ranges[i + 1u], &conn->ranges[i + 2u],
                (uint64_t)(conn->range_count - i - 2u) * sizeof(NetAckRange));
        conn->range_count -= 1u;
      }
      conn->ack_ranges_dirty = true_v;
      return;
    }
  }
  /* A new range at `i`; the oldest range leaves when the list is full. */
  if (conn->range_count == NET_ACK_RANGES_MAX) {
    if (i >= NET_ACK_RANGES_MAX) {
      return;
    }
    conn->range_count -= 1u;
  }
  MemCopy(&conn->ranges[i + 1u], &conn->ranges[i],
          (uint64_t)(conn->range_count - i) * sizeof(NetAckRange));
  conn->ranges[i] = (NetAckRange){.lo = pn, .hi = pn};
  conn->range_count += 1u;
  conn->ack_ranges_dirty = true_v;
}

/* The peer holds an ACK that reported up to `largest`: stop repeating it. */
static void net_ack_prune(NetConnection *conn, uint64_t largest) {
  if (largest <= conn->ack_floor && conn->ack_floor != 0u) {
    return;
  }
  conn->ack_floor = largest;
  while (conn->range_count > 0u &&
         conn->ranges[conn->range_count - 1u].hi <= largest) {
    conn->range_count -= 1u;
  }
  if (conn->range_count > 0u &&
      conn->ranges[conn->range_count - 1u].lo <= largest) {
    conn->ranges[conn->range_count - 1u].lo = largest + 1u;
  }
}

// =============================================================================
// Recovery: RTT, acknowledged and lost packets
// =============================================================================

static void net_update_rtt(NetConnection *conn, uint64_t sample,
                           uint64_t ack_delay) {
  conn->latest_rtt = sample;
  if (!conn->has_rtt) {
    conn->has_rtt = true_v;
    conn->min_rtt = sample;
    conn->srtt = sample;
    conn->rttvar = sample / 2u;
    return;
  }
  conn->min_rtt = Min(conn->min_rtt, sample);
  uint64_t adjusted = sample;
  ack_delay = Min(ack_delay, conn->max_ack_delay_us);
  if (sample >= conn->min_rtt + ack_delay) {
    adjusted = sample - ack_delay;
  }
  const uint64_t difference =
      conn->srtt > adjusted ? conn->srtt - adjusted : adjusted - conn->srtt;
  conn->rttvar = (3u * conn->rttvar + difference) / 4u;
  conn->srtt = (7u * conn->srtt + adjusted) / 8u;
}

static void net_record_fragment_acked(VkrNetCore *core, NetConnection *conn,
                                      const NetRecord *record) {
  NetChannel *channel = conn->channels[record->channel];
  if (!channel) {
    return;
  }
  NetSendMessage *message = net_live_message(channel, record->seq);
  if (!message || record->fragment >= message->fragment_count) {
    return;
  }
  if (bit_get(message->bits, record->fragment)) {
    return;
  }
  bit_set(message->bits, record->fragment);
  message->acked_count += 1u;
  if (message->acked_count == message->fragment_count) {
    net_message_release(core, channel, message);
    net_channel_advance_head(core, channel);
  }
}

static void net_record_lost(VkrNetCore *core, NetConnection *conn,
                            const NetRecord *record) {
  NetChannel *channel = conn->channels[record->channel];
  switch (record->type) {
  case NET_RECORD_FRAGMENT: {
    if (!channel) {
      return;
    }
    NetSendMessage *message = net_live_message(channel, record->seq);
    if (!message || record->fragment >= message->fragment_count) {
      return;
    }
    uint64_t *pending = message->bits + message->words;
    if (bit_get(message->bits, record->fragment) ||
        bit_get(pending, record->fragment)) {
      return;
    }
    if (net_retransmit_push(core, channel, record->seq, record->fragment)) {
      bit_set(pending, record->fragment);
    }
    return;
  }
  case NET_RECORD_NOTIFY:
    net_report_notify(core, conn, record->channel, record->seq, false_v);
    return;
  case NET_RECORD_WINDOW:
    if (channel) {
      channel->window_dirty = true_v;
    }
    return;
  case NET_RECORD_CANCEL:
    if (conn->cancel_count < NET_CANCEL_QUEUE) {
      conn->cancels[conn->cancel_count++] =
          (NetCancel){.seq = record->seq,
                      .size = record->fragment,
                      .channel = record->channel};
    }
    return;
  default:
    return;
  }
}

static void net_packet_records(VkrNetCore *core, NetConnection *conn,
                               const NetSentPacket *packet, bool8_t acked) {
  for (uint32_t i = 0u; i < packet->record_count; ++i) {
    const NetRecord *record =
        &conn->records[(packet->record_first + i) & conn->record_mask];
    if (acked) {
      if (record->type == NET_RECORD_FRAGMENT) {
        net_record_fragment_acked(core, conn, record);
      } else if (record->type == NET_RECORD_NOTIFY) {
        net_report_notify(core, conn, record->channel, record->seq, true_v);
      }
    } else {
      net_record_lost(core, conn, record);
    }
  }
}

static void net_packet_retire(NetConnection *conn, NetSentPacket *packet) {
  if (packet->flags & NET_PACKET_IN_FLIGHT) {
    conn->bytes_in_flight -= Min(conn->bytes_in_flight, (uint64_t)packet->size);
  }
  if (packet->flags & NET_PACKET_ACK_ELICITING) {
    conn->ack_eliciting_in_flight -= Min(conn->ack_eliciting_in_flight, 1u);
  }
  packet->flags = 0u;
}

static void net_sent_advance_tail(NetConnection *conn) {
  while (conn->sent_tail < conn->next_pn) {
    const NetSentPacket *packet =
        &conn->sent[conn->sent_tail & conn->sent_mask];
    if (packet->pn == conn->sent_tail && (packet->flags & NET_PACKET_LIVE)) {
      break;
    }
    conn->sent_tail += 1u;
  }
  if (conn->sent_tail < conn->next_pn) {
    conn->record_tail =
        conn->sent[conn->sent_tail & conn->sent_mask].record_first;
  } else {
    conn->record_tail = conn->record_head;
  }
}

static void net_mtu_probe_lost(NetConnection *conn, uint64_t now) {
  conn->probe_in_flight = false_v;
  conn->probe_failures += 1u;
  if (conn->probe_failures >= NET_MTU_PROBE_FAILURES) {
    conn->probe_failures = 0u;
    conn->probe_next_time = now + NET_MTU_PROBE_GIVE_UP_US;
  } else {
    conn->probe_next_time = now + NET_MTU_PROBE_RETRY_US;
  }
}

static void net_detect_lost(VkrNetCore *core, NetConnection *conn,
                            uint64_t now) {
  conn->loss_time = 0u;
  if (!conn->any_acked) {
    return;
  }
  const uint64_t rtt = Max(conn->latest_rtt, conn->srtt);
  const uint64_t delay =
      Max(rtt * conn->reorder_time_eighths / 8u, NET_GRANULARITY_US);
  uint64_t lost_bytes = 0u;
  for (uint64_t pn = conn->sent_tail; pn < conn->largest_acked; ++pn) {
    NetSentPacket *packet = &conn->sent[pn & conn->sent_mask];
    if (packet->pn != pn || !(packet->flags & NET_PACKET_LIVE)) {
      continue;
    }
    const bool8_t by_count = conn->largest_acked - pn >= conn->reorder_packets;
    const bool8_t by_time = packet->time_sent + delay <= now;
    if (!by_count && !by_time) {
      const uint64_t when = packet->time_sent + delay;
      conn->loss_time = conn->loss_time ? Min(conn->loss_time, when) : when;
      continue;
    }
    if (packet->flags & NET_PACKET_MTU_PROBE) {
      net_mtu_probe_lost(conn, now);
    } else if (packet->flags & NET_PACKET_IN_FLIGHT) {
      lost_bytes += packet->size;
      conn->packets_lost += 1u;
    }
    net_packet_records(core, conn, packet, false_v);
    net_packet_retire(conn, packet);
    packet->flags = NET_PACKET_DECLARED_LOST;
  }
  if (lost_bytes > 0u) {
    vkr_net_cc_on_loss(&conn->cc, lost_bytes);
  }
  net_sent_advance_tail(conn);
}

typedef struct NetAckState {
  bool8_t any;
  uint64_t newly_acked;
  uint64_t largest_newly_acked;
  const NetSentPacket *newest;
  NetSentPacket newest_copy;
  bool8_t largest_ack_eliciting;
  uint64_t largest_time_sent;
} NetAckState;

static void net_on_packet_acked(VkrNetCore *core, NetConnection *conn,
                                NetSentPacket *packet, uint64_t now,
                                NetAckState *state) {
  if (packet->flags & NET_PACKET_HAS_ACK) {
    net_ack_prune(conn, packet->ack_largest);
  }
  if (packet->flags & NET_PACKET_MTU_PROBE) {
    conn->mtu = packet->size;
    conn->probe_in_flight = false_v;
    conn->probe_failures = 0u;
    vkr_net_cc_set_mss(&conn->cc, conn->mtu);
    conn->probe_next_time = now;
    if (conn->probe_target <= conn->mtu) {
      conn->probe_target = conn->probe_target < core->config.max_datagram
                               ? core->config.max_datagram
                               : 0u;
    }
  }
  if (packet->flags & NET_PACKET_IN_FLIGHT) {
    conn->delivered += packet->size;
    conn->delivered_time = now;
    state->newly_acked += packet->size;
    if (!state->newest || packet->pn > state->newest_copy.pn) {
      state->newest_copy = *packet;
      state->newest = &state->newest_copy;
    }
  }
  if (!state->any || packet->pn >= state->largest_newly_acked) {
    state->any = true_v;
    state->largest_newly_acked = packet->pn;
    state->largest_ack_eliciting =
        (packet->flags & NET_PACKET_ACK_ELICITING) ? true_v : false_v;
    state->largest_time_sent = packet->time_sent;
  }
  net_packet_records(core, conn, packet, true_v);
  net_packet_retire(conn, packet);
}

// =============================================================================
// Packet construction
// =============================================================================

typedef struct NetBuilder {
  uint8_t *start;
  uint8_t *cursor;
  uint8_t *end;
  uint64_t record_first;
  uint32_t record_count;
  bool8_t ack_eliciting;
  bool8_t has_ack;
  uint64_t ack_largest;
  bool8_t has_data;
} NetBuilder;

static bool8_t builder_record(NetConnection *conn, NetBuilder *builder,
                              NetRecord record) {
  const uint64_t used = conn->record_head - conn->record_tail;
  if (used + 1u > (uint64_t)conn->record_mask + 1u ||
      builder->record_count >= 0xffffu) {
    return false_v;
  }
  conn->records[conn->record_head & conn->record_mask] = record;
  conn->record_head += 1u;
  builder->record_count += 1u;
  return true_v;
}

static bool8_t builder_room(const NetBuilder *builder, uint64_t size) {
  return (uint64_t)(builder->end - builder->cursor) >= size;
}

static uint32_t net_ack_frame_size_estimate(const NetConnection *conn) {
  return 1u + 8u + 4u + 2u + 4u +
         (conn->range_count > 1u ? (conn->range_count - 1u) * 4u : 0u);
}

static bool8_t net_write_ack(NetConnection *conn, NetBuilder *builder,
                             uint64_t now) {
  if (conn->range_count == 0u) {
    return false_v;
  }
  uint8_t *cursor = builder->cursor;
  const uint8_t *end = builder->end;
  const uint64_t largest = conn->ranges[0].hi;
  const uint64_t delay =
      now > conn->rx_largest_time ? (now - conn->rx_largest_time) >> 3 : 0u;

  uint8_t header[32];
  uint32_t header_size = 0u;
  header[header_size++] = FRAME_ACK;
  header_size +=
      vkr_net_varint_write(header + header_size, header + 32, largest);
  header_size += vkr_net_varint_write(header + header_size, header + 32, delay);

  /* Ranges that fit: count, first range, then gap and length pairs. */
  uint8_t body[NET_ACK_RANGES_MAX * 16u];
  uint32_t body_size = 0u;
  uint32_t written = 1u;
  body_size += vkr_net_varint_write(body, body + sizeof(body),
                                    conn->ranges[0].hi - conn->ranges[0].lo);
  for (uint32_t i = 1u; i < conn->range_count; ++i) {
    const uint64_t gap = conn->ranges[i - 1u].lo - conn->ranges[i].hi - 2u;
    const uint64_t length = conn->ranges[i].hi - conn->ranges[i].lo;
    uint8_t pair[16];
    uint32_t pair_size = vkr_net_varint_write(pair, pair + 16, gap);
    pair_size += vkr_net_varint_write(pair + pair_size, pair + 16, length);
    if (header_size + 8u + body_size + pair_size > (uint64_t)(end - cursor)) {
      break;
    }
    MemCopy(body + body_size, pair, pair_size);
    body_size += pair_size;
    written += 1u;
  }
  uint8_t count[8];
  const uint32_t count_size =
      vkr_net_varint_write(count, count + 8, (uint64_t)written - 1u);
  if (header_size + count_size + body_size > (uint64_t)(end - cursor)) {
    return false_v;
  }
  MemCopy(cursor, header, header_size);
  cursor += header_size;
  MemCopy(cursor, count, count_size);
  cursor += count_size;
  MemCopy(cursor, body, body_size);
  cursor += body_size;
  builder->cursor = cursor;
  builder->has_ack = true_v;
  builder->ack_largest = largest;
  conn->ack_ranges_dirty = false_v;
  conn->ack_now = false_v;
  conn->ack_eliciting_pending = 0u;
  return true_v;
}

static bool8_t net_write_simple(NetBuilder *builder, uint8_t type) {
  if (!builder_room(builder, 1u)) {
    return false_v;
  }
  *builder->cursor++ = type;
  return true_v;
}

static bool8_t net_write_window(NetConnection *conn, NetBuilder *builder,
                                uint8_t index, NetChannel *channel) {
  const uint64_t limit = channel->rx_consumed + channel->config.receive_window;
  if (!builder_room(builder, 1u + 1u + 8u)) {
    return false_v;
  }
  NetRecord record = {.type = NET_RECORD_WINDOW, .channel = index};
  if (!builder_record(conn, builder, record)) {
    return false_v;
  }
  uint8_t *cursor = builder->cursor;
  *cursor++ = FRAME_WINDOW;
  *cursor++ = index;
  cursor += vkr_net_varint_write(cursor, builder->end, limit);
  builder->cursor = cursor;
  builder->ack_eliciting = true_v;
  channel->rx_advertised = limit;
  channel->window_dirty = false_v;
  return true_v;
}

static bool8_t net_write_cancel(NetConnection *conn, NetBuilder *builder,
                                const NetCancel *cancel) {
  if (!builder_room(builder, 1u + 1u + 8u + 4u)) {
    return false_v;
  }
  NetRecord record = {.type = NET_RECORD_CANCEL,
                      .channel = cancel->channel,
                      .seq = cancel->seq,
                      .fragment = cancel->size};
  if (!builder_record(conn, builder, record)) {
    return false_v;
  }
  uint8_t *cursor = builder->cursor;
  *cursor++ = FRAME_CANCEL;
  *cursor++ = cancel->channel;
  cursor += vkr_net_varint_write(cursor, builder->end, cancel->seq);
  cursor += vkr_net_varint_write(cursor, builder->end, cancel->size);
  builder->cursor = cursor;
  builder->ack_eliciting = true_v;
  return true_v;
}

/* Writes one MESSAGE frame for `fragment` of `message` (or the whole
   message when it is not fragmented). False when it does not fit. */
static bool8_t net_write_message(NetConnection *conn, NetBuilder *builder,
                                 uint8_t index, NetChannel *channel,
                                 NetSendMessage *message, uint32_t fragment) {
  const uint8_t delivery = channel->config.delivery;
  const bool8_t reliable = net_delivery_reliable(delivery);
  const bool8_t fragmented = message->fragment_size != 0u;
  const bool8_t sequenced = reliable || delivery == VKR_NET_SEQUENCED;
  const bool8_t tagged = (message->flags & VKR_NET_SEND_TAG) != 0u;

  uint32_t offset = 0u;
  uint32_t length = message->size;
  if (fragmented) {
    offset = fragment * message->fragment_size;
    length = Min(message->fragment_size, message->size - offset);
  }

  uint8_t flags = MESSAGE_LEN;
  uint32_t header = 1u + 1u + vkr_net_varint_size(length);
  if (sequenced) {
    flags |= MESSAGE_SEQ;
    header += vkr_net_varint_size(message->seq);
  }
  if (fragmented) {
    flags |= MESSAGE_FRAG;
    header += vkr_net_varint_size(fragment) +
              vkr_net_varint_size(message->fragment_size) +
              vkr_net_varint_size(message->size);
  }
  if (tagged) {
    flags |= MESSAGE_TAG;
    header += vkr_net_varint_size(message->tag);
  }
  if (!builder_room(builder, (uint64_t)header + length)) {
    return false_v;
  }
  if (reliable) {
    NetRecord record = {.type = NET_RECORD_FRAGMENT,
                        .channel = index,
                        .seq = message->seq,
                        .fragment = fragment};
    if (!builder_record(conn, builder, record)) {
      return false_v;
    }
  } else if (message->flags & VKR_NET_SEND_NOTIFY) {
    NetRecord record = {
        .type = NET_RECORD_NOTIFY, .channel = index, .seq = message->tag};
    if (!builder_record(conn, builder, record)) {
      return false_v;
    }
  }

  uint8_t *cursor = builder->cursor;
  const uint8_t *end = builder->end;
  *cursor++ = (uint8_t)(FRAME_MESSAGE | flags);
  *cursor++ = index;
  if (sequenced) {
    cursor += vkr_net_varint_write(cursor, end, message->seq);
  }
  if (fragmented) {
    cursor += vkr_net_varint_write(cursor, end, fragment);
    cursor += vkr_net_varint_write(cursor, end, message->fragment_size);
    cursor += vkr_net_varint_write(cursor, end, message->size);
  }
  if (tagged) {
    cursor += vkr_net_varint_write(cursor, end, message->tag);
  }
  cursor += vkr_net_varint_write(cursor, end, length);
  MemCopy(cursor, message->data + offset, length);
  cursor += length;
  builder->cursor = cursor;
  builder->ack_eliciting = true_v;
  builder->has_data = true_v;
  return true_v;
}

static bool8_t net_message_expired(const NetSendMessage *message,
                                   uint64_t now) {
  return message->deadline_us != 0u && now > message->deadline_us;
}

/* Fills the packet from one channel, up to `budget` bytes of frames.
   Returns the bytes written. */
static uint64_t net_fill_channel(VkrNetCore *core, NetConnection *conn,
                                 NetBuilder *builder, uint8_t index,
                                 NetChannel *channel, uint64_t budget,
                                 uint64_t now) {
  const uint8_t *before = builder->cursor;
  const bool8_t reliable = net_delivery_reliable(channel->config.delivery);

  /* Retransmissions first. */
  uint32_t kept = 0u;
  uint32_t i = 0u;
  for (; i < channel->retransmit_count; ++i) {
    const NetRetransmit entry = channel->retransmit[i];
    NetSendMessage *message = net_live_message(channel, entry.seq);
    if (!message || bit_get(message->bits, entry.fragment)) {
      continue;
    }
    if (net_message_expired(message, now)) {
      net_message_abandon(core, conn, channel, index, message);
      continue;
    }
    if ((uint64_t)(builder->cursor - before) >= budget ||
        !net_write_message(conn, builder, index, channel, message,
                           entry.fragment)) {
      break;
    }
    bit_clear(message->bits + message->words, entry.fragment);
  }
  for (; i < channel->retransmit_count; ++i) {
    channel->retransmit[kept++] = channel->retransmit[i];
  }
  channel->retransmit_count = kept;
  if (kept > 0u) {
    return (uint64_t)(builder->cursor - before);
  }

  /* New data in sequence order. */
  while (channel->unsent_seq < channel->tail_seq &&
         (uint64_t)(builder->cursor - before) < budget) {
    const uint64_t seq = channel->unsent_seq;
    NetSendMessage *message = net_live_message(channel, seq);
    if (!message) {
      channel->unsent_seq = seq + 1u;
      continue;
    }
    if (net_message_expired(message, now)) {
      /* Abandoning may advance the head, and the cursor with it. */
      net_message_abandon(core, conn, channel, index, message);
      channel->unsent_seq = Max(channel->unsent_seq, seq + 1u);
      continue;
    }
    if (reliable && message->next_fragment == 0u &&
        message->offset + message->size > channel->peer_limit) {
      break;
    }
    if (!net_write_message(conn, builder, index, channel, message,
                           message->next_fragment)) {
      break;
    }
    message->next_fragment += 1u;
    if (message->next_fragment >= message->fragment_count) {
      channel->unsent_seq = seq + 1u;
      if (!reliable) {
        net_message_release(core, channel, message);
        net_channel_advance_head(core, channel);
      }
    }
  }
  return (uint64_t)(builder->cursor - before);
}

static bool8_t net_channel_has_data(const NetChannel *channel, uint64_t now) {
  if (channel->retransmit_count > 0u) {
    return true_v;
  }
  if (channel->unsent_seq >= channel->tail_seq) {
    return false_v;
  }
  if (net_delivery_reliable(channel->config.delivery)) {
    const NetSendMessage *message =
        &channel->ring[channel->unsent_seq & channel->ring_mask];
    /* An expired message still needs a pass that abandons it, even when
       the window holds it back. */
    if (message->state == NET_MESSAGE_QUEUED && message->next_fragment == 0u &&
        message->offset + message->size > channel->peer_limit &&
        !net_message_expired(message, now)) {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t net_has_data(const NetConnection *conn, uint64_t now) {
  for (uint32_t i = 0u; i < VKR_NET_CHANNEL_MAX; ++i) {
    const NetChannel *channel = conn->channels[i];
    if (channel && net_channel_has_data(channel, now)) {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t net_has_control(const NetConnection *conn) {
  if (conn->response_pending || conn->challenge_pending ||
      conn->cancel_count > 0u || conn->ping_pending ||
      conn->probes_pending > 0u) {
    return true_v;
  }
  for (uint32_t i = 0u; i < VKR_NET_CHANNEL_MAX; ++i) {
    if (conn->channels[i] && conn->channels[i]->window_dirty) {
      return true_v;
    }
  }
  return false_v;
}

/* Fills data frames by priority level, deficit round-robin by weight inside
   a level. */
static void net_fill_data(VkrNetCore *core, NetConnection *conn,
                          NetBuilder *builder, uint64_t now) {
  for (uint32_t level = 0u; level < 8u; ++level) {
    bool8_t any = true_v;
    while (any && builder_room(builder, 4u)) {
      any = false_v;
      for (uint32_t step = 0u; step < VKR_NET_CHANNEL_MAX; ++step) {
        const uint32_t index =
            (conn->rr_cursor[level] + step) % VKR_NET_CHANNEL_MAX;
        NetChannel *channel = conn->channels[index];
        if (!channel || channel->config.priority != level ||
            !net_channel_has_data(channel, now)) {
          continue;
        }
        const uint64_t quantum = (uint64_t)channel->config.weight * 1200u;
        const uint64_t written = net_fill_channel(
            core, conn, builder, (uint8_t)index, channel, quantum, now);
        if (written > 0u) {
          any = true_v;
          conn->rr_cursor[level] = (index + 1u) % VKR_NET_CHANNEL_MAX;
        }
        if (!builder_room(builder, 4u)) {
          break;
        }
      }
    }
    if (!builder_room(builder, 4u)) {
      return;
    }
  }
}

static uint32_t net_pn_length(const NetConnection *conn) {
  const uint64_t base = conn->any_acked ? conn->largest_acked : 0u;
  const uint64_t distance = (conn->next_pn - base + 1u) * 2u;
  if (distance < (1ull << 8)) {
    return 1u;
  }
  if (distance < (1ull << 16)) {
    return 2u;
  }
  if (distance < (1ull << 24)) {
    return 3u;
  }
  return 4u;
}

/* Builds one 1-RTT packet. Returns its size or zero. */
static uint32_t net_build_packet(VkrNetCore *core, NetConnection *conn,
                                 uint64_t now, uint8_t *buffer,
                                 uint32_t capacity) {
  if (!conn->has_keys || conn->state == NET_STATE_DRAINING) {
    return 0u;
  }
  if (conn->next_pn - conn->sent_tail > conn->sent_mask) {
    return 0u;
  }

  uint32_t max_size = Min(capacity, conn->mtu);
  if (!conn->validated) {
    const uint64_t allowance = conn->amp_received * 3u;
    if (conn->amp_sent >= allowance) {
      return 0u;
    }
    max_size = (uint32_t)Min((uint64_t)max_size, allowance - conn->amp_sent);
    if (max_size < NET_SHORT_HEADER_MAX + VKR_NET_TAG_SIZE + 8u) {
      return 0u;
    }
  }

  /* PMTU probe: PING and padding up to the target size. */
  bool8_t probe = false_v;
  if (conn->state == NET_STATE_ESTABLISHED && conn->probe_target &&
      !conn->probe_in_flight && now >= conn->probe_next_time &&
      conn->probe_target <= capacity && conn->validated &&
      conn->bytes_in_flight + conn->probe_target <= conn->cc.cwnd) {
    probe = true_v;
    max_size = conn->probe_target;
  }

  const uint32_t pn_length = net_pn_length(conn);
  const uint64_t pn = conn->next_pn;
  uint8_t *header = buffer;
  header[0] = (uint8_t)(NET_FLAG_FIXED | (pn_length - 1u));
  vkr_store_le_u32(header + 1, conn->remote_cid);
  for (uint32_t i = 0u; i < pn_length; ++i) {
    header[5u + i] = (uint8_t)(pn >> (8u * i));
  }
  const uint32_t header_size = 5u + pn_length;

  NetBuilder builder = {
      .start = buffer + header_size,
      .cursor = buffer + header_size,
      .end = buffer + max_size - VKR_NET_TAG_SIZE,
      .record_first = conn->record_head,
  };

  if (conn->state == NET_STATE_CLOSING) {
    if (!conn->close_pending) {
      return 0u;
    }
    uint8_t *cursor = builder.cursor;
    *cursor++ = FRAME_CLOSE;
    cursor += vkr_net_varint_write(cursor, builder.end, conn->close_code);
    cursor += vkr_net_varint_write(cursor, builder.end, 0u);
    builder.cursor = cursor;
    conn->close_pending = false_v;
  } else if (probe) {
    *builder.cursor++ = FRAME_PING;
    MemZero(builder.cursor, (uint64_t)(builder.end - builder.cursor));
    builder.cursor = builder.end;
    builder.ack_eliciting = true_v;
  } else {
    const bool8_t want_ack =
        conn->range_count > 0u &&
        (conn->ack_now ||
         (conn->ack_eliciting_pending > 0u && now >= conn->ack_deadline));
    const bool8_t cwnd_open =
        conn->bytes_in_flight + conn->mtu <= conn->cc.cwnd ||
        conn->probes_pending > 0u;

    /* Pacing: refill the token bucket at the pacing rate. */
    const uint64_t burst = (uint64_t)conn->mtu * NET_PACING_BURST_PACKETS;
    if (now > conn->pacing_stamp) {
      const uint64_t elapsed = now - conn->pacing_stamp;
      const uint64_t refill = conn->cc.pacing_rate * elapsed / 1000000u;
      conn->pacing_tokens =
          (int64_t)Min((uint64_t)Max(conn->pacing_tokens, 0) + refill, burst);
      conn->pacing_stamp = now;
    }
    const bool8_t paced = conn->pacing_tokens >= (int64_t)conn->mtu ||
                          conn->bytes_in_flight == 0u ||
                          conn->probes_pending > 0u;
    const bool8_t data_waiting = net_has_data(conn, now);
    const bool8_t may_send_data = cwnd_open && paced;

    if (!want_ack && !net_has_control(conn) &&
        !(data_waiting && may_send_data)) {
      if (data_waiting && cwnd_open && !paced && conn->cc.pacing_rate > 0u) {
        const uint64_t missing =
            (uint64_t)((int64_t)conn->mtu - conn->pacing_tokens);
        conn->pacing_release =
            now + Max(missing * 1000000u / conn->cc.pacing_rate, 1u);
      }
      if (!data_waiting && conn->bytes_in_flight < conn->cc.cwnd) {
        conn->app_limited_until = conn->delivered + conn->bytes_in_flight;
      }
      return 0u;
    }
    conn->pacing_release = 0u;

    if (conn->range_count > 0u && (want_ack || conn->ack_ranges_dirty) &&
        builder_room(&builder, net_ack_frame_size_estimate(conn))) {
      (void)net_write_ack(conn, &builder, now);
    }
    if (conn->response_pending && builder_room(&builder, 9u)) {
      *builder.cursor++ = FRAME_PATH_RESPONSE;
      MemCopy(builder.cursor, conn->response, 8u);
      builder.cursor += 8u;
      builder.ack_eliciting = true_v;
      conn->response_pending = false_v;
    }
    if (conn->challenge_pending && builder_room(&builder, 9u)) {
      *builder.cursor++ = FRAME_PATH_CHALLENGE;
      MemCopy(builder.cursor, conn->challenge, 8u);
      builder.cursor += 8u;
      builder.ack_eliciting = true_v;
      conn->challenge_pending = false_v;
      conn->challenge_waiting = true_v;
    }
    for (uint32_t i = 0u; i < VKR_NET_CHANNEL_MAX; ++i) {
      NetChannel *channel = conn->channels[i];
      if (channel && channel->window_dirty &&
          !net_write_window(conn, &builder, (uint8_t)i, channel)) {
        break;
      }
    }
    uint32_t cancels_written = 0u;
    while (cancels_written < conn->cancel_count &&
           net_write_cancel(conn, &builder, &conn->cancels[cancels_written])) {
      ++cancels_written;
    }
    if (cancels_written > 0u) {
      MemCopy(conn->cancels, conn->cancels + cancels_written,
              (uint64_t)(conn->cancel_count - cancels_written) *
                  sizeof(NetCancel));
      conn->cancel_count -= cancels_written;
    }
    if (may_send_data && data_waiting) {
      net_fill_data(core, conn, &builder, now);
    }
    if ((conn->ping_pending || conn->probes_pending > 0u) &&
        !builder.ack_eliciting && builder_room(&builder, 1u)) {
      (void)net_write_simple(&builder, FRAME_PING);
      builder.ack_eliciting = true_v;
    }
    if (builder.ack_eliciting) {
      conn->ping_pending = false_v;
      if (conn->probes_pending > 0u) {
        conn->probes_pending -= 1u;
      }
    }
  }

  if (builder.cursor == builder.start) {
    return 0u;
  }

  const uint32_t payload_size = (uint32_t)(builder.cursor - builder.start);
  const uint32_t packet_size = header_size + payload_size + VKR_NET_TAG_SIZE;
  vkr_net_packet_seal(&conn->tx, pn, header, header_size, builder.start,
                      payload_size, builder.cursor);

  NetSentPacket *sent = &conn->sent[pn & conn->sent_mask];
  *sent = (NetSentPacket){
      .pn = pn,
      .time_sent = now,
      .size = packet_size,
      .record_first = builder.record_first,
      .record_count = (uint16_t)builder.record_count,
      .ack_largest = builder.ack_largest,
      .flags = NET_PACKET_LIVE,
  };
  if (builder.has_ack) {
    sent->flags |= NET_PACKET_HAS_ACK;
  }
  if (builder.ack_eliciting) {
    if (conn->ack_eliciting_in_flight == 0u) {
      conn->first_sent_time = now;
      conn->delivered_time = now;
    }
    sent->flags |= NET_PACKET_ACK_ELICITING | NET_PACKET_IN_FLIGHT;
    sent->delivered = conn->delivered;
    sent->delivered_time = conn->delivered_time;
    sent->first_sent_time = conn->first_sent_time;
    if (conn->app_limited_until > conn->delivered) {
      sent->flags |= NET_PACKET_APP_LIMITED;
    }
    conn->bytes_in_flight += packet_size;
    conn->ack_eliciting_in_flight += 1u;
    conn->last_ack_eliciting_sent = now;
    if (!probe) {
      conn->pacing_tokens -= (int64_t)packet_size;
    }
  }
  if (probe) {
    sent->flags |= NET_PACKET_MTU_PROBE;
    conn->probe_in_flight = true_v;
  }
  conn->next_pn += 1u;
  conn->packets_sent += 1u;
  conn->bytes_sent += packet_size;
  conn->amp_sent += packet_size;
  return packet_size;
}

// =============================================================================
// Handshake packets
// =============================================================================

static uint32_t net_write_long_header(uint8_t *out, uint32_t type,
                                      uint32_t dcid, uint32_t scid) {
  out[0] =
      (uint8_t)(NET_FLAG_LONG | NET_FLAG_FIXED | (type << NET_LONG_TYPE_SHIFT));
  vkr_store_le_u32(out + 1, VKR_NET_PROTOCOL_VERSION);
  vkr_store_le_u32(out + 5, dcid);
  vkr_store_le_u32(out + 9, scid);
  return NET_LONG_HEADER_SIZE;
}

/* The client's Initial: header, token, the stored first Noise message and
   zero padding to VKR_NET_DATAGRAM_INITIAL bytes. */
static uint32_t net_build_initial(NetConnection *conn, uint8_t *buffer,
                                  uint32_t capacity) {
  if (capacity < VKR_NET_DATAGRAM_INITIAL) {
    return 0u;
  }
  uint8_t *cursor = buffer;
  const uint8_t *end = buffer + VKR_NET_DATAGRAM_INITIAL;
  cursor += net_write_long_header(cursor, NET_LONG_INITIAL, conn->initial_dcid,
                                  conn->local_cid);
  const uint32_t token_size = conn->has_token ? VKR_NET_COOKIE_SIZE : 0u;
  cursor += vkr_net_varint_write(cursor, end, token_size);
  MemCopy(cursor, conn->token, token_size);
  cursor += token_size;
  cursor += vkr_net_varint_write(cursor, end, conn->hs_size);
  MemCopy(cursor, conn->hs_bytes, conn->hs_size);
  cursor += conn->hs_size;
  MemZero(cursor, (uint64_t)(end - cursor));
  return VKR_NET_DATAGRAM_INITIAL;
}

static void net_queue_stateless(VkrNetCore *core, const VkrNetAddress *address,
                                const uint8_t *bytes, uint32_t size) {
  if (core->stateless_count == NET_STATELESS_QUEUE ||
      size > NET_STATELESS_SIZE) {
    return;
  }
  NetStateless *entry =
      &core->stateless[(core->stateless_head + core->stateless_count) %
                       NET_STATELESS_QUEUE];
  entry->address = *address;
  entry->size = size;
  MemCopy(entry->bytes, bytes, size);
  core->stateless_count += 1u;
}

/* The bytes a retry cookie binds: the address and the client's ID. */
static uint32_t net_cookie_subject(const VkrNetAddress *address, uint32_t scid,
                                   uint8_t out[32]) {
  out[0] = address->family;
  vkr_store_le_u32(out + 1, address->port);
  MemCopy(out + 5, address->bytes, 16u);
  vkr_store_le_u32(out + 21, scid);
  return 25u;
}

/* Server: msg2 in a Handshake packet. `status` zero accepts. */
static uint32_t net_write_handshake(VkrNetCore *core, VkrNetNoise *noise,
                                    uint32_t dcid, uint32_t scid, uint16_t code,
                                    const VkrNetAcceptResult *result,
                                    uint8_t *out, uint32_t capacity) {
  uint8_t payload[3u + VKR_NET_ACCEPT_PAYLOAD_MAX];
  uint32_t payload_size = 0u;
  payload[payload_size++] = code == 0u ? 0u : 1u;
  payload[payload_size++] = (uint8_t)(code & 0xffu);
  payload[payload_size++] = (uint8_t)(code >> 8);
  if (code == 0u && result) {
    MemCopy(payload + payload_size, result->payload, result->payload_size);
    payload_size += result->payload_size;
  }
  uint8_t *cursor = out;
  const uint8_t *end = out + capacity;
  cursor += net_write_long_header(cursor, NET_LONG_HANDSHAKE, dcid, scid);
  uint8_t message[VKR_NET_NOISE_MESSAGE2_OVERHEAD + sizeof(payload)];
  const uint32_t message_size = vkr_net_noise_write_message2(
      noise, NULL, payload, payload_size, message, sizeof(message));
  if (message_size == 0u) {
    return 0u;
  }
  const uint32_t length = vkr_net_varint_write(cursor, end, message_size);
  if (length == 0u || (uint64_t)(end - cursor - length) < message_size) {
    return 0u;
  }
  cursor += length;
  MemCopy(cursor, message, message_size);
  cursor += message_size;
  (void)core;
  return (uint32_t)(cursor - out);
}

static NetConnection *net_find_handshake(VkrNetCore *core,
                                         const VkrNetAddress *address,
                                         uint32_t client_cid) {
  for (uint32_t slot = 1u; slot < core->slot_count; ++slot) {
    NetConnection *conn = core->slots[slot];
    if (conn && conn->state != NET_STATE_FREE && !conn->is_client &&
        conn->remote_cid == client_cid &&
        vkr_net_address_equal(&conn->peer, address)) {
      return conn;
    }
  }
  return NULL;
}

static void net_receive_initial(VkrNetCore *core, const VkrNetAddress *from,
                                uint8_t *bytes, uint32_t size, uint64_t now) {
  if (!core->config.accept_incoming || size < VKR_NET_DATAGRAM_INITIAL) {
    return;
  }
  const uint8_t *cursor = bytes + NET_LONG_HEADER_SIZE;
  const uint8_t *end = bytes + size;
  const uint32_t client_cid = vkr_load_le_u32(bytes + 9);
  uint64_t token_size = 0u;
  uint32_t used = vkr_net_varint_read(cursor, end, &token_size);
  if (used == 0u || token_size > (uint64_t)(end - cursor - used)) {
    return;
  }
  cursor += used;
  const uint8_t *token = cursor;
  cursor += token_size;
  uint64_t message_size = 0u;
  used = vkr_net_varint_read(cursor, end, &message_size);
  if (used == 0u || message_size > (uint64_t)(end - cursor - used)) {
    return;
  }
  cursor += used;
  const uint8_t *message = cursor;

  /* A repeated Initial: the Handshake was lost; send it again. */
  NetConnection *existing = net_find_handshake(core, from, client_cid);
  if (existing) {
    if (existing->state == NET_STATE_SERVER_HANDSHAKE && existing->hs_bytes) {
      existing->hs_send_pending = true_v;
      existing->amp_received += size;
      net_mark_ready(core, existing);
    }
    return;
  }

  uint8_t subject[32];
  const uint32_t subject_size = net_cookie_subject(from, client_cid, subject);
  const uint32_t now_seconds = (uint32_t)(now / 1000000u);
  bool8_t cookie_valid = false_v;
  if (token_size == VKR_NET_COOKIE_SIZE) {
    cookie_valid =
        vkr_net_cookie_check(core->cookie_secret, subject, subject_size,
                             now_seconds, NET_COOKIE_LIFETIME_S, token);
  }
  const bool8_t need_cookie = core->config.require_retry ||
                              core->half_open >= core->config.retry_threshold;
  if (need_cookie && !cookie_valid) {
    uint8_t retry[NET_LONG_HEADER_SIZE + VKR_NET_COOKIE_SIZE];
    (void)net_write_long_header(retry, NET_LONG_RETRY, client_cid, 0u);
    vkr_net_cookie_make(core->cookie_secret, subject, subject_size, now_seconds,
                        retry + NET_LONG_HEADER_SIZE);
    net_queue_stateless(core, from, retry, sizeof(retry));
    return;
  }

  VkrNetNoise noise;
  vkr_net_noise_init(&noise, false_v, net_prologue, sizeof(net_prologue) - 1u,
                     &core->config.static_keys, NULL);
  uint32_t credential_size = 0u;
  if (!vkr_net_noise_read_message1(&noise, message, (uint32_t)message_size,
                                   core->scratch, sizeof(core->scratch),
                                   &credential_size)) {
    vkr_net_noise_wipe(&noise);
    return;
  }

  VkrNetAcceptResult result = {0};
  if (core->config.accept) {
    core->config.accept(core->config.accept_context, from, noise.rs,
                        core->scratch, credential_size, &result);
  }
  NetConnection *conn = NULL;
  if (result.code == 0u) {
    conn = net_connection_alloc(core);
    if (!conn) {
      result.code = VKR_NET_CLOSE_SERVER_FULL;
    } else if (!net_connection_alloc_rings(core, conn) ||
               !net_open_session_channel(core, conn)) {
      net_connection_free(core, conn);
      conn = NULL;
      result.code = VKR_NET_CLOSE_INTERNAL;
    }
  }

  uint8_t packet[NET_STATELESS_SIZE];
  const uint32_t packet_size =
      net_write_handshake(core, &noise, client_cid, conn ? conn->local_cid : 0u,
                          result.code, &result, packet, sizeof(packet));
  if (packet_size == 0u || !conn) {
    if (packet_size > 0u) {
      net_queue_stateless(core, from, packet, packet_size);
    }
    vkr_net_noise_wipe(&noise);
    return;
  }

  uint8_t initiator_key[VKR_NET_KEY_SIZE];
  uint8_t responder_key[VKR_NET_KEY_SIZE];
  vkr_net_noise_split(&noise, initiator_key, responder_key);
  net_install_keys(conn, responder_key, initiator_key);
  vkr_net_random(initiator_key, sizeof(initiator_key));
  vkr_net_random(responder_key, sizeof(responder_key));

  conn->state = NET_STATE_SERVER_HANDSHAKE;
  conn->is_client = false_v;
  conn->remote_cid = client_cid;
  conn->peer = *from;
  conn->validated = cookie_valid;
  conn->amp_received = size;
  conn->user = result.user;
  MemCopy(conn->peer_key, noise.rs, VKR_NET_KEY_SIZE);
  vkr_net_noise_wipe(&noise);
  conn->hs_bytes = pool_alloc(&core->pool, packet_size);
  if (conn->hs_bytes) {
    MemCopy(conn->hs_bytes, packet, packet_size);
    conn->hs_size = packet_size;
  }
  conn->hs_send_pending = true_v;
  conn->hs_started = now;
  conn->hs_deadline = now + (uint64_t)core->config.handshake_timeout_ms * 1000u;
  conn->last_rx = now;
  conn->last_ack_eliciting_sent = now;
  conn->idle_deadline = now + (uint64_t)core->config.idle_timeout_ms * 1000u;
  vkr_net_cc_init(&conn->cc, conn->mtu, core->config.background, now);
  conn->probe_target =
      core->config.max_datagram > conn->mtu
          ? Min(core->config.max_datagram, VKR_NET_DATAGRAM_ETHERNET)
          : 0u;
  core->half_open += 1u;
  net_report_connected(core, conn, NULL, 0u);
  net_mark_ready(core, conn);
  net_schedule(core, conn);
}

static void net_receive_handshake(VkrNetCore *core, const VkrNetAddress *from,
                                  uint8_t *bytes, uint32_t size, uint64_t now) {
  const uint32_t dcid = vkr_load_le_u32(bytes + 5);
  NetConnection *conn = net_connection(core, dcid);
  if (!conn || conn->state != NET_STATE_CLIENT_INITIAL ||
      !vkr_net_address_equal(&conn->peer, from)) {
    return;
  }
  const uint32_t server_cid = vkr_load_le_u32(bytes + 9);
  const uint8_t *cursor = bytes + NET_LONG_HEADER_SIZE;
  const uint8_t *end = bytes + size;
  uint64_t message_size = 0u;
  const uint32_t used = vkr_net_varint_read(cursor, end, &message_size);
  if (used == 0u || message_size > (uint64_t)(end - cursor - used)) {
    return;
  }
  cursor += used;

  /* Decrypt into a copy so a forged packet leaves the state intact. */
  VkrNetNoise trial = *conn->noise;
  uint8_t payload[3u + VKR_NET_ACCEPT_PAYLOAD_MAX];
  uint32_t payload_size = 0u;
  if (!vkr_net_noise_read_message2(&trial, cursor, (uint32_t)message_size,
                                   payload, sizeof(payload), &payload_size) ||
      payload_size < 3u) {
    vkr_net_noise_wipe(&trial);
    return;
  }
  const uint16_t code = (uint16_t)(payload[1] | (payload[2] << 8));
  if (payload[0] != 0u) {
    vkr_net_noise_wipe(&trial);
    net_connection_drop(core, conn,
                        code ? code : (uint16_t)VKR_NET_CLOSE_ACCESS_DENIED);
    return;
  }

  uint8_t initiator_key[VKR_NET_KEY_SIZE];
  uint8_t responder_key[VKR_NET_KEY_SIZE];
  vkr_net_noise_split(&trial, initiator_key, responder_key);
  vkr_net_noise_wipe(&trial);
  if (!net_connection_alloc_rings(core, conn) ||
      !net_open_session_channel(core, conn)) {
    net_connection_drop(core, conn, VKR_NET_CLOSE_INTERNAL);
    return;
  }
  net_install_keys(conn, initiator_key, responder_key);
  vkr_net_random(initiator_key, sizeof(initiator_key));
  vkr_net_random(responder_key, sizeof(responder_key));
  net_handshake_release(core, conn);

  conn->state = NET_STATE_ESTABLISHED;
  conn->remote_cid = server_cid;
  conn->validated = true_v;
  conn->last_rx = now;
  conn->last_ack_eliciting_sent = now;
  conn->idle_deadline = now + (uint64_t)core->config.idle_timeout_ms * 1000u;
  conn->bytes_received += size;
  vkr_net_cc_init(&conn->cc, conn->mtu, core->config.background, now);
  conn->probe_target =
      core->config.max_datagram > conn->mtu
          ? Min(core->config.max_datagram, VKR_NET_DATAGRAM_ETHERNET)
          : 0u;
  conn->probe_next_time = now;
  /* The server learns the handshake completed from this packet. */
  conn->ping_pending = true_v;
  net_report_connected(core, conn, payload + 3, payload_size - 3u);
  net_mark_ready(core, conn);
  net_schedule(core, conn);
}

static void net_receive_retry(VkrNetCore *core, const VkrNetAddress *from,
                              uint8_t *bytes, uint32_t size, uint64_t now) {
  const uint32_t dcid = vkr_load_le_u32(bytes + 5);
  NetConnection *conn = net_connection(core, dcid);
  if (!conn || conn->state != NET_STATE_CLIENT_INITIAL ||
      !vkr_net_address_equal(&conn->peer, from) ||
      size != NET_LONG_HEADER_SIZE + VKR_NET_COOKIE_SIZE ||
      conn->retries >= NET_RETRY_MAX) {
    return;
  }
  MemCopy(conn->token, bytes + NET_LONG_HEADER_SIZE, VKR_NET_COOKIE_SIZE);
  conn->has_token = true_v;
  conn->retries += 1u;
  conn->hs_send_pending = true_v;
  conn->hs_interval = NET_HANDSHAKE_RETRY_US;
  conn->hs_deadline = now + conn->hs_interval;
  net_mark_ready(core, conn);
  net_schedule(core, conn);
}

// =============================================================================
// Frame processing
// =============================================================================

typedef struct NetFrameResult {
  bool8_t ack_eliciting;
  bool8_t violation;
  uint16_t close_code;
} NetFrameResult;

static bool8_t net_process_ack(VkrNetCore *core, NetConnection *conn,
                               const uint8_t **cursor_io, const uint8_t *end,
                               uint64_t now) {
  const uint8_t *cursor = *cursor_io;
  uint64_t largest = 0u;
  uint64_t delay = 0u;
  uint64_t count = 0u;
  uint64_t first = 0u;
  uint32_t used = 0u;
  if ((used = vkr_net_varint_read(cursor, end, &largest)) == 0u) {
    return false_v;
  }
  cursor += used;
  if ((used = vkr_net_varint_read(cursor, end, &delay)) == 0u) {
    return false_v;
  }
  cursor += used;
  if ((used = vkr_net_varint_read(cursor, end, &count)) == 0u) {
    return false_v;
  }
  cursor += used;
  if ((used = vkr_net_varint_read(cursor, end, &first)) == 0u) {
    return false_v;
  }
  cursor += used;
  if (largest >= conn->next_pn || first > largest ||
      count > NET_ACK_RANGES_MAX * 4u) {
    return false_v;
  }

  NetAckState state = {0};
  uint64_t hi = largest;
  uint64_t lo = largest - first;
  const uint64_t floor = conn->sent_tail > NET_SPURIOUS_WINDOW
                             ? conn->sent_tail - NET_SPURIOUS_WINDOW
                             : 0u;
  for (uint64_t range = 0u;; ++range) {
    const uint64_t from = Max(lo, floor);
    for (uint64_t pn = from; pn <= hi && hi >= floor; ++pn) {
      NetSentPacket *packet = &conn->sent[pn & conn->sent_mask];
      if (packet->pn == pn && (packet->flags & NET_PACKET_LIVE)) {
        net_on_packet_acked(core, conn, packet, now, &state);
      } else if (packet->pn == pn &&
                 (packet->flags & NET_PACKET_DECLARED_LOST)) {
        /* It arrived after all: the path reorders. Widen both thresholds
           to the distance seen. */
        const uint64_t distance = largest - pn + 1u;
        conn->reorder_packets =
            (uint32_t)Min(Max((uint64_t)conn->reorder_packets, distance), 256u);
        conn->reorder_time_eighths = Min(conn->reorder_time_eighths + 1u, 16u);
        packet->flags = 0u;
      }
      if (pn == UINT64_MAX) {
        break;
      }
    }
    if (range == count) {
      break;
    }
    uint64_t gap = 0u;
    uint64_t length = 0u;
    if ((used = vkr_net_varint_read(cursor, end, &gap)) == 0u) {
      return false_v;
    }
    cursor += used;
    if ((used = vkr_net_varint_read(cursor, end, &length)) == 0u) {
      return false_v;
    }
    cursor += used;
    if (lo < gap + 2u || lo - gap - 2u < length) {
      return false_v;
    }
    hi = lo - gap - 2u;
    lo = hi - length;
  }
  *cursor_io = cursor;

  const bool8_t newly = state.any;
  if (newly && (!conn->any_acked || largest > conn->largest_acked)) {
    conn->largest_acked = largest;
    conn->any_acked = true_v;
    if (state.largest_newly_acked == largest && state.largest_ack_eliciting) {
      net_update_rtt(conn, now - state.largest_time_sent, delay << 3);
    }
  }
  if (!newly) {
    return true_v;
  }
  conn->pto_count = 0u;
  net_detect_lost(core, conn, now);

  if (state.newest) {
    const NetSentPacket *packet = state.newest;
    const uint64_t send_elapsed = packet->time_sent - packet->first_sent_time;
    const uint64_t ack_elapsed = conn->delivered_time - packet->delivered_time;
    const uint64_t interval = Max(send_elapsed, ack_elapsed);
    const uint64_t delivered = conn->delivered - packet->delivered;
    VkrNetCcSample sample = {
        .now_us = now,
        .delivered = conn->delivered,
        .packet_delivered = packet->delivered,
        .newly_acked = state.newly_acked,
        .rate = interval > 0u ? delivered * 1000000u / interval : 0u,
        .rate_app_limited = (packet->flags & NET_PACKET_APP_LIMITED) != 0u,
        .rtt_us = state.largest_ack_eliciting && conn->has_rtt
                      ? conn->latest_rtt
                      : 0u,
        .srtt_us = conn->srtt,
        .bytes_in_flight = conn->bytes_in_flight,
    };
    if (interval < conn->min_rtt && conn->has_rtt) {
      /* Intervals shorter than the RTT overstate the rate. */
      sample.rate = 0u;
    }
    vkr_net_cc_on_ack(&conn->cc, &sample);
    conn->first_sent_time = packet->time_sent;
    /* PROBE_RTT drains on purpose; its low rates must not count. */
    if (conn->cc.mode == VKR_NET_CC_PROBE_RTT) {
      conn->app_limited_until =
          Max(conn->app_limited_until,
              conn->delivered + conn->bytes_in_flight + 1u);
    }
  }
  net_sent_advance_tail(conn);
  return true_v;
}

/* Doubles the table until every live entry and `seq` map to distinct
   slots. Live sequences span less than NET_RX_DONE_WINDOW, which bounds the
   table at that size. */
static bool8_t net_reassembly_reserve(VkrNetCore *core, NetChannel *channel,
                                      uint64_t seq) {
  uint32_t capacity = channel->reassembly ? channel->reassembly_mask + 1u
                                          : NET_REASSEMBLY_INITIAL;
  if (channel->reassembly) {
    const NetReassembly *slot =
        &channel->reassembly[seq & channel->reassembly_mask];
    if (!slot->live) {
      return true_v;
    }
    capacity *= 2u;
  }
  for (; capacity <= NET_RX_DONE_WINDOW; capacity *= 2u) {
    const uint32_t mask = capacity - 1u;
    NetReassembly *table =
        net_alloc_zero(core, (uint64_t)capacity * sizeof(NetReassembly));
    if (!table) {
      return false_v;
    }
    bool8_t fits = true_v;
    if (channel->reassembly) {
      for (uint32_t i = 0u; i <= channel->reassembly_mask; ++i) {
        const NetReassembly *old = &channel->reassembly[i];
        if (!old->live) {
          continue;
        }
        NetReassembly *slot = &table[old->seq & mask];
        if (slot->live || (old->seq & mask) == (seq & mask)) {
          fits = false_v;
          break;
        }
        *slot = *old;
      }
    }
    if (!fits) {
      net_free(core, table, (uint64_t)capacity * sizeof(NetReassembly));
      continue;
    }
    if (channel->reassembly) {
      net_free(core, channel->reassembly,
               (uint64_t)(channel->reassembly_mask + 1u) *
                   sizeof(NetReassembly));
    }
    channel->reassembly = table;
    channel->reassembly_mask = mask;
    return true_v;
  }
  return false_v;
}

static NetReassembly *net_reassembly_add(VkrNetCore *core, NetChannel *channel,
                                         uint64_t seq, uint32_t size,
                                         uint32_t fragment_size,
                                         uint32_t fragment_count) {
  if (!net_reassembly_reserve(core, channel, seq)) {
    return NULL;
  }
  NetReassembly *entry = &channel->reassembly[seq & channel->reassembly_mask];
  MemZero(entry, sizeof(*entry));
  entry->data = pool_alloc(&core->pool, size);
  if (!entry->data) {
    return NULL;
  }
  const uint32_t words = bits_words(fragment_count);
  if (words > 1u) {
    entry->heap_bits =
        pool_alloc(&core->pool, (uint64_t)words * sizeof(uint64_t));
    if (!entry->heap_bits) {
      pool_free(&core->pool, entry->data, size);
      entry->data = NULL;
      return NULL;
    }
    MemZero(entry->heap_bits, (uint64_t)words * sizeof(uint64_t));
  }
  entry->seq = seq;
  entry->size = size;
  entry->fragment_size = fragment_size;
  entry->fragment_count = fragment_count;
  entry->live = true_v;
  channel->reassembly_count += 1u;
  channel->rx_buffered += size;
  return entry;
}

/* A completed reliable message: ordered channels wait for their turn,
   others deliver now. */
static void net_reliable_complete(VkrNetCore *core, NetConnection *conn,
                                  uint8_t index, NetChannel *channel,
                                  NetReassembly *entry) {
  entry->complete = true_v;
  if (channel->config.delivery == VKR_NET_RELIABLE_ORDERED) {
    net_deliver_ordered(core, conn, index, channel);
    return;
  }
  uint8_t *block = entry->data;
  const uint32_t size = entry->size;
  const uint64_t seq = entry->seq;
  entry->data = NULL;
  net_deliver(core, conn, index, channel, seq, block, size, entry->has_tag,
              entry->tag);
  net_reassembly_release(core, channel, entry, true_v);
  net_rx_mark_done(channel, seq);
}

static bool8_t net_process_message(VkrNetCore *core, NetConnection *conn,
                                   uint8_t type, const uint8_t **cursor_io,
                                   const uint8_t *end) {
  const uint8_t flags = type & 0x3fu;
  const uint8_t *cursor = *cursor_io;
  if (cursor >= end || (flags & 0x30u)) {
    return false_v;
  }
  const uint8_t index = *cursor++;
  if (index >= VKR_NET_CHANNEL_MAX) {
    return false_v;
  }
  NetChannel *channel = conn->channels[index];

  uint64_t seq = 0u;
  uint64_t fragment = 0u;
  uint64_t fragment_size = 0u;
  uint64_t total = 0u;
  uint64_t tag = 0u;
  uint64_t length = 0u;
  uint32_t used = 0u;
  if (flags & MESSAGE_SEQ) {
    if ((used = vkr_net_varint_read(cursor, end, &seq)) == 0u) {
      return false_v;
    }
    cursor += used;
  }
  if (flags & MESSAGE_FRAG) {
    if ((used = vkr_net_varint_read(cursor, end, &fragment)) == 0u) {
      return false_v;
    }
    cursor += used;
    if ((used = vkr_net_varint_read(cursor, end, &fragment_size)) == 0u) {
      return false_v;
    }
    cursor += used;
    if ((used = vkr_net_varint_read(cursor, end, &total)) == 0u) {
      return false_v;
    }
    cursor += used;
  }
  if (flags & MESSAGE_TAG) {
    if ((used = vkr_net_varint_read(cursor, end, &tag)) == 0u) {
      return false_v;
    }
    cursor += used;
  }
  if (flags & MESSAGE_LEN) {
    if ((used = vkr_net_varint_read(cursor, end, &length)) == 0u) {
      return false_v;
    }
    cursor += used;
  } else {
    length = (uint64_t)(end - cursor);
  }
  if (length > (uint64_t)(end - cursor)) {
    return false_v;
  }
  const uint8_t *data = cursor;
  *cursor_io = cursor + length;
  const bool8_t has_tag = (flags & MESSAGE_TAG) != 0u;

  /* A channel this side closed may still receive the peer's data in flight:
     drop it. */
  if (!channel) {
    return true_v;
  }
  const uint8_t delivery = channel->config.delivery;
  const bool8_t reliable = net_delivery_reliable(delivery);
  if (!(flags & MESSAGE_SEQ) && (reliable || delivery == VKR_NET_SEQUENCED)) {
    return false_v;
  }
  if ((flags & MESSAGE_FRAG) && !reliable) {
    return false_v;
  }

  if (!reliable) {
    if (length > channel->config.max_message_size) {
      return false_v;
    }
    if (delivery == VKR_NET_SEQUENCED) {
      if (channel->rx_any && seq <= channel->rx_newest_seq) {
        return true_v;
      }
      channel->rx_any = true_v;
      channel->rx_newest_seq = seq;
    }
    uint8_t *block = pool_alloc(&core->pool, length);
    if (!block) {
      return true_v;
    }
    MemCopy(block, data, length);
    net_deliver(core, conn, index, channel, seq, block, (uint32_t)length,
                has_tag, tag);
    return true_v;
  }

  /* Reliable classes: duplicates and messages past the window. */
  if (net_rx_is_done(channel, seq)) {
    return true_v;
  }
  if (seq - channel->rx_base_seq >= NET_RX_DONE_WINDOW) {
    return false_v;
  }

  if (!(flags & MESSAGE_FRAG)) {
    if (length > channel->config.max_message_size) {
      return false_v;
    }
    if (net_reassembly_find(channel, seq)) {
      return true_v;
    }
    if (channel->rx_buffered + length >
        (uint64_t)channel->config.receive_window +
            channel->config.max_message_size) {
      return false_v;
    }
    NetReassembly *entry =
        net_reassembly_add(core, channel, seq, (uint32_t)length, 0u, 1u);
    if (!entry) {
      return false_v;
    }
    MemCopy(entry->data, data, length);
    entry->received = 1u;
    entry->has_tag = has_tag;
    entry->tag = tag;
    net_reliable_complete(core, conn, index, channel, entry);
    return true_v;
  }

  if (total == 0u || total > channel->config.max_message_size ||
      fragment_size == 0u || fragment_size > total) {
    return false_v;
  }
  const uint64_t count = (total + fragment_size - 1u) / fragment_size;
  if (fragment >= count) {
    return false_v;
  }
  const uint64_t offset = fragment * fragment_size;
  const uint64_t expected = Min(fragment_size, total - offset);
  if (length != expected) {
    return false_v;
  }
  NetReassembly *entry = net_reassembly_find(channel, seq);
  if (!entry) {
    if (channel->rx_buffered + total >
        (uint64_t)channel->config.receive_window +
            channel->config.max_message_size) {
      return false_v;
    }
    entry = net_reassembly_add(core, channel, seq, (uint32_t)total,
                               (uint32_t)fragment_size, (uint32_t)count);
    if (!entry) {
      return false_v;
    }
  } else if (entry->size != total || entry->fragment_size != fragment_size) {
    return false_v;
  }
  uint64_t *received = net_reassembly_bits(entry);
  if (entry->complete || bit_get(received, (uint32_t)fragment)) {
    return true_v;
  }
  bit_set(received, (uint32_t)fragment);
  MemCopy(entry->data + offset, data, length);
  entry->received += 1u;
  if (has_tag) {
    entry->has_tag = true_v;
    entry->tag = tag;
  }
  if (entry->received == entry->fragment_count) {
    net_reliable_complete(core, conn, index, channel, entry);
  }
  return true_v;
}

static bool8_t net_process_cancel(VkrNetCore *core, NetConnection *conn,
                                  const uint8_t **cursor_io,
                                  const uint8_t *end) {
  const uint8_t *cursor = *cursor_io;
  if (cursor >= end) {
    return false_v;
  }
  const uint8_t index = *cursor++;
  uint64_t seq = 0u;
  uint64_t size = 0u;
  uint32_t used = 0u;
  if ((used = vkr_net_varint_read(cursor, end, &seq)) == 0u) {
    return false_v;
  }
  cursor += used;
  if ((used = vkr_net_varint_read(cursor, end, &size)) == 0u) {
    return false_v;
  }
  cursor += used;
  *cursor_io = cursor;
  if (index >= VKR_NET_CHANNEL_MAX) {
    return false_v;
  }
  NetChannel *channel = conn->channels[index];
  if (!channel) {
    return true_v;
  }
  if (channel->config.delivery != VKR_NET_DEADLINE) {
    return false_v;
  }
  if (net_rx_is_done(channel, seq)) {
    return true_v;
  }
  if (seq - channel->rx_base_seq >= NET_RX_DONE_WINDOW) {
    return false_v;
  }
  NetReassembly *entry = net_reassembly_find(channel, seq);
  if (entry) {
    net_reassembly_release(core, channel, entry, false_v);
  }
  /* The sender counted the bytes; count them as consumed here too. */
  net_rx_consume(core, conn, channel, size);
  net_rx_mark_done(channel, seq);
  return true_v;
}

static void net_process_frames(VkrNetCore *core, NetConnection *conn,
                               const uint8_t *cursor, const uint8_t *end,
                               uint64_t now, NetFrameResult *result) {
  while (cursor < end) {
    const uint8_t type = *cursor++;
    if (type == FRAME_PADDING) {
      continue;
    }
    if ((type & 0xc0u) == FRAME_MESSAGE) {
      if (!net_process_message(core, conn, type, &cursor, end)) {
        result->violation = true_v;
        return;
      }
      result->ack_eliciting = true_v;
      continue;
    }
    if ((type & 0xc0u) == FRAME_EXTENSION) {
      uint64_t length = 0u;
      const uint32_t used = vkr_net_varint_read(cursor, end, &length);
      if (used == 0u || length > (uint64_t)(end - cursor - used)) {
        result->violation = true_v;
        return;
      }
      cursor += used + length;
      result->ack_eliciting = true_v;
      continue;
    }
    switch (type) {
    case FRAME_PING:
      result->ack_eliciting = true_v;
      break;
    case FRAME_ACK:
      if (!net_process_ack(core, conn, &cursor, end, now)) {
        result->violation = true_v;
        return;
      }
      break;
    case FRAME_WINDOW: {
      if (cursor >= end) {
        result->violation = true_v;
        return;
      }
      const uint8_t index = *cursor++;
      uint64_t limit = 0u;
      const uint32_t used = vkr_net_varint_read(cursor, end, &limit);
      if (used == 0u || index >= VKR_NET_CHANNEL_MAX) {
        result->violation = true_v;
        return;
      }
      cursor += used;
      if (conn->channels[index]) {
        conn->channels[index]->peer_limit =
            Max(conn->channels[index]->peer_limit, limit);
      }
      result->ack_eliciting = true_v;
      break;
    }
    case FRAME_BLOCKED: {
      uint64_t limit = 0u;
      if (cursor >= end) {
        result->violation = true_v;
        return;
      }
      cursor += 1u;
      const uint32_t used = vkr_net_varint_read(cursor, end, &limit);
      if (used == 0u) {
        result->violation = true_v;
        return;
      }
      cursor += used;
      result->ack_eliciting = true_v;
      break;
    }
    case FRAME_CANCEL:
      if (!net_process_cancel(core, conn, &cursor, end)) {
        result->violation = true_v;
        return;
      }
      result->ack_eliciting = true_v;
      break;
    case FRAME_PATH_CHALLENGE:
      if ((uint64_t)(end - cursor) < 8u) {
        result->violation = true_v;
        return;
      }
      MemCopy(conn->response, cursor, 8u);
      conn->response_pending = true_v;
      cursor += 8u;
      result->ack_eliciting = true_v;
      break;
    case FRAME_PATH_RESPONSE:
      if ((uint64_t)(end - cursor) < 8u) {
        result->violation = true_v;
        return;
      }
      if (conn->challenge_waiting &&
          vkr_net_equal_secret(cursor, conn->challenge, 8u)) {
        conn->challenge_waiting = false_v;
        conn->validated = true_v;
      }
      cursor += 8u;
      result->ack_eliciting = true_v;
      break;
    case FRAME_CLOSE: {
      uint64_t code = 0u;
      uint64_t frame = 0u;
      uint32_t used = vkr_net_varint_read(cursor, end, &code);
      if (used == 0u) {
        result->violation = true_v;
        return;
      }
      cursor += used;
      used = vkr_net_varint_read(cursor, end, &frame);
      if (used == 0u) {
        result->violation = true_v;
        return;
      }
      cursor += used;
      result->close_code = (uint16_t)Min(code, 0xffffu);
      if (result->close_code == 0u) {
        result->close_code = VKR_NET_CLOSE_APPLICATION;
      }
      return;
    }
    default:
      result->violation = true_v;
      return;
    }
  }
}

static void net_receive_short(VkrNetCore *core, const VkrNetAddress *from,
                              uint8_t *bytes, uint32_t size, uint64_t now) {
  if (size < NET_SHORT_HEADER_MIN + VKR_NET_TAG_SIZE + 1u ||
      (bytes[0] & NET_SHORT_RESERVED_MASK)) {
    return;
  }
  const uint32_t dcid = vkr_load_le_u32(bytes + 1);
  NetConnection *conn = net_connection(core, dcid);
  if (!conn || !conn->has_keys || conn->state == NET_STATE_DRAINING) {
    return;
  }
  const uint32_t pn_length = (bytes[0] & NET_SHORT_PN_MASK) + 1u;
  const uint32_t header_size = 5u + pn_length;
  if (size < header_size + VKR_NET_TAG_SIZE + 1u) {
    return;
  }
  uint64_t truncated = 0u;
  for (uint32_t i = 0u; i < pn_length; ++i) {
    truncated |= (uint64_t)bytes[5u + i] << (8u * i);
  }
  const uint64_t largest = conn->rx_any ? conn->rx_largest : UINT64_MAX;
  const uint64_t pn =
      vkr_net_packet_number_decode(largest, truncated, pn_length * 8u);
  if (net_replay_seen(conn, pn)) {
    return;
  }
  const uint32_t payload_size = size - header_size - VKR_NET_TAG_SIZE;
  uint8_t *payload = bytes + header_size;
  if (!vkr_net_packet_open(&conn->rx, pn, bytes, header_size, payload,
                           payload_size, payload + payload_size)) {
    return;
  }
  const bool8_t newest = !conn->rx_any || pn > conn->rx_largest;
  net_replay_record(conn, pn, now);

  conn->packets_received += 1u;
  conn->bytes_received += size;
  conn->last_rx = now;
  conn->idle_deadline = now + (uint64_t)core->config.idle_timeout_ms * 1000u;

  if (conn->state == NET_STATE_SERVER_HANDSHAKE) {
    /* Only the client holds these keys: the handshake is confirmed and
       the client's address is proven. */
    conn->state = NET_STATE_ESTABLISHED;
    conn->validated = true_v;
    conn->probe_next_time = now;
    if (core->half_open > 0u) {
      core->half_open -= 1u;
    }
    net_handshake_release(core, conn);
  }

  if (!vkr_net_address_equal(&conn->peer, from)) {
    if (!newest) {
      return;
    }
    /* A NAT rebinding or a migration: validate the new path. */
    conn->peer = *from;
    conn->validated = false_v;
    conn->amp_received = 0u;
    conn->amp_sent = 0u;
    vkr_net_random(conn->challenge, sizeof(conn->challenge));
    conn->challenge_pending = true_v;
  }
  conn->amp_received += size;

  if (conn->state == NET_STATE_CLOSING) {
    /* Repeat CLOSE for a peer that keeps sending. */
    conn->close_pending = true_v;
    net_mark_ready(core, conn);
    return;
  }

  NetFrameResult result = {0};
  net_process_frames(core, conn, payload, payload + payload_size, now, &result);
  if (result.violation) {
    net_close_local(core, conn, VKR_NET_CLOSE_PROTOCOL_VIOLATION, now);
    return;
  }
  if (result.close_code) {
    conn->state = NET_STATE_DRAINING;
    conn->drain_deadline = now + 3u * net_pto(conn);
    net_report_closed(core, conn, result.close_code);
    net_schedule(core, conn);
    return;
  }

  net_ack_insert(conn, pn);
  if (result.ack_eliciting) {
    conn->ack_eliciting_pending += 1u;
    if (conn->ack_eliciting_pending == 1u) {
      conn->ack_deadline = now + conn->max_ack_delay_us;
    }
    if (conn->ack_eliciting_pending >= 2u || !newest) {
      conn->ack_now = true_v;
    }
  }
  net_mark_ready(core, conn);
  net_schedule(core, conn);
}

// =============================================================================
// Public API
// =============================================================================

VkrNetCore *vkr_net_core_create(VkrAllocator *allocator,
                                const VkrNetCoreConfig *config) {
  if (!allocator || !config || !vkr_net_crypto_init()) {
    return NULL;
  }
  VkrNetCore *core = vkr_allocator_alloc(allocator, sizeof(VkrNetCore),
                                         VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!core) {
    return NULL;
  }
  MemZero(core, sizeof(*core));
  core->allocator = allocator;
  core->pool.allocator = allocator;
  core->config = *config;

  VkrNetCoreConfig *c = &core->config;
  if (c->max_connections == 0u) {
    c->max_connections = 64u;
  }
  c->max_connections = Min(c->max_connections, 65535u);
  if (c->max_datagram == 0u) {
    c->max_datagram = VKR_NET_DATAGRAM_ETHERNET;
  }
  c->max_datagram =
      Clamp(c->max_datagram, VKR_NET_DATAGRAM_INITIAL, VKR_NET_DATAGRAM_JUMBO);
  if (c->idle_timeout_ms == 0u) {
    c->idle_timeout_ms = 15000u;
  }
  if (c->handshake_timeout_ms == 0u) {
    c->handshake_timeout_ms = 10000u;
  }
  if (c->max_ack_delay_us == 0u) {
    c->max_ack_delay_us = 5000u;
  }
  if (c->sent_packet_capacity == 0u) {
    c->sent_packet_capacity = 4096u;
  }
  if ((c->sent_packet_capacity & (c->sent_packet_capacity - 1u)) != 0u ||
      c->sent_packet_capacity < 64u) {
    vkr_allocator_free(allocator, core, sizeof(VkrNetCore),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    return NULL;
  }
  if (c->retry_threshold == 0u) {
    c->retry_threshold = 256u;
  }

  core->slot_count = c->max_connections + 1u;
  core->slots =
      net_alloc_zero(core, (uint64_t)core->slot_count * sizeof(void *));
  core->free_slots =
      net_alloc_zero(core, (uint64_t)core->slot_count * sizeof(uint32_t));
  core->ready =
      net_alloc_zero(core, (uint64_t)core->slot_count * sizeof(uint32_t));
  core->dirty =
      net_alloc_zero(core, (uint64_t)core->slot_count * sizeof(uint32_t));
  core->heap_capacity = core->slot_count * 2u;
  core->heap =
      net_alloc_zero(core, (uint64_t)core->heap_capacity * sizeof(NetTimer));
  if (!core->slots || !core->free_slots || !core->ready || !core->dirty ||
      !core->heap) {
    vkr_net_core_destroy(core);
    return NULL;
  }
  /* Free slots pop from the end, so slot 1 is used first. */
  for (uint32_t slot = core->slot_count - 1u; slot >= 1u; --slot) {
    core->free_slots[core->free_count++] = slot;
  }
  vkr_net_random(core->cookie_secret, sizeof(core->cookie_secret));
  return core;
}

void vkr_net_core_destroy(VkrNetCore *core) {
  if (!core) {
    return;
  }
  for (uint32_t slot = 1u; slot < core->slot_count && core->slots; ++slot) {
    NetConnection *conn = core->slots[slot];
    if (!conn) {
      continue;
    }
    if (conn->state != NET_STATE_FREE) {
      net_connection_free(core, conn);
    }
    net_free(core, conn, sizeof(NetConnection));
  }
  if (core->has_current) {
    pool_free(&core->pool, core->current.block, core->current.block_size);
  }
  for (uint32_t i = 0u; i < core->event_count; ++i) {
    NetEventEntry *entry =
        &core->events[(core->event_head + i) % core->event_capacity];
    pool_free(&core->pool, entry->block, entry->block_size);
  }
  net_free(core, core->events,
           (uint64_t)core->event_capacity * sizeof(NetEventEntry));
  net_free(core, core->heap, (uint64_t)core->heap_capacity * sizeof(NetTimer));
  net_free(core, core->dirty, (uint64_t)core->slot_count * sizeof(uint32_t));
  net_free(core, core->ready, (uint64_t)core->slot_count * sizeof(uint32_t));
  net_free(core, core->free_slots,
           (uint64_t)core->slot_count * sizeof(uint32_t));
  net_free(core, core->slots, (uint64_t)core->slot_count * sizeof(void *));
  pool_destroy(&core->pool);
  vkr_net_random(core->cookie_secret, sizeof(core->cookie_secret));
  VkrAllocator *allocator = core->allocator;
  vkr_allocator_free(allocator, core, sizeof(VkrNetCore),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

bool8_t vkr_net_core_connect(VkrNetCore *core, const VkrNetAddress *address,
                             const uint8_t server_key[VKR_NET_KEY_SIZE],
                             const uint8_t *credential,
                             uint32_t credential_size, uint64_t now_us,
                             VkrNetConnectionId *out_connection) {
  if (!core || !address || !server_key ||
      credential_size > VKR_NET_CREDENTIAL_MAX ||
      (credential_size > 0u && !credential)) {
    return false_v;
  }
  core->now = Max(core->now, now_us);
  NetConnection *conn = net_connection_alloc(core);
  if (!conn) {
    return false_v;
  }
  conn->noise = net_alloc_zero(core, sizeof(VkrNetNoise));
  const uint32_t message_capacity =
      VKR_NET_NOISE_MESSAGE1_OVERHEAD + credential_size;
  conn->hs_bytes = pool_alloc(&core->pool, message_capacity);
  if (!conn->noise || !conn->hs_bytes) {
    net_connection_free(core, conn);
    return false_v;
  }
  vkr_net_noise_init(conn->noise, true_v, net_prologue,
                     sizeof(net_prologue) - 1u, &core->config.static_keys,
                     server_key);
  conn->hs_size = vkr_net_noise_write_message1(conn->noise, NULL, credential,
                                               credential_size, conn->hs_bytes,
                                               message_capacity);
  if (conn->hs_size == 0u) {
    net_connection_free(core, conn);
    return false_v;
  }
  MemCopy(conn->peer_key, server_key, VKR_NET_KEY_SIZE);
  vkr_net_random(&conn->initial_dcid, sizeof(conn->initial_dcid));
  conn->state = NET_STATE_CLIENT_INITIAL;
  conn->is_client = true_v;
  conn->peer = *address;
  conn->validated = true_v;
  conn->hs_send_pending = true_v;
  conn->hs_started = now_us;
  conn->hs_interval = NET_HANDSHAKE_RETRY_US;
  conn->hs_deadline = now_us + conn->hs_interval;
  net_mark_ready(core, conn);
  net_schedule(core, conn);
  *out_connection = conn->local_cid;
  return true_v;
}

void vkr_net_core_receive(VkrNetCore *core, const VkrNetAddress *from,
                          uint8_t *bytes, uint32_t size, uint64_t now_us) {
  if (!core || !from || !bytes || size < 1u + NET_CID_SIZE ||
      !(bytes[0] & NET_FLAG_FIXED)) {
    return;
  }
  core->now = Max(core->now, now_us);
  if (!(bytes[0] & NET_FLAG_LONG)) {
    net_receive_short(core, from, bytes, size, now_us);
    return;
  }
  if (size < NET_LONG_HEADER_SIZE ||
      vkr_load_le_u32(bytes + 1) != VKR_NET_PROTOCOL_VERSION) {
    return;
  }
  const uint32_t type = (bytes[0] >> NET_LONG_TYPE_SHIFT) & 0x03u;
  if (type == NET_LONG_INITIAL) {
    net_receive_initial(core, from, bytes, size, now_us);
  } else if (type == NET_LONG_HANDSHAKE) {
    net_receive_handshake(core, from, bytes, size, now_us);
  } else if (type == NET_LONG_RETRY) {
    net_receive_retry(core, from, bytes, size, now_us);
  }
}

static void net_on_timer(VkrNetCore *core, NetConnection *conn, uint64_t now) {
  if (conn->state == NET_STATE_CLOSING || conn->state == NET_STATE_DRAINING) {
    if (now >= conn->drain_deadline) {
      net_connection_drop(core, conn,
                          conn->close_code
                              ? conn->close_code
                              : (uint16_t)VKR_NET_CLOSE_APPLICATION);
    }
    return;
  }
  const uint64_t handshake_timeout =
      (uint64_t)core->config.handshake_timeout_ms * 1000u;
  if (conn->state == NET_STATE_CLIENT_INITIAL) {
    if (now - conn->hs_started >= handshake_timeout) {
      net_connection_drop(core, conn, VKR_NET_CLOSE_TIMEOUT);
      return;
    }
    if (now >= conn->hs_deadline) {
      conn->hs_send_pending = true_v;
      conn->hs_interval = Min(conn->hs_interval * 2u, 2000000ull);
      conn->hs_deadline = now + conn->hs_interval;
    }
    return;
  }
  if (conn->state == NET_STATE_SERVER_HANDSHAKE &&
      now - conn->hs_started >= handshake_timeout) {
    net_connection_drop(core, conn, VKR_NET_CLOSE_TIMEOUT);
    return;
  }
  if (conn->idle_deadline && now >= conn->idle_deadline) {
    net_connection_drop(core, conn, VKR_NET_CLOSE_TIMEOUT);
    return;
  }
  if (conn->ack_eliciting_pending > 0u && now >= conn->ack_deadline) {
    conn->ack_now = true_v;
  }
  if (conn->loss_time && now >= conn->loss_time) {
    net_detect_lost(core, conn, now);
  }
  if (conn->ack_eliciting_in_flight > 0u) {
    const uint64_t pto = net_pto(conn) << Min(conn->pto_count, 16u);
    if (now >= conn->last_ack_eliciting_sent + pto) {
      conn->pto_count += 1u;
      conn->probes_pending = 1u;
      /* Restart the probe clock even if the probe cannot leave yet. */
      conn->last_ack_eliciting_sent = now;
    }
  }
  if (conn->idle_deadline) {
    const uint64_t idle = (uint64_t)core->config.idle_timeout_ms * 1000u;
    if (now >= conn->last_ack_eliciting_sent + idle / 3u) {
      conn->ping_pending = true_v;
    }
  }
}

void vkr_net_core_update(VkrNetCore *core, uint64_t now_us) {
  if (!core) {
    return;
  }
  core->now = Max(core->now, now_us);
  while (core->heap_count > 0u && core->heap[0].deadline <= now_us) {
    const NetTimer timer = heap_pop(core);
    NetConnection *conn = net_connection(core, timer.cid);
    if (!conn || conn->scheduled != timer.deadline) {
      continue;
    }
    conn->scheduled = UINT64_MAX;
    if (conn->timer <= now_us) {
      net_on_timer(core, conn, now_us);
    }
    if (conn->state != NET_STATE_FREE && conn->local_cid == timer.cid) {
      conn->pacing_release = 0u;
      net_mark_ready(core, conn);
      net_schedule(core, conn);
    }
  }
}

uint32_t vkr_net_core_transmit(VkrNetCore *core, uint64_t now_us,
                               uint8_t *buffer, uint32_t capacity,
                               VkrNetAddress *out_address) {
  if (!core || !buffer || !out_address) {
    return 0u;
  }
  core->now = Max(core->now, now_us);
  if (core->stateless_count > 0u) {
    const NetStateless *entry = &core->stateless[core->stateless_head];
    core->stateless_head = (core->stateless_head + 1u) % NET_STATELESS_QUEUE;
    core->stateless_count -= 1u;
    if (entry->size <= capacity) {
      MemCopy(buffer, entry->bytes, entry->size);
      *out_address = entry->address;
      return entry->size;
    }
  }
  while (core->ready_count > 0u) {
    const uint32_t slot = core->ready[core->ready_head];
    core->ready_head = (core->ready_head + 1u) % core->slot_count;
    core->ready_count -= 1u;
    NetConnection *conn = core->slots[slot];
    if (!conn || conn->state == NET_STATE_FREE) {
      continue;
    }
    conn->in_ready = false_v;

    uint32_t size = 0u;
    if (conn->hs_send_pending) {
      if (conn->state == NET_STATE_CLIENT_INITIAL) {
        size = net_build_initial(conn, buffer, capacity);
      } else if (conn->state == NET_STATE_SERVER_HANDSHAKE && conn->hs_bytes &&
                 conn->hs_size <= capacity) {
        /* At most three times the bytes received before validation. */
        if (conn->validated ||
            conn->amp_sent + conn->hs_size <= conn->amp_received * 3u) {
          MemCopy(buffer, conn->hs_bytes, conn->hs_size);
          size = conn->hs_size;
          conn->amp_sent += size;
        }
      }
      conn->hs_send_pending = false_v;
      if (size > 0u) {
        conn->bytes_sent += size;
        *out_address = conn->peer;
        net_mark_ready(core, conn);
        net_schedule(core, conn);
        return size;
      }
    }

    size = net_build_packet(core, conn, now_us, buffer, capacity);
    if (size > 0u) {
      *out_address = conn->peer;
      /* Round robin: the connection may have more to send. */
      net_mark_ready(core, conn);
      net_schedule(core, conn);
      return size;
    }
    net_schedule(core, conn);
  }
  return 0u;
}

uint64_t vkr_net_core_next_deadline(const VkrNetCore *core) {
  if (!core) {
    return UINT64_MAX;
  }
  if (core->stateless_count > 0u || core->ready_count > 0u) {
    return 0u;
  }
  return core->heap_count > 0u ? core->heap[0].deadline : UINT64_MAX;
}

bool8_t vkr_net_core_open_channel(VkrNetCore *core,
                                  VkrNetConnectionId connection,
                                  uint8_t channel,
                                  const VkrNetChannelConfig *config) {
  NetConnection *conn = core ? net_connection(core, connection) : NULL;
  if (!conn || !config || channel == 0u || !conn->has_keys) {
    return false_v;
  }
  return net_open_channel(core, conn, channel, config);
}

void vkr_net_core_close_channel(VkrNetCore *core, VkrNetConnectionId connection,
                                uint8_t channel) {
  NetConnection *conn = core ? net_connection(core, connection) : NULL;
  if (!conn || channel == 0u || channel >= VKR_NET_CHANNEL_MAX ||
      !conn->channels[channel]) {
    return;
  }
  net_channel_destroy(core, conn->channels[channel]);
  conn->channels[channel] = NULL;
}

VkrNetSendStatus
vkr_net_core_send(VkrNetCore *core, VkrNetConnectionId connection,
                  uint8_t channel_index, const void *data, uint32_t size,
                  const VkrNetSendOptions *options, uint64_t now_us) {
  NetConnection *conn = core ? net_connection(core, connection) : NULL;
  if (!conn || channel_index >= VKR_NET_CHANNEL_MAX || (size > 0u && !data)) {
    return VKR_NET_SEND_INVALID;
  }
  if (conn->state == NET_STATE_CLOSING || conn->state == NET_STATE_DRAINING ||
      conn->state == NET_STATE_CLIENT_INITIAL) {
    return VKR_NET_SEND_CLOSED;
  }
  NetChannel *channel = conn->channels[channel_index];
  if (!channel) {
    return VKR_NET_SEND_INVALID;
  }
  if (size > channel->config.max_message_size) {
    return VKR_NET_SEND_TOO_LARGE;
  }
  if (channel->tail_seq - channel->head_seq > channel->ring_mask) {
    return VKR_NET_SEND_QUEUE_FULL;
  }
  const VkrNetSendOptions defaults = {0};
  const VkrNetSendOptions *opts = options ? options : &defaults;
  const bool8_t reliable = net_delivery_reliable(channel->config.delivery);

  NetSendMessage *message =
      &channel->ring[channel->tail_seq & channel->ring_mask];
  MemZero(message, sizeof(*message));
  message->data = pool_alloc(&core->pool, size);
  if (!message->data) {
    return VKR_NET_SEND_NO_MEMORY;
  }
  if (size > 0u) {
    MemCopy(message->data, data, size);
  }
  message->size = size;
  message->seq = channel->tail_seq;
  message->flags = opts->flags;
  message->tag = opts->tag;
  if (channel->config.delivery == VKR_NET_DEADLINE) {
    message->deadline_us =
        opts->deadline_us
            ? opts->deadline_us
            : now_us + (uint64_t)channel->config.deadline_ms * 1000u;
  }

  /* One frame when the message fits a fragment; fixed fragments else. */
  const uint32_t fragment_size = net_fragment_size(conn);
  if (reliable && size > fragment_size) {
    message->fragment_size = fragment_size;
    message->fragment_count = (size + fragment_size - 1u) / fragment_size;
  } else {
    message->fragment_count = 1u;
  }
  message->words = bits_words(message->fragment_count);
  if (message->words == 1u) {
    message->bits = message->inline_bits;
  } else {
    message->bits = pool_alloc(&core->pool, (uint64_t)message->words * 2u *
                                                sizeof(uint64_t));
    if (!message->bits) {
      pool_free(&core->pool, message->data, size);
      message->data = NULL;
      return VKR_NET_SEND_NO_MEMORY;
    }
    MemZero(message->bits, (uint64_t)message->words * 2u * sizeof(uint64_t));
  }
  if (reliable) {
    message->offset = channel->send_offset;
    channel->send_offset += size;
  }
  message->state = NET_MESSAGE_QUEUED;
  channel->queued_bytes += size;
  channel->tail_seq += 1u;

  if (opts->flags & VKR_NET_SEND_IMMEDIATE) {
    net_mark_ready(core, conn);
  } else {
    net_mark_dirty(core, conn);
  }
  return VKR_NET_SEND_OK;
}

void vkr_net_core_flush(VkrNetCore *core) {
  if (!core) {
    return;
  }
  for (uint32_t i = 0u; i < core->dirty_count; ++i) {
    NetConnection *conn = core->slots[core->dirty[i]];
    if (conn && conn->state != NET_STATE_FREE) {
      conn->in_dirty = false_v;
      net_mark_ready(core, conn);
    }
  }
  core->dirty_count = 0u;
}

bool8_t vkr_net_core_poll(VkrNetCore *core, VkrNetEvent *out_event) {
  if (!core || !out_event) {
    return false_v;
  }
  if (core->has_current) {
    pool_free(&core->pool, core->current.block, core->current.block_size);
    core->has_current = false_v;
  }
  if (core->event_count == 0u) {
    return false_v;
  }
  core->current = core->events[core->event_head];
  core->event_head = (core->event_head + 1u) % core->event_capacity;
  core->event_count -= 1u;
  core->has_current = true_v;
  *out_event = core->current.event;

  if (core->current.event.type == VKR_NET_EVENT_MESSAGE) {
    if (!core->current.counts) {
      core->unreliable_events -= Min(core->unreliable_events, 1u);
    } else {
      NetConnection *conn = net_connection(core, out_event->connection);
      NetChannel *channel = conn ? conn->channels[out_event->channel] : NULL;
      if (channel) {
        channel->rx_buffered -=
            Min(channel->rx_buffered, (uint64_t)out_event->size);
        net_rx_consume(core, conn, channel, out_event->size);
      }
    }
  }
  return true_v;
}

void vkr_net_core_close(VkrNetCore *core, VkrNetConnectionId connection,
                        uint16_t code, uint64_t now_us) {
  NetConnection *conn = core ? net_connection(core, connection) : NULL;
  if (!conn) {
    return;
  }
  core->now = Max(core->now, now_us);
  net_close_local(core, conn, code ? code : (uint16_t)VKR_NET_CLOSE_APPLICATION,
                  now_us);
}

bool8_t vkr_net_core_path(const VkrNetCore *core, VkrNetConnectionId connection,
                          VkrNetPathStats *out_stats) {
  const NetConnection *conn = core ? net_connection(core, connection) : NULL;
  if (!conn || !out_stats) {
    return false_v;
  }
  *out_stats = (VkrNetPathStats){
      .srtt_us = conn->srtt,
      .rttvar_us = conn->rttvar,
      .min_rtt_us = conn->has_rtt ? conn->min_rtt : 0u,
      .bandwidth = conn->cc.bandwidth,
      .pacing_rate = conn->cc.pacing_rate,
      .cwnd = conn->cc.cwnd,
      .bytes_in_flight = conn->bytes_in_flight,
      .max_datagram = conn->mtu,
      .packets_sent = conn->packets_sent,
      .packets_received = conn->packets_received,
      .packets_lost = conn->packets_lost,
      .bytes_sent = conn->bytes_sent,
      .bytes_received = conn->bytes_received,
  };
  return true_v;
}

uint64_t vkr_net_core_user(const VkrNetCore *core,
                           VkrNetConnectionId connection) {
  const NetConnection *conn = core ? net_connection(core, connection) : NULL;
  return conn ? conn->user : 0u;
}

bool8_t vkr_net_core_peer_address(const VkrNetCore *core,
                                  VkrNetConnectionId connection,
                                  VkrNetAddress *out_address) {
  const NetConnection *conn = core ? net_connection(core, connection) : NULL;
  if (!conn || !out_address) {
    return false_v;
  }
  *out_address = conn->peer;
  return true_v;
}

uint64_t vkr_net_core_queued_bytes(const VkrNetCore *core,
                                   VkrNetConnectionId connection,
                                   uint8_t channel) {
  const NetConnection *conn = core ? net_connection(core, connection) : NULL;
  if (!conn || channel >= VKR_NET_CHANNEL_MAX || !conn->channels[channel]) {
    return 0u;
  }
  return conn->channels[channel]->queued_bytes;
}
