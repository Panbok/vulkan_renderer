#pragma once

#include "containers/array.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "renderer/renderer.h"

// =============================================================================
// Geometry resource types (decoupled from systems)
// =============================================================================

typedef struct VkrGeometryHandle {
  uint32_t id;
  uint32_t generation;
} VkrGeometryHandle;

typedef enum VkrGeometryVertexLayoutType {
  GEOMETRY_VERTEX_LAYOUT_POSITION_TEXCOORD = 0,
  GEOMETRY_VERTEX_LAYOUT_POSITION_COLOR,
  GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_COLOR,
  GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_TEXCOORD,
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
  TextureDescription description;
  TextureHandle handle;
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

typedef struct VkrPhongProperties {
  Vec4 diffuse_color;  // Base color factor
  Vec4 specular_color; // Specular reflection color
  float32_t shininess; // Specular exponent
  Vec3 emission_color; // Self-illumination
} VkrPhongProperties;

typedef struct VkrMaterialTexture {
  VkrTextureHandle handle;
  VkrTextureSlot slot;
  bool enabled; // Allow disabling without removing
} VkrMaterialTexture;

typedef struct VkrMaterial {
  uint32_t id;
  uint32_t pipeline_id; // pipeline family id (world/ui etc.)
  uint32_t generation;
  const char *name;

  // Phong lighting parameters
  VkrPhongProperties phong;

  // Texture maps
  VkrMaterialTexture textures[VKR_TEXTURE_SLOT_COUNT];
} VkrMaterial;

Array(VkrMaterial);

// =============================================================================
// Renderable (geometry + material + model) - app/scene-side draw unit
// =============================================================================

typedef struct VkrRenderable {
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  Mat4 model;
} VkrRenderable;
Array(VkrRenderable);
