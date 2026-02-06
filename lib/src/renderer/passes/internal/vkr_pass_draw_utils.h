#pragma once

#include "defines.h"
#include "renderer/renderer_frontend.h"
#include "renderer/vkr_render_packet.h"

/**
 * @brief Resolved draw range for indexed geometry.
 */
typedef struct VkrPassDrawRange {
  const VkrIndexBuffer *index_buffer; /**< Index buffer to draw from */
  uint32_t index_count;               /**< Number of indices to draw */
  uint32_t first_index;               /**< First index offset */
  int32_t vertex_offset;              /**< Vertex offset for non-indexed or base vertex */
} VkrPassDrawRange;

/**
 * @brief Upload instance data into the instance buffer pool.
 * @param rf Renderer frontend
 * @param instances Instance data to upload
 * @param instance_count Number of instances
 * @param out_base_instance Receives the base instance index for the uploaded range
 * @return true on success, false on allocation or upload failure
 */
bool8_t vkr_pass_packet_upload_instances(RendererFrontend *rf,
                                         const VkrInstanceDataGPU *instances,
                                         uint32_t instance_count,
                                         uint32_t *out_base_instance);

/**
 * @brief Resolve mesh instance, asset, and submesh for a draw item.
 * @param rf Renderer frontend
 * @param mesh Mesh handle (instance)
 * @param submesh_index Submesh index within the asset
 * @param out_instance Receives pointer to mesh instance; may be NULL
 * @param out_asset Receives pointer to mesh asset; may be NULL
 * @param out_submesh Receives pointer to asset submesh; may be NULL
 * @param out_instance_state Receives pointer to submesh instance state; may be NULL
 * @return true if all requested outputs were resolved
 */
bool8_t vkr_pass_packet_resolve_instance(
    RendererFrontend *rf, VkrMeshHandle mesh, uint32_t submesh_index,
    VkrMeshInstance **out_instance, VkrMeshAsset **out_asset,
    VkrMeshAssetSubmesh **out_submesh,
    VkrMeshSubmeshInstanceState **out_instance_state);

/**
 * @brief Returns true if the mesh handle refers to a mesh instance (generation != 0).
 * @param mesh Mesh handle to check
 * @return true if instance, false if static mesh or invalid
 */
bool8_t vkr_pass_packet_handle_is_instance(VkrMeshHandle mesh);

/**
 * @brief Resolve non-instanced mesh and submesh for a draw item.
 * @param rf Renderer frontend
 * @param mesh Mesh handle (static mesh)
 * @param submesh_index Submesh index
 * @param out_mesh Receives pointer to mesh; may be NULL
 * @param out_submesh Receives pointer to submesh; may be NULL
 * @return true if all requested outputs were resolved
 */
bool8_t vkr_pass_packet_resolve_mesh(RendererFrontend *rf, VkrMeshHandle mesh,
                                     uint32_t submesh_index,
                                     VkrMesh **out_mesh,
                                     VkrSubMesh **out_submesh);

/**
 * @brief Select draw range and optional opaque index buffer for an asset submesh.
 * @param rf Renderer frontend
 * @param submesh Asset submesh
 * @param allow_opaque If true, may use opaque index buffer when available
 * @param out_range Receives the resolved draw range
 * @return true on success
 */
bool8_t vkr_pass_packet_resolve_draw_range(RendererFrontend *rf,
                                           const VkrMeshAssetSubmesh *submesh,
                                           bool8_t allow_opaque,
                                           VkrPassDrawRange *out_range);

/**
 * @brief Select draw range for non-instanced mesh submeshes.
 * @param rf Renderer frontend
 * @param submesh Mesh submesh
 * @param allow_opaque If true, may use opaque index buffer when available
 * @param out_range Receives the resolved draw range
 * @return true on success
 */
bool8_t vkr_pass_packet_resolve_draw_range_mesh(RendererFrontend *rf,
                                                const VkrSubMesh *submesh,
                                                bool8_t allow_opaque,
                                                VkrPassDrawRange *out_range);

/**
 * @brief Returns the default shader name for a pipeline domain.
 * @param domain Pipeline domain
 * @return Shader name string (static, do not free)
 */
const char *vkr_pass_packet_default_shader_for_domain(VkrPipelineDomain domain);

/**
 * @brief Resolve pipeline handle for material + domain and bind shader.
 * If pipeline_override is valid and matches the domain, it is used instead of the material-driven lookup.
 * @param rf Renderer frontend
 * @param domain Pipeline domain
 * @param material Material for shader lookup (when pipeline_override not used)
 * @param pipeline_override Optional override; used if valid and domain matches
 * @param out_pipeline Receives the resolved pipeline handle
 * @return true on success
 */
bool8_t vkr_pass_packet_resolve_pipeline(RendererFrontend *rf,
                                         VkrPipelineDomain domain,
                                         const VkrMaterial *material,
                                         VkrPipelineHandle pipeline_override,
                                         VkrPipelineHandle *out_pipeline);
