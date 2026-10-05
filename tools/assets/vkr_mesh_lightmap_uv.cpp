#include "assets/vkr_mesh_lightmap_uv.h"

#include <xatlas.h>

#include <math.h>

#include <vector>

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

  /* xatlas treats faces below a fixed area epsilon as degenerate, which
     drops whole meshes authored at small scale. Unwrap a copy scaled to a
     unit largest extent at the matching density: the texel layout is the
     same, and only the epsilon stops depending on the authoring scale. */
  float lower[3] = {INFINITY, INFINITY, INFINITY};
  float upper[3] = {-INFINITY, -INFINITY, -INFINITY};
  for (uint32_t i = 0u; i < input->index_count; ++i) {
    const uint32_t index = input->indices[i];
    if (index >= input->vertex_count)
      return VKR_MESH_LIGHTMAP_UV_INVALID_INPUT;
    const float *position =
        (const float *)((const uint8_t *)input->positions +
                        (size_t)index * input->position_stride);
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      lower[axis] = fminf(lower[axis], position[axis]);
      upper[axis] = fmaxf(upper[axis], position[axis]);
    }
  }
  const float extent = fmaxf(upper[0] - lower[0],
                             fmaxf(upper[1] - lower[1], upper[2] - lower[2]));
  if (!isfinite(extent))
    return VKR_MESH_LIGHTMAP_UV_INVALID_INPUT;
  if (!(extent > 0.0f))
    return VKR_MESH_LIGHTMAP_UV_EMPTY_ATLAS;
  std::vector<float> scaled((size_t)input->vertex_count * 3u);
  for (uint32_t i = 0u; i < input->vertex_count; ++i) {
    const float *position = (const float *)((const uint8_t *)input->positions +
                                            (size_t)i * input->position_stride);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      scaled[3u * i + axis] = (position[axis] - lower[axis]) / extent;
  }

  xatlas::Atlas *atlas = xatlas::Create();
  if (!atlas)
    return VKR_MESH_LIGHTMAP_UV_ADD_MESH_FAILED;

  xatlas::MeshDecl mesh;
  mesh.vertexPositionData = scaled.data();
  mesh.vertexPositionStride = 3u * sizeof(float);
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
  pack_options.texelsPerUnit = input->texels_per_unit * extent;
  pack_options.bilinear = true;
  /* No 4x4 block alignment: Bistro's many sub-texel detail charts each
     grew to a whole block plus padding, so aligned atlases needed 40.3
     million scene texels against 32.1 million unaligned for the same 4.3
     million covered texels. Chart padding still separates block-compressed
     neighbors' filtering. */
  pack_options.blockAlign = false;
  xatlas::Generate(atlas, chart_options, pack_options);

  if (atlas->meshCount != 1u || atlas->width == 0u || atlas->height == 0u ||
      atlas->meshes[0].chartCount == 0u ||
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
