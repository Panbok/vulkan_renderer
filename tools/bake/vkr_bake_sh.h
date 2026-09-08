#pragma once

#include "math/vec.h"
#include "vkr_ibl_math.h"

/* Faces use KTX order and the renderer's cube coordinates. The caller owns
   6*face_size*face_size linear RGB values until this synchronous call returns.
 */
bool8_t vkr_bake_sh_project(const Vec3 *radiance, uint32_t face_size,
                            float32_t deringing, VkrShL2Packed *out);
Vec3 vkr_bake_cube_direction(uint32_t face, float32_t s, float32_t t);
