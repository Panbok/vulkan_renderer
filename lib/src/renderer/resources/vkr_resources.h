#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "containers/vkr_hashtable.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "math/mat.h"
#include "math/vkr_transform.h"
#include "renderer/vkr_buffer.h"
#include "renderer/vkr_renderer.h"

// =============================================================================
// Scene resource handles (runtime scene instances owned by resource system)
// =============================================================================

typedef struct VkrSceneRuntime VkrSceneRuntime;
typedef VkrSceneRuntime *VkrSceneHandle;

#define VKR_SCENE_HANDLE_INVALID ((VkrSceneHandle)0)

// =============================================================================
// Geometry resource types (decoupled from systems)
// =============================================================================

typedef struct VkrGeometryHandle {
  uint32_t id;
  uint32_t generation;
} VkrGeometryHandle;

#define VKR_GEOMETRY_HANDLE_INVALID                                            \
  (VkrGeometryHandle) { .id = 0, .generation = VKR_INVALID_ID }

#define GEOMETRY_NAME_MAX_LENGTH 64
#define MATERIAL_NAME_MAX_LENGTH 64

typedef enum VkrVertexType {
  VKR_VERTEX_TYPE_UNKNOWN = 0,
  VKR_VERTEX_TYPE_3D,
  VKR_VERTEX_TYPE_2D,
} VkrVertexType;

typedef struct VkrGeometry {
  uint32_t id;
  uint32_t pipeline_id;
  uint32_t generation;

  uint32_t vertex_size;
  uint32_t vertex_count;
  uint32_t index_size;
  uint32_t index_count;

  VkrVertexBuffer vertex_buffer;
  VkrIndexBuffer index_buffer;
  VkrIndexBuffer opaque_index_buffer; // Optional compacted opaque-only indices.
  uint32_t opaque_index_count; // Index count for opaque_index_buffer.

  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;

  char name[GEOMETRY_NAME_MAX_LENGTH];
  char material_name[MATERIAL_NAME_MAX_LENGTH];
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
Array(VkrTextureHandle);

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

// Default cutoff for authoring-driven cutout materials without an explicit
// alpha_cutoff value.
#define VKR_MATERIAL_ALPHA_CUTOFF_DEFAULT 0.1f

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
  float32_t alpha_cutoff; // Alpha test threshold for cutout; 0 disables.

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
  VkrRenderPassHandle renderpass;
  VkrPipelineOpaqueHandle backend_handle;
} VkrPipeline;
Array(VkrPipeline);
// =============================================================================
// Mesh/SubMesh - app/scene-side draw units
// =============================================================================

typedef enum VkrMeshLoadingState {
  VKR_MESH_LOADING_STATE_NOT_LOADED = 0,
  VKR_MESH_LOADING_STATE_LOADED = 1,
} VkrMeshLoadingState;

typedef struct VkrSubMesh {
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  VkrPipelineHandle pipeline;
  VkrRendererInstanceStateHandle instance_state;
  VkrPipelineDomain pipeline_domain;
  String8 shader_override;
  /** Stable identifier for a sub-range inside shared geometry buffers. */
  uint32_t range_id;
  /** Index buffer range; index_count==0 implies full-geometry draw. */
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  /** Optional opaque-only range in a compacted index buffer. */
  uint32_t opaque_first_index;
  uint32_t opaque_index_count;
  int32_t opaque_vertex_offset;
  /** Local-space bounds for the draw range (center + extents). */
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
  bool8_t pipeline_dirty;
  bool8_t owns_geometry;
  bool8_t owns_material;
  uint64_t last_render_frame;
} VkrSubMesh;
Array(VkrSubMesh);

typedef struct VkrMesh {
  VkrTransform transform;
  Mat4 model;
  Array_VkrSubMesh submeshes;
  VkrMeshLoadingState loading_state;
  uint32_t render_id;
  bool8_t visible;

  // Bounding sphere for frustum culling
  bool8_t bounds_valid;
  Vec3 bounds_local_center;   // Local-space bounding sphere center
  float32_t bounds_local_radius;
  Vec3 bounds_world_center;   // Cached world-space center (updated with model)
  float32_t bounds_world_radius;
} VkrMesh;
Array(VkrMesh);

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
  uint32_t location;    // within-scope index; for samplers = texture slot
  uint32_t offset;      // UBO offset (scopes 0/1); 0 for samplers
  uint32_t size;        // total size in bytes (element_size * array_count)
  uint32_t array_count; // 1 for scalars, >1 for arrays
} VkrShaderUniformDesc;
Array(VkrShaderUniformDesc);

typedef struct VkrShaderStageFile {
  VkrShaderStage stage; // vertex/fragment
  String8 filename;     // path to SPIR-V file
  String8 entry_point;  // default: vertexMain/fragmentMain
} VkrShaderStageFile;
Array(VkrShaderStageFile);

typedef struct VkrShaderConfig {
  String8 name;              // shader.unique name
  String8 renderpass_name;   // renderpass key string
  uint8_t use_instance;      // enable instance scope (set 1)
  uint8_t use_local;         // enable push constants
  VkrCullMode cull_mode;     // culling mode (default: VKR_CULL_MODE_BACK)
  VkrVertexType vertex_type; // inferred vertex type

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

// =============================================================================
// Font resource types (decoupled from systems)
// =============================================================================

/**
 * @brief A font handle.
 * @param id The font id.
 * @param generation The font generation.
 */
typedef struct VkrFontHandle {
  uint32_t id;         // The font id.
  uint32_t generation; // The font generation.
} VkrFontHandle;

#define VKR_FONT_HANDLE_INVALID                                                \
  (VkrFontHandle) { .id = 0, .generation = VKR_INVALID_ID }

/**
 * @brief A font glyph.
 * @param codepoint The codepoint of the glyph.
 * @param x The x position of the glyph.
 * @param y The y position of the glyph.
 * @param width The width of the glyph.
 * @param height The height of the glyph.
 * @param x_offset The x offset of the glyph.
 * @param y_offset The y offset of the glyph.
 * @param x_advance The x advance of the glyph.
 * @param page_id The page id of the glyph.
 */
typedef struct VkrFontGlyph {
  uint32_t codepoint; // The codepoint of the glyph.
  uint16_t x;         // The x position of the glyph.
  uint16_t y;         // The y position of the glyph.
  uint16_t width;     // The width of the glyph.
  uint16_t height;    // The height of the glyph.
  int16_t x_offset;   // The x offset of the glyph.
  int16_t y_offset;   // The y offset of the glyph.
  int16_t x_advance;  // The x advance of the glyph.
  uint8_t page_id;    // The page id of the glyph.
} VkrFontGlyph;
Array(VkrFontGlyph);
Vector(VkrFontGlyph);

/**
 * @brief A font kerning.
 * @param codepoint_0 The first codepoint.
 * @param codepoint_1 The second codepoint.
 * @param amount The kerning amount.
 */
typedef struct VkrFontKerning {
  uint32_t codepoint_0; // The first codepoint.
  uint32_t codepoint_1; // The second codepoint.
  int16_t amount;       // The kerning amount.
} VkrFontKerning;
Array(VkrFontKerning);
Vector(VkrFontKerning);

/**
 * @brief MTSDF glyph data (normalized coordinates).
 */
typedef struct VkrMtsdfGlyph {
  uint32_t unicode;
  float32_t advance; // Normalized advance

  // Plane bounds (normalized quad in EM space)
  float32_t plane_left;
  float32_t plane_bottom;
  float32_t plane_right;
  float32_t plane_top;

  // Atlas bounds (pixel coordinates in atlas)
  float32_t atlas_left;
  float32_t atlas_bottom;
  float32_t atlas_right;
  float32_t atlas_top;

  bool8_t has_geometry; // false for space-like glyphs
} VkrMtsdfGlyph;
Array(VkrMtsdfGlyph);
Vector(VkrMtsdfGlyph);

/**
 * @brief A font type.
 * @param VKR_FONT_TYPE_BITMAP The bitmap font type.
 * @param VKR_FONT_TYPE_SYSTEM The system font type.
 * @param VKR_FONT_TYPE_MTSDF The MTSDF (multi-channel signed distance field)
 * font type.
 */
typedef enum VkrFontType {
  VKR_FONT_TYPE_BITMAP, // The bitmap font type.
  VKR_FONT_TYPE_SYSTEM, // The system font type.
  VKR_FONT_TYPE_MTSDF   // The MTSDF font type.
} VkrFontType;

/**
 * @brief A font.
 * @param id The font id.
 * @param generation The font generation.
 * @param type The font type.
 * @param face The font face.
 * @param size The font size.
 * @param page_count The number of texture pages.
 * @param atlas The primary atlas texture handle (page 0).
 * @param atlas_pages The per-page atlas handles (indexed by page id).
 */
typedef struct VkrFont {
  uint32_t id;          // The font id.
  uint32_t generation;  // The font generation.
  VkrFontType type;     // The font type.
  char face[256];       // The font face.
  uint32_t size;        // The font size.
  int32_t line_height;  // The line height.
  int32_t baseline;     // The baseline.
  int32_t ascent;       // Distance from baseline to top of tallest glyph
  int32_t descent;      // Distance from baseline to bottom (typically negative)
  int32_t atlas_size_x; // The atlas size x.
  int32_t atlas_size_y; // The atlas size y.
  uint32_t page_count;  // Number of texture pages.
  VkrTextureHandle atlas;              // Page 0 atlas handle.
  Array_VkrTextureHandle atlas_pages;  // Page handles, indexed by page id.
  uint8_t *atlas_cpu_data;             // Optional CPU copy of atlas pixels.
  uint64_t atlas_cpu_size;             // Size of atlas_cpu_data in bytes.
  uint32_t atlas_cpu_channels;         // Channel count for atlas_cpu_data.
  VkrHashTable_uint32_t glyph_indices; // Codepoint -> glyph index lookup.
  Array_VkrFontGlyph glyphs;           // The font glyphs.
  Array_VkrFontKerning kernings;       // The font kernings.
  float32_t tab_x_advance;             // The tab x advance.
  Array_VkrMtsdfGlyph mtsdf_glyphs;    // MTSDF glyph metadata (if any).
  float32_t sdf_distance_range;        // MTSDF distance range for shader.
  float32_t em_size;                   // MTSDF EM size used for atlas.
} VkrFont;
Array(VkrFont);

/**
 * @brief A bitmap font page.
 * @param id The page id.
 * @param file The page file.
 */
typedef struct VkrBitmapFontPage {
  uint8_t id;     // The page id.
  char file[256]; // The page file.
} VkrBitmapFontPage;
Array(VkrBitmapFontPage);
Vector(VkrBitmapFontPage);

/**
 * @brief A bitmap font resource data.
 * @param font_id The font id.
 * @param pages The pages.
 */
typedef struct VkrBitmapFontResourceData {
  uint32_t font_id;              // The font id.
  Array_VkrBitmapFontPage pages; // The pages.
} VkrBitmapFontResourceData;
Array(VkrBitmapFontResourceData);

typedef struct VkrSystemFontResourceData {
  uint32_t font_id; // The font id.
  char file[256];   // The page file.
} VkrSystemFontResourceData;
Array(VkrSystemFontResourceData);
