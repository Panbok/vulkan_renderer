#pragma once

#include "vkr_depot_protocol.h"
#include "vkr_depot_store.h"

/* The depot server: a store directory served over the depot service.
 *
 *   <store>/objects/       chunks and objects (vkr_depot_store.h)
 *   <store>/refs/<branch>  the 32-byte commit ID of each branch
 *   <store>/locks          path locks: path, owner key, time
 *   <store>/access         keys and roles (read, write, admin)
 *   <store>/server.key     the server's X25519 secret key
 *
 * A branch moves only by compare-and-swap from the commit the pusher
 * expects, after the server checks that every object of the new commit is
 * present and that it changes no path another key has locked. */

typedef struct VkrDepotServerConfig {
  const char *store_root;
  const VkrDepotCodec *codec;
  /* Unknown keys write; for a trusted network only. */
  bool8_t open_access;
} VkrDepotServerConfig;

typedef struct VkrDepotServer VkrDepotServer;

/* Registers the depot service on `session`. */
VkrDepotServer *vkr_depot_server_create(VkrAllocator *allocator,
                                        VkrNetCore *core,
                                        VkrNetSession *session,
                                        const VkrDepotServerConfig *config);
void vkr_depot_server_destroy(VkrDepotServer *server);

/* The transport accept callback (VkrNetCoreConfig.accept): refuses keys
   without a role and stores the role as the connection's user value. */
void vkr_depot_server_accept(void *context, const VkrNetAddress *address,
                             const uint8_t peer_key[32],
                             const uint8_t *credential,
                             uint32_t credential_size,
                             VkrNetAcceptResult *out_result);

/* Passes CONNECTED and CLOSED events the session did not consume. */
void vkr_depot_server_on_event(VkrDepotServer *server,
                               const VkrNetEvent *event);

/* Sends queued object bytes while the data channel has room. Call after
   each pump. */
void vkr_depot_server_update(VkrDepotServer *server, uint64_t now_us);

/* Loads the store's server key, creating one on first use. */
bool8_t vkr_depot_server_key(const char *store_root, VkrNetKeyPair *out_keys);

/* Gives `key` a role in the store's access list (NONE removes it). */
bool8_t vkr_depot_access_set(const char *store_root, const uint8_t key[32],
                             VkrDepotRole role);

/* Reads a branch; false when it does not exist. */
bool8_t vkr_depot_ref_read(const char *store_root, const char *branch,
                           VkrDepotId *out_commit);
