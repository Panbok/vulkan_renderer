#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "defines.h"
#include "math/vec.h"
#include "memory/vkr_allocator.h"

/* The store owns decoded source texels for one offline bake. Material records
   retain only entry indices, which stay valid if the store grows. */
typedef struct VkrBakeTextureStore VkrBakeTextureStore;

typedef enum VkrBakeMaterialAlphaMode {
  VKR_BAKE_MATERIAL_ALPHA_OPAQUE = 0,
  VKR_BAKE_MATERIAL_ALPHA_CUTOUT = 1,
  VKR_BAKE_MATERIAL_ALPHA_BLEND = 2,
} VkrBakeMaterialAlphaMode;

typedef enum VkrBakeMaterialTextureSlot {
  VKR_BAKE_MATERIAL_TEXTURE_BASE_COLOR = 0,
  VKR_BAKE_MATERIAL_TEXTURE_NORMAL = 1,
  VKR_BAKE_MATERIAL_TEXTURE_SPECULAR = 2,
  VKR_BAKE_MATERIAL_TEXTURE_EMISSIVE = 3,
  VKR_BAKE_MATERIAL_TEXTURE_METALLIC_ROUGHNESS = 4,
  VKR_BAKE_MATERIAL_TEXTURE_OCCLUSION = 5,
  VKR_BAKE_MATERIAL_TEXTURE_TRANSMISSION = 6,
  VKR_BAKE_MATERIAL_TEXTURE_THICKNESS = 7,
  VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT = 8,
  VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT_ROUGHNESS = 9,
  VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT_NORMAL = 10,
  VKR_BAKE_MATERIAL_TEXTURE_SHEEN_COLOR = 11,
  VKR_BAKE_MATERIAL_TEXTURE_SHEEN_ROUGHNESS = 12,
  VKR_BAKE_MATERIAL_TEXTURE_ANISOTROPY = 13,
  VKR_BAKE_MATERIAL_TEXTURE_COUNT = 14,
} VkrBakeMaterialTextureSlot;

typedef struct VkrBakeMaterialTextureRef {
  uint32_t texture_index;
  bool8_t present;
  bool8_t srgb;
} VkrBakeMaterialTextureRef;

typedef struct VkrBakeMaterial {
  VkrBakeMaterialAlphaMode alpha_mode;
  bool8_t alpha_mode_explicit;
  bool8_t double_sided;
  float32_t alpha_cutoff;
  Vec4 base_color;
  float32_t metallic;
  float32_t roughness;
  float32_t normal_scale;
  float32_t occlusion_strength;
  Vec3 emissive_factor;
  Vec3 dielectric_specular;
  float32_t transmission_factor;
  float32_t ior;
  float32_t thickness_factor;
  Vec3 attenuation_color;
  float32_t attenuation_distance;
  float32_t clearcoat_factor;
  float32_t clearcoat_roughness;
  float32_t clearcoat_normal_scale;
  Vec3 sheen_color;
  float32_t sheen_roughness;
  float32_t subsurface_strength;
  uint32_t subsurface_profile;
  float32_t diffuse_transmission_strength;
  Vec3 diffuse_transmission_color;
  float32_t anisotropy_strength;
  float32_t anisotropy_rotation;
  VkrBakeMaterialTextureRef textures[VKR_BAKE_MATERIAL_TEXTURE_COUNT];
} VkrBakeMaterial;

typedef struct VkrBakeMaterialSample {
  Vec4 base_color;
  Vec3 emissive;
  Vec3 dielectric_specular;
  Vec3 tangent_normal;
  Vec3 attenuation_color;
  float32_t metallic;
  float32_t roughness;
  float32_t occlusion;
  float32_t transmission;
  float32_t ior;
  float32_t thickness;
  float32_t attenuation_distance;
  VkrBakeMaterialAlphaMode alpha_mode;
  float32_t alpha_cutoff;
  bool8_t double_sided;
  float32_t clearcoat_factor;
  float32_t clearcoat_roughness;
  Vec3 clearcoat_tangent_normal;
  Vec3 sheen_color;
  float32_t sheen_roughness;
  float32_t subsurface_strength;
  uint32_t subsurface_profile;
  float32_t diffuse_transmission_strength;
  Vec3 diffuse_transmission_color;
  float32_t anisotropy_strength;
  /* Unit direction in the base tangent frame; the integrator projects it
     against the mapped normal before BSDF creation. */
  Vec2 anisotropy_direction;
} VkrBakeMaterialSample;

typedef enum VkrBakeMaterialError {
  VKR_BAKE_MATERIAL_ERROR_NONE = 0,
  VKR_BAKE_MATERIAL_ERROR_INVALID_ARGUMENT,
  VKR_BAKE_MATERIAL_ERROR_IO,
  VKR_BAKE_MATERIAL_ERROR_PARSE,
  VKR_BAKE_MATERIAL_ERROR_TEXTURE,
  VKR_BAKE_MATERIAL_ERROR_UNSUPPORTED,
  VKR_BAKE_MATERIAL_ERROR_OUT_OF_MEMORY,
} VkrBakeMaterialError;

/* `allocator` owns entries, canonical paths, SHA-256 strings, and texels until
   vkr_bake_texture_store_release. It must outlive every material record. */
VkrBakeTextureStore *vkr_bake_texture_store_create(VkrAllocator *allocator);
void vkr_bake_texture_store_release(VkrBakeTextureStore *store);

/* Dependency records are borrowed until the next store mutation or release. */
uint32_t vkr_bake_texture_store_count(const VkrBakeTextureStore *store);
bool8_t vkr_bake_texture_store_dependency(const VkrBakeTextureStore *store,
                                          uint32_t texture_index,
                                          const char **out_path,
                                          const char **out_sha256,
                                          uint64_t *out_byte_count);

/* Environment paths are loaded from their authored source without selecting a
   material sidecar. Source texels and dependency records remain store-owned. */
bool8_t vkr_bake_texture_store_load_environment_2d(
    VkrBakeTextureStore *store, const char *path, bool8_t srgb,
    uint32_t *out_texture_index, VkrBakeMaterialError *out_error);
bool8_t vkr_bake_texture_store_load_environment_equirect(
    VkrBakeTextureStore *store, const char *path, uint32_t *out_texture_index,
    VkrBakeMaterialError *out_error);
bool8_t vkr_bake_texture_store_load_environment_cube_rgba16f(
    VkrBakeTextureStore *store, const char *path, uint32_t *out_texture_index,
    VkrBakeMaterialError *out_error);

/* Environment sampling is mip-zero linear filtering. `repeat_u` implements
   equirectangular longitude wrapping; cube faces use clamp-to-edge. */
Vec4 vkr_bake_texture_store_sample_environment_2d(
    const VkrBakeTextureStore *store, uint32_t texture_index, Vec2 uv,
    bool8_t repeat_u);
Vec4 vkr_bake_texture_store_sample_environment_cube_face(
    const VkrBakeTextureStore *store, uint32_t texture_index, uint32_t face,
    Vec2 uv);

/* Material and texture file paths are UTF-8, null-terminated and borrowed only
   for this call. The loader resolves source and sidecar `.vkt` assets exactly
   once per canonical selected file. */
bool8_t vkr_bake_material_load(VkrBakeTextureStore *store,
                               const char *material_path,
                               VkrBakeMaterial *out_material,
                               VkrBakeMaterialError *out_error);

/* `uv` and vertex color came from validated cooked geometry. Sampling repeats
   UVs and uses mip-zero bilinear filtering; it allocates nothing. */
void vkr_bake_material_sample(const VkrBakeTextureStore *store,
                              const VkrBakeMaterial *material, Vec2 uv,
                              Vec4 vertex_color,
                              VkrBakeMaterialSample *out_sample);

#ifdef __cplusplus
}
#endif
