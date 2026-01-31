#pragma once

#include "containers/vector.h"
#include "defines.h"
#include "math/mat.h"
#include "memory/vkr_allocator.h"

/**
 * @brief Sorting key for draw command batching.
 *
 * The key groups draws that can share identical binding state. All fields are
 * treated as stable identifiers for a single frame; callers must rebuild the
 * key after any resource or pipeline change.
 */
typedef struct VkrDrawKey {
  uint32_t pipeline_id;
  uint32_t material_id;
  uint32_t geometry_id;
  uint32_t range_id;
} VkrDrawKey;

/**
 * @brief Single draw command captured during visibility collection.
 *
 * When is_instance is false, mesh_index refers to a legacy VkrMesh slot.
 * When is_instance is true, mesh_index refers to a VkrMeshInstance slot.
 * Model is stored by value so later phases can stream instance data without
 * re-reading scene state.
 */
typedef struct VkrDrawCommand {
  VkrDrawKey key;
  uint32_t mesh_index;
  uint32_t submesh_index;
  Mat4 model;
  uint32_t object_id;
  float32_t camera_distance;
  bool8_t is_instance;
} VkrDrawCommand;

/**
 * @brief Batch of draw commands sharing the same key.
 *
 * first_command and command_count index into the command array owned by the
 * batcher. first_instance is reserved for instance-buffer indexing in later
 * phases.
 */
typedef struct VkrDrawBatch {
  VkrDrawKey key;
  uint32_t first_command;
  uint32_t command_count;
  uint32_t first_instance;
} VkrDrawBatch;

Vector(VkrDrawCommand);
Vector(VkrDrawBatch);

/**
 * @brief Per-frame draw batching state.
 *
 * This is reused across frames; callers must reset it at frame start.
 * Not thread-safe.
 */
typedef struct VkrDrawBatcher {
  Vector_VkrDrawCommand opaque_commands;
  Vector_VkrDrawCommand transparent_commands;
  Vector_VkrDrawBatch opaque_batches;
  Vector_VkrDrawBatch transparent_batches;

  uint32_t total_draws_collected;
  uint32_t batches_created;
  uint32_t draws_merged;
} VkrDrawBatcher;

/**
 * @brief Initialize a batcher with persistent storage.
 *
 * initial_capacity applies to both command and batch vectors and is expected to
 * represent a worst-case per-frame draw count to avoid realloc growth.
 */
bool8_t vkr_draw_batcher_init(VkrDrawBatcher *batcher, VkrAllocator *allocator,
                              uint32_t initial_capacity);
/**
 * @brief Releases all storage owned by the batcher.
 */
void vkr_draw_batcher_shutdown(VkrDrawBatcher *batcher);
/**
 * @brief Clears per-frame state while retaining allocated capacity.
 */
void vkr_draw_batcher_reset(VkrDrawBatcher *batcher);

/**
 * @brief Adds an opaque draw command for sorting and batching.
 */
void vkr_draw_batcher_add_opaque(VkrDrawBatcher *batcher,
                                 const VkrDrawCommand *cmd);
/**
 * @brief Adds a transparent draw command for distance sorting.
 */
void vkr_draw_batcher_add_transparent(VkrDrawBatcher *batcher,
                                      const VkrDrawCommand *cmd);

/**
 * @brief Sorts commands and builds contiguous batches.
 *
 * Opaque commands are sorted by key. Transparent commands are sorted by
 * distance (back-to-front) with key as a tie-breaker.
 */
void vkr_draw_batcher_finalize(VkrDrawBatcher *batcher);

/**
 * @brief Returns the number of opaque batches produced for this frame.
 */
uint32_t vkr_draw_batcher_opaque_batch_count(const VkrDrawBatcher *batcher);
/**
 * @brief Retrieves an opaque batch by index.
 */
const VkrDrawBatch *
vkr_draw_batcher_get_opaque_batch(const VkrDrawBatcher *batcher,
                                  uint32_t index);
/**
 * @brief Retrieves a draw command by global index.
 *
 * Indices cover the opaque command range first, followed by transparent
 * commands.
 */
const VkrDrawCommand *
vkr_draw_batcher_get_command(const VkrDrawBatcher *batcher, uint32_t index);
