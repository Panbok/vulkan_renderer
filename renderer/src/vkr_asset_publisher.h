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
 * geometry, texture or material handle it named; an IBL bake names its
 * prefilter and an atmosphere bake its source. */
typedef struct VkrPublicationCompletion {
  VkrPublicationKind kind;
  uint32_t id;
  uint32_t generation;
  VkrRendererError error;
} VkrPublicationCompletion;

/**
 * What the frame-loop thread knows about one resource's recorded commands.
 * The owning asset system counts each command it records and each completion
 * that names the resource. `error` is the first failure among the commands
 * recorded since the resource last settled, so a later attempt starts clean.
 * A resource is confirmed once nothing is outstanding and nothing failed.
 */
typedef struct VkrPublicationState {
  uint32_t outstanding;
  VkrRendererError error;
} VkrPublicationState;

static inline void vkr_publication_state_recorded(VkrPublicationState *state) {
  if (state->outstanding == 0u) {
    state->error = VKR_RENDERER_ERROR_NONE;
  }
  state->outstanding++;
}

static inline void vkr_publication_state_complete(VkrPublicationState *state,
                                                  VkrRendererError error) {
  if (state->outstanding > 0u) {
    state->outstanding--;
  }
  if (state->error == VKR_RENDERER_ERROR_NONE) {
    state->error = error;
  }
}

struct VkrAssetPublisher;

/* Counts a command recorded through `publisher` against `state`. A native
   table, which reports no completions, has already published. */
static inline void
vkr_publication_state_recorded_by(const struct VkrAssetPublisher *publisher,
                                  VkrPublicationState *state);

static inline bool8_t
vkr_publication_state_settled(const VkrPublicationState *state) {
  return state->outstanding == 0u;
}

static inline bool8_t
vkr_publication_state_confirmed(const VkrPublicationState *state) {
  return state->outstanding == 0u && state->error == VKR_RENDERER_ERROR_NONE;
}

/**
 * Coarse resource-publication seam selected once with the renderer.
 * Frame draw/dispatch loops never dispatch through this table.
 *
 * Each backend fills a native table whose calls publish at once. The
 * renderer's table, which asset systems use, records each call as a command
 * instead: the thread that renders the next frame runs the commands in order
 * before preparing that frame, and every result arrives later through
 * `poll_completion`. A publish, unpublish or bake call returns whether it was
 * recorded; recording copies the payload, except texture bytes the caller
 * retains until the completion (`VkrTexturePreparedLoad.upload_retained`). A
 * publication that succeeds is
 * resolvable in the frame it was recorded for, but only its completion tells
 * whether it succeeded, so asset systems track each resource with a
 * VkrPublicationState and admit loaded meshes, loaded textures and baked
 * environments into frames once confirmed. Queries report what the thread
 * rendering the last frame observed.
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
  /** True when a prepared texture payload can be retained for publication.
   * NULL in a native table. */
  bool8_t (*texture_upload_available)(void *state, uint64_t upload_bytes);
  /** Prepared texture bytes a native table can retain now, UINT64_MAX when
   * unbounded. NULL in the renderer's table. */
  uint64_t (*texture_upload_capacity)(void *state);
  bool8_t (*publish_geometry)(void *state, VkrGeometryHandle handle,
                              const struct VkrGeometryConfig *geometry);
  bool8_t (*publish_loaded_mesh)(void *state, VkrGeometryHandle handle,
                                 const struct VkrGeometryUpload *mesh);
  bool8_t (*unpublish_geometry)(void *state, VkrGeometryHandle handle);
  /** Opens/closes one render-thread upload batch: ordinary texture
   * publications and runs of geometry publications share its submission. */
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

static inline void
vkr_publication_state_recorded_by(const VkrAssetPublisher *publisher,
                                  VkrPublicationState *state) {
  if (publisher->poll_completion) {
    vkr_publication_state_recorded(state);
  }
}
