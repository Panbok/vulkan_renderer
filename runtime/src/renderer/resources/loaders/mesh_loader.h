#pragma once

#include "assets/vkr_mesh_source.h"
#include "containers/array.h"
#include "containers/str.h"
#include "core/vkr_job_system.h"
#include "math/vkr_transform.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "memory/vkr_dmemory.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_resource_system.h"
#include "vkr_packed_geometry.h"

// =============================================================================
// Mesh Loader Types
// =============================================================================

typedef struct VkrMeshLoaderSubset {
  VkrGeometryConfig geometry_config;
  String8 material_name;
  String8 shader_override;
  VkrPipelineDomain pipeline_domain;
  VkrMaterialHandle material_handle;
} VkrMeshLoaderSubset;
Array(VkrMeshLoaderSubset);

/**
 * @brief CPU-side mesh buffer payload owned by the loader arena.
 *
 * The data pointers remain valid until the loader result is unloaded.
 */
typedef struct VkrMeshLoaderBuffer {
  uint32_t vertex_size;
  uint32_t vertex_count;
  void *vertices;
  uint32_t index_size;
  uint32_t index_count;
  void *indices;
  VkrGpuVertexLayout vertex_layout;
  /** Arena-owned contiguous decode records indexed by submesh.decode_index. */
  VkrGpuGeometryDecodeRecord *decodes;
  uint32_t decode_count;
  VkrGeometryQuantizationMetrics quantization;
} VkrMeshLoaderBuffer;

Array(VkrMaterialHandle);

typedef struct VkrMeshLoaderResult {
  Arena *arena;           /**< Buffer-backed arena for mesh data */
  void *pool_chunk;       /**< Chunk pointer for returning to pool (NULL if not
                             pooled) */
  VkrAllocator allocator; /**< Arena allocator wrapper (used for accounting) */
  String8 source_path;
  VkrTransform root_transform;
  VkrMeshSource source;
  bool8_t
      has_mesh_buffer; /**< True when mesh_buffer/submeshes are populated. */
  VkrMeshLoaderBuffer mesh_buffer; /**< Merged vertex/index payload. */
  VkrMeshLoadMetrics load_metrics;
  Array_VkrGeometryUploadRange submeshes; /**< Decoder-owned geometry ranges. */
  /** Runtime material references, indexed by submeshes; released before arena
   * return. */
  Array_VkrMaterialHandle material_handles;
  Array_VkrMeshLoaderSubset subsets;
} VkrMeshLoaderResult;

typedef struct VkrMeshLoaderContext {
  VkrAllocator allocator;
  VkrDMemory async_memory;
  VkrAllocator async_allocator;
  VkrMutex async_mutex;
  VkrGeometrySystem *geometry_system;
  VkrMaterialSystem *material_system;
  VkrMeshManager *mesh_manager;
  VkrJobSystem *job_system; /**< For async mesh loading */
  VkrArenaPool *arena_pool; /**< Pool for mesh loading arenas (optional) */
} VkrMeshLoaderContext;

// =============================================================================
// Resource Loader Factory
// =============================================================================

/**
 * @brief Creates a mesh resource loader.
 *
 * The loader supports both single-item and batch loading through the resource
 * system. Use vkr_resource_system_load() for single meshes and
 * vkr_resource_system_load_batch() for parallel batch loading.
 *
 * @param context The mesh loader context (stored as resource_system pointer)
 * @return The configured resource loader
 */
VkrResourceLoader vkr_mesh_loader_create(VkrMeshLoaderContext *context);
