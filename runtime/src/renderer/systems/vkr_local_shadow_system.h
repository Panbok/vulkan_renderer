#pragma once

#include "math/vec.h"
#include "vkr_frame_input.h"

struct VkrLocalShadowSelection;

void vkr_local_shadow_prepare_selection(
    struct VkrLocalShadowSelection *selection, const VkrPointLight *lights,
    uint32_t light_count, Vec3 camera_position, uint32_t face_budget,
    uint32_t map_size, VkrLocalShadowPassPayload *out);
