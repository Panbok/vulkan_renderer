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
 * present and that it changes no path another key has locked.
 *
 * A key without a role is refused at the handshake and remembered as an
 * access request with the display name it sent (the last
 * VKR_DEPOT_REQUESTS_MAX keys), so the server's owner can give it a role
 * while the server runs; the refused client retries and gets in. The
 * server reloads the access file when it changes on disk (checked about
 * once a second in vkr_depot_server_update), so `vkr_depot access` takes
 * effect without a restart.
 *
 * A server is single-threaded: call it from the thread that pumps its core,
 * or under the lock of the host thread that does. */

#define VKR_DEPOT_REQUESTS_MAX 8u

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
   without a role, recording an access request with the credential's
   display name, and stores the role as the connection's user value. */
void vkr_depot_server_accept(void *context, const VkrNetAddress *address,
                             const uint8_t peer_key[32],
                             const uint8_t *credential,
                             uint32_t credential_size,
                             VkrNetAcceptResult *out_result);

/* Passes CONNECTED and CLOSED events the session did not consume. */
void vkr_depot_server_on_event(VkrDepotServer *server,
                               const VkrNetEvent *event);

/* Sends queued object bytes while the data channel has room and reloads a
   changed access file. Call after each pump. */
void vkr_depot_server_update(VkrDepotServer *server, uint64_t now_us);

typedef struct VkrDepotAccessRequest {
  uint8_t key[32];
  /* The display name of the latest try. */
  char name[VKR_DEPOT_DISPLAY_NAME];
  VkrNetAddress address;
  /* Unix microseconds of the latest try. */
  uint64_t time_us;
  uint32_t tries;
} VkrDepotAccessRequest;

typedef struct VkrDepotAccessEntry {
  uint8_t key[32];
  VkrDepotRole role;
} VkrDepotAccessEntry;

typedef struct VkrDepotServerStats {
  uint32_t peers;
  /* Object bytes received and sent on the data channel. */
  uint64_t bytes_in;
  uint64_t bytes_out;
} VkrDepotServerStats;

/* Pending access requests, newest first; returns how many it stored. A key
   given a role leaves the list. A key set to NONE leaves it too and stays
   out of it when it tries again, until the server restarts. */
uint32_t vkr_depot_server_requests(const VkrDepotServer *server,
                                   VkrDepotAccessRequest *out,
                                   uint32_t capacity);

/* The access list; returns how many keys it holds, storing at most
   `capacity`. */
uint32_t vkr_depot_server_access(const VkrDepotServer *server,
                                 VkrDepotAccessEntry *out, uint32_t capacity);

/* Gives `key` a role: writes the access file atomically, updates the list
   and the role of the key's live connections; NONE removes the key and
   closes its connections. */
bool8_t vkr_depot_server_set_role(VkrDepotServer *server, const uint8_t key[32],
                                  VkrDepotRole role, uint64_t now_us);

/* Reads the access file again and applies it to live connections. */
bool8_t vkr_depot_server_reload_access(VkrDepotServer *server, uint64_t now_us);

VkrDepotServerStats vkr_depot_server_stats(const VkrDepotServer *server);

/* Closes every live connection with `code`, as a server that stops does. */
void vkr_depot_server_close_peers(VkrDepotServer *server, uint16_t code,
                                  uint64_t now_us);

/* Loads the store's server key, creating one on first use. */
bool8_t vkr_depot_server_key(const char *store_root, VkrNetKeyPair *out_keys);

/* Gives `key` a role in the store's access list (NONE removes it). */
bool8_t vkr_depot_access_set(const char *store_root, const uint8_t key[32],
                             VkrDepotRole role);

/* Reads a branch; false when it does not exist. */
bool8_t vkr_depot_ref_read(const char *store_root, const char *branch,
                           VkrDepotId *out_commit);
