#pragma once

#include "vkr_buffer.h"
#include "vkr_gpu_abi.h"

typedef struct VkrGeometryQuantizationBudgets {
  float32_t position_relative;
  float32_t normal_degrees;
  float32_t tangent_degrees;
  float32_t uv_absolute;
  float32_t color_absolute;
} VkrGeometryQuantizationBudgets;

typedef struct VkrGeometryQuantizationMetrics {
  float32_t position_max;
  float32_t normal_degrees_max;
  float32_t tangent_degrees_max;
  float32_t uv_max;
  float32_t color_max;
} VkrGeometryQuantizationMetrics;

VkrGeometryQuantizationBudgets vkr_packed_geometry_default_budgets(void);

/** Validates the fixed static-v1 decode record before GPU publication; a
    standalone record, as cooked files hold, has no LOD row. */
bool8_t
vkr_packed_geometry_decode_is_valid(const VkrGpuGeometryDecodeRecord *decode);

/** Validates a geometry's metadata records: decode records, then the LOD rows
    they reference in the same order, four records each (ADR-084). Returns
    the decode record count in `out_decode_count`. */
bool8_t
vkr_packed_geometry_metadata_is_valid(const VkrGpuGeometryDecodeRecord *records,
                                      uint32_t record_count,
                                      uint32_t *out_decode_count);

/** The LOD row of decode record `decode_index`, or NULL for one level. The
    metadata must have passed vkr_packed_geometry_metadata_is_valid. */
const VkrGpuGeometryLodRow *
vkr_packed_geometry_lod_row(const VkrGpuGeometryDecodeRecord *records,
                            uint32_t decode_index);

/** Whether every level of a range's LOD row (if any) draws indices inside
    the geometry's `index_count` that reach vertices below `vertex_count`,
    and level 0 is the range itself. */
bool8_t vkr_packed_geometry_lod_ranges_are_valid(
    const VkrGpuGeometryDecodeRecord *records, uint32_t decode_index,
    uint32_t first_index, uint32_t range_index_count, int32_t vertex_offset,
    const uint32_t *indices, uint32_t index_count, uint32_t vertex_count);

/** Validates reserved fields and file-controlled float payloads. */
bool8_t vkr_packed_geometry_vertices_are_valid(
    const VkrPackedStaticVertex *vertices, uint32_t vertex_count,
    const VkrGpuGeometryDecodeRecord *decode);

bool8_t vkr_packed_geometry_pack(const VkrVertex3d *source,
                                 uint32_t vertex_count, Vec3 min_extents,
                                 Vec3 max_extents,
                                 const VkrGeometryQuantizationBudgets *budgets,
                                 VkrPackedStaticVertex *destination,
                                 VkrGpuGeometryDecodeRecord *out_decode,
                                 VkrGeometryQuantizationMetrics *out_metrics);

/** Stores a lightmap UV pair in [0,1] per vertex in word 7 as two unorm16
    values and marks the range's decode record; rejects values outside [0,1]. */
bool8_t vkr_packed_geometry_set_lightmap_uv(VkrPackedStaticVertex *vertices,
                                            uint32_t vertex_count,
                                            const float32_t *uv2,
                                            VkrGpuGeometryDecodeRecord *decode);

/** Decodes word 7 of a vertex whose range carries a lightmap UV set. */
Vec2 vkr_packed_geometry_lightmap_uv(const VkrPackedStaticVertex *vertex);

void vkr_packed_geometry_unpack(const VkrPackedStaticVertex *source,
                                uint32_t vertex_count,
                                const VkrGpuGeometryDecodeRecord *decode,
                                VkrVertex3d *destination);
