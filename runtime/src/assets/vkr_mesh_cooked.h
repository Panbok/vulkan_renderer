#pragma once

#include "assets/vkr_mesh_source.h"
#include "containers/str.h"
#include "memory/vkr_allocator.h"
#include "vkr_geometry_upload.h"
#include "vkr_packed_geometry.h"

#define VKR_MESH_COOKED_MAGIC 0x564B4D48u /* 'VKMH' */
#define VKR_MESH_COOKED_VERSION 17u
#define VKR_MESH_COOKED_ENDIAN_TAG 0x01020304u
#define VKR_MESH_COOKED_LAYOUT_STATIC_PACKED_V1 2u
#define VKR_MESH_COOKED_STREAM_ALIGNMENT 16u

typedef struct VkrMeshCookedDecoded {
  uint64_t source_bytes;
  uint64_t cooked_bytes;
  uint64_t decoded_bytes;
  VkrGeometryUploadBuffer mesh_buffer;
  VkrMeshSource source;
  Array_VkrGeometryUploadRange ranges;
  VkrGeometryQuantizationMetrics quantization;
} VkrMeshCookedDecoded;

/**
 * Validates the complete self-contained artifact and decodes all ranges into
 * result-owned current-ABI buffers. Dependency hashes remain build provenance;
 * runtime decode performs no authoring-source I/O.
 */
bool8_t vkr_mesh_cooked_decode(VkrAllocator *result_allocator,
                               VkrAllocator *scratch_allocator,
                               const uint8_t *data, uint64_t size,
                               VkrMeshCookedDecoded *out_decoded);
