#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKR_MESHOPT_VERTEX_CODEC_VERSION 1u
#define VKR_MESHOPT_INDEX_CODEC_VERSION 1u

typedef struct VkrMeshoptAnalysis {
  uint32_t vertices_transformed;
  uint32_t bytes_fetched;
  float cache_acmr;
  float cache_atvr;
  float fetch_overfetch;
} VkrMeshoptAnalysis;

uint32_t vkr_meshopt_library_version(void);

int vkr_meshopt_decode_vertices(void *destination, size_t vertex_count,
                                size_t vertex_stride, const uint8_t *encoded,
                                size_t encoded_size);
int vkr_meshopt_decode_indices(uint32_t *destination, size_t index_count,
                               const uint8_t *encoded,
                               size_t encoded_size);
int vkr_meshopt_vertex_codec_version(const uint8_t *encoded,
                                     size_t encoded_size);
int vkr_meshopt_index_codec_version(const uint8_t *encoded,
                                    size_t encoded_size);
int vkr_meshopt_analyze_range(const uint32_t *indices, size_t index_count,
                              size_t vertex_count, size_t vertex_stride,
                              VkrMeshoptAnalysis *out_analysis);

#ifdef __cplusplus
}
#endif
