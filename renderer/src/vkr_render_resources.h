#pragma once
#include "containers/array.h"
#include "containers/str.h"
#include "math/vec.h"
#include "vkr_renderer.h"

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
#define VKR_TEXTURE_MAX_ARRAY_LAYERS 2048
#define VKR_TEXTURE_MAX_UPLOAD_REGIONS 32768
#define VKR_TEXTURE_RGBA_CHANNELS 4
#define VKR_TEXTURE_RGB_CHANNELS 3
#define VKR_TEXTURE_RG_CHANNELS 2
#define VKR_TEXTURE_R_CHANNELS 1

typedef enum VkrTextureSlot {
  VKR_TEXTURE_SLOT_DIFFUSE = 0,
  VKR_TEXTURE_SLOT_NORMAL = 1,
  VKR_TEXTURE_SLOT_SPECULAR = 2,
  VKR_TEXTURE_SLOT_EMISSION = 3,
  VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS = 4,
  VKR_TEXTURE_SLOT_OCCLUSION = 5,
  VKR_TEXTURE_SLOT_TRANSMISSION = 6,
  VKR_TEXTURE_SLOT_THICKNESS = 7,
  VKR_TEXTURE_SLOT_COUNT
} VkrTextureSlot;

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

typedef enum VkrMaterialType {
  VKR_MATERIAL_TYPE_PHONG = 0,
  VKR_MATERIAL_TYPE_PBR = 1,
} VkrMaterialType;

typedef enum VkrMaterialAlphaMode {
  VKR_MATERIAL_ALPHA_OPAQUE = 0,
  VKR_MATERIAL_ALPHA_CUTOUT = 1,
  VKR_MATERIAL_ALPHA_BLEND = 2,
} VkrMaterialAlphaMode;

typedef struct VkrPbrProperties {
  Vec4 base_color;
  float32_t metallic;
  float32_t roughness;
  float32_t normal_scale;
  float32_t occlusion_strength;
  Vec3 emissive_factor;
  /** Dielectric normal-incidence reflectance; glTF metallic-roughness defaults
   * to 0.04 while prepared specular-glossiness may author lower RGB values. */
  Vec3 dielectric_specular;
  /** KHR_materials_transmission; independent of base-color alpha mode. */
  float32_t transmission_factor;
  float32_t ior;
  float32_t thickness_factor;
  Vec3 attenuation_color;
  /** World-space distance at which attenuation_color is reached; <=0 disables.
   */
  float32_t attenuation_distance;
  /** Authored rejection strength for temporally changing transparent shading.
   * Zero preserves stable accumulation; one rejects prior color completely. */
  float32_t temporal_reactivity;
} VkrPbrProperties;

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

  VkrMaterialType material_type;
  VkrMaterialAlphaMode alpha_mode;
  bool8_t alpha_mode_explicit;
  /** Disable face culling for thin or explicitly two-sided surfaces. */
  bool8_t double_sided;

  // Material parameters. `phong` remains for backwards compatibility.
  VkrPhongProperties phong;
  VkrPbrProperties pbr;
  float32_t alpha_cutoff; // Alpha test threshold for cutout; 0 disables.

  // Texture maps
  VkrMaterialTexture textures[VKR_TEXTURE_SLOT_COUNT];
} VkrMaterial;

Array(VkrMaterial);

typedef struct VkrMeshInstanceHandle {
  uint32_t id;
  uint32_t generation;
} VkrMeshInstanceHandle;

#define VKR_MESH_INSTANCE_HANDLE_INVALID                                       \
  (VkrMeshInstanceHandle) { .id = 0, .generation = VKR_INVALID_ID }
