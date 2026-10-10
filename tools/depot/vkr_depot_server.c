#include "vkr_depot_server.h"

#include "core/vkr_byte_io.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVER_PEER_MAX 256u
#define SERVER_PUT_SLOTS 16u
/* Object bytes that arrived before their PUT. */
#define SERVER_ORPHAN_MAX 1024u
#define SERVER_ACCESS_MAX 4096u

typedef struct ServerSendItem {
  uint64_t request;
  uint32_t index;
  bool8_t zstd;
  VkrDepotId id;
} ServerSendItem;

typedef struct ServerPut {
  bool8_t live;
  uint64_t request;
  uint32_t count;
  uint32_t remaining;
  VkrDepotId ids[VKR_DEPOT_BATCH];
  uint8_t received[VKR_DEPOT_BATCH / 8u];
  uint8_t bad[VKR_DEPOT_BATCH / 8u];
} ServerPut;

typedef struct ServerOrphan {
  uint64_t tag;
  uint8_t *bytes;
  uint32_t size;
} ServerOrphan;

typedef struct ServerPeer {
  bool8_t live;
  VkrNetConnectionId connection;
  uint8_t key[32];
  uint32_t role;
  ServerSendItem *queue;
  uint32_t queue_head;
  uint32_t queue_count;
  uint32_t queue_capacity;
  ServerPut puts[SERVER_PUT_SLOTS];
  ServerOrphan *orphans;
  uint32_t orphan_count;
} ServerPeer;

typedef struct ServerLock {
  char *path;
  uint8_t owner[32];
  uint64_t time_us;
} ServerLock;

typedef struct ServerAccess {
  uint8_t key[32];
  uint8_t role;
} ServerAccess;

struct VkrDepotServer {
  VkrAllocator *allocator;
  VkrNetCore *core;
  VkrNetSession *session;
  VkrDepotStore store;
  bool8_t open_access;
  ServerPeer peers[SERVER_PEER_MAX];
  ServerLock *locks;
  uint32_t lock_count;
  uint32_t lock_capacity;
  ServerAccess access[SERVER_ACCESS_MAX];
  uint32_t access_count;
  VkrDepotMessage message;
  VkrDepotMessage reply;
};

static void bits_set(uint8_t *bits, uint32_t index) {
  bits[index / 8u] |= (uint8_t)(1u << (index % 8u));
}

static bool8_t bits_get(const uint8_t *bits, uint32_t index) {
  return (bits[index / 8u] >> (index % 8u)) & 1u ? true_v : false_v;
}

static uint64_t server_unix_us(void) {
  return (uint64_t)vkr_bakery_unix_seconds() * 1000000u;
}

// =============================================================================
// Store files: refs, locks, access, key
// =============================================================================

static bool8_t server_file(const char *root, const char *name, char *out,
                           uint32_t capacity) {
  return vkr_bakery_path_join(out, capacity, root, name);
}

bool8_t vkr_depot_ref_read(const char *store_root, const char *branch,
                           VkrDepotId *out_commit) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  char relative[VKR_DEPOT_BRANCH_NAME + 8u];
  if (!vkr_depot_branch_valid(branch)) {
    return false_v;
  }
  snprintf(relative, sizeof(relative), "refs/%s", branch);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!server_file(store_root, relative, path, sizeof(path)) ||
      !vkr_bakery_is_file(path) ||
      !vkr_bakery_read_file(path, 64u, &bytes, &size)) {
    return false_v;
  }
  const bool8_t ok = size == VKR_DEPOT_ID_SIZE;
  if (ok) {
    memcpy(out_commit->bytes, bytes, VKR_DEPOT_ID_SIZE);
  }
  free(bytes);
  return ok;
}

static bool8_t server_ref_write(const char *store_root, const char *branch,
                                const VkrDepotId *commit) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  char directory[VKR_DEPOT_PATH_CAPACITY];
  char relative[VKR_DEPOT_BRANCH_NAME + 8u];
  snprintf(relative, sizeof(relative), "refs/%s", branch);
  return server_file(store_root, "refs", directory, sizeof(directory)) &&
         vkr_bakery_make_directories(directory) &&
         server_file(store_root, relative, path, sizeof(path)) &&
         vkr_bakery_write_file_atomic(path, commit->bytes, VKR_DEPOT_ID_SIZE);
}

bool8_t vkr_depot_server_key(const char *store_root, VkrNetKeyPair *out_keys) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_make_directories(store_root) ||
      !server_file(store_root, "server.key", path, sizeof(path))) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (vkr_bakery_is_file(path) &&
      vkr_bakery_read_file(path, 64u, &bytes, &size)) {
    const bool8_t ok = size == VKR_NET_KEY_SIZE;
    if (ok) {
      vkr_net_keypair_from_secret(bytes, out_keys);
    }
    vkr_net_random(bytes, size);
    free(bytes);
    return ok;
  }
  vkr_net_keypair_generate(out_keys);
  return vkr_bakery_write_file_atomic(path, out_keys->secret_key,
                                      VKR_NET_KEY_SIZE);
}

static bool8_t server_access_load(VkrDepotServer *server) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  server->access_count = 0u;
  if (!server_file(server->store.root, "access", path, sizeof(path))) {
    return false_v;
  }
  if (!vkr_bakery_is_file(path)) {
    return true_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_read_file(path, SERVER_ACCESS_MAX * 33u, &bytes, &size) ||
      size % 33u != 0u) {
    free(bytes);
    return false_v;
  }
  for (uint64_t offset = 0u; offset < size; offset += 33u) {
    ServerAccess *entry = &server->access[server->access_count++];
    memcpy(entry->key, bytes + offset, 32u);
    entry->role = bytes[offset + 32u];
  }
  free(bytes);
  return true_v;
}

bool8_t vkr_depot_access_set(const char *store_root, const uint8_t key[32],
                             VkrDepotRole role) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_make_directories(store_root) ||
      !server_file(store_root, "access", path, sizeof(path))) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (vkr_bakery_is_file(path) &&
      !vkr_bakery_read_file(path, SERVER_ACCESS_MAX * 33u, &bytes, &size)) {
    return false_v;
  }
  uint8_t *out = malloc((size_t)size + 33u);
  if (!out) {
    free(bytes);
    return false_v;
  }
  uint64_t written = 0u;
  for (uint64_t offset = 0u; offset + 33u <= size; offset += 33u) {
    if (memcmp(bytes + offset, key, 32u) != 0) {
      memcpy(out + written, bytes + offset, 33u);
      written += 33u;
    }
  }
  if (role != VKR_DEPOT_ROLE_NONE) {
    memcpy(out + written, key, 32u);
    out[written + 32u] = (uint8_t)role;
    written += 33u;
  }
  const bool8_t ok = vkr_bakery_write_file_atomic(path, out, written);
  free(out);
  free(bytes);
  return ok;
}

static uint32_t server_role(const VkrDepotServer *server, const uint8_t *key) {
  for (uint32_t i = 0u; i < server->access_count; ++i) {
    if (memcmp(server->access[i].key, key, 32u) == 0) {
      return server->access[i].role;
    }
  }
  return server->open_access ? VKR_DEPOT_ROLE_WRITE : VKR_DEPOT_ROLE_NONE;
}

static int server_lock_compare(const void *lhs, const void *rhs) {
  return strcmp(((const ServerLock *)lhs)->path,
                ((const ServerLock *)rhs)->path);
}

static bool8_t server_locks_save(const VkrDepotServer *server) {
  uint64_t size = 4u;
  for (uint32_t i = 0u; i < server->lock_count; ++i) {
    size += 2u + strlen(server->locks[i].path) + 32u + 8u;
  }
  uint8_t *bytes = malloc((size_t)size);
  if (!bytes) {
    return false_v;
  }
  uint8_t *cursor = bytes;
  vkr_store_le_u32(cursor, server->lock_count);
  cursor += 4u;
  for (uint32_t i = 0u; i < server->lock_count; ++i) {
    const ServerLock *lock = &server->locks[i];
    const uint16_t length = (uint16_t)strlen(lock->path);
    cursor[0] = (uint8_t)length;
    cursor[1] = (uint8_t)(length >> 8);
    memcpy(cursor + 2u, lock->path, length);
    cursor += 2u + length;
    memcpy(cursor, lock->owner, 32u);
    vkr_store_le_u64(cursor + 32, lock->time_us);
    cursor += 40u;
  }
  char path[VKR_DEPOT_PATH_CAPACITY];
  const bool8_t ok =
      server_file(server->store.root, "locks", path, sizeof(path)) &&
      vkr_bakery_write_file_atomic(path, bytes, size);
  free(bytes);
  return ok;
}

static bool8_t server_lock_add(VkrDepotServer *server, const char *path,
                               const uint8_t owner[32], uint64_t time_us) {
  if (server->lock_count == server->lock_capacity) {
    const uint32_t capacity =
        server->lock_capacity ? server->lock_capacity * 2u : 64u;
    ServerLock *locks =
        realloc(server->locks, (size_t)capacity * sizeof(ServerLock));
    if (!locks) {
      return false_v;
    }
    server->locks = locks;
    server->lock_capacity = capacity;
  }
  const size_t length = strlen(path);
  char *copy = malloc(length + 1u);
  if (!copy) {
    return false_v;
  }
  memcpy(copy, path, length + 1u);
  ServerLock *lock = &server->locks[server->lock_count++];
  lock->path = copy;
  memcpy(lock->owner, owner, 32u);
  lock->time_us = time_us;
  qsort(server->locks, server->lock_count, sizeof(ServerLock),
        server_lock_compare);
  return true_v;
}

static ServerLock *server_lock_find(VkrDepotServer *server, const char *path) {
  const ServerLock key = {.path = (char *)path};
  return bsearch(&key, server->locks, server->lock_count, sizeof(ServerLock),
                 server_lock_compare);
}

static bool8_t server_locks_load(VkrDepotServer *server) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!server_file(server->store.root, "locks", path, sizeof(path))) {
    return false_v;
  }
  if (!vkr_bakery_is_file(path)) {
    return true_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_read_file(path, 1ull << 30, &bytes, &size) || size < 4u) {
    free(bytes);
    return false_v;
  }
  bool8_t ok = true_v;
  const uint32_t count = vkr_load_le_u32(bytes);
  const uint8_t *cursor = bytes + 4u;
  const uint8_t *end = bytes + size;
  for (uint32_t i = 0u; i < count && ok; ++i) {
    if (end - cursor < 2) {
      ok = false_v;
      break;
    }
    const uint16_t length = (uint16_t)(cursor[0] | (cursor[1] << 8));
    cursor += 2u;
    if (end - cursor < (int64_t)length + 40 ||
        length >= VKR_DEPOT_LOCK_PATH_MAX) {
      ok = false_v;
      break;
    }
    char lock_path[VKR_DEPOT_LOCK_PATH_MAX];
    memcpy(lock_path, cursor, length);
    lock_path[length] = '\0';
    cursor += length;
    ok = server_lock_add(server, lock_path, cursor,
                         vkr_load_le_u64(cursor + 32));
    cursor += 40u;
  }
  free(bytes);
  return ok;
}

// =============================================================================
// Closure and lock checks
// =============================================================================

typedef struct TreeEntries {
  VkrDepotTreeEntry *entries;
  uint32_t count;
  uint32_t capacity;
  uint8_t *bytes;
} TreeEntries;

static bool8_t tree_entries_visit(void *context,
                                  const VkrDepotTreeEntry *entry) {
  TreeEntries *list = context;
  if (list->count == list->capacity) {
    const uint32_t capacity = list->capacity ? list->capacity * 2u : 32u;
    VkrDepotTreeEntry *entries =
        realloc(list->entries, (size_t)capacity * sizeof(VkrDepotTreeEntry));
    if (!entries) {
      return false_v;
    }
    list->entries = entries;
    list->capacity = capacity;
  }
  list->entries[list->count++] = *entry;
  return true_v;
}

/* Parses a tree; names borrow `list->bytes`. */
static bool8_t tree_entries_load(const VkrDepotStore *store,
                                 const VkrDepotId *tree, TreeEntries *list) {
  MemZero(list, sizeof(*list));
  uint64_t size = 0u;
  if (!vkr_depot_store_get(store, tree, &list->bytes, &size)) {
    return false_v;
  }
  if (!vkr_depot_tree_parse(list->bytes, size, tree_entries_visit, list)) {
    free(list->entries);
    free(list->bytes);
    MemZero(list, sizeof(*list));
    return false_v;
  }
  return true_v;
}

static void tree_entries_free(TreeEntries *list) {
  free(list->entries);
  free(list->bytes);
  MemZero(list, sizeof(*list));
}

static const VkrDepotTreeEntry *
tree_entries_find(const TreeEntries *list, const VkrDepotTreeEntry *key) {
  for (uint32_t i = 0u; i < list->count; ++i) {
    const VkrDepotTreeEntry *entry = &list->entries[i];
    if (entry->name_size == key->name_size &&
        memcmp(entry->name, key->name, key->name_size) == 0) {
      return entry;
    }
  }
  return NULL;
}

typedef struct ClosureCheck {
  const VkrDepotStore *store;
  VkrDepotFileList changed;
  bool8_t incomplete;
} ClosureCheck;

static bool8_t closure_chunk(void *context, const VkrDepotChunkRef *chunk) {
  ClosureCheck *check = context;
  if (!vkr_depot_store_has(check->store, &chunk->id)) {
    check->incomplete = true_v;
    return false_v;
  }
  return true_v;
}

static bool8_t closure_blob(ClosureCheck *check, const VkrDepotId *blob) {
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_depot_store_get(check->store, blob, &bytes, &size)) {
    check->incomplete = true_v;
    return false_v;
  }
  const bool8_t ok =
      vkr_depot_blob_parse(bytes, size, NULL, closure_chunk, check);
  free(bytes);
  if (!ok) {
    check->incomplete = true_v;
  }
  return ok;
}

static bool8_t closure_path(char *out, uint32_t capacity, const char *prefix,
                            const VkrDepotTreeEntry *entry) {
  const int written = prefix[0] ? snprintf(out, capacity, "%s/%.*s", prefix,
                                           (int)entry->name_size, entry->name)
                                : snprintf(out, capacity, "%.*s",
                                           (int)entry->name_size, entry->name);
  return written >= 0 && written < (int)capacity;
}

/* Records every file of a removed subtree as changed. */
static bool8_t closure_removed(ClosureCheck *check, const VkrDepotId *tree,
                               const char *prefix) {
  VkrDepotFileList files;
  if (!vkr_depot_tree_files(check->store, tree, &files)) {
    return false_v;
  }
  bool8_t ok = true_v;
  for (uint32_t i = 0u; i < files.count && ok; ++i) {
    char path[VKR_DEPOT_PATH_CAPACITY];
    snprintf(path, sizeof(path), "%s/%s", prefix, files.entries[i].path);
    ok = vkr_depot_file_list_add(&check->changed, path, 0u, 0,
                                 &files.entries[i].blob);
  }
  vkr_depot_file_list_free(&files);
  return ok;
}

/* Checks that `tree` and everything below it that differs from `old` is in
   the store, and lists the file paths that differ. */
static bool8_t closure_tree(ClosureCheck *check, const VkrDepotId *tree,
                            const VkrDepotId *old, const char *prefix) {
  TreeEntries current;
  if (!tree_entries_load(check->store, tree, &current)) {
    check->incomplete = true_v;
    return false_v;
  }
  TreeEntries previous = {0};
  const bool8_t has_previous =
      old && tree_entries_load(check->store, old, &previous);
  bool8_t ok = true_v;
  for (uint32_t i = 0u; i < current.count && ok; ++i) {
    const VkrDepotTreeEntry *entry = &current.entries[i];
    const VkrDepotTreeEntry *before =
        has_previous ? tree_entries_find(&previous, entry) : NULL;
    if (before && before->kind == entry->kind &&
        vkr_depot_id_equal(&before->id, &entry->id)) {
      continue;
    }
    char path[VKR_DEPOT_PATH_CAPACITY];
    if (!closure_path(path, sizeof(path), prefix, entry)) {
      ok = false_v;
      break;
    }
    if (entry->kind == VKR_DEPOT_ENTRY_TREE) {
      const bool8_t was_tree = before && before->kind == VKR_DEPOT_ENTRY_TREE;
      ok = closure_tree(check, &entry->id, was_tree ? &before->id : NULL, path);
      if (ok && before && !was_tree) {
        ok = vkr_depot_file_list_add(&check->changed, path, 0u, 0, &before->id);
      }
    } else {
      ok = closure_blob(check, &entry->id) &&
           vkr_depot_file_list_add(&check->changed, path, 0u, 0, &entry->id);
      if (ok && before && before->kind == VKR_DEPOT_ENTRY_TREE) {
        ok = closure_removed(check, &before->id, path);
      }
    }
  }
  for (uint32_t i = 0u; has_previous && i < previous.count && ok; ++i) {
    const VkrDepotTreeEntry *entry = &previous.entries[i];
    if (tree_entries_find(&current, entry)) {
      continue;
    }
    char path[VKR_DEPOT_PATH_CAPACITY];
    ok = closure_path(path, sizeof(path), prefix, entry);
    if (ok && entry->kind == VKR_DEPOT_ENTRY_TREE) {
      ok = closure_removed(check, &entry->id, path);
    } else if (ok) {
      ok = vkr_depot_file_list_add(&check->changed, path, 0u, 0, &entry->id);
    }
  }
  tree_entries_free(&current);
  if (has_previous) {
    tree_entries_free(&previous);
  }
  return ok;
}

// =============================================================================
// Peers
// =============================================================================

static ServerPeer *server_peer(VkrDepotServer *server,
                               VkrNetConnectionId connection) {
  for (uint32_t i = 0u; i < SERVER_PEER_MAX; ++i) {
    if (server->peers[i].live && server->peers[i].connection == connection) {
      return &server->peers[i];
    }
  }
  return NULL;
}

static void server_peer_release(ServerPeer *peer) {
  free(peer->queue);
  for (uint32_t i = 0u; i < peer->orphan_count; ++i) {
    free(peer->orphans[i].bytes);
  }
  free(peer->orphans);
  MemZero(peer, sizeof(*peer));
}

static bool8_t server_queue_push(ServerPeer *peer, ServerSendItem item) {
  if (peer->queue_count == peer->queue_capacity) {
    const uint32_t capacity =
        peer->queue_capacity ? peer->queue_capacity * 2u : 256u;
    ServerSendItem *queue = malloc((size_t)capacity * sizeof(ServerSendItem));
    if (!queue) {
      return false_v;
    }
    for (uint32_t i = 0u; i < peer->queue_count; ++i) {
      queue[i] = peer->queue[(peer->queue_head + i) % peer->queue_capacity];
    }
    free(peer->queue);
    peer->queue = queue;
    peer->queue_capacity = capacity;
    peer->queue_head = 0u;
  }
  peer->queue[(peer->queue_head + peer->queue_count) % peer->queue_capacity] =
      item;
  peer->queue_count += 1u;
  return true_v;
}

void vkr_depot_server_on_event(VkrDepotServer *server,
                               const VkrNetEvent *event) {
  if (event->type == VKR_NET_EVENT_CONNECTED) {
    for (uint32_t i = 0u; i < SERVER_PEER_MAX; ++i) {
      ServerPeer *peer = &server->peers[i];
      if (!peer->live) {
        peer->live = true_v;
        peer->connection = event->connection;
        memcpy(peer->key, event->peer_key, 32u);
        peer->role =
            (uint32_t)vkr_net_core_user(server->core, event->connection);
        return;
      }
    }
    vkr_net_core_close(server->core, event->connection,
                       VKR_NET_CLOSE_SERVER_FULL, 0u);
  } else if (event->type == VKR_NET_EVENT_CLOSED) {
    ServerPeer *peer = server_peer(server, event->connection);
    if (peer) {
      server_peer_release(peer);
    }
  }
}

void vkr_depot_server_accept(void *context, const VkrNetAddress *address,
                             const uint8_t peer_key[32],
                             const uint8_t *credential,
                             uint32_t credential_size,
                             VkrNetAcceptResult *out_result) {
  (void)address;
  (void)credential;
  (void)credential_size;
  VkrDepotServer *server = context;
  const uint32_t role = server_role(server, peer_key);
  if (role == VKR_DEPOT_ROLE_NONE) {
    out_result->code = VKR_NET_CLOSE_ACCESS_DENIED;
    return;
  }
  out_result->user = role;
}

// =============================================================================
// Requests
// =============================================================================

typedef struct RefNames {
  char names[VKR_DEPOT_REFS_MAX][VKR_DEPOT_BRANCH_NAME];
  uint32_t count;
} RefNames;

static bool8_t server_ref_name_visit(void *context, const char *name,
                                     bool8_t is_directory) {
  RefNames *list = context;
  if (is_directory || !vkr_depot_branch_valid(name)) {
    return true_v;
  }
  if (list->count == VKR_DEPOT_REFS_MAX) {
    return false_v;
  }
  snprintf(list->names[list->count++], VKR_DEPOT_BRANCH_NAME, "%s", name);
  return true_v;
}

static void server_on_refs(VkrDepotServer *server, ServerPeer *peer,
                           uint64_t request, uint64_t now) {
  VkrDepotRefsWire *reply = &server->reply.refs;
  MemZero(reply, sizeof(*reply));
  reply->request = request;
  char refs[VKR_DEPOT_PATH_CAPACITY];
  static RefNames names;
  names.count = 0u;
  if (server_file(server->store.root, "refs", refs, sizeof(refs)) &&
      vkr_bakery_is_directory(refs)) {
    /* More than VKR_DEPOT_REFS_MAX branches: the first ones answer. */
    (void)vkr_bakery_list_directory(refs, server_ref_name_visit, &names);
  }
  for (uint32_t i = 0u; i < names.count; ++i) {
    VkrDepotId commit;
    if (!vkr_depot_ref_read(server->store.root, names.names[i], &commit)) {
      continue;
    }
    VkrDepotRefWire *ref = &reply->refs[reply->ref_count++];
    snprintf(ref->name, sizeof(ref->name), "%s", names.names[i]);
    memcpy(ref->commit, commit.bytes, VKR_DEPOT_ID_SIZE);
  }
  (void)vkr_depot_send(server->session, peer->connection, VKR_DEPOT_MSG_REFS,
                       reply, now);
}

static void server_on_have(VkrDepotServer *server, ServerPeer *peer,
                           const VkrDepotIdsWire *have, uint64_t now) {
  VkrDepotBitsWire *reply = &server->reply.bits;
  MemZero(reply, sizeof(*reply));
  reply->request = have->request;
  reply->bits_size = (have->id_count + 7u) / 8u;
  for (uint32_t i = 0u; i < have->id_count; ++i) {
    VkrDepotId id;
    memcpy(id.bytes, have->ids[i], VKR_DEPOT_ID_SIZE);
    if (!vkr_depot_store_has(&server->store, &id)) {
      bits_set(reply->bits, i);
    }
  }
  (void)vkr_depot_send(server->session, peer->connection,
                       VKR_DEPOT_MSG_HAVE_REPLY, reply, now);
}

static void server_on_get(VkrDepotServer *server, ServerPeer *peer,
                          const VkrDepotIdsWire *get, uint64_t now) {
  VkrDepotBitsWire *reply = &server->reply.bits;
  MemZero(reply, sizeof(*reply));
  reply->request = get->request;
  reply->bits_size = (get->id_count + 7u) / 8u;
  for (uint32_t i = 0u; i < get->id_count; ++i) {
    ServerSendItem item = {
        .request = get->request, .index = i, .zstd = get->zstd};
    memcpy(item.id.bytes, get->ids[i], VKR_DEPOT_ID_SIZE);
    if (!vkr_depot_store_has(&server->store, &item.id) ||
        !server_queue_push(peer, item)) {
      bits_set(reply->bits, i);
    }
  }
  (void)vkr_depot_send(server->session, peer->connection,
                       VKR_DEPOT_MSG_GET_REPLY, reply, now);
}

static void server_put_finish(VkrDepotServer *server, ServerPeer *peer,
                              ServerPut *put, uint64_t now) {
  VkrDepotBitsWire *reply = &server->reply.bits;
  MemZero(reply, sizeof(*reply));
  reply->request = put->request;
  reply->bits_size = (put->count + 7u) / 8u;
  memcpy(reply->bits, put->bad, sizeof(put->bad));
  (void)vkr_depot_send(server->session, peer->connection,
                       VKR_DEPOT_MSG_PUT_REPLY, reply, now);
  put->live = false_v;
}

/* Stores one uploaded representation for a live PUT. */
static void server_put_bytes(VkrDepotServer *server, ServerPeer *peer,
                             ServerPut *put, uint32_t index,
                             const uint8_t *bytes, uint32_t size,
                             uint64_t now) {
  if (index >= put->count || bits_get(put->received, index)) {
    return;
  }
  bits_set(put->received, index);
  if (!vkr_depot_store_put_stored(&server->store, &put->ids[index], bytes,
                                  size)) {
    bits_set(put->bad, index);
  }
  put->remaining -= 1u;
  if (put->remaining == 0u) {
    server_put_finish(server, peer, put, now);
  }
}

static void server_on_put(VkrDepotServer *server, ServerPeer *peer,
                          const VkrDepotIdsWire *put_request, uint64_t now) {
  if (peer->role < VKR_DEPOT_ROLE_WRITE) {
    vkr_net_core_close(server->core, peer->connection,
                       VKR_NET_CLOSE_ACCESS_DENIED, now);
    return;
  }
  ServerPut *put = NULL;
  for (uint32_t i = 0u; i < SERVER_PUT_SLOTS; ++i) {
    if (!peer->puts[i].live) {
      put = &peer->puts[i];
      break;
    }
  }
  if (!put || put_request->id_count == 0u) {
    vkr_net_core_close(server->core, peer->connection,
                       VKR_NET_CLOSE_PROTOCOL_VIOLATION, now);
    return;
  }
  MemZero(put, sizeof(*put));
  put->live = true_v;
  put->request = put_request->request;
  put->count = put_request->id_count;
  put->remaining = put->count;
  for (uint32_t i = 0u; i < put->count; ++i) {
    memcpy(put->ids[i].bytes, put_request->ids[i], VKR_DEPOT_ID_SIZE);
  }
  /* Bytes that overtook their PUT on the other channel. */
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < peer->orphan_count; ++i) {
    ServerOrphan *orphan = &peer->orphans[i];
    if ((orphan->tag >> 8) == put->request && put->live) {
      server_put_bytes(server, peer, put, (uint32_t)(orphan->tag & 0xffu),
                       orphan->bytes, orphan->size, now);
      free(orphan->bytes);
    } else {
      peer->orphans[kept++] = *orphan;
    }
  }
  peer->orphan_count = kept;
}

static bool8_t server_lock_path_valid(const char *path) {
  const size_t size = strlen(path);
  if (size == 0u || path[0] == '/') {
    return false_v;
  }
  const char *component = path;
  for (;;) {
    const char *slash = strchr(component, '/');
    const uint32_t length =
        (uint32_t)(slash ? (size_t)(slash - component) : strlen(component));
    if (!vkr_depot_name_valid(component, length)) {
      return false_v;
    }
    if (!slash) {
      return true_v;
    }
    component = slash + 1;
  }
}

static void server_on_update_ref(VkrDepotServer *server, ServerPeer *peer,
                                 const VkrDepotUpdateRefWire *update,
                                 uint64_t now) {
  VkrDepotUpdateRefReplyWire *reply = &server->reply.update_ref_reply;
  MemZero(reply, sizeof(*reply));
  reply->request = update->request;
  VkrDepotId current = {0};
  const bool8_t exists =
      vkr_depot_ref_read(server->store.root, update->name, &current);
  memcpy(reply->current, current.bytes, VKR_DEPOT_ID_SIZE);
  VkrDepotId expected;
  VkrDepotId commit_id;
  memcpy(expected.bytes, update->expected, VKR_DEPOT_ID_SIZE);
  memcpy(commit_id.bytes, update->commit, VKR_DEPOT_ID_SIZE);

  if (peer->role < VKR_DEPOT_ROLE_WRITE ||
      !vkr_depot_branch_valid(update->name)) {
    reply->status = VKR_DEPOT_REF_DENIED;
  } else if ((exists && !vkr_depot_id_equal(&current, &expected)) ||
             (!exists && !vkr_depot_id_zero(&expected))) {
    reply->status = VKR_DEPOT_REF_CONFLICT;
  } else {
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    VkrDepotCommit commit;
    VkrDepotCommit previous = {0};
    uint8_t *previous_bytes = NULL;
    uint64_t previous_size = 0u;
    const bool8_t have_previous =
        exists &&
        vkr_depot_store_get(&server->store, &current, &previous_bytes,
                            &previous_size) &&
        vkr_depot_commit_parse(previous_bytes, previous_size, &previous);
    if (!vkr_depot_store_get(&server->store, &commit_id, &bytes, &size) ||
        !vkr_depot_commit_parse(bytes, size, &commit)) {
      reply->status = VKR_DEPOT_REF_INCOMPLETE;
    } else {
      ClosureCheck check = {.store = &server->store};
      const bool8_t complete =
          closure_tree(&check, &commit.tree,
                       have_previous ? &previous.tree : NULL, "") &&
          !check.incomplete;
      if (!complete) {
        reply->status = VKR_DEPOT_REF_INCOMPLETE;
      } else {
        reply->status = VKR_DEPOT_REF_OK;
        for (uint32_t i = 0u; i < check.changed.count; ++i) {
          const ServerLock *lock =
              server_lock_find(server, check.changed.entries[i].path);
          if (lock && memcmp(lock->owner, peer->key, 32u) != 0) {
            reply->status = VKR_DEPOT_REF_LOCKED;
            memcpy(reply->current, current.bytes, VKR_DEPOT_ID_SIZE);
            break;
          }
        }
        if (reply->status == VKR_DEPOT_REF_OK) {
          if (server_ref_write(server->store.root, update->name, &commit_id)) {
            memcpy(reply->current, commit_id.bytes, VKR_DEPOT_ID_SIZE);
          } else {
            reply->status = VKR_DEPOT_REF_DENIED;
          }
        }
      }
      vkr_depot_file_list_free(&check.changed);
    }
    free(bytes);
    free(previous_bytes);
  }
  (void)vkr_depot_send(server->session, peer->connection,
                       VKR_DEPOT_MSG_UPDATE_REF_REPLY, reply, now);
}

static void server_on_lock(VkrDepotServer *server, ServerPeer *peer,
                           const VkrDepotLockWire *lock, uint64_t now) {
  VkrDepotLockReplyWire *reply = &server->reply.lock_reply;
  MemZero(reply, sizeof(*reply));
  reply->request = lock->request;
  ServerLock *held = server_lock_find(server, lock->path);
  if (peer->role < VKR_DEPOT_ROLE_WRITE ||
      !server_lock_path_valid(lock->path)) {
    reply->status = VKR_DEPOT_LOCK_DENIED;
  } else if (lock->release) {
    if (!held) {
      reply->status = VKR_DEPOT_LOCK_NOT_HELD;
    } else if (memcmp(held->owner, peer->key, 32u) != 0 &&
               !(lock->force && peer->role >= VKR_DEPOT_ROLE_ADMIN)) {
      reply->status = VKR_DEPOT_LOCK_HELD;
      memcpy(reply->owner, held->owner, 32u);
    } else {
      const uint32_t index = (uint32_t)(held - server->locks);
      free(held->path);
      MemCopy(&server->locks[index], &server->locks[index + 1u],
              (uint64_t)(server->lock_count - index - 1u) * sizeof(ServerLock));
      server->lock_count -= 1u;
      reply->status =
          server_locks_save(server) ? VKR_DEPOT_LOCK_OK : VKR_DEPOT_LOCK_DENIED;
    }
  } else if (held) {
    reply->status = memcmp(held->owner, peer->key, 32u) == 0
                        ? VKR_DEPOT_LOCK_OK
                        : VKR_DEPOT_LOCK_HELD;
    memcpy(reply->owner, held->owner, 32u);
  } else {
    reply->status =
        server_lock_add(server, lock->path, peer->key, server_unix_us()) &&
                server_locks_save(server)
            ? VKR_DEPOT_LOCK_OK
            : VKR_DEPOT_LOCK_DENIED;
    memcpy(reply->owner, peer->key, 32u);
  }
  (void)vkr_depot_send(server->session, peer->connection,
                       VKR_DEPOT_MSG_LOCK_REPLY, reply, now);
}

static void server_on_locks(VkrDepotServer *server, ServerPeer *peer,
                            const VkrDepotLocksRequestWire *request,
                            uint64_t now) {
  VkrDepotLocksWire *reply = &server->reply.locks;
  MemZero(reply, sizeof(*reply));
  reply->request = request->request;
  for (uint32_t i = 0u; i < server->lock_count; ++i) {
    const ServerLock *lock = &server->locks[i];
    if (request->after[0] && strcmp(lock->path, request->after) <= 0) {
      continue;
    }
    if (reply->entry_count == VKR_DEPOT_LOCKS_PAGE) {
      reply->more = true_v;
      break;
    }
    VkrDepotLockEntryWire *entry = &reply->entries[reply->entry_count++];
    snprintf(entry->path, sizeof(entry->path), "%s", lock->path);
    memcpy(entry->owner, lock->owner, 32u);
    entry->time_us = lock->time_us;
  }
  (void)vkr_depot_send(server->session, peer->connection, VKR_DEPOT_MSG_LOCKS,
                       reply, now);
}

static void server_on_data(VkrDepotServer *server, ServerPeer *peer,
                           const VkrNetEvent *event, uint64_t now) {
  if (!event->has_tag) {
    vkr_net_core_close(server->core, peer->connection,
                       VKR_NET_CLOSE_PROTOCOL_VIOLATION, now);
    return;
  }
  const uint64_t request = event->tag >> 8;
  const uint32_t index = (uint32_t)(event->tag & 0xffu);
  for (uint32_t i = 0u; i < SERVER_PUT_SLOTS; ++i) {
    ServerPut *put = &peer->puts[i];
    if (put->live && put->request == request) {
      server_put_bytes(server, peer, put, index, event->data, event->size, now);
      return;
    }
  }
  /* The PUT has not arrived yet: keep a copy until it does. */
  if (peer->orphan_count == SERVER_ORPHAN_MAX ||
      peer->role < VKR_DEPOT_ROLE_WRITE) {
    vkr_net_core_close(server->core, peer->connection,
                       VKR_NET_CLOSE_PROTOCOL_VIOLATION, now);
    return;
  }
  if (!peer->orphans) {
    peer->orphans = malloc(SERVER_ORPHAN_MAX * sizeof(ServerOrphan));
    if (!peer->orphans) {
      return;
    }
  }
  uint8_t *copy = malloc(event->size ? event->size : 1u);
  if (!copy) {
    return;
  }
  memcpy(copy, event->data, event->size);
  peer->orphans[peer->orphan_count++] =
      (ServerOrphan){.tag = event->tag, .bytes = copy, .size = event->size};
}

static void server_service_event(void *context, VkrNetConnectionId connection,
                                 uint8_t channel, const VkrNetEvent *event,
                                 uint64_t now) {
  VkrDepotServer *server = context;
  ServerPeer *peer = server_peer(server, connection);
  if (!peer || event->type != VKR_NET_EVENT_MESSAGE) {
    return;
  }
  if (channel == VKR_DEPOT_DATA_CHANNEL) {
    server_on_data(server, peer, event, now);
    return;
  }
  const VkrWireMessage *descriptor = NULL;
  if (vkr_wire_decode(vkr_depot_schema(), event->data, event->size,
                      &server->message, sizeof(server->message),
                      &descriptor) != VKR_WIRE_OK) {
    vkr_net_core_close(server->core, connection, VKR_NET_CLOSE_SCHEMA_VIOLATION,
                       now);
    return;
  }
  const VkrDepotMessage *message = &server->message;
  switch (descriptor->id) {
  case VKR_DEPOT_MSG_REFS_REQUEST:
    server_on_refs(server, peer, message->request.request, now);
    break;
  case VKR_DEPOT_MSG_HAVE:
    server_on_have(server, peer, &message->ids, now);
    break;
  case VKR_DEPOT_MSG_GET:
    server_on_get(server, peer, &message->ids, now);
    break;
  case VKR_DEPOT_MSG_PUT:
    server_on_put(server, peer, &message->ids, now);
    break;
  case VKR_DEPOT_MSG_UPDATE_REF:
    server_on_update_ref(server, peer, &message->update_ref, now);
    break;
  case VKR_DEPOT_MSG_LOCK:
    server_on_lock(server, peer, &message->lock, now);
    break;
  case VKR_DEPOT_MSG_LOCKS_REQUEST:
    server_on_locks(server, peer, &message->locks_request, now);
    break;
  default:
    /* A reply sent to the server breaks the protocol. */
    vkr_net_core_close(server->core, connection,
                       VKR_NET_CLOSE_PROTOCOL_VIOLATION, now);
    break;
  }
}

static uint16_t server_service_accept(void *context,
                                      VkrNetConnectionId connection,
                                      uint16_t version) {
  (void)version;
  VkrDepotServer *server = context;
  const ServerPeer *peer = server_peer(server, connection);
  return peer && peer->role >= VKR_DEPOT_ROLE_READ
             ? 0u
             : (uint16_t)VKR_NET_CLOSE_ACCESS_DENIED;
}

// =============================================================================
// Lifetime and sending
// =============================================================================

VkrDepotServer *vkr_depot_server_create(VkrAllocator *allocator,
                                        VkrNetCore *core,
                                        VkrNetSession *session,
                                        const VkrDepotServerConfig *config) {
  if (!allocator || !core || !session || !config || !config->store_root) {
    return NULL;
  }
  VkrDepotServer *server = calloc(1u, sizeof(VkrDepotServer));
  if (!server) {
    return NULL;
  }
  server->allocator = allocator;
  server->core = core;
  server->session = session;
  server->open_access = config->open_access;
  if (!vkr_depot_store_open(&server->store, config->store_root,
                            config->codec) ||
      !server_access_load(server) || !server_locks_load(server)) {
    vkr_depot_server_destroy(server);
    return NULL;
  }
  VkrNetService service = vkr_depot_service_template();
  service.context = server;
  service.accept = server_service_accept;
  service.event = server_service_event;
  if (!vkr_net_session_register(session, &service)) {
    vkr_depot_server_destroy(server);
    return NULL;
  }
  return server;
}

void vkr_depot_server_destroy(VkrDepotServer *server) {
  if (!server) {
    return;
  }
  for (uint32_t i = 0u; i < SERVER_PEER_MAX; ++i) {
    if (server->peers[i].live) {
      server_peer_release(&server->peers[i]);
    }
  }
  for (uint32_t i = 0u; i < server->lock_count; ++i) {
    free(server->locks[i].path);
  }
  free(server->locks);
  free(server);
}

/* Sends one queued item; false when the data channel is full. */
static bool8_t server_send_item(VkrDepotServer *server, ServerPeer *peer,
                                const ServerSendItem *item, uint64_t now) {
  uint8_t *stored = NULL;
  uint64_t size = 0u;
  if (!vkr_depot_store_get_stored(&server->store, &item->id, &stored, &size)) {
    /* Removed since the GET: the client times out on it. */
    return true_v;
  }
  /* A client without zstd receives the raw representation. */
  if (!item->zstd && size > 0u && stored[0] != 0u) {
    uint8_t *decoded = NULL;
    uint64_t decoded_size = 0u;
    if (!vkr_depot_stored_decode(server->store.codec, stored, size, &decoded,
                                 &decoded_size)) {
      free(stored);
      return true_v;
    }
    free(stored);
    stored = malloc(VKR_DEPOT_STORED_HEADER + decoded_size);
    if (!stored) {
      free(decoded);
      return true_v;
    }
    stored[0] = 0u;
    vkr_store_le_u32(stored + 1, (uint32_t)decoded_size);
    memcpy(stored + VKR_DEPOT_STORED_HEADER, decoded, decoded_size);
    free(decoded);
    size = VKR_DEPOT_STORED_HEADER + decoded_size;
  }
  const VkrNetSendOptions options = {
      .flags = VKR_NET_SEND_TAG,
      .tag = vkr_depot_tag(item->request, item->index)};
  const VkrNetSendStatus status = vkr_net_session_send(
      server->session, peer->connection, VKR_NET_SERVICE_DEPOT,
      VKR_DEPOT_DATA_CHANNEL, stored, (uint32_t)size, &options, now);
  free(stored);
  return status != VKR_NET_SEND_QUEUE_FULL;
}

void vkr_depot_server_update(VkrDepotServer *server, uint64_t now_us) {
  for (uint32_t i = 0u; i < SERVER_PEER_MAX; ++i) {
    ServerPeer *peer = &server->peers[i];
    if (!peer->live) {
      continue;
    }
    while (peer->queue_count > 0u) {
      const ServerSendItem *item = &peer->queue[peer->queue_head];
      if (!server_send_item(server, peer, item, now_us)) {
        break;
      }
      peer->queue_head = (peer->queue_head + 1u) % peer->queue_capacity;
      peer->queue_count -= 1u;
    }
  }
  vkr_net_core_flush(server->core);
}
