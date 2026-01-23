#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "containers/vkr_hashtable.h"
#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/vkr_buffer.h"
#include "renderer/vkr_renderer.h"

// =============================================================================
// Geometry System - Manages geometry in vertex/index buffers
// =============================================================================

typedef struct VkrGeometrySystemConfig {
  uint32_t max_geometries;
} VkrGeometrySystemConfig;

/**
 * @brief Represents the configuration for a geometry.
 */
typedef struct VkrGeometryConfig {
  /** @brief The size of each vertex in bytes. */
  uint32_t vertex_size;
  /** @brief The number of vertices. */
  uint32_t vertex_count;
  /** @brief Pointer to vertex data. */
  const void *vertices;
  /** @brief The size of each index in bytes. */
  uint32_t index_size;
  /** @brief The number of indices. */
  uint32_t index_count;
  /** @brief Pointer to index data. */
  const void *indices;

  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;

  /** @brief The name of the geometry. */
  char name[GEOMETRY_NAME_MAX_LENGTH];
  /** @brief The name of the material bound to the geometry. */
  char material_name[MATERIAL_NAME_MAX_LENGTH];
} VkrGeometryConfig;

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

  // Default/fallback geometry
  VkrGeometryHandle default_geometry;
  VkrGeometryHandle default_plane;
  VkrGeometryHandle default_plane2d;

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
 * @brief Creates geometry from a configuration.
 * @param system Geometry system instance.
 * @param config Geometry configuration (vertex/index data and metadata).
 * @param auto_release Auto release flag for lifetime management.
 * @param out_error Error output (optional).
 */
VkrGeometryHandle vkr_geometry_system_create(VkrGeometrySystem *system,
                                             const VkrGeometryConfig *config,
                                             bool8_t auto_release,
                                             VkrRendererError *out_error);
/**
 * @brief Creates a cube geometry from the given width, height, and depth.
 * @param system The geometry system to create the cube in
 * @param width The width of the cube
 * @param height The height of the cube
 * @param depth The depth of the cube
 * @param name The name of the cube
 * @param out_error The error output
 */
VkrGeometryHandle
vkr_geometry_system_create_cube(VkrGeometrySystem *system, float32_t width,
                                float32_t height, float32_t depth,
                                const char *name, VkrRendererError *out_error);

/**
 * @brief Creates an axis-aligned box centered at the given position.
 * @param system The geometry system to create the box in.
 * @param center Center position for the box.
 * @param width Box width.
 * @param height Box height.
 * @param depth Box depth.
 * @param auto_release Whether to auto-release when refcount reaches zero.
 * @param name Geometry name for lookup/debugging.
 * @param out_error The error output.
 */
VkrGeometryHandle
vkr_geometry_system_create_box(VkrGeometrySystem *system, Vec3 center,
                               float32_t width, float32_t height,
                               float32_t depth, bool8_t auto_release,
                               const char *name, VkrRendererError *out_error);

/**
 * @brief Creates a cylinder aligned to an axis with optional caps.
 * @param system The geometry system to create the cylinder in.
 * @param radius Cylinder radius.
 * @param height Cylinder height along the axis direction.
 * @param segments Radial segment count (clamped to >= 3).
 * @param axis Axis direction; defaults to +Z when near-zero.
 * @param origin Base position (z=0 plane in local space).
 * @param cap_bottom Whether to generate the bottom cap.
 * @param cap_top Whether to generate the top cap.
 * @param name Geometry name for lookup/debugging.
 * @param out_error The error output.
 */
VkrGeometryHandle vkr_geometry_system_create_cylinder(
    VkrGeometrySystem *system, float32_t radius, float32_t height,
    uint32_t segments, Vec3 axis, Vec3 origin, bool8_t cap_bottom,
    bool8_t cap_top, const char *name, VkrRendererError *out_error);

/**
 * @brief Creates a cone aligned to an axis.
 * @param system The geometry system to create the cone in.
 * @param radius Cone base radius.
 * @param height Cone height along the axis direction.
 * @param segments Radial segment count (clamped to >= 3).
 * @param axis Axis direction; defaults to +Z when near-zero.
 * @param origin Base position (z=0 plane in local space).
 * @param cap_base Whether to generate the base cap.
 * @param name Geometry name for lookup/debugging.
 * @param out_error The error output.
 */
VkrGeometryHandle vkr_geometry_system_create_cone(
    VkrGeometrySystem *system, float32_t radius, float32_t height,
    uint32_t segments, Vec3 axis, Vec3 origin, bool8_t cap_base,
    const char *name, VkrRendererError *out_error);

/**
 * @brief Creates a torus centered at origin with its normal aligned to axis.
 * @param system The geometry system to create the torus in.
 * @param major_radius Distance from center to tube center.
 * @param minor_radius Tube radius.
 * @param major_segments Segment count around the major ring (>= 3).
 * @param minor_segments Segment count around the tube (>= 3).
 * @param axis Torus normal direction; defaults to +Z when near-zero.
 * @param origin Center position.
 * @param name Geometry name for lookup/debugging.
 * @param out_error The error output.
 */
VkrGeometryHandle vkr_geometry_system_create_torus(
    VkrGeometrySystem *system, float32_t major_radius, float32_t minor_radius,
    uint32_t major_segments, uint32_t minor_segments, Vec3 axis, Vec3 origin,
    const char *name, VkrRendererError *out_error);

/**
 * @brief Creates a UV sphere centered at origin with its poles aligned to axis.
 * @param system The geometry system to create the sphere in.
 * @param radius Sphere radius.
 * @param latitude_segments Segment count between poles (clamped to >= 2).
 * @param longitude_segments Segment count around the equator (clamped to >= 3).
 * @param axis Sphere pole direction; defaults to +Z when near-zero.
 * @param origin Center position.
 * @param name Geometry name for lookup/debugging.
 * @param out_error The error output.
 */
VkrGeometryHandle vkr_geometry_system_create_sphere(
    VkrGeometrySystem *system, float32_t radius, uint32_t latitude_segments,
    uint32_t longitude_segments, Vec3 axis, Vec3 origin, const char *name,
    VkrRendererError *out_error);

/**
 * @brief Creates an arrow (cylinder shaft + cone head) aligned to an axis.
 * @param system The geometry system to create the arrow in.
 * @param shaft_length Cylinder shaft length.
 * @param shaft_radius Cylinder shaft radius.
 * @param head_length Cone head length.
 * @param head_radius Cone head radius.
 * @param segments Radial segment count (clamped to >= 3).
 * @param axis Arrow direction; defaults to +Z when near-zero.
 * @param origin Base position (start of shaft).
 * @param name Geometry name for lookup/debugging.
 * @param out_error The error output.
 */
VkrGeometryHandle vkr_geometry_system_create_arrow(
    VkrGeometrySystem *system, float32_t shaft_length,
    float32_t shaft_radius, float32_t head_length, float32_t head_radius,
    uint32_t segments, Vec3 axis, Vec3 origin, const char *name,
    VkrRendererError *out_error);

/**
 * @brief Adds a reference to the geometry
 * @param system The geometry system to add the reference to
 * @param handle The geometry handle to add the reference to
 */
void vkr_geometry_system_acquire(VkrGeometrySystem *system,
                                 VkrGeometryHandle handle);

/**
 * @brief Attempts to acquire a geometry by its registered name.
 * @param system Geometry system instance.
 * @param name Geometry name (case-sensitive, max 64 chars).
 * @param auto_release Whether to auto-release when the refcount reaches zero.
 * @param out_error Optional pointer receiving the error status.
 * @return Valid handle when found; VKR_GEOMETRY_HANDLE_INVALID otherwise.
 */
VkrGeometryHandle
vkr_geometry_system_acquire_by_name(VkrGeometrySystem *system, String8 name,
                                    bool8_t auto_release,
                                    VkrRendererError *out_error);

/**
 * @brief Releases a reference to the geometry
 * @param system The geometry system to release the reference from
 * @param handle The geometry handle to release the reference from
 */
void vkr_geometry_system_release(VkrGeometrySystem *system,
                                 VkrGeometryHandle handle);

/**
 * @brief Returns a pointer to geometry referenced by the handle if valid.
 * @param system Geometry system instance.
 * @param handle Geometry handle to resolve.
 * @return Pointer to geometry or NULL if handle is invalid/stale.
 */
VkrGeometry *vkr_geometry_system_get_by_handle(VkrGeometrySystem *system,
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
void vkr_geometry_system_render_instanced(VkrRendererFrontendHandle renderer,
                                          VkrGeometrySystem *system,
                                          VkrGeometryHandle handle,
                                          uint32_t instance_count,
                                          uint32_t first_instance);
/**
 * @brief Renders a sub-range of a geometry's index buffer.
 *
 * Pass index_count == UINT32_MAX to draw the full geometry range. Callers must
 * supply ranges that stay within the geometry's index buffer.
 */
void vkr_geometry_system_render_instanced_range(VkrRendererFrontendHandle renderer,
                                                VkrGeometrySystem *system,
                                                VkrGeometryHandle handle,
                                                uint32_t index_count,
                                                uint32_t first_index,
                                                int32_t vertex_offset,
                                                uint32_t instance_count,
                                                uint32_t first_instance);

void vkr_geometry_system_render_instanced_range_with_index_buffer(
    VkrRendererFrontendHandle renderer, VkrGeometrySystem *system,
    VkrGeometryHandle handle, const VkrIndexBuffer *index_buffer,
    uint32_t index_count, uint32_t first_index, int32_t vertex_offset,
    uint32_t instance_count, uint32_t first_instance);
void vkr_geometry_system_render_indirect(VkrRendererFrontendHandle renderer,
                                         VkrGeometrySystem *system,
                                         VkrGeometryHandle handle,
                                         VkrBufferHandle indirect_buffer,
                                         uint64_t offset, uint32_t draw_count,
                                         uint32_t stride);

void vkr_geometry_system_render_indirect_with_index_buffer(
    VkrRendererFrontendHandle renderer, VkrGeometrySystem *system,
    VkrGeometryHandle handle, const VkrIndexBuffer *index_buffer,
    VkrBufferHandle indirect_buffer, uint64_t offset, uint32_t draw_count,
    uint32_t stride);

// =============================================================================
// Helpers
// =============================================================================

/**
 * @brief Generates tangents for the given vertices
 * @param allocator The allocator to use for temporary memory
 * @param verts Array of VkrVertex3d vertices to update in-place
 * @param vertex_count The number of vertices
 * @param indices The indices to use
 * @param index_count The number of indices
 */
void vkr_geometry_system_generate_tangents(VkrAllocator *allocator,
                                           VkrVertex3d *verts,
                                           uint32_t vertex_count,
                                           const uint32_t *indices,
                                           uint32_t index_count);

/**
 * @brief Deduplicates a vertex stream and remaps indices in-place.
 * @param system Geometry system that owns the allocator.
 * @param scratch_arena Temporary arena used for hashing and output buffers.
 * @param vertices Source vertices to deduplicate.
 * @param vertex_count Number of source vertices.
 * @param indices Indices to remap. Updated in-place to reference deduplicated
 * vertices.
 * @param index_count Number of indices.
 * @param out_vertices Pointer to receive scratch-allocated unique vertex data.
 * @param out_vertex_count Pointer to receive unique vertex count.
 * @return True if deduplication succeeded; false otherwise.
 */
bool8_t vkr_geometry_system_deduplicate_vertices(
    VkrGeometrySystem *system, VkrAllocator *scratch_alloc,
    const VkrVertex3d *vertices, uint32_t vertex_count, uint32_t *indices,
    uint32_t index_count, VkrVertex3d **out_vertices,
    uint32_t *out_vertex_count);

/**
 * @brief Gets a default cube (2x2x2 by default) using the POSITION_TEXCOORD
 * layout and set as default geometry. Dimensions are full extents.
 * @param system The geometry system to get the default cube from
 */
VkrGeometryHandle
vkr_geometry_system_get_default_geometry(VkrGeometrySystem *system);

/**
 * @brief Gets a default plane (2x2 by default) using the POSITION_TEXCOORD
 * layout and set as default geometry. Dimensions are full extents.
 * @param system The geometry system to get the default plane from
 */
VkrGeometryHandle
vkr_geometry_system_get_default_plane(VkrGeometrySystem *system);

/**
 * @brief Gets a default 2D plane (2x2 by default) using the
 * POSITION2_TEXCOORD layout. Vertex format: [x, y, u, v].
 * @param system The geometry system to get the default 2D plane from
 */
VkrGeometryHandle
vkr_geometry_system_get_default_plane2d(VkrGeometrySystem *system);
