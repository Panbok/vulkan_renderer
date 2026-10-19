#include "vkr_depot_service.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_sync.h"
#include "vkr_net_host.h"
#include "vkr_net_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* After every pump, with the lock held. */
static void service_step(void *context, uint64_t now_us) {
  VkrDepotService *service = context;
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
  if (!allocator || !config || !config->store_root) {
    snprintf(error, capacity, "the service needs a store");
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
  if (!vkr_depot_server_key(config->store_root,
                            &host_config.core.static_keys)) {
    snprintf(error, capacity, "could not open the store at %s",
             config->store_root);
    goto failed;
  }
  MemCopy(service->server_key, host_config.core.static_keys.public_key, 32u);
  if (config->owner_admin && !service_grant_owner(config->store_root)) {
    snprintf(error, capacity, "could not make this user the store's admin");
    goto failed;
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
  const VkrDepotServerConfig server_config = {.store_root = config->store_root,
                                              .codec = config->codec,
                                              .open_access =
                                                  config->open_access};
  service->server =
      service->session
          ? vkr_depot_server_create(&service->memory_allocator,
                                    vkr_net_host_core(service->host),
                                    service->session, &server_config)
          : NULL;
  if (!service->server) {
    snprintf(error, capacity, "could not start the server on %s",
             config->store_root);
    goto failed;
  }
  /* Other machines find this server by its name, the store's folder unless
     the configuration gives one. */
  service->discovery_port = config->discovery_port
                                ? config->discovery_port
                                : (uint16_t)VKR_DEPOT_DISCOVERY_PORT;
  service->discoverable = vkr_depot_responder_open(
      &service->responder, service->discovery_port, service->address.port,
      service->server_key,
      config->name ? config->name : vkr_bakery_path_name(config->store_root),
      config->host);
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
  vkr_net_host_thread_lock(service->thread);
  const VkrDepotServerStats stats = vkr_depot_server_stats(service->server);
  out_info->failed = vkr_net_host_thread_failed(service->thread);
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
