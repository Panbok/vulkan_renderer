#include "vkr_depot_sync.h"

#include "vkr_bakery_os.h"
#include "vkr_net_crypto.h"
#include "vkr_net_host.h"
#include "vkr_net_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Identity and addresses
// =============================================================================

static bool8_t depot_identity_path(char *out, uint32_t capacity) {
  const char *override = getenv("VKR_DEPOT_IDENTITY");
  if (override && override[0]) {
    return snprintf(out, capacity, "%s", override) < (int)capacity;
  }
  char cache[VKR_DEPOT_PATH_CAPACITY];
  return vkr_bakery_user_cache_directory(cache, sizeof(cache)) &&
         snprintf(out, capacity, "%s/vkr/depot/identity.key", cache) <
             (int)capacity;
}

bool8_t vkr_depot_identity(VkrNetKeyPair *out, bool8_t create) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!depot_identity_path(path, sizeof(path))) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (vkr_bakery_is_file(path) &&
      vkr_bakery_read_file(path, 64u, &bytes, &size) &&
      size == VKR_NET_KEY_SIZE) {
    vkr_net_keypair_from_secret(bytes, out);
    vkr_net_random(bytes, size);
    free(bytes);
    return true_v;
  }
  free(bytes);
  if (!create) {
    return false_v;
  }
  char directory[VKR_DEPOT_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  vkr_net_keypair_generate(out);
  return vkr_bakery_make_directories(directory) &&
         vkr_bakery_write_file_atomic(path, out->secret_key, VKR_NET_KEY_SIZE);
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
  vkr_depot_client_destroy(link->client);
  if (link->host && link->connected && !link->closed) {
    vkr_net_core_close(vkr_net_host_core(link->host), link->connection,
                       VKR_NET_CLOSE_APPLICATION, depot_connection_now(link));
    for (uint32_t i = 0u; i < 20u; ++i) {
      (void)depot_connection_pump(link, 10);
    }
  }
  vkr_net_session_destroy(link->session);
  vkr_net_host_destroy(link->host);
  free(link);
}

VkrDepotConnection *vkr_depot_connect(VkrAllocator *allocator,
                                      const char *address,
                                      const uint8_t server_key[32],
                                      const VkrDepotStore *store, char *error,
                                      uint32_t capacity) {
  VkrNetAddress server;
  VkrNetHostConfig config = {
      .socket = {.receive_buffer = 8u << 20, .send_buffer = 8u << 20}};
  if (!vkr_depot_address(address, &server)) {
    snprintf(error, capacity, "invalid depot address %s", address);
    return NULL;
  }
  if (!vkr_depot_identity(&config.core.static_keys, true_v)) {
    snprintf(error, capacity, "no depot identity key");
    return NULL;
  }
  VkrDepotConnection *link = calloc(1u, sizeof(*link));
  if (!link) {
    snprintf(error, capacity, "out of memory");
    return NULL;
  }
  config.bind = vkr_net_address_any((VkrNetAddressFamily)server.family, 0u);
  config.core.sent_packet_capacity = 16384u;
  config.core.background = true_v;
  link->host = vkr_net_host_create(allocator, &config);
  link->session =
      link->host
          ? vkr_net_session_create(allocator, vkr_net_host_core(link->host))
          : NULL;
  if (!link->session ||
      !vkr_net_core_connect(vkr_net_host_core(link->host), &server, server_key,
                            NULL, 0u, depot_connection_now(link),
                            &link->connection)) {
    snprintf(error, capacity, "could not start the connection");
    vkr_depot_disconnect(link);
    return NULL;
  }
  const uint64_t limit =
      depot_connection_now(link) + VKR_DEPOT_TIMEOUT_MS * 1000ull;
  while (!link->connected && !link->closed &&
         depot_connection_now(link) < limit) {
    if (!depot_connection_pump(link, 20)) {
      break;
    }
  }
  if (!link->connected) {
    snprintf(error, capacity, "could not connect to %s (code %u)", address,
             link->close_code);
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
  link->client =
      vkr_depot_client_create(&client_link, store, VKR_DEPOT_TIMEOUT_MS);
  if (!link->client) {
    snprintf(error, capacity, "the server refused the depot service");
    vkr_depot_disconnect(link);
    return NULL;
  }
  return link;
}

VkrDepotClient *vkr_depot_connection_client(VkrDepotConnection *connection) {
  return connection ? connection->client : NULL;
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
  local = vkr_depot_connect(allocator, local_address, local_key, store, error,
                            capacity);
  peer = local ? vkr_depot_connect(allocator, peer_address, peer_key, store,
                                   error, capacity)
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

void vkr_depot_pending_commit_free(VkrDepotPendingCommit *pending) {
  vkr_depot_file_list_free(&pending->files);
}

bool8_t vkr_depot_commit_snapshot(VkrDepotWorkspace *workspace,
                                  const VkrDepotId *parent, const char *message,
                                  VkrDepotPendingCommit *out_pending,
                                  char *error, uint32_t capacity) {
  VkrNetKeyPair identity;
  VkrDepotId tree;
  VkrDepotFileList files = {0};
  uint8_t *commit_bytes = NULL;
  uint64_t commit_size = 0u;
  bool8_t ok = false_v;
  MemZero(out_pending, sizeof(*out_pending));
  if (!vkr_depot_identity(&identity, true_v)) {
    snprintf(error, capacity, "no depot identity key");
    return false_v;
  }
  if (!vkr_depot_workspace_snapshot(workspace, &tree, &files)) {
    snprintf(error, capacity, "the working directory snapshot failed");
    goto cleanup;
  }
  VkrDepotCommit commit = {.tree = tree,
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
  if (!vkr_depot_commit_encode(&commit, &commit_bytes, &commit_size)) {
    snprintf(error, capacity, "the commit message is too long");
    goto cleanup;
  }
  vkr_depot_id_of(commit_bytes, commit_size, &out_pending->commit);
  if (!vkr_depot_store_put(&workspace->store, &out_pending->commit,
                           commit_bytes, commit_size)) {
    snprintf(error, capacity, "the commit could not be stored");
    goto cleanup;
  }
  out_pending->files = files;
  MemZero(&files, sizeof(files));
  ok = true_v;
cleanup:
  free(commit_bytes);
  vkr_depot_file_list_free(&files);
  return ok;
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
  VkrDepotTransferStats stats = {0};
  if (!vkr_depot_client_upload(client, &pending->commit, parent, &stats)) {
    snprintf(error, capacity, "upload failed: %s",
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
  return true_v;
}

typedef struct DepotChangeCollector {
  VkrDepotFileList *list;
} DepotChangeCollector;

static bool8_t depot_collect_change(void *context, const char *path,
                                    VkrDepotChange change) {
  (void)change;
  DepotChangeCollector *collector = context;
  const VkrDepotId zero = {0};
  return vkr_depot_file_list_add(collector->list, path, 0u, 0, &zero);
}

bool8_t vkr_depot_fetch_checkout(VkrDepotWorkspace *workspace,
                                 VkrDepotClient *client,
                                 const VkrDepotId *commit, uint32_t *out_kept,
                                 char *error, uint32_t capacity) {
  VkrDepotFileList local = {0};
  VkrDepotFileList changes = {0};
  VkrDepotFileList remote = {0};
  bool8_t ok = false_v;
  *out_kept = 0u;
  if (vkr_depot_id_equal(commit, &workspace->base)) {
    return true_v;
  }
  VkrDepotTransferStats stats = {0};
  if (!vkr_depot_client_fetch(client, commit, &stats)) {
    snprintf(error, capacity, "fetch failed: %s",
             vkr_depot_client_error(client));
    goto cleanup;
  }
  /* Local changes stay, unless the commit changed the same paths. */
  VkrDepotId tree;
  if (!vkr_depot_workspace_snapshot(workspace, &tree, &local)) {
    snprintf(error, capacity, "the working directory snapshot failed");
    goto cleanup;
  }
  DepotChangeCollector collector = {.list = &changes};
  (void)vkr_depot_file_list_diff(&workspace->index, &local,
                                 depot_collect_change, &collector);
  vkr_depot_file_list_sort(&changes);
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
  for (uint32_t i = 0u; i < changes.count; ++i) {
    const char *path = changes.entries[i].path;
    const VkrDepotIndexEntry *base =
        vkr_depot_file_list_find(&workspace->index, path);
    const VkrDepotIndexEntry *theirs = vkr_depot_file_list_find(&remote, path);
    if ((base == NULL) != (theirs == NULL) ||
        (base && theirs && !vkr_depot_id_equal(&base->blob, &theirs->blob))) {
      snprintf(error, capacity,
               "%s changed here and in the commit; nothing changed", path);
      goto cleanup;
    }
  }
  if (!vkr_depot_workspace_checkout(workspace, &commit_tree, &changes)) {
    snprintf(error, capacity, "checkout failed");
    goto cleanup;
  }
  workspace->base = *commit;
  if (!vkr_depot_workspace_save(workspace)) {
    snprintf(error, capacity, "the working copy state did not save");
    goto cleanup;
  }
  *out_kept = changes.count;
  ok = true_v;
cleanup:
  vkr_depot_file_list_free(&local);
  vkr_depot_file_list_free(&changes);
  vkr_depot_file_list_free(&remote);
  return ok;
}
