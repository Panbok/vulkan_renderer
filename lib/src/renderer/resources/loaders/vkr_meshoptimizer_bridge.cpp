#include "renderer/resources/loaders/vkr_meshoptimizer_bridge.h"

#include <meshoptimizer.h>

extern "C" uint32_t vkr_meshopt_library_version(void) {
  return MESHOPTIMIZER_VERSION;
}

extern "C" size_t vkr_meshopt_optimize_range(
    void *destination_vertices, uint32_t *destination_indices,
    const void *source_vertices, const uint32_t *source_indices,
    size_t vertex_count, size_t index_count, size_t vertex_stride) {
  if (!destination_vertices || !destination_indices || !source_vertices ||
      !source_indices || vertex_count == 0 || index_count == 0 ||
      index_count % 3 != 0 || vertex_stride == 0 || vertex_stride > 256 ||
      vertex_stride % 4 != 0) {
    return 0;
  }

  meshopt_optimizeVertexCache(destination_indices, source_indices, index_count,
                              vertex_count);
  return meshopt_optimizeVertexFetch(destination_vertices, destination_indices,
                                     index_count, source_vertices, vertex_count,
                                     vertex_stride);
}

extern "C" int vkr_meshopt_analyze_range(const uint32_t *indices,
                                         size_t index_count,
                                         size_t vertex_count,
                                         size_t vertex_stride,
                                         VkrMeshoptAnalysis *out_analysis) {
  if (!indices || !out_analysis || index_count == 0 || index_count % 3 != 0 ||
      vertex_count == 0 || vertex_stride == 0) {
    return -1;
  }
  const meshopt_VertexCacheStatistics cache = meshopt_analyzeVertexCache(
      indices, index_count, vertex_count, 16u, 32u, 256u);
  const meshopt_VertexFetchStatistics fetch = meshopt_analyzeVertexFetch(
      indices, index_count, vertex_count, vertex_stride);
  *out_analysis = VkrMeshoptAnalysis{
      cache.vertices_transformed,
      fetch.bytes_fetched,
      cache.acmr,
      cache.atvr,
      fetch.overfetch,
  };
  return 0;
}

extern "C" int vkr_meshopt_decode_gltf_buffer(
    void *destination, size_t count, size_t stride, const uint8_t *encoded,
    size_t encoded_size, VkrMeshoptGltfMode mode, VkrMeshoptGltfFilter filter) {
  if (!destination || !encoded || count == 0 || stride == 0) {
    return -1;
  }
  int result = -1;
  switch (mode) {
  case VKR_MESHOPT_GLTF_MODE_ATTRIBUTES:
    result = meshopt_decodeVertexBuffer(destination, count, stride, encoded,
                                        encoded_size);
    break;
  case VKR_MESHOPT_GLTF_MODE_TRIANGLES:
    result = meshopt_decodeIndexBuffer(destination, count, stride, encoded,
                                       encoded_size);
    break;
  case VKR_MESHOPT_GLTF_MODE_INDICES:
    result = meshopt_decodeIndexSequence(destination, count, stride, encoded,
                                         encoded_size);
    break;
  default:
    return -1;
  }
  if (result != 0) {
    return result;
  }
  switch (filter) {
  case VKR_MESHOPT_GLTF_FILTER_NONE:
    break;
  case VKR_MESHOPT_GLTF_FILTER_OCTAHEDRAL:
    meshopt_decodeFilterOct(destination, count, stride);
    break;
  case VKR_MESHOPT_GLTF_FILTER_QUATERNION:
    meshopt_decodeFilterQuat(destination, count, stride);
    break;
  case VKR_MESHOPT_GLTF_FILTER_EXPONENTIAL:
    meshopt_decodeFilterExp(destination, count, stride);
    break;
  case VKR_MESHOPT_GLTF_FILTER_COLOR:
    meshopt_decodeFilterColor(destination, count, stride);
    break;
  default:
    return -1;
  }
  return 0;
}

extern "C" size_t vkr_meshopt_vertex_encode_bound(size_t vertex_count,
                                                  size_t vertex_stride) {
  return meshopt_encodeVertexBufferBound(vertex_count, vertex_stride);
}

extern "C" size_t vkr_meshopt_index_encode_bound(size_t index_count,
                                                 size_t vertex_count) {
  return meshopt_encodeIndexBufferBound(index_count, vertex_count);
}

extern "C" size_t vkr_meshopt_encode_vertices(uint8_t *destination,
                                              size_t destination_size,
                                              const void *vertices,
                                              size_t vertex_count,
                                              size_t vertex_stride) {
  return meshopt_encodeVertexBufferLevel(destination, destination_size,
                                         vertices, vertex_count, vertex_stride,
                                         2, VKR_MESHOPT_VERTEX_CODEC_VERSION);
}

extern "C" size_t vkr_meshopt_encode_indices(uint8_t *destination,
                                             size_t destination_size,
                                             const uint32_t *indices,
                                             size_t index_count,
                                             size_t vertex_count) {
  static const bool index_codec_version_initialized = [] {
    meshopt_encodeIndexVersion(VKR_MESHOPT_INDEX_CODEC_VERSION);
    return true;
  }();
  (void)index_codec_version_initialized;
  return meshopt_encodeIndexBuffer(destination, destination_size, indices,
                                   index_count);
}

extern "C" int vkr_meshopt_decode_vertices(void *destination,
                                           size_t vertex_count,
                                           size_t vertex_stride,
                                           const uint8_t *encoded,
                                           size_t encoded_size) {
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
