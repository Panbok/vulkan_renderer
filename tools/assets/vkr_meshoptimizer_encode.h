#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
