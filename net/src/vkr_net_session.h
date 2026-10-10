#pragma once

#include "vkr_net_core.h"

/* Services over one connection (docs/proposals/network-protocol.md,
 * "Session and services"). Channel 0 carries the session's own schema
 * messages: SERVICE_OPEN names a service, the versions the opener speaks
 * with their schema hashes, and the channels it opened for it;
 * SERVICE_ACCEPT, SERVICE_REFUSE and SERVICE_CLOSE answer. The acceptor
 * picks the highest version both sides list with equal schema hashes and
 * refuses a service it does not have, a schema it does not share or
 * channels that differ from its own.
 *
 * The application polls the core and passes every event to
 * vkr_net_session_dispatch, which consumes channel-0 messages and the
 * events of service channels. A service sends only once it is open. */

#define VKR_NET_SERVICE_VERSION_MAX 4u
#define VKR_NET_SERVICE_CHANNEL_MAX 16u
#define VKR_NET_SESSION_SERVICE_MAX 32u

/* Service IDs of the proposal's registry. */
typedef enum VkrNetServiceId {
  VKR_NET_SERVICE_DEPOT = 1,
  VKR_NET_SERVICE_STREAM = 2,
  VKR_NET_SERVICE_WORLD = 3,
  VKR_NET_SERVICE_COLLAB = 4,
  VKR_NET_SERVICE_VIEW = 5,
  VKR_NET_SERVICE_BENCH = 6,
} VkrNetServiceId;

typedef struct VkrNetServiceVersion {
  uint16_t version;
  uint64_t schema_hash;
} VkrNetServiceVersion;

typedef struct VkrNetService {
  uint16_t id;
  VkrNetServiceVersion versions[VKR_NET_SERVICE_VERSION_MAX];
  uint32_t version_count;
  /* The service's channels; every version uses the same ones. */
  VkrNetChannelConfig channels[VKR_NET_SERVICE_CHANNEL_MAX];
  uint32_t channel_count;
  void *context;
  /* Acceptor: zero accepts, a close code refuses. NULL accepts. */
  uint16_t (*accept)(void *context, VkrNetConnectionId connection,
                     uint16_t version);
  /* Both sides, once the service is open. */
  void (*opened)(void *context, VkrNetConnectionId connection,
                 uint16_t version);
  /* MESSAGE, ACKED and LOST events of the service's channels; `channel` is
     the service's own index into `channels`. */
  void (*event)(void *context, VkrNetConnectionId connection, uint8_t channel,
                const VkrNetEvent *event, uint64_t now_us);
  /* The service ended on this connection: refused, closed by either side
     or with the connection. */
  void (*closed)(void *context, VkrNetConnectionId connection, uint16_t code);
} VkrNetService;

typedef struct VkrNetSession VkrNetSession;

VkrNetSession *vkr_net_session_create(VkrAllocator *allocator,
                                      VkrNetCore *core);
void vkr_net_session_destroy(VkrNetSession *session);

/* Copies the descriptor. False when the ID is taken or a channel config is
   invalid. */
bool8_t vkr_net_session_register(VkrNetSession *session,
                                 const VkrNetService *service);

/* Opens a registered service on a connection; `opened` or `closed`
   follows. */
bool8_t vkr_net_session_open(VkrNetSession *session,
                             VkrNetConnectionId connection, uint16_t service_id,
                             uint64_t now_us);

void vkr_net_session_close(VkrNetSession *session,
                           VkrNetConnectionId connection, uint16_t service_id,
                           uint16_t code, uint64_t now_us);

bool8_t vkr_net_session_is_open(const VkrNetSession *session,
                                VkrNetConnectionId connection,
                                uint16_t service_id);

/* The version both sides chose, or zero while the service is not open. */
uint16_t vkr_net_session_version(const VkrNetSession *session,
                                 VkrNetConnectionId connection,
                                 uint16_t service_id);

/* Sends on the service's channel `channel` (its index into `channels`). */
VkrNetSendStatus vkr_net_session_send(VkrNetSession *session,
                                      VkrNetConnectionId connection,
                                      uint16_t service_id, uint8_t channel,
                                      const void *data, uint32_t size,
                                      const VkrNetSendOptions *options,
                                      uint64_t now_us);

/* Bytes queued on a service channel and not yet acknowledged. */
uint64_t vkr_net_session_queued_bytes(const VkrNetSession *session,
                                      VkrNetConnectionId connection,
                                      uint16_t service_id, uint8_t channel);

/* Handles one core event. True when the session consumed it: channel-0
   messages and service channel events. CONNECTED and CLOSED stay with the
   caller too (false), after the session updated its state. */
bool8_t vkr_net_session_dispatch(VkrNetSession *session,
                                 const VkrNetEvent *event, uint64_t now_us);
