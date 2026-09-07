#include "assets/vkr_mesh_decode.h"

#include <meshoptimizer.h>

extern "C" uint32_t vkr_meshopt_library_version(void) {
  return MESHOPTIMIZER_VERSION;
}

extern "C" int vkr_meshopt_decode_vertices(
    void *destination, size_t vertex_count, size_t vertex_stride,
    const uint8_t *encoded, size_t encoded_size) {
  return meshopt_decodeVertexBuffer(destination, vertex_count, vertex_stride,
                                    encoded, encoded_size);
}

extern "C" int vkr_meshopt_decode_indices(uint32_t *destination,
                                            size_t index_count,
                                            const uint8_t *encoded,
                                            size_t encoded_size) {
  return meshopt_decodeIndexBuffer(destination, index_count, sizeof(uint32_t),
                                   encoded, encoded_size);
}

extern "C" int vkr_meshopt_vertex_codec_version(const uint8_t *encoded,
                                                  size_t encoded_size) {
  return meshopt_decodeVertexVersion(encoded, encoded_size);
}

extern "C" int vkr_meshopt_index_codec_version(const uint8_t *encoded,
                                                 size_t encoded_size) {
  return meshopt_decodeIndexVersion(encoded, encoded_size);
}

extern "C" int vkr_meshopt_analyze_range(
    const uint32_t *indices, size_t index_count, size_t vertex_count,
    size_t vertex_stride, VkrMeshoptAnalysis *out_analysis) {
  if (!indices || !out_analysis || index_count == 0u || index_count % 3u != 0u ||
      vertex_count == 0u || vertex_stride == 0u) {
    return -1;
  }
  const meshopt_VertexCacheStatistics cache = meshopt_analyzeVertexCache(
      indices, index_count, vertex_count, 16u, 32u, 256u);
  const meshopt_VertexFetchStatistics fetch = meshopt_analyzeVertexFetch(
      indices, index_count, vertex_count, vertex_stride);
  *out_analysis = VkrMeshoptAnalysis{cache.vertices_transformed,
                                     fetch.bytes_fetched, cache.acmr,
                                     cache.atvr, fetch.overfetch};
  return 0;
}
