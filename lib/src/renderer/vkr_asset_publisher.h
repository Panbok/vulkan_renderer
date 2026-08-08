#pragma once

#include "renderer/resources/vkr_resources.h"

struct VkrMaterial;
struct VkrGeometryConfig;
struct VkrMeshLoaderResult;
struct VkrTexturePreparedLoad;

/**
 * Coarse resource-publication seam selected once with the renderer.
 *
 * These callbacks run only during load/unload finalization. They preserve the
 * shared CPU handle identity while the selected renderer owns GPU storage and
 * retirement. Frame draw/dispatch loops never dispatch through this table.
 */
typedef struct VkrAssetPublisher {
  void *state;
  bool8_t (*publish_geometry)(void *state, VkrGeometryHandle handle,
                              const struct VkrGeometryConfig *geometry);
  bool8_t (*publish_loaded_mesh)(void *state, VkrGeometryHandle handle,
                                 const struct VkrMeshLoaderResult *mesh);
  bool8_t (*unpublish_geometry)(void *state, VkrGeometryHandle handle);
  bool8_t (*publish_texture)(void *state, VkrTextureHandle handle,
                             const struct VkrTexturePreparedLoad *texture);
  bool8_t (*publish_writable_texture)(void *state, VkrTextureHandle handle,
                                      const VkrTextureDescription *description);
  bool8_t (*update_texture_sampler)(void *state, VkrTextureHandle handle,
                                    const VkrTextureDescription *description);
  bool8_t (*bake_ibl_cubemap)(void *state, VkrTextureHandle source,
                              VkrTextureHandle irradiance,
                              VkrTextureHandle prefilter);
  bool8_t (*bake_hdr_environment)(void *state, VkrTextureHandle equirect,
                                  VkrTextureHandle source,
                                  VkrTextureHandle irradiance,
                                  VkrTextureHandle prefilter);
  bool8_t (*unpublish_texture)(void *state, VkrTextureHandle handle);
  bool8_t (*publish_material)(void *state, VkrMaterialHandle handle,
                              const struct VkrMaterial *material);
  bool8_t (*unpublish_material)(void *state, VkrMaterialHandle handle);
} VkrAssetPublisher;
