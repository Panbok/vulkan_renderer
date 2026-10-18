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
  if (!vkr_net_host_pump(link->host, timeout_ms)) {
    return false_v;
  }
  VkrNetCore *core = vkr_net_host_core(link->host);
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
    for (uint32_t i = 0u; i < 20u; ++i) {
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
                                         VKR_DEPOT_TIMEOUT_MS);
  if (!link->client) {
    const bool8_t denied = link->close_code == VKR_NET_CLOSE_ACCESS_DENIED;
    depot_connect_status(out_status, denied ? VKR_DEPOT_CONNECT_DENIED
                                            : VKR_DEPOT_CONNECT_REFUSED);
    snprintf(error, capacity, "the server refused the depot service");
    vkr_depot_disconnect(link);
    return NULL;
  }
  vkr_depot_client_set_progress(link->client, config->progress);
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

/* Stores a commit of `tree` with `parent` (none when zero) by this user's
   identity. */
static bool8_t depot_make_commit(const VkrDepotStore *store,
                                 const VkrDepotId *tree,
                                 const VkrDepotId *parent, const char *message,
                                 VkrDepotId *out_commit, char *error,
                                 uint32_t capacity) {
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
  if (parent && !vkr_depot_id_zero(parent)) {
    commit.parents[0] = *parent;
    commit.parent_count = 1u;
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
  depot_step_done(workspace);
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

bool8_t vkr_depot_fetch_checkout(
    VkrDepotWorkspace *workspace, VkrDepotClient *client,
    const VkrDepotId *commit, VkrDepotConflictPolicy policy, uint32_t *out_kept,
    VkrDepotChangeList *out_conflicts, char *error, uint32_t capacity) {
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
    depot_step_done(workspace);
    return true_v;
  }
  vkr_depot_client_set_progress(client, workspace->progress);
  VkrDepotTransferStats stats = {0};
  if (!vkr_depot_client_fetch(client, commit, &stats)) {
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
  if (!vkr_depot_workspace_checkout(workspace, &commit_tree, &keep, NULL)) {
    depot_step_error(workspace, error, capacity, "checkout failed", NULL);
    goto cleanup;
  }
  workspace->base = *commit;
  if (!vkr_depot_workspace_save(workspace)) {
    snprintf(error, capacity, "the working copy state did not save");
    goto cleanup;
  }
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
  } else if (!vkr_depot_workspace_create(&workspace, config->directory,
                                         config->store_root, config->codec)) {
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
  const VkrDepotConnectConfig connect = {.address = config->address,
                                         .server_key = config->server_key,
                                         .store = &workspace.store,
                                         .name = config->name,
                                         .progress = config->progress};
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
