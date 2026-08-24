#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKR_MESHOPT_VERTEX_CODEC_VERSION 1u
#define VKR_MESHOPT_INDEX_CODEC_VERSION 1u

uint32_t vkr_meshopt_library_version(void);

size_t vkr_meshopt_optimize_range(void *destination_vertices,
                                  uint32_t *destination_indices,
                                  const void *source_vertices,
                                  const uint32_t *source_indices,
                                  size_t vertex_count, size_t index_count,
                                  size_t vertex_stride);

size_t vkr_meshopt_vertex_encode_bound(size_t vertex_count,
                                       size_t vertex_stride);
size_t vkr_meshopt_index_encode_bound(size_t index_count, size_t vertex_count);

size_t vkr_meshopt_encode_vertices(uint8_t *destination,
                                   size_t destination_size,
                                   const void *vertices, size_t vertex_count,
                                   size_t vertex_stride);
size_t vkr_meshopt_encode_indices(uint8_t *destination, size_t destination_size,
                                  const uint32_t *indices, size_t index_count,
                                  size_t vertex_count);

int vkr_meshopt_decode_vertices(void *destination, size_t vertex_count,
                                size_t vertex_stride, const uint8_t *encoded,
                                size_t encoded_size);
int vkr_meshopt_decode_indices(uint32_t *destination, size_t index_count,
                               const uint8_t *encoded, size_t encoded_size);

int vkr_meshopt_vertex_codec_version(const uint8_t *encoded,
                                     size_t encoded_size);
int vkr_meshopt_index_codec_version(const uint8_t *encoded,
                                    size_t encoded_size);

#ifdef __cplusplus
}
#endif
