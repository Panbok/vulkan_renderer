#pragma once

#include "defines.h"

/**
 * Completion-slot-local authority for retained static GPU candidate rows.
 *
 * A backend stages a replacement after packing, then copies it into this record
 * only after submission succeeds. Zero generations and a zeroed record are
 * invalid by construction, so cancelled frames cannot accidentally publish
 * rows that never reached the slot's GPU buffers.
 */
typedef struct VkrCandidateResidencyState {
  uint64_t static_generation;
  uint64_t publication_generation;
  uint64_t resource_generation;
  /* The static rows' cull words (VkrWorldPassPayload::
     static_cull_generation), which change without a static content
     change. */
  uint64_t static_cull_generation;
  uint32_t packed_static_count;
  uint32_t omitted_static_count;
  bool8_t valid;
} VkrCandidateResidencyState;

/**
 * Why a slot packs its static rows again: bits of
 * VkrPacketBuildMetrics::static_repack_reasons, which recordings and the
 * harness report, so a frame that repacks can be traced to its cause.
 */
typedef enum VkrStaticRepackReason {
  /* No committed residency, or bucket counts taken for another one. */
  VKR_STATIC_REPACK_UNPACKED = 1u << 0u,
  /* A static caster's content, or the caster set, changed. */
  VKR_STATIC_REPACK_STATIC_CONTENT = 1u << 1u,
  /* The backend published or retired something a candidate row reads. */
  VKR_STATIC_REPACK_PUBLICATION = 1u << 2u,
  /* The candidate or instance buffers were replaced. */
  VKR_STATIC_REPACK_RESOURCE = 1u << 3u,
  /* A static row's cull word changed (rooms, hidden ids, proxies). */
  VKR_STATIC_REPACK_CULL = 1u << 4u,
} VkrStaticRepackReason;

/**
 * Kinds of backend publication that move the candidate publication
 * generation, and with it every slot's resident static rows: bits of
 * VkrPacketBuildMetrics::publication_changes.
 */
typedef enum VkrPublicationChange {
  /* A material's textures finished or started uploading. */
  VKR_PUBLICATION_CHANGE_TEXTURE_READINESS = 1u << 0u,
  /* Geometry was published or its upload completed. */
  VKR_PUBLICATION_CHANGE_GEOMETRY = 1u << 1u,
  /* Geometry was unpublished or retired. */
  VKR_PUBLICATION_CHANGE_GEOMETRY_RETIRE = 1u << 2u,
  /* A material was published or unpublished. */
  VKR_PUBLICATION_CHANGE_MATERIAL = 1u << 3u,
  /* A texture's sampler changed under live materials. */
  VKR_PUBLICATION_CHANGE_SAMPLER = 1u << 4u,
} VkrPublicationChange;

/* The VkrStaticRepackReason bits that make `state` stale; zero when the
   resident static rows still hold. */
static inline uint32_t vkr_candidate_residency_static_repack_reasons(
    const VkrCandidateResidencyState *state, uint64_t static_generation,
    uint64_t publication_generation, uint64_t resource_generation,
    uint64_t static_cull_generation) {
  if (!state->valid) {
    return VKR_STATIC_REPACK_UNPACKED;
  }
  uint32_t reasons = 0u;
  if (state->static_generation != static_generation) {
    reasons |= VKR_STATIC_REPACK_STATIC_CONTENT;
  }
  if (state->publication_generation != publication_generation) {
    reasons |= VKR_STATIC_REPACK_PUBLICATION;
  }
  if (state->resource_generation != resource_generation) {
    reasons |= VKR_STATIC_REPACK_RESOURCE;
  }
  if (state->static_cull_generation != static_cull_generation) {
    reasons |= VKR_STATIC_REPACK_CULL;
  }
  return reasons;
}

static inline VkrCandidateResidencyState vkr_candidate_residency_stage(
    uint64_t static_generation, uint64_t publication_generation,
    uint64_t resource_generation, uint64_t static_cull_generation,
    uint32_t packed_static_count, uint32_t omitted_static_count) {
  return (VkrCandidateResidencyState){
      .static_generation = static_generation,
      .publication_generation = publication_generation,
      .resource_generation = resource_generation,
      .static_cull_generation = static_cull_generation,
      .packed_static_count = packed_static_count,
      .omitted_static_count = omitted_static_count,
      .valid = static_generation != 0u && publication_generation != 0u &&
                       resource_generation != 0u
                   ? true_v
                   : false_v,
  };
}
