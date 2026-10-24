#include "vkr_depot_workspace.h"

#include "core/vkr_byte_io.h"
#include "filesystem/filesystem.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_repository.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKSPACE_STATE_MAGIC 0x57444b56u /* "VKDW" */
#define WORKSPACE_STATE_VERSION 1u
/* A sparse working copy's state: version 1 plus a flags word, which a
   program that predates sparse copies refuses instead of reading its
   unfetched files as deleted. */
#define WORKSPACE_STATE_VERSION_SPARSE 2u
/* Since repositories (ADR-133): a flags word, the branch, the base and the
   index. The server a working copy syncs with is the remote `origin` in its
   store; versions 1 and 2 also held it, and opening one adopts it. */
#define WORKSPACE_STATE_VERSION_3 3u
#define WORKSPACE_STATE_FLAG_SPARSE 1u
/* `.vkrdepot/cooked`: the cooked files last hashed or written, so a cooked
   snapshot hashes only files whose size or time changed. */
#define WORKSPACE_COOKED_MAGIC 0x43444b56u /* "VKDC" */

// =============================================================================
// File lists
// =============================================================================

void vkr_depot_file_list_free(VkrDepotFileList *list) {
  for (uint32_t i = 0u; i < list->count; ++i) {
    free(list->entries[i].path);
  }
  free(list->entries);
  MemZero(list, sizeof(*list));
}

bool8_t vkr_depot_file_list_add(VkrDepotFileList *list, const char *path,
                                uint64_t size, int64_t mtime_ns,
                                const VkrDepotId *blob) {
  if (list->count == list->capacity) {
    const uint32_t capacity = list->capacity ? list->capacity * 2u : 64u;
    VkrDepotIndexEntry *entries =
        realloc(list->entries, (size_t)capacity * sizeof(VkrDepotIndexEntry));
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
  memcpy(copy, path, length + 1u);
  list->entries[list->count++] = (VkrDepotIndexEntry){
      .path = copy, .size = size, .mtime_ns = mtime_ns, .blob = *blob};
  return true_v;
}

static int file_entry_compare(const void *lhs, const void *rhs) {
  return strcmp(((const VkrDepotIndexEntry *)lhs)->path,
                ((const VkrDepotIndexEntry *)rhs)->path);
}

void vkr_depot_file_list_sort(VkrDepotFileList *list) {
  qsort(list->entries, list->count, sizeof(VkrDepotIndexEntry),
        file_entry_compare);
}

const VkrDepotIndexEntry *vkr_depot_file_list_find(const VkrDepotFileList *list,
                                                   const char *path) {
  const VkrDepotIndexEntry key = {.path = (char *)path};
  return bsearch(&key, list->entries, list->count, sizeof(VkrDepotIndexEntry),
                 file_entry_compare);
}

bool8_t vkr_depot_file_list_diff(const VkrDepotFileList *from,
                                 const VkrDepotFileList *to,
                                 VkrDepotChangeVisitor visit, void *context) {
  uint32_t i = 0u;
  uint32_t j = 0u;
  while (i < from->count || j < to->count) {
    const int order = i == from->count ? 1
                      : j == to->count
                          ? -1
                          : strcmp(from->entries[i].path, to->entries[j].path);
    if (order < 0) {
      if (!visit(context, from->entries[i].path, VKR_DEPOT_CHANGE_DELETED)) {
        return false_v;
      }
      ++i;
    } else if (order > 0) {
      if (!visit(context, to->entries[j].path, VKR_DEPOT_CHANGE_ADDED)) {
        return false_v;
      }
      ++j;
    } else {
      if (!vkr_depot_id_equal(&from->entries[i].blob, &to->entries[j].blob) &&
          !visit(context, to->entries[j].path, VKR_DEPOT_CHANGE_MODIFIED)) {
        return false_v;
      }
      ++i;
      ++j;
    }
  }
  return true_v;
}

// =============================================================================
// State file
// =============================================================================

static bool8_t workspace_meta_path(const VkrDepotWorkspace *workspace,
                                   const char *name, char *out,
                                   uint32_t capacity) {
  char meta[VKR_DEPOT_PATH_CAPACITY];
  return vkr_bakery_path_join(meta, sizeof(meta), workspace->root,
                              VKR_DEPOT_META_DIR) &&
         (!name || vkr_bakery_path_join(out, capacity, meta, name)) &&
         (name || snprintf(out, capacity, "%s", meta) < (int)capacity);
}

bool8_t vkr_depot_cooked_cache_save(const VkrDepotWorkspace *workspace,
                                    const VkrDepotFileList *files) {
  uint64_t size = 8u;
  for (uint32_t i = 0u; i < files->count; ++i) {
    size += 2u + strlen(files->entries[i].path) + 16u + VKR_DEPOT_ID_SIZE;
  }
  uint8_t *bytes = malloc(size);
  if (!bytes) {
    return false_v;
  }
  uint8_t *cursor = bytes;
  vkr_store_le_u32(cursor, WORKSPACE_COOKED_MAGIC);
  vkr_store_le_u32(cursor + 4, files->count);
  cursor += 8u;
  for (uint32_t i = 0u; i < files->count; ++i) {
    const VkrDepotIndexEntry *entry = &files->entries[i];
    const uint16_t path_size = (uint16_t)strlen(entry->path);
    cursor[0] = (uint8_t)path_size;
    cursor[1] = (uint8_t)(path_size >> 8);
    memcpy(cursor + 2u, entry->path, path_size);
    cursor += 2u + path_size;
    vkr_store_le_u64(cursor, entry->size);
    vkr_store_le_u64(cursor + 8, (uint64_t)entry->mtime_ns);
    cursor += 16u;
    memcpy(cursor, entry->blob.bytes, VKR_DEPOT_ID_SIZE);
    cursor += VKR_DEPOT_ID_SIZE;
  }
  char path[VKR_DEPOT_PATH_CAPACITY];
  const bool8_t ok =
      workspace_meta_path(workspace, "cooked", path, sizeof(path)) &&
      vkr_bakery_write_file_atomic(path, bytes, size);
  free(bytes);
  return ok;
}

bool8_t vkr_depot_cooked_cache_load(const VkrDepotWorkspace *workspace,
                                    VkrDepotFileList *out_files) {
  MemZero(out_files, sizeof(*out_files));
  char path[VKR_DEPOT_PATH_CAPACITY];
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!workspace_meta_path(workspace, "cooked", path, sizeof(path)) ||
      !vkr_bakery_is_file(path)) {
    return true_v;
  }
  if (!vkr_bakery_read_file(path, 1ull << 31, &bytes, &size)) {
    return false_v;
  }
  const uint8_t *cursor = bytes;
  const uint8_t *end = bytes + size;
  bool8_t ok = size >= 8u && vkr_load_le_u32(cursor) == WORKSPACE_COOKED_MAGIC;
  const uint32_t count = ok ? vkr_load_le_u32(cursor + 4) : 0u;
  cursor += ok ? 8u : 0u;
  for (uint32_t i = 0u; ok && i < count; ++i) {
    ok = end - cursor >= 2;
    const uint16_t length =
        ok ? (uint16_t)(cursor[0] | (cursor[1] << 8)) : (uint16_t)0u;
    char entry_path[VKR_DEPOT_PATH_CAPACITY];
    ok = ok && end - cursor >= 2 + (int64_t)length + 16 + VKR_DEPOT_ID_SIZE &&
         length < sizeof(entry_path);
    if (!ok) {
      break;
    }
    memcpy(entry_path, cursor + 2u, length);
    entry_path[length] = '\0';
    cursor += 2u + length;
    VkrDepotId blob;
    const uint64_t file_size = vkr_load_le_u64(cursor);
    const int64_t mtime = (int64_t)vkr_load_le_u64(cursor + 8);
    memcpy(blob.bytes, cursor + 16u, VKR_DEPOT_ID_SIZE);
    cursor += 16u + VKR_DEPOT_ID_SIZE;
    ok =
        !vkr_depot_path_valid(entry_path) ||
        vkr_depot_file_list_add(out_files, entry_path, file_size, mtime, &blob);
  }
  free(bytes);
  /* A torn or older cache only costs hashing again. */
  if (!ok || cursor != end) {
    vkr_depot_file_list_free(out_files);
    return true_v;
  }
  vkr_depot_file_list_sort(out_files);
  return true_v;
}

/* The server a state of version 1 or 2 named; empty for none. */
typedef struct WorkspaceLegacyServer {
  char address[VKR_DEPOT_REMOTE_MAX];
  uint8_t key[32];
} WorkspaceLegacyServer;

/* Gives the store what the working copy's state cannot hold (ADR-133,
   "Working copies adopt the model on open"): the branch at the base and
   origin's record of that branch, and, from a state of version 1 or 2, the
   server it synced with as the remote `origin`. A working copy whose
   server is its store's own, such as one beside the server sharing its
   store, is that repository and gets no `origin`. Runs on every open and
   save; a store that cannot take the files only loses the new commands. */
static void workspace_adopt_repository(const VkrDepotWorkspace *workspace,
                                       const WorkspaceLegacyServer *legacy) {
  const char *store = workspace->store.root;
  uint8_t id[VKR_DEPOT_REPOSITORY_ID_SIZE];
  (void)vkr_depot_repository_id(store, id, true_v);

  uint8_t served[32];
  const bool8_t self = legacy && legacy->address[0] &&
                       vkr_depot_repository_served_key(store, served) &&
                       MemCompare(served, legacy->key, 32u) == 0;
  VkrDepotRemoteEntry origin = {0};
  bool8_t has_origin = vkr_depot_remote_find(store, VKR_DEPOT_ORIGIN, &origin);
  if (legacy && legacy->address[0] && !self &&
      (!has_origin || strcmp(origin.address, legacy->address) != 0 ||
       MemCompare(origin.key, legacy->key, 32u) != 0)) {
    if (!has_origin || MemCompare(origin.key, legacy->key, 32u)) {
      MemZero(&origin, sizeof(origin));
    }
    snprintf(origin.name, sizeof(origin.name), "%s", VKR_DEPOT_ORIGIN);
    snprintf(origin.address, sizeof(origin.address), "%s", legacy->address);
    MemCopy(origin.key, legacy->key, 32u);
    has_origin = vkr_depot_remote_set(store, &origin);
  }

  if (vkr_depot_id_zero(&workspace->base)) {
    return;
  }
  VkrDepotId commit;
  const VkrDepotId zero = {0};
  if (!vkr_depot_ref_read(store, workspace->branch, &commit)) {
    (void)vkr_depot_ref_swap(store, workspace->branch, &zero, &workspace->base,
                             NULL);
  }
  if (has_origin && !self &&
      !vkr_depot_tracking_read(store, VKR_DEPOT_ORIGIN, workspace->branch,
                               &commit)) {
    (void)vkr_depot_tracking_set(store, VKR_DEPOT_ORIGIN, workspace->branch,
                                 &workspace->base);
  }
}

bool8_t vkr_depot_workspace_save(const VkrDepotWorkspace *workspace) {
  uint64_t size =
      4u + 4u + 4u + 2u + strlen(workspace->branch) + VKR_DEPOT_ID_SIZE + 4u;
  for (uint32_t i = 0u; i < workspace->index.count; ++i) {
    size += 2u + strlen(workspace->index.entries[i].path) + 8u + 8u +
            VKR_DEPOT_ID_SIZE;
  }
  uint8_t *bytes = malloc(size);
  if (!bytes) {
    return false_v;
  }
  uint8_t *cursor = bytes;
  vkr_store_le_u32(cursor, WORKSPACE_STATE_MAGIC);
  vkr_store_le_u32(cursor + 4, WORKSPACE_STATE_VERSION_3);
  vkr_store_le_u32(cursor + 8,
                   workspace->sparse ? WORKSPACE_STATE_FLAG_SPARSE : 0u);
  cursor += 12u;
  const uint16_t branch_size = (uint16_t)strlen(workspace->branch);
  cursor[0] = (uint8_t)branch_size;
  cursor[1] = (uint8_t)(branch_size >> 8);
  memcpy(cursor + 2u, workspace->branch, branch_size);
  cursor += 2u + branch_size;
  memcpy(cursor, workspace->base.bytes, VKR_DEPOT_ID_SIZE);
  cursor += VKR_DEPOT_ID_SIZE;
  vkr_store_le_u32(cursor, workspace->index.count);
  cursor += 4u;
  for (uint32_t i = 0u; i < workspace->index.count; ++i) {
    const VkrDepotIndexEntry *entry = &workspace->index.entries[i];
    const uint16_t path_size = (uint16_t)strlen(entry->path);
    cursor[0] = (uint8_t)path_size;
    cursor[1] = (uint8_t)(path_size >> 8);
    memcpy(cursor + 2u, entry->path, path_size);
    cursor += 2u + path_size;
    vkr_store_le_u64(cursor, entry->size);
    vkr_store_le_u64(cursor + 8, (uint64_t)entry->mtime_ns);
    cursor += 16u;
    memcpy(cursor, entry->blob.bytes, VKR_DEPOT_ID_SIZE);
    cursor += VKR_DEPOT_ID_SIZE;
  }
  char path[VKR_DEPOT_PATH_CAPACITY];
  const bool8_t ok =
      workspace_meta_path(workspace, "state", path, sizeof(path)) &&
      vkr_bakery_write_file_atomic(path, bytes, size);
  free(bytes);
  if (ok) {
    workspace_adopt_repository(workspace, NULL);
  }
  return ok;
}

static bool8_t workspace_load_state(VkrDepotWorkspace *workspace,
                                    WorkspaceLegacyServer *out_legacy) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!workspace_meta_path(workspace, "state", path, sizeof(path)) ||
      !vkr_bakery_read_file(path, 1ull << 31, &bytes, &size)) {
    return false_v;
  }
  bool8_t ok = false_v;
  const uint8_t *cursor = bytes;
  const uint8_t *end = bytes + size;
  uint32_t count = 0u;
  uint16_t length = 0u;
  uint32_t version = 0u;
  if (size < 8u || vkr_load_le_u32(cursor) != WORKSPACE_STATE_MAGIC) {
    goto cleanup;
  }
  version = vkr_load_le_u32(cursor + 4);
  if (version != WORKSPACE_STATE_VERSION &&
      version != WORKSPACE_STATE_VERSION_SPARSE &&
      version != WORKSPACE_STATE_VERSION_3) {
    goto cleanup;
  }
  cursor += 8u;
  if (version != WORKSPACE_STATE_VERSION) {
    if (end - cursor < 4) {
      goto cleanup;
    }
    workspace->sparse =
        (vkr_load_le_u32(cursor) & WORKSPACE_STATE_FLAG_SPARSE) != 0u;
    cursor += 4u;
  }
  if (version != WORKSPACE_STATE_VERSION_3) {
    if (end - cursor < 2) {
      goto cleanup;
    }
    length = (uint16_t)(cursor[0] | (cursor[1] << 8));
    cursor += 2u;
    if (length >= VKR_DEPOT_REMOTE_MAX || end - cursor < (int64_t)length + 32) {
      goto cleanup;
    }
    memcpy(out_legacy->address, cursor, length);
    out_legacy->address[length] = '\0';
    cursor += length;
    memcpy(out_legacy->key, cursor, 32u);
    cursor += 32u;
  }
  if (end - cursor < 2) {
    goto cleanup;
  }
  length = (uint16_t)(cursor[0] | (cursor[1] << 8));
  cursor += 2u;
  if (length >= VKR_DEPOT_BRANCH_NAME ||
      end - cursor < (int64_t)length + VKR_DEPOT_ID_SIZE + 4) {
    goto cleanup;
  }
  memcpy(workspace->branch, cursor, length);
  workspace->branch[length] = '\0';
  cursor += length;
  memcpy(workspace->base.bytes, cursor, VKR_DEPOT_ID_SIZE);
  cursor += VKR_DEPOT_ID_SIZE;
  count = vkr_load_le_u32(cursor);
  cursor += 4u;
  for (uint32_t i = 0u; i < count; ++i) {
    if (end - cursor < 2) {
      goto cleanup;
    }
    length = (uint16_t)(cursor[0] | (cursor[1] << 8));
    cursor += 2u;
    if (end - cursor < (int64_t)length + 16 + VKR_DEPOT_ID_SIZE) {
      goto cleanup;
    }
    char entry_path[VKR_DEPOT_PATH_CAPACITY];
    if (length >= sizeof(entry_path)) {
      goto cleanup;
    }
    memcpy(entry_path, cursor, length);
    entry_path[length] = '\0';
    cursor += length;
    VkrDepotId blob;
    const uint64_t file_size = vkr_load_le_u64(cursor);
    const int64_t mtime = (int64_t)vkr_load_le_u64(cursor + 8);
    cursor += 16u;
    memcpy(blob.bytes, cursor, VKR_DEPOT_ID_SIZE);
    cursor += VKR_DEPOT_ID_SIZE;
    /* A checkout removes tracked files, so a path that leaves the working
       copy (`..`, absolute) or names its state must never be tracked. Such
       an entry, from a planted or older state, drops out: untracked. */
    if (!vkr_depot_path_valid(entry_path)) {
      fprintf(stderr, "depot: not tracking an invalid path: %s\n", entry_path);
      continue;
    }
    if (!vkr_depot_file_list_add(&workspace->index, entry_path, file_size,
                                 mtime, &blob)) {
      goto cleanup;
    }
  }
  vkr_depot_file_list_sort(&workspace->index);
  ok = cursor == end;
cleanup:
  free(bytes);
  return ok;
}

bool8_t vkr_depot_workspace_create(VkrDepotWorkspace *workspace,
                                   const char *root, const char *store_root,
                                   const VkrCodec *codec) {
  MemZero(workspace, sizeof(*workspace));
  if (!vkr_bakery_path_absolute(root, workspace->root,
                                sizeof(workspace->root)) ||
      !vkr_bakery_make_directories(workspace->root)) {
    return false_v;
  }
  char meta[VKR_DEPOT_PATH_CAPACITY];
  char store[VKR_DEPOT_PATH_CAPACITY];
  if (!workspace_meta_path(workspace, NULL, meta, sizeof(meta)) ||
      !vkr_bakery_make_directories(meta)) {
    return false_v;
  }
  if (store_root) {
    snprintf(store, sizeof(store), "%s", store_root);
  } else if (!vkr_bakery_path_join(store, sizeof(store), meta, "store")) {
    return false_v;
  }
  snprintf(workspace->branch, sizeof(workspace->branch), "main");
  if (!vkr_depot_store_open(&workspace->store, store, codec)) {
    return false_v;
  }
  char link[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(link, sizeof(link), meta, "store-root") ||
      !vkr_bakery_write_file_atomic(link, workspace->store.root,
                                    strlen(workspace->store.root))) {
    return false_v;
  }
  return vkr_depot_workspace_save(workspace);
}

bool8_t vkr_depot_workspace_open(VkrDepotWorkspace *workspace, const char *root,
                                 const VkrCodec *codec) {
  MemZero(workspace, sizeof(*workspace));
  char candidate[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_absolute(root, candidate, sizeof(candidate))) {
    return false_v;
  }
  /* The working copy is this directory or the nearest parent with state. */
  for (;;) {
    char meta[VKR_DEPOT_PATH_CAPACITY];
    if (vkr_bakery_path_join(meta, sizeof(meta), candidate,
                             VKR_DEPOT_META_DIR "/state") &&
        vkr_bakery_is_file(meta)) {
      break;
    }
    char parent[VKR_DEPOT_PATH_CAPACITY];
    vkr_bakery_path_parent(parent, sizeof(parent), candidate);
    if (parent[0] == '\0' || strcmp(parent, candidate) == 0) {
      return false_v;
    }
    snprintf(candidate, sizeof(candidate), "%s", parent);
  }
  snprintf(workspace->root, sizeof(workspace->root), "%s", candidate);
  char link[VKR_DEPOT_PATH_CAPACITY];
  uint8_t *store_root = NULL;
  uint64_t store_size = 0u;
  if (!workspace_meta_path(workspace, "store-root", link, sizeof(link)) ||
      !vkr_bakery_read_file(link, VKR_DEPOT_PATH_CAPACITY - 1u, &store_root,
                            &store_size)) {
    return false_v;
  }
  const bool8_t opened =
      vkr_depot_store_open(&workspace->store, (const char *)store_root, codec);
  free(store_root);
  WorkspaceLegacyServer legacy = {0};
  if (!opened || !workspace_load_state(workspace, &legacy)) {
    vkr_depot_workspace_close(workspace);
    return false_v;
  }
  workspace_adopt_repository(workspace, &legacy);
  return true_v;
}

void vkr_depot_workspace_close(VkrDepotWorkspace *workspace) {
  vkr_depot_file_list_free(&workspace->index);
}

// =============================================================================
// Snapshot
// =============================================================================

/* The patterns of `.vkrdepotignore`, pointing into `text`. */
typedef struct DepotIgnore {
  char *text;
  const char *patterns[VKR_DEPOT_IGNORE_MAX];
  uint32_t count;
} DepotIgnore;

/* Steps one pattern may take against one path: a pattern of many stars
   backtracks exponentially, and a pulled `.vkrdepotignore` is another
   machine's text. Past the budget the pattern does not match. */
#define IGNORE_GLOB_STEPS 100000u

/* Glob match of `text` against `pattern`: '*' within a name, '**' across
   names, '?' one byte other than '/'. */
static bool8_t ignore_glob(const char *pattern, const char *text,
                           uint32_t *steps) {
  while (*pattern) {
    if (++*steps > IGNORE_GLOB_STEPS) {
      return false_v;
    }
    if (pattern[0] == '*' && pattern[1] == '*') {
      pattern += 2;
      if (*pattern == '/') {
        ++pattern;
      }
      for (const char *rest = text;; ++rest) {
        if (ignore_glob(pattern, rest, steps)) {
          return true_v;
        }
        if (!*rest || *steps > IGNORE_GLOB_STEPS) {
          return false_v;
        }
      }
    }
    if (*pattern == '*') {
      ++pattern;
      for (const char *rest = text;; ++rest) {
        if (ignore_glob(pattern, rest, steps)) {
          return true_v;
        }
        if (!*rest || *rest == '/' || *steps > IGNORE_GLOB_STEPS) {
          return false_v;
        }
      }
    }
    if (!*text || (*pattern == '?' ? *text == '/' : *pattern != *text)) {
      return false_v;
    }
    ++pattern;
    ++text;
  }
  return *text == '\0';
}

bool8_t vkr_depot_ignore_match(const char *pattern, const char *path,
                               bool8_t is_directory) {
  char body[VKR_DEPOT_PATH_CAPACITY];
  snprintf(body, sizeof(body), "%s", pattern);
  size_t length = strlen(body);
  const bool8_t folder = length > 0u && body[length - 1u] == '/';
  if (folder) {
    body[--length] = '\0';
  }
  if (!length || (folder && !is_directory)) {
    return false_v;
  }
  uint32_t steps = 0u;
  if (!strchr(body, '/')) {
    const char *slash = strrchr(path, '/');
    return ignore_glob(body, slash ? slash + 1 : path, &steps);
  }
  return ignore_glob(body[0] == '/' ? body + 1 : body, path, &steps);
}

/* Reads the working copy's `.vkrdepotignore`: a missing file ignores
   nothing. */
static bool8_t ignore_load(const char *root, DepotIgnore *out) {
  MemZero(out, sizeof(*out));
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(path, sizeof(path), root, VKR_DEPOT_IGNORE_FILE)) {
    return false_v;
  }
  if (!vkr_bakery_is_file(path)) {
    return true_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_read_file(path, 1u << 20, &bytes, &size)) {
    return false_v;
  }
  char *text = malloc(size + 1u);
  if (!text) {
    free(bytes);
    return false_v;
  }
  MemCopy(text, bytes, size);
  text[size] = '\0';
  free(bytes);
  out->text = text;
  for (char *line = text; line && *line;) {
    char *next = strchr(line, '\n');
    if (next) {
      *next++ = '\0';
    }
    size_t length = strlen(line);
    while (length && (line[length - 1u] == '\r' || line[length - 1u] == ' ' ||
                      line[length - 1u] == '\t')) {
      line[--length] = '\0';
    }
    while (*line == ' ' || *line == '\t') {
      ++line;
    }
    if (*line && *line != '#' && out->count < VKR_DEPOT_IGNORE_MAX) {
      out->patterns[out->count++] = line;
    }
    line = next;
  }
  return true_v;
}

static void ignore_free(DepotIgnore *ignore) {
  free(ignore->text);
  MemZero(ignore, sizeof(*ignore));
}

/* The last rule that matches decides, as in git: `!pattern` takes back what
   an earlier rule ignored, so `*.vkt` then `!probe.vkt` keeps the baked
   probes, which no machine can cook from a recipe. */
static bool8_t ignore_matches(const DepotIgnore *ignore, const char *relative,
                              bool8_t is_directory) {
  bool8_t ignored = false_v;
  for (uint32_t i = 0u; i < ignore->count; ++i) {
    const char *pattern = ignore->patterns[i];
    const bool8_t keep = pattern[0] == '!';
    if (keep == ignored && vkr_depot_ignore_match(keep ? pattern + 1 : pattern,
                                                  relative, is_directory)) {
      ignored = !keep;
    }
  }
  return ignored;
}

/* Whether the rules match the file `path` or one of its folders. */
static bool8_t ignore_matches_path(const DepotIgnore *ignore,
                                   const char *path) {
  if (ignore->count == 0u) {
    return false_v;
  }
  char folder[VKR_DEPOT_PATH_CAPACITY];
  for (const char *slash = strchr(path, '/'); slash;
       slash = strchr(slash + 1, '/')) {
    const size_t length = (size_t)(slash - path);
    if (length >= sizeof(folder)) {
      return false_v;
    }
    MemCopy(folder, path, length);
    folder[length] = '\0';
    if (ignore_matches(ignore, folder, true_v)) {
      return true_v;
    }
  }
  return ignore_matches(ignore, path, false_v);
}

/* Whether the index tracks `path` or, for a folder, any file below it. */
static bool8_t index_tracks(const VkrDepotFileList *index, const char *path,
                            bool8_t is_directory) {
  if (!is_directory) {
    return vkr_depot_file_list_find(index, path) != NULL;
  }
  /* The first entry not before "<path>/" in byte order. */
  const size_t length = strlen(path);
  uint32_t low = 0u;
  uint32_t high = index->count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2u;
    const char *other = index->entries[middle].path;
    const int order = strncmp(other, path, length);
    if (order < 0 || (order == 0 && other[length] < '/')) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return low < index->count &&
         strncmp(index->entries[low].path, path, length) == 0 &&
         index->entries[low].path[length] == '/';
}

typedef struct WalkState {
  VkrDepotWorkspace *workspace;
  VkrDepotFileList *files;
  /* The folder being listed, relative to the root; empty at the root. */
  char relative[VKR_DEPOT_PATH_CAPACITY];
  uint32_t depth;
  /* Scratch for one file's absolute path. */
  char absolute[VKR_DEPOT_PATH_CAPACITY];
  bool8_t failed;
  /* Entries of `files` whose size or time differ from the index: hashed
     after the walk, so progress knows their count and bytes. */
  uint32_t *pending;
  uint32_t pending_count;
  uint32_t pending_capacity;
  uint64_t pending_bytes;
  DepotIgnore ignore;
  /* A cooked snapshot: only untracked platform files (VKR_DEPOT_COOKED_SUFFIX),
     which the ignore rules keep out of an ordinary snapshot, with the cooked
     cache standing in for the index. */
  bool8_t cooked;
  VkrDepotFileList cooked_known;
} WalkState;

/* Ignore rules apply to untracked paths only, as in git: a pulled
   `.vkrdepotignore` cannot hide a tracked file's local edits, which the
   next checkout would then overwrite. */
static bool8_t walk_ignored(const WalkState *state, const char *relative,
                            bool8_t is_directory) {
  return ignore_matches(&state->ignore, relative, is_directory) &&
         !index_tracks(&state->workspace->index, relative, is_directory);
}

/* Whether a cooked snapshot skips the file `relative`: anything but an
   untracked platform file. */
static bool8_t walk_not_cooked(const WalkState *state, const char *relative) {
  const size_t length = strlen(relative);
  const size_t suffix = sizeof(VKR_DEPOT_COOKED_SUFFIX) - 1u;
  return length <= suffix ||
         strcmp(relative + length - suffix, VKR_DEPOT_COOKED_SUFFIX) != 0 ||
         vkr_depot_file_list_find(&state->workspace->index, relative) != NULL;
}

/* Chunks a file's bytes into the store and stores its blob manifest. */
static bool8_t workspace_store_file(const VkrDepotStore *store,
                                    const uint8_t *data, uint64_t size,
                                    VkrDepotId *out_blob) {
  VkrDepotChunkRef *chunks = NULL;
  uint32_t count = 0u;
  uint32_t capacity = 0u;
  bool8_t ok = true_v;
  uint64_t offset = 0u;
  while (offset < size && ok) {
    const uint32_t length =
        vkr_depot_chunk_length(data + offset, size - offset);
    if (count == capacity) {
      capacity = capacity ? capacity * 2u : 16u;
      VkrDepotChunkRef *grown =
          realloc(chunks, (size_t)capacity * sizeof(VkrDepotChunkRef));
      if (!grown) {
        ok = false_v;
        break;
      }
      chunks = grown;
    }
    VkrDepotChunkRef *chunk = &chunks[count++];
    chunk->size = length;
    vkr_depot_id_of(data + offset, length, &chunk->id);
    ok = vkr_depot_store_put(store, &chunk->id, data + offset, length);
    offset += length;
  }
  uint8_t *manifest = NULL;
  uint64_t manifest_size = 0u;
  if (ok && vkr_depot_blob_encode(chunks, count, &manifest, &manifest_size)) {
    vkr_depot_id_of(manifest, manifest_size, out_blob);
    ok = vkr_depot_store_put(store, out_blob, manifest, manifest_size);
    free(manifest);
  } else {
    ok = false_v;
  }
  free(chunks);
  return ok;
}

static bool8_t walk_visit(void *context, const char *name,
                          bool8_t is_directory) {
  WalkState *state = context;
  if (state->depth == 0u && strcmp(name, VKR_DEPOT_META_DIR) == 0) {
    return true_v;
  }
  if (!vkr_depot_name_valid(name, (uint32_t)strlen(name)) ||
      (is_directory && state->depth + 1u > VKR_DEPOT_TREE_DEPTH_MAX)) {
    fprintf(stderr, "depot: skipping a name no tree may hold: %s/%s\n",
            state->relative, name);
    return true_v;
  }
  /* The child's path extends `relative` in place and is cut back after. */
  const size_t saved = strlen(state->relative);
  const int written =
      snprintf(state->relative + saved, sizeof(state->relative) - saved,
               saved ? "/%s" : "%s", name);
  if (written < 0 || (size_t)written >= sizeof(state->relative) - saved) {
    state->relative[saved] = '\0';
    state->failed = true_v;
    return false_v;
  }
  const char *relative = state->relative;
  bool8_t ok = true_v;
  if (!is_directory && state->cooked
          ? walk_not_cooked(state, relative)
          : walk_ignored(state, relative, is_directory)) {
    goto done;
  }
  if (is_directory) {
    /* On the heap: a listing may hold its path while children recurse. */
    char *absolute = malloc(VKR_DEPOT_PATH_CAPACITY);
    ok = absolute && vkr_bakery_path_join(absolute, VKR_DEPOT_PATH_CAPACITY,
                                          state->workspace->root, relative);
    if (ok) {
      state->depth += 1u;
      ok = vkr_bakery_list_directory(absolute, walk_visit, state);
      state->depth -= 1u;
    }
    free(absolute);
    state->failed = state->failed || !ok;
    goto done;
  }

  VkrBakeryStat stat;
  if (!vkr_bakery_path_join(state->absolute, sizeof(state->absolute),
                            state->workspace->root, relative) ||
      !vkr_bakery_stat(state->absolute, &stat) || !stat.exists) {
    state->failed = true_v;
    ok = false_v;
    goto done;
  }
  /* An unchanged file keeps the blob the index recorded. */
  const VkrDepotIndexEntry *known = vkr_depot_file_list_find(
      state->cooked ? &state->cooked_known : &state->workspace->index,
      relative);
  if (known && known->size == stat.size && known->mtime_ns == stat.mtime_ns &&
      vkr_depot_store_has(&state->workspace->store, &known->blob)) {
    ok = vkr_depot_file_list_add(state->files, relative, stat.size,
                                 stat.mtime_ns, &known->blob);
    state->failed = state->failed || !ok;
    goto done;
  }
  if (state->pending_count == state->pending_capacity) {
    const uint32_t capacity =
        state->pending_capacity ? state->pending_capacity * 2u : 64u;
    uint32_t *pending =
        realloc(state->pending, (size_t)capacity * sizeof(uint32_t));
    if (!pending) {
      state->failed = true_v;
      ok = false_v;
      goto done;
    }
    state->pending = pending;
    state->pending_capacity = capacity;
  }
  const VkrDepotId unhashed = {0};
  ok = vkr_depot_file_list_add(state->files, relative, stat.size, stat.mtime_ns,
                               &unhashed);
  if (!ok) {
    state->failed = true_v;
    goto done;
  }
  state->pending[state->pending_count++] = state->files->count - 1u;
  state->pending_bytes += stat.size;
done:
  state->relative[saved] = '\0';
  return ok;
}

/* Hashes the files the walk found changed into the store. */
static bool8_t walk_hash_pending(WalkState *state) {
  VkrDepotWorkspace *workspace = state->workspace;
  vkr_depot_progress_phase(workspace->progress, VKR_DEPOT_PHASE_SNAPSHOT,
                           state->pending_count, state->pending_bytes);
  for (uint32_t i = 0u; i < state->pending_count; ++i) {
    if (vkr_depot_progress_canceled(workspace->progress)) {
      return false_v;
    }
    VkrDepotIndexEntry *entry = &state->files->entries[state->pending[i]];
    char absolute[VKR_DEPOT_PATH_CAPACITY];
    uint8_t *data = NULL;
    uint64_t size = 0u;
    if (!vkr_bakery_path_join(absolute, sizeof(absolute), workspace->root,
                              entry->path) ||
        !vkr_bakery_read_file(absolute, VKR_DEPOT_FILE_MAX, &data, &size)) {
      return false_v;
    }
    const bool8_t stored =
        workspace_store_file(&workspace->store, data, size, &entry->blob);
    free(data);
    if (!stored) {
      return false_v;
    }
    entry->size = size;
    vkr_depot_progress_add(workspace->progress, 1u, size);
  }
  return true_v;
}

/* Builds the tree of the files in [begin, end), which share the directory
   prefix of `offset` bytes. */
static bool8_t workspace_build_tree(const VkrDepotStore *store,
                                    const VkrDepotFileList *files,
                                    uint32_t begin, uint32_t end,
                                    uint32_t offset, VkrDepotId *out_tree) {
  VkrDepotTreeEntry *entries =
      malloc((size_t)Max(end - begin, 1u) * sizeof(VkrDepotTreeEntry));
  if (!entries) {
    return false_v;
  }
  uint32_t count = 0u;
  bool8_t ok = true_v;
  uint32_t i = begin;
  while (i < end && ok) {
    const char *name = files->entries[i].path + offset;
    const char *slash = strchr(name, '/');
    VkrDepotTreeEntry *entry = &entries[count++];
    if (!slash) {
      *entry = (VkrDepotTreeEntry){.name = name,
                                   .name_size = (uint16_t)strlen(name),
                                   .kind = VKR_DEPOT_ENTRY_FILE,
                                   .size = files->entries[i].size,
                                   .id = files->entries[i].blob};
      ++i;
      continue;
    }
    /* Every path below this directory follows in sorted order. */
    const uint32_t component = (uint32_t)(slash - name);
    uint32_t j = i + 1u;
    while (j < end) {
      const char *other = files->entries[j].path + offset;
      if (strncmp(other, name, component + 1u) != 0) {
        break;
      }
      ++j;
    }
    *entry = (VkrDepotTreeEntry){.name = name,
                                 .name_size = (uint16_t)component,
                                 .kind = VKR_DEPOT_ENTRY_TREE};
    ok = workspace_build_tree(store, files, i, j, offset + component + 1u,
                              &entry->id);
    i = j;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (ok && vkr_depot_tree_encode(entries, count, &bytes, &size)) {
    vkr_depot_id_of(bytes, size, out_tree);
    ok = vkr_depot_store_put(store, out_tree, bytes, size);
    free(bytes);
  } else {
    ok = false_v;
  }
  free(entries);
  return ok;
}

bool8_t vkr_depot_workspace_note_fetched(const char *root, const char *path,
                                         const VkrDepotId *blob) {
  char log[VKR_DEPOT_PATH_CAPACITY];
  char line[VKR_DEPOT_ID_HEX + VKR_DEPOT_PATH_CAPACITY + 2u];
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(blob, hex);
  const int length = snprintf(line, sizeof(line), "%s %s\n", hex, path);
  return vkr_bakery_path_join(log, sizeof(log), root,
                              VKR_DEPOT_META_DIR "/" VKR_DEPOT_FETCHED_LOG) &&
         length > 0 && (size_t)length < sizeof(line) &&
         vkr_bakery_append_file(log, line, (uint64_t)length);
}

bool8_t vkr_depot_workspace_clear_fetched(const VkrDepotWorkspace *workspace) {
  char log[VKR_DEPOT_PATH_CAPACITY];
  return workspace_meta_path(workspace, VKR_DEPOT_FETCHED_LOG, log,
                             sizeof(log)) &&
         vkr_bakery_remove_file(log);
}

/* The files the fetched log names, each with the blob written, sorted. A
   missing log is empty; a torn last line is skipped. */
static bool8_t workspace_fetched_load(const VkrDepotWorkspace *workspace,
                                      VkrDepotFileList *out_files) {
  MemZero(out_files, sizeof(*out_files));
  char log[VKR_DEPOT_PATH_CAPACITY];
  if (!workspace_meta_path(workspace, VKR_DEPOT_FETCHED_LOG, log,
                           sizeof(log))) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_is_file(log)) {
    return true_v;
  }
  if (!vkr_bakery_read_file(log, 1ull << 30, &bytes, &size)) {
    return false_v;
  }
  bool8_t ok = true_v;
  uint64_t start = 0u;
  for (uint64_t i = 0u; i < size && ok; ++i) {
    if (bytes[i] != '\n') {
      continue;
    }
    char hex[VKR_DEPOT_ID_HEX];
    char path[VKR_DEPOT_PATH_CAPACITY];
    const uint64_t length = i - start;
    VkrDepotId blob;
    if (length > VKR_DEPOT_ID_HEX && length - VKR_DEPOT_ID_HEX < sizeof(path) &&
        bytes[start + VKR_DEPOT_ID_HEX - 1u] == ' ') {
      MemCopy(hex, bytes + start, VKR_DEPOT_ID_HEX - 1u);
      hex[VKR_DEPOT_ID_HEX - 1u] = '\0';
      MemCopy(path, bytes + start + VKR_DEPOT_ID_HEX,
              (size_t)(length - VKR_DEPOT_ID_HEX));
      path[length - VKR_DEPOT_ID_HEX] = '\0';
      if (vkr_depot_id_parse(hex, &blob) && vkr_depot_path_valid(path)) {
        ok = vkr_depot_file_list_add(out_files, path, 0u, 0, &blob);
      }
    }
    start = i + 1u;
  }
  free(bytes);
  if (!ok) {
    vkr_depot_file_list_free(out_files);
    return false_v;
  }
  vkr_depot_file_list_sort(out_files);
  return true_v;
}

/* Adds to the sorted `files` of a walk every file the index holds unfetched
   that the walk did not find on disk: it is unchanged, with its blob, unless
   the fetched log says it arrived with that blob, so it was deleted since. A
   file that arrived and stays was walked and hashed like any other. */
static bool8_t workspace_snapshot_unfetched(const VkrDepotWorkspace *workspace,
                                            VkrDepotFileList *files) {
  VkrDepotFileList fetched;
  if (!workspace_fetched_load(workspace, &fetched)) {
    return false_v;
  }
  const uint32_t walked = files->count;
  bool8_t ok = true_v;
  for (uint32_t i = 0u; i < workspace->index.count && ok; ++i) {
    const VkrDepotIndexEntry *entry = &workspace->index.entries[i];
    if (entry->mtime_ns != VKR_DEPOT_MTIME_UNFETCHED) {
      continue;
    }
    /* The walk's own files are the first `walked`, sorted. */
    const VkrDepotFileList found = {
        .entries = files->entries, .count = walked, .capacity = walked};
    const VkrDepotIndexEntry *arrived =
        vkr_depot_file_list_find(&fetched, entry->path);
    if (vkr_depot_file_list_find(&found, entry->path) ||
        (arrived && vkr_depot_id_equal(&arrived->blob, &entry->blob))) {
      continue;
    }
    ok = vkr_depot_file_list_add(files, entry->path, entry->size,
                                 VKR_DEPOT_MTIME_UNFETCHED, &entry->blob);
  }
  vkr_depot_file_list_free(&fetched);
  if (ok && files->count != walked) {
    vkr_depot_file_list_sort(files);
  }
  return ok;
}

static bool8_t workspace_snapshot(VkrDepotWorkspace *workspace, bool8_t cooked,
                                  VkrDepotId *out_tree,
                                  VkrDepotFileList *out_files) {
  MemZero(out_files, sizeof(*out_files));
  WalkState *state = calloc(1u, sizeof(WalkState));
  if (!state) {
    return false_v;
  }
  state->workspace = workspace;
  state->files = out_files;
  state->cooked = cooked;
  const bool8_t walked =
      (!cooked ||
       vkr_depot_cooked_cache_load(workspace, &state->cooked_known)) &&
      ignore_load(workspace->root, &state->ignore) &&
      vkr_bakery_list_directory(workspace->root, walk_visit, state) &&
      !state->failed && walk_hash_pending(state);
  ignore_free(&state->ignore);
  vkr_depot_file_list_free(&state->cooked_known);
  free(state->pending);
  free(state);
  if (!walked) {
    vkr_depot_file_list_free(out_files);
    return false_v;
  }
  vkr_depot_file_list_sort(out_files);
  if (!cooked && !workspace_snapshot_unfetched(workspace, out_files)) {
    vkr_depot_file_list_free(out_files);
    return false_v;
  }
  if (!workspace_build_tree(&workspace->store, out_files, 0u, out_files->count,
                            0u, out_tree)) {
    vkr_depot_file_list_free(out_files);
    return false_v;
  }
  return true_v;
}

bool8_t vkr_depot_workspace_snapshot(VkrDepotWorkspace *workspace,
                                     VkrDepotId *out_tree,
                                     VkrDepotFileList *out_files) {
  return workspace_snapshot(workspace, false_v, out_tree, out_files);
}

bool8_t vkr_depot_workspace_snapshot_cooked(VkrDepotWorkspace *workspace,
                                            VkrDepotId *out_tree,
                                            VkrDepotFileList *out_files) {
  if (!workspace_snapshot(workspace, true_v, out_tree, out_files)) {
    return false_v;
  }
  /* The cache only saves hashing; a snapshot stands without it. */
  (void)vkr_depot_cooked_cache_save(workspace, out_files);
  return true_v;
}

// =============================================================================
// Trees to files
// =============================================================================

typedef struct FlattenState {
  const VkrDepotStore *store;
  VkrDepotFileList *files;
  /* The folder being read; extended in place and cut back. */
  char prefix[VKR_DEPOT_PATH_CAPACITY];
  uint32_t depth;
  uint64_t bytes;
} FlattenState;

static bool8_t flatten_tree(FlattenState *state, const VkrDepotId *tree);

/* Enforces the tree limits of vkr_depot_object.h: a hostile tree that nests
   past them, or names one subtree at many paths until the files or bytes
   pass them, is refused instead of exhausting a stack, memory or disk. */
static bool8_t flatten_visit(void *context, const VkrDepotTreeEntry *entry) {
  FlattenState *state = context;
  const size_t saved = strlen(state->prefix);
  const size_t room = VKR_DEPOT_PATH_MAX - saved;
  const int written =
      snprintf(state->prefix + saved, room, saved ? "/%.*s" : "%.*s",
               (int)entry->name_size, entry->name);
  bool8_t ok = written >= 0 && (size_t)written < room;
  if (ok && entry->kind == VKR_DEPOT_ENTRY_FILE) {
    state->bytes += entry->size;
    ok = state->files->count < VKR_DEPOT_TREE_FILES_MAX &&
         state->bytes <= VKR_DEPOT_TREE_BYTES_MAX &&
         vkr_depot_file_list_add(state->files, state->prefix, entry->size, -1,
                                 &entry->id);
  } else if (ok) {
    ok = state->depth < VKR_DEPOT_TREE_DEPTH_MAX;
    if (ok) {
      state->depth += 1u;
      ok = flatten_tree(state, &entry->id);
      state->depth -= 1u;
    }
  }
  state->prefix[saved] = '\0';
  return ok;
}

static bool8_t flatten_tree(FlattenState *state, const VkrDepotId *tree) {
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_depot_store_get(state->store, tree, &bytes, &size)) {
    return false_v;
  }
  const bool8_t ok = vkr_depot_tree_parse(bytes, size, flatten_visit, state);
  free(bytes);
  return ok;
}

bool8_t vkr_depot_tree_files(const VkrDepotStore *store, const VkrDepotId *tree,
                             VkrDepotFileList *out_files) {
  MemZero(out_files, sizeof(*out_files));
  FlattenState *state = calloc(1u, sizeof(FlattenState));
  if (!state) {
    return false_v;
  }
  state->store = store;
  state->files = out_files;
  const bool8_t ok = flatten_tree(state, tree);
  free(state);
  if (!ok) {
    vkr_depot_file_list_free(out_files);
    return false_v;
  }
  vkr_depot_file_list_sort(out_files);
  return true_v;
}

typedef struct BlobWriteState {
  const VkrDepotStore *store;
  FILE *file;
} BlobWriteState;

static bool8_t blob_write_chunk(void *context, const VkrDepotChunkRef *chunk) {
  BlobWriteState *state = context;
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_depot_store_get(state->store, &chunk->id, &bytes, &size)) {
    return false_v;
  }
  const bool8_t ok = size == chunk->size &&
                     fwrite(bytes, 1u, (size_t)size, state->file) == size;
  free(bytes);
  return ok;
}

bool8_t vkr_depot_write_blob(const VkrDepotStore *store, const VkrDepotId *blob,
                             const char *path) {
  uint8_t *manifest = NULL;
  uint64_t manifest_size = 0u;
  if (!vkr_depot_store_get(store, blob, &manifest, &manifest_size)) {
    return false_v;
  }
  char directory[VKR_DEPOT_PATH_CAPACITY];
  char temp[VKR_DEPOT_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  bool8_t ok = vkr_bakery_make_directories(directory);
  vkr_bakery_temp_path(temp, sizeof(temp), directory, "depot");
  FILE *file = ok ? file_fopen(temp, "wb") : NULL;
  if (!file) {
    free(manifest);
    return false_v;
  }
  BlobWriteState state = {.store = store, .file = file};
  ok = vkr_depot_blob_parse(manifest, manifest_size, NULL, blob_write_chunk,
                            &state);
  free(manifest);
  /* No flush (owner decision): a crash may tear the file, so a checkout
     marks it for the next snapshot to hash. */
  ok = (fclose(file) == 0) && ok;
  if (!ok || !vkr_bakery_rename(temp, path, true_v)) {
    (void)vkr_bakery_remove_file(temp);
    return false_v;
  }
  return true_v;
}

typedef struct BlobReadState {
  const VkrDepotStore *store;
  uint64_t offset;
  uint64_t size;
  uint8_t *destination;
} BlobReadState;

/* Copies the part of one chunk that lies in the range. */
static bool8_t blob_read_chunk(void *context, const VkrDepotChunkRef *chunk,
                               uint64_t chunk_offset) {
  BlobReadState *state = context;
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_depot_store_get(state->store, &chunk->id, &bytes, &size)) {
    return false_v;
  }
  const bool8_t ok = size == chunk->size;
  if (ok) {
    const uint64_t begin = Max(chunk_offset, state->offset);
    const uint64_t end =
        Min(chunk_offset + chunk->size, state->offset + state->size);
    MemCopy(state->destination + (begin - state->offset),
            bytes + (begin - chunk_offset), (size_t)(end - begin));
  }
  free(bytes);
  return ok;
}

bool8_t vkr_depot_read_blob_range(const VkrDepotStore *store,
                                  const VkrDepotId *blob, uint64_t offset,
                                  uint64_t size, uint8_t *destination) {
  uint8_t *manifest = NULL;
  uint64_t manifest_size = 0u;
  if (!vkr_depot_store_get(store, blob, &manifest, &manifest_size)) {
    return false_v;
  }
  BlobReadState state = {.store = store,
                         .offset = offset,
                         .size = size,
                         .destination = destination};
  const bool8_t ok = vkr_depot_blob_range(manifest, manifest_size, offset, size,
                                          blob_read_chunk, &state);
  free(manifest);
  return ok;
}

uint32_t vkr_depot_workspace_settle(VkrDepotWorkspace *workspace,
                                    const VkrDepotFileList *files) {
  uint32_t settled = 0u;
  for (uint32_t i = 0u; i < workspace->index.count; ++i) {
    VkrDepotIndexEntry *entry = &workspace->index.entries[i];
    /* Files a checkout wrote, and files a sparse working copy fetched. */
    if (entry->mtime_ns != -1 && entry->mtime_ns != VKR_DEPOT_MTIME_UNFETCHED) {
      continue;
    }
    const VkrDepotIndexEntry *hashed =
        vkr_depot_file_list_find(files, entry->path);
    /* A file the snapshot found on disk; one still unfetched stays so. */
    if (hashed && hashed->mtime_ns != -1 &&
        hashed->mtime_ns != VKR_DEPOT_MTIME_UNFETCHED &&
        vkr_depot_id_equal(&hashed->blob, &entry->blob)) {
      entry->size = hashed->size;
      entry->mtime_ns = hashed->mtime_ns;
      settled += 1u;
    }
  }
  return settled;
}

/* Paths of a list in folded order (vkr_depot_name_fold_compare), for
   lookups that treat `Foo.png` and `foo.png` as the one file macOS and
   Windows make of them. */
typedef struct FoldIndex {
  const char **paths;
  uint32_t count;
} FoldIndex;

static int fold_path_compare(const void *lhs, const void *rhs) {
  const char *a = *(const char *const *)lhs;
  const char *b = *(const char *const *)rhs;
  return vkr_depot_name_fold_compare(a, (uint32_t)strlen(a), b,
                                     (uint32_t)strlen(b));
}

static bool8_t fold_index_build(const VkrDepotFileList *list, FoldIndex *out) {
  MemZero(out, sizeof(*out));
  if (!list || list->count == 0u) {
    return true_v;
  }
  out->paths = malloc((size_t)list->count * sizeof(const char *));
  if (!out->paths) {
    return false_v;
  }
  for (uint32_t i = 0u; i < list->count; ++i) {
    out->paths[i] = list->entries[i].path;
  }
  out->count = list->count;
  qsort(out->paths, out->count, sizeof(const char *), fold_path_compare);
  return true_v;
}

static bool8_t fold_index_has(const FoldIndex *index, const char *path) {
  return index->count &&
         bsearch(&path, index->paths, index->count, sizeof(const char *),
                 fold_path_compare) != NULL;
}

bool8_t vkr_depot_workspace_checkout(VkrDepotWorkspace *workspace,
                                     const VkrDepotId *tree,
                                     const VkrDepotFileList *keep,
                                     uint32_t *out_skipped) {
  if (out_skipped) {
    *out_skipped = 0u;
  }
  VkrDepotFileList target;
  if (!vkr_depot_tree_files(&workspace->store, tree, &target)) {
    return false_v;
  }
  VkrDepotFileList index = {0};
  DepotIgnore ignore = {0};
  FoldIndex kept = {0};
  uint32_t *writes = malloc((size_t)Max(target.count, 1u) * sizeof(uint32_t));
  uint32_t write_count = 0u;
  uint64_t write_bytes = 0u;
  /* The folder a sparse checkout made last; sorted paths share it. */
  char last_folder[VKR_DEPOT_PATH_CAPACITY] = {0};
  /* The rules as they stand before the checkout, which may bring others. */
  bool8_t ok = writes != NULL && ignore_load(workspace->root, &ignore) &&
               fold_index_build(keep, &kept);
  for (uint32_t i = 0u; i < target.count && ok; ++i) {
    VkrDepotIndexEntry *entry = &target.entries[i];
    char absolute[VKR_DEPOT_PATH_CAPACITY];
    ok = vkr_bakery_path_join(absolute, sizeof(absolute), workspace->root,
                              entry->path);
    if (!ok) {
      break;
    }
    /* Paths this machine ignores are its own (scripts it builds, caches):
       a tree never writes them. They stay untracked. */
    if (ignore_matches_path(&ignore, entry->path)) {
      fprintf(stderr, "depot: not checking out an ignored path: %s\n",
              entry->path);
      if (out_skipped) {
        *out_skipped += 1u;
      }
      continue;
    }
    if (fold_index_has(&kept, entry->path)) {
      /* A local change stays; -1 forces a hash on the next snapshot. */
      ok = vkr_depot_file_list_add(&index, entry->path, entry->size, -1,
                                   &entry->blob);
      continue;
    }
    const VkrDepotIndexEntry *known =
        vkr_depot_file_list_find(&workspace->index, entry->path);
    VkrBakeryStat stat = {0};
    const bool8_t present = vkr_bakery_stat(absolute, &stat) && stat.exists;
    /* A sparse working copy writes only the files it holds and its root
       ignore rules; every other file stays unfetched with the target's
       blob. Its folder is made, so the tree's folders exist on disk for
       every reader, as a checkout of the files would leave them. */
    if (workspace->sparse && !present &&
        strcmp(entry->path, VKR_DEPOT_IGNORE_FILE) != 0) {
      char folder[VKR_DEPOT_PATH_CAPACITY];
      vkr_bakery_path_parent(folder, sizeof(folder), absolute);
      if (strcmp(folder, last_folder) != 0) {
        ok = vkr_bakery_make_directories(folder);
        snprintf(last_folder, sizeof(last_folder), "%s", folder);
      }
      ok = ok &&
           vkr_depot_file_list_add(&index, entry->path, entry->size,
                                   VKR_DEPOT_MTIME_UNFETCHED, &entry->blob);
      continue;
    }
    /* A file that already holds the blob stays, including one an earlier
       checkout wrote, or a sparse working copy fetched, that no snapshot
       has hashed yet; -1 makes the next one hash it. */
    const bool8_t unhashed =
        known &&
        (known->mtime_ns == -1 || known->mtime_ns == VKR_DEPOT_MTIME_UNFETCHED);
    if (known && present && vkr_depot_id_equal(&known->blob, &entry->blob) &&
        known->size == stat.size &&
        (known->mtime_ns == stat.mtime_ns || unhashed)) {
      ok = vkr_depot_file_list_add(&index, entry->path, known->size,
                                   unhashed ? -1 : known->mtime_ns,
                                   &entry->blob);
      continue;
    }
    writes[write_count++] = i;
    write_bytes += entry->size;
  }

  /* Tracked files the target lacks leave first, unless they are local
     changes or ignored: a case-only rename, Foo.png to foo.png, removes the
     old name before writing the new one, which is the same file on macOS
     and Windows. */
  for (uint32_t i = 0u; i < workspace->index.count && ok; ++i) {
    const char *path = workspace->index.entries[i].path;
    if (vkr_depot_file_list_find(&target, path) ||
        fold_index_has(&kept, path) || ignore_matches_path(&ignore, path)) {
      continue;
    }
    char absolute[VKR_DEPOT_PATH_CAPACITY];
    ok = vkr_bakery_path_join(absolute, sizeof(absolute), workspace->root,
                              path) &&
         vkr_bakery_remove_file(absolute);
  }

  /* Written without a flush: -1 makes the next snapshot hash it, so a file a
     crash tore shows as a change instead of passing as current. */
  if (ok) {
    vkr_depot_progress_phase(workspace->progress, VKR_DEPOT_PHASE_CHECKOUT,
                             write_count, write_bytes);
  }
  for (uint32_t i = 0u; i < write_count && ok; ++i) {
    if (vkr_depot_progress_canceled(workspace->progress)) {
      ok = false_v;
      break;
    }
    const VkrDepotIndexEntry *entry = &target.entries[writes[i]];
    char absolute[VKR_DEPOT_PATH_CAPACITY];
    ok = vkr_bakery_path_join(absolute, sizeof(absolute), workspace->root,
                              entry->path) &&
         vkr_depot_write_blob(&workspace->store, &entry->blob, absolute) &&
         vkr_depot_file_list_add(&index, entry->path, entry->size, -1,
                                 &entry->blob);
    vkr_depot_progress_add(workspace->progress, 1u, entry->size);
  }
  free(writes);
  free(kept.paths);
  ignore_free(&ignore);
  vkr_depot_file_list_free(&target);
  if (!ok) {
    vkr_depot_file_list_free(&index);
    return false_v;
  }
  vkr_depot_file_list_sort(&index);
  vkr_depot_file_list_free(&workspace->index);
  workspace->index = index;
  return true_v;
}
