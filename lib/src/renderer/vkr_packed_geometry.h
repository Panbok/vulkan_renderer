#pragma once

#include "renderer/vkr_buffer.h"
#include "renderer/vkr_gpu_abi.h"

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

/** Validates the fixed static-v1 decode record before GPU publication. */
bool8_t
vkr_packed_geometry_decode_is_valid(const VkrGpuGeometryDecodeRecord *decode);

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

void vkr_packed_geometry_unpack(const VkrPackedStaticVertex *source,
                                uint32_t vertex_count,
                                const VkrGpuGeometryDecodeRecord *decode,
                                VkrVertex3d *destination);
