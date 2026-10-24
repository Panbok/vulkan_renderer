#include "vkr_depot_server.h"

#include "core/vkr_byte_io.h"
#include "core/vkr_threads.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_repository.h"
#include "vkr_depot_workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVER_PEER_MAX 256u
#define SERVER_PUT_SLOTS 16u
/* Object bytes that arrived before their PUT. */
#define SERVER_ORPHAN_MAX 1024u
/* Their bytes: about two data-channel receive windows. A peer past it is
   closed. */
#define SERVER_ORPHAN_BYTES_MAX (128u * 1024u * 1024u)
/* Objects queued to send to one peer: twice what a client's pipelined GETs
   ask for at once. GET_REPLY reports the rest as missing. */
#define SERVER_QUEUE_MAX 8192u
#define SERVER_ACCESS_MAX 4096u
/* Keys the owner refused, kept until the server stops; the oldest leaves
   when it is full. */
#define SERVER_DENIED_MAX 256u
/* Pending requests one network address may hold, so one machine cannot
   push everyone else's out of the list with many keys. */
#define SERVER_REQUESTS_PER_ADDRESS 2u
/* How often the server looks for a changed access file. */
#define SERVER_ACCESS_POLL_US 1000000u
/* Objects at the front of one peer's queue that loaders read and check
   ahead of their send. */
#define SERVER_LOAD_AHEAD 32u

typedef enum ServerLoadState {
  SERVER_LOAD_WAITING = 0,
  SERVER_LOAD_READY,
  /* Gone or torn: the client times out on it, as on a synchronous read. */
  SERVER_LOAD_MISSING,
} ServerLoadState;

/* One queued object a loader reads ahead of its send. The queued item owns
   it until its peer leaves while it waits; the loader then frees it. The
   fields after `id` are guarded by the loader's lock. */
typedef struct ServerLoad {
  struct ServerLoad *next;
  VkrDepotId id;
  bool8_t zstd;
  uint8_t state;
  bool8_t abandoned;
  uint8_t *stored;
  uint64_t size;
} ServerLoad;

/* Threads that read and check queued objects (VkrDepotServerConfig
   load_threads), so the host thread's pump reads no file and hashes
   nothing. */
typedef struct ServerLoader {
  VkrAllocator *allocator;
  VkrThread threads[VKR_DEPOT_SERVER_LOAD_THREADS_MAX];
  uint32_t thread_count;
  VkrMutex mutex;
  VkrCondVar work;
  ServerLoad *head;
  ServerLoad *tail;
  bool8_t stopping;
  const VkrDepotStore *store;
  void (*wake)(void *context);
  void *wake_context;
} ServerLoader;

typedef struct ServerSendItem {
  uint64_t request;
  uint32_t index;
  bool8_t zstd;
  VkrDepotId id;
  /* NULL until a loader takes it. */
  ServerLoad *load;
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
  /* The depot service version the connection opened with. */
  uint16_t version;
  /* A HELLO selected the repository; another one closes the connection. */
  bool8_t selected;
  /* The client's HELLO said it accepts no connection from outside. */
  bool8_t outbound_only;
  ServerSendItem *queue;
  uint32_t queue_head;
  uint32_t queue_count;
  uint32_t queue_capacity;
  ServerPut puts[SERVER_PUT_SLOTS];
  ServerOrphan *orphans;
  uint32_t orphan_count;
  uint64_t orphan_bytes;
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

typedef struct ServerRequest {
  bool8_t live;
  VkrDepotAccessRequest request;
} ServerRequest;

typedef struct RefNames {
  char names[VKR_DEPOT_REFS_MAX][VKR_DEPOT_BRANCH_NAME];
  uint32_t count;
} RefNames;

struct VkrDepotServer {
  VkrAllocator *allocator;
  VkrNetCore *core;
  VkrNetSession *session;
  VkrDepotStore store;
  /* NULL reads each object on the host thread as it is sent. */
  ServerLoader *loader;
  bool8_t open_access;
  ServerPeer peers[SERVER_PEER_MAX];
  ServerLock *locks;
  uint32_t lock_count;
  uint32_t lock_capacity;
  ServerAccess access[SERVER_ACCESS_MAX];
  uint32_t access_count;
  /* The access file as last read: whether it existed, its time and size. */
  bool8_t access_exists;
  int64_t access_mtime_ns;
  uint64_t access_size;
  uint64_t access_checked_us;
  ServerRequest requests[VKR_DEPOT_REQUESTS_MAX];
  uint8_t denied[SERVER_DENIED_MAX][32];
  uint32_t denied_count;
  uint32_t denied_next;
  VkrDepotServerStats stats;
  RefNames ref_names;
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
  return vkr_bakery_write_private_file_atomic(path, out_keys->secret_key,
                                              VKR_NET_KEY_SIZE);
}

/* The access file's existence, time and size, as the poll compares them. */
static void server_access_stat(const VkrDepotServer *server,
                               bool8_t *out_exists, int64_t *out_mtime_ns,
                               uint64_t *out_size) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  VkrBakeryStat stat = {0};
  *out_exists = server_file(server->store.root, "access", path, sizeof(path)) &&
                vkr_bakery_stat(path, &stat) && stat.exists;
  *out_mtime_ns = *out_exists ? stat.mtime_ns : 0;
  *out_size = *out_exists ? stat.size : 0u;
}

/* Reads the access file; a file that fails to read leaves the list as it
   was, so a passing failure locks nobody out. */
static bool8_t server_access_load(VkrDepotServer *server) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!server_file(server->store.root, "access", path, sizeof(path))) {
    return false_v;
  }
  bool8_t exists = false_v;
  int64_t mtime_ns = 0;
  uint64_t file_size = 0u;
  server_access_stat(server, &exists, &mtime_ns, &file_size);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (exists &&
      (!vkr_bakery_read_file(path, SERVER_ACCESS_MAX * 33u, &bytes, &size) ||
       size % 33u != 0u)) {
    free(bytes);
    return false_v;
  }
  server->access_exists = exists;
  server->access_mtime_ns = mtime_ns;
  server->access_size = file_size;
  server->access_count = 0u;
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

/* Paths compare without ASCII case: on macOS and Windows `Scene.json` and
   `scene.json` are one file, so a lock on either holds both. */
static int server_path_compare(const char *a, const char *b) {
  return vkr_depot_name_fold_compare(a, (uint32_t)strlen(a), b,
                                     (uint32_t)strlen(b));
}

static int server_lock_compare(const void *lhs, const void *rhs) {
  return server_path_compare(((const ServerLock *)lhs)->path,
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

/* Parses a tree; names borrow `list->bytes`. `*out_invalid` (may be NULL)
   tells a tree that does not parse from one that is missing. */
static bool8_t tree_entries_load(const VkrDepotStore *store,
                                 const VkrDepotId *tree, TreeEntries *list,
                                 bool8_t *out_invalid) {
  MemZero(list, sizeof(*list));
  uint64_t size = 0u;
  if (!vkr_depot_store_get(store, tree, &list->bytes, &size)) {
    return false_v;
  }
  if (!vkr_depot_tree_parse(list->bytes, size, tree_entries_visit, list)) {
    free(list->entries);
    free(list->bytes);
    MemZero(list, sizeof(*list));
    if (out_invalid) {
      *out_invalid = true_v;
    }
    return false_v;
  }
  return true_v;
}

static void tree_entries_free(TreeEntries *list) {
  free(list->entries);
  free(list->bytes);
  MemZero(list, sizeof(*list));
}

/* Byte order of names, the order a parsed tree holds them in. */
static int tree_entry_compare(const void *lhs, const void *rhs) {
  const VkrDepotTreeEntry *a = lhs;
  const VkrDepotTreeEntry *b = rhs;
  const int order = memcmp(a->name, b->name, Min(a->name_size, b->name_size));
  if (order != 0) {
    return order;
  }
  return a->name_size < b->name_size ? -1 : a->name_size > b->name_size;
}

static const VkrDepotTreeEntry *
tree_entries_find(const TreeEntries *list, const VkrDepotTreeEntry *key) {
  if (!list->count) {
    return NULL;
  }
  return bsearch(key, list->entries, list->count, sizeof(VkrDepotTreeEntry),
                 tree_entry_compare);
}

typedef struct ClosureCheck {
  const VkrDepotStore *store;
  VkrDepotFileList changed;
  bool8_t incomplete;
  /* A tree that does not parse or passes the limits of vkr_depot_object.h:
     the push is refused as invalid. */
  bool8_t invalid;
  /* The folder being checked; extended in place and cut back, so a deep
     tree costs no stack per level. */
  char path[VKR_DEPOT_PATH_CAPACITY];
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

/* Appends `entry`'s name to the check's path; returns the length to cut
   back to, or SIZE_MAX when the path is too long. */
static size_t closure_enter(ClosureCheck *check,
                            const VkrDepotTreeEntry *entry) {
  const size_t saved = strlen(check->path);
  const size_t room = VKR_DEPOT_PATH_MAX - saved;
  const int written =
      snprintf(check->path + saved, room, saved ? "/%.*s" : "%.*s",
               (int)entry->name_size, entry->name);
  if (written < 0 || (size_t)written >= room) {
    check->path[saved] = '\0';
    check->invalid = true_v;
    return SIZE_MAX;
  }
  return saved;
}

static bool8_t closure_change(ClosureCheck *check, const VkrDepotId *id) {
  if (check->changed.count >= VKR_DEPOT_TREE_FILES_MAX) {
    check->invalid = true_v;
    return false_v;
  }
  return vkr_depot_file_list_add(&check->changed, check->path, 0u, 0, id);
}

/* Records every file of a removed subtree, at the check's path, as
   changed. */
static bool8_t closure_removed(ClosureCheck *check, const VkrDepotId *tree) {
  VkrDepotFileList files;
  if (!vkr_depot_tree_files(check->store, tree, &files)) {
    check->invalid = true_v;
    return false_v;
  }
  bool8_t ok = true_v;
  const size_t saved = strlen(check->path);
  for (uint32_t i = 0u; i < files.count && ok; ++i) {
    const size_t room = VKR_DEPOT_PATH_MAX - saved;
    const int written =
        snprintf(check->path + saved, room, "/%s", files.entries[i].path);
    ok = written >= 0 && (size_t)written < room &&
         closure_change(check, &files.entries[i].blob);
    check->path[saved] = '\0';
  }
  vkr_depot_file_list_free(&files);
  return ok;
}

/* Checks that `tree` and everything below it that differs from `old` is in
   the store, and lists the file paths that differ. Refuses trees deeper
   than VKR_DEPOT_TREE_DEPTH_MAX or with more changed files than
   VKR_DEPOT_TREE_FILES_MAX, counting each path a shared subtree appears
   at, so lock checks see every path. */
static bool8_t closure_tree(ClosureCheck *check, const VkrDepotId *tree,
                            const VkrDepotId *old, uint32_t depth) {
  if (depth > VKR_DEPOT_TREE_DEPTH_MAX) {
    check->invalid = true_v;
    return false_v;
  }
  TreeEntries current;
  if (!tree_entries_load(check->store, tree, &current, &check->invalid)) {
    check->incomplete = !check->invalid;
    return false_v;
  }
  TreeEntries previous = {0};
  const bool8_t has_previous =
      old && tree_entries_load(check->store, old, &previous, NULL);
  bool8_t ok = true_v;
  for (uint32_t i = 0u; i < current.count && ok; ++i) {
    const VkrDepotTreeEntry *entry = &current.entries[i];
    const VkrDepotTreeEntry *before =
        has_previous ? tree_entries_find(&previous, entry) : NULL;
    if (before && before->kind == entry->kind &&
        vkr_depot_id_equal(&before->id, &entry->id)) {
      continue;
    }
    const size_t saved = closure_enter(check, entry);
    if (saved == SIZE_MAX) {
      ok = false_v;
      break;
    }
    if (entry->kind == VKR_DEPOT_ENTRY_TREE) {
      const bool8_t was_tree = before && before->kind == VKR_DEPOT_ENTRY_TREE;
      ok = closure_tree(check, &entry->id, was_tree ? &before->id : NULL,
                        depth + 1u);
      if (ok && before && !was_tree) {
        ok = closure_change(check, &before->id);
      }
    } else {
      ok = closure_blob(check, &entry->id) && closure_change(check, &entry->id);
      if (ok && before && before->kind == VKR_DEPOT_ENTRY_TREE) {
        ok = closure_removed(check, &before->id);
      }
    }
    check->path[saved] = '\0';
  }
  for (uint32_t i = 0u; has_previous && i < previous.count && ok; ++i) {
    const VkrDepotTreeEntry *entry = &previous.entries[i];
    if (tree_entries_find(&current, entry)) {
      continue;
    }
    const size_t saved = closure_enter(check, entry);
    if (saved == SIZE_MAX) {
      ok = false_v;
      break;
    }
    ok = entry->kind == VKR_DEPOT_ENTRY_TREE
             ? closure_removed(check, &entry->id)
             : closure_change(check, &entry->id);
    check->path[saved] = '\0';
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

/* The stored form of `id` a client receives: zstd frames only for a client
   that decodes them. False when the object is gone or fails its check. */
static bool8_t server_read_object(const VkrDepotStore *store,
                                  const VkrDepotId *id, bool8_t zstd,
                                  uint8_t **out_stored, uint64_t *out_size) {
  uint8_t *stored = NULL;
  uint64_t size = 0u;
  if (!vkr_depot_store_get_stored(store, id, &stored, &size)) {
    return false_v;
  }
  if (!zstd && size > 0u && stored[0] != 0u) {
    uint8_t *decoded = NULL;
    uint64_t decoded_size = 0u;
    if (!vkr_depot_stored_decode(store->codec, stored, size, &decoded,
                                 &decoded_size)) {
      free(stored);
      return false_v;
    }
    free(stored);
    stored = malloc(VKR_DEPOT_STORED_HEADER + decoded_size);
    if (!stored) {
      free(decoded);
      return false_v;
    }
    stored[0] = 0u;
    vkr_store_le_u32(stored + 1, (uint32_t)decoded_size);
    memcpy(stored + VKR_DEPOT_STORED_HEADER, decoded, decoded_size);
    free(decoded);
    size = VKR_DEPOT_STORED_HEADER + decoded_size;
  }
  *out_stored = stored;
  *out_size = size;
  return true_v;
}

// =============================================================================
// Loaders
// =============================================================================

static void *server_load_thread(void *argument) {
  ServerLoader *loader = argument;
  vkr_mutex_lock(loader->mutex);
  for (;;) {
    while (!loader->stopping && !loader->head) {
      vkr_cond_wait(loader->work, loader->mutex);
    }
    if (loader->stopping) {
      break;
    }
    ServerLoad *load = loader->head;
    loader->head = load->next;
    if (!loader->head) {
      loader->tail = NULL;
    }
    vkr_mutex_unlock(loader->mutex);

    uint8_t *stored = NULL;
    uint64_t size = 0u;
    const bool8_t read = server_read_object(loader->store, &load->id,
                                            load->zstd, &stored, &size);

    vkr_mutex_lock(loader->mutex);
    if (load->abandoned) {
      free(stored);
      free(load);
    } else {
      load->stored = stored;
      load->size = size;
      load->state = read ? SERVER_LOAD_READY : SERVER_LOAD_MISSING;
    }
    vkr_mutex_unlock(loader->mutex);
    if (loader->wake) {
      loader->wake(loader->wake_context);
    }
    vkr_mutex_lock(loader->mutex);
  }
  vkr_mutex_unlock(loader->mutex);
  return NULL;
}

/* Stops the loaders. A load still queued then belongs to its item, or, when
   abandoned, is freed here. */
static void server_loader_destroy(ServerLoader *loader) {
  if (!loader) {
    return;
  }
  if (loader->mutex) {
    vkr_mutex_lock(loader->mutex);
    loader->stopping = true_v;
    vkr_cond_broadcast(loader->work);
    vkr_mutex_unlock(loader->mutex);
  }
  for (uint32_t i = 0u; i < loader->thread_count; ++i) {
    (void)vkr_thread_join(loader->threads[i]);
    (void)vkr_thread_destroy(loader->allocator, &loader->threads[i]);
  }
  for (ServerLoad *load = loader->head; load;) {
    ServerLoad *next = load->next;
    if (load->abandoned) {
      free(load);
    }
    load = next;
  }
  if (loader->work) {
    (void)vkr_cond_destroy(loader->allocator, &loader->work);
  }
  if (loader->mutex) {
    (void)vkr_mutex_destroy(loader->allocator, &loader->mutex);
  }
  free(loader);
}

static ServerLoader *server_loader_create(VkrAllocator *allocator,
                                          const VkrDepotStore *store,
                                          const VkrDepotServerConfig *config) {
  ServerLoader *loader = calloc(1u, sizeof(ServerLoader));
  if (!loader) {
    return NULL;
  }
  loader->allocator = allocator;
  loader->store = store;
  loader->wake = config->wake;
  loader->wake_context = config->wake_context;
  if (!vkr_mutex_create(allocator, &loader->mutex) ||
      !vkr_cond_create(allocator, &loader->work)) {
    server_loader_destroy(loader);
    return NULL;
  }
  const uint32_t count =
      Min(config->load_threads, (uint32_t)VKR_DEPOT_SERVER_LOAD_THREADS_MAX);
  for (uint32_t i = 0u; i < count; ++i) {
    if (!vkr_thread_create(allocator, &loader->threads[i], server_load_thread,
                           loader)) {
      server_loader_destroy(loader);
      return NULL;
    }
    loader->thread_count += 1u;
  }
  return loader;
}

/* Gives the loaders the objects at the front of the peer's queue that none
   has taken yet. */
static void server_load_ahead(VkrDepotServer *server, ServerPeer *peer) {
  ServerLoader *loader = server->loader;
  const uint32_t ahead = Min(peer->queue_count, SERVER_LOAD_AHEAD);
  bool8_t queued = false_v;
  for (uint32_t i = 0u; i < ahead; ++i) {
    ServerSendItem *item =
        &peer->queue[(peer->queue_head + i) % peer->queue_capacity];
    if (item->load) {
      continue;
    }
    ServerLoad *load = calloc(1u, sizeof(ServerLoad));
    if (!load) {
      /* The item reads on the host thread when it is sent. */
      break;
    }
    load->id = item->id;
    load->zstd = item->zstd;
    item->load = load;
    if (!queued) {
      vkr_mutex_lock(loader->mutex);
      queued = true_v;
    }
    if (loader->tail) {
      loader->tail->next = load;
    } else {
      loader->head = load;
    }
    loader->tail = load;
  }
  if (queued) {
    vkr_cond_broadcast(loader->work);
    vkr_mutex_unlock(loader->mutex);
  }
}

/* Lets go of an item's load: freed now when its loader is done or gone,
   else by the loader when it is. */
static void server_load_release(ServerLoader *loader, ServerLoad *load) {
  if (!load) {
    return;
  }
  if (!loader) {
    free(load->stored);
    free(load);
    return;
  }
  vkr_mutex_lock(loader->mutex);
  const bool8_t waiting =
      load->state == SERVER_LOAD_WAITING && !loader->stopping;
  if (waiting) {
    load->abandoned = true_v;
  }
  vkr_mutex_unlock(loader->mutex);
  if (!waiting) {
    free(load->stored);
    free(load);
  }
}

static void server_peer_release(ServerLoader *loader, ServerPeer *peer) {
  for (uint32_t i = 0u; i < peer->queue_count; ++i) {
    server_load_release(
        loader,
        peer->queue[(peer->queue_head + i) % peer->queue_capacity].load);
  }
  free(peer->queue);
  for (uint32_t i = 0u; i < peer->orphan_count; ++i) {
    free(peer->orphans[i].bytes);
  }
  free(peer->orphans);
  MemZero(peer, sizeof(*peer));
}

/* Forgets the uploads a peer started and the bytes that overtook them. */
static void server_peer_drop_uploads(ServerPeer *peer) {
  for (uint32_t i = 0u; i < peer->orphan_count; ++i) {
    free(peer->orphans[i].bytes);
  }
  peer->orphan_count = 0u;
  peer->orphan_bytes = 0u;
  MemZero(peer->puts, sizeof(peer->puts));
}

static bool8_t server_queue_push(ServerPeer *peer, ServerSendItem item) {
  if (peer->queue_count >= SERVER_QUEUE_MAX) {
    return false_v;
  }
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
        /* The role as the access list stands now, which may have changed
           since the handshake was accepted. */
        const uint32_t role = server_role(server, event->peer_key);
        if (role == VKR_DEPOT_ROLE_NONE) {
          vkr_net_core_close(server->core, event->connection,
                             VKR_NET_CLOSE_ACCESS_DENIED, 0u);
          return;
        }
        peer->live = true_v;
        peer->connection = event->connection;
        memcpy(peer->key, event->peer_key, 32u);
        peer->role = role;
        return;
      }
    }
    vkr_net_core_close(server->core, event->connection,
                       VKR_NET_CLOSE_SERVER_FULL, 0u);
  } else if (event->type == VKR_NET_EVENT_CLOSED) {
    ServerPeer *peer = server_peer(server, event->connection);
    if (peer) {
      server_peer_release(server->loader, peer);
    }
  }
}

static ServerRequest *server_request_find(VkrDepotServer *server,
                                          const uint8_t key[32]) {
  for (uint32_t i = 0u; i < VKR_DEPOT_REQUESTS_MAX; ++i) {
    ServerRequest *slot = &server->requests[i];
    if (slot->live && MemCompare(slot->request.key, key, 32u) == 0) {
      return slot;
    }
  }
  return NULL;
}

static bool8_t server_same_host(const VkrNetAddress *a,
                                const VkrNetAddress *b) {
  VkrNetAddress x = *a;
  VkrNetAddress y = *b;
  x.port = 0u;
  y.port = 0u;
  return vkr_net_address_equal(&x, &y);
}

static int32_t server_denied_find(const VkrDepotServer *server,
                                  const uint8_t key[32]) {
  for (uint32_t i = 0u; i < server->denied_count; ++i) {
    if (MemCompare(server->denied[i], key, 32u) == 0) {
      return (int32_t)i;
    }
  }
  return -1;
}

static void server_denied_set(VkrDepotServer *server, const uint8_t key[32],
                              bool8_t denied) {
  const int32_t at = server_denied_find(server, key);
  if (denied && at < 0) {
    const uint32_t slot = server->denied_count < SERVER_DENIED_MAX
                              ? server->denied_count++
                              : server->denied_next++ % SERVER_DENIED_MAX;
    MemCopy(server->denied[slot], key, 32u);
  } else if (!denied && at >= 0) {
    server->denied_count -= 1u;
    MemCopy(server->denied[at], server->denied[server->denied_count], 32u);
  }
}

/* Records a refused key's try. A new key takes a free slot or the oldest
   one, the oldest of its own address once that address holds
   SERVER_REQUESTS_PER_ADDRESS. */
static void server_request_note(VkrDepotServer *server, const uint8_t key[32],
                                const char *name,
                                const VkrNetAddress *address) {
  ServerRequest *slot = server_request_find(server, key);
  if (!slot) {
    ServerRequest *oldest = NULL;
    ServerRequest *oldest_here = NULL;
    uint32_t here = 0u;
    for (uint32_t i = 0u; i < VKR_DEPOT_REQUESTS_MAX; ++i) {
      ServerRequest *candidate = &server->requests[i];
      if (!candidate->live) {
        slot = slot ? slot : candidate;
        continue;
      }
      if (!oldest || candidate->request.time_us < oldest->request.time_us) {
        oldest = candidate;
      }
      if (server_same_host(&candidate->request.address, address)) {
        here += 1u;
        if (!oldest_here ||
            candidate->request.time_us < oldest_here->request.time_us) {
          oldest_here = candidate;
        }
      }
    }
    if (here >= SERVER_REQUESTS_PER_ADDRESS) {
      slot = oldest_here;
    } else if (!slot) {
      slot = oldest;
    }
    MemZero(slot, sizeof(*slot));
    slot->live = true_v;
    MemCopy(slot->request.key, key, 32u);
  }
  snprintf(slot->request.name, sizeof(slot->request.name), "%s", name);
  slot->request.address = *address;
  slot->request.time_us = server_unix_us();
  slot->request.tries += 1u;
}

void vkr_depot_server_accept(void *context, const VkrNetAddress *address,
                             const uint8_t peer_key[32],
                             const uint8_t *credential,
                             uint32_t credential_size,
                             VkrNetAcceptResult *out_result) {
  VkrDepotServer *server = context;
  const uint32_t role = server_role(server, peer_key);
  if (role == VKR_DEPOT_ROLE_NONE) {
    /* A key its owner refused asks no more until the server restarts. */
    if (server_denied_find(server, peer_key) < 0) {
      char name[VKR_DEPOT_DISPLAY_NAME];
      (void)vkr_depot_display_name_clean(credential, credential_size, name);
      server_request_note(server, peer_key, name, address);
    }
    out_result->code = VKR_NET_CLOSE_ACCESS_DENIED;
    return;
  }
  out_result->user = role;
}

uint32_t vkr_depot_server_requests(const VkrDepotServer *server,
                                   VkrDepotAccessRequest *out,
                                   uint32_t capacity) {
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < VKR_DEPOT_REQUESTS_MAX && count < capacity; ++i) {
    const ServerRequest *slot = &server->requests[i];
    if (!slot->live) {
      continue;
    }
    /* Insertion by time, newest first. */
    uint32_t at = count;
    while (at > 0u && out[at - 1u].time_us < slot->request.time_us) {
      out[at] = out[at - 1u];
      at -= 1u;
    }
    out[at] = slot->request;
    count += 1u;
  }
  return count;
}

uint32_t vkr_depot_server_access(const VkrDepotServer *server,
                                 VkrDepotAccessEntry *out, uint32_t capacity) {
  const uint32_t count = Min(server->access_count, capacity);
  for (uint32_t i = 0u; i < count; ++i) {
    MemCopy(out[i].key, server->access[i].key, 32u);
    out[i].role = (VkrDepotRole)server->access[i].role;
  }
  return count;
}

/* Gives every live connection its key's current role and closes the ones
   left without one; drops requests of keys that now have a role. */
static void server_apply_access(VkrDepotServer *server, uint64_t now_us) {
  for (uint32_t i = 0u; i < SERVER_PEER_MAX; ++i) {
    ServerPeer *peer = &server->peers[i];
    if (!peer->live) {
      continue;
    }
    const uint32_t role = server_role(server, peer->key);
    if (role == VKR_DEPOT_ROLE_NONE && peer->role != VKR_DEPOT_ROLE_NONE) {
      vkr_net_core_close(server->core, peer->connection,
                         VKR_NET_CLOSE_ACCESS_DENIED, now_us);
    }
    /* A key that may no longer write finishes no upload it started. */
    if (role < VKR_DEPOT_ROLE_WRITE) {
      server_peer_drop_uploads(peer);
    }
    peer->role = role;
  }
  for (uint32_t i = 0u; i < VKR_DEPOT_REQUESTS_MAX; ++i) {
    ServerRequest *slot = &server->requests[i];
    if (slot->live &&
        server_role(server, slot->request.key) != VKR_DEPOT_ROLE_NONE) {
      MemZero(slot, sizeof(*slot));
    }
  }
}

bool8_t vkr_depot_server_set_role(VkrDepotServer *server, const uint8_t key[32],
                                  VkrDepotRole role, uint64_t now_us) {
  if ((uint32_t)role > VKR_DEPOT_ROLE_ADMIN ||
      !vkr_depot_access_set(server->store.root, key, role) ||
      !server_access_load(server)) {
    return false_v;
  }
  server_denied_set(server, key, role == VKR_DEPOT_ROLE_NONE);
  ServerRequest *slot = server_request_find(server, key);
  if (slot) {
    MemZero(slot, sizeof(*slot));
  }
  server_apply_access(server, now_us);
  return true_v;
}

bool8_t vkr_depot_server_reload_access(VkrDepotServer *server,
                                       uint64_t now_us) {
  if (!server_access_load(server)) {
    return false_v;
  }
  server_apply_access(server, now_us);
  return true_v;
}

void vkr_depot_server_close_peers(VkrDepotServer *server, uint16_t code,
                                  uint64_t now_us) {
  for (uint32_t i = 0u; i < SERVER_PEER_MAX; ++i) {
    if (server->peers[i].live) {
      vkr_net_core_close(server->core, server->peers[i].connection, code,
                         now_us);
    }
  }
}

VkrDepotServerStats vkr_depot_server_stats(const VkrDepotServer *server) {
  VkrDepotServerStats stats = server->stats;
  stats.peers = 0u;
  for (uint32_t i = 0u; i < SERVER_PEER_MAX; ++i) {
    stats.peers += server->peers[i].live ? 1u : 0u;
  }
  return stats;
}

// =============================================================================
// Requests
// =============================================================================

static bool8_t server_ref_name_visit(void *context, const char *name) {
  RefNames *list = context;
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
  RefNames *names = &server->ref_names;
  names->count = 0u;
  /* More than VKR_DEPOT_REFS_MAX branches: the first ones answer. */
  (void)vkr_depot_ref_list(server->store.root, server_ref_name_visit, names);
  for (uint32_t i = 0u; i < names->count; ++i) {
    VkrDepotId commit;
    if (!vkr_depot_ref_read(server->store.root, names->names[i], &commit)) {
      continue;
    }
    VkrDepotRefWire *ref = &reply->refs[reply->ref_count++];
    snprintf(ref->name, sizeof(ref->name), "%s", names->names[i]);
    memcpy(ref->commit, commit.bytes, VKR_DEPOT_ID_SIZE);
  }
  (void)vkr_depot_send(server->session, peer->connection, VKR_DEPOT_MSG_REFS,
                       reply, now);
}

static bool8_t server_default_branch_visit(void *context, const char *branch) {
  char *out = context;
  if (!out[0] || strcmp(branch, "main") == 0) {
    snprintf(out, VKR_DEPOT_BRANCH_NAME, "%s", branch);
  }
  return true_v;
}

/* Selects the repository a version 2 connection works on. The server
   serves one store, the default: a HELLO that names none selects it. A
   store gets its ID with the repository file (proposal "Repository
   layout"); until then a HELLO that names an ID finds nothing. A failed
   HELLO may be retried; one after a selection breaks the protocol. */
static void server_on_hello(VkrDepotServer *server, ServerPeer *peer,
                            const VkrDepotHelloWire *hello, uint64_t now) {
  if (peer->selected) {
    vkr_net_core_close(server->core, peer->connection,
                       VKR_NET_CLOSE_PROTOCOL_VIOLATION, now);
    return;
  }
  VkrDepotHelloReplyWire *reply = &server->reply.hello_reply;
  MemZero(reply, sizeof(*reply));
  reply->request = hello->request;
  bool8_t named = false_v;
  for (uint32_t i = 0u; i < hello->repository_size; ++i) {
    named = named || hello->repository[i] != 0u;
  }
  peer->outbound_only = (hello->flags & VKR_DEPOT_HELLO_OUTBOUND_ONLY) != 0u;
  if (named) {
    reply->status = VKR_DEPOT_HELLO_UNKNOWN_REPOSITORY;
  } else {
    reply->status = VKR_DEPOT_HELLO_OK;
    peer->selected = true_v;
    snprintf(reply->name, sizeof(reply->name), "%s",
             vkr_bakery_path_name(server->store.root));
    (void)vkr_depot_ref_list(server->store.root, server_default_branch_visit,
                             reply->default_branch);
  }
  (void)vkr_depot_send(server->session, peer->connection,
                       VKR_DEPOT_MSG_HELLO_REPLY, reply, now);
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
  if (!vkr_depot_store_put_stored(&server->store, &put->ids[index], bytes, size,
                                  NULL, NULL)) {
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
      peer->orphan_bytes -= orphan->size;
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
      /* On the heap: the check holds a path buffer. */
      ClosureCheck *check = calloc(1u, sizeof(ClosureCheck));
      if (check) {
        check->store = &server->store;
      }
      const bool8_t complete =
          check &&
          closure_tree(check, &commit.tree,
                       have_previous ? &previous.tree : NULL, 0u) &&
          !check->incomplete && !check->invalid;
      if (!complete) {
        reply->status = check && check->invalid ? VKR_DEPOT_REF_INVALID
                                                : VKR_DEPOT_REF_INCOMPLETE;
      } else {
        reply->status = VKR_DEPOT_REF_OK;
        for (uint32_t i = 0u; i < check->changed.count; ++i) {
          const ServerLock *lock =
              server_lock_find(server, check->changed.entries[i].path);
          if (lock && memcmp(lock->owner, peer->key, 32u) != 0) {
            reply->status = VKR_DEPOT_REF_LOCKED;
            memcpy(reply->current, current.bytes, VKR_DEPOT_ID_SIZE);
            break;
          }
        }
        if (reply->status == VKR_DEPOT_REF_OK) {
          /* A working copy that shares the store may have moved the
             branch since it was read: the swap checks again under the
             store's lock. */
          VkrDepotId after;
          const VkrDepotRefSwap swap = vkr_depot_ref_swap(
              server->store.root, update->name, &expected, &commit_id, &after);
          memcpy(reply->current, after.bytes, VKR_DEPOT_ID_SIZE);
          if (swap == VKR_DEPOT_REF_SWAP_MOVED) {
            reply->status = VKR_DEPOT_REF_CONFLICT;
          } else if (swap != VKR_DEPOT_REF_SWAP_OK) {
            reply->status = VKR_DEPOT_REF_DENIED;
          }
        }
      }
      if (check) {
        vkr_depot_file_list_free(&check->changed);
      }
      free(check);
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
    if (request->after[0] &&
        server_path_compare(lock->path, request->after) <= 0) {
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
      peer->orphan_bytes + event->size > SERVER_ORPHAN_BYTES_MAX ||
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
  peer->orphan_bytes += event->size;
}

static void server_service_event(void *context, VkrNetConnectionId connection,
                                 uint8_t channel, const VkrNetEvent *event,
                                 uint64_t now) {
  VkrDepotServer *server = context;
  ServerPeer *peer = server_peer(server, connection);
  /* A connection whose key lost its role is closing: it gets nothing. */
  if (!peer || peer->role == VKR_DEPOT_ROLE_NONE ||
      event->type != VKR_NET_EVENT_MESSAGE) {
    return;
  }
  if (channel == VKR_DEPOT_DATA_CHANNEL) {
    server->stats.bytes_in += event->size;
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
  case VKR_DEPOT_MSG_HELLO:
    if (peer->version < VKR_DEPOT_SERVICE_VERSION) {
      vkr_net_core_close(server->core, connection,
                         VKR_NET_CLOSE_PROTOCOL_VIOLATION, now);
      break;
    }
    server_on_hello(server, peer, &message->hello, now);
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
  VkrDepotServer *server = context;
  ServerPeer *peer = server_peer(server, connection);
  if (!peer || peer->role < VKR_DEPOT_ROLE_READ) {
    return (uint16_t)VKR_NET_CLOSE_ACCESS_DENIED;
  }
  peer->version = version;
  return 0u;
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
  if (config->load_threads > 0u) {
    server->loader = server_loader_create(allocator, &server->store, config);
    if (!server->loader) {
      vkr_depot_server_destroy(server);
      return NULL;
    }
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
  /* Stopped loaders leave every load they did not finish to its item. */
  server_loader_destroy(server->loader);
  for (uint32_t i = 0u; i < SERVER_PEER_MAX; ++i) {
    if (server->peers[i].live) {
      server_peer_release(NULL, &server->peers[i]);
    }
  }
  for (uint32_t i = 0u; i < server->lock_count; ++i) {
    free(server->locks[i].path);
  }
  free(server->locks);
  free(server);
}

/* Sends one queued item, or drops it when its object is gone or torn
   (the client times out on it). False, with the item kept, when the data
   channel is full or a loader has not read it yet. */
static bool8_t server_send_item(VkrDepotServer *server, ServerPeer *peer,
                                ServerSendItem *item, uint64_t now) {
  uint8_t *stored = NULL;
  uint64_t size = 0u;
  bool8_t found = false_v;
  ServerLoad *load = item->load;
  if (load) {
    vkr_mutex_lock(server->loader->mutex);
    const uint8_t state = load->state;
    vkr_mutex_unlock(server->loader->mutex);
    if (state == SERVER_LOAD_WAITING) {
      return false_v;
    }
    found = state == SERVER_LOAD_READY;
    stored = load->stored;
    size = load->size;
  } else {
    found = server_read_object(&server->store, &item->id, item->zstd, &stored,
                               &size);
  }
  bool8_t sent = true_v;
  if (found) {
    const VkrNetSendOptions options = {
        .flags = VKR_NET_SEND_TAG,
        .tag = vkr_depot_tag(item->request, item->index)};
    const VkrNetSendStatus status = vkr_net_session_send(
        server->session, peer->connection, VKR_NET_SERVICE_DEPOT,
        VKR_DEPOT_DATA_CHANNEL, stored, (uint32_t)size, &options, now);
    if (status == VKR_NET_SEND_OK) {
      server->stats.bytes_out += size;
    }
    sent = status != VKR_NET_SEND_QUEUE_FULL;
  }
  if (load) {
    /* A full channel keeps the bytes for the next try. */
    if (sent) {
      free(load->stored);
      free(load);
      item->load = NULL;
    }
  } else {
    free(stored);
  }
  return sent;
}

void vkr_depot_server_update(VkrDepotServer *server, uint64_t now_us) {
  /* `vkr_depot access` or another process changed the access file. */
  if (now_us - server->access_checked_us >= SERVER_ACCESS_POLL_US) {
    server->access_checked_us = now_us;
    bool8_t exists = false_v;
    int64_t mtime_ns = 0;
    uint64_t size = 0u;
    server_access_stat(server, &exists, &mtime_ns, &size);
    if (exists != server->access_exists ||
        mtime_ns != server->access_mtime_ns || size != server->access_size) {
      (void)vkr_depot_server_reload_access(server, now_us);
    }
  }
  for (uint32_t i = 0u; i < SERVER_PEER_MAX; ++i) {
    ServerPeer *peer = &server->peers[i];
    if (!peer->live) {
      continue;
    }
    if (server->loader) {
      server_load_ahead(server, peer);
    }
    while (peer->queue_count > 0u) {
      ServerSendItem *item = &peer->queue[peer->queue_head];
      if (!server_send_item(server, peer, item, now_us)) {
        break;
      }
      peer->queue_head = (peer->queue_head + 1u) % peer->queue_capacity;
      peer->queue_count -= 1u;
    }
  }
  vkr_net_core_flush(server->core);
}
