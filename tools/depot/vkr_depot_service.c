#include "vkr_depot_service.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_repository.h"
#include "vkr_depot_sync.h"
#include "vkr_net_crypto.h"
#include "vkr_net_host.h"
#include "vkr_net_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Server threads that read and check objects ahead of their send: one
   kept up with loopback on an M1 Pro, the second is headroom for a slower
   disk. */
#define SERVICE_LOAD_THREADS 2u
/* How often discovery's repository list follows the stores: a store gets
   its ID or a first branch while it is served. */
#define SERVICE_REFRESH_US 5000000u

struct VkrDepotService {
  /* The caller's: only the host thread's handle, made and freed on the
     caller's thread. */
  VkrAllocator *allocator;
  /* The host, session and server allocate from the service's own memory:
     the host thread allocates while it pumps, and an allocator is not
     thread-safe, so a caller that shared its allocator with the service
     corrupted it (a stalled transfer, a freelist that looped forever). The
     host lock serializes every use of this one. */
  VkrDMemory memory;
  VkrAllocator memory_allocator;
  bool8_t memory_ready;
  VkrNetHost *host;
  VkrNetSession *session;
  VkrDepotServer *server;
  VkrNetHostThread *thread;
  VkrDepotResponder responder;
  bool8_t discoverable;
  uint16_t discovery_port;
  VkrNetAddress address;
  uint8_t server_key[32];
  VkrDepotServiceAcceptFn on_accept;
  void *on_accept_context;
  bool8_t outbound_only;
  uint64_t refreshed_us;
  /* What vkr_depot_service_publish wrote last, without its time line. */
  char *published;
  uint64_t published_size;
};

// =============================================================================
// Host thread
// =============================================================================

/* The transport accept callback, on the host thread inside its pump. */
static void service_accept(void *context, const VkrNetAddress *address,
                           const uint8_t peer_key[32],
                           const uint8_t *credential, uint32_t credential_size,
                           VkrNetAcceptResult *out_result) {
  VkrDepotService *service = context;
  if (!service->server) {
    out_result->code = VKR_NET_CLOSE_SERVER_FULL;
    return;
  }
  vkr_depot_server_accept(service->server, address, peer_key, credential,
                          credential_size, out_result);
  if (service->on_accept) {
    char name[VKR_DEPOT_DISPLAY_NAME];
    (void)vkr_depot_display_name_clean(credential, credential_size, name);
    service->on_accept(service->on_accept_context, address, peer_key, name,
                       out_result->code == 0u);
  }
}

/* From a loader of the server once it read an object: the host thread,
   which may wait on the socket for up to 100 ms, sends it at once. A
   datagram to the host's own socket ends the wait; the host outlives the
   loaders, which stop with the server, while its thread stops first. */
static void service_wake(void *context) {
  VkrDepotService *service = context;
  vkr_net_host_wake(service->host);
}

/* Lists the served repositories in discovery answers. */
static void service_refresh_responder(VkrDepotService *service) {
  VkrDepotDiscoveredRepository listed[VKR_DEPOT_DISCOVERY_REPOSITORIES];
  uint32_t count = 0u;
  const uint32_t served = vkr_depot_server_repository_count(service->server);
  for (uint32_t i = 0u; i < served && count < ArrayCount(listed); ++i) {
    VkrDepotServerRepository repository;
    if (!vkr_depot_server_repository(service->server, i, &repository)) {
      continue;
    }
    VkrDepotDiscoveredRepository *entry = &listed[count++];
    MemZero(entry, sizeof(*entry));
    if (repository.has_id) {
      MemCopy(entry->id, repository.id, sizeof(entry->id));
    }
    snprintf(entry->name, sizeof(entry->name), "%s", repository.name);
    snprintf(entry->default_branch, sizeof(entry->default_branch), "%s",
             repository.default_branch);
  }
  vkr_depot_responder_set_repositories(&service->responder, listed, count,
                                       service->outbound_only);
}

/* After every pump, with the lock held. */
static void service_step(void *context, uint64_t now_us) {
  VkrDepotService *service = context;
  if (now_us - service->refreshed_us >= SERVICE_REFRESH_US) {
    service->refreshed_us = now_us;
    service_refresh_responder(service);
  }
  VkrNetCore *core = vkr_net_host_core(service->host);
  VkrNetEvent event;
  while (vkr_net_core_poll(core, &event)) {
    if (!vkr_net_session_dispatch(service->session, &event, now_us)) {
      vkr_depot_server_on_event(service->server, &event);
    }
  }
  vkr_depot_server_update(service->server, now_us);
  vkr_depot_responder_update(&service->responder);
}

// =============================================================================
// Lifetime
// =============================================================================

/* Makes the user's identity the store's admin when the store has no access
   list yet. */
static bool8_t service_grant_owner(const char *store_root) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(path, sizeof(path), store_root, "access")) {
    return false_v;
  }
  if (vkr_bakery_is_file(path)) {
    return true_v;
  }
  VkrNetKeyPair identity;
  if (!vkr_depot_identity(&identity, true_v)) {
    return false_v;
  }
  vkr_net_random(identity.secret_key, sizeof(identity.secret_key));
  return vkr_depot_access_set(store_root, identity.public_key,
                              VKR_DEPOT_ROLE_ADMIN);
}

/* Loads the secret key at `path`, creating one when the file is missing. */
static bool8_t service_key(const char *path, VkrNetKeyPair *out_keys) {
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (vkr_bakery_is_file(path)) {
    if (!vkr_bakery_read_file(path, 64u, &bytes, &size)) {
      return false_v;
    }
    const bool8_t ok = size == VKR_NET_KEY_SIZE;
    if (ok) {
      vkr_net_keypair_from_secret(bytes, out_keys);
    }
    vkr_net_random(bytes, size);
    free(bytes);
    return ok;
  }
  char parent[VKR_DEPOT_PATH_CAPACITY];
  vkr_bakery_path_parent(parent, sizeof(parent), path);
  vkr_net_keypair_generate(out_keys);
  return vkr_bakery_make_directories(parent) &&
         vkr_bakery_write_private_file_atomic(path, out_keys->secret_key,
                                              VKR_NET_KEY_SIZE);
}

/* Whether no other socket holds `bind`'s port in the other address family.
   macOS lets a dual-stack socket bind [::]:p while another socket holds
   0.0.0.0:p; IPv4 peers, loopback included, then reach that other socket.
   So a wildcard IPv6 port counts as free only when 0.0.0.0 on it binds
   too. */
static bool8_t service_port_free(const VkrNetAddress *bind) {
  const VkrNetAddress any =
      vkr_net_address_any((VkrNetAddressFamily)bind->family, bind->port);
  if (bind->family != VKR_NET_ADDRESS_IPV6 || bind->port == 0u ||
      !vkr_net_address_equal(bind, &any)) {
    return true_v;
  }
  const VkrNetAddress probe =
      vkr_net_address_any(VKR_NET_ADDRESS_IPV4, bind->port);
  VkrUdpSocket socket = {.handle = VKR_UDP_SOCKET_INVALID};
  if (!vkr_udp_socket_open(&probe, NULL, &socket)) {
    return false_v;
  }
  vkr_udp_socket_close(&socket);
  return true_v;
}

/* Binds the host on the configured port or, when it is taken, one of the
   next ones. */
static bool8_t service_bind(VkrDepotService *service,
                            const VkrDepotServiceConfig *config,
                            VkrNetHostConfig *host_config, char *error,
                            uint32_t capacity) {
  const char *bind = config->bind ? config->bind : "[::]";
  if (!vkr_depot_address(bind, &host_config->bind)) {
    snprintf(error, capacity, "invalid bind address %s", bind);
    return false_v;
  }
  const uint32_t first = host_config->bind.port;
  const uint32_t attempts =
      first == 0u ? 1u
                  : Min(config->port_attempts ? config->port_attempts
                                              : VKR_DEPOT_SERVICE_PORT_ATTEMPTS,
                        65536u - first);
  for (uint32_t i = 0u; i < attempts && !service->host; ++i) {
    host_config->bind.port = (uint16_t)(first + i);
    if (service_port_free(&host_config->bind)) {
      service->host =
          vkr_net_host_create(&service->memory_allocator, host_config);
    }
  }
  if (!service->host) {
    snprintf(error, capacity, "could not bind %s (UDP %u to %u taken?)", bind,
             first, first + attempts - 1u);
    return false_v;
  }
  return vkr_net_host_local_address(service->host, &service->address);
}

VkrDepotService *vkr_depot_service_start(VkrAllocator *allocator,
                                         const VkrDepotServiceConfig *config,
                                         char *error, uint32_t capacity) {
  const char *first_store = config && config->store_count
                                ? config->store_roots[0]
                                : (config ? config->store_root : NULL);
  const uint32_t store_count = config && config->store_count
                                   ? config->store_count
                                   : (first_store ? 1u : 0u);
  /* A service of no store yet needs its own key. */
  if (!allocator || !config || (!first_store && !config->key_path) ||
      store_count > VKR_DEPOT_SERVER_REPOSITORIES_MAX) {
    snprintf(error, capacity, "the service needs up to %u stores and a key",
             VKR_DEPOT_SERVER_REPOSITORIES_MAX);
    return NULL;
  }
  VkrDepotService *service = calloc(1u, sizeof(VkrDepotService));
  if (!service) {
    snprintf(error, capacity, "out of memory");
    return NULL;
  }
  service->allocator = allocator;
  service->responder.socket.handle = VKR_UDP_SOCKET_INVALID;
  service->on_accept = config->on_accept;
  service->on_accept_context = config->on_accept_context;
  service->outbound_only = config->outbound_only;
  /* Object chunks of up to 1 MiB in flight on every connection. */
  if (!vkr_dmemory_create(MB(8), GB(1), &service->memory)) {
    snprintf(error, capacity, "out of memory");
    goto failed;
  }
  service->memory_allocator = (VkrAllocator){.ctx = &service->memory};
  vkr_dmemory_allocator_create(&service->memory_allocator);
  service->memory_ready = true_v;
  VkrNetHostConfig host_config = {
      .socket = {.receive_buffer = 16u << 20, .send_buffer = 16u << 20}};
  if (config->key_path
          ? !service_key(config->key_path, &host_config.core.static_keys)
          : !vkr_depot_server_key(first_store, &host_config.core.static_keys)) {
    snprintf(error, capacity, "could not read the service key for %s",
             config->key_path ? config->key_path : first_store);
    goto failed;
  }
  MemCopy(service->server_key, host_config.core.static_keys.public_key, 32u);
  for (uint32_t i = 0u; i < store_count && config->owner_admin; ++i) {
    const char *store =
        config->store_count ? config->store_roots[i] : config->store_root;
    if (!service_grant_owner(store)) {
      snprintf(error, capacity, "could not make this user %s's admin", store);
      goto failed;
    }
  }
  host_config.core.accept_incoming = true_v;
  host_config.core.accept = service_accept;
  host_config.core.accept_context = service;
  host_config.core.max_connections = 256u;
  host_config.core.sent_packet_capacity = 16384u;
  const bool8_t bound =
      service_bind(service, config, &host_config, error, capacity);
  vkr_net_random(&host_config.core.static_keys,
                 sizeof(host_config.core.static_keys));
  if (!bound) {
    goto failed;
  }
  service->session = vkr_net_session_create(&service->memory_allocator,
                                            vkr_net_host_core(service->host));
  const VkrDepotServerConfig server_config = {
      .store_root = config->store_root,
      .store_roots = config->store_roots,
      .store_names = config->store_names,
      .store_count = config->store_count,
      .codec = config->codec,
      .open_access = config->open_access,
      .load_threads = SERVICE_LOAD_THREADS,
      .wake = service_wake,
      .wake_context = service};
  service->server =
      service->session
          ? vkr_depot_server_create(&service->memory_allocator,
                                    vkr_net_host_core(service->host),
                                    service->session, &server_config)
          : NULL;
  if (!service->server) {
    snprintf(error, capacity, "could not start the server on %s%s",
             first_store ? first_store : "no store",
             store_count > 1u ? " and its other stores" : "");
    goto failed;
  }
  /* Other machines find this server by its name, the first store's folder
     unless the configuration gives one. */
  service->discovery_port = config->discovery_port
                                ? config->discovery_port
                                : (uint16_t)VKR_DEPOT_DISCOVERY_PORT;
  service->discoverable =
      vkr_depot_responder_open(&service->responder, service->discovery_port,
                               service->address.port, service->server_key,
                               config->name  ? config->name
                               : first_store ? vkr_bakery_path_name(first_store)
                                             : "vkr-depot",
                               config->host);
  service_refresh_responder(service);
  service->thread = vkr_net_host_thread_start(allocator, service->host,
                                              service_step, service);
  if (!service->thread) {
    snprintf(error, capacity, "could not start the server thread");
    goto failed;
  }
  return service;
failed:
  vkr_depot_service_stop(service);
  return NULL;
}

void vkr_depot_service_stop(VkrDepotService *service) {
  if (!service) {
    return;
  }
  if (service->thread) {
    vkr_net_host_thread_stop(service->thread);
    service->thread = NULL;
  }
  /* The thread is gone: tell live peers the server left, so they fail now
     instead of at their idle timeout. */
  if (service->server) {
    VkrNetCore *core = vkr_net_host_core(service->host);
    vkr_depot_server_close_peers(service->server, VKR_NET_CLOSE_APPLICATION,
                                 vkr_net_host_now(service->host));
    for (uint32_t i = 0u; i < 10u; ++i) {
      if (!vkr_net_host_pump(service->host, 10)) {
        break;
      }
      service_step(service, vkr_net_host_now(service->host));
      vkr_net_core_flush(core);
    }
  }
  vkr_depot_responder_close(&service->responder);
  vkr_depot_server_destroy(service->server);
  vkr_net_session_destroy(service->session);
  vkr_net_host_destroy(service->host);
  if (service->memory_ready) {
    vkr_dmemory_allocator_destroy(&service->memory_allocator);
  }
  free(service->published);
  free(service);
}

// =============================================================================
// Accessors
// =============================================================================

void vkr_depot_service_info(VkrDepotService *service,
                            VkrDepotServiceInfo *out_info) {
  MemZero(out_info, sizeof(*out_info));
  out_info->address = service->address;
  MemCopy(out_info->server_key, service->server_key, 32u);
  out_info->discoverable = service->discoverable;
  out_info->discovery_port = service->discovery_port;
  snprintf(out_info->name, sizeof(out_info->name), "%s",
           service->responder.name);
  out_info->outbound_only = service->outbound_only;
  vkr_net_host_thread_lock(service->thread);
  const VkrDepotServerStats stats = vkr_depot_server_stats(service->server);
  out_info->failed = vkr_net_host_thread_failed(service->thread);
  out_info->repository_count =
      vkr_depot_server_repository_count(service->server);
  for (uint32_t i = 0u; i < out_info->repository_count; ++i) {
    (void)vkr_depot_server_repository(service->server, i,
                                      &out_info->repositories[i]);
  }
  out_info->known_peer_count = vkr_depot_server_peers(
      service->server, out_info->known_peers, VKR_DEPOT_SERVER_PEERS_MAX);
  vkr_net_host_thread_unlock(service->thread);
  out_info->peers = stats.peers;
  out_info->bytes_in = stats.bytes_in;
  out_info->bytes_out = stats.bytes_out;
}

uint32_t vkr_depot_service_requests(VkrDepotService *service,
                                    VkrDepotAccessRequest *out,
                                    uint32_t capacity) {
  vkr_net_host_thread_lock(service->thread);
  const uint32_t count =
      vkr_depot_server_requests(service->server, out, capacity);
  vkr_net_host_thread_unlock(service->thread);
  return count;
}

uint32_t vkr_depot_service_access(VkrDepotService *service,
                                  VkrDepotAccessEntry *out, uint32_t capacity) {
  vkr_net_host_thread_lock(service->thread);
  const uint32_t count =
      vkr_depot_server_access(service->server, out, capacity);
  vkr_net_host_thread_unlock(service->thread);
  return count;
}

bool8_t vkr_depot_service_set_role(VkrDepotService *service,
                                   const uint8_t key[32], VkrDepotRole role) {
  vkr_net_host_thread_lock(service->thread);
  const bool8_t ok = vkr_depot_server_set_role(service->server, key, role,
                                               vkr_net_host_now(service->host));
  vkr_net_host_thread_unlock(service->thread);
  return ok;
}

bool8_t vkr_depot_service_reload_access(VkrDepotService *service) {
  vkr_net_host_thread_lock(service->thread);
  const bool8_t ok = vkr_depot_server_reload_access(
      service->server, vkr_net_host_now(service->host));
  vkr_net_host_thread_unlock(service->thread);
  return ok;
}

uint32_t vkr_depot_service_access_in(VkrDepotService *service, uint32_t index,
                                     VkrDepotAccessEntry *out,
                                     uint32_t capacity) {
  vkr_net_host_thread_lock(service->thread);
  const uint32_t count =
      vkr_depot_server_access_in(service->server, index, out, capacity);
  vkr_net_host_thread_unlock(service->thread);
  return count;
}

bool8_t vkr_depot_service_set_role_in(VkrDepotService *service, uint32_t index,
                                      const uint8_t key[32],
                                      VkrDepotRole role) {
  vkr_net_host_thread_lock(service->thread);
  const bool8_t ok = vkr_depot_server_set_role_in(
      service->server, index, key, role, vkr_net_host_now(service->host));
  vkr_net_host_thread_unlock(service->thread);
  return ok;
}

bool8_t vkr_depot_service_add_repository(VkrDepotService *service,
                                         const char *store_root,
                                         const char *name, char *error,
                                         uint32_t capacity) {
  vkr_net_host_thread_lock(service->thread);
  const bool8_t ok =
      vkr_depot_server_add_repository(service->server, store_root, name);
  if (ok) {
    service_refresh_responder(service);
  }
  vkr_net_host_thread_unlock(service->thread);
  if (!ok) {
    snprintf(error, capacity, "could not serve %s", store_root);
  }
  return ok;
}

void vkr_depot_service_remove_repository(VkrDepotService *service,
                                         uint32_t index) {
  vkr_net_host_thread_lock(service->thread);
  vkr_depot_server_remove_repository(service->server, index,
                                     vkr_net_host_now(service->host));
  service_refresh_responder(service);
  vkr_net_host_thread_unlock(service->thread);
}

// =============================================================================
// The machine's service
// =============================================================================

#define SERVICE_LINE_MAX (VKR_DEPOT_PATH_CAPACITY + 256u)

/* Whether two absolute paths name one file: separators either way, and
   without case on Windows, as its file system compares them. */
static bool8_t service_same_path(const char *a, const char *b) {
  for (;; ++a, ++b) {
    char x = *a == '\\' ? '/' : *a;
    char y = *b == '\\' ? '/' : *b;
#if defined(_WIN32)
    x = x >= 'A' && x <= 'Z' ? (char)(x - 'A' + 'a') : x;
    y = y >= 'A' && y <= 'Z' ? (char)(y - 'A' + 'a') : y;
#endif
    if (x != y) {
      return false_v;
    }
    if (!x) {
      return true_v;
    }
  }
}

bool8_t vkr_depot_service_paths(VkrDepotServicePaths *out) {
  char data[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_user_data_directory(data, sizeof(data)) ||
      !vkr_bakery_path_join(out->folder, sizeof(out->folder), data, "depot") ||
      !vkr_bakery_path_join(out->conf, sizeof(out->conf), out->folder,
                            "service.conf") ||
      !vkr_bakery_path_join(out->key, sizeof(out->key), out->folder,
                            "service.key") ||
      !vkr_bakery_path_join(out->status, sizeof(out->status), out->folder,
                            "service.status") ||
      !vkr_bakery_path_join(out->bin_folder, sizeof(out->bin_folder),
                            out->folder, "bin") ||
      !vkr_bakery_path_join(out->log, sizeof(out->log), out->folder,
                            "service.log")) {
    return false_v;
  }
#if defined(_WIN32)
  return vkr_bakery_path_join(out->binary, sizeof(out->binary), out->bin_folder,
                              "vkr_depot.exe");
#else
  return vkr_bakery_path_join(out->binary, sizeof(out->binary), out->bin_folder,
                              "vkr_depot");
#endif
}

/* Calls `line_fn` with each line of `bytes`, without its end of line and
   trailing spaces; lines longer than SERVICE_LINE_MAX are cut. */
static void service_lines(const uint8_t *bytes, uint64_t size,
                          void (*line_fn)(void *context, char *line),
                          void *context) {
  uint64_t start = 0u;
  while (start < size) {
    uint64_t end = start;
    while (end < size && bytes[end] != '\n') {
      ++end;
    }
    char line[SERVICE_LINE_MAX];
    const uint64_t length = Min(end - start, (uint64_t)sizeof(line) - 1u);
    MemCopy(line, bytes + start, length);
    line[length] = '\0';
    uint64_t trimmed = length;
    while (trimmed > 0u &&
           (line[trimmed - 1u] == '\r' || line[trimmed - 1u] == ' ')) {
      line[--trimmed] = '\0';
    }
    start = end + 1u;
    if (line[0] && line[0] != '#') {
      line_fn(context, line);
    }
  }
}

static void service_settings_line(void *context, char *line) {
  VkrDepotServiceSettings *settings = context;
  char *value = strchr(line, ' ');
  if (!value) {
    return;
  }
  *value++ = '\0';
  if (strcmp(line, "bind") == 0) {
    snprintf(settings->bind, sizeof(settings->bind), "%s", value);
  } else if (strcmp(line, "name") == 0) {
    snprintf(settings->name, sizeof(settings->name), "%s", value);
  } else if (strcmp(line, "discovery-port") == 0) {
    const unsigned long port = strtoul(value, NULL, 10);
    settings->discovery_port =
        port > 0u && port < 65536u ? (uint32_t)port : VKR_DEPOT_DISCOVERY_PORT;
  } else if (strcmp(line, "outbound-only") == 0) {
    settings->outbound_only = strcmp(value, "1") == 0;
  } else if (strcmp(line, "store") == 0 &&
             settings->store_count < VKR_DEPOT_SERVER_REPOSITORIES_MAX) {
    /* `store NAME PATH`: the path is the rest of the line. */
    char *path = strchr(value, ' ');
    if (!path) {
      return;
    }
    *path++ = '\0';
    if (!vkr_depot_branch_valid(value) || !path[0]) {
      return;
    }
    VkrDepotServiceStore *store = &settings->stores[settings->store_count++];
    snprintf(store->name, sizeof(store->name), "%s", value);
    snprintf(store->root, sizeof(store->root), "%s", path);
  }
}

bool8_t vkr_depot_service_settings_load(const VkrDepotServicePaths *paths,
                                        VkrDepotServiceSettings *out) {
  MemZero(out, sizeof(*out));
  snprintf(out->bind, sizeof(out->bind), "%s", VKR_DEPOT_SERVICE_DEFAULT_BIND);
  out->discovery_port = VKR_DEPOT_DISCOVERY_PORT;
  if (!vkr_bakery_is_file(paths->conf)) {
    return true_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_read_file(paths->conf, 1u << 20, &bytes, &size)) {
    return false_v;
  }
  service_lines(bytes, size, service_settings_line, out);
  free(bytes);
  return true_v;
}

bool8_t
vkr_depot_service_settings_save(const VkrDepotServicePaths *paths,
                                const VkrDepotServiceSettings *settings) {
  const uint64_t capacity =
      1024u + (uint64_t)settings->store_count * SERVICE_LINE_MAX;
  char *text = malloc((size_t)capacity);
  if (!text) {
    return false_v;
  }
  int written = snprintf(text, (size_t)capacity,
                         "# vkr_depot service settings; `vkr_depot service` "
                         "and the editor write this file.\n"
                         "bind %s\n"
                         "discovery-port %u\n"
                         "outbound-only %u\n",
                         settings->bind, settings->discovery_port,
                         settings->outbound_only ? 1u : 0u);
  if (settings->name[0]) {
    written += snprintf(text + written, (size_t)capacity - (size_t)written,
                        "name %s\n", settings->name);
  }
  for (uint32_t i = 0u; i < settings->store_count; ++i) {
    written += snprintf(text + written, (size_t)capacity - (size_t)written,
                        "store %s %s\n", settings->stores[i].name,
                        settings->stores[i].root);
  }
  const bool8_t ok =
      vkr_bakery_make_directories(paths->folder) &&
      vkr_bakery_write_file_atomic(paths->conf, text, (uint64_t)written);
  free(text);
  return ok;
}

/* Copies the store's `server.key` to the service's when it has none. */
static bool8_t service_adopt_key(const VkrDepotServicePaths *paths,
                                 const char *store_root, bool8_t *out_adopted) {
  *out_adopted = false_v;
  char store_key[VKR_DEPOT_PATH_CAPACITY];
  if (vkr_bakery_is_file(paths->key) ||
      !vkr_bakery_path_join(store_key, sizeof(store_key), store_root,
                            "server.key") ||
      !vkr_bakery_is_file(store_key)) {
    return true_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  const bool8_t ok =
      vkr_bakery_read_file(store_key, 64u, &bytes, &size) &&
      size == VKR_NET_KEY_SIZE && vkr_bakery_make_directories(paths->folder) &&
      vkr_bakery_write_private_file_atomic(paths->key, bytes, size);
  if (bytes) {
    vkr_net_random(bytes, size);
  }
  free(bytes);
  *out_adopted = ok;
  return ok;
}

bool8_t vkr_depot_service_add(const VkrDepotServicePaths *paths,
                              VkrDepotServiceSettings *settings,
                              const char *store_root, const char *name,
                              char *message, uint32_t capacity) {
  char absolute[VKR_DEPOT_PATH_CAPACITY];
  char objects[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_absolute(store_root, absolute, sizeof(absolute)) ||
      !vkr_bakery_path_join(objects, sizeof(objects), absolute, "objects") ||
      !vkr_bakery_is_directory(objects)) {
    snprintf(message, capacity, "%s is not a depot store", store_root);
    return false_v;
  }
  const char *label = name ? name : vkr_bakery_path_name(absolute);
  if (!vkr_depot_branch_valid(label)) {
    snprintf(message, capacity, "invalid store name %s; give one", label);
    return false_v;
  }
  for (uint32_t i = 0u; i < settings->store_count; ++i) {
    if (service_same_path(settings->stores[i].root, absolute) ||
        strcmp(settings->stores[i].name, label) == 0) {
      snprintf(message, capacity, "the service already serves %s as %s",
               settings->stores[i].root, settings->stores[i].name);
      return false_v;
    }
  }
  if (settings->store_count == VKR_DEPOT_SERVER_REPOSITORIES_MAX) {
    snprintf(message, capacity, "the service serves at most %u stores",
             VKR_DEPOT_SERVER_REPOSITORIES_MAX);
    return false_v;
  }
  /* The first store's key becomes the machine's, so remotes that name a
     store this machine served already keep working. */
  bool8_t adopted = false_v;
  if (!service_adopt_key(paths, absolute, &adopted)) {
    snprintf(message, capacity, "could not adopt %s's server key", absolute);
    return false_v;
  }
  /* Discovery and HELLO name a repository by its ID. */
  uint8_t id[VKR_DEPOT_REPOSITORY_ID_SIZE];
  if (!vkr_depot_repository_id(absolute, id, true_v)) {
    snprintf(message, capacity, "could not give %s a repository ID", absolute);
    return false_v;
  }
  VkrDepotServiceStore *store = &settings->stores[settings->store_count++];
  snprintf(store->root, sizeof(store->root), "%s", absolute);
  snprintf(store->name, sizeof(store->name), "%s", label);
  if (!vkr_depot_service_settings_save(paths, settings)) {
    settings->store_count -= 1u;
    snprintf(message, capacity, "could not write %s", paths->conf);
    return false_v;
  }
  snprintf(message, capacity, "serving %s as %s%s", absolute, label,
           adopted ? "; the service adopts its server key" : "");
  return true_v;
}

bool8_t vkr_depot_service_remove(const VkrDepotServicePaths *paths,
                                 VkrDepotServiceSettings *settings,
                                 const char *which, char *message,
                                 uint32_t capacity) {
  char absolute[VKR_DEPOT_PATH_CAPACITY];
  const bool8_t path =
      vkr_bakery_path_absolute(which, absolute, sizeof(absolute));
  for (uint32_t i = 0u; i < settings->store_count; ++i) {
    if (strcmp(settings->stores[i].name, which) != 0 &&
        !(path && service_same_path(settings->stores[i].root, absolute))) {
      continue;
    }
    const VkrDepotServiceStore removed = settings->stores[i];
    for (uint32_t k = i; k + 1u < settings->store_count; ++k) {
      settings->stores[k] = settings->stores[k + 1u];
    }
    settings->store_count -= 1u;
    if (!vkr_depot_service_settings_save(paths, settings)) {
      snprintf(message, capacity, "could not write %s", paths->conf);
      return false_v;
    }
    snprintf(message, capacity, "no longer serving %s", removed.root);
    return true_v;
  }
  snprintf(message, capacity, "the service serves no %s", which);
  return false_v;
}

bool8_t vkr_depot_service_apply(VkrDepotService *service,
                                const VkrDepotServiceSettings *settings,
                                char *error, uint32_t capacity) {
  VkrDepotServiceInfo *info = malloc(sizeof(VkrDepotServiceInfo));
  if (!info) {
    snprintf(error, capacity, "out of memory");
    return false_v;
  }
  vkr_depot_service_info(service, info);
  /* From the last one down, so a removal moves no index still to visit. */
  for (uint32_t i = info->repository_count; i-- > 0u;) {
    const VkrDepotServerRepository *served = &info->repositories[i];
    bool8_t listed = false_v;
    for (uint32_t k = 0u; k < settings->store_count && !listed; ++k) {
      listed =
          service_same_path(settings->stores[k].root, served->store_root) &&
          strcmp(settings->stores[k].name, served->name) == 0;
    }
    if (!listed) {
      vkr_depot_service_remove_repository(service, i);
    }
  }
  vkr_depot_service_info(service, info);
  bool8_t ok = true_v;
  for (uint32_t k = 0u; k < settings->store_count; ++k) {
    bool8_t served = false_v;
    for (uint32_t i = 0u; i < info->repository_count && !served; ++i) {
      served = service_same_path(settings->stores[k].root,
                                 info->repositories[i].store_root);
    }
    if (!served && !vkr_depot_service_add_repository(
                       service, settings->stores[k].root,
                       settings->stores[k].name, error, capacity)) {
      ok = false_v;
    }
  }
  free(info);
  return ok;
}

static void service_hex(const uint8_t *bytes, uint32_t size, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (uint32_t i = 0u; i < size; ++i) {
    out[i * 2u] = digits[bytes[i] >> 4u];
    out[i * 2u + 1u] = digits[bytes[i] & 15u];
  }
  out[size * 2u] = '\0';
}

static bool8_t service_unhex(const char *text, uint8_t *out, uint32_t size) {
  for (uint32_t i = 0u; i < size * 2u; ++i) {
    const char c = text[i];
    const int32_t digit = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                 : -1;
    if (digit < 0) {
      return false_v;
    }
    out[i / 2u] = (uint8_t)(i % 2u ? (out[i / 2u] | digit) : digit << 4);
  }
  return text[size * 2u] == '\0' || text[size * 2u] == ' ';
}

bool8_t vkr_depot_service_publish(VkrDepotService *service, const char *path) {
  VkrDepotServiceInfo *info = malloc(sizeof(VkrDepotServiceInfo));
  const uint64_t capacity =
      4096u + (uint64_t)VKR_DEPOT_SERVER_REPOSITORIES_MAX * SERVICE_LINE_MAX +
      (uint64_t)VKR_DEPOT_SERVER_PEERS_MAX * 256u;
  char *text = malloc((size_t)capacity);
  if (!info || !text) {
    free(info);
    free(text);
    return false_v;
  }
  vkr_depot_service_info(service, info);
  char address[VKR_NET_ADDRESS_TEXT];
  vkr_net_address_format(&info->address, address, sizeof(address));
  /* The body without the time line, which the change check skips. */
  int written = snprintf(text, (size_t)capacity, "address %s\n", address);
  for (uint32_t i = 0u; i < info->repository_count; ++i) {
    written += snprintf(text + written, (size_t)capacity - (size_t)written,
                        "repository %s %s\n", info->repositories[i].name,
                        info->repositories[i].store_root);
  }
  for (uint32_t i = 0u; i < info->known_peer_count; ++i) {
    const VkrDepotServerPeer *peer = &info->known_peers[i];
    char key[65];
    service_hex(peer->key, 32u, key);
    /* Seconds, so steady traffic does not rewrite the file each time. */
    written += snprintf(text + written, (size_t)capacity - (size_t)written,
                        "peer %s %u %u %d %llu %s\n", key, peer->connections,
                        peer->outbound_only ? 1u : 0u, peer->repository,
                        (unsigned long long)(peer->last_seen_us / 1000000u),
                        peer->name);
  }
  free(info);
  const uint64_t size = (uint64_t)written;
  bool8_t ok = true_v;
  if (!service->published || service->published_size != size ||
      MemCompare(service->published, text, size) != 0) {
    char header[64];
    const int header_size = snprintf(
        header, sizeof(header), "# vkr_depot service status\ntime %llu\n",
        (unsigned long long)vkr_bakery_unix_seconds() * 1000000ull);
    char *file = malloc((size_t)(header_size + written));
    ok = file != NULL;
    if (ok) {
      MemCopy(file, header, (uint64_t)header_size);
      MemCopy(file + header_size, text, size);
      ok = vkr_bakery_write_file_atomic(path, file,
                                        (uint64_t)(header_size + written));
      free(file);
    }
    if (ok) {
      free(service->published);
      service->published = text;
      service->published_size = size;
      text = NULL;
    }
  }
  free(text);
  return ok;
}

static void service_status_line(void *context, char *line) {
  VkrDepotServiceStatus *status = context;
  if (strncmp(line, "time ", 5u) == 0) {
    status->published_us = strtoull(line + 5u, NULL, 10);
    return;
  }
  if (strncmp(line, "peer ", 5u) != 0 ||
      status->known_peer_count == VKR_DEPOT_SERVER_PEERS_MAX) {
    return;
  }
  VkrDepotServerPeer *peer = &status->known_peers[status->known_peer_count];
  MemZero(peer, sizeof(*peer));
  char *cursor = line + 5u;
  if (strlen(cursor) < 64u || !service_unhex(cursor, peer->key, 32u)) {
    return;
  }
  cursor += 64u;
  char *end = NULL;
  peer->connections = (uint32_t)strtoul(cursor, &end, 10);
  peer->outbound_only = strtoul(end, &end, 10) == 1u;
  peer->repository = (int32_t)strtol(end, &end, 10);
  peer->last_seen_us = strtoull(end, &end, 10) * 1000000u;
  snprintf(peer->name, sizeof(peer->name), "%s", *end == ' ' ? end + 1 : end);
  status->known_peer_count += 1u;
}

bool8_t vkr_depot_service_status(VkrDepotServiceStatus *out, uint32_t wait_ms) {
  MemZero(out, sizeof(*out));
  if (!vkr_depot_service_paths(&out->paths) ||
      !vkr_depot_service_settings_load(&out->paths, &out->settings)) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (vkr_bakery_is_file(out->paths.key) &&
      vkr_bakery_read_file(out->paths.key, 64u, &bytes, &size) &&
      size == VKR_NET_KEY_SIZE) {
    VkrNetKeyPair keys;
    vkr_net_keypair_from_secret(bytes, &keys);
    MemCopy(out->key, keys.public_key, 32u);
    out->has_key = true_v;
    vkr_net_random(&keys, sizeof(keys));
  }
  if (bytes) {
    vkr_net_random(bytes, size);
  }
  free(bytes);
  VkrNetAddress loopback;
  VkrDepotDiscovered found[4];
  const uint32_t count =
      vkr_depot_address("127.0.0.1:0", &loopback)
          ? vkr_depot_discover(&loopback,
                               (uint16_t)out->settings.discovery_port, wait_ms,
                               found, ArrayCount(found))
          : 0u;
  /* The machine's service answers with its key; another server on the
     port, such as an older editor's, is not it. */
  for (uint32_t i = 0u; i < count && !out->running; ++i) {
    out->running =
        !out->has_key || MemCompare(found[i].server_key, out->key, 32u) == 0;
    if (out->running) {
      out->answer = found[i];
    }
  }
  bytes = NULL;
  if (out->running && vkr_bakery_is_file(out->paths.status) &&
      vkr_bakery_read_file(out->paths.status, 1u << 20, &bytes, &size)) {
    service_lines(bytes, size, service_status_line, out);
  }
  free(bytes);
  return true_v;
}
