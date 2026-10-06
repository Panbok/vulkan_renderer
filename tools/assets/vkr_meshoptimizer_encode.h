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
/* Simplifies a triangle list toward `target_index_count` indices with open
 * borders locked and small disconnected parts prunable, weighing
 * `attribute_count` floats at `attribute_offset` in each vertex, and orders
 * the result for the vertex cache. `destination` holds `index_count`
 * indices. Writes the result's error in the positions' units, attribute and
 * pruning terms included, and returns its index count. */
size_t vkr_meshopt_simplify_level(uint32_t *destination,
                                  const uint32_t *indices, size_t index_count,
                                  const void *vertices, size_t vertex_count,
                                  size_t vertex_stride, size_t attribute_offset,
                                  const float *attribute_weights,
                                  size_t attribute_count,
                                  size_t target_index_count, float *out_error);

#ifdef __cplusplus
}
#endif
