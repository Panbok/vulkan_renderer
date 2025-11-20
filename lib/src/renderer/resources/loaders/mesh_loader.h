#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "math/vkr_transform.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_resource_system.h"

typedef struct VkrMeshLoaderSubset {
  VkrGeometryConfig geometry_config;
  String8 material_name;
  String8 shader_override;
  VkrPipelineDomain pipeline_domain;
  VkrMaterialHandle material_handle;
} VkrMeshLoaderSubset;
Array(VkrMeshLoaderSubset);

typedef struct VkrMeshLoaderResult {
  Arena *arena;
  String8 source_path;
  VkrTransform root_transform;
  Array_VkrMeshLoaderSubset subsets;
} VkrMeshLoaderResult;

typedef struct VkrMeshLoaderContext {
  Arena *arena;
  Arena *scratch_arena;
  VkrAllocator allocator;
  VkrGeometrySystem *geometry_system;
  VkrMaterialSystem *material_system;
  VkrMeshManager *mesh_manager;
} VkrMeshLoaderContext;

VkrResourceLoader vkr_mesh_loader_create(VkrMeshLoaderContext *context);
