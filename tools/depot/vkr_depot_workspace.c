#include "vkr_depot_workspace.h"

#include "core/vkr_byte_io.h"
#include "filesystem/filesystem.h"
#include "vkr_bakery_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKSPACE_STATE_MAGIC 0x57444b56u /* "VKDW" */
#define WORKSPACE_STATE_VERSION 1u

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

bool8_t vkr_depot_workspace_save(const VkrDepotWorkspace *workspace) {
  uint64_t size = 4u + 4u + 2u + strlen(workspace->remote) + 32u + 2u +
                  strlen(workspace->branch) + VKR_DEPOT_ID_SIZE + 4u;
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
  vkr_store_le_u32(cursor + 4, WORKSPACE_STATE_VERSION);
  cursor += 8u;
  const uint16_t remote_size = (uint16_t)strlen(workspace->remote);
  cursor[0] = (uint8_t)remote_size;
  cursor[1] = (uint8_t)(remote_size >> 8);
  memcpy(cursor + 2u, workspace->remote, remote_size);
  cursor += 2u + remote_size;
  memcpy(cursor, workspace->server_key, 32u);
  cursor += 32u;
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
  return ok;
}

static bool8_t workspace_load_state(VkrDepotWorkspace *workspace) {
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
  if (size < 8u || vkr_load_le_u32(cursor) != WORKSPACE_STATE_MAGIC ||
      vkr_load_le_u32(cursor + 4) != WORKSPACE_STATE_VERSION) {
    goto cleanup;
  }
  cursor += 8u;
  if (end - cursor < 2) {
    goto cleanup;
  }
  length = (uint16_t)(cursor[0] | (cursor[1] << 8));
  cursor += 2u;
  if (length >= VKR_DEPOT_REMOTE_MAX || end - cursor < (int64_t)length + 32) {
    goto cleanup;
  }
  memcpy(workspace->remote, cursor, length);
  workspace->remote[length] = '\0';
  cursor += length;
  memcpy(workspace->server_key, cursor, 32u);
  cursor += 32u;
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
                                   const VkrDepotCodec *codec) {
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
                                 const VkrDepotCodec *codec) {
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
  if (!opened || !workspace_load_state(workspace)) {
    vkr_depot_workspace_close(workspace);
    return false_v;
  }
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

static bool8_t ignore_matches(const DepotIgnore *ignore, const char *relative,
                              bool8_t is_directory) {
  for (uint32_t i = 0u; i < ignore->count; ++i) {
    if (vkr_depot_ignore_match(ignore->patterns[i], relative, is_directory)) {
      return true_v;
    }
  }
  return false_v;
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
} WalkState;

/* Ignore rules apply to untracked paths only, as in git: a pulled
   `.vkrdepotignore` cannot hide a tracked file's local edits, which the
   next checkout would then overwrite. */
static bool8_t walk_ignored(const WalkState *state, const char *relative,
                            bool8_t is_directory) {
  return ignore_matches(&state->ignore, relative, is_directory) &&
         !index_tracks(&state->workspace->index, relative, is_directory);
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
  if (walk_ignored(state, relative, is_directory)) {
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
  const VkrDepotIndexEntry *known =
      vkr_depot_file_list_find(&state->workspace->index, relative);
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

bool8_t vkr_depot_workspace_snapshot(VkrDepotWorkspace *workspace,
                                     VkrDepotId *out_tree,
                                     VkrDepotFileList *out_files) {
  MemZero(out_files, sizeof(*out_files));
  WalkState *state = calloc(1u, sizeof(WalkState));
  if (!state) {
    return false_v;
  }
  state->workspace = workspace;
  state->files = out_files;
  const bool8_t walked =
      ignore_load(workspace->root, &state->ignore) &&
      vkr_bakery_list_directory(workspace->root, walk_visit, state) &&
      !state->failed && walk_hash_pending(state);
  ignore_free(&state->ignore);
  free(state->pending);
  free(state);
  if (!walked) {
    vkr_depot_file_list_free(out_files);
    return false_v;
  }
  vkr_depot_file_list_sort(out_files);
  if (!workspace_build_tree(&workspace->store, out_files, 0u, out_files->count,
                            0u, out_tree)) {
    vkr_depot_file_list_free(out_files);
    return false_v;
  }
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

uint32_t vkr_depot_workspace_settle(VkrDepotWorkspace *workspace,
                                    const VkrDepotFileList *files) {
  uint32_t settled = 0u;
  for (uint32_t i = 0u; i < workspace->index.count; ++i) {
    VkrDepotIndexEntry *entry = &workspace->index.entries[i];
    if (entry->mtime_ns != -1) {
      continue;
    }
    const VkrDepotIndexEntry *hashed =
        vkr_depot_file_list_find(files, entry->path);
    if (hashed && hashed->mtime_ns != -1 &&
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
    /* A file that already holds the blob stays, including one an earlier
       checkout wrote that no snapshot has hashed yet (-1). */
    if (known && present && vkr_depot_id_equal(&known->blob, &entry->blob) &&
        known->size == stat.size &&
        (known->mtime_ns == stat.mtime_ns || known->mtime_ns == -1)) {
      ok = vkr_depot_file_list_add(&index, entry->path, known->size,
                                   known->mtime_ns, &entry->blob);
      continue;
    }
    writes[write_count++] = i;
    write_bytes += entry->size;
  }

  /* Tracked files the target lacks leave first, unless they are local
     changes or ignored: a case-only rename (Foo.png to foo.png) removes the
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
