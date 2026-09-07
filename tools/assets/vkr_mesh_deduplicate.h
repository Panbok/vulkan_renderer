#pragma once

#include "memory/vkr_allocator.h"
#include "vkr_buffer.h"

/** Deduplicates source vertices and remaps the supplied indices in-place. */
bool8_t vkr_mesh_cook_deduplicate_vertices(
    VkrAllocator *scratch_allocator, const VkrVertex3d *vertices,
    uint32_t vertex_count, uint32_t *indices, uint32_t index_count,
    VkrVertex3d **out_vertices, uint32_t *out_vertex_count);
