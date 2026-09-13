#pragma once

#include "assets/vkr_animation_cooked.h"

/* Output belongs to caller's scratch arena until caller scope/reset. */
bool8_t vkr_animation_cooked_encode(VkrAllocator *scratch_allocator,
                                    const VkrAnimationAsset *asset,
                                    uint8_t **bytes, uint64_t *size,
                                    const char **error);
