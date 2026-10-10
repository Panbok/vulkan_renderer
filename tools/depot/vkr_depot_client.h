#pragma once

#include "vkr_depot_protocol.h"
#include "vkr_depot_store.h"

/* The depot client: transfers between a local store and a depot server.
 *
 * It runs over a link the caller provides: the transport core, the session
 * and connection, a pump that advances the network and passes every core
 * event to the session, and a clock. The CLI pumps a UDP host; tests pump
 * the simulated network with the server in the same process.
 *
 * Every operation blocks until it completes, fails or makes no progress for
 * `timeout_ms`. Every received object is checked against its ID before it
 * enters the store. */

typedef struct VkrDepotLink {
  VkrNetCore *core;
  VkrNetSession *session;
  VkrNetConnectionId connection;
  void *context;
  /* Advances the network, waiting up to `timeout_ms`, and dispatches every
     core event to `session`. False when the network failed. */
  bool8_t (*pump)(void *context, int32_t timeout_ms);
  uint64_t (*now)(void *context);
} VkrDepotLink;

typedef struct VkrDepotRef {
  char name[VKR_DEPOT_BRANCH_NAME];
  VkrDepotId commit;
} VkrDepotRef;

typedef struct VkrDepotTransferStats {
  uint64_t objects;
  uint64_t stored_bytes;
  uint64_t decoded_bytes;
} VkrDepotTransferStats;

typedef bool8_t (*VkrDepotLockVisitor)(void *context, const char *path,
                                       const uint8_t owner[32],
                                       uint64_t time_us);

typedef struct VkrDepotClient VkrDepotClient;

/* Registers the depot service on the link's session and opens it. */
VkrDepotClient *vkr_depot_client_create(const VkrDepotLink *link,
                                        const VkrDepotStore *store,
                                        uint32_t timeout_ms);
void vkr_depot_client_destroy(VkrDepotClient *client);

/* Why the last operation failed. */
const char *vkr_depot_client_error(const VkrDepotClient *client);

bool8_t vkr_depot_client_refs(VkrDepotClient *client, VkrDepotRef *out_refs,
                              uint32_t capacity, uint32_t *out_count);

/* Downloads every object of `commit` the store lacks. */
bool8_t vkr_depot_client_fetch(VkrDepotClient *client, const VkrDepotId *commit,
                               VkrDepotTransferStats *out_stats);

/* Uploads every object of `commit` that is not part of `base` (zero for
   none) and that the server lacks. */
bool8_t vkr_depot_client_upload(VkrDepotClient *client,
                                const VkrDepotId *commit,
                                const VkrDepotId *base,
                                VkrDepotTransferStats *out_stats);

/* Moves `branch` from `expected` (zero creates it) to `commit`. Returns the
   server's status and stores where the branch is now. */
VkrDepotRefStatus vkr_depot_client_update_ref(VkrDepotClient *client,
                                              const char *branch,
                                              const VkrDepotId *expected,
                                              const VkrDepotId *commit,
                                              VkrDepotId *out_current);

VkrDepotLockStatus vkr_depot_client_lock(VkrDepotClient *client,
                                         const char *path, bool8_t release,
                                         bool8_t force, uint8_t out_owner[32]);

bool8_t vkr_depot_client_locks(VkrDepotClient *client,
                               VkrDepotLockVisitor visit, void *context);
