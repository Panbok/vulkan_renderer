/**
 * @file vkr_mesh_manager.h
 * @brief Manages the creation, destruction, and management of meshes. Mesh
 * is a collection of geometry, material, and transform. It acquires and
 * releases handles to the geometry, material systems and pipeline registry. It
 * also manages the pipeline state for the meshes.
 */
#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "containers/vkr_hashtable.h"
#include "defines.h"
#include "math/vkr_transform.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_dmemory.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/vkr_renderer.h"

// ============================================================================
// Types
// ============================================================================

/**
 * @brief Configuration for the mesh manager.
 * @param max_mesh_count The maximum number of meshes to manage.
 */
typedef struct VkrMeshManagerConfig {
  uint32_t max_mesh_count;
} VkrMeshManagerConfig;

/**
 * @brief Description for a submesh.
 * @param geometry Geometry handle (optional if geometry_name is provided).
 * @param material Material handle (optional if material_name is provided).
 * @param geometry_name Name of the geometry resource to acquire.
 * @param material_name Name of the material resource to acquire.
 * @param shader_override Optional shader override for the submesh.
 * @param pipeline_domain Rendering domain for the submesh.
 * @param range_id Stable identifier for a draw range within a shared geometry
 * buffer (used by batching). Use unique values when submeshes share the same
 * geometry but draw different ranges.
 * @param first_index Starting index within the bound index buffer.
 * @param index_count Index count for the range (0 uses full geometry).
 * @param vertex_offset Vertex offset applied to indices (typically 0 when
 * indices are absolute).
 * @param opaque_first_index Starting index in the compacted opaque buffer.
 * @param opaque_index_count Index count for the opaque-only range.
 * @param opaque_vertex_offset Vertex offset for opaque-only range.
 * @param center Range-local center in mesh space.
 * @param min_extents Minimum extents relative to center.
 * @param max_extents Maximum extents relative to center.
 * @param owns_geometry Indicates if the mesh manager should release the
 * geometry handle when the submesh is destroyed.
 * @param owns_material Indicates if the mesh manager should release the
 * material handle when the submesh is destroyed.
 */
typedef struct VkrSubMeshDesc {
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  String8 geometry_name;
  String8 material_name;
  String8 shader_override;
  VkrPipelineDomain pipeline_domain;
  uint32_t range_id;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t opaque_first_index;
  uint32_t opaque_index_count;
  int32_t opaque_vertex_offset;
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
  bool8_t owns_geometry;
  bool8_t owns_material;
} VkrSubMeshDesc;

typedef struct VkrMeshLoadDesc {
  String8 mesh_path;
  VkrTransform transform;
  VkrPipelineDomain pipeline_domain;
  String8 shader_override;
} VkrMeshLoadDesc;

/**
 * @brief Description for a mesh composed of multiple submeshes.
 * @param transform Per-mesh transform/root.
 * @param submeshes Array of submesh descriptors.
 * @param submesh_count Number of submeshes described.
 */
typedef struct VkrMeshDesc {
  VkrTransform transform;
  const VkrSubMeshDesc *submeshes;
  uint32_t submesh_count;
} VkrMeshDesc;

// Forward declaration
typedef struct VkrMeshLoaderContext VkrMeshLoaderContext;

/**
 * @brief Hash table entry for mesh asset lookup by key string.
 *
 * The key is a string combining mesh_path, pipeline_domain, and
 * shader_override. Format: "mesh_path|domain|shader_override"
 */
typedef struct VkrMeshAssetEntry {
  uint32_t asset_index; // Index into mesh_assets array
  const char *key;      // Owned key string (allocated from asset_dmemory)
} VkrMeshAssetEntry;
VkrHashTable(VkrMeshAssetEntry);

/**
 * @brief Manager for the mesh system.
 * @param arena The arena to use for the mesh manager.
 * @param scratch_arena The scratch arena to use for the mesh manager.
 * @param geometry_system The geometry system to use for the mesh manager.
 * @param material_system The material system to use for the mesh manager.
 * @param pipeline_registry The pipeline registry to use for the mesh manager.
 * @param config The configuration for the mesh manager.
 * @param meshes The meshes managed by the mesh manager.
 * @param free_indices The indices of the free meshes.
 */
typedef struct VkrMeshManager {
  Arena *arena;
  Arena *scratch_arena;
  VkrAllocator allocator;
  VkrAllocator scratch_allocator;
  VkrGeometrySystem *geometry_system;
  VkrMaterialSystem *material_system;
  VkrPipelineRegistry *pipeline_registry;
  VkrMeshLoaderContext *loader_context; // For batch loading
  VkrMeshManagerConfig config;

  Array_VkrMesh meshes;
  Array_uint32_t mesh_live_indices;
  Array_uint32_t free_indices;
  uint32_t free_count;
  uint32_t mesh_count;
  uint32_t next_free_index;

  VkrDMemory asset_dmemory; // Freeable allocator for asset strings/keys
  VkrAllocator asset_allocator;
  Array_VkrMeshAsset mesh_assets;
  Array_uint32_t asset_free_indices;
  uint32_t asset_free_count;
  uint32_t asset_count;
  uint32_t next_asset_index;
  uint32_t asset_generation_counter;
  VkrHashTable_VkrMeshAssetEntry asset_by_key;

  // Freeable allocator for per-instance state arrays
  VkrDMemory instance_dmemory;
  VkrAllocator instance_allocator;

  // Instance registry: per-entity mesh state
  Array_VkrMeshInstance mesh_instances;
  Array_uint32_t instance_live_indices;
  Array_uint32_t instance_free_indices;
  uint32_t instance_free_count;
  uint32_t instance_count;
  uint32_t next_instance_index;
  uint32_t instance_generation_counter;
} VkrMeshManager;

// ============================================================================
// Functions
// ============================================================================

/**
 * @brief Initializes the mesh manager.
 * @param manager The mesh manager to initialize.
 * @param geometry_system The geometry system to use for the mesh manager.
 * @param material_system The material system to use for the mesh manager.
 * @param pipeline_registry The pipeline registry to use for the mesh manager.
 * @param config The configuration for the mesh manager.
 * @return true if the mesh manager was initialized successfully, false
 * otherwise.
 */
bool8_t vkr_mesh_manager_init(VkrMeshManager *manager,
                              VkrGeometrySystem *geometry_system,
                              VkrMaterialSystem *material_system,
                              VkrPipelineRegistry *pipeline_registry,
                              const VkrMeshManagerConfig *config);

/**
 * @brief Shuts down the mesh manager.
 * @param manager The mesh manager to shutdown.
 */
void vkr_mesh_manager_shutdown(VkrMeshManager *manager);

/**
 * @brief Creates a mesh based on a description.
 * @param manager The mesh manager to create the mesh in.
 * @param desc The description for the mesh.
 * @param out_error The error code for the operation.
 * @param out_mesh The mesh created.
 * @return true if the mesh was created successfully, false otherwise.
 */
bool8_t vkr_mesh_manager_create(VkrMeshManager *manager,
                                const VkrMeshDesc *desc,
                                VkrRendererError *out_error,
                                VkrMesh **out_mesh);

/**
 * @brief Adds a mesh based on descriptor and returns its slot index.
 * @param manager The mesh manager to add the mesh to.
 * @param desc The description for the mesh.
 * @param out_index The index of the mesh in the mesh manager.
 * @param out_error The error code for the operation.
 * @return true if the mesh was added successfully, false otherwise.
 */
bool8_t vkr_mesh_manager_add(VkrMeshManager *manager, const VkrMeshDesc *desc,
                             uint32_t *out_index, VkrRendererError *out_error);

bool8_t vkr_mesh_manager_load(VkrMeshManager *manager,
                              const VkrMeshLoadDesc *desc,
                              uint32_t *out_first_index,
                              uint32_t *out_mesh_count,
                              VkrRendererError *out_error);

/**
 * @brief Batch load multiple meshes with parallel file I/O and material
 * loading.
 *
 * This function loads all meshes in parallel, batch loads all materials
 * and textures across all meshes, then creates the mesh entries.
 *
 * @param manager The mesh manager to load the meshes into.
 * @param descs Array of mesh load descriptors.
 * @param count Number of meshes to load.
 * @param out_indices Optional array to receive mesh indices (size = count).
 * @param out_errors Optional array to receive per-mesh errors (size = count).
 * @return Number of meshes successfully loaded.
 */
uint32_t vkr_mesh_manager_load_batch(VkrMeshManager *manager,
                                     const VkrMeshLoadDesc *descs,
                                     uint32_t count, uint32_t *out_indices,
                                     VkrRendererError *out_errors);

/**
 * @brief Removes a mesh by index.
 * @param manager The mesh manager to remove the mesh from.
 * @param index The index of the mesh to remove.
 * @return true if the mesh was removed successfully, false otherwise.
 */
bool8_t vkr_mesh_manager_remove(VkrMeshManager *manager, uint32_t index);

/**
 * @brief Returns pointer to mesh by index (NULL if invalid).
 * @param manager The mesh manager to get the mesh from.
 * @param index The index of the mesh to get.
 * @return The mesh at the given index.
 */
VkrMesh *vkr_mesh_manager_get(VkrMeshManager *manager, uint32_t index);

/**
 * @brief Get pointer to mesh by live index.
 *
 * Live index is a dense list of active mesh-slot entries (0..mesh_count-1).
 *
 * @param manager The mesh manager.
 * @param live_index Index into the active mesh list.
 * @param out_slot Optional pointer to receive the slot index.
 * @return Pointer to mesh, or NULL if invalid.
 */
VkrMesh *vkr_mesh_manager_get_mesh_by_live_index(VkrMeshManager *manager,
                                                 uint32_t live_index,
                                                 uint32_t *out_slot);

/**
 * @brief Returns the number of meshes in the mesh manager.
 * @param manager The mesh manager to get the count from.
 * @return The number of meshes in the mesh manager.
 */
uint32_t vkr_mesh_manager_count(const VkrMeshManager *manager);

/**
 * @brief Returns the capacity of the mesh manager.
 * @param manager The mesh manager to get the capacity from.
 * @return The capacity of the mesh manager.
 */
uint32_t vkr_mesh_manager_capacity(const VkrMeshManager *manager);

/**
 * @brief Sets material handle on mesh and marks pipeline dirty.
 * @param manager The mesh manager to set the material on.
 * @param index The index of the mesh to set the material on.
 * @param material The material to set on the mesh.
 * @param out_error The error code for the operation.
 * @return true if the material was set successfully, false otherwise.
 */
bool8_t vkr_mesh_manager_set_submesh_material(VkrMeshManager *manager,
                                              uint32_t mesh_index,
                                              uint32_t submesh_index,
                                              VkrMaterialHandle material,
                                              VkrRendererError *out_error);

/**
 * @brief Ensures mesh has valid pipeline/instance state for draw.
 * @param manager The mesh manager to refresh the pipeline for.
 * @param mesh_index The index of the mesh to refresh the pipeline for.
 * @param submesh_index The index of the submesh to refresh within the mesh.
 * @param desired_pipeline The desired pipeline to refresh the mesh to.
 * @param out_error The error code for the operation.
 * @return true if the pipeline was refreshed successfully, false otherwise.
 */
bool8_t vkr_mesh_manager_refresh_pipeline(VkrMeshManager *manager,
                                          uint32_t mesh_index,
                                          uint32_t submesh_index,
                                          VkrPipelineHandle desired_pipeline,
                                          VkrRendererError *out_error);

/**
 * @brief Marks mesh transform dirty and recomputes cached model matrix.
 * @param manager The mesh manager to update the model for.
 * @param index The index of the mesh to update the model for.
 */
void vkr_mesh_manager_update_model(VkrMeshManager *manager, uint32_t index);

/**
 * @brief Set mesh model matrix directly and update world bounds.
 * Use this for ECS-driven transforms where the scene system manages transforms
 * externally from the mesh's VkrTransform.
 * @param manager The mesh manager.
 * @param index The index of the mesh.
 * @param model The world matrix to set.
 * @return true if the model was set successfully, false otherwise.
 */
bool8_t vkr_mesh_manager_set_model(VkrMeshManager *manager, uint32_t index,
                                   Mat4 model);

/**
 * @brief Set mesh visibility flag used by view/picking systems.
 * @param manager The mesh manager.
 * @param index The index of the mesh.
 * @param visible True to render and pick, false to skip.
 * @return true if the visibility was set successfully, false otherwise.
 */
bool8_t vkr_mesh_manager_set_visible(VkrMeshManager *manager, uint32_t index,
                                     bool8_t visible);

/**
 * @brief Set mesh render_id used for picking.
 * @param manager The mesh manager.
 * @param index The index of the mesh.
 * @param render_id Persistent render id (0 disables picking).
 * @return true if the render id was set successfully, false otherwise.
 */
bool8_t vkr_mesh_manager_set_render_id(VkrMeshManager *manager, uint32_t index,
                                       uint32_t render_id);

/**
 * @brief Returns number of submeshes for a mesh.
 */
uint32_t vkr_mesh_manager_submesh_count(const VkrMesh *mesh);

/**
 * @brief Returns pointer to a specific submesh.
 */
VkrSubMesh *vkr_mesh_manager_get_submesh(VkrMeshManager *manager,
                                         uint32_t mesh_index,
                                         uint32_t submesh_index);

// ============================================================================
// Mesh Asset API
// ============================================================================

/**
 * @brief Acquire a mesh asset by key, creating it if necessary.
 *
 * If an asset with the given key already exists, increments its ref_count and
 * returns the handle. Otherwise, loads the mesh file, creates the asset, and
 * inserts it into the registry.
 *
 * @param manager The mesh manager.
 * @param mesh_path Path to the mesh file.
 * @param domain Pipeline domain for submeshes.
 * @param shader_override Optional shader override (can be empty).
 * @param out_error Error code if acquisition fails.
 * @return Handle to the asset, or VKR_MESH_ASSET_HANDLE_INVALID on failure.
 */
VkrMeshAssetHandle vkr_mesh_manager_acquire_asset(VkrMeshManager *manager,
                                                  String8 mesh_path,
                                                  VkrPipelineDomain domain,
                                                  String8 shader_override,
                                                  VkrRendererError *out_error);

/**
 * @brief Release a mesh asset reference.
 *
 * Decrements the asset's ref_count and destroys the asset when it reaches 0.
 *
 * @param manager The mesh manager.
 * @param asset Handle to the asset.
 */
void vkr_mesh_manager_release_asset(VkrMeshManager *manager,
                                    VkrMeshAssetHandle asset);

/**
 * @brief Get pointer to mesh asset by handle.
 *
 * @param manager The mesh manager.
 * @param handle Asset handle.
 * @return Pointer to asset, or NULL if handle is invalid.
 */
VkrMeshAsset *vkr_mesh_manager_get_asset(VkrMeshManager *manager,
                                         VkrMeshAssetHandle handle);

/**
 * @brief Get count of loaded mesh assets.
 */
uint32_t vkr_mesh_manager_asset_count(const VkrMeshManager *manager);

// ============================================================================
// Mesh Instance API
// ============================================================================

/**
 * @brief Create a mesh instance referencing a shared asset.
 *
 * Allocates per-submesh instance state sized to match asset's submesh count.
 * Increments the asset's ref_count.
 *
 * @param manager The mesh manager.
 * @param asset Handle to the asset this instance references.
 * @param model Initial model matrix.
 * @param render_id Render ID for picking (0 disables picking).
 * @param visible Initial visibility state.
 * @param out_error Error code if creation fails.
 * @return Handle to the instance, or VKR_MESH_INSTANCE_HANDLE_INVALID on
 * failure.
 */
VkrMeshInstanceHandle vkr_mesh_manager_create_instance(
    VkrMeshManager *manager, VkrMeshAssetHandle asset, Mat4 model,
    uint32_t render_id, bool8_t visible, VkrRendererError *out_error);

/**
 * @brief Batch create mesh instances from load descriptors.
 *
 * Acquires assets for unique keys (loading if necessary), then creates
 * instances. This is the recommended entry point for scene loading.
 *
 * @param manager The mesh manager.
 * @param descs Array of mesh load descriptors.
 * @param count Number of instances to create.
 * @param out_instances Optional array to receive instance handles (size =
 * count).
 * @param out_errors Optional array to receive per-instance errors (size =
 * count).
 * @return Number of instances successfully created.
 */
uint32_t vkr_mesh_manager_create_instances_batch(
    VkrMeshManager *manager, const VkrMeshLoadDesc *descs, uint32_t count,
    VkrMeshInstanceHandle *out_instances, VkrRendererError *out_errors);

/**
 * @brief Destroy a mesh instance.
 *
 * Releases pipeline state, decrements asset ref_count, and frees the slot.
 *
 * @param manager The mesh manager.
 * @param instance Handle to the instance.
 * @return true if destroyed, false if handle was invalid.
 */
bool8_t vkr_mesh_manager_destroy_instance(VkrMeshManager *manager,
                                          VkrMeshInstanceHandle instance);

/**
 * @brief Get pointer to mesh instance by handle.
 *
 * @param manager The mesh manager.
 * @param handle Instance handle.
 * @return Pointer to instance, or NULL if handle is invalid.
 */
VkrMeshInstance *vkr_mesh_manager_get_instance(VkrMeshManager *manager,
                                               VkrMeshInstanceHandle handle);

/**
 * @brief Get pointer to mesh instance by slot index.
 *
 * For direct access; returns instance at given slot if valid.
 *
 * @param manager The mesh manager.
 * @param index Slot index (0 to instance_capacity - 1).
 * @return Pointer to instance, or NULL if invalid/destroyed.
 */
VkrMeshInstance *vkr_mesh_manager_get_instance_by_index(VkrMeshManager *manager,
                                                        uint32_t index);

/**
 * @brief Get pointer to mesh instance by live index.
 *
 * Live index is a dense list of active instances (0..instance_count-1).
 *
 * @param manager The mesh manager.
 * @param live_index Index into the active instance list.
 * @param out_slot Optional pointer to receive the slot index.
 * @return Pointer to instance, or NULL if invalid.
 */
VkrMeshInstance *vkr_mesh_manager_get_instance_by_live_index(
    VkrMeshManager *manager, uint32_t live_index, uint32_t *out_slot);

/**
 * @brief Set model matrix on instance and update world bounds.
 */
bool8_t vkr_mesh_manager_instance_set_model(VkrMeshManager *manager,
                                            VkrMeshInstanceHandle instance,
                                            Mat4 model);

/**
 * @brief Set visibility on instance.
 */
bool8_t vkr_mesh_manager_instance_set_visible(VkrMeshManager *manager,
                                              VkrMeshInstanceHandle instance,
                                              bool8_t visible);

/**
 * @brief Set render_id on instance.
 */
bool8_t vkr_mesh_manager_instance_set_render_id(VkrMeshManager *manager,
                                                VkrMeshInstanceHandle instance,
                                                uint32_t render_id);

/**
 * @brief Refresh pipeline state for an instance's submesh.
 */
bool8_t vkr_mesh_manager_instance_refresh_pipeline(
    VkrMeshManager *manager, VkrMeshInstanceHandle instance,
    uint32_t submesh_index, VkrPipelineHandle desired_pipeline,
    VkrRendererError *out_error);

/**
 * @brief Get count of live mesh instances.
 */
uint32_t vkr_mesh_manager_instance_count(const VkrMeshManager *manager);

/**
 * @brief Get capacity of mesh instance storage.
 */
uint32_t vkr_mesh_manager_instance_capacity(const VkrMeshManager *manager);
