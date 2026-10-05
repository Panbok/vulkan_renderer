#pragma once

#include "defines.h"

#ifdef __cplusplus
extern "C" {
#endif

bool32_t run_mesh_cooked_tests(void);

/* Cooks a unit cube split into two source meshes with a lightmap UV set at
 * this density. The caller frees the returned bytes with free(). */
uint8_t *test_cook_lightmap_cube(float32_t texels_per_unit, uint64_t *out_size);

#ifdef __cplusplus
}
#endif
