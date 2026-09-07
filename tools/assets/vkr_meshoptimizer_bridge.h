#pragma once

#include <stddef.h>
#include <stdint.h>

#include "assets/vkr_mesh_decode.h"
#include "assets/vkr_meshoptimizer_encode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VkrMeshoptGltfMode {
  VKR_MESHOPT_GLTF_MODE_ATTRIBUTES = 1,
  VKR_MESHOPT_GLTF_MODE_TRIANGLES = 2,
  VKR_MESHOPT_GLTF_MODE_INDICES = 3,
} VkrMeshoptGltfMode;

typedef enum VkrMeshoptGltfFilter {
  VKR_MESHOPT_GLTF_FILTER_NONE = 0,
  VKR_MESHOPT_GLTF_FILTER_OCTAHEDRAL = 1,
  VKR_MESHOPT_GLTF_FILTER_QUATERNION = 2,
  VKR_MESHOPT_GLTF_FILTER_EXPONENTIAL = 3,
  VKR_MESHOPT_GLTF_FILTER_COLOR = 4,
} VkrMeshoptGltfFilter;

int vkr_meshopt_decode_gltf_buffer(void *destination, size_t count,
                                   size_t stride, const uint8_t *encoded,
                                   size_t encoded_size, VkrMeshoptGltfMode mode,
                                   VkrMeshoptGltfFilter filter);

#ifdef __cplusplus
}
#endif
