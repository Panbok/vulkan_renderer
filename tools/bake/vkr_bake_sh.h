#pragma once

#include "math/vec.h"
#include "vkr_ibl_math.h"

/* Projects cube-face radiance to L1 SH of E/pi (ADR-038's transfer). Faces
   use KTX order and the renderer's cube coordinates; the caller owns
   6*face_size*face_size linear RGB values until this synchronous call
   returns. out[c] holds channel c's linear x, y and z terms, then its
   constant term, so E/pi(n) = out[c][3] + dot(out[c].xyz, n). */
bool8_t vkr_bake_sh_project_l1(const Vec3 *radiance, uint32_t face_size,
                               float32_t deringing, float32_t out[3][4]);
Vec3 vkr_bake_cube_direction(uint32_t face, float32_t s, float32_t t);
