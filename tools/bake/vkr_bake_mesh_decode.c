#include "bake/vkr_bake_mesh_decode.h"

#include "assets/vkr_mesh_cooked.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "vkr_packed_geometry.h"

#include <math.h>

#define VKR_BAKE_MESH_DECODE_ARENA_RESERVE MB(512)
#define VKR_BAKE_MESH_DECODE_ARENA_COMMIT MB(16)
#define VKR_BAKE_MESH_MAX_HIERARCHY_DEPTH 256u

vkr_internal bool8_t vkr_bake_mesh_finite_vec3(Vec3 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

vkr_internal bool8_t vkr_bake_mesh_finite_vec4(Vec4 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z) &&
         isfinite(value.w);
}

vkr_internal bool8_t vkr_bake_mesh_normal_matrix(Mat4 world, Mat4 *out_normal,
                                                 bool8_t *out_flipped) {
  const Vec3 x =
      vec3_new(world.elements[0], world.elements[1], world.elements[2]);
  const Vec3 y =
      vec3_new(world.elements[4], world.elements[5], world.elements[6]);
  const Vec3 z =
      vec3_new(world.elements[8], world.elements[9], world.elements[10]);
  const float32_t determinant = vec3_dot(vec3_cross(x, y), z);
  if (!isfinite(determinant) || fabsf(determinant) <= 1.0e-8f) {
    return false_v;
  }
  *out_normal = mat4_transpose(mat4_inverse_affine(world));
  *out_flipped = determinant < 0.0f;
  return true_v;
}

vkr_internal bool8_t vkr_bake_mesh_emit_ranges(
    const VkrMeshCookedDecoded *decoded, uint32_t first_range,
    uint32_t range_count, Mat4 world, uint32_t source_instance,
    const VkrBakeMeshDecodeCallbacks *callbacks, void *user) {
  if (first_range > decoded->ranges.length ||
      range_count > decoded->ranges.length - first_range ||
      decoded->mesh_buffer.vertex_layout !=
          VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1 ||
      decoded->mesh_buffer.index_size != sizeof(uint32_t) ||
      decoded->mesh_buffer.vertex_size != sizeof(VkrPackedStaticVertex)) {
    return false_v;
  }

  Mat4 normal = {0};
  bool8_t flipped = false_v;
  if (!vkr_bake_mesh_normal_matrix(world, &normal, &flipped)) {
    return false_v;
  }

  const VkrPackedStaticVertex *packed = decoded->mesh_buffer.vertices;
  const uint32_t *indices = decoded->mesh_buffer.indices;
  for (uint32_t range_index = first_range;
       range_index < first_range + range_count; ++range_index) {
    const VkrGeometryUploadRange *range = &decoded->ranges.data[range_index];
    if (range->index_count == 0u || range->index_count % 3u != 0u ||
        range->decode_index >= decoded->mesh_buffer.decode_count ||
        range->first_index > decoded->mesh_buffer.index_count ||
        range->index_count >
            decoded->mesh_buffer.index_count - range->first_index) {
      return false_v;
    }

    const VkrGpuGeometryDecodeRecord *decode =
        &decoded->mesh_buffer.decodes[range->decode_index];
    for (uint32_t index = range->first_index;
         index < range->first_index + range->index_count; index += 3u) {
      VkrBakeTriangle triangle = {0};
      /* Range token is remapped to a scene material index by the callback. */
      triangle.material_index = range_index;
      triangle.source_instance_index = source_instance;
      for (uint32_t corner = 0; corner < 3u; ++corner) {
        if (indices[index + corner] >= decoded->mesh_buffer.vertex_count) {
          return false_v;
        }
        VkrVertex3d vertex = {0};
        vkr_packed_geometry_unpack(&packed[indices[index + corner]], 1u, decode,
                                   &vertex);
        const Vec3 position = vkr_vertex_unpack_vec3(vertex.position);
        const Vec3 source_normal = vkr_vertex_unpack_vec3(vertex.normal);
        const Vec4 world_normal =
            mat4_mul_vec4(normal, vec4_new(source_normal.x, source_normal.y,
                                           source_normal.z, 0.0f));
        const Vec4 world_tangent = mat4_mul_vec4(world,
            vec4_new(vertex.tangent.x, vertex.tangent.y, vertex.tangent.z, 0.0f));
        triangle.vertex[corner] = (VkrBakeVertex){
            .position = mat4_mul_vec3(world, position),
            .normal = vec3_normalize(
                vec3_new(world_normal.x, world_normal.y, world_normal.z)),
            .uv = vertex.texcoord,
            .color = vertex.colour,
            .tangent = vec4_new(world_tangent.x, world_tangent.y, world_tangent.z,
                                flipped ? -vertex.tangent.w : vertex.tangent.w)};
        if (!vkr_bake_mesh_finite_vec3(triangle.vertex[corner].position) ||
            !vkr_bake_mesh_finite_vec3(triangle.vertex[corner].normal) ||
            vec3_length(triangle.vertex[corner].normal) <= 1.0e-8f ||
            !isfinite(triangle.vertex[corner].uv.x) ||
            !isfinite(triangle.vertex[corner].uv.y) ||
            !vkr_bake_mesh_finite_vec4(triangle.vertex[corner].color)) {
          return false_v;
        }
      }
      if (flipped) {
        const VkrBakeVertex temporary = triangle.vertex[1];
        triangle.vertex[1] = triangle.vertex[2];
        triangle.vertex[2] = temporary;
      }
      if (!callbacks->emit_triangle(user, &triangle, range->material_name)) {
        return false_v;
      }
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_bake_mesh_emit_source_light(
    const VkrMeshSourceLight *source, Mat4 world,
    const VkrBakeMeshDecodeCallbacks *callbacks, void *user) {
  if (source->kind == 0u) {
    return true_v;
  }
  if (source->kind > 3u || !vkr_bake_mesh_finite_vec3(source->color) ||
      !isfinite(source->intensity) || !isfinite(source->range) ||
      !isfinite(source->inner_cone) || !isfinite(source->outer_cone)) {
    return false_v;
  }
  const Vec4 transformed =
      mat4_mul_vec4(world, vec4_new(0.0f, 0.0f, -1.0f, 0.0f));
  const Vec3 direction =
      vec3_normalize(vec3_new(transformed.x, transformed.y, transformed.z));
  if (!vkr_bake_mesh_finite_vec3(direction) ||
      vec3_length(direction) <= 1.0e-8f) {
    return false_v;
  }
  const VkrBakeMeshLight light = {
      .kind = source->kind == 1u   ? VKR_BAKE_MESH_LIGHT_DIRECTIONAL
              : source->kind == 2u ? VKR_BAKE_MESH_LIGHT_POINT
                                   : VKR_BAKE_MESH_LIGHT_SPOT,
      .position = mat4_mul_vec3(world, vec3_zero()),
      .direction = direction,
      .color = source->color,
      .intensity = source->intensity,
      .range = source->range,
      .inner_cone_angle = source->inner_cone,
      .outer_cone_angle = source->outer_cone,
  };
  return vkr_bake_mesh_finite_vec3(light.position) &&
         callbacks->emit_light(user, &light);
}

vkr_internal bool8_t vkr_bake_mesh_build_source_worlds(
    VkrAllocator *allocator, const VkrMeshSource *source, Mat4 **out_worlds) {
  if (source->nodes.length > UINT32_MAX || !out_worlds ||
      (source->nodes.length && !source->nodes.data)) {
    return false_v;
  }
  const uint32_t count = (uint32_t)source->nodes.length;
  Mat4 *worlds = vkr_allocator_alloc(allocator, count * sizeof(*worlds),
                                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint8_t *state =
      vkr_allocator_alloc(allocator, count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *stack = vkr_allocator_alloc(
      allocator, VKR_BAKE_MESH_MAX_HIERARCHY_DEPTH * sizeof(*stack),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (count && (!worlds || !state || !stack)) {
    return false_v;
  }
  MemZero(state, count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t cursor = i;
    uint32_t stack_count = 0u;
    while (state[cursor] == 0u) {
      if (stack_count == VKR_BAKE_MESH_MAX_HIERARCHY_DEPTH) {
        return false_v;
      }
      state[cursor] = 1u;
      stack[stack_count++] = cursor;
      const uint32_t parent = source->nodes.data[cursor].parent;
      if (parent == UINT32_MAX) {
        break;
      }
      if (parent >= count || state[parent] == 1u) {
        return false_v;
      }
      cursor = parent;
    }
    while (stack_count > 0u) {
      const uint32_t node_index = stack[--stack_count];
      const VkrMeshSourceNode *node = &source->nodes.data[node_index];
      worlds[node_index] = node->parent == UINT32_MAX
                               ? node->local
                               : mat4_mul(worlds[node->parent], node->local);
      for (uint32_t component = 0; component < 16u; ++component) {
        if (!isfinite(worlds[node_index].elements[component])) {
          return false_v;
        }
      }
      state[node_index] = 2u;
    }
  }
  *out_worlds = worlds;
  return true_v;
}

bool8_t vkr_bake_mesh_decode(const uint8_t *data, uint64_t size,
                             Mat4 entity_world,
                             uint32_t *in_out_source_instance,
                             const VkrBakeMeshDecodeCallbacks *callbacks,
                             void *user) {
  if (!data || size == 0u || !in_out_source_instance || !callbacks ||
      !callbacks->emit_triangle) {
    return false_v;
  }

  Arena *result_arena = arena_create(VKR_BAKE_MESH_DECODE_ARENA_RESERVE,
                                     VKR_BAKE_MESH_DECODE_ARENA_COMMIT);
  Arena *scratch_arena = arena_create(VKR_BAKE_MESH_DECODE_ARENA_RESERVE,
                                      VKR_BAKE_MESH_DECODE_ARENA_COMMIT);
  if (!result_arena || !scratch_arena) {
    arena_destroy(scratch_arena);
    arena_destroy(result_arena);
    return false_v;
  }

  VkrAllocator result_allocator = {.ctx = result_arena};
  VkrAllocator scratch_allocator = {.ctx = scratch_arena};
  VkrMeshCookedDecoded decoded = {0};
  bool8_t success =
      vkr_allocator_arena(&result_allocator) &&
      vkr_allocator_arena(&scratch_allocator) &&
      vkr_mesh_cooked_decode(&result_allocator, &scratch_allocator, data, size,
                             &decoded);
  if (success && decoded.source.nodes.length > 0u) {
    Mat4 *source_worlds = NULL;
    success = vkr_bake_mesh_build_source_worlds(
        &scratch_allocator, &decoded.source, &source_worlds);
    for (uint32_t i = 0; success && i < decoded.source.nodes.length; ++i) {
      const VkrMeshSourceNode *node = &decoded.source.nodes.data[i];
      if (!node->in_scene) {
        continue;
      }
      const Mat4 world = mat4_mul(entity_world, source_worlds[i]);
      if (node->mesh_variant != UINT32_MAX) {
        if (node->mesh_variant >= decoded.source.meshes.length) {
          success = false_v;
          break;
        }
        const VkrMeshSourceMesh *mesh =
            &decoded.source.meshes.data[node->mesh_variant];
        success = vkr_bake_mesh_emit_ranges(
            &decoded, mesh->first_range, mesh->range_count, world,
            (*in_out_source_instance)++, callbacks, user);
      }
      if (success && callbacks->emit_light) {
        success = vkr_bake_mesh_emit_source_light(&node->punctual, world,
                                                  callbacks, user);
      }
    }
  } else if (success) {
    success = vkr_bake_mesh_emit_ranges(
        &decoded, 0u, (uint32_t)decoded.ranges.length, entity_world,
        (*in_out_source_instance)++, callbacks, user);
  }

  vkr_allocator_release_global_accounting(&scratch_allocator);
  vkr_allocator_release_global_accounting(&result_allocator);
  arena_destroy(scratch_arena);
  arena_destroy(result_arena);
  return success;
}
