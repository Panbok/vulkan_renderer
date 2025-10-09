#pragma once

#include "containers/array.h"
#include "containers/vkr_freelist.h"
#include "containers/vkr_hashtable.h"
#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/vkr_buffer.h"
#include "renderer/vkr_renderer.h"

// =============================================================================
// Geometry System - Manages pooled geometry in shared vertex/index buffers
// =============================================================================

typedef struct VkrGeometrySystemConfig {
  uint32_t default_max_geometries;
  uint64_t
      default_max_vertices; // in vertices, not bytes (per layout pool default)
  uint64_t
      default_max_indices; // in indices, not bytes (per layout pool default)
  uint32_t default_vertex_stride_bytes; // default stride for primary layout
  VkrGeometryVertexLayoutType
      primary_layout; // primary layout to initialize eagerly
} VkrGeometrySystemConfig;

// Lifetime entry stored only in a hash table keyed by geometry name.
// 'id' is the index into the geometries array. This structure manages
// references and auto-release behavior only.
typedef struct VkrGeometryEntry {
  uint32_t id;          // index into geometries array
  uint32_t ref_count;   // number of holders
  bool8_t auto_release; // release when ref_count hits 0
  const char *name;     // geometry name (hash key)
} VkrGeometryEntry;

VkrHashTable(VkrGeometryEntry);

typedef struct VkrGeometryPool {
  bool8_t initialized;
  VkrGeometryVertexLayoutType layout;
  uint32_t vertex_stride_bytes;

  uint64_t capacity_vertices; // in vertices
  uint64_t capacity_indices;  // in indices

  // Shared GPU buffers
  VkrVertexBuffer vertex_buffer;
  VkrIndexBuffer index_buffer;

  // Free space management in BYTES
  void *vertex_freelist_memory; // Memory block for vertex freelist nodes
  void *index_freelist_memory;  // Memory block for index freelist nodes
  VkrFreeList vertex_freelist;  // total size = capacity_vertices * stride
  VkrFreeList
      index_freelist; // total size = capacity_indices * sizeof(uint32_t)
} VkrGeometryPool;

typedef struct VkrGeometrySystem {
  // Internal arena for CPU-side allocations owned by the geometry system
  Arena *arena;
  VkrAllocator allocator;

  VkrRendererFrontendHandle renderer;

  // Geometry storage and ID free list
  Array_VkrGeometry geometries;
  Array_uint32_t free_ids; // stack of free indices
  uint32_t free_count;
  uint32_t max_geometries;

  // Monotonic generation for geometry handles
  uint32_t generation_counter;

  // Config used when lazily initializing new layout pools
  VkrGeometrySystemConfig config;

  // Pools per layout
  VkrGeometryPool pools[GEOMETRY_VERTEX_LAYOUT_COUNT];

  // Default/fallback geometry
  VkrGeometryHandle default_geometry;

  // Lifetime map: name -> entry (ref_count/auto_release/index)
  VkrHashTable_VkrGeometryEntry geometry_by_name;
} VkrGeometrySystem;

// =============================================================================
// Initialization / Shutdown
// =============================================================================

/**
 * @brief Initializes the geometry system
 * @param system The geometry system to initialize
 * @param renderer The renderer to use
 * @param config The configuration for the geometry system
 * @param out_error The error output
 */
bool32_t vkr_geometry_system_init(VkrGeometrySystem *system,
                                  VkrRendererFrontendHandle renderer,
                                  const VkrGeometrySystemConfig *config,
                                  VkrRendererError *out_error);

/**
 * @brief Shuts down the geometry system
 * @param system The geometry system to shutdown
 */
void vkr_geometry_system_shutdown(VkrGeometrySystem *system);

// =============================================================================
// Geometry Creation/Release
// =============================================================================

/**
 * @brief Creates geometry from interleaved vertices matching the provided
 * layout.
 * @param system The geometry system to create the geometry from
 * @param layout The layout to use
 * @param vertices The vertices to use
 * @param vertex_count The number of vertices
 * @param indices The indices to use
 * @param index_count The number of indices
 * @param auto_release The auto release flag
 * @param debug_name The debug name to use
 * @param out_error The error output
 */
VkrGeometryHandle vkr_geometry_system_create_from_interleaved(
    VkrGeometrySystem *system, VkrGeometryVertexLayoutType layout,
    const void *vertices, uint32_t vertex_count, const uint32_t *indices,
    uint32_t index_count, bool8_t auto_release, String8 debug_name,
    VkrRendererError *out_error);

/**
 * @brief Adds a reference to the geometry
 * @param system The geometry system to add the reference to
 * @param handle The geometry handle to add the reference to
 */
void vkr_geometry_system_acquire(VkrGeometrySystem *system,
                                 VkrGeometryHandle handle);

/**
 * @brief Releases a reference to the geometry
 * @param system The geometry system to release the reference from
 * @param handle The geometry handle to release the reference from
 */
void vkr_geometry_system_release(VkrGeometrySystem *system,
                                 VkrGeometryHandle handle);

// =============================================================================
// Drawing
// =============================================================================

/**
 * @brief Renders the geometry
 * @param renderer The renderer to use
 * @param system The geometry system to render
 * @param handle The geometry handle to render
 * @param instance_count The number of instances to render
 */
void vkr_geometry_system_render(VkrRendererFrontendHandle renderer,
                                VkrGeometrySystem *system,
                                VkrGeometryHandle handle,
                                uint32_t instance_count);

// =============================================================================
// Helpers
// =============================================================================

/**
 * @brief Creates a unit cube (2x2x2 by default) using the POSITION_TEXCOORD
 * layout and set as default geometry. Dimensions are full extents.
 * @param system The geometry system to create the default cube in
 * @param width The width of the cube
 * @param height The height of the cube
 * @param depth The depth of the cube
 * @param out_error The error output
 */
VkrGeometryHandle vkr_geometry_system_create_default_cube(
    VkrGeometrySystem *system, float32_t width, float32_t height,
    float32_t depth, VkrRendererError *out_error);

/**
 * @brief Creates a default plane (2x2 by default) using the POSITION_TEXCOORD
 * layout and set as default geometry. Dimensions are full extents.
 * @param system The geometry system to create the default plane in
 * @param width The width of the plane
 * @param height The height of the plane
 * @param out_error The error output
 */
VkrGeometryHandle
vkr_geometry_system_create_default_plane(VkrGeometrySystem *system,
                                         float32_t width, float32_t height,
                                         VkrRendererError *out_error);

/**
 * @brief Allocates and fills vertex input descriptions for the given layout.
 * Memory is allocated from the provided arena. Returns stride via out_stride.
 * @param layout The layout to use
 * @param allocator The allocator to allocate the vertex input descriptions from
 * @param out_attr_count The number of attributes
 * @param out_bindings The bindings
 * @param out_attrs The attributes
 * @param out_attr_count The number of attributes
 * @param out_binding_count The number of bindings
 * @param out_stride The stride
 */
void vkr_geometry_fill_vertex_input_descriptions(
    VkrGeometryVertexLayoutType layout, VkrAllocator *allocator,
    uint32_t *out_attr_count, VkrVertexInputAttributeDescription **out_attrs,
    uint32_t *out_binding_count,
    VkrVertexInputBindingDescription **out_bindings, uint32_t *out_stride);
