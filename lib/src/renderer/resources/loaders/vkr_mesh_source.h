#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "math/mat.h"

/* Source metadata shares the loader result arena and is copied into VkrScene
   at instantiation. Indices refer to the original glTF arrays; UINT32_MAX is
   absent. Only nodes reachable from the selected scene are instantiated. */
typedef struct VkrMeshSourceLight {
  Vec3 color;
  float32_t intensity;
  float32_t range;
  float32_t inner_cone;
  float32_t outer_cone;
  uint32_t kind; // 0 absent, 1 directional, 2 point, 3 spot.
} VkrMeshSourceLight;

typedef struct VkrMeshSourceNode {
  String8 name;
  VkrMeshSourceLight punctual;
  Mat4 local;
  uint32_t parent;
  uint32_t mesh;
  uint32_t mesh_variant;
  uint32_t camera;
  uint32_t skin;
  uint32_t light;
  bool8_t in_scene;
} VkrMeshSourceNode;
Array(VkrMeshSourceNode);

typedef struct VkrMeshSourceMesh {
  uint32_t source_mesh_index;
  uint32_t first_range;
  uint32_t range_count;
} VkrMeshSourceMesh;
Array(VkrMeshSourceMesh);

typedef struct VkrMeshSource {
  Array_VkrMeshSourceNode nodes;
  Array_VkrMeshSourceMesh meshes;
  uint64_t fingerprint;
  uint32_t animation_count;
} VkrMeshSource;
