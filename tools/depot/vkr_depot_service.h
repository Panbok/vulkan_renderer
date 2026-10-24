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

/* `vkr_depot service add|remove|list|install|uninstall|status|run`, the
   machine's service (vkr_depot_service_cmd.c, in the vkr_depot command
   only); `argv[0]` is the verb. Returns the process exit code. */
int vkr_depot_service_command(int argc, char **argv);
