#pragma once

#include "assets/vkr_mesh_skin.h"
#include "assets/vkr_mesh_source.h"
#include "containers/str.h"
#include "memory/vkr_allocator.h"
#include "vkr_geometry_upload.h"
#include "vkr_packed_geometry.h"

#define VKR_MESH_COOKED_MAGIC 0x564B4D48u /* 'VKMH' */
#define VKR_MESH_COOKED_VERSION 17u
#define VKR_MESH_COOKED_SKIN_VERSION 18u
#define VKR_MESH_COOKED_ENDIAN_TAG 0x01020304u
#define VKR_MESH_COOKED_LAYOUT_STATIC_PACKED_V1 2u
#define VKR_MESH_COOKED_STREAM_ALIGNMENT 16u

/* Fixed layout and limits of the cooked mesh artifact, shared by the encoder
 * and the decoder. */
#define VKR_MESH_COOKED_HEADER_SIZE 272u
#define VKR_MESH_COOKED_DEPENDENCY_SIZE 64u
#define VKR_MESH_COOKED_RANGE_SIZE 200u
#define VKR_MESH_COOKED_MAX_RANGES 1048576u
#define VKR_MESH_COOKED_MAX_DEPENDENCIES 65536u
#define VKR_MESH_COOKED_MAX_STRING_LENGTH 65535u
#define VKR_MESH_COOKED_MAX_FILE_SIZE GB(8)
#define VKR_MESH_COOKED_HEADER_CRC_OFFSET 184u
#define VKR_MESH_COOKED_METADATA_CRC_OFFSET 188u

/* Header flag: the static mesh carries a lightmap UV set (ADR-087). Every
 * range's decode record then carries VKR_GPU_GEOMETRY_DECODE_LIGHTMAP_UV, and
 * a VKR_MESH_COOKED_LIGHTMAP_BLOCK_SIZE block follows the source metadata. */
#define VKR_MESH_COOKED_FLAG_LIGHTMAP_UV 1u
#define VKR_MESH_COOKED_LIGHTMAP_BLOCK_SIZE 16u
#define VKR_MESH_COOKED_MAX_LIGHTMAP_SIZE 8192u

/* Lightmap chart atlas of a cooked mesh: UV2 spans width x height texels at
 * texels_per_unit, with padding texels between charts. Zero width means the
 * mesh has no lightmap UV set. */
typedef struct VkrMeshCookedLightmap {
  uint32_t width;
  uint32_t height;
  float32_t texels_per_unit;
  uint32_t padding;
} VkrMeshCookedLightmap;

typedef struct VkrMeshCookedDecoded {
  uint64_t source_bytes;
  uint64_t cooked_bytes;
  uint64_t decoded_bytes;
  VkrGeometryUploadBuffer mesh_buffer;
  VkrMeshSource source;
  VkrMeshSkinData skin;
  VkrMeshCookedLightmap lightmap;
  Array_VkrGeometryUploadRange ranges;
  VkrGeometryQuantizationMetrics quantization;
} VkrMeshCookedDecoded;

/** Hashes the codec settings that the cooker and the runtime must share. */
void vkr_mesh_cooked_hash_settings(
    const VkrGeometryQuantizationBudgets *budgets, uint8_t out_hash[32]);

/**
 * Validates source node and mesh metadata against `range_count` ranges. The
 * cooker checks what it writes; the decoder checks what it reads.
 */
bool8_t vkr_mesh_cooked_source_validate(VkrAllocator *scratch_allocator,
                                        const VkrMeshSource *source,
                                        uint32_t range_count);

/**
 * Validates the complete self-contained artifact and decodes all ranges into
 * result-owned current-ABI buffers. Dependency hashes remain build provenance;
 * runtime decode performs no authoring-source I/O.
 */
bool8_t vkr_mesh_cooked_decode(VkrAllocator *result_allocator,
                               VkrAllocator *scratch_allocator,
                               const uint8_t *data, uint64_t size,
                               VkrMeshCookedDecoded *out_decoded);

/**
 * Reads a validated artifact's identity without decoding its ranges: the
 * source fingerprint the cooker stored and the codec settings hash. Both are
 * host-neutral, unlike the artifact bytes.
 */
bool8_t vkr_mesh_cooked_read_identity(const uint8_t *data, uint64_t size,
                                      uint64_t *out_source_fingerprint,
                                      uint8_t out_settings_hash[32]);

/** Apply an optional adjacent .remap.json to decoded material references.
 * Missing sidecar preserves the original references; an existing sidecar must
 * map every nonempty reference exactly once. Mapped views borrow scratch until
 * its release; consumers must copy them before returning to their caller.
 * No geometry/source identity or cooked bytes change. */
bool8_t vkr_mesh_cooked_apply_material_remap(VkrAllocator *scratch,
                                             String8 source_path,
                                             VkrMeshCookedDecoded *decoded);
