#pragma once

#include "vkr_atmosphere.h"
#include "vkr_render_resources.h"

struct VkrMaterial;
struct VkrGeometryConfig;
struct VkrGeometryUpload;
struct VkrTexturePreparedLoad;

/** Which recorded publication a completion reports. */
typedef enum VkrPublicationKind {
  VKR_PUBLICATION_GEOMETRY = 0,
  VKR_PUBLICATION_LOADED_MESH,
  VKR_PUBLICATION_UNPUBLISH_GEOMETRY,
  VKR_PUBLICATION_TEXTURE,
  VKR_PUBLICATION_WRITABLE_TEXTURE,
  VKR_PUBLICATION_TEXTURE_SAMPLER,
  VKR_PUBLICATION_UNPUBLISH_TEXTURE,
  VKR_PUBLICATION_IBL_BAKE,
  VKR_PUBLICATION_ATMOSPHERE_BAKE,
  VKR_PUBLICATION_MATERIAL,
  VKR_PUBLICATION_UNPUBLISH_MATERIAL,
} VkrPublicationKind;

/** The result of one recorded publication. `id` and `generation` are the
 * geometry, texture or material handle it named; bakes name their source. */
typedef struct VkrPublicationCompletion {
  VkrPublicationKind kind;
  uint32_t id;
  uint32_t generation;
  VkrRendererError error;
} VkrPublicationCompletion;

/**
 * Coarse resource-publication seam selected once with the renderer.
 * Frame draw/dispatch loops never dispatch through this table.
 *
 * Each backend fills a native table whose calls publish at once. The
 * renderer's table, which asset systems use, records each call as a command
 * instead: the thread that renders the next frame runs the commands in order
 * before preparing that frame, and every result arrives later through
 * `poll_completion`. A publish, unpublish or bake call returns whether it was
 * recorded. Recording copies the payload, except a texture's upload data and
 * regions, which stay borrowed until that texture's completion. A resource is
 * usable once its completion reports success; frames never reference one
 * before that. Queries report what the thread rendering the last frame
 * observed.
 */
typedef struct VkrAssetPublisher {
  void *state;
  /** Next completion of the renderer's table, false when none is waiting.
   * NULL in a native table. */
  bool8_t (*poll_completion)(void *state, VkrPublicationCompletion *out);
  /** True once every accepted publication is ordered before the next frame. */
  bool8_t (*publications_idle)(void *state);
  /** Monotonic nonzero stamp for geometry/material resolvability changes. */
  uint64_t (*publication_generation)(void *state);
  /** True when a prepared texture payload can be retained for publication. */
  bool8_t (*texture_upload_available)(void *state, uint64_t upload_bytes);
  bool8_t (*publish_geometry)(void *state, VkrGeometryHandle handle,
                              const struct VkrGeometryConfig *geometry);
  bool8_t (*publish_loaded_mesh)(void *state, VkrGeometryHandle handle,
                                 const struct VkrGeometryUpload *mesh);
  bool8_t (*unpublish_geometry)(void *state, VkrGeometryHandle handle);
  /** Opens/closes one render-thread batch for ordinary texture publications. */
  bool8_t (*begin_texture_upload_batch)(void *state);
  bool8_t (*end_texture_upload_batch)(void *state);
  VkrRendererError (*publish_texture)(
      void *state, VkrTextureHandle handle,
      const struct VkrTexturePreparedLoad *texture);
  bool8_t (*publish_writable_texture)(void *state, VkrTextureHandle handle,
                                      const VkrTextureDescription *description);
  bool8_t (*update_texture_sampler)(void *state, VkrTextureHandle handle,
                                    const VkrTextureDescription *description);
  /* `sh_deringing` is the authored L2 window exponent, already validated at
     scene load (ADR-038). Zero is the identity window. */
  bool8_t (*bake_ibl_cubemap)(void *state, VkrTextureHandle source,
                              VkrTextureHandle prefilter,
                              float32_t sh_deringing);
  /** Queues a candidate generation; copies params before returning. The
      generation writes its own transmittance and multiple-scattering lookup
      textures, which the camera-dependent sky reads after publication. */
  bool8_t (*bake_atmosphere)(void *state, const VkrAtmosphereGpuParams *params,
                             VkrTextureHandle source,
                             VkrTextureHandle prefilter,
                             VkrTextureHandle transmittance,
                             VkrTextureHandle multiple_scattering,
                             float32_t sh_deringing);
  /** Nonblocking completion query. READY includes the lookups, source,
   * prefilter and SH. */
  VkrAtmosphereBakeStatus (*atmosphere_bake_status)(void *state,
                                                    VkrTextureHandle source);
  /* Published L2 coefficient slot for a source cubemap, or VKR_SH_SLOT_BLACK
     when it has none yet. Cold: the scene resolves it once per frame while
     packing probes (ADR-038). */
  uint32_t (*ibl_sh_slot)(void *state, VkrTextureHandle source);
  bool8_t (*unpublish_texture)(void *state, VkrTextureHandle handle);
  bool8_t (*publish_material)(void *state, VkrMaterialHandle handle,
                              const struct VkrMaterial *material);
  bool8_t (*unpublish_material)(void *state, VkrMaterialHandle handle);
} VkrAssetPublisher;
