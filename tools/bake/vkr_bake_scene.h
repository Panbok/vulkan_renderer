#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vkr_bake_atmosphere.h"

extern "C" {
#include "memory/vkr_allocator.h"
#include "vkr_bake_geometry.h"
#include "vkr_bake_material.h"
#include "vkr_lighting.h"
#include "vkr_subsurface.h"
}

enum class VkrBakeSceneLightKind : uint8_t {
  Directional,
  Point,
  Spot,
  Polynomial,
  Rectangle,
};

struct VkrBakeSceneLight {
  VkrBakeSceneLightKind kind = VkrBakeSceneLightKind::Point;
  Vec3 position = {};
  Vec3 direction = {};
  Vec3 right = {};
  Vec3 up = {};
  Vec3 color = {};
  float32_t intensity = 0.0f;
  float32_t range = 0.0f;
  float32_t constant = 1.0f;
  float32_t linear = 0.0f;
  float32_t quadratic = 0.0f;
  float32_t inner_cone_angle = 0.0f;
  float32_t outer_cone_angle = 0.0f;
  float32_t half_width = 0.0f;
  float32_t half_height = 0.0f;
  float32_t radiance = 0.0f;
  bool8_t casts_shadow = false_v;
  bool8_t enabled = true_v;
};

enum class VkrBakeSceneEnvironmentKind : uint8_t {
  None,
  Equirect,
  CubemapPath,
  CubemapFaces,
};

/* The path fields stay owned by this scene until the bake has completed. */
struct VkrBakeSceneEnvironment {
  VkrBakeSceneEnvironmentKind kind = VkrBakeSceneEnvironmentKind::None;
  bool8_t enabled = false_v;
  std::string path;
  std::string base_path;
  std::string extension;
  float32_t intensity = 1.0f;
  float32_t diffuse_intensity = 1.0f;
  float32_t specular_intensity = 1.0f;
  float32_t sh_deringing = 0.0f;
  uint32_t texture_index = UINT32_MAX;
  uint32_t face_texture_indices[6] = {UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                      UINT32_MAX, UINT32_MAX, UINT32_MAX};
};

enum class VkrBakeSceneError : uint8_t {
  None,
  InvalidArgument,
  Io,
  Parse,
  Unsupported,
  CookedMesh,
  Material,
  OutOfMemory,
};

/*
 * Cold, single-owner scene record. `triangles` may be partitioned once by the
 * BVH builder after a successful load; workers then borrow the complete record
 * as const. `texture_store` owns decoded texels referenced by `materials`.
 */
struct VkrBakeScene {
  explicit VkrBakeScene(VkrAllocator *scene_allocator);
  ~VkrBakeScene();
  VkrBakeScene(const VkrBakeScene &) = delete;
  VkrBakeScene &operator=(const VkrBakeScene &) = delete;

  VkrAllocator *allocator = nullptr;
  std::vector<VkrBakeMaterial> materials;
  std::vector<VkrBakeTriangle> triangles;
  uint32_t zero_area_triangle_count = 0;
  std::vector<VkrBakeSceneLight> lights;
  std::vector<std::string> dependency_paths;
  VkrBakeSceneEnvironment environment;
  VkrBakeAtmosphere atmosphere;
  VkrSubsurfaceProfile subsurface_profiles[VKR_SUBSURFACE_PROFILE_COUNT] = {};
  uint32_t subsurface_profile_count = 0u;
  VkrBakeTextureStore *texture_store = nullptr;
};

/*
 * Fully prepares `scene`: all paths are read, geometry is flattened to world
 * space, and all material texture dependencies are resident in texture_store.
 * It never starts workers. On failure it leaves `scene` empty and releasable.
 */
bool vkr_bake_scene_load(VkrBakeScene *scene, const char *scene_path,
                         VkrBakeSceneError *out_error);

/* `scene` is successfully loaded and `direction` is finite and unit length.
   Returns linear radiance from the selected legacy environment or baked
   atmosphere. Disabled or absent environments return black. */
Vec3 vkr_bake_scene_sample_environment(const VkrBakeScene *scene,
                                       Vec3 direction);
