#include "vkr_depot_store.h"

#include "core/vkr_byte_io.h"
#include "vkr_bakery_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORED_RAW 0u
#define STORED_ZSTD 1u
#define STORE_COMPRESSION_LEVEL 3

bool8_t vkr_depot_store_open(VkrDepotStore *store, const char *root,
                             const VkrDepotCodec *codec) {
  MemZero(store, sizeof(*store));
  if (!root || strlen(root) >= sizeof(store->root)) {
    return false_v;
  }
  snprintf(store->root, sizeof(store->root), "%s", root);
  vkr_bakery_path_portable(store->root);
  store->codec = codec;
  char objects[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(objects, sizeof(objects), store->root, "objects")) {
    return false_v;
  }
  return vkr_bakery_make_directories(objects);
}

void vkr_depot_store_path(const VkrDepotStore *store, const VkrDepotId *id,
                          char *out, uint32_t capacity) {
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(id, hex);
  snprintf(out, capacity, "%s/objects/%.2s/%s", store->root, hex, hex + 2);
}

bool8_t vkr_depot_store_has(const VkrDepotStore *store, const VkrDepotId *id) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  vkr_depot_store_path(store, id, path, sizeof(path));
  return vkr_bakery_is_file(path);
}

static bool8_t store_write(const VkrDepotStore *store, const VkrDepotId *id,
                           const uint8_t *stored, uint64_t size) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  char directory[VKR_DEPOT_PATH_CAPACITY];
  vkr_depot_store_path(store, id, path, sizeof(path));
  if (vkr_bakery_is_file(path)) {
    return true_v;
  }
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  return vkr_bakery_make_directories(directory) &&
         vkr_bakery_write_file_atomic(path, stored, size);
}

bool8_t vkr_depot_store_put(const VkrDepotStore *store, const VkrDepotId *id,
                            const uint8_t *bytes, uint64_t size) {
  if (size > VKR_DEPOT_OBJECT_MAX) {
    return false_v;
  }
  VkrDepotId actual;
  vkr_depot_id_of(bytes, size, &actual);
  if (!vkr_depot_id_equal(&actual, id)) {
    return false_v;
  }
  if (vkr_depot_store_has(store, id)) {
    return true_v;
  }

  uint8_t *stored = NULL;
  uint64_t stored_size = 0u;
  if (store->codec && size >= 256u) {
    const uint64_t bound = store->codec->bound(size);
    stored = malloc(VKR_DEPOT_STORED_HEADER + bound);
    if (!stored) {
      return false_v;
    }
    const uint64_t packed =
        store->codec->compress(stored + VKR_DEPOT_STORED_HEADER, bound, bytes,
                               size, STORE_COMPRESSION_LEVEL);
    /* Keep the compressed form only when it saves a tenth, as .vkpak does. */
    if (packed > 0u && packed * 10u <= size * 9u) {
      stored[0] = STORED_ZSTD;
      stored_size = VKR_DEPOT_STORED_HEADER + packed;
    } else {
      free(stored);
      stored = NULL;
    }
  }
  if (!stored) {
    stored = malloc(VKR_DEPOT_STORED_HEADER + size);
    if (!stored) {
      return false_v;
    }
    stored[0] = STORED_RAW;
    if (size > 0u) {
      memcpy(stored + VKR_DEPOT_STORED_HEADER, bytes, size);
    }
    stored_size = VKR_DEPOT_STORED_HEADER + size;
  }
  vkr_store_le_u32(stored + 1, (uint32_t)size);
  const bool8_t written = store_write(store, id, stored, stored_size);
  free(stored);
  return written;
}

bool8_t vkr_depot_stored_decode(const VkrDepotCodec *codec,
                                const uint8_t *stored, uint64_t size,
                                uint8_t **out_bytes, uint64_t *out_size) {
  if (size < VKR_DEPOT_STORED_HEADER) {
    return false_v;
  }
  const uint32_t decoded_size = vkr_load_le_u32(stored + 1);
  if (decoded_size > VKR_DEPOT_OBJECT_MAX) {
    return false_v;
  }
  uint8_t *bytes = malloc(decoded_size ? decoded_size : 1u);
  if (!bytes) {
    return false_v;
  }
  const uint8_t *payload = stored + VKR_DEPOT_STORED_HEADER;
  const uint64_t payload_size = size - VKR_DEPOT_STORED_HEADER;
  bool8_t ok = false_v;
  if (stored[0] == STORED_RAW) {
    ok = payload_size == decoded_size;
    if (ok && decoded_size > 0u) {
      memcpy(bytes, payload, decoded_size);
    }
  } else if (stored[0] == STORED_ZSTD && codec) {
    ok = codec->decompress(bytes, decoded_size, payload, payload_size) ==
         decoded_size;
  }
  if (!ok) {
    free(bytes);
    return false_v;
  }
  *out_bytes = bytes;
  *out_size = decoded_size;
  return true_v;
}

bool8_t vkr_depot_store_put_stored(const VkrDepotStore *store,
                                   const VkrDepotId *id, const uint8_t *stored,
                                   uint64_t size) {
  uint8_t *bytes = NULL;
  uint64_t decoded = 0u;
  if (!vkr_depot_stored_decode(store->codec, stored, size, &bytes, &decoded)) {
    return false_v;
  }
  VkrDepotId actual;
  vkr_depot_id_of(bytes, decoded, &actual);
  free(bytes);
  if (!vkr_depot_id_equal(&actual, id)) {
    return false_v;
  }
  return store_write(store, id, stored, size);
}

bool8_t vkr_depot_store_get_stored(const VkrDepotStore *store,
                                   const VkrDepotId *id, uint8_t **out_bytes,
                                   uint64_t *out_size) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  vkr_depot_store_path(store, id, path, sizeof(path));
  return vkr_bakery_read_file(path, VKR_DEPOT_STORED_MAX, out_bytes, out_size);
}

bool8_t vkr_depot_store_get(const VkrDepotStore *store, const VkrDepotId *id,
                            uint8_t **out_bytes, uint64_t *out_size) {
  uint8_t *stored = NULL;
  uint64_t size = 0u;
  if (!vkr_depot_store_get_stored(store, id, &stored, &size)) {
    return false_v;
  }
  const bool8_t ok =
      vkr_depot_stored_decode(store->codec, stored, size, out_bytes, out_size);
  free(stored);
  return ok;
}

// =============================================================================
// FastCDC
// =============================================================================

/* Masks over the gear hash's high bits, which depend on the last 64 bytes:
   the strict mask before the average size, the loose one after it. */
#define CDC_MASK_STRICT (((1ull << 20) - 1u) << 44)
#define CDC_MASK_LOOSE (((1ull << 16) - 1u) << 48)

static uint64_t cdc_gear[256];
static bool8_t cdc_ready = false_v;

static void cdc_init(void) {
  /* splitmix64 from a fixed seed: every host derives the same table. */
  uint64_t state = 0x5644504f54434443ull;
  for (uint32_t i = 0u; i < 256u; ++i) {
    uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    cdc_gear[i] = z ^ (z >> 31);
  }
  cdc_ready = true_v;
}

uint32_t vkr_depot_chunk_length(const uint8_t *data, uint64_t size) {
  if (!cdc_ready) {
    cdc_init();
  }
  if (size <= VKR_DEPOT_CHUNK_MIN) {
    return (uint32_t)size;
  }
  const uint64_t limit = Min(size, (uint64_t)VKR_DEPOT_CHUNK_MAX);
  const uint64_t normal = Min(limit, (uint64_t)VKR_DEPOT_CHUNK_AVERAGE);
  uint64_t hash = 0u;
  uint64_t i = VKR_DEPOT_CHUNK_MIN;
  for (; i < normal; ++i) {
    hash = (hash << 1) + cdc_gear[data[i]];
    if ((hash & CDC_MASK_STRICT) == 0u) {
      return (uint32_t)(i + 1u);
    }
  }
  for (; i < limit; ++i) {
    hash = (hash << 1) + cdc_gear[data[i]];
    if ((hash & CDC_MASK_LOOSE) == 0u) {
      return (uint32_t)(i + 1u);
    }
  }
  return (uint32_t)limit;
}
