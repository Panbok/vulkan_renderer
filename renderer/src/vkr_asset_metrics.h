#pragma once
#include "defines.h"

typedef struct VkrMeshManagerMetrics {
  uint64_t source_bytes;
  uint64_t cooked_bytes;
  uint64_t decoded_bytes;
  uint64_t upload_bytes;
  uint64_t analyzed_triangles;
  uint64_t analyzed_vertex_bytes_before;
  uint64_t analyzed_vertex_bytes_after;
  uint64_t analyzed_vertices;
  uint64_t vertices_transformed_before;
  uint64_t vertices_transformed_after;
  uint64_t bytes_fetched_before;
  uint64_t bytes_fetched_after;
  uint64_t vertex_count;
  uint64_t index_count;
  uint64_t range_count;
  uint32_t live_assets;
  uint32_t source_assets;
  uint32_t cooked_assets;
  uint32_t runtime_optimized_assets;
} VkrMeshManagerMetrics;
