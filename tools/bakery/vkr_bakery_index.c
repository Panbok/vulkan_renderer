#include "vkr_bakery_internal.h"

#include "vkr_bakery_buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Persistent hash cache keyed by absolute path. A stored hash is reused while
 * the file's size, modification time and file identity are unchanged, so a
 * warm build hashes nothing. The table lives in malloc storage owned by the
 * index; entries are only added or overwritten during one run and the file
 * `<cache>/index/paths.v1` is rewritten atomically on close. Concurrent
 * processes may overwrite each other's additions; the loss only costs a
 * rehash. */

#define VKR_BAKERY_INDEX_FORMAT "vkr_bakery index 1\n"

typedef struct VkrBakeryIndexEntry {
  char *path;
  uint64_t size;
  int64_t mtime_ns;
  uint64_t device;
  uint64_t file_id;
  char hash[VKR_BAKERY_KEY_SIZE];
} VkrBakeryIndexEntry;

struct VkrBakeryIndex {
  char file_path[VKR_BAKERY_PATH_CAPACITY];
  VkrBakeryIndexEntry *slots;
  uint32_t capacity; /* Power of two. */
  uint32_t count;
  bool8_t dirty;
};

vkr_internal uint64_t vkr_bakery_index_key(const char *path) {
  uint64_t hash = 1469598103934665603ull;
  for (const uint8_t *c = (const uint8_t *)path; *c; ++c) {
    hash ^= *c;
    hash *= 1099511628211ull;
  }
  return hash;
}

vkr_internal VkrBakeryIndexEntry *vkr_bakery_index_slot(VkrBakeryIndex *index,
                                                        const char *path) {
  uint32_t slot = (uint32_t)vkr_bakery_index_key(path) & (index->capacity - 1u);
  for (;;) {
    VkrBakeryIndexEntry *entry = &index->slots[slot];
    if (!entry->path || strcmp(entry->path, path) == 0) {
      return entry;
    }
    slot = (slot + 1u) & (index->capacity - 1u);
  }
}

vkr_internal bool8_t vkr_bakery_index_grow(VkrBakeryIndex *index) {
  const uint32_t old_capacity = index->capacity;
  VkrBakeryIndexEntry *old_slots = index->slots;
  const uint32_t capacity = old_capacity ? old_capacity * 2u : 1024u;
  VkrBakeryIndexEntry *slots =
      (VkrBakeryIndexEntry *)calloc(capacity, sizeof(VkrBakeryIndexEntry));
  if (!slots) {
    return false_v;
  }
  index->slots = slots;
  index->capacity = capacity;
  for (uint32_t i = 0u; i < old_capacity; ++i) {
    if (old_slots[i].path) {
      *vkr_bakery_index_slot(index, old_slots[i].path) = old_slots[i];
    }
  }
  free(old_slots);
  return true_v;
}

vkr_internal VkrBakeryIndexEntry *vkr_bakery_index_put(VkrBakeryIndex *index,
                                                       const char *path) {
  if ((index->count + 1u) * 4u >= index->capacity * 3u &&
      !vkr_bakery_index_grow(index)) {
    return NULL;
  }
  VkrBakeryIndexEntry *entry = vkr_bakery_index_slot(index, path);
  if (!entry->path) {
    entry->path = strdup(path);
    if (!entry->path) {
      return NULL;
    }
    index->count += 1u;
  }
  return entry;
}

vkr_internal void vkr_bakery_index_load(VkrBakeryIndex *index) {
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(index->file_path, GB(1), &data, &length)) {
    return;
  }
  const uint64_t header = strlen(VKR_BAKERY_INDEX_FORMAT);
  if (length < header || MemCompare(data, VKR_BAKERY_INDEX_FORMAT, header)) {
    free(data);
    return; /* Unknown format: start empty. */
  }
  char *cursor = (char *)data + header;
  while (*cursor) {
    char *end = strchr(cursor, '\n');
    if (!end) {
      break;
    }
    *end = 0;
    unsigned long long size = 0;
    long long mtime = 0;
    unsigned long long device = 0;
    unsigned long long file_id = 0;
    char hash[VKR_BAKERY_KEY_SIZE];
    int consumed = 0;
    if (sscanf(cursor, "%64s %llu %lld %llu %llu %n", hash, &size, &mtime,
               &device, &file_id, &consumed) == 5 &&
        consumed > 0 && strlen(hash) == 64u) {
      VkrBakeryIndexEntry *entry =
          vkr_bakery_index_put(index, cursor + consumed);
      if (entry) {
        entry->size = size;
        entry->mtime_ns = mtime;
        entry->device = device;
        entry->file_id = file_id;
        MemCopy(entry->hash, hash, VKR_BAKERY_KEY_SIZE);
      }
    }
    cursor = end + 1;
  }
  free(data);
  index->dirty = false_v;
}

VkrBakeryIndex *vkr_bakery_index_open(const char *cache_dir) {
  VkrBakeryIndex *index = (VkrBakeryIndex *)calloc(1u, sizeof(VkrBakeryIndex));
  if (!index) {
    return NULL;
  }
  if (cache_dir && cache_dir[0]) {
    (void)snprintf(index->file_path, sizeof(index->file_path),
                   "%s/index/paths.v1", cache_dir);
  }
  if (!vkr_bakery_index_grow(index)) {
    free(index);
    return NULL;
  }
  if (index->file_path[0]) {
    vkr_bakery_index_load(index);
  }
  return index;
}

/* A hard link, or a file moved within its volume, keeps its volume, file
 * identity, size and modification time, so another path's entry with all
 * four equal names the same bytes: a revision of linked files is indexed
 * without reading them. Returns that entry's hash, or NULL. */
vkr_internal const char *vkr_bakery_index_same_file(const VkrBakeryIndex *index,
                                                    const VkrBakeryStat *info) {
  if (!info->file_id) {
    return NULL;
  }
  for (uint32_t i = 0u; i < index->capacity; ++i) {
    const VkrBakeryIndexEntry *entry = &index->slots[i];
    if (entry->path && entry->hash[0] && entry->file_id == info->file_id &&
        entry->device == info->device && entry->size == info->size &&
        entry->mtime_ns == info->mtime_ns) {
      return entry->hash;
    }
  }
  return NULL;
}

/* Gives `entry` the hash of the same file under another path, if any. */
vkr_internal bool8_t vkr_bakery_index_adopt_same_file(
    VkrBakeryIndex *index, VkrBakeryIndexEntry *entry,
    const VkrBakeryStat *info) {
  const char *hash = vkr_bakery_index_same_file(index, info);
  if (!entry || !hash) {
    return false_v;
  }
  char copied[VKR_BAKERY_KEY_SIZE];
  MemCopy(copied, hash, VKR_BAKERY_KEY_SIZE);
  entry->size = info->size;
  entry->mtime_ns = info->mtime_ns;
  entry->device = info->device;
  entry->file_id = info->file_id;
  MemCopy(entry->hash, copied, VKR_BAKERY_KEY_SIZE);
  index->dirty = true_v;
  return true_v;
}

bool8_t vkr_bakery_index_hash(VkrBakeryIndex *index, const char *path,
                              char out_hash[VKR_BAKERY_KEY_SIZE],
                              uint64_t *out_size) {
  VkrBakeryStat info;
  if (!vkr_bakery_stat(path, &info) || !info.exists || info.is_directory) {
    return false_v;
  }
  VkrBakeryIndexEntry *entry = vkr_bakery_index_put(index, path);
  if ((entry && entry->hash[0] && entry->size == info.size &&
       entry->mtime_ns == info.mtime_ns && entry->device == info.device &&
       entry->file_id == info.file_id) ||
      vkr_bakery_index_adopt_same_file(index, entry, &info)) {
    MemCopy(out_hash, entry->hash, VKR_BAKERY_KEY_SIZE);
    if (out_size) {
      *out_size = info.size;
    }
    return true_v;
  }
  uint64_t size = 0u;
  if (!vkr_bakery_hash_file(path, out_hash, &size)) {
    return false_v;
  }
  /* A file rewritten while it was hashed keeps no index entry. */
  VkrBakeryStat after;
  if (entry && vkr_bakery_stat(path, &after) && after.exists &&
      after.size == info.size && after.mtime_ns == info.mtime_ns) {
    entry->size = info.size;
    entry->mtime_ns = info.mtime_ns;
    entry->device = info.device;
    entry->file_id = info.file_id;
    MemCopy(entry->hash, out_hash, VKR_BAKERY_KEY_SIZE);
    index->dirty = true_v;
  }
  if (out_size) {
    *out_size = size;
  }
  return true_v;
}

typedef struct VkrBakeryIndexMiss {
  const char *path;
  VkrBakeryStat before;
  char hash[VKR_BAKERY_KEY_SIZE];
  bool8_t hashed;
} VkrBakeryIndexMiss;

typedef struct VkrBakeryIndexPrefetch {
  VkrBakeryIndexMiss *misses;
  uint32_t count;
  VkrAtomicUint32 next;
} VkrBakeryIndexPrefetch;

vkr_internal void *vkr_bakery_index_prefetch_worker(void *argument) {
  VkrBakeryIndexPrefetch *prefetch = argument;
  for (;;) {
    const uint32_t i = vkr_atomic_uint32_fetch_add(&prefetch->next, 1u,
                                                   VKR_MEMORY_ORDER_RELAXED);
    if (i >= prefetch->count) {
      return NULL;
    }
    VkrBakeryIndexMiss *miss = &prefetch->misses[i];
    miss->hashed = vkr_bakery_hash_file(miss->path, miss->hash, NULL);
  }
}

void vkr_bakery_index_prefetch(VkrBakeryIndex *index, const char *const *paths,
                               uint32_t count) {
  VkrBakeryIndexMiss *misses =
      count ? (VkrBakeryIndexMiss *)calloc(count, sizeof(*misses)) : NULL;
  if (!index || !misses) {
    free(misses);
    return;
  }
  uint32_t miss_count = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    VkrBakeryStat info;
    if (!vkr_bakery_stat(paths[i], &info) || !info.exists ||
        info.is_directory) {
      continue;
    }
    VkrBakeryIndexEntry *entry = vkr_bakery_index_put(index, paths[i]);
    if ((entry && entry->hash[0] && entry->size == info.size &&
         entry->mtime_ns == info.mtime_ns && entry->device == info.device &&
         entry->file_id == info.file_id) ||
        vkr_bakery_index_adopt_same_file(index, entry, &info)) {
      continue;
    }
    misses[miss_count++] =
        (VkrBakeryIndexMiss){.path = paths[i], .before = info};
  }

  VkrBakeryIndexPrefetch prefetch = {.misses = misses, .count = miss_count};
  Arena *arena = miss_count > 1u ? arena_create(KB(64), KB(64)) : NULL;
  VkrAllocator allocator = {.ctx = arena};
  VkrThread workers[15] = {0};
  uint32_t started = 0u;
  if (arena && vkr_allocator_arena(&allocator)) {
    const uint32_t extra = Min(Min(vkr_bakery_logical_cores(), miss_count),
                               (uint32_t)ArrayCount(workers) + 1u) -
                           1u;
    for (uint32_t i = 0u; i < extra; ++i) {
      if (vkr_thread_create(&allocator, &workers[started],
                            vkr_bakery_index_prefetch_worker, &prefetch)) {
        ++started;
      }
    }
  }
  (void)vkr_bakery_index_prefetch_worker(&prefetch);
  for (uint32_t i = 0u; i < started; ++i) {
    (void)vkr_thread_join(workers[i]);
    (void)vkr_thread_destroy(&allocator, &workers[i]);
  }
  if (arena) {
    arena_destroy(arena);
  }

  /* Recorded as vkr_bakery_index_hash records: only a file unchanged while
     it was hashed keeps the entry. */
  for (uint32_t i = 0u; i < miss_count; ++i) {
    const VkrBakeryIndexMiss *miss = &misses[i];
    VkrBakeryStat after;
    VkrBakeryIndexEntry *entry = vkr_bakery_index_put(index, miss->path);
    if (!miss->hashed || !entry || !vkr_bakery_stat(miss->path, &after) ||
        !after.exists || after.size != miss->before.size ||
        after.mtime_ns != miss->before.mtime_ns) {
      continue;
    }
    entry->size = miss->before.size;
    entry->mtime_ns = miss->before.mtime_ns;
    entry->device = miss->before.device;
    entry->file_id = miss->before.file_id;
    MemCopy(entry->hash, miss->hash, VKR_BAKERY_KEY_SIZE);
    index->dirty = true_v;
  }
  free(misses);
}

void vkr_bakery_index_move_tree(VkrBakeryIndex *index, const char *from,
                                const char *to) {
  if (!index || !index->count) {
    return;
  }
  const uint64_t from_length = strlen(from);
  uint32_t moved_count = 0u;
  VkrBakeryIndexEntry *moved =
      (VkrBakeryIndexEntry *)calloc(index->count, sizeof(*moved));
  if (!moved) {
    return;
  }
  for (uint32_t i = 0u; i < index->capacity; ++i) {
    VkrBakeryIndexEntry *entry = &index->slots[i];
    if (entry->path && entry->hash[0] &&
        strncmp(entry->path, from, from_length) == 0 &&
        (entry->path[from_length] == '/' || !entry->path[from_length])) {
      moved[moved_count] = *entry;
      moved[moved_count].path = entry->path;
      ++moved_count;
      /* The old path no longer exists; its entry keeps no hash. */
      entry->hash[0] = '\0';
    }
  }
  for (uint32_t i = 0u; i < moved_count; ++i) {
    char path[VKR_BAKERY_PATH_CAPACITY];
    const int written =
        snprintf(path, sizeof(path), "%s%s", to, moved[i].path + from_length);
    VkrBakeryIndexEntry *entry = written > 0 && (size_t)written < sizeof(path)
                                     ? vkr_bakery_index_put(index, path)
                                     : NULL;
    if (entry) {
      entry->size = moved[i].size;
      entry->mtime_ns = moved[i].mtime_ns;
      entry->device = moved[i].device;
      entry->file_id = moved[i].file_id;
      MemCopy(entry->hash, moved[i].hash, VKR_BAKERY_KEY_SIZE);
      index->dirty = true_v;
    }
  }
  free(moved);
}

void vkr_bakery_index_record(VkrBakeryIndex *index, const char *path,
                             const char *hash) {
  VkrBakeryStat info;
  if (!vkr_bakery_stat(path, &info) || !info.exists) {
    return;
  }
  VkrBakeryIndexEntry *entry = vkr_bakery_index_put(index, path);
  if (!entry) {
    return;
  }
  entry->size = info.size;
  entry->mtime_ns = info.mtime_ns;
  entry->device = info.device;
  entry->file_id = info.file_id;
  (void)snprintf(entry->hash, sizeof(entry->hash), "%s", hash);
  index->dirty = true_v;
}

void vkr_bakery_index_close(VkrBakeryIndex *index) {
  if (!index) {
    return;
  }
  if (index->dirty && index->file_path[0]) {
    VkrBakeryBuffer buffer = {0};
    vkr_bakery_buffer_append_cstr(&buffer, VKR_BAKERY_INDEX_FORMAT);
    for (uint32_t i = 0u; i < index->capacity; ++i) {
      const VkrBakeryIndexEntry *entry = &index->slots[i];
      if (!entry->path || !entry->hash[0] || strchr(entry->path, '\n')) {
        continue;
      }
      /* Drop entries for files that no longer exist. */
      if (!vkr_bakery_is_file(entry->path)) {
        continue;
      }
      vkr_bakery_buffer_appendf(
          &buffer, "%s %llu %lld %llu %llu %s\n", entry->hash,
          (unsigned long long)entry->size, (long long)entry->mtime_ns,
          (unsigned long long)entry->device, (unsigned long long)entry->file_id,
          entry->path);
    }
    if (!buffer.failed) {
      (void)vkr_bakery_write_file_atomic(index->file_path, buffer.data,
                                         buffer.length);
    }
    vkr_bakery_buffer_free(&buffer);
  }
  for (uint32_t i = 0u; i < index->capacity; ++i) {
    free(index->slots[i].path);
  }
  free(index->slots);
  free(index);
}
