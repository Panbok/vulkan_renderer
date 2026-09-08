#pragma once

#include <cstdint>

#include "bake/vkr_bake_bvh.h"
#include "bake/vkr_bake_bsdf.h"
#include "bake/vkr_bake_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VKR_BAKE_INTEGRATOR_MAX_DEPTH 64u
#define VKR_BAKE_INTEGRATOR_MAX_MEDIUM_DEPTH 32u
#define VKR_BAKE_INTEGRATOR_MAX_TRANSPARENT_LAYERS 1024u
#define VKR_BAKE_PHOTON_GRID_MAX_CELLS 8000000u

/* The callback receives a finite unit world direction and returns radiance in
   linear scene units. A null callback represents a black environment. */
typedef Vec3 (*VkrBakeEnvironmentRadianceFn)(void *user, Vec3 direction);

typedef struct VkrBakePhoton {
  Vec3 position;
  /* Unit direction followed from its analytic emitter to this deposit. */
  Vec3 incident_direction;
  Vec3 normal;
  /* Surface flux: projected-area irradiance is already represented by the
     landing density, so gathering applies no second incident cosine. */
  Vec3 flux;
  uint32_t source_instance_index;
  uint32_t subsurface_profile;
} VkrBakePhoton;

/* Storage is caller-owned for the whole bake. Reset derives the scene bounds
   and validates the caller-provided photon/cell capacity; build sorts photon
   records in place into a uniform grid, so workers must not trace until it
   succeeds. */
typedef struct VkrBakePhotonMap {
  VkrBakePhoton *photons;
  uint32_t photon_capacity;
  uint32_t photon_count;
  uint32_t grid_dimensions[3];
  VkrBakeAabb bounds;
  float32_t radius;
  uint32_t *cell_offsets;
  bool8_t grid_ready;
} VkrBakePhotonMap;

typedef struct VkrBakePhotonSettings {
  /* Every batch normalizes against `photon_count`; it emits
     [sample_offset, sample_offset + batch_count), allowing deterministic
     disjoint work partitioning without changing total light power. */
  uint64_t seed;
  uint64_t sample_offset;
  uint32_t photon_count;
  uint32_t batch_count;
  uint32_t max_depth;
  uint32_t grid_dimensions[3];
  float32_t radius;
} VkrBakePhotonSettings;

/* Every pointer is borrowed by the initialized integrator and must remain
   stable through every trace call. Scene loading owns material texels; BVH
   construction owns neither triangles nor its node Arena. */
typedef struct VkrBakeIntegratorScene {
  const VkrBakeBvh *bvh;
  const VkrBakeTextureStore *texture_store;
  const VkrBakeMaterial *materials;
  uint32_t material_count;
  const VkrBakeSceneLight *lights;
  uint32_t light_count;
  const VkrSubsurfaceProfile *subsurface_profiles;
  uint32_t subsurface_profile_count;
} VkrBakeIntegratorScene;

typedef struct VkrBakeIntegratorSettings {
  VkrBakeIntegratorScene scene;
  VkrBakeEnvironmentRadianceFn environment_radiance;
  void *environment_user;
  uint32_t max_depth;
  /* Zero disables Russian roulette; otherwise it begins at this one-based
     surface depth. */
  uint32_t rr_start_depth;
  uint32_t max_transparent_layers;
  float32_t ray_epsilon;
  /* A grid-ready map adds analytic-light caustic density after thick
     eta-changing glass or metal chains. Null disables photon lookup. */
  const VkrBakePhotonMap *photon_map;
} VkrBakeIntegratorSettings;

/* Prepared once before workers begin. It owns no storage and performs all
   scene/settings validation outside the per-path loop. */
typedef struct VkrBakeIntegrator {
  VkrBakeIntegratorSettings settings;
} VkrBakeIntegrator;

typedef enum VkrBakeIntegratorError {
  VKR_BAKE_INTEGRATOR_ERROR_NONE = 0,
  VKR_BAKE_INTEGRATOR_ERROR_INVALID_ARGUMENT,
  VKR_BAKE_INTEGRATOR_ERROR_INVALID_SCENE,
  VKR_BAKE_INTEGRATOR_ERROR_MEDIUM_STACK_OVERFLOW,
  VKR_BAKE_INTEGRATOR_ERROR_MEDIUM_STACK_MISMATCH,
  VKR_BAKE_INTEGRATOR_ERROR_TRANSPARENT_LAYER_LIMIT,
  VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT,
} VkrBakeIntegratorError;

typedef struct VkrBakeIntegratorResult {
  Vec3 radiance;
  uint32_t surface_depth;
  uint32_t transparent_layers;
} VkrBakeIntegratorResult;

bool8_t vkr_bake_integrator_init(const VkrBakeIntegratorSettings *settings,
                                 VkrBakeIntegrator *out_integrator,
                                 VkrBakeIntegratorError *out_error);

/* Active subsurface materials mix local and projected-disk spatial events.
   Profiles retain their full analytic tail; only the same object/profile and
   consistently oriented geometric side are eligible. The spatial event does
   not cross a medium boundary. Each trace uses bounded stack state only.

   `origin` is finite and `direction` is a unit world vector. The fixed PCG
   state and every medium record live on this call's stack. A transmitted BSDF
   sample already includes radiance-mode eta correction, so this function never
   applies `eta_ratio` to throughput a second time. */
bool8_t vkr_bake_integrator_trace(const VkrBakeIntegrator *integrator,
                                  Vec3 origin, Vec3 direction, uint64_t seed,
                                  VkrBakeIntegratorResult *out_result,
                                  VkrBakeIntegratorError *out_error);

bool8_t vkr_bake_integrator_photon_map_reset(
    const VkrBakeIntegrator *integrator,
    VkrBakePhotonSettings settings, VkrBakePhotonMap *in_out_map,
    VkrBakeIntegratorError *out_error);

bool8_t vkr_bake_integrator_emit_photons(
    const VkrBakeIntegrator *integrator,
    VkrBakePhotonSettings settings, VkrBakePhotonMap *in_out_map,
    VkrBakeIntegratorError *out_error);

bool8_t vkr_bake_integrator_build_photon_grid(
    VkrBakePhotonMap *in_out_map, VkrBakeIntegratorError *out_error);

#ifdef __cplusplus
}
#endif
