#include "vkr_bakery_commands.h"

#include "platform/vkr_platform.h"

#include "vkr_bakery_buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <utime.h>
#endif

/* Cache layout (docs/proposals/asset-build-system.md section 8.1):
 *   version                      "1"
 *   cas/<h[0:2]>/<h>             immutable product bytes
 *   actions/<k[0:2]>/<k>.json    result record for one full key
 *   manifests/<p[0:2]>/<p>.json  depfile variants recorded for one prekey
 *   index/paths.v1               source hash index
 *   tmp/                         per-action staging directories
 * Every file is published by an atomic rename, so concurrent processes can
 * share one cache; a lost manifest update only costs a rebuild. */

#define VKR_BAKERY_MANIFEST_VARIANTS 8u

bool8_t vkr_bakery_cache_prepare(const VkrBakeryConfig *config) {
  static const char *const directories[] = {"cas", "actions", "manifests",
                                            "index", "tmp"};
  char path[VKR_BAKERY_PATH_CAPACITY];
  for (uint32_t i = 0u; i < ArrayCount(directories); ++i) {
    if (!vkr_bakery_path_join(path, sizeof(path), config->cache_dir,
                              directories[i]) ||
        !vkr_bakery_make_directories(path)) {
      return false_v;
    }
  }
  if (!vkr_bakery_path_join(path, sizeof(path), config->cache_dir, "version")) {
    return false_v;
  }
  if (!vkr_bakery_is_file(path)) {
    return vkr_bakery_write_file_atomic(path, VKR_BAKERY_CACHE_VERSION "\n",
                                        2u);
  }
  return true_v;
}

void vkr_bakery_cas_path(const VkrBakeryConfig *config, const char *hash,
                         char *out, uint32_t capacity) {
  (void)snprintf(out, capacity, "%s/cas/%.2s/%s", config->cache_dir, hash,
                 hash);
}

void vkr_bakery_action_record_path(const VkrBakeryConfig *config,
                                   const char *key, char *out,
                                   uint32_t capacity) {
  (void)snprintf(out, capacity, "%s/actions/%.2s/%s.json", config->cache_dir,
                 key, key);
}

vkr_internal void vkr_bakery_manifest_path(const VkrBakeryConfig *config,
                                           const char *prekey, char *out,
                                           uint32_t capacity) {
  (void)snprintf(out, capacity, "%s/manifests/%.2s/%s.json", config->cache_dir,
                 prekey, prekey);
}

void vkr_bakery_full_key(const char *prekey, const VkrBakeryJson *depfile,
                         Arena *arena, char out_key[VKR_BAKERY_KEY_SIZE]) {
  String8 text = {0};
  VkrBakeryBuffer buffer = {0};
  vkr_bakery_buffer_append(&buffer, prekey, 64u);
  if (depfile && depfile->count &&
      vkr_bakery_json_write(arena, depfile, VKR_BAKERY_JSON_CANONICAL, &text)) {
    vkr_bakery_buffer_append(&buffer, text.str, text.length);
  } else {
    vkr_bakery_buffer_append(&buffer, "[]", 2u);
  }
  vkr_bakery_hash_bytes(buffer.data, buffer.length, out_key);
  vkr_bakery_buffer_free(&buffer);
}

vkr_internal VkrBakeryJson *vkr_bakery_read_json(Arena *arena,
                                                 const char *path) {
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(path, MB(64), &data, &length)) {
    return NULL;
  }
  VkrBakeryJson *value = vkr_bakery_json_parse(arena, data, length, 64u, NULL);
  free(data);
  return value;
}

vkr_internal void vkr_bakery_touch(const char *path) {
#if defined(_WIN32)
  (void)path; /* Windows gc relies on the record's creation time. */
#else
  (void)utime(path, NULL);
#endif
}

vkr_internal bool8_t vkr_bakery_depfile_matches(VkrBakeryGraph *graph,
                                                const VkrBakeryJson *depfile) {
  for (const VkrBakeryJson *entry = depfile ? depfile->first : NULL; entry;
       entry = entry->next) {
    const VkrBakeryJson *path = vkr_bakery_json_at(entry, 0u);
    const VkrBakeryJson *hash = vkr_bakery_json_at(entry, 1u);
    if (!path || !hash || path->type != VKR_BAKERY_JSON_STRING ||
        hash->type != VKR_BAKERY_JSON_STRING) {
      return false_v;
    }
    char current[VKR_BAKERY_KEY_SIZE];
    if (!vkr_bakery_index_hash(graph->index, (const char *)path->string.str,
                               current, NULL) ||
        !vkr_bakery_json_is_string(hash, current)) {
      return false_v;
    }
  }
  return true_v;
}

/* Fills the action's products from a result record whose CAS files exist. */
vkr_internal bool8_t vkr_bakery_cache_load_record(VkrBakeryGraph *graph,
                                                  VkrBakeryAction *action,
                                                  const char *key) {
  char record_path[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_action_record_path(graph->config, key, record_path,
                                sizeof(record_path));
  const uint64_t mark = arena_pos(graph->arena);
  VkrBakeryJson *record = vkr_bakery_read_json(graph->arena, record_path);
  const VkrBakeryJson *products = vkr_bakery_json_get(record, "products");
  if (!products || products->type != VKR_BAKERY_JSON_ARRAY) {
    arena_reset_to(graph->arena, mark, ARENA_MEMORY_TAG_STRUCT);
    return false_v;
  }
  for (uint32_t p = 0u; p < action->product_count; ++p) {
    free(action->products[p].destination);
  }
  action->product_count = 0u;
  for (const VkrBakeryJson *item = products->first; item; item = item->next) {
    String8 role = {0};
    String8 hash = {0};
    int64_t bytes = 0;
    if (!vkr_bakery_json_get_string(item, "role", &role) ||
        !vkr_bakery_json_get_string(item, "hash", &hash) ||
        !vkr_bakery_json_get_int(item, "bytes", &bytes) || hash.length != 64u ||
        role.length >= sizeof(action->products[0].role)) {
      return false_v;
    }
    VkrBakeryProduct *product = vkr_bakery_action_new_product(action);
    if (!product) {
      return false_v;
    }
    MemCopy(product->role, role.str, role.length);
    String8 destination = {0};
    if (vkr_bakery_json_get_string(item, "destination", &destination)) {
      product->destination = strdup((const char *)destination.str);
    }
    MemCopy(product->hash, hash.str, 64u);
    product->bytes = (uint64_t)bytes;
    vkr_bakery_cas_path(graph->config, product->hash, product->path,
                        sizeof(product->path));
    VkrBakeryStat info;
    if (!vkr_bakery_stat(product->path, &info) || !info.exists ||
        info.size != product->bytes) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_CACHE_CORRUPT, action->display,
                            0u, 0u,
                            "cached product is missing from the store; "
                            "rebuilding",
                            NULL);
      return false_v;
    }
  }
  (void)snprintf(action->key, sizeof(action->key), "%s", key);
  vkr_bakery_touch(record_path);
  return true_v;
}

bool8_t vkr_bakery_cache_lookup(VkrBakeryGraph *graph,
                                VkrBakeryAction *action) {
  if (graph->config->no_cache || action->force) {
    return false_v;
  }
  char manifest_path[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_manifest_path(graph->config, action->prekey, manifest_path,
                           sizeof(manifest_path));
  VkrBakeryJson *manifest = vkr_bakery_read_json(graph->arena, manifest_path);
  const VkrBakeryJson *variants = vkr_bakery_json_get(manifest, "variants");
  if (!variants || variants->type != VKR_BAKERY_JSON_ARRAY) {
    return false_v;
  }
  for (const VkrBakeryJson *variant = variants->first; variant;
       variant = variant->next) {
    const VkrBakeryJson *depfile = vkr_bakery_json_get(variant, "depfile");
    String8 key = {0};
    if (!vkr_bakery_json_get_string(variant, "key", &key) ||
        key.length != 64u || !vkr_bakery_depfile_matches(graph, depfile)) {
      continue;
    }
    char expected[VKR_BAKERY_KEY_SIZE];
    vkr_bakery_full_key(action->prekey, depfile, graph->arena, expected);
    if (MemCompare(expected, key.str, 64u) != 0) {
      continue; /* A tampered variant never aliases another key. */
    }
    if (vkr_bakery_cache_load_record(graph, action, expected)) {
      return true_v;
    }
  }
  return false_v;
}

// =============================================================================
// Store (worker thread)
// =============================================================================

vkr_internal int vkr_bakery_compare_paths(const void *lhs, const void *rhs) {
  return strcmp(*(const char *const *)lhs, *(const char *const *)rhs);
}

vkr_internal bool8_t vkr_bakery_cas_insert(const VkrBakeryConfig *config,
                                           VkrBakeryProduct *product) {
  char cas_path[VKR_BAKERY_PATH_CAPACITY];
  uint64_t size = 0u;
  if (!vkr_bakery_hash_file(product->path, product->hash, &size)) {
    return false_v;
  }
  product->bytes = size;
  vkr_bakery_cas_path(config, product->hash, cas_path, sizeof(cas_path));
  char directory[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), cas_path);
  if (!vkr_bakery_make_directories(directory)) {
    return false_v;
  }
  VkrBakeryStat existing;
  if (vkr_bakery_stat(cas_path, &existing) && existing.exists &&
      existing.size == size) {
    (void)vkr_bakery_remove_file(product->path);
  } else if (!vkr_bakery_rename(product->path, cas_path, true_v)) {
    return false_v;
  } else {
#if !defined(_WIN32)
    (void)chmod(cas_path, 0444);
#endif
  }
  (void)snprintf(product->path, sizeof(product->path), "%s", cas_path);
  return true_v;
}

bool8_t vkr_bakery_cache_store(const VkrBakeryConfig *config,
                               VkrBakeryAction *action, Arena *scratch) {
  for (uint32_t i = 0u; i < action->product_count; ++i) {
    if (!vkr_bakery_cas_insert(config, &action->products[i])) {
      (void)snprintf(action->failure, sizeof(action->failure),
                     "could not store product '%s' in the cache",
                     action->products[i].role);
      return false_v;
    }
  }
  /* Depfile entries are hashed here, directly, so the record reflects the
   * inputs the producer actually read. */
  VkrBakeryJson *depfile = vkr_bakery_json_array(scratch);
  if (action->discovered_count) {
    qsort(action->discovered, action->discovered_count, sizeof(char *),
          vkr_bakery_compare_paths);
  }
  for (uint32_t i = 0u; i < action->discovered_count; ++i) {
    if (i && strcmp(action->discovered[i], action->discovered[i - 1u]) == 0) {
      continue;
    }
    char hash[VKR_BAKERY_KEY_SIZE];
    if (!vkr_bakery_hash_file(action->discovered[i], hash, NULL)) {
      continue; /* A vanished include cannot validate a later lookup. */
    }
    VkrBakeryJson *entry = vkr_bakery_json_array(scratch);
    vkr_bakery_json_append(
        entry, vkr_bakery_json_cstr(scratch, action->discovered[i]));
    vkr_bakery_json_append(entry, vkr_bakery_json_cstr(scratch, hash));
    vkr_bakery_json_append(depfile, entry);
  }
  vkr_bakery_full_key(action->prekey, depfile, scratch, action->key);

  if (config->no_cache) {
    return true_v;
  }
  char timestamp[32];
  vkr_bakery_utc_timestamp(timestamp);
  VkrBakeryJson *record = vkr_bakery_json_object(scratch);
  vkr_bakery_json_set(scratch, record, "v", vkr_bakery_json_int(scratch, 1));
  vkr_bakery_json_set(scratch, record, "key",
                      vkr_bakery_json_cstr(scratch, action->key));
  vkr_bakery_json_set(scratch, record, "prekey",
                      vkr_bakery_json_cstr(scratch, action->prekey));
  vkr_bakery_json_set(scratch, record, "producer",
                      vkr_bakery_json_cstr(scratch, action->producer->id));
  vkr_bakery_json_set(scratch, record, "producer_version",
                      vkr_bakery_json_int(scratch, action->producer->version));
  vkr_bakery_json_set(
      scratch, record, "tool",
      vkr_bakery_json_cstr(scratch, action->producer->identity));
  vkr_bakery_json_set(
      scratch, record, "source",
      vkr_bakery_json_cstr(scratch, action->display ? action->display : ""));
  vkr_bakery_json_set(scratch, record, "recipe",
                      action->recipe
                          ? vkr_bakery_json_clone(scratch, action->recipe)
                          : vkr_bakery_json_object(scratch));
  VkrBakeryJson *inputs = vkr_bakery_json_array(scratch);
  for (uint32_t i = 0u; i < action->input_count; ++i) {
    VkrBakeryJson *input = vkr_bakery_json_object(scratch);
    vkr_bakery_json_set(scratch, input, "name",
                        vkr_bakery_json_cstr(scratch, action->inputs[i].name));
    vkr_bakery_json_set(scratch, input, "hash",
                        vkr_bakery_json_cstr(scratch, action->inputs[i].hash));
    vkr_bakery_json_append(inputs, input);
  }
  vkr_bakery_json_set(scratch, record, "inputs", inputs);
  vkr_bakery_json_set(scratch, record, "depfile", depfile);
  VkrBakeryJson *products = vkr_bakery_json_array(scratch);
  for (uint32_t i = 0u; i < action->product_count; ++i) {
    VkrBakeryJson *product = vkr_bakery_json_object(scratch);
    vkr_bakery_json_set(
        scratch, product, "role",
        vkr_bakery_json_cstr(scratch, action->products[i].role));
    vkr_bakery_json_set(
        scratch, product, "hash",
        vkr_bakery_json_cstr(scratch, action->products[i].hash));
    vkr_bakery_json_set(
        scratch, product, "bytes",
        vkr_bakery_json_int(scratch, (int64_t)action->products[i].bytes));
    if (action->products[i].destination) {
      vkr_bakery_json_set(
          scratch, product, "destination",
          vkr_bakery_json_cstr(scratch, action->products[i].destination));
    }
    vkr_bakery_json_append(products, product);
  }
  vkr_bakery_json_set(scratch, record, "products", products);
  vkr_bakery_json_set(scratch, record, "wall_ms",
                      vkr_bakery_json_int(scratch, (int64_t)action->wall_ms));
  vkr_bakery_json_set(scratch, record, "cpu_ms",
                      vkr_bakery_json_int(scratch, (int64_t)action->cpu_ms));
  vkr_bakery_json_set(
      scratch, record, "peak_rss_mib",
      vkr_bakery_json_int(scratch,
                          (int64_t)(action->peak_rss_bytes / (1024u * 1024u))));
  vkr_bakery_json_set(scratch, record, "created",
                      vkr_bakery_json_cstr(scratch, timestamp));
  String8 text = {0};
  char record_path[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_action_record_path(config, action->key, record_path,
                                sizeof(record_path));
  if (!vkr_bakery_json_write(scratch, record, VKR_BAKERY_JSON_PRETTY, &text) ||
      !vkr_bakery_write_file_atomic(record_path, text.str, text.length)) {
    (void)snprintf(action->failure, sizeof(action->failure),
                   "could not write the action record");
    return false_v;
  }

  /* Most recent variant first; older depfile variants stay reachable. */
  char manifest_path[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_manifest_path(config, action->prekey, manifest_path,
                           sizeof(manifest_path));
  VkrBakeryJson *manifest = vkr_bakery_json_object(scratch);
  VkrBakeryJson *variants = vkr_bakery_json_array(scratch);
  VkrBakeryJson *variant = vkr_bakery_json_object(scratch);
  vkr_bakery_json_set(scratch, variant, "key",
                      vkr_bakery_json_cstr(scratch, action->key));
  vkr_bakery_json_set(scratch, variant, "depfile", depfile);
  vkr_bakery_json_append(variants, variant);
  VkrBakeryJson *previous = vkr_bakery_read_json(scratch, manifest_path);
  const VkrBakeryJson *old_variants = vkr_bakery_json_get(previous, "variants");
  for (VkrBakeryJson *old = old_variants ? old_variants->first : NULL;
       old && variants->count < VKR_BAKERY_MANIFEST_VARIANTS;) {
    VkrBakeryJson *next = old->next;
    String8 key = {0};
    if (vkr_bakery_json_get_string(old, "key", &key) &&
        !(key.length == 64u && MemCompare(key.str, action->key, 64u) == 0)) {
      vkr_bakery_json_append(variants, old);
    }
    old = next;
  }
  vkr_bakery_json_set(scratch, manifest, "v", vkr_bakery_json_int(scratch, 1));
  vkr_bakery_json_set(scratch, manifest, "producer",
                      vkr_bakery_json_cstr(scratch, action->producer->id));
  vkr_bakery_json_set(scratch, manifest, "variants", variants);
  if (!vkr_bakery_json_write(scratch, manifest, VKR_BAKERY_JSON_COMPACT,
                             &text) ||
      !vkr_bakery_write_file_atomic(manifest_path, text.str, text.length)) {
    (void)snprintf(action->failure, sizeof(action->failure),
                   "could not write the cache manifest");
    return false_v;
  }
  return true_v;
}

// =============================================================================
// Garbage collection
// =============================================================================

typedef struct VkrBakeryHashSet {
  char (*hashes)[VKR_BAKERY_KEY_SIZE];
  uint32_t count;
  uint32_t capacity;
} VkrBakeryHashSet;

vkr_internal bool8_t vkr_bakery_hash_set_add(VkrBakeryHashSet *set,
                                             const char *hash) {
  if (set->count == set->capacity) {
    const uint32_t capacity = set->capacity ? set->capacity * 2u : 1024u;
    char(*grown)[VKR_BAKERY_KEY_SIZE] =
        realloc(set->hashes, sizeof(*set->hashes) * (size_t)capacity);
    if (!grown) {
      return false_v;
    }
    set->hashes = grown;
    set->capacity = capacity;
  }
  (void)snprintf(set->hashes[set->count++], VKR_BAKERY_KEY_SIZE, "%s", hash);
  return true_v;
}

vkr_internal int vkr_bakery_compare_hash(const void *lhs, const void *rhs) {
  return strcmp((const char *)lhs, (const char *)rhs);
}

typedef struct VkrBakeryGcWalk {
  const VkrBakeryConfig *config;
  const char *directory;
  int64_t cutoff;
  bool8_t dry_run;
  VkrBakeryGcStats *stats;
  VkrBakeryHashSet *live;
  bool8_t ok;
} VkrBakeryGcWalk;

vkr_internal bool8_t vkr_bakery_gc_records(void *context, const char *name,
                                           bool8_t is_directory) {
  VkrBakeryGcWalk *walk = (VkrBakeryGcWalk *)context;
  char path[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_join(path, sizeof(path), walk->directory, name)) {
    return true_v;
  }
  if (is_directory) {
    VkrBakeryGcWalk child = *walk;
    child.directory = path;
    (void)vkr_bakery_list_directory(path, vkr_bakery_gc_records, &child);
    walk->ok = walk->ok && child.ok;
    return true_v;
  }
  VkrBakeryStat info;
  if (!vkr_bakery_stat(path, &info) || !info.exists) {
    return true_v;
  }
  if (info.mtime_ns / 1000000000LL < walk->cutoff) {
    walk->stats->records_removed += 1u;
    if (!walk->dry_run) {
      (void)vkr_bakery_remove_file(path);
    }
    return true_v;
  }
  walk->stats->records_kept += 1u;
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(path, MB(64), &data, &length)) {
    return true_v;
  }
  /* Scan for product hashes without building a tree per record. */
  const char *cursor = (const char *)data;
  const char *products = strstr(cursor, "\"products\"");
  while (products && (cursor = strstr(products, "\"hash\": \"")) != NULL) {
    cursor += 9;
    char hash[VKR_BAKERY_KEY_SIZE];
    (void)snprintf(hash, sizeof(hash), "%.64s", cursor);
    walk->ok = vkr_bakery_hash_set_add(walk->live, hash) && walk->ok;
    products = cursor;
  }
  free(data);
  return true_v;
}

vkr_internal bool8_t vkr_bakery_gc_products(void *context, const char *name,
                                            bool8_t is_directory) {
  VkrBakeryGcWalk *walk = (VkrBakeryGcWalk *)context;
  char path[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_join(path, sizeof(path), walk->directory, name)) {
    return true_v;
  }
  if (is_directory) {
    VkrBakeryGcWalk child = *walk;
    child.directory = path;
    (void)vkr_bakery_list_directory(path, vkr_bakery_gc_products, &child);
    return true_v;
  }
  if (strlen(name) == 64u &&
      bsearch(name, walk->live->hashes, walk->live->count,
              sizeof(*walk->live->hashes), vkr_bakery_compare_hash)) {
    return true_v;
  }
  VkrBakeryStat info;
  if (vkr_bakery_stat(path, &info) && info.exists) {
    walk->stats->products_removed += 1u;
    walk->stats->bytes_removed += info.size;
    if (!walk->dry_run) {
      (void)vkr_bakery_remove_file(path);
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_bakery_gc_manifests(void *context, const char *name,
                                             bool8_t is_directory) {
  VkrBakeryGcWalk *walk = (VkrBakeryGcWalk *)context;
  char path[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_join(path, sizeof(path), walk->directory, name)) {
    return true_v;
  }
  if (is_directory) {
    VkrBakeryGcWalk child = *walk;
    child.directory = path;
    (void)vkr_bakery_list_directory(path, vkr_bakery_gc_manifests, &child);
    return true_v;
  }
  /* A manifest whose every recorded key lost its record is dropped. */
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(path, MB(16), &data, &length)) {
    return true_v;
  }
  bool8_t live = false_v;
  const char *cursor = (const char *)data;
  while (!live && (cursor = strstr(cursor, "\"key\":\"")) != NULL) {
    cursor += 7;
    char key[VKR_BAKERY_KEY_SIZE];
    (void)snprintf(key, sizeof(key), "%.64s", cursor);
    char record[VKR_BAKERY_PATH_CAPACITY];
    vkr_bakery_action_record_path(walk->config, key, record, sizeof(record));
    live = vkr_bakery_is_file(record);
  }
  free(data);
  if (!live && !walk->dry_run) {
    (void)vkr_bakery_remove_file(path);
  }
  return true_v;
}

vkr_internal bool8_t vkr_bakery_gc_staging(void *context, const char *name,
                                           bool8_t is_directory) {
  VkrBakeryGcWalk *walk = (VkrBakeryGcWalk *)context;
  char path[VKR_BAKERY_PATH_CAPACITY];
  VkrBakeryStat info;
  if (!vkr_bakery_path_join(path, sizeof(path), walk->directory, name) ||
      !vkr_bakery_stat(path, &info) || !info.exists) {
    return true_v;
  }
  (void)is_directory;
  /* Staging of a crashed run is abandoned after a day. */
  if (info.mtime_ns / 1000000000LL < vkr_bakery_unix_seconds() - 86400) {
    walk->stats->staging_removed += 1u;
    if (!walk->dry_run) {
      (void)vkr_bakery_remove_tree(path);
    }
  }
  return true_v;
}

bool8_t vkr_bakery_cache_gc(const VkrBakeryConfig *config, uint64_t days,
                            bool8_t dry_run, VkrBakeryGcStats *out_stats) {
  MemZero(out_stats, sizeof(*out_stats));
  if (!vkr_bakery_cache_prepare(config)) {
    return false_v;
  }
  VkrPlatformProcessLock lock;
  if (!vkr_platform_process_lock_acquire("vkr_bakery_gc", config->cache_dir,
                                         &lock)) {
    return false_v;
  }
  VkrBakeryHashSet live = {0};
  char directory[VKR_BAKERY_PATH_CAPACITY];
  VkrBakeryGcWalk walk = {
      .config = config,
      .cutoff = vkr_bakery_unix_seconds() - (int64_t)(days * 86400u),
      .dry_run = dry_run,
      .stats = out_stats,
      .live = &live,
      .ok = true_v,
  };
  (void)vkr_bakery_path_join(directory, sizeof(directory), config->cache_dir,
                             "actions");
  walk.directory = directory;
  (void)vkr_bakery_list_directory(directory, vkr_bakery_gc_records, &walk);
  if (walk.ok) {
    qsort(live.hashes, live.count, sizeof(*live.hashes),
          vkr_bakery_compare_hash);
    (void)vkr_bakery_path_join(directory, sizeof(directory), config->cache_dir,
                               "cas");
    (void)vkr_bakery_list_directory(directory, vkr_bakery_gc_products, &walk);
  }
  (void)vkr_bakery_path_join(directory, sizeof(directory), config->cache_dir,
                             "manifests");
  (void)vkr_bakery_list_directory(directory, vkr_bakery_gc_manifests, &walk);
  (void)vkr_bakery_path_join(directory, sizeof(directory), config->cache_dir,
                             "tmp");
  (void)vkr_bakery_list_directory(directory, vkr_bakery_gc_staging, &walk);
  free(live.hashes);
  vkr_platform_process_lock_release(&lock);
  return walk.ok;
}
