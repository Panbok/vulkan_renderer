#pragma once

#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_resource_system.h"

/** Maximum path length for texture paths in parsed material data. */
#define VKR_MATERIAL_PATH_MAX 512

/** Intended sampling color space for material textures. */
typedef enum VkrMaterialTextureColorSpace {
  VKR_MATERIAL_TEXTURE_COLORSPACE_LINEAR = 0,
  VKR_MATERIAL_TEXTURE_COLORSPACE_SRGB = 1,
  /* A tangent-space normal map in a Custom texture slot, whose class the
     slot does not give. */
  VKR_MATERIAL_TEXTURE_COLORSPACE_NORMAL = 2,
} VkrMaterialTextureColorSpace;

/**
 * @brief Parsed material data before textures are loaded: factors, state and
 * texture paths resolved against the `.mt` file. Batch loading parses on
 * workers before any GPU upload.
 */
typedef struct VkrParsedMaterialData {
  char name[VKR_MATERIAL_NAME_MAX];
  VkrMaterialType material_type;
  VkrMaterialAlphaMode alpha_mode;
  bool8_t alpha_mode_explicit;
  bool8_t double_sided;
  VkrPhongProperties phong;
  VkrPbrProperties pbr;
  float32_t alpha_cutoff;
  bool8_t alpha_cutoff_set;
  bool8_t cutout_enabled;
  /** The largest roughness the surface reaches, when the file records it. */
  float32_t roughness_max;
  bool8_t roughness_max_set;
  /** `world_size=`: meters per texture repeat on brush faces, zero unset;
   * `surface=`: the surface tag, 0 none. */
  Vec2 world_size;
  uint8_t surface;
  /** A layered material (`layer1=` to `layer3=` name layer `.mt` files):
   * their factors, and their maps in the layer texture slots, compose in
   * after parsing. `layer_mask` (a VkrMaterialLayerMask) and its range weigh
   * them. `layer_paths` hold the keys' text until then. */
  bool8_t layered;
  VkrMaterialLayer layers[VKR_MATERIAL_TERRAIN_LAYERS - 1u];
  uint8_t layer_mask;
  Vec2 layer_mask_range;
  char layer_paths[VKR_MATERIAL_TERRAIN_LAYERS - 1u][VKR_MATERIAL_PATH_MAX];
  /** A Custom graph's function (`custom_function=`) and parameters
   * (`custom_param<k>=`); its textures fill the Custom texture slots
   * (`custom<k>_texture=`, with `custom<k>_colorspace=` or
   * `custom<k>_normal=true`). */
  char custom_function[VKR_MATERIAL_CUSTOM_FUNCTION_CAPACITY];
  Vec4 custom_params[VKR_MATERIAL_CUSTOM_PARAMS];

  // Texture paths as fixed buffers (thread-safe for parallel parsing)
  char texture_paths[VKR_TEXTURE_SLOT_COUNT][VKR_MATERIAL_PATH_MAX];
  VkrMaterialTextureColorSpace texture_colorspace[VKR_TEXTURE_SLOT_COUNT];

  bool8_t parse_success;
  VkrRendererError parse_error;
} VkrParsedMaterialData;

// =============================================================================
// Resource Loader Factory
// =============================================================================

/**
 * @brief Creates a material resource loader.
 *
 * The loader supports both single-item and batch loading through the resource
 * system. Use vkr_resource_system_load() for single materials and
 * vkr_resource_system_load_batch() for parallel batch loading.
 *
 * @return The configured resource loader
 */
VkrResourceLoader vkr_material_loader_create(void);

/**
 * @brief Parses `.mt` text as the contents of a file at `path`, without
 * loading textures. A definition that names a graph lowers it first
 * (vkr_material_graph.h). Scratch allocations go to `allocator`.
 *
 * @return false with `out_data->parse_error` when the loader would reject
 * the definition.
 */
bool8_t vkr_material_loader_parse_definition(VkrAllocator *allocator,
                                             String8 path, String8 definition,
                                             VkrParsedMaterialData *out_data);

/**
 * @brief Replaces the live material that a `.mt` definition names.
 *
 * Parses `definition` as the contents of a file at `path`, against which
 * relative texture references resolve (the file need not exist), and hands
 * it to vkr_material_system_replace: the material's factors, state and
 * textures change in one publication once its textures have loaded. The
 * material keeps its handle, name and holders.
 *
 * @return false with VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED when no material
 * of that name is live, or another error for an invalid definition or full
 * replacement queues.
 */
bool8_t vkr_material_loader_replace_live(VkrMaterialSystem *system,
                                         String8 path, String8 definition,
                                         VkrAllocator *temp_alloc,
                                         VkrRendererError *out_error);

/**
 * @brief Replaces live material `material` with a terrain material blending
 * the `.mt` files of four layers.
 *
 * Layer 0 supplies the material's factors and every texture; layers 1 to 3
 * supply their base color, metallic, roughness, normal scale and occlusion
 * strength, and their base color, normal and ORM maps. An empty path is a
 * plain white layer. The result is opaque PBR; transmission, thickness,
 * diffuse transmission and subsurface are off. Textures stream like any
 * replacement's (vkr_material_system_replace).
 *
 * @return false with VKR_RENDERER_ERROR_FILE_NOT_FOUND when a layer file
 * cannot be read, or another error for a stale handle, an invalid file or
 * full replacement queues.
 */
bool8_t vkr_material_loader_replace_terrain(
    VkrMaterialSystem *system, VkrMaterialHandle material,
    const String8 layer_paths[VKR_MATERIAL_TERRAIN_LAYERS],
    VkrAllocator *temp_alloc, VkrRendererError *out_error);
