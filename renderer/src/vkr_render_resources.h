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

/** Most lightmap layers one frame samples: the two sun keys nearest the sun
 * and every lamp group (ADR-088). */
#define VKR_LIGHTMAP_MAX_ACTIVE_LAYERS 6u

/** A lightmapped instance's rectangle in texels on one page; the instance's
 * lightmap UVs span it (ADR-088). */
typedef struct VkrLightmapRect {
  uint32_t page;
  uint16_t x;
  uint16_t y;
  uint16_t width;
  uint16_t height;
  uint32_t reserved;
} VkrLightmapRect;

/** Borrowed scene-owned lightmap set. An invalid texture disables sampling.
 * The texture is a 2D array of square ASTC 4x4 HDR irradiance pages whose
 * slice page * layer_count + layer holds one layer of one page. An instance's
 * nonzero lightmap slot selects rects[slot - 1]; a frame sums the active
 * layers, each scaled by its weight. */
typedef struct VkrLightmapBinding {
  VkrTextureHandle texture;
  /** The desktop pipeline's dominant-direction pages in the slices of
      `texture` (RGB: 0.5 * d + 0.5, A: directionality), or invalid. */
  VkrTextureHandle direction;
  uint32_t page_size;
  uint32_t layer_count;
  const VkrLightmapRect *rects;
  uint32_t rect_count;
  uint32_t active_layer_count;
  uint32_t active_layers[VKR_LIGHTMAP_MAX_ACTIVE_LAYERS];
  float32_t active_weights[VKR_LIGHTMAP_MAX_ACTIVE_LAYERS];
  /** The tiled pipeline's lamp-direct pages (ADR-108): a 2D array whose slice
      page * direct_layer_count + d holds lamp-direct plane d, one rectangle
      per instance in `direct_rects`, and per active layer its plane or
      VKR_LIGHTMAP_NO_DIRECT. Invalid and empty without them. */
  VkrTextureHandle lamp_direct;
  const VkrLightmapRect *direct_rects;
  uint32_t direct_layer_count;
  uint32_t active_direct[VKR_LIGHTMAP_MAX_ACTIVE_LAYERS];
} VkrLightmapBinding;

/** An active lightmap layer without a lamp-direct plane. */
#define VKR_LIGHTMAP_NO_DIRECT 0xffffffffu

/** Most light layers one frame composes into a diffuse volume, as for
 * lightmaps: the two sun keys nearest the sun and every lamp group. */
#define VKR_DIFFUSE_VOLUME_MAX_ACTIVE_LAYERS VKR_LIGHTMAP_MAX_ACTIVE_LAYERS
/* Direct bands a volume holds at most, one per lamp group
   (VKR_LIGHT_LAYER_MAX_LAMP_GROUPS). */
#define VKR_DIFFUSE_VOLUME_MAX_DIRECT_BANDS 4u
/* An active layer without a direct band. */
#define VKR_DIFFUSE_VOLUME_NO_BAND 0xffffffffu
/** Probes per row of a diffuse volume's probe and SH textures. */
#define VKR_DIFFUSE_VOLUME_ROW_PROBES 1024u
/** Probes per row of a diffuse volume's moment atlas, one tile each. */
#define VKR_DIFFUSE_VOLUME_MOMENT_ROW_PROBES 512u
/** Texels per side of a probe's moment tile. */
#define VKR_DIFFUSE_VOLUME_MOMENT_TILE 10u

/** Borrowed scene-owned sparse diffuse volume (DVOL v3). An invalid
 * indirection disables sampling. Probe p is texel (p % 1024, p / 1024) of
 * `probes` (RGBA16F: relocation offset, validity), texels 3 (p % 1024) + c of
 * row p / 1024 of `sh` (RGBA16F: L1 SH of red, green, blue), and tile
 * (p % 512, p / 512) of the 512-tile-wide RG16F `moments` atlas. `layer_sh`
 * holds `band_count` SH bands in that layout, band b in rows
 * [b * rows, (b + 1) * rows) with rows = ceil(probe_count / 1024): the
 * layers, then the lamp groups' direct light at the probes, which their
 * layers leave out. `sh` has 2 * rows rows: the renderer writes the active
 * layers' weighted sum into the first `rows` whenever `composition`
 * changes, except on frames that sample baked lamps, which take the active
 * lamp groups (bits of `active_lamp_mask`) with their direct bands
 * (`active_direct_bands`) into the second `rows` apart, because a
 * lightmapped surface already holds their light and only a surface without
 * a lightmap reads them (ADR-104). SH is
 * stored divided by `sh_scale`. `indirection` is
 * R32_UINT, dimensions[0] wide and dimensions[1] * dimensions[2] high, with
 * one entry per level-0 brick span of 3 * spacing from `origin`. */
typedef struct VkrDiffuseVolumeBinding {
  VkrTextureHandle indirection;
  VkrTextureHandle probes;
  VkrTextureHandle moments;
  VkrTextureHandle layer_sh;
  VkrTextureHandle sh;
  Vec3 origin;
  float32_t spacing;
  float32_t sh_scale;
  uint32_t dimensions[3];
  uint32_t probe_count;
  uint32_t layer_count;
  uint32_t band_count;
  uint32_t active_layer_count;
  uint32_t active_layers[VKR_DIFFUSE_VOLUME_MAX_ACTIVE_LAYERS];
  float32_t active_weights[VKR_DIFFUSE_VOLUME_MAX_ACTIVE_LAYERS];
  uint32_t active_direct_bands[VKR_DIFFUSE_VOLUME_MAX_ACTIVE_LAYERS];
  uint32_t active_lamp_mask;
  uint32_t composition;
} VkrDiffuseVolumeBinding;

/** Borrowed scene-owned 65-by-8 RGBA32F diffusion bank; zero count disables. */
typedef struct VkrSubsurfaceBinding {
  VkrTextureHandle texture;
  uint32_t profile_count;
} VkrSubsurfaceBinding;

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
  VKR_TEXTURE_SLOT_CLEARCOAT = 8,
  VKR_TEXTURE_SLOT_CLEARCOAT_ROUGHNESS = 9,
  VKR_TEXTURE_SLOT_CLEARCOAT_NORMAL = 10,
  VKR_TEXTURE_SLOT_SHEEN_COLOR = 11,
  VKR_TEXTURE_SLOT_SHEEN_ROUGHNESS = 12,
  VKR_TEXTURE_SLOT_ANISOTROPY = 13,
  /* Terrain layers 1 to 3 (layer 0 uses the slots above): base color,
     normal and occlusion-roughness-metallic maps, three slots per layer. */
  VKR_TEXTURE_SLOT_LAYER1_BASE_COLOR = 14,
  VKR_TEXTURE_SLOT_LAYER1_NORMAL = 15,
  VKR_TEXTURE_SLOT_LAYER1_ORM = 16,
  VKR_TEXTURE_SLOT_LAYER2_BASE_COLOR = 17,
  VKR_TEXTURE_SLOT_LAYER2_NORMAL = 18,
  VKR_TEXTURE_SLOT_LAYER2_ORM = 19,
  VKR_TEXTURE_SLOT_LAYER3_BASE_COLOR = 20,
  VKR_TEXTURE_SLOT_LAYER3_NORMAL = 21,
  VKR_TEXTURE_SLOT_LAYER3_ORM = 22,
  /* A layered material's mask: its RGBA weigh layers 0 to 3 when the mask
     source is VKR_MATERIAL_LAYER_MASK_TEXTURE. */
  VKR_TEXTURE_SLOT_LAYER_MASK = 23,
  /* A Custom graph's textures (vkr_material_codegen.h), in its binding's
     order; their class comes from the material file, not the slot. */
  VKR_TEXTURE_SLOT_CUSTOM0 = 24,
  VKR_TEXTURE_SLOT_CUSTOM1 = 25,
  VKR_TEXTURE_SLOT_CUSTOM2 = 26,
  VKR_TEXTURE_SLOT_CUSTOM3 = 27,
  VKR_TEXTURE_SLOT_CUSTOM4 = 28,
  VKR_TEXTURE_SLOT_CUSTOM5 = 29,
  VKR_TEXTURE_SLOT_CUSTOM6 = 30,
  VKR_TEXTURE_SLOT_CUSTOM7 = 31,
  VKR_TEXTURE_SLOT_COUNT
} VkrTextureSlot;

/* Custom graph textures and parameters a material holds. */
#define VKR_MATERIAL_CUSTOM_TEXTURES 8u
#define VKR_MATERIAL_CUSTOM_PARAMS 8u
/* A Custom graph's surface function name: `vkr_custom_` and 16 hex
   digits. */
#define VKR_MATERIAL_CUSTOM_FUNCTION_CAPACITY 32u

/* What weighs a layered material's layers. The values match the shared
   kernel's VKR_LAYER_MASK_* (shaders/shared/terrain_kernel.slangh). */
typedef enum VkrMaterialLayerMask {
  /* The vertex colour's RGBA weigh layers 0 to 3 (terrain). */
  VKR_MATERIAL_LAYER_MASK_VERTEX_COLOR = 0,
  /* The mask texture's RGBA weigh layers 0 to 3. */
  VKR_MATERIAL_LAYER_MASK_TEXTURE = 1,
  /* Layer 1 over layer 0 as the world normal's Y rises through the range:
     surfaces that face up. */
  VKR_MATERIAL_LAYER_MASK_SLOPE = 2,
  /* Layer 1 over layer 0 as the world height rises through the range. */
  VKR_MATERIAL_LAYER_MASK_HEIGHT = 3,
  VKR_MATERIAL_LAYER_MASK_COUNT,
} VkrMaterialLayerMask;

/* Layers a terrain material blends; layer 0 is the material itself. */
#define VKR_MATERIAL_TERRAIN_LAYERS 4u
/* Texture slots each extra terrain layer holds. */
#define VKR_MATERIAL_TERRAIN_LAYER_SLOTS 3u

/* The base color slot of terrain layer `layer` (1 to 3); its normal and ORM
   slots follow it. */
static inline VkrTextureSlot vkr_texture_slot_terrain_layer(uint32_t layer) {
  return (VkrTextureSlot)(VKR_TEXTURE_SLOT_LAYER1_BASE_COLOR +
                          (layer - 1u) * VKR_MATERIAL_TERRAIN_LAYER_SLOTS);
}

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
  /** Independent dielectric coating; zero factor preserves the base BSDF. */
  float32_t clearcoat_factor;
  float32_t clearcoat_roughness;
  float32_t clearcoat_normal_scale;
  /** Charlie sheen under clearcoat; zero RGB preserves existing layers. */
  Vec3 sheen_color;
  float32_t sheen_roughness;
  /** Reflection-only GGX elongation; rotation is CCW in tangent-space radians.
   */
  float32_t anisotropy_strength;
  float32_t anisotropy_rotation;
  /** Thin-sheet Lambert transmission; strength partitions the base diffuse
   * lobe. Linear tint attenuates the transmitted fraction. No refraction. */
  float32_t diffuse_transmission_strength;
  Vec3 diffuse_transmission_color;
  /** Opaque surface diffusion; profile indexes the scene RGB distance bank. */
  float32_t subsurface_strength;
  uint32_t subsurface_profile;
} VkrPbrProperties;

/* Factors of one extra terrain layer; its textures are the layer's slots. */
typedef struct VkrMaterialLayer {
  Vec4 base_color;
  float32_t metallic;
  float32_t roughness;
  float32_t normal_scale;
  float32_t occlusion_strength;
} VkrMaterialLayer;

typedef struct VkrMaterialTexture {
  VkrTextureHandle handle;
  VkrTextureSlot slot;
  bool8_t enabled; // Allow disabling without removing
} VkrMaterialTexture;

typedef struct VkrMaterial {
  uint32_t id;
  uint32_t generation;
  const char *name;

  VkrMaterialType material_type;
  VkrMaterialAlphaMode alpha_mode;
  bool8_t alpha_mode_explicit;
  /** Disable face culling for thin or explicitly two-sided surfaces. */
  bool8_t double_sided;
  /** The surface's roughness, factor times texture, exceeds zero somewhere.
   * A material file records the bound as `roughness_max`; without one, an
   * untextured surface's factor is exact and a textured surface counts as
   * smooth. The tiled pipeline blurs thin glass only when set (ADR-087). */
  bool8_t rough;
  /** Art-pass metadata the renderer does not read: the meters one texture
   * repeat covers on brush faces (zero is 1 m), and the surface tag the
   * material is made of (a VkrSurface; 0 none). */
  Vec2 world_size;
  uint8_t surface;

  // Material parameters. `phong` remains for backwards compatibility.
  VkrPhongProperties phong;
  VkrPbrProperties pbr;
  float32_t alpha_cutoff; // Alpha test threshold for cutout; 0 disables.

  // Texture maps
  VkrMaterialTexture textures[VKR_TEXTURE_SLOT_COUNT];

  /* A layered material (a terrain, or a layered Standard material) blends
     layer 0 (this material's own factors and textures) with `layers` 1 to 3
     by the weights its `layer_mask` source gives; slope and height masks
     blend from `layer_mask_range.x` to `.y`. It is opaque PBR. */
  bool8_t terrain;
  VkrMaterialLayer layers[VKR_MATERIAL_TERRAIN_LAYERS - 1u];
  uint8_t layer_mask;
  Vec2 layer_mask_range;

  /* A Custom material shades with the surface function its graph generated
     (`custom_function`, empty for Standard), reading `custom_params` and the
     Custom texture slots; until the backend has its pipelines it draws as
     the Standard fallback the rest of this material describes. */
  char custom_function[VKR_MATERIAL_CUSTOM_FUNCTION_CAPACITY];
  Vec4 custom_params[VKR_MATERIAL_CUSTOM_PARAMS];
} VkrMaterial;

Array(VkrMaterial);

typedef struct VkrMeshInstanceHandle {
  uint32_t id;
  uint32_t generation;
} VkrMeshInstanceHandle;

#define VKR_MESH_INSTANCE_HANDLE_INVALID                                       \
  (VkrMeshInstanceHandle) { .id = 0, .generation = VKR_INVALID_ID }
