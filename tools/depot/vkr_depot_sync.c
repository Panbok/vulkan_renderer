#include "vkr_depot_sync.h"

#include "platform/vkr_platform.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_discovery.h"
#include "vkr_net_crypto.h"
#include "vkr_net_host.h"
#include "vkr_net_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Identity, name and addresses
// =============================================================================

/* VKR_DEPOT_IDENTITY as UTF-8: the CRT's getenv gives the active code page
   on Windows, which turns a non-ASCII path into another one. */
static bool8_t depot_identity_override(char *out, uint32_t capacity) {
  return vkr_platform_environment_variable("VKR_DEPOT_IDENTITY", out,
                                           capacity) &&
         out[0];
}

bool8_t vkr_depot_identity_path(char *out, uint32_t capacity) {
  char override[VKR_DEPOT_PATH_CAPACITY];
  if (depot_identity_override(override, sizeof(override))) {
    return snprintf(out, capacity, "%s", override) < (int)capacity;
  }
  char data[VKR_DEPOT_PATH_CAPACITY];
  return vkr_bakery_user_data_directory(data, sizeof(data)) &&
         snprintf(out, capacity, "%s/depot/identity.key", data) < (int)capacity;
}

/* Where the key lived before 2026-10-11: the user cache folder, which
   cleanup tools empty. */
static bool8_t depot_identity_legacy_path(char *out, uint32_t capacity) {
  char cache[VKR_DEPOT_PATH_CAPACITY];
  return vkr_bakery_user_cache_directory(cache, sizeof(cache)) &&
         snprintf(out, capacity, "%s/vkr/depot/identity.key", cache) <
             (int)capacity;
}

static bool8_t depot_identity_read(const char *path, VkrNetKeyPair *out) {
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  bool8_t ok = vkr_bakery_is_file(path) &&
               vkr_bakery_read_file(path, 64u, &bytes, &size) &&
               size == VKR_NET_KEY_SIZE;
  if (ok) {
    vkr_net_keypair_from_secret(bytes, out);
  }
  if (bytes) {
    vkr_net_random(bytes, size);
  }
  free(bytes);
  return ok;
}

bool8_t vkr_depot_identity(VkrNetKeyPair *out, bool8_t create) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_depot_identity_path(path, sizeof(path))) {
    return false_v;
  }
  if (depot_identity_read(path, out)) {
    return true_v;
  }
  char override[VKR_DEPOT_PATH_CAPACITY];
  char legacy[VKR_DEPOT_PATH_CAPACITY];
  const bool8_t migrated =
      !depot_identity_override(override, sizeof(override)) &&
      depot_identity_legacy_path(legacy, sizeof(legacy)) &&
      depot_identity_read(legacy, out);
  if (!migrated && !create) {
    return false_v;
  }
  if (!migrated) {
    vkr_net_keypair_generate(out);
  }
  char directory[VKR_DEPOT_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  const bool8_t written = vkr_bakery_make_directories(directory) &&
                          vkr_bakery_write_private_file_atomic(
                              path, out->secret_key, VKR_NET_KEY_SIZE);
  /* A migrated key is valid even where the copy failed; the old file stays
     and the next run tries again. A new key must persist. */
  return written || migrated;
}

void vkr_depot_display_name(char out[VKR_DEPOT_DISPLAY_NAME]) {
  char user[VKR_DEPOT_DISPLAY_NAME * 2u];
#if defined(_WIN32)
  (void)vkr_platform_environment_variable("USERNAME", user, sizeof(user));
#else
  (void)vkr_platform_environment_variable("USER", user, sizeof(user));
#endif
  char host[VKR_DEPOT_DISCOVERY_TEXT];
  vkr_depot_host_name(host, sizeof(host));
  /* macOS names hosts `<computer>.local`; the computer is what people
     recognize. */
  const size_t length = strlen(host);
  if (length > 6u && strcmp(host + length - 6u, ".local") == 0) {
    host[length - 6u] = '\0';
  }
  char name[VKR_DEPOT_DISPLAY_NAME * 2u];
  snprintf(name, sizeof(name), "%s@%s", user[0] ? user : "user", host);
  (void)vkr_depot_display_name_clean((const uint8_t *)name,
                                     (uint32_t)strlen(name), out);
}

bool8_t vkr_depot_address(const char *text, VkrNetAddress *out) {
  if (vkr_net_address_parse(text, VKR_DEPOT_DEFAULT_PORT, out)) {
    return true_v;
  }
  char host[256];
  const char *colon = strrchr(text, ':');
  uint32_t port = VKR_DEPOT_DEFAULT_PORT;
  size_t length = strlen(text);
  if (colon) {
    port = (uint32_t)strtoul(colon + 1, NULL, 10);
    length = (size_t)(colon - text);
  }
  if (length == 0u || length >= sizeof(host) || port == 0u || port > 65535u) {
    return false_v;
  }
  memcpy(host, text, length);
  host[length] = '\0';
  return vkr_net_address_resolve(host, (uint16_t)port, true_v, out);
}

// =============================================================================
// Connection
// =============================================================================

struct VkrDepotConnection {
  VkrNetHost *host;
  VkrNetSession *session;
  VkrNetConnectionId connection;
  bool8_t connected;
  bool8_t closed;
  uint16_t close_code;
  VkrDepotClient *client;
};

static bool8_t depot_connection_pump(void *context, int32_t timeout_ms) {
  VkrDepotConnection *link = context;
  VkrNetCore *core = vkr_net_host_core(link->host);
  /* What the client queued since the last pump leaves in this pump's first
     send, not after its wait: a request waited a whole pump otherwise. */
  vkr_net_core_flush(core);
  if (!vkr_net_host_pump(link->host, timeout_ms)) {
    return false_v;
  }
  const uint64_t now = vkr_net_host_now(link->host);
  VkrNetEvent event;
  while (vkr_net_core_poll(core, &event)) {
    if (vkr_net_session_dispatch(link->session, &event, now) ||
        event.connection != link->connection) {
      continue;
    }
    if (event.type == VKR_NET_EVENT_CONNECTED) {
      link->connected = true_v;
    } else if (event.type == VKR_NET_EVENT_CLOSED) {
      link->closed = true_v;
      link->close_code = event.code;
    }
  }
  vkr_net_core_flush(core);
  return true_v;
}

static uint64_t depot_connection_now(void *context) {
  return vkr_net_host_now(((VkrDepotConnection *)context)->host);
}

void vkr_depot_disconnect(VkrDepotConnection *link) {
  if (!link) {
    return;
  }
  if (link->host && link->connected && !link->closed) {
    vkr_net_core_close(vkr_net_host_core(link->host), link->connection,
                       VKR_NET_CLOSE_APPLICATION, depot_connection_now(link));
    /* The core reports the connection closed as it queues the close frame,
       so the pump that sends the frame ends the wait; a peer that misses it
       closes at its idle timeout. */
    for (uint32_t i = 0u; i < 20u && !link->closed; ++i) {
      (void)depot_connection_pump(link, 10);
    }
  }
  /* The session calls the client's service on CLOSED: the client goes last. */
  vkr_net_session_destroy(link->session);
  vkr_depot_client_destroy(link->client);
  vkr_net_host_destroy(link->host);
  free(link);
}

static void depot_connect_status(VkrDepotConnectStatus *out,
                                 VkrDepotConnectStatus status) {
  if (out) {
    *out = status;
  }
}

bool8_t vkr_depot_outbound_only(void) {
  const char *variable = getenv("VKR_DEPOT_OUTBOUND_ONLY");
  if (variable && variable[0]) {
    return strcmp(variable, "1") == 0;
  }
  char identity[VKR_DEPOT_PATH_CAPACITY];
  char folder[VKR_DEPOT_PATH_CAPACITY];
  char marker[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_depot_identity_path(identity, sizeof(identity))) {
    return false_v;
  }
  vkr_bakery_path_parent(folder, sizeof(folder), identity);
  return vkr_bakery_path_join(marker, sizeof(marker), folder,
                              "outbound-only") &&
         vkr_bakery_is_file(marker);
}

/* Sends HELLO on a new connection; false with the connection's status and
   a message when the service refuses it. */
static bool8_t depot_connect_hello(VkrDepotConnection *link,
                                   const VkrDepotConnectConfig *config,
                                   VkrDepotConnectStatus *out_status,
                                   char *error, uint32_t capacity) {
  VkrDepotHello hello;
  const uint64_t flags =
      vkr_depot_outbound_only() ? VKR_DEPOT_HELLO_OUTBOUND_ONLY : 0u;
  if (!vkr_depot_client_hello(link->client, config->repository, flags,
                              &hello)) {
    depot_connect_status(out_status, VKR_DEPOT_CONNECT_REFUSED);
    snprintf(error, capacity, "%s did not answer the repository selection: %s",
             config->address, vkr_depot_client_error(link->client));
    return false_v;
  }
  if (config->out_hello) {
    *config->out_hello = hello;
  }
  if (hello.status == VKR_DEPOT_HELLO_OK) {
    return true_v;
  }
  if (hello.status == VKR_DEPOT_HELLO_NO_ROLE) {
    depot_connect_status(out_status, VKR_DEPOT_CONNECT_DENIED);
    snprintf(error, capacity,
             "%s does not let this machine's key use that repository",
             config->address);
  } else if (hello.status == VKR_DEPOT_HELLO_UNKNOWN_REPOSITORY) {
    depot_connect_status(out_status, VKR_DEPOT_CONNECT_REFUSED);
    snprintf(error, capacity, "%s serves no such repository", config->address);
  } else {
    depot_connect_status(out_status, VKR_DEPOT_CONNECT_REFUSED);
    snprintf(error, capacity,
             "%s serves several repositories; name one in the remote",
             config->address);
  }
  return false_v;
}

VkrDepotConnection *vkr_depot_connect(VkrAllocator *allocator,
                                      const VkrDepotConnectConfig *config,
                                      VkrDepotConnectStatus *out_status,
                                      char *error, uint32_t capacity) {
  VkrNetAddress server;
  VkrNetHostConfig host_config = {
      .socket = {.receive_buffer = 8u << 20, .send_buffer = 8u << 20}};
  depot_connect_status(out_status, VKR_DEPOT_CONNECT_INVALID);
  if (!config || !config->address || !config->server_key ||
      !vkr_depot_address(config->address, &server)) {
    snprintf(error, capacity, "invalid depot address %s",
             config && config->address ? config->address : "(none)");
    return NULL;
  }
  if (!vkr_depot_identity(&host_config.core.static_keys, true_v)) {
    snprintf(error, capacity, "no depot identity key");
    return NULL;
  }
  char name[VKR_DEPOT_DISPLAY_NAME];
  if (config->name) {
    (void)vkr_depot_display_name_clean((const uint8_t *)config->name,
                                       (uint32_t)strlen(config->name), name);
  } else {
    vkr_depot_display_name(name);
  }
  VkrDepotConnection *link = calloc(1u, sizeof(*link));
  if (!link) {
    vkr_net_random(&host_config.core.static_keys,
                   sizeof(host_config.core.static_keys));
    snprintf(error, capacity, "out of memory");
    return NULL;
  }
  host_config.bind =
      vkr_net_address_any((VkrNetAddressFamily)server.family, 0u);
  host_config.core.sent_packet_capacity = 16384u;
  host_config.core.background = true_v;
  link->host = vkr_net_host_create(allocator, &host_config);
  vkr_net_random(&host_config.core.static_keys,
                 sizeof(host_config.core.static_keys));
  link->session =
      link->host
          ? vkr_net_session_create(allocator, vkr_net_host_core(link->host))
          : NULL;
  if (!link->session ||
      !vkr_net_core_connect(vkr_net_host_core(link->host), &server,
                            config->server_key, (const uint8_t *)name,
                            (uint32_t)strlen(name), depot_connection_now(link),
                            &link->connection)) {
    snprintf(error, capacity, "could not start the connection");
    vkr_depot_disconnect(link);
    return NULL;
  }
  vkr_depot_progress_phase(config->progress, VKR_DEPOT_PHASE_CONNECTING, 0u,
                           0u);
  const uint32_t timeout_ms =
      config->timeout_ms ? config->timeout_ms : VKR_DEPOT_TIMEOUT_MS;
  const uint64_t limit =
      depot_connection_now(link) + (uint64_t)timeout_ms * 1000ull;
  bool8_t canceled = false_v;
  while (!link->connected && !link->closed &&
         depot_connection_now(link) < limit) {
    canceled = vkr_depot_progress_canceled(config->progress);
    if (canceled || !depot_connection_pump(link, 20)) {
      break;
    }
  }
  if (!link->connected) {
    if (canceled) {
      depot_connect_status(out_status, VKR_DEPOT_CONNECT_CANCELED);
      snprintf(error, capacity, "canceled");
    } else if (link->close_code == VKR_NET_CLOSE_ACCESS_DENIED) {
      depot_connect_status(out_status, VKR_DEPOT_CONNECT_DENIED);
      snprintf(error, capacity,
               "%s does not know this machine's key yet; it asks its owner "
               "to allow %s",
               config->address, name);
    } else if (!link->closed || link->close_code == VKR_NET_CLOSE_TIMEOUT) {
      depot_connect_status(out_status, VKR_DEPOT_CONNECT_UNREACHABLE);
      snprintf(error, capacity, "no answer from %s within %u ms",
               config->address, timeout_ms);
    } else {
      depot_connect_status(out_status, VKR_DEPOT_CONNECT_REFUSED);
      snprintf(error, capacity, "%s refused the connection (code %u)",
               config->address, link->close_code);
    }
    vkr_depot_disconnect(link);
    return NULL;
  }
  const VkrDepotLink client_link = {
      .core = vkr_net_host_core(link->host),
      .session = link->session,
      .connection = link->connection,
      .context = link,
      .pump = depot_connection_pump,
      .now = depot_connection_now,
  };
  link->client = vkr_depot_client_create(&client_link, config->store,
                                         VKR_DEPOT_TIMEOUT_MS, allocator);
  if (!link->client) {
    const bool8_t denied = link->close_code == VKR_NET_CLOSE_ACCESS_DENIED;
    depot_connect_status(out_status, denied ? VKR_DEPOT_CONNECT_DENIED
                                            : VKR_DEPOT_CONNECT_REFUSED);
    snprintf(error, capacity, "the server refused the depot service");
    vkr_depot_disconnect(link);
    return NULL;
  }
  vkr_depot_client_set_progress(link->client, config->progress);
  if (!depot_connect_hello(link, config, out_status, error, capacity)) {
    vkr_depot_disconnect(link);
    return NULL;
  }
  depot_connect_status(out_status, VKR_DEPOT_CONNECT_OK);
  return link;
}

VkrDepotClient *vkr_depot_connection_client(VkrDepotConnection *connection) {
  return connection ? connection->client : NULL;
}

bool8_t vkr_depot_branches(VkrAllocator *allocator,
                           const VkrDepotConnectConfig *config,
                           VkrDepotRef *out_refs, uint32_t capacity,
                           uint32_t *out_count,
                           VkrDepotConnectStatus *out_status, char *error,
                           uint32_t error_capacity) {
  *out_count = 0u;
  VkrDepotConnectConfig refs_config = *config;
  refs_config.store = NULL;
  VkrDepotConnection *connection = vkr_depot_connect(
      allocator, &refs_config, out_status, error, error_capacity);
  if (!connection) {
    return false_v;
  }
  const bool8_t ok =
      vkr_depot_client_refs(connection->client, out_refs, capacity, out_count);
  if (!ok) {
    snprintf(error, error_capacity, "listing branches failed: %s",
             vkr_depot_client_error(connection->client));
  }
  vkr_depot_disconnect(connection);
  return ok;
}

// =============================================================================
// Working copy
// =============================================================================

bool8_t vkr_depot_find_ref(VkrDepotClient *client, const char *branch,
                           VkrDepotId *out_commit, bool8_t *out_exists) {
  VkrDepotRef refs[VKR_DEPOT_REFS_MAX];
  uint32_t count = 0u;
  if (!vkr_depot_client_refs(client, refs, VKR_DEPOT_REFS_MAX, &count)) {
    return false_v;
  }
  *out_exists = false_v;
  for (uint32_t i = 0u; i < count; ++i) {
    if (strcmp(refs[i].name, branch) == 0) {
      *out_commit = refs[i].commit;
      *out_exists = true_v;
    }
  }
  return true_v;
}

// =============================================================================
// Mirror
// =============================================================================

/* Commits a branch's history may walk before the mirror calls it diverged. */
#define DEPOT_MIRROR_WALK_MAX 4096u

/* Whether `ancestor` is `start` or one of its ancestors. A commit missing
   from the store comes from `client`'s server, whose history holds it. */
static bool8_t depot_mirror_descends(const VkrDepotStore *store,
                                     VkrDepotClient *client,
                                     const VkrDepotId *start,
                                     const VkrDepotId *ancestor,
                                     bool8_t *out_descends) {
  VkrDepotId *pending = malloc(DEPOT_MIRROR_WALK_MAX * sizeof(*pending));
  VkrDepotId *seen = malloc(DEPOT_MIRROR_WALK_MAX * sizeof(*seen));
  uint32_t pending_count = 0u;
  uint32_t seen_count = 0u;
  bool8_t ok = pending && seen;
  *out_descends = false_v;
  if (ok) {
    pending[pending_count++] = *start;
  }
  while (ok && pending_count && !*out_descends) {
    const VkrDepotId id = pending[--pending_count];
    bool8_t known = false_v;
    for (uint32_t i = 0u; i < seen_count && !known; ++i) {
      known = vkr_depot_id_equal(&seen[i], &id);
    }
    if (known) {
      continue;
    }
    if (seen_count == DEPOT_MIRROR_WALK_MAX) {
      break;
    }
    seen[seen_count++] = id;
    if (vkr_depot_id_equal(&id, ancestor)) {
      *out_descends = true_v;
      break;
    }
    VkrDepotTransferStats stats = {0};
    if (!vkr_depot_store_has(store, &id) &&
        !vkr_depot_client_fetch(client, &id, &stats)) {
      ok = false_v;
      break;
    }
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    VkrDepotCommit commit;
    if (!vkr_depot_store_get(store, &id, &bytes, &size) ||
        !vkr_depot_commit_parse(bytes, size, &commit)) {
      free(bytes);
      ok = false_v;
      break;
    }
    for (uint32_t i = 0u;
         i < commit.parent_count && pending_count < DEPOT_MIRROR_WALK_MAX;
         ++i) {
      pending[pending_count++] = commit.parents[i];
    }
    free(bytes);
  }
  free(pending);
  free(seen);
  return ok;
}

/* Moves `branch` on `client`'s server from `expected` (zero creates it) to
   `commit`; the detail names where it is on a conflict. */
static bool8_t depot_mirror_move(VkrDepotClient *client, const char *branch,
                                 const VkrDepotId *expected,
                                 const VkrDepotId *commit, char *detail,
                                 uint32_t capacity) {
  VkrDepotId current;
  const VkrDepotRefStatus status =
      vkr_depot_client_update_ref(client, branch, expected, commit, &current);
  if (status == VKR_DEPOT_REF_OK) {
    return true_v;
  }
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&current, hex);
  if (status == VKR_DEPOT_REF_CONFLICT) {
    snprintf(detail, capacity, "the branch moved meanwhile, to %.12s", hex);
  } else {
    snprintf(detail, capacity, "the server refused the move (status %d)",
             (int)status);
  }
  return false_v;
}

static void depot_mirror_branch(const VkrDepotStore *store,
                                VkrDepotClient *local, VkrDepotClient *peer,
                                const char *branch, const VkrDepotRef *mine,
                                const VkrDepotRef *theirs,
                                VkrDepotMirrorVisit visit, void *context) {
  static const VkrDepotId none = {0};
  char detail[160] = {0};
  VkrDepotTransferStats stats = {0};
  if (mine && theirs && vkr_depot_id_equal(&mine->commit, &theirs->commit)) {
    visit(context, branch, VKR_DEPOT_MIRROR_SAME, &mine->commit, "");
    return;
  }
  bool8_t to_peer = mine && !theirs;
  bool8_t from_peer = theirs && !mine;
  if (mine && theirs) {
    bool8_t descends = false_v;
    if (!depot_mirror_descends(store, local, &mine->commit, &theirs->commit,
                               &descends)) {
      visit(context, branch, VKR_DEPOT_MIRROR_FAILED, &mine->commit,
            vkr_depot_client_error(local));
      return;
    }
    to_peer = descends;
    if (!to_peer) {
      if (!vkr_depot_client_fetch(peer, &theirs->commit, &stats) ||
          !depot_mirror_descends(store, peer, &theirs->commit, &mine->commit,
                                 &descends)) {
        visit(context, branch, VKR_DEPOT_MIRROR_FAILED, &theirs->commit,
              vkr_depot_client_error(peer));
        return;
      }
      from_peer = descends;
    }
    if (!to_peer && !from_peer) {
      visit(context, branch, VKR_DEPOT_MIRROR_DIVERGED, &theirs->commit,
            "both servers moved it; mirror only fast-forwards, so carry one "
            "side's change over in a working copy of the other and commit");
      return;
    }
  }
  if (to_peer) {
    const VkrDepotId *base = theirs ? &theirs->commit : &none;
    if (!vkr_depot_client_upload(peer, &mine->commit, base, &stats)) {
      visit(context, branch, VKR_DEPOT_MIRROR_FAILED, &mine->commit,
            vkr_depot_client_error(peer));
    } else if (!depot_mirror_move(peer, branch, base, &mine->commit, detail,
                                  sizeof(detail))) {
      visit(context, branch, VKR_DEPOT_MIRROR_FAILED, &mine->commit, detail);
    } else {
      visit(context, branch, VKR_DEPOT_MIRROR_TO_PEER, &mine->commit, "");
    }
    return;
  }
  const VkrDepotId *base = mine ? &mine->commit : &none;
  if (!vkr_depot_client_fetch(peer, &theirs->commit, &stats)) {
    visit(context, branch, VKR_DEPOT_MIRROR_FAILED, &theirs->commit,
          vkr_depot_client_error(peer));
  } else if (!vkr_depot_client_upload(local, &theirs->commit, base, &stats)) {
    visit(context, branch, VKR_DEPOT_MIRROR_FAILED, &theirs->commit,
          vkr_depot_client_error(local));
  } else if (!depot_mirror_move(local, branch, base, &theirs->commit, detail,
                                sizeof(detail))) {
    visit(context, branch, VKR_DEPOT_MIRROR_FAILED, &theirs->commit, detail);
  } else {
    visit(context, branch, VKR_DEPOT_MIRROR_FROM_PEER, &theirs->commit, "");
  }
}

static const VkrDepotRef *
depot_mirror_find(const VkrDepotRef *refs, uint32_t count, const char *branch) {
  for (uint32_t i = 0u; i < count; ++i) {
    if (strcmp(refs[i].name, branch) == 0) {
      return &refs[i];
    }
  }
  return NULL;
}

bool8_t vkr_depot_mirror(VkrAllocator *allocator, const VkrDepotStore *store,
                         const char *local_address, const uint8_t local_key[32],
                         const char *peer_address, const uint8_t peer_key[32],
                         const char *branch, VkrDepotMirrorVisit visit,
                         void *context, char *error, uint32_t capacity) {
  VkrDepotRef *local_refs = malloc(VKR_DEPOT_REFS_MAX * sizeof(*local_refs));
  VkrDepotRef *peer_refs = malloc(VKR_DEPOT_REFS_MAX * sizeof(*peer_refs));
  uint32_t local_count = 0u;
  uint32_t peer_count = 0u;
  VkrDepotConnection *local = NULL;
  VkrDepotConnection *peer = NULL;
  bool8_t ok = false_v;
  if (!local_refs || !peer_refs) {
    snprintf(error, capacity, "out of memory");
    goto cleanup;
  }
  const VkrDepotConnectConfig local_config = {
      .address = local_address, .server_key = local_key, .store = store};
  const VkrDepotConnectConfig peer_config = {
      .address = peer_address, .server_key = peer_key, .store = store};
  local = vkr_depot_connect(allocator, &local_config, NULL, error, capacity);
  peer = local
             ? vkr_depot_connect(allocator, &peer_config, NULL, error, capacity)
             : NULL;
  if (!peer) {
    goto cleanup;
  }
  VkrDepotClient *local_client = vkr_depot_connection_client(local);
  VkrDepotClient *peer_client = vkr_depot_connection_client(peer);
  if (!vkr_depot_client_refs(local_client, local_refs, VKR_DEPOT_REFS_MAX,
                             &local_count) ||
      !vkr_depot_client_refs(peer_client, peer_refs, VKR_DEPOT_REFS_MAX,
                             &peer_count)) {
    snprintf(error, capacity, "listing branches failed: %s",
             vkr_depot_client_error(local_count ? peer_client : local_client));
    goto cleanup;
  }
  /* Every branch of either side once, this side's first. */
  for (uint32_t side = 0u; side < 2u; ++side) {
    const VkrDepotRef *refs = side == 0u ? local_refs : peer_refs;
    const uint32_t count = side == 0u ? local_count : peer_count;
    for (uint32_t i = 0u; i < count; ++i) {
      const char *name = refs[i].name;
      if ((branch && strcmp(name, branch) != 0) ||
          (side == 1u && depot_mirror_find(local_refs, local_count, name))) {
        continue;
      }
      depot_mirror_branch(store, local_client, peer_client, name,
                          depot_mirror_find(local_refs, local_count, name),
                          depot_mirror_find(peer_refs, peer_count, name), visit,
                          context);
    }
  }
  ok = true_v;
cleanup:
  vkr_depot_disconnect(peer);
  vkr_depot_disconnect(local);
  free(local_refs);
  free(peer_refs);
  return ok;
}

// =============================================================================
// Status and log
// =============================================================================

void vkr_depot_change_list_free(VkrDepotChangeList *list) {
  for (uint32_t i = 0u; i < list->count; ++i) {
    free(list->entries[i].path);
  }
  free(list->entries);
  MemZero(list, sizeof(*list));
}

static bool8_t depot_change_list_add(VkrDepotChangeList *list, const char *path,
                                     VkrDepotChange change) {
  if (list->count == list->capacity) {
    const uint32_t capacity = list->capacity ? list->capacity * 2u : 32u;
    VkrDepotChangeEntry *entries =
        realloc(list->entries, (size_t)capacity * sizeof(VkrDepotChangeEntry));
    if (!entries) {
      return false_v;
    }
    list->entries = entries;
    list->capacity = capacity;
  }
  const size_t length = strlen(path);
  char *copy = malloc(length + 1u);
  if (!copy) {
    return false_v;
  }
  MemCopy(copy, path, length + 1u);
  list->entries[list->count++] =
      (VkrDepotChangeEntry){.path = copy, .change = change};
  return true_v;
}

static bool8_t depot_collect_change(void *context, const char *path,
                                    VkrDepotChange change) {
  return depot_change_list_add(context, path, change);
}

/* A failed step's message: "canceled" when the progress asked for it. */
static void depot_step_error(const VkrDepotWorkspace *workspace, char *error,
                             uint32_t capacity, const char *message,
                             const char *detail) {
  if (vkr_depot_progress_canceled(workspace->progress)) {
    snprintf(error, capacity, "canceled");
  } else if (detail) {
    snprintf(error, capacity, "%s: %s", message, detail);
  } else {
    snprintf(error, capacity, "%s", message);
  }
}

static void depot_step_done(VkrDepotWorkspace *workspace) {
  if (workspace->progress) {
    vkr_atomic_uint32_store(&workspace->progress->phase, VKR_DEPOT_PHASE_DONE,
                            VKR_MEMORY_ORDER_RELEASE);
  }
}

bool8_t vkr_depot_status(VkrDepotWorkspace *workspace,
                         VkrDepotChangeList *out_changes, char *error,
                         uint32_t capacity) {
  MemZero(out_changes, sizeof(*out_changes));
  VkrDepotId tree;
  VkrDepotFileList files;
  if (!vkr_depot_workspace_snapshot(workspace, &tree, &files)) {
    depot_step_error(workspace, error, capacity,
                     "the working directory snapshot failed", NULL);
    return false_v;
  }
  bool8_t ok = vkr_depot_file_list_diff(&workspace->index, &files,
                                        depot_collect_change, out_changes);
  if (!ok) {
    snprintf(error, capacity, "out of memory");
    vkr_depot_change_list_free(out_changes);
  }
  /* Files a checkout wrote are hashed once; keep what this one verified. */
  if (ok && vkr_depot_workspace_settle(workspace, &files) &&
      !vkr_depot_workspace_save(workspace)) {
    snprintf(error, capacity, "could not save the working copy's index");
    vkr_depot_change_list_free(out_changes);
    ok = false_v;
  }
  vkr_depot_file_list_free(&files);
  if (ok) {
    depot_step_done(workspace);
  }
  return ok;
}

uint32_t vkr_depot_log(const VkrDepotStore *store, const VkrDepotId *start,
                       uint32_t count, VkrDepotLogVisitor visit,
                       void *context) {
  VkrDepotId commit = *start;
  uint32_t visited = 0u;
  while (visited < count && !vkr_depot_id_zero(&commit)) {
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    VkrDepotCommit parsed;
    if (!vkr_depot_store_get(store, &commit, &bytes, &size) ||
        !vkr_depot_commit_parse(bytes, size, &parsed)) {
      free(bytes);
      break;
    }
    VkrDepotLogEntry entry = {.id = commit,
                              .time_us = parsed.time_us,
                              .message = parsed.message,
                              .message_size = parsed.message_size};
    if (parsed.parent_count) {
      entry.parent = parsed.parents[0];
    }
    MemCopy(entry.author, parsed.author, 32u);
    visited += 1u;
    const bool8_t more = visit(context, &entry);
    commit = entry.parent;
    free(bytes);
    if (!more) {
      break;
    }
  }
  return visited;
}

// =============================================================================
// Commit, fetch and checkout
// =============================================================================

void vkr_depot_pending_commit_free(VkrDepotPendingCommit *pending) {
  vkr_depot_file_list_free(&pending->files);
}

void vkr_depot_cooked_branch(const char *branch, char *out, uint32_t capacity) {
#if defined(__APPLE__)
  const char *family = "astc";
#else
  const char *family = "bc";
#endif
  snprintf(out, capacity, "%s.cooked.%s", branch, family);
}

bool8_t vkr_depot_make_commit(const VkrDepotStore *store,
                              const VkrDepotId *tree, const VkrDepotId *parents,
                              uint32_t parent_count, const char *message,
                              VkrDepotId *out_commit, char *error,
                              uint32_t capacity) {
  if (parent_count > VKR_DEPOT_PARENTS_MAX) {
    snprintf(error, capacity, "a commit has at most %u parents",
             VKR_DEPOT_PARENTS_MAX);
    return false_v;
  }
  VkrNetKeyPair identity;
  if (!vkr_depot_identity(&identity, true_v)) {
    snprintf(error, capacity, "no depot identity key");
    return false_v;
  }
  VkrDepotCommit commit = {.tree = *tree,
                           .time_us =
                               (uint64_t)vkr_bakery_unix_seconds() * 1000000u,
                           .message = message,
                           .message_size = (uint32_t)strlen(message)};
  memcpy(commit.author, identity.public_key, 32u);
  vkr_net_random(identity.secret_key, sizeof(identity.secret_key));
  for (uint32_t i = 0u; i < parent_count; ++i) {
    if (!vkr_depot_id_zero(&parents[i])) {
      commit.parents[commit.parent_count++] = parents[i];
    }
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_depot_commit_encode(&commit, &bytes, &size)) {
    snprintf(error, capacity, "the commit message is too long");
    return false_v;
  }
  vkr_depot_id_of(bytes, size, out_commit);
  const bool8_t stored = vkr_depot_store_put(store, out_commit, bytes, size);
  free(bytes);
  if (!stored) {
    snprintf(error, capacity, "the commit could not be stored");
  }
  return stored;
}

/* Stores a commit of `tree` with `parent` (none when zero) by this user's
   identity. */
static bool8_t depot_make_commit(const VkrDepotStore *store,
                                 const VkrDepotId *tree,
                                 const VkrDepotId *parent, const char *message,
                                 VkrDepotId *out_commit, char *error,
                                 uint32_t capacity) {
  return vkr_depot_make_commit(store, tree, parent, parent ? 1u : 0u, message,
                               out_commit, error, capacity);
}

bool8_t vkr_depot_commit_snapshot(VkrDepotWorkspace *workspace,
                                  const VkrDepotId *parent, const char *message,
                                  VkrDepotPendingCommit *out_pending,
                                  char *error, uint32_t capacity) {
  VkrDepotId tree;
  VkrDepotFileList files = {0};
  MemZero(out_pending, sizeof(*out_pending));
  if (!vkr_depot_workspace_snapshot(workspace, &tree, &files)) {
    depot_step_error(workspace, error, capacity,
                     "the working directory snapshot failed", NULL);
    return false_v;
  }
  if (!depot_make_commit(&workspace->store, &tree, parent, message,
                         &out_pending->commit, error, capacity)) {
    vkr_depot_file_list_free(&files);
    return false_v;
  }
  out_pending->files = files;
  return true_v;
}

/* The project commit a cooked commit was cooked for, from its message;
   false when the message names none. */
static bool8_t depot_cooked_base(const VkrDepotCommit *commit,
                                 VkrDepotId *out_base) {
  const uint32_t prefix = (uint32_t)strlen(VKR_DEPOT_COOKED_MESSAGE);
  char hex[VKR_DEPOT_ID_HEX];
  if (commit->message_size != prefix + VKR_DEPOT_ID_HEX - 1u ||
      memcmp(commit->message, VKR_DEPOT_COOKED_MESSAGE, prefix) != 0) {
    return false_v;
  }
  memcpy(hex, commit->message + prefix, VKR_DEPOT_ID_HEX - 1u);
  hex[VKR_DEPOT_ID_HEX - 1u] = '\0';
  return vkr_depot_id_parse(hex, out_base);
}

/* The files of `commit`'s tree, when the store holds them; empty
   otherwise. */
static void depot_commit_files(const VkrDepotStore *store,
                               const VkrDepotId *commit,
                               VkrDepotFileList *out_files) {
  MemZero(out_files, sizeof(*out_files));
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  VkrDepotCommit parsed;
  if (!vkr_depot_id_zero(commit) &&
      vkr_depot_store_get(store, commit, &bytes, &size) &&
      vkr_depot_commit_parse(bytes, size, &parsed) &&
      !vkr_depot_tree_files(store, &parsed.tree, out_files)) {
    vkr_depot_file_list_free(out_files);
  }
  free(bytes);
}

bool8_t vkr_depot_fetch_cooked(VkrDepotWorkspace *workspace,
                               VkrDepotClient *client, const char *branch,
                               VkrDepotCookedFetch *out_fetch, char *error,
                               uint32_t capacity) {
  MemZero(out_fetch, sizeof(*out_fetch));
  vkr_depot_cooked_branch(branch, out_fetch->branch, sizeof(out_fetch->branch));
  bool8_t exists = false_v;
  if (!vkr_depot_find_ref(client, out_fetch->branch, &out_fetch->commit,
                          &exists)) {
    snprintf(error, capacity, "the branches did not arrive: %s",
             vkr_depot_client_error(client));
    return false_v;
  }
  if (!exists) {
    MemZero(&out_fetch->commit, sizeof(out_fetch->commit));
    return true_v;
  }
  if (!vkr_depot_client_fetch_trees(client, &out_fetch->commit, NULL)) {
    snprintf(error, capacity, "the cooked files' list did not arrive: %s",
             vkr_depot_client_error(client));
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  VkrDepotCommit commit;
  VkrDepotFileList cooked = {0};
  VkrDepotId base = {0};
  bool8_t ok = vkr_depot_store_get(&workspace->store, &out_fetch->commit,
                                   &bytes, &size) &&
               vkr_depot_commit_parse(bytes, size, &commit) &&
               vkr_depot_tree_files(&workspace->store, &commit.tree, &cooked);
  if (ok && !depot_cooked_base(&commit, &base)) {
    MemZero(&base, sizeof(base));
  }
  free(bytes);
  if (!ok) {
    snprintf(error, capacity, "the cooked commit could not be read");
    return false_v;
  }
  out_fetch->listed = cooked.count;

  /* A cooked `<source>.vkt` holds the source it was cooked from at that
     commit; another source cooks here instead. Build textures belong to a
     build revision that does not change. A file already here stays. */
  VkrDepotFileList then = {0};
  depot_commit_files(&workspace->store, &base, &then);
  VkrDepotFileList cache = {0};
  (void)vkr_depot_cooked_cache_load(workspace, &cache);
  uint32_t *wanted = calloc(cooked.count + 1u, sizeof(uint32_t));
  VkrDepotId *blobs = calloc(cooked.count + 1u, sizeof(VkrDepotId));
  uint32_t want_count = 0u;
  uint32_t blob_count = 0u;
  ok = wanted && blobs;
  for (uint32_t i = 0u; ok && i < cooked.count; ++i) {
    const VkrDepotIndexEntry *entry = &cooked.entries[i];
    const size_t length = strlen(entry->path);
    const size_t suffix = sizeof(VKR_DEPOT_COOKED_SUFFIX) - 1u;
    char target[VKR_DEPOT_PATH_CAPACITY];
    if (length <= suffix ||
        strcmp(entry->path + length - suffix, VKR_DEPOT_COOKED_SUFFIX) != 0 ||
        !vkr_depot_path_valid(entry->path) ||
        vkr_depot_file_list_find(&workspace->index, entry->path) ||
        !vkr_bakery_path_join(target, sizeof(target), workspace->root,
                              entry->path)) {
      continue;
    }
    if (vkr_bakery_is_file(target)) {
      out_fetch->present += 1u;
      continue;
    }
    char source[VKR_DEPOT_PATH_CAPACITY];
    snprintf(source, sizeof(source), "%.*s", (int)(length - suffix),
             entry->path);
    const VkrDepotIndexEntry *now =
        vkr_depot_file_list_find(&workspace->index, source);
    const VkrDepotIndexEntry *was = vkr_depot_file_list_find(&then, source);
    if (now && (!was || !vkr_depot_id_equal(&now->blob, &was->blob))) {
      out_fetch->stale += 1u;
      continue;
    }
    wanted[want_count++] = i;
    if (!vkr_depot_store_has(&workspace->store, &entry->blob)) {
      blobs[blob_count++] = entry->blob;
    }
  }
  VkrDepotTransferStats stats = {0};
  if (ok && blob_count &&
      !vkr_depot_client_fetch_files(client, blobs, blob_count, &stats)) {
    snprintf(error, capacity, "the cooked files did not arrive: %s",
             vkr_depot_client_error(client));
    ok = false_v;
  }
  out_fetch->stored_bytes = stats.stored_bytes;

  /* Written files join the cooked cache, so publishing them again hashes
     nothing. */
  VkrDepotFileList written = {0};
  for (uint32_t w = 0u; ok && w < want_count; ++w) {
    const VkrDepotIndexEntry *entry = &cooked.entries[wanted[w]];
    char target[VKR_DEPOT_PATH_CAPACITY];
    VkrBakeryStat stat;
    if (vkr_bakery_path_join(target, sizeof(target), workspace->root,
                             entry->path) &&
        vkr_depot_write_blob(&workspace->store, &entry->blob, target) &&
        vkr_bakery_stat(target, &stat) && stat.exists) {
      out_fetch->written += 1u;
      (void)vkr_depot_file_list_add(&written, entry->path, stat.size,
                                    stat.mtime_ns, &entry->blob);
    }
  }
  if (written.count) {
    vkr_depot_file_list_sort(&written);
    VkrDepotFileList merged = {0};
    for (uint32_t i = 0u; i < written.count; ++i) {
      const VkrDepotIndexEntry *entry = &written.entries[i];
      (void)vkr_depot_file_list_add(&merged, entry->path, entry->size,
                                    entry->mtime_ns, &entry->blob);
    }
    for (uint32_t i = 0u; i < cache.count; ++i) {
      const VkrDepotIndexEntry *entry = &cache.entries[i];
      if (!vkr_depot_file_list_find(&written, entry->path)) {
        (void)vkr_depot_file_list_add(&merged, entry->path, entry->size,
                                      entry->mtime_ns, &entry->blob);
      }
    }
    vkr_depot_file_list_sort(&merged);
    (void)vkr_depot_cooked_cache_save(workspace, &merged);
    vkr_depot_file_list_free(&merged);
  }
  vkr_depot_file_list_free(&written);
  vkr_depot_file_list_free(&cache);
  vkr_depot_file_list_free(&then);
  vkr_depot_file_list_free(&cooked);
  free(wanted);
  free(blobs);
  if (!ok && !error[0]) {
    snprintf(error, capacity, "out of memory");
  }
  return ok;
}

bool8_t vkr_depot_cooked_snapshot(VkrDepotWorkspace *workspace,
                                  VkrDepotId *out_tree, uint32_t *out_files,
                                  char *error, uint32_t capacity) {
  VkrDepotFileList files = {0};
  if (!vkr_depot_workspace_snapshot_cooked(workspace, out_tree, &files)) {
    depot_step_error(workspace, error, capacity,
                     "the cooked files could not be read", NULL);
    return false_v;
  }
  *out_files = files.count;
  vkr_depot_file_list_free(&files);
  if (*out_files == 0u) {
    snprintf(error, capacity, "the working copy holds no cooked files");
    return false_v;
  }
  return true_v;
}

bool8_t vkr_depot_publish_cooked(VkrDepotWorkspace *workspace,
                                 VkrDepotClient *client, const char *branch,
                                 const VkrDepotId *tree, uint32_t files,
                                 VkrDepotCookedResult *out_result, char *error,
                                 uint32_t capacity) {
  MemZero(out_result, sizeof(*out_result));
  out_result->files = files;
  vkr_depot_cooked_branch(branch, out_result->branch,
                          sizeof(out_result->branch));
  VkrDepotId parent = {0};
  bool8_t exists = false_v;
  if (!vkr_depot_find_ref(client, out_result->branch, &parent, &exists)) {
    snprintf(error, capacity, "the branches did not arrive: %s",
             vkr_depot_client_error(client));
    return false_v;
  }
  if (!exists) {
    MemZero(&parent, sizeof(parent));
  }
  /* The files the side branch holds already publish nothing. Its commit is
     in the store once this working copy published or fetched it. */
  if (!vkr_depot_id_zero(&parent)) {
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    VkrDepotCommit current;
    const bool8_t same =
        vkr_depot_store_get(&workspace->store, &parent, &bytes, &size) &&
        vkr_depot_commit_parse(bytes, size, &current) &&
        vkr_depot_id_equal(&current.tree, tree);
    free(bytes);
    if (same) {
      out_result->commit = parent;
      out_result->unchanged = true_v;
      return true_v;
    }
  }
  char base[VKR_DEPOT_ID_HEX];
  char message[160];
  vkr_depot_id_hex(&workspace->base, base);
  snprintf(message, sizeof(message), VKR_DEPOT_COOKED_MESSAGE "%s", base);
  if (!depot_make_commit(&workspace->store, tree, &parent, message,
                         &out_result->commit, error, capacity)) {
    return false_v;
  }
  vkr_depot_client_set_progress(client, workspace->progress);
  VkrDepotTransferStats stats = {0};
  if (!vkr_depot_client_upload(client, &out_result->commit, &parent, &stats)) {
    depot_step_error(workspace, error, capacity, "upload failed",
                     vkr_depot_client_error(client));
    return false_v;
  }
  VkrDepotId current;
  const VkrDepotRefStatus status = vkr_depot_client_update_ref(
      client, out_result->branch, &parent, &out_result->commit, &current);
  if (status == VKR_DEPOT_REF_CONFLICT) {
    snprintf(error, capacity, "%s moved meanwhile; publish again",
             out_result->branch);
    return false_v;
  }
  if (status != VKR_DEPOT_REF_OK) {
    snprintf(error, capacity,
             "the server refused the cooked commit (status %u)", status);
    return false_v;
  }
  out_result->uploaded_bytes = stats.stored_bytes;
  depot_step_done(workspace);
  return true_v;
}

/* Points the working copy's branch in its store at `commit`, and, with
   `track_origin`, origin's record of that branch too when the working copy's
   server is origin. The steps that move the base through the server (the
   editor's Submit and Get latest, clone) keep the store's branches in step
   so. Best effort: the base moved already, and a later commit or pull
   reports a branch that disagrees with it. */
static void depot_branch_follow(VkrDepotWorkspace *workspace,
                                const VkrDepotId *commit,
                                bool8_t track_origin) {
  const char *store = workspace->store.root;
  for (uint32_t attempt = 0u; attempt < 4u; ++attempt) {
    VkrDepotId current = {0};
    if (vkr_depot_ref_read(store, workspace->branch, &current) &&
        vkr_depot_id_equal(&current, commit)) {
      break;
    }
    if (vkr_depot_ref_swap(store, workspace->branch, &current, commit, NULL) !=
        VKR_DEPOT_REF_SWAP_MOVED) {
      break;
    }
  }
  VkrDepotRemoteEntry origin;
  if (track_origin && workspace->remote[0] &&
      vkr_depot_remote_find(store, VKR_DEPOT_ORIGIN, &origin) &&
      strcmp(origin.address, workspace->remote) == 0 &&
      MemCompare(origin.key, workspace->server_key, 32u) == 0) {
    (void)vkr_depot_tracking_set(store, VKR_DEPOT_ORIGIN, workspace->branch,
                                 commit);
  }
}

bool8_t vkr_depot_commit_push(VkrDepotWorkspace *workspace,
                              VkrDepotClient *client, const char *branch,
                              const VkrDepotId *parent,
                              const VkrDepotId *expected,
                              VkrDepotPendingCommit *pending, char *error,
                              uint32_t capacity) {
  if (!vkr_depot_branch_valid(branch)) {
    snprintf(error, capacity, "invalid branch name %s", branch);
    return false_v;
  }
  vkr_depot_client_set_progress(client, workspace->progress);
  VkrDepotTransferStats stats = {0};
  if (!vkr_depot_client_upload(client, &pending->commit, parent, &stats)) {
    depot_step_error(workspace, error, capacity, "upload failed",
                     vkr_depot_client_error(client));
    return false_v;
  }
  VkrDepotId current;
  const VkrDepotRefStatus status = vkr_depot_client_update_ref(
      client, branch, expected, &pending->commit, &current);
  if (status == VKR_DEPOT_REF_CONFLICT) {
    char hex[VKR_DEPOT_ID_HEX];
    vkr_depot_id_hex(&current, hex);
    snprintf(error, capacity, "%s moved to %.12s; pull first", branch, hex);
    return false_v;
  }
  if (status == VKR_DEPOT_REF_INVALID) {
    snprintf(error, capacity,
             "the server refused the commit: a name no tree may hold, or a "
             "tree past the depot's limits");
    return false_v;
  }
  if (status == VKR_DEPOT_REF_LOCKED) {
    snprintf(error, capacity, "the commit changes a file another user locked");
    return false_v;
  }
  if (status != VKR_DEPOT_REF_OK) {
    snprintf(error, capacity, "the server refused the commit (status %u)",
             status);
    return false_v;
  }
  pending->uploaded = stats;
  vkr_depot_file_list_free(&workspace->index);
  workspace->index = pending->files;
  MemZero(&pending->files, sizeof(pending->files));
  workspace->base = pending->commit;
  snprintf(workspace->branch, sizeof(workspace->branch), "%s", branch);
  if (!vkr_depot_workspace_save(workspace)) {
    snprintf(error, capacity,
             "pushed, but the working copy state did not save");
    return false_v;
  }
  /* The new index recorded every file the log names, or its deletion. */
  (void)vkr_depot_workspace_clear_fetched(workspace);
  depot_branch_follow(workspace, &pending->commit, true_v);
  depot_step_done(workspace);
  return true_v;
}

/* The blobs a sparse checkout of `remote` writes: files on disk that the
   commit changed, and the root ignore rules, unless `keep` holds them. */
static bool8_t depot_sparse_writes(const VkrDepotFileList *local,
                                   const VkrDepotFileList *remote,
                                   const VkrDepotFileList *keep,
                                   VkrDepotId **out_blobs,
                                   uint32_t *out_count) {
  *out_blobs = malloc((size_t)Max(remote->count, 1u) * sizeof(VkrDepotId));
  *out_count = 0u;
  if (!*out_blobs) {
    return false_v;
  }
  for (uint32_t i = 0u; i < remote->count; ++i) {
    const VkrDepotIndexEntry *theirs = &remote->entries[i];
    const VkrDepotIndexEntry *mine =
        vkr_depot_file_list_find(local, theirs->path);
    const bool8_t on_disk = mine && mine->mtime_ns != VKR_DEPOT_MTIME_UNFETCHED;
    const bool8_t ignore_rules =
        strcmp(theirs->path, VKR_DEPOT_IGNORE_FILE) == 0;
    if ((on_disk || ignore_rules) &&
        !vkr_depot_file_list_find(keep, theirs->path) &&
        !(on_disk && vkr_depot_id_equal(&mine->blob, &theirs->blob))) {
      (*out_blobs)[(*out_count)++] = theirs->blob;
    }
  }
  return true_v;
}

/* Whether two entries of a path, either NULL for absent, hold the same
   content. */
static bool8_t depot_same_content(const VkrDepotIndexEntry *a,
                                  const VkrDepotIndexEntry *b) {
  if (!a || !b) {
    return a == b;
  }
  return vkr_depot_id_equal(&a->blob, &b->blob);
}

/* vkr_depot_fetch_checkout; `track_origin` as for depot_branch_follow. */
static bool8_t depot_checkout_commit(VkrDepotWorkspace *workspace,
                                     VkrDepotClient *client,
                                     const VkrDepotId *commit,
                                     VkrDepotConflictPolicy policy,
                                     bool8_t track_origin, uint32_t *out_kept,
                                     VkrDepotChangeList *out_conflicts,
                                     char *error, uint32_t capacity) {
  VkrDepotFileList local = {0};
  VkrDepotChangeList changes = {0};
  VkrDepotChangeList conflicts = {0};
  VkrDepotFileList remote = {0};
  VkrDepotFileList keep = {0};
  bool8_t ok = false_v;
  *out_kept = 0u;
  if (out_conflicts) {
    MemZero(out_conflicts, sizeof(*out_conflicts));
  }
  if (vkr_depot_id_equal(commit, &workspace->base)) {
    depot_branch_follow(workspace, commit, track_origin);
    depot_step_done(workspace);
    return true_v;
  }
  VkrDepotTransferStats stats = {0};
  if (client) {
    vkr_depot_client_set_progress(client, workspace->progress);
  }
  /* A sparse working copy fetches the trees here, and below only the files
     its checkout writes. */
  const bool8_t fetched =
      !client ||
      (workspace->sparse ? vkr_depot_client_fetch_trees(client, commit, &stats)
                         : vkr_depot_client_fetch(client, commit, &stats));
  if (!fetched) {
    depot_step_error(workspace, error, capacity, "fetch failed",
                     vkr_depot_client_error(client));
    goto cleanup;
  }
  /* Local changes stay, unless the commit changed the same paths. */
  VkrDepotId tree;
  if (!vkr_depot_workspace_snapshot(workspace, &tree, &local)) {
    depot_step_error(workspace, error, capacity,
                     "the working directory snapshot failed", NULL);
    goto cleanup;
  }
  if (!vkr_depot_file_list_diff(&workspace->index, &local, depot_collect_change,
                                &changes)) {
    snprintf(error, capacity, "out of memory");
    goto cleanup;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  VkrDepotCommit parsed;
  if (!vkr_depot_store_get(&workspace->store, commit, &bytes, &size) ||
      !vkr_depot_commit_parse(bytes, size, &parsed) ||
      !vkr_depot_tree_files(&workspace->store, &parsed.tree, &remote)) {
    free(bytes);
    snprintf(error, capacity, "the fetched commit is unreadable");
    goto cleanup;
  }
  const VkrDepotId commit_tree = parsed.tree;
  free(bytes);
  const VkrDepotId zero = {0};
  for (uint32_t i = 0u; i < changes.count; ++i) {
    const char *path = changes.entries[i].path;
    const VkrDepotIndexEntry *base =
        vkr_depot_file_list_find(&workspace->index, path);
    const VkrDepotIndexEntry *mine = vkr_depot_file_list_find(&local, path);
    const VkrDepotIndexEntry *theirs = vkr_depot_file_list_find(&remote, path);
    bool8_t kept = true_v;
    /* A path both sides changed to the same content, as a canceled checkout
       leaves, simply takes the commit's. */
    if (depot_same_content(mine, theirs)) {
      kept = false_v;
    } else if (!depot_same_content(base, theirs)) {
      if (!depot_change_list_add(&conflicts, path, changes.entries[i].change)) {
        snprintf(error, capacity, "out of memory");
        goto cleanup;
      }
      kept = policy == VKR_DEPOT_CONFLICT_KEEP_MINE;
    }
    if (kept && !vkr_depot_file_list_add(&keep, path, 0u, 0, &zero)) {
      snprintf(error, capacity, "out of memory");
      goto cleanup;
    }
  }
  if (conflicts.count > 0u && policy == VKR_DEPOT_CONFLICT_FAIL) {
    snprintf(error, capacity,
             "%u paths changed here and in the commit (first %s); nothing "
             "changed",
             conflicts.count, conflicts.entries[0].path);
    goto cleanup;
  }
  vkr_depot_file_list_sort(&keep);
  if (workspace->sparse) {
    VkrDepotId *blobs = NULL;
    uint32_t blob_count = 0u;
    bool8_t written =
        depot_sparse_writes(&local, &remote, &keep, &blobs, &blob_count);
    if (written && blob_count > 0u && client) {
      written = vkr_depot_client_fetch_files(client, blobs, blob_count, &stats);
      if (!written) {
        depot_step_error(workspace, error, capacity, "fetch failed",
                         vkr_depot_client_error(client));
      }
    } else if (!written) {
      snprintf(error, capacity, "out of memory");
    }
    free(blobs);
    if (!written) {
      goto cleanup;
    }
  }
  if (!vkr_depot_workspace_checkout(workspace, &commit_tree, &keep, NULL)) {
    depot_step_error(workspace, error, capacity, "checkout failed", NULL);
    goto cleanup;
  }
  workspace->base = *commit;
  if (!vkr_depot_workspace_save(workspace)) {
    snprintf(error, capacity, "the working copy state did not save");
    goto cleanup;
  }
  (void)vkr_depot_workspace_clear_fetched(workspace);
  depot_branch_follow(workspace, commit, track_origin);
  *out_kept = keep.count;
  depot_step_done(workspace);
  ok = true_v;
cleanup:
  if (out_conflicts) {
    *out_conflicts = conflicts;
  } else {
    vkr_depot_change_list_free(&conflicts);
  }
  vkr_depot_file_list_free(&local);
  vkr_depot_change_list_free(&changes);
  vkr_depot_file_list_free(&remote);
  vkr_depot_file_list_free(&keep);
  return ok;
}

bool8_t vkr_depot_fetch_checkout(
    VkrDepotWorkspace *workspace, VkrDepotClient *client,
    const VkrDepotId *commit, VkrDepotConflictPolicy policy, uint32_t *out_kept,
    VkrDepotChangeList *out_conflicts, char *error, uint32_t capacity) {
  return depot_checkout_commit(workspace, client, commit, policy,
                               client != NULL, out_kept, out_conflicts, error,
                               capacity);
}

// =============================================================================
// Clone
// =============================================================================

/* Opens the working copy whose root is `directory` itself, not a parent. */
static bool8_t depot_clone_resume(VkrDepotWorkspace *workspace,
                                  const char *directory,
                                  const VkrCodec *codec) {
  char state[VKR_DEPOT_PATH_CAPACITY];
  return vkr_bakery_path_join(state, sizeof(state), directory,
                              VKR_DEPOT_META_DIR "/state") &&
         vkr_bakery_is_file(state) &&
         vkr_depot_workspace_open(workspace, directory, codec);
}

bool8_t vkr_depot_clone(VkrAllocator *allocator,
                        const VkrDepotCloneConfig *config,
                        VkrDepotCloneResult *out_result, char *error,
                        uint32_t capacity) {
  MemZero(out_result, sizeof(*out_result));
  out_result->connect = VKR_DEPOT_CONNECT_INVALID;
  const char *branch = config->branch ? config->branch : "main";
  if (!config->address || !config->server_key || !config->directory ||
      !vkr_depot_branch_valid(branch)) {
    snprintf(error, capacity,
             "clone needs an address, a key, a directory "
             "and a valid branch");
    return false_v;
  }
  VkrDepotWorkspace workspace;
  VkrDepotConnection *connection = NULL;
  bool8_t ok = false_v;
  if (depot_clone_resume(&workspace, config->directory, config->codec)) {
    if (MemCompare(workspace.server_key, config->server_key, 32u) != 0 ||
        (!vkr_depot_id_zero(&workspace.base) &&
         strcmp(workspace.branch, branch) != 0)) {
      snprintf(error, capacity,
               "%s already holds a working copy of another server or branch",
               config->directory);
      vkr_depot_workspace_close(&workspace);
      return false_v;
    }
  } else if (vkr_depot_workspace_create(&workspace, config->directory,
                                        config->store_root, config->codec)) {
    workspace.sparse = config->sparse;
  } else {
    snprintf(error, capacity, "could not create the working copy at %s",
             config->directory);
    return false_v;
  }
  workspace.progress = config->progress;
  /* Saved before connecting: a refused or canceled clone leaves a working
     copy that names its server, which a retry or `pull` completes. */
  snprintf(workspace.remote, sizeof(workspace.remote), "%s", config->address);
  MemCopy(workspace.server_key, config->server_key, 32u);
  snprintf(workspace.branch, sizeof(workspace.branch), "%s", branch);
  if (!vkr_depot_workspace_save(&workspace)) {
    snprintf(error, capacity, "could not save the working copy at %s",
             config->directory);
    goto cleanup;
  }
  /* Saving made the server origin; origin selects the repository too. */
  VkrDepotRemoteEntry origin;
  if (config->repository &&
      vkr_depot_remote_find(workspace.store.root, VKR_DEPOT_ORIGIN, &origin) &&
      MemCompare(origin.repository, config->repository,
                 sizeof(origin.repository)) != 0) {
    MemCopy(origin.repository, config->repository, sizeof(origin.repository));
    if (!vkr_depot_remote_set(workspace.store.root, &origin)) {
      snprintf(error, capacity, "could not save the remote origin at %s",
               config->directory);
      goto cleanup;
    }
  }
  const VkrDepotConnectConfig connect = {.address = config->address,
                                         .server_key = config->server_key,
                                         .store = &workspace.store,
                                         .name = config->name,
                                         .progress = config->progress,
                                         .repository = config->repository};
  connection = vkr_depot_connect(allocator, &connect, &out_result->connect,
                                 error, capacity);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  if (!client) {
    goto cleanup;
  }
  bool8_t exists = false_v;
  if (!vkr_depot_find_ref(client, branch, &out_result->commit, &exists)) {
    snprintf(error, capacity, "listing branches failed: %s",
             vkr_depot_client_error(client));
    goto cleanup;
  }
  if (!exists) {
    out_result->empty = true_v;
    depot_step_done(&workspace);
    ok = true_v;
    goto cleanup;
  }
  uint32_t kept = 0u;
  if (!vkr_depot_fetch_checkout(&workspace, client, &out_result->commit,
                                VKR_DEPOT_CONFLICT_FAIL, &kept, NULL, error,
                                capacity)) {
    goto cleanup;
  }
  out_result->files = workspace.index.count;
  ok = true_v;
cleanup:
  vkr_depot_disconnect(connection);
  vkr_depot_workspace_close(&workspace);
  return ok;
}

// =============================================================================
// History
// =============================================================================

/* Commits found by one walk, at most VKR_DEPOT_HISTORY_MAX. */
typedef struct DepotCommitSet {
  VkrDepotId *ids;
  uint32_t count;
  /* The walk met a commit the store lacks, or the bound. */
  bool8_t incomplete;
} DepotCommitSet;

static bool8_t depot_set_has(const DepotCommitSet *set, const VkrDepotId *id) {
  for (uint32_t i = 0u; i < set->count; ++i) {
    if (vkr_depot_id_equal(&set->ids[i], id)) {
      return true_v;
    }
  }
  return false_v;
}

static void depot_set_free(DepotCommitSet *set) {
  free(set->ids);
  MemZero(set, sizeof(*set));
}

/* A stored commit's parents; false when the store lacks it. */
static bool8_t depot_commit_parents(const VkrDepotStore *store,
                                    const VkrDepotId *id,
                                    VkrDepotId out[VKR_DEPOT_PARENTS_MAX],
                                    uint32_t *out_count) {
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  VkrDepotCommit commit;
  *out_count = 0u;
  if (!vkr_depot_store_has(store, id) ||
      !vkr_depot_store_get(store, id, &bytes, &size) ||
      !vkr_depot_commit_parse(bytes, size, &commit)) {
    free(bytes);
    return false_v;
  }
  for (uint32_t i = 0u; i < commit.parent_count; ++i) {
    out[i] = commit.parents[i];
  }
  *out_count = commit.parent_count;
  free(bytes);
  return true_v;
}

/* Every commit reachable from `start` through parents, `start` included,
   that the store holds, stopping at commits `stop` (may be NULL) holds. With
   a `client`, a commit the store lacks is fetched first with its trees but
   none of its files. */
static bool8_t depot_ancestors(const VkrDepotStore *store,
                               VkrDepotClient *client, const VkrDepotId *start,
                               const DepotCommitSet *stop,
                               DepotCommitSet *out) {
  MemZero(out, sizeof(*out));
  if (vkr_depot_id_zero(start)) {
    return true_v;
  }
  out->ids = malloc(VKR_DEPOT_HISTORY_MAX * sizeof(*out->ids));
  VkrDepotId *pending = malloc(VKR_DEPOT_HISTORY_MAX * sizeof(*pending));
  uint32_t pending_count = 0u;
  bool8_t ok = out->ids && pending;
  if (ok) {
    pending[pending_count++] = *start;
  }
  while (ok && pending_count) {
    const VkrDepotId id = pending[--pending_count];
    if (depot_set_has(out, &id) || (stop && depot_set_has(stop, &id))) {
      continue;
    }
    if (out->count == VKR_DEPOT_HISTORY_MAX) {
      out->incomplete = true_v;
      break;
    }
    if (client && !vkr_depot_store_has(store, &id)) {
      VkrDepotTransferStats stats = {0};
      if (!vkr_depot_client_fetch_trees(client, &id, &stats)) {
        ok = false_v;
        break;
      }
    }
    VkrDepotId parents[VKR_DEPOT_PARENTS_MAX];
    uint32_t parent_count = 0u;
    if (!depot_commit_parents(store, &id, parents, &parent_count)) {
      out->incomplete = true_v;
      continue;
    }
    out->ids[out->count++] = id;
    for (uint32_t i = 0u; i < parent_count; ++i) {
      if (pending_count == VKR_DEPOT_HISTORY_MAX) {
        out->incomplete = true_v;
        break;
      }
      pending[pending_count++] = parents[i];
    }
  }
  free(pending);
  if (!ok) {
    depot_set_free(out);
  }
  return ok;
}

bool8_t vkr_depot_descends(const VkrDepotStore *store, const VkrDepotId *start,
                           const VkrDepotId *ancestor, bool8_t *out_descends) {
  *out_descends = false_v;
  if (vkr_depot_id_zero(ancestor)) {
    *out_descends = true_v;
    return true_v;
  }
  DepotCommitSet history;
  if (!depot_ancestors(store, NULL, start, NULL, &history)) {
    return false_v;
  }
  *out_descends = depot_set_has(&history, ancestor);
  depot_set_free(&history);
  return true_v;
}

bool8_t vkr_depot_ahead_behind(const VkrDepotStore *store,
                               const VkrDepotId *local, const VkrDepotId *other,
                               uint32_t *out_ahead, uint32_t *out_behind) {
  *out_ahead = 0u;
  *out_behind = 0u;
  DepotCommitSet mine;
  DepotCommitSet theirs;
  if (!depot_ancestors(store, NULL, local, NULL, &mine)) {
    return false_v;
  }
  if (!depot_ancestors(store, NULL, other, NULL, &theirs)) {
    depot_set_free(&mine);
    return false_v;
  }
  for (uint32_t i = 0u; i < mine.count; ++i) {
    *out_ahead += !depot_set_has(&theirs, &mine.ids[i]);
  }
  for (uint32_t i = 0u; i < theirs.count; ++i) {
    *out_behind += !depot_set_has(&mine, &theirs.ids[i]);
  }
  depot_set_free(&mine);
  depot_set_free(&theirs);
  return true_v;
}

bool8_t vkr_depot_merge_base(const VkrDepotStore *store, const VkrDepotId *a,
                             const VkrDepotId *b, VkrDepotId *out_base) {
  MemZero(out_base, sizeof(*out_base));
  DepotCommitSet mine;
  if (!depot_ancestors(store, NULL, a, NULL, &mine)) {
    return false_v;
  }
  /* Breadth first from `b`: the first commit `a` also reaches is the
     nearest common ancestor on `b`'s side. */
  VkrDepotId *queue = malloc(VKR_DEPOT_HISTORY_MAX * sizeof(*queue));
  DepotCommitSet seen = {
      .ids = malloc(VKR_DEPOT_HISTORY_MAX * sizeof(VkrDepotId))};
  uint32_t head = 0u;
  uint32_t tail = 0u;
  bool8_t ok = queue && seen.ids;
  if (ok && !vkr_depot_id_zero(b)) {
    queue[tail++] = *b;
  }
  while (ok && head < tail) {
    const VkrDepotId id = queue[head++];
    if (depot_set_has(&mine, &id)) {
      *out_base = id;
      break;
    }
    if (depot_set_has(&seen, &id) || seen.count == VKR_DEPOT_HISTORY_MAX) {
      continue;
    }
    seen.ids[seen.count++] = id;
    VkrDepotId parents[VKR_DEPOT_PARENTS_MAX];
    uint32_t parent_count = 0u;
    (void)depot_commit_parents(store, &id, parents, &parent_count);
    for (uint32_t i = 0u; i < parent_count && tail < VKR_DEPOT_HISTORY_MAX;
         ++i) {
      queue[tail++] = parents[i];
    }
  }
  free(queue);
  depot_set_free(&seen);
  depot_set_free(&mine);
  return ok;
}

// =============================================================================
// Local commit and branches
// =============================================================================

/* The tree of a stored commit. */
static bool8_t depot_commit_tree(const VkrDepotStore *store,
                                 const VkrDepotId *id, VkrDepotId *out_tree) {
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  VkrDepotCommit commit;
  const bool8_t ok = vkr_depot_store_get(store, id, &bytes, &size) &&
                     vkr_depot_commit_parse(bytes, size, &commit);
  if (ok) {
    *out_tree = commit.tree;
  }
  free(bytes);
  return ok;
}

/* Where the working copy's branch is in its store; zero when it does not
   exist yet. */
static VkrDepotId depot_branch_at(const VkrDepotWorkspace *workspace,
                                  const char *branch) {
  VkrDepotId commit = {0};
  if (!vkr_depot_ref_read(workspace->store.root, branch, &commit)) {
    MemZero(&commit, sizeof(commit));
  }
  return commit;
}

bool8_t vkr_depot_commit_local(VkrDepotWorkspace *workspace,
                               const char *message,
                               VkrDepotCommitResult *out_result, char *error,
                               uint32_t capacity) {
  MemZero(out_result, sizeof(*out_result));
  const VkrDepotId branch_at = depot_branch_at(workspace, workspace->branch);
  if (!vkr_depot_id_equal(&branch_at, &workspace->base)) {
    char hex[VKR_DEPOT_ID_HEX];
    vkr_depot_id_hex(&branch_at, hex);
    snprintf(error, capacity,
             "%s is at %.12s in the store, not at this working copy's base; "
             "pull or switch to it first",
             workspace->branch, hex);
    return false_v;
  }

  VkrDepotPendingCommit pending;
  if (!vkr_depot_commit_snapshot(workspace, &workspace->base, message, &pending,
                                 error, capacity)) {
    return false_v;
  }
  VkrDepotId tree = {0};
  VkrDepotId base_tree = {0};
  bool8_t ok = depot_commit_tree(&workspace->store, &pending.commit, &tree);
  if (ok && !vkr_depot_id_zero(&workspace->base) &&
      depot_commit_tree(&workspace->store, &workspace->base, &base_tree) &&
      vkr_depot_id_equal(&tree, &base_tree)) {
    out_result->empty = true_v;
    out_result->commit = workspace->base;
    out_result->files = pending.files.count;
    vkr_depot_pending_commit_free(&pending);
    depot_step_done(workspace);
    return true_v;
  }

  VkrDepotId current;
  const VkrDepotRefSwap swap =
      ok ? vkr_depot_ref_swap(workspace->store.root, workspace->branch,
                              &workspace->base, &pending.commit, &current)
         : VKR_DEPOT_REF_SWAP_FAILED;
  if (swap == VKR_DEPOT_REF_SWAP_MOVED) {
    snprintf(error, capacity,
             "%s moved in the store meanwhile; pull or switch to it first",
             workspace->branch);
    ok = false_v;
  } else if (swap != VKR_DEPOT_REF_SWAP_OK) {
    snprintf(error, capacity, "%s could not move in the store",
             workspace->branch);
    ok = false_v;
  }
  if (ok) {
    out_result->commit = pending.commit;
    out_result->files = pending.files.count;
    vkr_depot_file_list_free(&workspace->index);
    workspace->index = pending.files;
    MemZero(&pending.files, sizeof(pending.files));
    workspace->base = pending.commit;
    if (!vkr_depot_workspace_save(workspace)) {
      snprintf(error, capacity,
               "committed, but the working copy state did not save");
      ok = false_v;
    } else {
      (void)vkr_depot_workspace_clear_fetched(workspace);
      depot_step_done(workspace);
    }
  }
  vkr_depot_pending_commit_free(&pending);
  return ok;
}

bool8_t vkr_depot_branch_create(VkrDepotWorkspace *workspace,
                                const char *branch, char *error,
                                uint32_t capacity) {
  const VkrDepotId zero = {0};
  if (!vkr_depot_branch_valid(branch)) {
    snprintf(error, capacity, "invalid branch name %s", branch);
    return false_v;
  }
  if (vkr_depot_id_zero(&workspace->base)) {
    snprintf(error, capacity, "nothing is committed yet");
    return false_v;
  }
  const VkrDepotRefSwap swap = vkr_depot_ref_swap(
      workspace->store.root, branch, &zero, &workspace->base, NULL);
  if (swap == VKR_DEPOT_REF_SWAP_MOVED) {
    snprintf(error, capacity, "branch %s exists", branch);
  } else if (swap != VKR_DEPOT_REF_SWAP_OK) {
    snprintf(error, capacity, "branch %s could not be written", branch);
  }
  return swap == VKR_DEPOT_REF_SWAP_OK;
}

typedef struct DepotHeldCheck {
  const VkrDepotWorkspace *workspace;
  const char *skip;
  const char *remote;
  const VkrDepotId *target;
  bool8_t held;
} DepotHeldCheck;

/* Whether the branch or tracked branch `name` holds the check's target. */
static bool8_t depot_held_visit(void *context, const char *name) {
  DepotHeldCheck *check = context;
  if (!check->remote && strcmp(name, check->skip) == 0) {
    return true_v;
  }
  VkrDepotId commit;
  const char *store = check->workspace->store.root;
  const bool8_t found =
      check->remote
          ? vkr_depot_tracking_read(store, check->remote, name, &commit)
          : vkr_depot_ref_read(store, name, &commit);
  bool8_t descends = false_v;
  if (found &&
      vkr_depot_descends(&check->workspace->store, &commit, check->target,
                         &descends) &&
      descends) {
    check->held = true_v;
    return false_v;
  }
  return true_v;
}

bool8_t vkr_depot_branch_delete(VkrDepotWorkspace *workspace,
                                const char *branch, bool8_t force, char *error,
                                uint32_t capacity) {
  VkrDepotId commit;
  const char *store = workspace->store.root;
  if (!vkr_depot_ref_read(store, branch, &commit)) {
    snprintf(error, capacity, "no branch %s", branch);
    return false_v;
  }
  if (strcmp(branch, workspace->branch) == 0) {
    snprintf(error, capacity, "%s is this working copy's branch", branch);
    return false_v;
  }
  if (!force) {
    DepotHeldCheck check = {
        .workspace = workspace, .skip = branch, .target = &commit};
    (void)vkr_depot_ref_list(store, depot_held_visit, &check);
    VkrDepotRemoteEntry remotes[VKR_DEPOT_REMOTES_MAX];
    uint32_t count = 0u;
    (void)vkr_depot_remotes_load(store, remotes, ArrayCount(remotes), &count);
    for (uint32_t i = 0u; i < count && !check.held; ++i) {
      check.remote = remotes[i].name;
      (void)vkr_depot_tracking_list(store, remotes[i].name, depot_held_visit,
                                    &check);
    }
    if (!check.held) {
      snprintf(error, capacity,
               "%s holds commits no other branch has; delete with --force",
               branch);
      return false_v;
    }
  }
  const VkrDepotId zero = {0};
  if (vkr_depot_ref_swap(store, branch, &commit, &zero, NULL) !=
      VKR_DEPOT_REF_SWAP_OK) {
    snprintf(error, capacity, "branch %s moved or could not be removed",
             branch);
    return false_v;
  }
  return true_v;
}

bool8_t vkr_depot_switch(VkrDepotWorkspace *workspace, const char *branch,
                         VkrDepotConflictPolicy policy, uint32_t *out_kept,
                         VkrDepotChangeList *out_conflicts, char *error,
                         uint32_t capacity) {
  *out_kept = 0u;
  if (out_conflicts) {
    MemZero(out_conflicts, sizeof(*out_conflicts));
  }
  VkrDepotId commit;
  if (!vkr_depot_ref_read(workspace->store.root, branch, &commit)) {
    snprintf(error, capacity, "no branch %s", branch);
    return false_v;
  }
  char previous[VKR_DEPOT_BRANCH_NAME];
  snprintf(previous, sizeof(previous), "%s", workspace->branch);
  snprintf(workspace->branch, sizeof(workspace->branch), "%s", branch);
  if (!depot_checkout_commit(workspace, NULL, &commit, policy, false_v,
                             out_kept, out_conflicts, error, capacity)) {
    snprintf(workspace->branch, sizeof(workspace->branch), "%s", previous);
    return false_v;
  }
  if (!vkr_depot_workspace_save(workspace)) {
    snprintf(error, capacity, "the working copy state did not save");
    return false_v;
  }
  return true_v;
}

// =============================================================================
// Remotes: fetch, push and pull
// =============================================================================

VkrDepotConnection *vkr_depot_connect_remote(VkrAllocator *allocator,
                                             VkrDepotWorkspace *workspace,
                                             const VkrDepotRemoteEntry *remote,
                                             VkrDepotConnectStatus *out_status,
                                             char *error, uint32_t capacity) {
  VkrDepotHello hello;
  const VkrDepotConnectConfig config = {.address = remote->address,
                                        .server_key = remote->key,
                                        .store = &workspace->store,
                                        .progress = workspace->progress,
                                        .repository = remote->repository,
                                        .out_hello = &hello};
  VkrDepotConnection *connection =
      vkr_depot_connect(allocator, &config, out_status, error, capacity);
  /* A remote added by address alone learns its repository's ID from the
     first reply, so a service that later serves several keeps finding
     it. */
  static const uint8_t none[VKR_DEPOT_REPOSITORY_ID_SIZE] = {0};
  if (connection && hello.repository_size == VKR_DEPOT_REPOSITORY_ID_SIZE &&
      MemCompare(remote->repository, none, sizeof(none)) == 0) {
    VkrDepotRemoteEntry learned = *remote;
    MemCopy(learned.repository, hello.repository, sizeof(learned.repository));
    (void)vkr_depot_remote_set(workspace->store.root, &learned);
  }
  return connection;
}

/* `name` NULL finds origin, else the only remote. */
static bool8_t depot_remote_named(const VkrDepotWorkspace *workspace,
                                  const char *name, VkrDepotRemoteEntry *out,
                                  char *error, uint32_t capacity) {
  if (vkr_depot_remote_find(workspace->store.root, name, out)) {
    return true_v;
  }
  if (name) {
    snprintf(error, capacity, "no remote %s; add it with `remote add`", name);
  } else {
    snprintf(error, capacity,
             "no remote origin; add one with `remote add` or name it");
  }
  return false_v;
}

/* Fetches one remote branch at `commit` and records it. */
static VkrDepotFetchAction depot_fetch_branch(VkrDepotWorkspace *workspace,
                                              VkrDepotClient *client,
                                              const char *remote,
                                              const char *branch,
                                              const VkrDepotId *commit) {
  const char *store = workspace->store.root;
  VkrDepotId tracked = {0};
  const bool8_t known =
      vkr_depot_tracking_read(store, remote, branch, &tracked);
  if (known && vkr_depot_id_equal(&tracked, commit) &&
      vkr_depot_store_has(&workspace->store, commit)) {
    return VKR_DEPOT_FETCH_SAME;
  }
  VkrDepotTransferStats stats = {0};
  const bool8_t fetched =
      workspace->sparse ? vkr_depot_client_fetch_trees(client, commit, &stats)
                        : vkr_depot_client_fetch(client, commit, &stats);
  DepotCommitSet history;
  if (!fetched ||
      !depot_ancestors(&workspace->store, client, commit, NULL, &history)) {
    return VKR_DEPOT_FETCH_FAILED;
  }
  depot_set_free(&history);
  if (!vkr_depot_tracking_set(store, remote, branch, commit)) {
    return VKR_DEPOT_FETCH_FAILED;
  }
  return known ? VKR_DEPOT_FETCH_MOVED : VKR_DEPOT_FETCH_NEW;
}

/* Fetches over an open connection; see vkr_depot_fetch. */
static bool8_t depot_fetch_over(VkrDepotWorkspace *workspace,
                                VkrDepotClient *client, const char *remote,
                                const char *branch, bool8_t cooked,
                                VkrDepotFetchVisit visit, void *context,
                                char *error, uint32_t capacity) {
  VkrDepotRef *refs = malloc(VKR_DEPOT_REFS_MAX * sizeof(*refs));
  uint32_t count = 0u;
  if (!refs ||
      !vkr_depot_client_refs(client, refs, VKR_DEPOT_REFS_MAX, &count)) {
    snprintf(error, capacity, "listing branches failed: %s",
             refs ? vkr_depot_client_error(client) : "out of memory");
    free(refs);
    return false_v;
  }
  bool8_t ok = true_v;
  for (uint32_t i = 0u; i < count; ++i) {
    const char *name = refs[i].name;
    /* A working copy's cooked side branches travel with
       vkr_depot_fetch_cooked; a served store keeps them all. */
    if ((branch && strcmp(name, branch) != 0) ||
        (!branch && !cooked && strstr(name, ".cooked."))) {
      continue;
    }
    const VkrDepotFetchAction action =
        depot_fetch_branch(workspace, client, remote, name, &refs[i].commit);
    if (action == VKR_DEPOT_FETCH_FAILED) {
      depot_step_error(workspace, error, capacity, "fetch failed",
                       vkr_depot_client_error(client));
      ok = false_v;
    }
    if (visit) {
      visit(context, name, action, &refs[i].commit,
            action == VKR_DEPOT_FETCH_FAILED ? error : "");
    }
  }
  free(refs);
  return ok;
}

bool8_t vkr_depot_fetch(VkrAllocator *allocator, VkrDepotWorkspace *workspace,
                        const char *remote, const char *branch,
                        VkrDepotFetchVisit visit, void *context,
                        VkrDepotConnectStatus *out_status, char *error,
                        uint32_t capacity) {
  VkrDepotRemoteEntry entry;
  if (!depot_remote_named(workspace, remote, &entry, error, capacity)) {
    return false_v;
  }
  VkrDepotConnection *connection = vkr_depot_connect_remote(
      allocator, workspace, &entry, out_status, error, capacity);
  if (!connection) {
    return false_v;
  }
  const bool8_t ok = depot_fetch_over(
      workspace, vkr_depot_connection_client(connection), entry.name, branch,
      false_v, visit, context, error, capacity);
  vkr_depot_disconnect(connection);
  if (ok) {
    depot_step_done(workspace);
  }
  return ok;
}

/* The commits reachable from `tip` and from none of `bases`, oldest first:
   each comes after its parents, so the remote can check each one's tree
   against its parent's. */
static bool8_t depot_push_order(const VkrDepotStore *store,
                                const VkrDepotId *tip, const VkrDepotId *bases,
                                uint32_t base_count, DepotCommitSet *out) {
  DepotCommitSet known = {
      .ids = malloc(VKR_DEPOT_HISTORY_MAX * sizeof(VkrDepotId))};
  if (!known.ids) {
    return false_v;
  }
  for (uint32_t b = 0u; b < base_count; ++b) {
    DepotCommitSet reached;
    if (!depot_ancestors(store, NULL, &bases[b], &known, &reached)) {
      depot_set_free(&known);
      return false_v;
    }
    for (uint32_t i = 0u;
         i < reached.count && known.count < VKR_DEPOT_HISTORY_MAX; ++i) {
      known.ids[known.count++] = reached.ids[i];
    }
    depot_set_free(&reached);
  }
  DepotCommitSet pending;
  const bool8_t walked = depot_ancestors(store, NULL, tip, &known, &pending);
  depot_set_free(&known);
  if (!walked) {
    return false_v;
  }
  out->ids = malloc((size_t)Max(pending.count, 1u) * sizeof(*out->ids));
  out->count = 0u;
  out->incomplete = pending.incomplete;
  bool8_t ok = out->ids != NULL;
  while (ok && out->count < pending.count) {
    bool8_t progressed = false_v;
    for (uint32_t i = 0u; i < pending.count; ++i) {
      if (depot_set_has(out, &pending.ids[i])) {
        continue;
      }
      VkrDepotId parents[VKR_DEPOT_PARENTS_MAX];
      uint32_t parent_count = 0u;
      (void)depot_commit_parents(store, &pending.ids[i], parents,
                                 &parent_count);
      bool8_t ready = true_v;
      for (uint32_t k = 0u; k < parent_count && ready; ++k) {
        ready = !depot_set_has(&pending, &parents[k]) ||
                depot_set_has(out, &parents[k]);
      }
      if (ready) {
        out->ids[out->count++] = pending.ids[i];
        progressed = true_v;
      }
    }
    ok = progressed;
  }
  depot_set_free(&pending);
  if (!ok) {
    depot_set_free(out);
  }
  return ok;
}

static VkrDepotPushStatus depot_push_over(VkrDepotWorkspace *workspace,
                                          VkrDepotClient *client,
                                          const char *remote,
                                          const char *branch,
                                          VkrDepotPushResult *out_result,
                                          char *error, uint32_t capacity) {
  const VkrDepotId local = depot_branch_at(workspace, branch);
  out_result->commit = local;
  if (vkr_depot_id_zero(&local)) {
    snprintf(error, capacity, "no branch %s in this repository", branch);
    return VKR_DEPOT_PUSH_FAILED;
  }
  /* The remote's branch, and every commit its branches reach: those are on
     the remote, so they are neither uploaded nor needed here with their
     files (fetched history holds none). */
  VkrDepotId theirs = {0};
  bool8_t exists = false_v;
  VkrDepotRef *refs = malloc(VKR_DEPOT_REFS_MAX * sizeof(*refs));
  VkrDepotId *bases = malloc(VKR_DEPOT_REFS_MAX * sizeof(*bases));
  uint32_t ref_count = 0u;
  uint32_t base_count = 0u;
  const bool8_t listed =
      refs && bases &&
      vkr_depot_client_refs(client, refs, VKR_DEPOT_REFS_MAX, &ref_count);
  for (uint32_t i = 0u; listed && i < ref_count; ++i) {
    if (strcmp(refs[i].name, branch) == 0) {
      theirs = refs[i].commit;
      exists = true_v;
    }
    if (vkr_depot_store_has(&workspace->store, &refs[i].commit)) {
      bases[base_count++] = refs[i].commit;
    }
  }
  free(refs);
  if (!listed) {
    snprintf(error, capacity, "listing branches failed: %s",
             bases ? vkr_depot_client_error(client) : "out of memory");
    free(bases);
    return VKR_DEPOT_PUSH_FAILED;
  }
  const char *store = workspace->store.root;
  if (exists && vkr_depot_id_equal(&theirs, &local)) {
    free(bases);
    (void)vkr_depot_tracking_set(store, remote, branch, &local);
    return VKR_DEPOT_PUSH_UP_TO_DATE;
  }
  bool8_t descends = false_v;
  if (exists &&
      (!vkr_depot_store_has(&workspace->store, &theirs) ||
       !vkr_depot_descends(&workspace->store, &local, &theirs, &descends) ||
       !descends)) {
    char hex[VKR_DEPOT_ID_HEX];
    vkr_depot_id_hex(&theirs, hex);
    snprintf(error, capacity,
             "%s on %s is at %.12s, which %s does not build on; pull, then "
             "push again",
             branch, remote, hex, branch);
    free(bases);
    return VKR_DEPOT_PUSH_NOT_FAST_FORWARD;
  }

  const VkrDepotId zero = {0};
  const VkrDepotId *base = exists ? &theirs : &zero;
  DepotCommitSet order;
  const bool8_t ordered =
      depot_push_order(&workspace->store, &local, bases, base_count, &order);
  free(bases);
  if (!ordered) {
    snprintf(error, capacity, "the history of %s is unreadable", branch);
    return VKR_DEPOT_PUSH_FAILED;
  }
  vkr_depot_client_set_progress(client, workspace->progress);
  bool8_t uploaded = true_v;
  for (uint32_t i = 0u; i < order.count && uploaded; ++i) {
    VkrDepotId parents[VKR_DEPOT_PARENTS_MAX];
    uint32_t parent_count = 0u;
    (void)depot_commit_parents(&workspace->store, &order.ids[i], parents,
                               &parent_count);
    uploaded = vkr_depot_client_upload(client, &order.ids[i],
                                       parent_count ? &parents[0] : &zero,
                                       &out_result->uploaded);
    out_result->commits += uploaded;
  }
  depot_set_free(&order);
  if (!uploaded) {
    depot_step_error(workspace, error, capacity, "upload failed",
                     vkr_depot_client_error(client));
    return VKR_DEPOT_PUSH_FAILED;
  }

  VkrDepotId current;
  const VkrDepotRefStatus status =
      vkr_depot_client_update_ref(client, branch, base, &local, &current);
  if (status == VKR_DEPOT_REF_OK) {
    (void)vkr_depot_tracking_set(store, remote, branch, &local);
    return VKR_DEPOT_PUSH_OK;
  }
  if (status == VKR_DEPOT_REF_CONFLICT) {
    char hex[VKR_DEPOT_ID_HEX];
    vkr_depot_id_hex(&current, hex);
    snprintf(error, capacity, "%s on %s moved to %.12s meanwhile; pull first",
             branch, remote, hex);
    return VKR_DEPOT_PUSH_REMOTE_MOVED;
  }
  if (status == VKR_DEPOT_REF_LOCKED) {
    snprintf(error, capacity,
             "a pushed commit changes a file another user "
             "locked");
    return VKR_DEPOT_PUSH_LOCKED;
  }
  if (status == VKR_DEPOT_REF_INVALID) {
    snprintf(error, capacity,
             "%s refused the commit: a name no tree may hold, or a tree past "
             "the depot's limits",
             remote);
  } else {
    snprintf(error, capacity, "%s refused the push (status %u)", remote,
             status);
  }
  return VKR_DEPOT_PUSH_FAILED;
}

bool8_t vkr_depot_push(VkrAllocator *allocator, VkrDepotWorkspace *workspace,
                       const char *remote, const char *branch,
                       VkrDepotPushResult *out_result,
                       VkrDepotConnectStatus *out_status, char *error,
                       uint32_t capacity) {
  MemZero(out_result, sizeof(*out_result));
  out_result->status = VKR_DEPOT_PUSH_FAILED;
  const char *name = branch ? branch : workspace->branch;
  VkrDepotRemoteEntry entry;
  if (!vkr_depot_branch_valid(name)) {
    snprintf(error, capacity, "invalid branch name %s", name);
    return false_v;
  }
  if (!depot_remote_named(workspace, remote, &entry, error, capacity)) {
    return false_v;
  }
  VkrDepotConnection *connection = vkr_depot_connect_remote(
      allocator, workspace, &entry, out_status, error, capacity);
  if (!connection) {
    return false_v;
  }
  out_result->status =
      depot_push_over(workspace, vkr_depot_connection_client(connection),
                      entry.name, name, out_result, error, capacity);
  vkr_depot_disconnect(connection);
  const bool8_t ok = out_result->status == VKR_DEPOT_PUSH_OK ||
                     out_result->status == VKR_DEPOT_PUSH_UP_TO_DATE;
  if (ok) {
    depot_step_done(workspace);
  }
  return ok;
}

bool8_t vkr_depot_pull(VkrAllocator *allocator, VkrDepotWorkspace *workspace,
                       const char *remote, VkrDepotConflictPolicy policy,
                       VkrDepotPullResult *out_result,
                       VkrDepotConnectStatus *out_status, char *error,
                       uint32_t capacity) {
  MemZero(out_result, sizeof(*out_result));
  out_result->status = VKR_DEPOT_PULL_FAILED;
  VkrDepotRemoteEntry entry;
  if (!depot_remote_named(workspace, remote, &entry, error, capacity)) {
    return false_v;
  }
  VkrDepotConnection *connection = vkr_depot_connect_remote(
      allocator, workspace, &entry, out_status, error, capacity);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  if (!client) {
    return false_v;
  }
  const char *store = workspace->store.root;
  bool8_t ok =
      depot_fetch_over(workspace, client, entry.name, workspace->branch,
                       false_v, NULL, NULL, error, capacity);
  VkrDepotId theirs = {0};
  if (ok &&
      !vkr_depot_tracking_read(store, entry.name, workspace->branch, &theirs)) {
    snprintf(error, capacity, "%s has no branch %s", entry.name,
             workspace->branch);
    ok = false_v;
  }
  const VkrDepotId local = depot_branch_at(workspace, workspace->branch);
  bool8_t forward = false_v;
  bool8_t ahead = false_v;
  if (ok && vkr_depot_id_equal(&theirs, &local)) {
    out_result->status = VKR_DEPOT_PULL_UP_TO_DATE;
  } else if (ok && (!vkr_depot_descends(&workspace->store, &theirs, &local,
                                        &forward) ||
                    !vkr_depot_descends(&workspace->store, &local, &theirs,
                                        &ahead))) {
    snprintf(error, capacity, "the history of %s is unreadable",
             workspace->branch);
    ok = false_v;
  } else if (ok && forward) {
    ok = depot_checkout_commit(workspace, client, &theirs, policy, false_v,
                               &out_result->kept, &out_result->conflicts, error,
                               capacity);
    out_result->status =
        ok ? VKR_DEPOT_PULL_FAST_FORWARD : VKR_DEPOT_PULL_FAILED;
  } else if (ok && ahead) {
    out_result->status = VKR_DEPOT_PULL_AHEAD;
  } else if (ok) {
    out_result->status = VKR_DEPOT_PULL_DIVERGED;
  }
  out_result->commit = ok ? theirs : local;
  vkr_depot_disconnect(connection);
  if (ok) {
    depot_step_done(workspace);
  }
  return ok;
}

// =============================================================================
// Served stores
// =============================================================================

typedef struct DepotSyncNames {
  char names[VKR_DEPOT_REPOSITORY_BRANCHES_MAX * 2u][VKR_DEPOT_BRANCH_NAME];
  uint32_t count;
} DepotSyncNames;

static bool8_t depot_sync_name_visit(void *context, const char *branch) {
  DepotSyncNames *names = context;
  for (uint32_t i = 0u; i < names->count; ++i) {
    if (strcmp(names->names[i], branch) == 0) {
      return true_v;
    }
  }
  if (names->count < ArrayCount(names->names)) {
    snprintf(names->names[names->count++], VKR_DEPOT_BRANCH_NAME, "%s", branch);
  }
  return true_v;
}

/* One branch of a served store against its remote's, both fetched. */
static VkrDepotSyncAction
depot_sync_branch(VkrDepotWorkspace *store, VkrDepotClient *client,
                  const char *remote, const char *branch,
                  VkrDepotId *out_commit, char *detail, uint32_t capacity) {
  const char *root = store->store.root;
  VkrDepotId local = {0};
  VkrDepotId theirs = {0};
  const bool8_t has_local = vkr_depot_ref_read(root, branch, &local);
  const bool8_t has_theirs =
      vkr_depot_tracking_read(root, remote, branch, &theirs);
  *out_commit = has_local ? local : theirs;
  if (has_local && has_theirs && vkr_depot_id_equal(&local, &theirs)) {
    return VKR_DEPOT_SYNC_SAME;
  }
  bool8_t behind = !has_local;
  bool8_t ahead = !has_theirs;
  if (has_local && has_theirs &&
      (!vkr_depot_descends(&store->store, &theirs, &local, &behind) ||
       !vkr_depot_descends(&store->store, &local, &theirs, &ahead))) {
    snprintf(detail, capacity, "its history is unreadable");
    return VKR_DEPOT_SYNC_FAILED;
  }
  if (behind) {
    const VkrDepotId zero = {0};
    *out_commit = theirs;
    const VkrDepotRefSwap swap = vkr_depot_ref_swap(
        root, branch, has_local ? &local : &zero, &theirs, NULL);
    if (swap != VKR_DEPOT_REF_SWAP_OK) {
      snprintf(detail, capacity, "it moved here meanwhile");
      return VKR_DEPOT_SYNC_FAILED;
    }
    return VKR_DEPOT_SYNC_PULLED;
  }
  if (!ahead) {
    snprintf(detail, capacity,
             "both sides have new commits; a working copy merges them");
    return VKR_DEPOT_SYNC_DIVERGED;
  }
  VkrDepotPushResult pushed = {0};
  const VkrDepotPushStatus status =
      depot_push_over(store, client, remote, branch, &pushed, detail, capacity);
  if (status == VKR_DEPOT_PUSH_OK || status == VKR_DEPOT_PUSH_UP_TO_DATE) {
    return VKR_DEPOT_SYNC_PUSHED;
  }
  return status == VKR_DEPOT_PUSH_NOT_FAST_FORWARD ||
                 status == VKR_DEPOT_PUSH_REMOTE_MOVED
             ? VKR_DEPOT_SYNC_DIVERGED
             : VKR_DEPOT_SYNC_FAILED;
}

bool8_t vkr_depot_repository_sync(VkrAllocator *allocator,
                                  const char *store_root, const VkrCodec *codec,
                                  const char *remote, VkrDepotSyncVisit visit,
                                  void *context,
                                  VkrDepotConnectStatus *out_status,
                                  char *error, uint32_t capacity) {
  /* The steps work on a working copy's store; a served store is one with
     no working directory. */
  VkrDepotWorkspace *store = calloc(1u, sizeof(*store));
  DepotSyncNames *names = calloc(1u, sizeof(*names));
  VkrDepotConnection *connection = NULL;
  VkrDepotRemoteEntry entry;
  bool8_t ok = false_v;
  if (!store || !names) {
    snprintf(error, capacity, "out of memory");
    goto cleanup;
  }
  if (!vkr_depot_store_open(&store->store, store_root, codec)) {
    snprintf(error, capacity, "no store at %s", store_root);
    goto cleanup;
  }
  if (!depot_remote_named(store, remote, &entry, error, capacity)) {
    goto cleanup;
  }
  connection = vkr_depot_connect_remote(allocator, store, &entry, out_status,
                                        error, capacity);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  if (!client || !depot_fetch_over(store, client, entry.name, NULL, true_v,
                                   NULL, NULL, error, capacity)) {
    goto cleanup;
  }
  (void)vkr_depot_ref_list(store_root, depot_sync_name_visit, names);
  (void)vkr_depot_tracking_list(store_root, entry.name, depot_sync_name_visit,
                                names);
  ok = true_v;
  for (uint32_t i = 0u; i < names->count; ++i) {
    char detail[160] = {0};
    VkrDepotId commit = {0};
    const VkrDepotSyncAction action =
        depot_sync_branch(store, client, entry.name, names->names[i], &commit,
                          detail, sizeof(detail));
    ok = ok && action != VKR_DEPOT_SYNC_FAILED;
    if (visit) {
      visit(context, names->names[i], action, &commit, detail);
    }
  }
cleanup:
  vkr_depot_disconnect(connection);
  free(names);
  free(store);
  return ok;
}
