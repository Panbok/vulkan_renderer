#include "renderer/systems/vkr_scene_frame.h"

#include <stdlib.h>

#include "math/vkr_frustum.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_picking_ids.h"

vkr_internal float32_t vkr_scene_transparent_depth(Mat4 view, Mat4 model,
                                                   Vec3 local_center) {
  Vec3 world_center = mat4_mul_vec3(model, local_center);
  Vec4 view_pos = mat4_mul_vec4(
      view, vec4_new(world_center.x, world_center.y, world_center.z, 1.0f));
  float32_t depth = -view_pos.z;
  return depth > 0.0f ? depth : 0.0f;
}

vkr_internal uint64_t
vkr_scene_pack_transparent_sort_key(float32_t distance, uint32_t tie_breaker) {
  uint32_t distance_bits = 0;
  MemCopy(&distance_bits, &distance, sizeof(distance_bits));
  return ((uint64_t)distance_bits << 32) | (uint64_t)tie_breaker;
}

typedef struct VkrSceneWorldSource {
  VkrMeshHandle mesh;
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  Mat4 model;
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
  VkrDrawAlphaRouting alpha;
  uint32_t submesh_index;
  uint32_t object_id;
  uint32_t temporal_index;
  uint32_t temporal_generation;
  bool8_t bounds_valid;
  bool8_t transmissive;
  bool8_t double_sided;
  VkrShadowCasterMobility shadow_mobility;
} VkrSceneWorldSource;

typedef struct VkrSceneWorldEmitContext {
  Mat4 view;
  const uint8_t *transparent_visible;
  VkrWorldDrawCandidate *gpu_candidates;
  VkrWorldDrawCandidate *transmission_gpu_candidates;
  VkrTransparentDrawCandidate *transparent_candidates;
  uint32_t source_index;
  uint32_t static_gpu_index;
  uint32_t dynamic_gpu_index;
  uint32_t transmission_index;
  uint32_t transparent_index;
} VkrSceneWorldEmitContext;

vkr_internal inline void
vkr_scene_emit_world_source(VkrSceneWorldEmitContext *context,
                            const VkrSceneWorldSource *source) {
  const Vec3 half_extents =
      vec3_scale(vec3_sub(source->max_extents, source->min_extents), 0.5f);
  const VkrWorldDrawCandidate candidate = {
      .mesh = source->mesh,
      .geometry = source->geometry,
      .submesh_index = source->submesh_index,
      .material = source->material,
      .instance =
          {
              .model = source->model,
              .object_id = source->object_id,
              .temporal_index = source->temporal_index,
              .temporal_generation = source->temporal_generation,
          },
      .local_bounding_sphere = {source->center.x, source->center.y,
                                source->center.z, vec3_length(half_extents)},
      .state_bucket = vkr_world_draw_state_bucket(
          source->alpha.shadow_alpha_tested ? VKR_MATERIAL_ALPHA_CUTOUT
                                            : VKR_MATERIAL_ALPHA_OPAQUE,
          source->double_sided),
      .flags =
          (source->bounds_valid ? VKR_WORLD_DRAW_CANDIDATE_BOUNDS_VALID : 0u) |
          (!source->transmissive && !source->alpha.world_transparent
               ? VKR_WORLD_DRAW_CANDIDATE_CAMERA_OPAQUE
               : 0u) |
          VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER,
  };
  const uint32_t gpu_index =
      source->shadow_mobility == VKR_SHADOW_CASTER_MOBILITY_STATIC
          ? context->static_gpu_index++
          : context->dynamic_gpu_index++;
  context->gpu_candidates[gpu_index] = candidate;
  if (source->transmissive)
    context->transmission_gpu_candidates[context->transmission_index++] =
        candidate;
  if (!source->transmissive && source->alpha.world_transparent &&
      context->transparent_visible[context->source_index]) {
    const float32_t depth = vkr_scene_transparent_depth(
        context->view, source->model, source->center);
    context->transparent_candidates[context->transparent_index++] =
        (VkrTransparentDrawCandidate){
            .instance = candidate.instance,
            .mesh = source->mesh,
            .geometry = source->geometry,
            .material = source->material,
            .submesh_index = source->submesh_index,
            .sort_key = vkr_scene_pack_transparent_sort_key(
                depth, context->source_index + 1u),
        };
  }
  context->source_index++;
}

/**
 * Grows a world-space AABB by one caster's bounding sphere.
 *
 * A sphere rather than the mesh's own AABB because that is what the renderer
 * already maintains; it over-covers, which is the safe direction for a volume
 * that must contain every caster.
 */
vkr_internal inline void
vkr_scene_accumulate_caster_bounds(Vec3 *min, Vec3 *max, bool8_t *valid,
                                   Vec3 center, float32_t radius) {
  min->x = vkr_min_f32(min->x, center.x - radius);
  min->y = vkr_min_f32(min->y, center.y - radius);
  min->z = vkr_min_f32(min->z, center.z - radius);
  max->x = vkr_max_f32(max->x, center.x + radius);
  max->y = vkr_max_f32(max->y, center.y + radius);
  max->z = vkr_max_f32(max->z, center.z + radius);
  *valid = true_v;
}

/* Called before cascade fitting, using current transforms and loaded bounds. */
void vkr_scene_measure_caster_bounds(VkrMeshManager *meshes,
                                     VkrShadowCasterDepthBounds *out_bounds) {
  Vec3 min = {VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX};
  Vec3 max = {-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX};
  bool8_t valid = false_v;

  const uint32_t mesh_count = vkr_mesh_manager_count(meshes);
  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh =
        vkr_mesh_manager_get_mesh_by_live_index(meshes, i, &mesh_slot);
    if (!mesh || !mesh->visible || !mesh->bounds_valid ||
        mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;
    vkr_scene_accumulate_caster_bounds(&min, &max, &valid,
                                       mesh->bounds_world_center,
                                       mesh->bounds_world_radius);
  }

  const uint32_t instance_count = vkr_mesh_manager_instance_count(meshes);
  for (uint32_t i = 0; i < instance_count; ++i) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *instance =
        vkr_mesh_manager_get_instance_by_live_index(meshes, i, &instance_slot);
    if (!instance || !instance->visible || !instance->bounds_valid ||
        instance->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;
    vkr_scene_accumulate_caster_bounds(&min, &max, &valid,
                                       instance->bounds_world_center,
                                       instance->bounds_world_radius);
  }

  *out_bounds = (VkrShadowCasterDepthBounds){
      .min = min,
      .max = max,
      .valid = valid,
  };
}

/**
 * @brief Builds the sole GPU-driven world source and retained blend list.
 *
 * Opaque, cutout, transmission, and shadow visibility remain unculled packet
 * candidates; the selected backend owns their multi-view classification.
 * Ordinary alpha blend is the only camera-culled and depth-sorted CPU list.
 */
VkrRendererError vkr_scene_build_world_draws(
    VkrMeshManager *meshes, VkrMaterialSystem *materials,
    bool8_t publication_pending, uint64_t publication_generation, Mat4 view,
    Mat4 projection, VkrAllocator *scratch, VkrWorldPassPayload *out_payload,
    VkrVisibilityStats *out_stats) {
  *out_payload = (VkrWorldPassPayload){0};
  if (out_stats)
    *out_stats = (VkrVisibilityStats){0};
  vkr_material_system_begin_texture_residency_frame(materials);
  const VkrFrustum camera_frustum =
      vkr_frustum_from_view_projection(view, projection);
  const uint32_t mesh_count = vkr_mesh_manager_count(meshes);
  const uint32_t live_instance_count = vkr_mesh_manager_instance_count(meshes);
  const uint32_t temporal_instance_offset = vkr_mesh_manager_capacity(meshes);
  const uint64_t temporal_slot_capacity =
      (uint64_t)temporal_instance_offset +
      vkr_mesh_manager_instance_capacity(meshes);
  if (temporal_slot_capacity > VKR_TEMPORAL_TRANSFORM_CAPACITY) {
    *out_payload = (VkrWorldPassPayload){0};
    return VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
  }
  VkrVisibilityStats stats = {0};
  uint64_t candidate_count_64 = 0u;
  uint64_t static_candidate_count_64 = 0u;
  /* Missing visible casters disable retained shadow reuse until published. */
  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh =
        vkr_mesh_manager_get_mesh_by_live_index(meshes, i, &mesh_slot);
    if (mesh->visible && mesh->loading_state == VKR_MESH_LOADING_STATE_LOADED) {
      const uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
      candidate_count_64 += submesh_count;
      if (mesh->shadow_mobility == VKR_SHADOW_CASTER_MOBILITY_STATIC)
        static_candidate_count_64 += submesh_count;
    } else if (mesh->visible) {
      publication_pending = true_v;
    }
  }
  for (uint32_t i = 0; i < live_instance_count; ++i) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *instance =
        vkr_mesh_manager_get_instance_by_live_index(meshes, i, &instance_slot);
    if (!instance->visible ||
        instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      publication_pending = publication_pending || instance->visible;
      continue;
    }
    VkrMeshAsset *asset =
        vkr_mesh_manager_get_live_asset(meshes, instance->asset);
    candidate_count_64 += asset->submeshes.length;
    if (instance->shadow_mobility == VKR_SHADOW_CASTER_MOBILITY_STATIC)
      static_candidate_count_64 += asset->submeshes.length;
  }

  if (candidate_count_64 > VKR_GPU_DRAW_CANDIDATE_CAPACITY)
    return VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;

  const uint32_t gpu_candidate_count = (uint32_t)candidate_count_64;
  const uint32_t static_candidate_count = (uint32_t)static_candidate_count_64;
  uint8_t *transparent_visible = NULL;
  if (gpu_candidate_count > 0u) {
    transparent_visible = vkr_allocator_alloc(scratch, gpu_candidate_count,
                                              VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!transparent_visible) {
      *out_payload = (VkrWorldPassPayload){0};
      return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    MemZero(transparent_visible, gpu_candidate_count);
  }

  uint32_t gpu_camera_opaque_candidate_count = 0u;
  uint32_t transmission_gpu_candidate_count = 0u;
  uint32_t transparent_draw_count = 0u;
  uint32_t source_index = 0u;

  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh =
        vkr_mesh_manager_get_mesh_by_live_index(meshes, i, &mesh_slot);
    if (!mesh->visible || mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;
    const uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    for (uint32_t s = 0; s < submesh_count; ++s) {
      VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(meshes, mesh_slot, s);
      VkrMaterial *material =
          vkr_material_system_get_live(materials, submesh->material);
      const VkrDrawAlphaRouting alpha = vkr_draw_alpha_routing(
          vkr_material_system_material_alpha_mode(materials, material));
      const bool8_t transmissive =
          vkr_material_system_material_is_transmissive(material);
      stats.objects_tested++;
      stats.objects_without_bounds += mesh->bounds_valid ? 0u : 1u;
      gpu_camera_opaque_candidate_count +=
          !transmissive && !alpha.world_transparent ? 1u : 0u;
      transmission_gpu_candidate_count += transmissive ? 1u : 0u;
      if (!transmissive && alpha.world_transparent) {
        bool8_t visible = true_v;
        if (mesh->bounds_valid) {
          Vec3 center = {0};
          float32_t radius = 0.0f;
          vkr_visibility_submesh_sphere(mesh->model, submesh->center,
                                        submesh->min_extents,
                                        submesh->max_extents, &center, &radius);
          visible = vkr_frustum_test_sphere(&camera_frustum, center, radius);
        }
        transparent_visible[source_index] = visible;
        transparent_draw_count += visible ? 1u : 0u;
        stats.objects_culled_camera += visible ? 0u : 1u;
      }
      source_index++;
    }
  }

  for (uint32_t i = 0; i < live_instance_count; ++i) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *instance =
        vkr_mesh_manager_get_instance_by_live_index(meshes, i, &instance_slot);
    if (!instance->visible ||
        instance->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;
    VkrMeshAsset *asset =
        vkr_mesh_manager_get_live_asset(meshes, instance->asset);
    const uint32_t submesh_count = (uint32_t)asset->submeshes.length;
    for (uint32_t s = 0; s < submesh_count; ++s) {
      VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[s];
      VkrMaterial *material =
          vkr_material_system_get_live(materials, submesh->material);
      const VkrDrawAlphaRouting alpha = vkr_draw_alpha_routing(
          vkr_material_system_material_alpha_mode(materials, material));
      const bool8_t transmissive =
          vkr_material_system_material_is_transmissive(material);
      stats.objects_tested++;
      stats.objects_without_bounds += instance->bounds_valid ? 0u : 1u;
      gpu_camera_opaque_candidate_count +=
          !transmissive && !alpha.world_transparent ? 1u : 0u;
      transmission_gpu_candidate_count += transmissive ? 1u : 0u;
      if (!transmissive && alpha.world_transparent) {
        bool8_t visible = true_v;
        if (instance->bounds_valid) {
          Vec3 center = {0};
          float32_t radius = 0.0f;
          vkr_visibility_submesh_sphere(instance->model, submesh->center,
                                        submesh->min_extents,
                                        submesh->max_extents, &center, &radius);
          visible = vkr_frustum_test_sphere(&camera_frustum, center, radius);
        }
        transparent_visible[source_index] = visible;
        transparent_draw_count += visible ? 1u : 0u;
        stats.objects_culled_camera += visible ? 0u : 1u;
      }
      source_index++;
    }
  }

  VkrWorldDrawCandidate *gpu_candidates = NULL;
  VkrWorldDrawCandidate *transmission_gpu_candidates = NULL;
  VkrTransparentDrawCandidate *transparent_candidates = NULL;
  VkrDrawItem *transparent_draws = NULL;
  VkrInstanceDataGPU *transparent_instances = NULL;
  if (gpu_candidate_count > 0u)
    gpu_candidates = vkr_allocator_alloc(
        scratch, sizeof(*gpu_candidates) * (uint64_t)gpu_candidate_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (transmission_gpu_candidate_count > 0u)
    transmission_gpu_candidates =
        vkr_allocator_alloc(scratch,
                            sizeof(*transmission_gpu_candidates) *
                                (uint64_t)transmission_gpu_candidate_count,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (transparent_draw_count > 0u) {
    transparent_candidates = vkr_allocator_alloc(
        scratch,
        sizeof(*transparent_candidates) * (uint64_t)transparent_draw_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    transparent_draws = vkr_allocator_alloc(
        scratch, sizeof(*transparent_draws) * (uint64_t)transparent_draw_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    transparent_instances = vkr_allocator_alloc(
        scratch,
        sizeof(*transparent_instances) * (uint64_t)transparent_draw_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if ((gpu_candidate_count > 0u && !gpu_candidates) ||
      (transmission_gpu_candidate_count > 0u && !transmission_gpu_candidates) ||
      (transparent_draw_count > 0u &&
       (!transparent_candidates || !transparent_draws ||
        !transparent_instances))) {
    *out_payload = (VkrWorldPassPayload){0};
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }

  VkrSceneWorldEmitContext emit = {
      .view = view,
      .transparent_visible = transparent_visible,
      .gpu_candidates = gpu_candidates,
      .dynamic_gpu_index = static_candidate_count,
      .transmission_gpu_candidates = transmission_gpu_candidates,
      .transparent_candidates = transparent_candidates,
  };

  /* Counted spans keep static casters first while each partition and both
     side streams retain source encounter order in this single traversal. */
  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh =
        vkr_mesh_manager_get_mesh_by_live_index(meshes, i, &mesh_slot);
    if (!mesh->visible || mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;
    const uint32_t object_id =
        mesh->render_id
            ? vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE, mesh->render_id)
            : 0u;
    const uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    for (uint32_t s = 0; s < submesh_count; ++s) {
      VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(meshes, mesh_slot, s);
      VkrMaterial *material =
          vkr_material_system_get_live(materials, submesh->material);
      const VkrMaterialHandle draw_material =
          material ? (VkrMaterialHandle){.id = material->id,
                                         .generation = material->generation}
                   : submesh->material;
      vkr_material_system_touch_texture_residency(materials, draw_material);
      const VkrDrawAlphaRouting alpha = vkr_draw_alpha_routing(
          vkr_material_system_material_alpha_mode(materials, material));
      const bool8_t transmissive =
          vkr_material_system_material_is_transmissive(material);
      const VkrSceneWorldSource source = {
          .mesh = {.id = mesh_slot + 1u, .generation = 0u},
          .geometry = submesh->geometry,
          .material = draw_material,
          .model = mesh->model,
          .center = submesh->center,
          .min_extents = submesh->min_extents,
          .max_extents = submesh->max_extents,
          .alpha = alpha,
          .submesh_index = s,
          .object_id = object_id,
          .temporal_index = mesh_slot,
          .temporal_generation = mesh->temporal_generation,
          .bounds_valid = mesh->bounds_valid,
          .transmissive = transmissive,
          .double_sided = material ? material->double_sided : false_v,
          .shadow_mobility = mesh->shadow_mobility,
      };
      vkr_scene_emit_world_source(&emit, &source);
    }
  }

  for (uint32_t i = 0; i < live_instance_count; ++i) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *instance =
        vkr_mesh_manager_get_instance_by_live_index(meshes, i, &instance_slot);
    if (!instance->visible ||
        instance->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;
    VkrMeshAsset *asset =
        vkr_mesh_manager_get_live_asset(meshes, instance->asset);
    const uint32_t object_id =
        instance->render_id ? vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE,
                                                    instance->render_id)
                            : 0u;
    const uint32_t submesh_count = (uint32_t)asset->submeshes.length;
    for (uint32_t s = 0; s < submesh_count; ++s) {
      VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[s];
      VkrMaterial *material =
          vkr_material_system_get_live(materials, submesh->material);
      const VkrMaterialHandle draw_material =
          material ? (VkrMaterialHandle){.id = material->id,
                                         .generation = material->generation}
                   : submesh->material;
      vkr_material_system_touch_texture_residency(materials, draw_material);
      const VkrDrawAlphaRouting alpha = vkr_draw_alpha_routing(
          vkr_material_system_material_alpha_mode(materials, material));
      const bool8_t transmissive =
          vkr_material_system_material_is_transmissive(material);
      const VkrSceneWorldSource source = {
          .mesh = {.id = instance_slot + 1u,
                   .generation = instance->generation},
          .geometry = submesh->geometry,
          .material = draw_material,
          .model = instance->model,
          .center = submesh->center,
          .min_extents = submesh->min_extents,
          .max_extents = submesh->max_extents,
          .alpha = alpha,
          .submesh_index = s,
          .object_id = object_id,
          .temporal_index = temporal_instance_offset + instance_slot,
          .temporal_generation = instance->generation,
          .bounds_valid = instance->bounds_valid,
          .transmissive = transmissive,
          .double_sided = material ? material->double_sided : false_v,
          .shadow_mobility = instance->shadow_mobility,
      };
      vkr_scene_emit_world_source(&emit, &source);
    }
  }

  if (transparent_draw_count > 1u)
    qsort(transparent_candidates, transparent_draw_count,
          sizeof(*transparent_candidates), vkr_transparent_draw_depth_compare);
  vkr_transparent_draw_emit(transparent_candidates, transparent_draw_count,
                            transparent_draws, transparent_instances);

  *out_payload = (VkrWorldPassPayload){
      .gpu_candidates = gpu_candidates,
      .gpu_candidate_count = gpu_candidate_count,
      .gpu_camera_opaque_candidate_count = gpu_camera_opaque_candidate_count,
      .gpu_shadow_candidate_count = gpu_candidate_count,
      .static_candidate_count = static_candidate_count,
      .static_generation = meshes->generations.static_content,
      .dynamic_generation = meshes->generations.dynamic_content,
      .publication_generation = publication_generation,
      .caster_bounds_generation = meshes->generations.caster_bounds,
      .publication_pending = publication_pending,
      .transmission_gpu_candidates = transmission_gpu_candidates,
      .transmission_gpu_candidate_count = transmission_gpu_candidate_count,
      .transparent_draws = transparent_draws,
      .transparent_draw_count = transparent_draw_count,
      .instances = transparent_instances,
      .instance_count = transparent_draw_count,
  };
  if (out_stats)
    *out_stats = stats;
  return VKR_RENDERER_ERROR_NONE;
}
