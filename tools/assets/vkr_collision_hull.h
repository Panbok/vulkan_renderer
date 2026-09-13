#pragma once
#include "defines.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Offline only; caller supplies256 xyz vertices and1536 index slots. */
bool8_t vkr_collision_build_hull(const float32_t *positions, uint32_t count,
                                 float32_t *out_positions,
                                 uint32_t *out_indices,
                                 uint32_t *out_vertex_count,
                                 uint32_t *out_index_count, const char **error);
#ifdef __cplusplus
}
#endif
