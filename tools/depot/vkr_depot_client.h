#pragma once

#include "vkr_depot_progress.h"
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

/* Registers the depot service on the link's session and opens it. `store`
   may be NULL for a client that only lists refs and locks. With an
   `allocator`, threads of the client's own check and store received chunks
   while the caller pumps; NULL checks them on the pumping thread. */
VkrDepotClient *vkr_depot_client_create(const VkrDepotLink *link,
                                        const VkrDepotStore *store,
                                        uint32_t timeout_ms,
                                        VkrAllocator *allocator);
void vkr_depot_client_destroy(VkrDepotClient *client);

/* Why the last operation failed. */
const char *vkr_depot_client_error(const VkrDepotClient *client);

/* Fetch and upload report through `progress` (borrowed; NULL for none) and
   stop with false, the error "canceled", once it is canceled. */
void vkr_depot_client_set_progress(VkrDepotClient *client,
                                   VkrDepotProgress *progress);

bool8_t vkr_depot_client_refs(VkrDepotClient *client, VkrDepotRef *out_refs,
                              uint32_t capacity, uint32_t *out_count);

/* Downloads every object of `commit` the store lacks. */
bool8_t vkr_depot_client_fetch(VkrDepotClient *client, const VkrDepotId *commit,
                               VkrDepotTransferStats *out_stats);

/* A sparse fetch: downloads `commit` and every tree below it the store
   lacks, but none of the files those trees list. */
bool8_t vkr_depot_client_fetch_trees(VkrDepotClient *client,
                                     const VkrDepotId *commit,
                                     VkrDepotTransferStats *out_stats);

/* Downloads files a tree lists: each of the `count` blobs and the chunks
   the store lacks, in one pipelined transfer. */
bool8_t vkr_depot_client_fetch_files(VkrDepotClient *client,
                                     const VkrDepotId *blobs, uint32_t count,
                                     VkrDepotTransferStats *out_stats);

/* Bytes [offset, offset + size) of the file `blob` names. */
typedef struct VkrDepotRange {
  VkrDepotId blob;
  uint64_t offset;
  uint64_t size;
} VkrDepotRange;

/* Downloads parts of files: each range's blob, then only the chunks the
   store lacks that hold bytes of the range, in two pipelined transfers.
   The files' other chunks stay on the server; a later fetch of the whole
   file downloads only those. */
bool8_t vkr_depot_client_fetch_ranges(VkrDepotClient *client,
                                      const VkrDepotRange *ranges,
                                      uint32_t count,
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

typedef struct VkrDepotHello {
  VkrDepotHelloStatus status;
  /* The selected repository; zero size for a store without an ID yet. */
  uint8_t repository[VKR_DEPOT_REPOSITORY_ID_SIZE];
  uint32_t repository_size;
  char name[VKR_DEPOT_BRANCH_NAME];
  char default_branch[VKR_DEPOT_BRANCH_NAME];
  /* The service chose version 1, which has no HELLO: nothing was sent and
     the connection uses the service's only repository. */
  bool8_t version_1;
} VkrDepotHello;

/* Selects `repository` (NULL or all zero: the service's default) and sends
   `flags` (VKR_DEPOT_HELLO_*), once per connection before other requests.
   False when no reply came; the status tells a refusal. */
bool8_t vkr_depot_client_hello(VkrDepotClient *client,
                               const uint8_t *repository, uint64_t flags,
                               VkrDepotHello *out_hello);

VkrDepotLockStatus vkr_depot_client_lock(VkrDepotClient *client,
                                         const char *path, bool8_t release,
                                         bool8_t force, uint8_t out_owner[32]);

bool8_t vkr_depot_client_locks(VkrDepotClient *client,
                               VkrDepotLockVisitor visit, void *context);
