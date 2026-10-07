#include "vkr_decal.h"

#include "vkr_lighting.h"

#include <math.h>

bool8_t vkr_decal_set_box(VkrDecal *decal, Mat4 box_to_world) {
  const Mat4 *m = &box_to_world;
  for (uint32_t i = 0u; i < 16u; ++i) {
    if (!isfinite(m->elements[i])) {
      return false_v;
    }
  }
  /* The threshold matches mat4_inverse_affine's, which returns the identity
     for a box it rejects. */
  const float32_t cofactor[3] = {
      m->m11 * m->m22 - m->m12 * m->m21,
      m->m12 * m->m20 - m->m10 * m->m22,
      m->m10 * m->m21 - m->m11 * m->m20,
  };
  const float32_t determinant =
      m->m00 * cofactor[0] + m->m01 * cofactor[1] + m->m02 * cofactor[2];
  const float32_t determinant_terms = fabsf(m->m00 * cofactor[0]) +
                                      fabsf(m->m01 * cofactor[1]) +
                                      fabsf(m->m02 * cofactor[2]);
  if (!isfinite(determinant) || !isfinite(determinant_terms) ||
      fabsf(determinant) <= 1e-6f * determinant_terms) {
    return false_v;
  }

  /* The box's corners lie at its centre plus or minus half of each column,
     so its bounds reach half the absolute column sum along each axis. */
  const Vec3 centre = vec3_new(m->m03, m->m13, m->m23);
  const Vec3 reach =
      vec3_new(0.5f * (fabsf(m->m00) + fabsf(m->m01) + fabsf(m->m02)),
               0.5f * (fabsf(m->m10) + fabsf(m->m11) + fabsf(m->m12)),
               0.5f * (fabsf(m->m20) + fabsf(m->m21) + fabsf(m->m22)));
  decal->world_to_box = mat4_inverse_affine(box_to_world);
  decal->bounds_min = vec3_sub(centre, reach);
  decal->bounds_max = vec3_add(centre, reach);
  return true_v;
}

bool8_t vkr_decal_valid(const VkrDecal *decal) {
  for (uint32_t i = 0u; i < 16u; ++i) {
    if (!isfinite(decal->world_to_box.elements[i])) {
      return false_v;
    }
  }
  const Mat4 *m = &decal->world_to_box;
  const float32_t determinant = m->m00 * (m->m11 * m->m22 - m->m12 * m->m21) -
                                m->m01 * (m->m10 * m->m22 - m->m12 * m->m20) +
                                m->m02 * (m->m10 * m->m21 - m->m11 * m->m20);
  return m->m30 == 0.0f && m->m31 == 0.0f && m->m32 == 0.0f && m->m33 == 1.0f &&
         determinant != 0.0f && isfinite(determinant) &&
         isfinite(decal->bounds_min.x) && isfinite(decal->bounds_min.y) &&
         isfinite(decal->bounds_min.z) && isfinite(decal->bounds_max.x) &&
         isfinite(decal->bounds_max.y) && isfinite(decal->bounds_max.z) &&
         decal->bounds_min.x <= decal->bounds_max.x &&
         decal->bounds_min.y <= decal->bounds_max.y &&
         decal->bounds_min.z <= decal->bounds_max.z && decal->opacity >= 0.0f &&
         decal->opacity <= 1.0f && decal->fade_cos_start <= 1.0f &&
         decal->fade_cos_start > decal->fade_cos_end &&
         decal->fade_cos_end >= -1.0f && decal->depth_fade >= 0.0f &&
         decal->depth_fade <= 1.0f;
}

static uint32_t decal_grid_index(const VkrDecalGrid *grid, uint32_t x,
                                 uint32_t y, uint32_t z) {
  return x + grid->dimensions[0] * (y + grid->dimensions[1] * z);
}

static uint32_t decal_mask_count(VkrDecalMask mask) {
  uint32_t count = 0u;
  for (uint32_t word = 0u; word < VKR_DECAL_GRID_MASK_WORDS; ++word) {
    uint32_t bits = mask.words[word];
    while (bits) {
      bits &= bits - 1u;
      count++;
    }
  }
  return count;
}

/* The cell along one axis that holds `position`, clamped into the grid. */
static int32_t decal_grid_cell(const VkrDecalGrid *grid, uint32_t axis,
                               float32_t position, float32_t origin) {
  const int32_t cell = (int32_t)floorf((position - origin) / grid->cell_size);
  return Clamp(cell, 0, (int32_t)grid->dimensions[axis] - 1);
}

void vkr_decal_grid_build(const VkrDecal *decals, uint32_t count,
                          VkrDecalGrid *grid) {
  MemZero(grid, sizeof(*grid));
  count = Min(count, VKR_MAX_FRAME_DECALS);
  if (count == 0u) {
    return;
  }

  Vec3 bounds_min = decals[0].bounds_min;
  Vec3 bounds_max = decals[0].bounds_max;
  for (uint32_t i = 1u; i < count; ++i) {
    bounds_min.x = Min(bounds_min.x, decals[i].bounds_min.x);
    bounds_min.y = Min(bounds_min.y, decals[i].bounds_min.y);
    bounds_min.z = Min(bounds_min.z, decals[i].bounds_min.z);
    bounds_max.x = Max(bounds_max.x, decals[i].bounds_max.x);
    bounds_max.y = Max(bounds_max.y, decals[i].bounds_max.y);
    bounds_max.z = Max(bounds_max.z, decals[i].bounds_max.z);
  }
  grid->origin = bounds_min;
  grid->cell_count = vkr_world_grid_fit(
      vec3_sub(bounds_max, bounds_min), VKR_DECAL_GRID_MIN_CELL_SIZE,
      VKR_DECAL_GRID_MAX_CELLS, &grid->cell_size, grid->dimensions);

  /* A box meets every cell its bounds overlap, so cells may hold a decal
     that touches only their corner; the shader's box test rejects it. */
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrDecal *decal = &decals[i];
    const int32_t min_cell[3] = {
        decal_grid_cell(grid, 0u, decal->bounds_min.x, grid->origin.x),
        decal_grid_cell(grid, 1u, decal->bounds_min.y, grid->origin.y),
        decal_grid_cell(grid, 2u, decal->bounds_min.z, grid->origin.z),
    };
    const int32_t max_cell[3] = {
        decal_grid_cell(grid, 0u, decal->bounds_max.x, grid->origin.x),
        decal_grid_cell(grid, 1u, decal->bounds_max.y, grid->origin.y),
        decal_grid_cell(grid, 2u, decal->bounds_max.z, grid->origin.z),
    };
    for (int32_t z = min_cell[2]; z <= max_cell[2]; ++z) {
      for (int32_t y = min_cell[1]; y <= max_cell[1]; ++y) {
        for (int32_t x = min_cell[0]; x <= max_cell[0]; ++x) {
          const uint32_t cell =
              decal_grid_index(grid, (uint32_t)x, (uint32_t)y, (uint32_t)z);
          grid->masks[cell].words[i / 32u] |= 1u << (i % 32u);
          grid->reference_count++;
        }
      }
    }
  }

  for (uint32_t cell = 0u; cell < grid->cell_count; ++cell) {
    grid->max_decals_per_cell =
        Max(grid->max_decals_per_cell, decal_mask_count(grid->masks[cell]));
  }
}

VkrDecalMask vkr_decal_grid_mask_at(const VkrDecalGrid *grid,
                                    Vec3 world_position) {
  VkrDecalMask result = {0};
  if (grid->cell_count == 0u || grid->cell_size <= 0.0f) {
    return result;
  }
  const float32_t position[3] = {world_position.x, world_position.y,
                                 world_position.z};
  const float32_t origin[3] = {grid->origin.x, grid->origin.y, grid->origin.z};
  uint32_t cell[3];
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const int32_t index =
        (int32_t)floorf((position[axis] - origin[axis]) / grid->cell_size);
    if (index < 0 || index >= (int32_t)grid->dimensions[axis]) {
      return result;
    }
    cell[axis] = (uint32_t)index;
  }
  return grid->masks[decal_grid_index(grid, cell[0], cell[1], cell[2])];
}
