#pragma once

#include "vkr_depot_protocol.h"
#include "vkr_depot_repository.h"
#include "vkr_depot_store.h"

/* The depot server: a store directory served over the depot service.
 *
 *   <store>/objects/       chunks and objects (vkr_depot_store.h)
 *   <store>/refs/<branch>  the 32-byte commit ID of each branch
 *                          (vkr_depot_repository.h)
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

/* Loader threads one server may run at most. */
#define VKR_DEPOT_SERVER_LOAD_THREADS_MAX 8u

/* Stores one server serves at most. */
#define VKR_DEPOT_SERVER_REPOSITORIES_MAX 16u

/* Keys a server remembers as peers, connected or not: the latest ones. */
#define VKR_DEPOT_SERVER_PEERS_MAX 64u

typedef struct VkrDepotServerConfig {
  /* The store, when `store_count` is zero. */
  const char *store_root;
  /* The stores it serves, as repositories (proposal "Service"); each keeps
     its own access list and locks. A connection selects one with HELLO; a
     server of one store gives it to every connection. */
  const char *const *store_roots;
  /* Names discovery and HELLO give each store; NULL, or a NULL entry, for
     its folder's name. */
  const char *const *store_names;
  uint32_t store_count;
  const VkrCodec *codec;
  /* Unknown keys write; for a trusted network only. */
  bool8_t open_access;
  /* Threads that read and check queued objects ahead of their send; zero
     reads each on the caller's thread as it is sent. */
  uint32_t load_threads;
  /* Called from a loader once an object is read, so that the thread that
     calls vkr_depot_server_update runs it soon; NULL when that thread
     updates often anyway. */
  void (*wake)(void *context);
  void *wake_context;
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
   out of it when it tries again, until the server restarts (the last 256
   such keys). One network address holds at most two requests. */
uint32_t vkr_depot_server_requests(const VkrDepotServer *server,
                                   VkrDepotAccessRequest *out,
                                   uint32_t capacity);

/* The access list; returns how many entries it stored, at most
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

typedef struct VkrDepotServerRepository {
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  /* False until the store has `<store>/repository`. */
  bool8_t has_id;
  uint8_t id[VKR_DEPOT_REPOSITORY_ID_SIZE];
  char name[VKR_DEPOT_BRANCH_NAME];
  /* `main` when the store has it, else its first branch; empty without
     branches. */
  char default_branch[VKR_DEPOT_BRANCH_NAME];
} VkrDepotServerRepository;

/* A key that connected, or tried to, since the server started. */
typedef struct VkrDepotServerPeer {
  uint8_t key[32];
  /* The display name of its latest handshake. */
  char name[VKR_DEPOT_DISPLAY_NAME];
  /* Live connections of the key now. */
  uint32_t connections;
  /* Its HELLO said it accepts no connection from outside: other machines
     reach it only while it is connected. */
  bool8_t outbound_only;
  /* The repository its latest connection selected, or -1 for none. */
  int32_t repository;
  /* Unix microseconds of its latest handshake or message. */
  uint64_t last_seen_us;
} VkrDepotServerPeer;

uint32_t vkr_depot_server_repository_count(const VkrDepotServer *server);

/* Reads the repository's ID and default branch as they stand now. */
bool8_t vkr_depot_server_repository(const VkrDepotServer *server,
                                    uint32_t index,
                                    VkrDepotServerRepository *out);

/* Known peers, the most recently seen first; returns how many it stored. */
uint32_t vkr_depot_server_peers(const VkrDepotServer *server,
                                VkrDepotServerPeer *out, uint32_t capacity);

/* vkr_depot_server_access and vkr_depot_server_set_role for repository
   `index`; the plain ones act on the first repository. */
uint32_t vkr_depot_server_access_in(const VkrDepotServer *server,
                                    uint32_t index, VkrDepotAccessEntry *out,
                                    uint32_t capacity);
bool8_t vkr_depot_server_set_role_in(VkrDepotServer *server, uint32_t index,
                                     const uint8_t key[32], VkrDepotRole role,
                                     uint64_t now_us);

/* Closes every live connection with `code`, as a server that stops does. */
void vkr_depot_server_close_peers(VkrDepotServer *server, uint16_t code,
                                  uint64_t now_us);

/* Loads the store's server key, creating one on first use. */
bool8_t vkr_depot_server_key(const char *store_root, VkrNetKeyPair *out_keys);

/* Gives `key` a role in the store's access list (NONE removes it). */
bool8_t vkr_depot_access_set(const char *store_root, const uint8_t key[32],
                             VkrDepotRole role);
