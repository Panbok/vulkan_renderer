#include "renderer/systems/vkr_scene_animation.h"
#include "renderer/systems/vkr_scene_physics.h"

#include "core/logger.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/animation_loader.h"
#include "renderer/resources/loaders/mesh_loader.h"

#include "renderer/systems/vkr_render_assets.h"
#include "vkr_packed_geometry.h"

#include <float.h>
#include <math.h>

typedef struct SceneAnimationBox {
  Vec3 min;
  Vec3 max;
  bool8_t valid;
} SceneAnimationBox;

typedef struct SceneAnimationNodeBounds {
  uint64_t first_joint;
  uint32_t joint_count;
  float64_t weight_error;
  SceneAnimationBox rigid;
} SceneAnimationNodeBounds;

static void scene_animation_box_add(SceneAnimationBox *box, Vec3 point) {
  if (!box->valid) {
    box->min = box->max = point;
    box->valid = true_v;
    return;
  }
  box->min = vec3_new(Min(box->min.x, point.x), Min(box->min.y, point.y),
                      Min(box->min.z, point.z));
  box->max = vec3_new(Max(box->max.x, point.x), Max(box->max.y, point.y),
                      Max(box->max.z, point.z));
}

/* Double intermediates bound the affine expression, including cancellation.
 * A float32 rounding margin is applied after the union. */
static bool8_t scene_animation_box_transform(const SceneAnimationBox *source,
                                             Mat4 matrix,
                                             SceneAnimationBox *result,
                                             float64_t terms[3]) {
  if (!source->valid) {
    return true_v;
  }
  for (uint32_t corner = 0; corner < 8; ++corner) {
    const float64_t p[4] = {(corner & 1u) ? source->max.x : source->min.x,
                            (corner & 2u) ? source->max.y : source->min.y,
                            (corner & 4u) ? source->max.z : source->min.z, 1.0};
    float32_t output[3];
    for (uint32_t axis = 0; axis < 3; ++axis) {
      float64_t value = 0;
      float64_t magnitude = 0;
      for (uint32_t column = 0; column < 4; ++column) {
        const float64_t term =
            (float64_t)matrix.elements[column * 4u + axis] * p[column];
        value += term;
        magnitude += fabs(term);
      }
      if (!isfinite(value) || magnitude > FLT_MAX * 0.5) {
        return false_v;
      }
      terms[axis] = Max(terms[axis], magnitude);
      output[axis] = (float32_t)value;
    }
    scene_animation_box_add(result, vec3_new(output[0], output[1], output[2]));
  }
  return true_v;
}

static bool8_t scene_animation_box_expand(SceneAnimationBox *box,
                                          const float64_t terms[3],
                                          float64_t weight_error) {
  if (!box->valid) {
    return false_v;
  }
  const float64_t lower[3] = {box->min.x, box->min.y, box->min.z};
  const float64_t upper[3] = {box->max.x, box->max.y, box->max.z};
  float32_t min[3];
  float32_t max[3];
  for (uint32_t axis = 0; axis < 3; ++axis) {
    /* Positive weights produce a convex combination after normalization.
     * Cover its source sum error, affine/weighted float32 operation ordering,
     * and the final conversion, even when large terms cancel near zero. */
    const float64_t margin =
        terms[axis] * (weight_error + 64.0 * FLT_EPSILON) + 1e-6;
    if (lower[axis] - margin < -FLT_MAX || upper[axis] + margin > FLT_MAX) {
      return false_v;
    }
    min[axis] = nextafterf((float32_t)(lower[axis] - margin), -INFINITY);
    max[axis] = nextafterf((float32_t)(upper[axis] + margin), INFINITY);
    if (!isfinite(min[axis]) || !isfinite(max[axis])) {
      return false_v;
    }
  }
  box->min = vec3_new(min[0], min[1], min[2]);
  box->max = vec3_new(max[0], max[1], max[2]);
  return true_v;
}

static bool8_t scene_animation_reserve_add(uint64_t *reserve, uint64_t count,
                                           uint64_t size) {
  if (count > (UINT64_MAX - *reserve) / size) {
    return false_v;
  }
  *reserve += count * size;
  return true_v;
}

struct s_VkrSceneAnimation {
  VkrSceneAnimation *next;
  VkrScene *scene;
  VkrVertex3d *bind_vertices;
  VkrSkinningInput *skinning;
  Vec3 *bounds_min;
  Vec3 *bounds_max;
  uint64_t bounds_generation;
  SceneAnimationNodeBounds *node_bounds;
  SceneAnimationBox *joint_bounds;
  SceneAnimationBox preview_bounds;
  bool8_t bounds_failed;
  Arena *arena;
  VkrAllocator allocator;
  VkrEntityId wrapper;
  VkrEntityId *nodes;
  const VkrMeshLoaderResult *mesh;
  VkrAnimationPlayer *player;
  VkrAnimationGraphInstance *controller;
  bool8_t controller_enabled;
  VkrResourceHandleInfo mesh_request;
  VkrResourceHandleInfo bank_request;
  String8 mesh_path;
  String8 bank_path;
  bool8_t owns_requests;
};

static bool8_t scene_animation_node_bounds(const VkrSceneAnimation *animation,
                                           uint32_t node, const Mat4 *palette,
                                           SceneAnimationBox *result) {
  const SceneAnimationNodeBounds *binding = &animation->node_bounds[node];
  float64_t terms[3] = {0};
  *result = (SceneAnimationBox){0};
  for (uint32_t joint = 0; joint < binding->joint_count; ++joint) {
    if (!scene_animation_box_transform(
            &animation->joint_bounds[binding->first_joint + joint],
            palette[joint], result, terms)) {
      return false_v;
    }
  }
  return scene_animation_box_expand(result, terms, binding->weight_error);
}

static bool8_t scene_animation_init_bounds(VkrSceneAnimation *animation,
                                           const VkrAnimationAsset *asset,
                                           VkrAllocator *scratch) {
  const VkrMeshLoaderResult *mesh = animation->mesh;
  uint64_t first_joint = 0;
  for (uint32_t n = 0; n < mesh->source.nodes.length; ++n) {
    const VkrMeshSourceNode *node = &mesh->source.nodes.data[n];
    if (!node->in_scene || node->mesh_variant >= mesh->source.meshes.length) {
      continue;
    }
    SceneAnimationNodeBounds *binding = &animation->node_bounds[n];
    binding->first_joint = first_joint;
    if (node->skin != UINT32_MAX) {
      binding->joint_count = mesh->skin.joint_counts[node->skin];
      first_joint += binding->joint_count;
    }
    const VkrMeshSourceMesh *source =
        &mesh->source.meshes.data[node->mesh_variant];
    for (uint32_t r = 0; r < source->range_count; ++r) {
      const VkrGeometryUploadRange *range =
          &mesh->submeshes.data[source->first_range + r];
      for (uint32_t k = 0; k < range->index_count; ++k) {
        const uint32_t index =
            ((const uint32_t *)
                 mesh->mesh_buffer.indices)[range->first_index + k];
        const VkrPackedVec3 vertex = animation->bind_vertices[index].position;
        const Vec3 position = vec3_new(vertex.x, vertex.y, vertex.z);
        if (!binding->joint_count) {
          scene_animation_box_add(&binding->rigid, position);
          continue;
        }
        const VkrMeshSkinVertex *influence = &mesh->skin.vertices[index];
        float64_t weight_sum = 0;
        for (uint32_t j = 0; j < 4; ++j) {
          const float32_t weight = influence->weights[j];
          weight_sum += weight;
          if (weight > 0) {
            scene_animation_box_add(
                &animation->joint_bounds[binding->first_joint +
                                         influence->joints[j]],
                position);
          }
        }
        binding->weight_error =
            Max(binding->weight_error, fabs(weight_sum - 1.0));
      }
    }
  }
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return false_v;
  }
  Mat4 *rest =
      vkr_allocator_alloc(scratch, (uint64_t)asset->node_count * sizeof(Mat4),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  bool8_t ok = rest != NULL;
  for (uint32_t i = 0; ok && i < asset->node_count; ++i) {
    const uint32_t n = asset->node_order[i];
    const uint32_t parent = asset->nodes[n].parent;
    rest[n] = parent == UINT32_MAX
                  ? asset->nodes[n].local
                  : mat4_mul(rest[parent], asset->nodes[n].local);
  }
  for (uint32_t n = 0; ok && n < mesh->source.nodes.length; ++n) {
    const SceneAnimationNodeBounds *binding = &animation->node_bounds[n];
    SceneAnimationBox bounds = {0};
    float64_t terms[3] = {0};
    if (binding->joint_count) {
      const VkrAnimationSkin *skin =
          &asset->skins[mesh->source.nodes.data[n].skin];
      for (uint32_t j = 0; ok && j < binding->joint_count; ++j) {
        const Mat4 palette =
            mat4_mul(rest[skin->joints[j]], skin->inverse_bind[j]);
        ok = scene_animation_box_transform(
            &animation->joint_bounds[binding->first_joint + j], palette,
            &bounds, terms);
      }
    } else if (binding->rigid.valid) {
      ok = scene_animation_box_transform(&binding->rigid, rest[n], &bounds,
                                         terms);
    }
    if (ok && bounds.valid) {
      ok = scene_animation_box_expand(&bounds, terms, binding->weight_error);
      if (ok) {
        scene_animation_box_add(&animation->preview_bounds, bounds.min);
        scene_animation_box_add(&animation->preview_bounds, bounds.max);
      }
    }
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return ok;
}

static bool8_t scene_animation_fail(const char **error, const char *message) {
  if (error) {
    *error = message;
  }
  return false_v;
}

static bool8_t scene_animation_mapping_valid(VkrScene *scene,
                                             VkrEntityId wrapper,
                                             const VkrMeshSource *source,
                                             const VkrEntityId *nodes) {
  const SceneTransform *wrapper_transform =
      vkr_scene_get_transform(scene, wrapper);
  const SceneSourceIdentity *wrapper_identity =
      vkr_entity_get_component_if_alive_const(scene->world, wrapper,
                                              scene->comp_source_identity);
  if (!wrapper_transform || !wrapper_identity) {
    return false_v;
  }
  for (uint32_t i = 0; i < source->nodes.length; ++i) {
    const VkrMeshSourceNode *node = &source->nodes.data[i];
    if (!node->in_scene) {
      if (nodes[i].u64 != VKR_ENTITY_ID_INVALID.u64) {
        return false_v;
      }
      continue;
    }
    const SceneTransform *transform = vkr_scene_get_transform(scene, nodes[i]);
    const SceneSourceIdentity *identity =
        vkr_entity_get_component_if_alive_const(scene->world, nodes[i],
                                                scene->comp_source_identity);
    const VkrEntityId parent =
        node->parent == UINT32_MAX ? wrapper : nodes[node->parent];
    if (!transform || !identity || nodes[i].u64 == wrapper.u64 ||
        transform->parent.u64 != parent.u64 ||
        ((transform->flags & SCENE_TRANSFORM_DIRTY_LOCAL) &&
         !transform->matrix_authored) ||
        MemCompare(&transform->local, &node->local, sizeof(Mat4)) != 0 ||
        identity->gltf_node_index != i ||
        identity->gltf_skin_index != node->skin ||
        identity->scene_entity_index != wrapper_identity->scene_entity_index ||
        identity->source_fingerprint != wrapper_identity->source_fingerprint) {
      return false_v;
    }
  }
  return true_v;
}

static void scene_animation_destroy(VkrSceneAnimation *animation) {
  Arena *arena = animation->arena;
  if (animation->scene && animation->scene->assets && animation->skinning) {
    VkrMeshManager *manager = &animation->scene->assets->mesh_manager;
    for (uint32_t i = 0; i < animation->mesh->source.nodes.length; ++i) {
      const SceneMeshRenderer *component =
          vkr_entity_get_component_if_alive_const(
              animation->scene->world, animation->nodes[i],
              animation->scene->comp_mesh_renderer);
      const SceneTransform *transform =
          vkr_scene_get_transform(animation->scene, animation->nodes[i]);
      VkrMeshInstance *instance =
          component
              ? vkr_mesh_manager_get_instance(manager, component->instance)
              : NULL;
      if (instance && transform &&
          instance->skinning == &animation->skinning[i]) {
        vkr_mesh_manager_instance_set_skinning(manager, component->instance,
                                               NULL, transform->world,
                                               vec3_zero(), vec3_zero());
      }
    }
  }
  vkr_animation_player_destroy(animation->player);
  if (animation->owns_requests) {
    vkr_resource_system_unload(&animation->bank_request, animation->bank_path);
    vkr_resource_system_unload(&animation->mesh_request, animation->mesh_path);
  }
  vkr_allocator_release_global_accounting(&animation->allocator);
  arena_destroy(arena);
}

bool8_t vkr_scene_animation_attach(VkrScene *scene, VkrEntityId wrapper,
                                   VkrResourceHandleInfo *mesh_request,
                                   VkrResourceHandleInfo *animation_request,
                                   const VkrEntityId *source_nodes,
                                   uint32_t node_count,
                                   const VkrSceneAnimationConfig *config,
                                   VkrAllocator *scratch, const char **error) {
  if (!vkr_scene_physics_mutations_allowed(scene)) {
    return false_v;
  }
  if (error) {
    *error = NULL;
  }
  if (!scene || !scene->world || !mesh_request || !animation_request ||
      !source_nodes || !config || !scratch ||
      mesh_request->type != VKR_RESOURCE_TYPE_MESH ||
      animation_request->type != VKR_RESOURCE_TYPE_ANIMATION ||
      vkr_scene_animation_get_player(scene, wrapper)) {
    return scene_animation_fail(error,
                                "Invalid or duplicate scene animation binding");
  }
  VkrResourceHandleInfo mesh_resolved = {0};
  VkrResourceHandleInfo bank_resolved = {0};
  if (!vkr_resource_system_try_get_resolved(mesh_request, &mesh_resolved) ||
      !vkr_resource_system_try_get_resolved(animation_request,
                                            &bank_resolved) ||
      !mesh_resolved.as.mesh || !bank_resolved.as.animation) {
    return scene_animation_fail(error,
                                "Animation binding requires ready resources");
  }
  const VkrMeshLoaderResult *mesh = mesh_resolved.as.mesh;
  const VkrAnimationLoaderResult *bank = bank_resolved.as.animation;
  const VkrAnimationAsset *asset = &bank->asset;
  if (!mesh->skin.skin_count ||
      mesh->skin.animation_fingerprint != asset->source_fingerprint ||
      node_count != asset->node_count ||
      node_count != mesh->source.nodes.length ||
      mesh->skin.skin_count != asset->skin_count ||
      mesh->source.animation_count != asset->clip_count) {
    return scene_animation_fail(error,
                                "Mesh and animation bank identities differ");
  }
  for (uint32_t i = 0; i < node_count; ++i) {
    if (mesh->source.nodes.data[i].parent != asset->nodes[i].parent ||
        MemCompare(&mesh->source.nodes.data[i].local, &asset->nodes[i].local,
                   sizeof(Mat4)) != 0) {
      return scene_animation_fail(
          error, "Animation source hierarchy or rest transform differs");
    }
  }
  for (uint32_t i = 0; i < asset->skin_count; ++i) {
    if (mesh->skin.joint_counts[i] != asset->skins[i].joint_count) {
      return scene_animation_fail(error, "Animation skin palette size differs");
    }
  }
  if (!scene_animation_mapping_valid(scene, wrapper, &mesh->source,
                                     source_nodes)) {
    return scene_animation_fail(error,
                                "Animation target mapping is stale or edited");
  }

  uint64_t joint_boxes = 0;
  for (uint32_t n = 0; n < node_count; ++n) {
    const VkrMeshSourceNode *node = &mesh->source.nodes.data[n];
    if (node->in_scene && node->skin != UINT32_MAX &&
        node->mesh_variant < mesh->source.meshes.length &&
        !scene_animation_reserve_add(&joint_boxes,
                                     asset->skins[node->skin].joint_count, 1)) {
      return scene_animation_fail(error, "Animation bounds count overflow");
    }
  }
  /* Header, allocation alignment and path copies fit inside the fixed slack;
   * bulk arrays scale with validated source counts, not an arbitrary mesh cap.
   */
  uint64_t reserve = MB(1);
  if (!scene_animation_reserve_add(&reserve, 1, sizeof(VkrSceneAnimation)) ||
      !scene_animation_reserve_add(&reserve, 1,
                                   sizeof(VkrAnimationGraphInstance)) ||
      !scene_animation_reserve_add(&reserve, mesh->source_path.length, 1) ||
      !scene_animation_reserve_add(&reserve, bank->source_path.length, 1) ||
      !scene_animation_reserve_add(&reserve, node_count, sizeof(VkrEntityId)) ||
      !scene_animation_reserve_add(&reserve, mesh->skin.vertex_count,
                                   sizeof(VkrVertex3d)) ||
      !scene_animation_reserve_add(&reserve, node_count,
                                   sizeof(VkrSkinningInput) +
                                       sizeof(Vec3) * 2u +
                                       sizeof(SceneAnimationNodeBounds)) ||
      !scene_animation_reserve_add(&reserve, joint_boxes,
                                   sizeof(SceneAnimationBox)) ||
      !scene_animation_reserve_add(&reserve, KB(64), 1)) {
    return scene_animation_fail(error, "Animation allocation size overflow");
  }
  reserve = AlignPow2Down(reserve, KB(64));
  Arena *arena = arena_create(reserve, MB(1));
  if (!arena) {
    return scene_animation_fail(error, "Animation binding allocation failed");
  }
  VkrAllocator allocator = {.ctx = arena};
  if (!vkr_allocator_arena(&allocator)) {
    arena_destroy(arena);
    return scene_animation_fail(error, "Animation binding allocator failed");
  }
  VkrSceneAnimation *animation = vkr_allocator_alloc(
      &allocator, sizeof(*animation), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!animation) {
    vkr_allocator_release_global_accounting(&allocator);
    arena_destroy(arena);
    return scene_animation_fail(error, "Animation binding allocation failed");
  }
  *animation = (VkrSceneAnimation){.arena = arena,
                                   .allocator = allocator,
                                   .wrapper = wrapper,
                                   .mesh = mesh,
                                   .scene = scene};
  animation->nodes = vkr_allocator_alloc(
      &animation->allocator, (uint64_t)node_count * sizeof(VkrEntityId),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  animation->mesh_path =
      string8_duplicate(&animation->allocator, &mesh->source_path);
  animation->bank_path =
      string8_duplicate(&animation->allocator, &bank->source_path);
  if (!animation->nodes || !animation->mesh_path.str ||
      !animation->bank_path.str) {
    scene_animation_destroy(animation);
    return scene_animation_fail(error, "Animation binding allocation failed");
  }
  MemCopy(animation->nodes, source_nodes,
          (uint64_t)node_count * sizeof(VkrEntityId));
  if (mesh->skin.vertex_count) {
    animation->bind_vertices = vkr_allocator_alloc(
        &animation->allocator,
        (uint64_t)mesh->skin.vertex_count * sizeof(VkrVertex3d),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    animation->skinning = vkr_allocator_alloc(
        &animation->allocator, (uint64_t)node_count * sizeof(VkrSkinningInput),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    animation->bounds_min = vkr_allocator_alloc(
        &animation->allocator, (uint64_t)node_count * sizeof(Vec3),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    animation->bounds_max = vkr_allocator_alloc(
        &animation->allocator, (uint64_t)node_count * sizeof(Vec3),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    animation->node_bounds = vkr_allocator_alloc(
        &animation->allocator,
        (uint64_t)node_count * sizeof(SceneAnimationNodeBounds),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (joint_boxes) {
      animation->joint_bounds = vkr_allocator_alloc(
          &animation->allocator, joint_boxes * sizeof(SceneAnimationBox),
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    if (!animation->bind_vertices || !animation->skinning ||
        !animation->bounds_min || !animation->bounds_max ||
        !animation->node_bounds || (joint_boxes && !animation->joint_bounds)) {
      scene_animation_destroy(animation);
      return scene_animation_fail(error,
                                  "Animation geometry allocation failed");
    }
    MemZero(animation->bind_vertices,
            (uint64_t)mesh->skin.vertex_count * sizeof(VkrVertex3d));
    MemZero(animation->skinning,
            (uint64_t)node_count * sizeof(VkrSkinningInput));
    MemZero(animation->node_bounds,
            (uint64_t)node_count * sizeof(SceneAnimationNodeBounds));
    if (joint_boxes) {
      MemZero(animation->joint_bounds, joint_boxes * sizeof(SceneAnimationBox));
    }
    const VkrPackedStaticVertex *vertices = mesh->mesh_buffer.vertices;
    const uint32_t *indices = mesh->mesh_buffer.indices;
    for (uint32_t r = 0; r < mesh->submeshes.length; ++r) {
      const VkrGeometryUploadRange *range = &mesh->submeshes.data[r];
      for (uint32_t k = 0; k < range->index_count; ++k) {
        uint32_t index = indices[range->first_index + k];
        vkr_packed_geometry_unpack(
            &vertices[index], 1,
            &mesh->mesh_buffer.decodes[range->decode_index],
            &animation->bind_vertices[index]);
      }
    }
  }
  if (mesh->skin.vertex_count &&
      !scene_animation_init_bounds(animation, asset, scratch)) {
    scene_animation_destroy(animation);
    return scene_animation_fail(
        error,
        "Animation rest bounds are not finite or scratch allocation failed");
  }
  animation->player =
      vkr_animation_player_create(asset, scratch, config->clip, config->loop,
                                  config->rate, config->playing, error);
  if (!animation->player) {
    scene_animation_destroy(animation);
    return false_v;
  }
  if (config->controller_json.length) {
    VkrJsonReader reader = vkr_json_reader_from_string(config->controller_json);
    VkrAnimationGraph graph;
    animation->controller = vkr_allocator_alloc(
        &animation->allocator, sizeof(*animation->controller),
        VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (!animation->controller ||
        !vkr_animation_graph_read_json(&reader, asset, &graph, error) ||
        !vkr_animation_graph_initialize(animation->controller, &graph, asset,
                                        error) ||
        !vkr_animation_graph_evaluate(animation->controller,
                                      animation->player)) {
      scene_animation_destroy(animation);
      return scene_animation_fail(error,
                                  "Animation controller attachment failed");
    }
    animation->controller_enabled = true_v;
  }
  animation->mesh_request = *mesh_request;
  animation->bank_request = *animation_request;
  animation->owns_requests = true_v;
  animation->next = scene->animations;
  scene->animations = animation;
  *mesh_request = (VkrResourceHandleInfo){.loader_id = VKR_INVALID_ID};
  *animation_request = (VkrResourceHandleInfo){.loader_id = VKR_INVALID_ID};
  return true_v;
}

void vkr_scene_animation_detach(VkrScene *scene, VkrEntityId wrapper) {
  if (!vkr_scene_physics_mutations_allowed(scene)) {
    return;
  }
  if (!scene) {
    return;
  }
  VkrSceneAnimation **cursor = &scene->animations;
  while (*cursor) {
    VkrSceneAnimation *animation = *cursor;
    if (animation->wrapper.u64 == wrapper.u64) {
      *cursor = animation->next;
      scene_animation_destroy(animation);
      return;
    }
    cursor = &animation->next;
  }
}

VkrAnimationPlayer *vkr_scene_animation_get_player(const VkrScene *scene,
                                                   VkrEntityId wrapper) {
  if (!scene) {
    return NULL;
  }
  for (VkrSceneAnimation *animation = scene->animations; animation;
       animation = animation->next) {
    if (animation->wrapper.u64 == wrapper.u64) {
      return animation->player;
    }
  }
  return NULL;
}

const VkrAnimationGraphInstance *
vkr_scene_animation_get_graph(const VkrScene *scene, VkrEntityId wrapper) {
  if (!scene) {
    return NULL;
  }
  for (VkrSceneAnimation *animation = scene->animations; animation;
       animation = animation->next) {
    if (animation->wrapper.u64 == wrapper.u64) {
      return animation->controller_enabled ? animation->controller : NULL;
    }
  }
  return NULL;
}

bool8_t vkr_scene_animation_apply_graph(const VkrScene *scene,
                                        VkrEntityId wrapper,
                                        const VkrAnimationGraph *graph,
                                        const char **error) {
  if (!vkr_scene_physics_mutations_allowed(scene)) {
    return false_v;
  }
  if (error) {
    *error = NULL;
  }
  if (!scene) {
    return scene_animation_fail(error, "Animation scene is unavailable");
  }
  for (VkrSceneAnimation *animation = scene->animations; animation;
       animation = animation->next) {
    if (animation->wrapper.u64 != wrapper.u64) {
      continue;
    }
    if (!graph) {
      if (!vkr_animation_player_select_clip(
              animation->player, vkr_animation_player_clip(animation->player),
              vkr_animation_player_loop(animation->player))) {
        return scene_animation_fail(error,
                                    "Animation clip could not be restored");
      }
      animation->controller_enabled = false_v;
      return true_v;
    }
    VkrAnimationGraphInstance next;
    if (!vkr_animation_graph_initialize(
            &next, graph, vkr_animation_player_asset(animation->player),
            error)) {
      return false_v;
    }
    if (!animation->controller) {
      animation->controller = vkr_allocator_alloc(
          &animation->allocator, sizeof(*animation->controller),
          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
      if (!animation->controller) {
        return scene_animation_fail(error,
                                    "Animation controller allocation failed");
      }
    }
    if (!vkr_animation_graph_evaluate(&next, animation->player)) {
      return scene_animation_fail(error,
                                  "Animation controller evaluation failed");
    }
    *animation->controller = next;
    animation->controller_enabled = true_v;
    return true_v;
  }
  return scene_animation_fail(error, "Entity has no animation player");
}

bool8_t vkr_scene_animation_seek(const VkrScene *scene, VkrEntityId wrapper,
                                 float64_t seconds) {
  if (!vkr_scene_physics_mutations_allowed(scene)) {
    return false_v;
  }
  if (!scene) {
    return false_v;
  }
  for (VkrSceneAnimation *animation = scene->animations; animation;
       animation = animation->next) {
    if (animation->wrapper.u64 == wrapper.u64) {
      return animation->controller_enabled
                 ? vkr_animation_graph_seek(animation->controller,
                                            animation->player, seconds)
                 : vkr_animation_player_seek(animation->player, seconds);
    }
  }
  return false_v;
}

bool8_t vkr_scene_animation_set_parameter(const VkrScene *scene,
                                          VkrEntityId wrapper,
                                          uint32_t parameter, float32_t value) {
  if (!vkr_scene_physics_mutations_allowed(scene)) {
    return false_v;
  }
  if (!scene) {
    return false_v;
  }
  for (VkrSceneAnimation *animation = scene->animations; animation;
       animation = animation->next) {
    if (animation->wrapper.u64 != wrapper.u64 ||
        !animation->controller_enabled) {
      continue;
    }
    VkrAnimationGraphInstance *controller = animation->controller;
    if (parameter >= controller->graph.parameter_count || !isfinite(value)) {
      return false_v;
    }
    float32_t previous = controller->parameters[parameter];
    if (!vkr_animation_graph_set_parameter(controller, parameter, value)) {
      return false_v;
    }
    if (!vkr_animation_graph_evaluate(controller, animation->player)) {
      controller->parameters[parameter] = previous;
      return false_v;
    }
    return true_v;
  }
  return false_v;
}

void vkr_scene_animation_update(VkrScene *scene, float64_t dt) {
  if (!vkr_scene_physics_mutations_allowed(scene)) {
    return;
  }
  if (!scene || !isfinite(dt) || dt < 0.0) {
    return;
  }
  VkrSceneAnimation **cursor = &scene->animations;
  while (*cursor) {
    VkrSceneAnimation *animation = *cursor;
    if (!scene_animation_mapping_valid(scene, animation->wrapper,
                                       &animation->mesh->source,
                                       animation->nodes)) {
      *cursor = animation->next;
      scene_animation_destroy(animation);
      continue;
    }
    bool8_t advanced = true_v;
    if (animation->controller_enabled) {
      if (vkr_animation_player_playing(animation->player)) {
        float64_t elapsed = dt * vkr_animation_player_rate(animation->player);
        if (!isfinite(elapsed)) {
          advanced = false_v;
        } else if (elapsed < 0.0) {
          advanced = vkr_animation_graph_seek(
              animation->controller, animation->player,
              Max(0.0, animation->controller->time + elapsed));
        } else {
          advanced = vkr_animation_graph_advance(animation->controller,
                                                 animation->player, elapsed);
        }
      }
    } else {
      advanced = vkr_animation_player_advance(animation->player, dt);
    }
    if (!advanced) {
      vkr_animation_player_set_playing(animation->player, false_v);
      log_error("Scene animation paused after pose evaluation failed");
    }
    cursor = &animation->next;
  }
}

void vkr_scene_animation_shutdown(VkrScene *scene) {
  if (!vkr_scene_physics_mutations_allowed(scene)) {
    return;
  }
  if (!scene) {
    return;
  }
  while (scene->animations) {
    VkrSceneAnimation *animation = scene->animations;
    scene->animations = animation->next;
    scene_animation_destroy(animation);
  }
}

void vkr_scene_animation_entity_destroying(VkrScene *scene,
                                           VkrEntityId entity) {
  if (!scene) {
    return;
  }
  VkrSceneAnimation **cursor = &scene->animations;
  while (*cursor) {
    VkrSceneAnimation *animation = *cursor;
    bool8_t matched = animation->wrapper.u64 == entity.u64;
    for (uint32_t i = 0; !matched && i < animation->mesh->source.nodes.length;
         ++i) {
      matched = animation->nodes[i].u64 == entity.u64;
    }
    if (matched) {
      *cursor = animation->next;
      scene_animation_destroy(animation);
    } else {
      cursor = &animation->next;
    }
  }
}

void vkr_scene_animation_sync_render(VkrScene *scene, VkrRenderAssets *assets) {
  if (!scene || !assets) {
    return;
  }
  VkrMeshManager *manager = &assets->mesh_manager;
  for (VkrSceneAnimation *animation = scene->animations; animation;
       animation = animation->next) {
    if (!animation->skinning) {
      continue;
    }
    const SceneTransform *wrapper =
        vkr_scene_get_transform(scene, animation->wrapper);
    const uint64_t generation =
        vkr_animation_player_generation(animation->player);
    const bool8_t update_bounds = animation->bounds_generation != generation;
    for (uint32_t n = 0; n < animation->mesh->source.nodes.length; ++n) {
      const VkrMeshSourceNode *node = &animation->mesh->source.nodes.data[n];
      if (!node->in_scene || node->skin == UINT32_MAX) {
        continue;
      }
      const SceneMeshRenderer *component =
          vkr_entity_get_component_if_alive_const(
              scene->world, animation->nodes[n], scene->comp_mesh_renderer);
      VkrMeshInstance *instance =
          component
              ? vkr_mesh_manager_get_instance(manager, component->instance)
              : NULL;
      VkrMeshAsset *asset =
          instance ? vkr_mesh_manager_get_live_asset(manager, instance->asset)
                   : NULL;
      if (!asset || !asset->submeshes.length ||
          instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
        continue;
      }
      const Mat4 *palette =
          vkr_animation_player_skin_palette(animation->player, node->skin);
      if (update_bounds || animation->bounds_failed ||
          !animation->skinning[n].vertex_count) {
        SceneAnimationBox bounds = {0};
        if (!scene_animation_node_bounds(animation, n, palette, &bounds)) {
          const SceneTransform *authored =
              vkr_scene_get_transform(scene, animation->nodes[n]);
          vkr_mesh_manager_instance_set_skinning(manager, component->instance,
                                                 NULL, authored->world,
                                                 vec3_zero(), vec3_zero());
          vkr_animation_player_set_playing(animation->player, false_v);
          if (!animation->bounds_failed) {
            log_error("Scene animation paused: deformed bounds exceed finite "
                      "float32 range");
            animation->bounds_failed = true_v;
          }
          continue;
        }
        animation->bounds_min[n] = bounds.min;
        animation->bounds_max[n] = bounds.max;
      }
      animation->skinning[n] = (VkrSkinningInput){
          .geometry = asset->submeshes.data[0].geometry,
          .temporal_index =
              vkr_mesh_manager_capacity(manager) + component->instance.id - 1u,
          .temporal_generation = component->instance.generation,
          .pose_generation = generation,
          .discontinuity =
              vkr_animation_player_discontinuity(animation->player),
          .vertices = animation->bind_vertices,
          .influences = animation->mesh->skin.vertices,
          .palette = palette,
          .vertex_count = animation->mesh->skin.vertex_count,
          .joint_count = animation->mesh->skin.joint_counts[node->skin],
      };
      vkr_mesh_manager_instance_set_skinning(
          manager, component->instance, &animation->skinning[n], wrapper->world,
          animation->bounds_min[n], animation->bounds_max[n]);
    }
    animation->bounds_generation = generation;
  }
}

bool8_t vkr_scene_animation_build_preview(
    const VkrScene *scene, VkrEntityId wrapper,
    const VkrAnimationPlayer *player, float32_t yaw, float32_t pitch,
    float32_t distance, VkrAllocator *scratch, VkrWorldPassPayload *world,
    VkrAnimationPreviewInput *preview) {
  if (!scene || !scene->assets || !player || !scratch || !world || !preview ||
      !isfinite(yaw) || !isfinite(pitch) || !isfinite(distance) ||
      distance <= 0.0f) {
    return false_v;
  }
  const VkrSceneAnimation *animation = scene->animations;
  while (animation && animation->wrapper.u64 != wrapper.u64) {
    animation = animation->next;
  }
  if (!animation || !animation->bind_vertices ||
      vkr_animation_player_asset(player) !=
          vkr_animation_player_asset(animation->player)) {
    return false_v;
  }
  VkrMeshManager *manager = &scene->assets->mesh_manager;
  uint32_t binding_count = 0;
  uint32_t draw_count = 0;
  for (uint32_t n = 0; n < animation->mesh->source.nodes.length; ++n) {
    const SceneMeshRenderer *component =
        vkr_entity_get_component_if_alive_const(
            scene->world, animation->nodes[n], scene->comp_mesh_renderer);
    VkrMeshInstance *instance =
        component ? vkr_mesh_manager_get_instance(manager, component->instance)
                  : NULL;
    VkrMeshAsset *asset =
        instance ? vkr_mesh_manager_get_live_asset(manager, instance->asset)
                 : NULL;
    if (asset && instance->loading_state == VKR_MESH_LOADING_STATE_LOADED) {
      draw_count += (uint32_t)asset->submeshes.length;
      binding_count += animation->mesh->source.nodes.data[n].skin != UINT32_MAX;
    }
  }
  if (!draw_count ||
      binding_count > VKR_SKINNING_BINDING_CAPACITY - world->skinning_count ||
      draw_count > VKR_GPU_DRAW_CANDIDATE_CAPACITY) {
    return false_v;
  }
  VkrSkinningInput *inputs = vkr_allocator_alloc(
      scratch,
      (uint64_t)(world->skinning_count + binding_count) * sizeof(*inputs),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrAnimationPreviewDraw *draws =
      vkr_allocator_alloc(scratch, (uint64_t)draw_count * sizeof(*draws),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!inputs || !draws) {
    return false_v;
  }
  if (world->skinning_count) {
    MemCopy(inputs, world->skinning,
            (uint64_t)world->skinning_count * sizeof(*inputs));
  }
  uint32_t skin_cursor = world->skinning_count;
  uint32_t draw_cursor = 0;
  if (!animation->preview_bounds.valid) {
    return false_v;
  }
  const Vec3 min = animation->preview_bounds.min;
  const Vec3 max = animation->preview_bounds.max;
  for (uint32_t n = 0; n < animation->mesh->source.nodes.length; ++n) {
    const SceneMeshRenderer *component =
        vkr_entity_get_component_if_alive_const(
            scene->world, animation->nodes[n], scene->comp_mesh_renderer);
    VkrMeshInstance *instance =
        component ? vkr_mesh_manager_get_instance(manager, component->instance)
                  : NULL;
    VkrMeshAsset *asset =
        instance ? vkr_mesh_manager_get_live_asset(manager, instance->asset)
                 : NULL;
    if (!asset || instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      continue;
    }
    const uint32_t skin = animation->mesh->source.nodes.data[n].skin;
    uint32_t skinning_index = 0;
    if (skin != UINT32_MAX) {
      inputs[skin_cursor] = (VkrSkinningInput){
          .geometry = asset->submeshes.data[0].geometry,
          /* Preview never participates in main-view temporal correspondence. */
          .temporal_index = VKR_TEMPORAL_TRANSFORM_CAPACITY - 1u,
          .temporal_generation = component->instance.generation,
          .pose_generation = vkr_animation_player_generation(player),
          .discontinuity = UINT64_MAX,
          .vertices = animation->bind_vertices,
          .influences = animation->mesh->skin.vertices,
          .palette = vkr_animation_player_skin_palette(player, skin),
          .vertex_count = animation->mesh->skin.vertex_count,
          .joint_count = animation->mesh->skin.joint_counts[skin],
      };
      skinning_index = ++skin_cursor;
    }
    for (uint32_t r = 0; r < asset->submeshes.length; ++r) {
      draws[draw_cursor++] = (VkrAnimationPreviewDraw){
          .geometry = asset->submeshes.data[r].geometry,
          .submesh_index = asset->submeshes.data[r].geometry_submesh_index,
          .skinning_index = skinning_index,
          .model = skin != UINT32_MAX
                       ? mat4_identity()
                       : vkr_animation_player_global_pose(player)[n],
      };
    }
  }
  /* Framing is checked in double so the float camera it produces stays finite
   * for any finite bounds. */
  Vec3 center = vec3_new((float32_t)(((float64_t)min.x + max.x) * 0.5),
                         (float32_t)(((float64_t)min.y + max.y) * 0.5),
                         (float32_t)(((float64_t)min.z + max.z) * 0.5));
  const float64_t dx = (float64_t)max.x - center.x;
  const float64_t dy = (float64_t)max.y - center.y;
  const float64_t dz = (float64_t)max.z - center.z;
  const float64_t extent = sqrt(dx * dx + dy * dy + dz * dz);
  const float64_t camera_radius = Max(extent, 0.01);
  if (!isfinite(camera_radius) ||
      camera_radius * (distance + 20.0) > FLT_MAX * 0.5 ||
      fabs(center.x) + camera_radius * distance > FLT_MAX * 0.5 ||
      fabs(center.y) + camera_radius * distance > FLT_MAX * 0.5 ||
      fabs(center.z) + camera_radius * distance > FLT_MAX * 0.5) {
    return false_v;
  }
  float32_t radius = (float32_t)camera_radius;
  Vec3 direction =
      vec3_new(cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw));
  Vec3 eye = vec3_add(center, vec3_scale(direction, radius * distance));
  Mat4 view = mat4_look_at(eye, center, vec3_new(0, 1, 0));
  Mat4 projection = mat4_perspective(0.7853981634f, 1.0f, radius * 0.01f,
                                     radius * (distance + 20.0f));
  *preview =
      (VkrAnimationPreviewInput){.view_projection = mat4_mul(projection, view),
                                 .draws = draws,
                                 .draw_count = draw_cursor};
  world->skinning = inputs;
  world->skinning_count = skin_cursor;
  return true_v;
}

VkrEntityId vkr_scene_animation_node_entity(const VkrScene *scene,
                                            VkrEntityId wrapper,
                                            uint32_t source_node) {
  for (VkrSceneAnimation *animation = scene ? scene->animations : NULL;
       animation; animation = animation->next) {
    if (animation->wrapper.u64 == wrapper.u64 &&
        source_node < animation->mesh->source.nodes.length) {
      return animation->nodes[source_node];
    }
  }
  return VKR_ENTITY_ID_INVALID;
}

bool8_t vkr_scene_animation_node_world(const VkrScene *scene,
                                       VkrEntityId wrapper,
                                       uint32_t source_node, Mat4 *world) {
  VkrAnimationPlayer *player = vkr_scene_animation_get_player(scene, wrapper);
  const VkrAnimationAsset *asset = vkr_animation_player_asset(player);
  Mat4 wrapper_world;
  if (!player || !asset || source_node >= asset->node_count || !world ||
      !vkr_scene_physics_resolve_world(scene, wrapper, &wrapper_world)) {
    return false_v;
  }
  *world = mat4_mul(wrapper_world,
                    vkr_animation_player_global_pose(player)[source_node]);
  return true_v;
}

bool8_t vkr_scene_animation_override_nodes(VkrScene *scene, VkrEntityId wrapper,
                                           const uint32_t *source_nodes,
                                           const Mat4 *world_matrices,
                                           uint32_t count, const char **error) {
  VkrAnimationPlayer *player = vkr_scene_animation_get_player(scene, wrapper);
  Mat4 wrapper_world;
  if (!player || count > VKR_SCENE_PHYSICS_MAX_BODIES ||
      !vkr_scene_physics_resolve_world(scene, wrapper, &wrapper_world)) {
    return scene_animation_fail(error,
                                "Cannot resolve ragdoll animation wrapper");
  }
  Mat4 globals[VKR_SCENE_PHYSICS_MAX_BODIES];
  const Mat4 inverse = mat4_inverse(wrapper_world);
  for (uint32_t i = 0; i < count; ++i) {
    globals[i] = mat4_mul(inverse, world_matrices[i]);
  }
  if (!vkr_animation_player_override_globals(player, source_nodes, globals,
                                             count)) {
    return scene_animation_fail(error, "Ragdoll bone pose publication failed");
  }
  return true_v;
}

typedef struct SceneAnimationResetEntry {
  struct SceneAnimationResetEntry *next;
  VkrSceneAnimation *animation;
  VkrAnimationPlayerCheckpoint *player;
  VkrAnimationGraphInstance graph;
} SceneAnimationResetEntry;

struct s_VkrSceneAnimationReset {
  Arena *arena;
  SceneAnimationResetEntry *entries;
};

void vkr_scene_animation_reset_finish(VkrSceneAnimationReset *reset,
                                      bool8_t commit) {
  if (!reset) {
    return;
  }
  for (SceneAnimationResetEntry *entry = reset->entries; entry;
       entry = entry->next) {
    vkr_animation_player_checkpoint_finish(entry->player, commit);
    if (!commit && entry->animation->controller_enabled) {
      *entry->animation->controller = entry->graph;
    }
  }
  arena_destroy(reset->arena);
}

VkrSceneAnimationReset *vkr_scene_animation_reset_begin(VkrScene *scene,
                                                        const char **error) {
  if (!scene || !vkr_scene_physics_mutations_allowed(scene)) {
    scene_animation_fail(error,
                         "Cannot reset animations during contact dispatch");
    return NULL;
  }
  uint64_t count = 0;
  for (VkrSceneAnimation *animation = scene->animations; animation;
       animation = animation->next) {
    count++;
  }
  /* Player checkpoint contains only scalar/pointer state (<1KiB); pose arrays
   * remain in the untouched original slot throughout this synchronous reset. */
  uint64_t reserve =
      ARENA_HEADER_SIZE + sizeof(VkrSceneAnimationReset) +
      count * (sizeof(SceneAnimationResetEntry) + KB(1) + 2 * MaxAlign());
  Arena *arena = arena_create(Max(reserve, KB(64)), KB(4));
  if (!arena) {
    scene_animation_fail(error, "Animation reset checkpoint allocation failed");
    return NULL;
  }
  VkrSceneAnimationReset *reset =
      arena_alloc(arena, sizeof(*reset), ARENA_MEMORY_TAG_STRUCT);
  if (!reset) {
    arena_destroy(arena);
    scene_animation_fail(error, "Animation reset checkpoint allocation failed");
    return NULL;
  }
  *reset = (VkrSceneAnimationReset){.arena = arena};
  for (VkrSceneAnimation *animation = scene->animations; animation;
       animation = animation->next) {
    SceneAnimationResetEntry *entry =
        arena_alloc(arena, sizeof(*entry), ARENA_MEMORY_TAG_STRUCT);
    if (!entry) {
      goto cleanup;
    }
    *entry = (SceneAnimationResetEntry){.next = reset->entries,
                                        .animation = animation};
    if (animation->controller_enabled) {
      entry->graph = *animation->controller;
    }
    reset->entries = entry;
    entry->player =
        vkr_animation_player_checkpoint_begin(animation->player, arena);
    if (!entry->player) {
      goto cleanup;
    }
  }
  for (SceneAnimationResetEntry *entry = reset->entries; entry;
       entry = entry->next) {
    if (!vkr_scene_animation_seek(scene, entry->animation->wrapper, 0.0)) {
      goto cleanup;
    }
  }
  return reset;
cleanup:
  vkr_scene_animation_reset_finish(reset, false_v);
  scene_animation_fail(error, "Animation reset checkpoint/seek failed");
  return NULL;
}
