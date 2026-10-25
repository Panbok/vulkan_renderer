#pragma once

#include "vkr_depot_discovery.h"
#include "vkr_depot_server.h"

/* A depot server running on its own thread (ADR-106, "Depot"): its stores,
 * the server, its session and UDP host, and the discovery responder, pumped
 * by a host thread (vkr_net_host.h). The `serve` command runs one until it
 * is killed; `service run` runs the machine's one over every store it
 * serves; the sharing editor runs one while it is open.
 *
 * Every function here may be called from any thread: each takes the host
 * thread's lock, so it waits at most for one pump and step. The accept
 * observer runs on the host thread with the lock held; it must not call
 * back into the service. */

/* Ports tried after a taken one when the configuration names none. */
#define VKR_DEPOT_SERVICE_PORT_ATTEMPTS 8u

/* Called for every handshake the server decides, with the credential's
   display name. */
typedef void (*VkrDepotServiceAcceptFn)(void *context,
                                        const VkrNetAddress *address,
                                        const uint8_t key[32], const char *name,
                                        bool8_t accepted);

typedef struct VkrDepotServiceConfig {
  /* The store, when `store_count` is zero. */
  const char *store_root;
  /* The stores the service serves as repositories, and the names
     discovery gives them (NULL entries for the folder's name); see
     VkrDepotServerConfig. */
  const char *const *store_roots;
  const char *const *store_names;
  uint32_t store_count;
  /* The file holding the service's secret key, created when missing; NULL
     for the first store's `server.key`. One key per machine lets remotes
     keep naming it whichever stores it serves. */
  const char *key_path;
  /* This machine accepts no connection from outside: discovery says so,
     and other machines reach it only while it connects to them. */
  bool8_t outbound_only;
  /* NULL for "[::]" on VKR_DEPOT_DEFAULT_PORT. */
  const char *bind;
  /* Ports to try from the bound one up, when it is taken; zero for
     VKR_DEPOT_SERVICE_PORT_ATTEMPTS, one to fail on a taken port. */
  uint32_t port_attempts;
  /* The name and host discovery answers with; NULL for the store folder's
     name and this machine's name. */
  const char *name;
  const char *host;
  /* Zero for VKR_DEPOT_DISCOVERY_PORT. A taken port leaves the service
     running without the responder. */
  uint16_t discovery_port;
  const VkrCodec *codec;
  /* Unknown keys write; for a trusted network only. */
  bool8_t open_access;
  /* A store without an access file makes the user's identity its admin,
     as the sharing editor's store needs. */
  bool8_t owner_admin;
  VkrDepotServiceAcceptFn on_accept;
  void *on_accept_context;
} VkrDepotServiceConfig;

typedef struct VkrDepotServiceInfo {
  /* The bound address with its port. */
  VkrNetAddress address;
  uint8_t server_key[32];
  bool8_t discoverable;
  uint16_t discovery_port;
  /* The name discovery answers with. */
  char name[VKR_DEPOT_DISCOVERY_TEXT];
  /* The socket failed: the service serves no more; stop it. */
  bool8_t failed;
  /* Live connections. */
  uint32_t peers;
  uint64_t bytes_in;
  uint64_t bytes_out;
  bool8_t outbound_only;
  VkrDepotServerRepository repositories[VKR_DEPOT_SERVER_REPOSITORIES_MAX];
  uint32_t repository_count;
  /* Keys that connected since the service started, the latest first. */
  VkrDepotServerPeer known_peers[VKR_DEPOT_SERVER_PEERS_MAX];
  uint32_t known_peer_count;
} VkrDepotServiceInfo;

typedef struct VkrDepotService VkrDepotService;

/* Opens the store, binds and starts the thread. NULL with a message.
   `allocator` makes only the thread's handle, on the calling thread; the
   host, session and server allocate from the service's own memory, which
   the host thread uses while it pumps. */
VkrDepotService *vkr_depot_service_start(VkrAllocator *allocator,
                                         const VkrDepotServiceConfig *config,
                                         char *error, uint32_t capacity);

/* Closes live connections, stops the thread and frees everything. */
void vkr_depot_service_stop(VkrDepotService *service);

void vkr_depot_service_info(VkrDepotService *service,
                            VkrDepotServiceInfo *out_info);

/* vkr_depot_server_requests, vkr_depot_server_access,
   vkr_depot_server_set_role and vkr_depot_server_reload_access under the
   lock. */
uint32_t vkr_depot_service_requests(VkrDepotService *service,
                                    VkrDepotAccessRequest *out,
                                    uint32_t capacity);
uint32_t vkr_depot_service_access(VkrDepotService *service,
                                  VkrDepotAccessEntry *out, uint32_t capacity);
bool8_t vkr_depot_service_set_role(VkrDepotService *service,
                                   const uint8_t key[32], VkrDepotRole role);
/* The two for repository `index` of the service. */
uint32_t vkr_depot_service_access_in(VkrDepotService *service, uint32_t index,
                                     VkrDepotAccessEntry *out,
                                     uint32_t capacity);
bool8_t vkr_depot_service_set_role_in(VkrDepotService *service, uint32_t index,
                                      const uint8_t key[32], VkrDepotRole role);
bool8_t vkr_depot_service_reload_access(VkrDepotService *service);

/* Serves one more store, or stops serving repository `index` (see
   vkr_depot_server_add_repository and _remove_repository); discovery lists
   the change at once. False with a message when the store cannot be
   served. */
bool8_t vkr_depot_service_add_repository(VkrDepotService *service,
                                         const char *store_root,
                                         const char *name, char *error,
                                         uint32_t capacity);
void vkr_depot_service_remove_repository(VkrDepotService *service,
                                         uint32_t index);

// =============================================================================
// The machine's service (ADR-134)
// =============================================================================

/* One background service per user and machine serves every repository
 * the user lets others fetch from and push to. Its folder, the user data
 * folder's `depot/`, holds:
 *
 *   service.conf    bind, name, discovery port, outbound only and the
 *                   served stores, one setting per line
 *   service.key     the service's secret key; the first store added gives
 *                   its `server.key`, so remotes that name a served store's
 *                   key stay valid
 *   service.status  what the running service last published: its address
 *                   and the keys that connected
 *   bin/            the installed copy of vkr_depot the service runs
 *   service.log     what `service run` prints when it runs as the service
 *
 * The running service rereads `service.conf` within a second of a change
 * and serves the stores it lists then; bind, name, port and outbound only
 * apply when it starts. It also syncs each served store with each of its
 * remotes that has an interval. The `vkr_depot service` command installs,
 * registers and runs it (vkr_depot_service_cmd.c, in the vkr_depot command
 * only). */

/* The app bundle in the service's folder that holds the binary on macOS. */
#define VKR_DEPOT_SERVICE_BUNDLE "VKR Depot.app"

typedef struct VkrDepotServicePaths {
  char folder[VKR_DEPOT_PATH_CAPACITY];
  char conf[VKR_DEPOT_PATH_CAPACITY];
  char key[VKR_DEPOT_PATH_CAPACITY];
  char status[VKR_DEPOT_PATH_CAPACITY];
  char bin_folder[VKR_DEPOT_PATH_CAPACITY];
  char binary[VKR_DEPOT_PATH_CAPACITY];
  char log[VKR_DEPOT_PATH_CAPACITY];
} VkrDepotServicePaths;

typedef struct VkrDepotServiceStore {
  /* The repository's name in discovery and HELLO. */
  char name[VKR_DEPOT_BRANCH_NAME];
  /* Absolute. */
  char root[VKR_DEPOT_PATH_CAPACITY];
} VkrDepotServiceStore;

#define VKR_DEPOT_SERVICE_DEFAULT_BIND "0.0.0.0:7320"

typedef struct VkrDepotServiceSettings {
  char bind[128];
  /* Empty for this machine's name. */
  char name[VKR_DEPOT_DISCOVERY_TEXT];
  uint32_t discovery_port;
  bool8_t outbound_only;
  VkrDepotServiceStore stores[VKR_DEPOT_SERVER_REPOSITORIES_MAX];
  uint32_t store_count;
} VkrDepotServiceSettings;

/* The service folder's files, under the user data folder. */
bool8_t vkr_depot_service_paths(VkrDepotServicePaths *out);

/* Reads `service.conf`; a missing file gives the defaults. Unknown lines
   are skipped, so a newer setting does not stop an older binary. */
bool8_t vkr_depot_service_settings_load(const VkrDepotServicePaths *paths,
                                        VkrDepotServiceSettings *out);
bool8_t
vkr_depot_service_settings_save(const VkrDepotServicePaths *paths,
                                const VkrDepotServiceSettings *settings);

/* Adds `store_root` to the served stores as `name` (NULL for its folder's
   name) and saves the settings: the store gets a repository ID, the user's
   identity becomes its admin when it has no access list, and the first
   store's `server.key` becomes the service's key when it has none.
   False with a message for a folder that is no store, a name or path
   already served, or a full list. A running service serves it within a
   second. */
bool8_t vkr_depot_service_add(const VkrDepotServicePaths *paths,
                              VkrDepotServiceSettings *settings,
                              const char *store_root, const char *name,
                              char *message, uint32_t capacity);

/* The index of the store at `store_root` in `settings`, compared as the
   service compares paths, or -1 when it serves none there. */
int32_t vkr_depot_service_find(const VkrDepotServiceSettings *settings,
                               const char *store_root);

/* Removes the served store named `which`, or at that path, and saves the
   settings. False with a message when none matches. */
bool8_t vkr_depot_service_remove(const VkrDepotServicePaths *paths,
                                 VkrDepotServiceSettings *settings,
                                 const char *which, char *message,
                                 uint32_t capacity);

/* A repository project is served from its own store: adds `store_root` to
   the machine's served stores unless it serves it already, under
   `project`'s name in the letters a store name takes (numbered when
   another store has it). A machine that takes no connections (the
   service's `outbound-only`, or vkr_depot_outbound_only) serves nothing,
   so `*out_served` stays false and the call succeeds. False with a message when
   the settings cannot be read or vkr_depot_service_add refuses. */
bool8_t vkr_depot_service_serve_project(const char *store_root,
                                        const char *project,
                                        bool8_t *out_served, char *message,
                                        uint32_t capacity);

/* Serves the stores `settings` lists and stops serving the others; a
   store whose name changed is served again under the new one. False when
   a store could not be served; the others still apply. */
bool8_t vkr_depot_service_apply(VkrDepotService *service,
                                const VkrDepotServiceSettings *settings,
                                char *error, uint32_t capacity);

/* Writes the running service's address, repositories and known peers to
   `path` for vkr_depot_service_status, when they changed since the last
   call. */
bool8_t vkr_depot_service_publish(VkrDepotService *service, const char *path);

typedef struct VkrDepotServiceStatus {
  VkrDepotServicePaths paths;
  VkrDepotServiceSettings settings;
  /* The service's public key, once it ran or adopted a store's. */
  bool8_t has_key;
  uint8_t key[32];
  /* The service answered discovery on loopback. */
  bool8_t running;
  VkrDepotDiscovered answer;
  /* From `service.status`: when the service last published, the keys
     that connected since it started, the latest first, and the keys it
     refused for want of a role, newest first (address left zero). */
  uint64_t published_us;
  VkrDepotServerPeer known_peers[VKR_DEPOT_SERVER_PEERS_MAX];
  uint32_t known_peer_count;
  VkrDepotAccessRequest requests[VKR_DEPOT_REQUESTS_MAX];
  uint32_t request_count;
} VkrDepotServiceStatus;

/* The settings, the key, and, waiting up to `wait_ms` for discovery on
   loopback, whether the service runs and what it last published. False
   when the settings cannot be read. */
bool8_t vkr_depot_service_status(VkrDepotServiceStatus *out, uint32_t wait_ms);

/* `vkr_depot service add|remove|list|install|uninstall|status|run`
   (vkr_depot_service_cmd.c, in the vkr_depot command only); `argv[0]` is
   the verb. Returns the process exit code. */
int vkr_depot_service_command(int argc, char **argv);
