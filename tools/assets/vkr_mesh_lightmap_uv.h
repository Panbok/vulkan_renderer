#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lightmap UV generation for one mesh through xatlas. The caller keeps the
 * input arrays alive for the call. Output faces keep the input face order;
 * charts split vertices at their seams, so each output vertex names the input
 * vertex it copies. UV2 is normalized to [0,1] over one atlas whose texel size
 * follows texels_per_unit. */
typedef struct VkrMeshLightmapUvInput {
  const float *positions;   /* xyz at each vertex, position_stride apart */
  uint32_t position_stride; /* bytes */
  const float *normals;     /* optional xyz, normal_stride apart */
  uint32_t normal_stride;   /* bytes */
  const uint32_t *indices;  /* triangle list */
  uint32_t index_count;
  uint32_t vertex_count;
  float texels_per_unit; /* lightmap texels per mesh unit */
  uint32_t padding;      /* texels between charts */
  uint32_t max_size;     /* largest accepted atlas edge in texels */
} VkrMeshLightmapUvInput;

typedef enum VkrMeshLightmapUvStatus {
  VKR_MESH_LIGHTMAP_UV_OK = 0,
  VKR_MESH_LIGHTMAP_UV_INVALID_INPUT = 1,
  VKR_MESH_LIGHTMAP_UV_ADD_MESH_FAILED = 2,
  /* No face forms a chart: the geometry is degenerate. */
  VKR_MESH_LIGHTMAP_UV_EMPTY_ATLAS = 3,
  VKR_MESH_LIGHTMAP_UV_TOO_LARGE = 4,
} VkrMeshLightmapUvStatus;

typedef struct VkrMeshLightmapUvAtlas VkrMeshLightmapUvAtlas;

typedef struct VkrMeshLightmapUvInfo {
  uint32_t vertex_count; /* output vertices */
  uint32_t index_count;  /* equals the input index count */
  uint32_t width;        /* atlas edge in texels */
  uint32_t height;
  uint32_t chart_count;
} VkrMeshLightmapUvInfo;

VkrMeshLightmapUvStatus
vkr_mesh_lightmap_uv_generate(const VkrMeshLightmapUvInput *input,
                              VkrMeshLightmapUvAtlas **out_atlas,
                              VkrMeshLightmapUvInfo *out_info);

/* Copies the result into caller arrays sized by the info counts: one input
 * vertex index and one normalized UV2 pair per output vertex, and the output
 * index list. */
void vkr_mesh_lightmap_uv_copy(const VkrMeshLightmapUvAtlas *atlas,
                               uint32_t *out_source_vertices, float *out_uv2,
                               uint32_t *out_indices);

void vkr_mesh_lightmap_uv_destroy(VkrMeshLightmapUvAtlas *atlas);

#ifdef __cplusplus
}
#endif
