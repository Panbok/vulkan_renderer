#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "vkr_gpu_abi.h"
#include "vkr_render_resources.h"
#include "vkr_renderer.h"

/**
 * Borrowed CPU geometry bytes and decode records consumed during GPU asset
 * publication. The publisher copies the data before this view expires.
 */
typedef struct VkrGeometryUploadBuffer {
  uint32_t vertex_size;
  uint32_t vertex_count;
  const void *vertices;
  uint32_t index_size;
  uint32_t index_count;
  const void *indices;
  VkrGpuVertexLayout vertex_layout;
  const VkrGpuGeometryDecodeRecord *decodes;
  uint32_t decode_count;
} VkrGeometryUploadBuffer;

/**
 * One draw range inside a shared upload buffer. Material identity remains a
 * runtime concern; the handle is resolved before publication.
 */
typedef struct VkrGeometryUploadRange {
  uint32_t range_id;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t decode_index;
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
  String8 material_name;
  String8 shader_override;
  VkrPipelineDomain pipeline_domain;
} VkrGeometryUploadRange;
Array(VkrGeometryUploadRange);

/* Distance volumes of the tiled pipeline's SDF sun shadows (ADR-087), which
 * the mesh cooker writes and the Metal backend generates for submeshes
 * without one: at most VKR_GEOMETRY_DISTANCE_MAX_DIM voxels a side with a
 * border of VKR_GEOMETRY_DISTANCE_PAD_VOXELS, voxels no smaller than
 * VKR_GEOMETRY_DISTANCE_MIN_VOXEL world metres (mesh units when cooked), and
 * each voxel the unsigned distance from its centre to the nearest triangle
 * over VKR_GEOMETRY_DISTANCE_BAND_VOXELS, rounded to 8 bits. Submeshes under
 * VKR_GEOMETRY_DISTANCE_MIN_EXTENT or over VKR_GEOMETRY_DISTANCE_MAX_EXTENT
 * world metres along their longest side have none. */
#define VKR_GEOMETRY_DISTANCE_MAX_DIM 64u
#define VKR_GEOMETRY_DISTANCE_PAD_VOXELS 2u
#define VKR_GEOMETRY_DISTANCE_MIN_VOXEL 0.25f
#define VKR_GEOMETRY_DISTANCE_BAND_VOXELS 6.0f
#define VKR_GEOMETRY_DISTANCE_MIN_EXTENT 0.25f
#define VKR_GEOMETRY_DISTANCE_MAX_EXTENT 120.0f

/**
 * One submesh's cooked distance volume: `dims` voxels of `box_min_voxel.w`
 * mesh units from `box_min_voxel.xyz`, `voxels` holding one byte per voxel, x
 * fastest then y then z. Zero dims and no voxels when the submesh has none.
 */
typedef struct VkrGeometryDistanceVolume {
  Vec4 box_min_voxel;
  uint32_t dims[3];
  const uint8_t *voxels;
} VkrGeometryDistanceVolume;

/**
 * Borrowed geometry publication input. `submeshes` and every buffer pointer
 * remain valid until the publisher returns. `distance_volumes`, when not
 * NULL, holds one entry per submesh.
 */
typedef struct VkrGeometryUpload {
  VkrGeometryUploadBuffer buffer;
  const VkrGeometryUploadRange *submeshes;
  uint32_t submesh_count;
  const VkrGeometryDistanceVolume *distance_volumes;
} VkrGeometryUpload;

/**
 * Geometry metadata used to create a publisher-owned GPU resource. The source
 * buffers and decode records are borrowed only for the creation call.
 */
typedef struct VkrGeometryConfig {
  uint32_t vertex_size;
  uint32_t vertex_count;
  const void *vertices;
  uint32_t index_size;
  uint32_t index_count;
  const void *indices;
  VkrGpuVertexLayout vertex_layout;
  const VkrGpuGeometryDecodeRecord *decodes;
  uint32_t decode_count;
  /** Indices the geometry's one range draws; zero draws them all. Indices
      past it hold that range's coarser `lod` levels. */
  uint32_t range_index_count;
  /** Levels of the range (ADR-085); a level_count below two means one level.
      Packing appends the row to the decode records. */
  VkrGpuGeometryLodRow lod;
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
  char name[GEOMETRY_NAME_MAX_LENGTH];
  char material_name[MATERIAL_NAME_MAX_LENGTH];
} VkrGeometryConfig;
