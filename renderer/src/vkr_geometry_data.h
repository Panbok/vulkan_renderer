#pragma once

#include "memory/vkr_allocator.h"
#include "vkr_buffer.h"

/** Generates tangent vectors in-place for a triangle vertex stream. */
void vkr_geometry_generate_tangents(VkrAllocator *allocator,
                                    VkrVertex3d *vertices,
                                    uint32_t vertex_count,
                                    const uint32_t *indices,
                                    uint32_t index_count);
