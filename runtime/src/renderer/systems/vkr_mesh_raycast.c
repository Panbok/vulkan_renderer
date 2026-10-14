#include "renderer/systems/vkr_mesh_raycast.h"

#include "assets/vkr_mesh_cooked.h"
#include "containers/vkr_hashtable.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_asset_path.h"
#include "math/mat.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"
#include "vkr_packed_geometry.h"

/* Triangles a leaf holds before the build splits it. */
#define VKR_MESH_RAYCAST_LEAF_TRIANGLES 4u

/* Median splits keep the tree balanced, so its depth stays below
   log2(UINT32_MAX) + 1 and a traversal holds at most depth + 1 nodes. */
#define VKR_MESH_RAYCAST_STACK_DEPTH 64u

/* Packed vertices unpacked at once while reading positions. */
#define VKR_MESH_RAYCAST_UNPACK_CHUNK 64u

/* Room the world bounding sphere test leaves for rounding in the bounds. */
#define VKR_MESH_RAYCAST_SPHERE_SLACK 1.01f

/* Unused time after which trim releases the held decoded file. */
#define VKR_MESH_RAYCAST_FILE_IDLE_SECONDS 10.0

typedef struct VkrMeshRaycastNode {
  VkrPackedVec3 min;
  /* An interior node's left child, whose right sibling follows it; a leaf's
     first triangle in leaf order. */
  uint32_t first;
  VkrPackedVec3 max;
  /* A leaf's triangle count; zero for an interior node. */
  uint32_t count;
} VkrMeshRaycastNode;

/* The cached triangles of one asset slot. One block holds the positions, the
   indices (three per triangle, in leaf order) and the nodes; node 0 is the
   root. A failed entry holds no block and stays until its asset changes, so
   an undecodable file is read and logged once. */
typedef struct VkrMeshRaycastEntry {
  bool8_t occupied;
  bool8_t failed;
  uint32_t asset_generation;
  uint64_t key_hash;
  uint64_t last_use;

  void *block;
  uint64_t block_bytes;
  const VkrPackedVec3 *positions;
  const uint32_t *indices;
  const VkrMeshRaycastNode *nodes;
  uint32_t triangle_count;
} VkrMeshRaycastEntry;

/* The most recently decoded cooked file. Every Bistro asset is a source mesh
   of one 31 MB file, so further assets of the held file extract their source
   mesh from it instead of decoding the whole file again (about a second in
   Debug). It is released when another file is decoded, when
   vkr_mesh_raycast_cache_trim finds it unused for more than
   VKR_MESH_RAYCAST_FILE_IDLE_SECONDS, and on destroy. Its bytes count toward
   no budget: Bistro holds about 88 MB decoded, and a decode peaks near
   120 MB while the file bytes are read. */
typedef struct VkrMeshRaycastFile {
  /* Owns `mesh_path` and `decoded`; NULL when no file is held. */
  Arena *arena;
  VkrAllocator allocator;
  String8 mesh_path;
  /* Size and modification time when decoded; a rewritten file decodes
     again. */
  FileStats stats;
  VkrMeshCookedDecoded decoded;
  /* Set by each extraction; trim restarts the idle time while it is set. */
  bool8_t used_since_trim;
  /* Whether a trim has stamped `idle_since_seconds` since the decode. */
  bool8_t trim_seen;
  float64_t idle_since_seconds;
} VkrMeshRaycastFile;

struct s_VkrMeshRaycastCache {
  VkrAllocator *allocator;
  uint64_t budget_bytes;
  uint64_t used_bytes;
  /* Advances on every use of an entry; the smallest last_use is evicted. */
  uint64_t use_clock;
  /* Indexed by asset slot (handle id - 1); grows when a higher slot first
     appears, never per ray. */
  VkrMeshRaycastEntry *entries;
  uint32_t entry_capacity;
  VkrMeshRaycastFile file;
  /* Cooked files decoded so far, for tests. */
  uint32_t decode_count;
};

/* Working state of one BVH build; every array lives in build scratch. */
typedef struct VkrMeshRaycastBuild {
  const VkrPackedVec3 *positions;
  const uint32_t *indices;
  /* Three per triangle. */
  float32_t *centroids;
  /* Triangle indices in leaf order. */
  uint32_t *order;
  VkrMeshRaycastNode *nodes;
  uint32_t node_count;
} VkrMeshRaycastBuild;

// =============================================================================
// Entries
// =============================================================================

vkr_internal void mesh_raycast_release_entry(VkrMeshRaycastCache *cache,
                                             VkrMeshRaycastEntry *entry) {
  if (entry->block) {
    vkr_allocator_free(cache->allocator, entry->block, entry->block_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    cache->used_bytes -= entry->block_bytes;
  }
  *entry = (VkrMeshRaycastEntry){0};
}

/* Releases the least recently used entries other than `keep` until the
   cached triangles fit the budget. An asset larger than the whole budget
   stays alone until another asset is cached. */
vkr_internal void mesh_raycast_evict(VkrMeshRaycastCache *cache,
                                     const VkrMeshRaycastEntry *keep) {
  while (cache->used_bytes > cache->budget_bytes) {
    VkrMeshRaycastEntry *oldest = NULL;
    for (uint32_t i = 0u; i < cache->entry_capacity; ++i) {
      VkrMeshRaycastEntry *entry = &cache->entries[i];
      if (entry == keep || !entry->block) {
        continue;
      }
      if (!oldest || entry->last_use < oldest->last_use) {
        oldest = entry;
      }
    }
    if (!oldest) {
      return;
    }
    mesh_raycast_release_entry(cache, oldest);
  }
}

vkr_internal bool8_t mesh_raycast_reserve_slot(VkrMeshRaycastCache *cache,
                                               uint32_t slot) {
  if (slot < cache->entry_capacity) {
    return true_v;
  }
  const uint32_t capacity =
      Max(Max(slot + 1u, cache->entry_capacity * 2u), 64u);
  VkrMeshRaycastEntry *entries = vkr_allocator_alloc(
      cache->allocator, (uint64_t)capacity * sizeof(*entries),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!entries) {
    return false_v;
  }
  MemZero(entries, (uint64_t)capacity * sizeof(*entries));
  if (cache->entries) {
    MemCopy(entries, cache->entries,
            (uint64_t)cache->entry_capacity * sizeof(*entries));
    vkr_allocator_free(cache->allocator, cache->entries,
                       (uint64_t)cache->entry_capacity * sizeof(*entries),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  cache->entries = entries;
  cache->entry_capacity = capacity;
  return true_v;
}

/* The geometry an asset draws follows from its file and source mesh. */
vkr_internal uint64_t mesh_raycast_key_hash(const VkrMeshAsset *asset) {
  const uint64_t path_hash = vkr_hash_table_key_string8(asset->mesh_path).hash;
  return vkr_hash_table_finalize(path_hash ^
                                 (uint64_t)asset->source_mesh_index_plus_one);
}

/* The asset `handle` names while it is live and loaded, else NULL. */
vkr_internal VkrMeshAsset *mesh_raycast_live_asset(VkrMeshManager *meshes,
                                                   VkrMeshAssetHandle handle) {
  if (handle.id == 0u || handle.id > meshes->mesh_assets.length) {
    return NULL;
  }
  VkrMeshAsset *asset = &meshes->mesh_assets.data[handle.id - 1u];
  if (asset->id != handle.id || asset->generation != handle.generation ||
      asset->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
    return NULL;
  }
  return asset;
}

/* Clears the entry of `handle`'s slot when it belongs to another asset or
   generation and marks it for `asset`. NULL when the slot cannot be held. */
vkr_internal VkrMeshRaycastEntry *
mesh_raycast_claim_entry(VkrMeshRaycastCache *cache, VkrMeshAssetHandle handle,
                         const VkrMeshAsset *asset) {
  const uint32_t slot = handle.id - 1u;
  if (!mesh_raycast_reserve_slot(cache, slot)) {
    return NULL;
  }
  VkrMeshRaycastEntry *entry = &cache->entries[slot];
  const uint64_t key_hash = mesh_raycast_key_hash(asset);
  if (entry->occupied && (entry->asset_generation != asset->generation ||
                          entry->key_hash != key_hash)) {
    mesh_raycast_release_entry(cache, entry);
  }
  if (!entry->occupied) {
    entry->occupied = true_v;
    entry->asset_generation = asset->generation;
    entry->key_hash = key_hash;
  }
  return entry;
}

// =============================================================================
// BVH build
// =============================================================================

vkr_internal void mesh_raycast_extend(VkrPackedVec3 *min, VkrPackedVec3 *max,
                                      VkrPackedVec3 point) {
  min->x = Min(min->x, point.x);
  min->y = Min(min->y, point.y);
  min->z = Min(min->z, point.z);
  max->x = Max(max->x, point.x);
  max->y = Max(max->y, point.y);
  max->z = Max(max->z, point.z);
}

/* Orders `order` so that the triangle at `k` has the centroid it would have
   sorted along `axis`, nothing before it greater and nothing after it less. */
vkr_internal void mesh_raycast_select(const VkrMeshRaycastBuild *build,
                                      uint32_t *order, uint32_t count,
                                      uint32_t k, uint32_t axis) {
  const float32_t *centroids = build->centroids;
  int64_t low = 0;
  int64_t high = (int64_t)count - 1;
  while (low < high) {
    const float32_t pivot =
        centroids[order[low + (high - low) / 2] * 3u + axis];
    int64_t i = low;
    int64_t j = high;
    while (i <= j) {
      while (centroids[order[i] * 3u + axis] < pivot) {
        ++i;
      }
      while (centroids[order[j] * 3u + axis] > pivot) {
        --j;
      }
      if (i <= j) {
        const uint32_t swap = order[i];
        order[i] = order[j];
        order[j] = swap;
        ++i;
        --j;
      }
    }
    if ((int64_t)k <= j) {
      high = j;
    } else if ((int64_t)k >= i) {
      low = i;
    } else {
      return;
    }
  }
}

/* Builds node `node_index` over the `count` triangles at `first` in leaf
   order, splitting at the median centroid of the longest centroid axis. */
vkr_internal void mesh_raycast_build_node(VkrMeshRaycastBuild *build,
                                          uint32_t node_index, uint32_t first,
                                          uint32_t count) {
  VkrPackedVec3 min = {VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX};
  VkrPackedVec3 max = {-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX};
  VkrPackedVec3 centroid_min = min;
  VkrPackedVec3 centroid_max = max;
  for (uint32_t i = first; i < first + count; ++i) {
    const uint32_t triangle = build->order[i];
    for (uint32_t corner = 0u; corner < 3u; ++corner) {
      const uint32_t vertex = build->indices[triangle * 3u + corner];
      mesh_raycast_extend(&min, &max, build->positions[vertex]);
    }
    const float32_t *centroid = &build->centroids[triangle * 3u];
    const VkrPackedVec3 point = {centroid[0], centroid[1], centroid[2]};
    mesh_raycast_extend(&centroid_min, &centroid_max, point);
  }

  VkrMeshRaycastNode *node = &build->nodes[node_index];
  *node = (VkrMeshRaycastNode){
      .min = min,
      .first = first,
      .max = max,
      .count = count,
  };
  if (count <= VKR_MESH_RAYCAST_LEAF_TRIANGLES) {
    return;
  }

  const float32_t extents[3] = {
      centroid_max.x - centroid_min.x,
      centroid_max.y - centroid_min.y,
      centroid_max.z - centroid_min.z,
  };
  uint32_t axis = 0u;
  if (extents[1] > extents[axis]) {
    axis = 1u;
  }
  if (extents[2] > extents[axis]) {
    axis = 2u;
  }
  /* Coincident centroids cannot be separated; they stay one leaf. */
  if (!(extents[axis] > 0.0f)) {
    return;
  }

  const uint32_t half = count / 2u;
  mesh_raycast_select(build, build->order + first, count, half, axis);
  const uint32_t left = build->node_count;
  build->node_count += 2u;
  node->first = left;
  node->count = 0u;
  mesh_raycast_build_node(build, left, first, half);
  mesh_raycast_build_node(build, left + 1u, first + half, count - half);
}

/* Validates the triangles, builds their BVH in `scratch` and copies
   positions, leaf-ordered indices and nodes into one cache block owned by
   `entry`, then evicts other entries past the budget. */
vkr_internal bool8_t mesh_raycast_build_entry(
    VkrMeshRaycastCache *cache, VkrMeshRaycastEntry *entry,
    const VkrPackedVec3 *positions, uint32_t position_count,
    const uint32_t *indices, uint32_t index_count, VkrAllocator *scratch) {
  if (!positions || !indices || position_count == 0u || index_count == 0u ||
      index_count % 3u != 0u) {
    return false_v;
  }
  for (uint32_t i = 0u; i < position_count; ++i) {
    const VkrPackedVec3 p = positions[i];
    if (!vkr_is_finite_f32(p.x) || !vkr_is_finite_f32(p.y) ||
        !vkr_is_finite_f32(p.z)) {
      return false_v;
    }
  }
  for (uint32_t i = 0u; i < index_count; ++i) {
    if (indices[i] >= position_count) {
      return false_v;
    }
  }

  const uint32_t triangle_count = index_count / 3u;
  const uint64_t node_capacity = (uint64_t)triangle_count * 2u;
  VkrMeshRaycastBuild build = {
      .positions = positions,
      .indices = indices,
      .centroids = vkr_allocator_alloc(
          scratch, (uint64_t)triangle_count * 3u * sizeof(float32_t),
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY),
      .order = vkr_allocator_alloc(scratch,
                                   (uint64_t)triangle_count * sizeof(uint32_t),
                                   VKR_ALLOCATOR_MEMORY_TAG_ARRAY),
      .nodes = vkr_allocator_alloc(scratch,
                                   node_capacity * sizeof(VkrMeshRaycastNode),
                                   VKR_ALLOCATOR_MEMORY_TAG_ARRAY),
      .node_count = 1u,
  };
  if (!build.centroids || !build.order || !build.nodes) {
    return false_v;
  }
  for (uint32_t triangle = 0u; triangle < triangle_count; ++triangle) {
    const VkrPackedVec3 a = positions[indices[triangle * 3u]];
    const VkrPackedVec3 b = positions[indices[triangle * 3u + 1u]];
    const VkrPackedVec3 c = positions[indices[triangle * 3u + 2u]];
    build.centroids[triangle * 3u] = (a.x + b.x + c.x) / 3.0f;
    build.centroids[triangle * 3u + 1u] = (a.y + b.y + c.y) / 3.0f;
    build.centroids[triangle * 3u + 2u] = (a.z + b.z + c.z) / 3.0f;
    build.order[triangle] = triangle;
  }
  mesh_raycast_build_node(&build, 0u, 0u, triangle_count);

  /* Every member is four-byte aligned, so the arrays pack back to back. */
  const uint64_t position_bytes =
      (uint64_t)position_count * sizeof(VkrPackedVec3);
  const uint64_t index_bytes = (uint64_t)index_count * sizeof(uint32_t);
  const uint64_t node_bytes =
      (uint64_t)build.node_count * sizeof(VkrMeshRaycastNode);
  const uint64_t block_bytes = position_bytes + index_bytes + node_bytes;
  uint8_t *block = vkr_allocator_alloc(cache->allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!block) {
    return false_v;
  }
  VkrPackedVec3 *block_positions = (VkrPackedVec3 *)block;
  uint32_t *block_indices = (uint32_t *)(block + position_bytes);
  VkrMeshRaycastNode *block_nodes =
      (VkrMeshRaycastNode *)(block + position_bytes + index_bytes);
  MemCopy(block_positions, positions, position_bytes);
  for (uint32_t i = 0u; i < triangle_count; ++i) {
    const uint32_t triangle = build.order[i];
    MemCopy(&block_indices[i * 3u], &indices[triangle * 3u],
            3u * sizeof(uint32_t));
  }
  MemCopy(block_nodes, build.nodes, node_bytes);

  entry->block = block;
  entry->block_bytes = block_bytes;
  entry->positions = block_positions;
  entry->indices = block_indices;
  entry->nodes = block_nodes;
  entry->triangle_count = triangle_count;
  entry->last_use = ++cache->use_clock;
  cache->used_bytes += block_bytes;
  mesh_raycast_evict(cache, entry);
  return true_v;
}

// =============================================================================
// Cooked file decode
// =============================================================================

/* The lowest and highest vertex a nonempty decoded range indexes. */
vkr_internal void mesh_raycast_range_span(const uint32_t *indices,
                                          const VkrGeometryUploadRange *range,
                                          uint32_t *out_min,
                                          uint32_t *out_max) {
  uint32_t min_vertex = UINT32_MAX;
  uint32_t max_vertex = 0u;
  for (uint32_t i = 0u; i < range->index_count; ++i) {
    const uint32_t vertex = indices[range->first_index + i];
    min_vertex = Min(min_vertex, vertex);
    max_vertex = Max(max_vertex, vertex);
  }
  *out_min = min_vertex;
  *out_max = max_vertex;
}

vkr_internal void mesh_raycast_release_file(VkrMeshRaycastCache *cache) {
  VkrMeshRaycastFile *file = &cache->file;
  if (file->arena) {
    vkr_allocator_release_global_accounting(&file->allocator);
    arena_destroy(file->arena);
  }
  *file = (VkrMeshRaycastFile){0};
}

/* The decoded cooked file at `mesh_path`: the held one while it is the same
   unchanged file, else a fresh decode that replaces it. The file bytes are
   read into `scratch`. vkr_mesh_cooked_decode validates and decodes every
   range of an artifact; no call decodes a single source mesh. */
vkr_internal const VkrMeshCookedDecoded *
mesh_raycast_acquire_file(VkrMeshRaycastCache *cache, String8 mesh_path,
                          VkrAllocator *scratch) {
  FilePath path = vkr_asset_path_file(scratch, mesh_path);
  FileStats stats = {0};
  if (path.path.length == 0u || file_stats(&path, &stats) != FILE_ERROR_NONE) {
    return NULL;
  }
  VkrMeshRaycastFile *held = &cache->file;
  if (held->arena && string8_equals(&held->mesh_path, &mesh_path) &&
      held->stats.size == stats.size &&
      held->stats.last_modified == stats.last_modified) {
    held->used_since_trim = true_v;
    return &held->decoded;
  }
  mesh_raycast_release_file(cache);

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle handle = {0};
  uint8_t *artifact = NULL;
  uint64_t artifact_size = 0u;
  const bool8_t read = file_open(&path, mode, &handle) == FILE_ERROR_NONE &&
                       file_read_all(&handle, scratch, &artifact,
                                     &artifact_size) == FILE_ERROR_NONE;
  file_close(&handle);
  if (!read) {
    return NULL;
  }

  Arena *arena = arena_create(MB(64), MB(1));
  if (!arena) {
    return NULL;
  }
  VkrAllocator allocator = {.ctx = arena};
  if (!vkr_allocator_arena(&allocator)) {
    arena_destroy(arena);
    return NULL;
  }
  *held = (VkrMeshRaycastFile){
      .arena = arena,
      .allocator = allocator,
      .stats = stats,
      .used_since_trim = true_v,
  };
  held->mesh_path = string8_duplicate(&held->allocator, &mesh_path);
  ++cache->decode_count;
  const float64_t start = vkr_platform_get_absolute_time();
  if (!held->mesh_path.str ||
      !vkr_mesh_cooked_decode(&held->allocator, scratch, artifact,
                              artifact_size, &held->decoded)) {
    mesh_raycast_release_file(cache);
    return NULL;
  }
  const VkrGeometryUploadBuffer *buffer = &held->decoded.mesh_buffer;
  if (held->decoded.ranges.length == 0u ||
      buffer->vertex_layout != VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1 ||
      buffer->vertex_size != sizeof(VkrPackedStaticVertex) ||
      buffer->index_size != sizeof(uint32_t)) {
    mesh_raycast_release_file(cache);
    return NULL;
  }
  log_debug("MeshRaycast: decoded '%.*s' (%llu bytes held) in %.1f ms",
            (int)mesh_path.length, mesh_path.str,
            (unsigned long long)arena_pos(arena),
            (vkr_platform_get_absolute_time() - start) * 1000.0);
  return &held->decoded;
}

/* Reads the LOD 0 positions and indices of `asset`'s source mesh from its
   decoded cooked file and builds `entry` from them. The source mesh is
   selected as the mesh manager selects its submeshes. Only the first asset
   of a file decodes it; later assets of the held file extract from it. */
vkr_internal bool8_t mesh_raycast_extract_asset(VkrMeshRaycastCache *cache,
                                                VkrMeshRaycastEntry *entry,
                                                const VkrMeshAsset *asset) {
  bool8_t success = false_v;
  bool8_t scratch_ready = false_v;
  Arena *scratch_arena = arena_create(MB(64), MB(1));
  VkrAllocator scratch = {.ctx = scratch_arena};
  if (!scratch_arena) {
    goto cleanup;
  }
  scratch_ready = vkr_allocator_arena(&scratch);
  if (!scratch_ready) {
    goto cleanup;
  }

  const VkrMeshCookedDecoded *decoded =
      mesh_raycast_acquire_file(cache, asset->mesh_path, &scratch);
  if (!decoded) {
    goto cleanup;
  }
  const VkrGeometryUploadBuffer *buffer = &decoded->mesh_buffer;

  uint32_t first_range = 0u;
  uint32_t range_count = (uint32_t)decoded->ranges.length;
  if (decoded->source.nodes.length != 0u ||
      asset->source_mesh_index_plus_one != 0u) {
    const uint32_t selected = asset->source_mesh_index_plus_one - 1u;
    if (asset->source_mesh_index_plus_one == 0u ||
        selected >= decoded->source.meshes.length) {
      goto cleanup;
    }
    const VkrMeshSourceMesh *source_mesh =
        &decoded->source.meshes.data[selected];
    first_range = source_mesh->first_range;
    range_count = source_mesh->range_count;
    if (range_count == 0u || first_range > decoded->ranges.length ||
        range_count > decoded->ranges.length - first_range) {
      goto cleanup;
    }
  }

  /* The decoder proved each range's indices reach only its own contiguous
     vertices; positions keep just the span each range uses. */
  const uint32_t *source_indices = buffer->indices;
  uint64_t vertex_total = 0u;
  uint64_t index_total = 0u;
  for (uint32_t r = first_range; r < first_range + range_count; ++r) {
    const VkrGeometryUploadRange *range = &decoded->ranges.data[r];
    if (range->index_count == 0u) {
      continue;
    }
    uint32_t min_vertex = 0u;
    uint32_t max_vertex = 0u;
    mesh_raycast_range_span(source_indices, range, &min_vertex, &max_vertex);
    vertex_total += (uint64_t)(max_vertex - min_vertex) + 1u;
    index_total += range->index_count;
  }
  if (vertex_total == 0u || vertex_total > UINT32_MAX ||
      index_total > UINT32_MAX) {
    goto cleanup;
  }

  VkrPackedVec3 *positions =
      vkr_allocator_alloc(&scratch, vertex_total * sizeof(VkrPackedVec3),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices = vkr_allocator_alloc(
      &scratch, index_total * sizeof(uint32_t), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!positions || !indices) {
    goto cleanup;
  }
  const VkrPackedStaticVertex *packed = buffer->vertices;
  uint32_t vertex_base = 0u;
  uint32_t index_base = 0u;
  for (uint32_t r = first_range; r < first_range + range_count; ++r) {
    const VkrGeometryUploadRange *range = &decoded->ranges.data[r];
    if (range->index_count == 0u) {
      continue;
    }
    uint32_t min_vertex = 0u;
    uint32_t max_vertex = 0u;
    mesh_raycast_range_span(source_indices, range, &min_vertex, &max_vertex);
    for (uint32_t i = 0u; i < range->index_count; ++i) {
      indices[index_base + i] =
          source_indices[range->first_index + i] - min_vertex + vertex_base;
    }
    index_base += range->index_count;

    const VkrGpuGeometryDecodeRecord *decode =
        &buffer->decodes[range->decode_index];
    const uint32_t range_vertices = max_vertex - min_vertex + 1u;
    VkrVertex3d unpacked[VKR_MESH_RAYCAST_UNPACK_CHUNK];
    for (uint32_t done = 0u; done < range_vertices;) {
      const uint32_t chunk =
          Min(range_vertices - done, VKR_MESH_RAYCAST_UNPACK_CHUNK);
      vkr_packed_geometry_unpack(&packed[min_vertex + done], chunk, decode,
                                 unpacked);
      for (uint32_t i = 0u; i < chunk; ++i) {
        positions[vertex_base + done + i] = unpacked[i].position;
      }
      done += chunk;
    }
    vertex_base += range_vertices;
  }

  success = mesh_raycast_build_entry(cache, entry, positions, vertex_base,
                                     indices, index_base, &scratch);
  if (success) {
    log_debug("MeshRaycast: '%.*s' source %u: %u triangles, %llu bytes",
              (int)asset->mesh_path.length, asset->mesh_path.str,
              asset->source_mesh_index_plus_one, entry->triangle_count,
              (unsigned long long)entry->block_bytes);
  }

cleanup:
  if (scratch_ready) {
    vkr_allocator_release_global_accounting(&scratch);
  }
  if (scratch_arena) {
    arena_destroy(scratch_arena);
  }
  return success;
}

uint32_t vkr_mesh_raycast_coarse_ranges(VkrMeshRaycastCache *cache,
                                        VkrMeshManager *meshes,
                                        VkrMeshAssetHandle asset_handle,
                                        VkrAllocator *allocator,
                                        VkrMeshCoarseRange *out,
                                        uint32_t capacity) {
  VkrMeshAsset *asset =
      cache && meshes ? mesh_raycast_live_asset(meshes, asset_handle) : NULL;
  if (!asset || !allocator || !out || !capacity) {
    return 0u;
  }
  uint32_t written = 0u;
  Arena *scratch_arena = arena_create(MB(64), MB(1));
  VkrAllocator scratch = {.ctx = scratch_arena};
  if (!scratch_arena || !vkr_allocator_arena(&scratch)) {
    goto cleanup;
  }
  const VkrMeshCookedDecoded *decoded =
      mesh_raycast_acquire_file(cache, asset->mesh_path, &scratch);
  if (!decoded) {
    goto cleanup;
  }
  const VkrGeometryUploadBuffer *buffer = &decoded->mesh_buffer;
  uint32_t first_range = 0u;
  uint32_t range_count = (uint32_t)decoded->ranges.length;
  if (decoded->source.nodes.length != 0u ||
      asset->source_mesh_index_plus_one != 0u) {
    const uint32_t selected = asset->source_mesh_index_plus_one - 1u;
    if (asset->source_mesh_index_plus_one == 0u ||
        selected >= decoded->source.meshes.length) {
      goto cleanup;
    }
    const VkrMeshSourceMesh *source_mesh =
        &decoded->source.meshes.data[selected];
    first_range = source_mesh->first_range;
    range_count = source_mesh->range_count;
    if (first_range > decoded->ranges.length ||
        range_count > decoded->ranges.length - first_range) {
      goto cleanup;
    }
  }

  const uint32_t *source_indices = buffer->indices;
  for (uint32_t r = first_range;
       r < first_range + range_count && written < capacity; ++r) {
    const VkrGeometryUploadRange *range = &decoded->ranges.data[r];
    VkrMeshCoarseRange *coarse = &out[written++];
    *coarse = (VkrMeshCoarseRange){0};
    if (range->index_count == 0u) {
      continue;
    }
    /* The range's last level, or level 0 without a level row. */
    uint32_t first_index = range->first_index;
    uint32_t index_count = range->index_count;
    const VkrGpuGeometryDecodeRecord *decode =
        &buffer->decodes[range->decode_index];
    if (decode->lod_record != 0u &&
        decode->lod_record + VKR_GPU_GEOMETRY_LOD_RECORDS <=
            buffer->decode_count) {
      const VkrGpuGeometryLodRow *row =
          (const VkrGpuGeometryLodRow *)(buffer->decodes + decode->lod_record);
      if (row->level_count > 1u &&
          row->level_count <= VKR_GPU_GEOMETRY_LOD_LEVEL_MAX) {
        const VkrGpuGeometryLodLevel *level =
            &row->levels[row->level_count - 1u];
        first_index = range->first_index + level->first_index;
        index_count = level->index_count;
      }
    }
    if (index_count == 0u || index_count % 3u != 0u ||
        first_index + index_count > buffer->index_count) {
      continue;
    }
    uint32_t min_vertex = UINT32_MAX;
    uint32_t max_vertex = 0u;
    for (uint32_t i = 0u; i < index_count; ++i) {
      const uint32_t vertex = source_indices[first_index + i];
      min_vertex = Min(min_vertex, vertex);
      max_vertex = Max(max_vertex, vertex);
    }
    if (max_vertex >= buffer->vertex_count) {
      continue;
    }
    const uint32_t vertex_count = max_vertex - min_vertex + 1u;
    coarse->vertices = vkr_allocator_alloc(
        allocator, (uint64_t)vertex_count * sizeof(VkrVertex3d),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    coarse->indices =
        vkr_allocator_alloc(allocator, (uint64_t)index_count * sizeof(uint32_t),
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!coarse->vertices || !coarse->indices) {
      written = 0u;
      goto cleanup;
    }
    vkr_packed_geometry_unpack(
        &((const VkrPackedStaticVertex *)buffer->vertices)[min_vertex],
        vertex_count, decode, coarse->vertices);
    for (uint32_t i = 0u; i < index_count; ++i) {
      coarse->indices[i] = source_indices[first_index + i] - min_vertex;
    }
    coarse->vertex_count = vertex_count;
    coarse->index_count = index_count;
  }

cleanup:
  if (scratch_arena) {
    vkr_allocator_release_global_accounting(&scratch);
    arena_destroy(scratch_arena);
  }
  return written;
}

// =============================================================================
// Ray tests
// =============================================================================

/* Whether the segment comes within the instance's world bounding sphere. */
vkr_internal bool8_t mesh_raycast_segment_near_sphere(Vec3 origin, Vec3 delta,
                                                      Vec3 center,
                                                      float32_t radius) {
  const float32_t length_sq = vec3_length_squared(delta);
  float32_t t = 0.0f;
  if (length_sq > 0.0f) {
    t = vec3_dot(vec3_sub(center, origin), delta) / length_sq;
    t = Clamp(t, 0.0f, 1.0f);
  }
  const Vec3 closest = vec3_add(origin, vec3_scale(delta, t));
  const float32_t reach = radius * VKR_MESH_RAYCAST_SPHERE_SLACK;
  return vec3_length_squared(vec3_sub(center, closest)) <= reach * reach;
}

/* Entry distance of the segment into `node` when it enters before `t_max`. */
vkr_internal bool8_t mesh_raycast_node_entry(const VkrMeshRaycastNode *node,
                                             Vec3 origin, Vec3 inverse_delta,
                                             float32_t t_max,
                                             float32_t *out_entry) {
  const float32_t x0 = (node->min.x - origin.x) * inverse_delta.x;
  const float32_t x1 = (node->max.x - origin.x) * inverse_delta.x;
  const float32_t y0 = (node->min.y - origin.y) * inverse_delta.y;
  const float32_t y1 = (node->max.y - origin.y) * inverse_delta.y;
  const float32_t z0 = (node->min.z - origin.z) * inverse_delta.z;
  const float32_t z1 = (node->max.z - origin.z) * inverse_delta.z;
  const float32_t entry =
      Max(Max(Min(x0, x1), Min(y0, y1)), Max(Min(z0, z1), 0.0f));
  const float32_t exit =
      Min(Min(Max(x0, x1), Max(y0, y1)), Min(Max(z0, z1), t_max));
  *out_entry = entry;
  return entry <= exit;
}

/* Möller–Trumbore against both faces: the segment parameter of the hit in
   [0, t_max] and the triangle's unnormalized geometric normal. */
vkr_internal bool8_t mesh_raycast_triangle(Vec3 origin, Vec3 delta, Vec3 a,
                                           Vec3 b, Vec3 c, float32_t t_max,
                                           float32_t *out_t, Vec3 *out_normal) {
  const Vec3 edge1 = vec3_sub(b, a);
  const Vec3 edge2 = vec3_sub(c, a);
  const Vec3 p = vec3_cross(delta, edge2);
  const float32_t det = vec3_dot(edge1, p);
  if (det == 0.0f) {
    return false_v;
  }
  const float32_t inverse_det = 1.0f / det;
  if (!vkr_is_finite_f32(inverse_det)) {
    return false_v;
  }

  const Vec3 s = vec3_sub(origin, a);
  const float32_t u = vec3_dot(s, p) * inverse_det;
  if (u < 0.0f || u > 1.0f) {
    return false_v;
  }
  const Vec3 q = vec3_cross(s, edge1);
  const float32_t v = vec3_dot(delta, q) * inverse_det;
  if (v < 0.0f || u + v > 1.0f) {
    return false_v;
  }
  const float32_t t = vec3_dot(edge2, q) * inverse_det;
  if (t < 0.0f || t > t_max) {
    return false_v;
  }

  *out_t = t;
  *out_normal = vec3_cross(edge1, edge2);
  return true_v;
}

vkr_internal Vec3 mesh_raycast_position(const VkrMeshRaycastEntry *entry,
                                        uint32_t vertex) {
  const VkrPackedVec3 p = entry->positions[vertex];
  return vec3_new(p.x, p.y, p.z);
}

/* Nearest hit of the local segment on the entry's triangles. */
vkr_internal bool8_t mesh_raycast_traverse(const VkrMeshRaycastEntry *entry,
                                           Vec3 origin, Vec3 delta,
                                           float32_t *out_t, Vec3 *out_normal) {
  /* A zero component divides to a signed infinity, which keeps the slab
     test exact for an origin inside or outside that slab. */
  const Vec3 inverse_delta =
      vec3_new(delta.x != 0.0f ? 1.0f / delta.x : VKR_FLOAT_MAX,
               delta.y != 0.0f ? 1.0f / delta.y : VKR_FLOAT_MAX,
               delta.z != 0.0f ? 1.0f / delta.z : VKR_FLOAT_MAX);

  float32_t best = 1.0f;
  bool8_t found = false_v;
  uint32_t stack_nodes[VKR_MESH_RAYCAST_STACK_DEPTH];
  float32_t stack_entries[VKR_MESH_RAYCAST_STACK_DEPTH];
  uint32_t depth = 0u;
  float32_t root_entry = 0.0f;
  if (!mesh_raycast_node_entry(&entry->nodes[0], origin, inverse_delta, best,
                               &root_entry)) {
    return false_v;
  }
  stack_nodes[depth] = 0u;
  stack_entries[depth] = root_entry;
  ++depth;

  while (depth > 0u) {
    --depth;
    if (stack_entries[depth] > best) {
      continue;
    }
    const VkrMeshRaycastNode *node = &entry->nodes[stack_nodes[depth]];

    if (node->count != 0u) {
      for (uint32_t i = node->first; i < node->first + node->count; ++i) {
        const uint32_t *triangle = &entry->indices[i * 3u];
        float32_t t = 0.0f;
        Vec3 normal = vec3_zero();
        if (mesh_raycast_triangle(
                origin, delta, mesh_raycast_position(entry, triangle[0]),
                mesh_raycast_position(entry, triangle[1]),
                mesh_raycast_position(entry, triangle[2]), best, &t, &normal)) {
          best = t;
          *out_normal = normal;
          found = true_v;
        }
      }
      continue;
    }

    float32_t left_entry = 0.0f;
    float32_t right_entry = 0.0f;
    const bool8_t left_hit = mesh_raycast_node_entry(
        &entry->nodes[node->first], origin, inverse_delta, best, &left_entry);
    const bool8_t right_hit =
        mesh_raycast_node_entry(&entry->nodes[node->first + 1u], origin,
                                inverse_delta, best, &right_entry);
    assert_log(depth + 2u <= VKR_MESH_RAYCAST_STACK_DEPTH,
               "Mesh raycast BVH deeper than its traversal stack");

    /* The nearer child goes on top so it is tested first. */
    const bool8_t left_first = left_entry <= right_entry;
    const uint32_t far_node = left_first ? node->first + 1u : node->first;
    const uint32_t near_node = left_first ? node->first : node->first + 1u;
    const bool8_t far_hit = left_first ? right_hit : left_hit;
    const bool8_t near_hit = left_first ? left_hit : right_hit;
    if (far_hit) {
      stack_nodes[depth] = far_node;
      stack_entries[depth] = left_first ? right_entry : left_entry;
      ++depth;
    }
    if (near_hit) {
      stack_nodes[depth] = near_node;
      stack_entries[depth] = left_first ? left_entry : right_entry;
      ++depth;
    }
  }

  *out_t = best;
  return found;
}

/* `v` at unit length, or zero when it has none. */
vkr_internal Vec3 mesh_raycast_unit(Vec3 v) {
  const float32_t length_sq = vec3_length_squared(v);
  if (!(length_sq > 0.0f) || !vkr_is_finite_f32(length_sq)) {
    return vec3_zero();
  }
  return vec3_scale(v, 1.0f / vkr_sqrt_f32(length_sq));
}

// =============================================================================
// Public API
// =============================================================================

VkrMeshRaycastCache *vkr_mesh_raycast_cache_create(VkrAllocator *allocator,
                                                   uint64_t budget_bytes) {
  if (!allocator) {
    return NULL;
  }
  VkrMeshRaycastCache *cache = vkr_allocator_alloc(
      allocator, sizeof(*cache), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!cache) {
    return NULL;
  }
  *cache = (VkrMeshRaycastCache){
      .allocator = allocator,
      .budget_bytes = budget_bytes,
  };
  return cache;
}

void vkr_mesh_raycast_cache_destroy(VkrMeshRaycastCache *cache) {
  if (!cache) {
    return;
  }
  mesh_raycast_release_file(cache);
  for (uint32_t i = 0u; i < cache->entry_capacity; ++i) {
    mesh_raycast_release_entry(cache, &cache->entries[i]);
  }
  if (cache->entries) {
    vkr_allocator_free(cache->allocator, cache->entries,
                       (uint64_t)cache->entry_capacity *
                           sizeof(VkrMeshRaycastEntry),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  vkr_allocator_free(cache->allocator, cache, sizeof(*cache),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

void vkr_mesh_raycast_cache_trim(VkrMeshRaycastCache *cache,
                                 float64_t now_seconds) {
  if (!cache || !cache->file.arena) {
    return;
  }
  VkrMeshRaycastFile *held = &cache->file;
  /* Idle time runs from the first trim after the last use. */
  if (held->used_since_trim || !held->trim_seen) {
    held->idle_since_seconds = now_seconds;
    held->trim_seen = true_v;
    held->used_since_trim = false_v;
    return;
  }
  if (now_seconds - held->idle_since_seconds >
      VKR_MESH_RAYCAST_FILE_IDLE_SECONDS) {
    mesh_raycast_release_file(cache);
  }
}

uint32_t vkr_mesh_raycast_cache_decode_count(const VkrMeshRaycastCache *cache) {
  return cache ? cache->decode_count : 0u;
}

bool8_t vkr_mesh_raycast_instance(VkrMeshRaycastCache *cache,
                                  VkrMeshManager *meshes,
                                  VkrMeshInstanceHandle instance_handle,
                                  Vec3 origin, Vec3 delta,
                                  VkrMeshRaycastHit *out_hit) {
  if (!cache || !meshes || !out_hit) {
    return false_v;
  }
  const VkrMeshInstance *instance =
      vkr_mesh_manager_get_instance(meshes, instance_handle);
  if (!instance || instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
    return false_v;
  }
  if (instance->bounds_valid &&
      !mesh_raycast_segment_near_sphere(origin, delta,
                                        instance->bounds_world_center,
                                        instance->bounds_world_radius)) {
    return false_v;
  }
  VkrMeshAsset *asset = mesh_raycast_live_asset(meshes, instance->asset);
  if (!asset) {
    return false_v;
  }

  /* A flattened model has no local segment. */
  const Mat4 model = instance->model;
  const Vec3 column0 =
      vec3_new(model.cols[0].x, model.cols[0].y, model.cols[0].z);
  const Vec3 column1 =
      vec3_new(model.cols[1].x, model.cols[1].y, model.cols[1].z);
  const Vec3 column2 =
      vec3_new(model.cols[2].x, model.cols[2].y, model.cols[2].z);
  const float32_t determinant = vec3_dot(column0, vec3_cross(column1, column2));
  const float32_t scale =
      vec3_length(column0) * vec3_length(column1) * vec3_length(column2);
  if (!vkr_is_finite_f32(determinant) ||
      !(vkr_abs_f32(determinant) > 1e-6f * scale)) {
    return false_v;
  }

  VkrMeshRaycastEntry *entry =
      mesh_raycast_claim_entry(cache, instance->asset, asset);
  if (!entry || entry->failed) {
    return false_v;
  }
  if (!entry->block) {
    if (asset->mesh_path.length == 0u) {
      /* In-memory geometry has no file to read; insert_triangles seeds it. */
      entry->failed = true_v;
      return false_v;
    }
    if (!mesh_raycast_extract_asset(cache, entry, asset)) {
      log_warn("MeshRaycast: cannot read triangles of '%.*s' source %u",
               (int)asset->mesh_path.length, asset->mesh_path.str,
               asset->source_mesh_index_plus_one);
      entry->failed = true_v;
      return false_v;
    }
  }
  entry->last_use = ++cache->use_clock;

  /* The local segment keeps the world parameter t: the map is affine. */
  const Mat4 inverse = mat4_inverse_affine(model);
  const Vec3 local_origin = mat4_mul_vec3(inverse, origin);
  const Vec4 local_delta4 =
      mat4_mul_vec4(inverse, vec4_new(delta.x, delta.y, delta.z, 0.0f));
  const Vec3 local_delta =
      vec3_new(local_delta4.x, local_delta4.y, local_delta4.z);

  float32_t t = 0.0f;
  Vec3 local_normal = vec3_zero();
  if (!mesh_raycast_traverse(entry, local_origin, local_delta, &t,
                             &local_normal)) {
    return false_v;
  }

  /* Normals map by the inverse transpose: row i of it is column i of the
     inverse. */
  const Vec3 world_normal = vec3_new(vec3_dot(inverse.cols[0], local_normal),
                                     vec3_dot(inverse.cols[1], local_normal),
                                     vec3_dot(inverse.cols[2], local_normal));
  Vec3 normal = mesh_raycast_unit(world_normal);
  if (vec3_dot(normal, delta) > 0.0f) {
    normal = vec3_negate(normal);
  }
  *out_hit = (VkrMeshRaycastHit){
      .position = vec3_add(origin, vec3_scale(delta, t)),
      .normal = normal,
      .fraction = t,
  };
  return true_v;
}

bool8_t vkr_mesh_raycast_cache_insert_triangles(
    VkrMeshRaycastCache *cache, VkrMeshManager *meshes,
    VkrMeshAssetHandle asset_handle, const VkrPackedVec3 *positions,
    uint32_t position_count, const uint32_t *indices, uint32_t index_count) {
  if (!cache || !meshes) {
    return false_v;
  }
  const VkrMeshAsset *asset = mesh_raycast_live_asset(meshes, asset_handle);
  if (!asset) {
    return false_v;
  }
  VkrMeshRaycastEntry *entry =
      mesh_raycast_claim_entry(cache, asset_handle, asset);
  if (!entry) {
    return false_v;
  }
  mesh_raycast_release_entry(cache, entry);
  entry->occupied = true_v;
  entry->asset_generation = asset->generation;
  entry->key_hash = mesh_raycast_key_hash(asset);

  bool8_t success = false_v;
  bool8_t scratch_ready = false_v;
  Arena *scratch_arena = arena_create(MB(16), KB(64));
  VkrAllocator scratch = {.ctx = scratch_arena};
  if (!scratch_arena) {
    goto cleanup;
  }
  scratch_ready = vkr_allocator_arena(&scratch);
  if (!scratch_ready) {
    goto cleanup;
  }
  success = mesh_raycast_build_entry(cache, entry, positions, position_count,
                                     indices, index_count, &scratch);

cleanup:
  if (!success) {
    *entry = (VkrMeshRaycastEntry){0};
  }
  if (scratch_ready) {
    vkr_allocator_release_global_accounting(&scratch);
  }
  if (scratch_arena) {
    arena_destroy(scratch_arena);
  }
  return success;
}
