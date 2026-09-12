#pragma once

#include "containers/str.h"
#include "math/mat.h"
#include "vkr_bake_geometry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VkrBakeMeshLightKind {
  VKR_BAKE_MESH_LIGHT_DIRECTIONAL = 0,
  VKR_BAKE_MESH_LIGHT_POINT = 1,
  VKR_BAKE_MESH_LIGHT_SPOT = 2,
} VkrBakeMeshLightKind;

typedef struct VkrBakeMeshLight {
  VkrBakeMeshLightKind kind;
  Vec3 position;
  Vec3 direction;
  Vec3 color;
  float32_t intensity;
  float32_t range;
  float32_t inner_cone_angle;
  float32_t outer_cone_angle;
} VkrBakeMeshLight;

/* `triangle` and `material_path` are borrowed for this callback only. */
typedef bool8_t (*VkrBakeMeshTriangleCallback)(void *user,
                                               const VkrBakeTriangle *triangle,
                                               String8 material_path);
typedef bool8_t (*VkrBakeMeshLightCallback)(void *user,
                                            const VkrBakeMeshLight *light);

typedef struct VkrBakeMeshDecodeCallbacks {
  VkrBakeMeshTriangleCallback emit_triangle;
  VkrBakeMeshLightCallback emit_light;
} VkrBakeMeshDecodeCallbacks;

/*
 * Decodes one cooked mesh, applies `entity_world` and source-node hierarchy,
 * then emits copied world-space transport inputs. This function owns and frees
 * its decoder arenas before returning. `in_out_source_instance` advances once
 * per emitted source mesh instance; callers assign no meaning to gaps caused
 * by source nodes with zero ranges.
 */
bool8_t vkr_bake_mesh_decode(const uint8_t *data, uint64_t size,
                             Mat4 entity_world,
                             uint32_t *in_out_source_instance,
                             const VkrBakeMeshDecodeCallbacks *callbacks,
                             void *user);

/* File-aware decode also applies the immutable import remap and resolves
 * explicit relative material references against source_path. */
bool8_t vkr_bake_mesh_decode_file(String8 source_path, const uint8_t *data,
                                  uint64_t size, Mat4 entity_world,
                                  uint32_t *in_out_source_instance,
                                  const VkrBakeMeshDecodeCallbacks *callbacks,
                                  void *user);

#ifdef __cplusplus
}
#endif
