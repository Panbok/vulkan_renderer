/**
 * @file scene_loader.h
 * @brief Scene JSON loader for building VkrScene instances.
 *
 * Parses a JSON scene description, creates ECS entities/components, and
 * loads referenced meshes via the mesh manager.
 */
#pragma once

#include "assets/vkr_mesh_source.h"
#include "containers/str.h"
#include "defines.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_system.h"

struct VkrRenderAssets;

/**
 * @brief Summary of a scene load operation.
 */
typedef struct VkrSceneLoadResult {
  uint32_t entity_count;
  uint32_t mesh_count;
  uint32_t text3d_count;
  uint32_t shape_count;
  uint32_t directional_light_count;
  uint32_t point_light_count;
} VkrSceneLoadResult;

/**
 * @brief Load a scene from a JSON file path.
 *
 * Runs the same prepare and finalize stages as a scene resource load, into
 * the caller's scene. Mesh, animation and material dependencies must load
 * synchronously, as they do inside a synchronous resource load and without a
 * job system; one that is still loading fails the load. A failed load
 * detaches the animations it attached; the entities it created stay in
 * `scene`.
 *
 * @param scene Target scene (must be initialized).
 * @param assets Published asset owner for mesh and text loading.
 * @param path Path to the .scene.json file.
 * @param temp_alloc Temporary allocator for file contents and parsing.
 * @param out_result Optional load summary.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_load_from_file(VkrScene *scene,
                                 struct VkrRenderAssets *assets, String8 path,
                                 VkrAllocator *temp_alloc,
                                 VkrSceneLoadResult *out_result,
                                 VkrSceneError *out_error);

/**
 * @brief Load a scene from a JSON buffer, as vkr_scene_load_from_file does,
 * with paths resolved from the repository root.
 *
 * @param scene Target scene (must be initialized).
 * @param assets Published asset owner for mesh and text loading.
 * @param json Scene JSON buffer (lifetime must cover this call).
 * @param temp_alloc Temporary allocator for parse scratch data.
 * @param out_result Optional load summary.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_load_from_json(VkrScene *scene,
                                 struct VkrRenderAssets *assets, String8 json,
                                 VkrAllocator *temp_alloc,
                                 VkrSceneLoadResult *out_result,
                                 VkrSceneError *out_error);

/**
 * @brief Creates a scene loader for registration with `VkrResourceSystem`.
 *
 * The loader produces `VKR_RESOURCE_TYPE_SCENE` resources where the returned
 * handle is a `VkrSceneHandle` (runtime scene instance).
 */
VkrResourceLoader vkr_scene_loader_create(void);

/* The source fingerprint of a model's wrapper and source nodes: the
   wrapper's document fingerprint `document` mixed with the cooked mesh's
   `mesh` (VkrMeshSource.fingerprint), or `mesh` alone for a wrapper no
   document made. A bake computes the same value (vkr_project_mesh_identity),
   so the overrides of a model's nodes bind to the mesh they were made on. */
uint64_t vkr_scene_source_node_fingerprint(uint64_t document, uint64_t mesh);

/* A change of the source fingerprint of the models of one mesh. */
typedef struct VkrSceneSourceRemap {
  uint64_t from;
  uint64_t to;
} VkrSceneSourceRemap;

/* After a live mesh swap (vkr_mesh_manager_swap_asset_path) a source node's
   instance draws a rebuilt mesh while its identity names the old one. Gives
   every document model whose node draws another mesh, its wrapper and all
   its nodes, and every other entity of the old fingerprint, the identity a
   fresh load of the document computes. Writes the distinct changes, at most
   `capacity`, to `out` and returns their count; zero for a scene no
   document made. A cold pass over the scene's entities. */
uint32_t vkr_scene_refresh_source_fingerprints(VkrScene *scene,
                                               VkrSceneSourceRemap *out,
                                               uint32_t capacity);

/* CPU scene instantiation used by raw/cooked loading. out_nodes has one slot
   per source node and belongs to caller scratch. The scene owns copied names.
   Failure removes created nodes and leaves the wrapper identity unchanged.
 */
bool8_t vkr_scene_instantiate_source_nodes(VkrScene *scene,
                                           const VkrMeshSource *source,
                                           VkrEntityId wrapper,
                                           uint32_t scene_entity_index,
                                           VkrEntityId *out_nodes,
                                           VkrSceneError *out_error);

/**
 * @brief Create the next asynchronous load of `path` as an additive
 * container: its scene gets `world_id` in entity ids and that container's
 * picking range (ADR-076). Zero clears the request. Main thread only.
 */
bool8_t vkr_scene_loader_request_container(struct VkrRenderAssets *assets,
                                           String8 path, uint16_t world_id);
