#pragma once

#include <stddef.h>

#include "containers/array.h"
#include "defines.h"
#include "math/vec.h"
#include "renderer/vkr_gpu_abi.h"
#include "renderer/vkr_renderer.h"

// =============================================================================
// Vertex Types
// =============================================================================

/**
 * @brief Packed 3-float payload for GPU vertex attributes.
 *
 * This type intentionally avoids SIMD padding so reflected offsets/stride can
 * map directly to host memory without relying on compiler-specific Vec3 ABI.
 */
typedef struct VkrPackedVec3 {
  float32_t x;
  float32_t y;
  float32_t z;
} VkrPackedVec3;

/**
 * @brief Converts a math Vec3 into packed vertex storage.
 */
vkr_internal INLINE VkrPackedVec3 vkr_vertex_pack_vec3(Vec3 value) {
  return (VkrPackedVec3){.x = value.x, .y = value.y, .z = value.z};
}

/**
 * @brief Converts packed vertex storage into math Vec3.
 */
vkr_internal INLINE Vec3 vkr_vertex_unpack_vec3(VkrPackedVec3 value) {
  return vec3_new(value.x, value.y, value.z);
}

/**
 * @brief Represents a single 3D vertex using packed position/normal storage.
 */
typedef struct VkrVertex3d {
  VkrPackedVec3 position; /**< Position of the vertex in object space. */
  VkrPackedVec3 normal;   /**< Vertex normal (used for lighting). */
  Vec2 texcoord;          /**< Texture coordinate (UV). */
  Vec4 colour;            /**< Vertex colour (RGBA). */
  Vec4 tangent;           /**< Tangent vector (xyz) + handedness (w). */
} VkrVertex3d;

_Static_assert(sizeof(VkrPackedVec3) == 12, "VkrPackedVec3 must be 12 bytes");
_Static_assert(sizeof(VkrVertex3d) == 64,
               "VkrVertex3d must match reflected packed layout");
_Static_assert(offsetof(VkrVertex3d, position) == 0,
               "position offset mismatch");
_Static_assert(offsetof(VkrVertex3d, normal) == 12, "normal offset mismatch");
_Static_assert(offsetof(VkrVertex3d, texcoord) == 24,
               "texcoord offset mismatch");
_Static_assert(offsetof(VkrVertex3d, colour) == 32, "colour offset mismatch");
_Static_assert(offsetof(VkrVertex3d, tangent) == 48, "tangent offset mismatch");

/**
 * @brief Represents a single vertex in 2D space.
 */
typedef struct VkrVertex2d {
  Vec2 position; /**< 2D position (screen or UI space). */
  Vec2 texcoord; /**< Texture coordinate (UV). */
} VkrVertex2d;
