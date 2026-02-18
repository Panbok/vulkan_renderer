#pragma once

#include "containers/str.h"
#include "containers/vector.h"
#include "defines.h"
#include "memory/vkr_allocator.h"
#include "renderer/vkr_buffer.h"
#include "renderer/vkr_renderer.h"

/**
 * @brief Flattened primitive payload emitted by the glTF parser.
 *
 * `vertices` and `indices` are valid only for the duration of the callback
 * invocation and point into parser-owned scratch allocations. Consumers that
 * need persistence must copy immediately.
 */
typedef struct VkrMeshLoaderGltfPrimitive {
  const VkrVertex3d *vertices;
  uint32_t vertex_count;
  const uint32_t *indices;
  uint32_t index_count;
  String8 material_path;
} VkrMeshLoaderGltfPrimitive;

/**
 * @brief Receives one flattened triangle primitive from a glTF source.
 *
 * Returning false aborts parsing and propagates a loader failure.
 * @param user_data User-defined data passed to the callback.
 * @param primitive The primitive to process.
 * @return True if the primitive was processed successfully, false otherwise.
 */
typedef bool8_t (*VkrMeshLoaderGltfPrimitiveFn)(
    void *user_data, const VkrMeshLoaderGltfPrimitive *primitive);

/**
 * @brief Configuration for parsing a `.gltf`/`.glb` source into primitives.
 *
 * `source_path`, `source_dir`, and `source_stem` must remain valid for the
 * call. `load_allocator` owns generated material paths/files and durable
 * parser output strings. `scratch_allocator` is used for per-primitive
 * temporary buffers and may be reset between callbacks. Optional output
 * vectors receive deduplicated absolute/relative dependency paths and
 * generated material file paths.
 */
typedef struct VkrMeshLoaderGltfParseInfo {
  String8 source_path;          // The path to the glTF source file.
  String8 source_dir;           // The directory of the glTF source file.
  String8 source_stem;          // The stem of the glTF source file.
  VkrAllocator *load_allocator; // The allocator to use for loading resources.
  VkrAllocator *scratch_allocator; // The allocator to use for scratch memory.
  VkrRendererError *out_error;     // The error to use for the output.
  VkrMeshLoaderGltfPrimitiveFn
      on_primitive; // The callback to use for the primitives.
  void *user_data;  // User-defined data to pass to the callback.
  Vector_String8 *out_dependency_paths; // The paths to the dependency files.
  Vector_String8 *out_generated_material_paths; // The paths to the generated
                                                // material files.
} VkrMeshLoaderGltfParseInfo;

/**
 * @brief Parse a glTF source, emit flattened triangle primitives, and generate
 * deterministic material files for referenced glTF materials.
 *
 * Embedded image sources (`data:` URIs and `image.buffer_view`) are rejected.
 * On failure, `out_error` is filled when provided.
 * @param info The information to use for the parse.
 * @return True if the parse was successful, false otherwise.
 */
bool8_t vkr_mesh_loader_gltf_parse(const VkrMeshLoaderGltfParseInfo *info);

/**
 * @brief Parse a glTF source and regenerate deterministic `.mt` files only.
 *
 * Geometry primitives are not emitted in this path. This is used to repair
 * missing generated material files when mesh geometry is loaded from cache.
 * @param info The information to use for the parse.
 * @return True if the parse was successful, false otherwise.
 */
bool8_t
vkr_mesh_loader_gltf_generate_materials(const VkrMeshLoaderGltfParseInfo *info);
