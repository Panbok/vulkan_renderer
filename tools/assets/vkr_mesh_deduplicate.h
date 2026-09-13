#pragma once

#include "assets/vkr_mesh_skin.h"
#include "memory/vkr_allocator.h"
#include "vkr_buffer.h"

/** Deduplicates paired geometry/influences and remaps indices in-place.
 * Both skin arguments may be NULL for static geometry. */
bool8_t vkr_mesh_cook_deduplicate_vertices(
    VkrAllocator *scratch_allocator, const VkrVertex3d *vertices,
    const VkrMeshSkinVertex *skin_vertices, uint32_t vertex_count,
    uint32_t *indices, uint32_t index_count, VkrVertex3d **out_vertices,
    VkrMeshSkinVertex **out_skin_vertices, uint32_t *out_vertex_count);
