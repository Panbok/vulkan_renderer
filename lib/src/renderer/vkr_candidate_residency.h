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
  uint32_t packed_static_count;
  uint32_t omitted_static_count;
  bool8_t valid;
} VkrCandidateResidencyState;

static inline bool8_t vkr_candidate_residency_needs_static_repack(
    const VkrCandidateResidencyState *state, uint64_t static_generation,
    uint64_t publication_generation, uint64_t resource_generation) {
  return !state->valid || state->static_generation != static_generation ||
                 state->publication_generation != publication_generation ||
                 state->resource_generation != resource_generation
             ? true_v
             : false_v;
}

static inline VkrCandidateResidencyState vkr_candidate_residency_stage(
    uint64_t static_generation, uint64_t publication_generation,
    uint64_t resource_generation, uint32_t packed_static_count,
    uint32_t omitted_static_count) {
  return (VkrCandidateResidencyState){
      .static_generation = static_generation,
      .publication_generation = publication_generation,
      .resource_generation = resource_generation,
      .packed_static_count = packed_static_count,
      .omitted_static_count = omitted_static_count,
      .valid = static_generation != 0u && publication_generation != 0u &&
                       resource_generation != 0u
                   ? true_v
                   : false_v,
  };
}
