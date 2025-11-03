#pragma once

#include "containers/array.h"
#include "containers/vkr_hashtable.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "math/mat.h"
#include "renderer/vkr_renderer.h"

// =============================================================================
// Geometry resource types (decoupled from systems)
// =============================================================================

typedef struct VkrGeometryHandle {
  uint32_t id;
  uint32_t generation;
} VkrGeometryHandle;

#define VKR_GEOMETRY_HANDLE_INVALID                                            \
  (VkrGeometryHandle) { .id = 0, .generation = VKR_INVALID_ID }
typedef enum VkrGeometryVertexLayoutType {
  GEOMETRY_VERTEX_LAYOUT_POSITION_TEXCOORD = 0,
  GEOMETRY_VERTEX_LAYOUT_POSITION_COLOR,
  GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_COLOR,
  GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_TEXCOORD,
  GEOMETRY_VERTEX_LAYOUT_POSITION2_TEXCOORD,
  GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_TEXCOORD2_COLOR_TANGENT,
  GEOMETRY_VERTEX_LAYOUT_FULL,
  GEOMETRY_VERTEX_LAYOUT_COUNT
} VkrGeometryVertexLayoutType;

// Describes a geometry instance stored in the geometry system's array.
// Lifetime is managed separately via VkrGeometryEntry (hash table).
typedef struct VkrGeometry {
  // Stable id (slot index + 1) and generation for handle validation
  uint32_t id;
  uint32_t pipeline_id; // pipeline family id (world/ui etc.)
  uint32_t generation;

  // Layout used by this geometry
  VkrGeometryVertexLayoutType layout;

  // Ranges into pooled buffers (indices into vertices/indices, not bytes)
  uint32_t first_vertex;
  uint32_t vertex_count;
  uint32_t first_index;
  uint32_t index_count;

  // Optional linkage back to lifetime entry (hash key). Stored for convenience
  // when operating via handle-based APIs.
  const char *name;
} VkrGeometry;
Array(VkrGeometry);

// =============================================================================
// Texture resource types (decoupled from systems)
// =============================================================================

#define VKR_TEXTURE_HANDLE_INVALID                                             \
  (VkrTextureHandle) { .id = 0, .generation = VKR_INVALID_ID }

typedef struct VkrTextureHandle {
  uint32_t id;
  uint32_t generation;
} VkrTextureHandle;

#define VKR_TEXTURE_MAX_DIMENSION 16384
#define VKR_TEXTURE_RGBA_CHANNELS 4
#define VKR_TEXTURE_RGB_CHANNELS 3
#define VKR_TEXTURE_RG_CHANNELS 2
#define VKR_TEXTURE_R_CHANNELS 1

typedef enum VkrTextureSlot {
  VKR_TEXTURE_SLOT_DIFFUSE = 0,
  VKR_TEXTURE_SLOT_NORMAL = 1,
  VKR_TEXTURE_SLOT_SPECULAR = 2,
  VKR_TEXTURE_SLOT_EMISSION = 3,
  VKR_TEXTURE_SLOT_COUNT
} VkrTextureSlot;

typedef struct VkrTexture {
  VkrTextureDescription description;
  VkrTextureOpaqueHandle handle;
  FilePath file_path;
  uint8_t *image;
} VkrTexture;
Array(VkrTexture);

// =============================================================================
// Material resource types (decoupled from systems)
// =============================================================================

typedef struct VkrMaterialHandle {
  uint32_t id;
  uint32_t generation;
} VkrMaterialHandle;

#define VKR_MATERIAL_HANDLE_INVALID                                            \
  (VkrMaterialHandle) { .id = 0, .generation = VKR_INVALID_ID }

typedef struct VkrPhongProperties {
  Vec4 diffuse_color;  // Base color factor
  Vec4 specular_color; // Specular reflection color
  float32_t shininess; // Specular exponent
  Vec3 emission_color; // Self-illumination
} VkrPhongProperties;

typedef struct VkrMaterialTexture {
  VkrTextureHandle handle;
  VkrTextureSlot slot;
  bool8_t enabled; // Allow disabling without removing
} VkrMaterialTexture;

typedef struct VkrMaterial {
  uint32_t id;
  uint32_t pipeline_id; // pipeline family id (world/ui etc.)
  uint32_t generation;
  const char *name;

  // Preferred shader name, e.g., "shader.default.world". If NULL, a
  // domain-based default is used.
  const char *shader_name;

  // Phong lighting parameters
  VkrPhongProperties phong;

  // Texture maps
  VkrMaterialTexture textures[VKR_TEXTURE_SLOT_COUNT];
} VkrMaterial;

Array(VkrMaterial);

// =============================================================================
// Pipeline resource types (decoupled from systems)
// =============================================================================

typedef struct VkrPipelineHandle {
  uint32_t id;
  uint32_t generation;
} VkrPipelineHandle;

#define VKR_PIPELINE_HANDLE_INVALID                                            \
  (VkrPipelineHandle) { .id = 0, .generation = VKR_INVALID_ID }

typedef struct VkrPipeline {
  VkrPipelineHandle handle;
  VkrGraphicsPipelineDescription description;

  VkrPipelineDomain domain;
  VkrPipelineOpaqueHandle backend_handle;
} VkrPipeline;
Array(VkrPipeline);

// =============================================================================
// Renderable (geometry + material + model) - app/scene-side draw unit
// =============================================================================

typedef struct VkrRenderable {
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  VkrPipelineHandle pipeline;
  Mat4 model;
  VkrRendererInstanceStateHandle local_state;
} VkrRenderable;
Array(VkrRenderable);

// =============================================================================
// Shader resource types (decoupled from systems)
// =============================================================================

typedef enum VkrShaderAttributeType {
  SHADER_ATTRIBUTE_TYPE_UNDEFINED = 0,
  SHADER_ATTRIBUTE_TYPE_VEC2,
  SHADER_ATTRIBUTE_TYPE_VEC3,
  SHADER_ATTRIBUTE_TYPE_VEC4,
  SHADER_ATTRIBUTE_TYPE_MAT4,
  SHADER_ATTRIBUTE_TYPE_INT32,
  SHADER_ATTRIBUTE_TYPE_UINT32,
} VkrShaderAttributeType;

typedef enum VkrShaderUniformType {
  SHADER_UNIFORM_TYPE_UNDEFINED = 0,
  SHADER_UNIFORM_TYPE_FLOAT32,
  SHADER_UNIFORM_TYPE_FLOAT32_2,
  SHADER_UNIFORM_TYPE_FLOAT32_3,
  SHADER_UNIFORM_TYPE_FLOAT32_4,
  SHADER_UNIFORM_TYPE_INT32,
  SHADER_UNIFORM_TYPE_UINT32,
  SHADER_UNIFORM_TYPE_MATRIX_4,
  SHADER_UNIFORM_TYPE_SAMPLER,
} VkrShaderUniformType;

typedef enum VkrShaderScope {
  VKR_SHADER_SCOPE_GLOBAL = 0,
  VKR_SHADER_SCOPE_INSTANCE = 1,
  VKR_SHADER_SCOPE_LOCAL = 2
} VkrShaderScope;

typedef struct VkrShaderAttributeDesc {
  VkrShaderAttributeType type;
  String8 name;
  uint32_t location; // assigned in declaration order
  uint32_t offset;   // tightly packed in declaration order
  uint32_t size;     // in bytes
} VkrShaderAttributeDesc;
Array(VkrShaderAttributeDesc);

typedef struct VkrShaderUniformDesc {
  VkrShaderUniformType type;
  VkrShaderScope scope; // 0=global,1=instance,2=local
  String8 name;
  uint32_t location; // within-scope index; for samplers = texture slot
  uint32_t offset;   // UBO offset (scopes 0/1); 0 for samplers
  uint32_t size;     // size in bytes (0 for samplers)
} VkrShaderUniformDesc;
Array(VkrShaderUniformDesc);

typedef struct VkrShaderStageFile {
  VkrShaderStage stage; // vertex/fragment
  String8 filename;     // path to SPIR-V file
  String8 entry_point;  // default: vertexMain/fragmentMain
} VkrShaderStageFile;
Array(VkrShaderStageFile);

typedef struct VkrShaderConfig {
  String8 name;                              // shader.unique name
  String8 renderpass_name;                   // renderpass key string
  uint8_t use_instance;                      // enable instance scope (set 1)
  uint8_t use_local;                         // enable push constants
  VkrGeometryVertexLayoutType vertex_layout; // explicit geometry vertex layout

  // Stages
  Array_VkrShaderStageFile stages;
  uint32_t stage_count; // filled stage entries

  // Attributes & uniforms
  Array_VkrShaderAttributeDesc attributes;
  Array_VkrShaderUniformDesc uniforms;
  uint32_t attribute_count; // filled entries
  uint32_t uniform_count;   // filled entries

  VkrHashTable_uint32_t uniform_name_to_index;
  VkrHashTable_uint32_t attribute_name_to_index;

  // Computed layout
  uint64_t attribute_stride;

  uint64_t global_ubo_size;
  uint64_t global_ubo_stride;
  uint32_t global_texture_count;

  uint64_t instance_ubo_size;
  uint64_t instance_ubo_stride;
  uint32_t instance_texture_count;

  uint64_t push_constant_size;
  uint64_t push_constant_stride;
} VkrShaderConfig;

typedef struct VkrShader {
  String8 name;
  uint32_t id;

  const VkrShaderConfig *config;

  // Scope tracking
  VkrShaderScope bound_scope;
  uint32_t bound_instance_id;

  // Instance resource tracking
  uint32_t *instance_ids;
  uint32_t instance_capacity;
  uint32_t instance_used_count;

  // Instance free list for O(1) allocation/deallocation
  uint32_t *instance_free_list;
  uint32_t instance_free_list_count;

  // Warn-once tracking for missing uniforms/samplers per shader
  VkrHashTable_uint8_t missing_uniform_warnings;
} VkrShader;
Array(VkrShader);

#define VKR_SHADER_NAME_MAX_LENGTH 256
#define VKR_SHADER_INVALID_UNIFORM_INDEX 0xFFFFu

#define VKR_SHADER_SYSTEM_CONFIG_DEFAULT                                       \
  {.max_shader_count = 512,                                                    \
   .max_uniform_count = 32,                                                    \
   .max_global_textures = 8,                                                   \
   .max_instance_textures = 8}