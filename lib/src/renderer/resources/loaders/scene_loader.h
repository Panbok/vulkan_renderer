/**
 * @file scene_loader.h
 * @brief Scene JSON loader for building VkrScene instances.
 *
 * Parses a JSON scene description, creates ECS entities/components, and
 * loads referenced meshes via the mesh manager.
 */
#pragma once

#include "containers/str.h"
#include "defines.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_system.h"

struct VkrRenderAssets;

/**
 * @brief Summary of a scene load operation.
 */
typedef struct VkrSceneLoadResult {
  uint32_t entity_count;
  uint32_t mesh_count;
  uint32_t text3d_count;
  uint32_t shape_count;
  uint32_t directional_light_count;
  uint32_t point_light_count;
} VkrSceneLoadResult;

typedef enum VkrSceneGltfPunctualLightType {
  VKR_SCENE_GLTF_LIGHT_DIRECTIONAL = 0,
  VKR_SCENE_GLTF_LIGHT_POINT,
  VKR_SCENE_GLTF_LIGHT_SPOT,
} VkrSceneGltfPunctualLightType;

typedef struct VkrSceneGltfPunctualLightImport {
  char name[64];
  Vec3 position;
  Vec3 direction;
  Vec3 color;
  float32_t intensity;
  float32_t range;
  float32_t inner_cone_angle;
  float32_t outer_cone_angle;
  VkrSceneGltfPunctualLightType type;
} VkrSceneGltfPunctualLightImport;

/**
 * @brief Inspect punctual-light nodes in one glTF instance.
 *
 * Applies the glTF node hierarchy followed by @p scene_world and writes at
 * most @p capacity deterministic import records. No GPU or scene state is
 * created; the async scene loader consumes the same records during finalize.
 */
bool8_t vkr_scene_loader_read_gltf_punctual_lights(
    String8 path, Mat4 scene_world, uint32_t scene_entity_index,
    VkrSceneGltfPunctualLightImport *out_lights, uint32_t capacity,
    uint32_t *out_count);

/**
 * @brief Load a scene from a JSON file path.
 *
 * @param scene Target scene (must be initialized).
 * @param assets Published asset owner for mesh and text loading.
 * @param path Path to the .scene.json file.
 * @param temp_alloc Temporary allocator for file contents and parsing.
 * @param out_result Optional load summary.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_load_from_file(VkrScene *scene,
                                 struct VkrRenderAssets *assets, String8 path,
                                 VkrAllocator *temp_alloc,
                                 VkrSceneLoadResult *out_result,
                                 VkrSceneError *out_error);

/**
 * @brief Load a scene from a JSON buffer.
 *
 * @param scene Target scene (must be initialized).
 * @param assets Published asset owner for mesh and text loading.
 * @param json Scene JSON buffer (lifetime must cover this call).
 * @param temp_alloc Temporary allocator for parse scratch data.
 * @param out_result Optional load summary.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_load_from_json(VkrScene *scene,
                                 struct VkrRenderAssets *assets, String8 json,
                                 VkrAllocator *temp_alloc,
                                 VkrSceneLoadResult *out_result,
                                 VkrSceneError *out_error);

/**
 * @brief Creates a scene loader for registration with `VkrResourceSystem`.
 *
 * The loader produces `VKR_RESOURCE_TYPE_SCENE` resources where the returned
 * handle is a `VkrSceneHandle` (runtime scene instance).
 */
VkrResourceLoader vkr_scene_loader_create(void);
