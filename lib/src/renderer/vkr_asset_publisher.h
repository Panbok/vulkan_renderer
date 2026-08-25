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
  /** True once every accepted publication is ordered before the next frame. */
  bool8_t (*publications_idle)(void *state);
  /** Monotonic nonzero stamp for geometry/material resolvability changes. */
  uint64_t (*publication_generation)(void *state);
  /** True when a prepared texture payload can be retained for publication. */
  bool8_t (*texture_upload_available)(void *state, uint64_t upload_bytes);
  bool8_t (*publish_geometry)(void *state, VkrGeometryHandle handle,
                              const struct VkrGeometryConfig *geometry);
  bool8_t (*publish_loaded_mesh)(void *state, VkrGeometryHandle handle,
                                 const struct VkrMeshLoaderResult *mesh);
  bool8_t (*unpublish_geometry)(void *state, VkrGeometryHandle handle);
  /** Opens/closes one render-thread batch for ordinary texture publications. */
  bool8_t (*begin_texture_upload_batch)(void *state);
  bool8_t (*end_texture_upload_batch)(void *state);
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
