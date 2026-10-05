#include "assets/vkr_mesh_lightmap_uv.h"

#include <xatlas.h>

#include <math.h>

/* The atlas handle is the xatlas atlas itself; xatlas owns its memory until
   vkr_mesh_lightmap_uv_destroy. */
struct VkrMeshLightmapUvAtlas {
  xatlas::Atlas *atlas;
};

extern "C" VkrMeshLightmapUvStatus
vkr_mesh_lightmap_uv_generate(const VkrMeshLightmapUvInput *input,
                              VkrMeshLightmapUvAtlas **out_atlas,
                              VkrMeshLightmapUvInfo *out_info) {
  if (!input || !out_atlas || !out_info || !input->positions ||
      !input->indices || input->vertex_count == 0u ||
      input->index_count == 0u || input->index_count % 3u != 0u ||
      input->position_stride < 3u * sizeof(float) ||
      (input->normals && input->normal_stride < 3u * sizeof(float)) ||
      !(input->texels_per_unit > 0.0f) || !isfinite(input->texels_per_unit) ||
      input->max_size == 0u) {
    return VKR_MESH_LIGHTMAP_UV_INVALID_INPUT;
  }
  *out_atlas = nullptr;
  *out_info = VkrMeshLightmapUvInfo{};

  xatlas::Atlas *atlas = xatlas::Create();
  if (!atlas)
    return VKR_MESH_LIGHTMAP_UV_ADD_MESH_FAILED;

  xatlas::MeshDecl mesh;
  mesh.vertexPositionData = input->positions;
  mesh.vertexPositionStride = input->position_stride;
  mesh.vertexNormalData = input->normals;
  mesh.vertexNormalStride = input->normals ? input->normal_stride : 0u;
  mesh.vertexCount = input->vertex_count;
  mesh.indexData = input->indices;
  mesh.indexCount = input->index_count;
  mesh.indexFormat = xatlas::IndexFormat::UInt32;
  if (xatlas::AddMesh(atlas, mesh, 1u) != xatlas::AddMeshError::Success) {
    xatlas::Destroy(atlas);
    return VKR_MESH_LIGHTMAP_UV_ADD_MESH_FAILED;
  }

  xatlas::ChartOptions chart_options;
  xatlas::PackOptions pack_options;
  pack_options.padding = input->padding;
  pack_options.texelsPerUnit = input->texels_per_unit;
  pack_options.bilinear = true;
  /* Block-compressed lightmap layers keep each chart inside whole 4x4
     blocks. */
  pack_options.blockAlign = true;
  xatlas::Generate(atlas, chart_options, pack_options);

  if (atlas->meshCount != 1u || atlas->width == 0u || atlas->height == 0u ||
      atlas->meshes[0].indexCount != input->index_count) {
    xatlas::Destroy(atlas);
    return VKR_MESH_LIGHTMAP_UV_EMPTY_ATLAS;
  }
  if (atlas->width > input->max_size || atlas->height > input->max_size) {
    out_info->width = atlas->width;
    out_info->height = atlas->height;
    xatlas::Destroy(atlas);
    return VKR_MESH_LIGHTMAP_UV_TOO_LARGE;
  }

  VkrMeshLightmapUvAtlas *result = new VkrMeshLightmapUvAtlas{atlas};
  out_info->vertex_count = atlas->meshes[0].vertexCount;
  out_info->index_count = atlas->meshes[0].indexCount;
  out_info->width = atlas->width;
  out_info->height = atlas->height;
  out_info->chart_count = atlas->meshes[0].chartCount;
  *out_atlas = result;
  return VKR_MESH_LIGHTMAP_UV_OK;
}

extern "C" void vkr_mesh_lightmap_uv_copy(const VkrMeshLightmapUvAtlas *atlas,
                                          uint32_t *out_source_vertices,
                                          float *out_uv2,
                                          uint32_t *out_indices) {
  const xatlas::Mesh &mesh = atlas->atlas->meshes[0];
  const float inverse_width = 1.0f / (float)atlas->atlas->width;
  const float inverse_height = 1.0f / (float)atlas->atlas->height;
  for (uint32_t i = 0u; i < mesh.vertexCount; ++i) {
    const xatlas::Vertex &vertex = mesh.vertexArray[i];
    out_source_vertices[i] = vertex.xref;
    /* Faces xatlas leaves out of every chart (zero area) keep (0, 0); they
       cover no texel. */
    out_uv2[2u * i + 0u] = vertex.uv[0] * inverse_width;
    out_uv2[2u * i + 1u] = vertex.uv[1] * inverse_height;
  }
  for (uint32_t i = 0u; i < mesh.indexCount; ++i)
    out_indices[i] = mesh.indexArray[i];
}

extern "C" void vkr_mesh_lightmap_uv_destroy(VkrMeshLightmapUvAtlas *atlas) {
  if (!atlas)
    return;
  xatlas::Destroy(atlas->atlas);
  delete atlas;
}
