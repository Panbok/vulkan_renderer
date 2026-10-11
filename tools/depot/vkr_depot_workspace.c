#include "vkr_depot_workspace.h"

#include "core/vkr_byte_io.h"
#include "filesystem/filesystem.h"
#include "vkr_bakery_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKSPACE_STATE_MAGIC 0x57444b56u /* "VKDW" */
#define WORKSPACE_STATE_VERSION 1u
/* Files are read whole while they are chunked. */
#define WORKSPACE_FILE_MAX (4ull * 1024ull * 1024ull * 1024ull)

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

typedef struct WalkState {
  VkrDepotWorkspace *workspace;
  VkrDepotFileList *files;
  char relative[VKR_DEPOT_PATH_CAPACITY];
  bool8_t failed;
  /* Entries of `files` whose size or time differ from the index: hashed
     after the walk, so progress knows their count and bytes. */
  uint32_t *pending;
  uint32_t pending_count;
  uint32_t pending_capacity;
  uint64_t pending_bytes;
  /* The patterns of `.vkrdepotignore`, pointing into `ignore_text`. */
  char *ignore_text;
  const char *ignore[VKR_DEPOT_IGNORE_MAX];
  uint32_t ignore_count;
} WalkState;

/* Glob match of `text` against `pattern`: '*' within a name, '**' across
   names, '?' one byte other than '/'. */
static bool8_t ignore_glob(const char *pattern, const char *text) {
  while (*pattern) {
    if (pattern[0] == '*' && pattern[1] == '*') {
      pattern += 2;
      if (*pattern == '/') {
        ++pattern;
      }
      for (const char *rest = text;; ++rest) {
        if (ignore_glob(pattern, rest)) {
          return true_v;
        }
        if (!*rest) {
          return false_v;
        }
      }
    }
    if (*pattern == '*') {
      ++pattern;
      for (const char *rest = text;; ++rest) {
        if (ignore_glob(pattern, rest)) {
          return true_v;
        }
        if (!*rest || *rest == '/') {
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
  if (!strchr(body, '/')) {
    const char *slash = strrchr(path, '/');
    return ignore_glob(body, slash ? slash + 1 : path);
  }
  return ignore_glob(body[0] == '/' ? body + 1 : body, path);
}

/* Reads `.vkrdepotignore`: a missing file ignores nothing. */
static bool8_t walk_load_ignore(WalkState *state) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(path, sizeof(path), state->workspace->root,
                            VKR_DEPOT_IGNORE_FILE)) {
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
  state->ignore_text = text;
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
    if (*line && *line != '#' && state->ignore_count < VKR_DEPOT_IGNORE_MAX) {
      state->ignore[state->ignore_count++] = line;
    }
    line = next;
  }
  return true_v;
}

static bool8_t walk_ignored(const WalkState *state, const char *relative,
                            bool8_t is_directory) {
  for (uint32_t i = 0u; i < state->ignore_count; ++i) {
    if (vkr_depot_ignore_match(state->ignore[i], relative, is_directory)) {
      return true_v;
    }
  }
  return false_v;
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
  if (state->relative[0] == '\0' && strcmp(name, VKR_DEPOT_META_DIR) == 0) {
    return true_v;
  }
  if (!vkr_depot_name_valid(name, (uint32_t)strlen(name))) {
    fprintf(stderr, "depot: skipping a name no tree may hold: %s/%s\n",
            state->relative, name);
    return true_v;
  }
  const size_t saved = strlen(state->relative);
  char relative[VKR_DEPOT_PATH_CAPACITY];
  if (saved > 0u) {
    if (!vkr_bakery_path_join(relative, sizeof(relative), state->relative,
                              name)) {
      state->failed = true_v;
      return false_v;
    }
  } else {
    snprintf(relative, sizeof(relative), "%s", name);
  }
  if (walk_ignored(state, relative, is_directory)) {
    return true_v;
  }
  char absolute[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(absolute, sizeof(absolute), state->workspace->root,
                            relative)) {
    state->failed = true_v;
    return false_v;
  }

  if (is_directory) {
    char previous[VKR_DEPOT_PATH_CAPACITY];
    snprintf(previous, sizeof(previous), "%s", state->relative);
    snprintf(state->relative, sizeof(state->relative), "%s", relative);
    const bool8_t listed =
        vkr_bakery_list_directory(absolute, walk_visit, state);
    snprintf(state->relative, sizeof(state->relative), "%s", previous);
    if (!listed) {
      state->failed = true_v;
    }
    return listed;
  }

  VkrBakeryStat stat;
  if (!vkr_bakery_stat(absolute, &stat) || !stat.exists) {
    state->failed = true_v;
    return false_v;
  }
  /* An unchanged file keeps the blob the index recorded. */
  const VkrDepotIndexEntry *known =
      vkr_depot_file_list_find(&state->workspace->index, relative);
  if (known && known->size == stat.size && known->mtime_ns == stat.mtime_ns &&
      vkr_depot_store_has(&state->workspace->store, &known->blob)) {
    return vkr_depot_file_list_add(state->files, relative, stat.size,
                                   stat.mtime_ns, &known->blob) ||
           (state->failed = true_v, false_v);
  }
  if (state->pending_count == state->pending_capacity) {
    const uint32_t capacity =
        state->pending_capacity ? state->pending_capacity * 2u : 64u;
    uint32_t *pending =
        realloc(state->pending, (size_t)capacity * sizeof(uint32_t));
    if (!pending) {
      state->failed = true_v;
      return false_v;
    }
    state->pending = pending;
    state->pending_capacity = capacity;
  }
  const VkrDepotId unhashed = {0};
  if (!vkr_depot_file_list_add(state->files, relative, stat.size, stat.mtime_ns,
                               &unhashed)) {
    state->failed = true_v;
    return false_v;
  }
  state->pending[state->pending_count++] = state->files->count - 1u;
  state->pending_bytes += stat.size;
  return true_v;
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
        !vkr_bakery_read_file(absolute, WORKSPACE_FILE_MAX, &data, &size)) {
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
  WalkState state = {.workspace = workspace, .files = out_files};
  const bool8_t walked =
      walk_load_ignore(&state) &&
      vkr_bakery_list_directory(workspace->root, walk_visit, &state) &&
      !state.failed && walk_hash_pending(&state);
  free(state.ignore_text);
  free(state.pending);
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
  char prefix[VKR_DEPOT_PATH_CAPACITY];
  bool8_t failed;
} FlattenState;

static bool8_t flatten_tree(FlattenState *state, const VkrDepotId *tree);

static bool8_t flatten_visit(void *context, const VkrDepotTreeEntry *entry) {
  FlattenState *state = context;
  char path[VKR_DEPOT_PATH_CAPACITY];
  const int written =
      state->prefix[0] ? snprintf(path, sizeof(path), "%s/%.*s", state->prefix,
                                  (int)entry->name_size, entry->name)
                       : snprintf(path, sizeof(path), "%.*s",
                                  (int)entry->name_size, entry->name);
  if (written < 0 || written >= (int)sizeof(path)) {
    return false_v;
  }
  if (entry->kind == VKR_DEPOT_ENTRY_FILE) {
    return vkr_depot_file_list_add(state->files, path, entry->size, -1,
                                   &entry->id);
  }
  char previous[VKR_DEPOT_PATH_CAPACITY];
  snprintf(previous, sizeof(previous), "%s", state->prefix);
  snprintf(state->prefix, sizeof(state->prefix), "%s", path);
  const bool8_t ok = flatten_tree(state, &entry->id);
  snprintf(state->prefix, sizeof(state->prefix), "%s", previous);
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
  FlattenState state = {.store = store, .files = out_files};
  if (!flatten_tree(&state, tree)) {
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

bool8_t vkr_depot_workspace_checkout(VkrDepotWorkspace *workspace,
                                     const VkrDepotId *tree,
                                     const VkrDepotFileList *keep) {
  VkrDepotFileList target;
  if (!vkr_depot_tree_files(&workspace->store, tree, &target)) {
    return false_v;
  }
  VkrDepotFileList index = {0};
  uint32_t *writes = malloc((size_t)Max(target.count, 1u) * sizeof(uint32_t));
  uint32_t write_count = 0u;
  uint64_t write_bytes = 0u;
  bool8_t ok = writes != NULL;
  for (uint32_t i = 0u; i < target.count && ok; ++i) {
    VkrDepotIndexEntry *entry = &target.entries[i];
    char absolute[VKR_DEPOT_PATH_CAPACITY];
    ok = vkr_bakery_path_join(absolute, sizeof(absolute), workspace->root,
                              entry->path);
    if (!ok) {
      break;
    }
    if (keep && vkr_depot_file_list_find(keep, entry->path)) {
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

  /* Written without a flush: -1 makes the next snapshot hash it, so a file a
     crash tore shows as a change instead of passing as current. */
  vkr_depot_progress_phase(workspace->progress, VKR_DEPOT_PHASE_CHECKOUT,
                           write_count, write_bytes);
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

  /* Tracked files the target lacks leave, unless they are local changes. */
  for (uint32_t i = 0u; i < workspace->index.count && ok; ++i) {
    const char *path = workspace->index.entries[i].path;
    if (vkr_depot_file_list_find(&target, path) ||
        (keep && vkr_depot_file_list_find(keep, path))) {
      continue;
    }
    char absolute[VKR_DEPOT_PATH_CAPACITY];
    ok = vkr_bakery_path_join(absolute, sizeof(absolute), workspace->root,
                              path) &&
         vkr_bakery_remove_file(absolute);
  }
  free(writes);
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
